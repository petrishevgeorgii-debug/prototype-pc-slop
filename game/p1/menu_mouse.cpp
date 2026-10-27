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
// MouseWheel event per notch. The position is in the movie's buffer pixels (its viewport at
// +0x64: buffer width, height, left, top, width, height...). scripts/proto_menu.py makes protomain
// draw its cursor there and tell the port when the menus are up (port_Pointer, options_menu.cpp).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game/guest.h"
#include "game/native_registry.h"

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

/// Movies seen (the root and any other): each gets its cursor count once, and is told when the
/// mouse leaves it.
struct Movie {
    void* movie;
    bool pointed;
};
Movie g_movies[8];

Movie* Seen(void* movie) {
    for (Movie& m : g_movies) {
        if (m.movie == movie) return &m;
        if (!m.movie) {
            m.movie = movie;
            Guest::Call<void>(kSetMouseCursorCount, movie, 1u);
            std::int32_t buffer[2];
            std::memcpy(buffer, static_cast<const std::uint8_t*>(movie) + 0x64, sizeof(buffer));
            std::printf("Menu mouse: movie %p (viewport %dx%d) takes the mouse\n", movie, buffer[0], buffer[1]);
            std::fflush(stdout);
            return &m;
        }
    }
    return nullptr;
}

GUEST_ABI float Advance(void* movie, float delta, std::uint32_t catch_up) {
    const TheOrderGameMenu* menu = Native::Menu();
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
        std::int32_t buffer[2];
        std::memcpy(buffer, m + 0x64, sizeof(buffer));
        const float x = p.x * float(buffer[0]), y = p.y * float(buffer[1]);
        Guest::Call<void>(kNotifyMouseState, movie, x, y, p.buttons, 0u);
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
        !Matches(kSetMouseCursorCount, {0x83, 0xfe, 0x04, 0xb8, 0x04, 0x00, 0x00, 0x00, 0x0f, 0x46, 0xc6}))
        return false;
    slot = reinterpret_cast<std::uintptr_t>(&Advance);
    return true;
}

NATIVE_REPLACEMENT({"mouse in the game's menus (GFxMovieRoot::Advance)", "menu", Native::Kind::Custom, kAdvance,
                    {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53}, nullptr,
                    nullptr, &InstallMenuMouse});

} // namespace
