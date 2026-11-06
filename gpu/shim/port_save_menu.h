// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the save list on the game's own save screen (the PC version's storage.gfx), the
// port's side. The system save dialog's list (src/runtime_services.c, theordergpu_choice_begin)
// goes to the game layer (game/p1/save_menu.cpp) when it says the screen works; else, or when
// it does not take the list within a second, the overlay's list shows it as before.

#pragma once

#include <string>
#include <vector>

#include "../theordergpu.h"

namespace PortSaveMenu {

/// The runtime (save dialog): true when the game's screen will show this list.
/// Each save's own name and detail, for the next Begin (the saves only, no new-save item).
void Rows(const std::vector<std::string>& names, const std::vector<std::string>& details);
bool Begin(const std::string& title, const std::vector<std::string>& items);
/// -1 still choosing, -2 cancelled, else the dialog's item; -3: not taken, show the overlay's.
int Poll();
bool Active();

void Ready();
int Take(TheOrderSaveMenu* out);
void Done(int choice);

} // namespace PortSaveMenu
