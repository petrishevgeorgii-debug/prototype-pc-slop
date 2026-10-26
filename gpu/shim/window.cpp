// port: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "port_overlay.h"
#include "port_graphics.h"
#include "port_settings.h"
#include "../theordergpu.h"

namespace {
// Mouse input for the pad (theordergpu_mouse_take), gathered while captured. Window thread
// writes, the pad thread takes.
std::mutex mouse_mutex;
TheOrderMouse mouse{};
bool window_focused = true;
} // namespace

extern "C" void theordergpu_mouse_take(TheOrderMouse* out) {
    std::scoped_lock lock{mouse_mutex};
    *out = mouse;
    out->sensitivity = PortSettings::Get().mouse_sensitivity / 100.0f;
    mouse.dx = mouse.dy = 0.0f; // buttons stay down until released
    mouse.wheel = 0;
}

extern "C" void theordergpu_pad_options(TheOrderPadOptions* out) {
    const auto& s = PortSettings::Get();
    out->vibration = s.vibration;
    out->light_bar = s.light_bar ? 1 : 0;
    out->menu_vibration = s.menu_vibration;
    out->extra_vibration = s.extra_vibration ? 1 : 0;
    out->haptics = s.haptics ? 1 : 0;
    out->haptics_sound = s.haptics_sound;
}

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    // port: the game's own icon (run.py converts sce_sys/icon0.png to BMP, SDL's built-in format).
    if (const char* icon_path = std::getenv("THEORDER_WINDOW_ICON")) {
        if (SDL_Surface* icon = SDL_LoadBMP(icon_path)) {
            SDL_SetWindowIcon(window, icon);
            SDL_DestroySurface(icon);
        }
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    // port: THEORDER_FULLSCREEN=1 (older launchers) still asks for fullscreen.
    if (const char* fullscreen = std::getenv("THEORDER_FULLSCREEN"); fullscreen && fullscreen[0] == '1' &&
        PortSettings::Get().display_mode == PortGraphics::Windowed) {
        PortSettings::Get().display_mode = PortGraphics::Borderless;
    }
    ApplyDisplayMode(PortSettings::Get().display_mode);
    // port: the game has no mouse; the in-game menu draws its own cursor (PortOverlay::SetOpen).
    SDL_HideCursor();
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

namespace {
/// PROTOTYPE's Resolution setting (Options > Display, the launcher) as a size, or false when it
/// has none (The Order: the window keeps its size).
bool ChosenSize(int& w, int& h) {
    const int r = PortSettings::Get().proto_res;
    return r >= 0 && r < PortSettings::ProtoResolutionCount &&
           std::sscanf(PortSettings::ProtoResolutions[r], "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}
} // namespace

// Windowed (the chosen resolution's size, made to fit the screen), borderless (desktop-sized, no
// mode change) or exclusive fullscreen (the display mode closest to the chosen resolution, else
// the desktop's). Window thread only (SDL). The swapchain follows the size events.
void WindowSDL::ApplyDisplayMode(int mode) {
    display_mode = mode;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    int w = 0, h = 0;
    const bool chosen = ChosenSize(w, h);
    applied_size = chosen ? w * 100000 + h : 0;
    if (mode == PortGraphics::Windowed) {
        SDL_SetWindowFullscreen(window, false);
        if (chosen) {
            // A window as large as the screen would look like borderless: it fits the usable area
            // with room for its title bar, keeping the resolution's shape.
            SDL_Rect usable{};
            if (SDL_GetDisplayUsableBounds(display, &usable) && usable.w > 0 && usable.h > 64) {
                const double fit = std::min({1.0, 0.92 * usable.w / w, 0.92 * (usable.h - 48) / h});
                w = static_cast<int>(w * fit);
                h = static_cast<int>(h * fit);
            }
            SDL_SetWindowSize(window, w, h);
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                                  SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        }
    } else {
        const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
        SDL_DisplayMode closest{};
        const SDL_DisplayMode* exclusive = nullptr;
        if (mode == PortGraphics::Fullscreen) {
            exclusive = desktop;
            if (chosen && SDL_GetClosestFullscreenDisplayMode(display, w, h, desktop ? desktop->refresh_rate : 0.0f,
                                                               false, &closest)) {
                exclusive = &closest;
            }
        }
        if (!SDL_SetWindowFullscreenMode(window, exclusive)) {
            LOG_WARNING(Frontend, "Fullscreen mode: {}", SDL_GetError());
        }
        SDL_SetWindowFullscreen(window, true);
    }
    SDL_SyncWindow(window);
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    std::printf("Window: %s, %dx%d\n", PortGraphics::DisplayModeNames[mode], pw, ph);
    std::fflush(stdout);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    PortOverlay::SetTextEntry(text_active, text_prompt, text);
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        PortOverlay::UpdateTextInput(window);
    }
    // The display mode, and the window's size when the Resolution setting changes (windowed and
    // exclusive fullscreen follow it; the render resolution itself on the next start).
    if (const int mode = PortSettings::Get().display_mode; mode != display_mode) {
        ApplyDisplayMode(mode);
    } else if (int w = 0, h = 0; mode != PortGraphics::Borderless && ChosenSize(w, h) && w * 100000 + h != applied_size) {
        ApplyDisplayMode(mode);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (PortOverlay::HandleEvent(event)) {
            continue;
        }
        if (mouse.captured) {
            std::scoped_lock lock{mouse_mutex};
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                mouse.dx += event.motion.xrel;
                mouse.dy += event.motion.yrel;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                mouse.buttons |= SDL_BUTTON_MASK(event.button.button);
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                mouse.buttons &= ~SDL_BUTTON_MASK(event.button.button);
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                // Whole notches; high-resolution wheels add up fractions until one is complete.
                static float wheel_rest = 0.0f;
                float y = event.wheel.y + wheel_rest;
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    y = -event.wheel.y + wheel_rest;
                }
                const int notches = int(y);
                wheel_rest = y - float(notches);
                mouse.wheel += notches;
            }
        }
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            if (event.key.key == SDLK_F7 && !event.key.repeat) {
                auto& s = PortSettings::Get();
                s.mouse_look = !s.mouse_look;
                PortSettings::Save();
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            window_focused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    // Captured (relative mode, cursor hidden) only while the game itself takes the input: a
    // click that focuses the window or picks a menu entry never reaches the pad.
    const bool capture = PortSettings::Get().mouse_look && window_focused && !text_active &&
                         !PortOverlay::CapturesInput();
    if (capture != bool(mouse.captured)) {
        SDL_SetWindowRelativeMouseMode(window, capture);
        std::scoped_lock lock{mouse_mutex};
        mouse = {};
        mouse.captured = capture;
    }
    return is_open;
}

} // namespace Frontend
