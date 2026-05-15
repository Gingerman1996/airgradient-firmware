/**
 * AirGradient Go — LIS2DH12 implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_accel.h"

#include "ag_log.h"

#ifndef TEST_HOST
#include <driver/gpio.h>
#endif

namespace {
constexpr const char *TAG = "Lis2dh12";
} // namespace

LIS2DH12::LIS2DH12(AccelI2cBusHandle bus, const Config &config) : _config(config), _bus(bus) {}

LIS2DH12::~LIS2DH12() {
#ifndef TEST_HOST
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
  }
#endif
}

bool LIS2DH12::init() {
#ifndef TEST_HOST
  if (_dev == nullptr) {
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = _config.address,
        .scl_speed_hz = _config.scl_speed_hz,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = 0},
    };
    if (i2c_master_bus_add_device(_bus, &cfg, &_dev) != ESP_OK) {
      AG_LOGE(TAG, "bus_add_device failed at addr 0x%02X", _config.address);
      return false;
    }
  }
#endif

  const uint8_t id = who_am_i();
  if (id != WHO_AM_I_EXPECTED) {
    AG_LOGE(TAG, "WHO_AM_I mismatch: got 0x%02X expected 0x%02X", id, WHO_AM_I_EXPECTED);
    return false;
  }

  if (!_write_reg(REG_CTRL_REG1, CTRL_REG1_CONFIG)) {
    AG_LOGE(TAG, "CTRL_REG1 write failed");
    return false;
  }
  if (!_write_reg(REG_CTRL_REG4, CTRL_REG4_CONFIG)) {
    AG_LOGE(TAG, "CTRL_REG4 write failed");
    return false;
  }

  // The first conversion after enabling needs one ODR period — at 100 Hz
  // that's 10 ms. Wait a bit longer so the first read is meaningful.
  RTOS::delay_ms(20);

  AG_LOGI(TAG, "initialised at I²C 0x%02X (WHO_AM_I=0x%02X)", _config.address, id);
  return true;
}

uint8_t LIS2DH12::who_am_i() {
  uint8_t v = 0;
  if (!_read_reg(REG_WHO_AM_I, v)) {
    return 0;
  }
  return v;
}

bool LIS2DH12::read(Reading &out) {
  uint8_t buf[6] = {};
  if (!_read_block(REG_OUT_X_L | AUTO_INCR, buf, sizeof(buf))) {
    return false;
  }
  // Each axis is a signed 16-bit value left-justified to 12 bits at ±2 g.
  // 1 mg / LSB after the >> 4 right-shift. (Datasheet table 3.)
  const int16_t raw_x = static_cast<int16_t>(buf[0] | (buf[1] << 8));
  const int16_t raw_y = static_cast<int16_t>(buf[2] | (buf[3] << 8));
  const int16_t raw_z = static_cast<int16_t>(buf[4] | (buf[5] << 8));
  out.x_mg = static_cast<int16_t>(raw_x >> 4);
  out.y_mg = static_cast<int16_t>(raw_y >> 4);
  out.z_mg = static_cast<int16_t>(raw_z >> 4);
  return true;
}

// ---------------------------------------------------------------------------
// Motion detection — high-pass-filtered "any axis high event" on INT1.
// ---------------------------------------------------------------------------

bool LIS2DH12::enable_motion_int1(uint16_t threshold_mg) {
  // 1 LSB of INT1_THS = 16 mg at ±2 g FS (datasheet table 33).
  // Clamp into the 7-bit threshold field.
  uint8_t ths = static_cast<uint8_t>(threshold_mg / 16);
  if (ths == 0) ths = 1;
  if (ths > 0x7F) ths = 0x7F;

  // CTRL_REG2 = 0x01 → HPM=normal, default cutoff (~ODR/50), HPIS1=1 routes
  // the high-pass-filtered data into Interrupt 1. Removes the gravity DC
  // bias so stationary acceleration of ~1 g doesn't trip the threshold.
  if (!_write_reg(REG_CTRL_REG2, 0x01)) return false;
  // CTRL_REG3 = 0x40 → I1_IA1 routes Interrupt 1 source to the INT1 pin.
  if (!_write_reg(REG_CTRL_REG3, 0x40)) return false;
  // CTRL_REG5 = 0x08 → LIR_INT1 latches the source register until read.
  if (!_write_reg(REG_CTRL_REG5, 0x08)) return false;
  // INT1_THS, INT1_DURATION (single-sample event = 0).
  if (!_write_reg(REG_INT1_THS, ths)) return false;
  if (!_write_reg(REG_INT1_DURATION, 0x00)) return false;
  // INT1_CFG = 0x2A → XHIE | YHIE | ZHIE (high-event on any axis, OR).
  if (!_write_reg(REG_INT1_CFG, 0x2A)) return false;

  AG_LOGI(TAG, "motion INT1 enabled: threshold %u mg (%u LSB)",
          static_cast<unsigned>(threshold_mg), static_cast<unsigned>(ths));
  return true;
}

uint8_t LIS2DH12::read_int1_src() {
  uint8_t v = 0;
  if (!_read_reg(REG_INT1_SRC, v)) return 0;
  return v;
}

// ---------------------------------------------------------------------------
// Interrupt-driven motion-log task — self-contained, no orchestrator plumbing.
// ---------------------------------------------------------------------------

namespace {
struct MotionTaskCtx {
  LIS2DH12 *self;
};

void motion_log_task_entry(void *arg) {
  auto *ctx = static_cast<MotionTaskCtx *>(arg);
  LIS2DH12 *self = ctx->self;
  // Poll the volatile flag set by the GPIO ISR. 50 ms latency is well below
  // human-perception of "I shook it" → "log appeared".
  for (;;) {
    RTOS::delay_ms(50);
    if (!self->motion_flag) continue;
    self->motion_flag = false;
    const uint8_t src = self->read_int1_src(); // clears latch
    LIS2DH12::Reading r{};
    if (self->read(r)) {
      AG_LOGI(TAG, "MOTION src=0x%02X  x=%+d mg  y=%+d mg  z=%+d mg",
              src, r.x_mg, r.y_mg, r.z_mg);
    } else {
      AG_LOGW(TAG, "MOTION src=0x%02X (read failed)", src);
    }
  }
}
} // namespace

void LIS2DH12::start_motion_log_task(int int_pin, uint16_t threshold_mg) {
  if (!enable_motion_int1(threshold_mg)) {
    AG_LOGE(TAG, "start_motion_log_task: enable_motion_int1 failed");
    return;
  }

#ifndef TEST_HOST
  // Configure ESP32 GPIO as input, no internal pull (LIS2DH12 INT1 is
  // push-pull active-high by default), rising-edge interrupt.
  const auto pin = static_cast<gpio_num_t>(int_pin);
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << pin;
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_POSEDGE;
  gpio_config(&cfg);
  gpio_install_isr_service(0); // idempotent — same call exists for buttons
  gpio_isr_handler_add(
      pin,
      [](void *arg) { *static_cast<volatile bool *>(arg) = true; },
      const_cast<bool *>(&motion_flag));
#endif

  // Clear any pending latched event so the first edge after this point is a
  // real one (chip may have asserted INT1 immediately after enable_motion).
  (void)read_int1_src();
  motion_flag = false;

  auto *ctx = new MotionTaskCtx{this};
  if (!RTOS::task_create(motion_log_task_entry, "accel_motion",
                         /*stack=*/2048, ctx, /*prio=*/2,
                         /*handle=*/nullptr)) {
    AG_LOGE(TAG, "start_motion_log_task: task_create failed");
    delete ctx;
    return;
  }
  AG_LOGI(TAG, "motion-log task started on GPIO%d", int_pin);
}

bool LIS2DH12::_write_reg(uint8_t reg, uint8_t value) {
#ifndef TEST_HOST
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[2] = {reg, value};
  return i2c_master_transmit(_dev, buf, sizeof(buf), _config.timeout_ms) == ESP_OK;
#else
  (void)reg;
  (void)value;
  return true;
#endif
}

bool LIS2DH12::_read_reg(uint8_t reg, uint8_t &out) {
#ifndef TEST_HOST
  if (_dev == nullptr) {
    return false;
  }
  return i2c_master_transmit_receive(_dev, &reg, 1, &out, 1, _config.timeout_ms) == ESP_OK;
#else
  (void)reg;
  out = 0;
  return true;
#endif
}

bool LIS2DH12::_read_block(uint8_t reg, uint8_t *buf, size_t len) {
#ifndef TEST_HOST
  if (_dev == nullptr || buf == nullptr || len == 0) {
    return false;
  }
  return i2c_master_transmit_receive(_dev, &reg, 1, buf, len, _config.timeout_ms) == ESP_OK;
#else
  (void)reg;
  (void)len;
  if (buf != nullptr) {
    for (size_t i = 0; i < len; ++i) buf[i] = 0;
  }
  return true;
#endif
}
