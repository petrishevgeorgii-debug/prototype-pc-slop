// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the controller's vibration stays on (docs/NATIVE.md).
//
// The game vibrates the controller only while its profile's rumble switch,
// Proto_Profile_GamepadRumbleEnabled, is on. The switch is the PS3's Rumble row of Options >
// Controller, which the PS4 hides, and PS4 profiles can hold it off: then the game never asks for
// vibration (scePadSetVibration) at all. The game database calls 0x5685a0 when the switch changes
// (also when a profile is loaded), and that hands the new value on to 0x568510(on, player), which
// turns the player's rumble on or off. Here that hand-over (a tail call) always says on: the
// port's own Vibration setting (Options > Controller, the port menu, the launcher) sets how strong
// it is, off included (src/runtime_pad.c).

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

} // namespace
