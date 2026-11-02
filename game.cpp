// Chili Rush: a pseudo-3D endless runner for the VGC Zero.
//
// The road is drawn one screen row at a time: each row below the horizon
// is a slice of road at depth z = FOCAL / (row - HORIZON). Sprites are
// scaled by their depth, so nothing here needs a 3D engine or a GPU.
// Game logic runs at the engine's fixed 100 updates per second.

#include "game.hpp"
#include "assets.hpp"
#include "audio.hpp"
#include "sprites.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace blit;

// ---------------------------------------------------------------------------
// Constants

namespace {

constexpr int SW = 128;
constexpr int SH = 128;
constexpr int HORIZON = 46;         // screen row of the horizon
// Projection: row offset = FOCAL / z. A long lens (large FOCAL) keeps far
// obstacles big enough to tell apart; the player's lane width does not change.
constexpr float FOCAL = 200.0f;
// bends were tuned at FOCAL 100: keep their shape on screen
constexpr float CURVE_K = (100.0f / FOCAL) * (100.0f / FOCAL);
constexpr float LANE_W = 0.45f;     // world width of a lane
constexpr float ROAD_HALF = 0.72f;  // half the asphalt width
constexpr float RUMBLE = 0.07f;     // kerb width
constexpr int PLAYER_FEET_Y = 120;  // screen row of the player's feet
constexpr float Z_PLAYER = FOCAL / float(PLAYER_FEET_Y - HORIZON);
constexpr float Z_FAR = 34.0f;      // objects appear here
constexpr float FADE_IN = 4.0f;     // and fade in over this depth
constexpr float Z_NEAR = Z_PLAYER * 0.3f;  // passed objects vanish here (at about 3x size)
constexpr float SEGMENT = 0.5f;     // stripe length on the road
constexpr float HIT_DEPTH = 0.22f;  // player hitbox along the road, either side of the feet
constexpr float DT = 0.01f;         // seconds per update()

constexpr float SPEED_START = 7.5f;   // world units per second
constexpr float SPEED_MAX = 19.0f;
constexpr float SPEED_RAMP = 2600.0f; // distance to get ~63% of the way to max

constexpr float JUMP_TIME = 0.62f;    // seconds in the air
constexpr float JUMP_HEIGHT = 0.36f;  // world units
constexpr float SLIDE_TIME = 0.62f;
constexpr float FAST_FALL = 2.5f;     // slide pressed in the air: fall this much faster
constexpr float LANE_TIME = 0.11f;    // seconds to move one lane

constexpr float DASH_TIME = 4.0f;
constexpr float DASH_SPEED = 1.45f;   // speed multiplier during a dash
constexpr float GRACE_TIME = 1.2f;    // blink after a shield pop or a dash
constexpr float MAGNET_TIME = 10.0f;

constexpr float BIOME_LENGTH = 900.0f; // distance per biome
constexpr float BIOME_BLEND = 60.0f;   // colours fade over this distance

constexpr float METRES_PER_UNIT = 1.0f;
// Long runs keep float precision: every REBASE units the run's distances
// are moved back by REBASE (exactly, a power of two) and counted in Run::origin.
constexpr float REBASE = 1024.0f;

// ---------------------------------------------------------------------------
// Small helpers

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }

Pen mix(const Pen &a, const Pen &b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    return Pen(int(lerpf(a.r, b.r, t)), int(lerpf(a.g, b.g, t)), int(lerpf(a.b, b.b, t)));
}

uint32_t rng_state = 0x1234567u;
uint32_t rnd() {
    // xorshift32: deterministic per run, seeded from the clock at run start
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x ? x : 0x9e3779b9u;
    return rng_state;
}
int rnd_int(int n) { return n <= 0 ? 0 : int(rnd() % uint32_t(n)); }
float rnd_float() { return float(rnd() & 0xffff) / 65535.0f; }
bool chance(float p) { return rnd_float() < p; }

// Hash for stable procedural scenery (buildings, mountain jitter).
uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void text_shadow(std::string_view s, const Font &font, Point p, Pen fg, TextAlign align = TextAlign::top_left) {
    screen.pen = Pen(20, 10, 24);
    screen.text(s, font, Point(p.x + 1, p.y + 1), true, align);
    screen.pen = fg;
    screen.text(s, font, p, true, align);
}

// ---------------------------------------------------------------------------
// Biomes: colours of the sky, ground and road, blended at the borders

struct Biome {
    const char *name;
    Pen sky_top, sky_low, sun, far_hills, near_hills;
    Pen grass_a, grass_b, road_a, road_b, kerb_a, kerb_b, line, fog;
};

const Biome biomes[3] = {
    {"DESERT", Pen(70, 40, 120), Pen(255, 150, 80), Pen(255, 230, 120), Pen(170, 80, 90), Pen(120, 50, 70),
     Pen(222, 160, 90), Pen(206, 142, 76), Pen(92, 80, 96), Pen(84, 72, 88), Pen(230, 50, 50), Pen(250, 240, 230),
     Pen(255, 230, 140), Pen(250, 170, 110)},
    {"NEON CITY", Pen(10, 8, 40), Pen(90, 30, 110), Pen(240, 240, 255), Pen(40, 30, 80), Pen(25, 18, 55),
     Pen(40, 36, 70), Pen(32, 28, 60), Pen(50, 48, 64), Pen(44, 42, 58), Pen(255, 60, 200), Pen(60, 230, 255),
     Pen(255, 240, 90), Pen(80, 34, 110)},
    {"VOLCANO", Pen(40, 10, 16), Pen(200, 60, 30), Pen(255, 200, 80), Pen(80, 30, 30), Pen(50, 18, 22),
     Pen(70, 40, 40), Pen(58, 32, 34), Pen(46, 38, 40), Pen(40, 32, 36), Pen(255, 120, 20), Pen(60, 30, 30),
     Pen(255, 170, 40), Pen(170, 60, 40)},
};

struct Colours {
    Pen sky_top, sky_low, sun, far_hills, near_hills;
    Pen grass_a, grass_b, road_a, road_b, kerb_a, kerb_b, line, fog;
};

Colours blend(const Biome &a, const Biome &b, float t) {
    return {mix(a.sky_top, b.sky_top, t), mix(a.sky_low, b.sky_low, t), mix(a.sun, b.sun, t),
            mix(a.far_hills, b.far_hills, t), mix(a.near_hills, b.near_hills, t), mix(a.grass_a, b.grass_a, t),
            mix(a.grass_b, b.grass_b, t), mix(a.road_a, b.road_a, t), mix(a.road_b, b.road_b, t),
            mix(a.kerb_a, b.kerb_a, t), mix(a.kerb_b, b.kerb_b, t), mix(a.line, b.line, t), mix(a.fog, b.fog, t)};
}

// Whole units taken off every distance along the road so far (REBASE).
uint32_t road_origin = 0;

// Units from the start of the run to `distance`, and how far into its biome.
uint32_t road_units(float distance) { return road_origin + uint32_t(std::max(0.0f, distance)); }
float total_distance(float distance) { return float(road_origin) + distance; }
int biome_at(float distance) { return int((road_units(distance) / uint32_t(BIOME_LENGTH)) % 3u); }
float in_biome(float distance) {
    return float(road_units(distance) % uint32_t(BIOME_LENGTH)) + (distance - floorf(distance));
}

// ---------------------------------------------------------------------------
// Save data: best score, totals, missions and settings in one struct

constexpr uint32_t SAVE_MAGIC = 0x48535243; // "CRSH"
constexpr uint16_t SAVE_VERSION = 1;
constexpr int MISSION_SLOTS = 3;

enum MissionType : uint8_t {
    M_CHILIS,    // collect N chilis in one run
    M_DISTANCE,  // run N metres in one run
    M_JUMPS,     // jump over N obstacles in one run
    M_SLIDES,    // slide under N obstacles in one run
    M_CLOSE,     // N close calls in one run
    M_SCORE,     // score N in one run
    M_DASHES,    // use fire dash N times in one run
    M_COMBO,     // reach a combo of N
    M_SMASH,     // smash N obstacles with fire dash in one run
    M_TYPES
};

struct Mission {
    uint8_t type;
    uint8_t level;    // grows each time this type is completed
    uint16_t pad;
    uint32_t target;
};

struct SaveData {
    uint32_t magic;
    uint16_t version;
    uint8_t music;
    uint8_t sound;
    uint32_t best;
    uint32_t best_distance;
    uint32_t total_chilis;
    uint32_t runs;
    uint32_t missions_done;
    Mission missions[MISSION_SLOTS];
    uint8_t type_level[M_TYPES];
    uint8_t seen_help;
    uint8_t pad[2];
};

SaveData save;

uint32_t mission_target(uint8_t type, uint8_t level) {
    const uint32_t l = level;
    switch(type) {
        case M_CHILIS:   return 30 + l * 25;
        case M_DISTANCE: return 500 + l * 400;
        case M_JUMPS:    return 5 + l * 4;
        case M_SLIDES:   return 5 + l * 4;
        case M_CLOSE:    return 3 + l * 3;
        case M_SCORE:    return 2000 + l * 2500;
        case M_DASHES:   return 1 + l;
        case M_COMBO:    return std::min<uint32_t>(10 + l * 10, 250);
        case M_SMASH:    return 3 + l * 3;
    }
    return 1;
}

const char *mission_text(const Mission &m, char *buf, int len) {
    const unsigned t = unsigned(m.target);
    switch(m.type) {
        case M_CHILIS:   snprintf(buf, len, "GRAB %u CHILIS", t); break;
        case M_DISTANCE: snprintf(buf, len, "RUN %u M", t); break;
        case M_JUMPS:    snprintf(buf, len, "JUMP OVER %u", t); break;
        case M_SLIDES:   snprintf(buf, len, "SLIDE UNDER %u", t); break;
        case M_CLOSE:    snprintf(buf, len, "%u CLOSE CALLS", t); break;
        case M_SCORE:    snprintf(buf, len, "SCORE %u", t); break;
        case M_DASHES:   snprintf(buf, len, "FIRE DASH %u TIMES", t); break;
        case M_COMBO:    snprintf(buf, len, "COMBO OF %u", t); break;
        case M_SMASH:    snprintf(buf, len, "SMASH %u WITH DASH", t); break;
        default:         snprintf(buf, len, "?"); break;
    }
    return buf;
}

// A new mission of a type no other slot has.
Mission new_mission(int slot) {
    for(int tries = 0; tries < 50; tries++) {
        uint8_t type = uint8_t(rnd_int(M_TYPES));
        bool used = false;
        for(int i = 0; i < MISSION_SLOTS; i++)
            if(i != slot && save.missions[i].type == type) used = true;
        if(!used) {
            uint8_t level = save.type_level[type];
            return {type, level, 0, mission_target(type, level)};
        }
    }
    return {M_DISTANCE, 0, 0, mission_target(M_DISTANCE, 0)};
}

void reset_save() {
    memset(&save, 0, sizeof(save));
    save.magic = SAVE_MAGIC;
    save.version = SAVE_VERSION;
    save.music = 1;
    save.sound = 1;
    // fixed easy first missions so the first run teaches the moves
    save.missions[0] = {M_CHILIS, 0, 0, mission_target(M_CHILIS, 0)};
    save.missions[1] = {M_JUMPS, 0, 0, mission_target(M_JUMPS, 0)};
    save.missions[2] = {M_DISTANCE, 0, 0, mission_target(M_DISTANCE, 0)};
}

// A save of this version with a damaged mission keeps everything else: the
// mission is replaced (bad or repeated type) or its target recomputed.
void repair_save() {
    for(int i = 0; i < MISSION_SLOTS; i++) {
        Mission &m = save.missions[i];
        bool repeat = false;
        for(int j = 0; j < i; j++) repeat |= save.missions[j].type == m.type;
        if(m.type >= M_TYPES || repeat) {
            m.type = M_TYPES;  // not a type: new_mission() ignores it
            m = new_mission(i);
        }
        m.target = mission_target(m.type, m.level);
    }
    save.music = save.music ? 1 : 0;
    save.sound = save.sound ? 1 : 0;
}

void load_game() {
    if(!read_save(save) || save.magic != SAVE_MAGIC || save.version != SAVE_VERSION) reset_save();
    repair_save();
}

void store_game() { write_save(save); }

// Rank: one star per three missions, each star adds +1 to the base multiplier.
int rank() { return int(std::min<uint32_t>(save.missions_done / 3, 20)); }
int base_multiplier() { return 1 + rank(); }

// ---------------------------------------------------------------------------
// World objects

enum Kind : uint8_t {
    K_NONE,
    K_LOW,      // jump over (fence, cart, lava rock)
    K_HIGH,     // slide under (banner, neon sign, arch)
    K_BLOCK,    // change lane (cactus, vending machine, spire)
    K_WEED,     // rolling tumbleweed, low, drifts across lanes
    K_CHILI,
    K_GOLDEN,
    K_MILK,
    K_SOMBRERO,
    K_DECOR,
};

bool is_obstacle(Kind k) { return k == K_LOW || k == K_HIGH || k == K_BLOCK || k == K_WEED; }
bool is_pickup(Kind k) { return k == K_CHILI || k == K_GOLDEN || k == K_MILK || k == K_SOMBRERO; }

struct Obj {
    Kind kind;
    uint8_t biome;
    uint8_t art;      // decor variant
    bool done;        // passed the player (scored or missed)
    float x;          // lateral world position
    float s;          // absolute distance along the road
    float h;          // height above the road (pickups on jump arcs)
    float vx;         // lateral speed (tumbleweed)
    float target_x;   // tumbleweed: lane it rolls to
    bool collected;
    bool smashed;
    float fly;        // smashed: time since smash
};

constexpr int MAX_OBJS = 256;
Obj objs[MAX_OBJS];

#ifdef CHILI_SELFTEST
// hooks for the headless self-test (selftest.hpp)
void st_spawned(Obj &o);
int st_pool_full = 0;
void st_profile(int phase);  // render timing
bool st_ghost = false;  // the test bot passes through obstacles
#endif

Obj *spawn(Kind k, float x, float s, float h = 0.0f) {
    for(auto &o : objs) {
        if(o.kind == K_NONE) {
            o = Obj{};
            o.kind = k;
            o.x = x;
            o.s = s;
            o.h = h;
            o.biome = uint8_t(biome_at(s));
#ifdef CHILI_SELFTEST
            st_spawned(o);
#endif
            return &o;
        }
    }
#ifdef CHILI_SELFTEST
    st_pool_full++;
#endif
    return nullptr; // pool full: skip (generation keeps well under the limit)
}

float lane_x(int lane) { return float(lane - 1) * LANE_W; }
bool in_lane_of(const Obj &o, float x) { return fabsf(o.x - x) < LANE_W * 0.55f; }

// ---------------------------------------------------------------------------
// Particles and pop-up texts

struct Particle {
    float x, y, vx, vy, life, max_life;
    Pen pen;
    uint8_t size;
};
constexpr int MAX_PARTICLES = 96;
Particle particles[MAX_PARTICLES];

void emit(float x, float y, float vx, float vy, float life, Pen pen, uint8_t size = 1) {
    for(auto &p : particles) {
        if(p.life <= 0.0f) {
            p = {x, y, vx, vy, life, life, pen, size};
            return;
        }
    }
}

void burst(float x, float y, int n, Pen pen, float speed = 40.0f, uint8_t size = 1) {
    for(int i = 0; i < n; i++) {
        float a = rnd_float() * 6.2831f;
        float v = speed * (0.4f + rnd_float() * 0.6f);
        emit(x, y, cosf(a) * v, sinf(a) * v - 20.0f, 0.3f + rnd_float() * 0.4f, pen, size);
    }
}

struct Popup {
    char text[16];
    float x, y, life;
    Pen pen;
};
constexpr int MAX_POPUPS = 6;
Popup popups[MAX_POPUPS];

void popup(const char *text, float x, float y, Pen pen) {
    Popup *slot = &popups[0];
    for(auto &p : popups) {
        if(p.life <= 0.0f) { slot = &p; break; }
        if(p.life < slot->life) slot = &p;
    }
    // stack under the live ones instead of drawing on top of them
    for(int pass = 0; pass < MAX_POPUPS; pass++) {
        bool moved = false;
        for(auto &p : popups)
            if(&p != slot && p.life > 0.0f && fabsf(p.y - y) < 9.0f) {
                y = p.y + 9.0f;
                moved = true;
            }
        if(!moved) break;
    }
    snprintf(slot->text, sizeof(slot->text), "%s", text);
    slot->x = x;
    slot->y = y;
    slot->life = 0.9f;
    slot->pen = pen;
}

// ---------------------------------------------------------------------------
// Game state

enum class State { TITLE, MISSIONS, SETTINGS, HELP, READY, RUN, PAUSE, RESUME, CRASH, OVER };
State state = State::TITLE;
float state_time = 0.0f;
int state_ticks = 0;  // updates in this state: exact timing for countdowns
int run_ticks_since_go = 1000;  // the big "GO!" shows for the first 0.35 s of a run

void set_state(State s) {
    state = s;
    state_time = 0.0f;
    state_ticks = 0;
}

// ---------------------------------------------------------------------------
// The runner's body: lane, jump and slide. Pure functions with no effects,
// so the self-test's planner (CHILI_SELFTEST) moves exactly like the game.

struct Body {
    int lane;
    float x;            // lateral position, eases to the lane
    float jump_t;       // >0 while in the air, counts up to JUMP_TIME
    float slide_t;      // >0 while sliding
    float jump_buffer;  // >0: jump pressed just before landing; <0: slide queued
    bool fast_fall;     // slide pressed in the air: dropping to the ground
};

enum BodyEvent : uint8_t { EV_JUMP = 1, EV_SLIDE = 2, EV_LAND = 4, EV_LANE = 8, EV_KERB = 16 };

bool body_in_air(const Body &b) { return b.jump_t > 0.0f; }
bool body_sliding(const Body &b) { return b.slide_t > 0.0f; }

float body_height(const Body &b) {
    if(!body_in_air(b)) return 0.0f;
    float t = b.jump_t / JUMP_TIME;  // 0..1
    return JUMP_HEIGHT * 4.0f * t * (1.0f - t);
}

uint8_t body_jump(Body &b) {
    if(body_in_air(b)) {
        b.jump_buffer = 0.12f;
        return 0;
    }
    b.slide_t = 0.0f;
    b.jump_t = 0.0001f;
    b.fast_fall = false;
    return EV_JUMP;
}

uint8_t body_slide(Body &b) {
    if(body_in_air(b)) {
        // drop out of the air fast and slide on landing; still rising: carry
        // on from the same height on the way down, so the runner never snaps
        if(b.jump_t < JUMP_TIME * 0.5f) b.jump_t = JUMP_TIME - b.jump_t;
        b.fast_fall = true;
        b.slide_t = 0.0f;
        b.jump_buffer = -0.2f;  // negative: slide queued
        return 0;
    }
    uint8_t ev = body_sliding(b) ? 0 : EV_SLIDE;
    b.slide_t = 0.0001f;
    return ev;
}

uint8_t body_lane(Body &b, int dir) {
    int lane = std::clamp(b.lane + dir, 0, 2);
    if(lane == b.lane) return EV_KERB;
    b.lane = lane;
    return EV_LANE;
}

// One update (DT) of the body.
uint8_t body_step(Body &b) {
    uint8_t ev = 0;
    // ease towards the lane
    // ease out: fast start, soft arrival; a minimum step so it always lands
    float target = float(b.lane - 1) * LANE_W;
    float delta = target - b.x;
    float step = delta * (DT / (LANE_TIME * 0.38f));
    float min_step = LANE_W * DT / (LANE_TIME * 2.5f);
    if(fabsf(step) < min_step) step = delta > 0.0f ? min_step : -min_step;
    if(fabsf(delta) <= fabsf(step)) b.x = target;
    else b.x += step;

    if(body_in_air(b)) {
        b.jump_t += b.fast_fall ? DT * FAST_FALL : DT;
        if(b.jump_t >= JUMP_TIME) {
            b.jump_t = 0.0f;
            b.fast_fall = false;
            ev |= EV_LAND;
            if(b.jump_buffer > 0.0f) {
                b.jump_buffer = 0.0f;
                ev |= body_jump(b);
            } else if(b.jump_buffer < 0.0f) {
                b.jump_buffer = 0.0f;
                b.slide_t = 0.0001f;
                ev |= EV_SLIDE;
            }
        }
    }
    if(b.jump_buffer > 0.0f) b.jump_buffer = std::max(0.0f, b.jump_buffer - DT);

    if(body_sliding(b)) {
        b.slide_t += DT;
        if(b.slide_t >= SLIDE_TIME) b.slide_t = 0.0f;
    }
    return ev;
}

struct Run {
    float distance;      // the player's position along the road, from road_origin
    float speed;
    float cam_x;
    // player
    Body p;
    float lane_change_t;  // time since last lane change (close calls)
    int prev_lane;        // the lane before the last change
    float run_anim;
    float land_t;          // >0 just after landing: squash frame
    // power
    float heat;           // 0..1, full: fire dash ready
    float dash_t;         // >0 during a dash
    float grace_t;        // >0: cannot be hit, blinks
    float magnet_t;
    bool shield;
    // score
    uint32_t score;
    float score_frac;
    uint32_t chilis;
    int combo;
    int best_combo;
    uint32_t jumps_over, slides_under, close_calls, dashes, smashed;
    // world generation
    float next_spawn;     // distance of the next obstacle row
    float next_decor;
    float next_power;
    float last_row;       // distance of the last obstacle row
    float curve, curve_target, next_curve;
    float bg_scroll;
    // feedback
    float shake;
    float flash;
    Pen flash_pen;
    bool new_best;
    bool missions_shown;
    int missions_completed_this_run;
    int last_biome;
    float biome_banner;
    bool recorded;        // totals and best saved for this run
};
Run run;

uint32_t best_at_start = 0;
float view_dx = 0.0f, view_dy = 0.0f; // screen shake offset
int menu_cursor = 0;
bool attract = true; // the title screen road runs by itself

// Sounds of the run itself; the title screen demo plays none of them.
void world_sfx(audio::Sfx s) {
    if(!attract) audio::sfx(s);
}

// Frame rate counter, toggled with X on the title screen (for testing on the console).
bool show_fps = false;
uint32_t fps_frames = 0, fps_last = 0, fps_value = 0, frame_us = 0;

// ---------------------------------------------------------------------------
// Missions progress

uint32_t run_metres() { return uint32_t(total_distance(run.distance) * METRES_PER_UNIT); }

uint32_t mission_value(uint8_t type) {
    switch(type) {
        case M_CHILIS:   return run.chilis;
        case M_DISTANCE: return run_metres();
        case M_JUMPS:    return run.jumps_over;
        case M_SLIDES:   return run.slides_under;
        case M_CLOSE:    return run.close_calls;
        case M_SCORE:    return run.score;
        case M_DASHES:   return run.dashes;
        case M_COMBO:    return uint32_t(run.best_combo);
        case M_SMASH:    return run.smashed;
    }
    return 0;
}

// Completed missions are kept in place (shown ticked on the results
// screen) and replaced by finish_missions() when the run ends.
bool mission_complete[MISSION_SLOTS];

void check_missions() {
    if(attract) return;
    for(int i = 0; i < MISSION_SLOTS; i++) {
        if(mission_complete[i]) continue;
        if(mission_value(save.missions[i].type) >= save.missions[i].target) {
            mission_complete[i] = true;
            run.missions_completed_this_run++;
            world_sfx(audio::Sfx::MISSION);
            popup("MISSION!", 64, 46, Pen(120, 255, 140));
        }
    }
}

void finish_missions() {
    for(int i = 0; i < MISSION_SLOTS; i++) {
        if(!mission_complete[i]) continue;
        uint8_t type = save.missions[i].type;
        if(save.type_level[type] < 250) save.type_level[type]++;
        save.missions_done++;
        save.missions[i] = new_mission(i);
        mission_complete[i] = false;
    }
}

// ---------------------------------------------------------------------------
// World generation
//
// Obstacles come in rows across the three lanes. Every row leaves a way
// through: an empty lane, or only obstacles you can jump or slide. Rows are
// at least `min_gap()` apart, which is the distance covered while doing one
// full jump plus a lane change, so any row can be reached from any lane.

float min_gap() {
    float v = run.speed;
    return std::max(3.2f, v * (JUMP_TIME + LANE_TIME * 2.0f + 0.12f));
}

// Difficulty 0..1 by distance.
float difficulty() { return clampf(total_distance(run.distance) / 4000.0f, 0.0f, 1.0f); }

void chili_line(int lane, float s, int count, float step) {
    for(int i = 0; i < count; i++) spawn(K_CHILI, lane_x(lane), s + float(i) * step);
}

// Chilis on the arc of a jump over an obstacle at distance s.
void chili_arc(int lane, float s) {
    float half = run.speed * JUMP_TIME * 0.5f;
    for(int i = -2; i <= 2; i++) {
        float t = float(i) / 2.5f;  // -0.8..0.8
        float h = JUMP_HEIGHT * (1.0f - t * t) * 0.92f;
        spawn(K_CHILI, lane_x(lane), s + t * half, h);
    }
}

Kind random_passable(float d) {
    // jumpable or slidable, weighted by difficulty
    float r = rnd_float();
    if(r < 0.5f) return K_LOW;
    if(r < 0.9f || d < 0.15f) return K_HIGH;
    return K_LOW;
}

void spawn_row(float s) {
    float d = difficulty();
    Kind lane_kind[3] = {K_NONE, K_NONE, K_NONE};

    bool weed = false;
    int pattern = rnd_int(100);
    if(total_distance(run.distance) < 120.0f) {
        // first rows of a run: one easy obstacle at a time, to warm up
        lane_kind[rnd_int(3)] = (rnd_int(2) ? K_LOW : K_BLOCK);
    } else if(pattern < 25) {
        // single obstacle
        lane_kind[rnd_int(3)] = chance(0.4f) ? K_BLOCK : random_passable(d);
    } else if(pattern < 55) {
        // two lanes blocked, one free
        int free_lane = rnd_int(3);
        for(int l = 0; l < 3; l++)
            if(l != free_lane) lane_kind[l] = chance(0.55f) ? K_BLOCK : random_passable(d);
    } else if(pattern < 70) {
        // full row of the same passable obstacle: must jump or slide
        Kind k = chance(0.5f) ? K_LOW : K_HIGH;
        for(auto &lk : lane_kind) lk = k;
    } else if(pattern < 85) {
        // two blocks and a passable obstacle in the third lane
        int open = rnd_int(3);
        for(int l = 0; l < 3; l++) lane_kind[l] = (l == open) ? random_passable(d) : K_BLOCK;
    } else {
        // a tumbleweed rolling across, plus maybe a sign in the middle lane
        weed = true;
        int from = chance(0.5f) ? 0 : 2;
        Obj *w = spawn(K_WEED, lane_x(from), s);
        if(w) {
            w->target_x = lane_x(2 - from);
            w->vx = (w->target_x - w->x) / 1.6f;  // crosses the road in 1.6 s
        }
        if(d > 0.3f && chance(0.5f)) lane_kind[1] = K_HIGH;
    }

    for(int l = 0; l < 3; l++)
        if(lane_kind[l] != K_NONE) spawn(lane_kind[l], lane_x(l), s);

    // chilis: a line in a free lane, or an arc over a jumpable obstacle
    int free_lanes[3], nf = 0;
    for(int l = 0; l < 3; l++)
        if(lane_kind[l] == K_NONE) free_lanes[nf++] = l;
    int line_lane = -1;
    if(nf > 0 && chance(0.75f)) {
        line_lane = free_lanes[rnd_int(nf)];
        // stays clear of the next row, which is at least min_gap() * 1.1 away
        int most = int(min_gap() * 1.4f / 0.7f);
        // a tumbleweed rolls through every lane: end the line before it
        if(weed) most = int((min_gap() * 0.45f - (HIT_DEPTH + 0.4f)) / 0.7f) + 1;
        chili_line(line_lane, s - min_gap() * 0.45f, std::min(4 + rnd_int(4), most), 0.7f);
    } else {
        for(int l = 0; l < 3; l++)
            if(lane_kind[l] == K_LOW && chance(0.7f)) { chili_arc(l, s); break; }
    }

    // golden chili now and then, in a free lane right after the row (not
    // on top of the chili line)
    if(nf > 0 && chance(0.04f + 0.04f * d)) {
        int i = rnd_int(nf);
        int l = free_lanes[i];
        if(l == line_lane) l = nf > 1 ? free_lanes[(i + 1 + rnd_int(nf - 1)) % nf] : -1;
        if(l >= 0) spawn(K_GOLDEN, lane_x(l), s + 1.5f);
    }
}

// A shield or a magnet at s, in a lane with no chilis close by.
void spawn_power(float s) {
    Kind k = chance(0.5f) ? K_MILK : K_SOMBRERO;
    int first = rnd_int(3);
    for(int i = 0; i < 3; i++) {
        int lane = (first + i) % 3;
        bool clear = true;
        for(auto &o : objs)
            if(is_pickup(o.kind) && fabsf(o.s - s) < 1.0f && in_lane_of(o, lane_x(lane))) clear = false;
        if(clear || i == 2) {
            spawn(k, lane_x(lane), s, 0.05f);
            return;
        }
    }
}

void spawn_decor(float s) {
    for(int side = -1; side <= 1; side += 2) {
        if(!chance(0.7f)) continue;
        Obj *o = spawn(K_DECOR, float(side) * (ROAD_HALF + 0.35f + rnd_float() * 0.6f), s + rnd_float() * 2.0f);
        if(o) o->art = uint8_t(rnd_int(4));
    }
}

void update_generation() {
    float horizon_s = run.distance + Z_FAR;
    while(run.next_spawn < horizon_s) {
        // a power-up halfway between two rows, never inside one
        if(run.next_power < run.next_spawn) {
            spawn_power((run.last_row + run.next_spawn) * 0.5f);
            run.next_power += 220.0f + rnd_float() * 180.0f;
        }
        spawn_row(run.next_spawn);
        run.last_row = run.next_spawn;
        float d = difficulty();
        float gap = min_gap() * lerpf(1.9f, 1.1f, d) + rnd_float() * min_gap() * lerpf(1.2f, 0.5f, d);
        run.next_spawn += gap;
    }
    while(run.next_decor < horizon_s) {
        spawn_decor(run.next_decor);
        run.next_decor += 1.6f + rnd_float() * 1.6f;
    }
}

// ---------------------------------------------------------------------------
// Run setup

void start_run(bool demo) {
    attract = demo;
    run = Run{};
    road_origin = 0;
    for(auto &o : objs) o.kind = K_NONE;
    for(auto &p : particles) p.life = 0.0f;
    for(auto &p : popups) p.life = 0.0f;
    for(auto &m : mission_complete) m = false;
    rng_state = (now() * 2654435761u) ^ 0xa5a5a5a5u;
    if(!rng_state) rng_state = 1;
    run.p.lane = 1;
    run.prev_lane = 1;
    run.p.x = 0.0f;
    run.speed = SPEED_START;
    run.next_spawn = 16.0f;
    run.next_decor = 0.0f;
    run.next_power = 160.0f + rnd_float() * 80.0f;
    run.next_curve = 40.0f;
    run.combo = 0;
    run.last_biome = 0;
    best_at_start = save.best;
    update_generation();
}

// ---------------------------------------------------------------------------
// Player actions

bool in_air() { return body_in_air(run.p); }
bool sliding() { return body_sliding(run.p); }
bool dashing() { return run.dash_t > 0.0f; }
float jump_height() { return body_height(run.p); }

void do_jump() {
    if(body_jump(run.p) & EV_JUMP) world_sfx(audio::Sfx::JUMP);
}

void do_slide() {
    if(body_slide(run.p) & EV_SLIDE) world_sfx(audio::Sfx::SLIDE);
}

void change_lane(int dir) {
    int from = run.p.lane;
    if(body_lane(run.p, dir) & EV_KERB) {
        // bumping the kerb: small shake, no penalty
        run.shake = std::max(run.shake, 0.06f);
        return;
    }
    run.prev_lane = from;
    run.lane_change_t = 0.0f;
    world_sfx(audio::Sfx::LANE);
}

void start_dash() {
    if(run.heat < 1.0f || dashing()) return;
    run.heat = 0.0f;
    run.dash_t = DASH_TIME;
    run.dashes++;
    run.flash = 0.25f;
    run.flash_pen = Pen(255, 160, 40);
    world_sfx(audio::Sfx::DASH_START);
    audio::play(audio::Track::DASH);
    popup("FIRE DASH!", 64, 52, Pen(255, 200, 60));
}

void add_score(uint32_t points) { run.score += points; }

int multiplier() { return base_multiplier() + std::min(run.combo / 10, 4); }

// ---------------------------------------------------------------------------
// Projection

struct Screen {
    float x, y, scale;
};

// World point (lateral x, height h, depth z ahead of the camera) to screen.
Screen project(float x, float h, float z) {
    float k = FOCAL / z;
    float sx = 64.0f + view_dx + (x - run.cam_x + run.curve * CURVE_K * z * z) * k;
    float sy = float(HORIZON) + view_dy + (1.0f - h) * k;
    return {sx, sy, Z_PLAYER / z};
}

float depth_of(const Obj &o) { return o.s - run.distance + Z_PLAYER; }


// ---------------------------------------------------------------------------
// Crash

// Count the run in the save, once, however it ends (crash, quit, power off):
// runs, chilis, best score and distance. Missions reached by now count too.
void record_run() {
    if(attract || run.recorded) return;
    check_missions();
    run.recorded = true;
    save.runs++;
    save.total_chilis += run.chilis;
    save.best_distance = std::max(save.best_distance, run_metres());
    save.best = std::max(save.best, run.score);
}

void crash() {
    world_sfx(audio::Sfx::HIT);
    audio::play(audio::Track::NONE);
    run.shake = 0.5f;
    run.flash = 0.2f;
    run.flash_pen = Pen(255, 255, 255);
    burst(64, float(PLAYER_FEET_Y - 10), 24, Pen(255, 80, 60), 70.0f, 2);
    set_state(State::CRASH);

    // results and saving happen once, here
    if(run.score > save.best) run.new_best = best_at_start > 0;  // the first run is always a best, no fanfare
    record_run();
    store_game();
}

void finish_run_save() {
    finish_missions();
    store_game();
}

// ---------------------------------------------------------------------------
// Update: one run step

void collect(Obj &o) {
    o.collected = true;
    o.done = true;
    Screen p = project(o.x, o.h, std::max(depth_of(o), 0.5f));
    switch(o.kind) {
        case K_CHILI: {
            run.chilis++;
            run.combo++;
            run.best_combo = std::max(run.best_combo, run.combo);
            run.heat = std::min(1.0f, run.heat + (dashing() ? 0.0f : 0.025f));
            add_score(uint32_t(10 * multiplier()));
            if(!attract) audio::sfx_combo(std::min(10, 1 + run.combo / 3));
            burst(p.x, p.y - 6, 5, Pen(255, 220, 80), 30.0f);
            if(run.combo > 0 && run.combo % 10 == 0 && run.combo / 10 <= 4) {  // the multiplier went up
                char buf[16];
                snprintf(buf, sizeof(buf), "X%d!", multiplier());
                popup(buf, 64, 60, Pen(255, 240, 120));
            }
            break;
        }
        case K_GOLDEN:
            run.heat = 1.0f;
            add_score(uint32_t(100 * multiplier()));
            world_sfx(audio::Sfx::GOLDEN);
            burst(p.x, p.y - 6, 14, Pen(255, 230, 60), 50.0f, 2);
            popup("HEAT FULL!", 64, 52, Pen(255, 200, 60));
            break;
        case K_MILK:
            run.shield = true;
            world_sfx(audio::Sfx::POWERUP);
            burst(p.x, p.y - 6, 12, Pen(240, 250, 255), 40.0f, 2);
            popup("SHIELD", 64, 52, Pen(220, 240, 255));
            break;
        case K_SOMBRERO:
            run.magnet_t = MAGNET_TIME;
            world_sfx(audio::Sfx::POWERUP);
            burst(p.x, p.y - 6, 12, Pen(255, 200, 80), 40.0f, 2);
            popup("MAGNET", 64, 52, Pen(255, 210, 120));
            break;
        default: break;
    }
}

// Is the body clear of this kind of obstacle (jumping or sliding)?
bool clears(const Body &b, Kind k) {
    switch(k) {
        case K_LOW:
        case K_WEED: return body_height(b) > 0.12f;
        case K_HIGH: return body_sliding(b);
        default: return false;
    }
}


// Does the obstacle hit a body whose feet are at `distance`?
bool hits(const Body &b, const Obj &o, float distance) {
    return is_obstacle(o.kind) && fabsf(o.s - distance) < HIT_DEPTH && in_lane_of(o, b.x) && !clears(b, o.kind);
}

// Tumbleweed: starts rolling once it is close enough to be seen (depth z),
// then rolls from side to side.
void roll_weed(Obj &o, float z) {
    if(z >= 16.0f) return;
    o.x += o.vx * DT;
    if((o.vx > 0.0f && o.x > o.target_x) || (o.vx < 0.0f && o.x < o.target_x)) {
        o.x = o.target_x;
        o.vx = -o.vx;
        o.target_x = lane_x(2) - o.target_x + lane_x(0);  // bounce back to the other side
    }
}

void obstacle_hit(Obj &o) {
    if(attract) {
        o.done = true;  // the title screen demo never crashes
        return;
    }
#ifdef CHILI_SELFTEST
    if(st_ghost) {
        o.done = true;
        return;
    }
#endif
    if(dashing()) {
        o.smashed = true;
        o.done = true;
        run.smashed++;
        add_score(uint32_t(50 * multiplier()));
        world_sfx(audio::Sfx::SMASH);
        run.shake = std::max(run.shake, 0.15f);
        Screen p = project(o.x, 0.0f, std::max(depth_of(o), 0.5f));
        burst(p.x, p.y - 10, 14, Pen(255, 140, 40), 60.0f, 2);
        return;
    }
    if(run.grace_t > 0.0f) {
        o.done = true;
        return;
    }
    if(run.shield) {
        run.shield = false;
        run.grace_t = GRACE_TIME;
        o.done = true;
        o.smashed = true;
        world_sfx(audio::Sfx::SHIELD_POP);
        run.shake = 0.25f;
        run.flash = 0.15f;
        run.flash_pen = Pen(230, 240, 255);
        popup("SAVED!", 64, 52, Pen(220, 240, 255));
        return;
    }
    crash();
}

void update_objects() {
    float px = run.p.x;
    for(auto &o : objs) {
        if(o.kind == K_NONE) continue;
        float z = depth_of(o);

        if(o.smashed) o.fly += DT;
        if(z < 0.2f) {
            o.kind = K_NONE;  // behind the camera
            continue;
        }

        if(o.kind == K_WEED && !o.done) roll_weed(o, z);

        // magnet pulls chilis into the player's lane
        if(o.kind == K_CHILI && !o.done && run.magnet_t > 0.0f && z < 9.0f) {
            o.x = lerpf(o.x, px, 0.12f);
            o.h = lerpf(o.h, jump_height() + 0.05f, 0.12f);
        }
        if(o.kind == K_CHILI && !o.done && dashing() && z < 7.0f) {
            o.x = lerpf(o.x, px, 0.15f);
            o.h = lerpf(o.h, 0.1f, 0.15f);
        }

        if(o.done || o.kind == K_DECOR) continue;

        float dz = o.s - run.distance;  // >0 ahead of the feet
        bool same_lane = fabsf(o.x - px) < LANE_W * 0.55f;

        if(is_pickup(o.kind)) {
            if(fabsf(dz) < 0.35f && same_lane && fabsf(o.h - jump_height()) < 0.24f) {
                collect(o);
            } else if(dz < -0.35f) {
                o.done = true;
                if(o.kind == K_CHILI && run.combo > 0) {
                    // a missed chili breaks the combo
                    if(run.combo >= 10) popup("COMBO LOST", 64, 70, Pen(200, 200, 220));
                    run.combo = 0;
                }
            }
            continue;
        }

        if(is_obstacle(o.kind)) {
            if(fabsf(dz) < HIT_DEPTH && same_lane) {
                if(hits(run.p, o, run.distance)) {
                    obstacle_hit(o);
                    if(state != State::RUN) return;
                }
            } else if(dz < -HIT_DEPTH) {
                o.done = true;
                // cleared it: count how
                if(fabsf(o.x - px) < LANE_W * 0.55f) {
                    if(o.kind == K_LOW || o.kind == K_WEED) run.jumps_over++;
                    if(o.kind == K_HIGH) run.slides_under++;
                    add_score(uint32_t(5 * multiplier()));
                } else if(in_lane_of(o, lane_x(run.prev_lane)) && run.lane_change_t < 0.3f && !dashing()) {
                    // dodged out of its lane at the last moment
                    run.close_calls++;
                    add_score(uint32_t(25 * multiplier()));
                    world_sfx(audio::Sfx::NEAR_MISS);
                    popup("CLOSE!", 64, 70, Pen(120, 230, 255));
                    run.heat = std::min(1.0f, run.heat + 0.06f);
                }
            }
        }
    }
}

void update_player() {
    uint8_t ev = body_step(run.p);
    if(run.land_t > 0.0f) run.land_t = std::max(0.0f, run.land_t - DT);
    run.lane_change_t += DT;
    if(ev & EV_LAND) {
        run.land_t = 0.09f;
        world_sfx(audio::Sfx::LAND);
        burst(64, float(PLAYER_FEET_Y), 4, Pen(200, 170, 140), 20.0f);
    }
    if(ev & EV_JUMP) world_sfx(audio::Sfx::JUMP);
    if(ev & EV_SLIDE) world_sfx(audio::Sfx::SLIDE);

    // 8 frames per stride; stride rate grows with speed but less than linearly
    run.run_anim = fmodf(run.run_anim + DT * (9.0f + run.speed * 0.9f), 8.0f);
}

// Move every distance along the road back by REBASE, exactly, so a run of
// hours keeps the same precision as the first kilometre.
void rebase() {
    road_origin += uint32_t(REBASE);
    run.distance -= REBASE;
    for(auto &o : objs) o.s -= REBASE;
    run.next_spawn -= REBASE;
    run.next_decor -= REBASE;
    run.next_power -= REBASE;
    run.next_curve -= REBASE;
    run.last_row -= REBASE;
}

// Speed eases up with distance, extra during a dash. One update's step.
float next_speed(float speed, float distance, bool dash) {
    float base = SPEED_START + (SPEED_MAX - SPEED_START) * (1.0f - expf(-total_distance(distance) / SPEED_RAMP));
    return lerpf(speed, base * (dash ? DASH_SPEED : 1.0f), 0.03f);
}

void update_run(bool input) {
    // speed: eases up with distance, extra during a dash
    run.speed = next_speed(run.speed, run.distance, dashing());

    if(input) {
        if(buttons.pressed & Button::DPAD_LEFT) change_lane(-1);
        if(buttons.pressed & Button::DPAD_RIGHT) change_lane(1);
        if(buttons.pressed & (Button::A | Button::DPAD_UP)) do_jump();
        if(buttons.pressed & (Button::B | Button::DPAD_DOWN)) do_slide();
        if(buttons.pressed & (Button::Y | Button::X)) start_dash();
    } else {
        // attract mode: a simple autopilot that dodges blocks
        float look = run.speed * 0.45f;
        for(auto &o : objs) {
            if(!is_obstacle(o.kind) || o.done) continue;
            float dz = o.s - run.distance;
            if(dz < 0.0f || dz > look) continue;
            if(fabsf(o.x - lane_x(run.p.lane)) > LANE_W * 0.5f) continue;
            if(o.kind == K_LOW && dz < run.speed * 0.2f) do_jump();
            else if(o.kind == K_HIGH && dz < run.speed * 0.15f) do_slide();
            else if(o.kind == K_BLOCK || o.kind == K_WEED) {
                // move to the nearest lane with nothing coming
                const int order[3][3] = {{1, 2, 0}, {chance(0.5f) ? 0 : 2, 0, 2}, {1, 0, 2}};
                for(int l : order[run.p.lane]) {
                    bool clear = l != run.p.lane;
                    for(auto &q : objs)
                        if(is_obstacle(q.kind) && !q.done && q.s - run.distance > -0.3f && q.s - run.distance < look &&
                           in_lane_of(q, lane_x(l)))
                            clear = false;
                    if(clear) {
                        run.p.lane = l;
                        break;
                    }
                }
            }
        }
    }

    float prev = run.distance;
    run.distance += run.speed * DT;

    // distance score, 1 point per metre times the multiplier
    run.score_frac += (run.distance - prev) * METRES_PER_UNIT * float(multiplier());
    if(run.score_frac >= 1.0f) {
        uint32_t whole = uint32_t(run.score_frac);
        add_score(whole);
        run.score_frac -= float(whole);
    }
    if(run.distance >= 2.0f * REBASE) rebase();

    // road curves: a new random bend every so often
    if(run.distance > run.next_curve) {
        run.curve_target = chance(0.3f) ? 0.0f : (rnd_float() * 2.0f - 1.0f) * 0.014f;
        run.next_curve = run.distance + 25.0f + rnd_float() * 50.0f;
    }
    run.curve = lerpf(run.curve, run.curve_target, 0.004f);
    run.bg_scroll += run.curve * run.speed * DT * 220.0f;

    // camera follows the player a little behind
    run.cam_x = lerpf(run.cam_x, run.p.x * 0.75f, 0.15f);

    update_player();
    update_generation();
    update_objects();
    if(state != State::RUN && !attract) return;

    if(run.dash_t > 0.0f) {
        run.dash_t -= DT;
        if(run.dash_t <= 0.0f) {
            run.dash_t = 0.0f;
            run.grace_t = GRACE_TIME;
            if(!attract) audio::play(audio::Track::RUN);
        }
        // flames behind the runner
        if(rnd_int(2) == 0)
            emit(64.0f + float(rnd_int(12) - 6), float(PLAYER_FEET_Y - 6 - rnd_int(14)), float(rnd_int(20) - 10), 30.0f,
                 0.35f, rnd_int(2) ? Pen(255, 180, 40) : Pen(255, 80, 20), 2);
    }
    if(run.grace_t > 0.0f) run.grace_t = std::max(0.0f, run.grace_t - DT);
    if(run.magnet_t > 0.0f) run.magnet_t = std::max(0.0f, run.magnet_t - DT);

    // running dust
    if(!in_air() && rnd_int(6) == 0)
        emit(64.0f + float(rnd_int(8) - 4), float(PLAYER_FEET_Y), float(rnd_int(16) - 8), 10.0f, 0.3f,
             Pen(220, 200, 170));

    // biome change banner
    int b = biome_at(run.distance);
    if(b != run.last_biome) {
        run.last_biome = b;
        run.biome_banner = 2.0f;
    }
    if(run.biome_banner > 0.0f) run.biome_banner -= DT;

    if(!attract) {
        audio::set_intensity(clampf((run.speed - SPEED_START) / (SPEED_MAX - SPEED_START), 0.0f, 1.0f));
        check_missions();
        if(!run.new_best && best_at_start > 0 && run.score > best_at_start) {
            run.new_best = true;
            world_sfx(audio::Sfx::NEW_BEST);
            popup("NEW BEST!", 64, 46, Pen(255, 240, 120));
        }
    }
}

void update_effects() {
    for(auto &p : particles) {
        if(p.life <= 0.0f) continue;
        p.life -= DT;
        p.x += p.vx * DT;
        p.y += p.vy * DT;
        p.vy += 120.0f * DT;
    }
    for(auto &p : popups) {
        if(p.life <= 0.0f) continue;
        p.life -= DT;
        p.y -= 10.0f * DT;  // 9 px over its life: never up into the biome banner
    }
    if(run.shake > 0.0f) run.shake = std::max(0.0f, run.shake - DT);
    if(run.flash > 0.0f) run.flash = std::max(0.0f, run.flash - DT);
}

// ---------------------------------------------------------------------------
// Rendering: background

Colours colours_now(float distance) {
    float into = in_biome(distance);
    int b = biome_at(distance);
    if(into > BIOME_LENGTH - BIOME_BLEND) {
        float t = (into - (BIOME_LENGTH - BIOME_BLEND)) / BIOME_BLEND;
        return blend(biomes[b], biomes[(b + 1) % 3], t);
    }
    return blend(biomes[b], biomes[b], 0.0f);
}

// Mountain or skyline height at world column fx (screen column plus the
// whole-pixel scroll) for the given biome.
int far_height(int biome, int32_t fx) {
    if(biome == 1) {
        // city: blocky towers
        int block = int(floorf(float(fx) / 9.0f));
        return 8 + int(hash32(uint32_t(block) + 77u) % 18u);
    }
    float f = float(fx);
    if(biome == 2) {
        // volcano: a big cone in the middle of the panorama plus ridges
        float c = float(((fx % 256) + 256) % 256);
        float cone = 26.0f - fabsf(c - 128.0f) * 0.45f;
        float ridge = 6.0f + 4.0f * sinf(f * 0.05f) + 3.0f * sinf(f * 0.13f);
        if(cone > 22.0f) cone = 22.0f + (cone - 22.0f) * 0.2f;  // flat crater top
        return int(std::max(cone, ridge));
    }
    return int(12.0f + 6.0f * sinf(f * 0.031f) + 4.0f * sinf(f * 0.077f + 1.0f) + 2.0f * sinf(f * 0.19f));
}

int near_height(int biome, int32_t fx) {
    if(biome == 1) {
        int block = int(floorf(float(fx) / 6.0f));
        return 3 + int(hash32(uint32_t(block) * 3u + 5u) % 9u);
    }
    float f = float(fx);
    return int(5.0f + 3.0f * sinf(f * 0.06f + 2.0f) + 2.0f * sinf(f * 0.15f));
}

// Heights by world column, kept while they are on screen: a bend scrolls
// the hills a pixel or two per frame, so only those columns call sinf(),
// the costliest part of a frame on the RP2350.
struct HillCache {
    int biome = -1;
    int32_t tag[256];
    int8_t h[256];

    void heights(int b, int32_t first, int (*f)(int, int32_t), int8_t *out) {
        if(b != biome) {
            biome = b;
            for(auto &t : tag) t = INT32_MIN;
        }
        for(int x = 0; x < SW; x++) {
            int32_t fx = first + x;
            int i = fx & 255;
            if(tag[i] != fx) {
                tag[i] = fx;
                h[i] = int8_t(f(b, fx));
            }
            out[x] = h[i];
        }
    }
};
HillCache far_cache, near_cache;

void render_sky(const Colours &c, const Pen *gradient, int biome, uint32_t time) {
    for(int y = 0; y <= HORIZON; y++) {
        screen.pen = gradient[y];
        screen.h_span(Point(0, y), SW);
    }
    // sun or moon, drifting with the bends
    int sx = 64 - int(run.bg_scroll * 0.1f) % 160;
    while(sx < -30) sx += 160;
    if(biome == 1) {
        screen.pen = c.sun;
        screen.circle(Point(sx, 16), 7);
        screen.pen = mix(c.sky_top, c.sky_low, 0.3f);
        screen.circle(Point(sx + 3, 14), 6);  // crescent
        // stars
        for(int i = 0; i < 18; i++) {
            uint32_t h = hash32(uint32_t(i) * 31u);
            if(((time / 300u) + h) % 7u == 0u) continue;  // twinkle
            screen.pen = Pen(255, 255, 255);
            screen.pixel(Point(int(h % 128u), int((h >> 8) % 30u)));
        }
    } else {
        int r = biome == 2 ? 9 : 12;
        int cy = HORIZON - 12;
        screen.pen = c.sun;
        screen.circle(Point(sx, cy), r);
        // retro stripes through the lower half of the sun
        screen.pen = gradient[cy + 4];
        for(int i = 0; i < 4; i++) screen.h_span(Point(sx - r, cy + 2 + i * 3), r * 2 + 1);
    }
    // far hills or skyline; the parallax moves in whole pixels
    int8_t far_h[SW], near_h[SW];
    int32_t far_first = int32_t(floorf(run.bg_scroll * 0.3f));
    far_cache.heights(biome, far_first, far_height, far_h);
    near_cache.heights(biome, int32_t(floorf(run.bg_scroll)), near_height, near_h);
    screen.pen = c.far_hills;
    for(int x = 0; x < SW; x++) screen.v_span(Point(x, HORIZON - far_h[x]), far_h[x] + 1);
    if(biome == 1) {
        // lit windows on the skyline
        screen.pen = Pen(255, 210, 90);
        for(int x = 1; x < SW; x += 3) {
            int block = int(floorf(float(far_first + x) / 9.0f));
            for(int y = HORIZON - far_h[x] + 2; y < HORIZON - 1; y += 3) {
                if(hash32(uint32_t(block * 131 + x * 7 + y)) % 3u) continue;
                screen.pixel(Point(x, y));
            }
        }
    }
    if(biome == 2) {
        // lava glow at the crater
        for(int x = 0; x < SW; x++) {
            if(far_h[x] >= 22) {
                screen.pen = Pen(255, 120 + int((time / 80u + uint32_t(x)) % 60u), 30);
                screen.pixel(Point(x, HORIZON - far_h[x]));
            }
        }
    }
    screen.pen = c.near_hills;
    for(int x = 0; x < SW; x++) screen.v_span(Point(x, HORIZON - near_h[x]), near_h[x] + 1);
}

// ---------------------------------------------------------------------------
// Rendering: road

// Everything about a screen row of road that does not change from frame to
// frame: its depth, fog and widths, and its colours until the biome colours
// change (they only do during the fade between biomes).
struct RoadRows {
    bool geometry = false;
    float z[SH], half[SH], kerb_w[SH];
    int line_w[SH];
    float fog[SH];
    bool have_colours = false;
    Colours colours;
    Pen grass[2][SH], kerb[2][SH], road[2][SH], line[SH];
    Pen sky[HORIZON + 1];

    void update(const Colours &c) {
        if(!geometry) {
            geometry = true;
            for(int y = HORIZON + 1; y < SH; y++) {
                float k = float(y - HORIZON);  // = FOCAL / z
                z[y] = FOCAL / k;
                half[y] = ROAD_HALF * k;
                kerb_w[y] = RUMBLE * k;
                line_w[y] = std::max(1, int(0.025f * k));
                float f = clampf((z[y] - 5.0f) / (Z_FAR - 5.0f), 0.0f, 1.0f);
                fog[y] = f * f;
            }
        }
        if(have_colours && memcmp(&c, &colours, sizeof(c)) == 0) return;
        have_colours = true;
        colours = c;
        for(int y = 0; y <= HORIZON; y++) sky[y] = mix(c.sky_top, c.sky_low, float(y) / float(HORIZON));
        for(int y = HORIZON + 1; y < SH; y++) {
            for(int alt = 0; alt < 2; alt++) {
                grass[alt][y] = mix(alt ? c.grass_a : c.grass_b, c.fog, fog[y]);
                kerb[alt][y] = mix(alt ? c.kerb_a : c.kerb_b, c.fog, fog[y]);
                road[alt][y] = mix(alt ? c.road_a : c.road_b, c.fog, fog[y]);
            }
            line[y] = mix(c.line, c.fog, fog[y]);
        }
    }
};
RoadRows rows;

void render_road() {
    for(int y = HORIZON + 1; y < SH; y++) {
        float k = float(y - HORIZON);
        float z = rows.z[y];
        float cx = 64.0f + view_dx + (run.curve * CURVE_K * z * z - run.cam_x) * k;
        float s = z + run.distance - Z_PLAYER;
        int alt = (int(floorf(s / SEGMENT)) & 1) != 0;

        // opaque spans are a fast copy, so drawing over the grass costs little
        screen.pen = rows.grass[alt][y];
        screen.h_span(Point(0, y), SW);
        float half = rows.half[y], kerb_w = rows.kerb_w[y];
        int l = int(cx - half - kerb_w);
        int r = int(cx + half + kerb_w);
        screen.pen = rows.kerb[alt][y];
        screen.h_span(Point(l, y), r - l + 1);
        l = int(cx - half);
        r = int(cx + half);
        screen.pen = rows.road[alt][y];
        screen.h_span(Point(l, y), r - l + 1);

        // dashed lane lines
        if(!alt) {
            int w = rows.line_w[y];
            screen.pen = rows.line[y];
            for(int side = -1; side <= 1; side += 2) {
                int lx = int(cx + float(side) * LANE_W * 0.5f * k) - w / 2;
                screen.h_span(Point(lx, y), w);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering: sprites

const Rect &obstacle_rect(Kind k, int biome, uint32_t time) {
    switch(k) {
        case K_LOW:   return biome == 1 ? spr::cart : biome == 2 ? spr::barrier : spr::fence;
        case K_HIGH:  return biome == 1 ? spr::banner_city : biome == 2 ? spr::banner_lava : spr::banner;
        case K_BLOCK: return biome == 1 ? spr::vending : biome == 2 ? spr::spire : spr::cactus;
        case K_WEED:  return spr::weed[(time / 90u) & 1u];
        case K_CHILI: return spr::chili[(time / 200u) & 1u];
        case K_GOLDEN: return spr::golden;
        case K_MILK: return spr::milk;
        case K_SOMBRERO: return spr::sombrero;
        default: break;
    }
    return spr::rock;
}

const Rect &decor_rect(int biome, int art) {
    static const Rect *desert[4] = {&spr::agave, &spr::rock, &spr::skull, &spr::sign};
    static const Rect *city[4] = {&spr::lamp, &spr::tower, &spr::lamp, &spr::sign};
    static const Rect *volcano[4] = {&spr::pillar, &spr::vent, &spr::pillar, &spr::sign};
    const Rect *const *set = biome == 1 ? city : biome == 2 ? volcano : desert;
    return *set[art & 3];
}

// Draw a sheet rect bottom-centre anchored at (x, y), scaled.
void draw_scaled(const Rect &src, float x, float y, float scale) {
    int w = int(float(src.w) * scale + 0.5f);
    int h = int(float(src.h) * scale + 0.5f);
    if(w < 1 || h < 1) return;
    Rect dst(int(x - float(w) * 0.5f), int(y) - h, w, h);
    if(w == src.w && h == src.h) {
        screen.blit(screen.sprites, src, Point(dst.x, dst.y));
    } else {
        screen.stretch_blit(screen.sprites, src, dst);
    }
}

void draw_shadow(float x, float y, float w) {
    int iw = int(w);
    if(iw < 2) return;
    screen.pen = Pen(0, 0, 0, 70);
    screen.h_span(Point(int(x) - iw / 2, int(y)), iw);
    screen.h_span(Point(int(x) - iw / 2 + 1, int(y) - 1), iw - 2);
}

struct DrawItem {
    float z;
    Obj *obj;
};

void render_objects(uint32_t time, bool before_player) {
    // far to near; objects behind the player's feet are drawn after it
    static DrawItem items[MAX_OBJS];
    int n = 0;
    for(auto &o : objs) {
        if(o.kind == K_NONE || o.collected) continue;
        float z = depth_of(o);
        if(z > Z_FAR + 1.0f || z < Z_NEAR) continue;
        bool behind = (o.s - run.distance) < -0.05f;
        if(behind == before_player) continue;
        items[n++] = {z, &o};
    }
    std::sort(items, items + n, [](const DrawItem &a, const DrawItem &b) { return a.z > b.z; });

    for(int i = 0; i < n; i++) {
        Obj &o = *items[i].obj;
        float z = items[i].z;
        float h = o.h;
        float xo = 0.0f;
        if(o.smashed) {
            // fly off the road, spinning out of view
            h += o.fly * 2.0f;
            xo = (o.x >= 0.0f ? 1.0f : -1.0f) * o.fly * 3.0f;
        }
        Screen p = project(o.x + xo, h, z);
        if(p.x < -40.0f || p.x > float(SW + 40)) continue;
        // fade in at the far end instead of popping up
        screen.alpha = uint8_t(255.0f * clampf((Z_FAR + 1.0f - z) / FADE_IN, 0.0f, 1.0f));

        if(o.kind == K_DECOR) {
            draw_scaled(decor_rect(o.biome, o.art), p.x, p.y, p.scale);
            continue;
        }
        Screen ground = project(o.x + xo, 0.0f, z);
        if(o.kind != K_HIGH) draw_shadow(ground.x, ground.y, float(obstacle_rect(o.kind, o.biome, time).w) * p.scale * 0.8f);

        float bob = 0.0f;
        if(is_pickup(o.kind)) bob = sinf(float(time) * 0.008f + o.s) * 1.5f * p.scale;
        draw_scaled(obstacle_rect(o.kind, o.biome, time), p.x, p.y - bob, p.scale);

        // power-ups glow so they read at a distance
        if((o.kind == K_MILK || o.kind == K_SOMBRERO || o.kind == K_GOLDEN) && p.scale > 0.25f && ((time / 120u) & 1u)) {
            const Rect &sp = spr::sparkle[(time / 240u) & 1u];
            draw_scaled(sp, p.x + 6.0f * p.scale, p.y - 10.0f * p.scale, p.scale);
        }
    }
    screen.alpha = 255;
}

void render_player(uint32_t time) {
    if(run.grace_t > 0.0f && ((time / 70u) & 1u) && state == State::RUN) return;

    float lift = jump_height() * FOCAL / Z_PLAYER;
    float px = 64.0f + view_dx + (run.p.x - run.cam_x) * FOCAL / Z_PLAYER;
    float py = float(PLAYER_FEET_Y) + view_dy;

    // shadow shrinks while in the air
    draw_shadow(px, py, 14.0f - lift * 0.15f);

    const Rect *r;
    if(state == State::CRASH || state == State::OVER) r = &spr::hurt;
    else if(dashing()) r = &spr::dash[(time / 80u) & 1u];
    else if(in_air()) {
        float t = run.p.jump_t / JUMP_TIME;
        r = t < 0.22f ? &spr::jump_up : (t < 0.62f ? &spr::jump : &spr::jump_down);
    }
    else if(sliding()) r = ((time / 80u) & 1u) ? &spr::slide2 : &spr::slide;
    else if(run.land_t > 0.0f) r = &spr::land;
    else r = &spr::run[int(run.run_anim) & 7];

    // lean into lane changes
    float lean = (lane_x(run.p.lane) - run.p.x) / LANE_W;
    int bob = 0;  // the run frames carry their own bob
    Point at(int(px - 8.0f + lean * 2.0f), int(py - lift) - 24 - bob);

    if(run.shield) {
        // milk bubble
        screen.pen = Pen(220, 240, 255, 70);
        screen.circle(Point(at.x + 8, at.y + 13), 13);
        screen.pen = Pen(255, 255, 255, 160);
        screen.pixel(Point(at.x + 3, at.y + 5));
        screen.pixel(Point(at.x + 4, at.y + 4));
    }
    screen.blit(screen.sprites, *r, at);
}

void render_particles() {
    for(auto &p : particles) {
        if(p.life <= 0.0f) continue;
        Pen pen = p.pen;
        pen.a = uint8_t(clampf(p.life / p.max_life, 0.0f, 1.0f) * 255.0f);
        screen.pen = pen;
        if(p.size <= 1) screen.pixel(Point(int(p.x), int(p.y)));
        else screen.rectangle(Rect(int(p.x), int(p.y), p.size, p.size));
    }
}

void render_speed_lines(uint32_t time) {
    float t = clampf((run.speed - 12.0f) / 10.0f, 0.0f, 1.0f);
    if(dashing()) t = 1.0f;
    if(t <= 0.0f) return;
    int count = int(t * 6.0f);
    for(int i = 0; i < count; i++) {
        uint32_t h = hash32(uint32_t(i) * 97u + (time / 50u));
        int side = (h & 1u) ? 1 : -1;
        int y = HORIZON + 10 + int((h >> 3) % 60u);
        int len = 6 + int((h >> 9) % 10u);
        int x = side > 0 ? SW - 2 - int((h >> 13) % 20u) - len : 2 + int((h >> 13) % 20u);
        screen.pen = Pen(255, 255, 255, 110);
        screen.h_span(Point(x, y), len);
    }
}

// ---------------------------------------------------------------------------
// Rendering: HUD and menus

void render_number(uint32_t value, Point p, TextAlign align, Pen pen, const Font &font = minimal_font) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)value);
    text_shadow(buf, font, p, pen, align);
}

void render_heat_bar(uint32_t time) {
    Rect bar(SW - 46, 3, 40, 5);
    screen.pen = Pen(20, 10, 24, 200);
    screen.rectangle(Rect(bar.x - 1, bar.y - 1, bar.w + 2, bar.h + 2));
    int w = int(float(bar.w) * (dashing() ? run.dash_t / DASH_TIME : run.heat));
    bool full = run.heat >= 1.0f && !dashing();
    Pen fill = full ? (((time / 120u) & 1u) ? Pen(255, 250, 120) : Pen(255, 140, 30))
                    : mix(Pen(255, 200, 40), Pen(255, 40, 20), run.heat);
    if(dashing()) fill = Pen(255, 120 + int((time / 40u) % 100u), 20);
    screen.pen = fill;
    screen.rectangle(Rect(bar.x, bar.y, w, bar.h));
    screen.blit(screen.sprites, spr::hud_flame, Point(bar.x - 10, bar.y - 2));
    if(full) {
        text_shadow("Y: FIRE!", minimal_font, Point(SW - 4, 11), Pen(255, 240, 140), TextAlign::top_right);
    }
}

void render_hud(uint32_t time) {
    // score top left, chilis below
    render_number(run.score, Point(3, 2), TextAlign::top_left, Pen(255, 255, 255));
    screen.blit(screen.sprites, spr::hud_chili, Point(2, 11));
    render_number(run.chilis, Point(12, 12), TextAlign::top_left, Pen(255, 220, 120));

    // multiplier
    int m = multiplier();
    if(m > 1) {
        char buf[8];
        snprintf(buf, sizeof(buf), "X%d", m);
        Pen pen = m >= base_multiplier() + 4 ? Pen(255, 120, 60) : Pen(255, 240, 160);
        text_shadow(buf, minimal_font, Point(3, 21), pen);
    }
    render_heat_bar(time);

    // active power-ups, bottom left
    int x = 3;
    if(run.shield) {
        screen.blit(screen.sprites, spr::hud_shield, Point(x, SH - 11));
        x += 10;
    }
    if(run.magnet_t > 0.0f) {
        if(run.magnet_t > 2.0f || ((time / 100u) & 1u)) screen.blit(screen.sprites, spr::hud_magnet, Point(x, SH - 11));
        screen.pen = Pen(255, 210, 120);
        screen.rectangle(Rect(x, SH - 2, int(8.0f * run.magnet_t / MAGNET_TIME) + 1, 1));
        x += 10;
    }

    // distance bottom right
    char buf[16];
    snprintf(buf, sizeof(buf), "%luM", (unsigned long)run_metres());
    text_shadow(buf, minimal_font, Point(SW - 3, SH - 10), Pen(230, 230, 240), TextAlign::top_right);

    if(run.biome_banner > 0.0f && ((time / 150u) & 1u || run.biome_banner < 1.5f)) {
        text_shadow(biomes[run.last_biome].name, outline_font, Point(64, 26), Pen(255, 255, 255), TextAlign::center_center);
    }
}

void render_popups() {
    for(auto &p : popups) {
        if(p.life <= 0.0f) continue;
        Pen pen = p.pen;
        text_shadow(p.text, minimal_font, Point(int(p.x), int(p.y)), pen, TextAlign::center_center);
    }
}

void dim(uint8_t a) {
    screen.pen = Pen(10, 5, 20, a);
    screen.rectangle(Rect(0, 0, SW, SH));
}

void render_menu(const char *const *items, int count, int y, int sel, uint32_t time) {
    for(int i = 0; i < count; i++) {
        bool on = i == sel;
        Pen pen = on ? Pen(255, 230, 90) : Pen(220, 210, 230);
        text_shadow(items[i], minimal_font, Point(64, y + i * 11), pen, TextAlign::top_center);
        if(on) {
            int w = screen.measure_text(items[i], minimal_font).w;
            int bounce = int((time / 150u) & 1u);
            screen.blit(screen.sprites, spr::hud_chili, Point(64 - w / 2 - 11 - bounce, y + i * 11 - 1));
        }
    }
}

Surface *logo = nullptr;  // assets/logo.png, 112x34

// Big blocky glyphs for the countdown: 3x5 cells drawn as squares of
// `scale` pixels with a dark outline, so "3, 2, 1, GO!" reads at a glance.
const uint16_t *big_glyph(char c) {
    static const uint16_t digits[10] = {
        0b111101101101111, 0b010110010010111, 0b111001111100111, 0b111001111001111, 0b101101111001001,
        0b111100111001111, 0b111100111101111, 0b111001010010010, 0b111101111101111, 0b111101111001111};
    static const uint16_t g = 0b111100101101111, o = 0b111101101101111, bang = 0b010010010000010;
    if(c >= '0' && c <= '9') return &digits[c - '0'];
    if(c == 'G') return &g;
    if(c == 'O') return &o;
    if(c == '!') return &bang;
    return nullptr;
}

void big_text(const char *str, int cx, int cy, int scale, Pen pen) {
    int n = int(strlen(str));
    int w = n * 3 * scale + (n - 1) * scale;
    int x0 = cx - w / 2, y0 = cy - 5 * scale / 2;
    for(int pass = 0; pass < 3; pass++) {
        // 0: drop shadow, 1: outline, 2: fill with a lighter top row
        for(int i = 0; i < n; i++) {
            const uint16_t *bits = big_glyph(str[i]);
            if(!bits) continue;
            for(int row = 0; row < 5; row++) {
                for(int col = 0; col < 3; col++) {
                    if(!((*bits >> (14 - row * 3 - col)) & 1)) continue;
                    Rect r(x0 + i * 4 * scale + col * scale, y0 + row * scale, scale, scale);
                    if(pass == 0) { screen.pen = Pen(60, 10, 20, 200); r.x += 2; r.y += 3; }
                    else if(pass == 1) { screen.pen = Pen(26, 15, 31); r.x -= 2; r.y -= 2; r.w += 4; r.h += 4; }
                    else screen.pen = row == 0 ? mix(pen, Pen(255, 255, 255), 0.5f) : pen;
                    screen.rectangle(r);
                }
            }
        }
    }
}

// Each number pops in large and settles, t = seconds since it appeared.
int pop_scale(float t) { return t < 0.06f ? 7 : (t < 0.12f ? 6 : 5); }

void render_logo(int y, uint32_t time) {
    if(logo) {
        int bob = int(sinf(float(time) * 0.004f) * 2.0f);
        screen.blit(logo, Rect(0, 0, logo->bounds.w, logo->bounds.h), Point(64 - logo->bounds.w / 2, y + bob));
        return;
    }
    // fallback: "CHILI RUSH" with a wobble, letters coloured red to yellow
    const char *word = "CHILI RUSH";
    int total = screen.measure_text(word, outline_font).w;
    int x = 64 - total / 2;
    for(int i = 0; word[i]; i++) {
        char ch[2] = {word[i], 0};
        int wob = int(sinf(float(time) * 0.006f + float(i) * 0.7f) * 2.0f);
        Pen pen = mix(Pen(255, 60, 40), Pen(255, 220, 60), float(i) / 9.0f);
        text_shadow(ch, outline_font, Point(x, y + wob), pen);
        x += screen.measure_text(ch, outline_font).w;
    }
}

const char *title_items[] = {"PLAY", "MISSIONS", "HOW TO PLAY", "SETTINGS"};
const char *pause_items[] = {"RESUME", "RESTART", "QUIT TO TITLE"};

void render_title(uint32_t time) {
    dim(110);
    render_logo(2, time);
    render_menu(title_items, 4, 58, menu_cursor, time);
    char buf[32];
    snprintf(buf, sizeof(buf), "BEST %lu", (unsigned long)save.best);
    text_shadow(buf, minimal_font, Point(64, 39), Pen(255, 240, 160), TextAlign::top_center);
    // rank stars
    int stars = rank();
    if(stars > 0) {
        int shown = std::min(stars, 10);
        int x0 = 64 - shown * 5;
        for(int i = 0; i < shown; i++) screen.blit(screen.sprites, spr::hud_star, Point(x0 + i * 10, 48));
        if(stars > 10) {
            snprintf(buf, sizeof(buf), "+%d", stars - 10);
            text_shadow(buf, minimal_font, Point(x0 + shown * 10 + 1, 48), Pen(255, 240, 160));
        }
    }
    if((time / 500u) & 1u) text_shadow("A: SELECT", minimal_font, Point(64, SH - 10), Pen(200, 190, 210), TextAlign::top_center);
}

void render_missions(uint32_t time) {
    dim(170);
    text_shadow("MISSIONS", outline_font, Point(64, 6), Pen(255, 220, 90), TextAlign::top_center);
    char buf[32];
    for(int i = 0; i < MISSION_SLOTS; i++) {
        const Mission &m = save.missions[i];
        int y = 26 + i * 22;
        screen.pen = Pen(255, 255, 255, 30);
        screen.rectangle(Rect(6, y - 2, SW - 12, 19));
        text_shadow(mission_text(m, buf, sizeof(buf)), minimal_font, Point(10, y), Pen(255, 255, 255));
        snprintf(buf, sizeof(buf), "LEVEL %d", int(m.level) + 1);
        text_shadow(buf, minimal_font, Point(10, y + 8), Pen(170, 160, 190));
    }
    int stars = rank();
    snprintf(buf, sizeof(buf), "DONE %lu  RANK %d", (unsigned long)save.missions_done, stars);
    text_shadow(buf, minimal_font, Point(64, 94), Pen(255, 240, 160), TextAlign::top_center);
    snprintf(buf, sizeof(buf), "SCORE BONUS X%d", base_multiplier());
    text_shadow(buf, minimal_font, Point(64, 103), Pen(200, 190, 210), TextAlign::top_center);
    text_shadow("3 MISSIONS = 1 STAR", minimal_font, Point(64, 112), Pen(150, 140, 170), TextAlign::top_center);
    (void)time;
}

void render_help(uint32_t time) {
    dim(190);
    text_shadow("HOW TO PLAY", outline_font, Point(64, 4), Pen(255, 220, 90), TextAlign::top_center);
    const char *lines[] = {
        "LEFT/RIGHT: CHANGE LANE",
        "A or UP: JUMP",
        "B or DOWN: SLIDE",
        "Y: FIRE DASH (HEAT FULL)",
        "",
        "JUMP LOW THINGS,",
        "SLIDE UNDER SIGNS,",
        "DODGE THE TALL ONES.",
        "CHILIS FILL THE HEAT BAR.",
        "MILK = SHIELD, HAT = MAGNET",
    };
    for(int i = 0; i < 10; i++) text_shadow(lines[i], minimal_font, Point(64, 22 + i * 9), Pen(235, 230, 240), TextAlign::top_center);
    if((time / 500u) & 1u) text_shadow("A: OK", minimal_font, Point(64, SH - 10), Pen(200, 190, 210), TextAlign::top_center);
}

void render_settings(uint32_t time) {
    dim(170);
    text_shadow("SETTINGS", outline_font, Point(64, 10), Pen(255, 220, 90), TextAlign::top_center);
    const char *items[3] = {save.music ? "MUSIC: ON" : "MUSIC: OFF", save.sound ? "SOUND: ON" : "SOUND: OFF", "BACK"};
    render_menu(items, 3, 44, menu_cursor, time);
    char buf[40];
    snprintf(buf, sizeof(buf), "RUNS %lu", (unsigned long)save.runs);
    text_shadow(buf, minimal_font, Point(64, 88), Pen(170, 160, 190), TextAlign::top_center);
    snprintf(buf, sizeof(buf), "CHILIS %lu", (unsigned long)save.total_chilis);
    text_shadow(buf, minimal_font, Point(64, 97), Pen(170, 160, 190), TextAlign::top_center);
    snprintf(buf, sizeof(buf), "FURTHEST %luM", (unsigned long)save.best_distance);
    text_shadow(buf, minimal_font, Point(64, 106), Pen(170, 160, 190), TextAlign::top_center);
}

void render_resume() {
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", 3 - std::min(2, state_ticks / 30));
    big_text(buf, 64, 62, pop_scale(float(state_ticks % 30) * 0.01f), Pen(255, 255, 255));
}

void render_ready(uint32_t time) {
    int n = 3 - state_ticks / 40;
    char buf[16];
    if(n > 0) snprintf(buf, sizeof(buf), "%d", n);
    else snprintf(buf, sizeof(buf), "GO!");
    Pen pen = n > 0 ? mix(Pen(255, 255, 255), Pen(255, 200, 60), float(3 - n) / 2.0f) : Pen(255, 220, 60);
    big_text(buf, 64, 62, pop_scale(float(state_ticks % 40) * 0.01f), pen);
    (void)time;
}

void render_over(uint32_t time) {
    dim(150);
    text_shadow(run.new_best ? "NEW BEST!" : "RUN OVER", outline_font, Point(64, 6), run.new_best ? Pen(255, 230, 80) : Pen(255, 120, 90), TextAlign::top_center);
    char buf[40];
    // score counts up for the first second
    float t = clampf(state_time / 0.8f, 0.0f, 1.0f);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)(uint32_t(float(run.score) * t)));
    text_shadow(buf, outline_font, Point(64, 24), Pen(255, 255, 255), TextAlign::top_center);
    snprintf(buf, sizeof(buf), "%luM   %lu CHILIS", (unsigned long)run_metres(), (unsigned long)run.chilis);
    text_shadow(buf, minimal_font, Point(64, 40), Pen(255, 220, 120), TextAlign::top_center);
    snprintf(buf, sizeof(buf), "BEST %lu", (unsigned long)save.best);
    text_shadow(buf, minimal_font, Point(64, 49), Pen(200, 190, 210), TextAlign::top_center);

    for(int i = 0; i < MISSION_SLOTS; i++) {
        const Mission &m = save.missions[i];
        int y = 62 + i * 13;
        bool ok = mission_complete[i];
        uint32_t v = std::min(mission_value(m.type), m.target);
        // the box fills up with the progress; the target is in the text
        screen.pen = Pen(255, 255, 255, 25);
        screen.rectangle(Rect(4, y - 2, SW - 8, 12));
        int fill = ok ? SW - 8 : int(float(SW - 8) * float(v) / float(m.target));
        screen.pen = ok ? Pen(80, 200, 100, 90) : Pen(255, 190, 80, 55);
        screen.rectangle(Rect(4, y - 2, fill, 12));
        char mt[28];
        mission_text(m, mt, sizeof(mt));
        text_shadow(mt, minimal_font, Point(7, y), ok ? Pen(150, 255, 160) : Pen(230, 230, 240));
        if(ok) snprintf(buf, sizeof(buf), "OK");
        else snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);
        text_shadow(buf, minimal_font, Point(SW - 7, y), ok ? Pen(150, 255, 160) : Pen(170, 160, 190), TextAlign::top_right);
    }
    if(state_time > 0.6f) {
        text_shadow("A: RUN AGAIN", minimal_font, Point(64, SH - 19), ((time / 400u) & 1u) ? Pen(255, 240, 120) : Pen(255, 255, 255), TextAlign::top_center);
        text_shadow("B: TITLE", minimal_font, Point(64, SH - 10), Pen(200, 190, 210), TextAlign::top_center);
    }
}

void render_pause(uint32_t time) {
    dim(150);
    text_shadow("PAUSED", outline_font, Point(64, 16), Pen(255, 220, 90), TextAlign::top_center);
    render_menu(pause_items, 3, 50, menu_cursor, time);
    char buf[32];
    for(int i = 0; i < MISSION_SLOTS; i++) {
        const Mission &m = save.missions[i];
        mission_text(m, buf, sizeof(buf));
        bool ok = mission_complete[i];
        text_shadow(buf, minimal_font, Point(64, 92 + i * 9), ok ? Pen(150, 255, 160) : Pen(170, 160, 190), TextAlign::top_center);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Engine entry points

// The console is quitting the game (HOME held): keep the run that was going on.
void on_quit_save() {
    if(state == State::RUN || state == State::PAUSE || state == State::RESUME) record_run();
    if(!attract) finish_missions();
    store_game();
}

void init() {
    set_screen_mode(ScreenMode::hires);
    screen.sprites = Surface::load(asset_sprites);
    logo = Surface::load(asset_logo);
    load_game();
    audio::init();
    audio::set_music(save.music);
    audio::set_sound(save.sound);
    blit::on_quit = on_quit_save;
    start_run(true);
    set_state(State::TITLE);
    audio::play(audio::Track::TITLE);
}

void render(uint32_t time) {
    uint32_t frame_start = now_us();
    screen.alpha = 255;
    screen.mask = nullptr;
    screen.clip = Rect(0, 0, SW, SH);

    float shake = run.shake;
    Colours c = colours_now(run.distance);
    int biome = biome_at(run.distance);
    // near the end of a biome draw the next one's scenery behind the fade
    if(in_biome(run.distance) > BIOME_LENGTH - BIOME_BLEND * 0.5f) biome = (biome + 1) % 3;

    // screen shake moves the road, sprites and player, not the HUD
    view_dx = view_dy = 0.0f;
    if(shake > 0.0f) {
        uint32_t h = hash32(time);
        view_dx = (float(h & 0xff) / 255.0f - 0.5f) * shake * 14.0f;
        view_dy = (float((h >> 8) & 0xff) / 255.0f - 0.5f) * shake * 8.0f;
    }

#ifdef CHILI_SELFTEST
#define PROFILE(n) st_profile(n)
#else
#define PROFILE(n)
#endif
    PROFILE(0);
    rows.update(c);
    render_sky(c, rows.sky, biome, time);
    PROFILE(1);
    render_road();
    PROFILE(2);
    render_objects(time, true);
    render_player(time);
    render_objects(time, false);
    PROFILE(3);
    render_particles();
    if(state == State::RUN || (attract && state == State::TITLE)) render_speed_lines(time);

    if(run.flash > 0.0f) {
        Pen p = run.flash_pen;
        p.a = uint8_t(clampf(run.flash * 4.0f, 0.0f, 1.0f) * 160.0f);
        screen.pen = p;
        screen.rectangle(Rect(0, 0, SW, SH));
    }

    switch(state) {
        case State::TITLE: render_title(time); break;
        case State::MISSIONS: render_missions(time); break;
        case State::HELP: render_help(time); break;
        case State::SETTINGS: render_settings(time); break;
        case State::READY: render_hud(time); render_ready(time); break;
        case State::RUN:
            render_hud(time);
            render_popups();
            if(run_ticks_since_go < 35) big_text("GO!", 64, 62, pop_scale(float(run_ticks_since_go) * 0.01f), Pen(255, 220, 60));
            break;
        case State::PAUSE: render_hud(time); render_pause(time); break;
        case State::RESUME: render_hud(time); render_resume(); break;
        case State::CRASH: render_hud(time); render_popups(); break;
        case State::OVER: render_over(time); break;
    }

    if(show_fps) {
        fps_frames++;
        if(time - fps_last >= 1000u) {
            fps_value = fps_frames;
            fps_frames = 0;
            fps_last = time;
        }
        frame_us = us_diff(frame_start, now_us());
        char buf[48];
        snprintf(buf, sizeof(buf), "%lu FPS %luMS", (unsigned long)fps_value, (unsigned long)(frame_us / 1000u));
        screen.pen = Pen(0, 0, 0, 180);
        screen.rectangle(Rect(0, SH - 18, 64, 8));
        screen.pen = Pen(120, 255, 120);
        screen.text(buf, minimal_font, Point(1, SH - 18));
    }
    PROFILE(4);
}

namespace {

bool menu_nav(int count) {
    if(buttons.pressed & Button::DPAD_UP) {
        menu_cursor = (menu_cursor + count - 1) % count;
        audio::sfx(audio::Sfx::MENU_MOVE);
    }
    if(buttons.pressed & Button::DPAD_DOWN) {
        menu_cursor = (menu_cursor + 1) % count;
        audio::sfx(audio::Sfx::MENU_MOVE);
    }
    return (buttons.pressed & Button::A) != 0;
}

void begin_run() {
    start_run(false);
    set_state(State::READY);
    audio::play(audio::Track::NONE);
    audio::sfx(audio::Sfx::COUNTDOWN);
}

void resume_run() {
    set_state(State::RESUME);
    audio::sfx(audio::Sfx::COUNTDOWN);
}

void to_title() {
    start_run(true);
    set_state(State::TITLE);
    menu_cursor = 0;
    audio::play(audio::Track::TITLE);
}

} // namespace

void update(uint32_t time) {
    audio::update(time);
    state_time += DT;
    state_ticks++;
    if(run_ticks_since_go < 1000) run_ticks_since_go++;

    // After the console's system menu (HOME) or any long stall the engine
    // runs every missed update at once, with old timestamps. Pause rather
    // than play seconds of the run blind.
    if(now() - time > 250u && (state == State::RUN || state == State::READY || state == State::RESUME)) {
        set_state(State::PAUSE);
        menu_cursor = 0;
    }

    switch(state) {
        case State::TITLE:
            update_run(false);
            if(buttons.pressed & Button::X) show_fps = !show_fps;
            if(menu_nav(4)) {
                audio::sfx(audio::Sfx::MENU_OK);
                switch(menu_cursor) {
                    case 0:
                        if(!save.seen_help) {
                            save.seen_help = 1;
                            store_game();
                            set_state(State::HELP);
                        } else {
                            begin_run();
                        }
                        break;
                    case 1: set_state(State::MISSIONS); break;
                    case 2: set_state(State::HELP); break;
                    case 3: set_state(State::SETTINGS); menu_cursor = 0; break;
                }
            }
            break;

        case State::MISSIONS:
            update_run(false);
            if(buttons.pressed & (Button::A | Button::B)) {
                audio::sfx(audio::Sfx::MENU_OK);
                set_state(State::TITLE);
                menu_cursor = 1;
            }
            break;

        case State::HELP:
            update_run(false);
            if(state_time > 0.3f && (buttons.pressed & (Button::A | Button::B))) {
                audio::sfx(audio::Sfx::MENU_OK);
                // first time: the help screen leads into the run
                if(menu_cursor == 0) begin_run();
                else set_state(State::TITLE);
            }
            break;

        case State::SETTINGS: {
            update_run(false);
            bool ok = menu_nav(3);
            bool flip = ok || (buttons.pressed & (Button::DPAD_LEFT | Button::DPAD_RIGHT));
            if(flip && menu_cursor == 0) {
                save.music = !save.music;
                audio::set_music(save.music);
                if(save.music) audio::play(audio::Track::TITLE);
                store_game();
                audio::sfx(audio::Sfx::MENU_OK);
            } else if(flip && menu_cursor == 1) {
                save.sound = !save.sound;
                audio::set_sound(save.sound);
                store_game();
                audio::sfx(audio::Sfx::MENU_OK);
            } else if((ok && menu_cursor == 2) || (buttons.pressed & Button::B)) {
                audio::sfx(audio::Sfx::MENU_OK);
                set_state(State::TITLE);
                menu_cursor = 3;
            }
            break;
        }

        case State::READY: {
            // 3 (beep at the start), 2, 1, GO: 0.4 s each, counted in updates
            if(state_ticks == 40 || state_ticks == 80) audio::sfx(audio::Sfx::COUNTDOWN);
            // lanes can be chosen during the countdown
            if(buttons.pressed & Button::DPAD_LEFT) change_lane(-1);
            if(buttons.pressed & Button::DPAD_RIGHT) change_lane(1);
            update_player();
            if(state_ticks >= 120) {
                audio::sfx(audio::Sfx::GO);
                set_state(State::RUN);
                audio::play(audio::Track::RUN);
                run_ticks_since_go = 0;
            }
            break;
        }

        case State::RESUME:
            // 3, 2, 1 after the pause menu with the world held still, so a
            // resume never drops the player straight into an obstacle
            if(buttons.pressed & (Button::MENU | Button::HOME)) {
                set_state(State::PAUSE);
                menu_cursor = 0;
                break;
            }
            if(state_ticks == 30 || state_ticks == 60) audio::sfx(audio::Sfx::COUNTDOWN);
            if(state_ticks >= 90) {
                audio::sfx(audio::Sfx::GO);
                set_state(State::RUN);
            }
            break;

        case State::RUN:
            if(buttons.pressed & (Button::MENU | Button::HOME)) {
                set_state(State::PAUSE);
                menu_cursor = 0;
                audio::sfx(audio::Sfx::MENU_OK);
                break;
            }
            update_run(true);
            break;

        case State::PAUSE:
            if(buttons.pressed & (Button::MENU | Button::B)) {
                resume_run();
                break;
            }
            if(menu_nav(3)) {
                audio::sfx(audio::Sfx::MENU_OK);
                if(menu_cursor == 0) resume_run();
                else {
                    // leaving a run counts like a crash: totals, best, missions
                    record_run();
                    finish_run_save();
                    if(menu_cursor == 1) begin_run();
                    else to_title();
                }
            }
            break;

        case State::CRASH:
            update_player();
            if(state_time > 0.9f) {
                set_state(State::OVER);
                audio::play(audio::Track::GAME_OVER);
            }
            break;

        case State::OVER:
            if(state_time > 0.6f) {
                if(buttons.pressed & Button::A) {
                    finish_run_save();
                    audio::sfx(audio::Sfx::MENU_OK);
                    begin_run();
                } else if(buttons.pressed & Button::B) {
                    finish_run_save();
                    audio::sfx(audio::Sfx::MENU_OK);
                    to_title();
                }
            }
            break;
    }

    update_effects();
    (void)time;
}

#ifdef CHILI_SELFTEST
#include "selftest.hpp"
#endif
