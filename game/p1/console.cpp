// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the Lua console. The overlay's console (the key under Esc: ` on English keyboards,
// Ё on Russian ones; gpu/shim/port_console.cpp) queues what is typed; here, on the thread that
// runs the game's Lua (protomain's Advance, menu_mouse.cpp), each command runs in the game's own
// Lua 5.1 with the script manager the trainer uses (trainer.cpp), and what it printed or returned
// goes back to the console.
//
// A helper written in Lua (kHelper, defined once as the global __port_console) does the work:
// `help`, `find <text>` (the globals whose names hold the text: the game's script functions are
// globals, go_GetHealth, db_SetInt...), then the command as an expression (`return ` + it) or,
// if that does not compile, as statements; print is redirected meanwhile. Its result is one
// string: "\1" first for an error, "\2" for a note.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "game/guest.h"
#include "game/native_registry.h"
#include "game/p1/console.h"

namespace {

// The script manager and its helpers (trainer.cpp), and Lua 5.1's lua_tolstring.
constexpr std::uint64_t kScriptManager = 0xca650, kGetTop = 0xd1b20, kSetTop = 0xd1b30, kPushFunction = 0xd1ad0,
                        kPushString = 0xd1a80, kCall = 0xd1800, kType = 0xd1870, kToLString = 0x4d8390;
constexpr int kLuaFunction = 6, kLuaString = 4;

const char kHelper[] = R"LUA(
__port_console = function(src)
  local G = _G or getfenv(0)
  local out = {}
  local function show(...)
    local n = select('#', ...)
    local t = {}
    for i = 1, n do t[i] = tostring((select(i, ...))) end
    out[#out + 1] = table.concat(t, '    ')
  end
  local cmd, arg = string.match(src, '^%s*(%a+)%s*(.-)%s*$')
  if cmd == 'help' and arg == '' then
    if __port_ru then
      return '\2Введите код Lua и нажмите Enter: команды выполняются, у выражения показывается значение.\n' ..
        'find <текст>   функции и значения игры, в имени которых есть этот текст\n' ..
        'clear          очищает журнал\n' ..
        'Например:  find go_Set    go_GetHealth(PLAYER_1)    go_SetHealth(PLAYER_1, 5000)\n' ..
        'go_SlowMotion(0.5) — замедление (1 — обычная скорость)    fe_ToggleHideHUDOption() — скрыть HUD (или F3)'
    end
    return '\2Type Lua and press Enter: statements run, an expression shows its value.\n' ..
      'find <text>   the game\'s functions and values whose names hold the text\n' ..
      'clear         empties this log\n' ..
      'For example:  find go_Set    go_GetHealth(PLAYER_1)    go_SetHealth(PLAYER_1, 5000)\n' ..
      'go_SlowMotion(0.5) slows the game (1 is normal)    fe_ToggleHideHUDOption() hides the HUD (or F3)'
  end
  if cmd == 'find' and arg ~= '' then
    local names, needle = {}, string.lower(arg)
    for k, v in pairs(G) do
      if type(k) == 'string' and string.find(string.lower(k), needle, 1, true) then
        names[#names + 1] = k .. '  (' .. type(v) .. ')'
      end
    end
    table.sort(names)
    if #names == 0 then
      return '\2' .. (__port_ru and ('нет ничего с «' .. arg .. '» в имени') or ('nothing is named like "' .. arg .. '"'))
    end
    local shown = #names > 300 and 300 or #names
    local more = #names > shown and (__port_ru and ('\n... и ещё ' .. (#names - shown) .. ': уточните поиск')
      or ('\n... and ' .. (#names - shown) .. ' more: be more exact')) or ''
    return table.concat(names, '\n', 1, shown) .. more
  end
  local f, err = loadstring('return ' .. src, '=console')
  if not f then f, err = loadstring(src, '=console') end
  if not f then return '\1' .. tostring(err) end
  local function pack(...) return select('#', ...), {...} end
  local saved = print
  print = show
  local n, r = pack(pcall(f))
  print = saved
  if not r[1] then
    out[#out + 1] = tostring(r[2])
    return '\1' .. table.concat(out, '\n')
  end
  if n > 1 then show(unpack(r, 2, n)) end
  if #out == 0 then return '\2ok' end
  return table.concat(out, '\n')
end
)LUA";

bool Matches(std::uint64_t at, std::initializer_list<std::uint8_t> bytes) {
    return std::memcmp(Guest::Image() + at, std::data(bytes), bytes.size()) == 0;
}

bool Ready() {
    static const int ready = [] {
        const bool ok =
            Matches(kScriptManager, {0x48, 0x8b, 0x05, 0x39, 0x22, 0x1a, 0x01, 0xc3}) &&
            Matches(kGetTop, {0x48, 0x8b, 0x7f, 0x18, 0xe9}) && Matches(kSetTop, {0x48, 0x8b, 0x7f, 0x18, 0xe9}) &&
            Matches(kPushString, {0x48, 0x8b, 0x7f, 0x18, 0xe9}) &&
            Matches(kType, {0x48, 0x8b, 0x7f, 0x18, 0xbe, 0xff, 0xff, 0xff, 0xff, 0xe9}) &&
            Matches(kPushFunction, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x48, 0x8b, 0x7b, 0x18}) &&
            Matches(kCall, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0x48, 0x8b, 0x7b, 0x18, 0xba,
                            0x01, 0x00, 0x00, 0x00}) &&
            Matches(kToLString, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x49, 0x89, 0xd6, 0x89,
                                 0xf3});
        std::printf("Console: %s\n", ok ? "ready (the key under Esc)" : "off (the game's code is not where it was)");
        std::fflush(stdout);
        return ok ? 1 : 0;
    }();
    return ready != 0;
}

/// Defines the helper (once; again if the game's Lua lost it, after a load).
bool Define(void* manager) {
    const int top = Guest::Call<int>(kGetTop, manager);
    bool ok = false;
    if (Guest::Call<std::uint8_t>(kPushFunction, manager, "loadstring")) {
        // The game's language (run.py: THEORDER_LANGUAGE, 8 Russian) for the helper's own texts.
        static const std::string code = [] {
            const char* language = std::getenv("THEORDER_LANGUAGE");
            return std::string(language && std::strcmp(language, "8") == 0 ? "__port_ru = true\n" : "__port_ru = false\n") +
                   kHelper;
        }();
        Guest::Call<void>(kPushString, manager, code.c_str());
        ok = Guest::Call<std::uint8_t>(kCall, manager, 1) && Guest::Call<int>(kType, manager) == kLuaFunction &&
             Guest::Call<std::uint8_t>(kCall, manager, 0);
    }
    Guest::Call<void>(kSetTop, manager, top);
    return ok;
}

void Run(const TheOrderGameMenu* menu, const char* command) {
    void* manager = Guest::Call<void*>(kScriptManager);
    if (!manager) {
        menu->console_print("the game's Lua is not running yet", 2);
        return;
    }
    const int top = Guest::Call<int>(kGetTop, manager);
    bool have = Guest::Call<std::uint8_t>(kPushFunction, manager, "__port_console") != 0;
    if (!have) {
        Guest::Call<void>(kSetTop, manager, top);
        have = Define(manager) && Guest::Call<std::uint8_t>(kPushFunction, manager, "__port_console") != 0;
    }
    if (!have) {
        Guest::Call<void>(kSetTop, manager, top);
        menu->console_print("the console's helper could not be defined in the game's Lua", 2);
        return;
    }
    Guest::Call<void>(kPushString, manager, command);
    if (!Guest::Call<std::uint8_t>(kCall, manager, 1) || Guest::Call<int>(kType, manager) != kLuaString) {
        Guest::Call<void>(kSetTop, manager, top);
        menu->console_print("the game's Lua did not answer", 2);
        return;
    }
    void* lua = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(manager) + 0x18);
    const char* text = Guest::Call<const char*>(kToLString, lua, -1, nullptr);
    if (text && text[0] == '\1') {
        menu->console_print(text + 1, 2);
    } else if (text && text[0] == '\2') {
        menu->console_print(text + 1, 3);
    } else if (text) {
        menu->console_print(text, 1);
    }
    Guest::Call<void>(kSetTop, manager, top);
}

} // namespace

namespace Native {

void ConsoleFrame() {
    const TheOrderGameMenu* menu = Native::Menu();
    if (!menu || !menu->console_take || !menu->console_print) return;
    char command[512];
    for (int i = 0; i < 4 && menu->console_take(command, sizeof(command)); ++i) {
        if (!Ready()) {
            menu->console_print("the console does not know this build of the game", 2);
            continue;
        }
        Run(menu, command);
    }
}

} // namespace Native
