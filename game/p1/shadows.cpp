// SPDX-License-Identifier: GPL-2.0-or-later
// port: PROTOTYPE's sun shadows, native part (docs/NATIVE.md, docs/PROTOTYPE.md "Shadow ...").
//
// pure3d::Shadow (0x2c5e20 builds one) is the sun's cascaded shadow. What is known of it:
//
//   +0x300  u32    cascade count (SetNumCascades 0x2c9fa0 resizes the per-cascade state)
//   +0x304  f32[5] per-cascade bias (?) values, times +0x334
//   +0x318  f32[5] cascade extents: cascade i ends 0.5 x unit x extent[i] along the view
//                  (clipped to the shadow box); the sun light sets 1 / 2 / 4 / 24
//   +0x32c  f32    unit divisor (40): unit = 2 x range x scale / divisor = 10 m
//   +0x330  f32    unit scale (1)
//   +0x334  f32    bias scale (1)
//   +0x338  f32    range: the shadow box is +- range around the camera; Update copies it from
//                  the global at 0x1218930 (200, rm_SetGlobalShadowRangeHack) every frame
//
// pure3d::Shadow::Update (0x2c78d0) runs every frame for each shadow; ComputeCascades (0x2c9590,
// from Update) tests the range against 200.0 (0x2c95db, the constant at 0xc9f818) and uses a
// fixed debug box around (0, 0, -300) for any other value.
//
// Native: both of the renderer's calls to Update come here first. With the port menu's SHADOW
// DISTANCE set (PortSettings::shadow_distance, metres), the sun's cascades are spread from 5 m to
// that distance (each one ending a constant factor further than the last), the box grows to
// hold them, and the range check compares with the native range instead of 200. With GAME, the
// game's own values are put back.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "game/guest.h"
#include "game/native_registry.h"
#include "port_settings.h"

namespace {

using Guest::Field;

constexpr std::uint64_t kGlobalRange = 0x1218930;
constexpr std::uint64_t kRangeCheck = 0x2c95db;
constexpr std::uint64_t kRangeCheckConstant = 0xc9f818;

constexpr std::size_t kCascadeCount = 0x300;
constexpr std::size_t kCascadeExtents = 0x318;
constexpr std::size_t kUnitDivisor = 0x32c;
constexpr std::size_t kUnitScale = 0x330;
constexpr int kMaxCascades = 5;

constexpr float kNearestCascade = 5.0f; ///< metres, as the game's first cascade
constexpr float kUnit = 10.0f;          ///< metres, as the game's

std::uint64_t g_update = 0;          ///< the game's Shadow::Update
float* g_range_check = nullptr;      ///< what ComputeCascades adds to the range (-range)
float g_game_range = 200.0f;         ///< the game's global range and check (after patches)
float g_game_check = -200.0f;

/// The game's own values of the shadows the native distance changed, to put them back.
struct Saved {
    void* shadow = nullptr;
    float extents[kMaxCascades] = {};
    float divisor = 0.0f;
};
Saved g_saved[4];
int g_logged_distance = -1;

Saved* SavedFor(void* shadow, bool create) {
    for (auto& s : g_saved) {
        if (s.shadow == shadow) {
            return &s;
        }
    }
    if (!create) {
        return nullptr;
    }
    for (auto& s : g_saved) {
        if (!s.shadow) {
            s.shadow = shadow;
            for (int i = 0; i < kMaxCascades; ++i) {
                s.extents[i] = Field<float>(shadow, kCascadeExtents + 4 * i);
            }
            s.divisor = Field<float>(shadow, kUnitDivisor);
            return &s;
        }
    }
    return nullptr;
}

void Apply(void* shadow, int distance) {
    const std::uint32_t count = Field<std::uint32_t>(shadow, kCascadeCount);
    if (count < 2 || count > kMaxCascades) {
        return; // not the sun's cascaded shadow
    }
    if (distance <= 0) {
        if (Saved* s = SavedFor(shadow, false)) {
            for (int i = 0; i < kMaxCascades; ++i) {
                Field<float>(shadow, kCascadeExtents + 4 * i) = s->extents[i];
            }
            Field<float>(shadow, kUnitDivisor) = s->divisor;
            *s = Saved{};
            Guest::At<float>(kGlobalRange) = g_game_range;
            *g_range_check = g_game_check;
        }
        return;
    }
    if (!SavedFor(shadow, true)) {
        return;
    }
    const float last = std::max(float(distance), 2.0f * kNearestCascade);
    const float range = std::max(200.0f, last * 1.25f);
    for (std::uint32_t i = 0; i < count; ++i) {
        const float end = kNearestCascade * std::pow(last / kNearestCascade, float(i) / float(count - 1));
        Field<float>(shadow, kCascadeExtents + 4 * i) = end / (0.5f * kUnit);
    }
    Field<float>(shadow, kUnitDivisor) = 2.0f * range * Field<float>(shadow, kUnitScale) / kUnit;
    Guest::At<float>(kGlobalRange) = range;
    *g_range_check = -range;
}

GUEST_ABI std::uint64_t ShadowUpdate(void* shadow, void* a, void* b, void* c) {
    if (shadow && g_range_check) {
        const int distance = PortSettings::Get().shadow_distance.load(std::memory_order_relaxed);
        Apply(shadow, distance);
        if (distance != g_logged_distance) {
            g_logged_distance = distance;
            if (distance > 0) {
                std::printf("Native shadows: sun shadows out to %d m\n", distance);
            } else {
                std::printf("Native shadows: the game's own shadow distance\n");
            }
        }
    }
    return Guest::Call<std::uint64_t>(g_update, shadow, a, b, c);
}

bool InstallRangeCheck(std::uint64_t address) {
    auto* check = static_cast<float*>(Native::NearData(sizeof(float)));
    if (!check) {
        return false;
    }
    // A patch (Shadow Distance) may have changed the global and the constant: those are the
    // game's values from now on.
    g_game_range = Guest::At<float>(kGlobalRange);
    g_game_check = Guest::At<float>(kRangeCheckConstant);
    *check = g_game_check;
    const std::intptr_t next = reinterpret_cast<std::intptr_t>(Guest::Image() + address + 8);
    const std::int64_t disp = reinterpret_cast<std::intptr_t>(check) - next;
    if (disp < INT32_MIN || disp > INT32_MAX) {
        return false;
    }
    const std::int32_t d = static_cast<std::int32_t>(disp);
    Native::WriteCode(address + 4, &d, 4);
    g_range_check = check;
    return true;
}

// vaddss xmm3, xmm3, [rip -> -200.0]: reads the native value instead.
NATIVE_REPLACEMENT({"pure3d::Shadow::ComputeCascades range check", "shadows", Native::Kind::Custom, kRangeCheck,
                    {0xc5, 0xe2, 0x58, 0x1d, 0x35, 0x62, 0x9d, 0x00}, nullptr, nullptr, InstallRangeCheck});
NATIVE_REPLACEMENT({"pure3d::Shadow::Update (call 1)", "shadows", Native::Kind::CallSite, 0x330dca,
                    {0xe8, 0x01, 0x6b, 0xf9, 0xff, 0x41, 0x80, 0x7e, 0x58, 0x00},
                    reinterpret_cast<void*>(&ShadowUpdate), &g_update});
NATIVE_REPLACEMENT({"pure3d::Shadow::Update (call 2)", "shadows", Native::Kind::CallSite, 0x344eb3,
                    {0xe8, 0x18, 0x2a, 0xf8, 0xff, 0x80, 0x7b, 0x58, 0x00},
                    reinterpret_cast<void*>(&ShadowUpdate), &g_update});

} // namespace
