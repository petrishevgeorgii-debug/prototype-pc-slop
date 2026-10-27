// SPDX-License-Identifier: GPL-2.0-or-later
// PROTOTYPE: the trainer (docs/NATIVE.md). Its screen is a page of the game's own pause menu
// (scripts/proto_menu.py makes it from the pause Controller screen, in place of the developers'
// debug screen PROTO_PAUSE_DEBUG; its rows are the port's page 3, gpu/shim/port_game_menu.cpp).
// F1 or the touchpad held opens it: from the game the port presses Options (the pause menu opens)
// and then pushes the screen; backing out of it, or F1 again, goes back to the game.
//
// What the rows do runs as Lua in the game's own scripting (the functions its mission scripts
// call: go_SetHealth, go_SetDamageable, db_SetInt, UnlockForMission...), with the game's script
// manager (0xca650): loadstring, then the chunk, through its own push and call helpers. It runs
// in protomain's Advance (menu_mouse.cpp), on the thread that runs the game's Lua.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game/guest.h"
#include "game/native_registry.h"
#include "game/p1/trainer.h"

namespace {

// The game's script manager (Lua 5.1) and its helpers: (manager) the Lua state at +0x18.
constexpr std::uint64_t kScriptManager = 0xca650, kGetTop = 0xd1b20, kSetTop = 0xd1b30, kPushFunction = 0xd1ad0,
                        kPushString = 0xd1a80, kCall = 0xd1800, kType = 0xd1870, kGetFloat = 0xd1960;
// GFxMovie::Invoke(method, GFxValue* result, const GFxValue* args, UInt count).
constexpr std::uint64_t kInvoke = 0x4055e0;
constexpr int kLuaFunction = 6;

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
            Matches(kGetFloat, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x49, 0x89, 0xf7}) &&
            Matches(kInvoke, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                              0x83, 0xec, 0x58, 0x45, 0x89, 0xc4});
        std::printf("Trainer: %s\n", ok ? "ready (F1, or hold the touchpad)" : "off (the game's code is not where it was)");
        std::fflush(stdout);
        return ok ? 1 : 0;
    }();
    return ready != 0;
}

/// Runs `code` as a Lua chunk; its first result (a number) into `result` when given.
bool RunLua(const char* code, float* result = nullptr) {
    void* manager = Guest::Call<void*>(kScriptManager);
    if (!manager) return false;
    const int top = Guest::Call<int>(kGetTop, manager);
    bool ok = false;
    if (Guest::Call<std::uint8_t>(kPushFunction, manager, "loadstring")) {
        Guest::Call<void>(kPushString, manager, code);
        if (Guest::Call<std::uint8_t>(kCall, manager, 1) && Guest::Call<int>(kType, manager) == kLuaFunction &&
            Guest::Call<std::uint8_t>(kCall, manager, 0)) {
            ok = true;
            if (result) {
                void* lua = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(manager) + 0x18);
                float value = 0.0f;
                *result = Guest::Call<std::uint8_t>(kGetFloat, lua, &value) ? value : -1.0f;
            }
        }
    }
    Guest::Call<void>(kSetTop, manager, top);
    return ok;
}

bool Invoke(void* movie, const char* method) {
    std::uint8_t result[16] = {};
    if (Guest::Call<std::uint8_t>(kInvoke, movie, method, result, nullptr, 0u)) return true;
    char rooted[64];
    std::snprintf(rooted, sizeof(rooted), "_root.%s", method);
    return Guest::Call<std::uint8_t>(kInvoke, movie, rooted, result, nullptr, 0u) != 0;
}

void Log(const char* text) {
    std::printf("Trainer: %s\n", text);
    std::fflush(stdout);
}

// ---- what the rows do

// go_SetDamageable counts (the health behaviour adds 1 or -1: +0x28), so it is said once per
// player object: __port_god holds the one made undamageable, and only that one is given back.
const char kGod[] = "local p=PLAYER_1 if p and go_IsValid(p) then "
                    "if __port_god~=p then go_SetDamageable(p,false) __port_god=p end "
                    "local m=go_GetMaxRegenHealth(p) if m and go_GetHealth(p)<m then go_SetHealth(p,m) end end";
const char kMortal[] = "local p=PLAYER_1 if p and __port_god==p and go_IsValid(p) then go_SetDamageable(p,true) end "
                       "__port_god=nil";
// Critical mass: Alex's health behaviour (proto::PrototypeHealthBehaviour) takes a SetHealth above
// its normal top only scaled by criticalMassHealthGainFactor, so the game's own go_SetHealth
// never filled it. The trainer asks with a marker value instead, which HealthMessage below (its
// message handler, in the class's vtable) turns into the full bar: the top + critical mass.
constexpr float kFillMarker = 98765.0f;
const char kCriticalMass[] = "local p=PLAYER_1 if p and go_IsValid(p) then "
                             "go_SetHealth(p,98765) "
                             "return go_GetHealth(p) end return -1";

// proto::PrototypeHealthBehaviour: its vtable's message handler (slot 5 past the vtable's two
// header words), engine::HealthBehaviour's health setter.
constexpr std::uint64_t kHealthMessage = 0x6bc570, kHealthMessageSlot = 0x1092230 + 7 * 8, kSetHealth = 0x1258c0;
bool g_fill_hooked = false;

GUEST_ABI void HealthMessage(std::uint8_t* self, std::uint8_t* message) {
    std::uint32_t type;
    std::uint16_t kind;
    float value;
    std::memcpy(&type, message + 0x08, 4);
    std::memcpy(&kind, message + 0x0c, 2);
    std::memcpy(&value, message + 0x10, 4);
    if (type == 0x14 && kind == 0 && value == kFillMarker) { // SetHealth, from the trainer
        // Topped up whenever below (a devastator spends it; the marker comes every few frames).
        float health, top, mass;
        std::memcpy(&health, self + 0x14, 4);
        std::memcpy(&top, self + 0x1c, 4);  // the top (go_GetMaxHealth)
        std::memcpy(&mass, self + 0x1dc, 4); // critical mass on top of it
        if (health < top + mass) Guest::Call<void>(kSetHealth, self, top + mass);
        return;
    }
    Guest::Call<void>(kHealthMessage, self, message);
}

bool InstallHealth(std::uint64_t) {
    auto& slot = Guest::At<std::uintptr_t>(kHealthMessageSlot);
    if (slot != reinterpret_cast<std::uintptr_t>(Guest::Image()) + kHealthMessage) return false;
    if (std::memcmp(Guest::Image() + kSetHealth, "\xc5\xfa\x11\x47\x14\xc5\xf0\x57\xc9", 9) != 0) return false;
    slot = reinterpret_cast<std::uintptr_t>(&HealthMessage);
    g_fill_hooked = true;
    return true;
}

NATIVE_REPLACEMENT({"trainer's critical mass (Alex's health messages)", "trainer", Native::Kind::Custom, kHealthMessage,
                    {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xf3, 0x49, 0x89, 0xfe, 0x8b, 0x43, 0x08},
                    nullptr, nullptr, &InstallHealth});
const char kAddEp[] = "local n='Proto_Save_EvolutionPoints' "
                      "if ep_AwardEP then ep_AwardEP(100000) "
                      "else db_SetInt(n,0,(db_GetInt(n,0,0) or 0)+100000,0) end "
                      "return db_GetInt(n,0,0)";
const char kUnlock[] =
    "local f=UnlockForMission or unlockable_UnlockForMission local n=0 if f then "
    "for _,m in ipairs({'e01m00','e01m01','e01m02','e02m00','e02m01','e02m02','e02m05','e03m00','e03m01',"
    "'e03m02','e04m00','e04m01','e04m02','e04m03','e04m04','e04m05','e05m00','e05m01','e05m02','e05m03',"
    "'e06m00','e06m01','e06m02','e06m03','e06m04','e06m05','e07m00','e07m01','e07m02','e07m04','e08m00',"
    "'e08m02','e08m03','e08m04','e09m00','e09m01','e09m03','e10m00','e10m04','e11m00','e11m01'}) do "
    "if pcall(f,m,true,true,true) then n=n+1 end end end return n";

void Actions(const TheOrderGameMenu* menu) {
    const unsigned actions = menu->trainer_actions ? menu->trainer_actions() : 0;
    char text[128];
    if (actions & (1u << THEORDER_TRAINER_EP)) {
        float ep = -1.0f;
        const bool ok = RunLua(kAddEp, &ep);
        std::snprintf(text, sizeof(text), ok ? "100,000 evolution points added (now %.0f)" : "adding evolution points failed",
                      ep);
        Log(text);
    }
    if (actions & (1u << THEORDER_TRAINER_UNLOCK)) {
        float missions = 0.0f;
        const bool ok = RunLua(kUnlock, &missions);
        std::snprintf(text, sizeof(text), ok ? "powers and upgrades unlocked (%.0f missions' worth)" : "unlocking failed",
                      missions);
        Log(text);
    }
}

void Switches(const TheOrderGameMenu* menu, bool playing) {
    static int god_was = 0, frames = 0, failures = 0;
    const int god = menu->get(THEORDER_MENU_TRAINER, THEORDER_TRAINER_GOD);
    const int mass = menu->get(THEORDER_MENU_TRAINER, THEORDER_TRAINER_CRITICAL_MASS);
    if (god != god_was) {
        god_was = god;
        if (!god) RunLua(kMortal);
        Log(god ? "god mode on" : "god mode off");
    }
    if (!playing || (!god && !mass) || ++frames < 10) return;
    frames = 0;
    bool ok = true;
    if (god) ok = RunLua(kGod) && ok;
    if (mass) {
        // What the game made of it, for the log: a few times, and whenever it is not at the top.
        static int logged = 0;
        float health = -1.0f;
        ok = RunLua(kCriticalMass, &health) && ok;
        float max = -1.0f;
        if (logged < 3 || (health >= 0 && logged < 12 && RunLua("return go_GetMaxHealth(PLAYER_1)", &max) &&
                           health < max)) {
            ++logged;
            char text[96];
            std::snprintf(text, sizeof(text), "critical mass: health %.0f after filling it%s", health,
                          g_fill_hooked ? "" : " (the health hook is not installed)");
            Log(text);
        }
    }
    if (!ok && failures++ < 3) Log("a switch's Lua failed (the game's log has the error)");
}

/// The player's health, once, for the log (what the switches work with).
void LogHealth() {
    static bool logged = false;
    if (logged) return;
    logged = true;
    float health = -1, regen = -1, max = -1;
    RunLua("return go_GetHealth(PLAYER_1)", &health);
    RunLua("return go_GetMaxRegenHealth(PLAYER_1)", &regen);
    RunLua("return go_GetMaxHealth(PLAYER_1)", &max);
    char text[128];
    std::snprintf(text, sizeof(text), "Alex's health %.0f, regenerates to %.0f, critical mass to %.0f", health, regen, max);
    Log(text);
}

// ---- opening and closing
//
// The screen stack holds the screens' names, not their ids: the pause menu is PAUSEMENU, the
// trainer (PROTO_PAUSE_DEBUG) DEBUG, the map MAP...; NONE in between two screens.

constexpr const char* kTrainer = "DEBUG";
constexpr const char* kPause = "PAUSEMENU";
/// The pause menu's other screens (the touchpad opens the map straight from the game): the trainer
/// takes their place.
constexpr const char* kPauseScreens[] = {"MAP", "CONTROLLERCONFIG", "EVENTS", "OBJECTIVES", "POWERPURCHASE",
                                         "STATISTICS", "PROTOPAUSEGAME", "PROTOPAUSEVIDEO", "PROTOPAUSEAUDIO",
                                         "PROTOPAUSECONTROLLER", "SAVE", "QUIT"};

enum class State { Idle, Asked, OpeningPause, Pushing, Open };
State g_state = State::Idle;
bool g_back_to_game = false; ///< leaving the trainer goes on to the game (not opened from PAUSEMENU)
int g_wait = 0;
bool g_left_web = false;

bool Is(const char* screen, const char* name) {
    return std::strcmp(screen, name) == 0;
}

bool PauseScreen(const char* screen) {
    for (const char* name : kPauseScreens)
        if (Is(screen, name)) return true;
    return false;
}

/// Between two screens: no decision on it.
bool Passing(const char* screen) {
    return !*screen || Is(screen, "NONE");
}

void Opened(bool ok) {
    Log(ok ? "opened" : "could not open (the pause menu's screens did not answer)");
}

} // namespace

namespace Native {

void TrainerFrame(void* movie, const char* screen) {
    const TheOrderGameMenu* menu = Native::Menu();
    if (!menu || !menu->trainer_open || !menu->press_options || !menu->press_pause || !Ready()) return;
    const bool trainer = Is(screen, kTrainer);
    if (menu->trainer_open()) {
        char text[96];
        std::snprintf(text, sizeof(text), "asked (screen %s)", *screen ? screen : "(none)");
        Log(text);
        if (trainer) {
            // Again: on to the game (Options on the trainer's screen is its ExitPause).
            menu->press_options();
            g_back_to_game = false;
            g_state = State::Idle;
        } else {
            g_state = State::Asked;
            g_wait = 90; // the screen it was asked on may still be coming (NONE)
            g_left_web = false;
        }
    }
    switch (g_state) {
    case State::Asked:
        if (Is(screen, "IGHUD")) {
            // The PS4 game's pause menu is the touchpad's (Options is the Web of Intrigue).
            menu->press_pause();
            g_state = State::OpeningPause;
            g_back_to_game = true;
            g_wait = 150;
        } else if (Is(screen, kPause)) {
            g_back_to_game = false;
            Opened(Invoke(movie, "PortTrainerPush"));
            g_state = State::Open;
            g_wait = 60;
        } else if (PauseScreen(screen)) {
            // Over the map (the pause menu opens on it) or another pause screen: backing out of
            // the trainer comes back to it, and then on to the game. (Taking its place left the
            // screen stack without the pause menu: backing out froze the game.)
            g_back_to_game = true;
            Opened(Invoke(movie, "PortTrainerPush"));
            g_state = State::Open;
            g_wait = 60;
        } else if (Is(screen, "WEBOFINTRIGUE") && !g_left_web) {
            // The touchpad's left side (or Options) opens the Web of Intrigue: out of it first
            // (Options), then the pause menu from the game.
            menu->press_options();
            g_left_web = true;
            g_wait = 150;
        } else if ((!Passing(screen) && !Is(screen, "WEBOFINTRIGUE")) || --g_wait <= 0) {
            char text[96];
            std::snprintf(text, sizeof(text), "not from here (screen %s)", *screen ? screen : "(none)");
            Log(text);
            g_state = State::Idle;
        }
        break;
    case State::OpeningPause:
        if (Is(screen, kPause) || PauseScreen(screen)) {
            g_state = State::Pushing;
            g_wait = 15; // the pause menu's own opening first
        } else if (--g_wait <= 0) {
            Log("the pause menu did not open");
            g_state = State::Idle;
        }
        break;
    case State::Pushing:
        if (trainer) {
            g_state = State::Open;
        } else if (!Is(screen, kPause) && !PauseScreen(screen)) {
            if (!Passing(screen)) g_state = State::Idle; // somewhere else meanwhile
        } else if (--g_wait <= 0) {
            // Over the pause menu, or the map it opens with.
            Opened(Invoke(movie, "PortTrainerPush"));
            g_state = State::Open;
            g_wait = 60;
        }
        break;
    case State::Open:
        if (trainer) {
            g_wait = 0;
            LogHealth();
        } else if (Passing(screen)) {
            // on the way in or out
        } else if (g_wait > 0) {
            --g_wait; // still on its way in
        } else {
            // Left it for the pause menu, opened from the game: on to the game (Options is the
            // pause menu's ExitPause).
            if ((Is(screen, kPause) || PauseScreen(screen)) && g_back_to_game) menu->press_options();
            g_back_to_game = false;
            g_state = State::Idle;
        }
        break;
    default: break;
    }
    Actions(menu);
    Switches(menu, Is(screen, "IGHUD"));
}

} // namespace Native
