/**
 * AirGradient Go — Melody library implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_melody.h"

#include "rtos.h"

namespace {

// 12-TET equal-temperament frequencies, A4 = 440 Hz.
// Rounded to the nearest integer hertz.  108 entries: C0..B8.
constexpr uint16_t NOTE_FREQ_HZ[108] = {
    // Octave 0 — C0..B0
    16, 17, 18, 19, 21, 22, 23, 25, 26, 28, 29, 31,
    // Octave 1 — C1..B1
    33, 35, 37, 39, 41, 44, 46, 49, 52, 55, 58, 62,
    // Octave 2 — C2..B2
    65, 69, 73, 78, 82, 87, 92, 98, 104, 110, 117, 123,
    // Octave 3 — C3..B3
    131, 139, 147, 156, 165, 175, 185, 196, 208, 220, 233, 247,
    // Octave 4 — C4..B4  (A4 = 440)
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494,
    // Octave 5 — C5..B5
    523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988,
    // Octave 6 — C6..B6
    1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976,
    // Octave 7 — C7..B7
    2093, 2217, 2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520, 3729, 3951,
    // Octave 8 — C8..B8
    4186, 4435, 4699, 4978, 5274, 5588, 5920, 6272, 6645, 7040, 7459, 7902,
};

// Predefined melodies live below the table so they can reference the helpers.

// Chime — short rising arpeggio, ~0.6 s @ 200 BPM.  Pitched in the buzzer's
// audible window (C6..G6 around the HYG-8503A resonance at 2.7 kHz).
constexpr MelodyNote CHIME_NOTES[] = {
    {MusicalNote::C6, NoteValue::EIGHTH},
    {MusicalNote::E6, NoteValue::EIGHTH},
    {MusicalNote::G6, NoteValue::QUARTER},
};

// Tetris theme — first verse of Korobeiniki.  Transposed up one octave from
// the standard A-minor sheet so the melody sits closer to the magnetic
// buzzer's 2.7 kHz resonant peak (E6..A6 instead of E5..A5).  ~6 s @ 144 BPM.
constexpr MelodyNote TETRIS_NOTES[] = {
    {MusicalNote::E6,  NoteValue::QUARTER},
    {MusicalNote::B5,  NoteValue::EIGHTH},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::D6,  NoteValue::QUARTER},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::B5,  NoteValue::EIGHTH},
    {MusicalNote::A5,  NoteValue::QUARTER},
    {MusicalNote::A5,  NoteValue::EIGHTH},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::E6,  NoteValue::QUARTER},
    {MusicalNote::D6,  NoteValue::EIGHTH},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::B5,  NoteValue::DOTTED_QUARTER},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::D6,  NoteValue::QUARTER},
    {MusicalNote::E6,  NoteValue::QUARTER},
    {MusicalNote::C6,  NoteValue::QUARTER},
    {MusicalNote::A5,  NoteValue::QUARTER},
    {MusicalNote::A5,  NoteValue::QUARTER},
    {MusicalNote::REST, NoteValue::QUARTER},
    {MusicalNote::D6,  NoteValue::DOTTED_QUARTER},
    {MusicalNote::F6,  NoteValue::EIGHTH},
    {MusicalNote::A6,  NoteValue::QUARTER},
    {MusicalNote::G6,  NoteValue::EIGHTH},
    {MusicalNote::F6,  NoteValue::EIGHTH},
    {MusicalNote::E6,  NoteValue::DOTTED_QUARTER},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::E6,  NoteValue::QUARTER},
    {MusicalNote::D6,  NoteValue::EIGHTH},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::B5,  NoteValue::QUARTER},
    {MusicalNote::B5,  NoteValue::EIGHTH},
    {MusicalNote::C6,  NoteValue::EIGHTH},
    {MusicalNote::D6,  NoteValue::QUARTER},
    {MusicalNote::E6,  NoteValue::QUARTER},
    {MusicalNote::C6,  NoteValue::QUARTER},
    {MusicalNote::A5,  NoteValue::QUARTER},
    {MusicalNote::A5,  NoteValue::QUARTER},
};

// Pitch-to-colour palette indexed by semitone within the octave (0=C..11=B).
// Walks the colour wheel red→orange→yellow→green→cyan→blue→violet→magenta so
// adjacent semitones look distinct on the AQI LEDs.
struct RgbColor {
  uint8_t r, g, b;
};

constexpr RgbColor SEMITONE_COLORS[12] = {
    {255, 0, 0},     // C   — red
    {255, 80, 0},    // C#  — red-orange
    {255, 160, 0},   // D   — orange
    {255, 220, 0},   // D#  — amber
    {255, 255, 0},   // E   — yellow
    {160, 255, 0},   // F   — yellow-green
    {0, 255, 0},     // F#  — green
    {0, 255, 160},   // G   — cyan-green
    {0, 255, 255},   // G#  — cyan
    {0, 80, 255},    // A   — blue
    {120, 0, 255},   // A#  — purple
    {255, 0, 200},   // B   — magenta
};

// Guards against concurrent visual tasks.  The buzzer queue itself
// serialises re-plays at the audio layer; this flag stops a second task
// from fighting over the back LEDs.
volatile bool g_visual_task_active = false;

struct VisualTaskArgs {
  LedService *led;
  Melody melody; // shallow copy; .notes must remain valid until the task ends
};

void visual_task_entry(void *arg) {
  auto *args = static_cast<VisualTaskArgs *>(arg);
  for (size_t i = 0; i < args->melody.count; ++i) {
    const auto &mn = args->melody.notes[i];
    if (mn.pitch == MusicalNote::REST) {
      args->led->set_back_leds_rgb(0, 0, 0);
    } else {
      const uint8_t semitone = static_cast<uint8_t>(mn.pitch) % 12;
      const auto &c = SEMITONE_COLORS[semitone];
      args->led->set_back_leds_rgb(c.r, c.g, c.b);
    }
    const uint32_t duration = note_duration_ms(mn.value, args->melody.bpm);
    if (duration > 0) {
      RTOS::delay_ms(duration);
    }
  }
  args->led->set_back_leds_rgb(0, 0, 0);
  delete args;
  g_visual_task_active = false;
  RTOS::task_delete(nullptr);
}

} // namespace

const Melody MELODY_CHIME = {
    /*.bpm=*/200,
    /*.notes=*/CHIME_NOTES,
    /*.count=*/sizeof(CHIME_NOTES) / sizeof(CHIME_NOTES[0]),
};

const Melody MELODY_TETRIS = {
    /*.bpm=*/144,
    /*.notes=*/TETRIS_NOTES,
    /*.count=*/sizeof(TETRIS_NOTES) / sizeof(TETRIS_NOTES[0]),
};

uint32_t note_freq_hz(MusicalNote note) {
  const uint8_t idx = static_cast<uint8_t>(note);
  if (idx >= sizeof(NOTE_FREQ_HZ) / sizeof(NOTE_FREQ_HZ[0])) {
    return 0; // REST or out-of-range
  }
  return NOTE_FREQ_HZ[idx];
}

void play_melody(BuzzerService &buzzer, const Melody &melody) {
  if (melody.notes == nullptr || melody.count == 0 || melody.bpm == 0) {
    return;
  }

  BuzzerService::Note buf[MELODY_MAX_NOTES];
  const size_t count = (melody.count < MELODY_MAX_NOTES) ? melody.count : MELODY_MAX_NOTES;

  for (size_t i = 0; i < count; ++i) {
    buf[i].freq_hz = note_freq_hz(melody.notes[i].pitch); // 0 for REST
    buf[i].duration_ms = note_duration_ms(melody.notes[i].value, melody.bpm);
  }

  buzzer.play(buf, count);
}

void play_sound_select(BuzzerService &buzzer, SoundSelect sound) {
  switch (sound) {
  case SoundSelect::Chime:
    play_melody(buzzer, MELODY_CHIME);
    break;
  case SoundSelect::Tetris:
    play_melody(buzzer, MELODY_TETRIS);
    break;
  case SoundSelect::Off:
  default:
    break;
  }
}

uint32_t melody_total_duration_ms(const Melody &melody) {
  uint32_t total = 0;
  for (size_t i = 0; i < melody.count; ++i) {
    total += note_duration_ms(melody.notes[i].value, melody.bpm);
  }
  return total;
}

void play_melody_with_leds(BuzzerService &buzzer, LedService &led, const Melody &melody) {
  play_melody(buzzer, melody);

  if (melody.notes == nullptr || melody.count == 0) {
    return;
  }
  if (g_visual_task_active) {
    return; // a previous visual is still running; only the audio re-plays
  }

  auto *args = new VisualTaskArgs{&led, melody};
  g_visual_task_active = true;

  // Priority matches BuzzerService default (3); 2 KB stack covers the
  // I²C writes + delay frames with headroom.
  if (!RTOS::task_create(visual_task_entry, "melody_viz", 2048, args, 3, nullptr)) {
    delete args;
    g_visual_task_active = false;
  }
}

uint32_t play_sound_select(BuzzerService &buzzer, LedService &led, SoundSelect sound) {
  const Melody *m = nullptr;
  switch (sound) {
  case SoundSelect::Chime:
    m = &MELODY_CHIME;
    break;
  case SoundSelect::Tetris:
    m = &MELODY_TETRIS;
    break;
  case SoundSelect::Off:
  default:
    return 0;
  }
  play_melody_with_leds(buzzer, led, *m);
  return melody_total_duration_ms(*m);
}
