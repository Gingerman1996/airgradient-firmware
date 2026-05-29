/**
 * AirGradient Go — CloudService implementation
 *
 * POST runs first each iteration; FETCH runs after.  Deadlines are
 * start-anchored so wall-clock cadence is preserved across varying
 * call durations.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_cloud.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "ag_log.h"
#include "common.h"
#include "go_cloud_types.h"
#include "go_events.h"
#include "services/ag_client.h"
#include "types/wifi_types.h"
#include "go_wifi.h"

static constexpr const char *TAG = "CloudService";

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

static constexpr uint32_t CLOUD_TASK_STACK_SIZE = 8192;
static constexpr uint32_t CLOUD_TASK_PRIORITY = 4;
static constexpr size_t FETCH_BUFFER_BYTES = 1024;

/// Dashboard "no RSSI" convention; avoids a misleading 0 dB reading.
static constexpr int RSSI_UNAVAILABLE = -127;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CloudService::CloudService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg)
    : _event_queue(event_queue), _client(deps.client), _wifi(deps.wifi), _cfg(cfg),
      _disable_cloud(cfg.disable_cloud) {}

CloudService::~CloudService() { stop(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool CloudService::start() {
  if (_task_handle != nullptr) {
    return true;
  }

  log_heap(TAG, "cloud.start:enter");
  _shutdown_pending.store(false);

  _fetch_buf = static_cast<char *>(std::malloc(FETCH_BUFFER_BYTES));
  if (_fetch_buf == nullptr) {
    AG_LOGE(TAG, "start: fetch buffer alloc failed (%zu bytes)", FETCH_BUFFER_BYTES);
    return false;
  }

  // Semaphore must exist before task_create so stop() never races.
  if (!_done_sem.create()) {
    AG_LOGE(TAG, "start: semaphore creation failed");
    std::free(_fetch_buf);
    _fetch_buf = nullptr;
    return false;
  }

  const bool ok = RTOS::task_create(_task_entry, "cloud", CLOUD_TASK_STACK_SIZE, this,
                                    CLOUD_TASK_PRIORITY, &_task_handle);
  if (!ok) {
    AG_LOGE(TAG, "start: task_create failed");
    _done_sem.destroy();
    std::free(_fetch_buf);
    _fetch_buf = nullptr;
    _task_handle = nullptr;
    return false;
  }

  AG_LOGI(TAG, "start: task spawned (stack=%lu, prio=%lu, fetch_buf=%zu)",
          static_cast<unsigned long>(CLOUD_TASK_STACK_SIZE),
          static_cast<unsigned long>(CLOUD_TASK_PRIORITY), FETCH_BUFFER_BYTES);
  log_heap(TAG, "cloud.start:exit");
  return true;
}

void CloudService::stop() {
  if (_task_handle == nullptr) {
    return;
  }

  log_heap(TAG, "cloud.stop:enter");
  AG_LOGI(TAG, "stop: latching shutdown");
  _shutdown_pending.store(true);
  _wake();

  // Bounded by one WifiHttpClient timeout (~15 s).
  _done_sem.take();

  // Task is parked on notify-wait; safe to delete externally.
  RTOS::task_delete(_task_handle);
  _task_handle = nullptr;

  if (_fetch_buf != nullptr) {
    std::free(_fetch_buf);
    _fetch_buf = nullptr;
  }
  _done_sem.destroy();

  _shutdown_pending.store(false);
  _post_due = 0;
  _fetch_due = 0;
  _was_armed = false;
  log_heap(TAG, "cloud.stop:exit");
}

// ---------------------------------------------------------------------------
// State actions
// ---------------------------------------------------------------------------

void CloudService::arm(bool fire_now) {
  if (fire_now) {
    _fire_now_pending.store(true);
  }
  _armed.store(true);
  _wake();
}

void CloudService::disarm() {
  _armed.store(false);
  _wake();
}

void CloudService::set_disable_cloud(bool disable) {
  _disable_cloud.store(disable);
  _wake();
}

void CloudService::update_measures_snapshot(const MeasuresAGo &m) {
  _snapshot_mtx.lock();
  _latest_snapshot = m;
  _snapshot_mtx.unlock();
}

// ---------------------------------------------------------------------------
// Task entry / loop
// ---------------------------------------------------------------------------

// static
void CloudService::_task_entry(void *arg) {
  static_cast<CloudService *>(arg)->_run();
  // Park forever — stop() deletes the task externally.
  while (true) {
    RTOS::delay_ms(60000);
  }
}

void CloudService::_run() {
  for (;;) {
    const uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
    const uint32_t wake_ms = _run_iteration(now);

    if (_shutdown_pending.load()) {
      // Park until stop() calls task_delete (avoids double-delete race).
      RTOS::task_notify_take(UINT32_MAX);
      return;
    }

    if (wake_ms > 0) {
      RTOS::task_notify_take(wake_ms);
    }
  }
}

uint32_t CloudService::_run_iteration(uint32_t now) {
  if (_shutdown_pending.load()) {
#ifdef TEST_HOST
    _test_done_signal_count += 1;
#endif
    _done_sem.give();
    return UINT32_MAX;
  }

  const bool armed = _armed.load();
  const bool disable = _disable_cloud.load();

  // Handle Disarmed→Armed transition: snap deadlines to now (fire_now)
  // or one interval into the future.
  if (armed != _was_armed) {
    if (armed) {
      const bool fire_now = _fire_now_pending.exchange(false);
      _post_due = fire_now ? now : now + _cfg.post_interval_ms;
      _fetch_due = fire_now ? now : now + _cfg.fetch_interval_ms;
    }
    _was_armed = armed;
  }

  if (!armed || disable) {
    // Slide expired deadlines forward while disabled so re-enable
    // doesn't catch up on missed intervals.
    if (armed) {
      if (static_cast<int32_t>(now - _post_due) >= 0) {
        _post_due = now + _cfg.post_interval_ms;
      }
      if (static_cast<int32_t>(now - _fetch_due) >= 0) {
        _fetch_due = now + _cfg.fetch_interval_ms;
      }
    }
    if (!armed) {
      return UINT32_MAX;
    }
    const uint32_t next = std::min(_post_due, _fetch_due);
    const int64_t delta = static_cast<int64_t>(next) - static_cast<int64_t>(now);
    return delta > 0 ? static_cast<uint32_t>(delta) : 0;
  }

  // POST priority: fire POST first; return 0 to re-sample state before
  // considering FETCH (gates out disarm/disable arriving mid-POST).
  if (static_cast<int32_t>(now - _post_due) >= 0) {
    _do_post(now);
    return 0;
  }

  if (static_cast<int32_t>(now - _fetch_due) >= 0) {
    _do_fetch(now);
    return 0;
  }

  const uint32_t next = std::min(_post_due, _fetch_due);
  const int64_t delta = static_cast<int64_t>(next) - static_cast<int64_t>(now);
  return delta > 0 ? static_cast<uint32_t>(delta) : 0;
}

// ---------------------------------------------------------------------------
// HTTP legs
// ---------------------------------------------------------------------------

void CloudService::_do_post(uint32_t now_ms) {
  const uint32_t post_started_at = now_ms;
  MeasuresAGo snap = _snapshot_copy();

  const int raw_rssi = _wifi.rssi();
  const int rssi = (raw_rssi == WIFI_RSSI_INVALID) ? RSSI_UNAVAILABLE : raw_rssi;

  log_heap(TAG, "cloud.post:pre-tls");
  const AgClientResult result = _client.http_post_measures(snap, rssi);
  log_heap(TAG, "cloud.post:post-tls");
  AG_LOGI(TAG, "post_measures result=%d rssi=%d", static_cast<int>(result), rssi);

  Event evt{};
  evt.type = EventType::PostMeasuresResult;
  evt.cloud_result = static_cast<CloudResultByte>(result);
  RTOS::queue_send(_event_queue, &evt);

  // Anchor to start time, not completion.
  _post_due = post_started_at + _cfg.post_interval_ms;
}

void CloudService::_do_fetch(uint32_t now_ms) {
  const uint32_t fetch_started_at = now_ms;
  size_t bytes = 0;
  log_heap(TAG, "cloud.fetch:pre-tls");
  const AgClientResult result = _client.http_fetch_config(_fetch_buf, FETCH_BUFFER_BYTES, &bytes);
  log_heap(TAG, "cloud.fetch:post-tls");

  const size_t logged = bytes < FETCH_BUFFER_BYTES ? bytes : FETCH_BUFFER_BYTES;
  AG_LOGI(TAG, "fetch_config result=%d bytes=%zu body=%.*s", static_cast<int>(result), bytes,
          static_cast<int>(logged), _fetch_buf);

  Event evt{};
  evt.type = EventType::FetchConfigResult;
  evt.cloud_result = static_cast<CloudResultByte>(result);
  RTOS::queue_send(_event_queue, &evt);

  _fetch_due = fetch_started_at + _cfg.fetch_interval_ms;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

MeasuresAGo CloudService::_snapshot_copy() {
  _snapshot_mtx.lock();
  MeasuresAGo out = _latest_snapshot;
  _snapshot_mtx.unlock();
  return out;
}

void CloudService::_wake() {
  if (_task_handle == nullptr) {
    return;
  }
  RTOS::task_notify_send(_task_handle, 0);
}
