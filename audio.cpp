// Chili Rush sound: original chiptune music and sound effects.
//
// All music here was written for Chili Rush. The melodies, harmonies and
// grooves are original; nothing is sampled or transcribed.
//
// Channel map (blit::channels[]):
//   0, 1, 2  sound effects (a small priority allocator shares them)
//   3        music lead     ("trumpet": 25% pulse square, vibrato on long notes)
//   4        music harmony  (offbeat chord strums, or a second trumpet a third below)
//   5        music bass     (bouncing root / fifth / octave, follows the chords)
//   6        music drums    (kick = falling square, snare / hats / crash = noise)
//   7        unused
//
// Music is written in a readable note notation (see "Notation" below) that is
// turned into uint8_t tables at compile time, so the source is the score and
// the binary holds only the compact tables. Bass notes and chord strums are
// computed from the chord chart, so they always follow the progression.
//
// Mix budget (no clipping): a square at volume v swings +-0x7fff*v/65536 and
// SDK noise up to twice that, and the SDK sums all channels. Music squares sum
// to about 0x6400, sound effects use at most 0x2c00 per channel and the big
// ones duck the music, so the worst case stays under the 16-bit limit.
#include "audio.hpp"

#include <cmath>
#include <cstddef>

#include "audio/audio.hpp"

namespace audio {
namespace {

using blit::AudioChannel;
using blit::channels;

///////////////////////////////////////////////////////////////////////////
// Notation, parsed at compile time
//
// Notes:  C5 F#5 Bb4 ...  (MIDI octave numbers: C4 is middle C, A4 = 440 Hz)
//         -   hold the previous note one more step
//         .   rest (the note stops)
//         |   bar line (checked: every bar must have 16 steps of a 16th note)
// Chords: two per bar (one per half bar): F  Dm  Bb  A7  Cm ...
//         (m = minor, 7 = dominant seventh, m7 = minor seventh)

constexpr uint8_t HOLD = 0;
constexpr uint8_t REST = 1;
constexpr int kSteps = 16; // steps (16th notes) in a bar

// Calling this in a constant expression stops the build: the score has a typo.
inline void bad_notation() {}

constexpr bool is_gap(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

constexpr int pitch_class(char c) {
    switch(c) {
        case 'C': return 0;  case 'D': return 2;  case 'E': return 4;  case 'F': return 5;
        case 'G': return 7;  case 'A': return 9;  case 'B': return 11;
        default: bad_notation(); return 0;
    }
}

// Counts tokens and checks that each bar holds `per_bar` of them.
constexpr size_t count_tokens(const char *s, size_t per_bar) {
    size_t total = 0, in_bar = 0;
    for(size_t i = 0;; i++) {
        char c = s[i];
        if(c == '|' || c == 0) {
            if(in_bar != 0 && in_bar != per_bar) bad_notation();
            in_bar = 0;
            if(c == 0) break;
            continue;
        }
        if(is_gap(c)) continue;
        total++;
        in_bar++;
        while(s[i + 1] && !is_gap(s[i + 1]) && s[i + 1] != '|') i++;
    }
    return total;
}

template <size_t N> struct Table {
    uint8_t v[N];
    constexpr uint8_t operator[](size_t i) const { return v[i]; }
    static constexpr size_t size() { return N; }
};

template <size_t N> constexpr Table<N> parse_notes(const char *s) {
    Table<N> t{};
    size_t n = 0;
    for(size_t i = 0; s[i];) {
        char c = s[i];
        if(is_gap(c) || c == '|') { i++; continue; }
        if(c == '-') { t.v[n++] = HOLD; i++; continue; }
        if(c == '.') { t.v[n++] = REST; i++; continue; }
        int pc = pitch_class(c);
        i++;
        if(s[i] == '#') { pc++; i++; }
        else if(s[i] == 'b') { pc--; i++; }
        if(s[i] < '0' || s[i] > '9') bad_notation();
        int midi = (s[i] - '0' + 1) * 12 + pc;
        i++;
        if(midi < 12 || midi > 119) bad_notation();
        t.v[n++] = uint8_t(midi);
    }
    if(n != N) bad_notation();
    return t;
}

enum Quality : uint8_t { MAJ = 0, MIN = 1, DOM7 = 2, MIN7 = 3 };

// A chord is (quality << 4) | root pitch class.
template <size_t N> constexpr Table<N> parse_chords(const char *s) {
    Table<N> t{};
    size_t n = 0;
    for(size_t i = 0; s[i];) {
        char c = s[i];
        if(is_gap(c) || c == '|') { i++; continue; }
        int pc = pitch_class(c);
        i++;
        if(s[i] == '#') { pc++; i++; }
        else if(s[i] == 'b') { pc--; i++; }
        int q = MAJ;
        if(s[i] == 'm') { q = MIN; i++; }
        if(s[i] == '7') { q = (q == MIN) ? MIN7 : DOM7; i++; }
        if(s[i] && !is_gap(s[i]) && s[i] != '|') bad_notation();
        t.v[n++] = uint8_t((q << 4) | ((pc + 12) % 12));
    }
    if(n != N) bad_notation();
    return t;
}

#define NOTES(s) parse_notes<count_tokens(s, kSteps)>(s)
#define CHORDS(s) parse_chords<count_tokens(s, 2)>(s)

///////////////////////////////////////////////////////////////////////////
// Groove patterns, one character per 16th step. Songs pick one per bar by
// letter ('A' is the first pattern of each pool).
//
// Drums:  k kick, s snare, h closed hat, o open hat, c crash, . nothing
constexpr char kDrumPool[][kSteps + 1] = {
    "k.h.s.h.k.h.s.h.", // A  title: straight
    "k.h.s.h.k.hks.hs", // B  title: extra kick and pickup snare
    "k.h.s.h.k.s.ssss", // C  title: fill into the next phrase
    "c.h.s.h.k.h.s.h.", // D  title: crash on the downbeat
    "k.hhs.hkk.hhs.hh", // E  run: driving
    "k.hhs.hkk.hks.ss", // F  run: fill
    "c.hhs.hkk.hhs.hh", // G  run: crash on the downbeat
    "k..hs..hk.khs.ho", // H  run B section: syncopated, open hat
    "khhhshhhkhkhshhh", // I  dash: doubled hats
    "khhhshhhkhkhssss", // J  dash: fill
    "c.......s.......", // K  game over bar 1
    "k.......c.......", // L  game over bar 2
};
// Bass (relative to the chord of the half bar): 1 root, 3 third, 5 fifth,
// 7 seventh, 8 octave, w chromatic step up into the next chord's root,
// - hold, . rest
constexpr char kBassPool[][kSteps + 1] = {
    "1...5...1...5...", // A  title: oom-pah
    "1...5...1...5.w.", // B  title: oom-pah, walk into the next bar
    "1.5.8.5.1.5.8.5.", // C  run: bouncing eighths
    "1.5.8.5.1.5.3.w.", // D  run: bouncing, walk into the next bar
    "1..5..8.1..5..8.", // E  run B section: tresillo (3+3+2)
    "1..5..8.1..5.w..", // F  run B section: tresillo with walk
    "1-------1-------", // G  game over: long notes
};
// Harmony: x short chord strum, X long strum, t second trumpet (a third
// under the lead), . nothing
constexpr char kHarmPool[][kSteps + 1] = {
    "..x...x...x...x.", // A  offbeat strums (the "pah")
    "tttttttttttttttt", // B  trumpet duet
    "x..x..x.x..x..x.", // C  tresillo strums
    "X.......X.......", // D  game over: long strums
};

///////////////////////////////////////////////////////////////////////////
// TITLE  "Plaza del Sol"   F major, 116 BPM, 16 bars, A (8) + B (8)
//
// A: | F  | C  | Bb | C    | F  | Dm    | Bb C | F |   offbeat strums
// B: | Bb | F  | Gm C | F  | Bb | A7 Dm | Gm C7 | F |  trumpet duet, then tresillo
// The hook (bar 1) is answered a step lower (bar 2); B climbs and the
// A7 -> Dm turn gives the Latin colour before the ii-V home.
constexpr auto kTitleLead = NOTES(R"(
    A5 - . C6 - - A5 - F5 - G5 - A5 - - - |
    G5 - . Bb5 - - G5 - E5 - F5 - G5 - . . |
    F5 - D6 - - - C6 - Bb5 - A5 - Bb5 - D6 - |
    C6 - - - Bb5 - A5 - G5 - - - . E5 F5 G5 |
    A5 - . C6 - - A5 - F6 - - - E6 - C6 - |
    D6 - - - A5 - . F5 - - A5 - D6 - E6 - |
    F6 - D6 - Bb5 - . . E6 - C6 - G5 - Bb5 - |
    A5 - - - F5 - - - . . C5 - F5 - . . |
    D6 - D6 - C6 D6 - F6 - - D6 - Bb5 - - - |
    C6 - C6 - Bb5 C6 - A5 - - F5 - . . G5 A5 |
    Bb5 - Bb5 - A5 Bb5 - D6 C6 - Bb5 - G5 - E5 - |
    F5 - - - A5 - C6 - F6 - - - . . . . |
    D6 - . D6 - - C6 - Bb5 - . F5 - - G5 - |
    A5 - C#6 - E6 - C#6 - D6 - A5 - F5 - A5 - |
    Bb5 - D6 - G5 - Bb5 - E6 - C6 - Bb5 - G5 - |
    A5 - - - - - . . C6 - A5 - F5 - G5 - |
)");
constexpr auto kTitleChords = CHORDS(R"(
    F F | C C | Bb Bb | C C | F F | Dm Dm | Bb C | F F |
    Bb Bb | F F | Gm C | F F | Bb Bb | A7 Dm | Gm C7 | F F |
)");
constexpr char kTitleDrums[] = "DAAB" "AAAC" "AAAB" "AAAC";
constexpr char kTitleBass[]  = "AAAB" "AAAB" "AAAB" "AAAB";
constexpr char kTitleHarm[]  = "AAAA" "AAAA" "BBBB" "CCCA";

///////////////////////////////////////////////////////////////////////////
// RUN  "Carrera Picante"   G major / E minor, 144 BPM (up to ~166 with
// intensity), 32 bars: A (8) A' (8) B (8) B' (8)
//
// A : | G | C | D | G | G  | C | Am D | G  |     offbeat strums
// A': | G | C | D | G | Em | C | Am D | B7 |     trumpet duet, ends on B7
// B : | Em | B7 | Em | E7 | Am | Em | C B7 | Em | tresillo, E minor
// B': | C | D | Bm Em | Am | C | D | Am D7 | G |  strums, then duet home
// The A hook is a repeated-note fanfare (G G G B D); B answers it with a
// tresillo (3+3+2) rhythm in E minor with the bright B7 / E7 dominants.
constexpr auto kRunLead = NOTES(R"(
    G5 . G5 G5 B5 - D6 - . . B5 - D6 - E6 D6 |
    C6 - . E6 - - C6 - G5 - . E5 - - G5 - |
    F#5 . F#5 F#5 A5 - D6 - . . A5 - C6 - B5 A5 |
    G5 - B5 - D6 - - - . . D6 C6 B5 - A5 - |
    G5 . G5 G5 B5 - D6 - . . G6 - D6 - B5 - |
    E6 - . C6 - - G5 - E6 - D6 - C6 - . . |
    C6 - A5 - E5 - A5 - D6 - F#5 - A5 - C6 - |
    B5 - - - G5 - - - . . . . D5 E5 F#5 - |

    G5 . G5 G5 B5 - D6 - . . B5 - D6 - E6 D6 |
    E6 - . C6 - - E6 - G6 - . E6 - - C6 - |
    D6 . D6 D6 F#6 - D6 - . . A5 - C6 - D6 - |
    B5 - - - G5 - B5 - D6 - - - . . . . |
    E6 - E6 - D6 E6 - G6 - - E6 - B5 - - - |
    C6 - C6 - B5 C6 - E6 - - C6 - G5 - - - |
    A5 - C6 - E6 - C6 - D6 - A5 - F#5 - A5 - |
    B5 - - - A5 - F#5 - D#5 - F#5 - A5 - B5 - |

    E5 - - G5 - - B5 - - - - - . . C6 B5 |
    A5 - - F#5 - - D#5 - - - - - . . E5 F#5 |
    G5 - - B5 - - E6 - - - D6 - B5 - G5 - |
    G#5 - - B5 - - D6 - - - . . E6 - D6 - |
    C6 - - - B5 - A5 - E6 - - - . . . . |
    B5 - - - A5 - G5 - E5 - - - . . . . |
    E5 - G5 - C6 - E6 - D#6 - - - B5 - A5 - |
    G5 - - - F#5 - - - E5 - - - . . . . |

    E6 - . E6 - - D6 - C6 - . G5 - - E5 - |
    F#6 - . F#6 - - E6 - D6 - . A5 - - F#5 - |
    D6 - B5 - F#5 - B5 - E6 - B5 - G5 - B5 - |
    C6 - - - A5 - - - E5 - A5 - C6 - E6 - |
    E6 - . G6 - - E6 - C6 - - - G5 - - - |
    D6 - . F#6 - - D6 - A5 - - - F#5 - - - |
    A5 B5 C6 - E6 - C6 - D6 - C6 - A5 - F#5 - |
    G5 - - - - - . . . . D5 - E5 - F#5 - |
)");
constexpr auto kRunChords = CHORDS(R"(
    G G | C C | D D | G G | G G | C C | Am D | G G |
    G G | C C | D D | G G | Em Em | C C | Am D | B7 B7 |
    Em Em | B7 B7 | Em Em | E7 E7 | Am Am | Em Em | C B7 | Em Em |
    C C | D D | Bm Em | Am Am | C C | D D | Am D7 | G G |
)");
constexpr char kRunDrums[] = "GEEF" "EEEF" "GEEF" "EEEF" "HHHH" "HHHF" "GEEF" "EEEF";
constexpr char kRunBass[]  = "CCCD" "CCCD" "CCCD" "CCCD" "EEEE" "EEEF" "CCCD" "CCCD";
constexpr char kRunHarm[]  = "AAAA" "AAAA" "BBBB" "BBBB" "CCCC" "CCCC" "AAAA" "BBBB";

///////////////////////////////////////////////////////////////////////////
// DASH  "Fuego"   G major, RUN tempo + 8%, 8 bars, loops
//
// | G | C | D | G | Em | C | Am D | G D7 |
// Sixteenth-note arpeggios an octave above RUN's range, hats on every 16th.
constexpr auto kDashLead = NOTES(R"(
    G5 B5 D6 G6 D6 B5 G5 B5 D6 G6 D6 B5 D6 - E6 D6 |
    G5 C6 E6 G6 E6 C6 G5 C6 E6 G6 E6 C6 E6 - D6 C6 |
    A5 D6 F#6 A6 F#6 D6 A5 D6 F#6 A6 F#6 D6 F#6 - E6 D6 |
    B5 - G6 - B5 - G6 - D6 E6 F#6 G6 - - . . |
    E6 B5 G5 B5 E6 B5 G5 B5 E6 F#6 G6 - F#6 - E6 - |
    E6 C6 G5 C6 E6 C6 G5 C6 G6 - E6 - C6 - G5 - |
    A5 C6 E6 A6 E6 C6 A5 C6 F#6 D6 A5 D6 F#6 A6 F#6 D6 |
    G6 - D6 - B5 - G5 - F#5 A5 C6 D6 C6 - A5 - |
)");
constexpr auto kDashChords = CHORDS(R"(
    G G | C C | D D | G G | Em Em | C C | Am D | G D7 |
)");
constexpr char kDashDrums[] = "IIIJ" "IIIJ";
constexpr char kDashBass[]  = "CCCD" "CCCD";
constexpr char kDashHarm[]  = "AAAA" "AAAA";

///////////////////////////////////////////////////////////////////////////
// GAME_OVER  "Adios"   C minor, 2 bars of 88 ms steps (~2.9 s), plays once
//
// | Cm Fm | G7 Cm |  falls an octave from C6 with a Phrygian Db sigh,
// then a short B-D-F (G7) lift that lands on C.
constexpr auto kOverLead = NOTES(R"(
    C6 - G5 - Eb5 - C5 - Ab5 - F5 - Db5 - C5 - |
    B4 - D5 - F5 - - - C5 - - - - - . . |
)");
constexpr auto kOverChords = CHORDS("Cm Fm | G7 Cm");
constexpr char kOverDrums[] = "KL";
constexpr char kOverBass[]  = "GG";
constexpr char kOverHarm[]  = "DD";

struct Song {
    const uint8_t *lead;   // bars * 16 steps
    const uint8_t *chords; // bars * 2
    const char *drums;     // one pool letter per bar
    const char *bass;
    const char *harm;
    uint8_t bars;
    uint8_t key;           // tonic pitch class (major scale) for the duet
    bool loop;
    float step_ms;         // 16th-note length at normal tempo
};

#define SONG_CHECK(lead, chords, drums, bass, harm)                      \
    static_assert(lead.size() == (sizeof(drums) - 1) * kSteps, #lead);   \
    static_assert(chords.size() == (sizeof(drums) - 1) * 2, #chords);    \
    static_assert(sizeof(bass) == sizeof(drums), #bass);                 \
    static_assert(sizeof(harm) == sizeof(drums), #harm)

SONG_CHECK(kTitleLead, kTitleChords, kTitleDrums, kTitleBass, kTitleHarm);
SONG_CHECK(kRunLead, kRunChords, kRunDrums, kRunBass, kRunHarm);
SONG_CHECK(kDashLead, kDashChords, kDashDrums, kDashBass, kDashHarm);
SONG_CHECK(kOverLead, kOverChords, kOverDrums, kOverBass, kOverHarm);

constexpr float bpm_step(float bpm) { return 60000.0f / bpm / 4.0f; }

const Song kSongs[] = {
    {kTitleLead.v, kTitleChords.v, kTitleDrums, kTitleBass, kTitleHarm, 16, 5, true, bpm_step(116.0f)},
    {kRunLead.v, kRunChords.v, kRunDrums, kRunBass, kRunHarm, 32, 7, true, bpm_step(144.0f)},
    {kDashLead.v, kDashChords.v, kDashDrums, kDashBass, kDashHarm, 8, 7, true, bpm_step(144.0f * 1.08f)},
    {kOverLead.v, kOverChords.v, kOverDrums, kOverBass, kOverHarm, 2, 0, false, 88.0f},
};

///////////////////////////////////////////////////////////////////////////
// Pitch helpers

// Top octave (MIDI 108..119, C8..B8) in Hz; lower octaves halve it.
const float kTopOctave[12] = {4186.01f, 4434.92f, 4698.64f, 4978.03f, 5274.04f, 5587.65f,
                              5919.91f, 6271.93f, 6644.88f, 7040.00f, 7458.62f, 7902.13f};

float note_hz(int midi) {
    if(midi < 12) midi = 12;
    if(midi > 119) midi = 119;
    return kTopOctave[midi % 12] / float(1 << (9 - midi / 12));
}

uint16_t to_freq(float hz) {
    if(hz < 20.0f) hz = 20.0f;
    if(hz > 22050.0f) hz = 22050.0f;
    return uint16_t(hz + 0.5f);
}

int chord_root(uint8_t chord) { return chord & 15; }
Quality chord_quality(uint8_t chord) { return Quality(chord >> 4); }
int chord_third(uint8_t chord) { Quality q = chord_quality(chord); return (q == MIN || q == MIN7) ? 3 : 4; }
int chord_seventh(uint8_t chord) { return chord_quality(chord) == MAJ ? 11 : 10; }

bool chord_has(uint8_t chord, int pc) {
    int rel = (pc - chord_root(chord) + 12) % 12;
    if(rel == 0 || rel == 7 || rel == chord_third(chord)) return true;
    Quality q = chord_quality(chord);
    return (q == DOM7 || q == MIN7) && rel == 10;
}

// Second trumpet: a diatonic third under the lead, or for a note outside
// the key the nearest chord tone at least a minor third under it.
int duet_note(int lead, uint8_t chord, int key) {
    static const uint8_t major[7] = {0, 2, 4, 5, 7, 9, 11};
    int rel = (lead % 12 - key + 12) % 12;
    for(int i = 0; i < 7; i++) {
        if(major[i] == rel) {
            int below = (rel - major[(i + 5) % 7] + 12) % 12;
            return lead - below;
        }
    }
    for(int d = 3; d < 10; d++)
        if(chord_has(chord, (lead - d) % 12)) return lead - d;
    return lead - 3;
}

///////////////////////////////////////////////////////////////////////////
// Channels and levels

constexpr int kLeadCh = 3, kHarmCh = 4, kBassCh = 5, kDrumCh = 6;
constexpr int kSfxChannels = 3;

constexpr uint16_t kLeadVol  = 0x1a00;
constexpr uint16_t kDuetVol  = 0x1000;
constexpr uint16_t kStrumVol = 0x0e00;
constexpr uint16_t kBassVol  = 0x1c00;
constexpr uint16_t kKickVol  = 0x1c00;
constexpr uint16_t kSnareVol = 0x0e00;
constexpr uint16_t kHatVol   = 0x0600;
constexpr uint16_t kCrashVol = 0x0900;

constexpr float kLeadGapMs = 22.0f; // silence between two lead notes (tonguing)
constexpr float kBassGapMs = 35.0f; // bass is played short and bouncy

void start_note(AudioChannel &ch, uint8_t waves, uint16_t pulse, float hz,
                uint16_t attack, uint16_t decay, uint16_t sustain, uint16_t release) {
    ch.waveforms = waves;
    ch.pulse_width = pulse;
    ch.frequency = to_freq(hz);
    ch.attack_ms = attack;
    ch.decay_ms = decay;
    ch.sustain = sustain;
    ch.release_ms = release;
    ch.trigger_attack(); // ramps from the current level, so no click
}

void release(AudioChannel &ch, uint16_t ms) {
    if(ch.adsr_phase == blit::ADSRPhase::OFF || ch.adsr_phase == blit::ADSRPhase::RELEASE) return;
    ch.release_ms = ms;
    ch.trigger_release();
}

// A pitched music voice that is gated: it releases a little before the next
// note, so repeated notes are articulated.
struct Tone {
    int ch;
    uint16_t vol = 0;
    float hz = 0.0f;
    float held_ms = 0.0f;
    float release_in = -1.0f; // ms until the gate closes, < 0 = no gate pending
    bool vibrato = false;
    float lfo = 0.0f;
};

struct Strum {
    uint8_t notes[4] = {};
    uint8_t count = 0;
    uint8_t pos = 0;
    float next_ms = 0.0f;
    bool on = false;
};

enum class Drum : uint8_t { NONE, KICK, SNARE, HAT };

struct MusicState {
    Track track = Track::NONE;
    const Song *song = nullptr;
    uint16_t step = 0;      // next step to play
    float clock = 0.0f;     // ms since the last step was due
    bool finished = false;
    int run_resume_bar = -1;
    bool enabled = true;
    float intensity = 0.0f;
    float intensity_target = 0.0f;
    float gain = 1.0f;      // ducking while a big sound effect plays
    Tone lead{kLeadCh}, harm{kHarmCh}, bass{kBassCh};
    Strum strum;
    Drum drum = Drum::NONE;
    float drum_hz = 0.0f;
    uint16_t harm_vol = 0, drum_vol = 0;
};

MusicState m;

float song_step_ms() {
    if(!m.song) return 100.0f;
    float tempo = 1.0f;
    if(m.track == Track::RUN || m.track == Track::DASH) tempo += 0.15f * m.intensity;
    return m.song->step_ms / tempo;
}

uint8_t lead_at(const Song &s, int step) { return s.lead[step]; }
char bass_at(const Song &s, int step) { return kBassPool[s.bass[step / kSteps] - 'A'][step % kSteps]; }
char harm_at(const Song &s, int step) { return kHarmPool[s.harm[step / kSteps] - 'A'][step % kSteps]; }
char drum_at(const Song &s, int step) { return kDrumPool[s.drums[step / kSteps] - 'A'][step % kSteps]; }
uint8_t chord_at(const Song &s, int step) { return s.chords[step / (kSteps / 2)]; }

void silence_music(uint16_t release_ms) {
    for(int c = kLeadCh; c <= kDrumCh; c++) release(channels[c], release_ms);
    m.lead.release_in = m.harm.release_in = m.bass.release_in = -1.0f;
    m.lead.vibrato = false;
    m.strum.on = false;
    m.drum = Drum::NONE;
}

void tone_on(Tone &t, int midi, uint8_t waves, uint16_t pulse, uint16_t vol,
             uint16_t attack, uint16_t decay, uint16_t sustain, uint16_t release_ms) {
    AudioChannel &ch = channels[t.ch];
    t.hz = note_hz(midi);
    t.vol = vol;
    t.held_ms = 0.0f;
    t.lfo = 0.0f;
    t.release_in = -1.0f;
    ch.volume = uint16_t(float(vol) * m.gain);
    start_note(ch, waves, pulse, t.hz, attack, decay, sustain, release_ms);
}

void lead_on(int midi) {
    tone_on(m.lead, midi, blit::Waveform::SQUARE, 0x4000, kLeadVol, 6, 110, 0xa000, 18);
    m.lead.vibrato = true;
}

void duet_on(int midi) {
    m.strum.on = false;
    tone_on(m.harm, midi, blit::Waveform::SQUARE, 0x6000, kDuetVol, 6, 110, 0x9000, 18);
    m.harm_vol = kDuetVol;
}

void strum_on(uint8_t chord, bool long_strum) {
    // voicing: root between G3 and F#4, then third, fifth (and seventh)
    int root = 55 + (chord_root(chord) - 7 + 12) % 12;
    Strum &s = m.strum;
    s.notes[0] = uint8_t(root);
    s.notes[1] = uint8_t(root + chord_third(chord));
    s.notes[2] = uint8_t(root + 7);
    s.count = 3;
    Quality q = chord_quality(chord);
    if(q == DOM7 || q == MIN7) s.notes[s.count++] = uint8_t(root + 10);
    s.pos = 0;
    s.next_ms = 20.0f;
    s.on = true;
    m.harm.release_in = -1.0f;
    m.harm_vol = kStrumVol;
    AudioChannel &ch = channels[kHarmCh];
    ch.volume = uint16_t(float(kStrumVol) * m.gain);
    start_note(ch, blit::Waveform::SQUARE, 0x2000, note_hz(s.notes[0]),
               3, long_strum ? 500 : 90, 0, 20);
}

void drum_hit(char d) {
    AudioChannel &ch = channels[kDrumCh];
    uint16_t vol = 0;
    switch(d) {
        case 'k':
            m.drum = Drum::KICK; m.drum_hz = 170.0f; vol = kKickVol;
            start_note(ch, blit::Waveform::SQUARE, 0x8000, m.drum_hz, 1, 90, 0, 5);
            break;
        case 's':
            m.drum = Drum::SNARE; m.drum_hz = 9000.0f; vol = kSnareVol;
            start_note(ch, blit::Waveform::NOISE, 0x8000, m.drum_hz, 1, 130, 0, 5);
            break;
        case 'h':
            m.drum = Drum::HAT; vol = kHatVol;
            start_note(ch, blit::Waveform::NOISE, 0x8000, 22050.0f, 1, 30, 0, 5);
            break;
        case 'o':
            m.drum = Drum::HAT; vol = kHatVol;
            start_note(ch, blit::Waveform::NOISE, 0x8000, 22050.0f, 1, 120, 0, 5);
            break;
        case 'c':
            m.drum = Drum::HAT; vol = kCrashVol;
            start_note(ch, blit::Waveform::NOISE, 0x8000, 15000.0f, 1, 500, 0, 5);
            break;
        default:
            return;
    }
    m.drum_vol = vol;
    ch.volume = uint16_t(float(vol) * m.gain);
}

// Gate a tone so it is silent for `gap` ms before the next step is due.
void gate(Tone &t, float until_next, float gap) {
    float in = until_next - gap;
    t.release_in = in > 1.0f ? in : 1.0f;
}

void play_step() {
    const Song &s = *m.song;
    const int total = s.bars * kSteps;
    if(m.step >= total) {
        if(!s.loop) {
            m.finished = true;
            silence_music(80);
            return;
        }
        m.step = 0;
    }
    const int step = m.step;
    const int next = (step + 1 < total) ? step + 1 : (s.loop ? 0 : -1);
    const uint8_t chord = chord_at(s, step);
    const float step_ms = song_step_ms();
    const float until_next = step_ms - m.clock; // m.clock = how late this step is

    // lead
    uint8_t lt = lead_at(s, step);
    uint8_t lnext = next >= 0 ? lead_at(s, next) : REST;
    if(lt >= 12) lead_on(lt);
    else if(lt == REST) { release(channels[kLeadCh], 30); m.lead.release_in = -1.0f; }
    if(lt != REST && lnext != HOLD) gate(m.lead, until_next, kLeadGapMs);

    // harmony
    char ht = harm_at(s, step);
    if(ht == 't') {
        if(lt >= 12) duet_on(duet_note(lt, chord, s.key));
        else if(lt == REST) { release(channels[kHarmCh], 30); m.harm.release_in = -1.0f; }
        if(lt != REST && lnext != HOLD) gate(m.harm, until_next, kLeadGapMs);
    } else {
        if(m.harm.release_in < 0.0f && !m.strum.on) release(channels[kHarmCh], 30);
        if(ht == 'x' || ht == 'X') strum_on(chord, ht == 'X');
    }

    // bass: from the chord of this half bar
    char bt = bass_at(s, step);
    int root = 43 + (chord_root(chord) - 7 + 12) % 12; // G2 .. F#3
    int bn = -1;
    switch(bt) {
        case '1': bn = root; break;
        case '3': bn = root + chord_third(chord); break;
        case '5': bn = root + 7; break;
        case '7': bn = root + chord_seventh(chord); break;
        case '8': bn = root + 12; break;
        case 'w': {
            int nstep = ((step / (kSteps / 2)) + 1) * (kSteps / 2);
            if(nstep >= total) nstep = 0;
            int nroot = 43 + (chord_root(chord_at(s, nstep)) - 7 + 12) % 12;
            bn = nroot - 1;
            break;
        }
        case '.': release(channels[kBassCh], 30); m.bass.release_in = -1.0f; break;
        default: break;
    }
    if(bn >= 0) tone_on(m.bass, bn, blit::Waveform::SQUARE, 0x8000, kBassVol, 3, 120, 0x7000, 25);
    char bnext = next >= 0 ? bass_at(s, next) : '.';
    if(bt != '.' && bnext != '-') gate(m.bass, until_next, kBassGapMs);

    // drums
    drum_hit(drum_at(s, step));

    m.step++;
}

void update_tone(Tone &t, float dt) {
    AudioChannel &ch = channels[t.ch];
    ch.volume = uint16_t(float(t.vol) * m.gain);
    if(t.release_in >= 0.0f) {
        t.release_in -= dt;
        if(t.release_in <= 0.0f) {
            t.release_in = -1.0f;
            release(ch, 15);
        }
    }
    t.held_ms += dt;
    if(t.vibrato && t.held_ms > 160.0f && ch.adsr_phase != blit::ADSRPhase::OFF) {
        // gentle 5.5 Hz vibrato that fades in, like a held trumpet note
        t.lfo += dt * (2.0f * 3.14159265f * 5.5f / 1000.0f);
        if(t.lfo > 6.2831853f) t.lfo -= 6.2831853f;
        float depth = t.held_ms > 300.0f ? 0.008f : 0.008f * (t.held_ms - 160.0f) / 140.0f;
        ch.frequency = to_freq(t.hz * (1.0f + depth * std::sin(t.lfo)));
    }
}

void update_music(float dt) {
    // smooth intensity and ducking so tempo and level never jump
    float k = dt / 800.0f;
    if(k > 1.0f) k = 1.0f;
    m.intensity += (m.intensity_target - m.intensity) * k;

    update_tone(m.lead, dt);
    update_tone(m.bass, dt);
    if(m.strum.on) {
        AudioChannel &ch = channels[kHarmCh];
        ch.volume = uint16_t(float(m.harm_vol) * m.gain);
        if(ch.adsr_phase == blit::ADSRPhase::OFF) m.strum.on = false;
        else {
            m.strum.next_ms -= dt;
            if(m.strum.next_ms <= 0.0f) {
                m.strum.next_ms += 20.0f;
                m.strum.pos = uint8_t((m.strum.pos + 1) % m.strum.count);
                ch.frequency = to_freq(note_hz(m.strum.notes[m.strum.pos]));
            }
        }
    } else {
        update_tone(m.harm, dt);
    }

    AudioChannel &dch = channels[kDrumCh];
    dch.volume = uint16_t(float(m.drum_vol) * m.gain);
    if(m.drum == Drum::KICK) {
        m.drum_hz *= 0.72f; // falling pitch: the chiptune kick
        if(m.drum_hz < 45.0f) m.drum_hz = 45.0f;
        dch.frequency = to_freq(m.drum_hz);
    } else if(m.drum == Drum::SNARE) {
        m.drum_hz *= 0.88f;
        if(m.drum_hz < 3500.0f) m.drum_hz = 3500.0f;
        dch.frequency = to_freq(m.drum_hz);
    }

    if(!m.enabled || !m.song || m.finished) return;

    // sequencer: at most one step per update, so no note is ever skipped
    float step_ms = song_step_ms();
    m.clock += dt;
    if(m.clock >= step_ms) {
        m.clock -= step_ms;
        if(m.clock > step_ms) m.clock = 0.0f; // far behind (paused): do not rush
        play_step();
    }
}

///////////////////////////////////////////////////////////////////////////
// Sound effects: tiny per-channel sequencer of segments. Each segment is one
// note whose pitch glides exponentially from f0 to f1 over its length.

constexpr uint8_t SQ = blit::Waveform::SQUARE;
constexpr uint8_t TR = blit::Waveform::TRIANGLE;
constexpr uint8_t NZ = blit::Waveform::NOISE;

struct SfxSeg {
    uint8_t wave;
    uint8_t pulse;  // square duty, x/256
    uint16_t f0, f1; // Hz
    uint8_t ticks;  // length in 10 ms
    uint8_t vol;    // channel volume, x/256 of full
    uint8_t sustain; // 0 = fade out over the segment, else hold at x/256
};

const SfxSeg kSeg[] = {
    // 0 MENU_MOVE: short high tick
    {SQ, 0x40, 1320, 1320, 3, 0x18, 0},
    // 1 MENU_OK: two-note "ding-ding" up a fifth
    {SQ, 0x40, 988, 988, 5, 0x1e, 0}, {SQ, 0x40, 1480, 1480, 10, 0x1e, 0},
    // 3 LANE: tiny rising noise whoosh
    {NZ, 0, 3000, 11000, 8, 0x14, 0},
    // 4 JUMP: upward sweep
    {SQ, 0x40, 280, 840, 12, 0x22, 0x90},
    // 5 LAND: soft low thump
    {SQ | TR, 0x80, 170, 55, 7, 0x26, 0},
    // 6 SLIDE: falling noise swish
    {NZ, 0, 10000, 2500, 22, 0x18, 0x90},
    // 7 CHILI: bright blip and a fifth above (scaled by the combo)
    {SQ, 0x20, 1047, 1047, 3, 0x22, 0}, {SQ, 0x20, 1568, 1568, 8, 0x22, 0},
    // 9 GOLDEN: sparkling arpeggio C6 E6 G6 C7 E7
    {SQ, 0x40, 1047, 1047, 3, 0x22, 0}, {SQ, 0x40, 1319, 1319, 3, 0x22, 0},
    {SQ, 0x40, 1568, 1568, 3, 0x22, 0}, {SQ, 0x40, 2093, 2093, 3, 0x22, 0},
    {SQ, 0x40, 2637, 2637, 16, 0x22, 0},
    // 14 POWERUP: three rising sweeps
    {SQ, 0x60, 330, 660, 6, 0x22, 0xa0}, {SQ, 0x60, 494, 988, 6, 0x22, 0xa0},
    {SQ, 0x60, 660, 1320, 12, 0x22, 0},
    // 17 SHIELD_POP: crack then a falling "bwong"
    {NZ, 0, 14000, 14000, 2, 0x22, 0}, {SQ, 0x80, 900, 240, 12, 0x22, 0},
    // 19 NEAR_MISS: zip and a high ping
    {NZ, 0, 16000, 6000, 4, 0x16, 0}, {TR, 0, 2349, 2349, 8, 0x28, 0},
    // 21 DASH_START: rising noise roar + rising square (layer)
    {NZ, 0, 1200, 9000, 28, 0x22, 0xc0},
    {SQ, 0x40, 110, 440, 28, 0x18, 0xc0},
    // 23 SMASH: crunch + falling thud (layer)
    {NZ, 0, 2500, 2500, 2, 0x2c, 0}, {NZ, 0, 1400, 300, 14, 0x28, 0},
    {SQ, 0x80, 220, 50, 10, 0x20, 0},
    // 26 HIT: long falling noise crash + falling square (layer)
    {NZ, 0, 7000, 600, 45, 0x2c, 0},
    {SQ, 0x80, 880, 880, 3, 0x22, 0}, {SQ, 0x80, 700, 55, 40, 0x22, 0},
    // 29 COUNTDOWN: G5 beep
    {SQ, 0x80, 784, 784, 12, 0x20, 0},
    // 30 GO: G6 with G5 under it (layer)
    {SQ, 0x40, 1568, 1568, 30, 0x22, 0xa0},
    {SQ, 0x40, 784, 784, 30, 0x16, 0xa0},
    // 32 NEW_BEST: G5 C6 E6 G6 C7 fanfare + triangle a third under (layer)
    {SQ, 0x40, 784, 784, 5, 0x22, 0}, {SQ, 0x40, 1047, 1047, 5, 0x22, 0},
    {SQ, 0x40, 1319, 1319, 5, 0x22, 0}, {SQ, 0x40, 1568, 1568, 5, 0x22, 0},
    {SQ, 0x40, 2093, 2093, 40, 0x22, 0xa0},
    {TR, 0, 659, 659, 5, 0x20, 0}, {TR, 0, 784, 784, 5, 0x20, 0},
    {TR, 0, 1047, 1047, 5, 0x20, 0}, {TR, 0, 1319, 1319, 5, 0x20, 0},
    {TR, 0, 1568, 1568, 40, 0x20, 0xa0},
    // 42 MISSION: E6 G6 C7
    {SQ, 0x40, 1319, 1319, 5, 0x22, 0}, {SQ, 0x40, 1568, 1568, 5, 0x22, 0},
    {SQ, 0x40, 2093, 2093, 18, 0x22, 0x90},
    // 45 SHADES: slap of noise and a low falling thwack
    {NZ, 0, 9000, 2000, 4, 0x2c, 0}, {SQ, 0x80, 330, 80, 14, 0x28, 0},
    // 47 AIRHORN: BWAA-BWA-BWAAAA on Bb4, a fifth above as a layer
    {SQ, 0x60, 480, 466, 14, 0x2a, 0xd0}, {SQ, 0x60, 466, 466, 4, 0x00, 0},
    {SQ, 0x60, 480, 466, 8, 0x2a, 0xd0}, {SQ, 0x60, 466, 466, 4, 0x00, 0},
    {SQ, 0x60, 480, 452, 45, 0x2a, 0xc0},
    {SQ, 0x30, 720, 698, 14, 0x20, 0xd0}, {SQ, 0x30, 698, 698, 4, 0x00, 0},
    {SQ, 0x30, 720, 698, 8, 0x20, 0xd0}, {SQ, 0x30, 698, 698, 4, 0x00, 0},
    {SQ, 0x30, 720, 678, 45, 0x20, 0xc0},
};

struct SfxDef {
    uint8_t first, count;           // main part
    uint8_t layer_first, layer_count; // played at the same time on another channel
    uint8_t priority;               // 0 low .. 3 always heard
};

// in the order of audio::Sfx
const SfxDef kSfx[] = {
    {0, 1, 0, 0, 1},   // MENU_MOVE
    {1, 2, 0, 0, 1},   // MENU_OK
    {3, 1, 0, 0, 0},   // LANE
    {4, 1, 0, 0, 1},   // JUMP
    {5, 1, 0, 0, 0},   // LAND
    {6, 1, 0, 0, 1},   // SLIDE
    {7, 2, 0, 0, 1},   // CHILI
    {9, 5, 0, 0, 2},   // GOLDEN
    {14, 3, 0, 0, 2},  // POWERUP
    {17, 2, 0, 0, 2},  // SHIELD_POP
    {19, 2, 0, 0, 1},  // NEAR_MISS
    {21, 1, 22, 1, 2}, // DASH_START
    {23, 2, 25, 1, 2}, // SMASH
    {26, 1, 27, 2, 3}, // HIT
    {29, 1, 0, 0, 2},  // COUNTDOWN
    {30, 1, 31, 1, 2}, // GO
    {32, 5, 37, 5, 3}, // NEW_BEST
    {42, 3, 0, 0, 2},  // MISSION
    {45, 2, 0, 0, 3},  // SHADES
    {47, 5, 52, 5, 3}, // AIRHORN
};
static_assert(sizeof(kSfx) / sizeof(kSfx[0]) == size_t(Sfx::AIRHORN) + 1, "one entry per Sfx");
static_assert(sizeof(kSeg) / sizeof(kSeg[0]) == 57, "segment indices above");

// CHILI combo 1..10 climbs a major scale from C6
const uint8_t kComboSteps[10] = {0, 2, 4, 5, 7, 9, 11, 12, 14, 16};

struct SfxVoice {
    const SfxSeg *seg = nullptr;
    uint8_t left = 0;     // segments after this one
    uint8_t priority = 0;
    uint8_t id = 0xff;
    bool active = false;
    float t = 0.0f, dur = 1.0f;
    float ratio = 1.0f;   // pitch scale (CHILI combo)
    float glide = 0.0f;   // log2(f1 / f0)
    uint32_t serial = 0;
};

SfxVoice voices[kSfxChannels];
uint32_t sfx_serial = 0;
bool sound_enabled = true;

void start_seg(int c) {
    SfxVoice &v = voices[c];
    const SfxSeg &s = *v.seg;
    AudioChannel &ch = channels[c];
    v.t = 0.0f;
    v.dur = float(s.ticks) * 10.0f;
    v.glide = std::log2(float(s.f1) / float(s.f0));
    ch.volume = uint16_t(s.vol << 8);
    uint16_t decay = s.sustain ? 40 : uint16_t(s.ticks * 10);
    start_note(ch, s.wave, uint16_t(s.pulse ? s.pulse << 8 : 0x8000), float(s.f0) * v.ratio,
               2, decay, uint16_t(s.sustain << 8), 40);
}

int pick_channel(uint8_t id, uint8_t priority, int exclude, bool layer) {
    for(int c = 0; c < kSfxChannels; c++) // same sound already playing: restart it there
        if(c != exclude && voices[c].active && voices[c].id == id) return c;
    for(int c = 0; c < kSfxChannels; c++)
        if(c != exclude && !voices[c].active) return c;
    int best = -1;
    for(int c = 0; c < kSfxChannels; c++) {
        if(c == exclude) continue;
        const SfxVoice &v = voices[c];
        bool may = layer ? v.priority < priority : (v.priority <= priority || priority >= 3);
        if(!may) continue;
        if(best < 0 || v.priority < voices[best].priority ||
           (v.priority == voices[best].priority && v.serial < voices[best].serial))
            best = c;
    }
    return best;
}

void start_part(int c, uint8_t id, uint8_t priority, uint8_t first, uint8_t count, float ratio) {
    SfxVoice &v = voices[c];
    v.seg = &kSeg[first];
    v.left = uint8_t(count - 1);
    v.priority = priority;
    v.id = id;
    v.active = true;
    v.ratio = ratio;
    v.serial = ++sfx_serial;
    start_seg(c);
}

void play_sfx(Sfx s, float ratio) {
    if(!sound_enabled) return;
    const uint8_t id = uint8_t(s);
    const SfxDef &d = kSfx[id];
    int c = pick_channel(id, d.priority, -1, false);
    if(c < 0) return;
    start_part(c, id, d.priority, d.first, d.count, ratio);
    if(d.layer_count) {
        // the layer gets its own id so it is not mistaken for the main part
        int l = pick_channel(uint8_t(id | 0x80), d.priority, c, true);
        if(l >= 0) start_part(l, uint8_t(id | 0x80), d.priority, d.layer_first, d.layer_count, ratio);
    }
}

void update_sfx(float dt) {
    bool big = false;
    for(int c = 0; c < kSfxChannels; c++) {
        SfxVoice &v = voices[c];
        if(!v.active) continue;
        AudioChannel &ch = channels[c];
        v.t += dt;
        if(v.t >= v.dur) {
            if(v.left == 0) {
                v.active = false;
                release(ch, 40);
                continue;
            }
            v.left--;
            v.seg++;
            start_seg(c);
        } else if(v.glide != 0.0f) {
            float hz = float(v.seg->f0) * v.ratio * std::exp2(v.glide * (v.t / v.dur));
            ch.frequency = to_freq(hz);
        }
        if(v.priority >= 3) big = true;
    }
    // duck the music under HIT and NEW_BEST, and let it come back gently
    float target = big ? 0.5f : 1.0f;
    float k = dt / (big ? 30.0f : 400.0f);
    if(k > 1.0f) k = 1.0f;
    m.gain += (target - m.gain) * k;
}

uint32_t last_time = 0;
bool have_time = false;

} // namespace

///////////////////////////////////////////////////////////////////////////
// Public interface

void init() {
    for(int c = 0; c <= kDrumCh; c++) {
        AudioChannel &ch = channels[c];
        ch.off();
        ch.waveforms = 0;
        ch.volume = 0;
        ch.adsr = 0;
        ch.filter_enable = false;
        ch.wave_buffer_callback = nullptr;
    }
    for(auto &v : voices) v = SfxVoice();
    bool music = m.enabled;
    m = MusicState();
    m.enabled = music;
    have_time = false;
}

void update(uint32_t time_ms) {
    float dt = 10.0f;
    if(have_time) {
        uint32_t d = time_ms - last_time;
        dt = d > 50 ? 50.0f : float(d); // after a pause, carry on, do not rush
    }
    last_time = time_ms;
    have_time = true;
    update_sfx(dt);
    update_music(dt);
}

void play(Track track) {
    if(track == m.track) return;
    // RUN -> DASH -> RUN picks RUN up again at the bar after the one the
    // dash interrupted, so the run tune carries on instead of restarting.
    int start_bar = 0;
    if(track == Track::DASH && m.track == Track::RUN && m.song) {
        int bar = m.step > 0 ? (m.step - 1) / kSteps : 0;
        m.run_resume_bar = (bar + 1) % m.song->bars;
    } else if(track == Track::RUN && m.track == Track::DASH && m.run_resume_bar >= 0) {
        start_bar = m.run_resume_bar;
    }
    if(track != Track::DASH) m.run_resume_bar = -1;
    silence_music(20);
    m.track = track;
    m.finished = false;
    m.song = track == Track::NONE ? nullptr : &kSongs[int(track) - 1];
    m.step = uint16_t(start_bar * kSteps);
    if(m.song) m.clock = m.song->step_ms; // first step on the next update
}

void sfx(Sfx s) {
    if(s == Sfx::CHILI) { sfx_combo(1); return; }
    play_sfx(s, 1.0f);
}

void sfx_combo(int combo) {
    if(combo < 1) combo = 1;
    if(combo > 10) combo = 10;
    float ratio = std::exp2(float(kComboSteps[combo - 1]) / 12.0f);
    play_sfx(Sfx::CHILI, ratio);
}

void set_intensity(float t) {
    if(!(t >= 0.0f)) t = 0.0f; // also catches NaN
    if(t > 1.0f) t = 1.0f;
    m.intensity_target = t;
}

void set_music(bool on) {
    if(on == m.enabled) return;
    m.enabled = on;
    if(!on) silence_music(8);
}

void set_sound(bool on) {
    if(on == sound_enabled) return;
    sound_enabled = on;
    if(!on) {
        for(int c = 0; c < kSfxChannels; c++) {
            voices[c].active = false;
            release(channels[c], 8);
        }
    }
}

bool music_on() { return m.enabled; }
bool sound_on() { return sound_enabled; }

} // namespace audio
