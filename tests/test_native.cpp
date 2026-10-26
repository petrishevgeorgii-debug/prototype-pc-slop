// SPDX-License-Identifier: GPL-2.0-or-later
// Native game layer (game/): installation into a stand-in game image. A test replacement takes
// over one call: it is installed only where the original bytes are found, can be turned off with
// THEORDER_NATIVE, and calls the original through the address it is handed. Then the game's
// PlaceLuaCall hand-over (game/p1/options_menu.cpp) answers the Video screen's port rows from a
// stand-in menu and leaves everything else to the game. Linux (build.sh --test).

#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "game/guest.h"
#include "game/native.h"
#include "game/native_registry.h"
#include "port_settings.h"

namespace PortSettings {
Values& Get() {
    static Values values;
    return values;
}
} // namespace PortSettings

namespace {

int failures = 0;
#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

constexpr std::size_t kImage = 0x20000, kCode = 0x10000, kData = 0x1000;
constexpr std::uint64_t kCallee = 0x1000; ///< the stand-in game function: marks byte +0 of its argument
constexpr std::uint64_t kSite = 0x2000;   ///< a call to it
std::uint8_t* g_base = nullptr;
std::size_t g_code_used = 0;

std::uint8_t* AllocCode(std::size_t bytes) {
    if (g_code_used + bytes > kCode) return nullptr;
    std::uint8_t* p = g_base + kImage + g_code_used;
    g_code_used += (bytes + 15) & ~std::size_t(15);
    return p;
}

void Put(std::uint64_t at, std::initializer_list<int> bytes) {
    for (int b : bytes) g_base[at++] = std::uint8_t(b);
}

constexpr std::uint64_t kHandOver = 0x1cbf2, kPlaceLuaCall = 0x1cd90;

void BuildGame() {
    std::memset(g_base, 0xcc, kImage);
    Put(kCallee, {0xc6, 0x07, 0x01, 0xc3});                     // mov byte [rdi], 1; ret
    Put(kSite, {0xe8, 0xfb, 0xef, 0xff, 0xff, 0x90, 0x90});     // call kCallee
    // The ExternalInterface handler's tail call to PlaceLuaCall, which here marks its result
    // with type 5 (the game's Lua answered).
    Put(kHandOver, {0xe9, 0x99, 0x01, 0x00, 0x00, 0x49, 0x81, 0xc5, 0x40, 0x02, 0x00, 0x00});
    Put(kPlaceLuaCall, {0xc7, 0x01, 0x05, 0x00, 0x00, 0x00, 0xc3}); // mov dword [rcx], 5; ret
}

// ---- a stand-in menu: two Video rows, one Controller row
int g_values[2][2] = {{1, 0}, {4, 0}}, g_saved = 0;
int MenuCount(int page) { return page == 0 ? 2 : page == 1 ? 1 : 0; }
const char* MenuLabel(int page, int row) { return page ? "PAD" : row ? "B" : "A"; }
int MenuChoices(int page, int row) { return page ? 9 : row ? 2 : 3; }
const char* MenuChoice(int page, int row, int index) { return !page && row == 0 && index == 2 ? "THIRD" : "X"; }
int MenuGet(int page, int row) { return g_values[page][row]; }
void MenuSet(int page, int row, int index) { g_values[page][row] = index; }
void MenuReset(int page, int row) { g_values[page][row] = 0; }
void MenuSave() { ++g_saved; }
const TheOrderGameMenu kMenu = {MenuCount, MenuLabel, MenuChoices, MenuChoice, MenuGet, MenuSet, MenuReset, MenuSave};

struct Value {
    std::uint32_t type, pad;
    union {
        double number;
        const char* string;
        std::uint64_t raw;
    };
};
Value Str(const char* s) { Value v{4, 0, {}}; v.string = s; return v; }
Value Num(double n) { Value v{3, 0, {}}; v.number = n; return v; }

/// Calls the hand-over as the game's ExternalInterface handler does.
Value LuaCall(std::initializer_list<Value> args) {
    using HandOver = void(GUEST_ABI*)(void*, const Value*, int, Value*);
    Value list[8];
    int n = 0;
    for (const Value& v : args) list[n++] = v;
    Value ret{0, 0, {}};
    reinterpret_cast<HandOver>(g_base + kHandOver)(nullptr, list, n, &ret);
    return ret;
}

std::uint64_t g_original = 0;
int g_calls = 0;

GUEST_ABI std::uint64_t Replacement(std::uint8_t* object) {
    ++g_calls;
    object[1] = 2; // the native part's own work
    return Guest::Call<std::uint64_t>(g_original, object);
}

NATIVE_REPLACEMENT({"test call", "test", Native::Kind::CallSite, kSite, {0xe8, 0xfb, 0xef, 0xff, 0xff, 0x90, 0x90},
                    reinterpret_cast<void*>(&Replacement), &g_original});

using Target = std::uint64_t(GUEST_ABI*)(std::uint8_t*);

Target CallTarget(std::uint64_t site) {
    std::int32_t rel;
    std::memcpy(&rel, g_base + site + 1, 4);
    return reinterpret_cast<Target>(g_base + site + 5 + rel);
}

} // namespace

int main() {
    g_base = static_cast<std::uint8_t*>(mmap(nullptr, kImage + kCode + kData, PROT_READ | PROT_WRITE | PROT_EXEC,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (g_base == MAP_FAILED) return 2;
    NativeHost host{g_base, kImage, AllocCode, g_base + kImage + kCode, kData, nullptr};

    // Off: nothing changes.
    BuildGame();
    setenv("THEORDER_NATIVE", "0", 1);
    native_game_install(&host);
    CHECK(reinterpret_cast<std::uint8_t*>(CallTarget(kSite)) == g_base + kCallee);

    // The group left out: nothing changes either.
    BuildGame();
    setenv("THEORDER_NATIVE", "-test", 1);
    native_game_install(&host);
    CHECK(reinterpret_cast<std::uint8_t*>(CallTarget(kSite)) == g_base + kCallee);

    // On: the call goes to a native thunk, which runs the replacement and the original.
    BuildGame();
    unsetenv("THEORDER_NATIVE");
    native_game_install(&host);
    CHECK(reinterpret_cast<std::uint8_t*>(CallTarget(kSite)) >= g_base + kImage);
    std::uint8_t object[2] = {0, 0};
    CallTarget(kSite)(object);
    CHECK(g_calls == 1 && object[0] == 1 && object[1] == 2);

    // A build whose code differs: nothing is installed there.
    BuildGame();
    g_base[kSite + 5] = 0x91;
    native_game_install(&host);
    CHECK(reinterpret_cast<std::uint8_t*>(CallTarget(kSite)) == g_base + kCallee);

    // The Video and Controller screens' rows: answered from the menu; the game's own calls go to its Lua. The
    // loader's host is a local that is gone when the game runs: so is this one.
    BuildGame();
    {
        auto* local = new NativeHost(host);
        local->menu = &kMenu;
        native_game_install(local);
        std::memset(static_cast<void*>(local), 0xff, sizeof(*local)); // (kept allocated: reads see the poison)
    }
    Value r = LuaCall({Str("port_Count"), Num(0)});
    CHECK(r.type == 3 && r.number == 2);
    r = LuaCall({Str("port_Count"), Num(1)});
    CHECK(r.type == 3 && r.number == 1);
    r = LuaCall({Str("port_Count"), Num(2)});
    CHECK(r.type == 3 && r.number == 0); // no such page
    r = LuaCall({Str("port_Label"), Num(0), Num(1)});
    CHECK(r.type == 4 && !std::strcmp(r.string, "B"));
    r = LuaCall({Str("port_Label"), Num(1), Num(0)});
    CHECK(r.type == 4 && !std::strcmp(r.string, "PAD"));
    r = LuaCall({Str("port_Choices"), Num(0), Num(0)});
    CHECK(r.type == 3 && r.number == 3);
    r = LuaCall({Str("port_Choices"), Num(1), Num(0)});
    CHECK(r.type == 3 && r.number == 9);
    r = LuaCall({Str("port_Choice"), Num(0), Num(0), Num(2)});
    CHECK(r.type == 4 && !std::strcmp(r.string, "THIRD"));
    r = LuaCall({Str("db_GetInt"), Str("PortSetting0_0"), Num(0), Num(0)});
    CHECK(r.type == 3 && r.number == 1);
    r = LuaCall({Str("db_GetInt"), Str("PortSetting1_0"), Num(0), Num(0)});
    CHECK(r.type == 3 && r.number == 4);
    r = LuaCall({Str("db_SetInt"), Str("PortSetting0_1"), Num(0), Num(1), Num(0)});
    CHECK(g_values[0][1] == 1 && g_saved == 1 && r.type != 5);
    r = LuaCall({Str("db_SetInt"), Str("PortSetting1_0"), Num(0), Num(2), Num(0)});
    CHECK(g_values[1][0] == 2 && g_saved == 2 && r.type != 5);
    r = LuaCall({Str("fe_RestoreGameDBDefault"), Str("PortSetting0_0"), Num(0)});
    CHECK(g_values[0][0] == 0 && g_saved == 3);
    CHECK(LuaCall({Str("db_GetInt"), Str("SettingVideoGammaCorrection"), Num(0), Num(0)}).type == 5);
    CHECK(LuaCall({Str("db_GetInt"), Str("PortSetting0_7"), Num(0), Num(0)}).type == 5); // no such row
    CHECK(LuaCall({Str("db_GetInt"), Str("PortSetting1_1"), Num(0), Num(0)}).type == 5);
    CHECK(LuaCall({Str("db_GetInt"), Str("PortSetting2_0"), Num(0), Num(0)}).type == 5); // no such page
    CHECK(LuaCall({Str("db_GetInt"), Str("PortSetting0"), Num(0), Num(0)}).type == 5);   // not a name of ours
    CHECK(LuaCall({Str("db_GetInt"), Str("PortSetting0_0x"), Num(0), Num(0)}).type == 5);
    CHECK(LuaCall({Str("fe_IsStorageAvailable")}).type == 5);

    // Without the port's menu every call goes to the game.
    BuildGame();
    native_game_install(&host);
    CHECK(LuaCall({Str("port_Count"), Num(0)}).type == 5);

    if (failures) {
        std::fprintf(stderr, "native test: %d failures\n", failures);
        return 1;
    }
    std::puts("native test: ok");
    return 0;
}
