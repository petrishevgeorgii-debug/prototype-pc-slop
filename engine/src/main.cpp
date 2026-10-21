// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: a PC engine that runs PROTOTYPE from your own copy's data (docs/ENGINE.md).
// Milestone 1: fly through the city.
//
//   manhattan <game folder> [--at x,y,z] [--look yaw,pitch] [--screenshot file.png]
//             [--size WxH] [--validation] [--mirror]
//
// The game folder is the one with eboot.bin (THEORDER_GAME_DIR is used when none is given).
// Controls: hold the right mouse button (or press Tab) to look around; W A S D to move, Space / C
// up and down, Shift faster, the mouse wheel changes the speed, Esc releases the mouse or quits.

#define SDL_MAIN_HANDLED // a plain main(); SDL is told it is ready
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "city.h"
#include "renderer.h"
#include "log.h"

using engine::Log;

namespace {

struct Camera {
    float position[3] = {0, 100, 0};
    float yaw = 0, pitch = -0.3f; // radians; yaw 0 looks along -z
    float speed = 30.0f;          // metres per second

    void Forward(float f[3]) const {
        f[0] = std::cos(pitch) * std::sin(yaw);
        f[1] = std::sin(pitch);
        f[2] = -std::cos(pitch) * std::cos(yaw);
    }
    void LookAt(const float target[3]) {
        const float dx = target[0] - position[0], dy = target[1] - position[1], dz = target[2] - position[2];
        yaw = std::atan2(dx, -dz);
        pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
    }
};

void Normalize(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
}

void Cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/// Column-major 4 x 4: out = a * b.
void Multiply(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
    }
}

/// The camera's view and an infinite, reverse-Z perspective (depth 1 at the near plane).
void ViewProjection(const Camera& cam, float aspect, bool mirror, float out[16]) {
    float f[3], up[3] = {0, 1, 0}, r[3], u[3];
    cam.Forward(f);
    Cross(f, up, r);
    Normalize(r);
    Cross(r, f, u);
    const float* e = cam.position;
    const float view[16] = {r[0], u[0], -f[0], 0,
                            r[1], u[1], -f[1], 0,
                            r[2], u[2], -f[2], 0,
                            -(r[0] * e[0] + r[1] * e[1] + r[2] * e[2]),
                            -(u[0] * e[0] + u[1] * e[1] + u[2] * e[2]),
                            f[0] * e[0] + f[1] * e[1] + f[2] * e[2], 1};
    const float near_plane = 0.25f;
    const float t = 1.0f / std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
    const float projection[16] = {(mirror ? -t : t) / aspect, 0, 0, 0,
                                  0, -t, 0, 0,
                                  0, 0, 0, -1,
                                  0, 0, near_plane, 0};
    Multiply(projection, view, out);
}

bool ParseFloats(const char* text, float* out, int n) {
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        out[i] = std::strtof(text, &end);
        if (end == text) return false;
        text = *end == ',' ? end + 1 : end;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path game;
    std::string screenshot;
    bool validation = false, mirror = false, at_given = false, look_given = false;
    int width = 1600, height = 900;
    Camera camera;
    float look[2] = {0, 0};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else if (a == "--validation") validation = true;
        else if (a == "--mirror") mirror = true;
        else if (a == "--at" && i + 1 < argc) at_given = ParseFloats(argv[++i], camera.position, 3);
        else if (a == "--look" && i + 1 < argc) look_given = ParseFloats(argv[++i], look, 2);
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &width, &height);
        else if (a == "--help" || a == "-h") {
            std::puts("manhattan <game folder> [--at x,y,z] [--look yaw,pitch] [--screenshot file.png] [--size WxH] "
                      "[--validation] [--mirror]");
            return 0;
        } else game = a;
    }
    if (game.empty()) {
        if (const char* env = std::getenv("THEORDER_GAME_DIR")) game = env;
    }
    engine::OpenLog("manhattan.log");
    Log("Manhattan Engine, milestone 1 (docs/ENGINE.md); game folder %s\n", game.string().c_str());
    if (game.empty()) {
        Log("Manhattan Engine: give the game folder (the one with eboot.bin)\n");
        return 2;
    }
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        Log("SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Manhattan Engine", width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        Log("SDL: %s\n", SDL_GetError());
        return 1;
    }
    engine::Renderer renderer;
    std::string error;
    if (!renderer.Init(window, validation, error)) {
        Log("Manhattan Engine: %s\n", error.c_str());
        renderer.Shutdown();
        SDL_Quit();
        return 1;
    }
    Log("Manhattan Engine on %s\n", renderer.DeviceName().c_str());
    int status = 0;
    {
        engine::City city;
        if (!city.Open(game, renderer, error)) {
            Log("Manhattan Engine: %s\n", error.c_str());
            city.Close();
            renderer.Shutdown();
            SDL_Quit();
            return 1;
        }
        float target[3];
        if (!at_given) {
            city.StartView(camera.position, target);
            camera.LookAt(target);
        }
        if (look_given) {
            camera.yaw = look[0] * 3.14159265f / 180.0f;
            camera.pitch = look[1] * 3.14159265f / 180.0f;
        }
        bool running = true, looking = false;
        auto last = std::chrono::steady_clock::now();
        auto title_time = last;
        int frames = 0, drawn = 0, idle_frames = 0, total_frames = 0;
        float sun[3] = {0.35f, 0.8f, 0.45f};
        Normalize(sun);
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                switch (ev.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_KEY_DOWN:
                    if (ev.key.key == SDLK_ESCAPE) {
                        if (looking) looking = false;
                        else running = false;
                    } else if (ev.key.key == SDLK_TAB) {
                        looking = !looking;
                    } else if (ev.key.key == SDLK_P) {
                        Log("Camera: --at %.1f,%.1f,%.1f --look %.1f,%.1f\n", camera.position[0], camera.position[1],
                                    camera.position[2], camera.yaw * 180 / 3.14159265f, camera.pitch * 180 / 3.14159265f);
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (ev.button.button == SDL_BUTTON_RIGHT) looking = true;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ev.button.button == SDL_BUTTON_RIGHT) looking = false;
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (looking) {
                        camera.yaw += ev.motion.xrel * 0.0025f * (mirror ? -1.0f : 1.0f);
                        camera.pitch = std::clamp(camera.pitch - ev.motion.yrel * 0.0025f, -1.5f, 1.5f);
                    }
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    camera.speed = std::clamp(camera.speed * (ev.wheel.y > 0 ? 1.25f : 0.8f), 2.0f, 2000.0f);
                    break;
                default: break;
                }
            }
            SDL_SetWindowRelativeMouseMode(window, looking);
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
            last = now;
            const bool* keys = SDL_GetKeyboardState(nullptr);
            float f[3], up[3] = {0, 1, 0}, r[3];
            camera.Forward(f);
            Cross(f, up, r);
            Normalize(r);
            const float speed = camera.speed * dt * (keys[SDL_SCANCODE_LSHIFT] ? 6.0f : 1.0f);
            const float side = mirror ? -1.0f : 1.0f;
            for (int k = 0; k < 3; ++k) {
                camera.position[k] += f[k] * speed * ((keys[SDL_SCANCODE_W] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_S] ? 1.0f : 0.0f));
                camera.position[k] += r[k] * speed * side * ((keys[SDL_SCANCODE_D] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_A] ? 1.0f : 0.0f));
            }
            camera.position[1] += speed * ((keys[SDL_SCANCODE_SPACE] ? 1.0f : 0.0f) - (keys[SDL_SCANCODE_C] ? 1.0f : 0.0f));

            city.Update(camera.position);
            std::vector<const engine::Model*> models;
            std::vector<bool> hidden;
            city.Models(models, hidden);
            std::uint32_t w = 0, h = 0;
            renderer.Size(w, h);
            engine::FrameView view{};
            ViewProjection(camera, h ? float(w) / float(h) : 1.0f, mirror, view.view_projection);
            std::copy(camera.position, camera.position + 3, view.camera);
            std::copy(sun, sun + 3, view.sun);
            view.fog_density = 0.0006f;
            if (renderer.DrawFrame(view, models, hidden)) ++drawn;
            ++frames;
            ++total_frames;
            if (!screenshot.empty()) {
                // Headless check: once everything around the camera is in, save and stop.
                idle_frames = city.Idle() ? idle_frames + 1 : 0;
                if ((idle_frames > 5 && drawn > 3) || total_frames > 20000) {
                    if (!renderer.SaveScreenshot(screenshot, error)) {
                        Log("Screenshot: %s\n", error.c_str());
                        status = 1;
                    } else {
                        Log("Screenshot: %s (%s)\n", screenshot.c_str(), city.Status().c_str());
                    }
                    running = false;
                }
            }
            if (std::chrono::duration<float>(now - title_time).count() > 0.5f) {
                const float fps = frames / std::chrono::duration<float>(now - title_time).count();
                char title[256];
                std::snprintf(title, sizeof(title), "Manhattan Engine  |  %.0f fps  |  %s  |  %.0f, %.0f, %.0f  |  %.0f m/s",
                              fps, city.Status().c_str(), camera.position[0], camera.position[1], camera.position[2],
                              camera.speed);
                SDL_SetWindowTitle(window, title);
                title_time = now;
                frames = 0;
            }
        }
        city.Close();
    }
    renderer.Shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
