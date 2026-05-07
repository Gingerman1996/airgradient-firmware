/**
 * AirGradient Go — Buzzer Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_buzzer.h"

#include "ag_log.h"

#ifndef TEST_HOST
#include "driver/ledc.h"
#endif

namespace {
constexpr const char *TAG = "Buzzer";

#ifndef TEST_HOST
// 10-bit duty resolution: 1024 steps, freq * 1024 must stay <= LEDC clk (80 MHz).
// At 1024 steps, max freq is ~78 kHz — comfortably above any audible buzzer.
constexpr ledc_timer_bit_t LEDC_DUTY_RES = LEDC_TIMER_10_BIT;
constexpr uint32_t LEDC_DUTY_FULL = 1u << 10;
#endif

constexpr uint32_t QUEUE_SEND_TIMEOUT_MS = 0; // never block the caller
} // namespace

BuzzerService::BuzzerService(const Config &config) : _config(config) {}

BuzzerService::~BuzzerService() {
  if (_task != nullptr) {
    RTOS::task_delete(_task);
    _task = nullptr;
  }
  if (_queue != nullptr) {
    RTOS::queue_delete(_queue);
    _queue = nullptr;
  }
}

bool BuzzerService::init() {
  if (!enabled()) {
    AG_LOGI(TAG, "init: pin not configured, buzzer disabled");
    return false;
  }

#ifndef TEST_HOST
  ledc_timer_config_t timer_cfg = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_DUTY_RES,
      .timer_num = static_cast<ledc_timer_t>(_config.ledc_timer),
      .freq_hz = _config.default_freq_hz,
      .clk_cfg = LEDC_AUTO_CLK,
      .deconfigure = false,
  };
  if (ledc_timer_config(&timer_cfg) != ESP_OK) {
    AG_LOGE(TAG, "init: ledc_timer_config failed");
    return false;
  }

  ledc_channel_config_t ch_cfg = {
      .gpio_num = _config.pin,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = static_cast<ledc_channel_t>(_config.ledc_channel),
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = static_cast<ledc_timer_t>(_config.ledc_timer),
      .duty = 0, // start muted
      .hpoint = 0,
      .flags = {.output_invert = 0},
  };
  if (ledc_channel_config(&ch_cfg) != ESP_OK) {
    AG_LOGE(TAG, "init: ledc_channel_config failed");
    return false;
  }
  _ledc_ready = true;
#endif

  if (_queue == nullptr) {
    _queue = RTOS::queue_create(_config.queue_depth, sizeof(Note));
    if (_queue == nullptr) {
      AG_LOGE(TAG, "init: queue_create failed");
      return false;
    }
  }

  AG_LOGI(TAG, "init: pin=%d freq=%uHz duty=%u%%", _config.pin,
          static_cast<unsigned>(_config.default_freq_hz),
          static_cast<unsigned>(_config.duty_percent));
  return true;
}

bool BuzzerService::start() {
  if (!enabled() || _queue == nullptr) {
    return false;
  }
  if (_task != nullptr) {
    return true;
  }
  return RTOS::task_create(&BuzzerService::_task_entry, "buzzer", _config.task_stack_size, this,
                           _config.task_priority, &_task);
}

void BuzzerService::play(const Note *notes, size_t count) {
  if (!enabled() || _queue == nullptr || notes == nullptr || count == 0) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    RTOS::queue_send(_queue, &notes[i], QUEUE_SEND_TIMEOUT_MS);
  }
}

void BuzzerService::beep(uint32_t duration_ms) {
  Note n{_config.default_freq_hz, duration_ms};
  play(&n, 1);
}

void BuzzerService::stop() {
  _set_freq(0);
  // Drain any queued notes so the next play() starts cleanly.
  if (_queue != nullptr) {
    Note dropped;
    while (RTOS::queue_receive(_queue, &dropped, 0)) {
      // discard
    }
  }
}

void BuzzerService::_task_entry(void *arg) { static_cast<BuzzerService *>(arg)->_run(); }

void BuzzerService::_run() {
  Note n;
  while (RTOS::queue_receive(_queue, &n, UINT32_MAX)) {
    _set_freq(n.freq_hz);
    if (n.duration_ms > 0) {
      RTOS::delay_ms(n.duration_ms);
    }
    _set_freq(0); // ensure inter-note silence (and trailing mute)
  }
}

void BuzzerService::_set_freq(uint32_t freq_hz) {
#ifndef TEST_HOST
  if (!_ledc_ready) {
    return;
  }
  auto mode = LEDC_LOW_SPEED_MODE;
  auto channel = static_cast<ledc_channel_t>(_config.ledc_channel);
  auto timer = static_cast<ledc_timer_t>(_config.ledc_timer);

  if (freq_hz == 0) {
    ledc_set_duty(mode, channel, 0);
    ledc_update_duty(mode, channel);
    return;
  }

  ledc_set_freq(mode, timer, freq_hz);
  uint32_t duty = (LEDC_DUTY_FULL * _config.duty_percent) / 100u;
  if (duty == 0) {
    duty = 1; // guarantee audible edge if duty_percent rounds to 0
  }
  ledc_set_duty(mode, channel, duty);
  ledc_update_duty(mode, channel);
#else
  (void)freq_hz;
#endif
}
