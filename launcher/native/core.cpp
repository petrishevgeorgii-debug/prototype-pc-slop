// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE launcher, the native window: the launcher process link, fonts, text, pictures, input.
#include "app.h"

#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "misc/freetype/imgui_freetype.h"
#include "texts.h"

// ================================================================ the launcher process
namespace Ipc {
namespace {
std::mutex lock;
std::vector<std::string> inbox;
std::map<int, std::function<void(const Json &, const std::string &)>> waiting;
int next_id = 1;
bool open = false;
volatile bool closed = false;

void reader() {
    std::string buffer;
    char chunk[65536];
    for (;;) {
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), chunk, sizeof chunk, &got, nullptr) || !got) break;
        size_t n = got;
#else
        ssize_t got = read(0, chunk, sizeof chunk);
        if (got <= 0) break;
        size_t n = (size_t)got;
#endif
        buffer.append(chunk, n);
        size_t start = 0, nl;
        std::lock_guard<std::mutex> guard(lock);
        while ((nl = buffer.find('\n', start)) != std::string::npos) {
            inbox.push_back(buffer.substr(start, nl - start));
            start = nl + 1;
        }
        buffer.erase(0, start);
    }
    closed = true;
}
}  // namespace

bool start() {
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!in || in == INVALID_HANDLE_VALUE || !out || out == INVALID_HANDLE_VALUE || GetFileType(in) != FILE_TYPE_PIPE)
        return false;
#endif
    open = true;
    std::thread(reader).detach();
    return true;
}

bool alive() { return open && !closed; }

void request(const char *action, Json args, std::function<void(const Json &, const std::string &)> done) {
    if (!open) return;
    int id = next_id++;
    Json msg = Json::object();
    msg.set("id", id);
    msg.set("action", action);
    msg.set("args", std::move(args));
    std::string line = msg.dump() + "\n";
    {
        std::lock_guard<std::mutex> guard(lock);
        if (done) waiting[id] = std::move(done);
    }
#ifdef _WIN32
    DWORD wrote = 0;
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), (DWORD)line.size(), &wrote, nullptr)) closed = true;
#else
    if (fwrite(line.data(), 1, line.size(), stdout) != line.size() || fflush(stdout)) closed = true;
#endif
}

void pump() {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> guard(lock);
        lines.swap(inbox);
    }
    for (auto &line : lines) {
        Json reply = Json::parse(line);
        int id = reply["id"].integer(-1);
        std::function<void(const Json &, const std::string &)> done;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto it = waiting.find(id);
            if (it == waiting.end()) continue;
            done = std::move(it->second);
            waiting.erase(it);
        }
        if (done) done(reply["result"], reply["ok"].truthy() ? std::string() : reply["error"].str());
    }
}
}  // namespace Ipc

// ================================================================ look
Palette P;
const ImVec4 OK = ImVec4(0x6f / 255.f, 0xbf / 255.f, 0x8e / 255.f, 1), WARN = ImVec4(0xd2 / 255.f, 0xa2 / 255.f, 0x4a / 255.f, 1);
float S = 1, g_alpha = 1, g_time = 0, g_dt = 1 / 60.f;
ImDrawList *DL = nullptr;
SDL_Renderer *g_renderer = nullptr;
bool g_block_input = false;

ImU32 col(const ImVec4 &c, float a) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, ImClamp(c.w * a * g_alpha, 0.f, 1.f)));
}
ImVec4 rgba(int r, int g, int b, float a) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }
ImVec4 mix(const ImVec4 &a, const ImVec4 &b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
float ease_out(float t) { t = ImClamp(t, 0.f, 1.f); return 1 - (1 - t) * (1 - t) * (1 - t); }
float ease_in_out(float t) { t = ImClamp(t, 0.f, 1.f); return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2; }

static std::unordered_map<ImGuiID, float> g_values;
float approach(ImGuiID id, float target, float speed) {
    auto it = g_values.find(id);
    if (it == g_values.end()) return g_values[id] = target;
    float &v = it->second;
    v += (target - v) * (1 - expf(-g_dt * speed));
    if (fabsf(target - v) < 0.001f) v = target;
    return v;
}
float approach(const char *id, float target, float speed) { return approach(ImHashStr(id), target, speed); }

// ---------------------------------------------------------------- fonts
static ImFont *g_fonts[FONT_COUNT];
ImFont *font(Font f) { return g_fonts[f] ? g_fonts[f] : ImGui::GetIO().Fonts->Fonts[0]; }

static bool file_exists(const std::string &p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

// The face index (named instance << 16) of a variable font's instance nearest to wght/wdth.
static long variable_instance(const std::string &path, float wght, float wdth) {
    FT_Library lib;
    if (FT_Init_FreeType(&lib)) return 0;
    long best = 0;
    FT_Face face;
    size_t size = 0;
    void *data = SDL_LoadFile(path.c_str(), &size);  // UTF-8 paths on every system
    if (data && !FT_New_Memory_Face(lib, (const FT_Byte *)data, (FT_Long)size, 0, &face)) {
        FT_MM_Var *mm = nullptr;
        if ((face->face_flags & FT_FACE_FLAG_MULTIPLE_MASTERS) && !FT_Get_MM_Var(face, &mm)) {
            int iw = -1, id = -1;
            for (FT_UInt a = 0; a < mm->num_axis; ++a) {
                if (mm->axis[a].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) iw = (int)a;
                if (mm->axis[a].tag == FT_MAKE_TAG('w', 'd', 't', 'h')) id = (int)a;
            }
            float best_d = 1e9f;
            for (FT_UInt i = 0; i < mm->num_namedstyles; ++i) {
                float w = iw >= 0 ? mm->namedstyle[i].coords[iw] / 65536.f : 400, d = id >= 0 ? mm->namedstyle[i].coords[id] / 65536.f : 100;
                float dist = fabsf(w - wght) / 100 + fabsf(d - wdth) / 5;
                if (dist < best_d) best_d = dist, best = (long)(i + 1) << 16;
            }
            FT_Done_MM_Var(lib, mm);
        }
        FT_Done_Face(face);
    }
    SDL_free(data);
    FT_Done_FreeType(lib);
    return best;
}

static std::vector<std::string> font_dirs() {
    std::vector<std::string> dirs;
#ifdef _WIN32
    char buf[MAX_PATH];
    if (GetWindowsDirectoryA(buf, MAX_PATH)) dirs.push_back(std::string(buf) + "\\Fonts\\");
    if (const char *local = SDL_getenv("LOCALAPPDATA")) dirs.push_back(std::string(local) + "\\Microsoft\\Windows\\Fonts\\");
#else
    dirs = {"/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/TTF/", "/usr/share/fonts/dejavu/"};
#endif
    if (const char *extra = SDL_getenv("THEORDER_LAUNCHER_FONTS")) dirs.insert(dirs.begin(), std::string(extra) + "/");
    return dirs;
}

static std::string find_font(std::initializer_list<const char *> names) {
    for (auto &dir : font_dirs())
        for (auto name : names)
            if (file_exists(dir + name)) return dir + name;
    return {};
}

static ImFont *add_font(const std::string &path, long index, unsigned flags = 0) {
    if (path.empty()) return nullptr;
    ImFontConfig cfg;
    cfg.FontNo = (ImU32)index;
    cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting | flags;
    cfg.OversampleH = 1;
    ImFont *f = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), 16, &cfg);
    // Symbols the text faces may lack (◀ ▶ ✕ ○ □ △ arrows, ☰ ⧉).
    static std::string symbols = find_font({"seguisym.ttf", "DejaVuSans.ttf"});
    if (f && !symbols.empty()) {
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
        ImGui::GetIO().Fonts->AddFontFromFileTTF(symbols.c_str(), 16, &merge);
    }
    return f;
}

bool load_fonts() {
    // Windows' own faces (nothing ships): Bahnschrift's condensed cuts for titles, Segoe UI for
    // text, Cascadia Mono (else Consolas) for the console lines. Other systems: DejaVu.
    std::string bahn = find_font({"bahnschrift.ttf"});
    if (!bahn.empty()) {
        g_fonts[DISPLAY] = add_font(bahn, variable_instance(bahn, 400, 75));
        g_fonts[DISPLAY_M] = add_font(bahn, variable_instance(bahn, 600, 75));
        g_fonts[DISPLAY_B] = add_font(bahn, variable_instance(bahn, 700, 75));
    } else {
        std::string cond = find_font({"DejaVuSansCondensed.ttf", "arialn.ttf"});
        std::string cond_b = find_font({"DejaVuSansCondensed-Bold.ttf", "arialnb.ttf"});
        g_fonts[DISPLAY] = add_font(cond, 0);
        g_fonts[DISPLAY_M] = add_font(cond_b.empty() ? cond : cond_b, 0);
        g_fonts[DISPLAY_B] = g_fonts[DISPLAY_M];
    }
    std::string body = find_font({"segoeui.ttf", "DejaVuSans.ttf"});
    std::string body_m = find_font({"seguisb.ttf", "DejaVuSans-Bold.ttf"});
    g_fonts[BODY] = add_font(body, 0);
    g_fonts[BODY_M] = add_font(body_m.empty() ? body : body_m, 0);
    std::string mono = find_font({"CascadiaMono.ttf", "consola.ttf", "DejaVuSansMono.ttf"});
    g_fonts[MONO] = add_font(mono, mono.find("Cascadia") != std::string::npos ? variable_instance(mono, 400, 100) : 0);
    if (!g_fonts[BODY]) ImGui::GetIO().Fonts->AddFontDefault();
    return g_fonts[BODY] != nullptr;
}

static inline unsigned next_char(const char *&p, const char *end) {
    unsigned c;
    int n = ImTextCharFromUtf8(&c, p, end);
    p += n ? n : 1;
    return c;
}

float text_width(Font f, float px, const std::string &s, float track) {
    ImFontBaked *b = font(f)->GetFontBaked(px * S);
    float w = 0;
    const char *p = s.c_str(), *end = p + s.size();
    int n = 0;
    while (p < end) {
        unsigned c = next_char(p, end);
        w += b->GetCharAdvance((ImWchar)c) + track * px * S;
        ++n;
    }
    return n ? w - track * px * S : 0;
}

float text(Font f, float px, float x, float y, ImU32 color, const std::string &s, float track, float max_w) {
    ImFont *fo = font(f);
    float size = px * S;
    ImFontBaked *b = fo->GetFontBaked(size);
    const char *p = s.c_str(), *end = p + s.size();
    float spacing = track * size;
    float limit = max_w > 0 ? max_w : 1e9f;
    float dots = max_w > 0 ? b->GetCharAdvance((ImWchar)0x2026) : 0;
    bool cut = max_w > 0 && text_width(f, px, s, track) > max_w;
    float cx = x, top = y + size * 0.1f;
    while (p < end) {
        unsigned c = next_char(p, end);
        float adv = b->GetCharAdvance((ImWchar)c);
        if (cut && cx + adv + dots - x > limit) {
            fo->RenderChar(DL, size, ImVec2(IM_ROUND(cx), IM_ROUND(top)), color, (ImWchar)0x2026);
            cx += dots;
            break;
        }
        if (c != ' ') fo->RenderChar(DL, size, ImVec2(IM_ROUND(cx), IM_ROUND(top)), color, (ImWchar)c);
        cx += adv + spacing;
    }
    return cx - x;
}

float para(Font f, Font bold, float px, float x, float y, float width, ImU32 color, ImU32 bold_color, const std::string &s,
           float line, bool draw) {
    // Words with their style; <b> and </b> switch the style, \n breaks the line.
    struct Word { std::string text; bool bold; bool br; };
    std::vector<Word> words;
    bool is_bold = false;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) words.push_back({cur, is_bold, false}); cur.clear(); };
    for (size_t i = 0; i < s.size();) {
        if (!s.compare(i, 3, "<b>")) { flush(); is_bold = true; i += 3; continue; }
        if (!s.compare(i, 4, "</b>")) { flush(); is_bold = false; i += 4; continue; }
        char c = s[i++];
        if (c == ' ') { flush(); words.push_back({" ", is_bold, false}); }
        else if (c == '\n') { flush(); words.push_back({"", false, true}); }
        else cur += c;
    }
    flush();
    float lh = px * line * S, cx = x, cy = y, space = text_width(f, px, " ");
    bool line_start = true;
    for (size_t i = 0; i < words.size(); ++i) {
        const Word &w = words[i];
        if (w.br) { cx = x; cy += lh; line_start = true; continue; }
        if (w.text == " ") { if (!line_start) cx += space; continue; }
        Font wf = w.bold ? bold : f;
        float ww = text_width(wf, px, w.text);
        if (!line_start && cx + ww > x + width) { cx = x; cy += lh; }
        if (draw) text(wf, px, cx, cy + (lh - px * 1.2f * S) / 2, w.bold ? bold_color : color, w.text);
        cx += ww;
        line_start = false;
    }
    return cy + lh - y;
}

std::string upper(const std::string &s) {
    std::string out;
    const char *p = s.c_str(), *end = p + s.size();
    while (p < end) {
        unsigned c = next_char(p, end);
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c >= 0x430 && c <= 0x44f) c -= 0x20;
        else if (c >= 0x450 && c <= 0x45f) c -= 0x50;
        char buf[5];
        int n = ImTextCharToUtf8(buf, c);
        out.append(buf, n);
    }
    return out;
}

// ---------------------------------------------------------------- pictures
static std::map<std::string, Picture *> g_pictures;

Picture *picture(const std::string &path) {
    auto it = g_pictures.find(path);
    if (it != g_pictures.end()) return it->second;
    Picture *pic = nullptr;
    if (SDL_Surface *raw = SDL_LoadBMP(path.c_str())) {
        if (SDL_Surface *s = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32)) {
            pic = new Picture;
            pic->w = s->w;
            pic->h = s->h;
            pic->color = SDL_CreateTextureFromSurface(g_renderer, s);
            // The grey copy, a little contrast added (as the page's grayscale + contrast filters).
            for (int y = 0; y < s->h; ++y) {
                Uint8 *row = (Uint8 *)s->pixels + y * s->pitch;
                for (int x = 0; x < s->w; ++x) {
                    Uint8 *px = row + x * 4;
                    float v = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
                    v = ImClamp((v - 128) * 1.1f + 128, 0.f, 255.f);
                    px[0] = px[1] = px[2] = (Uint8)v;
                }
            }
            pic->gray = SDL_CreateTextureFromSurface(g_renderer, s);
            SDL_SetTextureScaleMode(pic->color, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureScaleMode(pic->gray, SDL_SCALEMODE_LINEAR);
            SDL_DestroySurface(s);
        }
        SDL_DestroySurface(raw);
    }
    g_pictures[path] = pic;
    return pic;
}

void free_pictures() {
    for (auto &kv : g_pictures)
        if (kv.second) {
            SDL_DestroyTexture(kv.second->color);
            SDL_DestroyTexture(kv.second->gray);
            delete kv.second;
        }
    g_pictures.clear();
}

void draw_picture(Picture *p, ImRect box, float gray, float brightness, float alpha, float zoom, ImVec2 shift, float focus_y) {
    if (!p || box.GetWidth() <= 0 || box.GetHeight() <= 0) return;
    float A = box.GetWidth() / box.GetHeight(), a = (float)p->w / p->h;
    float uw = a > A ? A / a : 1, uh = a > A ? 1 : a / A;
    uw /= zoom;
    uh /= zoom;
    float cx = 0.5f - shift.x * uw, cy = (1 - uh) * focus_y + uh / 2 - shift.y * uh;
    cx = ImClamp(cx, uw / 2, 1 - uw / 2);
    cy = ImClamp(cy, uh / 2, 1 - uh / 2);
    ImVec2 uv0(cx - uw / 2, cy - uh / 2), uv1(cx + uw / 2, cy + uh / 2);
    ImVec4 tint(brightness, brightness, brightness, 1);
    if (gray > 0.001f) DL->AddImage((ImTextureID)(intptr_t)p->gray, box.Min, box.Max, uv0, uv1, col(tint, alpha));
    if (gray < 0.999f) DL->AddImage((ImTextureID)(intptr_t)p->color, box.Min, box.Max, uv0, uv1, col(tint, alpha * (gray > 0.001f ? 1 - gray : 1)));
}

void quad_h_gradient(ImTextureID tex, ImRect r, ImVec2 uv0, ImVec2 uv1, const float *stops, const float *alphas, int n, ImU32 tint) {
    DL->PushTexture(ImTextureRef(tex));
    DL->PrimReserve((n - 1) * 6, (n - 1) * 4);
    ImVec4 base = ImGui::ColorConvertU32ToFloat4(tint);
    for (int i = 0; i + 1 < n; ++i) {
        float x0 = r.Min.x + r.GetWidth() * stops[i], x1 = r.Min.x + r.GetWidth() * stops[i + 1];
        float u0 = uv0.x + (uv1.x - uv0.x) * stops[i], u1 = uv0.x + (uv1.x - uv0.x) * stops[i + 1];
        ImU32 c0 = ImGui::ColorConvertFloat4ToU32(ImVec4(base.x, base.y, base.z, base.w * alphas[i]));
        ImU32 c1 = ImGui::ColorConvertFloat4ToU32(ImVec4(base.x, base.y, base.z, base.w * alphas[i + 1]));
        ImDrawIdx idx = (ImDrawIdx)DL->_VtxCurrentIdx;
        DL->PrimWriteIdx(idx); DL->PrimWriteIdx(idx + 1); DL->PrimWriteIdx(idx + 2);
        DL->PrimWriteIdx(idx); DL->PrimWriteIdx(idx + 2); DL->PrimWriteIdx(idx + 3);
        DL->PrimWriteVtx(ImVec2(x0, r.Min.y), ImVec2(u0, uv0.y), c0);
        DL->PrimWriteVtx(ImVec2(x1, r.Min.y), ImVec2(u1, uv0.y), c1);
        DL->PrimWriteVtx(ImVec2(x1, r.Max.y), ImVec2(u1, uv1.y), c1);
        DL->PrimWriteVtx(ImVec2(x0, r.Max.y), ImVec2(u0, uv1.y), c0);
    }
    DL->PopTexture();
}

// ---------------------------------------------------------------- input
bool g_in_modal = false;
Hit hit(const std::string &id, ImRect r, bool enabled) {
    Hit h;
    if (!enabled || (g_block_input && !g_in_modal)) return h;
    ImGuiID gid = ImGui::GetID(id.c_str());
    if (!ImGui::ItemAdd(r, gid)) return h;
    h.pressed = ImGui::ButtonBehavior(r, gid, &h.hovered, &h.held, ImGuiButtonFlags_MouseButtonLeft);
    if (h.hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (h.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) h.right = true;
    return h;
}

// ---------------------------------------------------------------- texts
static Json g_texts;
bool ru() { return ST["lang"].str() == "ru"; }
const Json &tj(const char *key) {
    if (g_texts.is_null()) g_texts = Json::parse(kTexts);
    const Json &lang = g_texts[ru() ? "ru" : "en"];
    return lang.path(key);
}
std::string t(const char *key, std::initializer_list<std::pair<const char *, std::string>> vars) {
    const Json &j = tj(key);
    std::string s = j.kind == Json::String ? j.s : std::string(key);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '{') {
            size_t close = s.find('}', i);
            if (close != std::string::npos) {
                std::string name = s.substr(i + 1, close - i - 1);
                for (auto &v : vars)
                    if (name == v.first) { out += v.second; break; }
                i = close;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}
std::string plural(int n, const char *forms_key) {
    const Json &forms = tj(forms_key);
    if (!ru()) return forms[n == 1 ? 0 : 1].str();
    int a = abs(n) % 100, b = a % 10;
    return forms[a > 10 && a < 20 ? 2 : b == 1 ? 0 : b >= 2 && b <= 4 ? 1 : 2].str();
}
std::string count(int n, const char *forms_key) { return std::to_string(n) + " " + plural(n, forms_key); }

std::string fmt_date(long long sec) {
    static const char *ru_m[] = {"января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа", "сентября", "октября", "ноября", "декабря"};
    static const char *en_m[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    time_t tt = (time_t)sec;
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char buf[64];
    snprintf(buf, sizeof buf, "%d %s, %02d:%02d", tmv.tm_mday, (ru() ? ru_m : en_m)[tmv.tm_mon], tmv.tm_hour, tmv.tm_min);
    return buf;
}
std::string fmt_time(long long sec) {
    long long m = sec / 60, h = m / 60;
    return h ? t("hours", {{"h", std::to_string(h)}, {"m", std::to_string(m % 60)}}) : t("minutes", {{"m", std::to_string(m)}});
}
