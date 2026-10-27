// SPDX-License-Identifier: GPL-2.0-or-later
// port: PROTOTYPE's shadow fade, moved out with the shadow distance (Options > Video > SHADOW
// DISTANCE, game/p1/shadows.cpp).
//
// The game's lit pixel shaders fade the sun's shadow out by distance, a constant in the shader
// itself (art/startup_shaders.p3d, the HLSL kept beside the binaries):
//
//     float shadowFade = saturate( dist_to_vertex * 0.02 - 3.0 );   // 150 m to 200 m
//     SoftShadowSample = lerp( SoftShadowSample, 1.0, shadowFade * shadowFade );
//
// so however far the cascades reach, nothing past 200 m is shadowed. In the GCN code the two
// numbers are literals (0.02 = 0x3ca3d70a, -3.0 = 0xc0400000, each after an instruction taking a
// literal operand, a few dwords apart). A fragment shader that has them gets the shadow distance
// setting in its runtime info (hw.fs.shadow_fade: its own permutation), and its code is translated
// from a copy with the fade moved to the last 15% of the distance (250 m: 212 to 250 m, 1000 m:
// 850 to 1000 m).

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace PortShadowFade {

/// The level a fragment shader gets (0: the game's own fade, or a shader without it; 1-3: the
/// setting's 250, 500, 1000 m). `hash` is the shader's hash, `code` its GCN code.
std::uint32_t Level(std::uint64_t hash, std::span<const std::uint32_t> code);

/// `code` with the fade's literals replaced for `level` (the copy kept in `storage`), or `code`
/// unchanged (level 0, or no fade found).
std::span<const std::uint32_t> Apply(std::span<const std::uint32_t> code, std::uint32_t level,
                                     std::vector<std::uint32_t>& storage);

} // namespace PortShadowFade
