// port: the mouse as a pointer in the game's own menus (game/p1/menu_mouse.cpp, docs/NATIVE.md).
//
// While the game's menus are up (the game's menu movie says so, theordergpu_pointer_mode) and the
// "Mouse in menus" setting is on, the window lets go of the mouse (no relative mode: the mouse look
// stops) and its moves, buttons and wheel come here, the position as a place in the game's
// picture: the presenter tells where in the window that picture is (letterboxed, and only the
// picture part of the frame, SetPicture). The game's menu movie draws its own cursor (the PC
// version's) where the mouse is.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

#include <SDL3/SDL.h>

#include "../theordergpu.h"
#include "port_pointer.h"
#include "port_settings.h"

namespace PortPointer {
namespace {

std::mutex lock;
struct {
    int x = 0, y = 0, w = 1, h = 1;      ///< the picture in the swapchain (pixels)
    float picture[4] = {0, 0, 1, 1};     ///< ... and which part of the frame it is
} map;
TheOrderPointer state{};
std::atomic<int> mode{0};                ///< the game's menus are up (1) or not
std::atomic<long long> moved_ms{0};      ///< the last move or click, for the cursor's visibility
int logged_mode = -1;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

void SetPicture(int x, int y, int w, int h, const float picture[4]) {
    std::scoped_lock guard{lock};
    map.x = x;
    map.y = y;
    map.w = std::max(w, 1);
    map.h = std::max(h, 1);
    std::copy(picture, picture + 4, map.picture);
}

bool Wanted() {
    return mode.load() && PortSettings::Get().menu_mouse;
}

void Event(const SDL_Event& event, SDL_Window* window) {
    std::scoped_lock guard{lock};
    auto place = [&](float wx, float wy) {
        // Window points to swapchain pixels (they differ on high-DPI displays).
        int pw = 0, ph = 0, ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        SDL_GetWindowSize(window, &ww, &wh);
        const float sx = ww > 0 ? float(pw) / float(ww) : 1.0f, sy = wh > 0 ? float(ph) / float(wh) : 1.0f;
        const float u = std::clamp((wx * sx - float(map.x)) / float(map.w), 0.0f, 1.0f);
        const float v = std::clamp((wy * sy - float(map.y)) / float(map.h), 0.0f, 1.0f);
        state.x = map.picture[0] + u * map.picture[2];
        state.y = map.picture[1] + v * map.picture[3];
        state.inside = wx * sx >= float(map.x) && wx * sx < float(map.x + map.w) &&
                       wy * sy >= float(map.y) && wy * sy < float(map.y + map.h);
    };
    auto bit = [](Uint8 button) -> unsigned {
        return button == SDL_BUTTON_LEFT ? 1u : button == SDL_BUTTON_RIGHT ? 2u : button == SDL_BUTTON_MIDDLE ? 4u : 0u;
    };
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        place(event.motion.x, event.motion.y);
        moved_ms = NowMs();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        place(event.button.x, event.button.y);
        state.buttons |= bit(event.button.button);
        moved_ms = NowMs();
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        place(event.button.x, event.button.y);
        state.buttons &= ~bit(event.button.button);
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        static float rest = 0.0f;
        float y = (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y) + rest;
        const int notches = int(y);
        rest = y - float(notches);
        state.wheel += notches;
        moved_ms = NowMs();
        break;
    }
    default:
        break;
    }
}

void Release() {
    std::scoped_lock guard{lock};
    state.buttons = 0;
    state.wheel = 0;
}

std::atomic<int> clicks{0};

void NoteClick() {
    ++clicks;
}

bool BackHeld() {
    std::scoped_lock guard{lock};
    return Wanted() && (state.buttons & 2u);
}

} // namespace PortPointer

extern "C" int theordergpu_pointer_mode(int on) {
    using namespace PortPointer;
    if (on >= 0) {
        mode = on ? 1 : 0;
        if (logged_mode != mode.load()) {
            logged_mode = mode.load();
            std::printf("Menu mouse: %s\n", logged_mode ? "the game's menus are up: the mouse points" :
                                                          "in game: the mouse looks around");
            std::fflush(stdout);
        }
    }
    // The cursor shows a while after the mouse moved or clicked (not over a menu worked with a
    // controller or keys).
    return Wanted() && moved_ms.load() && NowMs() - moved_ms.load() < 8000 ? 1 : 0;
}

extern "C" int theordergpu_pointer_clicks(void) {
    return PortPointer::clicks.exchange(0);
}

extern "C" void theordergpu_pointer_take(TheOrderPointer* out) {
    using namespace PortPointer;
    std::scoped_lock guard{lock};
    *out = state;
    out->active = Wanted() ? 1 : 0;
    state.wheel = 0;
}
