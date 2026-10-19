// SPDX-License-Identifier: GPL-2.0-or-later
#include "port_overlay.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <string>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <SDL3/SDL.h>
#include "port_settings.h"
#include "port_toggles.h"
#include "port_graphics.h"
#include "port_perf_stats.h"

extern "C" void runtime_restart(void); // src/probe.c: starts run.py again, ends this process
#include "common/elf_info.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global port_font_ttf\n"
    "port_font_ttf:\n"
    ".incbin \"" THEORDER_FONT_PATH "\"\n"
    ".global port_font_ttf_end\n"
    "port_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden port_font_ttf\n"
    ".global port_font_ttf\n"
    "port_font_ttf:\n"
    ".incbin \"" THEORDER_FONT_PATH "\"\n"
    ".hidden port_font_ttf_end\n"
    ".global port_font_ttf_end\n"
    "port_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char port_font_ttf[];
extern "C" const unsigned char port_font_ttf_end[];

// Tektur (SIL OFL 1.1), semi-condensed instance: the PROTOTYPE port menu's typeface, close to
// the game's own menus (Latin and Cyrillic).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global port_menu_font_ttf\n"
    "port_menu_font_ttf:\n"
    ".incbin \"" THEORDER_MENU_FONT_PATH "\"\n"
    ".global port_menu_font_ttf_end\n"
    "port_menu_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden port_menu_font_ttf\n"
    ".global port_menu_font_ttf\n"
    "port_menu_font_ttf:\n"
    ".incbin \"" THEORDER_MENU_FONT_PATH "\"\n"
    ".hidden port_menu_font_ttf_end\n"
    ".global port_menu_font_ttf_end\n"
    "port_menu_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char port_menu_font_ttf[];
extern "C" const unsigned char port_menu_font_ttf_end[];

namespace PortOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool r3_down = false, l2_down = false;
std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
bool dirty = false; // settings changed while open: saved on close
int requested_tab = -1; // switch on the next frame (L1/R1, Q/E, menu opened)
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::vector<std::string> choice_items;
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<PortPng::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
float base_scale = 1.0f;
ImFont* menu_font = nullptr; // Tektur (the PROTOTYPE menu)

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (value) {
        requested_tab = PortSettings::Get().menu_tab;
    }
    if (!value && dirty) {
        dirty = false;
        PortSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

// A combo over `count` labels storing an index or a value from `values`.
void Choice(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
            int count) {
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if ((values ? values[i] : i) == target.load()) current = i;
    }
    if (ImGui::Combo(label, &current, labels, count)) {
        Store(target, values ? values[current] : current, true);
    }
}

// Controller button names in the help text: PlayStation unless THEORDER_GLYPHS names xbox or
// nintendo. Read once: the environment is fixed for a run.
static const char* PadText(const char* playstation, const char* xbox, const char* nintendo) {
    static const char* style = std::getenv("THEORDER_GLYPHS");
    if (!style) return playstation;
    return !std::strcmp(style, "xbox") ? xbox : !std::strcmp(style, "nintendo") ? nintendo : playstation;
}

// The note for the item under the mouse or the gamepad/keyboard focus, shown in the footer.
const char* footer_help = nullptr;

void Help(const char* text) {
    if (text && (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused())) {
        footer_help = text;
    }
}

// Two-column rows: the name on the left, the control on the right.
bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.52f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    return true;
}

void Row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
}

void CheckRow(const char* label, std::atomic<bool>& value, const char* help = nullptr) {
    Row(label);
    Checkbox("##value", value);
    Help(help);
    ImGui::PopID();
}

void ChoiceRow(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
               int count, const char* help = nullptr) {
    Row(label);
    Choice("##value", target, labels, values, count);
    Help(help);
    ImGui::PopID();
}

void InfoRow(const char* label, const char* text) {
    Row(label);
    ImGui::TextDisabled("%s", text);
    ImGui::PopID();
}

void SliderRow(const char* label, std::atomic<int>& value, int lo, int hi, const char* format, const char* help) {
    Row(label);
    int v = value;
    if (ImGui::SliderInt("##value", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
        Store(value, std::clamp(v, lo, hi), true);
    }
    Help(help);
    ImGui::PopID();
}

const char* UpscalerLabel(int upscaler) {
    switch (upscaler) {
    case PortSettings::UpscalerFsr3: return "FSR 3.1";
    case PortSettings::UpscalerFsr4: return "FSR 4";
    case PortSettings::UpscalerFsr411: return "FSR 4.1.1";
    case PortSettings::UpscalerTaa: return "TAA";
    case PortSettings::UpscalerDlss: return "DLSS";
    default: return "";
    }
}

const Vulkan::Instance* overlay_instance = nullptr; // for the overlay's measurements

// The performance overlay's text with the current settings (also the menu's preview).
std::string OverlayText() {
    namespace P = PortPerfStats;
    const auto& s = PortSettings::Get();
    const unsigned items = s.overlay_items;
    if ((items & (P::Gpu | P::Cpu | P::Ram | P::Vram)) && overlay_instance) {
        P::Start(*overlay_instance);
    }
    P::Sample sample = P::Latest();
    sample.fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    sample.frame_ms = frame_ms_avg;
    sample.upscaler = UpscalerLabel(s.upscaler);
    return P::Format(sample, items, s.overlay_layout == 0);
}

void ImageTab() {
    namespace G = PortGraphics;
    auto& s = PortSettings::Get();
    ImGui::SeparatorText("Display");
    if (BeginRows("##display")) {
        ChoiceRow("Display mode", s.display_mode, G::DisplayModeNames.data(), nullptr, G::DisplayModeCount,
                  "Windowed, borderless window or exclusive fullscreen.");
        CheckRow("VSync", s.vsync, "Waits for the monitor: no tearing, a little more input delay.");
        SliderRow("Field of view", s.fov, 50, 200, "%d%%",
                  "How much of the scene the camera shows. Above 100% shows more on every side.");
        PortSettings::ApplyFov();
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Mouse");
    if (BeginRows("##mouse")) {
        CheckRow("Mouse look (F7)", s.mouse_look,
                 "The mouse turns the camera; left button fires (R2), right button aims (L2), "
                 "middle R3, side buttons L1/R1.");
        ImGui::BeginDisabled(!s.mouse_look);
        SliderRow("Mouse sensitivity", s.mouse_sensitivity, 10, 400, "%d%%",
                  "How fast the camera turns with the mouse.");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    const int iw = s.image_width, ih = s.image_height, ww = s.window_width, wh = s.window_height;
    const bool downscaling = iw > ww || ih > wh;
    const bool rcas_blocked = s.rcas && !s.rcas_applied;
    ImGui::SeparatorText("Applied on restart");
    if (BeginRows("##image_restart")) {
        ChoiceRow("Aspect ratio", s.aspect, G::AspectNames.data(), nullptr, int(G::AspectNames.size()),
                  "The shape of the game's picture. Pick your screen's to fill it without black bars: "
                  "the view keeps the original width and shows more above and below.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Sharpening");
    if (BeginRows("##scaling")) {
        ImGui::BeginDisabled(rcas_blocked);
        CheckRow("Sharpening (RCAS)", s.rcas,
                 rcas_blocked && downscaling ? "No effect: the game's image is larger than the window."
                 : rcas_blocked              ? "No effect: the game's image is smaller than the window."
                                             : "Sharpens the final image.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.rcas || rcas_blocked);
        SliderRow("Sharpening strength", s.rcas_strength, 0, 100, "%d%%", "How strong the sharpening is.");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Information");
    if (BeginRows("##image_info")) {
        char text[96];
        std::snprintf(text, sizeof(text), "%d x %d", iw, ih);
        InfoRow("Game image", text);
        std::snprintf(text, sizeof(text), "%d x %d", ww, wh);
        InfoRow("Window", text);
        InfoRow("Sharpening", s.rcas_applied ? "applied" : "not applied");
        ImGui::EndTable();
    }
}

void PerformanceTab() {
    namespace G = PortGraphics;
    auto& s = PortSettings::Get();
    ImGui::SeparatorText("Live");
    if (BeginRows("##live")) {
        static const char* const fps_labels[] = {"30", "60", "120", "240", "Unlimited"};
        ChoiceRow("Frame rate limit", s.fps_limit, fps_labels, G::FpsLimits.data(), int(G::FpsLimits.size()),
                  "Caps the frame rate. The engine frame rate (below) is the highest it can go.");
        // The Order presents every other vblank of its engine rate: 30 FPS at "60", 60 with the patch.
        const int effective = G::EffectiveFps(s.fps_limit, s.applied_engine_fps / 2);
        if (s.fps_limit == 0 || effective < s.fps_limit) {
            char text[48];
            std::snprintf(text, sizeof(text), "%d FPS (engine frame rate)", effective);
            InfoRow("Effective limit", text);
        }
        if (s.applied_engine_fps < s.startup_engine_fps) {
            InfoRow("Engine frame rate", "30 FPS: the 60 FPS patch is not available for this game build");
        }
        static const char* const queued_labels[] = {"1 (lowest latency)", "2", "Unlimited"};
        ChoiceRow("Frames queued", s.frames_queued, queued_labels, G::FramesQueued.data(),
                  int(G::FramesQueued.size()), "More frames queued: steadier FPS, slower response to input.");
        CheckRow("Compile new shaders in the background", s.async_shaders,
                 "Fewer stutters in new areas; an object or effect may appear a moment later.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Applied on restart");
    if (BeginRows("##restart")) {
        // Engine 60 is The Order's own 30 FPS; 120 asks for its 60 FPS patch.
        static const char* const engine_labels[] = {"30 (original)", "60 (experimental)"};
        ChoiceRow("Engine frame rate", s.engine_fps, engine_labels, G::EngineFps.data(), 2,
                  "The highest frame rate the game itself runs at (a game patch).");
        CheckRow("Game reads GPU data without waiting", s.stale_readback,
                 "Big FPS gain. Switch it off if lighting or objects look wrong.");
        CheckRow("Deferred GPU readbacks", s.deferred_readback,
                 "Fewer waits for the GPU. Switch it off if textures or shadows look wrong.");
        ImGui::EndTable();
    }
}

void OverlayTab() {
    namespace P = PortPerfStats;
    auto& s = PortSettings::Get();
    if (BeginRows("##overlay_on")) {
        CheckRow("Show overlay", s.show_fps, "The performance overlay in a corner of the screen.");
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!s.show_fps);
    ImGui::SeparatorText("What to show");
    if (ImGui::BeginTable("##overlay_items", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        static const char* const names[] = {"FPS", "Frametime", "Upscaler", "GPU (name and use)", "CPU (game)",
                                            "RAM (game / total)", "VRAM (used / available)"};
        for (unsigned item = 0; item < 7; ++item) {
            const unsigned bit = 1u << item;
            bool on = (s.overlay_items & bit) != 0;
            if (ImGui::Checkbox(names[item], &on)) {
                s.overlay_items = on ? (s.overlay_items | bit) : (s.overlay_items & ~bit);
                dirty = true;
            }
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Preview");
        ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
        const std::string text = OverlayText();
        ImGui::TextUnformatted(text.empty() ? "(nothing selected)" : text.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Appearance");
    if (BeginRows("##appearance")) {
        SliderRow("Font size", s.overlay_scale, 50, 300, "%d%%", "Size of the overlay text.");
        static const char* const corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        ChoiceRow("Corner", s.overlay_corner, corners, nullptr, 4, "Where the overlay sits.");
        SliderRow("Background opacity", s.overlay_opacity, 0, 100, "%d%%", "0% is no background.");
        static const char* const layouts[] = {"One line", "One item per line"};
        ChoiceRow("Layout", s.overlay_layout, layouts, nullptr, 2, "Items side by side or stacked.");
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText("About");
    if (BeginRows("##about")) {
        const auto& elf = Common::ElfInfo::Instance();
        char text[160];
        std::snprintf(text, sizeof(text), "%.*s v%.*s", int(elf.GameSerial().size()), elf.GameSerial().data(),
                      int(elf.AppVer().size()), elf.AppVer().data());
        InfoRow("Game", text);
        const u32 total = background_total;
        if (total) {
            std::snprintf(text, sizeof(text), "loading %u%%", u32(u64(background_done) * 100 / total));
        } else {
            std::snprintf(text, sizeof(text), "ready");
        }
        InfoRow("Shader cache", text);
        ImGui::EndTable();
    }
}

// Startup options changed since this launch (applied by "Apply and restart").
int RestartChanges() {
    const auto& s = PortSettings::Get();
    return (s.aspect != s.startup_aspect) + (s.engine_fps != s.startup_engine_fps) +
           (s.deferred_readback != s.startup_deferred_readback) + (s.stale_readback != s.startup_stale_readback);
}

void Menu() {
    auto& s = PortSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f * base_scale, viewport->WorkSize.x * 0.95f),
                                    std::min(680.0f * base_scale, viewport->WorkSize.y * 0.9f)),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    // port: the game's own title (param.sfo), not a fixed one.
    static const std::string heading = [] {
        const std::string_view title = Common::ElfInfo::Instance().Title();
        return std::string(title.empty() ? "The Order: 1886" : title) + PadText("  (Insert / R3+L2)", "  (Insert / RS+LT)", "  (Insert / RS+ZL)");
    }();
    if (!ImGui::Begin(heading.c_str(), &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    footer_help = nullptr;
    constexpr int TabCount = PortSettings::MenuTabCount;
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        const int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                         : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false)
                             ? -1
                             : 0;
        if (step) {
            requested_tab = (s.menu_tab + step + TabCount) % TabCount;
        }
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    if (ImGui::BeginTabBar("##tabs")) {
        static const char* const tabs[TabCount] = {"Image", "Performance", "Overlay"};
        for (int tab = 0; tab < TabCount; ++tab) {
            const ImGuiTabItemFlags flags = requested_tab == tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(tabs[tab], nullptr, flags)) {
                continue;
            }
            if (s.menu_tab != tab && requested_tab < 0) {
                s.menu_tab = tab;
                dirty = true;
            }
            if (ImGui::BeginChild("##body", ImVec2(0, -footer))) {
                switch (tab) {
                case 0: ImageTab(); break;
                case 1: PerformanceTab(); break;
                default: OverlayTab(); break;
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (requested_tab >= 0) {
            s.menu_tab = requested_tab;
            dirty = true;
            requested_tab = -1;
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", footer_help ? footer_help : PadText("L1 / R1 or Q / E: switch tabs. Settings are saved in theorder.ini.", "LB / RB or Q / E: switch tabs. Settings are saved in theorder.ini.", "L / R or Q / E: switch tabs. Settings are saved in theorder.ini."));
    ImGui::PopTextWrapPos();
    if (const int changes = RestartChanges()) {
        ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%d change%s need%s a restart (unsaved progress is lost).",
                           changes, changes == 1 ? "" : "s", changes == 1 ? "s" : "");
        ImGui::SameLine();
        ImGui::BeginDisabled(!std::getenv("THEORDER_RESTART_COMMAND"));
        if (ImGui::Button("Apply and restart")) {
            // The new launch confirms the options once the game shows; run.py restores the
            // previous ones if it never does (restart_unconfirmed).
            s.restart_unconfirmed = true;
            PortSettings::Save();
            runtime_restart();
        }
        ImGui::EndDisabled();
    } else {
        ImGui::NewLine();
    }
    const float close_width = ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - close_width);
    if (ImGui::Button("Close")) {
        keep_open = false;
    }
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("%s", PadText("Controller: Cross (A) = OK, Circle (B) = cancel", "Controller: A = OK, B = cancel", "Controller: B = OK, A = cancel"));
    ImGui::End();
}

// PROTOTYPE port: the save/load list as a full-screen dialog in the game's colors (charcoal,
// blood red accent), each save a row with its picture, title, location and date.
namespace Theme {
constexpr ImU32 Dim = IM_COL32(0, 0, 0, 170);
constexpr ImU32 Panel = IM_COL32(12, 12, 14, 246);
constexpr ImU32 Border = IM_COL32(255, 255, 255, 22);
constexpr ImU32 Red = IM_COL32(200, 20, 28, 255);
constexpr ImU32 RedSoft = IM_COL32(200, 20, 28, 34);
constexpr ImU32 Text = IM_COL32(236, 231, 226, 255);
constexpr ImU32 Muted = IM_COL32(150, 146, 142, 255);
constexpr ImU32 Faint = IM_COL32(95, 92, 89, 255);
constexpr ImU32 Line = IM_COL32(255, 255, 255, 14);
constexpr ImU32 Slot = IM_COL32(26, 26, 29, 255);
} // namespace Theme

// A key hint: a small framed key label followed by its action.
float KeyHint(ImDrawList* dl, ImVec2 pos, const char* key, const char* action, float size, bool measure_only = false) {
    ImFont* font = ImGui::GetFont();
    const ImVec2 ks = font->CalcTextSizeA(size * 0.82f, FLT_MAX, 0.0f, key);
    const ImVec2 as = font->CalcTextSizeA(size, FLT_MAX, 0.0f, action);
    const float pad = size * 0.35f, gap = size * 0.45f;
    const float width = ks.x + pad * 2 + gap + as.x;
    if (!measure_only) {
        const ImVec2 kmin(pos.x, pos.y), kmax(pos.x + ks.x + pad * 2, pos.y + size * 1.25f);
        dl->AddRect(kmin, kmax, Theme::Muted, 2.0f, 0, 1.0f);
        dl->AddText(font, size * 0.82f, ImVec2(kmin.x + pad, kmin.y + (kmax.y - kmin.y - ks.y) * 0.5f), Theme::Text, key);
        dl->AddText(font, size, ImVec2(kmax.x + gap, pos.y + (size * 1.25f - as.y) * 0.5f), Theme::Muted, action);
    }
    return width;
}

std::string Upper(std::string text) {
    for (char& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

void ChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float s = base_scale;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    // The game behind dims; the dialog sits in the middle.
    bg->AddRectFilled(viewport->WorkPos, ImVec2(viewport->WorkPos.x + viewport->WorkSize.x,
                                                viewport->WorkPos.y + viewport->WorkSize.y), Theme::Dim);
    const ImVec2 picture(228.0f * 0.78f * s, 128.0f * 0.78f * s);
    const float row_h = picture.y + 22.0f * s, pad = 28.0f * s;
    const float width = std::min(860.0f * s, viewport->WorkSize.x * 0.9f);
    const float list_h = std::min(row_h * float(std::max<size_t>(choice_items.size(), 1)),
                                  viewport->WorkSize.y * 0.62f);
    const float header_h = 92.0f * s, footer_h = 64.0f * s;
    const ImVec2 size(width, header_h + list_h + footer_h);
    const ImVec2 origin(viewport->WorkPos.x + (viewport->WorkSize.x - size.x) * 0.5f,
                        viewport->WorkPos.y + (viewport->WorkSize.y - size.y) * 0.5f);
    ImGui::SetNextWindowPos(origin, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    dl->AddRectFilled(origin, end, Theme::Panel);
    dl->AddRect(origin, end, Theme::Border);
    dl->AddRectFilled(origin, ImVec2(origin.x + 4.0f * s, origin.y + header_h - 18.0f * s), Theme::Red);
    // Header: a small red label above the title.
    dl->AddText(font, fs * 0.8f, ImVec2(origin.x + pad, origin.y + 20.0f * s), Theme::Red, "SAVE DATA");
    dl->AddText(font, fs * 1.55f, ImVec2(origin.x + pad, origin.y + 38.0f * s), Theme::Text,
                Upper(choice_title).c_str());
    char count[48];
    const int saves = int(std::count_if(choice_items.begin(), choice_items.end(),
                                        [](const std::string& t) { return t != "New save"; }));
    std::snprintf(count, sizeof(count), "%d SAVE%s", saves, saves == 1 ? "" : "S");
    const ImVec2 cs = font->CalcTextSizeA(fs * 0.72f, FLT_MAX, 0.0f, count);
    dl->AddText(font, fs * 0.72f, ImVec2(end.x - pad - cs.x, origin.y + 26.0f * s), Theme::Faint, count);
    dl->AddLine(ImVec2(origin.x + pad, origin.y + header_h - 6.0f * s),
                ImVec2(end.x - pad, origin.y + header_h - 6.0f * s), Theme::Line);
    // The list scrolls inside its own region; the selected row stays in view.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + header_h));
    ImGui::BeginChild("##choice_list", ImVec2(size.x, list_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoBackground);
    ImDrawList* ld = ImGui::GetWindowDrawList();
    for (int i = 0; i < int(choice_items.size()); ++i) {
        const bool selected = i == choice_index;
        const ImVec2 rmin = ImGui::GetCursorScreenPos();
        const ImVec2 rmax(rmin.x + size.x, rmin.y + row_h);
        if (selected) {
            ld->AddRectFilled(ImVec2(rmin.x + pad * 0.5f, rmin.y), ImVec2(rmax.x - pad * 0.5f, rmax.y), Theme::RedSoft);
            ld->AddRectFilled(ImVec2(rmin.x + pad * 0.5f, rmin.y), ImVec2(rmin.x + pad * 0.5f + 3.0f * s, rmax.y), Theme::Red);
        }
        const ImVec2 pmin(rmin.x + pad, rmin.y + (row_h - picture.y) * 0.5f);
        const ImVec2 pmax(pmin.x + picture.x, pmin.y + picture.y);
        ImTextureData* tex = i < int(choice_textures.size()) ? choice_textures[i] : nullptr;
        if (tex) {
            ld->AddImage(tex->GetTexRef(), pmin, pmax);
        } else {
            ld->AddRectFilled(pmin, pmax, Theme::Slot);
            const float c = 9.0f * s;
            const ImVec2 mid((pmin.x + pmax.x) * 0.5f, (pmin.y + pmax.y) * 0.5f);
            ld->AddLine(ImVec2(mid.x - c, mid.y), ImVec2(mid.x + c, mid.y), selected ? Theme::Red : Theme::Faint, 2.0f * s);
            ld->AddLine(ImVec2(mid.x, mid.y - c), ImVec2(mid.x, mid.y + c), selected ? Theme::Red : Theme::Faint, 2.0f * s);
        }
        ld->AddRect(pmin, pmax, selected ? Theme::Red : Theme::Border, 0.0f, 0, selected ? 2.0f * s : 1.0f);
        // Title, location and date (the save's lines).
        const std::string& item = choice_items[i];
        std::vector<std::string> lines;
        for (size_t at = 0;;) {
            const size_t nl = item.find('\n', at);
            lines.push_back(item.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
            if (nl == std::string::npos) break;
            at = nl + 1;
        }
        const float tx = pmax.x + 22.0f * s;
        const float clip = rmax.x - pad;
        float ty = pmin.y + 4.0f * s;
        if (lines.size() == 1) {
            ty = (pmin.y + pmax.y) * 0.5f - fs * 0.6f;
        }
        for (size_t l = 0; l < lines.size(); ++l) {
            const bool first = l == 0;
            const float size_l = first ? fs * 1.12f : fs * 0.86f;
            const ImU32 color = first ? (selected ? Theme::Text : IM_COL32(205, 200, 195, 255))
                                      : (l + 1 == lines.size() && lines.size() > 1 ? Theme::Faint : Theme::Muted);
            const std::string text = first ? Upper(lines[l]) : lines[l];
            ld->AddText(font, size_l, ImVec2(tx, ty), color, text.c_str(), nullptr, 0.0f,
                        nullptr);
            ty += size_l + 6.0f * s;
            (void)clip;
        }
        if (i + 1 < int(choice_items.size()) && !selected && i + 1 != choice_index) {
            ld->AddLine(ImVec2(rmin.x + pad, rmax.y), ImVec2(rmax.x - pad, rmax.y), Theme::Line);
        }
        ImGui::Dummy(ImVec2(size.x, row_h));
        if (selected) {
            ImGui::SetScrollHereY(0.5f);
        }
    }
    ImGui::EndChild();
    // Footer: key hints, right-aligned.
    const float fy = end.y - footer_h + 20.0f * s;
    dl->AddLine(ImVec2(origin.x + pad, end.y - footer_h + 4.0f * s), ImVec2(end.x - pad, end.y - footer_h + 4.0f * s),
                Theme::Line);
    const char* ok = PadText("ENTER / CROSS", "ENTER / A", "ENTER / B");
    const char* back = PadText("ESC / CIRCLE", "ESC / B", "ESC / A");
    const float hs = fs * 0.92f;
    const float w1 = KeyHint(dl, ImVec2(), "UP / DOWN", "Select", hs, true);
    const float w2 = KeyHint(dl, ImVec2(), ok, "Confirm", hs, true);
    const float w3 = KeyHint(dl, ImVec2(), back, "Back", hs, true);
    const float gap = 26.0f * s;
    float x = end.x - pad - (w1 + w2 + w3 + gap * 2);
    x += KeyHint(dl, ImVec2(x, fy), "UP / DOWN", "Select", hs) + gap;
    x += KeyHint(dl, ImVec2(x, fy), ok, "Confirm", hs) + gap;
    KeyHint(dl, ImVec2(x, fy), back, "Back", hs);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

// ---------------------------------------------------------------------------------------------
// PROTOTYPE: the port menu drawn like the game's own options screens (black and blood red,
// orange labels, the selected row on a dark red bar, value bars of eleven cells, the frame
// lines and markers, button hints at the bottom). Keyboard, controller and mouse.

bool PrototypeMenuEnabled() {
    static const bool on = [] {
        const char* game = std::getenv("THEORDER_GAME");
        return game && std::strcmp(game, "prototype") == 0;
    }();
    return on;
}

namespace Proto {
constexpr ImU32 Red = IM_COL32(224, 18, 26, 255);
constexpr ImU32 RedDim = IM_COL32(110, 10, 14, 255);
constexpr ImU32 RedLine = IM_COL32(150, 12, 18, 200);
constexpr ImU32 RedFaint = IM_COL32(120, 8, 12, 110);
constexpr ImU32 Orange = IM_COL32(255, 150, 54, 255);
constexpr ImU32 OrangeGlow = IM_COL32(255, 90, 20, 46);
constexpr ImU32 Cream = IM_COL32(255, 236, 214, 255);
constexpr ImU32 Bar = IM_COL32(92, 0, 2, 235);
constexpr ImU32 Cell = IM_COL32(176, 14, 22, 255);
constexpr ImU32 CellOff = IM_COL32(44, 4, 6, 255);
constexpr ImU32 Marker = IM_COL32(230, 120, 30, 255);

bool russian() {
    static const bool ru = [] {
        const char* lang = std::getenv("THEORDER_LANGUAGE");
        return lang && std::strcmp(lang, "8") == 0;
    }();
    return ru;
}
const char* T(const char* en, const char* ru) {
    return russian() ? ru : en;
}

// Text with the game's soft orange/red glow.
void GlowText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, const char* text,
              ImU32 glow = OrangeGlow) {
    const float r = size * 0.06f;
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * 0.7853982f;
        dl->AddText(font, size, ImVec2(pos.x + std::cos(a) * r, pos.y + std::sin(a) * r), glow, text);
    }
    dl->AddText(font, size, pos, color, text);
}

// A controller button glyph (circle with its symbol) as in the game's hints.
void ButtonGlyph(ImDrawList* dl, ImVec2 c, float r, int symbol, ImU32 color) {
    dl->AddCircle(c, r, color, 32, r * 0.14f);
    const float s = r * 0.5f, t = r * 0.13f;
    switch (symbol) {
    case 0: // cross
        dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), color, t);
        dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), color, t);
        break;
    case 1: // circle
        dl->AddCircle(c, s, color, 24, t);
        break;
    case 2: // triangle
        dl->AddTriangle(ImVec2(c.x, c.y - s), ImVec2(c.x + s * 0.95f, c.y + s * 0.7f),
                        ImVec2(c.x - s * 0.95f, c.y + s * 0.7f), color, t);
        break;
    default: // square
        dl->AddRect(ImVec2(c.x - s * 0.8f, c.y - s * 0.8f), ImVec2(c.x + s * 0.8f, c.y + s * 0.8f), color, 0.0f, 0, t);
        break;
    }
}

struct Option {
    const char* label;
    const char* help;
    int kind;                       // 0 choice, 1 bar (11 cells), 2 info
    std::vector<std::string> values; // choice texts
    int value;                      // current index (bar: 0..10)
    bool enabled = true;
};

int category = 0;
int row = 0;
} // namespace Proto

void PrototypeMenu() {
    using namespace Proto;
    auto& s = PortSettings::Get();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float W = vp->WorkSize.x, H = vp->WorkSize.y;
    const ImVec2 o = vp->WorkPos;
    ImFont* font = menu_font ? menu_font : ImGui::GetFont();
    auto X = [&](float f) { return o.x + W * f; };
    auto Y = [&](float f) { return o.y + H * f; };

    // ---- the options of each category
    static const char* const categories_en[] = {"UPSCALING", "IMAGE", "DISPLAY", "FSR TESTS"};
    static const char* const categories_ru[] = {"АПСКЕЙЛИНГ", "ИЗОБРАЖЕНИЕ", "ЭКРАН", "ТЕСТЫ FSR"};
    constexpr int CategoryCount = 4;
    auto toggle_on = [](u64 bit) {
        return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
    };
    const bool fsr4 = s.fsr4_supported, fsr411 = s.fsr411_supported;
    std::vector<int> providers = {PortSettings::UpscalerOff, PortSettings::UpscalerFsr3};
    if (fsr4) providers.push_back(PortSettings::UpscalerFsr4);
    if (fsr411) providers.push_back(PortSettings::UpscalerFsr411);
    auto provider_name = [&](int u) -> std::string {
        switch (u) {
        case PortSettings::UpscalerFsr3: return "FSR 3.1";
        case PortSettings::UpscalerFsr4: return "FSR 4";
        case PortSettings::UpscalerFsr411: return "FSR 4.1";
        default: return T("OFF", "ВЫКЛ");
        }
    };
    const std::string on = T("ON", "ВКЛ"), off = T("OFF", "ВЫКЛ");
    std::vector<Option> options;
    switch (category) {
    case 0: {
        Option up{T("UPSCALER", "АПСКЕЙЛЕР"),
                  fsr411 ? T("Temporal anti-aliasing and reconstruction by AMD FidelityFX Super Resolution. "
                             "FSR 4 and 4.1 need a Radeon RX 9000 series GPU.",
                             "Временное сглаживание и реконструкция AMD FidelityFX Super Resolution. FSR 4 "
                             "и 4.1 требуют видеокарту Radeon RX 9000.")
                         : T("Temporal anti-aliasing and reconstruction by AMD FidelityFX Super Resolution. "
                             "FSR 4 needs a Radeon RX 9000 GPU; FSR 4.1 is hidden: your driver lacks the "
                             "extension it needs (so far only Linux Mesa drivers have it).",
                             "Временное сглаживание и реконструкция AMD FidelityFX Super Resolution. FSR 4 "
                             "требует Radeon RX 9000; FSR 4.1 скрыт: драйвер не поддерживает нужное "
                             "расширение (пока оно есть только в драйверах Mesa для Linux)."),
                  0, {}, 0};
        for (size_t i = 0; i < providers.size(); ++i) {
            up.values.push_back(provider_name(providers[i]));
            if (providers[i] == s.upscaler) up.value = int(i);
        }
        options.push_back(up);
        const bool active = s.upscaler != PortSettings::UpscalerOff;
        options.push_back({T("QUALITY", "КАЧЕСТВО"),
                           T("Native AA renders the scene at the launcher's render resolution; Quality, "
                             "Balanced, Performance and Ultra Performance render it at 1/1.5, 1/1.7, 1/2 "
                             "and 1/3 of that and FSR rebuilds it (faster, softer). The HUD stays sharp.",
                             "Native AA рисует сцену в разрешении из лаунчера; Качество, Баланс, "
                             "Производительность и Ультра рисуют её в 1/1.5, 1/1.7, 1/2 и 1/3 от него, а FSR "
                             "восстанавливает (быстрее, мягче). Интерфейс остаётся чётким."),
                           0,
                           {"NATIVE AA", T("QUALITY", "КАЧЕСТВО"), T("BALANCED", "БАЛАНС"),
                            T("PERFORMANCE", "ПРОИЗВОДИТ."), T("ULTRA PERFORMANCE", "УЛЬТРА")},
                           std::clamp(s.preset.load(), 0, 4), active});
        options.push_back({T("FSR SHARPENING", "РЕЗКОСТЬ FSR"),
                           T("FSR's own contrast-adaptive sharpening.", "Встроенное повышение резкости FSR."),
                           0, {off, on}, s.sharpen ? 1 : 0, active});
        options.push_back({T("SHARPNESS", "СИЛА РЕЗКОСТИ"),
                           T("How strong FSR's sharpening is.", "Насколько сильно FSR повышает резкость."),
                           1, {}, std::clamp(int(std::lround(s.sharpness.load() * 5.0f)), 0, 10),
                           active && s.sharpen});
        options.push_back({T("MOTION VECTORS VIEW", "ВЕКТОРЫ ДВИЖЕНИЯ"),
                           T("Debug view: the camera motion FSR receives (red/green = horizontal/vertical).",
                             "Отладка: движение камеры, которое получает FSR (красный/зелёный)."),
                           0, {off, on}, s.debug_view == PortSettings::DebugMotion ? 1 : 0, active});
        const char* problem = s.fsr4_problem;
        char status[160];
        if (problem && PortSettings::IsFsr4(s.upscaler)) {
            std::snprintf(status, sizeof(status), "%s", problem);
        } else if (active) {
            std::snprintf(status, sizeof(status), "%s  %d × %d", provider_name(s.upscaler).c_str(),
                          s.active_render_width.load(), s.active_render_height.load());
        } else {
            std::snprintf(status, sizeof(status), "%s", T("OFF", "ВЫКЛ"));
        }
        options.push_back({T("STATUS", "СОСТОЯНИЕ"), "", 2, {status}, 0, true});
        break;
    }
    case 1:
        options.push_back({T("FSR 1 UPSCALING", "FSR 1"),
                           T("Scales the game image to the window with FSR 1 when the window is larger.",
                             "Масштабирует изображение игры до окна с помощью FSR 1, если окно больше."),
                           0, {off, on}, s.fsr1 ? 1 : 0});
        options.push_back({T("FINAL SHARPENING", "ИТОГОВАЯ РЕЗКОСТЬ"),
                           T("Contrast-adaptive sharpening (RCAS) of the final image.",
                             "Повышение резкости итогового изображения (RCAS)."),
                           0, {off, on}, s.rcas ? 1 : 0});
        options.push_back({T("SHARPENING STRENGTH", "СИЛА РЕЗКОСТИ"),
                           T("How strong the final sharpening is.", "Насколько сильно повышается резкость."),
                           1, {}, std::clamp(s.rcas_strength.load() / 10, 0, 10), s.rcas.load()});
        break;
    case 3:
        options.push_back({T("JITTER", "ДРОЖАНИЕ КАДРА"),
                           T("FSR shifts the scene by a fraction of a pixel every frame to gather detail. "
                             "Off: no shimmer from it, but less smoothing. For finding the cause of shimmer.",
                             "FSR сдвигает сцену на доли пикселя каждый кадр. Выкл: без мерцания от этого, "
                             "но меньше сглаживания. Для поиска причины мерцания."),
                           0, {off, on}, s.jitter ? 1 : 0});
        options.push_back({T("JITTER SIGN", "ЗНАК ДРОЖАНИЯ"),
                           T("Tells FSR the shift in the other direction. The right one gives a calm image.",
                             "Сообщает FSR сдвиг в другую сторону. Правильный даёт спокойную картинку."),
                           0, {T("NORMAL", "ОБЫЧНЫЙ"), T("INVERTED", "ОБРАТНЫЙ")},
                           toggle_on(1ull << 26) ? 1 : 0});
        options.push_back({T("VERTICAL MOTION", "ВЕРТИКАЛЬНОЕ ДВИЖЕНИЕ"),
                           T("Flips the up/down direction of the camera motion given to FSR. The right one "
                             "keeps the image calm while you look up and down.",
                             "Меняет направление вертикального движения камеры для FSR. Правильное "
                             "оставляет картинку спокойной, когда смотришь вверх и вниз."),
                           0, {T("NORMAL", "ОБЫЧНОЕ"), T("FLIPPED", "ОБРАТНОЕ")},
                           toggle_on(1ull << 28) ? 1 : 0});
        options.push_back({T("MOTION VECTORS VIEW", "ВЕКТОРЫ ДВИЖЕНИЯ"),
                           T("Debug view: dark when nothing moves, red/green for horizontal/vertical motion.",
                             "Отладка: темно, если ничего не движется; красный/зелёный — движение."),
                           0, {off, on}, s.debug_view == PortSettings::DebugMotion ? 1 : 0});
        break;
    default:
        options.push_back({T("DISPLAY MODE", "РЕЖИМ ЭКРАНА"),
                           T("Windowed, borderless window or exclusive fullscreen.",
                             "Окно, окно без рамки или полный экран."),
                           0, {T("WINDOWED", "ОКНО"), T("BORDERLESS", "БЕЗ РАМКИ"), T("FULLSCREEN", "ПОЛНЫЙ ЭКРАН")},
                           std::clamp(s.display_mode.load(), 0, 2)});
        options.push_back({T("V-SYNC", "ВЕРТ. СИНХРОНИЗАЦИЯ"),
                           T("Waits for the monitor: no tearing.", "Ждёт монитор: без разрывов кадра."),
                           0, {off, on}, s.vsync ? 1 : 0});
        options.push_back({T("FRAME COUNTER", "СЧЁТЧИК КАДРОВ"),
                           T("Frame rate in a corner of the screen.", "Частота кадров в углу экрана."),
                           0, {off, on}, s.show_fps ? 1 : 0});
        break;
    }
    const int count = int(options.size());
    row = std::clamp(row, 0, count - 1);

    // ---- input
    auto apply = [&](int index, int value) {
        Option& opt = options[index];
        if (!opt.enabled || opt.kind == 2) return;
        if (opt.kind == 0) value = (value % int(opt.values.size()) + int(opt.values.size())) % int(opt.values.size());
        else value = std::clamp(value, 0, 10);
        dirty = true;
        switch (category * 16 + index) {
        case 0: s.upscaler = providers[value]; break;
        case 1: s.preset = value; break;
        case 2: s.sharpen = value == 1; break;
        case 3: s.sharpness = float(value) / 5.0f; break;
        case 4: s.debug_view = value ? PortSettings::DebugMotion : PortSettings::DebugNone; break;
        case 16: s.fsr1 = value == 1; break;
        case 17: s.rcas = value == 1; break;
        case 18: s.rcas_strength = value * 10; break;
        case 32: s.display_mode = value; break;
        case 33: s.vsync = value == 1; break;
        case 34: s.show_fps = value == 1; break;
        case 48: s.jitter = value == 1; break;
        case 49:
        case 50: {
            const u64 bit = index == 1 ? (1ull << 26) : (1ull << 28);
            if (value) __atomic_fetch_or(&runtime_disabled_optimizations, bit, __ATOMIC_RELAXED);
            else __atomic_fetch_and(&runtime_disabled_optimizations, ~bit, __ATOMIC_RELAXED);
            break;
        }
        case 51: s.debug_view = value ? PortSettings::DebugMotion : PortSettings::DebugNone; break;
        default: break;
        }
    };
    auto pressed = [](ImGuiKey a, ImGuiKey b = ImGuiKey_None) {
        return ImGui::IsKeyPressed(a, true) || (b != ImGuiKey_None && ImGui::IsKeyPressed(b, true));
    };
    if (pressed(ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp)) {
        for (int i = 1; i <= count; ++i) {
            const int r = (row - i + count) % count;
            if (options[r].kind != 2) { row = r; break; }
        }
    }
    if (pressed(ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown)) {
        for (int i = 1; i <= count; ++i) {
            const int r = (row + i) % count;
            if (options[r].kind != 2) { row = r; break; }
        }
    }
    if (pressed(ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft)) apply(row, options[row].value - 1);
    if (pressed(ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight) || pressed(ImGuiKey_Enter, ImGuiKey_GamepadFaceDown))
        apply(row, options[row].value + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
        category = (category + CategoryCount - 1) % CategoryCount;
        row = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_E, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Tab, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) {
        category = (category + 1) % CategoryCount;
        row = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
        // Defaults of this category.
        if (category == 0) { s.upscaler = PortSettings::UpscalerOff; s.preset = 0; s.sharpen = true; s.sharpness = 0.4f; s.debug_view = PortSettings::DebugNone; }
        if (category == 1) { s.fsr1 = false; s.rcas = true; s.rcas_strength = 60; }
        if (category == 2) { s.display_mode = 1; s.vsync = true; s.show_fps = false; }
        if (category == 3) {
            s.jitter = true;
            s.debug_view = PortSettings::DebugNone;
            __atomic_fetch_and(&runtime_disabled_optimizations, ~((1ull << 26) | (1ull << 28)),
                               __ATOMIC_RELAXED);
        }
        dirty = true;
    }
    bool close = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);

    // ---- frame: dim the game, the screen's lines, markers and side decoration
    ImGui::SetNextWindowPos(o);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##proto_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), IM_COL32(0, 0, 0, 228));
    // A dull red haze behind the options.
    dl->AddRectFilledMultiColor(ImVec2(X(0.31f), Y(0.45f)), ImVec2(X(0.80f), Y(0.89f)), IM_COL32(60, 0, 0, 0),
                                IM_COL32(60, 0, 0, 0), IM_COL32(90, 4, 6, 120), IM_COL32(90, 4, 6, 120));
    const float lw = std::max(1.0f, H / 900.0f);
    for (const float y : {0.043f, 0.055f, 0.092f, 0.889f, 0.929f, 0.94f}) {
        dl->AddLine(ImVec2(o.x, Y(y)), ImVec2(o.x + W, Y(y)), y == 0.092f || y == 0.889f ? RedLine : RedFaint, lw);
    }
    const float m = H * 0.005f;
    for (const float x : {0.078f, 0.378f, 0.73f, 0.95f}) {
        dl->AddTriangleFilled(ImVec2(X(x) - m, Y(0.046f)), ImVec2(X(x) + m, Y(0.046f)), ImVec2(X(x), Y(0.046f) + m), Marker);
        dl->AddTriangleFilled(ImVec2(X(x) - m, Y(0.937f)), ImVec2(X(x) + m, Y(0.937f)), ImVec2(X(x), Y(0.937f) - m), Marker);
    }
    // Left rail: the bent line, its scale numbers, the hatched strip.
    const ImVec2 rail[] = {{X(0.079f), Y(0.095f)}, {X(0.079f), Y(0.265f)}, {X(0.092f), Y(0.29f)},
                           {X(0.092f), Y(0.51f)},  {X(0.085f), Y(0.545f)}, {X(0.085f), Y(0.889f)}};
    dl->AddPolyline(rail, 6, RedLine, 0, lw * 1.6f);
    const float small = H * 0.0115f;
    for (int i = 0; i < 6; ++i) {
        char n[8];
        std::snprintf(n, sizeof(n), "%d", 103 + i);
        dl->AddText(font, small, ImVec2(X(0.06f), Y(0.128f + 0.139f * float(i))), RedDim, n);
    }
    for (float y = Y(0.6f); y < Y(0.885f); y += H * 0.011f) {
        dl->AddLine(ImVec2(X(0.099f), y + H * 0.009f), ImVec2(X(0.109f), y), RedFaint, lw * 2.0f);
    }
    // Static "data" dashes in the left column.
    static std::vector<ImVec4> dashes = [] {
        std::vector<ImVec4> out;
        u32 seed = 0x50524f54;
        auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / 16777216.0f; };
        for (int i = 0; i < 160; ++i) {
            out.push_back(ImVec4(0.19f + rnd() * 0.12f, 0.12f + rnd() * 0.76f, 0.005f + rnd() * 0.04f, rnd()));
        }
        return out;
    }();
    for (const auto& d : dashes) {
        dl->AddLine(ImVec2(X(d.x), Y(d.y)), ImVec2(X(d.x + d.z), Y(d.y)), IM_COL32(150, 10, 16, int(30 + 70 * d.w)),
                    lw * 1.5f);
    }
    // The options panel's frame.
    dl->AddLine(ImVec2(X(0.3125f), Y(0.093f)), ImVec2(X(0.3125f), Y(0.889f)), RedLine, lw * 1.5f);
    dl->AddLine(ImVec2(X(0.796f), Y(0.093f)), ImVec2(X(0.796f), Y(0.889f)), RedLine, lw * 1.5f);

    // ---- the category title and list
    const char* const* names = russian() ? categories_ru : categories_en;
    GlowText(dl, font, H * 0.036f, ImVec2(X(0.085f), Y(0.222f)), Red, names[category], IM_COL32(255, 20, 20, 40));
    for (int c = 0; c < CategoryCount; ++c) {
        const bool sel = c == category;
        const std::string text = sel ? std::string("[") + names[c] + "]" : std::string(names[c]);
        const ImVec2 p(X(0.098f), Y(0.307f + 0.041f * float(c)));
        const float size = H * 0.03f;
        if (sel) GlowText(dl, font, size, p, Orange, text.c_str());
        else dl->AddText(font, size, p, RedDim, text.c_str());
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
        if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + ts.x, p.y + ts.y)) && ImGui::IsMouseClicked(0)) {
            category = c;
            row = 0;
        }
    }

    // ---- the option rows
    const float row_h = H * 0.0444f, text_size = H * 0.0285f;
    const float px0 = X(0.3135f), px1 = X(0.7955f);
    const float label_x = X(0.33f), value_cx = X(0.684f);
    float y = Y(0.315f);
    const char* help = "";
    for (int i = 0; i < count; ++i) {
        const Option& opt = options[i];
        const bool sel = i == row && opt.kind != 2;
        const ImVec2 rmin(px0, y), rmax(px1, y + row_h * 0.92f);
        const bool hover = ImGui::IsMouseHoveringRect(rmin, rmax) && opt.kind != 2;
        if (hover && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) row = i;
        if (sel) {
            dl->AddRectFilled(rmin, rmax, Bar);
            help = opt.help;
        }
        const float ty = y + (row_h * 0.92f - text_size) * 0.5f;
        const ImU32 label_color = !opt.enabled ? RedDim : sel ? Cream : Orange;
        if (opt.enabled && !sel) GlowText(dl, font, text_size, ImVec2(label_x, ty), label_color, opt.label);
        else dl->AddText(font, text_size, ImVec2(label_x, ty), label_color, opt.label);
        const ImU32 value_color = !opt.enabled ? RedDim : sel ? Cream : Orange;
        if (opt.kind == 1) {
            const float cw = H * 0.0105f, gap = H * 0.0092f, ch = row_h * 0.62f;
            const float total = 11 * cw + 10 * gap;
            float cx = value_cx - total * 0.5f;
            const float cy = y + (row_h * 0.92f - ch) * 0.5f;
            for (int c = 0; c < 11; ++c) {
                const ImU32 color = !opt.enabled ? CellOff : c == opt.value ? Cream : c < opt.value ? Cell : CellOff;
                dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + cw, cy + ch), color, cw * 0.3f);
                cx += cw + gap;
            }
        } else {
            const std::string& text = opt.values.empty() ? std::string() : opt.values[std::clamp(opt.value, 0, int(opt.values.size()) - 1)];
            const float vsize = opt.kind == 2 ? text_size * 0.8f : text_size;
            const ImVec2 ts = font->CalcTextSizeA(vsize, FLT_MAX, 0.0f, text.c_str());
            const ImVec2 vp2(value_cx - ts.x * 0.5f, y + (row_h * 0.92f - ts.y) * 0.5f);
            if (opt.enabled && !sel && opt.kind != 2) GlowText(dl, font, vsize, vp2, value_color, text.c_str());
            else dl->AddText(font, vsize, vp2, opt.kind == 2 ? RedDim : value_color, text.c_str());
        }
        // Arrows on the selected row (the mouse clicks them).
        if (sel && opt.enabled && opt.kind != 2) {
            const float a = row_h * 0.22f, cy = y + row_h * 0.46f;
            const float lx = X(0.597f), rx = X(0.772f);
            dl->AddTriangleFilled(ImVec2(lx - a * 0.6f, cy), ImVec2(lx + a * 0.6f, cy - a), ImVec2(lx + a * 0.6f, cy + a), Red);
            dl->AddTriangleFilled(ImVec2(rx + a * 0.6f, cy), ImVec2(rx - a * 0.6f, cy - a), ImVec2(rx - a * 0.6f, cy + a), Red);
            if (ImGui::IsMouseClicked(0)) {
                const ImVec2 mp = ImGui::GetIO().MousePos;
                if (mp.y >= rmin.y && mp.y <= rmax.y) {
                    if (std::abs(mp.x - lx) < a * 2.0f) apply(i, opt.value - 1);
                    else if (std::abs(mp.x - rx) < a * 2.0f || (mp.x > label_x && mp.x < lx - a * 2.0f)) apply(i, opt.value + 1);
                }
            }
        }
        y += row_h;
    }
    // The selected option's description, under the rows.
    if (help && *help) {
        dl->AddText(font, H * 0.019f, ImVec2(label_x, Y(0.80f)), IM_COL32(190, 60, 40, 255), help, nullptr,
                    px1 - label_x - W * 0.02f);
    }

    // ---- button hints
    const float hy = Y(0.909f), gr = H * 0.017f, hs = H * 0.028f;
    auto hint = [&](float x, int symbol, const char* text, const char* key) -> bool {
        ButtonGlyph(dl, ImVec2(x, hy), gr, symbol, Red);
        const ImVec2 tp(x + gr * 1.5f, hy - hs * 0.55f);
        dl->AddText(font, hs, tp, Red, text);
        const ImVec2 ts = font->CalcTextSizeA(hs, FLT_MAX, 0.0f, text);
        dl->AddText(font, hs * 0.55f, ImVec2(tp.x + ts.x + gr * 0.6f, hy - hs * 0.2f), RedDim, key);
        return ImGui::IsMouseHoveringRect(ImVec2(x - gr, hy - gr), ImVec2(tp.x + ts.x, hy + gr)) && ImGui::IsMouseClicked(0);
    };
    if (hint(X(0.09f), 1, T("BACK", "НАЗАД"), "ESC")) close = true;
    if (hint(X(0.338f), 2, T("DEFAULTS", "ПО УМОЛЧАНИЮ"), "BACKSPACE")) {
        if (category == 0) { s.upscaler = PortSettings::UpscalerOff; s.preset = 0; s.sharpen = true; s.sharpness = 0.4f; s.debug_view = PortSettings::DebugNone; }
        if (category == 1) { s.fsr1 = false; s.rcas = true; s.rcas_strength = 60; }
        if (category == 2) { s.display_mode = 1; s.vsync = true; s.show_fps = false; }
        if (category == 3) {
            s.jitter = true;
            s.debug_view = PortSettings::DebugNone;
            __atomic_fetch_and(&runtime_disabled_optimizations, ~((1ull << 26) | (1ull << 28)),
                               __ATOMIC_RELAXED);
        }
        dirty = true;
    }
    if (hint(X(0.588f), 3, T("SECTION", "РАЗДЕЛ"), "TAB  ·  Q / E  ·  L1 / R1")) {
        category = (category + 1) % CategoryCount;
        row = 0;
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (close) {
        SetOpen(false);
    }
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(total ? "Loading shader cache..." : "Preparing graphics...");
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total,
                      u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::TextDisabled("The first start after an update takes longer.");
    ImGui::End();
}

void FpsCounter() {
    const auto& s = PortSettings::Get();
    const std::string text = OverlayText();
    if (text.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const int corner = s.overlay_corner;
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    ImGui::SetNextWindowPos(ImVec2(right ? viewport->WorkPos.x + viewport->WorkSize.x - pad : viewport->WorkPos.x + pad,
                                   bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(s.overlay_opacity / 100.0f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Loading shader cache %u%%", u32(u64(done) * 100 / total));
    ImGui::End();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "port";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(port_font_ttf),
                                   int(port_font_ttf_end - port_font_ttf), 18.0f, &font_config);
    ImFontConfig menu_config;
    menu_config.FontDataOwnedByAtlas = false;
    menu_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(port_menu_font_ttf),
                                               int(port_menu_font_ttf_end - port_menu_font_ttf), 32.0f,
                                               &menu_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // Font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 128;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    overlay_instance = &instance;
    initialized = true;
    std::printf("Overlay: menu ready (Insert or R3+L2)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const PortPng::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        finish = button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : button == SDL_GAMEPAD_BUTTON_EAST ? 2 : 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
    return true;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (choice_active && !menu_open) {
        return HandleChoiceEvent(event);
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        static const bool capture_key = [] {
            const char* env = std::getenv("THEORDER_FRAME_CAPTURE_KEY");
            return env && *env == '1';
        }();
        if (down && !event.key.repeat && capture_key && event.key.key == SDLK_F11) {
            Vulkan::FrameCapture::Request();
            return true;
        }
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        // R3+L2 opens the menu: L3+R3 is left to the game.
        if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
            if (down && l2_down) {
                SetOpen(!is_open);
                return true;
            }
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            const bool down = event.gaxis.value > 16000;
            if (down && !l2_down && r3_down) {
                l2_down = true;
                SetOpen(!is_open);
                return true;
            }
            l2_down = down;
        }
        return false;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

// After "Apply and restart", the new options are kept once the game has shown about ten
// seconds of frames after loading (run.py restores the previous ones otherwise).
void ConfirmRestart() {
    static u32 frames = 0;
    auto& s = PortSettings::Get();
    if (!s.restart_unconfirmed || loading || ++frames < 600) {
        return;
    }
    s.restart_unconfirmed = false;
    PortSettings::Save();
    std::puts("Settings: graphics options confirmed after restart");
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

bool Visible() {
    const auto& s = PortSettings::Get();
    return initialized && (loading || background_total || menu_open || text_entry_active || choice_active ||
                           choice_retiring || s.show_fps);
}

bool CapturesInput() {
    return menu_open || text_entry_active || choice_active;
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<PortPng::Image> pictures, int focus) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_title = title;
    choice_items = items;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    return choice_result;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    ConfirmRestart();
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateChoiceTextures();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (loading) {
        LoadingScreen();
    }
    if (menu_open) {
        if (PrototypeMenuEnabled()) {
            PrototypeMenu();
        } else {
            Menu();
        }
    }
    if (PortSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (background_total && !loading && !menu_open) {
        BackgroundLoading();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active && !menu_open) {
        ChoiceBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace PortOverlay
