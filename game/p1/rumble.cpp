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
// Consuming has no vibration of its own in the game's moves. 0x849900(consumer, victim, how) is
// the consume itself (the victim is taken in, health gained: it sends ConsumeHealthGainedMessage);
// the consume moves (ConsumeAction and the others) and the script function go_Consume call it.
// Each of those calls goes through native code that adds a strong pulse when it is done.

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

std::uint64_t g_consume = 0; ///< 0x849900(consumer, victim, how)

GUEST_ABI std::uint64_t Consume(std::uint64_t consumer, std::uint64_t victim, std::uint64_t how) {
    const std::uint64_t result = Guest::Call<std::uint64_t>(g_consume, consumer, victim, how); // (go_Consume returns it)
    static int logged = 0;
    if (logged < 3) {
        ++logged;
        std::printf("Rumble: consume\n");
        std::fflush(stdout);
    }
    Native::Pulse(Native::Pulses::Game, 1.0f, 0.7f, 450);
    return result;
}

#define CONSUME_CALL(site, ...)                                                                          \
    NATIVE_REPLACEMENT({"consume vibration (call of the consume at " #site ")", "rumble", Native::Kind::CallSite, \
                        site, {__VA_ARGS__}, reinterpret_cast<void*>(&Consume), &g_consume})

CONSUME_CALL(0x694a61, 0xe9, 0x9a, 0x4e, 0x1b, 0x00, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00); // go_Consume
CONSUME_CALL(0x84a0b2, 0xe8, 0x49, 0xf8, 0xff, 0xff, 0x49, 0x8b, 0x07, 0x48, 0x3b, 0x45, 0xe0); // ConsumeAction
CONSUME_CALL(0x84a42a, 0xe8, 0xd1, 0xf4, 0xff, 0xff, 0x49, 0x8b, 0x07, 0x48, 0x3b, 0x45, 0xd8);
CONSUME_CALL(0x84a72c, 0xe8, 0xcf, 0xf1, 0xff, 0xff, 0x31, 0xc0, 0x48, 0x83, 0xc4, 0x08, 0x5b);
CONSUME_CALL(0x84b127, 0xe8, 0xd4, 0xe7, 0xff, 0xff, 0xc6, 0x43, 0x50, 0x01, 0xc7, 0x45, 0xd0);
CONSUME_CALL(0x84b28b, 0xe8, 0x70, 0xe6, 0xff, 0xff, 0xc7, 0x45, 0xc0, 0x3c, 0x00, 0x00, 0x00);

} // namespace
