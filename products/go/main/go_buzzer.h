/**
 * AirGradient Go — Buzzer Service
 *
 * Drives the v0.3 magnetic buzzer (HYG-8503A) through Q3 (NPN low-side
 * switch on EN_BUZZ / GPIO8) using LEDC PWM.  Plays asynchronous note
 * sequences from a small FreeRTOS task so callers (orchestrator, UI) can
 * fire-and-forget beeps without blocking.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "rtos.h"

#include <cstddef>
#include <cstdint>

class BuzzerService {
public:
  /// One step in a beep sequence.
  /// freq_hz = 0 produces silence for the duration (used for inter-note gaps).
  struct Note {
    uint32_t freq_hz;
    uint32_t duration_ms;
  };

  struct Config {
    int pin = -1;                    ///< GPIO; -1 disables the service
    uint32_t default_freq_hz = 2700; ///< HYG-8503A resonant frequency
    uint8_t duty_percent = 50;       ///< 0–100, 50% loudest for square wave
    uint8_t ledc_channel = 0;        ///< LEDC channel index (0–7)
    uint8_t ledc_timer = 0;          ///< LEDC timer index (0–3)
    uint16_t task_stack_size = 2048;
    uint8_t task_priority = 3;
    uint8_t queue_depth = 16; ///< Max queued notes; excess `play()` calls drop
  };

  explicit BuzzerService(const Config &config);
  ~BuzzerService();

  BuzzerService(const BuzzerService &) = delete;
  BuzzerService &operator=(const BuzzerService &) = delete;

  /// Configure the LEDC timer/channel and create the worker queue.
  /// Idempotent.  Returns false on hardware error or when `pin < 0`.
  bool init();

  /// Start the worker task.  Must follow init().
  bool start();

  /// Enqueue a sequence of notes.  Non-blocking; drops the entire sequence
  /// if the queue cannot accept all of it without waiting.
  void play(const Note *notes, size_t count);

  /// Single beep at the configured default frequency.
  void beep(uint32_t duration_ms);

  /// Mute immediately and drop any queued notes.  Safe to call from any task.
  void stop();

  bool enabled() const { return _config.pin >= 0; }

private:
  Config _config;
  RtosQueueHandle _queue = nullptr;
  RtosTaskHandle _task = nullptr;
  bool _ledc_ready = false;

  static void _task_entry(void *arg);
  void _run();
  void _set_freq(uint32_t freq_hz); ///< 0 mutes; non-zero re-arms LEDC
};
