// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE's Lua console, the port's side: the commands typed in the overlay's console
// (port_overlay.cpp, the key under Esc) wait here for the game layer (game/p1/console.cpp), which
// runs them in the game's own Lua and sends back what they printed and returned.

#pragma once

#include <string>
#include <vector>

namespace PortConsole {

enum class Kind { Input, Output, Error, Info };

struct Line {
    Kind kind;
    std::string text;
};

/// The overlay: a typed command (shown in the log and queued for the game).
void Submit(const std::string& command);
/// The game layer: the next queued command into `out` (NUL-ended, cut to `size`); false if none.
bool Take(char* out, int size);
/// The game layer: what a command printed, returned or failed with (several lines at once).
void Print(const char* text, Kind kind);
/// The overlay: the log so far (its last few hundred lines) and a counter that changes with it.
std::vector<Line> Lines(unsigned* version);
void Clear();
/// Commands typed this session, oldest first (Up/Down in the input line).
std::vector<std::string> History();

} // namespace PortConsole
