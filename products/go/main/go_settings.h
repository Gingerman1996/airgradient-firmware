#ifndef GO_SETTINGS_H
#define GO_SETTINGS_H

#include <cstdint>
#include <string>

#include "config_store.h"
#include "go_melody.h"
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
  /// Default = 0 (Off) so production units ship dark; user opts in.
  uint8_t led_brightness = 0;

  /// Back AQI LEDs (LED3/5/6/7/9 — PM2.5 colour indicator).
  /// 5-level 0–100 % scale: 0=Off, 1=25%, 2=50%, 3=75%, 4=100%.
  /// Default = 0 (Off); user opts in.
  uint8_t back_led_brightness = 0;

  /// Touch-feedback LEDs (LED1/LED2/LED10 — white flash on accepted
  /// capacitive touch).  3-level scale: 0=Off, 1=Dim, 2=Bright.
  /// Off suppresses the flash entirely.  Default = 0 (Off).
  uint8_t touch_led_brightness = 0;

  // --- Admin / calibration mode ---
  /// When true, factory-calibration affordances are exposed.  Default
  /// false for production.  Entered by a deliberate touch gesture, exited
  /// from the Settings menu.
  bool admin_mode = false;

  /// Master toggle for the BQ27427 battery-learning workflow.  When ON:
  ///   (a) charge-done UX fires (melody + 500 s rest + LED8 + beep)
  ///   (b) ICHG is overridden to 1500 mA while charging (faster top-up
  ///       so the full learning cycle finishes within a working day)
  ///   (c) on charge-done + USB-removed transition, the device enters
  ///       LOW_POWER mode: PMID boost is forced off (kills SPS30's +5V
  ///       rail), PM_A sampling is skipped, GPS is put into CFG-SLEEP.
  ///       This drops total idle current below the BQ27427's
  ///       `sleep_current_ma` threshold so the gauge can enter Sleep →
  ///       Relax → take its OCV measurement → update Qmax.
  /// Only meaningful inside `admin_mode` — production users never see
  /// the toggle and the workflow is silent regardless of this flag.
  /// Default OFF so production devices ship with no learning behaviour
  /// and admin explicitly opts in for a bench-test session.
  bool battery_learning_enabled = false;

  /// When true, clear the BMS EN_CHG bit as soon as the fuel gauge
  /// reports Full Charge (FC=1), and re-enable when FC clears.  Better
  /// for long-term cell health on units that sit on USB; default Off so
  /// devices keep topping up at 100 % unless an admin opts in.
  bool charge_cutoff_at_full = false;

  /// Fast-charge current limit applied to the BMS (BQ25629 ICHG), in mA.
  /// Admin-only; in production the value stays at 500 mA.  Not persisted
  /// to NVS — exiting admin mode forces the value back to 500 mA so a
  /// reboot from production can never inherit a non-default ICHG.
  /// Allowed values are 200 / 500 / 1000.
  uint16_t charge_current_ma = 500;

  /// Admin-only manual override: when true, the orchestrator clears the
  /// BMS EN_CHG bit unconditionally, regardless of FG Full-Charge state.
  /// Used during BQ27427 learning-cycle bench work to put the cell into
  /// zero-current relax while USB-C stays plugged for serial monitoring.
  /// Higher precedence than `charge_cutoff_at_full`; lower precedence
  /// than the thermal over-temperature cutoff.  Not persisted — admin
  /// must opt back in per session so a stale "disabled" can never carry
  /// across a reboot and prevent the cell from charging.
  bool charge_disabled = false;

  /// User-selected melody preference for the Settings → Play Sound row.
  /// Persisted across reboots; the menu fires the chosen melody on every
  /// re-confirm.  Default Off so production devices stay quiet unless the
  /// user opts in.
  SoundSelect sound_select = SoundSelect::Off;

  // --- Identity ---
  std::string device_name = "airgradient-go";
};

GoSettings load_go_settings(ConfigStore &store);
bool save_go_settings(ConfigStore &store, const GoSettings &settings);
void print_settings(const GoSettings &settings);

#endif // GO_SETTINGS_H
