#include "go_settings.h"

#include "ag_log.h"

static constexpr const char *TAG = "Settings";

namespace {

constexpr const char *KEY_MEASURE_INTERVAL_SECONDS = "mi";
constexpr const char *KEY_INACTIVITY_TIMEOUT_SECONDS = "ito";
constexpr const char *KEY_GPS_INTERVAL_SECONDS = "gis";
constexpr const char *KEY_GPS_MODE = "gpm";
constexpr const char *KEY_OPERATING_MODE = "opm";
constexpr const char *KEY_DEVICE_NAME = "dn";
constexpr const char *KEY_USE_FAHRENHEIT = "uf";
constexpr const char *KEY_PM_USE_USAQI = "pmu";
constexpr const char *KEY_AUTO_LOCK_SECONDS = "als";
constexpr const char *KEY_LED_BRIGHTNESS = "lb";
constexpr const char *KEY_BACK_LED_BRIGHTNESS = "blb";
constexpr const char *KEY_TOUCH_LED_BRIGHTNESS = "tlb";
constexpr const char *KEY_ADMIN_MODE = "adm";
constexpr const char *KEY_BATTERY_LEARNING = "blr";
constexpr const char *KEY_CHARGE_CUTOFF = "cco";
constexpr const char *KEY_SOUND_SELECT = "sd";
constexpr const char *KEY_BLEARN_STAGE = "bls";
constexpr const char *KEY_BLEARN_CYCLE = "bln";
constexpr const char *KEY_BLEARN_ITPOR_LOSSES = "bli";

bool is_measure_interval_valid(int value) { return value >= 1 && value <= 3600; }

bool is_inactivity_timeout_valid(int value) { return value >= 5 && value <= 600; }

bool is_gps_interval_valid(int value) { return value >= 1 && value <= 60; }

bool is_gps_mode_valid(int value) { return value >= 0 && value <= 2; }

bool is_operating_mode_valid(int value) { return value >= 0 && value <= 2; }

bool is_auto_lock_valid(int value) {
  return value == 0 || value == 10 || value == 30 || value == 60;
}

// Display LED uses a 6-level 0–10 % scale (0=Off, 1=2%, 2=4%, 3=6%, 4=8%,
// 5=10%) whereas the AQI back LEDs use a 5-level 0–100 % scale.  This
// validator accepts both ranges; the UI layer enforces per-field option
// counts.
bool is_led_brightness_valid(int value) { return value >= 0 && value <= 5; }

// Touch-feedback LEDs use a 3-level scale: 0=Off, 1=Dim, 2=Bright.
bool is_touch_led_brightness_valid(int value) { return value >= 0 && value <= 2; }

bool is_device_name_valid(const std::string &value) { return !value.empty() && value.size() <= 64; }

bool is_sound_select_valid(int value) {
  return value >= 0 && value < static_cast<int>(SOUND_SELECT_COUNT);
}

bool is_blearn_stage_valid(int value) {
  return value >= 0 && value <= static_cast<int>(BlearnStage::Failed);
}

const char *sound_select_name(SoundSelect s) {
  switch (s) {
  case SoundSelect::Off:    return "off";
  case SoundSelect::Chime:  return "chime";
  case SoundSelect::Tetris: return "tetris";
  }
  return "off";
}

} // namespace

GoSettings load_go_settings(ConfigStore &store) {
  GoSettings settings;

  int measure_interval_seconds = 0;
  if (store.get_int(KEY_MEASURE_INTERVAL_SECONDS, measure_interval_seconds) ==
          ConfigStoreResult::OK &&
      is_measure_interval_valid(measure_interval_seconds)) {
    settings.measure_interval_seconds = measure_interval_seconds;
  }

  int inactivity_timeout_seconds = 0;
  if (store.get_int(KEY_INACTIVITY_TIMEOUT_SECONDS, inactivity_timeout_seconds) ==
          ConfigStoreResult::OK &&
      is_inactivity_timeout_valid(inactivity_timeout_seconds)) {
    settings.inactivity_timeout_seconds = inactivity_timeout_seconds;
  }

  int gps_interval_seconds = 0;
  if (store.get_int(KEY_GPS_INTERVAL_SECONDS, gps_interval_seconds) == ConfigStoreResult::OK &&
      is_gps_interval_valid(gps_interval_seconds)) {
    settings.gps_interval_seconds = gps_interval_seconds;
  }

  int gps_mode = 0;
  if (store.get_int(KEY_GPS_MODE, gps_mode) == ConfigStoreResult::OK &&
      is_gps_mode_valid(gps_mode)) {
    settings.gps_mode = static_cast<GpsMode>(gps_mode);
  }

  int operating_mode = 0;
  if (store.get_int(KEY_OPERATING_MODE, operating_mode) == ConfigStoreResult::OK &&
      is_operating_mode_valid(operating_mode)) {
    settings.operating_mode = static_cast<OperatingMode>(operating_mode);
  }

  std::string device_name;
  if (store.get_string(KEY_DEVICE_NAME, device_name) == ConfigStoreResult::OK &&
      is_device_name_valid(device_name)) {
    settings.device_name = device_name;
  }

  bool use_fahrenheit = false;
  if (store.get_bool(KEY_USE_FAHRENHEIT, use_fahrenheit) == ConfigStoreResult::OK) {
    settings.use_fahrenheit = use_fahrenheit;
  }

  bool pm_use_usaqi = false;
  if (store.get_bool(KEY_PM_USE_USAQI, pm_use_usaqi) == ConfigStoreResult::OK) {
    settings.pm_use_usaqi = pm_use_usaqi;
  }

  int auto_lock_seconds = 0;
  if (store.get_int(KEY_AUTO_LOCK_SECONDS, auto_lock_seconds) == ConfigStoreResult::OK &&
      is_auto_lock_valid(auto_lock_seconds)) {
    settings.auto_lock_seconds = auto_lock_seconds;
  }

  int led_brightness = 0;
  if (store.get_int(KEY_LED_BRIGHTNESS, led_brightness) == ConfigStoreResult::OK &&
      is_led_brightness_valid(led_brightness)) {
    settings.led_brightness = static_cast<uint8_t>(led_brightness);
  }

  int back_led_brightness = 0;
  if (store.get_int(KEY_BACK_LED_BRIGHTNESS, back_led_brightness) == ConfigStoreResult::OK &&
      is_led_brightness_valid(back_led_brightness)) {
    settings.back_led_brightness = static_cast<uint8_t>(back_led_brightness);
  }

  int touch_led_brightness = 0;
  if (store.get_int(KEY_TOUCH_LED_BRIGHTNESS, touch_led_brightness) == ConfigStoreResult::OK &&
      is_touch_led_brightness_valid(touch_led_brightness)) {
    settings.touch_led_brightness = static_cast<uint8_t>(touch_led_brightness);
  }

  bool admin_mode = false;
  if (store.get_bool(KEY_ADMIN_MODE, admin_mode) == ConfigStoreResult::OK) {
    settings.admin_mode = admin_mode;
  }

  // battery_learning_enabled is NOT loaded from NVS — see save_go_settings().
  // The in-struct default (false) wins on every boot; admin must opt in fresh
  // per bench-test session.  KEY_BATTERY_LEARNING is reserved for wire-compat
  // with older BLE clients that may still send the key.

  bool charge_cutoff_at_full = false;
  if (store.get_bool(KEY_CHARGE_CUTOFF, charge_cutoff_at_full) == ConfigStoreResult::OK) {
    settings.charge_cutoff_at_full = charge_cutoff_at_full;
  }

  int sound_select = 0;
  if (store.get_int(KEY_SOUND_SELECT, sound_select) == ConfigStoreResult::OK &&
      is_sound_select_valid(sound_select)) {
    settings.sound_select = static_cast<SoundSelect>(sound_select);
  }

  // Battery-learning run state — persisted (drives multi-cycle auto-resume).
  int blearn_stage = 0;
  if (store.get_int(KEY_BLEARN_STAGE, blearn_stage) == ConfigStoreResult::OK &&
      is_blearn_stage_valid(blearn_stage)) {
    settings.blearn_stage = static_cast<BlearnStage>(blearn_stage);
  }

  int blearn_cycle = 0;
  if (store.get_int(KEY_BLEARN_CYCLE, blearn_cycle) == ConfigStoreResult::OK &&
      blearn_cycle >= 0 && blearn_cycle <= 255) {
    settings.blearn_cycle = static_cast<uint8_t>(blearn_cycle);
  }

  int blearn_itpor_losses = 0;
  if (store.get_int(KEY_BLEARN_ITPOR_LOSSES, blearn_itpor_losses) == ConfigStoreResult::OK &&
      blearn_itpor_losses >= 0 && blearn_itpor_losses <= 255) {
    settings.blearn_itpor_losses = static_cast<uint8_t>(blearn_itpor_losses);
  }

  return settings;
}

bool save_go_settings(ConfigStore &store, const GoSettings &settings) {
  if (!is_measure_interval_valid(settings.measure_interval_seconds)) {
    return false;
  }

  if (!is_inactivity_timeout_valid(settings.inactivity_timeout_seconds)) {
    return false;
  }

  if (!is_gps_interval_valid(settings.gps_interval_seconds)) {
    return false;
  }

  if (!is_gps_mode_valid(static_cast<int>(settings.gps_mode))) {
    return false;
  }

  if (!is_operating_mode_valid(static_cast<int>(settings.operating_mode))) {
    return false;
  }

  if (!is_auto_lock_valid(settings.auto_lock_seconds)) {
    return false;
  }

  if (!is_led_brightness_valid(settings.led_brightness)) {
    return false;
  }

  if (!is_led_brightness_valid(settings.back_led_brightness)) {
    return false;
  }

  if (!is_touch_led_brightness_valid(settings.touch_led_brightness)) {
    return false;
  }

  if (!is_device_name_valid(settings.device_name)) {
    return false;
  }

  if (store.set_int(KEY_MEASURE_INTERVAL_SECONDS, settings.measure_interval_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_INACTIVITY_TIMEOUT_SECONDS, settings.inactivity_timeout_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_GPS_INTERVAL_SECONDS, settings.gps_interval_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_GPS_MODE, static_cast<int>(settings.gps_mode)) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_OPERATING_MODE, static_cast<int>(settings.operating_mode)) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_string(KEY_DEVICE_NAME, settings.device_name) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_USE_FAHRENHEIT, settings.use_fahrenheit) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_PM_USE_USAQI, settings.pm_use_usaqi) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_AUTO_LOCK_SECONDS, settings.auto_lock_seconds) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_LED_BRIGHTNESS, settings.led_brightness) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_BACK_LED_BRIGHTNESS, settings.back_led_brightness) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_TOUCH_LED_BRIGHTNESS, settings.touch_led_brightness) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_ADMIN_MODE, settings.admin_mode) != ConfigStoreResult::OK) {
    return false;
  }

  // battery_learning_enabled is admin-only and intentionally not persisted —
  // see load_go_settings() for rationale.  No write to KEY_BATTERY_LEARNING.

  if (store.set_bool(KEY_CHARGE_CUTOFF, settings.charge_cutoff_at_full) != ConfigStoreResult::OK) {
    return false;
  }

  if (!is_sound_select_valid(static_cast<int>(settings.sound_select))) {
    return false;
  }

  if (store.set_int(KEY_SOUND_SELECT, static_cast<int>(settings.sound_select)) !=
      ConfigStoreResult::OK) {
    return false;
  }

  // Persist the blearn run state alongside the rest of the block.  Stage
  // transitions during a run go through save_blearn_state() (single-key
  // atomic commit), but a full save_go_settings() must not drop these fields.
  if (store.set_int(KEY_BLEARN_STAGE, static_cast<int>(settings.blearn_stage)) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_BLEARN_CYCLE, settings.blearn_cycle) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_BLEARN_ITPOR_LOSSES, settings.blearn_itpor_losses) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.commit() != ConfigStoreResult::OK) {
    return false;
  }

  print_settings(settings);
  return true;
}

bool save_blearn_state(ConfigStore &store, BlearnStage stage, uint8_t cycle,
                       uint8_t itpor_losses) {
  if (!is_blearn_stage_valid(static_cast<int>(stage))) {
    return false;
  }
  if (store.set_int(KEY_BLEARN_STAGE, static_cast<int>(stage)) != ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_int(KEY_BLEARN_CYCLE, cycle) != ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_int(KEY_BLEARN_ITPOR_LOSSES, itpor_losses) != ConfigStoreResult::OK) {
    return false;
  }
  if (store.commit() != ConfigStoreResult::OK) {
    return false;
  }
  AG_LOGI(TAG, "blearn state persisted: stage=%d cycle=%u itpor_losses=%u",
          static_cast<int>(stage), cycle, itpor_losses);
  return true;
}

void print_settings(const GoSettings &settings) {
  AG_LOGI(TAG,
          "** settings | meas_int=%d | gps_int=%d gps_mode=%d "
          "op_mode=%d | inactivity_to=%d auto_lock=%d | fahrenheit=%s usaqi=%s | "
          "led_brightness=%u back_led_brightness=%u touch_led=%u | admin=%s blearn=%s "
          "chg_cutoff=%s chg_curr=%umA sound=%s | device_name=%s **",
          settings.measure_interval_seconds, settings.gps_interval_seconds, settings.gps_mode,
          settings.operating_mode, settings.inactivity_timeout_seconds, settings.auto_lock_seconds,
          settings.use_fahrenheit ? "true" : "false", settings.pm_use_usaqi ? "true" : "false",
          settings.led_brightness, settings.back_led_brightness, settings.touch_led_brightness,
          settings.admin_mode ? "true" : "false",
          settings.battery_learning_enabled ? "true" : "false",
          settings.charge_cutoff_at_full ? "true" : "false",
          static_cast<unsigned>(settings.charge_current_ma),
          sound_select_name(settings.sound_select), settings.device_name.c_str());
}
