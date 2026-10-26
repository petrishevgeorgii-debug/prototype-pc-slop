// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the controller's vibration stays on, and consuming vibrates (docs/NATIVE.md).
//
// The game vibrates the controller only while its profile's rumble switch,
// Proto_Profile_GamepadRumbleEnabled, is on. The switch is the PS3's Rumble row of Options >
// Controller, which the PS4 hides, and PS4 profiles can hold it off: then the game never asks for
// vibration (scePadSetVibration) at all. The game database calls 0x5685a0 when the switch changes
// (also when a profile is loaded), and that hands the new value on to 0x568510(on, player), which
// turns the player's rumble on or off. Here that hand-over (a tail call) always says on: the
// port's own Vibration setting (Options > Controller, the port menu, the launcher) sets how strong
// it is, off included (src/runtime_pad.c).
//
// Consuming has no vibration of its own in the game's moves. The consume moves start and end the
// consume effect (the tendrils around the victim) with an EffectConsumeAction: its start (vtable
// slot 6, 0x856cd0) and its end (slot 7, 0x8570a0) now also start a swelling rumble and end it.
// (The consume itself, 0x849900, comes only at the end of the move: too late to be felt with it.)

#include <cstdint>
#include <cstdio>

#include "game/guest.h"
#include "game/native_registry.h"

namespace {

constexpr std::uint64_t kSwitchChanged = 0x5685cb; ///< jmp 0x568510 in the listener 0x5685a0
std::uint64_t g_set_rumble = 0;                     ///< 0x568510(on, player)

GUEST_ABI void SetRumble(std::uint32_t on, std::uint32_t player) {
    static int logged = 0;
    if (logged < 4) {
        ++logged;
        std::printf("Rumble: the profile's rumble switch is %s for player %u: kept on\n", on & 0xff ? "on" : "off",
                    player & 0xff);
        std::fflush(stdout);
    }
    Guest::Call<void>(g_set_rumble, 1u, player);
}

NATIVE_REPLACEMENT({"rumble switch (the profile's GamepadRumbleEnabled, kept on)", "rumble", Native::Kind::CallSite,
                    kSwitchChanged, {0xe9, 0x40, 0xff, 0xff, 0xff, 0x48, 0x3b, 0x47, 0x08, 0x75, 0x11, 0x48},
                    reinterpret_cast<void*>(&SetRumble), &g_set_rumble});

// EffectConsumeAction (vtable 0x109b420): slot 6 starts the consume effect, slot 7 ends it.
constexpr std::uint64_t kEffectStart = 0x856cd0, kEffectEnd = 0x8570a0;
constexpr std::uint64_t kStartSlot = 0x109b450, kEndSlot = 0x109b458;

GUEST_ABI std::uint64_t ConsumeEffectStart(void* action) {
    static int logged = 0;
    if (logged < 3) {
        ++logged;
        std::printf("Rumble: consume\n");
        std::fflush(stdout);
    }
    // Rises over the first half second and holds (2.5 s at most) until the effect ends.
    Native::Pulse(Native::Pulses::Game, 0.9f, 0.55f, 2500, 500);
    return Guest::Call<std::uint64_t>(kEffectStart, action);
}

GUEST_ABI std::uint64_t ConsumeEffectEnd(void* action) {
    Native::Pulse(Native::Pulses::Game, 0, 0, 0);
    return Guest::Call<std::uint64_t>(kEffectEnd, action);
}

/// The slots hold the relocated addresses of the two functions: they now hold the native ones.
bool InstallConsumeEffect(std::uint64_t) {
    auto& start = Guest::At<std::uintptr_t>(kStartSlot);
    auto& end = Guest::At<std::uintptr_t>(kEndSlot);
    const auto image = reinterpret_cast<std::uintptr_t>(Guest::Image());
    if (start != image + kEffectStart || end != image + kEffectEnd) return false;
    start = reinterpret_cast<std::uintptr_t>(&ConsumeEffectStart);
    end = reinterpret_cast<std::uintptr_t>(&ConsumeEffectEnd);
    return true;
}

NATIVE_REPLACEMENT({"consume vibration (EffectConsumeAction start and end)", "rumble", Native::Kind::Custom,
                    kEffectStart, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54},
                    nullptr, nullptr, &InstallConsumeEffect});

} // namespace
