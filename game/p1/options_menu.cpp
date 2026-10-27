// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the port's settings in the game's own Options > Video and Options > Controller
// screens (docs/NATIVE.md).
//
// The game's menus are Scaleform movies. Their ActionScript reaches the game's code through
// ExternalInterface.call("PlaceLuaCall", function, arguments...) (commonregisters.DoCallBack):
// the movie's ExternalInterface handler (0x1caf0) sees the method name and hands PlaceLuaCall to
// 0x1cd90, which calls the Lua function of that name and returns its result to the movie.
//
// scripts/proto_menu.py rewrites the Video screens (protovideooption.gfx, protopausevideo.gfx)
// and the Controller screens (protocontrolleroption.gfx, protopausecontroller.gfx) so that after
// Brightness / Target toggle they ask for the port's rows of their page (0 Video, 1 Controller; the
// Display screen it makes asks for page 2):
// port_Count(page), port_Label(page, row), port_Choices(page, row), port_Choice(page, row, index),
// and add a game toggle per row whose game database name is "PortSetting<page>_<row>". Here the
// hand-over to 0x1cd90 (a tail call) goes through native code that answers those, and the
// screens' db_GetInt / db_SetInt / fe_RestoreGameDBDefault for PortSetting names, from the port's
// settings (gpu/shim/port_game_menu.cpp). Everything else goes to the game's Lua as before.
// Without the rewritten screens nothing asks for these names. The menus' sounds pass through here
// too: they also tap the controller (MenuSound). And the story mission Discord shows is read here
// (Presence): the game database's Proto_Save_NextStoryMission, as the pause menu reads it.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "game/guest.h"
#include "game/native_registry.h"

namespace {

/// The game's Scaleform value (GFx 2.x GFxValue): a type and its value.
struct GfxValue {
    enum : std::uint32_t { Undefined = 0, Null = 1, Boolean = 2, Number = 3, String = 4 };
    std::uint32_t type;
    std::uint32_t pad;
    union {
        double number;
        const char* string;
        std::uint8_t boolean;
        std::uint64_t raw;
    };
};
static_assert(sizeof(GfxValue) == 16, "the game's argument array has 16-byte values");

constexpr std::uint64_t kHandOver = 0x1cbf2; ///< jmp PlaceLuaCall (0x1cd90) in the ExternalInterface handler
std::uint64_t g_place_lua_call = 0;           ///< 0x1cd90

int Int(const GfxValue* args, int argc, int i) {
    if (i >= argc) return 0;
    if (args[i].type == GfxValue::Number) return static_cast<int>(args[i].number);
    if (args[i].type == GfxValue::Boolean) return args[i].boolean ? 1 : 0;
    if (args[i].type == GfxValue::String && args[i].string) return std::atoi(args[i].string);
    return 0;
}

const char* Text(const GfxValue* args, int argc, int i) {
    return i < argc && args[i].type == GfxValue::String ? args[i].string : nullptr;
}

void ReturnNumber(GfxValue* ret, double value) {
    ret->type = GfxValue::Number;
    ret->number = value;
}

void ReturnText(GfxValue* ret, const char* text) {
    // The movie copies the text when the call returns: a small ring of buffers is enough.
    static char buffers[8][128];
    static unsigned next = 0;
    char* buffer = buffers[next++ % 8];
    std::snprintf(buffer, sizeof(buffers[0]), "%s", text ? text : "");
    ret->type = GfxValue::String;
    ret->string = buffer;
}

struct Setting {
    int page = -1, row = -1;
};

bool Digits(const char*& at, int& value) {
    if (*at < '0' || *at > '9') return false;
    value = 0;
    while (*at >= '0' && *at <= '9' && value < 1000) value = value * 10 + (*at++ - '0');
    return true;
}

/// The page and row of a "PortSetting<page>_<row>" database name (page -1: not one).
Setting Parse(const char* name, const TheOrderGameMenu* menu) {
    Setting s;
    if (!name || std::strncmp(name, "PortSetting", 11) != 0) return s;
    const char* at = name + 11;
    int page = 0, row = 0;
    if (!Digits(at, page) || *at++ != '_' || !Digits(at, row) || *at) return s;
    if (page >= THEORDER_MENU_PAGES || row >= menu->count(page)) return s;
    s.page = page;
    s.row = row;
    return s;
}

/// Calls the game's PlaceLuaCall with `args`; the result.
GfxValue GameCall(void* handler, GfxValue* args, int argc) {
    GfxValue ret{};
    ret.type = GfxValue::Undefined;
    Guest::Call<void>(g_place_lua_call, handler, args, argc, &ret);
    return ret;
}

GfxValue String(const char* text) {
    GfxValue v{};
    v.type = GfxValue::String;
    v.string = text;
    return v;
}

GfxValue Number(double number) {
    GfxValue v{};
    v.type = GfxValue::Number;
    v.number = number;
    return v;
}

/// Keeps the game's own setting a row is linked to (TheOrderGameMenu::linked) on while the row is
/// not 0, and off when it is: `force` sets it either way, otherwise only an off one is turned on.
/// `profile` and `index` are the screen's own arguments, passed on as they are.
void Link(void* handler, const char* name, bool on, bool force, const GfxValue& profile, const GfxValue& index) {
    if (!name) return;
    if (!force) {
        if (!on) return;
        GfxValue get[] = {String("db_GetInt"), String(name), profile, index};
        const GfxValue now = GameCall(handler, get, 4);
        if (now.type != GfxValue::Number || now.number != 0) return;
    }
    GfxValue set[] = {String("db_SetInt"), String(name), profile, Number(on ? 1 : 0), index};
    GameCall(handler, set, 5);
    std::printf("Menu: the game's %s set to %d\n", name, on ? 1 : 0);
    std::fflush(stdout);
}

const GfxValue& Arg(const GfxValue* args, int argc, int i) {
    static const GfxValue none = Number(0);
    return i < argc ? args[i] : none;
}

/// The port's answer to a PlaceLuaCall, or false for the game's Lua.
bool Answer(void* handler, const GfxValue* args, int argc, GfxValue* ret) {
    const TheOrderGameMenu* menu = Native::Menu();
    const char* function = Text(args, argc, 0);
    if (!menu || !function || !ret) return false;
    // The mouse in the menus (menu_mouse.cpp): protomain's cursor asks every frame, with the
    // screen on top (IGHUD while playing: the mouse looks around; any other, the main menu, the
    // pause menu and the screens under them, loading: it points); the answer shows or hides it.
    if (!std::strcmp(function, "port_Pointer")) {
        const char* screen = Text(args, argc, 1);
        int up;
        if (screen) {
            up = *screen && std::strcmp(screen, "IGHUD") && std::strcmp(screen, "NONE") ? 1 : 0;
            static char last[64] = "?";
            if (std::strncmp(last, screen, sizeof(last) - 1)) {
                std::snprintf(last, sizeof(last), "%s", screen);
                std::printf("Menu mouse: screen %s\n", *screen ? screen : "(none)");
                std::fflush(stdout);
            }
        } else {
            up = Int(args, argc, 1) ? 1 : 0; // an older protomain: whether the menus are up
        }
        const int shown = menu->pointer_mode ? menu->pointer_mode(up) : 0;
        ReturnNumber(ret, shown);
        // The cursor's frame: 2 ('enable') while a control is under it (the controls' OnRollOver),
        // 1 ('disable') otherwise; and where it is on the stage. Logged as it changes.
        static int over = -1;
        const int frame = Int(args, argc, 2);
        if (shown && argc >= 5 && frame && (frame == 2) != (over == 1)) {
            over = frame == 2 ? 1 : 0;
            std::printf("Menu mouse: %s at %d,%d on the stage\n", over ? "over a control" : "off the controls",
                        Int(args, argc, 3), Int(args, argc, 4));
            std::fflush(stdout);
        }
        return true;
    }
    if (std::strncmp(function, "port_", 5) == 0) {
        const int page = Int(args, argc, 1);
        const int count = page >= 0 && page < THEORDER_MENU_PAGES ? menu->count(page) : 0;
        const int row = Int(args, argc, 2);
        const bool valid = row >= 0 && row < count;
        static bool logged[THEORDER_MENU_PAGES] = {};
        if (!std::strcmp(function, "port_Count") && count && !logged[page]) {
            logged[page] = true;
            static const char* const names[] = {"Video", "Controller", "Display", "Trainer"};
            std::printf("Menu: the %s screen shows the port's %d settings\n", page < 4 ? names[page] : "?", count);
            std::fflush(stdout);
        }
        if (!std::strcmp(function, "port_Count")) ReturnNumber(ret, count);
        else if (!std::strcmp(function, "port_Label")) ReturnText(ret, valid ? menu->label(page, row) : "");
        else if (!std::strcmp(function, "port_Choices")) ReturnNumber(ret, valid ? menu->choices(page, row) : 0);
        else if (!std::strcmp(function, "port_Choice"))
            ReturnText(ret, valid ? menu->choice(page, row, Int(args, argc, 3)) : "");
        else ret->type = GfxValue::Undefined;
        return true;
    }
    // Quit to the desktop: the main menu's Quit button and the pause menu's Quit > Quit to desktop
    // (the PC version's, shown on the PS4 by scripts/proto_menu.py). The PS4 game has no such call.
    if (!std::strcmp(function, "fe_ExitGame")) {
        if (!menu->quit) return false;
        std::printf("Menu: quit to the desktop\n");
        std::fflush(stdout);
        menu->quit();
        ret->type = GfxValue::Null;
        return true;
    }
    // db_GetInt(name, profile, index), db_SetInt(name, profile, value, index),
    // fe_RestoreGameDBDefault(name, profile)
    const Setting setting = Parse(Text(args, argc, 1), menu);
    if (setting.page < 0) return false;
    const int page = setting.page, row = setting.row;
    const char* linked = menu->linked ? menu->linked(page, row) : nullptr;
    if (!std::strcmp(function, "db_GetInt")) {
        const int value = menu->get(page, row);
        Link(handler, linked, value != 0, false, Arg(args, argc, 2), Arg(args, argc, 3));
        ReturnNumber(ret, value);
    } else if (!std::strcmp(function, "db_SetInt")) {
        menu->set(page, row, Int(args, argc, 3));
        menu->save();
        static const char* const pages[] = {"Video", "Controller", "Display", "Trainer"};
        std::printf("Menu: %s > %s set to %s\n", page < 4 ? pages[page] : "?", menu->label(page, row),
                    menu->choice(page, row, menu->get(page, row)));
        std::fflush(stdout);
        Link(handler, linked, menu->get(page, row) != 0, true, Arg(args, argc, 2), Arg(args, argc, 4));
        ret->type = GfxValue::Null;
    } else if (!std::strcmp(function, "fe_RestoreGameDBDefault")) {
        menu->reset(page, row);
        menu->save();
        Link(handler, linked, menu->get(page, row) != 0, true, Arg(args, argc, 2), Number(0));
        ret->type = GfxValue::Null;
    } else {
        return false; // another database call for these names: the game's own answer
    }
    return true;
}

bool Has(const char* text, const char* part) {
    return std::strstr(text, part) != nullptr;
}

/// The menus' sounds (protomain's soundmanager: PlaySound(id) is DoCallBack("snd_PlayPatchOverlapped",
/// patch), a few movies call snd_PlayPatch(patch, "play")) also tap the controller: a press
/// (select, confirm, back, tab: ui_fe_main_select, ui_fe_ht_back...) or a move (ui_fe_main_nav,
/// ui_fe_ht_menu_end, ui_fe_ht_slider, fe_move...). The menu_vibration option picks which; moving
/// in the main menu (ui_fe_main_nav) counts as a press.
void MenuSound(const GfxValue* args, int argc) {
    const char* function = Text(args, argc, 0);
    if (!function || (std::strcmp(function, "snd_PlayPatchOverlapped") != 0 && std::strcmp(function, "snd_PlayPatch") != 0))
        return;
    const char* patch = Text(args, argc, 1);
    const char* action = Text(args, argc, 2);
    if (!patch || (action && !std::strcmp(action, "stop"))) return;
    static int logged = 0;
    if (logged < 16) {
        ++logged;
        std::printf("Menu: sound %s\n", patch);
        std::fflush(stdout);
    }
    using Native::Pulses;
    if (Has(patch, "select") || Has(patch, "confirm") || Has(patch, "start_game") || Has(patch, "start_screen_exit"))
        Native::Pulse(Pulses::MenuPress, 0.45f, 0.6f, 110, 0, Native::Texture::Click);
    else if (Has(patch, "_back"))
        Native::Pulse(Pulses::MenuPress, 0.25f, 0.5f, 90, 0, Native::Texture::Click);
    else if (Has(patch, "_tab"))
        Native::Pulse(Pulses::MenuPress, 0.2f, 0.45f, 80, 0, Native::Texture::Click);
    else if (Has(patch, "ui_fe_main_nav"))  // the main menu (New game, Continue, Options...): on Presses too
        Native::Pulse(Pulses::MenuPress, 0.15f, 0.45f, 60, 0, Native::Texture::Click);
    else if (Has(patch, "menu_end") || Has(patch, "locked"))
        Native::Pulse(Pulses::MenuMove, 0.35f, 0.2f, 70, 0, Native::Texture::Click);
    else if (Has(patch, "_nav") || Has(patch, "slider") || Has(patch, "fe_move"))
        Native::Pulse(Pulses::MenuMove, 0.15f, 0.45f, 60, 0, Native::Texture::Click);
}

// ---- Discord (src/runtime_discord.c): playing or in the menus, and the story mission

constexpr const char* kStoryMission = "Proto_Save_NextStoryMission";
std::mutex g_mission_lock;
char g_mission[32] = "";
std::chrono::steady_clock::time_point g_mission_read{};

void NoteMission(const GfxValue& value) {
    if (value.type != GfxValue::String || !value.string) return;
    std::scoped_lock lock{g_mission_lock};
    std::snprintf(g_mission, sizeof(g_mission), "%s", value.string);
}

/// A menu's call: the game's answer to a read of the story mission is kept, and while a game is
/// played the mission is read again now and then (on the menus' thread, as the pause menu does).
void Presence(void* handler, const GfxValue* args, int argc, const GfxValue* ret, bool before) {
    const char* function = Text(args, argc, 0);
    if (!function) return;
    if (!before) {
        const char* name = Text(args, argc, 1);
        if (ret && name && !std::strcmp(function, "db_GetString") && !std::strcmp(name, kStoryMission)) NoteMission(*ret);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - g_mission_read < std::chrono::seconds(15) || !Native::PlayerExists()) return;
    g_mission_read = now;
    GfxValue get[] = {String("db_GetString"), String(kStoryMission), Number(0), Number(0)};
    NoteMission(GameCall(handler, get, 4));
}

int PresenceSource(char* mission, int size) {
    {
        std::scoped_lock lock{g_mission_lock};
        std::snprintf(mission, std::size_t(size), "%s", g_mission);
    }
    return Native::PlayerExists() ? 1 : 0;
}

/// Verified at a read of the player's handle (0x13491c0), which PlayerExists also reads.
bool InstallPresence(std::uint64_t) {
    Native::SetPresenceSource(&PresenceSource);
    return true;
}

NATIVE_REPLACEMENT({"Discord status (playing or the menus, the story mission)", "presence", Native::Kind::Custom,
                    0x505de2, {0x48, 0x8d, 0x05, 0xd7, 0x33, 0xe4, 0x00, 0x8b, 0x00, 0x89, 0x45, 0x98}, nullptr,
                    nullptr, &InstallPresence});

GUEST_ABI void PlaceLuaCall(void* handler, const GfxValue* args, int argc, GfxValue* ret) {
    MenuSound(args, argc);
    Presence(handler, args, argc, ret, true);
    if (Answer(handler, args, argc, ret)) return;
    Guest::Call<void>(g_place_lua_call, handler, args, argc, ret);
    Presence(handler, args, argc, ret, false);
}

NATIVE_REPLACEMENT({"Scaleform PlaceLuaCall (the port's settings in Options > Video, Controller)", "menu", Native::Kind::CallSite,
                    kHandOver, {0xe9, 0x99, 0x01, 0x00, 0x00, 0x49, 0x81, 0xc5, 0x40, 0x02, 0x00, 0x00},
                    reinterpret_cast<void*>(&PlaceLuaCall), &g_place_lua_call});

} // namespace
