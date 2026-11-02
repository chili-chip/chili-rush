#pragma once
// Chili Rush sound: original chiptune music and sound effects on the
// 32blit audio channels. Channels 0-2 are sound effects, 3-6 are music.
#include <cstdint>

namespace audio {

enum class Track : uint8_t {
    NONE,
    TITLE,     // title and menus: catchy, mid tempo
    RUN,       // in a run: driving, fast; tempo follows set_intensity()
    DASH,      // fire dash (a few seconds): frantic variation of RUN
    GAME_OVER, // short jingle, plays once then silence
};

enum class Sfx : uint8_t {
    MENU_MOVE,
    MENU_OK,
    LANE,       // lane change whoosh
    JUMP,
    LAND,
    SLIDE,
    CHILI,      // pickup; pitch rises with combo (see sfx_combo)
    GOLDEN,     // golden chili
    POWERUP,    // shield or magnet picked up
    SHIELD_POP, // shield absorbs a hit
    NEAR_MISS,
    DASH_START,
    SMASH,      // dash breaks an obstacle
    HIT,        // crash, run over
    COUNTDOWN,  // 3, 2, 1
    GO,
    NEW_BEST,
    MISSION,    // a mission is completed
    SHADES,     // intro: the shades land on the hero's face
    AIRHORN,    // intro: three air horn blasts
};

void init();
// Call every update() with blit::now(). Steps the sequencer.
void update(uint32_t time_ms);
void play(Track track);       // switching to the playing track is a no-op
void sfx(Sfx s);
void sfx_combo(int combo);    // CHILI pickup at combo level 1..10, higher pitch
void set_intensity(float t);  // 0..1 during a run, raises RUN tempo a little
void set_music(bool on);
void set_sound(bool on);
bool music_on();
bool sound_on();
}
