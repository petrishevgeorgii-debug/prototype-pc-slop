// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE launcher, the native window (SDL3 + Dear ImGui): what the parts share.
//
// The window draws everything itself with ImGui's draw lists (no stock widgets): the layout and
// the look of the web launcher (launcher/web), with its animations. All data and every action
// go through the launcher process (launcher/prototype_native.py) as JSON lines on stdin/stdout.
#pragma once
#include <SDL3/SDL.h>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "json.h"

// ---------------------------------------------------------------- the launcher process
namespace Ipc {
bool start();                                   // false when stdin/stdout are not a launcher's pipes
void request(const char *action, Json args = Json::object(), std::function<void(const Json &result, const std::string &error)> done = nullptr);
void pump();                                    // runs the replies' callbacks (main thread)
bool alive();
}

// ---------------------------------------------------------------- look
enum Font { DISPLAY, DISPLAY_M, DISPLAY_B, BODY, BODY_M, MONO, FONT_COUNT };
struct Palette {
    ImVec4 void_, panel, panel_solid, line, line_strong, text, muted, dim, accent, accent_hi, accent_ink;
};
extern Palette P;                // the current colours (between PROTOTYPE's and PROTOTYPE 2's)
extern const ImVec4 OK, WARN;
extern float S;                  // pixels per CSS pixel (the display's scale)
extern float g_alpha;            // the alpha every colour is drawn with (page transitions)
extern float g_time, g_dt;
extern ImDrawList *DL;

ImU32 col(const ImVec4 &c, float a = 1.0f);
ImVec4 rgba(int r, int g, int b, float a = 1.0f);
ImVec4 mix(const ImVec4 &a, const ImVec4 &b, float t);
float ease_out(float t);
float ease_in_out(float t);
float approach(ImGuiID id, float target, float speed = 14.0f);  // a value easing towards target
float approach(const char *id, float target, float speed = 14.0f);

bool load_fonts();
ImFont *font(Font f);
float text_width(Font f, float px, const std::string &text, float track = 0);
// Draws text at (x, y): y is the top of a line of height px * 1.2. track: letter spacing in em.
// max_w > 0 cuts the text with an ellipsis. Returns the width drawn.
float text(Font f, float px, float x, float y, ImU32 color, const std::string &s, float track = 0, float max_w = 0);
// Wrapped text with <b>...</b> spans (bold in `bold` font and `bold_color`); returns the height.
float para(Font f, Font bold, float px, float x, float y, float width, ImU32 color, ImU32 bold_color, const std::string &s,
           float line = 1.5f, bool draw = true);
std::string upper(const std::string &s);

// ---------------------------------------------------------------- pictures
struct Picture {
    SDL_Texture *color = nullptr, *gray = nullptr;
    int w = 0, h = 0;
};
Picture *picture(const std::string &path);      // loaded once (BMP), nullptr if missing
void draw_picture(Picture *p, ImRect box, float gray, float brightness, float alpha, float zoom = 1, ImVec2 shift = {0, 0},
                  float focus_y = 0.45f);
void quad_h_gradient(ImTextureID tex, ImRect r, ImVec2 uv0, ImVec2 uv1, const float *stops, const float *alphas, int n, ImU32 tint);
void free_pictures();
extern SDL_Renderer *g_renderer;

// ---------------------------------------------------------------- input
struct Hit {
    bool hovered = false, held = false, pressed = false, right = false;
};
Hit hit(const std::string &id, ImRect r, bool enabled = true);
extern bool g_block_input;       // a modal is open: only it takes clicks

// ---------------------------------------------------------------- texts
const Json &tj(const char *key);
std::string t(const char *key, std::initializer_list<std::pair<const char *, std::string>> vars = {});
std::string plural(int n, const char *forms_key);
std::string count(int n, const char *forms_key);
bool ru();
std::string fmt_date(long long sec);
std::string fmt_time(long long sec);

// ---------------------------------------------------------------- app state and actions
extern Json ST;                  // the launcher's state
extern std::string g_page;
void go(const std::string &page);
void act(const char *action, Json args = Json::object(), std::function<void(const Json &)> then = nullptr);
void toast(const std::string &text, const std::string &kind = "");
void ask(const std::string &title, const std::string &text, const std::string &list, const std::string &yes,
         std::function<void()> on_yes);
void play();
std::string game_name();         // "PROTOTYPE" or "PROTOTYPE 2"
const Json &V(const char *key);  // ST.values[key]
const Json &G(const char *key);  // ST.perGame[game][key]
void setV(const char *key, Json value);
void setG(const char *key, Json value);

// key capture (Controls)
struct Capture {
    bool on = false;
    std::string action;
    int slot = 0;
    double armed = 0;
};
extern Capture g_capture;
void set_key(const std::string &key);
void capture_box(ImRect r);         // where the key being changed is (a click there is the left button)

// ---------------------------------------------------------------- parts (pages.cpp)
float page_home(float x, float y, float w);
float page_settings(const std::string &page, float x, float y, float w);
float page_controls(float x, float y, float w);
float page_saves(float x, float y, float w);
float page_mods(float x, float y, float w);
float page_news(float x, float y, float w);
float page_folder(float x, float y, float w);
void wall_tick();
void wall_reset();
void hero_glitch();
