/**
 * AirGradient Go — Melody library
 *
 * Higher-level music layer that composes `BuzzerService::Note[]` sequences
 * from musical notes (C0..B8), note values (whole/half/quarter/...),
 * and a tempo in BPM.  Plays through the existing BuzzerService.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "go_buzzer.h"
#include "go_led.h"

#include <cstddef>
#include <cstdint>

// 12-TET equal-temperament pitches, C0..B8 (108 semitones).  REST = silence.
// Indices 0..107 map to C0..B8 so the underlying value can directly index
// NOTE_FREQ_HZ.  255 is reserved for REST.
enum class MusicalNote : uint8_t {
  C0 = 0,  Cs0, D0, Ds0, E0, F0, Fs0, G0, Gs0, A0, As0, B0,
  C1 = 12, Cs1, D1, Ds1, E1, F1, Fs1, G1, Gs1, A1, As1, B1,
  C2 = 24, Cs2, D2, Ds2, E2, F2, Fs2, G2, Gs2, A2, As2, B2,
  C3 = 36, Cs3, D3, Ds3, E3, F3, Fs3, G3, Gs3, A3, As3, B3,
  C4 = 48, Cs4, D4, Ds4, E4, F4, Fs4, G4, Gs4, A4, As4, B4,
  C5 = 60, Cs5, D5, Ds5, E5, F5, Fs5, G5, Gs5, A5, As5, B5,
  C6 = 72, Cs6, D6, Ds6, E6, F6, Fs6, G6, Gs6, A6, As6, B6,
  C7 = 84, Cs7, D7, Ds7, E7, F7, Fs7, G7, Gs7, A7, As7, B7,
  C8 = 96, Cs8, D8, Ds8, E8, F8, Fs8, G8, Gs8, A8, As8, B8,
  REST = 255,
};

// Note value in sixteenth-note units, so a quarter = 4, half = 8, whole = 16.
// Dotted variants are 1.5x — pre-computed here to avoid float math at runtime.
enum class NoteValue : uint8_t {
  SIXTEENTH = 1,
  EIGHTH = 2,
  DOTTED_EIGHTH = 3,
  QUARTER = 4,
  DOTTED_QUARTER = 6,
  HALF = 8,
  DOTTED_HALF = 12,
  WHOLE = 16,
};

struct MelodyNote {
  MusicalNote pitch;
  NoteValue value;
};

struct Melody {
  uint16_t bpm;             ///< Beats per minute; one beat = quarter note.
  const MelodyNote *notes;
  size_t count;
};

/// Frequency in Hz for each MusicalNote.  Index by `uint8_t(note)`; valid
/// for indices 0..107.  Out-of-range values (including REST=255) return 0.
uint32_t note_freq_hz(MusicalNote note);

/// Duration in ms for one sixteenth note at the given BPM.
constexpr uint32_t sixteenth_ms(uint16_t bpm) {
  // quarter_ms = 60000 / bpm; sixteenth = quarter / 4 = 15000 / bpm
  return (bpm == 0) ? 0u : (15000u / bpm);
}

/// Total ms for a NoteValue at the given BPM.
constexpr uint32_t note_duration_ms(NoteValue value, uint16_t bpm) {
  return sixteenth_ms(bpm) * static_cast<uint32_t>(value);
}

/// Maximum melody length play_melody() can handle in one call.  Sized to
/// fit the longest predefined melody (Tetris ~40 notes) with headroom.
inline constexpr size_t MELODY_MAX_NOTES = 64;

/// Convert a Melody to BuzzerService::Note[] and enqueue.  Non-blocking
/// (delegates to BuzzerService::play).  Truncates at MELODY_MAX_NOTES.
/// REST entries become freq_hz=0 silence.
void play_melody(BuzzerService &buzzer, const Melody &melody);

/// Total wall-clock duration of a melody in milliseconds (sum of note
/// durations).  Useful for scheduling a post-melody LED restore.
uint32_t melody_total_duration_ms(const Melody &melody);

/// Play a melody on `buzzer` and drive the back-side AQI LEDs on `led` in
/// lockstep — each note changes the LED colour (semitone-derived), REST
/// turns them off.  At end of the melody all back LEDs are set to off;
/// the caller (orchestrator) is responsible for restoring the normal AQI
/// indicator state after `melody_total_duration_ms()` has elapsed.
///
/// Spawns a short-lived RTOS task; concurrent calls drop the new visual
/// (buzzer still plays).
void play_melody_with_leds(BuzzerService &buzzer, LedService &led, const Melody &melody);

// --- Sound selection (settings menu) ---

enum class SoundSelect : uint8_t {
  Off = 0,
  Chime = 1,
  Tetris = 2,
};

inline constexpr uint8_t SOUND_SELECT_COUNT = 3;

/// Predefined melodies — externs so tests can inspect them.
extern const Melody MELODY_CHIME;
extern const Melody MELODY_TETRIS;

/// Play the melody for `sound`; no-op when `sound == Off`.
void play_sound_select(BuzzerService &buzzer, SoundSelect sound);

/// Same as the above but also runs the back-LED visual.  Returns the
/// melody duration in ms (0 when sound==Off) so the caller can schedule
/// a state restore.
uint32_t play_sound_select(BuzzerService &buzzer, LedService &led, SoundSelect sound);
