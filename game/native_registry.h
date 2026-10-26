// SPDX-License-Identifier: GPL-2.0-or-later
// port: how a native C++ part of the game replaces the original code (docs/NATIVE.md).
//
// Every replacement names the original code it takes over and the exact bytes that code has in
// the game build it was written for. It is installed only when those bytes are found unchanged:
// another build of the game, or a patch that changed the same code, leaves the original running.
//
//   Function   the whole function: its first instruction jumps to the native one
//   CallSite   one call (or tail call: jmp) to a function: it goes to the native one instead,
//              which can call the original (`callee`) before or after its own work
//   Custom     anything else (an operand redirected to native data...): `install` does it

#pragma once

#include <cstdint>
#include <vector>

#include "gpu/theordergpu.h"

namespace Native {

enum class Kind { Function, CallSite, Custom };

struct Site;

struct Replacement {
    const char* name;  ///< what it replaces, e.g. "pure3d::Shadow::Update (call from the renderer)"
    const char* group; ///< the subsystem, e.g. "shadows": THEORDER_NATIVE=-shadows leaves it out
    Kind kind;
    std::uint64_t address;               ///< the site (virtual address)
    std::vector<std::uint8_t> original;  ///< its bytes in the build this was written for
    void* native = nullptr;              ///< Function, CallSite: the GUEST_ABI replacement
    std::uint64_t* callee = nullptr;     ///< CallSite: receives the original callee's address
    bool (*install)(std::uint64_t address) = nullptr; ///< Custom
};

/// Adds a replacement (static initialisation, see NATIVE_REPLACEMENT).
void Register(const Replacement& replacement);

/// Native memory within 2 GiB of the game's code, for redirected rip-relative operands.
/// Valid during installation; nullptr when the host gave none or it is used up.
void* NearData(std::size_t bytes, std::size_t align = 16);

/// Writes `bytes` over the game's code at `address` (installation only).
void WriteCode(std::uint64_t address, const void* bytes, std::size_t size);

/// The port's settings for the game's own menus (gpu/theordergpu.h), or nullptr.
const TheOrderGameMenu* Menu();

/// What a vibration pulse is for: the player's options decide which ones are felt.
enum class Pulses { Game = 0, MenuPress = 1, MenuMove = 2 };

/// How a vibration feels on a DualSense's HD haptics (USB, src/runtime_haptics.c); the plain
/// rumble motors only follow its levels. As src/runtime.h RUNTIME_TEXTURE_*.
enum class Texture { Plain = 0, Crack, Thud, Tug, Step, Scrape, Swell, Hit, Click };

/// A vibration added to the game's own (src/runtime_pad.c runtime_pad_effect): motor levels 0..1
/// (large: the low rumble, small: the high one), `ms` long; `rise_ms` 0: a tap, else a swell
/// rising over that time. `ms` 0 ends the current one. Nothing without a host for it.
void Pulse(Pulses kind, float large, float small, unsigned ms, unsigned rise_ms = 0, Texture texture = Texture::Plain);

/// The same with an id (the next effect with it replaces it; `ms` 0 ends it with a short fade) and
/// a rhythm: a beat every `period_ms` over a steady 40%.
void Effect(Pulses kind, int id, float large, float small, unsigned ms, unsigned rise_ms = 0, unsigned period_ms = 0,
            Texture texture = Texture::Plain);

/// PROTOTYPE: whether an object handle (u16 index, u16 generation, as the game keeps them) is the
/// player's.
bool IsPlayer(std::uint32_t handle);

struct Registrar {
    explicit Registrar(const Replacement& replacement) { Register(replacement); }
};

} // namespace Native

#define NATIVE_CONCAT2(a, b) a##b
#define NATIVE_CONCAT(a, b) NATIVE_CONCAT2(a, b)
/// NATIVE_REPLACEMENT({...}); at namespace scope in a game/<game>/*.cpp file.
#define NATIVE_REPLACEMENT(...) \
    static const ::Native::Registrar NATIVE_CONCAT(native_registrar_, __LINE__){::Native::Replacement __VA_ARGS__}
