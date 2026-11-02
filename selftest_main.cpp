// Headless host for the Chili Rush self-test (cmake -DCHILI_SELFTEST=ON).
//
// Stands in for the SDL/console firmware: a fake clock, an RGB565 frame
// buffer like the VGC Zero's, and the save file kept in memory so tests can
// corrupt it. No window and no sound. The tests themselves are in
// selftest.hpp, compiled into game.cpp so they reach the game's internals.

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "32blit.hpp"
#include "engine/api_private.hpp"
#include "engine/api_version.h"

using namespace blit;

namespace selftest_host {
uint32_t clock_ms = 1000;
uint32_t clock_us = 1000000;
std::map<std::string, std::vector<char>> files;
int save_writes = 0;
uint16_t framebuffer[128 * 128];
}

using namespace selftest_host;

namespace {

uint32_t host_now() { return clock_ms; }
uint32_t host_random() { static uint32_t x = 2463534242u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
void host_debug(const char *message) { fputs(message, stderr); }
uint32_t host_us() { return clock_us; }
uint32_t host_max_us() { return 0xffffffffu; }
const char *host_save_path() { return ""; }
bool host_storage() { return true; }

bool host_set_mode(ScreenMode, SurfaceTemplate &t) {
    t.data = reinterpret_cast<uint8_t *>(framebuffer);
    t.format = PixelFormat::RGB565;
    t.bounds = Size(128, 128);
    return true;
}

struct Handle {
    std::string name;
    int mode;
};

void *host_open(const std::string &name, int mode) {
    if(!(mode & OpenMode::write) && !files.count(name)) return nullptr;
    if(mode & OpenMode::write) files[name];  // create
    return new Handle{name, mode};
}
int32_t host_read(void *fh, uint32_t offset, uint32_t length, char *buffer) {
    auto &f = files[static_cast<Handle *>(fh)->name];
    if(offset >= f.size()) return 0;
    uint32_t n = std::min<uint32_t>(length, uint32_t(f.size()) - offset);
    memcpy(buffer, f.data() + offset, n);
    return int32_t(n);
}
int32_t host_write(void *fh, uint32_t offset, uint32_t length, const char *buffer) {
    auto &f = files[static_cast<Handle *>(fh)->name];
    if(f.size() < offset + length) f.resize(offset + length);
    memcpy(f.data() + offset, buffer, length);
    if(static_cast<Handle *>(fh)->name.rfind("save", 0) == 0) save_writes++;
    return int32_t(length);
}
int32_t host_close(void *fh) { delete static_cast<Handle *>(fh); return 0; }
uint32_t host_length(void *fh) { return uint32_t(files[static_cast<Handle *>(fh)->name].size()); }
bool host_exists(const std::string &name) { return files.count(name) > 0; }
bool host_dir_exists(const std::string &) { return true; }
bool host_mkdir(const std::string &) { return true; }
bool host_remove(const std::string &name) { return files.erase(name) > 0; }

APIConst make_api() {
    APIConst a{};
    a.version_major = BLIT_API_VERSION_MAJOR;
    a.version_minor = BLIT_API_VERSION_MINOR;
    a.now = host_now;
    a.random = host_random;
    a.debug = host_debug;
    a.open_file = host_open;
    a.read_file = host_read;
    a.write_file = host_write;
    a.close_file = host_close;
    a.get_file_length = host_length;
    a.file_exists = host_exists;
    a.directory_exists = host_dir_exists;
    a.create_directory = host_mkdir;
    a.remove_file = host_remove;
    a.get_save_path = host_save_path;
    a.is_storage_available = host_storage;
    a.get_us_timer = host_us;
    a.get_max_us_timer = host_max_us;
    a.set_screen_mode_format = host_set_mode;
    return a;
}

APIConst host_api_const = make_api();
APIData host_api_data;  // zero: static storage

} // namespace

namespace blit {
const APIConst &api = host_api_const;
APIData &api_data = host_api_data;
}

int chili_selftest(int argc, char **argv);  // selftest.hpp

int main(int argc, char **argv) { return chili_selftest(argc, argv); }
