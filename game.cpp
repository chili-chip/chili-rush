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
constexpr float FOCAL = 100.0f;     // projection: row offset = FOCAL / z
constexpr float LANE_W = 0.45f;     // world width of a lane
constexpr float ROAD_HALF = 0.72f;  // half the asphalt width
constexpr float RUMBLE = 0.07f;     // kerb width
constexpr int PLAYER_FEET_Y = 120;  // screen row of the player's feet
constexpr float Z_PLAYER = FOCAL / float(PLAYER_FEET_Y - HORIZON);
constexpr float Z_FAR = 34.0f;      // objects appear here
constexpr float SEGMENT = 0.5f;     // stripe length on the road
constexpr float DT = 0.01f;         // seconds per update()

constexpr float SPEED_START = 7.5f;   // world units per second
constexpr float SPEED_MAX = 19.0f;
constexpr float SPEED_RAMP = 2600.0f; // distance to get ~63% of the way to max

constexpr float JUMP_TIME = 0.62f;    // seconds in the air
constexpr float JUMP_HEIGHT = 0.36f;  // world units
constexpr float SLIDE_TIME = 0.62f;
constexpr float LANE_TIME = 0.11f;    // seconds to move one lane

constexpr float DASH_TIME = 4.0f;
constexpr float DASH_SPEED = 1.45f;   // speed multiplier during a dash
constexpr float GRACE_TIME = 1.2f;    // blink after a shield pop or a dash
constexpr float MAGNET_TIME = 10.0f;

constexpr float BIOME_LENGTH = 900.0f; // distance per biome
constexpr float BIOME_BLEND = 60.0f;   // colours fade over this distance

constexpr float METRES_PER_UNIT = 1.0f;

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

int biome_at(float distance) { return int(distance / BIOME_LENGTH) % 3; }

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

bool save_valid() {
    if(save.magic != SAVE_MAGIC || save.version != SAVE_VERSION) return false;
    for(auto &m : save.missions)
        if(m.type >= M_TYPES || m.target == 0) return false;
    return true;
}

void load_game() {
    if(!read_save(save) || !save_valid()) reset_save();
    save.music = save.music ? 1 : 0;
    save.sound = save.sound ? 1 : 0;
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

Obj *spawn(Kind k, float x, float s, float h = 0.0f) {
    for(auto &o : objs) {
        if(o.kind == K_NONE) {
            o = Obj{};
            o.kind = k;
            o.x = x;
            o.s = s;
            o.h = h;
            o.biome = uint8_t(biome_at(s));
            return &o;
        }
    }
    return nullptr; // pool full: skip (generation keeps well under the limit)
}

float lane_x(int lane) { return float(lane - 1) * LANE_W; }

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
    snprintf(slot->text, sizeof(slot->text), "%s", text);
    slot->x = x;
    slot->y = y;
    slot->life = 0.9f;
    slot->pen = pen;
}

// ---------------------------------------------------------------------------
// Game state

enum class State { TITLE, MISSIONS, SETTINGS, HELP, READY, RUN, PAUSE, CRASH, OVER };
State state = State::TITLE;
float state_time = 0.0f;

void set_state(State s) {
    state = s;
    state_time = 0.0f;
}

struct Run {
    float distance;      // the player's position along the road
    float speed;
    float cam_x;
    // player
    int lane;
    float x;              // lateral position, eases to the lane
    float jump_t;         // >0 while in the air, counts up to JUMP_TIME
    float slide_t;        // >0 while sliding
    float lane_change_t;  // time since last lane change (close calls)
    float jump_buffer;    // jump pressed just before landing
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
};
Run run;

uint32_t best_at_start = 0;
float view_dx = 0.0f, view_dy = 0.0f; // screen shake offset
int menu_cursor = 0;
bool attract = true; // the title screen road runs by itself

// Frame rate counter, toggled with X on the title screen (for testing on the console).
bool show_fps = false;
uint32_t fps_frames = 0, fps_last = 0, fps_value = 0, frame_us = 0;

// ---------------------------------------------------------------------------
// Missions progress

uint32_t mission_value(uint8_t type) {
    switch(type) {
        case M_CHILIS:   return run.chilis;
        case M_DISTANCE: return uint32_t(run.distance * METRES_PER_UNIT);
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
            audio::sfx(audio::Sfx::MISSION);
            popup("MISSION!", 64, 40, Pen(120, 255, 140));
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
float difficulty() { return clampf(run.distance / 4000.0f, 0.0f, 1.0f); }

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

    int pattern = rnd_int(100);
    if(run.distance < 120.0f) {
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
        // a tumbleweed rolling across, plus maybe one block
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
    if(nf > 0 && chance(0.75f)) {
        int l = free_lanes[rnd_int(nf)];
        // stays clear of the next row, which is at least min_gap() * 1.1 away
        int most = int(min_gap() * 1.4f / 0.7f);
        chili_line(l, s - min_gap() * 0.45f, std::min(4 + rnd_int(4), most), 0.7f);
    } else {
        for(int l = 0; l < 3; l++)
            if(lane_kind[l] == K_LOW && chance(0.7f)) { chili_arc(l, s); break; }
    }

    // golden chili now and then, in a free lane right after the row
    if(nf > 0 && chance(0.04f + 0.04f * d)) spawn(K_GOLDEN, lane_x(free_lanes[rnd_int(nf)]), s + 1.5f);
}

void spawn_power(float s) {
    Kind k = chance(0.5f) ? K_MILK : K_SOMBRERO;
    spawn(k, lane_x(rnd_int(3)), s, 0.05f);
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
        // free space before a power-up so it is not inside a row
        if(run.next_power < run.next_spawn) {
            spawn_power(run.next_power);
            run.next_power += 220.0f + rnd_float() * 180.0f;
        }
        spawn_row(run.next_spawn);
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
    for(auto &o : objs) o.kind = K_NONE;
    for(auto &p : particles) p.life = 0.0f;
    for(auto &p : popups) p.life = 0.0f;
    for(auto &m : mission_complete) m = false;
    rng_state = (now() * 2654435761u) ^ 0xa5a5a5a5u;
    if(!rng_state) rng_state = 1;
    run.lane = 1;
    run.x = 0.0f;
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

bool in_air() { return run.jump_t > 0.0f; }
bool sliding() { return run.slide_t > 0.0f; }
bool dashing() { return run.dash_t > 0.0f; }

float jump_height() {
    if(!in_air()) return 0.0f;
    float t = run.jump_t / JUMP_TIME;  // 0..1
    return JUMP_HEIGHT * 4.0f * t * (1.0f - t);
}

void do_jump() {
    if(in_air()) {
        run.jump_buffer = 0.12f;
        return;
    }
    run.slide_t = 0.0f;
    run.jump_t = 0.0001f;
    audio::sfx(audio::Sfx::JUMP);
}

void do_slide() {
    if(in_air()) {
        // drop out of the air fast and slide on landing
        run.jump_t = std::max(run.jump_t, JUMP_TIME * 0.82f);
        run.slide_t = 0.0f;
        run.jump_buffer = -0.2f;  // negative: slide queued
        return;
    }
    if(!sliding()) audio::sfx(audio::Sfx::SLIDE);
    run.slide_t = 0.0001f;
}

void change_lane(int dir) {
    int lane = std::clamp(run.lane + dir, 0, 2);
    if(lane == run.lane) {
        // bumping the kerb: small shake, no penalty
        run.shake = std::max(run.shake, 0.06f);
        return;
    }
    run.lane = lane;
    run.lane_change_t = 0.0f;
    audio::sfx(audio::Sfx::LANE);
}

void start_dash() {
    if(run.heat < 1.0f || dashing()) return;
    run.heat = 0.0f;
    run.dash_t = DASH_TIME;
    run.dashes++;
    run.flash = 0.25f;
    run.flash_pen = Pen(255, 160, 40);
    audio::sfx(audio::Sfx::DASH_START);
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
    float sx = 64.0f + view_dx + (x - run.cam_x + run.curve * z * z) * k;
    float sy = float(HORIZON) + view_dy + (1.0f - h) * k;
    return {sx, sy, Z_PLAYER / z};
}

float depth_of(const Obj &o) { return o.s - run.distance + Z_PLAYER; }

// Player hitbox along the road, in world units either side of the feet.
constexpr float HIT_DEPTH = 0.22f;

// ---------------------------------------------------------------------------
// Crash

void crash() {
    audio::sfx(audio::Sfx::HIT);
    audio::play(audio::Track::NONE);
    run.shake = 0.5f;
    run.flash = 0.2f;
    run.flash_pen = Pen(255, 255, 255);
    burst(64, float(PLAYER_FEET_Y - 10), 24, Pen(255, 80, 60), 70.0f, 2);
    set_state(State::CRASH);

    // results and saving happen once, here
    save.runs++;
    save.total_chilis += run.chilis;
    uint32_t metres = uint32_t(run.distance * METRES_PER_UNIT);
    if(metres > save.best_distance) save.best_distance = metres;
    if(run.score > save.best) {
        save.best = run.score;
        run.new_best = best_at_start > 0;  // the first run is always a best, no fanfare
    }
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
            audio::sfx_combo(std::min(10, 1 + run.combo / 3));
            burst(p.x, p.y - 6, 5, Pen(255, 220, 80), 30.0f);
            if(run.combo > 0 && run.combo % 10 == 0) {
                char buf[16];
                snprintf(buf, sizeof(buf), "X%d!", multiplier());
                popup(buf, 64, 60, Pen(255, 240, 120));
            }
            break;
        }
        case K_GOLDEN:
            run.heat = 1.0f;
            add_score(uint32_t(100 * multiplier()));
            audio::sfx(audio::Sfx::GOLDEN);
            burst(p.x, p.y - 6, 14, Pen(255, 230, 60), 50.0f, 2);
            popup("HEAT FULL!", 64, 52, Pen(255, 200, 60));
            break;
        case K_MILK:
            run.shield = true;
            audio::sfx(audio::Sfx::POWERUP);
            burst(p.x, p.y - 6, 12, Pen(240, 250, 255), 40.0f, 2);
            popup("SHIELD", 64, 52, Pen(220, 240, 255));
            break;
        case K_SOMBRERO:
            run.magnet_t = MAGNET_TIME;
            audio::sfx(audio::Sfx::POWERUP);
            burst(p.x, p.y - 6, 12, Pen(255, 200, 80), 40.0f, 2);
            popup("MAGNET", 64, 52, Pen(255, 210, 120));
            break;
        default: break;
    }
}

// Is the player clear of this obstacle right now?
bool avoids(const Obj &o) {
    switch(o.kind) {
        case K_LOW:
        case K_WEED: return jump_height() > 0.12f;
        case K_HIGH: return sliding();
        default: return false;
    }
}

void obstacle_hit(Obj &o) {
    if(attract) {
        o.done = true;  // the title screen demo never crashes
        return;
    }
    if(dashing()) {
        o.smashed = true;
        o.done = true;
        run.smashed++;
        add_score(uint32_t(50 * multiplier()));
        audio::sfx(audio::Sfx::SMASH);
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
        audio::sfx(audio::Sfx::SHIELD_POP);
        run.shake = 0.25f;
        run.flash = 0.15f;
        run.flash_pen = Pen(230, 240, 255);
        popup("SAVED!", 64, 52, Pen(220, 240, 255));
        return;
    }
    crash();
}

void update_objects() {
    float px = run.x;
    for(auto &o : objs) {
        if(o.kind == K_NONE) continue;
        float z = depth_of(o);

        if(o.smashed) o.fly += DT;
        if(z < 0.2f) {
            o.kind = K_NONE;  // behind the camera
            continue;
        }

        if(o.kind == K_WEED && !o.done) {
            // starts rolling once it is close enough to be seen
            if(z < 16.0f) {
                o.x += o.vx * DT;
                if((o.vx > 0.0f && o.x > o.target_x) || (o.vx < 0.0f && o.x < o.target_x)) {
                    o.x = o.target_x;
                    o.vx = -o.vx;
                    o.target_x = lane_x(2) - o.target_x + lane_x(0);  // bounce back to the other side
                }
            }
        }

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
                if(!avoids(o)) {
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
                } else if(fabsf(o.x - px) < LANE_W * 1.3f && run.lane_change_t < 0.3f && !dashing()) {
                    // dodged into the next lane at the last moment
                    run.close_calls++;
                    add_score(uint32_t(25 * multiplier()));
                    audio::sfx(audio::Sfx::NEAR_MISS);
                    popup("CLOSE!", 64, 70, Pen(120, 230, 255));
                    run.heat = std::min(1.0f, run.heat + 0.06f);
                }
            }
        }
    }
}

void update_player() {
    // ease towards the lane
    // ease out: fast start, soft arrival; a minimum step so it always lands
    float target = lane_x(run.lane);
    float delta = target - run.x;
    float step = delta * (DT / (LANE_TIME * 0.38f));
    float min_step = LANE_W * DT / (LANE_TIME * 2.5f);
    if(fabsf(step) < min_step) step = delta > 0.0f ? min_step : -min_step;
    if(fabsf(delta) <= fabsf(step)) run.x = target;
    else run.x += step;
    if(run.land_t > 0.0f) run.land_t = std::max(0.0f, run.land_t - DT);
    run.lane_change_t += DT;

    if(in_air()) {
        run.jump_t += DT;
        if(run.jump_t >= JUMP_TIME) {
            run.jump_t = 0.0f;
            run.land_t = 0.09f;
            audio::sfx(audio::Sfx::LAND);
            burst(64, float(PLAYER_FEET_Y), 4, Pen(200, 170, 140), 20.0f);
            if(run.jump_buffer > 0.0f) {
                run.jump_buffer = 0.0f;
                do_jump();
            } else if(run.jump_buffer < 0.0f) {
                run.jump_buffer = 0.0f;
                run.slide_t = 0.0001f;
                audio::sfx(audio::Sfx::SLIDE);
            }
        }
    }
    if(run.jump_buffer > 0.0f) run.jump_buffer = std::max(0.0f, run.jump_buffer - DT);

    if(sliding()) {
        run.slide_t += DT;
        if(run.slide_t >= SLIDE_TIME) run.slide_t = 0.0f;
    }

    // 8 frames per stride; stride rate grows with speed but less than linearly
    run.run_anim += DT * (9.0f + run.speed * 0.9f);
}

void update_run(bool input) {
    // speed: eases up with distance, extra during a dash
    float base = SPEED_START + (SPEED_MAX - SPEED_START) * (1.0f - expf(-run.distance / SPEED_RAMP));
    float want = base * (dashing() ? DASH_SPEED : 1.0f);
    run.speed = lerpf(run.speed, want, 0.03f);

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
            if(fabsf(o.x - lane_x(run.lane)) > LANE_W * 0.5f) continue;
            if(o.kind == K_LOW && dz < run.speed * 0.2f) do_jump();
            else if(o.kind == K_HIGH && dz < run.speed * 0.15f) do_slide();
            else if(o.kind == K_BLOCK || o.kind == K_WEED) {
                int l = run.lane == 1 ? (chance(0.5f) ? 0 : 2) : 1;
                run.lane = l;
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

    // road curves: a new random bend every so often
    if(run.distance > run.next_curve) {
        run.curve_target = chance(0.3f) ? 0.0f : (rnd_float() * 2.0f - 1.0f) * 0.014f;
        run.next_curve = run.distance + 25.0f + rnd_float() * 50.0f;
    }
    run.curve = lerpf(run.curve, run.curve_target, 0.004f);
    run.bg_scroll += run.curve * run.speed * DT * 220.0f;

    // camera follows the player a little behind
    run.cam_x = lerpf(run.cam_x, run.x * 0.75f, 0.15f);

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
            audio::sfx(audio::Sfx::NEW_BEST);
            popup("NEW BEST!", 64, 40, Pen(255, 240, 120));
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
        p.y -= 18.0f * DT;
    }
    if(run.shake > 0.0f) run.shake = std::max(0.0f, run.shake - DT);
    if(run.flash > 0.0f) run.flash = std::max(0.0f, run.flash - DT);
}

// ---------------------------------------------------------------------------
// Rendering: background

Colours colours_now(float distance) {
    float in_biome = fmodf(distance, BIOME_LENGTH);
    int b = biome_at(distance);
    if(in_biome > BIOME_LENGTH - BIOME_BLEND) {
        float t = (in_biome - (BIOME_LENGTH - BIOME_BLEND)) / BIOME_BLEND;
        return blend(biomes[b], biomes[(b + 1) % 3], t);
    }
    return blend(biomes[b], biomes[b], 0.0f);
}

// Mountain or skyline height at a screen column for the given biome.
int far_height(int biome, int x, float scroll) {
    float fx = float(x) + scroll * 0.3f;
    if(biome == 1) {
        // city: blocky towers
        int block = int(floorf(fx / 9.0f));
        return 8 + int(hash32(uint32_t(block) + 77u) % 18u);
    }
    if(biome == 2) {
        // volcano: a big cone in the middle of the panorama plus ridges
        float c = fmodf(fx, 256.0f);
        if(c < 0.0f) c += 256.0f;
        float cone = 26.0f - fabsf(c - 128.0f) * 0.45f;
        float ridge = 6.0f + 4.0f * sinf(fx * 0.05f) + 3.0f * sinf(fx * 0.13f);
        if(cone > 22.0f) cone = 22.0f + (cone - 22.0f) * 0.2f;  // flat crater top
        return int(std::max(cone, ridge));
    }
    return int(12.0f + 6.0f * sinf(fx * 0.031f) + 4.0f * sinf(fx * 0.077f + 1.0f) + 2.0f * sinf(fx * 0.19f));
}

int near_height(int biome, int x, float scroll) {
    float fx = float(x) + scroll;
    if(biome == 1) {
        int block = int(floorf(fx / 6.0f));
        return 3 + int(hash32(uint32_t(block) * 3u + 5u) % 9u);
    }
    return int(5.0f + 3.0f * sinf(fx * 0.06f + 2.0f) + 2.0f * sinf(fx * 0.15f));
}

void render_sky(const Colours &c, int biome, uint32_t time) {
    for(int y = 0; y <= HORIZON; y++) {
        screen.pen = mix(c.sky_top, c.sky_low, float(y) / float(HORIZON));
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
        screen.pen = mix(c.sky_top, c.sky_low, float(cy + 4) / float(HORIZON));
        for(int i = 0; i < 4; i++) screen.h_span(Point(sx - r, cy + 2 + i * 3), r * 2 + 1);
    }
    // far hills or skyline
    for(int x = 0; x < SW; x++) {
        int h = far_height(biome, x, run.bg_scroll);
        screen.pen = c.far_hills;
        screen.v_span(Point(x, HORIZON - h), h + 1);
    }
    if(biome == 1) {
        // lit windows on the skyline
        for(int x = 1; x < SW; x += 3) {
            float fx = float(x) + run.bg_scroll * 0.3f;
            int block = int(floorf(fx / 9.0f));
            int h = 8 + int(hash32(uint32_t(block) + 77u) % 18u);
            for(int y = HORIZON - h + 2; y < HORIZON - 1; y += 3) {
                if(hash32(uint32_t(block * 131 + x * 7 + y)) % 3u) continue;
                screen.pen = Pen(255, 210, 90);
                screen.pixel(Point(x, y));
            }
        }
    }
    if(biome == 2) {
        // lava glow at the crater
        for(int x = 0; x < SW; x++) {
            int h = far_height(biome, x, run.bg_scroll);
            if(h >= 22) {
                screen.pen = Pen(255, 120 + int((time / 80u + uint32_t(x)) % 60u), 30);
                screen.pixel(Point(x, HORIZON - h));
            }
        }
    }
    for(int x = 0; x < SW; x++) {
        int h = near_height(biome, x, run.bg_scroll);
        screen.pen = c.near_hills;
        screen.v_span(Point(x, HORIZON - h), h + 1);
    }
}

// ---------------------------------------------------------------------------
// Rendering: road

void render_road(const Colours &c) {
    for(int y = HORIZON + 1; y < SH; y++) {
        float k = float(y - HORIZON);  // = FOCAL / z
        float z = FOCAL / k;
        float cx = 64.0f + view_dx + (run.curve * z * z - run.cam_x) * k;
        float s = z + run.distance - Z_PLAYER;
        bool alt = (int(floorf(s / SEGMENT)) & 1) != 0;
        float fog = clampf((z - 5.0f) / (Z_FAR - 5.0f), 0.0f, 1.0f);
        fog = fog * fog;

        Pen grass = mix(alt ? c.grass_a : c.grass_b, c.fog, fog);
        Pen kerb = mix(alt ? c.kerb_a : c.kerb_b, c.fog, fog);
        Pen road = mix(alt ? c.road_a : c.road_b, c.fog, fog);
        Pen line = mix(c.line, c.fog, fog);

        screen.pen = grass;
        screen.h_span(Point(0, y), SW);

        float half = ROAD_HALF * k;
        float kerb_w = RUMBLE * k;
        int l = int(cx - half - kerb_w);
        int r = int(cx + half + kerb_w);
        screen.pen = kerb;
        screen.h_span(Point(l, y), r - l + 1);
        l = int(cx - half);
        r = int(cx + half);
        screen.pen = road;
        screen.h_span(Point(l, y), r - l + 1);

        // dashed lane lines
        if(!alt) {
            int w = std::max(1, int(0.025f * k));
            screen.pen = line;
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
        if(z > Z_FAR + 1.0f || z < 0.35f) continue;
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
}

void render_player(uint32_t time) {
    if(run.grace_t > 0.0f && ((time / 70u) & 1u) && state == State::RUN) return;

    float lift = jump_height() * FOCAL / Z_PLAYER;
    float px = 64.0f + view_dx + (run.x - run.cam_x) * FOCAL / Z_PLAYER;
    float py = float(PLAYER_FEET_Y) + view_dy;

    // shadow shrinks while in the air
    draw_shadow(px, py, 14.0f - lift * 0.15f);

    const Rect *r;
    if(state == State::CRASH || state == State::OVER) r = &spr::hurt;
    else if(dashing()) r = &spr::dash[(time / 80u) & 1u];
    else if(in_air()) {
        float t = run.jump_t / JUMP_TIME;
        r = t < 0.22f ? &spr::jump_up : (t < 0.62f ? &spr::jump : &spr::jump_down);
    }
    else if(sliding()) r = ((time / 80u) & 1u) ? &spr::slide2 : &spr::slide;
    else if(run.land_t > 0.0f) r = &spr::land;
    else r = &spr::run[int(run.run_anim) & 7];

    // lean into lane changes
    float lean = (lane_x(run.lane) - run.x) / LANE_W;
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
    snprintf(buf, sizeof(buf), "%dM", int(run.distance * METRES_PER_UNIT));
    text_shadow(buf, minimal_font, Point(SW - 3, SH - 10), Pen(230, 230, 240), TextAlign::top_right);

    if(run.biome_banner > 0.0f && ((time / 150u) & 1u || run.biome_banner < 1.5f)) {
        text_shadow(biomes[run.last_biome].name, outline_font, Point(64, 30), Pen(255, 255, 255), TextAlign::center_center);
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

void render_logo(int y, uint32_t time) {
    // "CHILI RUSH" with a wobble, letters coloured red to yellow
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
    render_logo(12, time);
    render_menu(title_items, 4, 58, menu_cursor, time);
    char buf[32];
    snprintf(buf, sizeof(buf), "BEST %lu", (unsigned long)save.best);
    text_shadow(buf, minimal_font, Point(64, 36), Pen(255, 240, 160), TextAlign::top_center);
    // rank stars
    int stars = rank();
    if(stars > 0) {
        int shown = std::min(stars, 10);
        int x0 = 64 - shown * 5;
        for(int i = 0; i < shown; i++) screen.blit(screen.sprites, spr::hud_star, Point(x0 + i * 10, 45));
        if(stars > 10) {
            snprintf(buf, sizeof(buf), "+%d", stars - 10);
            text_shadow(buf, minimal_font, Point(x0 + shown * 10 + 1, 45), Pen(255, 240, 160));
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
    text_shadow(buf, minimal_font, Point(64, 90), Pen(170, 160, 190), TextAlign::top_center);
    snprintf(buf, sizeof(buf), "CHILIS %lu  BEST %luM", (unsigned long)save.total_chilis, (unsigned long)save.best_distance);
    text_shadow(buf, minimal_font, Point(64, 100), Pen(170, 160, 190), TextAlign::top_center);
}

void render_ready(uint32_t time) {
    int n = 3 - int(state_time / 0.4f);
    char buf[16];
    if(n > 0) snprintf(buf, sizeof(buf), "%d", n);
    else snprintf(buf, sizeof(buf), "GO!");
    Pen pen = n > 0 ? Pen(255, 255, 255) : Pen(255, 220, 60);
    text_shadow(buf, outline_font, Point(64, 50), pen, TextAlign::center_center);
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
    snprintf(buf, sizeof(buf), "%dM   %lu CHILIS", int(run.distance * METRES_PER_UNIT), (unsigned long)run.chilis);
    text_shadow(buf, minimal_font, Point(64, 40), Pen(255, 220, 120), TextAlign::top_center);
    snprintf(buf, sizeof(buf), "BEST %lu", (unsigned long)save.best);
    text_shadow(buf, minimal_font, Point(64, 49), Pen(200, 190, 210), TextAlign::top_center);

    for(int i = 0; i < MISSION_SLOTS; i++) {
        const Mission &m = save.missions[i];
        int y = 62 + i * 13;
        bool ok = mission_complete[i];
        screen.pen = ok ? Pen(80, 200, 100, 90) : Pen(255, 255, 255, 25);
        screen.rectangle(Rect(4, y - 2, SW - 8, 12));
        char mt[28];
        mission_text(m, mt, sizeof(mt));
        text_shadow(mt, minimal_font, Point(7, y), ok ? Pen(150, 255, 160) : Pen(230, 230, 240));
        uint32_t v = std::min(mission_value(m.type), m.target);
        if(ok) snprintf(buf, sizeof(buf), "OK");
        else snprintf(buf, sizeof(buf), "%lu/%lu", (unsigned long)v, (unsigned long)m.target);
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

void on_quit_save() { store_game(); }

void init() {
    set_screen_mode(ScreenMode::hires);
    screen.sprites = Surface::load(asset_sprites);
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
    if(fmodf(run.distance, BIOME_LENGTH) > BIOME_LENGTH - BIOME_BLEND * 0.5f) biome = (biome + 1) % 3;

    // screen shake moves the road, sprites and player, not the HUD
    view_dx = view_dy = 0.0f;
    if(shake > 0.0f) {
        uint32_t h = hash32(time);
        view_dx = (float(h & 0xff) / 255.0f - 0.5f) * shake * 14.0f;
        view_dy = (float((h >> 8) & 0xff) / 255.0f - 0.5f) * shake * 8.0f;
    }

    render_sky(c, biome, time);
    render_road(c);
    render_objects(time, true);
    render_player(time);
    render_objects(time, false);
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
        case State::RUN: render_hud(time); render_popups(); break;
        case State::PAUSE: render_hud(time); render_pause(time); break;
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
            int before = int((state_time - DT) / 0.4f);
            int after = int(state_time / 0.4f);
            if(after != before) {
                if(after < 3) audio::sfx(audio::Sfx::COUNTDOWN);
                else if(after == 3) audio::sfx(audio::Sfx::GO);
            }
            // lanes can be chosen during the countdown
            if(buttons.pressed & Button::DPAD_LEFT) change_lane(-1);
            if(buttons.pressed & Button::DPAD_RIGHT) change_lane(1);
            update_player();
            if(state_time >= 1.2f) {
                set_state(State::RUN);
                audio::play(audio::Track::RUN);
                popup("GO!", 64, 50, Pen(255, 220, 60));
            }
            break;
        }

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
                set_state(State::RUN);
                break;
            }
            if(menu_nav(3)) {
                audio::sfx(audio::Sfx::MENU_OK);
                if(menu_cursor == 0) set_state(State::RUN);
                else {
                    // leaving a run counts like a crash for the totals but
                    // not for the best score unless it was beaten
                    save.runs++;
                    save.total_chilis += run.chilis;
                    if(run.score > save.best) save.best = run.score;
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
