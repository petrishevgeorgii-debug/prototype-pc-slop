// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the controller's vibration stays on, and Alex's powers and moves vibrate
// (docs/NATIVE.md).
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
// The game's own vibration is sparse, so the port adds its own ("game effects", which the Game
// effects setting turns off) where the game's moves start and end. Every move is a fight action
// (proto::fightitems::motion::*Action): its vtable's slot 6 starts it and slot 7 ends it. Those
// slots of the moves below now go through a native function that adds a vibration and calls the
// game's own. Moves anyone can make (jumps, falls, throws, hit reactions...) vibrate only when the
// player makes them: the action's owner (an object handle in the action) is compared with the
// player's (Native::IsPlayer). And a camera shake (explosions, landings, impacts: the game's
// CameraManager applies the requested one at 0x70c2ed) vibrates with the strength the game gave it.

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>

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

// ---- Moves --------------------------------------------------------------------------------------

using Native::Effect;
using Native::Pulse;
constexpr auto kGame = Native::Pulses::Game;

/// Effects that last (they end with the move): one channel each, so the end finds them.
enum Ids { kConsume = 1, kWhipPull, kWallRun, kWallSlide, kDevastator };

std::uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/// Whose move it is.
enum class Who : std::uint8_t {
    Anyone, ///< moves only Alex has (his powers): no check
    Player, ///< the owner handle at `owner` must be the player's
};

struct Hook {
    const char* name;
    std::uint64_t vtable;
    int slot;            ///< 6 start, 7 end
    std::uint64_t game;  ///< the game's function the slot holds
    Who who;
    std::uint16_t owner; ///< offset of the owner's object handle in the action (Who::Player)
    void (*feel)();
};

std::uint64_t g_fall_start = 0;

// Large is the low, heavy motor; small the high, sharp one (both 0..1, scaled by the Vibration
// setting). Times in ms.
constexpr Hook kHooks[] = {
    // Consuming: swells while the tendrils pull the victim in.
    {"consume start", 0x109b420, 6, 0x856cd0, Who::Anyone, 0, [] { Effect(kGame, kConsume, 0.9f, 0.55f, 2500, 500); }},
    {"consume end", 0x109b420, 7, 0x8570a0, Who::Anyone, 0, [] { Effect(kGame, kConsume, 0, 0, 0); }},
    // Whipfist: a crack when it lashes out, a tug when it catches, a pull while it drags Alex.
    {"whipfist lash", 0x109fd30, 6, 0x8ac8d0, Who::Anyone, 0, [] { Pulse(kGame, 0.3f, 0.9f, 110); }},
    {"whipfist catch", 0x109fdf0, 6, 0x8ad3e0, Who::Anyone, 0, [] { Pulse(kGame, 0.75f, 0.5f, 150); }},
    {"whipfist pull start", 0x109ff10, 6, 0x8ae5f0, Who::Anyone, 0,
     [] { Effect(kGame, kWhipPull, 0.55f, 0.35f, 1500, 250); }},
    {"whipfist pull end", 0x109ff10, 7, 0x8aebf0, Who::Anyone, 0, [] { Effect(kGame, kWhipPull, 0, 0, 0); }},
    {"whipfist retract", 0x109fd90, 6, 0x8acf70, Who::Anyone, 0, [] { Pulse(kGame, 0.12f, 0.4f, 90); }},
    // Wall running: a thump onto the wall, then steps while running, a push off it on a jump.
    {"wall run enter", 0x109fa10, 6, 0x8a68d0, Who::Anyone, 0, [] { Pulse(kGame, 0.75f, 0.4f, 160); }},
    {"wall run start", 0x109f9b0, 6, 0x8a4a60, Who::Anyone, 0,
     [] { Effect(kGame, kWallRun, 0.34f, 0.22f, 30000, 0, 190); }},
    {"wall run end", 0x109f9b0, 7, 0x8a63a0, Who::Anyone, 0, [] { Effect(kGame, kWallRun, 0, 0, 0); }},
    {"wall jump", 0x109f8b0, 6, 0x8a1aa0, Who::Anyone, 0, [] { Pulse(kGame, 0.6f, 0.45f, 150); }},
    {"wall jump up", 0x109f910, 6, 0x8a2e00, Who::Anyone, 0, [] { Pulse(kGame, 0.6f, 0.45f, 150); }},
    {"wall slide start", 0x109fb30, 6, 0x8a8040, Who::Anyone, 0,
     [] { Effect(kGame, kWallSlide, 0.12f, 0.3f, 8000, 200); }},
    {"wall slide end", 0x109fb30, 7, 0x8a8580, Who::Anyone, 0, [] { Effect(kGame, kWallSlide, 0, 0, 0); }},
    // In the air: the air dash, air attacks, and the dive's landing.
    {"air dash", 0x109b620, 6, 0x859b70, Who::Anyone, 0, [] { Pulse(kGame, 0.45f, 0.6f, 180); }},
    {"air attack", 0x109b520, 6, 0x858680, Who::Anyone, 0, [] { Pulse(kGame, 0.4f, 0.5f, 140); }},
    {"air dive start", 0x109b6a0, 6, 0x85a590, Who::Anyone, 0, [] { Pulse(kGame, 0.25f, 0.35f, 120); }},
    {"air dive landing", 0x109b6a0, 7, 0x85ac80, Who::Anyone, 0, [] { Pulse(kGame, 1.0f, 0.6f, 350); }},
    // Devastators: build up, and each burst of ground spikes.
    {"devastator start", 0x109af00, 6, 0x850ba0, Who::Anyone, 0,
     [] { Effect(kGame, kDevastator, 1.0f, 0.7f, 2400, 900); }},
    {"devastator end", 0x109af00, 7, 0x852e10, Who::Anyone, 0, [] { Effect(kGame, kDevastator, 0, 0, 0); }},
    {"ground spikes", 0x10713f0, 6, 0x5dca90, Who::Anyone, 0, [] { Pulse(kGame, 0.85f, 0.5f, 220); }},
    // Shield: raising it, and a blow it turns away.
    {"shield up", 0x10723a0, 6, 0x5ef270, Who::Anyone, 0, [] { Pulse(kGame, 0.55f, 0.3f, 200); }},
    {"shield block", 0x1072400, 6, 0x5ef5f0, Who::Anyone, 0, [] { Pulse(kGame, 0.4f, 0.8f, 120); }},
    // Moves anyone makes: only the player's.
    {"throw", 0x109eeb0, 6, 0x89a200, Who::Player, 0x40, [] { Pulse(kGame, 0.6f, 0.4f, 160); }},
    {"grab", 0x109baa0, 6, 0x85ead0, Who::Player, 0x40, [] { Pulse(kGame, 0.5f, 0.3f, 120); }},
    {"hit taken", 0x109c8a0, 6, 0x8683c0, Who::Player, 0xc8, [] { Pulse(kGame, 0.85f, 0.6f, 260); }},
    {"hit landed", 0x109b7a0, 6, 0x85b5d0, Who::Player, 0x40, [] { Pulse(kGame, 0.5f, 0.75f, 90); }},
    {"heavy step", 0x109b720, 6, 0x85af20, Who::Player, 0x28, [] { Pulse(kGame, 0.4f, 0.2f, 120); }},
    {"jump", 0x109d0e0, 6, 0x872900, Who::Player, 0x40, [] { Pulse(kGame, 0.15f, 0.25f, 70); }},
    {"fall start", 0x109b4a0, 6, 0x8574e0, Who::Player, 0x28, [] { g_fall_start = NowMs(); }},
    {"fall landing", 0x109b4a0, 7, 0x858290, Who::Player, 0x28,
     [] {
         // The longer the fall, the harder the landing; short drops are left alone.
         if (!g_fall_start) return;
         const float seconds = static_cast<float>(NowMs() - g_fall_start) / 1000.0f;
         g_fall_start = 0;
         if (seconds < 0.35f) return;
         const float f = seconds >= 2.0f ? 1.0f : (seconds - 0.35f) / 1.65f;
         Pulse(kGame, 0.35f + 0.65f * f, 0.2f + 0.4f * f, static_cast<unsigned>(140 + 260 * f));
     }},
};
constexpr std::size_t kHookCount = sizeof(kHooks) / sizeof(kHooks[0]);

void Feel(std::size_t index, void* action) {
    const Hook& hook = kHooks[index];
    static int logged[kHookCount];
    bool mine = true;
    std::uint32_t handle = 0;
    if (hook.who == Who::Player) {
        handle = Guest::Field<std::uint32_t>(action, hook.owner);
        mine = Native::IsPlayer(handle);
    }
    if (logged[index] < 2) {
        ++logged[index];
        if (hook.who == Who::Player)
            std::printf("Rumble: %s (owner %08x, player %08x: %s)\n", hook.name, handle,
                        Guest::At<std::uint32_t>(0x13491c0), mine ? "the player" : "not the player");
        else
            std::printf("Rumble: %s\n", hook.name);
        std::fflush(stdout);
    }
    if (mine) hook.feel();
}

/// Slot functions take the action and whatever the game passes after it: all of it goes on.
template <std::size_t I>
GUEST_ABI std::uint64_t Hooked(void* action, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                               std::uint64_t e, double x0, double x1) {
    Feel(I, action);
    return Guest::Call<std::uint64_t>(kHooks[I].game, action, a, b, c, d, e, x0, x1);
}

using Slot = std::uint64_t(GUEST_ABI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                       std::uint64_t, double, double);
template <std::size_t... I>
constexpr auto MakeSlots(std::index_sequence<I...>) {
    return std::array<Slot, sizeof...(I)>{&Hooked<I>...};
}

/// Each slot holds the relocated address of the game's function: it now holds the native one.
/// A slot that holds anything else (another game build) keeps it.
bool InstallMoves(std::uint64_t) {
    static constexpr auto slots = MakeSlots(std::make_index_sequence<kHookCount>{});
    const auto image = reinterpret_cast<std::uintptr_t>(Guest::Image());
    unsigned hooked = 0;
    for (std::size_t i = 0; i < kHookCount; ++i) {
        auto& slot = Guest::At<std::uintptr_t>(kHooks[i].vtable + 8 * kHooks[i].slot);
        if (slot != image + kHooks[i].game) {
            std::printf("Rumble: %s left alone (its vtable slot holds something else)\n", kHooks[i].name);
            continue;
        }
        slot = reinterpret_cast<std::uintptr_t>(slots[i]);
        ++hooked;
    }
    std::printf("Rumble: %u of %zu moves vibrate\n", hooked, kHookCount);
    std::fflush(stdout);
    return hooked > 0;
}

// Verified at the first move's function (WhipFistExtendAction's start); InstallMoves checks every
// slot on its own.
NATIVE_REPLACEMENT({"move vibration (fight action starts and ends)", "rumble", Native::Kind::Custom, 0x8ac8d0,
                    {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec}, nullptr, nullptr,
                    &InstallMoves});

// ---- Camera shakes ------------------------------------------------------------------------------

// The CameraManager (its pointer at 0x135c5f0) keeps a requested shake until its update applies
// it: [+0x90] its level (0 small, 1 medium, 2 large; -1 none), [+0x94] its strength (the level's
// 1, 2 or 3 times how near it is, 0..1), [+0x18] the camera. The update applies each part of the
// level's shake with 0x103e90(camera, rotation, position, ...) at 0x70c2ed.
constexpr std::uint64_t kShakeCall = 0x70c2ed, kCameraManager = 0x135c5f0;
std::uint64_t g_apply_shake = 0;

GUEST_ABI void ApplyShake(void* camera, void* rotation, void* position, float a, float b, float c, float d) {
    static std::uint64_t last = 0;
    const auto manager = Guest::At<std::uint8_t*>(kCameraManager);
    if (manager && Guest::Field<void*>(manager, 0x18) == camera && Guest::Field<std::int32_t>(manager, 0x90) >= 0) {
        const std::uint64_t now = NowMs();
        if (now - last > 60) { // once for all of the shake's parts
            last = now;
            const std::int32_t level = Guest::Field<std::int32_t>(manager, 0x90);
            float s = Guest::Field<float>(manager, 0x94) / 3.0f;
            s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
            static int logged = 0;
            if (logged < 4) {
                ++logged;
                std::printf("Rumble: camera shake (level %d, strength %.2f)\n", level, s);
                std::fflush(stdout);
            }
            if (s > 0.04f) Pulse(kGame, 0.25f + 0.75f * s, 0.15f + 0.5f * s, static_cast<unsigned>(120 + 280 * s));
        }
    }
    Guest::Call<void>(g_apply_shake, camera, rotation, position, a, b, c, d);
}

NATIVE_REPLACEMENT({"camera shake vibration (the CameraManager's update)", "rumble", Native::Kind::CallSite, kShakeCall,
                    {0xe8, 0x9e, 0x7b, 0x9f, 0xff, 0x49, 0x83, 0xc4, 0x24, 0xff, 0xcb, 0x0f},
                    reinterpret_cast<void*>(&ApplyShake), &g_apply_shake});

} // namespace
