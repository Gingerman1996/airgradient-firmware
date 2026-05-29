#ifndef GO_DISPLAY_H
#define GO_DISPLAY_H

#include <cstdint>

#include "measures_types.h"
#include "rtos.h"

// ---------------------------------------------------------------------------
// Host-compatible types (no ESP-IDF dependency)
// ---------------------------------------------------------------------------

enum class Screen : uint8_t {
  Home,
  MainMenu,
  Settings,
  SettingsChoice,
  TagList,
  About,
  Confirm,
  Shutdown,
  PairingPasskey,   ///< Shows 6-digit BLE pairing passkey
  DischargeComplete, ///< Battery-learning EDV cutoff reached — shown before ship mode
  // --- Automated battery-learning phase screens (design §3.5) ---
  BlearnCharging,    ///< Charging to Full Charge.
  BlearnResting,     ///< Charge off, capturing OCV1.
  BlearnUnplug,      ///< "Unplug charger" cue during discharge.
  BlearnVerifying,   ///< Re-plugged after final cycle; checking pass criteria.
  BlearnComplete,    ///< Learning passed verification.
  BlearnFailed,      ///< Learning gave up (POR-loss loop or verify failed at cap).
};

enum class Metric : uint8_t {
  None,
  Pm25,
  Co2,
  Temp,
  Humidity,
  Tvoc,
  Nox,
};

inline constexpr uint8_t MAX_LIST_ROWS = 10;

struct ListRow {
  char text[48];
  bool disabled = false;
};

/// Aggregated power-dashboard snapshot, rendered on the home screen when
/// `DisplayValues::show_power_dashboard` is true (admin Battery Learning
/// mode).  All fields populated by Orchestrator::build_context() from the
/// most recent PowerSnapshot.
struct PowerDashboardData {
  bool valid = false;        ///< false → "FG no data" rendered
  uint8_t soc_pct = 0;
  uint16_t voltage_mv = 0;
  int16_t current_ma = 0;    ///< signed; + = charging, − = discharging
  uint16_t remaining_mah = 0;
  uint16_t full_charge_mah = 0;
  float temperature_c = 0.0f;
  bool flag_fc = false;
  bool flag_chg = false;
  bool flag_dsg = false;
  uint16_t vsys_mv = 0;      ///< BMS system rail voltage
  uint16_t vpmid_mv = 0;     ///< BMS PMID rail — should read ~0 in LOW_POWER on cell
  uint16_t charge_current_ma = 0; ///< ICHG currently programmed at the BMS
  uint8_t bms_charging_state = 0; ///< BmsChargingState raw enum value
  bool low_power_active = false;  ///< Orchestrator::_in_learning_low_power mirror
  bool plugged_in = false;        ///< For phase derivation (CHARGE vs RELAX label)
  const char *blearn_phase = nullptr; ///< When set, the automated-learning stage
                                      ///< label overrides the CHARGE/RELAX banner.
};

struct DisplayValues {
  // --- Sensor readings (channel A) ---
  int co2_ppm = MeasuresInvalid::CO2;
  float pm25_ugm3 = MeasuresInvalid::PM;
  float temperature_c = MeasuresInvalid::TEMPERATURE;
  float humidity_pct = MeasuresInvalid::HUMIDITY;
  int tvoc_index = MeasuresInvalid::TVOC;
  int nox_index = MeasuresInvalid::NOX;
  float pressure_hpa = MeasuresInvalid::PRESSURE;
  float altitude_m = MeasuresInvalid::ALTITUDE;

  // --- Admin Battery Learning power dashboard (replaces sensor home view) ---
  bool show_power_dashboard = false;
  PowerDashboardData power_dashboard{};

  // --- Battery ---
  uint8_t battery_pct = 0xFF; // 0xFF = no data
  bool is_battery_charging = false;

  // --- Status flags ---
  bool locked = true;
  bool ble_enabled = false;
  bool ble_connected = false;
  bool wifi_enabled = false;
  bool gps_enabled = true;
  bool gps_fix = false;
  bool tracking_active = false;
  bool display_off = false;
  bool use_fahrenheit = false;
  bool pm_use_usaqi = false;

  // --- Screen navigation ---
  Screen screen = Screen::Home;
  Metric active_metric = Metric::None;

  // --- List/menu content ---
  ListRow rows[MAX_LIST_ROWS] = {};
  uint8_t row_count = 0;
  uint8_t selected_row = 0;
  bool show_separator_after_back = false;

  // --- About screen ---
  const char *about_title = nullptr;
  const char *about_firmware = nullptr;
  const char *about_serial = nullptr;
  const char *about_hardware = nullptr;

  // --- Chart data ---
  const float *chart_samples = nullptr;
  uint8_t chart_count = 0;
  float chart_min = 0.0f;
  float chart_max = 0.0f;

  // --- Snackbar ---
  const char *snackbar_text = nullptr;

  // --- BLE pairing ---
  uint32_t ble_passkey = 0; ///< 6-digit passkey for PairingPasskey screen
};

// ---------------------------------------------------------------------------
// RTC display snapshot — saved before deep sleep, loaded on button wake.
// Scalar data only; no pointers.  Estimated size: ~50 bytes.
// ---------------------------------------------------------------------------

struct RtcDisplaySnapshot {
  // Sensor values
  int co2_ppm;
  float pm25_ugm3;
  float temperature_c;
  float humidity_pct;
  int tvoc_index;
  int nox_index;
  float pressure_hpa;
  float altitude_m;

  // Battery
  uint8_t battery_pct;
  bool is_battery_charging;

  // Status flags
  bool gps_enabled;
  bool gps_fix;
  bool tracking_active;
  bool ble_enabled;

  // Rendering settings
  bool use_fahrenheit;
  bool pm_use_usaqi;
};

// ---------------------------------------------------------------------------
// DisplayService (hardware-dependent, excluded from host builds)
// ---------------------------------------------------------------------------

#ifndef TEST_HOST

#include <driver/spi_master.h>

extern "C" {
#include "u8g2.h"
}

/// Display refresh tier — controls how the e-paper controller updates.
enum class RefreshMode : uint8_t {
  Full,    ///< Full GC waveform (flash). Resets basemap in both RAM planes.
  Fast,    ///< Full-screen differential (no flash). Writes full frame to RAM 0x24.
  Partial, ///< Body-only differential (no flash). Writes body region to RAM 0x24.
};

class DisplayService {
public:
  struct Config {
    // SPI
    spi_host_device_t spi_host;
    int pin_cs;
    int pin_dc;
    int pin_rst;
    int pin_busy;
    int clock_hz = 4000000;

    // Worker task
    uint16_t task_stack_size = 4096;
    uint8_t task_priority = 4;

    // Refresh limits
    uint8_t max_partial_ops = 20;
  };

  explicit DisplayService(const Config &config);

  /// Initialize display hardware and start worker task.
  ///
  /// When defer_refresh is false (default): performs a synchronous full
  /// refresh, then starts the async worker task.  Unchanged behavior for
  /// run_full_boot() and run_fast_path().
  ///
  /// When defer_refresh is true: renders into the software buffer, starts
  /// the worker task, and posts the initial full refresh as the worker's
  /// first job.  Returns in ~10 ms without waiting for the e-paper refresh.
  /// The worker holds the SPI bus for the duration (~3 s), naturally
  /// serializing any other SPI device (NAND) without explicit coordination.
  bool init(const DisplayValues &initial, bool defer_refresh = false);

  /// Submit a new frame for display.
  /// Renders into framebuffer (fast), then signals worker task.
  /// wait=false and worker busy: returns false (skipped).
  /// wait=true: blocks until worker finishes previous refresh.
  bool update(const DisplayValues &values, bool wait = false);

  /// Synchronous one-shot update for fast-path boot.
  /// Renders and drives SPI inline (blocking). Does not use worker task.
  void update_sync(const DisplayValues &values);

  /// Clear display to white (full refresh). Blocking.
  void clear();

  /// Put EPD controller into deep sleep mode.
  void deep_sleep();

  /// Stop worker task. Call before entering ESP deep sleep.
  void stop();

private:
  static constexpr int BUF_ROW_BYTES = 16;
  static constexpr int BUF_TILE_HEIGHT = 32;
  static constexpr int BUF_SIZE = BUF_ROW_BYTES * BUF_TILE_HEIGHT * 8; // 4096
  static constexpr int BODY_Y = 18;
  static constexpr int BODY_H = 232;
  static constexpr int REGION_SIZE = BUF_ROW_BYTES * BODY_H; // 3712

  Config _config;

  // u8g2 instance and render buffer
  u8g2_t _u8g2;
  uint8_t _render_buf[BUF_SIZE];

  // SPI transmit buffer (owned by worker after signal)
  uint8_t _spi_buf[BUF_SIZE];
  uint8_t _region_buf[REGION_SIZE];

  // Refresh state
  DisplayValues _prev_values;
  uint8_t _diff_count = 0;
  RefreshMode _pending_mode = RefreshMode::Full;
  bool _defer_header_check = false;
  bool _menu_exited = false;

  // Worker task
  RtosTaskHandle _task_handle = nullptr;
  volatile bool _running = false;
  volatile bool _worker_busy = false;

  // Render methods
  void _render_frame(const DisplayValues &v);
  bool _is_header_changed(const DisplayValues &a, const DisplayValues &b) const;

  void _draw_status_bar(const DisplayValues &v);
  void _draw_home(const DisplayValues &v);
  void _draw_power_dashboard(const DisplayValues &v);
  void _draw_menu_overlay(const DisplayValues &v);
  void _draw_full_screen_list(const DisplayValues &v);
  void _draw_snackbar(const DisplayValues &v);
  void _draw_shutdown();
  void _draw_discharge_complete();
  void _draw_blearn_phase(const char *line1, const char *line2);
  void _draw_pairing_passkey(const DisplayValues &v);
  void _draw_chart(const DisplayValues &v);

  // Worker
  static void _worker_entry(void *arg);
  void _worker_loop();
};

// ---------------------------------------------------------------------------
// Free functions — RTC display snapshot persistence (non-TEST_HOST only)
// ---------------------------------------------------------------------------

/// Save display values to RTC memory as a snapshot for the next button wake.
/// Called from prepare_for_sleep() after the final display update.
void save_rtc_display_snapshot(const DisplayValues &values);

/// Load the RTC display snapshot saved before the last deep sleep.
/// Returns true and fills *snapshot_out when a valid snapshot exists.
/// Returns false when no valid snapshot is present (first power-on).
bool load_rtc_display_snapshot(RtcDisplaySnapshot *snapshot_out);

#else // TEST_HOST

// ---------------------------------------------------------------------------
// Host-test stub — no-op implementations so the Orchestrator compiles
// without conditional compilation in its own source files.
// ---------------------------------------------------------------------------

class DisplayService {
public:
  struct Config {};

  explicit DisplayService(const Config &) {}

  bool init(const DisplayValues &, bool = false) { return true; }

  bool update(const DisplayValues &, bool = false) {
    ++spy_update_count;
    return true;
  }

  void update_sync(const DisplayValues &) {}
  void clear() {}
  void deep_sleep() { spy_deep_sleep_called = true; }
  void stop() {}

  // Test spies — reset via test_spy::reset() in stubs.
  inline static bool spy_deep_sleep_called = false;
  inline static uint32_t spy_update_count = 0;
};

// Stub implementations for host builds.
inline void save_rtc_display_snapshot(const DisplayValues &) {}
inline bool load_rtc_display_snapshot(RtcDisplaySnapshot *) { return false; }

#endif // !TEST_HOST

#endif // GO_DISPLAY_H
