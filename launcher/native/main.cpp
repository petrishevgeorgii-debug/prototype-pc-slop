// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE launcher, the native window: the frame (background, top bar, rail, dock), the launch
// panel, notices and questions, and the main loop.
//
//   launcher-ui                 run by the launcher process (stdin/stdout: its JSON lines)
//   launcher-ui --state F.json  a fixed state, no launcher process (for looking at the window)
//     --page NAME --game p1|p2 --shot OUT.bmp --frames N   draw N frames, save the last, quit
#include "app.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

Json ST;
std::string g_page = "home";
Capture g_capture;
extern bool g_in_modal;

namespace {
SDL_Window *g_win = nullptr;
Palette P1, P2;
float g_game_mix = 0;                  // 0: PROTOTYPE's colours, 1: PROTOTYPE 2's
SDL_Texture *t_noise = nullptr, *t_radial = nullptr;
float g_page_t0 = -10;
float g_scroll = 0, g_scroll_target = 0, g_scroll_max = 0;
bool g_boot_shown = false;
std::string g_last_exit = "-";
bool g_minimised_for_game = false;
bool g_offline = false;                // --state: no launcher process
struct Scripted { int frame; bool key; std::string what; };
std::vector<Scripted> g_script;        // --click / --key (tests)
std::string g_art_from, g_art_to;
float g_art_t0 = -10;
float g_poll_at = 0;
bool g_state_pending = false;
std::string g_last_busy;
ImRect g_capture_box;

struct Toast { std::string text, kind; float t0 = -10; } g_toast;
struct Modal {
    bool on = false;
    std::string title, text, list, yes;
    std::function<void()> on_yes;
    float t0 = 0;
} g_modal;

const char *PAGES[] = {"home", "graphics", "display", "controls", "gamepad", "saves", "mods", "news", "folder"};

void palettes() {
    P1 = {rgba(8, 8, 10), rgba(14, 13, 15, 0.72f), rgba(14, 13, 15), rgba(236, 228, 218, 0.10f), rgba(236, 228, 218, 0.18f),
          rgba(0xe9, 0xe4, 0xde), rgba(0x8d, 0x87, 0x82), rgba(0x57, 0x52, 0x4e), rgba(0xb5, 0x14, 0x1c), rgba(0xd8, 0x26, 0x1f),
          rgba(0xf3, 0xdc, 0xd8)};
    P2 = {rgba(7, 9, 11), rgba(12, 15, 18, 0.72f), rgba(12, 15, 18), rgba(220, 232, 240, 0.10f), rgba(220, 232, 240, 0.18f),
          rgba(0xe3, 0xe8, 0xeb), rgba(0x86, 0x90, 0x9a), rgba(0x50, 0x5a, 0x63), rgba(0x2a, 0x6f, 0x9c), rgba(0x3d, 0x8c, 0xc0),
          rgba(0xdb, 0xe9, 0xf2)};
}

void blend_palette() {
    float m = g_game_mix;
    ImVec4 *a = &P1.void_, *b = &P2.void_, *o = &P.void_;
    for (int i = 0; i < 11; ++i) o[i] = mix(a[i], b[i], m);
}

std::string art_dir() { return ST["artDir"].str(); }
std::string sep() {
#ifdef _WIN32
    return "\\";
#else
    return "/";
#endif
}

// ---------------------------------------------------------------- textures made here
void make_textures() {
    t_noise = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, 220, 140);
    SDL_SetTextureBlendMode(t_noise, SDL_BLENDMODE_ADD);
    SDL_SetTextureScaleMode(t_noise, SDL_SCALEMODE_NEAREST);
    // The page's radial tint: clear at the centre, half dark at 60 %, the background colour at the edge.
    const int N = 256;
    std::vector<Uint32> px(N * N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float nx = (x + 0.5f) / N * 2 - 1, ny = (y + 0.5f) / N * 2 - 1, d = sqrtf(nx * nx + ny * ny);
            float a = d < 0.6f ? 0.5f * d / 0.6f : 0.5f + 0.5f * ImMin(1.f, (d - 0.6f) / 0.4f);
            px[y * N + x] = 0x00ffffffu | ((Uint32)(a * 255) << 24);
        }
    t_radial = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, N, N);
    SDL_UpdateTexture(t_radial, nullptr, px.data(), N * 4);
    SDL_SetTextureBlendMode(t_radial, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t_radial, SDL_SCALEMODE_LINEAR);
}

void refresh_noise() {
    static float last = -1;
    if (g_time - last < 0.12f) return;
    last = g_time;
    void *pixels;
    int pitch;
    if (!SDL_LockTexture(t_noise, nullptr, &pixels, &pitch)) return;
    static Uint32 seed = 12345;
    for (int y = 0; y < 140; ++y) {
        Uint32 *row = (Uint32 *)((Uint8 *)pixels + y * pitch);
        for (int x = 0; x < 220; ++x) {
            seed = seed * 1664525u + 1013904223u;
            Uint32 v = seed >> 24;
            row[x] = v | v << 8 | v << 16 | 0xff000000u;
        }
    }
    SDL_UnlockTexture(t_noise);
}

// ---------------------------------------------------------------- notices
void notice(const Json &n) {
    if (n.is_null()) return;
    std::string kind = n["kind"].str();
    if (!n["text"].str().empty()) { toast(n["text"].str(), kind); return; }
    std::string key = "n_" + n["key"].str();
    toast(t(key.c_str(), {{"n", std::to_string(n["n"].integer())}}), kind);
}

void apply_state(const Json &s) {
    std::string old_game = ST["game"].str();
    bool had_art = !ST["art"].is_null();
    ST = s;
    if (!old_game.empty() && old_game != ST["game"].str()) hero_glitch();
    if (!had_art && !ST["art"].is_null()) wall_reset();
    // The window's icon: the game's own (made from sce_sys/icon0.png with the pictures).
    static bool icon_set = false;
    if (!icon_set && !art_dir().empty()) {
        if (SDL_Surface *icon = SDL_LoadBMP((art_dir() + sep() + "icon.bmp").c_str())) {
            SDL_SetWindowIcon(g_win, icon);
            SDL_DestroySurface(icon);
            icon_set = true;
        }
    }
}

void fetch_state() {
    if (g_offline || g_state_pending) return;
    g_state_pending = true;
    Ipc::request("state", Json::object(), [](const Json &r, const std::string &err) {
        g_state_pending = false;
        if (err.empty()) apply_state(r), notice(r["notice"]);
    });
}

void exit_notice() {
    const Json &l = ST["launch"];
    std::string e = l["exit"].dump();
    if (!l["exit"].is_null() && e != g_last_exit) {
        if (g_last_exit != "-" || g_boot_shown) {
            int code = l["exit"]["code"].integer();
            toast(code == 0 ? t("exitOk") : code == 42 ? t("exit42") : t("exitCode", {{"code", std::to_string(code)}}), code == 0 ? "" : "bad");
        }
        g_boot_shown = false;
    }
    g_last_exit = e;
}

void poll() {
    if (g_offline || g_time < g_poll_at) return;
    bool booting = ST["launch"]["running"].truthy() && g_boot_shown;
    g_poll_at = g_time + (booting ? 0.6f : 1.5f);
    Ipc::request("poll", Json::object(), [](const Json &p, const std::string &err) {
        if (!err.empty()) return;
        bool was_running = ST["launch"]["running"].truthy(), had_art = !ST["art"].is_null();
        Json st = ST;
        st.set("launch", p["launch"]);
        st.set("pad", p["pad"]);
        if (!p["update"].is_null()) st.set("update", p["update"]);
        ST = st;
        notice(p["notice"]);
        std::string busy = p["launch"]["busy"].str();
        if ((p["artReady"].truthy() && !had_art) || (was_running && !p["launch"]["running"].truthy()) || busy != g_last_busy) fetch_state();
        g_last_busy = busy;
        exit_notice();
    });
}

// ---------------------------------------------------------------- drawing helpers for the frame
void hline(float x0, float x1, float y, ImU32 c) { DL->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + ImMax(1.f, S)), c); }
void vline(float x, float y0, float y1, ImU32 c) { DL->AddRectFilled(ImVec2(x, y0), ImVec2(x + ImMax(1.f, S), y1), c); }

void icon(int which, ImVec2 o, float size, ImU32 c) {
    float k = size / 24, th = 1.6f * k;
    auto p = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
    auto line = [&](float x0, float y0, float x1, float y1) { DL->AddLine(p(x0, y0), p(x1, y1), c, th); };
    auto poly = [&](std::initializer_list<ImVec2> pts, bool closed) {
        std::vector<ImVec2> v;
        for (auto q : pts) v.push_back(p(q.x, q.y));
        DL->AddPolyline(v.data(), (int)v.size(), c, th, closed ? ImDrawFlags_Closed : 0);
    };
    switch (which) {
    case 0: poly({{4, 20}, {4, 9}, {12, 4}, {20, 9}, {20, 20}, {15, 20}, {15, 14}, {9, 14}, {9, 20}}, true); break;
    case 1:
        DL->AddRect(p(3, 6), p(21, 18), c, 0, th);
        poly({{3, 15}, {8, 10}, {12, 14}, {15, 11}, {21, 17}}, false);
        DL->AddCircle(p(16, 9), 1.5f * k, c, 12, th);
        break;
    case 2: DL->AddRect(p(3, 4), p(21, 16), c, 0, th); line(8, 20, 16, 20); line(12, 16, 12, 20); break;
    case 3:
        DL->AddRect(p(3, 7), p(21, 17), c, 0, th);
        for (float x : {6.f, 9.f, 12.f, 15.f}) line(x, 10, x + 1, 10);
        line(7, 14, 17, 14);
        break;
    case 4:
        DL->PathLineTo(p(7, 8));
        DL->PathLineTo(p(17, 8));
        DL->PathArcTo(p(17, 12), 4 * k, -IM_PI / 2, 0, 8);
        DL->PathLineTo(p(20.4f, 16.2f));
        DL->PathArcTo(p(18.2f, 16.f), 2.2f * k, 0.1f, IM_PI * 0.75f, 6);
        DL->PathLineTo(p(14.5f, 15));
        DL->PathLineTo(p(9.5f, 15));
        DL->PathArcTo(p(5.8f, 16.f), 2.2f * k, IM_PI * 0.25f, IM_PI * 0.9f, 6);
        DL->PathLineTo(p(3, 12));
        DL->PathArcTo(p(7, 12), 4 * k, IM_PI, IM_PI * 1.5f, 8);
        DL->PathStroke(c, th, ImDrawFlags_Closed);
        line(7.5f, 11, 7.5f, 14);
        line(6, 12.5f, 9, 12.5f);
        break;
    case 5:
        poly({{5, 3}, {16, 3}, {19, 6}, {19, 21}, {5, 21}}, true);
        poly({{8, 3}, {8, 8}, {15, 8}, {15, 3}}, false);
        poly({{8, 21}, {8, 14}, {16, 14}, {16, 21}}, false);
        break;
    case 6:
        poly({{12, 3}, {20, 7.5f}, {20, 16.5f}, {12, 21}, {4, 16.5f}, {4, 7.5f}}, true);
        line(12, 12, 20, 7.5f); line(12, 12, 12, 21); line(12, 12, 4, 7.5f);
        break;
    case 7:
        poly({{4, 5}, {17, 5}, {17, 19}, {6, 19}, {4.6f, 18.4f}, {4, 17}}, true);
        poly({{17, 9}, {20, 9}, {20, 17}, {19.4f, 18.4f}, {18, 19}}, false);
        line(7, 9, 14, 9); line(7, 12, 14, 12); line(7, 15, 11, 15);
        break;
    case 8: poly({{3, 6}, {10, 6}, {12, 8}, {21, 8}, {21, 19}, {3, 19}}, true); break;
    }
}

void mark(ImVec2 o, float size, ImU32 c) {  // the rail's emblem (32 units)
    float k = size / 32, th = 2 * k;
    auto p = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
    DL->AddCircle(p(16, 16), 4.2f * k, c, 24, th);
    DL->AddLine(p(16, 11.8f), p(16, 3.5f), c, th);
    DL->AddLine(p(12.4f, 18.1f), p(5.2f, 22.3f), c, th);
    DL->AddLine(p(19.6f, 18.1f), p(26.8f, 22.3f), c, th);
    auto arc = [&](float x0, float y0, float x1, float y1) {
        float a0 = atan2f(y0 - 16, x0 - 16), a1 = atan2f(y1 - 16, x1 - 16);
        if (a1 - a0 > IM_PI) a1 -= 2 * IM_PI;
        if (a0 - a1 > IM_PI) a1 += 2 * IM_PI;
        DL->PathArcTo(p(16, 16), 11 * k, a0, a1, 16);
        DL->PathStroke(c, th);
    };
    arc(10, 6.5f, 5.4f, 18.3f);
    arc(22, 6.5f, 26.6f, 18.3f);
    arc(9.4f, 26.6f, 22.6f, 26.6f);
}

// ---------------------------------------------------------------- background
void background(float W, float H) {
    DL = ImGui::GetBackgroundDrawList();
    DL->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), col(P.void_));
    std::string want = ST["game"].str() == "p2" ? "heller" : "alex";
    if (!ST["art"]["art"].contains_string(want)) want.clear();
    if (want != g_art_to) { g_art_from = g_art_to; g_art_to = want; g_art_t0 = g_time; }
    float fade = ease_in_out((g_time - g_art_t0) / 0.7f);
    ImVec2 mouse = ImGui::GetIO().MousePos;
    ImVec2 par(mouse.x > 0 ? (mouse.x / W - 0.5f) * -10 * S : 0, mouse.y > 0 ? (mouse.y / H - 0.5f) * -6 * S : 0);
    static ImVec2 par_s(0, 0);
    par_s.x += (par.x - par_s.x) * (1 - expf(-g_dt * 3));
    par_s.y += (par.y - par_s.y) * (1 - expf(-g_dt * 3));
    float h = H * 1.06f, w = h * 600 / 675, x1 = W + W * 0.04f, y0 = -H * 0.02f;
    float breathe = 1 + 0.012f * (0.5f + 0.5f * sinf(g_time * 0.25f));
    ImRect r(ImVec2(x1 - w * breathe + par_s.x, y0 + par_s.y), ImVec2(x1 + par_s.x, y0 + h * breathe + par_s.y));
    static const float stops[] = {0, 0.42f, 0.86f, 1}, alphas[] = {0, 1, 1, 0};
    auto draw_art = [&](const std::string &name, float a) {
        if (name.empty() || a <= 0.001f) return;
        Picture *pic = picture(art_dir() + sep() + name + ".bmp");
        if (!pic) return;
        ImVec4 tint(0.92f, 0.92f, 0.92f, 1);
        quad_h_gradient((ImTextureID)(intptr_t)pic->gray, r, ImVec2(0, 0), ImVec2(1, 1), stops, alphas, 4, col(tint, 0.5f * a));
        quad_h_gradient((ImTextureID)(intptr_t)pic->color, r, ImVec2(0, 0), ImVec2(1, 1), stops, alphas, 4, col(tint, 0.5f * a * 0.45f));
    };
    draw_art(g_art_from, 1 - fade);
    draw_art(g_art_to, fade);
    // Tint: the radial one around the art, then the left-to-right one under the text.
    float rx = 1100 * S, ry = 760 * S, cx = W * 0.8f, cy = H * 0.28f;
    ImU32 v = col(P.void_);
    DL->AddImage((ImTextureID)(intptr_t)t_radial, ImVec2(cx - rx, cy - ry), ImVec2(cx + rx, cy + ry), ImVec2(0, 0), ImVec2(1, 1), v);
    if (cy + ry < H) DL->AddRectFilled(ImVec2(0, cy + ry), ImVec2(W, H), v);
    if (cx - rx > 0) DL->AddRectFilled(ImVec2(0, 0), ImVec2(cx - rx, H), v);
    ImVec4 vo = P.void_;
    const float ls[] = {0, 0.36f, 0.72f, 1}, la[] = {1, 0.9f, 0.35f, 0.6f};
    for (int i = 0; i < 3; ++i) {
        ImU32 a = col(ImVec4(vo.x, vo.y, vo.z, la[i])), b = col(ImVec4(vo.x, vo.y, vo.z, la[i + 1]));
        DL->AddRectFilledMultiColor(ImVec2(W * ls[i], 0), ImVec2(W * ls[i + 1], H), a, b, b, a);
    }
    refresh_noise();
    DL->AddImage((ImTextureID)(intptr_t)t_noise, ImVec2(0, 0), ImVec2(W, H), ImVec2(0, 0), ImVec2(1, 1), col(ImVec4(1, 1, 1, 0.045f)));
    ImU32 scan = col(ImVec4(0, 0, 0, 0.16f));
    float step = 3 * ImMax(1.f, floorf(S + 0.25f)), lh = ImMax(1.f, floorf(S + 0.25f));
    for (float y = 0; y < H; y += step) DL->AddRectFilled(ImVec2(0, y), ImVec2(W, y + lh), scan);
}

// ---------------------------------------------------------------- top bar
void top_bar(float x, float W) {
    float h = 52 * S, y = 0;
    hline(x, W, h - S, col(P.line));
    float cx = x + 28 * S;
    // [PROTOTYPE]
    float by = y + (h - 17 * 1.2f * S) / 2;
    cx += text(DISPLAY_M, 17, cx, by, col(P.accent_hi), "[", 0.14f);
    cx += 0.14f * 17 * S;
    cx += text(DISPLAY_M, 17, cx, by, col(P.text), "PROTOTYPE", 0.14f);
    cx += 0.14f * 17 * S;
    cx += text(DISPLAY_M, 17, cx, by, col(P.accent_hi), "]", 0.14f);
    if (W > 1380 * S) cx += 12 * S + text(MONO, 10, cx + 12 * S, by + 5 * S, col(P.dim), t("brandSub"), 0.18f);
    cx += 18 * S + 22 * S;
    // The game switch, its underline gliding to the chosen one.
    bool running = ST["launch"]["running"].truthy();
    static float ul_x = -1, ul_w = 0;
    for (int i = 0; i < 2; ++i) {
        const char *g = i ? "p2" : "p1";
        std::string name = i ? "PROTOTYPE 2" : "PROTOTYPE", sub = t(i ? "p2Sub" : "p1Sub");
        bool on = ST["game"].str() == g;
        float wn = text_width(DISPLAY, 13, name, 0.16f), ws = W > 1100 * S ? text_width(MONO, 10, sub, 0.04f) + 8 * S : 0;
        ImRect r(ImVec2(cx, y), ImVec2(cx + wn + ws, h));
        Hit hb = hit(std::string("switch-") + g, r, !running);
        float hv = approach((std::string("sw-h-") + g).c_str(), on || hb.hovered ? 1.f : 0.f, 12);
        float a = running ? 0.5f : 1;
        text(DISPLAY, 13, cx, y + (h - 13 * 1.2f * S) / 2, col(mix(P.dim, P.text, hv), a), name, 0.16f);
        if (ws) text(MONO, 10, cx + wn + 8 * S, y + (h - 10 * 1.2f * S) / 2 + 1 * S, col(P.dim, a), sub, 0.04f);
        if (on) {
            if (ul_x < 0) ul_x = cx, ul_w = wn + ws;
            ul_x += (cx - ul_x) * (1 - expf(-g_dt * 14));
            ul_w += (wn + ws - ul_w) * (1 - expf(-g_dt * 14));
        }
        if (hb.pressed && !on) {
            act("game", [&] { Json j = Json::object(); j.set("game", g); return j; }());
            ST.set("game", g);  // at once: the colours and the picture start moving now
            hero_glitch();
            wall_reset();
        }
        cx += wn + ws + 22 * S;
    }
    if (ul_x >= 0) hline(ul_x, ul_x + ul_w, h - S, col(P.accent_hi));
    // Right: the gamepad and the version.
    float rx = W - 16 * S;
    std::string ver = "v" + ST["version"].str();
    std::string beta = ST["beta"].integer() ? " · b" + std::to_string(ST["beta"].integer()) : "";
    float vw = text_width(MONO, 10.5f, "v", 0.04f) + text_width(MONO, 10.5f, ST["version"].str(), 0.04f) + text_width(MONO, 10.5f, beta, 0.04f);
    rx -= vw + 12 * S;
    float ty = y + (h - 10.5f * 1.2f * S) / 2;
    float vx = rx + text(MONO, 10.5f, rx, ty, col(P.muted), "v", 0.04f);
    vx += text(MONO, 10.5f, vx, ty, col(P.text), ST["version"].str(), 0.04f);
    text(MONO, 10.5f, vx, ty, col(P.muted), beta, 0.04f);
    const Json &pad = ST["pad"];
    if (!pad.is_null()) {
        std::string kind = pad["kind"].str() == "xbox" ? "XBOX" : upper(pad["type"].str().empty() ? "PLAYSTATION" : pad["type"].str());
        if (kind == "PS5") kind = "DUALSENSE";
        if (kind == "PS4") kind = "DUALSHOCK 4";
        std::string lead = ru() ? "ГЕЙМПАД " : "GAMEPAD ";
        float pw = 12 * S + text_width(MONO, 10.5f, lead, 0.04f) + text_width(MONO, 10.5f, kind, 0.04f);
        float px = rx - 18 * S - pw;
        float pulse = 0.6f + 0.4f * (0.5f + 0.5f * sinf(g_time * 2.2f));
        DL->AddCircleFilled(ImVec2(px + 2.5f * S, ty + 6.5f * S), 2.5f * S, col(OK, pulse));
        px += 12 * S;
        px += text(MONO, 10.5f, px, ty, col(P.muted), lead, 0.04f);
        text(MONO, 10.5f, px, ty, col(P.text), kind, 0.04f);
    }
}

// ---------------------------------------------------------------- rail
void rail(float H) {
    float w = 76 * S, top = 0, dock = 88 * S;
    (void)dock;
    DL->AddRectFilled(ImVec2(0, top), ImVec2(w, H), col(ImVec4(0, 0, 0, 0.42f)));
    vline(w - S, top, H, col(P.line));
    mark(ImVec2((w - 26 * S) / 2, top + 8 * S + (44 - 26) / 2 * S), 26 * S, col(P.accent_hi));
    const char *labels[] = {"navHome", "navGraphics", "navDisplay", "navKeys", "navPad", "navSaves", "navMods", "navNews", "navFolder"};
    float y = top + 8 * S + 44 * S + 10 * S, item = 62 * S;
    static float bar_y = -1;
    float target_bar = -1;
    for (int i = 0; i < 9; ++i) {
        float iy = i == 8 ? H - 12 * S - item : y + i * item;
        ImRect r(ImVec2(0, iy), ImVec2(w, iy + item));
        Hit h = hit(std::string("nav-") + PAGES[i], r);
        if (h.pressed) go(PAGES[i]);
        bool on = g_page == PAGES[i];
        float hv = approach((std::string("nav-h-") + PAGES[i]).c_str(), on || h.hovered ? 1.f : 0.f, 12);
        ImU32 c = col(mix(P.dim, P.text, hv));
        icon(i, ImVec2((w - 19 * S) / 2, iy + 10 * S), 19 * S, c);
        std::string label = t(labels[i]);
        float lw = text_width(BODY_M, 10, label, 0.02f);
        text(BODY_M, 10, (w - lw) / 2, iy + 10 * S + 19 * S + 5 * S, c, label, 0.02f);
        char k[2] = {(char)('1' + i), 0};
        text(MONO, 8.5f, w - 15 * S, iy + 6 * S, col(P.dim, 0.5f + 0.5f * hv), k);
        if (i == 7 && ST["update"]["newer"].truthy())
            DL->AddCircleFilled(ImVec2(w / 2 + 12 * S, iy + 10 * S), 2.5f * S, col(P.accent_hi));
        if (on) target_bar = iy;
    }
    if (target_bar >= 0) {
        if (bar_y < 0) bar_y = target_bar;
        bar_y += (target_bar - bar_y) * (1 - expf(-g_dt * 16));
        DL->AddRectFilled(ImVec2(0, bar_y + 10 * S), ImVec2(2 * S, bar_y + item - 10 * S), col(P.accent_hi));
    }
}

// ---------------------------------------------------------------- dock
void dock(float x, float W, float H) {
    float h = 88 * S, y = H - h;
    DL->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(W, H), col(ImVec4(0, 0, 0, 0.25f)), col(ImVec4(0, 0, 0, 0.25f)),
                                col(ImVec4(0, 0, 0, 0.7f)), col(ImVec4(0, 0, 0, 0.7f)));
    hline(x, W, y, col(P.line));
    const Json &l = ST["launch"];
    bool running = l["running"].truthy(), ready = ST["ready"].truthy(), busy = !l["busy"].str().empty();
    // PLAY
    float bx = x + 40 * S, bh = 54 * S, by = y + (h - bh) / 2;
    std::string label = running ? t("stop") : t("play");
    std::string sub = running ? t("running") + " · " + fmt_time((long long)(time(nullptr) - (long long)l["since"].num())) : game_name() + " · ENTER";
    float lw = ImMax(text_width(DISPLAY_M, 22, label, 0.22f), text_width(MONO, 9.5f, sub, 0.12f));
    float bw = ImMax(236 * S, 26 * S + 16 * S + 16 * S + lw + 30 * S);
    ImRect pr(ImVec2(bx, by), ImVec2(bx + bw, by + bh));
    bool enabled = running || (ready && !busy);
    Hit hp = hit("play", pr, enabled);
    if (hp.pressed) play();
    float hv = approach("play-h", hp.hovered ? 1.f : 0.f, 14);
    float press = approach("play-p", hp.held ? 1.f : 0.f, 30);
    float armed = approach("dock-armed", running || hp.hovered ? 1.f : 0.f, 5);
    hline(x, x + (W - x) * ease_out(armed), y - S, col(P.accent_hi));
    float a = enabled ? 1 : 0.45f;
    ImRect pb = pr;
    pb.Expand(-press * 1.5f * S);
    if (running) {
        DL->AddRectFilled(pb.Min, pb.Max, col(ImVec4(1, 1, 1, 0.05f * hv)));
        DL->AddRect(pb.Min, pb.Max, col(P.line_strong), 0, S);
    } else {
        DL->AddRectFilled(pb.Min, pb.Max, col(mix(P.accent, P.accent_hi, hv), a));
        // A light sweep across the button while it is under the cursor.
        if (hv > 0.01f) {
            float sx = pb.Min.x + fmodf(g_time * 0.9f, 1.6f) / 1.6f * (pb.GetWidth() + 120 * S) - 60 * S;
            DL->PushClipRect(pb.Min, pb.Max, true);
            DL->AddRectFilledMultiColor(ImVec2(sx - 40 * S, pb.Min.y), ImVec2(sx, pb.Max.y), col(ImVec4(1, 1, 1, 0)), col(ImVec4(1, 1, 1, 0.10f * hv)),
                                        col(ImVec4(1, 1, 1, 0.10f * hv)), col(ImVec4(1, 1, 1, 0)));
            DL->AddRectFilledMultiColor(ImVec2(sx, pb.Min.y), ImVec2(sx + 40 * S, pb.Max.y), col(ImVec4(1, 1, 1, 0.10f * hv)), col(ImVec4(1, 1, 1, 0)),
                                        col(ImVec4(1, 1, 1, 0)), col(ImVec4(1, 1, 1, 0.10f * hv)));
            DL->PopClipRect();
        }
    }
    ImU32 ink = col(running ? P.text : ImVec4(1, 1, 1, 1), a);
    float ix = pb.Min.x + 26 * S, iy = pb.Min.y + bh / 2;
    if (running) DL->AddRectFilled(ImVec2(ix + 2 * S, iy - 6 * S), ImVec2(ix + 14 * S, iy + 6 * S), ink);
    else DL->AddTriangleFilled(ImVec2(ix + 1 * S, iy - 7.5f * S), ImVec2(ix + 15 * S, iy), ImVec2(ix + 1 * S, iy + 7.5f * S), ink);
    float tx = ix + 16 * S + 16 * S;
    text(DISPLAY_M, 22, tx, iy - 17 * S, ink, label, 0.22f);
    text(MONO, 9.5f, tx, iy + 7 * S, col(running ? P.text : ImVec4(1, 1, 1, 1), a * 0.75f), sub, 0.12f);
    // The facts.
    const Json &prep = ST["prepared"][ST["game"].str()];
    long long played = (long long)ST["playTime"][ST["game"].str()].num();
    struct Fact { std::string k, v; bool ok; };
    std::vector<Fact> facts = {
        {t("prepState"), prep.truthy() ? t("prepReady") : t("prepFirst"), prep.truthy()},
        {t("cache"), std::to_string(ST["cache"]["mb"].integer()) + (ru() ? " МБ · " : " MB · ") + std::to_string(ST["cache"]["files"].integer()), false},
        {t("playTime"), played ? fmt_time(played) : t("never"), false},
        {"Discord", V("discord").truthy() ? t("discordOn") : t("discordOff"), false}};
    float fx = pr.Max.x + 30 * S;
    float links_w = 0;
    std::string l1 = t("log"), l2 = t("portFolder");
    links_w = text_width(BODY, 12, l1) + text_width(BODY, 12, l2) + 4 * 10 * S + 4 * S;
    for (auto &f : facts) {
        float fw = ImMax(text_width(MONO, 9.5f, upper(f.k), 0.1f), text_width(BODY_M, 13, f.v) + (f.ok ? 12 * S : 0));
        if (fx + fw > W - 28 * S - links_w - 20 * S) break;
        float fy = y + h / 2 - 18 * S;
        text(MONO, 9.5f, fx, fy, col(P.dim), upper(f.k), 0.1f);
        float vx = fx;
        if (f.ok) { DL->AddCircleFilled(ImVec2(vx + 2.5f * S, fy + 22 * S), 2.5f * S, col(OK)); vx += 12 * S; }
        text(BODY_M, 13, vx, fy + 14 * S, col(P.text), f.v);
        fx += fw + 30 * S;
    }
    // Links.
    float lx = W - 28 * S - links_w;
    for (int i = 0; i < 2; ++i) {
        const std::string &s = i ? l2 : l1;
        float sw = text_width(BODY, 12, s) + 20 * S;
        ImRect r(ImVec2(lx, y + h / 2 - 14 * S), ImVec2(lx + sw, y + h / 2 + 14 * S));
        Hit hl = hit(std::string("link-") + (i ? "port" : "log"), r);
        float lh = approach((std::string("link-h") + (i ? "p" : "l")).c_str(), hl.hovered ? 1.f : 0.f);
        text(BODY, 12, lx + 10 * S, y + h / 2 - 7.5f * S, col(mix(P.muted, P.text, lh)), s);
        if (hl.pressed) {
            Json j = Json::object();
            j.set("what", i ? "port" : "log");
            Ipc::request("open", j, [](const Json &r, const std::string &) { if (!r["ok"].truthy()) toast(t("n_openFailed")); });
        }
        lx += sw + 4 * S;
    }
}

// ---------------------------------------------------------------- the launch panel
void boot_panel(float W, float H) {
    const Json &l = ST["launch"];
    bool on = l["running"].truthy() && g_boot_shown;
    float a = approach("boot-on", on ? 1.f : 0.f, 10);
    float dim = approach("boot-dim", on ? 1.f : 0.f, 4);
    if (dim > 0.01f) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(W, H));
        ImGui::Begin("##dim", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        DL = ImGui::GetWindowDrawList();
        DL->AddRectFilled(ImVec2(0, 0), ImVec2(W, 52 * S), col(ImVec4(0, 0, 0, 0.55f * dim)));
        DL->AddRectFilled(ImVec2(0, 52 * S), ImVec2(W, H - 88 * S), col(ImVec4(0, 0, 0, 0.55f * dim)));
        ImGui::End();
    }
    if (a < 0.01f) return;
    float x = 76 * S + 40 * S, w = ImMin(680 * S, W - 76 * S - 80 * S), h = 18 * S + 22 * S + 10 * S + 2 * S + 10 * S + 196 * S + 10 * S + 30 * S + 16 * S;
    float y = H - 88 * S - 14 * S - h + (1 - ease_out(a)) * 12 * S;
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("##boot", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    DL = ImGui::GetWindowDrawList();
    float old = g_alpha;
    g_alpha = a;
    DL->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col(rgba(10, 9, 11, 0.97f)));
    DL->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col(P.line_strong), 0, S);
    float px = x + 20 * S, pw = w - 40 * S, cy = y + 18 * S;
    long long secs = ImMax(0LL, (long long)(time(nullptr) - (long long)l["since"].num()));
    std::string title = t("starting", {{"game", l["game"].str() == "p2" ? "PROTOTYPE 2" : "PROTOTYPE"}});
    text(DISPLAY_M, 18, px, cy, col(P.text), title, 0.14f);
    std::string tsec = std::to_string(secs) + " s";
    text(MONO, 11, px + pw - text_width(MONO, 11, tsec), cy + 6 * S, col(P.muted), tsec);
    cy += 22 * S + 10 * S;
    DL->AddRectFilled(ImVec2(px, cy), ImVec2(px + pw, cy + 2 * S), col(P.line));
    float prog = approach("boot-bar", ImMin(1.f, secs / 12.f), 3);
    DL->AddRectFilled(ImVec2(px, cy), ImVec2(px + pw * prog, cy + 2 * S), col(P.accent_hi));
    if (prog < 1) {  // a glint running along the bar while it fills
        float gx = px + pw * prog * fmodf(g_time * 0.8f, 1.f);
        DL->AddRectFilled(ImVec2(gx - 12 * S, cy), ImVec2(gx, cy + 2 * S), col(ImVec4(1, 1, 1, 0.35f)));
    }
    cy += 2 * S + 10 * S;
    float lh = 11.5f * 1.7f * S;
    int fit = (int)(196 * S / lh);
    const Json &log = l["log"];
    int n = (int)log.size(), from = ImMax(0, n - fit);
    DL->PushClipRect(ImVec2(px, cy), ImVec2(px + pw, cy + 196 * S), true);
    for (int i = from; i < n; ++i) {
        std::string line = log[i].str();
        std::string low = line;
        for (auto &ch : low) ch = (char)tolower((unsigned char)ch);
        bool ok = low.find("ready") != std::string::npos || low.find(" ok") != std::string::npos || low.find("up to date") != std::string::npos ||
                  low.find("backed up") != std::string::npos;
        bool bad = low.find("error") != std::string::npos || low.find("failed") != std::string::npos || low.find("fatal") != std::string::npos;
        ImVec4 c = bad ? P.text : ok ? OK : P.muted;
        // The newest line types itself in.
        std::string shown = line;
        if (i == n - 1) {
            static std::string last_line;
            static float since = 0;
            if (line != last_line) last_line = line, since = g_time;
            size_t chars = (size_t)((g_time - since) * 240);
            if (chars < shown.size()) shown = shown.substr(0, chars);
        }
        text(MONO, 11.5f, px, cy + (i - from) * lh + (lh - 11.5f * 1.2f * S) / 2, col(c), shown, 0, pw);
    }
    DL->PopClipRect();
    cy += 196 * S + 10 * S;
    text(BODY, 12, px, cy + 7 * S, col(P.muted), secs < 12 ? t("launchNote") : t("launchRunning"));
    std::string hide = t("hide");
    float hw = text_width(BODY_M, 12.5f, hide) + 28 * S;
    ImRect hr(ImVec2(px + pw - hw, cy), ImVec2(px + pw, cy + 30 * S));
    Hit hh = hit("boot-hide", hr);
    float hv = approach("boot-hide-h", hh.hovered ? 1.f : 0.f);
    DL->AddRectFilled(hr.Min, hr.Max, col(ImVec4(1, 1, 1, 0.05f * hv)));
    DL->AddRect(hr.Min, hr.Max, col(mix(P.line_strong, P.muted, hv)), 0, S);
    text(BODY_M, 12.5f, hr.Min.x + 14 * S, cy + 7 * S, col(P.text), hide);
    if (hh.pressed) g_boot_shown = false;
    g_alpha = old;
    ImGui::End();
}

// ---------------------------------------------------------------- notices and questions
void toast_draw(float W, float H) {
    float age = g_time - g_toast.t0;
    float a = age < 3.6f ? ease_out(age / 0.25f) : 1 - ease_out((age - 3.6f) / 0.25f);
    if (a <= 0.01f || g_toast.text.empty()) return;
    DL = ImGui::GetForegroundDrawList();
    float maxw = ImMin(420 * S, W - 32 * S);
    float tw = para(BODY, BODY_M, 13, 0, 0, maxw - 32 * S, 0, 0, g_toast.text, 1.5f, false);
    float textw = ImMin(maxw - 32 * S, text_width(BODY, 13, g_toast.text));
    float w = textw + 32 * S, h = tw + 20 * S;
    float x = W - 28 * S - w, y = H - 88 * S - 16 * S - h + (1 - a) * 10 * S;
    float old = g_alpha;
    g_alpha = a;
    ImVec4 border = g_toast.kind == "bad" ? mix(P.line_strong, P.accent_hi, 0.6f) : g_toast.kind == "ok" ? mix(P.line_strong, OK, 0.5f) : P.line_strong;
    DL->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col(P.panel_solid));
    DL->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col(border), 0, S);
    if (g_toast.kind == "bad" || g_toast.kind == "ok")
        DL->AddRectFilled(ImVec2(x, y), ImVec2(x + 2 * S, y + h), col(g_toast.kind == "bad" ? P.accent_hi : OK));
    para(BODY, BODY_M, 13, x + 16 * S, y + 10 * S, maxw - 32 * S, col(P.text), col(P.text), g_toast.text);
    g_alpha = old;
}

void modal_draw(float W, float H) {
    float a = approach("modal-on", g_modal.on ? 1.f : 0.f, 16);
    g_block_input = g_modal.on;
    if (a < 0.01f) return;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::Begin("##modal", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFocus();
    DL = ImGui::GetWindowDrawList();
    float old = g_alpha;
    g_alpha = a;
    g_in_modal = true;
    DL->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), col(rgba(4, 4, 6, 0.7f)));
    float w = ImMin(520 * S, W - 32 * S), inner = w - 44 * S;
    float th = para(DISPLAY_M, DISPLAY_M, 20, 0, 0, inner, 0, 0, upper(g_modal.title), 1.2f, false);
    float ph = para(BODY, BODY_M, 14, 0, 0, inner, 0, 0, g_modal.text, 1.5f, false);
    int lines = 0;
    for (char c : g_modal.list) lines += c == '\n';
    float lh = g_modal.list.empty() ? 0 : ImMin(180 * S, (lines + 1) * 11.5f * 1.6f * S + 16 * S);
    float h = 20 * S + th + 12 * S + ph + (lh ? 12 * S + lh : 0) + 20 * S + 32 * S + 20 * S;
    float x = (W - w) / 2, y = (H - h) / 2 + (1 - ease_out(a)) * 8 * S;
    DL->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col(P.panel_solid));
    DL->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col(P.line_strong), 0, S);
    float cy = y + 20 * S, px = x + 22 * S;
    para(DISPLAY_M, DISPLAY_M, 20, px, cy, inner, col(P.text), col(P.text), upper(g_modal.title), 1.2f);
    cy += th + 12 * S;
    para(BODY, BODY_M, 14, px, cy, inner, col(P.muted), col(P.text), g_modal.text);
    cy += ph;
    if (lh) {
        cy += 12 * S;
        DL->AddRectFilled(ImVec2(px, cy), ImVec2(px + inner, cy + lh), col(ImVec4(0, 0, 0, 0.35f)));
        DL->PushClipRect(ImVec2(px, cy), ImVec2(px + inner, cy + lh), true);
        float ly = cy + 8 * S;
        std::istringstream in(g_modal.list);
        std::string ln;
        while (std::getline(in, ln)) { text(MONO, 11.5f, px + 10 * S, ly, col(P.muted), ln, 0, inner - 20 * S); ly += 11.5f * 1.6f * S; }
        DL->PopClipRect();
        cy += lh;
    }
    cy += 20 * S;
    float bx = px + inner;
    bool yes = false, no = false;
    for (int i = 0; i < 2; ++i) {
        std::string label = i == 0 ? (g_modal.yes.empty() ? t("yes") : g_modal.yes) : t("no");
        float bw = text_width(BODY_M, 12.5f, label) + 28 * S;
        bx -= bw;
        ImRect r(ImVec2(bx, cy), ImVec2(bx + bw, cy + 32 * S));
        Hit hb = g_modal.on ? hit(std::string("modal-") + (i ? "no" : "yes"), r) : Hit();
        float hv = approach(i ? "modal-no-h" : "modal-yes-h", hb.hovered ? 1.f : 0.f);
        if (i == 0) DL->AddRectFilled(r.Min, r.Max, col(mix(P.text, ImVec4(1, 1, 1, 1), hv)));
        else {
            DL->AddRectFilled(r.Min, r.Max, col(ImVec4(1, 1, 1, 0.05f * hv)));
            DL->AddRect(r.Min, r.Max, col(mix(P.line_strong, P.muted, hv)), 0, S);
        }
        text(BODY_M, 12.5f, bx + 14 * S, cy + 8 * S, col(i == 0 ? P.void_ : P.text), label);
        if (hb.pressed) (i == 0 ? yes : no) = true;
        bx -= 8 * S;
    }
    g_in_modal = false;
    g_alpha = old;
    ImGui::End();
    if (yes) { g_modal.on = false; if (g_modal.on_yes) g_modal.on_yes(); }
    if (no) g_modal.on = false;
}

// ---------------------------------------------------------------- the page area
void page_area(float x, float y, float W, float H) {
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginChild("##main", ImVec2(W - x, H - y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    DL = ImGui::GetWindowDrawList();
    // Smooth scrolling (the wheel moves a target; the page glides to it).
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !g_block_input && !g_capture.on) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) g_scroll_target -= wheel * 90 * S;
    }
    g_scroll_target = ImClamp(g_scroll_target, 0.f, g_scroll_max);
    g_scroll += (g_scroll_target - g_scroll) * (1 - expf(-g_dt * 16));
    if (fabsf(g_scroll - g_scroll_target) < 0.5f) g_scroll = g_scroll_target;
    float tp = ImClamp((g_time - g_page_t0) / 0.35f, 0.f, 1.f);
    float old = g_alpha;
    g_alpha = ease_out(tp);
    float slide = (1 - ease_out(tp)) * 10 * S;
    float px = x + 40 * S, pw = W - x - 40 * S - 34 * S, py = y - g_scroll + 30 * S + slide;
    float content = 0;
    if (g_page == "home") content = page_home(px, py, pw);
    else if (g_page == "controls") content = page_controls(px, py, pw);
    else if (g_page == "saves") content = page_saves(px, py, pw);
    else if (g_page == "mods") content = page_mods(px, py, pw);
    else if (g_page == "news") content = page_news(px, py, pw);
    else if (g_page == "folder") content = page_folder(px, py, pw);
    else content = page_settings(g_page, px, py, pw);
    g_alpha = old;
    float total = 30 * S + content + 34 * S;
    float view = H - y;
    g_scroll_max = ImMax(0.f, total - view);
    // A thin scroll bar.
    if (g_scroll_max > 0) {
        float bar = view * view / total, by = y + (view - bar) * (g_scroll / g_scroll_max);
        DL->AddRectFilled(ImVec2(W - 4 * S, by), ImVec2(W - 2 * S, by + bar), col(P.line_strong));
    }
    ImGui::EndChild();
}

void frame(float W, float H) {
    blend_palette();
    background(W, H);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::Begin("##app", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
    DL = ImGui::GetWindowDrawList();
    float rail_w = 76 * S;
    rail(H);
    DL = ImGui::GetWindowDrawList();
    top_bar(rail_w, W);
    dock(rail_w, W, H);
    page_area(rail_w, 52 * S, W, H - 88 * S);
    ImGui::End();
    boot_panel(W, H);
    modal_draw(W, H);
    toast_draw(W, H);
}

// ---------------------------------------------------------------- keys
void on_key(const SDL_KeyboardEvent &e) {
    if (g_modal.on) {
        if (e.scancode == SDL_SCANCODE_ESCAPE) g_modal.on = false;
        else if (e.scancode == SDL_SCANCODE_RETURN) { g_modal.on = false; if (g_modal.on_yes) g_modal.on_yes(); }
        return;
    }
    if (e.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) return;
    if (e.scancode >= SDL_SCANCODE_1 && e.scancode <= SDL_SCANCODE_9) go(PAGES[e.scancode - SDL_SCANCODE_1]);
    else if (e.scancode == SDL_SCANCODE_RETURN || e.scancode == SDL_SCANCODE_KP_ENTER) play();
    else if (e.scancode == SDL_SCANCODE_ESCAPE && g_boot_shown) g_boot_shown = false;
}

bool capture_event(const SDL_Event &e) {
    if (!g_capture.on) return false;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat) return true;
        if (e.key.scancode == SDL_SCANCODE_DELETE) set_key("");
        else if (const char *name = SDL_GetScancodeName(e.key.scancode); name && *name) set_key(name);
        return true;
    case SDL_EVENT_KEY_UP: return true;
    case SDL_EVENT_MOUSE_WHEEL: set_key(e.wheel.y > 0 ? "Wheel Up" : "Wheel Down"); return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        bool inside = g_capture_box.Contains(ImVec2(e.button.x, e.button.y));
        if (e.button.button == SDL_BUTTON_LEFT) {
            if (!inside) { g_capture.on = false; return false; }  // a click elsewhere cancels (and does its own thing)
            if (SDL_GetTicks() / 1000.0 - g_capture.armed < 0.25) return true;
            set_key("Mouse Left");
        } else if (e.button.button == SDL_BUTTON_RIGHT) set_key("Mouse Right");
        else if (e.button.button == SDL_BUTTON_MIDDLE) set_key("Mouse Middle");
        else if (e.button.button == SDL_BUTTON_X1) set_key("Mouse X1");
        else if (e.button.button == SDL_BUTTON_X2) set_key("Mouse X2");
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: return true;
    default: return false;
    }
}

std::string read_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

// ================================================================ the app's actions
SDL_Window *app_window() { return g_win; }
std::string game_name() { return ST["game"].str() == "p2" ? "PROTOTYPE 2" : "PROTOTYPE"; }
const Json &V(const char *key) { return ST["values"][key]; }
const Json &G(const char *key) { return ST["perGame"][ST["game"].str()][key]; }

void act(const char *action, Json args, std::function<void(const Json &)> then) {
    if (g_offline) { if (then) then(Json::object()); return; }
    Ipc::request(action, std::move(args), [then](const Json &r, const std::string &err) {
        if (!err.empty()) { toast(err, "bad"); return; }
        if (!r["version"].is_null()) apply_state(r);
        notice(r["notice"]);
        if (then) then(r);
    });
}
void setV(const char *key, Json value) {
    Json j = Json::object();
    j.set("key", key);
    j.set("value", value);
    Json st = ST;  // shown at once; the launcher's answer confirms it
    Json values = st["values"];
    values.set(key, value);
    st.set("values", values);
    ST = st;
    act("set", j);
}
void setG(const char *key, Json value) {
    Json j = Json::object();
    j.set("key", std::string("game.") + key);
    j.set("value", value);
    Json st = ST, per = st["perGame"], game = per[st["game"].str()];
    game.set(key, value);
    per.set(st["game"].str(), game);
    st.set("perGame", per);
    ST = st;
    act("set", j);
}

void go(const std::string &page) {
    std::string p = page;
    bool known = false;
    for (auto name : PAGES) known |= p == name;
    if (!known) p = "home";
    g_capture.on = false;
    if (p == g_page) return;
    g_page = p;
    g_page_t0 = g_time;
    g_scroll = g_scroll_target = 0;
}

void toast(const std::string &text, const std::string &kind) {
    g_toast.text = text;
    g_toast.kind = kind;
    g_toast.t0 = g_time;
}

void ask(const std::string &title, const std::string &text, const std::string &list, const std::string &yes, std::function<void()> on_yes) {
    g_modal.on = true;
    g_modal.title = title;
    g_modal.text = text;
    g_modal.list = list;
    g_modal.yes = yes;
    g_modal.on_yes = std::move(on_yes);
    g_modal.t0 = g_time;
}

void play() {
    if (ST["launch"]["running"].truthy()) { act("stop"); return; }
    if (!ST["ready"].truthy() || !ST["launch"]["busy"].str().empty()) return;
    g_boot_shown = true;
    act("play");
}

void set_key(const std::string &key) {
    static const char *reserved[] = {"Insert", "F7", "F9", "F10", ",", "=", "+"};
    Capture c = g_capture;
    g_capture.on = false;
    for (auto r : reserved)
        if (key == r) { toast(t("reservedKey", {{"key", key}}), "bad"); return; }
    Json j = Json::object();
    j.set("action", c.action);
    j.set("slot", c.slot);
    j.set("key", key);
    act("key", j);
}

void capture_box(ImRect r) { g_capture_box = r; }

// ================================================================ main
int main(int argc, char **argv) {
    std::string state_file, shot, start_page, start_game;
    int frames = 0, size_w = 0, size_h = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--state") state_file = next();
        else if (a == "--shot") shot = next();
        else if (a == "--page") start_page = next();
        else if (a == "--game") start_game = next();
        else if (a == "--frames") frames = atoi(next().c_str());
        else if (a == "--boot") g_boot_shown = true;
        else if (a == "--ask") ask("Восстановить сохранения?", "Сохранения вернутся к копии от 10 ноября, 20:15. Текущие перед этим тоже попадут в копию, так что это можно отменить.", "", "Восстановить", nullptr);
        else if (a == "--click" || a == "--key") {  // --click X,Y,FRAME  --key NAME,FRAME: input for tests
            std::string v = next();
            size_t c = v.rfind(',');
            g_script.push_back({atoi(v.c_str() + c + 1), a == "--key", v.substr(0, c)});
        } else if (a == "--size") sscanf(next().c_str(), "%dx%d", &size_w, &size_h);
    }
    g_offline = !state_file.empty();
    if (g_offline) {
        ST = Json::parse(read_file(state_file.c_str()));
        if (!start_game.empty()) ST.set("game", start_game);
    } else if (!Ipc::start()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "PROTOTYPE", "Start Prototype.exe: this window is its launcher.", nullptr);
        return 2;
    }
    SDL_SetHint(SDL_HINT_APP_NAME, "PROTOTYPE");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "launcher-ui: %s\n", SDL_GetError());
        return 3;
    }
    g_win = SDL_CreateWindow("PROTOTYPE", 1280, 820, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!g_win) { fprintf(stderr, "launcher-ui: %s\n", SDL_GetError()); return 3; }
    // S: pixels per CSS pixel in window coordinates (the display's scale over the pixel density).
    auto ui_scale = [] {
        float ds = SDL_GetWindowDisplayScale(g_win), pd = SDL_GetWindowPixelDensity(g_win);
        return ds > 0 && pd > 0 ? ds / pd : 1.f;
    };
    S = ui_scale();
    {
        SDL_Rect usable;
        float want_w = 1280 * S, want_h = 820 * S;
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(g_win), &usable)) {
            want_w = ImMin(want_w, usable.w * 0.94f);
            want_h = ImMin(want_h, usable.h * 0.92f);
        }
        if (size_w > 0 && size_h > 0) want_w = size_w * S, want_h = size_h * S;
        SDL_SetWindowSize(g_win, (int)want_w, (int)want_h);
        SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_SetWindowMinimumSize(g_win, (int)ImMin(1120 * S, want_w), (int)ImMin(700 * S, want_h));
    }
    g_renderer = SDL_CreateRenderer(g_win, nullptr);
    if (!g_renderer) { fprintf(stderr, "launcher-ui: %s\n", SDL_GetError()); return 3; }
    SDL_SetRenderVSync(g_renderer, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::GetStyle().WindowPadding = ImVec2(0, 0);
    ImGui::GetStyle().WindowBorderSize = 0;
    ImGui::GetStyle().ChildBorderSize = 0;
    ImGui_ImplSDL3_InitForSDLRenderer(g_win, g_renderer);
    ImGui_ImplSDLRenderer3_Init(g_renderer);
    load_fonts();
    palettes();
    make_textures();
    if (!g_offline) {
        Ipc::request("state", Json::object(), [](const Json &r, const std::string &err) {
            if (!err.empty()) return;
            apply_state(r);
            g_last_exit = r["launch"]["exit"].is_null() ? "-" : r["launch"]["exit"].dump();
            if (ST["gameDir"].str().empty()) go("folder");
        });
        // The first state comes before the first frame: the window opens complete.
        Uint64 until = SDL_GetTicks() + 4000;
        while (ST.is_null() && SDL_GetTicks() < until && Ipc::alive()) { Ipc::pump(); SDL_Delay(5); }
    }
    if (!start_page.empty()) { g_page = start_page; g_page_t0 = -10; }
    g_game_mix = ST["game"].str() == "p2" ? 1.f : 0.f;
    bool shown = false, running = true;
    Uint64 last = SDL_GetPerformanceCounter();
    int frame_no = 0;
    while (running) {
        std::vector<Scripted> later;
        for (auto &sc : g_script) {
            if (sc.frame != frame_no || !sc.what.compare(0, 3, "DN:")) continue;
            SDL_Event ev{};
            bool up = !sc.what.compare(0, 3, "UP:");
            float cx = 0, cy = 0;
            if (sc.key) {
                ev.type = SDL_EVENT_KEY_DOWN;
                ev.key.scancode = SDL_GetScancodeFromName(sc.what.c_str());
                ev.key.down = true;
                ev.key.windowID = SDL_GetWindowID(g_win);
                SDL_PushEvent(&ev);
                continue;
            }
            sscanf(sc.what.c_str() + (up ? 3 : 0), "%f,%f", &cx, &cy);
            if (!up) {
                ev.type = SDL_EVENT_MOUSE_MOTION;
                ev.motion.x = cx, ev.motion.y = cy;
                ev.motion.windowID = SDL_GetWindowID(g_win);
                SDL_PushEvent(&ev);
                later.push_back({frame_no + 2, false, "UP:" + sc.what});
            }
            SDL_Event d{};
            d.type = up ? SDL_EVENT_MOUSE_BUTTON_UP : SDL_EVENT_MOUSE_BUTTON_DOWN;
            d.button.button = SDL_BUTTON_LEFT, d.button.down = !up, d.button.x = cx, d.button.y = cy, d.button.clicks = 1;
            d.button.windowID = SDL_GetWindowID(g_win);
            if (up) SDL_PushEvent(&d);
            else later.push_back({frame_no + 1, false, "DN:" + sc.what});
        }
        for (auto &sc : g_script)  // the press one frame after the move (ImGui sees the hover first)
            if (sc.frame == frame_no && !sc.what.compare(0, 3, "DN:")) {
                float cx = 0, cy = 0;
                sscanf(sc.what.c_str() + 3, "%f,%f", &cx, &cy);
                SDL_Event d{};
                d.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                d.button.button = SDL_BUTTON_LEFT, d.button.down = true, d.button.x = cx, d.button.y = cy, d.button.clicks = 1;
                d.button.windowID = SDL_GetWindowID(g_win);
                SDL_PushEvent(&d);
            }
        g_script.insert(g_script.end(), later.begin(), later.end());
        SDL_Event e;
        bool minimised = SDL_GetWindowFlags(g_win) & SDL_WINDOW_MINIMIZED;
        if (minimised && SDL_WaitEventTimeout(&e, 250)) SDL_PushEvent(&e);
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(g_win))) running = false;
            if (e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) S = ui_scale();
            if (capture_event(e)) continue;
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) on_key(e.key);
        }
        if (!g_offline) {
            Ipc::pump();
            if (!Ipc::alive()) break;  // the launcher is gone
            poll();
        }
        // While the game runs the window steps aside (minimised), and comes back when it ends.
        const Json &l = ST["launch"];
        bool minimise = V("minimize_on_play").is_null() || V("minimize_on_play").truthy();
        if (l["running"].truthy() && minimise) {
            if (!g_minimised_for_game && time(nullptr) - (long long)l["since"].num() >= 6) {
                SDL_MinimizeWindow(g_win);
                g_minimised_for_game = true;
            }
        } else if (g_minimised_for_game && !l["running"].truthy()) {
            SDL_RestoreWindow(g_win);
            SDL_RaiseWindow(g_win);
            g_minimised_for_game = false;
        }
        Uint64 now = SDL_GetPerformanceCounter();
        g_dt = ImClamp((float)(now - last) / SDL_GetPerformanceFrequency(), 1 / 500.f, 1 / 15.f);
        last = now;
        if (!shot.empty()) g_dt = 1 / 60.f;
        g_time += g_dt;
        if (minimised && shot.empty()) continue;
        float target_mix = ST["game"].str() == "p2" ? 1.f : 0.f;
        g_game_mix += (target_mix - g_game_mix) * (1 - expf(-g_dt * 9));
        wall_tick();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        frame(io.DisplaySize.x, io.DisplaySize.y);
        ImGui::Render();
        SDL_SetRenderDrawColor(g_renderer, 8, 8, 10, 255);
        SDL_RenderClear(g_renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
        ++frame_no;
        if (!shot.empty() && frame_no >= ImMax(frames, 2)) {
            if (SDL_Surface *s = SDL_RenderReadPixels(g_renderer, nullptr)) {
                SDL_SaveBMP(s, shot.c_str());
                SDL_DestroySurface(s);
            }
            running = false;
        }
        SDL_RenderPresent(g_renderer);
        if (!shown) { SDL_ShowWindow(g_win); shown = true; }
    }
    free_pictures();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
