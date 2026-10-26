// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the port's settings in the game's own Options > Video screen (docs/NATIVE.md).
//
// The game's menus are Scaleform movies. Their ActionScript reaches the game's code through
// ExternalInterface.call("PlaceLuaCall", function, arguments...) (commonregisters.DoCallBack):
// the movie's ExternalInterface handler (0x1caf0) sees the method name and hands PlaceLuaCall to
// 0x1cd90, which calls the Lua function of that name and returns its result to the movie.
//
// scripts/proto_menu.py rewrites the Video screens (protovideooption.gfx, protopausevideo.gfx) so
// that after Brightness they ask for the port's rows: port_Count, port_Label(row),
// port_Choices(row), port_Choice(row, index), and add a game toggle per row whose game database
// name is "PortSetting<row>". Here the hand-over to 0x1cd90 (a tail call) goes through native
// code that answers those, and the screen's db_GetInt / db_SetInt / fe_RestoreGameDBDefault for
// PortSetting names, from the port's settings (gpu/shim/port_game_menu.cpp). Everything else goes
// to the game's Lua as before. Without the rewritten screens nothing asks for these names.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

/// The row of a "PortSetting<row>" database name, or -1.
int Row(const char* name, int count) {
    if (!name || std::strncmp(name, "PortSetting", 11) != 0 || name[11] < '0' || name[11] > '9') return -1;
    const int row = std::atoi(name + 11);
    return row < count ? row : -1;
}

/// The port's answer to a PlaceLuaCall, or false for the game's Lua.
bool Answer(const GfxValue* args, int argc, GfxValue* ret) {
    const TheOrderGameMenu* menu = Native::Menu();
    const char* function = Text(args, argc, 0);
    if (!menu || !function || !ret) return false;
    const int count = menu->count();
    if (std::strncmp(function, "port_", 5) == 0) {
        const int row = Int(args, argc, 1);
        const bool valid = row >= 0 && row < count;
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::printf("Menu: the Video screen shows the port's %d settings\n", count);
            std::fflush(stdout);
        }
        if (!std::strcmp(function, "port_Count")) ReturnNumber(ret, count);
        else if (!std::strcmp(function, "port_Label")) ReturnText(ret, valid ? menu->label(row) : "");
        else if (!std::strcmp(function, "port_Choices")) ReturnNumber(ret, valid ? menu->choices(row) : 0);
        else if (!std::strcmp(function, "port_Choice"))
            ReturnText(ret, valid ? menu->choice(row, Int(args, argc, 2)) : "");
        else ret->type = GfxValue::Undefined;
        return true;
    }
    // db_GetInt(name, profile, index), db_SetInt(name, profile, value, index),
    // fe_RestoreGameDBDefault(name, profile)
    const int row = Row(Text(args, argc, 1), count);
    if (row < 0) return false;
    if (!std::strcmp(function, "db_GetInt")) {
        ReturnNumber(ret, menu->get(row));
    } else if (!std::strcmp(function, "db_SetInt")) {
        menu->set(row, Int(args, argc, 3));
        menu->save();
        ret->type = GfxValue::Null;
    } else if (!std::strcmp(function, "fe_RestoreGameDBDefault")) {
        menu->reset(row);
        menu->save();
        ret->type = GfxValue::Null;
    } else {
        return false; // another database call for these names: the game's own answer
    }
    return true;
}

GUEST_ABI void PlaceLuaCall(void* handler, const GfxValue* args, int argc, GfxValue* ret) {
    if (Answer(args, argc, ret)) return;
    Guest::Call<void>(g_place_lua_call, handler, args, argc, ret);
}

NATIVE_REPLACEMENT({"Scaleform PlaceLuaCall (the Video screen's port settings)", "menu", Native::Kind::CallSite,
                    kHandOver, {0xe9, 0x99, 0x01, 0x00, 0x00, 0x49, 0x81, 0xc5, 0x40, 0x02, 0x00, 0x00},
                    reinterpret_cast<void*>(&PlaceLuaCall), &g_place_lua_call});

} // namespace
