// SPDX-License-Identifier: GPL-2.0-or-later
// Native game layer (game/): installation into a stand-in game image. A test replacement takes
// over one call: it is installed only where the original bytes are found, can be turned off with
// THEORDER_NATIVE, and calls the original through the address it is handed. Then the game's
// PlaceLuaCall hand-over (game/p1/options_menu.cpp) answers the Video and Controller screens'
// port rows from a stand-in menu, keeps a linked game setting in step, and leaves everything else
// to the game. Linux (build.sh --test).

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

constexpr std::size_t kImage = 0x1360000, kCode = 0x10000, kData = 0x1000;
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
constexpr std::uint64_t kLuaCalls = 0x1f000; ///< the stand-in PlaceLuaCall counts its calls here
// The rumble switch's hand-over (game/p1/rumble.cpp) to 0x568510(on, player), which here stores
// its arguments at kRumbleArgs.
constexpr std::uint64_t kSwitchSite = 0x5685cb, kSetRumble = 0x568510, kRumbleArgs = 0x1f100;
// Moves (game/p1/rumble.cpp): the first move's first bytes as in the game (they are checked), and
// vtable slots 6 and 7 of the consume effect, slot 6 of the whipfist's lash and of the hit reaction
// (its owner handle at +0xc8) holding the game's functions: stand-ins put in after installing.
constexpr std::uint64_t kEffectStart = 0x856cd0, kEffectEnd = 0x8570a0, kEffectSlots = 0x109b450;
constexpr std::uint64_t kLash = 0x8ac8d0, kLashSlot = 0x109fd30 + 0x30;
constexpr std::uint64_t kHit = 0x8683c0, kHitSlot = 0x109c8a0 + 0x30, kPlayer = 0x13491c0;
// The camera shake: the CameraManager's call (to kApplyShake), its pointer at kCameraManager to a
// stand-in manager at kManager.
// The Discord status: verified at a read of the player's handle; the object table's pointer at
// kObjects to a stand-in table at kTable (the player's entry: index 0x12, generation 7).
// The mouse in the menus: GFxMovieRoot's Advance (its vtable slot) and the functions it calls,
// their first bytes as in the game, stand-ins put in after installing that note their arguments
// at kGfxNotes: NotifyMouseState x, y, buttons, mouse; the cursor count; HandleEvent's calls and
// last event type.
constexpr std::uint64_t kAdvance = 0x3ff610, kAdvanceSlot = 0x1065d30 + 31 * 8, kHandleEvent = 0x402250,
                        kNotifyMouse = 0x402ad0, kCursorCount = 0xad0410, kGfxNotes = 0x1f600;
constexpr std::uint64_t kPresenceSite = 0x505de2, kObjects = 0x1233670, kTable = 0x1f400;
int (*g_presence)(char*, int) = nullptr;
void PresenceSource(int (*source)(char*, int)) { g_presence = source; }
constexpr std::uint64_t kShakeCall = 0x70c2ed, kApplyShake = 0x103e90, kCameraManager = 0x135c5f0, kManager = 0x1f200;

// Pulses the native layer asks the pad for.
int g_pulses = 0, g_pulse_kind = -1, g_pulse_id = 0, g_pulse_texture = -1;
float g_pulse_large = 0, g_pulse_small = 0;
unsigned g_pulse_ms = 0, g_pulse_rise = 0;
void PadEffect(int kind, int id, float large, float small, unsigned ms, unsigned rise_ms, unsigned, int texture) {
    ++g_pulses;
    g_pulse_texture = texture;
    g_pulse_kind = kind;
    g_pulse_id = id;
    g_pulse_large = large;
    g_pulse_small = small;
    g_pulse_ms = ms;
    g_pulse_rise = rise_ms;
}

void PutRel32(std::uint64_t at, std::uint64_t target, std::uint64_t end) {
    const std::int32_t rel = std::int32_t(target - end);
    std::memcpy(g_base + at, &rel, 4);
}

int LuaCalls() {
    std::int32_t n;
    std::memcpy(&n, g_base + kLuaCalls, 4);
    return n;
}

void BuildGame() {
    std::memset(g_base, 0xcc, kImage);
    Put(kCallee, {0xc6, 0x07, 0x01, 0xc3});                     // mov byte [rdi], 1; ret
    Put(kSite, {0xe8, 0xfb, 0xef, 0xff, 0xff, 0x90, 0x90});     // call kCallee
    // The ExternalInterface handler's tail call to PlaceLuaCall, which here marks its result
    // with type 5 (the game's Lua answered).
    Put(kHandOver, {0xe9, 0x99, 0x01, 0x00, 0x00, 0x49, 0x81, 0xc5, 0x40, 0x02, 0x00, 0x00});
    // inc dword [rip -> kLuaCalls]; mov dword [rcx], 5; ret
    const std::int32_t rel = std::int32_t(kLuaCalls - (kPlaceLuaCall + 6));
    Put(kPlaceLuaCall, {0xff, 0x05, rel & 0xff, (rel >> 8) & 0xff, (rel >> 16) & 0xff, (rel >> 24) & 0xff,
                        0xc7, 0x01, 0x05, 0x00, 0x00, 0x00, 0xc3});
    g_base[kLuaCalls] = g_base[kLuaCalls + 1] = g_base[kLuaCalls + 2] = g_base[kLuaCalls + 3] = 0;
    Put(kSetRumble, {0x89, 0x3d, 0, 0, 0, 0, 0x89, 0x35, 0, 0, 0, 0, 0xc3}); // mov [args], edi; mov [args+4], esi; ret
    PutRel32(kSetRumble + 2, kRumbleArgs, kSetRumble + 6);
    PutRel32(kSetRumble + 8, kRumbleArgs + 4, kSetRumble + 12);
    Put(kSwitchSite, {0xe9, 0x40, 0xff, 0xff, 0xff, 0x48, 0x3b, 0x47, 0x08, 0x75, 0x11, 0x48});
    std::memset(g_base + kRumbleArgs, 0xff, 8);
    Put(kEffectStart, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54});
    const std::uintptr_t slots[2] = {reinterpret_cast<std::uintptr_t>(g_base + kEffectStart),
                                     reinterpret_cast<std::uintptr_t>(g_base + kEffectEnd)};
    std::memcpy(g_base + kEffectSlots, slots, sizeof(slots));
    Put(kLash, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec});
    const std::uintptr_t lash = reinterpret_cast<std::uintptr_t>(g_base + kLash),
                         hit = reinterpret_cast<std::uintptr_t>(g_base + kHit);
    std::memcpy(g_base + kLashSlot, &lash, 8);
    std::memcpy(g_base + kHitSlot, &hit, 8);
    const std::uint32_t player = 0x00070012;
    std::memcpy(g_base + kPlayer, &player, 4);
    Put(kShakeCall, {0xe8, 0x9e, 0x7b, 0x9f, 0xff, 0x49, 0x83, 0xc4, 0x24, 0xff, 0xcb, 0x0f});
    Put(kApplyShake, {0xc3});
    std::uint8_t* manager = g_base + kManager;
    std::memcpy(g_base + kCameraManager, &manager, 8);
    Put(kPresenceSite, {0x48, 0x8d, 0x05, 0xd7, 0x33, 0xe4, 0x00, 0x8b, 0x00, 0x89, 0x45, 0x98});
    Put(kAdvance, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53});
    Put(kHandleEvent, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xf3});
    Put(kNotifyMouse, {0x55, 0x41, 0x56, 0x53, 0x39, 0x97, 0x80, 0x0a, 0x00, 0x00});
    Put(kCursorCount, {0x83, 0xfe, 0x04, 0xb8, 0x04, 0x00, 0x00, 0x00, 0x0f, 0x46, 0xc6});
    const std::uintptr_t advance = reinterpret_cast<std::uintptr_t>(g_base + kAdvance);
    std::memcpy(g_base + kAdvanceSlot, &advance, 8);
    std::memset(g_base + kGfxNotes, 0, 32);
    std::uint8_t* table = g_base + kTable;
    std::memcpy(g_base + kObjects, &table, 8);
    std::memset(table + 0x12 * 16, 0, 16);
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
const char* MenuLinked(int page, int row) { return page == 1 && row == 0 ? "GameSwitch" : nullptr; }
int g_quits = 0;
void MenuQuit() { ++g_quits; }
int g_pointer_mode = -1;
int MenuPointerMode(int mode) { g_pointer_mode = mode; return mode; }
void MenuPointerTake(TheOrderPointer* out) { *out = {1, 1, 0.5f, 0.25f, 1u, 2}; }
const TheOrderGameMenu kMenu = {MenuCount, MenuLabel, MenuChoices, MenuChoice, MenuGet, MenuSet, MenuReset, MenuSave,
                                MenuLinked, MenuQuit, MenuPointerMode, MenuPointerTake};

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
    NativeHost host{g_base, kImage, AllocCode, g_base + kImage + kCode, kData, nullptr, PadEffect, PresenceSource};

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
    int calls = LuaCalls();
    r = LuaCall({Str("db_GetInt"), Str("PortSetting0_0"), Num(0), Num(0)});
    CHECK(LuaCalls() == calls); // not linked: the game is not asked
    r = LuaCall({Str("db_GetInt"), Str("PortSetting1_0"), Num(0), Num(0)});
    CHECK(r.type == 3 && r.number == 4);
    CHECK(LuaCalls() == calls + 1); // linked and on: the game's switch is read (not a number here: left)
    r = LuaCall({Str("db_SetInt"), Str("PortSetting0_1"), Num(0), Num(1), Num(0)});
    CHECK(g_values[0][1] == 1 && g_saved == 1 && r.type != 5);
    calls = LuaCalls();
    r = LuaCall({Str("db_SetInt"), Str("PortSetting1_0"), Num(0), Num(2), Num(0)});
    CHECK(g_values[1][0] == 2 && g_saved == 2 && r.type != 5);
    CHECK(LuaCalls() == calls + 1); // the game's switch is set
    r = LuaCall({Str("fe_RestoreGameDBDefault"), Str("PortSetting0_0"), Num(0)});
    CHECK(g_values[0][0] == 0 && g_saved == 3);
    CHECK(LuaCall({Str("db_GetInt"), Str("SettingVideoGammaCorrection"), Num(0), Num(0)}).type == 5);
    CHECK(LuaCall({Str("db_GetInt"), Str("PortSetting0_7"), Num(0), Num(0)}).type == 5); // no such row
    CHECK(LuaCall({Str("db_GetInt"), Str("PortSetting1_1"), Num(0), Num(0)}).type == 5);
    CHECK(LuaCall({Str("db_GetInt"), Str("PortSetting2_0"), Num(0), Num(0)}).type == 5); // no such page
    CHECK(LuaCall({Str("db_GetInt"), Str("PortSetting0"), Num(0), Num(0)}).type == 5);   // not a name of ours
    CHECK(LuaCall({Str("db_GetInt"), Str("PortSetting0_0x"), Num(0), Num(0)}).type == 5);
    CHECK(LuaCall({Str("fe_IsStorageAvailable")}).type == 5);
    // Quit to the desktop: the port's, not the game's (the PS4 game has none).
    calls = LuaCalls();
    CHECK(LuaCall({Str("fe_ExitGame")}).type != 5 && g_quits == 1 && LuaCalls() == calls);

    // The profile's rumble switch: off is handed on as on.
    {
        using Switch = void(GUEST_ABI*)(std::uint32_t, std::uint32_t);
        reinterpret_cast<Switch>(g_base + kSwitchSite)(0, 3);
        std::uint32_t got[2];
        std::memcpy(got, g_base + kRumbleArgs, 8);
        CHECK(got[0] == 1 && got[1] == 3);
    }

    // Moves: the consume effect's start swells the vibration, its end fades it; the game's own run too.
    {
        Put(kEffectStart, {0xb8, 0x05, 0x00, 0x00, 0x00, 0xc3}); // stand-ins: mov eax, 5 / 6 / 7 / 8; ret
        Put(kEffectEnd, {0xb8, 0x06, 0x00, 0x00, 0x00, 0xc3});
        Put(kLash, {0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3});
        Put(kHit, {0xb8, 0x08, 0x00, 0x00, 0x00, 0xc3});
        using Slot = std::uint64_t(GUEST_ABI*)(void*);
        std::uintptr_t slots[2];
        std::memcpy(slots, g_base + kEffectSlots, sizeof(slots));
        CHECK(slots[0] != reinterpret_cast<std::uintptr_t>(g_base + kEffectStart));
        int before = g_pulses;
        CHECK(reinterpret_cast<Slot>(slots[0])(nullptr) == 5);
        CHECK(g_pulses == before + 1 && g_pulse_kind == 0 && g_pulse_id != 0 && g_pulse_ms > 0 && g_pulse_rise > 0);
        const int consume = g_pulse_id;
        CHECK(reinterpret_cast<Slot>(slots[1])(nullptr) == 6);
        CHECK(g_pulses == before + 2 && g_pulse_ms == 0 && g_pulse_id == consume);

        // The whipfist's lash: a tap, whoever's.
        std::uintptr_t slot;
        std::memcpy(&slot, g_base + kLashSlot, 8);
        CHECK(reinterpret_cast<Slot>(slot)(nullptr) == 7);
        CHECK(g_pulses == before + 3 && g_pulse_id == 0 && g_pulse_ms > 0 && g_pulse_small > g_pulse_large);
        CHECK(g_pulse_texture == static_cast<int>(Native::Texture::Crack)); // the HD haptics' whip crack

        // A hit reaction: only the player's.
        std::uint8_t action[0x100] = {};
        std::memcpy(&slot, g_base + kHitSlot, 8);
        const std::uint32_t other = 0x00070013, player = 0x00070012;
        std::memcpy(action + 0xc8, &other, 4);
        CHECK(reinterpret_cast<Slot>(slot)(action) == 8);
        CHECK(g_pulses == before + 3);
        std::memcpy(action + 0xc8, &player, 4);
        CHECK(reinterpret_cast<Slot>(slot)(action) == 8);
        CHECK(g_pulses == before + 4 && g_pulse_large > 0.5f);

        // A camera shake vibrates by its strength, once for all its parts, and is still applied.
        using Apply = void(GUEST_ABI*)(void*, void*, void*, float, float, float, float);
        CHECK(reinterpret_cast<std::uint8_t*>(CallTarget(kShakeCall)) != g_base + kApplyShake);
        std::uint8_t camera[16] = {};
        std::uint8_t* manager = g_base + kManager;
        void* cam = camera;
        const std::int32_t level = 2;
        const float strength = 3.0f;
        std::memcpy(manager + 0x18, &cam, 8);
        std::memcpy(manager + 0x90, &level, 4);
        std::memcpy(manager + 0x94, &strength, 4);
        before = g_pulses;
        reinterpret_cast<Apply>(CallTarget(kShakeCall))(camera, nullptr, nullptr, 1, 2, 3, 4);
        reinterpret_cast<Apply>(CallTarget(kShakeCall))(camera, nullptr, nullptr, 1, 2, 3, 4);
        CHECK(g_pulses == before + 1 && g_pulse_large == 1.0f);
        const std::int32_t none = -1;
        std::memcpy(manager + 0x90, &none, 4); // the Lua's shakes: no level
        reinterpret_cast<Apply>(CallTarget(kShakeCall))(camera, nullptr, nullptr, 1, 2, 3, 4);
        CHECK(g_pulses == before + 1);
    }

    // The menus' sounds tap the controller: a press, a move, not a stop, not other calls.
    int pulses = g_pulses;
    LuaCall({Str("snd_PlayPatchOverlapped"), Str("ui_fe_main_select")});
    CHECK(g_pulses == pulses + 1 && g_pulse_kind == 1);
    LuaCall({Str("snd_PlayPatchOverlapped"), Str("ui_fe_ht_back")});
    CHECK(g_pulses == pulses + 2 && g_pulse_kind == 1);
    LuaCall({Str("snd_PlayPatchOverlapped"), Str("ui_fe_ht_nav")});
    CHECK(g_pulses == pulses + 3 && g_pulse_kind == 2);
    LuaCall({Str("snd_PlayPatchOverlapped"), Str("ui_fe_main_nav")}); // the main menu: a press
    CHECK(g_pulses == pulses + 4 && g_pulse_kind == 1);
    ++pulses;
    LuaCall({Str("snd_PlayPatch"), Str("fe_select"), Str("play")});
    CHECK(g_pulses == pulses + 4 && g_pulse_kind == 1);
    LuaCall({Str("snd_PlayPatch"), Str("ui_fe_main_select"), Str("stop")});
    LuaCall({Str("snd_PlayPatchOverlapped"), Str("ui_fe_ht_wipe")});
    LuaCall({Str("fe_select")});
    CHECK(g_pulses == pulses + 4);
    calls = LuaCalls();
    LuaCall({Str("snd_PlayPatchOverlapped"), Str("ui_fe_main_select")});
    CHECK(LuaCalls() == calls + 1); // the sound itself still plays

    // The Discord status: playing while the player's object is in the table.
    {
        CHECK(g_presence != nullptr);
        char mission[32] = "x";
        CHECK(g_presence(mission, sizeof(mission)) == 0 && mission[0] == 0); // no object: the menus
        const std::uint64_t object = 0x1234;
        const std::uint16_t generation = 7;
        std::memcpy(g_base + kTable + 0x12 * 16, &object, 8);
        std::memcpy(g_base + kTable + 0x12 * 16 + 8, &generation, 2);
        CHECK(g_presence(mission, sizeof(mission)) == 1);
        const std::uint16_t other = 8; // a later object in the same slot: not the player
        std::memcpy(g_base + kTable + 0x12 * 16 + 8, &other, 2);
        CHECK(g_presence(mission, sizeof(mission)) == 0);
    }

    // The mouse in the menus: the cursor asks whether to show; a frame of a movie hands GFx the
    // mouse (once a cursor count and the focus), then runs the game's Advance.
    {
        CHECK(LuaCall({Str("port_Pointer"), Num(1)}).number == 1 && g_pointer_mode == 1);
        CHECK(LuaCall({Str("port_Pointer"), Num(0)}).number == 0 && g_pointer_mode == 0);
        // Stand-ins: Advance returns 3.0f; NotifyMouseState and the cursor count note their
        // arguments, HandleEvent counts its calls and notes the event's type.
        Put(kAdvance, {0xb8, 0x00, 0x00, 0x40, 0x40, 0x66, 0x0f, 0x6e, 0xc0, 0xc3});
        Put(kNotifyMouse, {0xf3, 0x0f, 0x11, 0x05, 0, 0, 0, 0, 0xf3, 0x0f, 0x11, 0x0d, 0, 0, 0, 0,
                           0x89, 0x35, 0, 0, 0, 0, 0x89, 0x15, 0, 0, 0, 0, 0xc3});
        PutRel32(kNotifyMouse + 4, kGfxNotes, kNotifyMouse + 8);
        PutRel32(kNotifyMouse + 12, kGfxNotes + 4, kNotifyMouse + 16);
        PutRel32(kNotifyMouse + 18, kGfxNotes + 8, kNotifyMouse + 22);
        PutRel32(kNotifyMouse + 24, kGfxNotes + 12, kNotifyMouse + 28);
        Put(kCursorCount, {0x89, 0x35, 0, 0, 0, 0, 0xc3});
        PutRel32(kCursorCount + 2, kGfxNotes + 16, kCursorCount + 6);
        Put(kHandleEvent, {0x8b, 0x06, 0x89, 0x05, 0, 0, 0, 0, 0xff, 0x05, 0, 0, 0, 0, 0xc3});
        PutRel32(kHandleEvent + 4, kGfxNotes + 20, kHandleEvent + 8);
        PutRel32(kHandleEvent + 10, kGfxNotes + 24, kHandleEvent + 14);
        std::uintptr_t slot;
        std::memcpy(&slot, g_base + kAdvanceSlot, 8);
        CHECK(slot != reinterpret_cast<std::uintptr_t>(g_base + kAdvance));
        static std::uint8_t movie[0x1000] = {};
        const std::int32_t buffer[2] = {1920, 1080};
        std::memcpy(movie + 0x64, buffer, sizeof(buffer));
        using Advance = float(GUEST_ABI*)(void*, float, std::uint32_t);
        CHECK(reinterpret_cast<Advance>(slot)(movie, 0.033f, 2) == 3.0f);
        float xy[2];
        std::uint32_t notes[5];
        std::memcpy(xy, g_base + kGfxNotes, 8);
        std::memcpy(notes, g_base + kGfxNotes + 8, 20);
        CHECK(xy[0] == 960.0f && xy[1] == 270.0f && notes[0] == 1 && notes[1] == 0); // buttons, mouse 0
        CHECK(notes[2] == 1);                                                          // one cursor
        CHECK(notes[4] == 3 && notes[3] == 4); // SetFocus, then the wheel's two notches
    }

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
