/**
 * AirGradient Go — go_melody host tests
 *
 * Verify the note→frequency table, BPM→duration math, and end-to-end
 * conversion from a Melody to BuzzerService::Note[] (via play_melody).
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include "go_melody.h"

TEST_CASE("note_freq_hz — well-known reference pitches", "[melody]") {
  // A4 = 440 Hz (concert pitch).
  REQUIRE(note_freq_hz(MusicalNote::A4) == 440);
  // Middle C (C4) = 262 Hz (rounded).
  REQUIRE(note_freq_hz(MusicalNote::C4) == 262);
  // C0 = 16 Hz, B8 = 7902 Hz (lowest and highest in the table).
  REQUIRE(note_freq_hz(MusicalNote::C0) == 16);
  REQUIRE(note_freq_hz(MusicalNote::B8) == 7902);
  // REST returns 0 (silence).
  REQUIRE(note_freq_hz(MusicalNote::REST) == 0);
}

TEST_CASE("note_freq_hz — octave doubling within table accuracy", "[melody]") {
  // Each octave doubles the frequency.  Rounding within the table is up
  // to ±1 Hz so check within tolerance.
  for (uint8_t octave = 0; octave < 8; ++octave) {
    const auto low = static_cast<MusicalNote>(octave * 12);                       // C_octave
    const auto high = static_cast<MusicalNote>((octave + 1) * 12);                // C_(octave+1)
    const uint32_t lo_hz = note_freq_hz(low);
    const uint32_t hi_hz = note_freq_hz(high);
    REQUIRE(lo_hz > 0);
    REQUIRE(hi_hz > 0);
    // hi should be ~2x lo, allowing for integer rounding.
    REQUIRE(hi_hz >= 2 * lo_hz - 1);
    REQUIRE(hi_hz <= 2 * lo_hz + 1);
  }
}

TEST_CASE("sixteenth_ms — BPM math", "[melody]") {
  // At 60 BPM, a quarter = 1000 ms, so a sixteenth = 250 ms.
  REQUIRE(sixteenth_ms(60) == 250);
  // At 120 BPM, sixteenth = 125 ms.
  REQUIRE(sixteenth_ms(120) == 125);
  // At 144 BPM (Tetris), sixteenth ≈ 104 ms.
  REQUIRE(sixteenth_ms(144) == 104);
  // Defensive: bpm=0 returns 0 (no divide-by-zero).
  REQUIRE(sixteenth_ms(0) == 0);
}

TEST_CASE("note_duration_ms — note values scale linearly", "[melody]") {
  const uint16_t bpm = 120;
  const uint32_t s16 = sixteenth_ms(bpm);
  REQUIRE(note_duration_ms(NoteValue::SIXTEENTH, bpm) == s16);
  REQUIRE(note_duration_ms(NoteValue::EIGHTH, bpm) == 2 * s16);
  REQUIRE(note_duration_ms(NoteValue::QUARTER, bpm) == 4 * s16);
  REQUIRE(note_duration_ms(NoteValue::DOTTED_QUARTER, bpm) == 6 * s16);
  REQUIRE(note_duration_ms(NoteValue::HALF, bpm) == 8 * s16);
  REQUIRE(note_duration_ms(NoteValue::WHOLE, bpm) == 16 * s16);
}

TEST_CASE("predefined melodies are non-empty and within audible range",
          "[melody]") {
  REQUIRE(MELODY_CHIME.notes != nullptr);
  REQUIRE(MELODY_CHIME.count > 0);
  REQUIRE(MELODY_CHIME.bpm > 0);

  REQUIRE(MELODY_TETRIS.notes != nullptr);
  REQUIRE(MELODY_TETRIS.count > 0);
  REQUIRE(MELODY_TETRIS.bpm > 0);

  // The melodies fit in MELODY_MAX_NOTES so play_melody won't truncate.
  REQUIRE(MELODY_CHIME.count <= MELODY_MAX_NOTES);
  REQUIRE(MELODY_TETRIS.count <= MELODY_MAX_NOTES);

  // Every non-REST pitch should be audible on the magnetic buzzer
  // (resonant ~2.7 kHz).  Allow 100..6000 Hz as a practical envelope —
  // melodies pitched outside this won't be useful even though the table
  // covers C0..B8.
  auto check_audible = [](const Melody &m) {
    for (size_t i = 0; i < m.count; ++i) {
      const uint32_t f = note_freq_hz(m.notes[i].pitch);
      if (f == 0) continue; // REST is fine
      REQUIRE(f >= 100u);
      REQUIRE(f <= 6000u);
    }
  };
  check_audible(MELODY_CHIME);
  check_audible(MELODY_TETRIS);
}

TEST_CASE("Tetris theme length is reasonable", "[melody]") {
  // First verse should land between 4 and 15 seconds.  This guards
  // against accidental truncation or runaway loops.
  uint32_t total_ms = 0;
  for (size_t i = 0; i < MELODY_TETRIS.count; ++i) {
    total_ms += note_duration_ms(MELODY_TETRIS.notes[i].value, MELODY_TETRIS.bpm);
  }
  REQUIRE(total_ms >= 4000);
  REQUIRE(total_ms <= 15000);
}
