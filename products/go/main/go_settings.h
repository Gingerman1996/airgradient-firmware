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

  // --- LED brightness ---
  /// Front display indicator LEDs (LED25 / LED26 via LP5036 OUT30/31).
  /// 6-level 0–10 % scale: 0=Off, 1=2%, 2=4%, 3=6%, 4=8%, 5=10%.  The
  /// display LEDs are small and very bright; even 10 % is visible
  /// indoors so the user-facing scale is intentionally compressed.
  /// Default = 5 (10 %, the brightest available option).
  uint8_t led_brightness = 5;

  /// Back AQI LEDs (LED3/5/6/7/9 — PM2.5 colour indicator).
  /// 5-level 0–100 % scale: 0=Off, 1=25%, 2=50%, 3=75%, 4=100%.
  uint8_t back_led_brightness = 4;

  // --- Admin / calibration mode ---
  /// When true, factory-calibration affordances are exposed.  Default
  /// false for production.  Entered by a deliberate touch gesture, exited
  /// from the Settings menu.
  bool admin_mode = false;

  /// Master toggle for the BQ27427 battery-learning UX (charge-done
  /// melody + 500 s rest countdown + LED8 unplug-me blink + alert beep).
  /// Only meaningful inside `admin_mode` — production users never see
  /// the toggle and the UX stays silent regardless of this flag.
  /// Default true so calibration runs immediately after the first admin
  /// entry; admin can disable the notifications once Qmax has converged.
  bool battery_learning_enabled = true;

  // --- Identity ---
  std::string device_name = "airgradient-go";
};

GoSettings load_go_settings(ConfigStore &store);
bool save_go_settings(ConfigStore &store, const GoSettings &settings);
void print_settings(const GoSettings &settings);

#endif // GO_SETTINGS_H
