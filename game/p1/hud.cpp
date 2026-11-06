// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: Hide HUD on a key (F3; gpu/shim/port_trainer.cpp counts the presses).
//
// The game has a Hide HUD switch of its own (the byte at 0x122f5c4 that fe_ToggleHideHUDOption
// flips, asked through 0x1c060), but flipping it in play changed nothing on screen (b8): the
// HUD's elements read it when they are made. So the key hides what draws the HUD instead: the
// Scaleform front end (engine::render::FrontEndPass, the health bar, the map, the hints) and the
// 3D HUD (pure3d::ProtoHUDPass, the markers), only while the game's top screen is the HUD itself
// (IGHUD): a menu over it (pause, map, Web of Intrigue) shows as always, and so does the HUD
// under it. The passes' Render (vtable slot 5) is wrapped; the render map (test builds) may have
// wrapped it first, and is then called in turn.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game/guest.h"
#include "game/native_registry.h"
#include "game/p1/hud.h"

namespace {

struct Pass {
    const char* name;
    std::uint64_t vtable; ///< the class's vtable (its Render at slot 5)
    std::uint64_t render;
};
constexpr Pass kPasses[] = {{"FrontEndPass", 0x104ab60, 0xc36f0}, {"ProtoHUDPass", 0x1062070, 0x33e8e0}};
std::uint64_t g_render[2]; ///< what the slots held: the game's Render, or the render map's wrapper
std::atomic<bool> g_hidden{false}, g_in_game{false};

template <int I>
GUEST_ABI std::uint64_t Render(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d, std::uint64_t e,
                               std::uint64_t f) {
    if (g_hidden.load(std::memory_order_relaxed) && g_in_game.load(std::memory_order_relaxed)) return 0;
    using Original = std::uint64_t(GUEST_ABI*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t, std::uint64_t);
    return reinterpret_cast<Original>(g_render[I])(a, b, c, d, e, f);
}

bool InstallHud(std::uint64_t) {
    using Thunk = std::uint64_t(GUEST_ABI*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                            std::uint64_t, std::uint64_t);
    constexpr Thunk kThunks[] = {&Render<0>, &Render<1>};
    const auto image = reinterpret_cast<std::uintptr_t>(Guest::Image());
    for (const auto& pass : kPasses) { // both still hold a Render (the game's, or a wrapper of it)
        const auto slot = Guest::At<std::uintptr_t>(pass.vtable + 5 * 8);
        if (!slot || (slot - image < 0x2000000 && slot != image + pass.render)) return false;
    }
    for (int i = 0; i < 2; ++i) {
        auto& slot = Guest::At<std::uintptr_t>(kPasses[i].vtable + 5 * 8);
        g_render[i] = slot;
        slot = reinterpret_cast<std::uintptr_t>(kThunks[i]);
    }
    return true;
}

// The site checked: FrontEndPass's Render as this build has it.
NATIVE_REPLACEMENT({"Hide HUD (the front end and HUD passes while the HUD is on top)", "hud", Native::Kind::Custom,
                    0xc36f0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0x80, 0x3d}, nullptr,
                    nullptr, &InstallHud});

} // namespace

namespace Native {

void HudFrame(const char* screen) {
    g_in_game = std::strcmp(screen, "IGHUD") == 0;
    const TheOrderGameMenu* menu = Native::Menu();
    if (!menu || !menu->hud_toggle) return;
    const int presses = menu->hud_toggle();
    if (!presses || presses % 2 == 0) return;
    if (!g_render[0]) {
        std::printf("Hide HUD: off (the game's code is not where it was)\n");
        std::fflush(stdout);
        return;
    }
    g_hidden = !g_hidden;
    std::printf("Hide HUD: the HUD %s (F3)\n", g_hidden ? "hidden" : "shown");
    std::fflush(stdout);
}

} // namespace Native
