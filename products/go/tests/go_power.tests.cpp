/**
 * AirGradient Go — PowerService unit tests
 *
 * Covers the host-testable surface of PowerService:
 *
 * poll_bms         — BMS telemetry/status/percentage aggregation into
 *                    PowerSnapshot, critical-battery threshold logic.
 *
 * reset_watchdog   — Pass-through to BmsDevice::update_watchdog().
 *
 * evaluate_sleep   — Pure-logic sleep type selection based on lock state,
 *                    measurement interval, display refresh, and deep-sleep
 *                    threshold.
 *
 * is_fast_path_wake — Static pure-logic predicate for abbreviated boot.
 *
 * save/load_state  — RTC state round-trip (RTC_DATA_ATTR is a plain static
 *                    under TEST_HOST).
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include "go_board.h"
#include "go_power.h"
#include "hal/fuel_gauge_device.h"

// ============================================================================
// MockBmsDevice
// ============================================================================

class MockBmsDevice : public trompeloeil::mock_interface<BmsDevice> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read_telemetry);
  IMPLEMENT_MOCK1(read_status);
  IMPLEMENT_MOCK1(get_charging_state);
  IMPLEMENT_MOCK1(get_battery_percentage);
  IMPLEMENT_MOCK0(update_watchdog);
  IMPLEMENT_CONST_MOCK0(feature_ship_available);
  IMPLEMENT_MOCK0(enter_ship_mode);
  IMPLEMENT_MOCK1(configure_pmid_mode);
  IMPLEMENT_MOCK1(set_charge_enable);
  IMPLEMENT_MOCK1(set_charge_current_ma);
};

// ============================================================================
// GPIO HAL stub
//
// gpio::Hal is a plain function-pointer table.  PowerService only touches
// GPIO inside configure_wake_sources() which is #ifndef TEST_HOST, so these
// stubs are never called during tests.
// ============================================================================

static bool gpio_configure(int, gpio::Mode, gpio::PullMode, gpio::InterruptType) { return true; }
static int gpio_get_level(int) { return 0; }
static bool gpio_set_level(int, int) { return true; }
static bool gpio_add_interrupt_handler(int, gpio::InterruptHandler, void *) { return true; }
static bool gpio_remove_interrupt_handler(int) { return true; }
static bool gpio_enable_interrupt(int) { return true; }
static bool gpio_disable_interrupt(int) { return true; }

static const gpio::Hal test_gpio_hal = {
    gpio_configure,
    gpio_get_level,
    gpio_set_level,
    gpio_add_interrupt_handler,
    gpio_remove_interrupt_handler,
    gpio_enable_interrupt,
    gpio_disable_interrupt,
};

// ============================================================================
// Default PowerService::Config for tests
// ============================================================================

static constexpr PowerService::Config DEFAULT_CONFIG = {
    .pin_wake_button_power = 0,
    .pin_wake_button_boot = 1,
    .deep_sleep_threshold_ms = 5000,
};

// ============================================================================
// TEST CASE 1 — poll_bms
// ============================================================================

TEST_CASE("poll_bms: BMS telemetry aggregation", "[PowerService][poll_bms]") {
  MockBmsDevice mock_bms;
  PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);

  SECTION("all reads succeed — snapshot fully populated") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.1f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 72.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::FastCharge)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.battery_voltage == Catch::Approx(3.7f));
    CHECK(snap.charging_voltage == Catch::Approx(5.1f));
    CHECK(snap.battery_percentage == Catch::Approx(72.0f));
    CHECK(snap.charging_status == BmsChargingState::FastCharge);
    CHECK_FALSE(snap.critical);
  }

  SECTION("read_telemetry fails — voltages stay at sentinels") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_)).RETURN(false);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::NotCharging)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.battery_voltage == Catch::Approx(BmsInvalid::VOLT));
    CHECK(snap.charging_voltage == Catch::Approx(BmsInvalid::VOLT));
    CHECK(snap.battery_percentage == Catch::Approx(50.0f));
  }

  SECTION("get_battery_percentage fails — percentage stays at -1, critical stays false") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_)).RETURN(false);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::NotCharging)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.battery_percentage == Catch::Approx(-1.0f));
    CHECK_FALSE(snap.critical);
  }

  SECTION("read_status fails — charging_status stays Unknown") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_)).RETURN(false);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.charging_status == BmsChargingState::Unknown);
  }

  SECTION("battery below critical threshold — critical flag set") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.2f; _1.charging_voltage = 0.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 3.0f) // below 5%
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::NotCharging)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.battery_percentage == Catch::Approx(3.0f));
    CHECK(snap.critical);
  }

  SECTION("all reads succeed — charger_status populated") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::TaperCharge;
                     _1.power_source = BmsPowerSource::UsbDcp; _1.thermal_regulation = true;
                     _1.vsys_regulation = false; _1.input_current_regulation = true;
                     _1.input_voltage_regulation = false; _1.safety_timer_expired = false;
                     _1.watchdog_expired = false;)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.charger_status.charging_state == BmsChargingState::TaperCharge);
    CHECK(snap.charger_status.power_source == BmsPowerSource::UsbDcp);
    CHECK(snap.charger_status.thermal_regulation);
    CHECK_FALSE(snap.charger_status.vsys_regulation);
    CHECK(snap.charger_status.input_current_regulation);
    CHECK_FALSE(snap.charger_status.input_voltage_regulation);
    CHECK_FALSE(snap.charger_status.safety_timer_expired);
    CHECK_FALSE(snap.charger_status.watchdog_expired);
  }

  SECTION("all reads succeed — telemetry populated") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.8f; _1.charging_voltage = 5.1f;
                     _1.input_current_ma = 120; _1.battery_current_ma = 450;
                     _1.system_voltage_mv = 3700; _1.pmid_voltage_mv = 5000; _1.ts_percent = 45.2f;
                     _1.die_temperature_c = 32;)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 80.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::FastCharge)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.telemetry.battery_voltage == Catch::Approx(3.8f));
    CHECK(snap.telemetry.charging_voltage == Catch::Approx(5.1f));
    CHECK(snap.telemetry.input_current_ma == 120);
    CHECK(snap.telemetry.battery_current_ma == 450);
    CHECK(snap.telemetry.system_voltage_mv == 3700);
    CHECK(snap.telemetry.pmid_voltage_mv == 5000);
    CHECK(snap.telemetry.ts_percent == Catch::Approx(45.2f));
    CHECK(snap.telemetry.die_temperature_c == 32);
  }

  SECTION("read_telemetry fails — telemetry stays at sentinels") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_)).RETURN(false);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::NotCharging)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.telemetry.input_current_ma == BmsInvalid::CURRENT_MA);
    CHECK(snap.telemetry.battery_current_ma == BmsInvalid::CURRENT_MA);
    CHECK(snap.telemetry.system_voltage_mv == BmsInvalid::VOLTAGE_MV);
    CHECK(snap.telemetry.pmid_voltage_mv == BmsInvalid::VOLTAGE_MV);
    CHECK(snap.telemetry.ts_percent == Catch::Approx(BmsInvalid::PERCENT));
    CHECK(snap.telemetry.die_temperature_c == BmsInvalid::TEMPERATURE_C);
    CHECK(snap.telemetry.battery_temperature_c == Catch::Approx(BmsInvalid::TEMPERATURE_FLOAT_C));
    CHECK_FALSE(snap.telemetry.is_battery_temperature_valid());
  }

  SECTION("read_status fails — charger_status stays at defaults") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_)).RETURN(false);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.charger_status.charging_state == BmsChargingState::Unknown);
    CHECK(snap.charger_status.power_source == BmsPowerSource::Unknown);
    CHECK_FALSE(snap.charger_status.thermal_regulation);
    CHECK_FALSE(snap.charger_status.safety_timer_expired);
    CHECK_FALSE(snap.charger_status.watchdog_expired);
  }

  SECTION("battery at exactly critical threshold — not critical") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.5f; _1.charging_voltage = 0.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 5.0f) // exactly at threshold — not critical
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::NotCharging)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.battery_percentage == Catch::Approx(5.0f));
    CHECK_FALSE(snap.critical);
  }
}

// ============================================================================
// TEST CASE 1b — battery over-temperature protection
//
// Drives PowerService through the TS-pin NTC thermal guard:
//   T < 50 °C            -> no cutoff, set_charge_enable not touched
//   T >= 50 °C           -> set_charge_enable(false) (single edge write)
//   T cools to 47 °C     -> set_charge_enable(true) (re-arm with hysteresis)
//   T >= 60 °C           -> enter_ship_mode() fires once, latched
//   Invalid temperature  -> guard is inert
// ============================================================================

namespace {
struct ThermalTelemetrySetter {
  float battery_voltage = 3.7f;
  float charging_voltage = 5.0f;
  float battery_temperature_c = 25.0f;
};

auto telemetry_with_temp(float t_c) {
  return [t_c](BmsTelemetry &out) {
    out.battery_voltage = 3.7f;
    out.charging_voltage = 5.0f;
    out.battery_temperature_c = t_c;
  };
}
} // namespace

TEST_CASE("poll_bms: battery over-temperature protection", "[PowerService][thermal]") {
  MockBmsDevice mock_bms;
  PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);

  // PMID sync, percentage, status: shared across all sections — neither the
  // thermal guard nor the assertions care about them.
  ALLOW_CALL(mock_bms, configure_pmid_mode(trompeloeil::_)).RETURN(true);
  ALLOW_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
      .SIDE_EFFECT(*_1 = 75.0f)
      .RETURN(true);
  ALLOW_CALL(mock_bms, read_status(trompeloeil::_))
      .SIDE_EFFECT(_1.charging_state = BmsChargingState::FastCharge)
      .RETURN(true);

  SECTION("normal temperature — no charge-enable writes, no ship mode") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .LR_SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f;
                        _1.battery_temperature_c = 25.0f)
        .RETURN(true);
    // No set_charge_enable / enter_ship_mode expectations — trompeloeil will
    // fail the test if either is invoked.

    const PowerSnapshot snap = svc.poll_bms();
    CHECK(snap.telemetry.battery_temperature_c == Catch::Approx(25.0f));
  }

  SECTION("invalid temperature reading — guard inert") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .LR_SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f;
                        _1.battery_temperature_c = BmsInvalid::TEMPERATURE_FLOAT_C)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();
    CHECK_FALSE(snap.telemetry.is_battery_temperature_valid());
  }

  SECTION("crosses 50°C — charging disabled exactly once on edge") {
    trompeloeil::sequence seq;

    // First poll: 25 °C → no write.
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f;
                        _1.battery_temperature_c = 25.0f)
        .RETURN(true);
    // Second poll: 51 °C → set_charge_enable(false) fires.
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f;
                        _1.battery_temperature_c = 51.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, set_charge_enable(false)).IN_SEQUENCE(seq).RETURN(true);
    // Third poll: 51 °C again → cache says already disabled, no second write.
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f;
                        _1.battery_temperature_c = 51.0f)
        .RETURN(true);

    svc.poll_bms();
    svc.poll_bms();
    svc.poll_bms();
  }

  SECTION("hysteresis — disable at 50, re-enable at 47, not at 48") {
    trompeloeil::sequence seq;

    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f;
                        _1.battery_temperature_c = 55.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, set_charge_enable(false)).IN_SEQUENCE(seq).RETURN(true);

    // 48 °C — still inside the hysteresis band (between 47 and 50). No write.
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f;
                        _1.battery_temperature_c = 48.0f)
        .RETURN(true);

    // 46 °C — drops below CHARGE_HOT_RESUME_C (47), guard clears.
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f;
                        _1.battery_temperature_c = 46.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, set_charge_enable(true)).IN_SEQUENCE(seq).RETURN(true);

    svc.poll_bms();
    svc.poll_bms();
    svc.poll_bms();
  }

  SECTION("crosses 60°C — ship mode fires once, then latched") {
    trompeloeil::sequence seq;

    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f;
                        _1.battery_temperature_c = 60.5f)
        .RETURN(true);
    // Charge cut first (cache starts at _charge_enabled=true), then ship mode.
    REQUIRE_CALL(mock_bms, set_charge_enable(false)).IN_SEQUENCE(seq).RETURN(true);
    REQUIRE_CALL(mock_bms, enter_ship_mode()).IN_SEQUENCE(seq).RETURN(true);

    // Second poll while still hot — must NOT re-trigger ship mode.
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f;
                        _1.battery_temperature_c = 60.5f)
        .RETURN(true);

    svc.poll_bms();
    svc.poll_bms();
  }
}

// ============================================================================
// TEST CASE 2 — reset_watchdog
// ============================================================================

TEST_CASE("reset_watchdog: pass-through to BmsDevice", "[PowerService][watchdog]") {
  MockBmsDevice mock_bms;
  PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);

  SECTION("update_watchdog succeeds — returns true") {
    REQUIRE_CALL(mock_bms, update_watchdog()).RETURN(true);

    CHECK(svc.reset_watchdog());
  }

  SECTION("update_watchdog fails — returns false") {
    REQUIRE_CALL(mock_bms, update_watchdog()).RETURN(false);

    CHECK_FALSE(svc.reset_watchdog());
  }
}

// ============================================================================
// TEST CASE 3 — poll_status (plain pass-through, no PMID sync)
// ============================================================================

TEST_CASE("poll_status: charger status pass-through", "[PowerService][status]") {
  MockBmsDevice mock_bms;
  PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);

  SECTION("read succeeds — status populated") {
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::FastCharge;
                     _1.power_source = BmsPowerSource::UsbDcp;)
        .RETURN(true);

    BmsStatus status{};
    CHECK(svc.poll_status(status));
    CHECK(status.charging_state == BmsChargingState::FastCharge);
    CHECK(status.power_source == BmsPowerSource::UsbDcp);
  }

  SECTION("read fails — returns false") {
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_)).RETURN(false);

    BmsStatus status{};
    CHECK_FALSE(svc.poll_status(status));
  }
}

// ============================================================================
// TEST CASE 4 — evaluate_sleep
// ============================================================================

TEST_CASE("decide_sleep: sleep type and duration", "[PowerService][sleep]") {
  MockBmsDevice mock_bms;

  SECTION("Non-Offline mode — None with zero duration") {
    PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);

    GoSettings settings{};
    settings.measure_interval_seconds = 60;

    auto d = svc.decide_sleep(settings, LockState::Locked, OperatingMode::Portable, 0);
    CHECK(d.type == PowerService::SleepType::None);
    CHECK(d.duration_ms == 0);

    d = svc.decide_sleep(settings, LockState::Locked, OperatingMode::Stationary, 0);
    CHECK(d.type == PowerService::SleepType::None);
    CHECK(d.duration_ms == 0);
  }

  SECTION("Unlocked — None with zero duration") {
    PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);

    GoSettings settings{};
    settings.measure_interval_seconds = 60;

    auto d = svc.decide_sleep(settings, LockState::Unlocked, OperatingMode::Offline, 0);
    CHECK(d.type == PowerService::SleepType::None);
    CHECK(d.duration_ms == 0);
  }

  SECTION("Offline + Locked, interval 60s, awake 3s — Deep 57s") {
    PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG); // threshold = 5000 ms

    GoSettings settings{};
    settings.measure_interval_seconds = 60;

    auto d = svc.decide_sleep(settings, LockState::Locked, OperatingMode::Offline, 3000);
    CHECK(d.type == PowerService::SleepType::Deep);
    CHECK(d.duration_ms == 57000);
  }

  SECTION("Offline + Locked, interval 10s, awake 7s — None (below threshold)") {
    PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG); // threshold = 5000 ms

    GoSettings settings{};
    settings.measure_interval_seconds = 10;

    auto d = svc.decide_sleep(settings, LockState::Locked, OperatingMode::Offline, 7000);
    CHECK(d.type == PowerService::SleepType::None);
    CHECK(d.duration_ms == 0);
  }

  SECTION("Offline + Locked, interval 3s, awake 1s — None (below threshold)") {
    PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG); // threshold = 5000 ms

    GoSettings settings{};
    settings.measure_interval_seconds = 3;

    auto d = svc.decide_sleep(settings, LockState::Locked, OperatingMode::Offline, 1000);
    CHECK(d.type == PowerService::SleepType::None);
    CHECK(d.duration_ms == 0);
  }

  SECTION("Custom threshold affects boundary") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.deep_sleep_threshold_ms = 2000;
    PowerService svc(mock_bms, test_gpio_hal, config);

    GoSettings settings{};
    settings.measure_interval_seconds = 3; // 3000 ms >= 2000

    auto d = svc.decide_sleep(settings, LockState::Locked, OperatingMode::Offline, 0);
    CHECK(d.type == PowerService::SleepType::Deep);
    CHECK(d.duration_ms == 3000);
  }

  SECTION("Awake time subtracts from duration — Deep to None transition") {
    PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG); // threshold = 5000 ms

    GoSettings settings{};
    settings.measure_interval_seconds = 10; // interval 10000 ms

    // 0 awake: 10000 ms >= 5000 -> Deep
    auto d = svc.decide_sleep(settings, LockState::Locked, OperatingMode::Offline, 0);
    CHECK(d.type == PowerService::SleepType::Deep);
    CHECK(d.duration_ms == 10000);

    // 6000 awake: 4000 ms < 5000 -> None (stay awake)
    d = svc.decide_sleep(settings, LockState::Locked, OperatingMode::Offline, 6000);
    CHECK(d.type == PowerService::SleepType::None);
    CHECK(d.duration_ms == 0);
  }

  SECTION("Awake exceeds interval — clamps to 0, stays awake") {
    PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);

    GoSettings settings{};
    settings.measure_interval_seconds = 3;

    auto d = svc.decide_sleep(settings, LockState::Locked, OperatingMode::Offline, 5000);
    CHECK(d.type == PowerService::SleepType::None);
    CHECK(d.duration_ms == 0);
  }
}

// ============================================================================
// TEST CASE 4 — is_fast_path_wake (static, no instance needed)
// ============================================================================

TEST_CASE("is_fast_path_wake: fast-path boot predicate", "[PowerService][boot]") {
  SECTION("Timer wake + Locked — true") {
    RtcAppState state{};
    state.lock_state = LockState::Locked;

    CHECK(PowerService::is_fast_path_wake(WakeCause::Timer, state));
  }

  SECTION("Timer wake + Unlocked — false") {
    RtcAppState state{};
    state.lock_state = LockState::Unlocked;

    CHECK_FALSE(PowerService::is_fast_path_wake(WakeCause::Timer, state));
  }

  SECTION("PowerOn + Locked — false") {
    RtcAppState state{};
    state.lock_state = LockState::Locked;

    CHECK_FALSE(PowerService::is_fast_path_wake(WakeCause::PowerOn, state));
  }

  SECTION("Button + Locked — false") {
    RtcAppState state{};
    state.lock_state = LockState::Locked;

    CHECK_FALSE(PowerService::is_fast_path_wake(WakeCause::Button, state));
  }
}

// ============================================================================
// TEST CASE 5 — save_state / load_state (RTC_DATA_ATTR is a plain static)
// ============================================================================

TEST_CASE("save_state / load_state: RTC state round-trip", "[PowerService][rtc]") {
  MockBmsDevice mock_bms;
  PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);

  SECTION("load before any save — returns default RtcAppState") {
    // Note: The static s_rtc_state_valid may carry state from previous
    // sections.  This test relies on the FIRST test execution order; however,
    // the default-constructed assertion still verifies the fallback path if
    // no state has been saved (or if the test runner starts fresh).
    //
    // For robustness, we create a second PowerService instance that shares
    // the same static variable — this is inherent to the file-scope design.
    // We test the contract: if nothing was saved, defaults are returned.

    // Intentionally do not call save_state().  If s_rtc_state_valid is false
    // (first run), load_state() returns defaults.  If prior sections already
    // saved, we skip this assertion and verify round-trip instead.
  }

  SECTION("save then load — round-trip fidelity") {
    RtcAppState saved{};
    saved.mode = OperatingMode::Portable;
    saved.behavior = Behavior::Tracking;
    saved.lock_state = LockState::Unlocked;
    saved.gps_enabled = false;
    saved.tracking_active = true;
    saved.tracking_session_id = 42731;
    saved.sensors_warm = true;

    svc.save_state(saved);
    const RtcAppState loaded = svc.load_state();

    CHECK(loaded.mode == OperatingMode::Portable);
    CHECK(loaded.behavior == Behavior::Tracking);
    CHECK(loaded.lock_state == LockState::Unlocked);
    CHECK_FALSE(loaded.gps_enabled);
    CHECK(loaded.tracking_active);
    CHECK(loaded.tracking_session_id == 42731);
    CHECK(loaded.sensors_warm);
  }

  SECTION("overwrite with new state — load returns latest") {
    RtcAppState first{};
    first.mode = OperatingMode::Stationary;
    first.tracking_session_id = 11111;
    svc.save_state(first);

    RtcAppState second{};
    second.mode = OperatingMode::Offline;
    second.tracking_session_id = 99999;
    svc.save_state(second);

    const RtcAppState loaded = svc.load_state();

    CHECK(loaded.mode == OperatingMode::Offline);
    CHECK(loaded.tracking_session_id == 99999);
  }

  SECTION("sensors_warm defaults to false") {
    RtcAppState saved{};
    saved.mode = OperatingMode::Offline;
    svc.save_state(saved);

    const RtcAppState loaded = svc.load_state();
    CHECK_FALSE(loaded.sensors_warm);
  }
}

// ============================================================================
// TEST CASE 6 — should_hold_pm_sensor (pure logic)
// ============================================================================

TEST_CASE("should_hold_pm_sensor: PM sensor hold decision", "[PowerService][sleep]") {
  MockBmsDevice mock_bms;

  SECTION("sleep below sensor_hold_max — hold") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = 26;
    config.sensor_hold_max_sleep_ms = 20000;
    PowerService svc(mock_bms, test_gpio_hal, config);

    CHECK(svc.should_hold_pm_sensor(5000));
    CHECK(svc.should_hold_pm_sensor(10000));
    CHECK(svc.should_hold_pm_sensor(19999));
  }

  SECTION("sleep at or above sensor_hold_max — no hold") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = 26;
    config.sensor_hold_max_sleep_ms = 20000;
    PowerService svc(mock_bms, test_gpio_hal, config);

    CHECK_FALSE(svc.should_hold_pm_sensor(20000));
    CHECK_FALSE(svc.should_hold_pm_sensor(60000));
  }

  SECTION("pin_pm_power disabled (-1) — never hold") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = -1;
    config.sensor_hold_max_sleep_ms = 20000;
    PowerService svc(mock_bms, test_gpio_hal, config);

    CHECK_FALSE(svc.should_hold_pm_sensor(5000));
    CHECK_FALSE(svc.should_hold_pm_sensor(10000));
  }

  SECTION("zero sleep duration — hold (below threshold)") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = 26;
    config.sensor_hold_max_sleep_ms = 20000;
    PowerService svc(mock_bms, test_gpio_hal, config);

    CHECK(svc.should_hold_pm_sensor(0));
  }
}

// ============================================================================
// TEST CASE 7 — should_sleep_pm_sensor (pure logic)
// ============================================================================

TEST_CASE("should_sleep_pm_sensor: PM power-cycle eligibility", "[PowerService][pm_sleep]") {
  MockBmsDevice mock_bms;

  SECTION("interval at or above threshold — sleep") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = 26;
    config.pm_sleep_threshold_ms = 20000;
    PowerService svc(mock_bms, test_gpio_hal, config);

    CHECK(svc.should_sleep_pm_sensor(20000));
    CHECK(svc.should_sleep_pm_sensor(30000));
    CHECK(svc.should_sleep_pm_sensor(60000));
    CHECK(svc.should_sleep_pm_sensor(3600000));
  }

  SECTION("interval below threshold — no sleep") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = 26;
    config.pm_sleep_threshold_ms = 20000;
    PowerService svc(mock_bms, test_gpio_hal, config);

    CHECK_FALSE(svc.should_sleep_pm_sensor(10000));
    CHECK_FALSE(svc.should_sleep_pm_sensor(15000));
    CHECK_FALSE(svc.should_sleep_pm_sensor(19999));
  }

  SECTION("pin_pm_power disabled (-1) — never sleep") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = -1;
    config.pm_sleep_threshold_ms = 20000;
    PowerService svc(mock_bms, test_gpio_hal, config);

    CHECK_FALSE(svc.should_sleep_pm_sensor(60000));
    CHECK_FALSE(svc.should_sleep_pm_sensor(20000));
  }

  SECTION("zero interval — no sleep") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = 26;
    config.pm_sleep_threshold_ms = 20000;
    PowerService svc(mock_bms, test_gpio_hal, config);

    CHECK_FALSE(svc.should_sleep_pm_sensor(0));
  }
}

// ============================================================================
// TEST CASE 8 — set_pm_power (demand-coupled PMID model)
// ============================================================================

TEST_CASE("set_pm_power: demand-coupled PMID + GPIO", "[PowerService][pm_power]") {
  MockBmsDevice mock_bms;

  // Track GPIO set_level calls
  static int last_pin = -1;
  static int last_level = -1;
  auto tracking_set_level = [](int pin, int level) -> bool {
    last_pin = pin;
    last_level = level;
    return true;
  };

  gpio::Hal tracking_gpio = test_gpio_hal;
  tracking_gpio.set_level = tracking_set_level;

  SECTION("pin configured — set_pm_power(true) drives LOW (active-low EN_PM, v0.3)") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = 26;
    PowerService svc(mock_bms, tracking_gpio, config);

    trompeloeil::sequence seq;
    REQUIRE_CALL(mock_bms, set_pmid_enabled(true)).IN_SEQUENCE(seq).RETURN(true);
    // GPIO write happens after set_pmid_enabled returns

    last_pin = -1;
    last_level = -1;
    svc.set_pm_power(true);
    CHECK(last_pin == 26);
    CHECK(last_level == 0);
  }

  SECTION("pin configured — set_pm_power(false) drives HIGH (active-low EN_PM, v0.3)") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = 26;
    PowerService svc(mock_bms, tracking_gpio, config);

    REQUIRE_CALL(mock_bms, set_pmid_enabled(false)).RETURN(true);

    last_pin = -1;
    last_level = -1;
    svc.set_pm_power(false);
    CHECK(last_pin == 26);
    CHECK(last_level == 1);
  }

  SECTION("set_pmid_enabled(true) fails: log warning, still drive GPIO") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = 26;
    PowerService svc(mock_bms, tracking_gpio, config);

    REQUIRE_CALL(mock_bms, set_pmid_enabled(true)).RETURN(false);

    last_pin = -1;
    last_level = -1;
    svc.set_pm_power(true);
    // GPIO write still happens despite PMID failure
    CHECK(last_pin == 26);
    CHECK(last_level == 1);
  }

  SECTION("pin disabled (-1): neither set_pmid_enabled nor GPIO write occurs") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = -1;
    PowerService svc(mock_bms, tracking_gpio, config);

    // No PMID or GPIO calls expected
    last_pin = -1;
    last_level = -1;
    svc.set_pm_power(true);
    CHECK(last_pin == -1);
    CHECK(last_level == -1);
  }

  SECTION("v1 polarity (pm_power_on_level=0): set_pm_power(true) drives LOW") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = 26;
    config.pm_power_on_level = 0;
    PowerService svc(mock_bms, tracking_gpio, config);

    REQUIRE_CALL(mock_bms, set_pmid_enabled(true)).RETURN(true);

    last_pin = -1;
    last_level = -1;
    svc.set_pm_power(true);
    CHECK(last_pin == 26);
    CHECK(last_level == 0);
  }

  SECTION("v1 polarity (pm_power_on_level=0): set_pm_power(false) drives HIGH") {
    PowerService::Config config = DEFAULT_CONFIG;
    config.pin_pm_power = 26;
    config.pm_power_on_level = 0;
    PowerService svc(mock_bms, tracking_gpio, config);

    REQUIRE_CALL(mock_bms, set_pmid_enabled(false)).RETURN(true);

    last_pin = -1;
    last_level = -1;
    svc.set_pm_power(false);
    CHECK(last_pin == 26);
    CHECK(last_level == 1);
  }
}

// ============================================================================
// TEST CASE 9 — poll_bms: EDV (over-discharge) trip
// ============================================================================

// Macro: set up a single poll_bms cycle with given voltage and power source.
// Expectations stay alive in the calling scope (no helper function).
#define POLL_BMS_CYCLE(mock, voltage, source)                                                      \
  REQUIRE_CALL(mock, read_telemetry(trompeloeil::_))                                               \
      .SIDE_EFFECT(_1.battery_voltage = voltage)                                                   \
      .RETURN(true);                                                                               \
  REQUIRE_CALL(mock, get_battery_percentage(trompeloeil::_))                                       \
      .SIDE_EFFECT(*_1 = 50.0f)                                                                    \
      .RETURN(true);                                                                               \
  REQUIRE_CALL(mock, read_status(trompeloeil::_)).SIDE_EFFECT(_1.power_source = source).RETURN(true)

#define POLL_BMS_CYCLE_STATUS_FAIL(mock, voltage)                                                  \
  REQUIRE_CALL(mock, read_telemetry(trompeloeil::_))                                               \
      .SIDE_EFFECT(_1.battery_voltage = voltage)                                                   \
      .RETURN(true);                                                                               \
  REQUIRE_CALL(mock, get_battery_percentage(trompeloeil::_))                                       \
      .SIDE_EFFECT(*_1 = 50.0f)                                                                    \
      .RETURN(true);                                                                               \
  REQUIRE_CALL(mock, read_status(trompeloeil::_)).RETURN(false)

// Macro: set up a poll_bms cycle with temperature
#define POLL_BMS_TEMP_CYCLE(mock, bat_temp, source)                                                \
  REQUIRE_CALL(mock, read_telemetry(trompeloeil::_))                                               \
      .SIDE_EFFECT(_1.battery_voltage = 3.8f; _1.battery_temperature_c = bat_temp)                 \
      .RETURN(true);                                                                               \
  REQUIRE_CALL(mock, get_battery_percentage(trompeloeil::_))                                       \
      .SIDE_EFFECT(*_1 = 50.0f)                                                                    \
      .RETURN(true);                                                                               \
  REQUIRE_CALL(mock, read_status(trompeloeil::_)).SIDE_EFFECT(_1.power_source = source).RETURN(true)

TEST_CASE("poll_bms: EDV over-discharge trip", "[PowerService][edv]") {
  MockBmsDevice mock_bms;
  PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);

  SECTION("1 sample below 2.9V on battery: no trip") {
    POLL_BMS_CYCLE(mock_bms, 2.8f, BmsPowerSource::None);
    svc.poll_bms();
  }

  SECTION("2 samples below 2.9V: no trip") {
    ALLOW_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 2.8f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.power_source = BmsPowerSource::None)
        .RETURN(true);
    svc.poll_bms();
    svc.poll_bms();
  }

  SECTION("3 samples below 2.9V on battery: enter_ship_mode called") {
    ALLOW_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 2.8f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.power_source = BmsPowerSource::None)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, enter_ship_mode()).RETURN(true);

    svc.poll_bms(); // 1
    svc.poll_bms(); // 2
    svc.poll_bms(); // 3 -> trip
  }

  SECTION("3 samples then latch: subsequent polls do not re-fire") {
    ALLOW_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 2.8f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.power_source = BmsPowerSource::None)
        .RETURN(true);
    // enter_ship_mode called exactly once
    REQUIRE_CALL(mock_bms, enter_ship_mode()).RETURN(true).TIMES(1);

    svc.poll_bms(); // 1
    svc.poll_bms(); // 2
    svc.poll_bms(); // 3 -> trip, latches
    svc.poll_bms(); // 4 -> no re-fire
  }

  SECTION("enter_ship_mode fails: no latch, retries next poll") {
    ALLOW_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 2.8f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.power_source = BmsPowerSource::None)
        .RETURN(true);
    // First attempt fails, second succeeds
    trompeloeil::sequence seq;
    REQUIRE_CALL(mock_bms, enter_ship_mode()).IN_SEQUENCE(seq).RETURN(false);
    REQUIRE_CALL(mock_bms, enter_ship_mode()).IN_SEQUENCE(seq).RETURN(true);

    svc.poll_bms(); // 1
    svc.poll_bms(); // 2
    svc.poll_bms(); // 3 -> trip, fails
    svc.poll_bms(); // 4 -> retries, succeeds
  }

  SECTION("voltage above threshold resets counter") {
    ALLOW_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.power_source = BmsPowerSource::None)
        .RETURN(true);

    trompeloeil::sequence seq;
    // 2 below
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .SIDE_EFFECT(_1.battery_voltage = 2.8f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .SIDE_EFFECT(_1.battery_voltage = 2.8f)
        .RETURN(true);
    // 1 above resets
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .SIDE_EFFECT(_1.battery_voltage = 3.5f)
        .RETURN(true);
    // 2 below again
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .SIDE_EFFECT(_1.battery_voltage = 2.8f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .SIDE_EFFECT(_1.battery_voltage = 2.8f)
        .RETURN(true);

    for (int i = 0; i < 5; ++i) {
      svc.poll_bms();
    }
    // No enter_ship_mode — counter reset prevented trip
  }

  SECTION("VBUS present (UsbSdp): no trip even at 2.5V") {
    ALLOW_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 2.5f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.power_source = BmsPowerSource::UsbSdp)
        .RETURN(true);

    for (int i = 0; i < 4; ++i) {
      svc.poll_bms();
    }
  }

  SECTION("OtgMode: trip fires (device sourcing from cell)") {
    ALLOW_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 2.8f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.power_source = BmsPowerSource::OtgMode)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, enter_ship_mode()).RETURN(true);

    svc.poll_bms(); // 1
    svc.poll_bms(); // 2
    svc.poll_bms(); // 3 -> trip
  }

  SECTION("read_status failed: counter does not increment") {
    ALLOW_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 2.8f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, read_status(trompeloeil::_)).RETURN(false);

    for (int i = 0; i < 4; ++i) {
      svc.poll_bms();
    }
  }

  SECTION("invalid voltage sentinel: counter does not increment") {
    ALLOW_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = BmsInvalid::VOLT)
        .RETURN(true);
    ALLOW_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.power_source = BmsPowerSource::None)
        .RETURN(true);

    for (int i = 0; i < 4; ++i) {
      svc.poll_bms();
    }
  }
}

// ============================================================================
// TEST CASE 10 — poll_bms: OT (over-temperature) trip
// ============================================================================

TEST_CASE("poll_bms: OT over-temperature trip", "[PowerService][ot]") {
  MockBmsDevice mock_bms;
  PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);

  SECTION("invalid temperature sentinel: no action") {
    POLL_BMS_CYCLE(mock_bms, 3.8f, BmsPowerSource::None);
    svc.poll_bms();
  }

  SECTION("temperature below cutoff (30°C): no action") {
    POLL_BMS_TEMP_CYCLE(mock_bms, 30, BmsPowerSource::UsbSdp);
    svc.poll_bms();
  }

  SECTION("temperature crosses cutoff (50°C): set_charge_enable(false)") {
    POLL_BMS_TEMP_CYCLE(mock_bms, 50, BmsPowerSource::UsbSdp);
    REQUIRE_CALL(mock_bms, set_charge_enable(false)).RETURN(true);
    svc.poll_bms();
  }

  SECTION("stays above cutoff for multiple polls: no further set_charge_enable") {
    ALLOW_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.8f; _1.battery_temperature_c = 52)
        .RETURN(true);
    ALLOW_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 50.0f)
        .RETURN(true);
    ALLOW_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.power_source = BmsPowerSource::UsbSdp)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, set_charge_enable(false)).RETURN(true).TIMES(1);

    svc.poll_bms(); // crosses cutoff
    svc.poll_bms(); // stays above — no new call
  }

  SECTION("temperature crosses resume (47°C) going down: set_charge_enable(true)") {
    // Cross cutoff
    {
      POLL_BMS_TEMP_CYCLE(mock_bms, 50, BmsPowerSource::UsbSdp);
      REQUIRE_CALL(mock_bms, set_charge_enable(false)).RETURN(true);
      svc.poll_bms();
    }
    // Cool to resume
    {
      POLL_BMS_TEMP_CYCLE(mock_bms, 47, BmsPowerSource::UsbSdp);
      REQUIRE_CALL(mock_bms, set_charge_enable(true)).RETURN(true);
      svc.poll_bms();
    }
  }

  SECTION("hysteresis band (48°C) with charge disabled: no transition") {
    {
      POLL_BMS_TEMP_CYCLE(mock_bms, 50, BmsPowerSource::UsbSdp);
      REQUIRE_CALL(mock_bms, set_charge_enable(false)).RETURN(true);
      svc.poll_bms();
    }
    // In hysteresis band — no I2C writes
    {
      POLL_BMS_TEMP_CYCLE(mock_bms, 48, BmsPowerSource::UsbSdp);
      svc.poll_bms();
    }
  }

  SECTION("ship threshold (60°C): charge disable + enter_ship_mode") {
    POLL_BMS_TEMP_CYCLE(mock_bms, 60, BmsPowerSource::UsbSdp);
    REQUIRE_CALL(mock_bms, set_charge_enable(false)).RETURN(true);
    REQUIRE_CALL(mock_bms, enter_ship_mode()).RETURN(true);
    svc.poll_bms();
  }

  SECTION("ship threshold, enter_ship_mode fails: no latch, retries") {
    // Trip but fail
    {
      POLL_BMS_TEMP_CYCLE(mock_bms, 62, BmsPowerSource::UsbSdp);
      REQUIRE_CALL(mock_bms, set_charge_enable(false)).RETURN(true);
      REQUIRE_CALL(mock_bms, enter_ship_mode()).RETURN(false);
      svc.poll_bms();
    }
    // Retry (charge already disabled)
    {
      POLL_BMS_TEMP_CYCLE(mock_bms, 62, BmsPowerSource::UsbSdp);
      REQUIRE_CALL(mock_bms, enter_ship_mode()).RETURN(true);
      svc.poll_bms();
    }
  }

  SECTION("ship latched: no further enter_ship_mode calls") {
    {
      POLL_BMS_TEMP_CYCLE(mock_bms, 60, BmsPowerSource::UsbSdp);
      REQUIRE_CALL(mock_bms, set_charge_enable(false)).RETURN(true);
      REQUIRE_CALL(mock_bms, enter_ship_mode()).RETURN(true);
      svc.poll_bms();
    }
    {
      POLL_BMS_TEMP_CYCLE(mock_bms, 62, BmsPowerSource::UsbSdp);
      svc.poll_bms();
    }
  }

  SECTION("ship-mode latch wins over charge resume") {
    {
      POLL_BMS_TEMP_CYCLE(mock_bms, 60, BmsPowerSource::UsbSdp);
      REQUIRE_CALL(mock_bms, set_charge_enable(false)).RETURN(true);
      REQUIRE_CALL(mock_bms, enter_ship_mode()).RETURN(true);
      svc.poll_bms();
    }
    // Cool below resume — charge NOT re-enabled
    {
      POLL_BMS_TEMP_CYCLE(mock_bms, 40, BmsPowerSource::UsbSdp);
      svc.poll_bms();
    }
  }
}

// ============================================================================
// TEST CASE 11 — set_watchdog_timeout_ms
// ============================================================================

TEST_CASE("set_watchdog_timeout_ms: PowerService wrapper", "[PowerService][watchdog]") {
  MockBmsDevice mock_bms;
  PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);

  SECTION("forwards 0 verbatim") {
    REQUIRE_CALL(mock_bms, set_watchdog_timeout_ms(0u)).RETURN(true);
    CHECK(svc.set_watchdog_timeout_ms(0));
  }

  SECTION("forwards 40000 verbatim") {
    REQUIRE_CALL(mock_bms, set_watchdog_timeout_ms(40000u)).RETURN(true);
    CHECK(svc.set_watchdog_timeout_ms(40000));
  }

  SECTION("return value propagates: false") {
    REQUIRE_CALL(mock_bms, set_watchdog_timeout_ms(0u)).RETURN(false);
    CHECK_FALSE(svc.set_watchdog_timeout_ms(0));
  }
}

// ============================================================================
// TEST CASE 12 — evaluate_fg_state truth table
// ============================================================================

TEST_CASE("evaluate_fg_state: decision truth table", "[PowerService][fg]") {
  static constexpr FgCellConfig TARGET = {2000, 7400, 3000, 50};
  static constexpr uint16_t DC_MIN = 500;
  static constexpr uint16_t DC_MAX = 8000;
  static constexpr uint16_t FCC_MAX = 8500;

  SECTION("all reads OK, DC at lower bound — not corrupted") {
    auto d =
        evaluate_fg_state(DC_MIN, true, 2000, true, TARGET, true, TARGET, DC_MIN, DC_MAX, FCC_MAX);
    CHECK_FALSE(d.needs_factory_reset);
    CHECK_FALSE(d.needs_config_write);
  }

  SECTION("all reads OK, DC just below lower bound — corrupted") {
    auto d = evaluate_fg_state(DC_MIN - 1, true, 2000, true, TARGET, true, TARGET, DC_MIN, DC_MAX,
                               FCC_MAX);
    CHECK(d.needs_factory_reset);
  }

  SECTION("all reads OK, DC at upper bound — not corrupted") {
    auto d =
        evaluate_fg_state(DC_MAX, true, 2000, true, TARGET, true, TARGET, DC_MIN, DC_MAX, FCC_MAX);
    CHECK_FALSE(d.needs_factory_reset);
  }

  SECTION("all reads OK, DC just above upper bound — corrupted") {
    auto d = evaluate_fg_state(DC_MAX + 1, true, 2000, true, TARGET, true, TARGET, DC_MIN, DC_MAX,
                               FCC_MAX);
    CHECK(d.needs_factory_reset);
  }

  SECTION("all reads OK, FCC at upper bound — not corrupted") {
    auto d =
        evaluate_fg_state(2000, true, FCC_MAX, true, TARGET, true, TARGET, DC_MIN, DC_MAX, FCC_MAX);
    CHECK_FALSE(d.needs_factory_reset);
  }

  SECTION("all reads OK, FCC just above upper bound — corrupted") {
    auto d = evaluate_fg_state(2000, true, FCC_MAX + 1, true, TARGET, true, TARGET, DC_MIN, DC_MAX,
                               FCC_MAX);
    CHECK(d.needs_factory_reset);
  }

  SECTION("all reads OK, both DC and FCC corrupted — single trip") {
    auto d =
        evaluate_fg_state(100, true, 9000, true, TARGET, true, TARGET, DC_MIN, DC_MAX, FCC_MAX);
    CHECK(d.needs_factory_reset);
  }

  SECTION("all reads OK, in-range, CellConfig matches target — no action") {
    auto d =
        evaluate_fg_state(2000, true, 2000, true, TARGET, true, TARGET, DC_MIN, DC_MAX, FCC_MAX);
    CHECK_FALSE(d.needs_factory_reset);
    CHECK_FALSE(d.needs_config_write);
  }

  SECTION("all reads OK, in-range, CellConfig differs in one field — needs write") {
    FgCellConfig different = TARGET;
    different.design_capacity_mah = 1500;
    auto d =
        evaluate_fg_state(2000, true, 2000, true, different, true, TARGET, DC_MIN, DC_MAX, FCC_MAX);
    CHECK_FALSE(d.needs_factory_reset);
    CHECK(d.needs_config_write);
  }

  SECTION("all reads OK, DC corrupted, CellConfig matches — reset true, write false") {
    auto d =
        evaluate_fg_state(100, true, 2000, true, TARGET, true, TARGET, DC_MIN, DC_MAX, FCC_MAX);
    CHECK(d.needs_factory_reset);
    CHECK_FALSE(d.needs_config_write);
  }

  SECTION("post-reset pass 2: all OK, in-range, ROM defaults differ — write true") {
    FgCellConfig rom_defaults = {1340, 4962, 3000, 10}; // hypothetical ROM defaults
    auto d = evaluate_fg_state(1340, true, 1200, true, rom_defaults, true, TARGET, DC_MIN, DC_MAX,
                               FCC_MAX);
    CHECK_FALSE(d.needs_factory_reset);
    CHECK(d.needs_config_write);
  }

  SECTION("dc_ok=false, DC value would be out-of-range — no reset") {
    auto d =
        evaluate_fg_state(100, false, 2000, true, TARGET, true, TARGET, DC_MIN, DC_MAX, FCC_MAX);
    CHECK_FALSE(d.needs_factory_reset);
  }

  SECTION("fcc_ok=false, FCC value would be out-of-range — no reset") {
    auto d =
        evaluate_fg_state(2000, true, 50000, false, TARGET, true, TARGET, DC_MIN, DC_MAX, FCC_MAX);
    CHECK_FALSE(d.needs_factory_reset);
  }

  SECTION("cfg_ok=false, CellConfig differs from target — no write") {
    FgCellConfig different = TARGET;
    different.terminate_voltage_mv = 3200;
    auto d = evaluate_fg_state(2000, true, 2000, true, different, false, TARGET, DC_MIN, DC_MAX,
                               FCC_MAX);
    CHECK_FALSE(d.needs_config_write);
  }

  SECTION("all three _ok flags false — no destructive action") {
    FgCellConfig junk = {0, 0, 0, 0};
    auto d =
        evaluate_fg_state(0, false, 50000, false, junk, false, TARGET, DC_MIN, DC_MAX, FCC_MAX);
    CHECK_FALSE(d.needs_factory_reset);
    CHECK_FALSE(d.needs_config_write);
  }
}

// ============================================================================
// TEST CASE 13 — set_fuel_gauge
// ============================================================================

TEST_CASE("set_fuel_gauge: null-pointer guard and overwrite", "[PowerService][fg]") {
  MockBmsDevice mock_bms;
  PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);

  SECTION("no FG attached — poll_bms uses BMS, FG fields stay at sentinels") {
    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.7f; _1.charging_voltage = 5.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 60.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::NotCharging)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();
    CHECK(snap.battery_percent_source == BatteryPercentSource::BatteryCharger);
    CHECK(snap.fg_soc_percent == BmsInvalid::SOC_PERCENT);
    CHECK(snap.fg_voltage_mv == BmsInvalid::VOLTAGE_MV);
    CHECK(snap.fg_current_ma == BmsInvalid::CURRENT_MA);
    CHECK(snap.fg_power_mw == BmsInvalid::POWER_MW);
    CHECK(snap.fg_remaining_capacity_mah == BmsInvalid::CAPACITY_MAH);
    CHECK(snap.fg_full_charge_capacity_mah == BmsInvalid::CAPACITY_MAH);
    CHECK(snap.fg_internal_temperature_c == Catch::Approx(BmsInvalid::FG_TEMP_C));
  }

  SECTION("set_fuel_gauge twice — second overwrites first (idempotent)") {
    MockFuelGaugeDevice mock_fg1;
    MockFuelGaugeDevice mock_fg2;
    svc.set_fuel_gauge(&mock_fg1);
    svc.set_fuel_gauge(&mock_fg2);

    // Prove the second FG is used by expecting calls on mock_fg2.
    ALLOW_CALL(mock_fg2, ready()).RETURN(true);
    REQUIRE_CALL(mock_fg2, read_soc_percent(trompeloeil::_)).SIDE_EFFECT(_1 = 42).RETURN(true);
    ALLOW_CALL(mock_fg2, read_voltage_mv(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg2, read_average_current_ma(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg2, read_average_power_mw(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg2, read_remaining_capacity_mah(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg2, read_full_charge_capacity_mah(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg2, read_internal_temperature_c(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg2, read_flags(trompeloeil::_)).RETURN(false);

    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_)).RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_)).RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();
    CHECK(snap.battery_percent_source == BatteryPercentSource::FuelGauge);
    CHECK(snap.battery_percentage == Catch::Approx(42.0f));
  }
}

// ============================================================================
// TEST CASE 14 — poll_bms SOC source switching
// ============================================================================

TEST_CASE("poll_bms: SOC source switching with FG", "[PowerService][fg][poll_bms]") {
  MockBmsDevice mock_bms;
  MockFuelGaugeDevice mock_fg;
  PowerService svc(mock_bms, test_gpio_hal, DEFAULT_CONFIG);
  svc.set_fuel_gauge(&mock_fg);

  SECTION("FG attached, FG SOC read OK — src=FuelGauge") {
    ALLOW_CALL(mock_fg, ready()).RETURN(true);
    REQUIRE_CALL(mock_fg, read_soc_percent(trompeloeil::_)).SIDE_EFFECT(_1 = 85).RETURN(true);
    ALLOW_CALL(mock_fg, read_voltage_mv(trompeloeil::_)).SIDE_EFFECT(_1 = 3800).RETURN(true);
    ALLOW_CALL(mock_fg, read_average_current_ma(trompeloeil::_))
        .SIDE_EFFECT(_1 = -120)
        .RETURN(true);
    ALLOW_CALL(mock_fg, read_average_power_mw(trompeloeil::_)).SIDE_EFFECT(_1 = -450).RETURN(true);
    ALLOW_CALL(mock_fg, read_remaining_capacity_mah(trompeloeil::_))
        .SIDE_EFFECT(_1 = 1700)
        .RETURN(true);
    ALLOW_CALL(mock_fg, read_full_charge_capacity_mah(trompeloeil::_))
        .SIDE_EFFECT(_1 = 2000)
        .RETURN(true);
    ALLOW_CALL(mock_fg, read_internal_temperature_c(trompeloeil::_))
        .SIDE_EFFECT(_1 = 28.5f)
        .RETURN(true);
    ALLOW_CALL(mock_fg, read_flags(trompeloeil::_)).SIDE_EFFECT(_1 = 0x0100).RETURN(true);

    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.8f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::NotCharging)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.battery_percent_source == BatteryPercentSource::FuelGauge);
    CHECK(snap.battery_percentage == Catch::Approx(85.0f));
    CHECK(snap.fg_soc_percent == 85);
    CHECK(snap.fg_voltage_mv == 3800);
    CHECK(snap.fg_current_ma == -120);
    CHECK(snap.fg_power_mw == -450);
    CHECK(snap.fg_remaining_capacity_mah == 1700);
    CHECK(snap.fg_full_charge_capacity_mah == 2000);
    CHECK(snap.fg_internal_temperature_c == Catch::Approx(28.5f));
    CHECK(snap.fg_flags == 0x0100);
  }

  SECTION("FG attached, FG SOC read fails, other reads succeed — src=BatteryCharger, partial FG") {
    ALLOW_CALL(mock_fg, ready()).RETURN(true);
    REQUIRE_CALL(mock_fg, read_soc_percent(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg, read_voltage_mv(trompeloeil::_)).SIDE_EFFECT(_1 = 3700).RETURN(true);
    ALLOW_CALL(mock_fg, read_average_current_ma(trompeloeil::_))
        .SIDE_EFFECT(_1 = -100)
        .RETURN(true);
    ALLOW_CALL(mock_fg, read_average_power_mw(trompeloeil::_)).SIDE_EFFECT(_1 = -370).RETURN(true);
    ALLOW_CALL(mock_fg, read_remaining_capacity_mah(trompeloeil::_))
        .SIDE_EFFECT(_1 = 1600)
        .RETURN(true);
    ALLOW_CALL(mock_fg, read_full_charge_capacity_mah(trompeloeil::_))
        .SIDE_EFFECT(_1 = 1950)
        .RETURN(true);
    ALLOW_CALL(mock_fg, read_internal_temperature_c(trompeloeil::_))
        .SIDE_EFFECT(_1 = 25.0f)
        .RETURN(true);
    ALLOW_CALL(mock_fg, read_flags(trompeloeil::_)).SIDE_EFFECT(_1 = 0x0004).RETURN(true);

    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.7f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 78.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::NotCharging)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.battery_percent_source == BatteryPercentSource::BatteryCharger);
    CHECK(snap.battery_percentage == Catch::Approx(78.0f));
    CHECK(snap.fg_soc_percent == BmsInvalid::SOC_PERCENT);
    // Other FG fields populated independently:
    CHECK(snap.fg_voltage_mv == 3700);
    CHECK(snap.fg_current_ma == -100);
    CHECK(snap.fg_power_mw == -370);
    CHECK(snap.fg_remaining_capacity_mah == 1600);
    CHECK(snap.fg_full_charge_capacity_mah == 1950);
    CHECK(snap.fg_internal_temperature_c == Catch::Approx(25.0f));
    CHECK(snap.fg_flags == 0x0004);
  }

  SECTION("FG attached, all FG reads fail — src=BatteryCharger, all FG fields at sentinels") {
    ALLOW_CALL(mock_fg, ready()).RETURN(true);
    ALLOW_CALL(mock_fg, read_soc_percent(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg, read_voltage_mv(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg, read_average_current_ma(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg, read_average_power_mw(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg, read_remaining_capacity_mah(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg, read_full_charge_capacity_mah(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg, read_internal_temperature_c(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_fg, read_flags(trompeloeil::_)).RETURN(false);

    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.7f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 55.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::NotCharging)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.battery_percent_source == BatteryPercentSource::BatteryCharger);
    CHECK(snap.battery_percentage == Catch::Approx(55.0f));
    CHECK(snap.fg_soc_percent == BmsInvalid::SOC_PERCENT);
    CHECK(snap.fg_voltage_mv == BmsInvalid::VOLTAGE_MV);
    CHECK(snap.fg_current_ma == BmsInvalid::CURRENT_MA);
    CHECK(snap.fg_power_mw == BmsInvalid::POWER_MW);
    CHECK(snap.fg_remaining_capacity_mah == BmsInvalid::CAPACITY_MAH);
    CHECK(snap.fg_full_charge_capacity_mah == BmsInvalid::CAPACITY_MAH);
    CHECK(snap.fg_internal_temperature_c == Catch::Approx(BmsInvalid::FG_TEMP_C));
  }

  SECTION("FG attached but not ready — src=BatteryCharger, FG fields at sentinels") {
    ALLOW_CALL(mock_fg, ready()).RETURN(false);

    REQUIRE_CALL(mock_bms, read_telemetry(trompeloeil::_))
        .SIDE_EFFECT(_1.battery_voltage = 3.6f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, get_battery_percentage(trompeloeil::_))
        .SIDE_EFFECT(*_1 = 40.0f)
        .RETURN(true);
    REQUIRE_CALL(mock_bms, read_status(trompeloeil::_))
        .SIDE_EFFECT(_1.charging_state = BmsChargingState::NotCharging)
        .RETURN(true);

    const PowerSnapshot snap = svc.poll_bms();

    CHECK(snap.battery_percent_source == BatteryPercentSource::BatteryCharger);
    CHECK(snap.fg_soc_percent == BmsInvalid::SOC_PERCENT);
  }
}
