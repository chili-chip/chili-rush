// Offline renderer for Chili Rush audio: drives audio::update() every 10 ms
// (the 32blit engine's update rate), pulls samples through the SDK mixer
// (blit::get_audio_frame) at 22050 Hz and writes WAV files to tools/out/.
// Also writes tools/out/events.csv: every music note-on (time, channel, Hz).
//
// Build (see tools/render.sh):
//   g++ -std=c++20 -O2 -I../32blit-sdk/32blit -I. tools/render_audio.cpp audio.cpp
//       ../32blit-sdk/32blit/audio/audio.cpp -o tools/render_audio
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "audio/audio.hpp"
#include "audio.hpp"

// Minimal stand-in for the SDK's api.cpp: the channel array.
namespace blit {
static AudioChannel channel_storage[CHANNEL_COUNT];
static AudioChannel *channel_ptr = channel_storage;
AudioChannel *const &channels = channel_ptr;
}

static std::vector<int16_t> pcm;
static uint32_t now_ms = 0;
static double sample_debt = 0.0;
static FILE *events = nullptr;
static const char *scene = "";

// one engine update (10 ms) and the samples that play until the next one
static void tick() {
    audio::update(now_ms);
    for(int c = 3; c <= 6; c++) {
        auto &ch = blit::channels[c];
        if(ch.adsr_phase == blit::ADSRPhase::ATTACK && ch.adsr_frame == 0 && events)
            fprintf(events, "%s,%u,%d,%u\n", scene, now_ms, c, ch.frequency);
    }
    sample_debt += blit::sample_rate * 0.010;
    while(sample_debt >= 1.0) {
        pcm.push_back(int16_t(int32_t(blit::get_audio_frame()) - 0x8000));
        sample_debt -= 1.0;
    }
    now_ms += 10;
}

static void run_ms(uint32_t ms) { for(uint32_t t = 0; t < ms; t += 10) tick(); }

static void write_wav(const std::string &path) {
    FILE *f = fopen(path.c_str(), "wb");
    if(!f) { perror(path.c_str()); exit(1); }
    uint32_t data = uint32_t(pcm.size() * 2), rate = blit::sample_rate, byte_rate = rate * 2;
    uint32_t riff = 36 + data, fmt_len = 16;
    uint16_t fmt = 1, chans = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&chans, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);

    // stats: peak, RMS, clipped samples, longest near-silence
    double sum = 0; int peak = 0; size_t clipped = 0, quiet = 0, longest_quiet = 0;
    for(int16_t s : pcm) {
        int a = std::abs(int(s));
        peak = a > peak ? a : peak;
        sum += double(s) * s;
        if(a >= 32767) clipped++;
        if(a < 100) { quiet++; if(quiet > longest_quiet) longest_quiet = quiet; } else quiet = 0;
    }
    double rms = std::sqrt(sum / double(pcm.size()));
    printf("%-14s %6.2fs  peak %5d (%6.1f dBFS)  rms %6.0f (%6.1f dBFS)  clipped %zu  longest gap %.0f ms\n",
           path.substr(path.rfind('/') + 1).c_str(), double(pcm.size()) / rate, peak,
           20 * std::log10(peak / 32768.0 + 1e-9), rms, 20 * std::log10(rms / 32768.0 + 1e-9), clipped,
           1000.0 * double(longest_quiet) / rate);
    pcm.clear();
}

static void reset(const char *name) {
    for(int c = 0; c < CHANNEL_COUNT; c++) blit::channels[c] = blit::AudioChannel();
    audio::init();
    audio::set_music(true);
    audio::set_sound(true);
    audio::set_intensity(0.0f);
    scene = name;
    now_ms = 0;
    sample_debt = 0.0;
}

int main(int argc, char **argv) {
    std::string out = argc > 1 ? argv[1] : "tools/out";
    events = fopen((out + "/events.csv").c_str(), "w");
    fprintf(events, "scene,ms,channel,hz\n");

    reset("title");
    audio::play(audio::Track::TITLE);
    run_ms(30000);
    write_wav(out + "/title.wav");

    // RUN: 20 s at intensity 0, then a 20 s ramp to 1
    reset("run");
    audio::play(audio::Track::RUN);
    run_ms(20000);
    for(int i = 0; i < 2000; i++) { audio::set_intensity(float(i) / 2000.0f); tick(); }
    write_wav(out + "/run.wav");

    reset("dash");
    audio::set_intensity(0.5f);
    audio::play(audio::Track::DASH);
    run_ms(8000);
    write_wav(out + "/dash.wav");

    // RUN -> DASH -> RUN, as in a game (the run picks up again after the dash)
    reset("run_dash_run");
    audio::play(audio::Track::RUN);
    run_ms(5000);
    audio::play(audio::Track::DASH);
    audio::sfx(audio::Sfx::DASH_START);
    run_ms(3000);
    audio::play(audio::Track::RUN);
    run_ms(5000);
    write_wav(out + "/run_dash_run.wav");

    reset("game_over");
    audio::play(audio::Track::RUN);
    run_ms(2000);
    audio::sfx(audio::Sfx::HIT);
    audio::play(audio::Track::GAME_OVER);
    run_ms(5000);
    audio::play(audio::Track::GAME_OVER); // must not restart
    run_ms(1000);
    write_wav(out + "/game_over.wav");

    // every sound effect alone, 0.8 s apart, then CHILI combo 1..10
    reset("sfx");
    run_ms(300);
    for(int s = 0; s <= int(audio::Sfx::MISSION); s++) {
        audio::sfx(audio::Sfx(s));
        run_ms(s == int(audio::Sfx::HIT) || s == int(audio::Sfx::NEW_BEST) ? 1200 : 800);
    }
    for(int c = 1; c <= 10; c++) { audio::sfx_combo(c); run_ms(160); }
    run_ms(600);
    write_wav(out + "/sfx.wav");

    // stress: RUN at full intensity with a pile of overlapping effects
    reset("stress");
    audio::set_intensity(1.0f);
    audio::play(audio::Track::RUN);
    run_ms(1000);
    for(int i = 0; i < 40; i++) {
        audio::sfx_combo(1 + i % 10);
        if(i % 3 == 0) audio::sfx(audio::Sfx::JUMP);
        if(i % 5 == 0) audio::sfx(audio::Sfx::SMASH);
        if(i % 7 == 0) audio::sfx(audio::Sfx::DASH_START);
        if(i == 20) audio::sfx(audio::Sfx::NEW_BEST);
        if(i == 30) audio::sfx(audio::Sfx::HIT);
        run_ms(100);
    }
    run_ms(1500);
    // toggles: music off must go silent at once, sound off too
    audio::set_music(false);
    audio::sfx(audio::Sfx::GOLDEN);
    run_ms(1000);
    audio::set_sound(false);
    audio::sfx(audio::Sfx::HIT);
    run_ms(1000);
    write_wav(out + "/stress.wav");

    // long renders that pass the loop points (TITLE loops at 33.1 s, RUN at 53.3 s)
    reset("title_long");
    audio::play(audio::Track::TITLE);
    run_ms(70000);
    write_wav(out + "/title_long.wav");
    reset("run_long");
    audio::play(audio::Track::RUN);
    run_ms(110000);
    write_wav(out + "/run_long.wav");

    fclose(events);
    return 0;
}
