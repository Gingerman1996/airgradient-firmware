#include "go_ui.h"

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Setting option labels
// ---------------------------------------------------------------------------

static const char *const UNITS_OPTIONS[] = {"C", "F"};
static constexpr uint8_t UNITS_COUNT = 2;

static const char *const PM_DISPLAY_OPTIONS[] = {"ug/m3", "USAQI"};
static constexpr uint8_t PM_DISPLAY_COUNT = 2;

static const char *const MEASURE_INTERVAL_OPTIONS[] = {"1s", "10s", "30s", "60s",
                                                       "5m", "15m", "1h"};
static constexpr uint8_t MEASURE_INTERVAL_COUNT = 7;

static const char *const GPS_MODE_OPTIONS[] = {"Always Off", "On When Tracking", "Always On"};
static constexpr uint8_t GPS_MODE_COUNT = 3;

static const char *const MODE_OPTIONS[] = {"Stationary", "Portable", "Offline / Airplane Mode"};
static constexpr uint8_t MODE_COUNT = 3;

static const char *const AUTO_LOCK_OPTIONS[] = {"Off", "10 Seconds", "30 Seconds", "60 Seconds"};
static constexpr uint8_t AUTO_LOCK_COUNT = 4;

static const char *const LED_BRIGHTNESS_OPTIONS[] = {"Off", "25%", "50%", "75%", "100%"};
static constexpr uint8_t LED_BRIGHTNESS_COUNT = 5;

// Tag labels (indices 2..11 in the tag list screen)
static const char *const TAG_LABELS[] = {
    "Traffic Emissions", "Road Dust",         "Construction Work", "Biomass Burning",
    "Garbage Burning",   "Factory Emissions", "Smoking/Vaping",    "Cooking",
    "Paint/Solvents",    "Other Pollution",
};
static constexpr uint8_t TAG_COUNT = 10;

// ---------------------------------------------------------------------------
// Settings row index constants
// ---------------------------------------------------------------------------

static constexpr uint8_t SETTING_UNITS = 2;
static constexpr uint8_t SETTING_PM_DISPLAY = 3;
static constexpr uint8_t SETTING_MEASURE_INTERVAL = 4;
static constexpr uint8_t SETTING_GPS_MODE = 5;
static constexpr uint8_t SETTING_MODE = 6;
static constexpr uint8_t SETTING_AUTO_LOCK = 7;
static constexpr uint8_t SETTING_LED_BRIGHTNESS = 8;
static constexpr uint8_t SETTING_BACK_LED_BRIGHTNESS = 9;
static constexpr uint8_t SETTING_CO2_CALIBRATION = 10;
static constexpr uint8_t SETTING_CLEAR_DATA = 11;
static constexpr uint8_t SETTING_EXIT_ADMIN = 12; // only shown when admin_mode

static constexpr uint8_t SETTINGS_TOTAL = 13;       // indices 0..12
static constexpr uint8_t TAG_LIST_TOTAL = 12;       // indices 0..11
static constexpr uint8_t MAIN_MENU_TOTAL = 4;       // indices 0..3
static constexpr uint8_t CONFIRM_TOTAL = 5;         // indices 0..4
static constexpr uint8_t ABOUT_SELECTABLE_ROWS = 2; // indices 0..1

/// Visible content items per page (excluding Exit/Back header rows).
static constexpr uint8_t PAGE_SIZE = 8;

/// Sentinel deadline: snackbar was just shown, not yet armed.
static constexpr uint32_t SNACKBAR_PENDING = UINT32_MAX;

/// Selectable metrics in browse order: None, Pm25, Co2, Temp, Humidity.
static constexpr Metric METRIC_CYCLE[] = {Metric::None, Metric::Pm25, Metric::Co2, Metric::Temp,
                                          Metric::Humidity};
static constexpr uint8_t METRIC_CYCLE_LEN = 5;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Copy text into a DisplayValues row slot.
static void copy_row(DisplayValues &v, uint8_t index, const char *text, bool disabled) {
  if (index >= MAX_LIST_ROWS)
    return;
  if (text != nullptr) {
    (void)snprintf(v.rows[index].text, sizeof(v.rows[index].text), "%s", text);
  } else {
    v.rows[index].text[0] = '\0';
  }
  v.rows[index].disabled = disabled;
}

/// Circular modulo that handles negative values.
static int wrap(int value, int count) { return ((value % count) + count) % count; }

/// Compute page-based scroll start for a given logical index.
/// Indices 0-1 (Exit/Back) always map to scroll 0.
static uint8_t page_scroll(uint8_t index) {
  if (index <= 1)
    return 0;
  return (uint8_t)(((index - 2) / PAGE_SIZE) * PAGE_SIZE);
}

/// Convert a logical list index to its display row, given the scroll offset.
/// Indices 0-1 map directly; content indices are offset by scroll.
static uint8_t display_row(uint8_t index, uint8_t scroll) {
  if (index <= 1)
    return index;
  return (uint8_t)(2 + (index - 2 - scroll));
}

// ---------------------------------------------------------------------------
// UIManager — construction
// ---------------------------------------------------------------------------

UIManager::UIManager(const Config &config) : _config(config) {
  (void)snprintf(_about_firmware, sizeof(_about_firmware), "Firmware %s",
                 _config.firmware_version ? _config.firmware_version : "?");
  (void)snprintf(_about_serial, sizeof(_about_serial), "Serial %s",
                 _config.serial_number ? _config.serial_number : "?");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

UIActionResult UIManager::handle_input(InputSource source, InputType type) {
  // Only short-press touch events are forwarded by the orchestrator.
  if (type != InputType::ShortPress)
    return {};

  switch (_screen) {
  case Screen::Home:
    return dispatch_home(source, type);
  case Screen::MainMenu:
    return dispatch_menu(source, type);
  case Screen::Settings:
    return dispatch_settings(source, type);
  case Screen::SettingsChoice:
    return dispatch_settings_choice(source, type);
  case Screen::TagList:
    return dispatch_tag_list(source, type);
  case Screen::About:
    return dispatch_about(source, type);
  case Screen::Confirm:
    return dispatch_confirm(source, type);
  case Screen::Shutdown:
  case Screen::PairingPasskey:
    return {};
  }
  return {};
}

DisplayValues UIManager::build_values(const BuildContext &ctx) const {
  // Cache tracking state for handle_input decisions.
  _tracking_active = ctx.tracking_active;

  DisplayValues v{};

  // --- Sensor readings (channel A) ---
  v.co2_ppm = ctx.sensor_data.co2.co2;
  v.pm25_ugm3 = ctx.sensor_data.pm_a.pm_25;
  v.temperature_c = ctx.sensor_data.temp_hum_a.temperature;
  v.humidity_pct = ctx.sensor_data.temp_hum_a.humidity;
  v.tvoc_index = ctx.sensor_data.tvoc_nox.tvoc_index;
  v.nox_index = ctx.sensor_data.tvoc_nox.nox_index;
  v.pressure_hpa = ctx.sensor_data.pressure.pressure;
  v.altitude_m = ctx.sensor_data.pressure.altitude;

  // --- Battery ---
  v.battery_pct = ctx.battery_pct;
  v.is_battery_charging = ctx.is_battery_charging;

  // --- Status flags ---
  v.locked = ctx.locked;
  v.ble_enabled = ctx.ble_enabled;
  v.ble_connected = ctx.ble_connected;
  v.wifi_enabled = ctx.wifi_enabled;
  v.gps_enabled = ctx.gps_enabled;
  v.gps_fix = ctx.gps_fix;
  v.tracking_active = ctx.tracking_active;
  v.display_off = ctx.display_off;
  v.use_fahrenheit = ctx.use_fahrenheit;
  v.pm_use_usaqi = ctx.pm_use_usaqi;

  // --- Screen navigation ---
  v.screen = _screen;
  v.active_metric = _active_metric;

  // --- Populate rows based on current screen ---
  switch (_screen) {
  case Screen::Home:
    populate_chart(v, ctx.cache, ctx.cache_count);
    break;
  case Screen::MainMenu:
    populate_menu_rows(v);
    populate_chart(v, ctx.cache, ctx.cache_count);
    break;
  case Screen::Settings:
    populate_settings_rows(v);
    break;
  case Screen::SettingsChoice:
    populate_settings_choice_rows(v);
    break;
  case Screen::TagList:
    populate_tag_list_rows(v);
    break;
  case Screen::About:
    populate_about_rows(v);
    break;
  case Screen::Confirm:
    populate_confirm_rows(v);
    break;
  case Screen::Shutdown:
    break;
  case Screen::PairingPasskey:
    v.ble_passkey = _ble_passkey;
    break;
  }

  // --- Snackbar ---
  v.snackbar_text = snackbar_active() ? _snackbar_text : nullptr;

  return v;
}

void UIManager::set_screen(Screen screen) { _screen = screen; }

Screen UIManager::current_screen() const { return _screen; }

bool UIManager::is_on_menu_screen() const {
  switch (_screen) {
  case Screen::MainMenu:
  case Screen::Settings:
  case Screen::SettingsChoice:
  case Screen::TagList:
  case Screen::Confirm:
  case Screen::About:
    return true;
  default:
    return false;
  }
}

void UIManager::show_snackbar(const char *text) {
  if (text == nullptr) {
    _snackbar_text[0] = '\0';
    _snackbar_deadline_ms = 0;
    return;
  }
  (void)snprintf(_snackbar_text, sizeof(_snackbar_text), "%s", text);
  _snackbar_deadline_ms = SNACKBAR_PENDING;
}

void UIManager::clear_expired_snackbar(uint32_t now_ms) {
  if (_snackbar_text[0] == '\0')
    return;

  // Arm the deadline on first clear call after show_snackbar.
  if (_snackbar_deadline_ms == SNACKBAR_PENDING) {
    _snackbar_deadline_ms = now_ms + SNACKBAR_DURATION_MS;
    return;
  }

  // Check expiry (wraparound-safe).
  if (_snackbar_deadline_ms != 0 && (int32_t)(_snackbar_deadline_ms - now_ms) <= 0) {
    _snackbar_text[0] = '\0';
    _snackbar_deadline_ms = 0;
  }
}

void UIManager::sync_settings(const GoSettings &s) {
  _setting_units = s.use_fahrenheit ? 1 : 0;
  _setting_pm_display = s.pm_use_usaqi ? 1 : 0;

  // Map interval seconds to option index.
  // Options: "1s"=0, "10s"=1, "30s"=2, "60s"=3, "5m"=4, "15m"=5, "1h"=6
  // Display interval also has "Display Off"=7 (seconds == 0).
  // PM/other sensor intervals use "Off"=7 (seconds == 0).
  static constexpr int INTERVAL_SECONDS[] = {1, 10, 30, 60, 300, 900, 3600};
  static constexpr uint8_t INTERVAL_COUNT = 7;

  auto seconds_to_index = [](int seconds, bool has_off) -> uint8_t {
    if (seconds <= 0)
      return has_off ? 7 : 0;
    for (uint8_t i = 0; i < INTERVAL_COUNT; ++i) {
      if (seconds <= INTERVAL_SECONDS[i])
        return i;
    }
    return 6; // >= 1h → clamp to "1h"
  };

  _setting_measure_interval = seconds_to_index(s.measure_interval_seconds, false);

  // GPS mode
  switch (s.gps_mode) {
  case GpsMode::AlwaysOff:
    _setting_gps_mode = 0;
    break;
  case GpsMode::OnWhenTracking:
    _setting_gps_mode = 1;
    break;
  case GpsMode::AlwaysOn:
    _setting_gps_mode = 2;
    break;
  }

  // Operating mode
  switch (s.operating_mode) {
  case OperatingMode::Stationary:
    _setting_mode = 0;
    break;
  case OperatingMode::Portable:
    _setting_mode = 1;
    break;
  case OperatingMode::Offline:
    _setting_mode = 2;
    break;
  }

  // Auto-lock: 0=Off, 10=1, 30=2, 60=3
  if (s.auto_lock_seconds <= 0)
    _setting_auto_lock = 0;
  else if (s.auto_lock_seconds <= 10)
    _setting_auto_lock = 1;
  else if (s.auto_lock_seconds <= 30)
    _setting_auto_lock = 2;
  else
    _setting_auto_lock = 3;

  _setting_led_brightness = (s.led_brightness < LED_BRIGHTNESS_COUNT) ? s.led_brightness : 4;
  _setting_back_led_brightness =
      (s.back_led_brightness < LED_BRIGHTNESS_COUNT) ? s.back_led_brightness : 4;
}

void UIManager::apply_to_settings(GoSettings &settings) const {
  // Boolean flags
  settings.use_fahrenheit = (_setting_units == 1);
  settings.pm_use_usaqi = (_setting_pm_display == 1);

  // Map interval option index back to seconds.
  // Indices 0-6 map to {1, 10, 30, 60, 300, 900, 3600}; index 7 = 0 (Off).
  static constexpr int INTERVAL_SECONDS[] = {1, 10, 30, 60, 300, 900, 3600};
  static constexpr uint8_t INTERVAL_COUNT = 7;

  auto index_to_seconds = [](uint8_t index) -> int {
    if (index < INTERVAL_COUNT)
      return INTERVAL_SECONDS[index];
    return 0; // index 7 = Off / Display Off
  };

  settings.measure_interval_seconds = index_to_seconds(_setting_measure_interval);

  // GPS mode
  switch (_setting_gps_mode) {
  case 0:
    settings.gps_mode = GpsMode::AlwaysOff;
    break;
  case 1:
    settings.gps_mode = GpsMode::OnWhenTracking;
    break;
  case 2:
    settings.gps_mode = GpsMode::AlwaysOn;
    break;
  default:
    settings.gps_mode = GpsMode::OnWhenTracking;
    break;
  }

  // Operating mode
  switch (_setting_mode) {
  case 0:
    settings.operating_mode = OperatingMode::Stationary;
    break;
  case 1:
    settings.operating_mode = OperatingMode::Portable;
    break;
  case 2:
    settings.operating_mode = OperatingMode::Offline;
    break;
  default:
    settings.operating_mode = OperatingMode::Offline;
    break;
  }

  // Auto-lock: index 0=Off(0s), 1=10s, 2=30s, 3=60s
  static constexpr int AUTO_LOCK_SECONDS[] = {0, 10, 30, 60};
  settings.auto_lock_seconds = (_setting_auto_lock < 4) ? AUTO_LOCK_SECONDS[_setting_auto_lock] : 0;

  settings.led_brightness = (_setting_led_brightness < LED_BRIGHTNESS_COUNT)
                                ? _setting_led_brightness
                                : 4;
  settings.back_led_brightness = (_setting_back_led_brightness < LED_BRIGHTNESS_COUNT)
                                     ? _setting_back_led_brightness
                                     : 4;
}

void UIManager::reset_to_home() {
  _screen = Screen::Home;
  _active_metric = Metric::None;
}

void UIManager::show_pairing_passkey(uint32_t passkey) {
  _ble_passkey = passkey;
  _screen = Screen::PairingPasskey;
}

void UIManager::dismiss_pairing_passkey() {
  _ble_passkey = 0;
  if (_screen == Screen::PairingPasskey) {
    _screen = Screen::Home;
    _active_metric = Metric::None;
  }
}

// ---------------------------------------------------------------------------
// Internal queries
// ---------------------------------------------------------------------------

bool UIManager::snackbar_active() const {
  return _snackbar_text[0] != '\0' && _snackbar_deadline_ms != 0;
}

// ---------------------------------------------------------------------------
// Navigation helpers
// ---------------------------------------------------------------------------

void UIManager::go_home() { _screen = Screen::Home; }

void UIManager::open_main_menu() {
  _active_metric = Metric::None; // clear selection behind the overlay
  _screen = Screen::MainMenu;
  _menu_index = 0;
}

void UIManager::open_settings() {
  _screen = Screen::Settings;
  _settings_index = 1;
  _settings_scroll_start = 0;
}

void UIManager::open_settings_choice(uint8_t setting_id) {
  _editing_setting_id = setting_id;
  _screen = Screen::SettingsChoice;
  // Pre-select the currently active option.
  _settings_choice_index = (uint8_t)(2 + setting_current_option(setting_id));
  sync_choice_scroll();
}

void UIManager::open_about() {
  _screen = Screen::About;
  _about_index = 1;
}

void UIManager::open_tag_list() {
  _screen = Screen::TagList;
  _tag_list_index = 1;
  _tag_scroll_start = 0;
}

void UIManager::open_confirm(uint8_t source_setting) {
  _confirm_source_setting = source_setting;
  _screen = Screen::Confirm;
  _confirm_index = 1;
}

// ---------------------------------------------------------------------------
// Movement helpers
// ---------------------------------------------------------------------------

void UIManager::move_menu(int delta) {
  // Circular navigation — all 4 rows are always enabled.
  _menu_index = (uint8_t)wrap((int)_menu_index + delta, MAIN_MENU_TOTAL);
}

void UIManager::move_settings(int delta) {
  // Circular navigation, page-based scroll.  Skip the Exit Admin row when
  // admin mode is off so it isn't selectable.
  const uint8_t total = _admin_mode ? SETTINGS_TOTAL : (SETTINGS_TOTAL - 1);
  _settings_index = (uint8_t)wrap((int)_settings_index + delta, total);
  _settings_scroll_start = page_scroll(_settings_index);
}

void UIManager::move_settings_choice(int delta) {
  // Circular wrapping (including Exit/Back).
  int total = 2 + (int)setting_option_count(_editing_setting_id);
  _settings_choice_index = (uint8_t)wrap((int)_settings_choice_index + delta, total);
  sync_choice_scroll();
}

void UIManager::move_tag_list(int delta) {
  // Circular navigation, page-based scroll.
  _tag_list_index = (uint8_t)wrap((int)_tag_list_index + delta, TAG_LIST_TOTAL);
  _tag_scroll_start = page_scroll(_tag_list_index);
}

void UIManager::move_about(int delta) {
  // Circular between Exit (0) and Back (1).
  _about_index = (uint8_t)wrap((int)_about_index + delta, ABOUT_SELECTABLE_ROWS);
}

void UIManager::move_confirm(int delta) {
  // Circular across all 5 rows (index 2 is non-selectable but navigable).
  _confirm_index = (uint8_t)wrap((int)_confirm_index + delta, CONFIRM_TOTAL);
}

void UIManager::browse_metric(int delta) {
  // No display_off guard here — browse_metric is only reachable from
  // dispatch_home() → handle_input(), which the orchestrator only calls
  // when unlocked.  When unlocked the dashboard is always visible
  // (display_off is suppressed by the orchestrator), so browsing is valid.
  //
  // Cycle through 5 selectable entries: None, Pm25, Co2, Temp, Humidity.
  // TVOC and NOx remain in the enum but are not selectable.
  int idx = 0;
  for (int i = 0; i < METRIC_CYCLE_LEN; ++i) {
    if (METRIC_CYCLE[i] == _active_metric) {
      idx = i;
      break;
    }
  }
  idx = wrap(idx + delta, METRIC_CYCLE_LEN);
  _active_metric = METRIC_CYCLE[idx];
}

// ---------------------------------------------------------------------------
// Settings choice helpers
// ---------------------------------------------------------------------------

uint8_t UIManager::setting_option_count(uint8_t setting_id) const {
  switch (setting_id) {
  case SETTING_UNITS:
    return UNITS_COUNT;
  case SETTING_PM_DISPLAY:
    return PM_DISPLAY_COUNT;
  case SETTING_MEASURE_INTERVAL:
    return MEASURE_INTERVAL_COUNT;
  case SETTING_GPS_MODE:
    return GPS_MODE_COUNT;
  case SETTING_MODE:
    return MODE_COUNT;
  case SETTING_AUTO_LOCK:
    return AUTO_LOCK_COUNT;
  case SETTING_LED_BRIGHTNESS:
  case SETTING_BACK_LED_BRIGHTNESS:
    return LED_BRIGHTNESS_COUNT;
  default:
    return 0;
  }
}

uint8_t UIManager::setting_current_option(uint8_t setting_id) const {
  switch (setting_id) {
  case SETTING_UNITS:
    return _setting_units;
  case SETTING_PM_DISPLAY:
    return _setting_pm_display;
  case SETTING_MEASURE_INTERVAL:
    return _setting_measure_interval;
  case SETTING_GPS_MODE:
    return _setting_gps_mode;
  case SETTING_MODE:
    return _setting_mode;
  case SETTING_AUTO_LOCK:
    return _setting_auto_lock;
  case SETTING_LED_BRIGHTNESS:
    return _setting_led_brightness;
  case SETTING_BACK_LED_BRIGHTNESS:
    return _setting_back_led_brightness;
  default:
    return 0;
  }
}

void UIManager::apply_setting_choice(uint8_t option_index) {
  // The UI Manager updates its internal state and returns
  // UIAction::SettingsChanged. The orchestrator is responsible for
  // reading back the new values and persisting to NVS.

  switch (_editing_setting_id) {
  case SETTING_UNITS:
    _setting_units = option_index;
    break;
  case SETTING_PM_DISPLAY:
    _setting_pm_display = option_index;
    break;
  case SETTING_MEASURE_INTERVAL:
    _setting_measure_interval = option_index;
    break;
  case SETTING_GPS_MODE:
    _setting_gps_mode = option_index;
    break;
  case SETTING_MODE:
    _setting_mode = option_index;
    break;
  case SETTING_AUTO_LOCK:
    _setting_auto_lock = option_index;
    break;
  case SETTING_LED_BRIGHTNESS:
    _setting_led_brightness = option_index;
    break;
  case SETTING_BACK_LED_BRIGHTNESS:
    _setting_back_led_brightness = option_index;
    break;
  default:
    break;
  }

  // Return to Settings screen.
  _screen = Screen::Settings;
  _settings_choice_index = 1;
  _settings_choice_scroll_start = 0;
  _editing_setting_id = 0;
}

void UIManager::sync_choice_scroll() {
  uint8_t option_count = setting_option_count(_editing_setting_id);

  // No scrolling needed for header rows or short lists.
  if (_settings_choice_index <= 1 || option_count <= PAGE_SIZE) {
    _settings_choice_scroll_start = 0;
    return;
  }

  // Sliding-window scroll: keep the selected option visible.
  uint8_t option_idx = (uint8_t)(_settings_choice_index - 2);
  if (option_idx < _settings_choice_scroll_start) {
    _settings_choice_scroll_start = option_idx;
  } else if (option_idx >= (uint8_t)(_settings_choice_scroll_start + PAGE_SIZE)) {
    _settings_choice_scroll_start = (uint8_t)(option_idx - (PAGE_SIZE - 1));
  }

  uint8_t max_scroll = (option_count > PAGE_SIZE) ? (uint8_t)(option_count - PAGE_SIZE) : 0;
  if (_settings_choice_scroll_start > max_scroll) {
    _settings_choice_scroll_start = max_scroll;
  }
}

// ---------------------------------------------------------------------------
// Input dispatch — per screen
// ---------------------------------------------------------------------------

UIActionResult UIManager::dispatch_home(InputSource source, InputType type) {
  (void)type;
  switch (source) {
  case InputSource::TouchUp:
    browse_metric(-1);
    break;
  case InputSource::TouchDown:
    browse_metric(1);
    break;
  case InputSource::TouchEnter:
    open_main_menu();
    break;
  default:
    break;
  }
  return {};
}

UIActionResult UIManager::dispatch_menu(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
    move_menu(-1);
    break;
  case InputSource::TouchDown:
    move_menu(1);
    break;
  case InputSource::TouchEnter:
    switch (_menu_index) {
    case 0: // Exit Menu
      go_home();
      break;
    case 1: // Start / Stop Tracking
      if (_tracking_active) {
        go_home();
        result.action = UIAction::StopTracking;
      } else {
        go_home();
        result.action = UIAction::StartTracking;
      }
      break;
    case 2: // Settings
      open_settings();
      break;
    case 3: // About Device
      open_about();
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_settings(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
    move_settings(-1);
    break;
  case InputSource::TouchDown:
    move_settings(1);
    break;
  case InputSource::TouchEnter:
    if (_settings_index == 0) {
      // Exit → Home
      go_home();
    } else if (_settings_index == 1) {
      // Back → MainMenu (cursor on "Settings", index 2)
      _screen = Screen::MainMenu;
      _menu_index = 2;
    } else if (_settings_index == SETTING_CO2_CALIBRATION ||
               _settings_index == SETTING_CLEAR_DATA) {
      // Open confirm dialog for action items
      open_confirm(_settings_index);
    } else if (_settings_index == SETTING_EXIT_ADMIN && _admin_mode) {
      // Admin-mode-only action: orchestrator clears the flag, saves,
      // and shows a snackbar.  Returns to the Settings screen.
      _admin_mode = false; // local mirror — orchestrator will save settings
      result.action = UIAction::ExitAdminMode;
    } else if (_settings_index >= SETTING_UNITS && _settings_index <= SETTING_BACK_LED_BRIGHTNESS) {
      // Open choice screen for this setting
      open_settings_choice(_settings_index);
    }
    break;
  default:
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_settings_choice(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
    move_settings_choice(-1);
    break;
  case InputSource::TouchDown:
    move_settings_choice(1);
    break;
  case InputSource::TouchEnter:
    if (_settings_choice_index == 0) {
      // Exit → Home
      go_home();
    } else if (_settings_choice_index == 1) {
      // Back → Settings
      _screen = Screen::Settings;
    } else {
      // Apply chosen option
      uint8_t option_index = (uint8_t)(_settings_choice_index - 2);

      if (_editing_setting_id == SETTING_MODE) {
        // Mode change has its own UIAction with the new mode.
        apply_setting_choice(option_index);
        result.action = UIAction::ChangeMode;
        switch (option_index) {
        case 0:
          result.new_mode = OperatingMode::Stationary;
          break;
        case 1:
          result.new_mode = OperatingMode::Portable;
          break;
        case 2:
        default:
          result.new_mode = OperatingMode::Offline;
          break;
        }
      } else {
        apply_setting_choice(option_index);
        result.action = UIAction::SettingsChanged;
      }
    }
    break;
  default:
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_about(InputSource source, InputType type) {
  (void)type;

  switch (source) {
  case InputSource::TouchUp:
    move_about(-1);
    break;
  case InputSource::TouchDown:
    move_about(1);
    break;
  case InputSource::TouchEnter:
    if (_about_index == 0) {
      // Exit → Home
      go_home();
    } else if (_about_index == 1) {
      // Back → MainMenu (cursor on "About Device", index 3)
      _screen = Screen::MainMenu;
      _menu_index = 3;
    }
    break;
  default:
    break;
  }
  return {};
}

UIActionResult UIManager::dispatch_confirm(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
    move_confirm(-1);
    break;
  case InputSource::TouchDown:
    move_confirm(1);
    break;
  case InputSource::TouchEnter:
    switch (_confirm_index) {
    case 0: // Exit → Home
      go_home();
      break;
    case 1: // Back → Settings (cursor on source setting)
      _screen = Screen::Settings;
      _settings_index = _confirm_source_setting;
      _settings_scroll_start = page_scroll(_settings_index);
      break;
    case 3: // No → Settings (cursor on source setting)
      _screen = Screen::Settings;
      _settings_index = _confirm_source_setting;
      _settings_scroll_start = page_scroll(_settings_index);
      break;
    case 4: // Yes → perform action, go home
      go_home();
      if (_confirm_source_setting == SETTING_CLEAR_DATA) {
        result.action = UIAction::ClearData;
      } else if (_confirm_source_setting == SETTING_CO2_CALIBRATION) {
        result.action = UIAction::CalibrateCo2;
      }
      break;
    case 2: // Question label — non-selectable, do nothing
    default:
      break;
    }
    break;
  default:
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_tag_list(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
    move_tag_list(-1);
    break;
  case InputSource::TouchDown:
    move_tag_list(1);
    break;
  case InputSource::TouchEnter:
    if (_tag_list_index == 0) {
      // Exit → Home
      go_home();
    } else if (_tag_list_index == 1) {
      // Back → MainMenu (tag entry point removed; fall back to Settings)
      _screen = Screen::MainMenu;
      _menu_index = 2;
    } else {
      // Select tag
      uint8_t tag_idx = (uint8_t)(_tag_list_index - 2);
      if (tag_idx < TAG_COUNT) {
        go_home();
        result.action = UIAction::SaveTag;
        result.tag_index = tag_idx;
        result.tag_label = TAG_LABELS[tag_idx];
      }
    }
    break;
  default:
    break;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Row population
// ---------------------------------------------------------------------------

void UIManager::populate_menu_rows(DisplayValues &v) const {
  v.row_count = MAIN_MENU_TOTAL;
  copy_row(v, 0, "Exit Menu", false);
  copy_row(v, 1, _tracking_active ? "Stop Tracking" : "Start Tracking", false);
  copy_row(v, 2, "Settings", false);
  copy_row(v, 3, "About Device", false);
  v.selected_row = _menu_index;
}

void UIManager::populate_settings_rows(DisplayValues &v) const {
  // Fixed header rows
  copy_row(v, 0, "Exit", false);
  copy_row(v, 1, "Back", false);
  v.show_separator_after_back = true;

  // Compute page-based scroll
  uint8_t scroll = page_scroll(_settings_index);

  // Content items: indices 2..10 (9 items total)
  static constexpr uint8_t CONTENT_COUNT = SETTINGS_TOTAL - 2;
  uint8_t visible = 0;

  for (uint8_t i = 0; i < PAGE_SIZE && (scroll + i) < CONTENT_COUNT; ++i) {
    uint8_t item_index = (uint8_t)(SETTING_UNITS + scroll + i);
    char label[48];

    switch (item_index) {
    case SETTING_UNITS:
      (void)snprintf(label, sizeof(label), "Units: %s", UNITS_OPTIONS[_setting_units]);
      break;
    case SETTING_PM_DISPLAY:
      (void)snprintf(label, sizeof(label), "PM Display: %s",
                     PM_DISPLAY_OPTIONS[_setting_pm_display]);
      break;
    case SETTING_MEASURE_INTERVAL:
      (void)snprintf(label, sizeof(label), "Measure Int.: %s",
                     MEASURE_INTERVAL_OPTIONS[_setting_measure_interval]);
      break;
    case SETTING_GPS_MODE:
      (void)snprintf(label, sizeof(label), "GPS Mode: %s", GPS_MODE_OPTIONS[_setting_gps_mode]);
      break;
    case SETTING_MODE:
      (void)snprintf(label, sizeof(label), "Mode: %s", MODE_OPTIONS[_setting_mode]);
      break;
    case SETTING_AUTO_LOCK:
      (void)snprintf(label, sizeof(label), "Auto Lock: %s", AUTO_LOCK_OPTIONS[_setting_auto_lock]);
      break;
    case SETTING_LED_BRIGHTNESS:
      (void)snprintf(label, sizeof(label), "Display LED: %s",
                     LED_BRIGHTNESS_OPTIONS[_setting_led_brightness]);
      break;
    case SETTING_BACK_LED_BRIGHTNESS:
      (void)snprintf(label, sizeof(label), "AQI LED: %s",
                     LED_BRIGHTNESS_OPTIONS[_setting_back_led_brightness]);
      break;
    case SETTING_CO2_CALIBRATION:
      (void)snprintf(label, sizeof(label), "CO2: Calibrate");
      break;
    case SETTING_CLEAR_DATA:
      (void)snprintf(label, sizeof(label), "Data: Clear Data");
      break;
    case SETTING_EXIT_ADMIN:
      // Hidden in production — render only when admin mode is active.
      if (!_admin_mode) {
        continue;
      }
      (void)snprintf(label, sizeof(label), "Exit Admin Mode");
      break;
    default:
      label[0] = '\0';
      break;
    }

    copy_row(v, (uint8_t)(2 + visible), label, false);
    ++visible;
  }

  v.row_count = (uint8_t)(2 + visible);
  v.selected_row = display_row(_settings_index, scroll);
}

void UIManager::populate_settings_choice_rows(DisplayValues &v) const {
  copy_row(v, 0, "Exit", false);
  copy_row(v, 1, "Back", false);
  v.show_separator_after_back = true;

  uint8_t option_count = setting_option_count(_editing_setting_id);
  const char *const *options = nullptr;

  switch (_editing_setting_id) {
  case SETTING_UNITS:
    options = UNITS_OPTIONS;
    break;
  case SETTING_PM_DISPLAY:
    options = PM_DISPLAY_OPTIONS;
    break;
  case SETTING_MEASURE_INTERVAL:
    options = MEASURE_INTERVAL_OPTIONS;
    break;
  case SETTING_GPS_MODE:
    options = GPS_MODE_OPTIONS;
    break;
  case SETTING_MODE:
    options = MODE_OPTIONS;
    break;
  case SETTING_AUTO_LOCK:
    options = AUTO_LOCK_OPTIONS;
    break;
  case SETTING_LED_BRIGHTNESS:
  case SETTING_BACK_LED_BRIGHTNESS:
    options = LED_BRIGHTNESS_OPTIONS;
    break;
  default:
    break;
  }

  uint8_t visible = 0;
  if (options != nullptr) {
    for (uint8_t i = 0; i < PAGE_SIZE && (_settings_choice_scroll_start + i) < option_count; ++i) {
      copy_row(v, (uint8_t)(2 + visible), options[_settings_choice_scroll_start + i], false);
      ++visible;
    }
  }

  v.row_count = (uint8_t)(2 + visible);
  v.selected_row = display_row(_settings_choice_index, _settings_choice_scroll_start);
}

void UIManager::populate_about_rows(DisplayValues &v) const {
  copy_row(v, 0, "Exit", false);
  copy_row(v, 1, "Back", false);
  v.row_count = 2;
  v.selected_row = _about_index;
  v.show_separator_after_back = true;

  v.about_title = "AirGradient Go";
  v.about_firmware = _about_firmware;
  v.about_serial = _about_serial;
  v.about_hardware = "Open Source Hardware";
}

void UIManager::populate_confirm_rows(DisplayValues &v) const {
  copy_row(v, 0, "Exit", false);
  copy_row(v, 1, "Back", false);

  const char *question = "Confirm?";
  if (_confirm_source_setting == SETTING_CLEAR_DATA) {
    question = "Clear Data?";
  } else if (_confirm_source_setting == SETTING_CO2_CALIBRATION) {
    question = "Calibrate CO2?";
  }
  copy_row(v, 2, question, true); // non-selectable

  copy_row(v, 3, "No", false);
  copy_row(v, 4, "Yes", false);
  v.row_count = 5;
  v.selected_row = _confirm_index;
  v.show_separator_after_back = true;
}

void UIManager::populate_tag_list_rows(DisplayValues &v) const {
  copy_row(v, 0, "Exit", false);
  copy_row(v, 1, "Back", false);
  v.show_separator_after_back = true;

  // Compute page-based scroll
  uint8_t scroll = page_scroll(_tag_list_index);

  uint8_t visible = 0;
  for (uint8_t i = 0; i < PAGE_SIZE && (scroll + i) < TAG_COUNT; ++i) {
    copy_row(v, (uint8_t)(2 + visible), TAG_LABELS[scroll + i], false);
    ++visible;
  }

  v.row_count = (uint8_t)(2 + visible);
  v.selected_row = display_row(_tag_list_index, scroll);
}

// ---------------------------------------------------------------------------
// Chart extraction
// ---------------------------------------------------------------------------

void UIManager::populate_chart(DisplayValues &v, const MeasuresAGo *cache,
                               uint8_t cache_count) const {
  if (_active_metric == Metric::None || cache == nullptr || cache_count == 0) {
    v.chart_samples = nullptr;
    v.chart_count = 0;
    return;
  }

  uint8_t valid_count = 0;
  float min_val = 0.0f;
  float max_val = 0.0f;
  bool first = true;

  for (uint8_t i = 0; i < cache_count && i < UI_CHART_BUF_SIZE; ++i) {
    float sample = 0.0f;
    bool valid = false;

    switch (_active_metric) {
    case Metric::Pm25:
      if (cache[i].pm_a.pm_25 != MeasuresInvalid::PM) {
        sample = cache[i].pm_a.pm_25;
        valid = true;
      }
      break;
    case Metric::Co2:
      if (cache[i].co2.co2 != MeasuresInvalid::CO2) {
        sample = static_cast<float>(cache[i].co2.co2);
        valid = true;
      }
      break;
    case Metric::Temp:
      if (cache[i].temp_hum_a.temperature != MeasuresInvalid::TEMPERATURE) {
        sample = cache[i].temp_hum_a.temperature;
        valid = true;
      }
      break;
    case Metric::Humidity:
      if (cache[i].temp_hum_a.humidity != MeasuresInvalid::HUMIDITY) {
        sample = cache[i].temp_hum_a.humidity;
        valid = true;
      }
      break;
    case Metric::Tvoc:
      if (cache[i].tvoc_nox.tvoc_index != MeasuresInvalid::TVOC) {
        sample = static_cast<float>(cache[i].tvoc_nox.tvoc_index);
        valid = true;
      }
      break;
    case Metric::Nox:
      if (cache[i].tvoc_nox.nox_index != MeasuresInvalid::NOX) {
        sample = static_cast<float>(cache[i].tvoc_nox.nox_index);
        valid = true;
      }
      break;
    case Metric::None:
    default:
      break;
    }

    if (valid) {
      _chart_buf[valid_count] = sample;
      if (first) {
        min_val = sample;
        max_val = sample;
        first = false;
      } else {
        if (sample < min_val)
          min_val = sample;
        if (sample > max_val)
          max_val = sample;
      }
      ++valid_count;
    }
  }

  v.chart_samples = (valid_count > 0) ? _chart_buf : nullptr;
  v.chart_count = valid_count;
  v.chart_min = (valid_count > 0) ? min_val : 0.0f;
  v.chart_max = (valid_count > 0) ? max_val : 0.0f;
}
