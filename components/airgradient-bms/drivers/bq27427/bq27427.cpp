/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/bq27427/bq27427.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char *TAG = "BQ27427";

// Standard Command addresses (TRM §5).
constexpr uint8_t CMD_CONTROL = 0x00;
constexpr uint8_t CMD_TEMPERATURE = 0x02;
constexpr uint8_t CMD_VOLTAGE = 0x04;
constexpr uint8_t CMD_FLAGS = 0x06;
constexpr uint8_t CMD_AVG_CURRENT = 0x10;
constexpr uint8_t CMD_AVG_POWER = 0x18;
constexpr uint8_t CMD_SOC = 0x1C;
constexpr uint8_t CMD_INT_TEMP = 0x1E;
constexpr uint8_t CMD_REMAIN_CAP = 0x2A;        // Filtered
constexpr uint8_t CMD_FULL_CHARGE_CAP = 0x2E;   // Filtered

// Control() subcommands (TRM §4).
constexpr uint16_t CTRL_CONTROL_STATUS = 0x0000;
constexpr uint16_t CTRL_DEVICE_TYPE = 0x0001;
constexpr uint16_t CTRL_CHEM_ID = 0x0008;
constexpr uint16_t CTRL_SET_CFGUPDATE = 0x0013;
constexpr uint16_t CTRL_CHEM_B = 0x0031; // selects Chem ID 1202 (4.2 V) — TRM p7/§5.1.15
constexpr uint16_t CTRL_RESET = 0x0041;
constexpr uint16_t CTRL_SOFT_RESET = 0x0042;

// Chemistry profile IDs returned by Control(CHEM_ID) (TRM p7).  The chip
// defaults to 0x3230 (4.35 V); this board's cell charges to 4.20 V, so we
// need the 4.2 V profile.  Confirm these against the first on-device read.
constexpr uint16_t CHEM_ID_4V2 = 0x1202;

// CONTROL_STATUS low-byte bit 0 = CHEMCHANGE — set when a chemistry change
// has been requested/accepted (TRM §5.1.1).
constexpr uint16_t CONTROL_STATUS_CHEMCHANGE = (1u << 0);
// Per TRM §7.1.3, UNSEAL is performed by writing the unseal key (`0x8000`)
// to Control() twice.  Both halves of the BQ27427 unseal key are identical.
constexpr uint16_t CTRL_UNSEAL_KEY = 0x8000;

// Extended command interface (TRM §6).
constexpr uint8_t CMD_DATA_BLOCK_CLASS = 0x3E;
constexpr uint8_t CMD_DATA_BLOCK = 0x3F;
constexpr uint8_t CMD_BLOCK_DATA_BASE = 0x40;
constexpr uint8_t CMD_BLOCK_DATA_CHECKSUM = 0x60;
constexpr uint8_t CMD_BLOCK_DATA_CONTROL = 0x61;

// Subclass / offsets in Data Memory (TRM §7.4.2.3.5–7.4.2.3.10).
constexpr uint8_t SUBCLASS_STATE = 0x52;        // 82 decimal
constexpr uint8_t OFFSET_QMAX_CELL0 = 0;        // bytes 0/1 (TRM §7.4.2.3.1)
constexpr uint8_t OFFSET_UPDATE_STATUS = 2;     // byte 2 (TRM §7.4.2.3.2)
constexpr uint8_t OFFSET_DESIGN_CAPACITY = 6;   // bytes 6/7 within block 0
constexpr uint8_t OFFSET_DESIGN_ENERGY = 8;     // bytes 8/9
constexpr uint8_t OFFSET_TERMINATE_VOLTAGE = 10; // bytes 10/11
constexpr uint8_t OFFSET_SLEEP_CURRENT = 23;    // bytes 23/24

// Ra (impedance) table — 15 contiguous int16 values, MSB-first, in their own
// subclass with no header word (TRM §7.4.3, p48).
constexpr uint8_t SUBCLASS_RA0_RAM = 0x59;      // 89 decimal

// Flags() bit 4 = CFGUPDATE mode active.
constexpr uint16_t FLAG_CFGUPDATE = (1u << 4);

// Update Status (subclass 0x52, offset 2) — bit0 (Qmax) + bit1 (Ra).  Setting
// both removes the per-update change limits for a from-scratch learn (TRM
// §7.4.2.3.2, p43; fg_learning_sequence.md:55/93).
constexpr uint8_t UPDATE_STATUS_LEARN_BITS = 0x03;

// Per datasheet §6.3.1.3, the Control() subcommand result is not ready
// immediately after the write — but the spec says only that read-WRITE
// standard commands need 2 s.  Control() is treated as read-only after the
// subcommand select, and works without delay in practice.  Add a small
// settle wait to be conservative.
constexpr int CONTROL_SETTLE_MS = 2;
} // namespace

BQ27427::BQ27427(i2c_master_bus_handle_t i2c_bus) : _bus(i2c_bus) {}

BQ27427::BQ27427(i2c_master_bus_handle_t i2c_bus, const Config &config)
    : _bus(i2c_bus), _config(config) {}

BQ27427::~BQ27427() {
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
  }
}

bool BQ27427::init() {
  if (_dev != nullptr) {
    return true; // already attached
  }

  esp_err_t err = i2c_master_probe(_bus, _config.address, _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "probe at 0x%02X failed: %s", _config.address, esp_err_to_name(err));
    return false;
  }

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _config.address,
      .scl_speed_hz = _config.scl_speed_hz,
      // Datasheet §6.3.1.4: the gauge can clock-stretch up to ~4 ms during
      // INITIALIZATION / NORMAL modes while it performs data-flow control,
      // and longer during Data Memory writes that trigger flash commits.
      // 0 = default-no-wait would cause ESP_ERR_INVALID_STATE on those.
      .scl_wait_us = 20000,
      .flags = {},
  };
  err = i2c_master_bus_add_device(_bus, &dev_cfg, &_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(err));
    _dev = nullptr;
    return false;
  }

  uint16_t device_type = 0;
  if (!control_subcommand(CTRL_DEVICE_TYPE, device_type)) {
    ESP_LOGE(TAG, "DEVICE_TYPE read failed");
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
    return false;
  }
  if (device_type != DEVICE_TYPE_BQ27427) {
    ESP_LOGE(TAG, "DEVICE_TYPE=0x%04X, expected 0x%04X — not a BQ27427", device_type,
             DEVICE_TYPE_BQ27427);
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
    return false;
  }

  ESP_LOGI(TAG, "BQ27427 found at 0x%02X (DEVICE_TYPE=0x%04X)", _config.address, device_type);
  return true;
}

// ---------------------------------------------------------------------------
// Standard Command reads
// ---------------------------------------------------------------------------

bool BQ27427::read_soc_percent(uint8_t &out) {
  uint16_t raw = 0;
  if (!_read_word(CMD_SOC, raw)) {
    return false;
  }
  if (raw > 100) {
    // Out-of-range — gauge may not be initialised yet.  Don't lie to caller.
    return false;
  }
  out = static_cast<uint8_t>(raw);
  return true;
}

bool BQ27427::read_voltage_mv(uint16_t &out) { return _read_word(CMD_VOLTAGE, out); }

bool BQ27427::read_average_current_ma(int16_t &out) {
  uint16_t raw = 0;
  if (!_read_word(CMD_AVG_CURRENT, raw)) {
    return false;
  }
  out = static_cast<int16_t>(raw); // signed 16-bit
  return true;
}

bool BQ27427::read_internal_temperature_dk(uint16_t &out) { return _read_word(CMD_INT_TEMP, out); }

bool BQ27427::read_internal_temperature_c(float &out) {
  uint16_t dk = 0;
  if (!read_internal_temperature_dk(dk)) {
    return false;
  }
  out = (static_cast<float>(dk) * 0.1f) - 273.15f;
  return true;
}

bool BQ27427::read_remaining_capacity_mah(uint16_t &out) { return _read_word(CMD_REMAIN_CAP, out); }

bool BQ27427::read_full_charge_capacity_mah(uint16_t &out) {
  return _read_word(CMD_FULL_CHARGE_CAP, out);
}

bool BQ27427::read_average_power_mw(int16_t &out) {
  uint16_t raw = 0;
  if (!_read_word(CMD_AVG_POWER, raw)) {
    return false;
  }
  out = static_cast<int16_t>(raw);
  return true;
}

bool BQ27427::read_flags(uint16_t &out) { return _read_word(CMD_FLAGS, out); }

bool BQ27427::control_subcommand(uint16_t subcmd, uint16_t &result) {
  if (!_write_word(CMD_CONTROL, subcmd)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(CONTROL_SETTLE_MS));
  return _read_word(CMD_CONTROL, result);
}

// ---------------------------------------------------------------------------
// I²C helpers
// ---------------------------------------------------------------------------

bool BQ27427::_read_word(uint8_t cmd, uint16_t &out) {
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[2] = {0, 0};
  // Single transaction: write 1-byte command, repeated-start, read 2 bytes.
  esp_err_t err = i2c_master_transmit_receive(_dev, &cmd, 1, buf, sizeof(buf), _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "read cmd 0x%02X failed: %s", cmd, esp_err_to_name(err));
    return false;
  }
  // BQ27xxx standard commands are little-endian (LSB at cmd, MSB at cmd+1).
  out = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
  return true;
}

bool BQ27427::_read_byte(uint8_t reg, uint8_t &out) {
  if (_dev == nullptr) {
    return false;
  }
  esp_err_t err = i2c_master_transmit_receive(_dev, &reg, 1, &out, 1, _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "read byte 0x%02X failed: %s", reg, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool BQ27427::_write_byte(uint8_t reg, uint8_t value) {
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[2] = {reg, value};
  esp_err_t err = i2c_master_transmit(_dev, buf, sizeof(buf), _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write byte 0x%02X failed: %s", reg, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool BQ27427::_read_block(uint8_t reg, uint8_t *buf, size_t len) {
  if (_dev == nullptr || buf == nullptr || len == 0) {
    return false;
  }
  esp_err_t err = i2c_master_transmit_receive(_dev, &reg, 1, buf, len, _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "read block 0x%02X len=%u failed: %s", reg, (unsigned)len,
             esp_err_to_name(err));
    return false;
  }
  return true;
}

bool BQ27427::_write_block(uint8_t reg, const uint8_t *buf, size_t len) {
  if (_dev == nullptr || buf == nullptr || len == 0 || len > 64) {
    return false;
  }
  uint8_t tx[65];
  tx[0] = reg;
  for (size_t i = 0; i < len; ++i) {
    tx[1 + i] = buf[i];
  }
  esp_err_t err = i2c_master_transmit(_dev, tx, len + 1, _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write block 0x%02X len=%u failed: %s", reg, (unsigned)len,
             esp_err_to_name(err));
    return false;
  }
  return true;
}

bool BQ27427::_select_data_block(uint8_t subclass, uint8_t block_offset) {
  // TRM §4.1 step 4–6: enable raw block access, then point at the right
  // subclass + 32-byte block within it.
  if (!_write_byte(CMD_BLOCK_DATA_CONTROL, 0x00)) {
    return false;
  }
  if (!_write_byte(CMD_DATA_BLOCK_CLASS, subclass)) {
    return false;
  }
  if (!_write_byte(CMD_DATA_BLOCK, block_offset)) {
    return false;
  }
  return true;
}

bool BQ27427::_unseal() {
  // TRM §7.1.3: write the unseal key twice to Control().  Both halves are
  // the same for BQ27427 (0x8000).  No-op when the chip is already UNSEALED,
  // so it's safe to call unconditionally before any Data Memory write.
  if (!_write_word(CMD_CONTROL, CTRL_UNSEAL_KEY)) {
    return false;
  }
  if (!_write_word(CMD_CONTROL, CTRL_UNSEAL_KEY)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  return true;
}

bool BQ27427::_wait_cfgupdate_flag(bool expected_set, uint32_t timeout_ms) {
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  while (true) {
    uint16_t flags = 0;
    if (read_flags(flags)) {
      const bool now_set = (flags & FLAG_CFGUPDATE) != 0;
      if (now_set == expected_set) {
        return true;
      }
    }
    if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
      ESP_LOGW(TAG, "CFGUPDATE flag did not become %s within %ums",
               expected_set ? "set" : "clear", timeout_ms);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

bool BQ27427::read_design_capacity_mah(uint16_t &out) {
  // No CFGUPDATE needed for reads — just point at the block and read.  TRM
  // §4.1: offsets 0–31 live in block 0.  Data Memory is MSB-first.
  //
  // IMPORTANT: reads must START at 0x40 to trigger the chip's block-buffer
  // fill from DM.  A read that starts at 0x46 (the Design Capacity offset)
  // directly returns stale/zero bytes — the chip only populates the buffer
  // when accessed from the block base.  So we read 8 bytes from 0x40 and
  // pick offsets 6/7 from the buffer.
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  uint8_t buf[8] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, buf, sizeof(buf))) {
    return false;
  }
  out = (static_cast<uint16_t>(buf[OFFSET_DESIGN_CAPACITY]) << 8) |
        buf[OFFSET_DESIGN_CAPACITY + 1];
  return true;
}

bool BQ27427::read_qmax_cell0(uint16_t &raw_out) {
  // Pure read — no CFGUPDATE.  Qmax Cell 0 is the first word of the State
  // subclass block (TRM §7.4.2.3.1, p43).  Same constraint as
  // read_design_capacity_mah: the block read must START at 0x40 so the chip
  // fills its buffer from Data Memory; we then pick offsets 0/1 (MSB-first).
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  uint8_t buf[8] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, buf, sizeof(buf))) {
    return false;
  }
  raw_out = (static_cast<uint16_t>(buf[OFFSET_QMAX_CELL0]) << 8) |
            buf[OFFSET_QMAX_CELL0 + 1];
  return true;
}

bool BQ27427::read_ra_table(int16_t out[RA_TABLE_SIZE]) {
  // Pure read — no CFGUPDATE.  The 15 Ra grid values live in their own
  // subclass (Ra0 RAM, 0x59) as contiguous MSB-first int16 at offsets 0..29
  // with no header word, all within block 0 (TRM §7.4.3, p48).  Block
  // transfers are UNSEALED-only (TRM §7.1.1–7.1.2, p29) — no mode change.
  if (out == nullptr) {
    return false;
  }
  if (!_select_data_block(SUBCLASS_RA0_RAM, 0x00)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  uint8_t buf[RA_TABLE_SIZE * 2] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, buf, sizeof(buf))) {
    return false;
  }
  for (int i = 0; i < RA_TABLE_SIZE; ++i) {
    out[i] = static_cast<int16_t>((static_cast<uint16_t>(buf[i * 2]) << 8) |
                                  buf[i * 2 + 1]);
  }
  return true;
}

bool BQ27427::read_chem_id(uint16_t &out) {
  // Control(CHEM_ID) returns the active 16-bit chemistry profile ID (TRM
  // §5.1.15 / p7).  Pure read — no CFGUPDATE.
  return control_subcommand(CTRL_CHEM_ID, out);
}

bool BQ27427::select_chemistry_4v2() {
  if (_dev == nullptr) {
    return false;
  }

  // Idempotent: only switch when the active profile is not already 4.2 V, so
  // we never enter CFGUPDATE (and never wipe learning) on a correctly-
  // configured chip.  Mirrors set_design_capacity_mah / configure_cell.
  uint16_t chem = 0;
  if (!read_chem_id(chem)) {
    ESP_LOGW(TAG, "Chem ID read failed — cannot verify/select chemistry");
    return false;
  }
  if (chem == CHEM_ID_4V2) {
    ESP_LOGI(TAG, "Chem ID already 0x%04X (4.2 V profile) — no change", chem);
    return true;
  }
  ESP_LOGW(TAG, "Chem ID is 0x%04X (not 4.2 V) — switching to CHEM_B (0x%04X). "
                "This RESETS Impedance-Track learning (Qmax/Ra cleared).",
           chem, CHEM_ID_4V2);

  // UNSEAL — chemistry selection is an UNSEALED operation; no-op if already
  // unsealed.
  if (!_unseal()) {
    return false;
  }

  // Enter CFGUPDATE (TRM p18 §4.2): CHEM_A/B/C are only honoured in this mode.
  if (!_write_word(CMD_CONTROL, CTRL_SET_CFGUPDATE)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(true, 2000)) {
    ESP_LOGW(TAG, "Could not enter CFGUPDATE — aborting chemistry switch");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(1100)); // TRM: allow the chip to settle in CFGUPDATE

  // Select CHEM_B → Chem ID 1202 (4.2 V).
  if (!_write_word(CMD_CONTROL, CTRL_CHEM_B)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(1100)); // let the profile load before SOFT_RESET

  // SOFT_RESET exits CFGUPDATE and re-initialises IT with the new chemistry.
  if (!_write_word(CMD_CONTROL, CTRL_SOFT_RESET)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(false, 2000)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(50)); // chip needs time after SOFT_RESET

  uint16_t status = 0;
  if (control_subcommand(CTRL_CONTROL_STATUS, status)) {
    ESP_LOGI(TAG, "CONTROL_STATUS=0x%04X (CHEMCHANGE=%d)", status,
             (status & CONTROL_STATUS_CHEMCHANGE) ? 1 : 0);
  }

  // Verify the switch stuck — don't claim success otherwise.
  uint16_t verify = 0;
  if (!read_chem_id(verify)) {
    ESP_LOGE(TAG, "Chem ID readback FAILED after switch");
    return false;
  }
  if (verify != CHEM_ID_4V2) {
    ESP_LOGE(TAG, "Chemistry switch did NOT stick — wanted 0x%04X, readback 0x%04X",
             CHEM_ID_4V2, verify);
    return false;
  }
  ESP_LOGI(TAG, "Chemistry switched to 0x%04X (4.2 V) — learning reset, re-learn required",
           verify);
  return true;
}

bool BQ27427::set_update_status_learning(bool enable) {
  if (_dev == nullptr) {
    return false;
  }

  // Read the current State block so we can compare just the Update Status byte
  // (offset 2) and stay idempotent — same constraint as the other DM accesses:
  // the read must START at 0x40 so the chip fills its buffer from Data Memory.
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  uint8_t block[32] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, block, sizeof(block))) {
    return false;
  }

  const uint8_t cur = block[OFFSET_UPDATE_STATUS];
  const uint8_t want = enable ? static_cast<uint8_t>(cur | UPDATE_STATUS_LEARN_BITS)
                              : static_cast<uint8_t>(cur & ~UPDATE_STATUS_LEARN_BITS);

  // Idempotency: leave learned state untouched (no CFGUPDATE) when the bits
  // already hold the requested value.
  if (cur == want) {
    ESP_LOGI(TAG, "Update Status already 0x%02X (learn bits %s) — no change", cur,
             enable ? "set" : "clear");
    return true;
  }
  ESP_LOGI(TAG, "Update Status 0x%02X → 0x%02X (learn bits %s — %s per-update change limits)",
           cur, want, enable ? "set" : "clear", enable ? "lifting" : "restoring");

  // UNSEAL — required for any DM commit (TRM §6.4).  No-op when already unsealed.
  if (!_unseal()) {
    return false;
  }

  // Enter CFGUPDATE.
  if (!_write_word(CMD_CONTROL, CTRL_SET_CFGUPDATE)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(true, 2000)) {
    ESP_LOGW(TAG, "Could not enter CFGUPDATE — aborting Update Status write");
    return false;
  }

  // Re-select the block (CFGUPDATE entry can clear the block pointer).
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  if (!_read_block(CMD_BLOCK_DATA_BASE, block, sizeof(block))) {
    return false;
  }

  // Modify only the Update Status byte locally.
  block[OFFSET_UPDATE_STATUS] = enable
                                    ? static_cast<uint8_t>(block[OFFSET_UPDATE_STATUS] |
                                                           UPDATE_STATUS_LEARN_BITS)
                                    : static_cast<uint8_t>(block[OFFSET_UPDATE_STATUS] &
                                                           ~UPDATE_STATUS_LEARN_BITS);

  // Write the full 32-byte block back in one transaction.
  if (!_write_block(CMD_BLOCK_DATA_BASE, block, sizeof(block))) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));

  // Fresh checksum from the modified block.
  uint16_t sum = 0;
  for (size_t i = 0; i < sizeof(block); ++i) {
    sum += block[i];
  }
  const uint8_t new_csum = static_cast<uint8_t>(255 - (sum & 0xFF));

  // Commit: writing the checksum transfers BlockData() to RAM.
  if (!_write_byte(CMD_BLOCK_DATA_CHECKSUM, new_csum)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(20));

  // Exit CFGUPDATE.
  if (!_write_word(CMD_CONTROL, CTRL_SOFT_RESET)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(false, 2000)) {
    return false;
  }

  // Readback verification — re-read the Update Status byte and compare.
  vTaskDelay(pdMS_TO_TICKS(50));
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  uint8_t verify[8] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, verify, sizeof(verify))) {
    return false;
  }
  if (verify[OFFSET_UPDATE_STATUS] != want) {
    ESP_LOGE(TAG, "Update Status write did NOT stick — wanted 0x%02X, readback 0x%02X",
             want, verify[OFFSET_UPDATE_STATUS]);
    return false;
  }
  ESP_LOGI(TAG, "Update Status verified at 0x%02X (csum=0x%02X)", want, new_csum);
  return true;
}

bool BQ27427::set_design_capacity_mah(uint16_t mah) {
  if (_dev == nullptr) {
    return false;
  }

  uint16_t current = 0;
  if (read_design_capacity_mah(current) && current == mah) {
    ESP_LOGI(TAG, "Design Capacity already %umAh — no change", mah);
    return true;
  }
  ESP_LOGI(TAG, "Updating Design Capacity %u → %u mAh", current, mah);

  // 0. UNSEAL — required because BlockDataChecksum (0x60) writes are
  //    UNSEALED-only (TRM §6.4).  Safe to call unconditionally; no-op when
  //    the chip is already in UNSEALED mode.
  if (!_unseal()) {
    return false;
  }

  // 1. Enter CFGUPDATE mode (TRM §4.1 step 2/3).
  if (!_write_word(CMD_CONTROL, CTRL_SET_CFGUPDATE)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(true, 2000)) {
    ESP_LOGW(TAG, "Could not enter CFGUPDATE — aborting Design Capacity write");
    return false;
  }

  // 2. Point at State subclass, block 0 (TRM §4.1 step 4–6).
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10)); // let the chip copy DM → command buffer

  // 3. Read the entire 32-byte block in one transaction.  More robust than
  //    using the replacement-checksum formula: we know every byte, so the
  //    final checksum is computed fresh from the modified block.
  uint8_t block[32] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, block, sizeof(block))) {
    return false;
  }
  ESP_LOGI(TAG, "DM block (old): DC=0x%02X%02X DE=0x%02X%02X TermV=0x%02X%02X",
           block[6], block[7], block[8], block[9], block[10], block[11]);

  // 4. Modify the Design Capacity bytes locally (MSB at offset 6, LSB at 7).
  block[OFFSET_DESIGN_CAPACITY]     = static_cast<uint8_t>((mah >> 8) & 0xFF);
  block[OFFSET_DESIGN_CAPACITY + 1] = static_cast<uint8_t>(mah & 0xFF);

  // 5. Write the full 32-byte block back in one transaction.
  if (!_write_block(CMD_BLOCK_DATA_BASE, block, sizeof(block))) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10)); // settle before commit write

  // 6. Compute fresh checksum: 255 - (sum_of_block_bytes mod 256).
  uint16_t sum = 0;
  for (size_t i = 0; i < sizeof(block); ++i) {
    sum += block[i];
  }
  const uint8_t new_csum = static_cast<uint8_t>(255 - (sum & 0xFF));
  ESP_LOGI(TAG, "DM block (new): DC=0x%02X%02X sum=0x%04X csum=0x%02X", block[6], block[7],
           sum, new_csum);

  // 7. Write checksum — this is the commit that transfers BlockData() to RAM.
  if (!_write_byte(CMD_BLOCK_DATA_CHECKSUM, new_csum)) {
    // Diagnostic: try to read 0x60 back to see what the chip actually has now.
    uint8_t echo = 0;
    if (_read_byte(CMD_BLOCK_DATA_CHECKSUM, echo)) {
      ESP_LOGW(TAG, "checksum write failed; readback=0x%02X (expected 0x%02X)", echo, new_csum);
    } else {
      ESP_LOGW(TAG, "checksum write failed and readback also failed");
    }
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(20)); // allow chip to commit before SOFT_RESET

  // 8. Exit CFGUPDATE and let the chip resume gauging (TRM §4.1 step 12/13).
  if (!_write_word(CMD_CONTROL, CTRL_SOFT_RESET)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(false, 2000)) {
    return false;
  }

  // 9. Diagnostic: re-load and dump the State block so we can see exactly
  //    what's in Data Memory after the commit + SOFT_RESET — independent of
  //    any single-field read path.
  vTaskDelay(pdMS_TO_TICKS(50)); // chip needs time after SOFT_RESET
  if (_select_data_block(SUBCLASS_STATE, 0x00)) {
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t verify_block[12] = {};
    if (_read_block(CMD_BLOCK_DATA_BASE, verify_block, sizeof(verify_block))) {
      ESP_LOGI(TAG, "DM block (after commit): DC=0x%02X%02X DE=0x%02X%02X TermV=0x%02X%02X",
               verify_block[6], verify_block[7], verify_block[8], verify_block[9],
               verify_block[10], verify_block[11]);
    }
  }

  // 10. Readback verification — the only check that the chip accepted the
  //     write without bqStudio in the loop.  If readback doesn't match what
  //     we wrote, we must NOT claim success.
  uint16_t verify = 0;
  if (!read_design_capacity_mah(verify)) {
    ESP_LOGE(TAG, "Design Capacity readback FAILED after write");
    return false;
  }
  if (verify != mah) {
    ESP_LOGE(TAG, "Design Capacity write did NOT stick — wrote %u, readback %u", mah, verify);
    return false;
  }
  ESP_LOGI(TAG, "Design Capacity verified at %umAh (csum=0x%02X)", mah, new_csum);
  return true;
}

bool BQ27427::configure_cell(const CellConfig &cfg) {
  if (_dev == nullptr) {
    return false;
  }

  // Read current State block (offsets 0..23 cover all fields we touch).
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  uint8_t block[32] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, block, sizeof(block))) {
    return false;
  }

  auto pack = [&block](uint8_t offset, uint16_t value) {
    block[offset]     = static_cast<uint8_t>((value >> 8) & 0xFF);
    block[offset + 1] = static_cast<uint8_t>(value & 0xFF);
  };
  auto unpack = [&block](uint8_t offset) -> uint16_t {
    return (static_cast<uint16_t>(block[offset]) << 8) | block[offset + 1];
  };

  const uint16_t cur_dc    = unpack(OFFSET_DESIGN_CAPACITY);
  const uint16_t cur_de    = unpack(OFFSET_DESIGN_ENERGY);
  const uint16_t cur_tv    = unpack(OFFSET_TERMINATE_VOLTAGE);
  const uint16_t cur_sleep = unpack(OFFSET_SLEEP_CURRENT);

  // Idempotency: if every field already matches, do nothing — preserves
  // any Qmax / impedance learning the chip has already accumulated.
  if (cur_dc == cfg.design_capacity_mah && cur_de == cfg.design_energy_mwh &&
      cur_tv == cfg.terminate_voltage_mv && cur_sleep == cfg.sleep_current_ma) {
    ESP_LOGI(TAG,
             "Cell config already correct (DC=%u DE=%u TermV=%u SleepI=%u) — no change",
             cur_dc, cur_de, cur_tv, cur_sleep);
    return true;
  }
  ESP_LOGI(TAG, "Updating cell config:");
  ESP_LOGI(TAG, "  DC      %u → %u mAh", cur_dc, cfg.design_capacity_mah);
  ESP_LOGI(TAG, "  DE      %u → %u mWh", cur_de, cfg.design_energy_mwh);
  ESP_LOGI(TAG, "  TermV   %u → %u mV",  cur_tv, cfg.terminate_voltage_mv);
  ESP_LOGI(TAG, "  SleepI  %u → %u mA",  cur_sleep, cfg.sleep_current_ma);

  // UNSEAL — required for any DM commit (TRM §6.4).  No-op when already unsealed.
  if (!_unseal()) {
    return false;
  }

  // Enter CFGUPDATE.
  if (!_write_word(CMD_CONTROL, CTRL_SET_CFGUPDATE)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(true, 2000)) {
    ESP_LOGW(TAG, "Could not enter CFGUPDATE — aborting cell config");
    return false;
  }

  // Re-select the block (CFGUPDATE entry can clear the block pointer).
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  if (!_read_block(CMD_BLOCK_DATA_BASE, block, sizeof(block))) {
    return false;
  }

  // Modify all four fields locally (MSB-first per Data Memory convention).
  pack(OFFSET_DESIGN_CAPACITY, cfg.design_capacity_mah);
  pack(OFFSET_DESIGN_ENERGY, cfg.design_energy_mwh);
  pack(OFFSET_TERMINATE_VOLTAGE, cfg.terminate_voltage_mv);
  pack(OFFSET_SLEEP_CURRENT, cfg.sleep_current_ma);

  // Write the full 32-byte block back in one transaction.
  if (!_write_block(CMD_BLOCK_DATA_BASE, block, sizeof(block))) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));

  // Fresh checksum from the modified block.
  uint16_t sum = 0;
  for (size_t i = 0; i < sizeof(block); ++i) {
    sum += block[i];
  }
  const uint8_t new_csum = static_cast<uint8_t>(255 - (sum & 0xFF));
  ESP_LOGI(TAG, "  new csum=0x%02X (block sum=0x%04X)", new_csum, sum);

  // Commit: writing the checksum transfers BlockData() to RAM.
  if (!_write_byte(CMD_BLOCK_DATA_CHECKSUM, new_csum)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(20));

  // Exit CFGUPDATE.
  if (!_write_word(CMD_CONTROL, CTRL_SOFT_RESET)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(false, 2000)) {
    return false;
  }

  // Readback verification — re-read all four fields and compare.
  vTaskDelay(pdMS_TO_TICKS(50));
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  uint8_t verify[32] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, verify, sizeof(verify))) {
    return false;
  }
  const uint16_t v_dc    = (uint16_t(verify[OFFSET_DESIGN_CAPACITY]) << 8) | verify[OFFSET_DESIGN_CAPACITY + 1];
  const uint16_t v_de    = (uint16_t(verify[OFFSET_DESIGN_ENERGY]) << 8) | verify[OFFSET_DESIGN_ENERGY + 1];
  const uint16_t v_tv    = (uint16_t(verify[OFFSET_TERMINATE_VOLTAGE]) << 8) | verify[OFFSET_TERMINATE_VOLTAGE + 1];
  const uint16_t v_sleep = (uint16_t(verify[OFFSET_SLEEP_CURRENT]) << 8) | verify[OFFSET_SLEEP_CURRENT + 1];
  ESP_LOGI(TAG, "Cell config readback: DC=%u DE=%u TermV=%u SleepI=%u",
           v_dc, v_de, v_tv, v_sleep);

  if (v_dc != cfg.design_capacity_mah || v_de != cfg.design_energy_mwh ||
      v_tv != cfg.terminate_voltage_mv || v_sleep != cfg.sleep_current_ma) {
    ESP_LOGE(TAG, "Cell config write did NOT stick — one or more fields wrong");
    return false;
  }
  ESP_LOGI(TAG, "Cell config verified");
  return true;
}

bool BQ27427::reset_to_factory_defaults() {
  // Per TRM §5.1.16, RESET is UNSEALED-only and should be issued from inside
  // CFGUPDATE so the chip is in a known state when reinitialising RAM from
  // ROM.
  ESP_LOGW(TAG, "Resetting fuel gauge to factory defaults (Control RESET=0x0041)");

  if (!_unseal()) {
    return false;
  }
  if (!_write_word(CMD_CONTROL, CTRL_SET_CFGUPDATE)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(true, 2000)) {
    ESP_LOGW(TAG, "Could not enter CFGUPDATE — aborting RESET");
    return false;
  }

  if (!_write_word(CMD_CONTROL, CTRL_RESET)) {
    return false;
  }
  // The chip re-initialises RAM from ROM, then enters INITIALIZATION and
  // automatically exits CFGUPDATE once the copy is done.  Datasheet figure 2-1.
  vTaskDelay(pdMS_TO_TICKS(500));
  if (!_wait_cfgupdate_flag(false, 3000)) {
    ESP_LOGW(TAG, "CFGUPDATE did not clear after RESET");
    return false;
  }

  uint16_t dc = 0;
  if (read_design_capacity_mah(dc)) {
    ESP_LOGI(TAG, "Reset complete — Design Capacity is now %umAh (ROM default)", dc);
  } else {
    ESP_LOGW(TAG, "Reset complete but could not read back Design Capacity");
  }
  return true;
}

bool BQ27427::_write_word(uint8_t cmd, uint16_t value) {
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[3] = {cmd, static_cast<uint8_t>(value & 0xFF),
                    static_cast<uint8_t>((value >> 8) & 0xFF)};
  esp_err_t err = i2c_master_transmit(_dev, buf, sizeof(buf), _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write cmd 0x%02X failed: %s", cmd, esp_err_to_name(err));
    return false;
  }
  return true;
}
