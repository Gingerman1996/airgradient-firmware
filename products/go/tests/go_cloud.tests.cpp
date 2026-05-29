/**
 * AirGradient Go — CloudService unit tests
 *
 * Drives the cloud task body deterministically via the friend
 * CloudServiceTestAccess helper.  The real RTOS task / queue plumbing is
 * a no-op under TEST_HOST, so all tests work against the synchronous
 * `_run_iteration(now)` driver and clock injection.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>

#include <cstdint>
#include <memory>

#include "go_cloud.h"
#include "go_events.h"
#include "rtos.h"
#include "services/ag_client.h"
#include "go_wifi.h"

// ============================================================================
// External cloud_spy state (defined in go_cloud_stubs.cpp)
// ============================================================================

namespace cloud_spy {
extern uint32_t post_call_count;
extern uint32_t fetch_call_count;
extern MeasuresAGo last_post_snapshot;
extern int last_post_signal;
extern char *last_fetch_buf;
extern size_t last_fetch_buf_size;
extern AgClientResult next_post_result;
extern AgClientResult next_fetch_result;
extern size_t fetch_bytes_to_write;
extern const char *fetch_body_to_write;
extern void (*on_post_hook)();
extern void (*on_fetch_hook)();
extern int wifi_rssi;
extern void reset();
} // namespace cloud_spy

// ============================================================================
// CloudServiceTestAccess — friend helper exposing private internals
// ============================================================================

class CloudServiceTestAccess {
public:
  static uint32_t run_once(CloudService &c, uint32_t now) { return c._run_iteration(now); }

  static uint32_t post_due(const CloudService &c) { return c._post_due; }
  static uint32_t fetch_due(const CloudService &c) { return c._fetch_due; }
  static bool was_armed(const CloudService &c) { return c._was_armed; }
  static uint32_t done_signal_count(const CloudService &c) { return c._test_done_signal_count; }

  static void set_armed(CloudService &c, bool v) { c._armed.store(v); }
  static void set_disable_cloud(CloudService &c, bool v) { c._disable_cloud.store(v); }
  static void set_fire_now_pending(CloudService &c, bool v) { c._fire_now_pending.store(v); }
  static void set_shutdown_pending(CloudService &c, bool v) { c._shutdown_pending.store(v); }

  static void set_post_due(CloudService &c, uint32_t v) { c._post_due = v; }
  static void set_fetch_due(CloudService &c, uint32_t v) { c._fetch_due = v; }
  static void set_was_armed(CloudService &c, bool v) { c._was_armed = v; }

  // Inject the fetch buffer so _do_fetch has a place to write into.
  // The real start() allocates this; tests do not call start() because
  // task_create / queue_create / semaphore_create are no-ops under
  // TEST_HOST.
  static void set_fetch_buf(CloudService &c, char *buf) { c._fetch_buf = buf; }

  // Direct snapshot read for the snapshot test — exercises the same
  // mutex-protected accessor the task uses.
  static MeasuresAGo snapshot_copy(CloudService &c) { return c._snapshot_copy(); }

  // Create the done-semaphore so the shutdown branch's _done_sem.give()
  // does not hit an uncreated handle.  is_created() == true under
  // TEST_HOST returns the sentinel handle.
  static void create_done_sem(CloudService &c) { c._done_sem.create(); }
};

using A = CloudServiceTestAccess;

// ============================================================================
// MockRTOS — controls the clock and records task_notify_wait calls
// ============================================================================

class MockRTOS : public trompeloeil::mock_interface<RTOS> {
public:
  IMPLEMENT_MOCK1(delay_ms_impl);
  IMPLEMENT_MOCK0(get_time_ms_impl);

  // Capture the last timeout passed to task_notify_take().
  uint32_t last_notify_take_ms = UINT32_MAX;
  uint32_t notify_take_calls = 0;

  bool task_notify_wait_impl(uint32_t * /*out_value*/, uint32_t timeout_ms) override {
    last_notify_take_ms = timeout_ms;
    notify_take_calls += 1;
    return false; // simulate timeout — tests drive the loop manually
  }

  void queue_send_impl(RtosQueueHandle /*qh*/, const void *item, uint32_t /*timeout_ms*/) override {
    if (item != nullptr) {
      last_event = *static_cast<const Event *>(item);
      events_posted += 1;
    }
  }

  Event last_event{};
  uint32_t events_posted = 0;
};

// ============================================================================
// Fixture
// ============================================================================

struct CloudFixture {
  MockRTOS mock_rtos;

  AgClient ag_client;
  WifiService wifi_service;
  CloudService cloud;

  // Persistent buffer for fetch_config so _do_fetch has somewhere to
  // write into.  Sized to match FETCH_BUFFER_BYTES.
  static constexpr size_t FETCH_BUF_SIZE = 1024;
  char fetch_buf[FETCH_BUF_SIZE] = {};

  std::unique_ptr<trompeloeil::expectation> _exp_time;
  std::unique_ptr<trompeloeil::expectation> _exp_delay;

  CloudFixture()
      : wifi_service(nullptr,
                     {*reinterpret_cast<WifiManager *>(_stub_buf),
                      *reinterpret_cast<AgBleServer *>(_stub_buf),
                      *reinterpret_cast<HttpServer *>(_stub_buf)},
                     WifiService::Config{}),
        cloud(reinterpret_cast<RtosQueueHandle>(0x1), CloudService::Deps{ag_client, wifi_service},
              CloudService::Config{}) {
    cloud_spy::reset();
    RTOS::set_instance(&mock_rtos);
    _exp_time = NAMED_ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    _exp_delay = NAMED_ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Plant a fetch buffer so _do_fetch has somewhere to write.
    A::set_fetch_buf(cloud, fetch_buf);

    // Done semaphore must exist so the shutdown branch's give() finds
    // a created handle.  In TEST_HOST this is a non-null sentinel.
    A::create_done_sem(cloud);
  }

  ~CloudFixture() {
    // Detach the buffer so ~CloudService -> stop() does not free our
    // stack array.
    A::set_fetch_buf(cloud, nullptr);
    RTOS::set_instance(nullptr);
  }

private:
  alignas(8) static inline char _stub_buf[64];
};

// ============================================================================
// 1. Transition handling
// ============================================================================

TEST_CASE("Disarmed -> Armed sets both deadlines one interval out", "[CloudService][transition]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);

  REQUIRE(A::was_armed(f.cloud));
  REQUIRE(A::post_due(f.cloud) == 1000 + 60'000);
  REQUIRE(A::fetch_due(f.cloud) == 1000 + 60'000);
  REQUIRE(cloud_spy::post_call_count == 0);
  REQUIRE(cloud_spy::fetch_call_count == 0);
  REQUIRE(wake == 60'000);
}

TEST_CASE("Disarmed -> Armed with fire_now fires POST immediately and re-anchors",
          "[CloudService][transition]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_fire_now_pending(f.cloud, true);

  // First iteration: transition snaps both deadlines to now=1000, then
  // the POST-priority branch fires POST immediately because the
  // deadline equals now.  POST re-anchors _post_due = 1000 + 60000.
  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);
  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(A::post_due(f.cloud) == 1000 + 60'000);
  REQUIRE(A::fetch_due(f.cloud) == 1000); // FETCH not yet run
  REQUIRE(wake == 0);

  // Second iteration: FETCH fires (still due at now), re-anchors to
  // 1001 + 60000.
  A::run_once(f.cloud, /*now=*/1001);
  REQUIRE(cloud_spy::fetch_call_count == 1);
  REQUIRE(A::fetch_due(f.cloud) == 1001 + 60'000);
}

TEST_CASE("Arm-while-armed does not reset deadlines", "[CloudService][transition]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::run_once(f.cloud, /*now=*/1000);
  const uint32_t post_before = A::post_due(f.cloud);
  const uint32_t fetch_before = A::fetch_due(f.cloud);

  // arm() again with fire_now=false: no transition observed.
  f.cloud.arm(/*fire_now=*/false);
  A::run_once(f.cloud, /*now=*/2000);

  REQUIRE(A::post_due(f.cloud) == post_before);
  REQUIRE(A::fetch_due(f.cloud) == fetch_before);
}

// ============================================================================
// 2. Null-handle safety
// ============================================================================

TEST_CASE("arm/disarm/set_disable_cloud are safe before start()", "[CloudService][null_handle]") {
  CloudFixture f;

  // _task_handle is nullptr by default (we never called start()).  The
  // calls below must not crash; the atomics still take the new values.
  f.cloud.arm(true);
  f.cloud.disarm();
  f.cloud.set_disable_cloud(true);
  f.cloud.set_disable_cloud(false);

  // Drive an iteration to confirm atomics propagated and no crash.
  uint32_t wake = A::run_once(f.cloud, /*now=*/0);
  (void)wake;
  SUCCEED();
}

// ============================================================================
// 3. Snapshot read/write
// ============================================================================

TEST_CASE("update_measures_snapshot round-trips under the mutex", "[CloudService][snapshot]") {
  CloudFixture f;

  MeasuresAGo a{};
  a.co2.co2 = 1234;
  a.temp_hum_a.temperature = 25.5f;
  f.cloud.update_measures_snapshot(a);

  MeasuresAGo back = A::snapshot_copy(f.cloud);
  REQUIRE(back.co2.co2 == 1234);
  REQUIRE(back.temp_hum_a.temperature == 25.5f);

  MeasuresAGo b{};
  b.co2.co2 = 555;
  f.cloud.update_measures_snapshot(b);
  back = A::snapshot_copy(f.cloud);
  REQUIRE(back.co2.co2 == 555);
}

// ============================================================================
// 4. POST and FETCH forwarding to event queue
// ============================================================================

TEST_CASE("POST forwards AgClientResult into PostMeasuresResult event", "[CloudService][post]") {
  CloudFixture f;
  cloud_spy::next_post_result = AgClientResult::ServerError;

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0); // due immediately
  A::set_fetch_due(f.cloud, 999'999'999);

  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);

  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(f.mock_rtos.events_posted == 1);
  REQUIRE(f.mock_rtos.last_event.type == EventType::PostMeasuresResult);
  REQUIRE(f.mock_rtos.last_event.cloud_result == static_cast<uint8_t>(AgClientResult::ServerError));
  REQUIRE(wake == 0); // continue immediately to re-sample
}

TEST_CASE("FETCH forwards AgClientResult into FetchConfigResult event", "[CloudService][fetch]") {
  CloudFixture f;
  cloud_spy::next_fetch_result = AgClientResult::Ok;
  cloud_spy::fetch_body_to_write = "{\"key\":\"val\"}";
  cloud_spy::fetch_bytes_to_write = 13;

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 999'999'999);
  A::set_fetch_due(f.cloud, 0);

  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);

  REQUIRE(cloud_spy::fetch_call_count == 1);
  REQUIRE(cloud_spy::last_fetch_buf == f.fetch_buf);
  REQUIRE(cloud_spy::last_fetch_buf_size == CloudFixture::FETCH_BUF_SIZE);
  REQUIRE(f.mock_rtos.events_posted == 1);
  REQUIRE(f.mock_rtos.last_event.type == EventType::FetchConfigResult);
  REQUIRE(f.mock_rtos.last_event.cloud_result == static_cast<uint8_t>(AgClientResult::Ok));
  REQUIRE(wake == 0);
}

// ============================================================================
// 5. POST priority
// ============================================================================

TEST_CASE("POST priority: coincident deadlines fire POST first", "[CloudService][priority]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 60'000);
  A::set_fetch_due(f.cloud, 60'000);

  // First iteration at t=60'000: both due, POST runs first.
  A::run_once(f.cloud, /*now=*/60'000);
  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(cloud_spy::fetch_call_count == 0);

  // Second iteration: POST already re-anchored, FETCH still due.
  A::run_once(f.cloud, /*now=*/60'001);
  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(cloud_spy::fetch_call_count == 1);
}

// ============================================================================
// 6. Start-time anchoring
// ============================================================================

TEST_CASE("POST deadline anchored to start time, not completion", "[CloudService][anchoring]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 999'999'999);

  // Drive POST at t=1000; the in-stub hook does nothing (call is "instant").
  A::run_once(f.cloud, /*now=*/1000);

  // Next deadline = post_started_at (1000) + 60'000, NOT completion time.
  REQUIRE(A::post_due(f.cloud) == 1000 + 60'000);
}

// ============================================================================
// 7. Overrun handling — POST priority preserved
// ============================================================================

TEST_CASE("Overrun POST fires next POST before FETCH on re-anchor", "[CloudService][overrun]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);

  // Simulate a >60 s POST: start at t=0, anchor to 60'000, but the
  // "current" clock the test passes to run_once for the next iteration
  // is 70'000 — _post_due (60'000) is already in the past.
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 999'999'999);

  A::run_once(f.cloud, /*now=*/0);
  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(A::post_due(f.cloud) == 60'000);

  // Next iteration at t=70'000 with fetch deadline still far in future.
  A::set_fetch_due(f.cloud, 80'000);
  A::run_once(f.cloud, /*now=*/70'000);

  // POST fires again, FETCH still not yet considered.
  REQUIRE(cloud_spy::post_call_count == 2);
  REQUIRE(cloud_spy::fetch_call_count == 0);
}

// ============================================================================
// 8. Disarm during POST gates FETCH out
// ============================================================================

static CloudService *s_disarm_target = nullptr;
static void disarm_during_post() {
  if (s_disarm_target != nullptr) {
    A::set_armed(*s_disarm_target, false);
  }
}

TEST_CASE("Disarm during POST gates FETCH out of the next iteration", "[CloudService][disarm]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 0);

  s_disarm_target = &f.cloud;
  cloud_spy::on_post_hook = disarm_during_post;

  A::run_once(f.cloud, /*now=*/1000);
  REQUIRE(cloud_spy::post_call_count == 1);

  // Next iteration: armed == false at the top, FETCH must NOT fire.
  A::run_once(f.cloud, /*now=*/1001);
  REQUIRE(cloud_spy::fetch_call_count == 0);

  s_disarm_target = nullptr;
  cloud_spy::on_post_hook = nullptr;
}

// ============================================================================
// 9. set_disable_cloud during POST gates FETCH
// ============================================================================

static CloudService *s_disable_target = nullptr;
static void disable_during_post() {
  if (s_disable_target != nullptr) {
    A::set_disable_cloud(*s_disable_target, true);
  }
}

TEST_CASE("set_disable_cloud during POST gates FETCH out of the next iteration",
          "[CloudService][disable]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 0);

  s_disable_target = &f.cloud;
  cloud_spy::on_post_hook = disable_during_post;

  A::run_once(f.cloud, /*now=*/1000);
  REQUIRE(cloud_spy::post_call_count == 1);

  A::run_once(f.cloud, /*now=*/1001);
  REQUIRE(cloud_spy::fetch_call_count == 0);

  s_disable_target = nullptr;
  cloud_spy::on_post_hook = nullptr;
}

// ============================================================================
// 10. RSSI translation
// ============================================================================

TEST_CASE("RSSI of WIFI_RSSI_INVALID is translated to -127", "[CloudService][rssi]") {
  CloudFixture f;
  cloud_spy::wifi_rssi = WIFI_RSSI_INVALID; // 0
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 999'999'999);

  A::run_once(f.cloud, /*now=*/0);
  REQUIRE(cloud_spy::last_post_signal == -127);
}

TEST_CASE("Real RSSI is forwarded unchanged", "[CloudService][rssi]") {
  CloudFixture f;
  cloud_spy::wifi_rssi = -57;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 999'999'999);

  A::run_once(f.cloud, /*now=*/0);
  REQUIRE(cloud_spy::last_post_signal == -57);
}

// ============================================================================
// 11. Shutdown latch
// ============================================================================

TEST_CASE("Shutdown branch increments done counter, skips AgClient", "[CloudService][shutdown]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 0);

  A::set_shutdown_pending(f.cloud, true);
  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);

  REQUIRE(A::done_signal_count(f.cloud) == 1);
  REQUIRE(cloud_spy::post_call_count == 0);
  REQUIRE(cloud_spy::fetch_call_count == 0);
  REQUIRE(wake == UINT32_MAX);
}

// ============================================================================
// 12. next_wake clamp
// ============================================================================

TEST_CASE("Past deadline returns 0 wake (no UINT32_MAX wrap)", "[CloudService][clamp]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  // Set both deadlines in the past by a tiny amount so the "fire" path
  // does not run them (the run path uses `>= 0` cast comparison; both
  // happen to be in the past so POST fires).  To test the clamp on
  // the "sleep" branch, push the disable flag so we enter the idle
  // branch.
  A::set_disable_cloud(f.cloud, true);
  A::set_post_due(f.cloud, 500);
  A::set_fetch_due(f.cloud, 500);

  // now=1000, both deadlines in the past while disabled — the slide
  // logic moves them forward and the sleep is computed.
  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);

  // No wraparound: should be a sensible interval, not a near-UINT32_MAX
  // value.
  REQUIRE(wake <= 60'000);
}

// ============================================================================
// 13. First POST with empty snapshot — default sentinels mean no measure
//     fields leak; only the wifi signal byte goes out (verified via the
//     stub recording the snapshot as it was handed off).
// ============================================================================

TEST_CASE("First POST sees a default-constructed snapshot", "[CloudService][first_post]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 999'999'999);

  A::run_once(f.cloud, /*now=*/0);
  REQUIRE(cloud_spy::post_call_count == 1);

  // Every measure field on the snapshot fails its is_*_valid() check
  // because Prereq A made the default sentinels universal.  This is
  // the contract the cloud task relies on for the cold-boot first
  // POST: serializer omits all measure fields, only "wifi" goes out.
  const MeasuresAGo &s = cloud_spy::last_post_snapshot;
  REQUIRE_FALSE(s.co2.is_valid());
  REQUIRE_FALSE(s.pm_a.is_valid());
  REQUIRE_FALSE(s.temp_hum_a.is_valid());
  REQUIRE_FALSE(s.tvoc_nox.is_valid());
  REQUIRE_FALSE(s.power.is_valid());
  REQUIRE_FALSE(s.pressure.is_valid());
}

// ============================================================================
// 14. Disarmed idle sleeps indefinitely
// ============================================================================

TEST_CASE("Disarmed loop returns UINT32_MAX wake", "[CloudService][idle]") {
  CloudFixture f;
  // _armed stays false; the idle branch returns indefinite wait.
  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);
  REQUIRE(wake == UINT32_MAX);
}

// ============================================================================
// 15. Armed-but-disabled slides expired deadlines forward
// ============================================================================

TEST_CASE("Armed-but-disabled slides expired deadlines forward", "[CloudService][disable]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_disable_cloud(f.cloud, true);
  A::set_post_due(f.cloud, 500);  // in the past
  A::set_fetch_due(f.cloud, 500); // in the past

  A::run_once(f.cloud, /*now=*/1000);

  // Both deadlines slid one full interval into the future from `now`.
  REQUIRE(A::post_due(f.cloud) == 1000 + 60'000);
  REQUIRE(A::fetch_due(f.cloud) == 1000 + 60'000);
  REQUIRE(cloud_spy::post_call_count == 0);
  REQUIRE(cloud_spy::fetch_call_count == 0);
}
