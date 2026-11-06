// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: Hide HUD on a key (F3; gpu/shim/port_trainer.cpp counts the presses).
//
// The game has a Hide HUD switch of its own: the byte at 0x122f5c4, which fe_ToggleHideHUDOption
// (0x1bd70, a Lua function: the console can call it too) flips, and which the HUD's elements ask
// through 0x1c060 (16 callers; the -HideHUD start switch does the same). The key flips the byte.
// The menus are not part of the HUD: they still show.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game/guest.h"
#include "game/native_registry.h"
#include "game/p1/hud.h"

namespace {

constexpr std::uint64_t kIsHudHidden = 0x1c060, kToggleHideHud = 0x1bd70, kHideHud = 0x122f5c4;

bool Ready() {
    static const int ready = [] {
        static const std::uint8_t is_hidden[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x0d, 0x59,
                                                 0x35, 0x21, 0x01, 0xb0, 0x01, 0x80, 0x39, 0x00};
        static const std::uint8_t toggle[] = {0x48, 0x8d, 0x05, 0x4d, 0x38, 0x21, 0x01, 0x80, 0x30, 0x01, 0xc3};
        const bool ok = std::memcmp(Guest::Image() + kIsHudHidden, is_hidden, sizeof(is_hidden)) == 0 &&
                        std::memcmp(Guest::Image() + kToggleHideHud, toggle, sizeof(toggle)) == 0;
        std::printf("Hide HUD: %s\n", ok ? "ready (F3)" : "off (the game's code is not where it was)");
        std::fflush(stdout);
        return ok ? 1 : 0;
    }();
    return ready != 0;
}

} // namespace

namespace Native {

void HudFrame() {
    const TheOrderGameMenu* menu = Native::Menu();
    if (!menu || !menu->hud_toggle) return;
    const int presses = menu->hud_toggle();
    if (!presses || !Ready() || presses % 2 == 0) return;
    auto& hidden = Guest::At<std::uint8_t>(kHideHud);
    hidden = hidden ? 0 : 1;
    std::printf("Hide HUD: the HUD %s (F3)\n", hidden ? "hidden" : "shown");
    std::fflush(stdout);
}

} // namespace Native
