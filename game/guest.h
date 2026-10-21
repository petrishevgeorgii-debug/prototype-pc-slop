// SPDX-License-Identifier: GPL-2.0-or-later
// port: access to the game's own code and data from native C++ (docs/NATIVE.md).
//
// The game's code runs directly on the CPU in this process, so its objects are ordinary memory
// and its functions ordinary code: addresses are the eboot's virtual addresses (the ones in
// game/p1/known_functions.txt and out/game_map) plus the image base. The game uses the System V
// calling convention (the PS4's) also on Windows: every function the game calls, or that calls
// the game, is GUEST_ABI.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#define GUEST_ABI __attribute__((sysv_abi))

namespace Guest {

/// The eboot's image (virtual address 0).
std::uint8_t* Image();

/// A global of the game at virtual address `vaddr`.
template <class T>
inline T& At(std::uint64_t vaddr) {
    return *reinterpret_cast<T*>(Image() + vaddr);
}

/// A field of one of the game's objects, at a byte offset.
template <class T>
inline T& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
}

/// Calls the game's function at virtual address `vaddr`.
template <class R, class... Args>
inline R Call(std::uint64_t vaddr, Args... args) {
    using Function = R(GUEST_ABI*)(Args...);
    return reinterpret_cast<Function>(Image() + vaddr)(args...);
}

} // namespace Guest
