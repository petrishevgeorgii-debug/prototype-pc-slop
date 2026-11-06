// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the Lua console (console.cpp).
#pragma once

namespace Native {
/// Every frame of protomain (menu_mouse.cpp), on the thread that runs the game's Lua.
void ConsoleFrame();
} // namespace Native
