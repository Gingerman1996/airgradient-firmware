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

  // Extend watchdog to 200s so periodic resets have ample margin.
  err = _charger.set_watchdog_timeout(drivers::WatchdogTimeout::Disable);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_watchdog_timeout failed: %s", esp_err_to_name(err));
    return false;
  }

  drivers::VBusStatus raw_vbus_status{};
  err = _charger.get_vbus_status(raw_vbus_status);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "get_vbus_status failed during init: %s", esp_err_to_name(err));
    return false;
  }

  const BmsPowerSource power_source = map_vbus_status(raw_vbus_status);
  const BmsPmidMode pmid_mode = bms_power_source_has_external_input(power_source)
                                    ? BmsPmidMode::PassThrough
                                    : BmsPmidMode::Boost;

  if (!configure_pmid_mode(pmid_mode)) {
    ESP_LOGE(TAG, "configure_pmid_mode(%s) failed during init", bms_pmid_mode_str(pmid_mode));
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

  // --- Shared preamble (both modes) ---
  //
  // Each register write is followed by a 10 ms settling delay, matching the
  // sequencing in BQ25629::enable_pmid_5v_boost().  Back-to-back writes
  // without delays can leave the IC in a transient state when OTG boost is
  // subsequently enabled, contributing to battery-side inrush brownout.
  //
  // 1. HIZ off    — required for any active PMID operation.
  // 2. TS config  — ensure TS check state is defined before OTG enable.
  // 3. VOTG 5 V   — target for the OTG boost converter (harmless when OTG is
  //                  disabled, but keeps the register primed for a later switch).
  // 4. Bypass off — EN_BYPASS_OTG connects battery directly to PMID without
  //                 regulation.  Neither pass-through nor regulated boost
  //                 wants that path enabled.

  static constexpr uint32_t STEP_DELAY_MS = 10;

  esp_err_t err = _charger.disable_hiz_mode();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "disable_hiz_mode failed: %s", esp_err_to_name(err));
    return false;
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  err = _charger.set_ts_ignore(false);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set_ts_ignore failed: %s (continuing)", esp_err_to_name(err));
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  err = _charger.set_votg_voltage(5100);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_votg_voltage failed: %s", esp_err_to_name(err));
    return false;
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  err = _charger.enable_bypass_otg(false);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "enable_bypass_otg(false) failed: %s", esp_err_to_name(err));
    return false;
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  // --- Mode-specific: only EN_OTG differs ---
  //
  // PassThrough — PMID is fed from external input; OTG boost is off.
  // Boost       — OTG boost converts battery to regulated 5 V on PMID.

  const bool otg_enable = (mode == BmsPmidMode::Boost);

  // Log battery/system voltages before OTG toggle.  A weak battery may sag
  // below the brownout threshold when the boost converter starts, causing a
  // reboot loop
  drivers::BQ25629_ADC_Data adc{};
  if (_charger.read_adc(adc) == ESP_OK) {
    ESP_LOGI(TAG, "pre-OTG ADC: vbat=%umV vsys=%umV vpmid=%umV vbus=%umV ibat=%dmA", adc.vbat_mv,
             adc.vsys_mv, adc.vpmid_mv, adc.vbus_mv, adc.ibat_ma);
  }

  err = _charger.enable_otg(otg_enable);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "enable_otg(%s) failed: %s", otg_enable ? "true" : "false", esp_err_to_name(err));
    return false;
  }

  // Wait for PMID rails stable
  RTOS::delay_ms(300);

  // Verify the chip actually honored the request.  EN_OTG can be cleared
  // autonomously by the IC on BAT_OTGZ, OTG hiccup, TS-out-of-window, etc.
  // (datasheet §8.3.10.3 / §8.3.10.4) — the register cache cannot be trusted
  // as authoritative.  Read the truth out of the chip and log it so the
  // failure mode is visible when boost OTG silently refuses to enter.
  uint8_t ctrl2 = 0;
  uint8_t status1 = 0;
  uint8_t fault0 = 0;
  drivers::BQ25629_ADC_Data post_adc{};
  const bool have_ctrl2 = _charger.read_register(0x18, ctrl2) == ESP_OK;
  const bool have_status1 = _charger.read_register(0x1E, status1) == ESP_OK;
  const bool have_fault0 = _charger.read_register(0x1F, fault0) == ESP_OK;
  const bool have_adc = _charger.read_adc(post_adc) == ESP_OK;
  const bool en_otg_bit = have_ctrl2 && (ctrl2 & (1 << 6));
  const bool en_bypass_bit = have_ctrl2 && (ctrl2 & (1 << 7));
  const uint8_t vbus_stat = have_status1 ? (status1 & 0x07) : 0xFF;
  ESP_LOGI(TAG,
           "post-OTG verify: ctrl2=0x%02x (EN_OTG=%d EN_BYPASS=%d) vbus_stat=0x%x "
           "fault0=0x%02x vpmid=%umV vbat=%umV",
           ctrl2, en_otg_bit, en_bypass_bit, vbus_stat, fault0,
           have_adc ? post_adc.vpmid_mv : 0, have_adc ? post_adc.vbat_mv : 0);

  if (otg_enable && !en_otg_bit) {
    // Chip dropped EN_OTG on us — boost OTG entry was refused or aborted.
    // Do NOT cache the requested mode; leave _pmid_mode unchanged so the
    // next sync_pmid_mode() poll re-attempts the configuration.
    ESP_LOGW(TAG, "boost OTG refused by chip (EN_OTG=0 after enable). fault0=0x%02x", fault0);
    return false;
  }

  _pmid_mode = mode;
  ESP_LOGI(TAG, "PMID mode set to %s", bms_pmid_mode_str(mode));
  return true;
}
