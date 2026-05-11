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

bool is_measure_interval_valid(int value) { return value >= 1 && value <= 3600; }

bool is_inactivity_timeout_valid(int value) { return value >= 5 && value <= 600; }

bool is_gps_interval_valid(int value) { return value >= 1 && value <= 60; }

bool is_gps_mode_valid(int value) { return value >= 0 && value <= 2; }

bool is_operating_mode_valid(int value) { return value >= 0 && value <= 2; }

bool is_auto_lock_valid(int value) {
  return value == 0 || value == 10 || value == 30 || value == 60;
}

bool is_led_brightness_valid(int value) { return value >= 0 && value <= 4; }

bool is_device_name_valid(const std::string &value) { return !value.empty() && value.size() <= 64; }

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

  if (store.commit() != ConfigStoreResult::OK) {
    return false;
  }

  print_settings(settings);
  return true;
}

void print_settings(const GoSettings &settings) {
  AG_LOGI(TAG,
          "** settings | meas_int=%d | gps_int=%d gps_mode=%d "
          "op_mode=%d | inactivity_to=%d auto_lock=%d | fahrenheit=%s usaqi=%s | "
          "led_brightness=%u | device_name=%s **",
          settings.measure_interval_seconds, settings.gps_interval_seconds, settings.gps_mode,
          settings.operating_mode, settings.inactivity_timeout_seconds, settings.auto_lock_seconds,
          settings.use_fahrenheit ? "true" : "false", settings.pm_use_usaqi ? "true" : "false",
          settings.led_brightness, settings.device_name.c_str());
}
