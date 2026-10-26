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

struct Registrar {
    explicit Registrar(const Replacement& replacement) { Register(replacement); }
};

} // namespace Native

#define NATIVE_CONCAT2(a, b) a##b
#define NATIVE_CONCAT(a, b) NATIVE_CONCAT2(a, b)
/// NATIVE_REPLACEMENT({...}); at namespace scope in a game/<game>/*.cpp file.
#define NATIVE_REPLACEMENT(...) \
    static const ::Native::Registrar NATIVE_CONCAT(native_registrar_, __LINE__){::Native::Replacement __VA_ARGS__}
