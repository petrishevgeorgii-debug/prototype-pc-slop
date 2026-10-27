// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the port's settings as rows of the game's own options screens (theordergpu.h
// TheOrderGameMenu; scripts/proto_menu.py adds the rows, game/p1/options_menu.cpp answers the
// screens): Options > Video (the image: upscaling, sharpening), Options > Controller (the
// gamepad's three after the game's six) and the Display screen the port adds (resolution, frame
// rate, display mode, V-Sync, frame counter). The same settings as the port menu's
// (port_overlay.cpp), applied live, except the resolution and frame rate: the game is patched
// for them at start, so they apply on the next start, or at once with Apply > Restart now.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "../theordergpu.h"
#include "port_settings.h"
#include "port_trainer.h"

extern "C" void runtime_restart(void); // src/probe.c: starts run.py again, ends this process

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

enum Row { Upscaler, Quality, Sharpening, RowCount };
enum DisplayRow { Resolution, FrameRate, DisplayMode, VSync, FrameCounter, Apply, DisplayRowCount };
enum PadRow { Vibration, LightBar, MenuVibration, GameEffects, Haptics, SoundHaptics, PadRowCount };
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
    default: return "";
    }
}

int VideoChoices(int row) {
    switch (row) {
    case Upscaler: return int(Providers().size());
    case Quality: return 5;
    case Sharpening: return 11;
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
    default: break;
    }
}

// ---- Options > Controller: the gamepad (runtime_pad.c)

const char* PadLabel(int row) {
    switch (row) {
    case Vibration: return T("VIBRATION", "ВИБРАЦИЯ");
    case LightBar: return T("LIGHT BAR", "СВЕТОВАЯ ПАНЕЛЬ");
    case MenuVibration: return T("MENU VIBRATION", "ВИБРАЦИЯ В МЕНЮ");
    case GameEffects: return T("ACTION VIBRATION", "ВИБРАЦИЯ ДЕЙСТВИЙ");
    case Haptics: return T("HD HAPTICS", "HD-ВИБРАЦИЯ");
    case SoundHaptics: return T("SOUND HAPTICS", "ЗВУК В ВИБРАЦИИ");
    default: return "";
    }
}

int PadChoices(int row) {
    switch (row) {
    case Vibration: return kVibrationSteps + 1;
    case LightBar: return 2;
    case MenuVibration: return 3;
    case GameEffects: return 2;
    case Haptics: return 2;
    case SoundHaptics: return 4;
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
    case LightBar:
    case GameEffects:
    case Haptics: return OnOff(index);
    case SoundHaptics: {
        static const char* const en[] = {"OFF", "LOW", "MEDIUM", "HIGH"};
        static const char* const ru[] = {"ВЫКЛ", "СЛАБО", "СРЕДНЕ", "СИЛЬНО"};
        index = std::clamp(index, 0, 3);
        return Russian() ? ru[index] : en[index];
    }
    case MenuVibration: {
        static const char* const en[] = {"OFF", "PRESSES", "ALL"};
        static const char* const ru[] = {"ВЫКЛ", "НАЖАТИЯ", "ВСЕ"};
        index = std::clamp(index, 0, 2);
        return Russian() ? ru[index] : en[index];
    }
    default: return "";
    }
}

int PadGet(int row) {
    auto& s = PortSettings::Get();
    switch (row) {
    case Vibration: return std::clamp(int(std::lround(s.vibration.load() / 25.0)), 0, kVibrationSteps);
    case LightBar: return s.light_bar ? 1 : 0;
    case MenuVibration: return std::clamp(s.menu_vibration.load(), 0, 2);
    case GameEffects: return s.extra_vibration ? 1 : 0;
    case Haptics: return s.haptics ? 1 : 0;
    case SoundHaptics: return std::clamp(s.haptics_sound.load(), 0, 3);
    default: return 0;
    }
}

void PadSet(int row, int index) {
    auto& s = PortSettings::Get();
    switch (row) {
    case Vibration: s.vibration = std::clamp(index, 0, kVibrationSteps) * 25; break;
    case LightBar: s.light_bar = index != 0; break;
    case MenuVibration: s.menu_vibration = std::clamp(index, 0, 2); break;
    case GameEffects: s.extra_vibration = index != 0; break;
    case Haptics: s.haptics = index != 0; break;
    case SoundHaptics: s.haptics_sound = std::clamp(index, 0, 3); break;
    default: break;
    }
}

void PadReset(int row) {
    auto& s = PortSettings::Get();
    switch (row) {
    case Vibration: s.vibration = 100; break;
    case LightBar: s.light_bar = true; break;
    case MenuVibration: s.menu_vibration = 1; break;
    case GameEffects: s.extra_vibration = true; break;
    case Haptics: s.haptics = true; break;
    case SoundHaptics: s.haptics_sound = 2; break;
    default: break;
    }
}

// ---- the Display screen: resolution and frame rate (on the next start), the window

int ProtoRes() {
    const int r = PortSettings::Get().proto_res;
    return r >= 0 && r < PortSettings::ProtoResolutionCount ? r : 2; // 1920x1080
}

int ProtoFps60() {
    return PortSettings::Get().proto_fps60 != 0 ? 1 : 0; // unknown: the launcher's default, 60
}

const char* DisplayLabel(int row) {
    switch (row) {
    case Resolution: return T("RESOLUTION", "РАЗРЕШЕНИЕ");
    case FrameRate: return T("FRAME RATE", "ЧАСТОТА КАДРОВ");
    case DisplayMode: return T("DISPLAY MODE", "РЕЖИМ ЭКРАНА");
    case VSync: return T("V-SYNC", "ВЕРТ. СИНХРОНИЗАЦИЯ");
    case FrameCounter: return T("FRAME COUNTER", "СЧЁТЧИК КАДРОВ");
    case Apply: return T("APPLY RESOLUTION", "ПРИМЕНИТЬ");
    default: return "";
    }
}

int DisplayChoices(int row) {
    switch (row) {
    case Resolution: return PortSettings::ProtoResolutionCount;
    case FrameRate: return 2;
    case DisplayMode: return 3;
    case VSync: return 2;
    case FrameCounter: return 2;
    case Apply: return 2;
    default: return 0;
    }
}

const char* DisplayChoice(int row, int index) {
    switch (row) {
    case Resolution: return PortSettings::ProtoResolutions[std::clamp(index, 0, PortSettings::ProtoResolutionCount - 1)];
    case FrameRate: return index ? "60 FPS" : "30 FPS";
    case DisplayMode: {
        static const char* const en[] = {"WINDOWED", "BORDERLESS", "FULLSCREEN"};
        static const char* const ru[] = {"ОКНО", "БЕЗ РАМКИ", "ПОЛНЫЙ ЭКРАН"};
        index = std::clamp(index, 0, 2);
        return Russian() ? ru[index] : en[index];
    }
    case VSync:
    case FrameCounter: return OnOff(index);
    case Apply: return index ? T("RESTART NOW", "ПЕРЕЗАПУСК") : T("ON NEXT START", "ПРИ ЗАПУСКЕ");
    default: return "";
    }
}

int DisplayGet(int row) {
    auto& s = PortSettings::Get();
    switch (row) {
    case Resolution: return ProtoRes();
    case FrameRate: return ProtoFps60();
    case DisplayMode: return std::clamp(s.display_mode.load(), 0, 2);
    case VSync: return s.vsync ? 1 : 0;
    case FrameCounter: return s.show_fps ? 1 : 0;
    default: return 0;
    }
}

void DisplaySet(int row, int index) {
    auto& s = PortSettings::Get();
    switch (row) {
    case Resolution: s.proto_res = std::clamp(index, 0, PortSettings::ProtoResolutionCount - 1); break;
    case FrameRate: s.proto_fps60 = index ? 1 : 0; break;
    case DisplayMode: s.display_mode = std::clamp(index, 0, 2); break;
    case VSync: s.vsync = index != 0; break;
    case FrameCounter: s.show_fps = index != 0; break;
    case Apply:
        if (index) {
            // The game starts again through run.py, which patches it for the new resolution and
            // frame rate (theorder.ini). Unsaved progress is lost: the row says so.
            PortSettings::Save();
            std::printf("Menu: restarting for resolution %s, %d FPS\n", PortSettings::ProtoResolutions[ProtoRes()],
                        ProtoFps60() ? 60 : 30);
            std::fflush(stdout);
            runtime_restart();
        }
        break;
    default: break;
    }
}

void DisplayReset(int row) {
    auto& s = PortSettings::Get();
    switch (row) {
    case Resolution: s.proto_res = 2; break;
    case FrameRate: s.proto_fps60 = 1; break;
    case DisplayMode: s.display_mode = 1; break;
    case VSync: s.vsync = true; break;
    case FrameCounter: s.show_fps = false; break;
    default: break;
    }
}

// ---- the trainer (port_trainer.cpp, game/p1/trainer.cpp)

const char* TrainerLabel(int row) {
    switch (row) {
    case THEORDER_TRAINER_GOD: return T("GOD MODE", "БЕССМЕРТИЕ");
    case THEORDER_TRAINER_CRITICAL_MASS: return T("INFINITE CRITICAL MASS", "БЕСКОНЕЧНАЯ КРИТ. МАССА");
    case THEORDER_TRAINER_EP: return T("EVOLUTION POINTS", "ОЧКИ ЭВОЛЮЦИИ");
    case THEORDER_TRAINER_UNLOCK: return T("ALL POWERS AND UPGRADES", "ВСЕ СПОСОБНОСТИ");
    default: return "";
    }
}

const char* TrainerChoice(int row, int index) {
    switch (row) {
    case THEORDER_TRAINER_EP: return index ? T("+100,000 ADDED", "+100 000 ДОБАВЛЕНО") : T("ADD 100,000", "ДОБАВИТЬ 100 000");
    case THEORDER_TRAINER_UNLOCK: return index ? T("UNLOCKED", "ОТКРЫТО") : T("UNLOCK", "ОТКРЫТЬ");
    default: return OnOff(index);
    }
}

// ---- the pages

/// The Display screen exists (run.py: scripts/proto_menu.py made it, loose game files): Display
/// mode and V-Sync are on it. Without it they stay rows of the Video screen.
bool DisplayScreen() {
    static const bool on = [] {
        const char* env = std::getenv("THEORDER_GAME_MENU_DISPLAY");
        return env && std::strcmp(env, "1") == 0;
    }();
    return on;
}

/// A Video row past the screen's own: the Display screen's Display mode and V-Sync.
int VideoExtra(int row) {
    if (DisplayScreen() || row < RowCount || row >= RowCount + 2) return -1;
    return row == RowCount ? DisplayMode : VSync;
}

int Count(int page) {
    switch (page) {
    case THEORDER_MENU_VIDEO: return VideoCount() + (DisplayScreen() ? 0 : 2);
    case THEORDER_MENU_CONTROLLER: return PadRowCount;
    case THEORDER_MENU_DISPLAY: return DisplayRowCount;
    case THEORDER_MENU_TRAINER: return THEORDER_TRAINER_ROWS;
    default: return 0;
    }
}

const char* Label(int page, int row) {
    switch (page) {
    case THEORDER_MENU_VIDEO: return VideoExtra(row) >= 0 ? DisplayLabel(VideoExtra(row)) : VideoLabel(row);
    case THEORDER_MENU_CONTROLLER: return PadLabel(row);
    case THEORDER_MENU_DISPLAY: return DisplayLabel(row);
    case THEORDER_MENU_TRAINER: return TrainerLabel(row);
    default: return "";
    }
}

int Choices(int page, int row) {
    switch (page) {
    case THEORDER_MENU_VIDEO: return VideoExtra(row) >= 0 ? DisplayChoices(VideoExtra(row)) : VideoChoices(row);
    case THEORDER_MENU_CONTROLLER: return PadChoices(row);
    case THEORDER_MENU_DISPLAY: return DisplayChoices(row);
    case THEORDER_MENU_TRAINER: return 2;
    default: return 0;
    }
}

const char* Choice(int page, int row, int index) {
    switch (page) {
    case THEORDER_MENU_VIDEO:
        return VideoExtra(row) >= 0 ? DisplayChoice(VideoExtra(row), index) : VideoChoice(row, index);
    case THEORDER_MENU_CONTROLLER: return PadChoice(row, index);
    case THEORDER_MENU_DISPLAY: return DisplayChoice(row, index);
    case THEORDER_MENU_TRAINER: return TrainerChoice(row, index);
    default: return "";
    }
}

int Get(int page, int row) {
    switch (page) {
    case THEORDER_MENU_VIDEO: return VideoExtra(row) >= 0 ? DisplayGet(VideoExtra(row)) : VideoGet(row);
    case THEORDER_MENU_CONTROLLER: return PadGet(row);
    case THEORDER_MENU_DISPLAY: return DisplayGet(row);
    case THEORDER_MENU_TRAINER: return PortTrainer::Get(row);
    default: return 0;
    }
}

void Set(int page, int row, int index) {
    switch (page) {
    case THEORDER_MENU_VIDEO:
        if (VideoExtra(row) >= 0) DisplaySet(VideoExtra(row), index);
        else VideoSet(row, index);
        break;
    case THEORDER_MENU_CONTROLLER: PadSet(row, index); break;
    case THEORDER_MENU_DISPLAY: DisplaySet(row, index); break;
    case THEORDER_MENU_TRAINER: PortTrainer::Set(row, index); break;
    default: break;
    }
}

void Reset(int page, int row) {
    switch (page) {
    case THEORDER_MENU_VIDEO:
        if (VideoExtra(row) >= 0) DisplayReset(VideoExtra(row));
        else VideoReset(row);
        break;
    case THEORDER_MENU_CONTROLLER: PadReset(row); break;
    case THEORDER_MENU_DISPLAY: DisplayReset(row); break;
    case THEORDER_MENU_TRAINER: PortTrainer::Set(row, 0); break;
    default: break;
    }
}

void Save() {
    PortSettings::Save();
}

/// PROTOTYPE's own rumble switch: the PS3's Rumble row, hidden on the PS4, where the profile can
/// hold it off (then the game never asks the controller to vibrate).
const char* Linked(int page, int row) {
    return page == THEORDER_MENU_CONTROLLER && row == Vibration ? "Proto_Profile_GamepadRumbleEnabled" : nullptr;
}

/// The game's Quit buttons: the window closes as with its close button (theordergpu.cpp: the
/// window thread ends the process once the event is read).
void Quit() {
    PortSettings::Save();
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}

int TrainerOpen() {
    return PortTrainer::TakeRequests();
}

unsigned TrainerActions() {
    return PortTrainer::TakeActions();
}

void PressOptions() {
    PortTrainer::PressOptions();
}

const TheOrderGameMenu kMenu = {Count, Label, Choices, Choice, Get, Set, Reset, Save, Linked, Quit,
                                theordergpu_pointer_mode, theordergpu_pointer_take,
                                theordergpu_pointer_clicks, TrainerOpen, TrainerActions, PressOptions};

} // namespace

extern "C" const TheOrderGameMenu* theordergpu_game_menu(void) {
    return &kMenu;
}
