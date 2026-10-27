// SPDX-License-Identifier: GPL-2.0-or-later
// port: the render map's GPU side (port_render_map.cpp, game/p1/render_map.cpp).
#pragma once

#include <cstdint>
#include <string_view>

namespace PortRenderMap {
/// THEORDER_RENDER_MAP=1.
bool Enabled();
/// A pass begins (the game's push marker) / ends (pop), a draw (its pixel shader's address), a
/// frame (a flip).
void Push(std::string_view name);
void Pop();
void Draw(std::uint64_t ps_address, std::uint64_t vs_address);
/// A draw tag (game/p1/render_map.cpp: ps4Context's DrawPrimGroup(s), slot 4 or 5) and its end:
/// the caller, the primitive group's shader (material), its class (vtable, image-relative) and
/// its template's name hash.
void Tag(std::uint32_t slot, std::uint32_t caller, std::uint64_t shader, std::uint32_t shader_class,
         std::uint32_t template_name);
void TagEnd();
/// A shader template's name (game/p1/render_map.cpp writes each once): the log names templates and
/// sums a frame's draws by kind (world, people, vehicles...) from them.
void Name(std::uint32_t hash, std::string_view name);
/// The kind of the draw just counted (Draw): -1 when it was outside every draw tag. Its name, the
/// number of kinds, and whether it is solid geometry (world, roads, Alex, people, vehicles: what
/// the scene capture takes, vk_scene_capture.h).
int CurrentKind();
const char* KindName(int kind);
int KindCount();
bool Solid(int kind);
void Frame();
} // namespace PortRenderMap
