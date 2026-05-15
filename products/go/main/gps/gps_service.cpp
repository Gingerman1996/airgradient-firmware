/**
 * AirGradient Go — GPS Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "gps/gps_service.h"

#include "ag_log.h"
#include "go_events.h"
#include "rtos.h"

#include <ctime>

static constexpr const char *TAG = "GpsService";

// Yield interval between GpsDriver::read() calls when the serial buffer is
// empty.  Keeps CPU usage low without introducing latency gaps larger than
// one NMEA epoch (~1 second).
static constexpr uint32_t TASK_YIELD_MS = 10;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GpsService::GpsService(GpsDriver &driver, RtosQueueHandle event_queue, const Config &config)
    : _driver(driver), _event_queue(event_queue), _config(config) {}

GpsService::~GpsService() {
  if (_running) {
    stop();
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool GpsService::start() {
  if (_running) {
    return true; // already running — idempotent
  }
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
  AG_LOGI(TAG, "stop: task only (GNSS keeps tracking)");
  _running = false;
  if (_task_handle != nullptr && _done_sem.is_created()) {
    _done_sem.take();
    _done_sem.destroy();
    _task_handle = nullptr;
  }
  _driver.end();
}

void GpsService::stop_and_idle_gnss() {
  AG_LOGI(TAG, "stop_and_idle_gnss: stopping task and GNSS receiver");
  if (!_running && _task_handle == nullptr) {
    // Task already stopped — use one-shot path to ensure module receives
    // GNSS stop even when no task was running.
    idle_gnss();
    return;
  }
  _running = false;
  if (_task_handle != nullptr && _done_sem.is_created()) {
    _done_sem.take();
    _done_sem.destroy();
    _task_handle = nullptr;
  }
  // Serial link is still open — send GNSS stop before closing.
  _driver.gnss_stop();
  _driver.end();
  AG_LOGI(TAG, "GNSS receiver stopped");
}

void GpsService::idle_gnss() {
  AG_LOGI(TAG, "idle_gnss: sending GNSS stop (no task)");
  _driver.begin(_config.baud_rate);
  _driver.gnss_stop();
  _driver.end();
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

void GpsService::set_aiding_data(const GpsAidingData &data) {
  _mutex.lock();
  _aiding_data = data;
  _aiding_pending = true;
  _mutex.unlock();
}

void GpsService::sleep_for_ms(uint32_t duration_ms) {
  if (duration_ms == 0) {
    return; // 0 is "no request" sentinel in _sleep_ms_pending
  }
  _mutex.lock();
  _sleep_ms_pending = duration_ms;
  _mutex.unlock();
  AG_LOGI(TAG, "sleep_for_ms: queued %u ms", static_cast<unsigned>(duration_ms));
}

void GpsService::wake_from_sleep() {
  // The wake handler runs synchronously and only touches the I/O expander —
  // no UART access, so it is safe to call outside the task loop. UART resync
  // is deferred to the task loop via _resync_pending to avoid concurrent
  // access with read() on the next tick.
  AG_LOGI(TAG, "wake_from_sleep: requested");
  _driver.wake_from_sleep();
  _mutex.lock();
  _resync_pending = true;
  _mutex.unlock();
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

  const uint64_t start_ms = RTOS::get_time_ms();
  _driver.begin(_config.baud_rate);
  _driver.gnss_start();
  AG_LOGI(TAG, "GNSS receiver started");

  uint64_t last_post_ms = 0;
  uint64_t last_summary_ms = 0;
  bool ttff_logged = false;
  GpsFixType last_fix_type = GpsFixType::NoFix;

  while (_running) {
    // Check for pending aiding data (set via set_aiding_data() from any
    // thread).  Copy under mutex; inject outside mutex to avoid holding the
    // lock during serial I/O.
    {
      GpsAidingData aid_local;
      bool inject = false;
      _mutex.lock();
      if (_aiding_pending) {
        aid_local = _aiding_data;
        _aiding_pending = false;
        inject = true;
      }
      _mutex.unlock();
      if (inject) {
        AG_LOGI(TAG, "Inject aiding: lat=%.6f lon=%.6f alt=%.1f acc=%.0f epoch=%lld tacc=%u",
                aid_local.latitude, aid_local.longitude, aid_local.altitude_m, aid_local.pos_acc_m,
                static_cast<long long>(aid_local.epoch_s),
                static_cast<unsigned>(aid_local.time_acc_ms));
        if (!_clock_synced && has_aiding_time(aid_local)) {
          RTOS::set_system_time_from_epoch(aid_local.epoch_s);
          // Do not set _clock_synced: GPS-derived time from RMC is more accurate
        }
        _driver.inject_aiding(aid_local);
      }
    }

    // Pending CFG-SLEEP request — drain under mutex, send on UART outside it.
    // Co-located with aiding injection so the task loop is the sole UART writer.
    {
      uint32_t sleep_local = 0;
      _mutex.lock();
      if (_sleep_ms_pending != 0) {
        sleep_local = _sleep_ms_pending;
        _sleep_ms_pending = 0;
      }
      _mutex.unlock();
      if (sleep_local != 0) {
        _driver.sleep_for_ms(sleep_local);
        // After CFG-SLEEP, the TAU1113's UART resets to 9600 baud on wake.
        // Schedule a resync just past the timer expiry (1.5 s margin for
        // module bring-up) so the link is restored even if no one calls
        // wake_from_sleep() — i.e., the timer-only path.
        _sleep_until_ms = RTOS::get_time_ms() + sleep_local + 1500;
      }
    }

    if (_driver.read()) {
      const GpsData data = _driver.get_data();
      update_latest_fix(data);

      // Fix-state transition log: surfaces every NoFix↔2D↔3D change so a
      // brief reception drop-out is obvious in the trace.
      if (data.fix.fix_type != last_fix_type) {
        AG_LOGI(TAG, "fix state: %d -> %d (sat=%d hdop=%.1f)",
                static_cast<int>(last_fix_type), static_cast<int>(data.fix.fix_type),
                data.fix.satellite_count, static_cast<double>(data.fix.hdop));
        last_fix_type = data.fix.fix_type;
      }

      // TTFF (time-to-first-fix) — one-shot marker on first valid fix after
      // this task started. Useful for comparing cold-start vs aided-start.
      if (!ttff_logged && is_fix_valid(data.fix)) {
        const uint64_t ttff_ms = RTOS::get_time_ms() - start_ms;
        AG_LOGI(TAG, "TTFF: %llu ms (fix=%d sat=%d hdop=%.1f)",
                static_cast<unsigned long long>(ttff_ms),
                static_cast<int>(data.fix.fix_type), data.fix.satellite_count,
                static_cast<double>(data.fix.hdop));
        ttff_logged = true;
      }

      if (!_clock_synced && is_gps_timestamp_valid(data.timestamp)) {
        sync_system_clock(data.timestamp);
        _clock_synced = true;
      }
    }

    const uint64_t now_ms = RTOS::get_time_ms();
    if (now_ms - last_post_ms >= static_cast<uint64_t>(_config.posting_interval_ms)) {
      if (_driver.has_valid_fix()) {
        post_fix_event();
      }
      last_post_ms = now_ms;
    }

    // Drain host-wake resync request (signaled by public wake_from_sleep()).
    bool host_wake_resync = false;
    _mutex.lock();
    if (_resync_pending) {
      host_wake_resync = true;
      _resync_pending = false;
    }
    _mutex.unlock();
    if (host_wake_resync) {
      AG_LOGI(TAG, "host-wake: resyncing UART");
      _driver.resync_after_wake();
      // Intentionally do NOT clear _sleep_until_ms here: if the host-wake
      // pulse failed to actually wake the module (e.g. wake-source byte
      // mismatch), the deadline-based resync at timer expiry still fires
      // and recovers the link. Two resyncs are harmless (idempotent).
    }

    // Auto-resync after a sleep timer expires. Fires once when the deadline
    // (set in the sleep-request drain above) has passed and we haven't been
    // host-woken in the meantime. Resets to 0 after one resync per sleep.
    if (_sleep_until_ms != 0 && now_ms >= _sleep_until_ms) {
      AG_LOGI(TAG, "sleep deadline reached, auto-resyncing UART");
      _driver.resync_after_wake();
      _sleep_until_ms = 0;
    }

    // Periodic summary every 10 s — only visible when this tag is at debug
    // level. Cheap pulse to confirm the task is alive and tracking quality.
    if (now_ms - last_summary_ms >= 10000) {
      const GpsData snapshot = _driver.get_data();
      AG_LOGD(TAG, "summary: fix=%d sat=%d hdop=%.1f synced=%d",
              static_cast<int>(snapshot.fix.fix_type), snapshot.fix.satellite_count,
              static_cast<double>(snapshot.fix.hdop), _clock_synced ? 1 : 0);
      last_summary_ms = now_ms;
    }

    // Sleep / wake is triggered by the admin "GPS Sleep Test" menu item; the
    // run loop drains _sleep_ms_pending above and the deadline-based
    // auto-resync below recovers UART comms after the module's own wake.

    RTOS::delay_ms(TASK_YIELD_MS);
  }

  // Signal stop()/stop_and_idle_gnss() that the task loop has exited before
  // self-deleting.  The caller controls the shutdown sequence: serial link
  // remains open so the caller can optionally send GNSS stop before end().
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
  evt.gps_data = _driver.get_data();

  AG_LOGD(TAG, "fix: lat=%.6f lon=%.6f alt=%.1f fix=%d sat=%d hdop=%.1f",
          evt.gps_data.position.latitude, evt.gps_data.position.longitude, evt.gps_data.altitude_m,
          static_cast<int>(evt.gps_data.fix.fix_type), evt.gps_data.fix.satellite_count,
          evt.gps_data.fix.hdop);

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

GpsData gps_read_once(GpsDriver &driver, int baud_rate, uint32_t timeout_ms,
                      const volatile bool &abort) {
  driver.begin(baud_rate);
  driver.gnss_start(); // defensive: ensures module is tracking
  const uint64_t deadline_ms = RTOS::get_time_ms() + timeout_ms;
  while (RTOS::get_time_ms() < deadline_ms && !abort) {
    if (driver.read() && driver.has_valid_fix()) {
      break;
    }
    RTOS::delay_ms(10);
  }
  const GpsData data = driver.get_data();
  driver.end();
  return data;
}
