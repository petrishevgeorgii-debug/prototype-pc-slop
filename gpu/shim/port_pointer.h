// port: the mouse as a pointer in the game's own menus (port_pointer.cpp).
#pragma once

#include <SDL3/SDL.h>

namespace PortPointer {

/// The presenter: where the game's picture is in the swapchain (pixels) and which part of the
/// frame it shows (x, y, width, height as fractions of the frame).
void SetPicture(int x, int y, int w, int h, const float picture[4]);

/// The game's menus are up and the Mouse in menus setting is on: the window lets go of the mouse.
bool Wanted();

/// The window: a mouse event while Wanted().
void Event(const SDL_Event& event, SDL_Window* window);

/// The window: the pointer stopped (no buttons held, no wheel left).
void Release();

} // namespace PortPointer
