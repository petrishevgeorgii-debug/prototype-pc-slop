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
void Frame();
} // namespace PortRenderMap
