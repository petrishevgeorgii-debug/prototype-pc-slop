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
void Draw(std::uint64_t ps_address);
/// A draw tag (game/p1/render_map.cpp: ps4Context's DrawPrimGroup(s), slot 4 or 5) and its end:
/// the caller, the primitive group's shader and the classes (vtables, image-relative).
void Tag(std::uint32_t slot, std::uint32_t caller, std::uint64_t shader, std::uint32_t shader_class,
         std::uint32_t group_class);
void TagEnd();
void Frame();
} // namespace PortRenderMap
