#ifndef GO_SETTINGS_H
#define GO_SETTINGS_H

#include <string>

#include "config_store.h"
#include "go_types.h"

struct GoSettings {
  // --- Measurement interval ---
  int measure_interval_seconds = 10; // 1..3600

  // --- Display ---
  bool use_fahrenheit = false;
  bool pm_use_usaqi = false;

  // --- GPS ---
  int gps_interval_seconds = 5;
  GpsMode gps_mode = GpsMode::OnWhenTracking;

  // --- Device behavior ---
  OperatingMode operating_mode = OperatingMode::Portable;
  int inactivity_timeout_seconds = 5;
  int auto_lock_seconds = 0; // 0 = auto-lock disabled

  // --- LED brightness (5 levels: 0=Off, 1=25%, 2=50%, 3=75%, 4=100%) ---
  /// Front display indicator LEDs (LED25 / LED26 via LP5036 OUT30/31).
  uint8_t led_brightness = 4;

  /// Back AQI LEDs (LED3/5/6/7/9 — PM2.5 colour indicator).
  uint8_t back_led_brightness = 4;

  // --- Admin / calibration mode ---
  /// When true, factory-calibration affordances are exposed (e.g. the
  /// charge-done melody + 500 s rest countdown + unplug-me alert used to
  /// guide BQ27427 Qmax learning).  Default false for production — those
  /// affordances are noise for end users.  Entered by a deliberate touch
  /// gesture, exited from the Settings menu.
  bool admin_mode = false;

  // --- Identity ---
  std::string device_name = "airgradient-go";
};

GoSettings load_go_settings(ConfigStore &store);
bool save_go_settings(ConfigStore &store, const GoSettings &settings);
void print_settings(const GoSettings &settings);

#endif // GO_SETTINGS_H
