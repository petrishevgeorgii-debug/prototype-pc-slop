// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: photo mode (1.1). F2 in the window, or L3 and R3 pressed together, opens it straight
// from the game (gpu/shim/port_trainer.cpp counts the requests); a second press closes it.
//
// Step 1 (this build): the pieces the game itself has for it, used from the game and logged.
//   * The game's own Hide HUD: fe_ToggleHideHUDOption (0x1bd70) flips a byte at 0x122f5c4 that
//     the HUD's elements ask about (0x1c060, 16 callers; the -HideHUD switch does the same).
//   * The game's clock: go_SlowMotion (0x68ba60) sets the time scale at [+0x44] of the object
//     0xbc00 returns ([0x122c618]). Nearly 0 stops the city: people, cars, debris in mid-air.
//   * The camera director ([0x135c5f0] the CameraManager, [+0x18] its director): its mode at
//     +0x34 (8 the game's cameras, 2 the developers' debug camera), the debug camera at +0x70
//     and the switch to it at +0x7cd (cm_EnableDebugCamera). The retail game never makes the
//     debug camera (its constructor, 0x103f10, is not called): the log says what is there.
// The photo mode screen (a screen of the game's own menus, scripts/proto_menu.py) and the free
// camera come next; docs/NATIVE.md.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game/guest.h"
#include "game/native_registry.h"
#include "game/p1/photo_mode.h"

namespace {

constexpr std::uint64_t kIsHudHidden = 0x1c060, kHideHud = 0x122f5c4;
constexpr std::uint64_t kClockOf = 0xbc00, kClock = 0x122c618;
constexpr std::uint64_t kCameraManager = 0x135c5f0;
constexpr float kFrozen = 0.001f; // a time scale of 0 is one the game's scripts never ask for

bool Matches(std::uint64_t at, std::initializer_list<std::uint8_t> bytes) {
    return std::memcmp(Guest::Image() + at, std::data(bytes), bytes.size()) == 0;
}

void Log(const char* text) {
    std::printf("Photo mode: %s\n", text);
    std::fflush(stdout);
}

bool Ready() {
    static const int ready = [] {
        const bool ok = Matches(kIsHudHidden, {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x0d, 0x59, 0x35, 0x21, 0x01, 0xb0,
                                               0x01, 0x80, 0x39, 0x00}) &&
                        Matches(kClockOf, {0x48, 0x8b, 0x05, 0x11, 0x0a, 0x22, 0x01, 0xc3});
        Log(ok ? "ready (F2, or L3 + R3 in the game)" : "off (the game's code is not where it was)");
        return ok ? 1 : 0;
    }();
    return ready != 0;
}

std::uint8_t* Clock() {
    return Guest::At<std::uint8_t*>(kClock);
}

std::uint8_t* Director() {
    auto* manager = Guest::At<std::uint8_t*>(kCameraManager);
    return manager ? Guest::Field<std::uint8_t*>(manager, 0x18) : nullptr;
}

void LogState(const char* when) {
    char text[256];
    auto* clock = Clock();
    auto* director = Director();
    std::snprintf(text, sizeof(text), "%s: HUD hidden %d, time scale %.3f, camera director %s, mode %d, debug camera %s, "
                  "debug switch %d",
                  when, Guest::At<std::uint8_t>(kHideHud), clock ? Guest::Field<float>(clock, 0x44) : -1.0f,
                  director ? "found" : "missing", director ? Guest::Field<std::int32_t>(director, 0x34) : -1,
                  director && Guest::Field<void*>(director, 0x70) ? "made" : "not made",
                  director ? Guest::Field<std::uint8_t>(director, 0x7cd) : -1);
    Log(text);
}

bool g_on = false;
std::uint8_t g_hud_was = 0;
float g_scale_was = 1.0f;
int g_frames = 0;
char g_screen_was[64] = "";

void Open() {
    auto* clock = Clock();
    g_hud_was = Guest::At<std::uint8_t>(kHideHud);
    Guest::At<std::uint8_t>(kHideHud) = 1;
    if (clock) {
        g_scale_was = Guest::Field<float>(clock, 0x44);
        Guest::Field<float>(clock, 0x44) = kFrozen;
    }
    g_on = true;
    g_frames = 0;
    LogState("opened (the HUD hidden, time stopped)");
}

void Close(const char* why) {
    auto* clock = Clock();
    Guest::At<std::uint8_t>(kHideHud) = g_hud_was;
    if (clock) Guest::Field<float>(clock, 0x44) = g_scale_was > 0.0f ? g_scale_was : 1.0f;
    g_on = false;
    LogState(why);
}

} // namespace

namespace Native {

void PhotoFrame(void*, const char* screen) {
    const TheOrderGameMenu* menu = Native::Menu();
    if (!menu || !menu->photo_open || !Ready()) return;
    if (menu->photo_open()) {
        if (g_on) {
            Close("closed (F2 or L3 + R3 again)");
        } else if (std::strcmp(screen, "IGHUD") == 0) {
            Open();
        } else {
            char text[96];
            std::snprintf(text, sizeof(text), "only from the game (screen %s)", *screen ? screen : "(none)");
            Log(text);
        }
    }
    if (!g_on) return;
    ++g_frames;
    if (g_frames == 30 || g_frames == 300) LogState(g_frames == 30 ? "half a second later" : "5 seconds later");
    // What the game does meanwhile: a menu over it (pause, map), or the time scale changed back.
    if (std::strcmp(screen, g_screen_was) != 0) {
        std::snprintf(g_screen_was, sizeof(g_screen_was), "%s", screen);
        if (g_frames > 1) {
            char text[96];
            std::snprintf(text, sizeof(text), "screen %s while open", *screen ? screen : "(none)");
            Log(text);
        }
    }
    auto* clock = Clock();
    if (clock && Guest::Field<float>(clock, 0x44) != kFrozen) {
        static int logged = 0;
        if (logged++ < 3) LogState("the game changed the time scale");
    }
}

} // namespace Native
