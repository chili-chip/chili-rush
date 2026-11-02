// Chili Rush headless self-test, built with -DCHILI_SELFTEST (cmake
// -DCHILI_SELFTEST=ON) into chili-rush-selftest. Included at the end of
// game.cpp, so it drives the real update code and sees the game's state.
//
// 1. Fairness: a planning bot plays many seeds at every speed, dashes
//    included. Next to it the test keeps the set of every player state any
//    sequence of inputs (one input per 0.05 s) could be in by now, from the
//    start of the run. If that set ever becomes empty, the obstacles just
//    seen cannot be survived at all; that is rechecked with an input every
//    10 ms and reported with the obstacles that caused it.
// 2. Invariants every update: object pool, chilis inside obstacles,
//    power-ups inside rows, NaN/inf, score/combo/multiplier, the body model
//    matching the game.
// 3. Flows: countdown, pause and resume, saving and totals on every way out
//    of a run, the results screen guard, attract mode, save corruption, long
//    stalls, distance precision hours into a run, text that must fit.
// 4. --frames DIR renders fixed scenes to images; --bench times the renderer.
//
// Prints a summary; exits 1 if anything failed.

#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace selftest_host {
extern uint32_t clock_ms, clock_us;
extern std::map<std::string, std::vector<char>> files;
extern int save_writes;
extern uint16_t framebuffer[128 * 128];
}

// ---------------------------------------------------------------------------
// Audio stand-in: counts what the game asks for.

namespace audio {
int sfx_count[32];
Track track_now = Track::NONE;
void init() {}
void update(uint32_t) {}
void play(Track t) { track_now = t; }
void sfx(Sfx s) { sfx_count[int(s)]++; }
void sfx_combo(int) { sfx_count[int(Sfx::CHILI)]++; }
void set_intensity(float) {}
static bool music = true, sound = true;
void set_music(bool on) { music = on; }
void set_sound(bool on) { sound = on; }
bool music_on() { return music; }
bool sound_on() { return sound; }
}

namespace {

namespace st {

using namespace selftest_host;

// ---------------------------------------------------------------------------
// Failure bookkeeping

struct Failure {
    int count = 0;
    std::string first;
};
std::map<std::string, Failure> failures;
std::map<std::string, std::string> notes;  // non-fatal observations

void fail(const char *key, const char *fmt, ...) {
    auto &f = failures[key];
    if(f.count++ == 0) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        f.first = buf;
    }
}

bool verbose = false;

// ---------------------------------------------------------------------------
// Driving the game: one update per call, like the engine at 100 Hz.

void step(uint32_t pressed = 0) {
    buttons.state = pressed;
    buttons.pressed = pressed;
    buttons.released = 0;
    ::update(clock_ms);
    clock_ms += 10;
    clock_us += 10000;
    buttons.pressed = 0;
    buttons.state = 0;
}

void steps(int n, uint32_t pressed = 0) {
    for(int i = 0; i < n; i++) step(pressed);
}

// ---------------------------------------------------------------------------
// Inputs the planner can choose, one per decision step.

enum Act : int8_t { A_NONE, A_LEFT, A_RIGHT, A_JUMP, A_SLIDE, A_COUNT };
constexpr int DECIDE = 5;     // updates between decisions: 0.05 s
constexpr int HORIZON = 120;  // updates the bot looks ahead: 1.2 s

void apply(Body &b, int a) {
    switch(a) {
        case A_LEFT: body_lane(b, -1); break;
        case A_RIGHT: body_lane(b, 1); break;
        case A_JUMP: body_jump(b); break;
        case A_SLIDE: body_slide(b); break;
        default: break;
    }
}

uint32_t act_buttons(int a) {
    switch(a) {
        case A_LEFT: return Button::DPAD_LEFT;
        case A_RIGHT: return Button::DPAD_RIGHT;
        case A_JUMP: return Button::A;
        case A_SLIDE: return Button::B;
        default: return 0;
    }
}

// States closer than this are merged (keeps the search from blowing up
// on lane flip-flops that never settle): x to 1/256 of a world unit,
// timers to whole updates, a queued jump that runs out before landing to none.
inline int round_pos(float v) { return int(v + 0.5f); }  // v >= 0

uint64_t key_of(const Body &b) {
    int buffer = b.jump_buffer < 0.0f ? -1 : round_pos(b.jump_buffer * 100.0f);
    int jump = round_pos(b.jump_t * 100.0f);
    if(buffer > 0 && buffer < 62 - jump) buffer = 0;  // decays to nothing before landing
    uint64_t k = uint64_t(b.lane);
    k = k * 1024 + uint64_t(round_pos(b.x * 256.0f + 512.0f));
    k = k * 128 + uint64_t(jump);
    k = k * 128 + uint64_t(round_pos(b.slide_t * 100.0f));
    k = k * 64 + uint64_t(buffer + 1);
    k = k * 2 + b.fast_fall;
    return k;
}

bool body_equal(const Body &a, const Body &b) {
    return a.lane == b.lane && a.x == b.x && a.jump_t == b.jump_t && a.slide_t == b.slide_t &&
           a.jump_buffer == b.jump_buffer && a.fast_fall == b.fast_fall;
}

// ---------------------------------------------------------------------------
// Forecast: what the next updates look like whatever the player does. Speed,
// dash, grace and tumbleweeds do not depend on the player's moves.

struct Timeline {
    float speed, distance, dash_t, grace_t;
};

// mirrors the order of update_run(): speed, input, distance ... collisions, dash timers
void tl_move(Timeline &t) {
    t.speed = next_speed(t.speed, t.distance, t.dash_t > 0.0f);
    t.distance += t.speed * DT;
}
void tl_timers(Timeline &t) {
    if(t.dash_t > 0.0f) {
        t.dash_t -= DT;
        if(t.dash_t <= 0.0f) {
            t.dash_t = 0.0f;
            t.grace_t = GRACE_TIME;
        }
    }
    if(t.grace_t > 0.0f) t.grace_t = std::max(0.0f, t.grace_t - DT);
}

struct FTick {
    float distance;
    bool inv;
    int h0, h1;  // hazards in the hit window
    int c0, c1;  // chilis in the pickup window
};

struct Forecast {
    std::vector<FTick> t;
    std::vector<Obj> haz, chili;
};

void make_forecast(Forecast &f, int n, bool dash_now) {
    f.t.clear();
    f.haz.clear();
    f.chili.clear();
    Timeline tl{run.speed, run.distance, run.dash_t, run.grace_t};
    static std::vector<Obj> obs, ch;
    obs.clear();
    ch.clear();
    for(auto &o : objs) {
        if(is_obstacle(o.kind) && o.s - run.distance > -1.0f) obs.push_back(o);
        if(o.kind == K_CHILI && !o.done) ch.push_back(o);
    }
    for(int k = 0; k < n; k++) {
        tl_move(tl);
        if(k == 0 && dash_now && tl.dash_t <= 0.0f) tl.dash_t = DASH_TIME;
        FTick ft{tl.distance, tl.dash_t > 0.0f || tl.grace_t > 0.0f, int(f.haz.size()), 0, int(f.chili.size()), 0};
        for(auto &o : obs) {
            if(o.kind == K_WEED && !o.done) {
                roll_weed(o, o.s - tl.distance + Z_PLAYER);
                if(o.s - tl.distance < -HIT_DEPTH) o.done = true;
            }
            if(fabsf(o.s - tl.distance) < HIT_DEPTH) f.haz.push_back(o);
        }
        for(auto &c : ch)
            if(fabsf(c.s - tl.distance) < 0.35f) f.chili.push_back(c);
        ft.h1 = int(f.haz.size());
        ft.c1 = int(f.chili.size());
        f.t.push_back(ft);
        tl_timers(tl);
    }
}

bool hit_at(const Body &b, const FTick &ft, const Forecast &f) {
    if(ft.inv) return false;
    for(int i = ft.h0; i < ft.h1; i++)
        if(hits(b, f.haz[i], ft.distance)) return true;
    return false;
}

int chilis_at(const Body &b, const FTick &ft, const Forecast &f) {
    int n = 0;
    for(int i = ft.c0; i < ft.c1; i++) {
        const Obj &c = f.chili[i];
        if(in_lane_of(c, b.x) && fabsf(c.h - body_height(b)) < 0.24f) n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Planner: breadth-first over decisions, merging equal body states.
// Returns the first action of the best plan that survives the forecast, or
// -1 when every sequence of inputs is hit.

struct Node {
    Body b;
    int8_t first, second;
    int score;
};

long plan_nodes = 0;

// The bot only uses sensible moves (the reachable set below uses every
// move): change lane once settled, jump and slide from the ground, slide in
// the air to drop.
bool bot_allows(const Body &b, int a) {
    switch(a) {
        case A_LEFT: return b.lane > 0 && b.x == lane_x(b.lane);
        case A_RIGHT: return b.lane < 2 && b.x == lane_x(b.lane);
        case A_JUMP: return !body_in_air(b);
        case A_SLIDE: return !body_sliding(b) && b.jump_buffer >= 0.0f;
        default: return true;
    }
}

int plan(const Body &start, const Forecast &f, int *second = nullptr) {
    static std::vector<Node> cur, nxt;
    static std::unordered_map<uint64_t, int> seen;
    cur.clear();
    cur.push_back({start, -1, -1, 0});
    int layers = int(f.t.size()) / DECIDE;
    for(int d = 0; d < layers; d++) {
        nxt.clear();
        seen.clear();
        for(auto &n : cur) {
            for(int a = 0; a < A_COUNT; a++) {
                if(!bot_allows(n.b, a)) continue;
                Body b = n.b;
                apply(b, a);
                int score = n.score - (a == A_NONE ? 0 : 1);
                bool dead = false;
                for(int k = 0; k < DECIDE && !dead; k++) {
                    const FTick &ft = f.t[d * DECIDE + k];
                    body_step(b);
                    if(hit_at(b, ft, f)) dead = true;
                    else score += 6 * chilis_at(b, ft, f);
                }
                plan_nodes++;
                if(dead) continue;
                int8_t first = d == 0 ? int8_t(a) : n.first;
                int8_t second = d == 1 ? int8_t(a) : n.second;
                uint64_t key = key_of(b);
                auto it = seen.find(key);
                if(it == seen.end()) {
                    seen.emplace(key, int(nxt.size()));
                    nxt.push_back({b, first, second, score});
                } else if(nxt[it->second].score < score) {
                    nxt[it->second] = {b, first, second, score};
                }
            }
        }
        std::swap(cur, nxt);
        if(cur.empty()) return -1;
    }
    // prefer ending on the ground, settled in a lane, ready for anything
    int best = -1, best2 = A_NONE, best_v = -(1 << 30);
    for(auto &n : cur) {
        int v = n.score;
        if(!body_in_air(n.b) && !body_sliding(n.b)) v += 3;
        if(n.b.x == lane_x(n.b.lane)) v += 2;
        if(n.b.lane == 1) v += 1;
        if(v > best_v) {
            best_v = v;
            best = n.first;
            best2 = n.second < 0 ? int(A_NONE) : int(n.second);
        }
    }
    if(second) *second = best2;
    return best < 0 ? A_NONE : best;
}

// ---------------------------------------------------------------------------
// Reachable set: every body state some input sequence could be in, stepped
// alongside the real game against the real obstacles.

// Open-addressing set of state keys, cleared in O(1) by a generation stamp.
struct KeySet {
    std::vector<uint64_t> keys;
    std::vector<uint32_t> gen;
    uint32_t now = 1;
    size_t used = 0;
    void clear(size_t expect) {
        size_t want = 1024;
        while(want < expect * 2) want <<= 1;
        if(want > keys.size()) {
            keys.assign(want, 0);
            gen.assign(want, 0);
        }
        now++;
        used = 0;
    }
    bool insert(uint64_t k) {
        size_t mask = keys.size() - 1;
        size_t i = size_t((k * 0x9e3779b97f4a7c15ull) >> 20) & mask;
        while(gen[i] == now) {
            if(keys[i] == k) return false;
            i = (i + 1) & mask;
        }
        if(++used * 2 > keys.size()) {  // grow and rehash
            std::vector<uint64_t> old;
            for(size_t j = 0; j < keys.size(); j++)
                if(gen[j] == now) old.push_back(keys[j]);
            keys.assign(keys.size() * 2, 0);
            gen.assign(keys.size(), 0);
            now++;
            used = 0;
            for(auto o : old) insert(o);
            return insert(k);
        }
        gen[i] = now;
        keys[i] = k;
        return true;
    }
};

struct Reach {
    std::vector<Body> set, tmp;
    KeySet seen;
    size_t max_size = 0;

    void reset(const Body &b) {
        set.clear();
        set.push_back(b);
    }
    void reset_lanes() {
        set.clear();
        for(int l = 0; l < 3; l++) set.push_back(Body{l, lane_x(l), 0, 0, 0, false});
    }
    // one update; decide: inputs allowed this update (every one for `fine`)
    void step(bool decide, const std::vector<Obj> &haz, float distance, bool inv) {
        tmp.clear();
        if(decide) seen.clear(set.size() * 5);
        for(auto &s : set) {
            for(int a = 0; a < (decide ? int(A_COUNT) : 1); a++) {
                Body b = s;
                apply(b, a);
                body_step(b);
                bool dead = false;
                if(!inv)
                    for(auto &o : haz)
                        if(hits(b, o, distance)) { dead = true; break; }
                if(dead) continue;
                if(!decide || seen.insert(key_of(b))) tmp.push_back(b);
            }
        }
        std::swap(set, tmp);
        max_size = std::max(max_size, set.size());
    }
};

// A ring of recent updates, to recheck an empty reachable set with an
// input allowed every update.
struct Recent {
    std::vector<Obj> haz;
    float distance;
    bool inv;
    bool decide;
};

// ---------------------------------------------------------------------------
// Objects spawned during an update, checked once the row is complete.

std::vector<int> spawned;
long r_max = 0;  // largest reachable set
int max_live_objs = 0;
long chilis_spawned = 0, powerups_spawned = 0;

}  // namespace st

void st_spawned(Obj &o) { st::spawned.push_back(int(&o - objs)); }

// render phase timing for --bench: sky, road, sprites, effects and HUD
double st_phase_us[5];
std::chrono::steady_clock::time_point st_phase_t;
void st_profile(int phase) {
    auto now_t = std::chrono::steady_clock::now();
    if(phase > 0) st_phase_us[phase - 1] += std::chrono::duration<double, std::micro>(now_t - st_phase_t).count();
    st_phase_t = now_t;
}

namespace st {

void check_spawns() {
    for(int idx : spawned) {
        const Obj &p = objs[idx];
        if(!is_pickup(p.kind)) continue;
        if(p.kind == K_CHILI) chilis_spawned++;
        else powerups_spawned++;
        for(auto &o : objs) {
            if(!is_obstacle(o.kind)) continue;
            float ds = fabsf(p.s - o.s);
            if(p.kind == K_CHILI) {
                bool lane = in_lane_of(o, p.x) || o.kind == K_WEED;  // a tumbleweed rolls over every lane
                if(!lane || ds >= HIT_DEPTH + 0.35f) continue;
                bool bad = o.kind == K_BLOCK || ((o.kind == K_LOW || o.kind == K_WEED) && p.h < 0.12f) ||
                           (o.kind == K_HIGH && p.h >= 0.24f);
                if(bad)
                    fail("chili inside an obstacle", "chili at s=%.2f x=%.2f h=%.2f, obstacle kind %d at s=%.2f x=%.2f",
                         p.s, p.x, p.h, int(o.kind), o.s, o.x);
            } else if(ds < 1.0f) {
                fail("power-up inside a row", "pickup kind %d at s=%.2f x=%.2f, obstacle kind %d at s=%.2f x=%.2f",
                     int(p.kind), p.s, p.x, int(o.kind), o.s, o.x);
            }
        }
    }
    spawned.clear();
    int live = 0;
    for(auto &o : objs) live += o.kind != K_NONE;
    max_live_objs = std::max(max_live_objs, live);
}

bool finite_all() {
    const float run_floats[] = {run.distance, run.speed, run.cam_x, run.p.x, run.p.jump_t, run.p.slide_t,
                                run.p.jump_buffer, run.lane_change_t, run.run_anim, run.land_t, run.heat,
                                run.dash_t, run.grace_t, run.magnet_t, run.score_frac, run.next_spawn,
                                run.next_decor, run.next_power, run.curve, run.curve_target, run.next_curve,
                                run.bg_scroll, run.shake, run.flash, run.biome_banner};
    for(float f : run_floats)
        if(!std::isfinite(f)) return false;
    for(auto &o : objs)
        if(o.kind != K_NONE && !(std::isfinite(o.x) && std::isfinite(o.s) && std::isfinite(o.h) &&
                                 std::isfinite(o.vx) && std::isfinite(o.target_x) && std::isfinite(o.fly)))
            return false;
    for(auto &p : particles)
        if(p.life > 0.0f && !(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.vx) && std::isfinite(p.vy)))
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// Fairness runs

struct Stats {
    double sim_seconds = 0, dash_seconds = 0, distance = 0;
    float max_speed = 0;
    long runs = 0, decisions = 0, rescues = 0, ghost_ticks = 0, bot_hits = 0;
    long impossible = 0, coarse_only = 0, fidelity = 0, dashes = 0, rows = 0;
    long chilis = 0, close_calls = 0, smashed = 0;
    double plan_ms = 0;
} stats;

void dump_obstacles(const char *why) {
    if(!verbose) return;
    fprintf(stderr, "  %s at distance %.2f speed %.2f dash %.2f grace %.2f lane %d x %.3f jump %.3f slide %.3f\n", why,
            run.distance, run.speed, run.dash_t, run.grace_t, run.p.lane, run.p.x, run.p.jump_t, run.p.slide_t);
    for(auto &o : objs)
        if(is_obstacle(o.kind) && o.s - run.distance > -3.0f && o.s - run.distance < 12.0f)
            fprintf(stderr, "    kind %d s %+.2f x %+.3f%s\n", int(o.kind), o.s - run.distance, o.x, o.done ? " done" : "");
}

// Warp a fresh run to `d` along the road (tests far distances quickly).
void warp(float d) {
    for(auto &o : objs) o.kind = K_NONE;
    road_origin = uint32_t(d / REBASE) * uint32_t(REBASE);
    d -= float(road_origin);
    run.distance = d;
    run.speed = next_speed(SPEED_START, d, false);
    for(int i = 0; i < 400; i++) run.speed = next_speed(run.speed, d, false);
    run.next_spawn = d + 16.0f;
    run.next_decor = d;
    run.next_power = d + 160.0f;
    run.last_row = d;
    run.next_curve = d + 40.0f;
    run.last_biome = biome_at(d);
    run.biome_banner = 0.0f;
    update_generation();
}

struct RunSpec {
    uint32_t seed;
    float start;       // warp to this distance
    float length;      // distance to run
    bool dash;         // fire dash whenever the heat is full
    float heat_every;  // cheat: fill the heat bar every so many seconds (0: never)
    int inject = 0;    // detector check: 1 = a wall of three blocks, 2 = hard but fair rows
};

// Replace the rows just ahead with a hand-made pattern.
void inject_rows(int which) {
    float s0 = run.distance + 14.0f;
    for(auto &o : objs)
        if(o.kind != K_DECOR && o.kind != K_NONE && o.s > run.distance + 2.0f) o.kind = K_NONE;
    float g = min_gap() * 1.1f;  // the closest two rows can be
    if(which == 1) {
        for(int l = 0; l < 3; l++) spawn(K_BLOCK, lane_x(l), s0);
    } else {
        // full low row, full high row, then block-block-low, block-low-block: all at the closest gap
        for(int l = 0; l < 3; l++) spawn(K_LOW, lane_x(l), s0);
        for(int l = 0; l < 3; l++) spawn(K_HIGH, lane_x(l), s0 + g);
        spawn(K_BLOCK, lane_x(0), s0 + 2 * g); spawn(K_BLOCK, lane_x(1), s0 + 2 * g); spawn(K_LOW, lane_x(2), s0 + 2 * g);
        spawn(K_BLOCK, lane_x(2), s0 + 3 * g); spawn(K_HIGH, lane_x(1), s0 + 3 * g); spawn(K_BLOCK, lane_x(0), s0 + 3 * g);
        for(int l = 0; l < 3; l++) spawn(K_HIGH, lane_x(l), s0 + 4 * g);
        for(int l = 0; l < 3; l++) spawn(K_LOW, lane_x(l), s0 + 5 * g);
    }
    run.next_spawn = s0 + 7.0f * g;
    spawned.clear();
}

long bot_run(const RunSpec &spec) {
    long impossible_here = 0;
    clock_ms = 1000 + spec.seed * 7919u;
    begin_run();
    // countdown
    int guard = 0;
    while(state == State::READY && guard++ < 1000) step();
    if(state != State::RUN) {
        fail("countdown", "run did not start after the countdown (state %d)", int(state));
        return 0;
    }
    if(spec.start > 0.0f) warp(spec.start);
    if(spec.inject) inject_rows(spec.inject);
    float start_distance = total_distance(run.distance);
    stats.runs++;

    Reach reach;
    reach.reset_lanes();
    // the bot's own body is one of them
    reach.set[run.p.lane] = run.p;
    std::vector<Recent> ring(400);
    std::vector<std::pair<long, std::vector<Body>>> snaps;
    long t = 0;
    int ghost = 0;
    float heat_clock = 0.0f;
    uint32_t prev_score = run.score;
    Forecast fc;
    std::vector<Obj> haz;
    int next_act = -1;

    while(total_distance(run.distance) - start_distance < spec.length && t < 200000) {
        bool decide = (t % DECIDE) == 0;
        int act = A_NONE;
        uint32_t extra = 0;
        if(spec.heat_every > 0.0f) {
            heat_clock += DT;
            if(heat_clock >= spec.heat_every) {
                heat_clock = 0.0f;
                if(!dashing()) run.heat = 1.0f;
            }
        }
        bool dash_now = decide && spec.dash && run.heat >= 1.0f && !dashing();
        if(dash_now) extra |= Button::Y;
        if(decide && next_act >= 0 && !dash_now) {
            act = next_act;  // the second step of the last plan
            next_act = -1;
            stats.decisions++;
        } else if(decide) {
            auto t0 = std::chrono::steady_clock::now();
            make_forecast(fc, HORIZON, dash_now);
            act = plan(run.p, fc, &next_act);
            if(act < 0) {
                // the bot walked into a dead end: jump to any state the
                // reachable set says a player could be in, that has a way out
                int tries = 0;
                for(size_t i = 0; i < reach.set.size() && tries < 200; i += 1 + reach.set.size() / 200, tries++) {
                    int a = plan(reach.set[i], fc, &next_act);
                    if(a >= 0) {
                        run.p = reach.set[i];
                        act = a;
                        stats.rescues++;
                        break;
                    }
                }
                if(act < 0) {
                    next_act = -1;
                    if(!ghost) dump_obstacles("no plan from any reachable state");
                    ghost = 150;  // pass through; the reachable set decides if it was fair
                    act = A_NONE;
                }
            }
            stats.plan_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            stats.decisions++;
        }
        st_ghost = ghost > 0;
        if(ghost > 0) {
            ghost--;
            stats.ghost_ticks++;
        }

        // what the body and the road should do this update
        Body expect = run.p;
        apply(expect, act);
        body_step(expect);
        Timeline tl{run.speed, run.distance, run.dash_t, run.grace_t};
        tl_move(tl);
        bool inv_before = run.dash_t > 0.0f || run.grace_t > 0.0f;
        uint32_t origin_before = road_origin;

        step(act_buttons(act) | extra);
        t++;
        check_spawns();

        if(state != State::RUN) {
            stats.bot_hits++;
            fail("bot crashed", "state %d at distance %.2f (planner and game disagree?)", int(state), run.distance);
            dump_obstacles("bot crashed");
            break;
        }
        if(road_origin != origin_before) tl.distance -= REBASE;
        if(!body_equal(expect, run.p) || tl.distance != run.distance) {
            stats.fidelity++;
            fail("body model differs from the game", "t=%ld lane %d/%d x %.6f/%.6f jump %.6f/%.6f distance %.4f/%.4f", t,
                 expect.lane, run.p.lane, expect.x, run.p.x, expect.jump_t, run.p.jump_t, tl.distance, run.distance);
        }
        if(!finite_all()) fail("NaN or inf", "at distance %.2f", run.distance);
        if(run.score < prev_score) fail("score went down", "%u -> %u", prev_score, run.score);
        if(run.score - prev_score > 20000) fail("score jump", "%u -> %u in one update", prev_score, run.score);
        prev_score = run.score;
        if(run.combo < 0 || run.best_combo < run.combo) fail("combo", "combo %d best %d", run.combo, run.best_combo);
        if(multiplier() < 1 || multiplier() > base_multiplier() + 4) fail("multiplier", "%d", multiplier());
        if(run.heat < 0.0f || run.heat > 1.0f) fail("heat", "%f", double(run.heat));
        for(auto &p : popups)
            if(p.life > 0.0f && p.y < 36.0f) fail("popup over the HUD or biome banner", "'%s' at y %.0f", p.text, double(p.y));
        if(run.chilis > uint32_t(chilis_spawned) + 1000) fail("chilis", "more collected than spawned");

        // the reachable set, against the real obstacles
        haz.clear();
        for(auto &o : objs)
            if(is_obstacle(o.kind) && fabsf(o.s - run.distance) < HIT_DEPTH) haz.push_back(o);
        bool inv = inv_before || run.dash_t > 0.0f || run.grace_t > 0.0f;
        ring[t % ring.size()] = {haz, run.distance, inv, decide};
        if(t % 25 == 0) {
            snaps.push_back({t, reach.set});
            if(snaps.size() > 20) snaps.erase(snaps.begin());
        }
        reach.step(decide, haz, run.distance, inv);
        if(reach.set.empty()) {
            // recheck with an input allowed every update, from ~2 s back
            size_t si = 0;
            while(si + 1 < snaps.size() && t - snaps[si + 1].first >= 200) si++;
            bool fair = false;
            if(!snaps.empty() && t - snaps[si].first < long(ring.size())) {
                Reach fine;
                fine.set = snaps[si].second;
                for(long k = snaps[si].first + 1; k <= t && !fine.set.empty(); k++) {
                    const Recent &r = ring[k % ring.size()];
                    fine.step(true, r.haz, r.distance, r.inv);
                }
                fair = !fine.set.empty();
            }
            if(fair) stats.coarse_only++;
            else if(spec.inject) impossible_here++;
            else {
                stats.impossible++;
                impossible_here++;
                fail("impossible obstacles", "seed %u at %.1f m, speed %.2f%s", spec.seed, run.distance, run.speed,
                     dashing() ? " (dash)" : "");
                dump_obstacles("IMPOSSIBLE");
            }
            reach.reset(run.p);
            if(!st_ghost) ghost = 60;
        }
        // keep the bot's body inside the reachable set (it always is, unless
        // it passed through something as a ghost)
        if(ghost == 0 && reach.set.size() > 50000) reach.reset(run.p);

        stats.sim_seconds += DT;
        if(dashing()) stats.dash_seconds += DT;
        stats.max_speed = std::max(stats.max_speed, run.speed);
    }
    st_ghost = false;
    stats.distance += total_distance(run.distance) - start_distance;
    stats.dashes += run.dashes;
    stats.chilis += run.chilis;
    stats.close_calls += run.close_calls;
    stats.smashed += run.smashed;
    if(verbose)
        fprintf(stderr, "seed %u: %.0f m to %.0f m, score %u, chilis %u, dashes %u, |R| max %zu\n", spec.seed,
                double(start_distance), double(total_distance(run.distance)), run.score, run.chilis, run.dashes, reach.max_size);
    r_max = std::max(r_max, long(reach.max_size));
    return impossible_here;
}

// The detector must catch a wall of blocks and pass the hardest fair rows.
void detector_check() {
    for(float speed_at : {0.0f, 6000.0f}) {
        RunSpec wall{77, speed_at, 80.0f, false, 0.0f, 1};
        long n = bot_run(wall);
        if(n < 1) fail("detector", "a wall of three blocks at %.0f m was not reported impossible", double(speed_at));
        RunSpec hard{78, speed_at, 150.0f, false, 0.0f, 2};
        n = bot_run(hard);
        if(n > 0) fail("detector", "the hardest fair rows at %.0f m were reported impossible", double(speed_at));
        if(failures.count("bot crashed")) fail("detector", "the bot crashed on the hard rows");
    }
    notes["detector"] = failures.count("detector") ? "BROKEN" : "catches a wall of blocks, passes the hardest fair rows";
}

int fairness(int seeds, int job, int jobs) {
    for(int i = 0; i < seeds; i++) {
        if((i + i / 4) % jobs != job) continue;  // spread the long runs over the processes
        RunSpec spec;
        spec.seed = uint32_t(i + 1);
        spec.dash = (i % 3) != 2;
        spec.heat_every = (i % 2) ? 6.0f : 0.0f;  // many dashes in half the runs
        switch(i % 4) {
            case 0: spec.start = 0.0f; spec.length = 3000.0f; break;            // a normal start, slow speeds
            case 1: spec.start = 2500.0f; spec.length = 4000.0f; break;         // middle speeds
            case 2: spec.start = 9000.0f; spec.length = 5000.0f; break;         // top speed
            default: spec.start = 0.0f; spec.length = 12000.0f; break;          // the whole ramp in one run
        }
        bot_run(spec);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Flows and edge cases

int flow_checks = 0;

bool save_valid() {
    if(save.magic != SAVE_MAGIC || save.version != SAVE_VERSION) return false;
    for(auto &m : save.missions)
        if(m.type >= M_TYPES || m.target == 0) return false;
    return true;
}

void expect(bool ok, const char *key, const char *fmt, ...) {
    flow_checks++;
    if(ok) return;
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fail(key, "%s", buf);
}

void fresh_save() {
    files.clear();
    reset_save();
    reset_style();
    apply_style();
    save.seen_help = 1;
    store_game();
}

bool to_running() {
    begin_run();
    for(int i = 0; i < 1000 && state != State::RUN; i++) step();
    return state == State::RUN;
}

// Would the next update with no input crash the runner?
bool crash_next() {
    Body b = run.p;
    body_step(b);
    Timeline tl{run.speed, run.distance, run.dash_t, run.grace_t};
    tl_move(tl);
    if(tl.dash_t > 0.0f || tl.grace_t > 0.0f || run.shield) return false;
    for(auto &o : objs)
        if(!o.done && hits(b, o, tl.distance)) return true;
    return false;
}

void crash_now() {
    st_ghost = false;
    run.shield = false;
    run.grace_t = 0.0f;
    for(int i = 0; i < 20000 && state == State::RUN; i++) step();
}

void flow_countdown() {
    for(int k = 0; k < 20; k++) {
        int c0 = audio::sfx_count[int(audio::Sfx::COUNTDOWN)], g0 = audio::sfx_count[int(audio::Sfx::GO)];
        begin_run();
        int n = 0;
        while(state == State::READY && n < 500) {
            step();
            n++;
        }
        int c = audio::sfx_count[int(audio::Sfx::COUNTDOWN)] - c0, g = audio::sfx_count[int(audio::Sfx::GO)] - g0;
        expect(c == 3 && g == 1 && n == 120, "flow: countdown", "%d beeps, %d GO, %d updates (want 3, 1, 120)", c, g, n);
    }
}

void flow_crash_tick_mission() {
    // a distance mission reached on the very update of the crash
    int tried = 0, ok = 0;
    for(int k = 0; k < 40; k++) {
        fresh_save();
        clock_ms += 7777;
        if(!to_running()) return;
        for(int i = 0; i < 30000 && state == State::RUN; i++) {
            if(crash_next()) {
                Timeline tl{run.speed, run.distance, run.dash_t, run.grace_t};
                tl_move(tl);
                uint32_t before = uint32_t(run.distance * METRES_PER_UNIT), after = uint32_t(tl.distance * METRES_PER_UNIT);
                if(after > before) {
                    save.missions[0] = {M_DISTANCE, 0, 0, after};
                    step();
                    tried++;
                    ok += mission_complete[0];
                    break;
                }
            }
            step();
        }
    }
    expect(tried == 0 || ok == tried, "flow: mission reached on the crash update", "%d of %d not completed", tried - ok, tried);
    notes["mission on the crash update"] = std::to_string(tried) + " crashes tried";
}

// Every way out of a run counts it once, with its chilis, best and missions.
void flow_totals() {
    const char *paths[] = {"crash, A",          "crash, B",           "pause, restart",
                           "pause, quit",       "quit to home in run", "quit to home paused",
                           "quit to home crash", "quit to home results"};
    for(int p = 0; p < 8; p++) {
        fresh_save();
        save.missions[0] = {M_DISTANCE, 0, 0, 50};
        uint32_t runs0 = save.runs, done0 = save.missions_done;
        if(!to_running()) return;
        st_ghost = true;
        steps(1500);
        st_ghost = false;
        uint32_t chilis = run.chilis, metres = uint32_t(run.distance);
        bool reached = mission_complete[0];
        auto quit_home = [] {
            on_quit_save();
            load_game();  // what the next start would see
        };
        switch(p) {
            case 0:
            case 1:
                crash_now();
                chilis = run.chilis;
                metres = uint32_t(run.distance);
                steps(200);
                step(p == 0 ? Button::A : Button::B);
                break;
            case 2:
            case 3:
                step(Button::MENU);
                menu_cursor = p == 2 ? 1 : 2;
                step(Button::A);
                break;
            case 4: quit_home(); break;
            case 5: step(Button::MENU); quit_home(); break;
            case 6:
                crash_now();
                chilis = run.chilis;
                metres = uint32_t(run.distance);
                steps(30);
                quit_home();
                break;
            case 7:
                crash_now();
                chilis = run.chilis;
                metres = uint32_t(run.distance);
                steps(200);
                quit_home();
                break;
        }
        SaveData disk{};
        read_save(disk);
        expect(disk.runs == runs0 + 1, "flow: runs counted once", "%s: runs %u -> %u", paths[p], runs0, disk.runs);
        expect(disk.total_chilis == chilis, "flow: chilis counted once", "%s: total %u, run had %u", paths[p], disk.total_chilis, chilis);
        expect(disk.best_distance >= metres, "flow: best distance", "%s: best %u m, ran %u m", paths[p], disk.best_distance, metres);
        expect(!reached || disk.missions_done == done0 + 1, "flow: mission kept", "%s: missions done %u -> %u", paths[p], done0,
               disk.missions_done);
    }
}

void flow_results_guard() {
    fresh_save();
    if(!to_running()) return;
    crash_now();
    int in_over = 0, n = 0;
    while(n++ < 1000 && (state == State::CRASH || state == State::OVER)) {
        if(state == State::OVER) in_over++;
        step(Button::A);  // mashing A from the crash on
        step();
    }
    expect(in_over >= 30, "flow: results screen guard", "results left after %d updates of mashing A", in_over * 2);
}

void flow_attract() {
    fresh_save();
    to_title();
    SaveData before = save;
    int writes = save_writes;
    const audio::Sfx world[] = {audio::Sfx::JUMP, audio::Sfx::LAND, audio::Sfx::SLIDE, audio::Sfx::CHILI, audio::Sfx::GOLDEN,
                                audio::Sfx::POWERUP, audio::Sfx::LANE, audio::Sfx::SMASH, audio::Sfx::HIT};
    int sfx0[16];
    for(int i = 0; i < 9; i++) sfx0[i] = audio::sfx_count[int(world[i])];
    for(int i = 0; i < 60000; i++) {
        step();
        if(state != State::TITLE) break;
        if(i % 997 == 0 && !finite_all()) fail("flow: attract", "NaN on the title screen");
    }
    int sounds = 0;
    for(int i = 0; i < 9; i++) sounds += audio::sfx_count[int(world[i])] - sfx0[i];
    expect(state == State::TITLE, "flow: attract", "the title demo left the title screen (state %d)", int(state));
    expect(memcmp(&before, &save, sizeof(save)) == 0 && save_writes == writes, "flow: attract", "the title demo changed the save");
    expect(sounds == 0, "flow: attract sounds", "the title demo played %d game sounds over the title music", sounds);
}

void flow_stall() {
    // the console's system menu (HOME) stops the game; on resume the engine
    // catches up every missed update at once
    fresh_save();
    if(!to_running()) return;
    st_ghost = true;
    steps(300);
    st_ghost = false;
    uint32_t t = clock_ms;
    clock_ms += 8000;  // real time has moved on
    for(int i = 0; i < 800; i++, t += 10) {
        buttons.pressed = 0;
        ::update(t);
    }
    expect(state == State::PAUSE, "flow: catch-up after a stall", "8 s of catch-up updates left the game in state %d", int(state));
}

void flow_saves() {
    // garbage
    files.clear();
    std::vector<char> junk(sizeof(SaveData));
    for(auto &c : junk) c = char(rand());
    files["save0"] = junk;
    load_game();
    expect(save_valid() && save.best == 0, "flow: save garbage", "garbage save not reset");
    // another version
    fresh_save();
    save.best = 1234;
    save.version = SAVE_VERSION + 1;
    store_game();
    load_game();
    expect(save.version == SAVE_VERSION && save.best == 0, "flow: save version", "other version not reset");
    // a short file
    files["save0"] = std::vector<char>(10, 1);
    load_game();
    expect(save_valid(), "flow: short save", "short save not reset");
    // a damaged mission keeps the rest of the save
    fresh_save();
    save.best = 4321;
    save.missions[1].target = 999999;
    save.missions[2].type = save.missions[0].type;
    save.music = 7;
    store_game();
    load_game();
    bool unique = save.missions[0].type != save.missions[1].type && save.missions[1].type != save.missions[2].type &&
                  save.missions[0].type != save.missions[2].type;
    bool targets = true;
    for(auto &m : save.missions) targets &= m.target == mission_target(m.type, m.level);
    expect(save.best == 4321 && unique && targets && save.music == 1, "flow: damaged missions",
           "best %u, unique %d, targets %d, music %d", save.best, unique, targets, save.music);
}

void flow_close_calls() {
    for(int c = 0; c < 2; c++) {
        fresh_save();
        if(!to_running()) return;
        for(auto &o : objs) if(o.kind != K_DECOR) o.kind = K_NONE;
        run.next_spawn = run.distance + 60.0f;
        int from = c == 0 ? 0 : 2;
        run.p = Body{from, lane_x(from), 0, 0, 0, false};
        float s = run.distance + run.speed * 0.2f;
        if(c == 0) { spawn(K_BLOCK, lane_x(0), s); spawn(K_BLOCK, lane_x(2), s); }
        else spawn(K_BLOCK, lane_x(0), s);
        step(c == 0 ? Button::DPAD_RIGHT : Button::DPAD_LEFT);
        steps(60);
        int want = c == 0 ? 1 : 0;
        expect(state == State::RUN && int(run.close_calls) == want, "flow: close calls",
               "%s: %u close calls, want %d", c == 0 ? "dodged one of two blocks" : "never in the block's lane", run.close_calls, want);
    }
}

void flow_popups() {
    fresh_save();
    if(!to_running()) return;
    for(auto &p : popups) p.life = 0.0f;
    run.heat = 1.0f;
    step(Button::Y);  // FIRE DASH!
    Obj g{};
    g.kind = K_GOLDEN;
    collect(g);  // HEAT FULL! at the same moment
    int live = 0;
    bool overlap = false;
    for(auto &a : popups)
        for(auto &b : popups)
            if(&a != &b && a.life > 0.0f && b.life > 0.0f && fabsf(a.y - b.y) < 8.0f) overlap = true;
    for(auto &p : popups) live += p.life > 0.0f;
    expect(!overlap, "flow: popups overlap", "%d popups drawn on top of each other", live);
    // the multiplier popup only when the multiplier goes up
    for(auto &p : popups) p.life = 0.0f;
    run.combo = 49;
    Obj ch{};
    ch.kind = K_CHILI;
    collect(ch);
    bool shown = false;
    for(auto &p : popups) shown |= p.life > 0.0f && p.text[0] == 'X';
    expect(!shown, "flow: multiplier popup", "X popup at combo 50 although the multiplier is already at its top");
}

void flow_fast_fall() {
    fresh_save();
    if(!to_running()) return;
    st_ghost = true;
    step(Button::A);
    while(run.p.jump_t < JUMP_TIME * 0.5f) step();
    float h = jump_height(), worst = 0.0f;
    step(Button::B);
    int n = 0;
    while(in_air() && n++ < 100) {
        worst = std::max(worst, h - jump_height());
        h = jump_height();
        step();
    }
    worst = std::max(worst, h);
    st_ghost = false;
    expect(worst < 0.07f, "flow: slide in the air", "the runner drops %.2f in one update (snaps down)", double(worst));
    expect(sliding(), "flow: slide in the air", "no slide on landing");
}

// Strings must fit their place on the 128 px screen.
// Obstacles must read from far enough to react: at top speed, an obstacle
// 0.45 s away is drawn at least 7 px wide (lanes are 32 px wide at the player).
void flow_readability() {
    fresh_save();
    to_running();
    const float react = 0.45f;
    float z = Z_PLAYER + SPEED_MAX * react;
    Screen p = project(0.0f, 0.0f, z);
    float w = float(spr::fence.w) * p.scale, h = float(spr::fence.h) * p.scale;
    expect(w >= 7.0f && h >= 3.5f, "flow: readability",
           "a fence %.2f s away at top speed is %.1f x %.1f px (want 7 x 3.5)", double(react), double(w), double(h));
}


// --- v1.2: cars, the STYLE menu and the start-of-run camera move ---------

bool same_rect(const Rect &a, const Rect &b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

// Cars replace the cactus and the vending machine on the road; their
// headlights are off, dipped, on full beam or flashing, fixed per car.
void flow_cars() {
    expect(same_rect(obstacle_rect(K_BLOCK, 0, 0), spr::car_desert), "flow: cars", "desert BLOCK is not the pickup");
    expect(same_rect(obstacle_rect(K_BLOCK, 1, 0), spr::car_city), "flow: cars", "city BLOCK is not the racer");
    expect(same_rect(obstacle_rect(K_BLOCK, 2, 0), spr::spire), "flow: cars", "volcano BLOCK is not the spire");
    int count[LIGHT_MODES] = {0};
    bool stable = true;
    for(int i = 0; i < 4000; i++) {
        float s = 10.0f + float(i) * 3.7f;
        int m = light_mode(s);
        stable &= m == light_mode(s);
        if(m >= 0 && m < LIGHT_MODES) count[m]++;
    }
    bool all = true;
    for(int m = 0; m < LIGHT_MODES; m++) all &= count[m] > 200;
    expect(all && stable, "flow: headlights", "modes off %d dipped %d full %d flashing %d of 4000, stable %d", count[0],
           count[1], count[2], count[3], stable);
    // a spawned car keeps its mode in o.art
    fresh_save();
    to_running();
    int cars = 0, right = 0;
    for(int i = 0; i < 3000 && cars < 20; i++) {
        step();
        for(auto &o : objs)
            if(o.kind == K_BLOCK && o.biome < 2 && !o.done) {
                cars++;
                right += o.art == light_mode(o.s);
                o.done = true;
            }
    }
    expect(cars > 0 && right == cars, "flow: headlights", "%d of %d cars carry their light mode", right, cars);
    // rendering a lit car leaves the global alpha at 255
    ::render(clock_ms);
    expect(screen.alpha == 255, "flow: headlights", "alpha left at %d after drawing", int(screen.alpha));

    // full beams glow round each lamp: no flat streak of light sideways
    // across the screen (it read as a stray line on the results screen)
    for(int biome = 0; biome < 2; biome++) {
        Obj car{};
        car.kind = K_BLOCK;
        car.biome = uint8_t(biome);
        car.x = 0.0f;
        float z = Z_PLAYER + 7.0f;  // near, but before the dazzle starts
        screen.alpha = 255;
        screen.pen = Pen(0, 0, 0);
        screen.clear();
        draw_glow(car, z, true);
        Screen l = project(-LAMP_X, lamp_height(biome), z);
        Screen r = project(LAMP_X, lamp_height(biome), z);
        int reach = int(8.0f * l.scale) + 1;  // the outer halo's radius
        int y = int(l.y), lit_outside = 0;
        for(int x = 0; x < SW; x++) {
            bool near_lamp = abs(x - int(l.x)) <= reach || abs(x - int(r.x)) <= reach;
            if(!near_lamp && framebuffer[y * SW + x] != 0) lit_outside++;
        }
        expect(lit_outside == 0, "flow: headlights", "biome %d: %d pixels lit on the lamp row outside the halos", biome,
               lit_outside);
    }
}

// Every paintable colour is one palette entry of the sprite sheet, and
// changing a style touches that obstacle's entries and nothing else.
void flow_style_paint() {
    Surface *sh = screen.sprites;
    expect(sh && sh->palette, "flow: style palette", "sprite sheet is not paletted");
    if(!sh || !sh->palette) return;
    reset_style();
    apply_style();
    Pen before[256];
    memcpy(before, sh->palette, sizeof(before));
    for(int item = 0; item < paint::ITEMS; item++) {
        for(int v = 1; v < paint::VARIANTS; v++) {
            style.v[item] = uint8_t(v);
            apply_style();
            const auto &it = paint::items[item];
            bool ok = true;
            for(int k = 0; k < it.count; k++) {
                uint32_t c = it.rgb[v][k];
                Pen p = sh->palette[paint_slot[item][k]];
                ok &= p.r == ((c >> 16) & 255) && p.g == ((c >> 8) & 255) && p.b == (c & 255) && p.a == 255;
            }
            int others = 0;
            for(int i = 0; i < 256; i++) {
                bool mine = false;
                for(int k = 0; k < it.count; k++) mine |= paint_slot[item][k] == i;
                if(!mine && memcmp(&sh->palette[i], &before[i], sizeof(Pen)) != 0) others++;
            }
            expect(ok && others == 0, "flow: style palette", "%s variant %d: own colours %s, %d other entries changed",
                   it.name, v, ok ? "ok" : "wrong", others);
        }
        style.v[item] = 0;
        apply_style();
    }
    expect(memcmp(before, sh->palette, sizeof(before)) == 0, "flow: style palette", "variant 0 does not restore the sheet");
}

// The STYLE menu: from the title, up/down picks an obstacle, left/right its
// colour; the choice is saved and survives a restart; bad values reset.
void flow_style_menu() {
    fresh_save();
    to_title();
    steps(5);
    step(Button::DPAD_DOWN);
    step(Button::DPAD_DOWN);
    step(Button::A);
    expect(state == State::STYLE, "flow: style menu", "title item 3 did not open STYLE (state %d)", int(state));
    step(Button::DPAD_DOWN);  // second item
    step(Button::DPAD_RIGHT);
    step(Button::DPAD_RIGHT);
    step(Button::DPAD_DOWN);  // third item
    step(Button::DPAD_LEFT);  // wraps to 3
    expect(style.v[1] == 2 && style.v[2] == 3 && style.v[0] == 0, "flow: style menu", "styles %d %d %d (want 0 2 3)",
           style.v[0], style.v[1], style.v[2]);
    step(Button::B);
    expect(state == State::TITLE && menu_cursor == 2, "flow: style menu", "B did not return to the title on STYLE");
    memset(&style, 0, sizeof(style));
    load_game();
    expect(style.v[1] == 2 && style.v[2] == 3, "flow: style saved", "after reload %d %d (want 2 3)", style.v[1], style.v[2]);
    style.v[4] = 9;
    store_style();
    load_game();
    expect(style.v[4] == 0 && style.v[1] == 2, "flow: style repair", "a bad style kept (%d) or good ones lost", style.v[4]);
    files.erase("save1");
    load_game();
    bool zero = true;
    for(auto v : style.v) zero &= v == 0;
    expect(zero && save.seen_help == 1, "flow: style missing", "no style file must mean the original colours");
    fresh_save();
    apply_style();
}

// The start-of-run camera move: PLAY on the title plays it, any button
// skips it, it ends in the countdown, and restarts go straight to it.
void flow_intro() {
    fresh_save();
    to_title();
    steps(5);
    step(Button::A);
    expect(state == State::INTRO, "flow: intro", "PLAY did not start the intro (state %d)", int(state));
    float d0 = run.distance;
    int n = 0;
    bool alpha_ok = true;
    while(state == State::INTRO && n < 1000) {
        step();
        ::render(clock_ms);
        alpha_ok &= screen.alpha == 255 && finite_all();
        n++;
    }
    expect(state == State::READY && n == INTRO_TICKS, "flow: intro", "intro took %d updates, then state %d (want %d, READY)", n,
           int(state), INTRO_TICKS);
    // the whole move runs a little slower than the first 3 s version (about
    // 1.3x), every part alike, so the line keeps its share of the time
    int shown = 0;
    for(int t = 0; t < INTRO_TICKS; t++) shown += intro_line_shown(t);
    expect(INTRO_TICKS >= 370 && INTRO_TICKS <= 410 && shown * 100 >= INTRO_TICKS * 25 && shown * 100 <= INTRO_TICKS * 30,
           "flow: intro pace", "intro %d updates (want 370-410), '%s' shown %d of them (want 25-30%%)", INTRO_TICKS, INTRO_LINE,
           shown);
    expect(run.distance == d0 && alpha_ok, "flow: intro", "the run moved during the intro or drawing left alpha/NaN");
    // the hills and the sun move smoothly through the intro and into the
    // countdown: no jump when the camera hands over to the run
    to_title();
    steps(5);
    step(Button::A);
    float prev = scenery_scroll(), worst = 0.0f;
    int worst_at = 0;
    for(int i = 0; i < INTRO_TICKS + 30; i++) {
        step();
        float now = scenery_scroll();
        if(fabsf(now - prev) > worst) {
            worst = fabsf(now - prev);
            worst_at = i;
        }
        prev = now;
    }
    expect(worst <= 6.0f, "flow: intro smooth", "the scenery jumps %.1f px in one update (update %d of the intro, which has %d)",
           double(worst), worst_at, INTRO_TICKS);
    // skip
    to_title();
    steps(5);
    step(Button::A);
    steps(10);
    step(Button::A);
    expect(state == State::READY, "flow: intro skip", "A did not skip the intro (state %d)", int(state));
    // a restart from the results screen goes straight to the countdown
    to_running();
    crash_now();
    steps(200);
    step(Button::A);
    expect(state == State::READY, "flow: intro restart", "restart went to state %d, not READY", int(state));
    fresh_save();
}

// Dust from the shoes, landing puffs and dash flames start at the runner's
// feet in every lane, not at the middle of the screen (the camera follows
// the runner only part of the way, so side lanes sit off centre).
void flow_dust_at_feet() {
    for(int lane_dir : {-1, 1}) {
        fresh_save();
        to_running();
        st_ghost = true;  // nothing may end the run while we watch the feet
        step(lane_dir < 0 ? Button::DPAD_LEFT : Button::DPAD_RIGHT);
        steps(60);
        float feet = player_screen_x();
        int far = 0, seen = 0;
        for(int i = 0; i < 300; i++) {
            for(auto &p : particles) p.life = 0.0f;
            st_ghost = true;
            step(i == 100 ? Button::A : 0);  // a jump gives a landing puff too
            for(auto &p : particles) {
                if(p.life <= 0.0f) continue;
                seen++;
                if(fabsf(p.x - feet) > 7.0f) far++;
            }
            feet = player_screen_x();
        }
        expect(seen > 10 && far == 0 && fabsf(feet - 64.0f) > 4.0f, "flow: dust at feet",
               "lane %+d: %d of %d particles away from the feet at x %.0f", lane_dir, far, seen, double(feet));
    }
    st_ghost = false;
    fresh_save();
}

// Jumps and slides sometimes use their alternate animation (star jump,
// knee slide), never always and never never.
void flow_alt_moves() {
    fresh_save();
    to_running();
    int jumps = 0, jump_alts = 0, slides = 0, slide_alts = 0;
    for(int k = 0; k < 80; k++) {
        st_ghost = true;
        bool jump = k % 2 == 0;
        step(jump ? Button::A : Button::B);
        bool alt = false, normal = false;
        for(int i = 0; i < 70; i++) {
            st_ghost = true;
            const Rect *r = player_frame(clock_ms);
            if(jump) {
                alt |= r == &spr::jump_up_alt || r == &spr::jump_alt;
                normal |= r == &spr::jump_up || r == &spr::jump;
            } else {
                alt |= r == &spr::slide_alt[0] || r == &spr::slide_alt[1];
                normal |= r == &spr::slide || r == &spr::slide2;
            }
            step();
        }
        if(alt == normal) continue;  // not a clean jump or slide (both or neither shown)
        if(jump) { jumps++; jump_alts += alt; }
        else { slides++; slide_alts += alt; }
    }
    expect(jumps > 30 && slides > 30 && jump_alts * 6 > jumps && jump_alts * 2 < jumps && slide_alts * 6 > slides &&
               slide_alts * 2 < slides,
           "flow: alternate moves", "star jumps %d of %d, knee slides %d of %d", jump_alts, jumps, slide_alts, slides);
    st_ghost = false;
    fresh_save();
}

void flow_text_fit() {
    auto w = [](const char *s) { return screen.measure_text(s, minimal_font).w; };
    for(auto &it : paint::items)
        expect(w(it.name) <= 80, "flow: text fit", "style name '%s' is %d px", it.name, w(it.name));
    int dw = screen.measure_text(INTRO_LINE, outline_font).w;
    expect(dw <= SW - 4, "flow: text fit", "'%s' is %d px", INTRO_LINE, dw);
    char a[40], b[40];
    snprintf(a, sizeof(a), "%lu", 99999999ul);
    expect(3 + w(a) < SW - 47, "flow: text fit", "score %s runs into the heat bar", a);
    snprintf(a, sizeof(a), "FURTHEST %luM", 999999ul);
    expect(w(a) <= SW - 2, "flow: text fit", "settings line '%s' is %d px", a, w(a));
    snprintf(a, sizeof(a), "%dM   %lu CHILIS", 99999, 9999ul);
    expect(w(a) <= SW - 2, "flow: text fit", "results line '%s' is %d px", a, w(a));
    for(int type = 0; type < M_TYPES; type++)
        for(int level = 0; level < 60; level++) {
            Mission m{uint8_t(type), uint8_t(level), 0, mission_target(uint8_t(type), uint8_t(level))};
            mission_text(m, a, sizeof(a));
            snprintf(b, sizeof(b), "%lu", (unsigned long)(m.target - 1));
            if(7 + w(a) + 3 > SW - 7 - w(b)) {
                fail("flow: text fit", "results: '%s' runs into '%s' (level %d)", a, b, level + 1);
                if(verbose) fprintf(stderr, "fit: %s | %s L%d\n", a, b, level + 1);
                level = 60;
            }
            if(w(a) > SW - 4) fail("flow: text fit", "mission '%s' is %d px", a, w(a));
        }
}

void flow_resume() {
    fresh_save();
    if(!to_running()) return;
    st_ghost = true;
    steps(100);
    step(Button::MENU);
    expect(state == State::PAUSE, "flow: pause", "MENU did not pause (state %d)", int(state));
    step(Button::B);
    float d = run.distance;
    Body b = run.p;
    int held = 0;
    while(state == State::RESUME && held < 500) {
        step();
        held++;
        if(state == State::RESUME && (run.distance != d || !body_equal(b, run.p))) {
            fail("flow: resume", "the world moved during the resume countdown");
            break;
        }
    }
    st_ghost = false;
    expect(state == State::RUN && held == 90, "flow: resume", "resume countdown took %d updates (state %d)", held, int(state));
}

// Hours into a run: distances keep their precision.
void flow_precision() {
    fresh_save();
    if(!to_running()) return;
    warp(200000.0f);
    st_ghost = true;
    double worst = 0.0, prev = double(road_origin) + double(run.distance);
    float biggest = 0.0f;
    for(int i = 0; i < 6000; i++) {
        step();
        double now_d = double(road_origin) + double(run.distance);
        double want = double(run.speed) * double(DT);
        worst = std::max(worst, fabs((now_d - prev) - want) / want);
        prev = now_d;
        biggest = std::max(biggest, run.distance);
    }
    st_ghost = false;
    char buf[96];
    snprintf(buf, sizeof(buf), "%.1e worst step error at %.0f m, local distance below %.0f", worst, prev, double(biggest));
    notes["precision"] = buf;
    expect(worst < 1e-3 && biggest < 2.0f * REBASE + 1.0f, "flow: distance precision", "%s", buf);
    expect(biome_at(run.distance) == int((uint32_t(prev) / 900u) % 3u), "flow: distance precision", "biome wrong after a rebase");
}

// ---------------------------------------------------------------------------
// Scenes: deterministic game states rendered to images (--frames) and timed
// (--bench). No cheats in the game itself: the test sets the state.

struct Scene {
    const char *name;
    void (*setup)();
};

void scene_run_at(float d, int updates) {
    fresh_save();
    save.best = 18000;
    save.missions_done = 7;
    clock_ms = 50000;
    to_running();
    if(d > 0.0f) warp(d);
    st_ghost = true;
    steps(updates);
}

// Three parked cars at fixed distances: full beam, dipped and off.
void scene_cars(float d) {
    scene_run_at(d, 1);
    for(auto &o : objs) o = Obj{};
    struct { int lane; float ahead; uint8_t mode; } cars[3] = {{0, 10.0f, LIGHT_FULL}, {2, 15.0f, LIGHT_DIPPED}, {1, 20.0f, LIGHT_OFF}};
    for(auto &c : cars) {
        Obj *o = spawn(K_BLOCK, lane_x(c.lane), run.distance + c.ahead);
        if(o) o->art = c.mode;
    }
    for(int i = 0; i < 40; i++) {
        st_ghost = true;
        step();
    }
}

const Scene scenes[] = {
    {"title", [] { fresh_save(); save.best = 12345; save.missions_done = 7; clock_ms = 9000; to_title(); steps(300); }},
    {"help", [] { set_state(State::HELP); }},
    {"missions", [] { set_state(State::MISSIONS); }},
    {"settings", [] { save.runs = 123; save.total_chilis = 99999; save.best_distance = 12345; set_state(State::SETTINGS); }},
    {"ready", [] { fresh_save(); begin_run(); steps(50); }},
    {"first_run", [] { scene_run_at(0.0f, 600); }},
    {"desert_fast", [] { scene_run_at(600.0f, 300); }},
    {"biome_blend", [] { scene_run_at(860.0f, 150); }},
    {"city_banner", [] { scene_run_at(895.0f, 60); }},
    {"city", [] { scene_run_at(1150.0f, 300); }},
    {"volcano", [] { scene_run_at(2000.0f, 300); }},
    {"volcano_far", [] { scene_run_at(20000.0f, 300); }},
    {"dash", [] { scene_run_at(1200.0f, 100); run.heat = 1.0f; step(Button::Y); steps(60); }},
    {"magnet", [] { scene_run_at(300.0f, 100); run.magnet_t = MAGNET_TIME; steps(120); }},
    {"shield", [] { scene_run_at(400.0f, 100); run.shield = true; steps(30); }},
    {"heat_full", [] { scene_run_at(500.0f, 100); run.heat = 1.0f; run.combo = 23; steps(10); }},
    {"popups", [] {
         scene_run_at(700.0f, 100);
         run.combo = 19;
         Obj c{};
         c.kind = K_CHILI;
         collect(c);
         popup("MISSION!", 64, 46, Pen(120, 255, 140));
         popup("NEW BEST!", 64, 46, Pen(255, 240, 120));
         popup("CLOSE!", 64, 70, Pen(120, 230, 255));
         steps(10);
     }},
    {"style", [] { fresh_save(); style_item = 6; style.v[6] = 1; apply_style(); set_state(State::STYLE); steps(30); }},
    {"style_racer", [] { style_item = 7; style.v[7] = 3; apply_style(); steps(10); }},
    {"intro_close", [] { style.v[6] = 0; style.v[7] = 0; apply_style(); to_title(); steps(5); step(Button::A); steps(60); }},
    {"intro_shades", [] { steps(INTRO_HIT - 60 - 20); }},
    {"intro_deal", [] { steps(30); }},
    {"intro_turn1", [] { steps(INTRO_ORBIT - INTRO_HIT - 10 + 45); }},
    {"intro_turn2", [] { steps(25); }},
    {"intro_turn3", [] { steps(25); }},
    {"cars_desert", [] { scene_cars(300.0f); }},
    {"cars_city", [] { scene_cars(1100.0f); }},
    {"crash", [] { scene_run_at(150.0f, 100); save.missions[0] = {M_DISTANCE, 0, 0, 200}; crash_now(); steps(15); }},
    {"results", [] { steps(250); }},
    {"pause", [] { scene_run_at(1000.0f, 200); step(Button::MENU); }},
    {"resume", [] { step(Button::B); steps(40); }},
};

uint32_t frame_hash() {
    uint32_t h = 2166136261u;
    for(auto px : framebuffer) h = (h ^ px) * 16777619u;
    return h;
}

void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if(!f) return;
    fprintf(f, "P6 128 128 255\n");
    for(auto px : framebuffer) {
        uint8_t rgb[3] = {uint8_t((px & 31) << 3), uint8_t(((px >> 5) & 63) << 2), uint8_t((px >> 11) << 3)};  // blue in the high bits
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

void render_scenes(const char *dir, bool bench) {
    double total[5] = {0};
    if(bench) printf("render on this machine, us per frame: scene  total  sky road sprites effects hud\n");
    for(auto &sc : scenes) {
        sc.setup();
        st_ghost = false;
        uint32_t t = clock_ms;
        ::render(t);
        if(dir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s.ppm", dir, sc.name);
            write_ppm(path);
            printf("%-12s %08x\n", sc.name, frame_hash());
        }
        if(bench) {
            for(auto &p : st_phase_us) p = 0.0;
            const int n = 400;
            for(int i = 0; i < n; i++) ::render(t + uint32_t(i) * 20u);
            double sum = 0;
            for(auto p : st_phase_us) sum += p;
            printf("  %-12s %6.1f  %5.1f %5.1f %5.1f %5.1f %5.1f\n", sc.name, sum / n, st_phase_us[0] / n,
                   st_phase_us[1] / n, st_phase_us[2] / n, st_phase_us[3] / n, st_phase_us[4] / n);
            for(int k = 0; k < 5; k++) total[k] += st_phase_us[k] / n;
        }
    }
    if(bench) {
        double sum = 0;
        for(auto v : total) sum += v;
        int ns = int(sizeof(scenes) / sizeof(scenes[0]));
        printf("  %-12s %6.1f  %5.1f %5.1f %5.1f %5.1f %5.1f\n", "mean", sum / ns, total[0] / ns, total[1] / ns,
               total[2] / ns, total[3] / ns, total[4] / ns);
    }
}

// --clip DIR: a run from `d` metres, one image per drawn frame (every 20 ms),
// to compare how the road reads in motion. The runner cannot crash here.
// A negative `d` starts from the title instead: the intro, then the run.
void render_clip(const char *dir, float d, int frames) {
    if(d < 0.0f) {
        fresh_save();
        to_title();
        steps(5);
        step(Button::A);
    } else {
        scene_run_at(d, 1);
    }
    for(int i = 0; i < frames; i++) {
        st_ghost = true;
        steps(2);
        ::render(clock_ms);
        char path[512];
        snprintf(path, sizeof(path), "%s/clip%04d.ppm", dir, i);
        write_ppm(path);
    }
    st_ghost = false;
}

void flows() {
    flow_resume();
    flow_precision();
    flow_countdown();
    flow_crash_tick_mission();
    flow_totals();
    flow_results_guard();
    flow_attract();
    flow_stall();
    flow_saves();
    flow_close_calls();
    flow_popups();
    flow_fast_fall();
    flow_text_fit();
    flow_readability();
    flow_cars();
    flow_style_paint();
    flow_style_menu();
    flow_intro();
    flow_dust_at_feet();
    flow_alt_moves();
    st_ghost = false;
    fresh_save();
}

}  // namespace st
}  // namespace

namespace {
namespace st {

// One process's results, passed to the parent through a pipe as text.
void write_results(FILE *f) {
    fprintf(f, "S %.17g %.17g %.17g %.9g %.17g %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %d %d %ld %ld %d %ld\n",
            stats.sim_seconds, stats.dash_seconds, stats.distance, double(stats.max_speed), stats.plan_ms, stats.runs,
            stats.decisions, stats.rescues, stats.ghost_ticks, stats.bot_hits, stats.impossible, stats.coarse_only,
            stats.fidelity, stats.dashes, stats.chilis, stats.close_calls, stats.smashed, max_live_objs, st_pool_full,
            chilis_spawned, powerups_spawned, flow_checks, r_max);
    for(auto &x : failures) fprintf(f, "F\t%s\t%d\t%s\n", x.first.c_str(), x.second.count, x.second.first.c_str());
    for(auto &n : notes) fprintf(f, "N\t%s\t%s\n", n.first.c_str(), n.second.c_str());
}

void read_results(FILE *f) {
    char line[1200];
    while(fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if(line[0] == 'S') {
            Stats s;
            double ms;
            int live, full, checks;
            long cs, ps, rm;
            if(sscanf(line + 2, "%lf %lf %lf %lf %lf %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %d %d %ld %ld %d %ld",
                      &s.sim_seconds, &s.dash_seconds, &s.distance, &ms, &s.plan_ms, &s.runs, &s.decisions, &s.rescues,
                      &s.ghost_ticks, &s.bot_hits, &s.impossible, &s.coarse_only, &s.fidelity, &s.dashes, &s.chilis,
                      &s.close_calls, &s.smashed, &live, &full, &cs, &ps, &checks, &rm) != 23)
                continue;
            stats.sim_seconds += s.sim_seconds; stats.dash_seconds += s.dash_seconds; stats.distance += s.distance;
            stats.max_speed = std::max(stats.max_speed, float(ms)); stats.plan_ms += s.plan_ms; stats.runs += s.runs;
            stats.decisions += s.decisions; stats.rescues += s.rescues; stats.ghost_ticks += s.ghost_ticks;
            stats.bot_hits += s.bot_hits; stats.impossible += s.impossible; stats.coarse_only += s.coarse_only;
            stats.fidelity += s.fidelity; stats.dashes += s.dashes; stats.chilis += s.chilis;
            stats.close_calls += s.close_calls; stats.smashed += s.smashed;
            max_live_objs = std::max(max_live_objs, live); st_pool_full += full; chilis_spawned += cs;
            powerups_spawned += ps; flow_checks += checks; r_max = std::max(r_max, rm);
        } else if(line[0] == 'F') {
            char *key = line + 2, *count = strchr(key, '\t');
            if(!count) continue;
            *count++ = 0;
            char *first = strchr(count, '\t');
            if(!first) continue;
            *first++ = 0;
            auto &x = failures[key];
            if(x.count == 0) x.first = first;
            x.count += atoi(count);
        } else if(line[0] == 'N') {
            char *key = line + 2, *val = strchr(key, '\t');
            if(!val) continue;
            *val++ = 0;
            notes[key] = val;
        }
    }
}

}  // namespace st
}  // namespace

#ifdef __unix__
#include <sys/wait.h>
#include <unistd.h>
#endif

int chili_selftest(int argc, char **argv) {
    int seeds = 24, jobs = 1;
    const char *frames_dir = nullptr, *clip_dir = nullptr;
    float clip_from = 600.0f;
    bool bench = false, flows_only = false;
#ifdef __unix__
    jobs = std::max(1, std::min(16, int(sysconf(_SC_NPROCESSORS_ONLN))));
#endif
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-v")) st::verbose = true;
        else if(!strcmp(argv[i], "--seeds") && i + 1 < argc) seeds = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--jobs") && i + 1 < argc) jobs = std::max(1, atoi(argv[++i]));
        else if(!strcmp(argv[i], "--quick")) seeds = 4;
        else if(!strcmp(argv[i], "--frames") && i + 1 < argc) frames_dir = argv[++i];
        else if(!strcmp(argv[i], "--bench")) bench = true;
        else if(!strcmp(argv[i], "--clip") && i + 1 < argc) clip_dir = argv[++i];
        else if(!strcmp(argv[i], "--clip-from") && i + 1 < argc) clip_from = float(atof(argv[++i]));
        else if(!strcmp(argv[i], "--flows")) flows_only = true;
        else {
            printf("usage: chili-rush-selftest [--seeds N] [--quick] [--jobs N] [-v] [--flows] [--frames DIR] [--bench] [--clip DIR [--clip-from M]]\n");
            return 2;
        }
    }
    jobs = std::min(jobs, std::max(1, seeds));
    auto t0 = std::chrono::steady_clock::now();
    ::init();
    save.seen_help = 1;
    if(clip_dir) {
        st::render_clip(clip_dir, clip_from, clip_from < 0.0f ? 400 : 250);
        return 0;
    }
    if(frames_dir || bench) {
        st::render_scenes(frames_dir, bench);
        return 0;
    }

    auto work = [&](int job) {
        if(job == 0) st::flows();
        if(!flows_only) {
            if(job == 0) st::detector_check();
            st::fairness(seeds, job, jobs);
        }
    };
#ifdef __unix__
    if(jobs > 1) {
        fflush(stdout);
        std::vector<std::pair<pid_t, FILE *>> kids;
        for(int j = 0; j < jobs; j++) {
            int fd[2];
            if(pipe(fd) != 0) return 2;
            pid_t pid = fork();
            if(pid == 0) {
                close(fd[0]);
                work(j);
                FILE *f = fdopen(fd[1], "w");
                st::write_results(f);
                fclose(f);
                _exit(0);
            }
            close(fd[1]);
            kids.push_back({pid, fdopen(fd[0], "r")});
        }
        for(auto &k : kids) {
            st::read_results(k.second);
            fclose(k.second);
            int status = 0;
            waitpid(k.first, &status, 0);
            if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) st::fail("self-test process died", "status %d", status);
        }
    } else
#endif
        work(0);
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    st::notes["reachable set, largest"] = std::to_string(st::r_max) + " states";

    using st::stats;
    printf("Chili Rush self-test (%d seeds, %d processes)\n", seeds, jobs);
    printf("  fairness: %ld runs, %.0f s simulated (%.0f s in dash), %.0f m, top speed %.2f\n", stats.runs,
           stats.sim_seconds, stats.dash_seconds, stats.distance, double(stats.max_speed));
    printf("  bot: %ld decisions, %ld rescues from dead ends, %ld updates as a ghost, %ld crashes, %.1f ms/decision\n",
           stats.decisions, stats.rescues, stats.ghost_ticks, stats.bot_hits,
           stats.decisions ? stats.plan_ms / double(stats.decisions) : 0.0);
    printf("  play: %ld chilis, %ld dashes, %ld smashed, %ld close calls\n", stats.chilis, stats.dashes,
           stats.smashed, stats.close_calls);
    printf("  impossible configurations: %ld (plus %ld only at 0.05 s input steps, fine at 10 ms)\n", stats.impossible,
           stats.coarse_only);
    printf("  objects: %d live at most of %d, pool full %d times; %ld chilis and %ld power-ups checked\n",
           st::max_live_objs, MAX_OBJS, st_pool_full, st::chilis_spawned, st::powerups_spawned);
    printf("  flows and edge cases: %d checks\n", st::flow_checks);
    for(auto &n : st::notes) printf("  %s: %s\n", n.first.c_str(), n.second.c_str());
    if(st_pool_full) st::fail("object pool full", "%d spawns dropped", st_pool_full);
    printf("  wall time %.1f s\n", wall);
    if(st::failures.empty()) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    for(auto &f : st::failures) printf("  %s: %d times, first: %s\n", f.first.c_str(), f.second.count, f.second.first.c_str());
    return 1;
}
