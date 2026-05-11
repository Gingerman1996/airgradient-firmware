#ifndef GO_UI_H
#define GO_UI_H

#include <cstdint>

#include "go_display.h"
#include "go_settings.h"
#include "go_types.h"
#include "measures_types.h"

/// Chart buffer size — matches the payload cache capacity (Kconfig default 16).
inline constexpr uint8_t UI_CHART_BUF_SIZE = 16;

/// Snackbar display duration in milliseconds.
inline constexpr uint32_t SNACKBAR_DURATION_MS = 3000;

// ---------------------------------------------------------------------------
// UIAction — returned by handle_input to signal application-level changes.
// ---------------------------------------------------------------------------

enum class UIAction : uint8_t {
  None,
  StartTracking,
  StopTracking,
  ChangeMode,      ///< Accompanied by UIActionResult::new_mode.
  SettingsChanged, ///< UI Manager updated internal settings state.
  ClearData,
  CalibrateCo2,
  SaveTag, ///< Accompanied by UIActionResult::tag_index.
};

struct UIActionResult {
  UIAction action = UIAction::None;
  OperatingMode new_mode = OperatingMode::Offline;
  uint8_t tag_index = 0;
  const char *tag_label = nullptr; ///< Points to static tag label string.
};

// ---------------------------------------------------------------------------
// BuildContext — all external state for building a DisplayValues snapshot.
// ---------------------------------------------------------------------------

struct BuildContext {
  const Measures &sensor_data;

  // Battery
  uint8_t battery_pct; // 0xFF = no data
  bool is_battery_charging;

  // Status flags
  bool locked;
  bool ble_enabled;
  bool ble_connected;
  bool wifi_enabled;
  bool gps_enabled;
  bool gps_fix;
  bool tracking_active;

  // Settings-derived flags
  bool display_off;
  bool use_fahrenheit;
  bool pm_use_usaqi;

  // Temporary measurement cache (for chart rendering)
  const MeasuresAGo *cache;
  uint8_t cache_count;

  // Current timestamp for snackbar expiry
  uint32_t now_ms;
};

// ---------------------------------------------------------------------------
// UIManager — pure state machine for screen navigation and UI logic.
//
// The UI Manager has ZERO hardware or RTOS dependencies. It is driven
// entirely by the orchestrator which calls handle_input() and build_values().
// ---------------------------------------------------------------------------

class UIManager {
public:
  struct Config {
    const char *firmware_version; // e.g. "0.1.0"
    const char *serial_number;    // 12-char hex string
  };

  explicit UIManager(const Config &config);

  /// Process an input event. Returns an action if the input triggered
  /// an application-level state change (start tracking, change mode, etc.).
  /// The orchestrator should not call this when the device is locked.
  UIActionResult handle_input(InputSource source, InputType type);

  /// Build a complete DisplayValues snapshot for the Display Service.
  /// The orchestrator passes in all external state via BuildContext.
  DisplayValues build_values(const BuildContext &ctx) const;

  /// Force the screen to a specific value. Used by the orchestrator
  /// for Shutdown and for restoring state after deep sleep wake.
  void set_screen(Screen screen);

  /// Get the current screen (for orchestrator decisions).
  Screen current_screen() const;

  /// True when the user is on any menu-navigation screen (MainMenu, Settings,
  /// SettingsChoice, TagList, Confirm, About).  Used by the orchestrator to
  /// suppress background display updates that would interrupt menu interaction.
  bool is_on_menu_screen() const;

  /// Show a snackbar message. Duration is armed on the next
  /// clear_expired_snackbar() call (3 seconds from that point).
  void show_snackbar(const char *text);

  /// Clear snackbar if expired. Called by orchestrator before build_values.
  void clear_expired_snackbar(uint32_t now_ms);

  /// Synchronize internal option indices from persisted GoSettings.
  /// Called by the orchestrator once after loading settings from NVS.
  void sync_settings(const GoSettings &settings);

  /// Convert internal option indices back to GoSettings field values.
  /// Reverse of sync_settings().  Called by the orchestrator when a
  /// setting is changed through the UI.
  void apply_to_settings(GoSettings &settings) const;

  /// Reset to Home screen with no metric selected. Used on auto-lock.
  void reset_to_home();

  /// Show the BLE pairing passkey on a dedicated screen.
  /// The passkey is displayed until dismiss_pairing_passkey() is called
  /// or the user navigates away.
  void show_pairing_passkey(uint32_t passkey);

  /// Dismiss the pairing passkey screen and return to Home.
  void dismiss_pairing_passkey();

private:
  Config _config;

  // Pre-formatted about screen text
  char _about_firmware[32] = {};
  char _about_serial[32] = {};

  // Screen state
  Screen _screen = Screen::Home;
  Metric _active_metric = Metric::None;

  // Menu selection indices (per screen)
  uint8_t _menu_index = 0;
  uint8_t _settings_index = 1;
  uint8_t _settings_choice_index = 1;
  uint8_t _about_index = 1;
  uint8_t _confirm_index = 1;
  uint8_t _tag_list_index = 1;

  // Scroll state
  uint8_t _settings_scroll_start = 0;
  uint8_t _settings_choice_scroll_start = 0;
  uint8_t _tag_scroll_start = 0;

  // Active settings choice context
  uint8_t _editing_setting_id = 0;

  // Active confirm context (which setting opened the confirm dialog)
  uint8_t _confirm_source_setting = 0;

  // Internal settings state (option indices).
  // Synced from GoSettings via sync_settings() at startup.
  uint8_t _setting_units = 0;            // 0=C, 1=F
  uint8_t _setting_pm_display = 0;       // 0=ug/m3, 1=USAQI
  uint8_t _setting_measure_interval = 1; // default index 1 = "10s"
  uint8_t _setting_gps_mode = 1;         // 1="On When Tracking"
  uint8_t _setting_mode = 1;             // 1="Portable"
  uint8_t _setting_auto_lock = 0;        // 0="Off"
  uint8_t _setting_led_brightness = 4;   // 0=Off, 1=25%, 2=50%, 3=75%, 4=100%

  // Snackbar
  char _snackbar_text[48] = {};
  uint32_t _snackbar_deadline_ms = 0;

  // BLE pairing
  uint32_t _ble_passkey = 0;

  // Chart output buffer (mutable: written by const build_values)
  mutable float _chart_buf[UI_CHART_BUF_SIZE] = {};

  // Cached from last build_values call (mutable for const correctness)
  mutable bool _tracking_active = false;

  // --- Input dispatch (per screen) ---
  UIActionResult dispatch_home(InputSource source, InputType type);
  UIActionResult dispatch_menu(InputSource source, InputType type);
  UIActionResult dispatch_settings(InputSource source, InputType type);
  UIActionResult dispatch_settings_choice(InputSource source, InputType type);
  UIActionResult dispatch_about(InputSource source, InputType type);
  UIActionResult dispatch_confirm(InputSource source, InputType type);
  UIActionResult dispatch_tag_list(InputSource source, InputType type);

  // --- Navigation helpers ---
  void go_home();
  void open_main_menu();
  void open_settings();
  void open_settings_choice(uint8_t setting_id);
  void open_about();
  void open_tag_list();
  void open_confirm(uint8_t source_setting);

  // --- Movement helpers ---
  void move_menu(int delta);
  void move_settings(int delta);
  void move_settings_choice(int delta);
  void move_tag_list(int delta);
  void move_about(int delta);
  void move_confirm(int delta);
  void browse_metric(int delta);

  // --- Row population ---
  void populate_menu_rows(DisplayValues &v) const;
  void populate_settings_rows(DisplayValues &v) const;
  void populate_settings_choice_rows(DisplayValues &v) const;
  void populate_about_rows(DisplayValues &v) const;
  void populate_confirm_rows(DisplayValues &v) const;
  void populate_tag_list_rows(DisplayValues &v) const;

  // --- Chart extraction ---
  void populate_chart(DisplayValues &v, const MeasuresAGo *cache, uint8_t cache_count) const;

  // --- Settings choice helpers ---
  uint8_t setting_option_count(uint8_t setting_id) const;
  uint8_t setting_current_option(uint8_t setting_id) const;
  void apply_setting_choice(uint8_t option_index);
  void sync_choice_scroll();

  // --- Internal queries ---
  bool snackbar_active() const;
};

#endif // GO_UI_H
