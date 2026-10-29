// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the mouse in the game's own menus, with the PC version's cursor (docs/NATIVE.md).
//
// The menus are Scaleform GFx 2.x movies (the root, protomain.gfx, loads every screen). On PC the
// game told GFx where the mouse was and its movies have the mouse in them still: lists, toggles,
// sliders and buttons react to it (onRollOver, onRelease, onMouseWheel), and protomain has the PC
// cursor (mMouseCursor, the red arrow). The PS4 game never gives GFx a mouse, so none of it ran.
//
// Here the movie's Advance (GFxMovieRoot vtable slot 31, 0x3ff610), which runs every frame on the
// thread that drives the menus, first hands GFx the mouse (gpu/shim/port_pointer.cpp: the place in
// the game's picture, the buttons, the wheel): once SetMouseCursorCount(1) (slot 46: GFx ignores
// mice beyond the count, which the PS4 game leaves at 0) and a SetFocus event (HandleEvent, slot
// 36, drops other events for an unfocused movie), then NotifyMouseState (slot 38) every frame and a
// MouseWheel event per notch. The position is in the pixels of the rect the movie is drawn in (its
// viewport at +0x64: buffer width, height, then that rect's left, top, width, height).
//
// Whether the menus are up is the screen on top of protomain's screen stack, read with the movie's
// GetVariable (slot 13): IGHUD while playing (the mouse looks around), any other is a menu (the
// main menu, the pause menu, the map, loading...: it points). The PC cursor (mMouseCursor, which
// scripts/proto_menu.py makes _global.gMouseCursor for the controls to light up) is shown and
// moved with SetVariable (slot 12). (protomain's own frame script asking the port, as before, did
// not run once the game had been played.)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game/guest.h"
#include "game/native_registry.h"
#include "game/p1/trainer.h"

namespace {

constexpr std::uint64_t kAdvance = 0x3ff610, kAdvanceSlot = 0x1065d30 + 31 * 8;
constexpr std::uint64_t kHandleEvent = 0x402250, kNotifyMouseState = 0x402ad0, kSetMouseCursorCount = 0xad0410;

/// GFxEvent / GFxMouseEvent as HandleEvent reads them.
struct GfxEvent {
    std::uint32_t type;      ///< 4 MouseWheel, 8 SetFocus
    std::uint8_t modifiers;
    std::uint8_t pad[3];
    float x, y, scroll;
    std::uint32_t button, mouse;
};
static_assert(sizeof(GfxEvent) == 0x1c, "GFxMouseEvent");
constexpr std::uint32_t kMouseWheel = 4, kSetFocus = 8;

/// GFxMovie::SetVariable(path, const GFxValue&, SetVarType) and GetVariable(GFxValue*, path).
constexpr std::uint64_t kSetVariable = 0x403ee0, kGetVariable = 0x404630;

/// GFxValue (GFx 2.x: 16 bytes).
struct GfxValue {
    enum : std::uint32_t { Undefined = 0, Null = 1, Boolean = 2, Number = 3, String = 4 };
    std::uint32_t type;
    std::uint32_t pad;
    union {
        double number;
        const char* string;
        std::uint8_t boolean;
        std::uint64_t raw;
    };
};
static_assert(sizeof(GfxValue) == 16, "GFxValue");

bool GetVariable(void* movie, const char* path, GfxValue* out) {
    *out = GfxValue{};
    return Guest::Call<std::uint8_t>(kGetVariable, movie, out, path) != 0;
}

void SetVariable(void* movie, const char* path, const GfxValue& value) {
    Guest::Call<std::uint8_t>(kSetVariable, movie, path, &value, 0u); // SV_Normal
}

GfxValue Bool(bool on) {
    GfxValue v{};
    v.type = GfxValue::Boolean;
    v.boolean = on ? 1 : 0;
    return v;
}

/// The screen on top of the movie's screen stack (protomain's screen manager) into `out`; false
/// for a movie without one.
bool TopScreen(void* movie, char* out, std::size_t size) {
    static const char kStack[] = "_global.managers.screen.arrScreenStack";
    GfxValue v;
    char path[96];
    std::snprintf(path, sizeof(path), "%s.length", kStack);
    if (!GetVariable(movie, path, &v) || v.type != GfxValue::Number) return false;
    const int count = int(v.number);
    out[0] = 0;
    if (count <= 0) return true;
    std::snprintf(path, sizeof(path), "%s.%d", kStack, count - 1);
    if (GetVariable(movie, path, &v) && v.type == GfxValue::String && v.string)
        std::snprintf(out, size, "%s", v.string);
    return true;
}

/// The movie's viewport (GFxViewport at +0x64): its render buffer and the rect the movie is drawn
/// in. GFx takes the mouse in the rect's pixels (NotifyMouseState maps them to the stage the way
/// the movie is drawn): the buffer can be smaller (the PS4 game says 1280x720 while the menus are
/// drawn at the render resolution, 2560x1440 with that patch: the mouse reached only the top left
/// quarter).
struct Viewport {
    std::int32_t buffer_width, buffer_height, left, top, width, height;
};

Viewport ViewportOf(const void* movie) {
    Viewport v;
    std::memcpy(&v, static_cast<const std::uint8_t*>(movie) + 0x64, sizeof(v));
    return v;
}

/// Movies seen (the root and any other): each gets its cursor count once, and is told when the
/// mouse leaves it.
struct Movie {
    void* movie;
    bool pointed;
    Viewport viewport;
    std::uint32_t buttons;
};
Movie g_movies[8];

void LogViewport(const void* movie, const Viewport& v, const char* what) {
    std::printf("Menu mouse: movie %p %s (viewport %d,%d %dx%d in a %dx%d buffer)\n", movie, what, v.left, v.top,
                v.width, v.height, v.buffer_width, v.buffer_height);
    std::fflush(stdout);
}

Movie* Seen(void* movie) {
    for (Movie& m : g_movies) {
        if (m.movie == movie) return &m;
        if (!m.movie) {
            m.movie = movie;
            Guest::Call<void>(kSetMouseCursorCount, movie, 1u);
            m.viewport = ViewportOf(movie);
            LogViewport(movie, m.viewport, "takes the mouse");
            return &m;
        }
    }
    return nullptr;
}

// A left click skips the movie playing (the intros, the story's cutscenes), as on PC: the game's
// fmv_Skip (its Lua's, 0x19840) when fmv_IsPlaying (0x19890) would say so: the player is set up
// (0x122f3b8), a movie is playing (0x122f358) and loaded (0x122f3c0), not skipped yet (0x122f3c8).
constexpr std::uint64_t kFmvSkip = 0x19840, kFmvIsPlaying = 0x19890;
constexpr std::uint64_t kFmvReady = 0x122f3b8, kFmvPlaying = 0x122f358, kFmvMovie = 0x122f3c0,
                        kFmvSkipped = 0x122f3c8;
bool g_skip = false;

void SkipMovie(const TheOrderGameMenu* menu) {
    if (!g_skip || !menu || !menu->pointer_clicks || !menu->pointer_clicks()) return;
    if (!Guest::At<std::uint8_t>(kFmvReady) || !Guest::At<std::uint32_t>(kFmvPlaying) ||
        !Guest::At<std::uint64_t>(kFmvMovie) || Guest::At<std::uint8_t>(kFmvSkipped))
        return;
    Guest::Call<std::uint8_t>(kFmvSkip);
    std::printf("Menu mouse: the movie skipped (left click)\n");
    std::fflush(stdout);
}

/// protomain: the menus up or not from its top screen, and its PC cursor shown where the mouse is.
void Screens(void* movie, const TheOrderGameMenu* menu) {
    char screen[64];
    if (!menu || !menu->pointer_mode || !TopScreen(movie, screen, sizeof(screen))) return;
    static char last[64] = "?";
    if (std::strcmp(last, screen)) {
        std::snprintf(last, sizeof(last), "%s", screen);
        std::printf("Menu mouse: screen %s\n", *screen ? screen : "(none)");
        std::fflush(stdout);
    }
    Native::TrainerFrame(movie, screen);
    const bool up = *screen && std::strcmp(screen, "IGHUD") && std::strcmp(screen, "NONE");
    const bool shown = menu->pointer_mode(up ? 1 : 0) != 0;
    static int was_shown = -1;
    if (shown || was_shown != 0) SetVariable(movie, "_root.mMouseCursor._visible", Bool(shown));
    was_shown = shown ? 1 : 0;
    if (!shown) return;
    GfxValue x, y;
    if (GetVariable(movie, "_root._xmouse", &x) && x.type == GfxValue::Number &&
        GetVariable(movie, "_root._ymouse", &y) && y.type == GfxValue::Number) {
        SetVariable(movie, "_root.mMouseCursor._x", x);
        SetVariable(movie, "_root.mMouseCursor._y", y);
    }
    // The cursor's frame: 2 ('enable') while a control is under it (the controls' OnRollOver).
    GfxValue frame;
    static int over = -1;
    if (GetVariable(movie, "_root.mMouseCursor._currentframe", &frame) && frame.type == GfxValue::Number &&
        (frame.number == 2) != (over == 1)) {
        over = frame.number == 2 ? 1 : 0;
        std::printf("Menu mouse: %s at %.0f,%.0f on the stage\n", over ? "over a control" : "off the controls",
                    x.number, y.number);
        std::fflush(stdout);
    }
}

GUEST_ABI float Advance(void* movie, float delta, std::uint32_t catch_up) {
    const TheOrderGameMenu* menu = Native::Menu();
    SkipMovie(menu);
    Screens(movie, menu);
    TheOrderPointer p{};
    if (menu && menu->pointer_take) menu->pointer_take(&p);
    Movie* seen = p.active ? Seen(movie) : nullptr;
    if (seen) {
        auto* m = static_cast<std::uint8_t*>(movie);
        if (!m[0xc91]) { // not focused: GFx drops the wheel (and keys) of an unfocused movie
            GfxEvent focus{};
            focus.type = kSetFocus;
            Guest::Call<std::uint32_t>(kHandleEvent, movie, &focus);
        }
        const Viewport v = ViewportOf(movie);
        if (std::memcmp(&v, &seen->viewport, sizeof(v))) {
            seen->viewport = v;
            LogViewport(movie, v, "changed");
        }
        // The game's picture is the movie's rect (the menus cover the screen).
        const bool rect = v.width > 0 && v.height > 0;
        const float x = float(rect ? v.left : 0) + p.x * float(rect ? v.width : v.buffer_width);
        const float y = float(rect ? v.top : 0) + p.y * float(rect ? v.height : v.buffer_height);
        Guest::Call<void>(kNotifyMouseState, movie, x, y, p.buttons, 0u);
        if ((p.buttons & 1u) && !(seen->buttons & 1u)) {
            std::printf("Menu mouse: click at %.0f,%.0f (%.3f,%.3f of the picture)\n", x, y, p.x, p.y);
            std::fflush(stdout);
        }
        seen->buttons = p.buttons;
        for (int n = p.wheel; n != 0; n += n > 0 ? -1 : 1) {
            GfxEvent wheel{};
            wheel.type = kMouseWheel;
            wheel.x = x;
            wheel.y = y;
            wheel.scroll = n > 0 ? 1.0f : -1.0f;
            Guest::Call<std::uint32_t>(kHandleEvent, movie, &wheel);
        }
        seen->pointed = true;
    } else if (!p.active) {
        // Back in the game: the mouse leaves the movie (no item stays under it, nothing held).
        for (Movie& m : g_movies) {
            if (m.movie == movie && m.pointed) {
                Guest::Call<void>(kNotifyMouseState, movie, -10000.0f, -10000.0f, 0u, 0u);
                m.pointed = false;
            }
        }
    }
    return Guest::Call<float>(kAdvance, movie, delta, catch_up);
}

bool Matches(std::uint64_t at, std::initializer_list<std::uint8_t> bytes) {
    return std::memcmp(Guest::Image() + at, std::data(bytes), bytes.size()) == 0;
}

/// The vtable slot holds the relocated Advance: it now holds the native one. The functions it
/// calls are checked too (another build: the menus stay as they were).
bool InstallMenuMouse(std::uint64_t) {
    auto& slot = Guest::At<std::uintptr_t>(kAdvanceSlot);
    const auto image = reinterpret_cast<std::uintptr_t>(Guest::Image());
    if (slot != image + kAdvance) return false;
    if (!Matches(kHandleEvent, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xf3}) ||
        !Matches(kNotifyMouseState, {0x55, 0x41, 0x56, 0x53, 0x39, 0x97, 0x80, 0x0a, 0x00, 0x00}) ||
        !Matches(kSetMouseCursorCount, {0x83, 0xfe, 0x04, 0xb8, 0x04, 0x00, 0x00, 0x00, 0x0f, 0x46, 0xc6}) ||
        !Matches(kSetVariable, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                0x83, 0xec, 0x38, 0x41, 0x89, 0xcf}) ||
        !Matches(kGetVariable, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                0x83, 0xec, 0x38, 0x49, 0x89, 0xd5}))
        return false;
    slot = reinterpret_cast<std::uintptr_t>(&Advance);
    // The movie skip: fmv_Skip and fmv_IsPlaying as this build has them.
    g_skip = Matches(kFmvSkip, {0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0x3d, 0x74, 0x5b, 0x21, 0x01, 0x00, 0x74, 0x0e,
                                0x8a, 0x05, 0x74, 0x5b, 0x21, 0x01}) &&
             Matches(kFmvIsPlaying, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x8a, 0x05, 0x1b, 0x5b, 0x21, 0x01,
                                     0x84, 0xc0, 0x74, 0x23});
    if (!g_skip) {
        std::printf("Menu mouse: no movie skip (fmv_Skip is not where it was)\n");
        std::fflush(stdout);
    }
    return true;
}

NATIVE_REPLACEMENT({"mouse in the game's menus (GFxMovieRoot::Advance)", "menu", Native::Kind::Custom, kAdvance,
                    {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53}, nullptr,
                    nullptr, &InstallMenuMouse});

} // namespace
