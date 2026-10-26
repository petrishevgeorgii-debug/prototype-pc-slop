// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the port's settings as rows of the game's own Options > Video and Options >
// Controller screens (theordergpu.h TheOrderGameMenu; scripts/proto_menu.py adds the rows,
// game/p1/options_menu.cpp answers the screens). The same settings as the port menu's
// (port_overlay.cpp), applied live; the port menu keeps the rest (FSR 1, the frame counter, FSR
// tests, the shader browser). The Video screen has room for six rows, Brightness and five of
// these: the pause menu's cuts off the seventh. The Controller screen gets the gamepad's two
// after the game's six (Target toggle last), as many as the PC version's screen had there.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../theordergpu.h"
#include "port_settings.h"

namespace {

bool Russian() {
    static const bool ru = [] {
        const char* lang = std::getenv("THEORDER_LANGUAGE");
        return lang && std::strcmp(lang, "8") == 0;
    }();
    return ru;
}

const char* T(const char* en, const char* ru) {
    return Russian() ? ru : en;
}

enum Row { Upscaler, Quality, Sharpening, DisplayMode, VSync, RowCount };
enum PadRow { Vibration, LightBar, PadRowCount };
constexpr int kVibrationSteps = 8; ///< 0 off, then 25% steps up to 200% (the port menu's)

/// The upscalers this PC can use, in menu order.
std::vector<int> Providers() {
    auto& s = PortSettings::Get();
    std::vector<int> list = {PortSettings::UpscalerOff, PortSettings::UpscalerFsr3};
    if (s.fsr4_supported) list.push_back(PortSettings::UpscalerFsr4);
    if (s.fsr411_supported) list.push_back(PortSettings::UpscalerFsr411);
    return list;
}

const char* OnOff(int index) {
    return index ? T("ON", "ВКЛ") : T("OFF", "ВЫКЛ");
}

// ---- Options > Video

int VideoCount() {
    return RowCount;
}

const char* VideoLabel(int row) {
    switch (row) {
    case Upscaler: return T("UPSCALING", "АПСКЕЙЛИНГ");
    case Quality: return T("UPSCALING QUALITY", "КАЧЕСТВО АПСКЕЙЛА");
    case Sharpening: return T("SHARPENING", "РЕЗКОСТЬ");
    case DisplayMode: return T("DISPLAY MODE", "РЕЖИМ ЭКРАНА");
    case VSync: return T("V-SYNC", "ВЕРТ. СИНХРОНИЗАЦИЯ");
    default: return "";
    }
}

int VideoChoices(int row) {
    switch (row) {
    case Upscaler: return int(Providers().size());
    case Quality: return 5;
    case Sharpening: return 11;
    case DisplayMode: return 3;
    case VSync: return 2;
    default: return 0;
    }
}

const char* VideoChoice(int row, int index) {
    static std::string text;
    switch (row) {
    case Upscaler: {
        const auto list = Providers();
        if (index < 0 || index >= int(list.size())) return "";
        switch (list[index]) {
        case PortSettings::UpscalerFsr3: return "FSR 3.1";
        case PortSettings::UpscalerFsr4: return "FSR 4";
        case PortSettings::UpscalerFsr411: return "FSR 4.1";
        default: return T("OFF", "ВЫКЛ");
        }
    }
    case Quality: {
        static const char* const en[] = {"NATIVE AA", "QUALITY", "BALANCED", "PERFORMANCE", "ULTRA PERFORMANCE"};
        static const char* const ru[] = {"NATIVE AA", "КАЧЕСТВО", "БАЛАНС", "ПРОИЗВОДИТЕЛЬНОСТЬ", "УЛЬТРА"};
        index = std::clamp(index, 0, 4);
        return Russian() ? ru[index] : en[index];
    }
    case Sharpening:
        if (index <= 0) return T("OFF", "ВЫКЛ");
        text = std::to_string(std::min(index, 10) * 10) + "%";
        return text.c_str();
    case DisplayMode: {
        static const char* const en[] = {"WINDOWED", "BORDERLESS", "FULLSCREEN"};
        static const char* const ru[] = {"ОКНО", "БЕЗ РАМКИ", "ПОЛНЫЙ ЭКРАН"};
        index = std::clamp(index, 0, 2);
        return Russian() ? ru[index] : en[index];
    }
    case VSync: return OnOff(index);
    default: return "";
    }
}

int VideoGet(int row) {
    auto& s = PortSettings::Get();
    switch (row) {
    case Upscaler: {
        const auto list = Providers();
        const auto at = std::find(list.begin(), list.end(), s.upscaler.load());
        return at == list.end() ? 0 : int(at - list.begin());
    }
    case Quality: return std::clamp(s.preset.load(), 0, 4);
    case Sharpening: return s.rcas ? std::clamp(int(std::lround(s.rcas_strength.load() / 10.0)), 1, 10) : 0;
    case DisplayMode: return std::clamp(s.display_mode.load(), 0, 2);
    case VSync: return s.vsync ? 1 : 0;
    default: return 0;
    }
}

void VideoSet(int row, int index) {
    auto& s = PortSettings::Get();
    switch (row) {
    case Upscaler: {
        const auto list = Providers();
        s.upscaler = list[std::clamp(index, 0, int(list.size()) - 1)];
        break;
    }
    case Quality: s.preset = std::clamp(index, 0, 4); break;
    case Sharpening:
        s.rcas = index > 0;
        if (index > 0) s.rcas_strength = std::min(index, 10) * 10;
        break;
    case DisplayMode: s.display_mode = std::clamp(index, 0, 2); break;
    case VSync: s.vsync = index != 0; break;
    default: break;
    }
}

/// The port menu's defaults (its Backspace / Triangle).
void VideoReset(int row) {
    auto& s = PortSettings::Get();
    switch (row) {
    case Upscaler: s.upscaler = PortSettings::UpscalerOff; break;
    case Quality: s.preset = 0; break;
    case Sharpening: s.rcas = true; s.rcas_strength = 60; break;
    case DisplayMode: s.display_mode = 1; break;
    case VSync: s.vsync = true; break;
    default: break;
    }
}

// ---- Options > Controller: the gamepad (runtime_pad.c)

const char* PadLabel(int row) {
    switch (row) {
    case Vibration: return T("VIBRATION", "ВИБРАЦИЯ");
    case LightBar: return T("LIGHT BAR", "СВЕТОВАЯ ПАНЕЛЬ");
    default: return "";
    }
}

int PadChoices(int row) {
    switch (row) {
    case Vibration: return kVibrationSteps + 1;
    case LightBar: return 2;
    default: return 0;
    }
}

const char* PadChoice(int row, int index) {
    static std::string text;
    switch (row) {
    case Vibration:
        if (index <= 0) return T("OFF", "ВЫКЛ");
        text = std::to_string(std::min(index, kVibrationSteps) * 25) + "%";
        return text.c_str();
    case LightBar: return OnOff(index);
    default: return "";
    }
}

int PadGet(int row) {
    auto& s = PortSettings::Get();
    switch (row) {
    case Vibration: return std::clamp(int(std::lround(s.vibration.load() / 25.0)), 0, kVibrationSteps);
    case LightBar: return s.light_bar ? 1 : 0;
    default: return 0;
    }
}

void PadSet(int row, int index) {
    auto& s = PortSettings::Get();
    switch (row) {
    case Vibration: s.vibration = std::clamp(index, 0, kVibrationSteps) * 25; break;
    case LightBar: s.light_bar = index != 0; break;
    default: break;
    }
}

void PadReset(int row) {
    auto& s = PortSettings::Get();
    switch (row) {
    case Vibration: s.vibration = 100; break;
    case LightBar: s.light_bar = true; break;
    default: break;
    }
}

// ---- the pages

int Count(int page) {
    return page == THEORDER_MENU_VIDEO ? VideoCount() : page == THEORDER_MENU_CONTROLLER ? PadRowCount : 0;
}

const char* Label(int page, int row) {
    return page == THEORDER_MENU_VIDEO ? VideoLabel(row) : page == THEORDER_MENU_CONTROLLER ? PadLabel(row) : "";
}

int Choices(int page, int row) {
    return page == THEORDER_MENU_VIDEO ? VideoChoices(row) : page == THEORDER_MENU_CONTROLLER ? PadChoices(row) : 0;
}

const char* Choice(int page, int row, int index) {
    return page == THEORDER_MENU_VIDEO ? VideoChoice(row, index)
         : page == THEORDER_MENU_CONTROLLER ? PadChoice(row, index) : "";
}

int Get(int page, int row) {
    return page == THEORDER_MENU_VIDEO ? VideoGet(row) : page == THEORDER_MENU_CONTROLLER ? PadGet(row) : 0;
}

void Set(int page, int row, int index) {
    if (page == THEORDER_MENU_VIDEO) VideoSet(row, index);
    else if (page == THEORDER_MENU_CONTROLLER) PadSet(row, index);
}

void Reset(int page, int row) {
    if (page == THEORDER_MENU_VIDEO) VideoReset(row);
    else if (page == THEORDER_MENU_CONTROLLER) PadReset(row);
}

void Save() {
    PortSettings::Save();
}

const TheOrderGameMenu kMenu = {Count, Label, Choices, Choice, Get, Set, Reset, Save};

} // namespace

extern "C" const TheOrderGameMenu* theordergpu_game_menu(void) {
    return &kMenu;
}
