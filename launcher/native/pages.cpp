// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE launcher, the native window: the pages (home with the camera wall, settings, keys,
// saves, mods, news, the game folder).
#include "app.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>

SDL_Window *app_window();

namespace {
// ---------------------------------------------------------------- small parts
const float GAP = 40;

std::string lower(const std::string &s) {
    std::string out;
    const char *p = s.c_str(), *end = p + s.size();
    while (p < end) {
        unsigned c;
        int n = ImTextCharFromUtf8(&c, p, end);
        p += n ? n : 1;
        if (c >= 'A' && c <= 'Z') c += 32;
        else if (c >= 0x410 && c <= 0x42f) c += 0x20;
        else if (c >= 0x400 && c <= 0x40f) c += 0x50;
        char buf[5];
        out.append(buf, ImTextCharToUtf8(buf, c));
    }
    return out;
}
std::string capital(const std::string &s) {
    if (s.empty()) return s;
    unsigned c;
    int n = ImTextCharFromUtf8(&c, s.c_str(), s.c_str() + s.size());
    return upper(s.substr(0, n)) + s.substr(n);
}
std::string pad2(int v) { char b[8]; snprintf(b, sizeof b, "%02d", v); return b; }

void hline(float x0, float x1, float y, ImU32 c) { DL->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + ImMax(1.f, S)), c); }

float title_block(float x, float y, float w, const std::string &title, const std::string &lede) {
    float cy = y + 8 * S;
    text(DISPLAY_M, 30, x, cy, col(P.text), upper(title), 0.06f);
    cy += 30 * 1.2f * S + 6 * S;
    if (!lede.empty()) cy += para(BODY, BODY_M, 14, x, cy, ImMin(w, 620 * S), col(P.muted), col(P.text), lede) + 26 * S;
    else cy += 20 * S;
    return cy - y;
}

float group_head(float x, float y, float w, const std::string &label, const std::string &tag = "") {
    float tw = text(MONO, 10.5f, x, y, col(P.muted), upper(label), 0.16f);
    if (!tag.empty()) text(MONO, 9.5f, x + tw + 10 * S, y + 1 * S, col(P.dim), upper(tag), 0.08f);
    float h = 10.5f * 1.2f * S + 8 * S;
    hline(x, x + w, y + h, col(P.line));
    return h + S + 4 * S;
}

float note(float x, float y, float w, const std::string &markup) {
    float h = para(BODY, BODY_M, 12.5f, x + 16 * S, y + 16 * S + 6 * S, ImMin(w, 600 * S) - 16 * S, col(P.muted), col(P.text), markup, 1.5f);
    DL->AddRectFilled(ImVec2(x, y + 16 * S), ImVec2(x + S, y + 16 * S + h + 12 * S), col(P.line_strong));
    return 16 * S + h + 12 * S;
}

// A button: returns true when pressed; *width gets its width.
bool button(const std::string &id, float x, float y, const std::string &label, bool primary = false, bool enabled = true, float *width = nullptr) {
    float w = text_width(BODY_M, 12.5f, label) + 28 * S, h = 32 * S;
    if (width) *width = w;
    ImRect r(ImVec2(x, y), ImVec2(x + w, y + h));
    Hit hb = hit(id, r, enabled);
    float hv = approach(("btn-h-" + id).c_str(), hb.hovered ? 1.f : 0.f, 16);
    float a = enabled ? 1 : 0.45f;
    if (primary) DL->AddRectFilled(r.Min, r.Max, col(mix(P.text, ImVec4(1, 1, 1, 1), hv), a));
    else {
        DL->AddRectFilled(r.Min, r.Max, col(ImVec4(1, 1, 1, 0.05f * hv)));
        DL->AddRect(r.Min, r.Max, col(mix(P.line_strong, P.muted, hv), a), 0, S);
    }
    text(BODY_M, 12.5f, x + 14 * S, y + (h - 12.5f * 1.2f * S) / 2, col(primary ? P.void_ : P.text, a), label);
    return hb.pressed;
}

// ---------------------------------------------------------------- settings rows
struct Row {
    std::string id, label, desc, tag;
    enum Kind { TOGGLE, OPTIONS, RANGE } kind = TOGGLE;
    std::vector<std::pair<Json, std::string>> options;
    int lo = 0, hi = 100;
    Json value;
    std::function<void(Json)> set;
    bool locked = false;
};

bool same(const Json &a, const Json &b) {
    if (a.kind == Json::String || b.kind == Json::String) return a.str() == b.str() && a.kind == b.kind;
    if (a.kind == Json::Bool && b.kind == Json::Bool) return a.b == b.b;
    return a.num(-12345) == b.num(-54321) || (a.kind == Json::Bool && b.kind == Json::Number && a.num() == b.num()) ||
           (a.kind == Json::Number && b.kind == Json::Bool && a.num() == b.num());
}

void toggle(const std::string &id, float x, float y, bool on, bool enabled, const std::function<void(Json)> &set) {
    ImRect r(ImVec2(x, y), ImVec2(x + 38 * S, y + 20 * S));
    Hit h = hit(id, r, enabled);
    float k = approach(("tg-" + id).c_str(), on ? 1.f : 0.f, 18);
    float hv = approach(("tg-h-" + id).c_str(), h.hovered ? 1.f : 0.f, 16);
    DL->AddRectFilled(r.Min, r.Max, col(mix(ImVec4(0, 0, 0, 0.3f), P.accent, k)), 10 * S);
    DL->AddRect(r.Min, r.Max, col(mix(mix(P.line_strong, P.muted, hv * (1 - k)), P.accent, k)), 10 * S, S);
    float kx = x + 3 * S + 6 * S + k * 18 * S;
    DL->AddCircleFilled(ImVec2(kx, y + 10 * S), 6 * S, col(mix(P.muted, ImVec4(1, 1, 1, 1), k)), 20);
    if (h.pressed && set) set(Json(!on));
}

void stepper(const Row &row, float x, float y, bool enabled) {
    int n = (int)row.options.size(), i = 0;
    for (int k = 0; k < n; ++k)
        if (same(row.options[k].first, row.value)) i = k;
    float h = 30 * S, bw = 28 * S, ow = 120 * S;
    for (auto &o : row.options) ow = ImMax(ow, text_width(BODY_M, 13, o.second) + 16 * S);
    ImRect r(ImVec2(x, y), ImVec2(x + bw * 2 + ow, y + h));
    DL->AddRect(r.Min, r.Max, col(P.line_strong), 0, S);
    // The value slides in from the side it came from.
    static std::map<std::string, std::pair<int, float>> last;  // id: index, when it changed
    static std::map<std::string, int> dir;
    auto &l = last[row.id];
    if (l.second == 0 && l.first == 0) l = {i, -10};
    if (l.first != i) { dir[row.id] = (i > l.first) == !(l.first == n - 1 && i == 0) ? 1 : -1; if (l.first == 0 && i == n - 1) dir[row.id] = -1; l = {i, g_time}; }
    float t = ease_out((g_time - l.second) / 0.22f);
    for (int side = 0; side < 2; ++side) {
        ImRect b = side ? ImRect(ImVec2(r.Max.x - bw, y), r.Max) : ImRect(r.Min, ImVec2(x + bw, y + h));
        Hit hb = hit(row.id + (side ? "+" : "-"), b, enabled);
        float hv = approach((row.id + (side ? "+h" : "-h")).c_str(), hb.hovered ? 1.f : 0.f, 16);
        DL->AddRectFilled(b.Min, b.Max, col(ImVec4(1, 1, 1, 0.04f * hv)));
        ImVec2 c = b.GetCenter();
        ImU32 ic = col(mix(P.dim, P.text, hv));
        float s = 4 * S;
        if (side) DL->AddTriangleFilled(ImVec2(c.x - s * 0.7f, c.y - s), ImVec2(c.x + s * 0.8f, c.y), ImVec2(c.x - s * 0.7f, c.y + s), ic);
        else DL->AddTriangleFilled(ImVec2(c.x + s * 0.7f, c.y - s), ImVec2(c.x - s * 0.8f, c.y), ImVec2(c.x + s * 0.7f, c.y + s), ic);
        if (hb.pressed && row.set) row.set(row.options[((i + (side ? 1 : -1)) % n + n) % n].first);
    }
    const std::string &label = row.options.empty() ? std::string() : row.options[i].second;
    float lw = text_width(BODY_M, 13, label);
    float ox = x + bw + (ow - lw) / 2 + (1 - t) * 10 * S * dir[row.id];
    DL->PushClipRect(ImVec2(x + bw, y), ImVec2(x + bw + ow, y + h), true);
    float old = g_alpha;
    g_alpha *= 0.2f + 0.8f * t;
    text(BODY_M, 13, ox, y + (h - 13 * 1.2f * S) / 2, col(P.text), label);
    g_alpha = old;
    DL->PopClipRect();
}

void slider(const Row &row, float x, float y, bool enabled) {
    static std::string dragging;
    static float drag_value = 0;
    float w = 150 * S, h = 16 * S;
    ImRect r(ImVec2(x, y), ImVec2(x + w, y + h));
    Hit hb = hit(row.id + "-slider", r, enabled);
    float v = (float)row.value.num(row.lo);
    if (hb.held) {
        float f = ImClamp((ImGui::GetIO().MousePos.x - x) / w, 0.f, 1.f);
        drag_value = std::round(row.lo + f * (row.hi - row.lo));
        dragging = row.id;
    } else if (dragging == row.id) {
        dragging.clear();
        if ((int)drag_value != (int)v && row.set) row.set(Json((int)drag_value));
        v = drag_value;
    }
    if (dragging == row.id) v = drag_value;
    float f = (v - row.lo) / (float)(row.hi - row.lo);
    float shown = approach((row.id + "-sv").c_str(), f, 22);
    float cy = y + h / 2;
    DL->AddRectFilled(ImVec2(x, cy - 1.5f * S), ImVec2(x + w, cy + 1.5f * S), col(P.line_strong), 2 * S);
    DL->AddRectFilled(ImVec2(x, cy - 1.5f * S), ImVec2(x + w * shown, cy + 1.5f * S), col(P.accent_hi), 2 * S);
    float hv = approach((row.id + "-sh").c_str(), hb.hovered || hb.held ? 1.f : 0.f, 16);
    DL->AddCircleFilled(ImVec2(x + w * shown, cy), (6 + 1.5f * hv) * S, col(P.accent_hi), 20);
    char buf[16];
    snprintf(buf, sizeof buf, "%d", (int)v);
    float tw = text_width(MONO, 12, buf);
    text(MONO, 12, x + w + 12 * S + 30 * S - tw, y + (h - 12 * 1.2f * S) / 2, col(P.muted), buf);
}

float control_width(const Row &row) {
    if (row.kind == Row::TOGGLE) return 38 * S;
    if (row.kind == Row::RANGE) return 150 * S + 12 * S + 30 * S;
    float ow = 120 * S;
    for (auto &o : row.options) ow = ImMax(ow, text_width(BODY_M, 13, o.second) + 16 * S);
    return 56 * S + ow;
}

float row_draw(float x, float y, float w, const Row &row) {
    float old = g_alpha;
    if (row.locked) g_alpha *= 0.4f;
    float cw = control_width(row), ch = row.kind == Row::OPTIONS ? 30 * S : row.kind == Row::TOGGLE ? 20 * S : 16 * S;
    float top = y + 13 * S, line_h = ImMax(14 * 1.5f * S, ch);
    float lw = text(BODY_M, 14, x + 2 * S, top + (line_h - 14 * 1.2f * S) / 2, col(P.text), row.label, 0, w - cw - 24 * S);
    if (!row.tag.empty()) text(MONO, 9.5f, x + 2 * S + lw + 10 * S, top + (line_h - 9.5f * 1.2f * S) / 2 + 1 * S, col(WARN), upper(row.tag), 0.08f);
    float cx = x + w - 2 * S - cw, cy = top + (line_h - ch) / 2;
    bool enabled = !row.locked;
    if (row.kind == Row::TOGGLE) toggle(row.id, cx, cy, row.value.truthy(), enabled, row.set);
    else if (row.kind == Row::OPTIONS) stepper(row, cx, cy, enabled);
    else slider(row, cx, cy, enabled);
    float h = 13 * S + line_h;
    if (!row.desc.empty()) h += 4 * S + para(BODY, BODY_M, 12.5f, x + 2 * S, y + h + 4 * S, ImMin(w - 4 * S, 560 * S), col(P.muted), col(P.text), row.desc, 1.45f);
    h += 13 * S;
    g_alpha = old;
    hline(x, x + w, y + h - S, col(P.line));
    return h;
}

Row setting(const char *key, Json value, std::function<void(Json)> set) {
    Row r;
    r.id = key;
    std::string path = std::string("rows.") + key;
    r.label = tj(path.c_str())[0].str();
    r.desc = tj(path.c_str())[1].str();
    r.value = value;
    r.set = std::move(set);
    return r;
}
Row toggle_row(const char *key, Json value, std::function<void(Json)> set) { return setting(key, value, std::move(set)); }
Row options_row(const char *key, Json value, std::vector<std::pair<Json, std::string>> options, std::function<void(Json)> set) {
    Row r = setting(key, value, std::move(set));
    r.kind = Row::OPTIONS;
    r.options = std::move(options);
    return r;
}
Row range_row(const char *key, Json value, int lo, int hi, std::function<void(Json)> set) {
    Row r = setting(key, value, std::move(set));
    r.kind = Row::RANGE;
    r.lo = lo;
    r.hi = hi;
    return r;
}
std::function<void(Json)> sv(const char *key) { return [key](Json v) { setV(key, v); }; }
std::function<void(Json)> sg(const char *key) { return [key](Json v) { setG(key, v); }; }

float group(float x, float y, float w, const std::string &head, const std::vector<Row> &rows, const std::string &tail_note = "") {
    float cy = y + group_head(x, y, w, head);
    for (auto &r : rows) cy += row_draw(x, cy, w, r);
    if (!tail_note.empty()) cy += note(x, cy, w, tail_note);
    return cy - y + 30 * S;
}

std::string opt(const char *k) { return t((std::string("opt.") + k).c_str()); }

// ---------------------------------------------------------------- the camera wall
struct Monitor { int feed = 0, slot = 0; float cut = -10; std::string id; };
Monitor g_mons[5];
int g_feed_idx = 0;
bool g_wall_dirty = true;
float g_next_cut = 0;
float g_glitch_t0 = -10;
SDL_Texture *t_vignette = nullptr, *t_static = nullptr;

std::vector<std::string> wall_ids() {
    std::vector<std::string> ids;
    const Json &list = ST["art"][ST["game"].str() == "p2" ? "p2" : "p1"];
    for (size_t i = 0; i < list.size(); ++i) ids.push_back(list[i].str());
    return ids;
}

void set_feed(Monitor &m, int i) {
    auto ids = wall_ids();
    if (ids.empty()) return;
    m.feed = g_feed_idx;
    m.id = ids[g_feed_idx++ % ids.size()];
    m.slot = i;
    m.cut = g_time;
}

void textures() {
    if (t_vignette) return;
    const int N = 128;
    std::vector<Uint32> px(N * N), st(96 * 64);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float nx = (x + 0.5f) / N * 2 - 1, ny = (y + 0.5f) / N * 2 - 1, d = sqrtf(nx * nx + ny * ny) / sqrtf(2.f);
            float a = d < 0.55f ? 0 : 0.6f * (d - 0.55f) / 0.45f;
            px[y * N + x] = (Uint32)(ImClamp(a, 0.f, 1.f) * 255) << 24;
        }
    t_vignette = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, N, N);
    SDL_UpdateTexture(t_vignette, nullptr, px.data(), N * 4);
    SDL_SetTextureBlendMode(t_vignette, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t_vignette, SDL_SCALEMODE_LINEAR);
    t_static = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 96, 64);
    SDL_SetTextureBlendMode(t_static, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t_static, SDL_SCALEMODE_NEAREST);
}

void refresh_static() {
    static float last = -1;
    if (g_time - last < 0.05f) return;
    last = g_time;
    void *pixels;
    int pitch;
    if (!SDL_LockTexture(t_static, nullptr, &pixels, &pitch)) return;
    static Uint32 seed = 777;
    for (int y = 0; y < 64; ++y) {
        Uint32 *row = (Uint32 *)((Uint8 *)pixels + y * pitch);
        Uint32 band = (seed >> 7) & 31;
        for (int x = 0; x < 96; ++x) {
            seed = seed * 1103515245u + 12345u;
            Uint32 v = (seed >> 16) & 255;
            if (band < 3) v = v / 2 + 110;
            row[x] = 0xff000000u | v << 16 | v << 8 | v;
        }
    }
    SDL_UnlockTexture(t_static);
}

void monitor(Monitor &m, ImRect r, bool big, int index) {
    DL->AddRectFilled(r.Min, r.Max, col(ImVec4(0, 0, 0, 1)));
    auto ids = wall_ids();
    DL->PushClipRect(r.Min, r.Max, true);
    if (ids.empty() || m.id.empty()) {
        DL->AddImage((ImTextureID)(intptr_t)t_static, r.Min, r.Max, ImVec2(0, 0), ImVec2(1, 1), col(ImVec4(1, 1, 1, 0.07f)));
        std::string s = ST["art"].is_null() ? "…" : t("noSignal");
        float w = text_width(MONO, 11, s, 0.2f);
        text(MONO, 11, r.GetCenter().x - w / 2, r.GetCenter().y - 7 * S, col(P.dim), s, 0.2f);
        DL->PopClipRect();
        return;
    }
    bool p2 = ST["game"].str() == "p2";
    std::string path = ST["artDir"].str() + "/" + (p2 ? "p2/" : "p1/") + m.id + ".bmp";
    Picture *pic = picture(path);
    // Slow drift (16 s, every other one 20 s the other way), and the cut when the feed changes.
    float dur = index % 2 ? 20.f : 16.f, ph = fmodf(g_time / dur + index * 0.37f, 2.f), e = ease_in_out(ph < 1 ? ph : 2 - ph);
    if (index % 2) e = 1 - e;
    float zoom = 1.03f + 0.05f * e;
    ImVec2 shift((-0.01f + 0.02f * e), (0.004f - 0.008f * e));
    float ct = (g_time - m.cut) / 0.3f, alpha = 1;
    if (ct >= 0 && ct < 1) {
        if (ct < 0.5f) { alpha = 0.2f; zoom += 0.01f; shift.x -= 4 * S / r.GetWidth(); }
        else alpha = 0.65f;
    }
    draw_picture(pic, r, p2 ? 0.4f : 0.85f, p2 ? 0.95f : 0.9f, alpha, zoom, shift, 0.5f);
    // Static and a torn band right after a cut.
    float st = (g_time - m.cut) / 0.45f;
    if (st >= 0 && st < 1) {
        DL->AddImage((ImTextureID)(intptr_t)t_static, r.Min, r.Max, ImVec2(0, 0), ImVec2(1, 1), col(ImVec4(1, 1, 1, 0.35f * (1 - st))));
        float by = r.Min.y + r.GetHeight() * fmodf(g_time * 3.7f + index * 0.31f, 1.f);
        DL->AddRectFilled(ImVec2(r.Min.x, by), ImVec2(r.Max.x, by + 6 * S), col(ImVec4(1, 1, 1, 0.12f * (1 - st))));
    }
    // A slow rolling bar, like an old surveillance monitor.
    float roll = fmodf(g_time * 0.12f + index * 0.21f, 1.4f) - 0.2f;
    float ry = r.Min.y + r.GetHeight() * roll;
    DL->AddRectFilledMultiColor(ImVec2(r.Min.x, ry - 18 * S), ImVec2(r.Max.x, ry), col(ImVec4(1, 1, 1, 0)), col(ImVec4(1, 1, 1, 0)),
                                col(ImVec4(1, 1, 1, 0.035f)), col(ImVec4(1, 1, 1, 0.035f)));
    // Scan lines and the vignette.
    ImU32 scan = col(ImVec4(0, 0, 0, 0.28f));
    float lh = ImMax(1.f, floorf(S + 0.25f));
    for (float y = r.Min.y; y < r.Max.y; y += 3 * lh) DL->AddRectFilled(ImVec2(r.Min.x, y), ImVec2(r.Max.x, y + lh), scan);
    DL->AddImage((ImTextureID)(intptr_t)t_vignette, r.Min, r.Max, ImVec2(0, 0), ImVec2(1, 1), col(ImVec4(1, 1, 1, 1)));
    // The camera's caption.
    ImVec4 hud = rgba(240, 236, 230, 0.78f);
    float px = r.Min.x + 10 * S, pw = r.GetWidth() - 20 * S, ty = r.Min.y + 8 * S, by = r.Max.y - 8 * S - 12 * 1.2f * S;
    std::string cam = t("camera") + " " + pad2(3 + ((m.feed + 1) * 7) % 40);
    std::string loc;
    if (!p2) {
        const Json &districts = ST["art"]["districts"];
        if (districts.size()) cam += " · " + upper(districts[(size_t)((m.feed + 1) * 3 + m.slot) % districts.size()].str());
        loc = upper(ST["art"]["missions"][m.id].str());
    } else loc = tj("zones")[(m.feed + 1 + m.slot) % 3].str();
    const Json &threats = tj("threats");
    std::string threat = big && threats.size() ? threats[(size_t)(m.feed + 1 + m.slot) % threats.size()].str() : "";
    float thw = threat.empty() ? 0 : text_width(MONO, 9.5f, threat, 0.08f);
    if (!threat.empty()) {
        bool blink = fmodf(g_time, 1.2f) < 0.85f;
        text(MONO, 9.5f, r.Max.x - 10 * S - thw, ty, col(mix(P.accent_hi, ImVec4(1, 1, 1, 1), 0.2f), blink ? 1.f : 0.35f), threat, 0.08f);
    }
    text(MONO, 9.5f, px, ty, col(hud), cam, 0.08f, pw - thw - 8 * S);
    time_t now = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    int ff = ((int)(SDL_GetTicks() % 1000) / 40 + index) % 25;
    std::string tc = pad2(tmv.tm_min) + ":" + pad2(tmv.tm_sec) + ":" + pad2(ff);
    float tcw = text_width(MONO, 9.5f, tc, 0.08f);
    text(MONO, 9.5f, r.Max.x - 10 * S - tcw, by + 2 * S, col(hud), tc, 0.08f);
    text(DISPLAY, 12, px, by, col(rgba(242, 238, 233)), loc, 0.1f, pw - tcw - 10 * S);
    DL->PopClipRect();
}

void hero(float x, float y, float size, const std::string &first, const std::string &last) {
    float lh = size * 0.92f * S;
    float age = g_time - g_glitch_t0;
    auto draw = [&](float dx, float dy, ImU32 c1, ImU32 c2) {
        text(DISPLAY_B, size, x + dx, y + dy - size * 0.1f * S, c1, first, 0.02f);
        text(DISPLAY_B, size, x + dx, y + dy + lh - size * 0.1f * S, c2, last, 0.02f);
    };
    float total_h = lh * 2;
    if (age < 0 || age > 0.45f) { draw(0, 0, col(P.text), col(P.accent_hi)); return; }
    int step = (int)(age / 0.15f);
    float w = 2000 * S;
    // RGB split, fading.
    float k = 1 - age / 0.45f;
    draw(-4 * S * k, 0, col(ImVec4(1, 0.1f, 0.15f, 0.5f * k)), col(ImVec4(1, 0.1f, 0.15f, 0.5f * k)));
    draw(4 * S * k, 0, col(ImVec4(0.1f, 0.9f, 1, 0.4f * k)), col(ImVec4(0.1f, 0.9f, 1, 0.4f * k)));
    if (step == 0) {  // the top part, pushed left
        DL->PushClipRect(ImVec2(x - 10 * S, y), ImVec2(x + w, y + total_h * 0.45f), true);
        float old = g_alpha; g_alpha *= 0.6f;
        draw(-4 * S, 0, col(P.text), col(P.accent_hi));
        g_alpha = old;
        DL->PopClipRect();
    } else if (step == 1) {  // the bottom part, pushed right, in torn slices
        for (int i = 0; i < 4; ++i) {
            float a0 = 0.45f + i * 0.1375f, a1 = a0 + 0.1375f;
            float off = (((int)(g_time * 60) * 7 + i * 13) % 9 - 4) * S;
            DL->PushClipRect(ImVec2(x - 10 * S, y + total_h * a0), ImVec2(x + w, y + total_h * a1), true);
            draw(3 * S + off, 0, col(P.text), col(P.accent_hi));
            DL->PopClipRect();
        }
    } else draw(0, 0, col(P.text), col(P.accent_hi));
}

std::string folder_pick;
std::mutex folder_lock;
void SDLCALL folder_chosen(void *, const char *const *files, int) {
    if (files && files[0]) {
        std::lock_guard<std::mutex> g(folder_lock);
        folder_pick = files[0];
    }
}
}  // namespace

// ================================================================ the wall's clock
void wall_reset() { g_wall_dirty = true; }
void hero_glitch() { g_glitch_t0 = g_time; }
void wall_tick() {
    textures();
    refresh_static();
    if (g_wall_dirty && !ST.is_null()) {
        g_feed_idx = 0;
        for (int i = 0; i < 5; ++i) { g_mons[i] = Monitor(); set_feed(g_mons[i], i); g_mons[i].cut = g_time + i * 0.08f; }
        g_wall_dirty = ST["art"].is_null();
        g_next_cut = g_time + 2.6f;
    }
    if (g_time > g_next_cut) {
        g_next_cut = g_time + 2.6f;
        if (!wall_ids().empty()) {
            int i = rand() % 5;
            set_feed(g_mons[i], i);
        }
    }
    std::string pick;
    {
        std::lock_guard<std::mutex> g(folder_lock);
        pick.swap(folder_pick);
    }
    if (!pick.empty()) {
        Json j = Json::object();
        j.set("path", pick);
        act("set_folder", j, [](const Json &) { wall_reset(); });
    }
}

// ================================================================ home
float page_home(float x, float y, float w) {
    float cw = (w - 44 * S) / 2, cy = y;
    // The file.
    bool blink = fmodf(g_time, 1.6f) < 1.1f;
    DL->AddCircleFilled(ImVec2(x + 3 * S, cy + 6.5f * S), 3 * S, col(P.accent_hi, blink ? 1.f : 0.25f));
    float ew = text(MONO, 10.5f, x + 13 * S, cy, col(P.accent_hi), t("rec"), 0.14f);
    text(MONO, 10.5f, x + 13 * S + ew + 12 * S, cy, col(P.muted), t((ST["game"].str() + "Cam").c_str()), 0.14f, cw - ew - 30 * S);
    cy += 10.5f * 1.2f * S + 2 * S;
    const Json &h = tj("heroes")[ST["game"].str()];
    float W = ImGui::GetIO().DisplaySize.x / S;
    float size = ImClamp(W * 0.056f, 44.f, 84.f);
    while (size > 44 && ImMax(text_width(DISPLAY_B, size, h[0].str(), 0.02f), text_width(DISPLAY_B, size, h[1].str(), 0.02f)) > cw) size -= 2;
    hero(x, cy, size, h[0].str(), h[1].str());
    cy += size * 0.92f * S * 2 + 22 * S;
    // KEY value  KEY value ...
    float fx = x, fy = cy;
    for (size_t i = 0; i < h[2].size(); ++i) {
        std::string k = h[2][i][0].str(), v = h[2][i][1].str();
        float iw = text_width(MONO, 11, k, 0.06f) + 6 * S + text_width(MONO, 11, v, 0.06f);
        if (fx > x && fx + iw > x + cw) { fx = x; fy += 11 * 1.5f * S + 6 * S; }
        float kx = fx + text(MONO, 11, fx, fy, col(P.dim), k, 0.06f) + 6 * S;
        text(MONO, 11, kx, fy, col(P.text), v, 0.06f);
        fx += iw + 26 * S;
    }
    cy = fy + 11 * 1.5f * S + 22 * S;
    // Status.
    std::string exe = ST["game"].str() == "p1" ? "eboot.bin" : "prototype2_ps4f.elf";
    const Json *bad = nullptr;
    for (size_t i = 0; i < ST["checks"].size(); ++i)
        if (ST["checks"][i][1].str() == "bad") { bad = &ST["checks"][i]; break; }
    std::string prompts = V("prompts").str();
    bool xbox_pad = ST["pad"]["kind"].str() == "xbox";
    std::string icons = ST["game"].str() == "p2" ? t("iconsPS") + " — " + t("p2IconsNote")
                        : prompts == "xbox" ? t("iconsXbox") : prompts == "gamepad" ? t("iconsPS") : prompts == "keyboard" ? t("iconsKeys")
                        : !ST["pad"].is_null() ? (xbox_pad ? t("iconsXbox") : t("iconsPS")) + " — " + t("padConnected") : t("iconsAuto");
    struct St { ImVec4 c; std::string a, b; };
    std::vector<St> st = {
        {bad ? P.accent_hi : OK, t("stFiles"), ST["gameDir"].str().empty() ? t("notSelected") : bad ? t("filesBad", {{"what", (*bad)[0].str()}, {"detail", (*bad)[2].str()}}) : t("filesOk", {{"exe", exe}})},
        {OK, t("stPrep"), ST["prepared"][ST["game"].str()].truthy() ? t("prepDone") : t("prepLater")},
        {ST["game"].str() == "p1" ? OK : WARN, t("stSupport"), ST["game"].str() == "p1" ? t("supportFull") : t("supportExp")},
        {OK, t("stIcons"), icons}};
    float sw = ImMin(cw, 540 * S);
    hline(x, x + sw, cy, col(P.line));
    for (auto &s : st) {
        float rh = 9 * S * 2 + 13 * 1.5f * S;
        DL->AddCircleFilled(ImVec2(x + 3 * S, cy + rh / 2), 3 * S, col(s.c));
        text(BODY_M, 13, x + 26 * S, cy + 9 * S + 2 * S, col(P.text), s.a, 0, 140 * S);
        text(BODY, 13, x + 26 * S + 152 * S, cy + 9 * S + 2 * S, col(P.muted), s.b, 0, sw - 178 * S);
        cy += rh;
        hline(x, x + sw, cy, col(P.line));
    }
    cy += 22 * S;
    // The last save.
    const Json &saves = ST["saves"][ST["game"].str()];
    if (saves.size()) {
        size_t li = 0;
        for (size_t i = 1; i < saves.size(); ++i)
            if (saves[i]["time"].num() > saves[li]["time"].num()) li = i;
        const Json &s = saves[li];
        float chh = 176 * S * 9 / 16;
        ImRect r(ImVec2(x, cy), ImVec2(x + sw, cy + chh));
        Hit hc = hit("continue", r, !ST["launch"]["running"].truthy());
        if (hc.pressed) play();
        float hv = approach("continue-h", hc.hovered ? 1.f : 0.f, 14);
        DL->AddRectFilled(r.Min, r.Max, col(P.panel));
        ImRect shot(r.Min, ImVec2(x + 176 * S, r.Max.y));
        std::string id = ST["game"].str() == "p1" ? s["mission"].str() : "";
        const Json &list = ST["art"][ST["game"].str()];
        if (ST["game"].str() == "p2" && list.size()) {
            unsigned hsh = 0;
            for (char c : s["title"].str() + s["slot"].str()) hsh = hsh * 31 + (unsigned char)c;
            id = list[(size_t)(hsh % list.size())].str();
        }
        if (!id.empty() && list.contains_string(id))
            draw_picture(picture(ST["artDir"].str() + "/" + ST["game"].str() + "/" + id + ".bmp"), shot, 1 - hv, 1, 1, 1 + 0.04f * hv);
        else DL->AddRectFilledMultiColor(shot.Min, shot.Max, col(rgba(23, 22, 26)), col(rgba(15, 14, 17)), col(rgba(11, 10, 12)), col(rgba(15, 14, 17)));
        DL->AddRect(r.Min, r.Max, col(mix(P.line, P.line_strong, hv)), 0, S);
        float tx = x + 176 * S + 16 * S, tw = sw - 176 * S - 32 * S, ty = cy + (chh - (10 * 1.2f + 3 + 17 * 1.2f + 3 + 12 * 1.5f) * S) / 2;
        text(MONO, 10, tx, ty, col(P.dim), upper(t("lastSave")), 0.12f);
        ty += 10 * 1.2f * S + 3 * S;
        std::string title = upper(s["title"].str()) + (s["number"].is_null() ? "" : " (" + std::to_string(s["number"].integer()) + ")");
        text(DISPLAY_M, 17, tx, ty, col(P.text), title, 0.06f, tw);
        ty += 17 * 1.2f * S + 3 * S;
        text(BODY, 12, tx, ty + 2 * S, col(P.muted), t("slot", {{"slot", s["slot"].str()}}) + " · " + fmt_date((long long)s["time"].num()), 0, tw);
        // A play mark that appears on hover.
        if (hv > 0.01f) {
            ImVec2 c(x + 176 * S / 2, cy + chh / 2);
            DL->AddCircleFilled(c, 18 * S, col(ImVec4(0, 0, 0, 0.55f * hv)), 32);
            DL->AddTriangleFilled(ImVec2(c.x - 5 * S, c.y - 8 * S), ImVec2(c.x + 9 * S, c.y), ImVec2(c.x - 5 * S, c.y + 8 * S), col(ImVec4(1, 1, 1, hv)));
        }
        cy += chh + 22 * S;
    }
    // Launch settings.
    text(MONO, 10.5f, x, cy, col(P.muted), upper(t("launchSettings")), 0.14f);
    cy += 10.5f * 1.2f * S + 8 * S;
    const Json &pg = ST["perGame"][ST["game"].str()];
    std::string fps = ST["game"].str() == "p1" ? std::to_string(pg["fps"].integer()) : pg["fps60"].truthy() ? "60" : "30";
    std::string res = pg["render_res"].str();
    size_t xp = res.find('x');
    if (xp != std::string::npos) res = res.substr(0, xp) + " × " + res.substr(xp + 1);
    std::string pmode = prompts == "auto" ? lower(opt("auto")) : prompts == "keyboard" ? lower(opt("keyboard")) : prompts == "gamepad" ? "PlayStation" : prompts == "xbox" ? "Xbox" : prompts;
    struct Pf { std::string k, v, page; };
    std::vector<Pf> prof = {
        {t("pfRes"), res, "graphics"}, {t("pfFps"), fps + " FPS", "graphics"},
        {t("pfFsr"), V("fsr1").truthy() ? t("on") : t("off"), "graphics"},
        {t("pfSharp"), V("rcas").truthy() ? std::to_string(V("rcas_strength").integer()) + "%" : t("off"), "graphics"},
        {t("pfScreen"), lower(opt(V("display_mode").str().c_str())), "display"}, {t("pfVsync"), V("vsync").truthy() ? t("on") : t("off"), "display"},
        {t("pfIcons"), pmode, "controls"},
        {t("pfMouse"), V("mouse_look").truthy() ? t("look", {{"n", std::to_string(V("mouse_sensitivity").integer())}}) : t("off"), "controls"}};
    float gw = sw / 4, gh = 8 * S * 2 + 9.5f * 1.2f * S + 13 * 1.25f * S;
    hline(x, x + sw, cy, col(P.line));
    DL->AddRectFilled(ImVec2(x, cy), ImVec2(x + S, cy + gh * 2), col(P.line));
    for (int i = 0; i < 8; ++i) {
        float bx = x + (i % 4) * gw, by = cy + (i / 4) * gh;
        ImRect r(ImVec2(bx, by), ImVec2(bx + gw, by + gh));
        Hit hp = hit("pf-" + std::to_string(i), r);
        float hv = approach(("pf-h" + std::to_string(i)).c_str(), hp.hovered ? 1.f : 0.f, 16);
        DL->AddRectFilled(r.Min, r.Max, col(ImVec4(1, 1, 1, 0.035f * hv)));
        DL->AddRectFilled(ImVec2(r.Max.x - S, r.Min.y), r.Max, col(P.line));
        DL->AddRectFilled(ImVec2(r.Min.x, r.Max.y - S), r.Max, col(P.line));
        text(MONO, 9.5f, bx + 12 * S, by + 8 * S, col(P.dim), upper(prof[i].k), 0.1f, gw - 20 * S);
        text(BODY_M, 13, bx + 12 * S, by + 8 * S + 9.5f * 1.2f * S, col(P.text), prof[i].v, 0, gw - 20 * S);
        if (hp.pressed) go(prof[i].page);
    }
    cy += gh * 2;
    float left_h = cy - y;
    // ---- the wall
    float wx = x + cw + 44 * S, wy = y;
    text(MONO, 10.5f, wx, wy, col(P.muted), t((ST["game"].str() + "Wall").c_str()), 0.14f);
    time_t now = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char clock[16];
    snprintf(clock, sizeof clock, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    text(MONO, 10.5f, wx + cw - text_width(MONO, 10.5f, clock, 0.14f), wy, col(P.muted), clock, 0.14f);
    wy += 10.5f * 1.2f * S + 10 * S;
    float bh = cw / 2.1f;
    monitor(g_mons[0], ImRect(ImVec2(wx, wy), ImVec2(wx + cw, wy + bh)), true, 0);
    wy += bh + 6 * S;
    float mw = (cw - 6 * S) / 2, mh = mw / 1.6f;
    for (int i = 1; i < 5; ++i) {
        float mx = wx + ((i - 1) % 2) * (mw + 6 * S), my = wy + ((i - 1) / 2) * (mh + 6 * S);
        monitor(g_mons[i], ImRect(ImVec2(mx, my), ImVec2(mx + mw, my + mh)), false, i);
    }
    wy += mh * 2 + 6 * S + 10 * S;
    std::string foot = ST["art"].is_null() ? "" : wall_ids().empty() ? t("noArt") : t((ST["game"].str() + "Foot").c_str());
    if (!foot.empty()) wy += para(BODY, BODY_M, 11.5f, wx, wy, cw, col(P.dim), col(P.text), foot);
    return ImMax(left_h, wy - y);
}

// ================================================================ graphics, display, gamepad
float page_settings(const std::string &page, float x, float y, float w) {
    float cw = (w - GAP * S) / 2, cy = y;
    float a = 0, b = 0;
    if (page == "graphics") {
        bool p1 = ST["game"].str() == "p1";
        cy += title_block(x, cy, w, t("navGraphics"), t(p1 ? "gfxLedeP1" : "gfxLedeP2"));
        std::vector<std::pair<Json, std::string>> res;
        for (auto r : {"1280x720", "1600x900", "1920x1080", "2560x1440", "3200x1800", "3840x2160"}) {
            std::string s = r;
            size_t xp = s.find('x');
            res.push_back({Json(r), s.substr(0, xp) + " × " + s.substr(xp + 1)});
        }
        std::vector<Row> game;
        Row rr = options_row("res", G("render_res"), res, sg("render_res"));
        rr.tag = t("tag");
        game.push_back(rr);
        if (p1) {
            std::vector<std::pair<Json, std::string>> f;
            for (int v : {30, 60, 90, 120, 144, 165, 240}) f.push_back({Json(v), std::to_string(v) + " FPS"});
            game.push_back(options_row("fpsP1", G("fps"), f, sg("fps")));
            game.push_back(toggle_row("tendrils", G("tendril_fix"), sg("tendril_fix")));
            game.push_back(toggle_row("discord", V("discord"), sv("discord")));
        } else {
            Row f = options_row("fpsP2", Json(G("fps60").truthy()), {{Json(false), "30 FPS"}, {Json(true), "60 FPS"}}, sg("fps60"));
            f.tag = t("tag");
            game.push_back(f);
        }
        a = group(x, cy, cw, t("onlyFor", {{"game", game_name()}}), game);
        std::vector<Row> image = {toggle_row("fsr1", V("fsr1"), sv("fsr1")), toggle_row("rcas", V("rcas"), sv("rcas"))};
        Row sharp = range_row("sharp", V("rcas_strength"), 0, 100, sv("rcas_strength"));
        sharp.locked = !V("rcas").truthy();
        image.push_back(sharp);
        image.push_back(options_row("aniso", V("anisotropy"), {{Json(0), opt("asGame")}, {Json(2), "2×"}, {Json(4), "4×"}, {Json(8), "8×"}, {Json(16), "16×"}}, sv("anisotropy")));
        b = group(x + cw + GAP * S, cy, cw, t("imageBoth"), image);
    } else if (page == "display") {
        cy += title_block(x, cy, w, t("navDisplay"), t("displayLede"));
        a = group(x, cy, cw, t("window"), {options_row("display", V("display_mode"), {{Json("windowed"), opt("windowed")}, {Json("borderless"), opt("borderless")}, {Json("fullscreen"), opt("fullscreen")}}, sv("display_mode")),
                                           toggle_row("vsync", V("vsync"), sv("vsync"))});
        b = group(x + cw + GAP * S, cy, cw, t("inGame"), {toggle_row("fpsCounter", V("show_fps"), sv("show_fps"))}, t("inGameNote"));
    } else if (page == "gamepad") {
        cy += title_block(x, cy, w, t("navPad"), t("padLede"));
        std::vector<std::pair<Json, std::string>> vib;
        for (int v : {0, 25, 50, 75, 100, 150, 200}) vib.push_back({Json(v), v ? std::to_string(v) + "%" : opt("offOpt")});
        a = group(x, cy, cw, t("vibration"), {options_row("vib", V("vibration"), vib, sv("vibration")),
                                               options_row("menuVib", V("menu_vibration"), {{Json(0), opt("offOpt")}, {Json(1), opt("presses")}, {Json(2), opt("all")}}, sv("menu_vibration")),
                                               toggle_row("actionVib", V("extra_vibration"), sv("extra_vibration"))});
        Row snd = options_row("soundHaptics", V("haptics_sound"), {{Json(0), opt("offOpt")}, {Json(1), opt("low")}, {Json(2), opt("medium")}, {Json(3), opt("high")}}, sv("haptics_sound"));
        snd.locked = !V("haptics").truthy();
        b = group(x + cw + GAP * S, cy, cw, "DualSense", {toggle_row("lightbar", V("light_bar"), sv("light_bar")), toggle_row("haptics", V("haptics"), sv("haptics")), snd});
    }
    return cy - y + ImMax(a, b);
}

// ================================================================ keyboard and mouse
float page_controls(float x, float y, float w) {
    static std::string group_id = "move";
    float cw = (w - GAP * S) / 2, cy = y + title_block(x, y, w, t("keysTitle"), t("keysLede"));
    Row sens = range_row("sens", V("mouse_sensitivity"), 10, 400, sv("mouse_sensitivity"));
    sens.locked = !V("mouse_look").truthy();
    float a = group(x, cy, cw, t("mouseIcons"), {options_row("prompts", V("prompts"), {{Json("auto"), opt("auto")}, {Json("keyboard"), opt("keyboard")}, {Json("gamepad"), "PlayStation"}, {Json("xbox"), "Xbox"}}, sv("prompts")),
                                                 toggle_row("mouseLook", V("mouse_look"), sv("mouse_look")), sens,
                                                 toggle_row("menuMouse", V("menu_mouse"), sv("menu_mouse"))});
    // Actions.
    float x2 = x + cw + GAP * S, ky = cy + group_head(x2, cy, cw, t("actions"));
    const Json &c = ST["controls"];
    bool found = false;
    for (size_t i = 0; i < c["groups"].size(); ++i) found |= c["groups"][i][0].str() == group_id;
    if (!found && c["groups"].size()) group_id = c["groups"][0][0].str();
    float tx = x2;
    static float ul_x = -1, ul_w = 0;
    for (size_t i = 0; i < c["groups"].size(); ++i) {
        const Json &g = c["groups"][i];
        std::string label = g[ru() ? 2 : 1].str();
        float lw = text_width(BODY, 13, label);
        ImRect r(ImVec2(tx, ky), ImVec2(tx + lw, ky + 34 * S));
        Hit h = hit("kg-" + g[0].str(), r);
        bool on = g[0].str() == group_id;
        float hv = approach(("kg-h" + g[0].str()).c_str(), on || h.hovered ? 1.f : 0.f, 14);
        text(BODY, 13, tx, ky + 6 * S, col(mix(P.muted, P.text, hv)), label);
        if (h.pressed) { group_id = g[0].str(); g_capture.on = false; }
        if (on) {
            if (ul_x < 0) ul_x = tx, ul_w = lw;
            ul_x += (tx - ul_x) * (1 - expf(-g_dt * 16));
            ul_w += (lw - ul_w) * (1 - expf(-g_dt * 16));
        }
        tx += lw + 18 * S;
    }
    hline(x2, x2 + cw, ky + 34 * S, col(P.line));
    if (ul_x >= 0) hline(ul_x, ul_x + ul_w, ky + 34 * S, col(P.accent_hi));
    ky += 34 * S + S + 6 * S;
    std::string prompts = V("prompts").str();
    bool xbox = prompts == "xbox" || (prompts == "auto" && ST["pad"]["kind"].str() == "xbox");
    static const std::map<std::string, std::pair<const char *, const char *>> PAD = {
        {"cross", {"✕", "A"}}, {"circle", {"○", "B"}}, {"square", {"□", "X"}}, {"triangle", {"△", "Y"}}, {"l1", {"L1", "LB"}}, {"r1", {"R1", "RB"}},
        {"l2", {"L2", "LT"}}, {"r2", {"R2", "RT"}}, {"l3", {"L3", "LS"}}, {"r3", {"R3", "RS"}}, {"up", {"↑", "↑"}}, {"down", {"↓", "↓"}},
        {"left", {"←", "←"}}, {"right", {"→", "→"}}, {"touchpad", {"TP", "⧉"}}, {"options", {"OPT", "☰"}}};
    static const std::map<std::string, const char *> KEY_RU = {{"Space", "Пробел"}, {"Mouse Left", "ЛКМ"}, {"Mouse Right", "ПКМ"}, {"Mouse Middle", "СКМ"},
                                                                {"Mouse X1", "Мышь 4"}, {"Mouse X2", "Мышь 5"}, {"Wheel Up", "Колесо ↑"}, {"Wheel Down", "Колесо ↓"}};
    static const std::map<std::string, const char *> KEY_EN = {{"Mouse Left", "Left click"}, {"Mouse Right", "Right click"}, {"Mouse Middle", "Middle click"},
                                                                {"Mouse X1", "Mouse 4"}, {"Mouse X2", "Mouse 5"}, {"Wheel Up", "Wheel ↑"}, {"Wheel Down", "Wheel ↓"}};
    static const std::map<std::string, const char *> KEY_SHORT = {{"Return", "Enter"}, {"Escape", "Esc"}, {"Left Shift", "L-Shift"}, {"Right Shift", "R-Shift"},
                                                                   {"Left Ctrl", "L-Ctrl"}, {"Right Ctrl", "R-Ctrl"}, {"Left Alt", "L-Alt"}, {"Right Alt", "R-Alt"}};
    auto key_text = [&](const std::string &k) -> std::string {
        auto &m = ru() ? KEY_RU : KEY_EN;
        if (auto it = m.find(k); it != m.end()) return it->second;
        if (auto it = KEY_SHORT.find(k); it != KEY_SHORT.end()) return it->second;
        return k;
    };
    std::vector<std::string> clashes;
    for (size_t i = 0; i < c["clashes"].size(); ++i) clashes.push_back(lower(c["clashes"][i].str()));
    int slots = c["slots"].integer(2);
    for (size_t i = 0; i < c["actions"].size(); ++i) {
        const Json &a = c["actions"][i];
        if (a["group"].str() != group_id) continue;
        float rh = 7 * S * 2 + 24 * S;
        std::string pad = a["pad"].str(), pn;
        if (!pad.compare(0, 6, "lstick")) {
            std::string d = pad.substr(pad.find('_') + 1);
            pn = std::string("L") + (d == "up" ? "↑" : d == "down" ? "↓" : d == "left" ? "←" : "→");
        } else if (auto it = PAD.find(pad); it != PAD.end()) pn = xbox ? it->second.second : it->second.first;
        else pn = pad;
        text(MONO, 10, x2, ky + (rh - 12 * S) / 2, col(P.dim), pn);
        float kw = 96 * S, names_w = cw - 34 * S - (kw + 10 * S) * slots - 10 * S;
        text(BODY, 13, x2 + 44 * S, ky + (rh - 15.6f * S) / 2, col(P.text), a[ru() ? "ru" : "en"].str(), 0, names_w);
        for (int s = 0; s < slots; ++s) {
            std::string key = a["keys"][s].str();
            float kx = x2 + cw - (kw + 10 * S) * (slots - s) + 10 * S;
            ImRect r(ImVec2(kx, ky + 7 * S), ImVec2(kx + kw, ky + rh - 7 * S));
            std::string id = "kb-" + a["id"].str() + "-" + std::to_string(s);
            bool waiting = g_capture.on && g_capture.action == a["id"].str() && g_capture.slot == s;
            Hit h = hit(id, r);
            if (h.pressed && !waiting) {
                g_capture.on = true;
                g_capture.action = a["id"].str();
                g_capture.slot = s;
                g_capture.armed = SDL_GetTicks() / 1000.0;
                waiting = true;
            }
            if (waiting) capture_box(r);
            float hv = approach((id + "h").c_str(), h.hovered ? 1.f : 0.f, 16);
            bool clash = !key.empty() && std::find(clashes.begin(), clashes.end(), lower(key)) != clashes.end();
            DL->AddRectFilled(r.Min, r.Max, col(ImVec4(0, 0, 0, 0.35f)));
            ImVec4 border = waiting ? P.accent_hi : clash ? mix(WARN, P.line_strong, 0.5f) : mix(P.line_strong, P.muted, hv);
            float pulse = waiting ? 0.55f + 0.45f * sinf(g_time * 6) : 1;
            DL->AddRect(r.Min, r.Max, col(border, pulse), 0, S);
            std::string label = waiting ? t("pressKey") : key.empty() ? "—" : key_text(key);
            ImVec4 tc = waiting ? P.accent_hi : key.empty() ? P.dim : clash ? WARN : P.text;
            float lw = ImMin(text_width(MONO, 11, label), kw - 12 * S);
            text(MONO, 11, r.GetCenter().x - lw / 2, r.Min.y + (r.GetHeight() - 13.2f * S) / 2, col(tc), label, 0, kw - 12 * S);
        }
        ky += rh;
        hline(x2, x2 + cw, ky - S, col(P.line));
    }
    ky += 12 * S;
    if (button("keys-reset", x2, ky, t("resetKeys"))) act("reset_keys");
    ky += 32 * S + 10 * S;
    ky += para(BODY, BODY_M, 12, x2, ky, cw, col(P.muted), col(P.text), t("keysHint"));
    return ImMax(cy - y + a, ky - y);
}

// ================================================================ saves
float page_saves(float x, float y, float w) {
    float cy = y + title_block(x, y, w, t("navSaves"), t("savesLede"));
    float bw;
    if (button("sv-backup", x, cy, t("backupNow"), true, true, &bw)) act("backup");
    float bw2;
    if (button("sv-folder", x + bw + 8 * S, cy, t("openFolder"), false, true, &bw2)) {
        Json j = Json::object();
        j.set("what", "saves");
        Ipc::request("open", j, [](const Json &r, const std::string &) { if (!r["ok"].truthy()) toast(t("n_openFailed")); });
    }
    const Json &list = ST["saves"][ST["game"].str()];
    std::string cnt = count((int)list.size(), "cells") + " · " + game_name();
    text(MONO, 11, x + w - text_width(MONO, 11, cnt), cy + 9 * S, col(P.muted), cnt);
    cy += 32 * S + 18 * S;
    if (!list.size()) cy += para(BODY, BODY_M, 14, x, cy + 26 * S, ImMin(w, 560 * S), col(P.muted), col(P.text), t("noSavesYet")) + 52 * S;
    std::vector<size_t> order;
    for (size_t i = 0; i < list.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return list[a]["time"].num() > list[b]["time"].num(); });
    int cols = ImMax(1, (int)((w + 12 * S) / (232 * S + 12 * S)));
    float cardw = (w - 12 * S * (cols - 1)) / cols, shoth = cardw * 9 / 16, infoh = 12 * S + 18 * S + 3 * S + 13 * S + 3 * S + 13 * S + 12 * S;
    const Json &art = ST["art"][ST["game"].str()];
    bool p2 = ST["game"].str() == "p2";
    for (size_t n = 0; n < order.size(); ++n) {
        const Json &s = list[order[n]];
        float cx = x + (n % cols) * (cardw + 12 * S), ry = cy + (n / cols) * (shoth + infoh + 12 * S);
        // Cards come in one after another when the page opens.
        ImRect r(ImVec2(cx, ry), ImVec2(cx + cardw, ry + shoth + infoh));
        std::string id = "save-" + s["dir"].str();
        Hit h = hit(id, r);
        float hv = approach((id + "h").c_str(), h.hovered ? 1.f : 0.f, 10);
        DL->AddRectFilled(r.Min, r.Max, col(P.panel_solid));
        ImRect shot(r.Min, ImVec2(r.Max.x, ry + shoth));
        std::string pic = !p2 ? s["mission"].str() : "";
        if (p2 && art.size()) {
            unsigned hsh = 0;
            for (char c : s["title"].str() + s["slot"].str()) hsh = hsh * 31 + (unsigned char)c;
            pic = art[(size_t)(hsh % art.size())].str();
        }
        DL->PushClipRect(shot.Min, shot.Max, true);
        if (!pic.empty() && art.contains_string(pic))
            draw_picture(picture(ST["artDir"].str() + "/" + ST["game"].str() + "/" + pic + ".bmp"), shot, (p2 ? 0.25f : 0.8f) * (1 - hv), 1, 1, 1 + 0.05f * hv);
        else DL->AddRectFilledMultiColor(shot.Min, shot.Max, col(rgba(23, 22, 26)), col(rgba(15, 14, 17)), col(rgba(11, 10, 12)), col(rgba(15, 14, 17)));
        DL->PopClipRect();
        std::string slot = s["number"].is_null() ? s["slot"].str() : pad2(s["number"].integer());
        text(DISPLAY_M, 20, cx + 11 * S + 1 * S, ry + 9 * S + 1 * S, col(ImVec4(0, 0, 0, 0.7f)), slot);
        text(DISPLAY_M, 20, cx + 11 * S, ry + 9 * S, col(ImVec4(1, 1, 1, 1)), slot);
        float ix = cx + 12 * S, iw = cardw - 24 * S, iy = ry + shoth + 10 * S;
        text(DISPLAY_M, 15, ix, iy, col(P.text), upper(s["title"].str()), 0.05f, iw);
        iy += 18 * S + 3 * S;
        std::vector<std::string> parts;
        std::string detail = s["detail"].str(), part;
        for (size_t i = 0; i <= detail.size(); ++i) {
            if (i == detail.size() || detail[i] == '\n') {
                size_t a = part.find_first_not_of(" \t\r"), b = part.find_last_not_of(" \t\r");
                if (a != std::string::npos) parts.push_back(lower(part.substr(a, b - a + 1)));
                part.clear();
            } else part += detail[i];
        }
        if (!p2 && parts.size() == 1 && parts[0].find_first_of(":%") == std::string::npos) parts[0] = lower(t("difficulty", {{"d", parts[0]}}));
        if (s["auto"].truthy()) parts.insert(parts.begin(), t("auto"));
        std::string d;
        for (auto &p : parts) d += (d.empty() ? "" : " · ") + p;
        text(MONO, 10.5f, ix, iy, col(P.muted), capital(d), 0, iw);
        iy += 13 * S + 3 * S;
        bool latest = n == 0;
        std::string lt = latest ? t("latest") : "";
        float ltw = lt.empty() ? 0 : text_width(MONO, 10.5f, lt);
        text(MONO, 10.5f, ix, iy, col(P.muted), t("slot", {{"slot", s["slot"].str()}}) + " · " + fmt_date((long long)s["time"].num()), 0, iw - ltw - 8 * S);
        if (latest) text(MONO, 10.5f, ix + iw - ltw, iy, col(P.accent_hi), lt);
        DL->AddRect(r.Min, r.Max, col(mix(P.line, P.line_strong, hv)), 0, S);
    }
    if (order.size()) cy += ((order.size() + cols - 1) / cols) * (shoth + infoh + 12 * S) - 12 * S;
    cy += 30 * S;
    cy += group_head(x, cy, w, t("backups"));
    const Json &bk = ST["backups"];
    if (!bk.size()) cy += para(BODY, BODY_M, 14, x, cy + 26 * S, ImMin(w, 560 * S), col(P.muted), col(P.text), t("noBackups")) + 52 * S;
    for (size_t i = 0; i < bk.size(); ++i) {
        const Json &b = bk[i];
        float rh = 9 * S * 2 + 32 * S;
        text(MONO, 11.5f, x, cy + (rh - 13.8f * S) / 2, col(P.text), fmt_date((long long)b["time"].num()));
        text(BODY, 13, x + 166 * S, cy + (rh - 15.6f * S) / 2, col(P.muted), count(b["slots"].integer(), "cells"));
        float rw = text_width(BODY_M, 12.5f, t("restore")) + 28 * S;
        std::string name = b["name"].str(), when = fmt_date((long long)b["time"].num());
        if (button("restore-" + name, x + w - rw, cy + 9 * S, t("restore")))
            ask(t("restoreTitle"), t("restoreText", {{"when", when}}), "", t("restore"), [name] {
                Json j = Json::object();
                j.set("name", name);
                act("restore_backup", j);
            });
        cy += rh;
        hline(x, x + w, cy - S, col(P.line));
    }
    return cy - y;
}

// ================================================================ mods
float page_mods(float x, float y, float w) {
    float cw = (w - GAP * S) / 2, cy = y + title_block(x, y, w, t("navMods"), t("modsLede"));
    float my = cy + group_head(x, cy, cw, t("shaderMods"));
    const Json &mods = ST["mods"];
    if (!mods["named"].size() && !mods["loose"].integer()) my += para(BODY, BODY_M, 14, x, my + 26 * S, cw, col(P.muted), col(P.text), t("noMods")) + 52 * S;
    for (size_t i = 0; i < mods["named"].size(); ++i) {
        const Json &m = mods["named"][i];
        float rh = 12 * S * 2 + 40 * S;
        DL->AddRect(ImVec2(x, my + 12 * S), ImVec2(x + 40 * S, my + 52 * S), col(P.line_strong), 0, S);
        float gw = text_width(MONO, 9.5f, "GLSL");
        text(MONO, 9.5f, x + 20 * S - gw / 2, my + 32 * S - 6 * S, col(P.muted), "GLSL");
        text(BODY_M, 14, x + 54 * S, my + 12 * S + 2 * S, col(P.text), m["name"].str(), 0, cw - 54 * S - 60 * S);
        text(MONO, 10.5f, x + 54 * S, my + 12 * S + 22 * S, col(P.dim), "mods/" + m["name"].str() + " · " + count(m["files"].integer(), "files"), 0, cw - 54 * S - 60 * S);
        std::string name = m["name"].str();
        bool enabled = m["enabled"].truthy();
        toggle("mod-" + name, x + cw - 38 * S, my + rh / 2 - 10 * S, enabled, true, [name, enabled](Json) {
            Json j = Json::object();
            j.set("name", name);
            j.set("enabled", !enabled);
            act("mod", j, [name, enabled](const Json &) { toast(t(enabled ? "modOff" : "modOn", {{"name", name}})); });
        });
        my += rh;
        hline(x, x + cw, my - S, col(P.line));
    }
    if (mods["loose"].integer()) my += 10 * S + para(BODY, BODY_M, 12, x, my + 10 * S, cw, col(P.muted), col(P.text), t("looseShaders", {{"n", std::to_string(mods["loose"].integer())}}));
    my += 14 * S;
    if (button("mods-open", x, my, t("openMods"))) {
        Json j = Json::object();
        j.set("what", "mods");
        Ipc::request("open", j);
    }
    my += 32 * S;
    my += note(x, my, cw, t("modsNote")) + 30 * S;
    float x2 = x + cw + GAP * S, ly = cy + group_head(x2, cy, cw, t("luaScripts"), t("in12"));
    ly += note(x2, ly - 16 * S, cw, t("luaNote"));
    return ImMax(my, ly) - y;
}

// ================================================================ news
float page_news(float x, float y, float w) {
    float cy = y + 8 * S;
    text(DISPLAY_M, 30, x, cy, col(P.text), upper(t("navNewsTitle")), 0.06f);
    cy += 30 * 1.2f * S + 6 * S + 20 * S;
    // The update box.
    const Json &up = ST["update"];
    float bw = ImMin(w, 760 * S), bh = 64 * S;
    DL->AddRectFilled(ImVec2(x, cy), ImVec2(x + bw, cy + bh), col(P.panel));
    DL->AddRect(ImVec2(x, cy), ImVec2(x + bw, cy + bh), col(P.line), 0, S);
    ImVec4 dot = WARN;
    std::string head, sub, btn;
    std::function<void()> on;
    auto check = [] {
        Json st = ST;
        st.set("update", Json());
        ST = st;
        Ipc::request("check_update", Json::object(), [](const Json &r, const std::string &) {
            Json s = ST;
            s.set("update", r["update"]);
            ST = s;
        });
    };
    int beta = ST["beta"].integer();
    if (up.is_null()) head = t("updChecking");
    else if (!up["ok"].truthy()) { head = t("updFail"); sub = t("updFailText"); btn = t("checkUpdates"); on = check; }
    else if (up["newer"].truthy()) {
        dot = P.accent_hi;
        head = t("updNewer", {{"v", up["latest"].str()}});
        sub = t("updNewerText");
        btn = t("download");
        std::string url = up["url"].str();
        on = [url] { Json j = Json::object(); j.set("url", url); act("url", j); };
    } else {
        dot = beta ? WARN : OK;
        head = beta ? t("updTest", {{"v", ST["version"].str() + " · b" + std::to_string(beta)}}) : t("updLatest");
        sub = t("updLatestText", {{"v", up["latest"].str().empty() ? "—" : up["latest"].str()}});
        btn = t("checkUpdates");
        on = check;
    }
    float pulse = up.is_null() ? 0.4f + 0.6f * (0.5f + 0.5f * sinf(g_time * 5)) : 1;
    DL->AddCircleFilled(ImVec2(x + 18 * S + 3.5f * S, cy + bh / 2), 3.5f * S, col(dot, pulse));
    float tx = x + 18 * S + 7 * S + 16 * S;
    if (sub.empty()) text(BODY_M, 14, tx, cy + (bh - 16.8f * S) / 2, col(P.text), head);
    else {
        text(BODY_M, 14, tx, cy + 12 * S, col(P.text), head);
        text(BODY, 12.5f, tx, cy + 12 * S + 20 * S, col(P.muted), sub, 0, bw - (tx - x) - 200 * S);
    }
    if (!btn.empty()) {
        float b = text_width(BODY_M, 12.5f, btn) + 28 * S;
        if (button("upd-btn", x + bw - 18 * S - b, cy + (bh - 32 * S) / 2, btn) && on) on();
    }
    cy += bh + 22 * S;
    // The releases.
    const Json &rel = tj("releases");
    for (size_t i = 0; i < rel.size(); ++i) {
        const Json &r = rel[i];
        hline(x, x + w, cy, col(P.line));
        float ry = cy + 22 * S;
        text(DISPLAY_M, 26, x, ry, col(P.text), r[0].str(), 0.06f);
        std::string when = r[1].str().empty() ? (beta ? "b" + std::to_string(beta) : "") : r[1].str();
        text(MONO, 11, x, ry + 26 * 1.2f * S + 2 * S, col(P.muted), when);
        float lx = x + 150 * S + 28 * S, lw = w - 178 * S, ly = ry;
        for (size_t k = 0; k < r[2].size(); ++k) {
            const Json &it = r[2][k];
            text(MONO, 9.5f, lx, ly + 3 * S, col(P.muted), it[0].str(), 0.1f);
            text(BODY_M, 14, lx + 106 * S, ly, col(P.text), it[1].str(), 0, lw - 106 * S);
            ly += 14 * 1.5f * S + 2 * S;
            ly += para(BODY, BODY_M, 12.5f, lx + 106 * S, ly, ImMin(lw - 106 * S, 600 * S), col(P.muted), col(P.text), it[2].str()) + 14 * S;
        }
        cy = ImMax(ly - 14 * S, ry + 60 * S) + 22 * S;
    }
    return cy - y;
}

// ================================================================ the game folder
float page_folder(float x, float y, float w) {
    float cy = y + title_block(x, y, w, t("gameFolder"), t("folderLede"));
    float pw = ImMin(w, 760 * S);
    float cbw = text_width(BODY_M, 12.5f, t("change")) + 28 * S;
    ImRect box(ImVec2(x, cy), ImVec2(x + pw - cbw - 8 * S, cy + 34 * S));
    DL->AddRectFilled(box.Min, box.Max, col(ImVec4(0, 0, 0, 0.45f)));
    DL->AddRect(box.Min, box.Max, col(P.line), 0, S);
    std::string path = ST["gameDir"].str().empty() ? t("notSelected") : ST["gameDir"].str();
    text(MONO, 12.5f, x + 12 * S, cy + (34 * S - 15 * S) / 2, col(ST["gameDir"].str().empty() ? P.dim : P.text), path, 0, box.GetWidth() - 24 * S);
    if (button("fd-change", box.Max.x + 8 * S, cy + 1 * S, t("change"))) {
        static std::string start;
        start = ST["gameDir"].str();
        SDL_ShowOpenFolderDialog(folder_chosen, nullptr, app_window(), start.empty() ? nullptr : start.c_str(), false);
    }
    cy += 34 * S + 16 * S;
    hline(x, x + pw, cy, col(P.line));
    const Json &checks = ST["checks"];
    for (size_t i = 0; i < checks.size(); ++i) {
        float rh = 9 * S * 2 + 13 * 1.5f * S;
        std::string state = checks[i][1].str();
        DL->AddCircleFilled(ImVec2(x + 3 * S, cy + rh / 2), 3 * S, col(state == "bad" ? P.accent_hi : state == "warn" ? WARN : OK));
        std::string detail = checks[i][2].str();
        float dw = ImMin(text_width(MONO, 11.5f, detail), pw * 0.55f);
        text(BODY_M, 13, x + 26 * S, cy + 9 * S + 2 * S, col(P.text), checks[i][0].str(), 0, pw - 26 * S - dw - 16 * S);
        text(MONO, 11.5f, x + pw - dw, cy + 9 * S + 4 * S, col(P.muted), detail, 0, dw + 1);
        cy += rh;
        hline(x, x + pw, cy, col(P.line));
    }
    cy += 20 * S;
    float bx = x, bw;
    bool preparing = ST["launch"]["busy"].str() == "prepare";
    std::string prep = preparing ? t("preparing") : t("prepare");
    if (preparing) {  // dots that walk while it works
        int dots = (int)(g_time * 3) % 4;
        prep = t("preparing");
        while (!prep.empty() && (prep.back() == '.' || (unsigned char)prep.back() >= 0x80)) prep.pop_back();
        prep += std::string(dots, '.');
    }
    if (button("fd-prepare", bx, cy, prep, false, ST["ready"].truthy() && ST["launch"]["busy"].str().empty(), &bw)) act("prepare");
    bx += bw + 8 * S;
    if (ST["restorable"].truthy()) {
        if (button("fd-restore", bx, cy, t("restoreOriginals"), false, true, &bw))
            ask(t("restoreOrigTitle"), t("restoreOrigText"), "", "", [] { act("restore_originals"); });
        bx += bw + 8 * S;
    }
    if (button("fd-clean", bx, cy, t("clean"), false, true, &bw)) {
        Ipc::request("clean_preview", Json::object(), [](const Json &p, const std::string &err) {
            if (!err.empty()) return;
            int n = (int)p["files"].size();
            if (!n) { toast(t("cleanNothing")); return; }
            std::string list;
            for (int i = 0; i < n; ++i) list += (i ? "\n" : "") + p["files"][i].str();
            ask(t("cleanTitle"), t("cleanText", {{"n", std::to_string(n)}, {"files", plural(n, "files")}, {"mb", std::to_string(p["mb"].integer())}}), list, "",
                [] { act("clean"); });
        });
    }
    bx += bw + 8 * S;
    if (button("fd-log", bx, cy, t("openLog"))) {
        Json j = Json::object();
        j.set("what", "log");
        Ipc::request("open", j, [](const Json &r, const std::string &) { if (!r["ok"].truthy()) toast(t("n_openFailed")); });
    }
    cy += 32 * S + 30 * S;
    std::vector<std::pair<Json, std::string>> langs = {{Json("auto"), t("langAuto")}, {Json("en"), "English"}, {Json("ru"), "Русский"}};
    Json lang = V("ui_language").str().empty() ? Json("auto") : V("ui_language");
    Json minimise = V("minimize_on_play").is_null() ? Json(true) : V("minimize_on_play");
    cy += group(x, cy, ImMin(w, (w - GAP * S) / 2), t("launcher"), {options_row("language", lang, langs, sv("ui_language")), toggle_row("minimize", minimise, sv("minimize_on_play"))});
    return cy - y;
}
