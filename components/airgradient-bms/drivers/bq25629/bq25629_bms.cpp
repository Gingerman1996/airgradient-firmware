/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/bq25629/bq25629_bms.h"

#include "esp_log.h"

static constexpr const char *TAG = "BQ25629Bms";

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

  // Extend watchdog to 200s so periodic resets have ample margin.
  err = _charger.set_watchdog_timeout(drivers::WatchdogTimeout::Sec200);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_watchdog_timeout failed: %s", esp_err_to_name(err));
    return false;
  }

  // Configure PMID power path.  Auto-transition (EN_CHG + EN_OTG +
  // EN_BYPASS_OTG) is set by _charger.init(); here we only ensure HIZ
  // is off and set the VOTG target for the OTG boost fallback.
  err = _charger.disable_hiz_mode();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "disable_hiz_mode failed: %s", esp_err_to_name(err));
    return false;
  }

  err = _charger.set_votg_voltage(5200);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_votg_voltage failed: %s", esp_err_to_name(err));
    return false;
  }

  // Reset the watchdog timer after the full post-init sequence.
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
// BmsDevice -- boost
// ---------------------------------------------------------------------------

bool BQ25629Bms::enable_boost() {
  // Re-assert the auto-transition configuration: ensure HIZ is off,
  // EN_OTG and EN_BYPASS_OTG are set, and VOTG target is correct.
  esp_err_t err = _charger.disable_hiz_mode();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "disable_hiz_mode failed: %s", esp_err_to_name(err));
    return false;
  }

  err = _charger.enable_otg(true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "enable_otg failed: %s", esp_err_to_name(err));
    return false;
  }

  err = _charger.enable_bypass_otg(true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "enable_bypass_otg failed: %s", esp_err_to_name(err));
    return false;
  }

  err = _charger.set_votg_voltage(5200);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_votg_voltage failed: %s", esp_err_to_name(err));
    return false;
  }

  return true;
}

bool BQ25629Bms::disable_boost() {
  esp_err_t err = _charger.enable_otg(false);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "disable_boost: enable_otg(false) failed: %s", esp_err_to_name(err));
    return false;
  }

  err = _charger.enable_bypass_otg(false);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "disable_boost: enable_bypass_otg(false) failed: %s", esp_err_to_name(err));
    return false;
  }

  return true;
}
