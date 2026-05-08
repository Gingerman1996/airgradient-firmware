/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/bq25629/bq25629_bms.h"

#include "esp_log.h"
#include "rtos.h"

static constexpr const char *TAG = "BQ25629Bms";

static BmsPowerSource map_vbus_status(drivers::VBusStatus vs);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

BQ25629Bms::BQ25629Bms(i2c_master_bus_handle_t i2c_bus, const drivers::BQ25629_Config &config,
                       uint8_t address)
    : _charger(i2c_bus, address), _config(config) {}

// ---------------------------------------------------------------------------
// BmsDevice -- init
// ---------------------------------------------------------------------------

bool BQ25629Bms::init() {
  esp_err_t err = _charger.init(_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "BQ25629 init failed: %s", esp_err_to_name(err));
    return false;
  }

  // Watchdog disabled so the chip never auto-clears EN_OTG behind our back.
  err = _charger.set_watchdog_timeout(drivers::WatchdogTimeout::Disable);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_watchdog_timeout failed: %s", esp_err_to_name(err));
    return false;
  }

  // Configure OTG once. After this point the chip handles buck↔boost
  // transitions autonomously based on its own VBUS-detect:
  //   VBUS present → buck (charging from VBUS, EN_OTG masked internally)
  //   VBUS absent  → boost (PMID = 5 V from VBAT)
  // We never write EN_OTG again.
  //
  // The chip has a silicon interlock that requires VBUS < VBAT + V_SLEEP
  // (~45 mV typ; BQ25628 datasheet §8.3.6.1 condition #2) before the boost
  // converter is allowed to start. With v0.3 hardware we saw VBUS float at
  // ~VBAT after USB unplug due to back-feed on the VBUS net — the chip
  // stayed out of Sleep mode, the boost stayed gated, and PMID was pinned
  // near VBAT until the battery was physically removed. v0.4 adds a 10 kΩ
  // pulldown on VBUS to drain it within tens of ms after unplug so the
  // chip's V_SLEEP comparator trips and OTG engages naturally.
  //
  // Software-driven mode transitions (REG_RST + EN_OTG toggling around the
  // unplug event) were also observed to latch the chip into a state with
  // vbus_stat=7 / en_otg=1 / no fault flags but vpmid stuck near vbat,
  // recoverable only by VBAT removal — so the safest behavior is to leave
  // EN_OTG armed and let the chip's autonomous logic handle transitions.
  if (!_apply_otg_config()) {
    ESP_LOGE(TAG, "apply_otg_config failed during init");
    return false;
  }

  // Initial _pmid_mode reflects what the chip is doing right now, derived
  // from VBUS_STAT. Used only for telemetry / orchestrator UI.
  drivers::VBusStatus raw_vbus_status{};
  err = _charger.get_vbus_status(raw_vbus_status);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "get_vbus_status failed during init: %s", esp_err_to_name(err));
    return false;
  }
  const BmsPowerSource power_source = map_vbus_status(raw_vbus_status);
  _pmid_mode = bms_power_source_has_external_input(power_source) ? BmsPmidMode::PassThrough
                                                                 : BmsPmidMode::Boost;
  ESP_LOGI(TAG, "Initial PMID mode: %s", bms_pmid_mode_str(_pmid_mode));

  err = _charger.reset_watchdog();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "reset_watchdog failed: %s", esp_err_to_name(err));
    return false;
  }

  ESP_LOGI(TAG, "BQ25629Bms initialized");
  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- telemetry
// ---------------------------------------------------------------------------

bool BQ25629Bms::read_telemetry(BmsTelemetry &out) {
  out = BmsTelemetry{}; // Reset all fields to invalid sentinels.

  drivers::BQ25629_ADC_Data adc{};
  esp_err_t err = _charger.read_adc(adc);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "read_adc failed: %s", esp_err_to_name(err));
    return false;
  }

  // Voltages — convert mV to V for the legacy float fields.
  out.battery_voltage = static_cast<float>(adc.vbat_mv) / 1000.0f;
  out.charging_voltage = static_cast<float>(adc.vbus_mv) / 1000.0f;

  // Currents
  out.input_current_ma = adc.ibus_ma;
  out.battery_current_ma = adc.ibat_ma;

  // Additional voltages (mV, native ADC units)
  out.system_voltage_mv = adc.vsys_mv;
  out.pmid_voltage_mv = adc.vpmid_mv;

  // Temperature
  out.ts_percent = adc.ts_percent;
  out.die_temperature_c = adc.tdie_c;

  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- status
// ---------------------------------------------------------------------------

/// Map vendor ChargeStatus to the shared BmsChargingState enum.
///
/// The BQ25629 uses a 2-bit charge status field that cannot distinguish
/// trickle, pre-charge, and fast-charge individually.  The combined value
/// TRICKLE_PRECHARGE_FASTCHARGE is mapped to FastCharge because CC-mode
/// fast-charging is by far the most common state represented by this code.
static BmsChargingState map_charge_status(drivers::ChargeStatus cs) {
  switch (cs) {
  case drivers::ChargeStatus::NOT_CHARGING:
    return BmsChargingState::NotCharging;
  case drivers::ChargeStatus::TRICKLE_PRECHARGE_FASTCHARGE:
    return BmsChargingState::FastCharge;
  case drivers::ChargeStatus::TAPER_CHARGE:
    return BmsChargingState::TaperCharge;
  case drivers::ChargeStatus::TOPOFF_TIMER_ACTIVE:
    return BmsChargingState::TopOffTimerActiveCharging;
  default:
    return BmsChargingState::Unknown;
  }
}

/// Map vendor VBusStatus to the shared BmsPowerSource enum.
static BmsPowerSource map_vbus_status(drivers::VBusStatus vs) {
  switch (vs) {
  case drivers::VBusStatus::NO_ADAPTER:
    return BmsPowerSource::None;
  case drivers::VBusStatus::USB_SDP:
    return BmsPowerSource::UsbSdp;
  case drivers::VBusStatus::USB_CDP:
    return BmsPowerSource::UsbCdp;
  case drivers::VBusStatus::USB_DCP:
    return BmsPowerSource::UsbDcp;
  case drivers::VBusStatus::UNKNOWN_ADAPTER:
    return BmsPowerSource::UnknownAdapter;
  case drivers::VBusStatus::NON_STANDARD:
    return BmsPowerSource::NonStandard;
  case drivers::VBusStatus::OTG_MODE:
    return BmsPowerSource::OtgMode;
  default:
    return BmsPowerSource::Unknown;
  }
}

bool BQ25629Bms::read_status(BmsStatus &out) {
  drivers::BQ25629_Status raw{};
  esp_err_t err = _charger.read_status(raw);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "read_status failed: %s", esp_err_to_name(err));
    out.charging_state = BmsChargingState::Unknown;
    out.power_source = BmsPowerSource::Unknown;
    return false;
  }

  out.charging_state = map_charge_status(raw.charge_status);
  out.power_source = map_vbus_status(raw.vbus_status);
  out.thermal_regulation = raw.treg_stat;
  out.vsys_regulation = raw.vsys_stat;
  out.input_current_regulation = raw.iindpm_stat;
  out.input_voltage_regulation = raw.vindpm_stat;
  out.safety_timer_expired = raw.safety_tmr_stat;
  out.watchdog_expired = raw.wd_stat;
  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- charging state (lightweight, single register)
// ---------------------------------------------------------------------------

bool BQ25629Bms::get_charging_state(BmsChargingState &state) {
  drivers::ChargeStatus raw{};
  esp_err_t err = _charger.get_charge_status(raw);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "get_charge_status failed: %s", esp_err_to_name(err));
    state = BmsChargingState::Unknown;
    return false;
  }
  state = map_charge_status(raw);
  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- battery percentage
// ---------------------------------------------------------------------------

bool BQ25629Bms::get_battery_percentage(float *output) {
  if (output == nullptr) {
    return false;
  }

  uint8_t percent = 0;
  esp_err_t err = _charger.estimate_battery_percent(percent);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "estimate_battery_percent failed: %s", esp_err_to_name(err));
    return false;
  }

  *output = static_cast<float>(percent);
  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- watchdog
// ---------------------------------------------------------------------------

bool BQ25629Bms::update_watchdog() {
  esp_err_t err = _charger.reset_watchdog();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "reset_watchdog failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- ship mode
// ---------------------------------------------------------------------------

bool BQ25629Bms::feature_ship_available() const { return true; }

bool BQ25629Bms::enter_ship_mode() {
  esp_err_t err = _charger.enter_ship_mode();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "enter_ship_mode failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- PMID mode
// ---------------------------------------------------------------------------

bool BQ25629Bms::configure_pmid_mode(BmsPmidMode mode) {
  if (mode == BmsPmidMode::Unknown) {
    ESP_LOGW(TAG, "configure_pmid_mode: refusing Unknown mode");
    return false;
  }

  if (_pmid_mode == mode) {
    return true;
  }

  // No I²C writes here. The chip's own VBUS-detect drives the buck↔boost
  // transition autonomously; EN_OTG was armed once in init() and stays at
  // 1 forever. See init() for the full background on why we don't touch
  // EN_OTG on plug/unplug events (silicon interlock + v0.4 board fix).
  ESP_LOGI(TAG, "PMID mode -> %s (chip handles transition autonomously)",
           bms_pmid_mode_str(mode));
  _pmid_mode = mode;
  return true;
}

// ---------------------------------------------------------------------------
// One-shot OTG configuration (called once from init())
// ---------------------------------------------------------------------------

bool BQ25629Bms::_apply_otg_config() {
  static constexpr uint32_t STEP_DELAY_MS = 10;

  // 1. HIZ off    — required for any active PMID operation.
  esp_err_t err = _charger.disable_hiz_mode();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "disable_hiz_mode failed: %s", esp_err_to_name(err));
    return false;
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  // 2. TS check on — define TS state before EN_OTG.
  err = _charger.set_ts_ignore(false);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set_ts_ignore failed: %s (continuing)", esp_err_to_name(err));
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  // 3. VOTG = 5.1 V — boost target (effective whenever boost engages).
  err = _charger.set_votg_voltage(5100);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_votg_voltage failed: %s", esp_err_to_name(err));
    return false;
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  // 4. Bypass off — never want direct VBAT→PMID without regulation.
  err = _charger.enable_bypass_otg(false);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "enable_bypass_otg(false) failed: %s", esp_err_to_name(err));
    return false;
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  // 5. EN_OTG = 1 — armed once, stays at 1 forever. The chip masks it
  //    internally while VBUS is detected and re-engages the boost when VBUS
  //    drops, without any further register writes.
  err = _charger.enable_otg(true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "enable_otg(true) failed: %s", esp_err_to_name(err));
    return false;
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  drivers::BQ25629_ADC_Data adc{};
  if (_charger.read_adc(adc) == ESP_OK) {
    ESP_LOGI(TAG, "post-init OTG: vbat=%umV vsys=%umV vpmid=%umV vbus=%umV ibat=%dmA",
             adc.vbat_mv, adc.vsys_mv, adc.vpmid_mv, adc.vbus_mv, adc.ibat_ma);
  }

  return true;
}
