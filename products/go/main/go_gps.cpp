/**
 * AirGradient Go — GPS Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_gps.h"

#include "ag_log.h"
#include "go_events.h"
#include "rtos.h"

#include <ctime>

static constexpr const char *TAG = "GpsService";

// Yield interval between GpsSensor::read() calls when the serial buffer is
// empty.  Keeps CPU usage low without introducing latency gaps larger than
// one NMEA epoch (~1 second).
static constexpr uint32_t TASK_YIELD_MS = 10;

static const char *fix_type_str(GpsFixType fix_type) {
  switch (fix_type) {
  case GpsFixType::Fix2D:
    return "2D";
  case GpsFixType::Fix3D:
    return "3D";
  case GpsFixType::NoFix:
  default:
    return "none";
  }
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GpsService::GpsService(GpsSensor &gps, RtosQueueHandle event_queue, const Config &config)
    : _gps(gps), _event_queue(event_queue), _config(config) {}

GpsService::~GpsService() {
  if (_running) {
    stop();
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool GpsService::start() {
  if (!_mutex.is_valid()) {
    return false;
  }
  _clock_synced = false;
  _running = true;
  const bool ok =
      RTOS::task_create(task_entry, "gps_task", static_cast<uint32_t>(_config.task_stack_size),
                        this, static_cast<uint32_t>(_config.task_priority), &_task_handle);
  if (!ok) {
    _running = false;
    return false;
  }
  return true;
}

void GpsService::stop() {
  _running = false;
  if (_task_handle != nullptr && _done_sem.is_created()) {
    _done_sem.take();
    _done_sem.destroy();
    _task_handle = nullptr;
  }
}

GpsData GpsService::get_latest_fix() const {
  _mutex.lock();
  const GpsData snapshot = _latest_fix;
  _mutex.unlock();
  return snapshot;
}

void GpsService::set_posting_interval_ms(int interval_ms) {
  _config.posting_interval_ms = interval_ms;
}

// ---------------------------------------------------------------------------
// Task entry point (static)
// ---------------------------------------------------------------------------

// static
void GpsService::task_entry(void *arg) {
  static_cast<GpsService *>(arg)->run();
  RTOS::task_delete(nullptr);
}

// ---------------------------------------------------------------------------
// Task loop
// ---------------------------------------------------------------------------

void GpsService::run() {
  // Create the done semaphore inside the task so it is valid for the entire
  // task lifetime.  stop() blocks on this semaphore before returning.
  _done_sem.create();

  AG_LOGI(TAG, "start: baud=%d post_interval=%d ms", _config.baud_rate, _config.posting_interval_ms);
  if (!_gps.begin(_config.baud_rate)) {
    AG_LOGE(TAG, "start: gps.begin(%d) failed", _config.baud_rate);
    _running = false;
  }

  const uint64_t acquisition_start_ms = RTOS::get_time_ms();
  uint64_t last_post_ms = 0;
  bool saw_sentence = false;
  bool acquisition_logged = false;

  while (_running) {
    if (_gps.read()) {
      const GpsData data = _gps.get_data();
      update_latest_fix(data);

      if (!saw_sentence) {
        saw_sentence = true;
        AG_LOGI(TAG, "acquisition: first NMEA sentence received");
      }

      if (!_clock_synced && is_gps_timestamp_valid(data.timestamp)) {
        sync_system_clock(data.timestamp);
        _clock_synced = true;
        AG_LOGI(TAG, "clock synced from GPS timestamp %04d-%02d-%02d %02d:%02d:%02d UTC",
                data.timestamp.year, data.timestamp.month, data.timestamp.day, data.timestamp.hour,
                data.timestamp.minute, data.timestamp.second);
      }

      if (!acquisition_logged && is_fix_valid(data.fix)) {
        acquisition_logged = true;
        if (is_position_valid(data.position) && is_altitude_valid(data.altitude_m)) {
          AG_LOGI(TAG,
                  "acquisition: valid %s fix after %lu ms sat=%d lat=%.6f lon=%.6f alt=%.1f m",
                  fix_type_str(data.fix.fix_type),
                  static_cast<unsigned long>(RTOS::get_time_ms() - acquisition_start_ms),
                  data.fix.satellite_count, data.position.latitude, data.position.longitude,
                  data.altitude_m);
        } else {
          AG_LOGI(TAG, "acquisition: valid %s fix after %lu ms sat=%d",
                  fix_type_str(data.fix.fix_type),
                  static_cast<unsigned long>(RTOS::get_time_ms() - acquisition_start_ms),
                  data.fix.satellite_count);
        }
      }
    }

    const uint64_t now_ms = RTOS::get_time_ms();
    if (now_ms - last_post_ms >= static_cast<uint64_t>(_config.posting_interval_ms)) {
      if (_gps.has_valid_fix()) {
        post_fix_event();
        const GpsData data = _gps.get_data();
        AG_LOGI(TAG, "publish: fix=%s sat=%d", fix_type_str(data.fix.fix_type),
                data.fix.satellite_count);
      } else if (saw_sentence) {
        const GpsData data = _gps.get_data();
        AG_LOGI(TAG, "acquisition: waiting for fix sat=%d hdop=%.1f",
                data.fix.satellite_count, data.fix.hdop);
      }
      last_post_ms = now_ms;
    }

    RTOS::delay_ms(TASK_YIELD_MS);
  }

  _gps.end();
  AG_LOGI(TAG, "stop");

  // Signal stop() that the task loop has exited before self-deleting.
  if (_done_sem.is_created()) {
    _done_sem.give();
  }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void GpsService::update_latest_fix(const GpsData &data) {
  _mutex.lock();
  _latest_fix = data;
  _mutex.unlock();
}

void GpsService::post_fix_event() {
  Event evt{};
  evt.type = EventType::GpsFixUpdate;
  evt.gps_data = _gps.get_data();
  // Non-blocking send: drop the event if the queue is full.
  RTOS::queue_send(_event_queue, &evt, 0);
}

void GpsService::sync_system_clock(const GpsTimestamp &ts) {
  // GPS timestamps are UTC.  On ESP-IDF the default timezone is UTC, so
  // mktime() produces the correct POSIX epoch without timezone adjustment.
  struct tm t{};
  t.tm_year = ts.year - 1900;
  t.tm_mon = ts.month - 1;
  t.tm_mday = ts.day;
  t.tm_hour = ts.hour;
  t.tm_min = ts.minute;
  t.tm_sec = ts.second;
  t.tm_isdst = 0;
  const time_t epoch = mktime(&t);
  if (epoch != static_cast<time_t>(-1)) {
    RTOS::set_system_time_from_epoch(static_cast<int64_t>(epoch));
  }
}

// ---------------------------------------------------------------------------
// One-shot synchronous read (fast-path timer wake)
// ---------------------------------------------------------------------------

GpsData gps_read_once(GpsSensor &gps, int baud_rate, uint32_t timeout_ms) {
  gps.begin(baud_rate);
  const uint64_t deadline_ms = RTOS::get_time_ms() + timeout_ms;
  while (RTOS::get_time_ms() < deadline_ms) {
    if (gps.read() && gps.has_valid_fix()) {
      break;
    }
    RTOS::delay_ms(10);
  }
  const GpsData data = gps.get_data();
  gps.end();
  return data;
}
