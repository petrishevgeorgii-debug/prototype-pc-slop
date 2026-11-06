// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the PC version's save screen for the save lists (save_menu.cpp).
#pragma once

namespace Native {
/// Every frame of protomain (menu_mouse.cpp), on the thread that runs the game's menus.
void SaveMenuFrame(void* movie);
} // namespace Native
