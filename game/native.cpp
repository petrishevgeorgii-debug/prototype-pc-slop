// SPDX-License-Identifier: GPL-2.0-or-later
// port: installs the native game layer's replacements (native_registry.h, docs/NATIVE.md).

#include "game/native.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game/guest.h"
#include "game/native_registry.h"

namespace {

std::uint8_t* g_image = nullptr;
const NativeHost* g_host = nullptr; ///< during installation only: the loader's host lives on its stack
const TheOrderGameMenu* g_menu = nullptr;
void (*g_pad_effect)(int, int, float, float, unsigned, unsigned, unsigned, int) = nullptr;
void (*g_presence_source)(int (*)(char*, int)) = nullptr;
std::size_t g_data_used = 0;

std::vector<Native::Replacement>& Registry() {
    static std::vector<Native::Replacement> registry;
    return registry;
}

/// THEORDER_NATIVE: "0"/"off" turns the layer off; "-group,-name" leaves parts out.
bool Wanted(const Native::Replacement& r, bool& all_off) {
    const char* env = std::getenv("THEORDER_NATIVE");
    if (!env || !*env) {
        return true;
    }
    std::string list = env;
    if (list == "0" || list == "off") {
        all_off = true;
        return false;
    }
    std::size_t start = 0;
    while (start <= list.size()) {
        std::size_t end = list.find(',', start);
        if (end == std::string::npos) {
            end = list.size();
        }
        std::string item = list.substr(start, end - start);
        if (!item.empty() && item[0] == '-' && (item.substr(1) == r.group || item.substr(1) == r.name)) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

/// A jump to a native function, near the image: movabs r11, target; jmp r11.
std::uint8_t* Thunk(void* target) {
    std::uint8_t* code = g_host->alloc_code ? g_host->alloc_code(16) : nullptr;
    if (!code) {
        return nullptr;
    }
    const std::uintptr_t to = reinterpret_cast<std::uintptr_t>(target);
    code[0] = 0x49;
    code[1] = 0xbb;
    std::memcpy(code + 2, &to, 8);
    code[10] = 0x41;
    code[11] = 0xff;
    code[12] = 0xe3;
    return code;
}

bool Rel32(std::uint64_t site, const std::uint8_t* to, std::int32_t& out) {
    const std::int64_t delta = reinterpret_cast<std::intptr_t>(to) -
                               reinterpret_cast<std::intptr_t>(g_image + site + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        return false;
    }
    out = static_cast<std::int32_t>(delta);
    return true;
}

const char* Install(const Native::Replacement& r) {
    if (r.address + r.original.size() > g_host->image_size || r.original.empty()) {
        return "outside the game";
    }
    if (std::memcmp(g_image + r.address, r.original.data(), r.original.size()) != 0) {
        return "the game's code there differs (another build, or a patch changed it)";
    }
    switch (r.kind) {
    case Native::Kind::Function:
    case Native::Kind::CallSite: {
        // A call site is a call (e8) or a tail call (e9: jmp rel32); it stays what it was.
        if (r.original.size() < 5 ||
            (r.kind == Native::Kind::CallSite && r.original[0] != 0xe8 && r.original[0] != 0xe9)) {
            return "bad description";
        }
        std::uint8_t* thunk = Thunk(r.native);
        std::int32_t rel = 0;
        if (!thunk || !Rel32(r.address, thunk, rel)) {
            return "no memory for the jump near the game";
        }
        if (r.kind == Native::Kind::CallSite && r.callee) {
            std::int32_t old = 0;
            std::memcpy(&old, g_image + r.address + 1, 4);
            *r.callee = r.address + 5 + static_cast<std::int64_t>(old);
        }
        std::uint8_t bytes[5] = {static_cast<std::uint8_t>(r.kind == Native::Kind::CallSite ? r.original[0] : 0xe9)};
        std::memcpy(bytes + 1, &rel, 4);
        Native::WriteCode(r.address, bytes, 5);
        return nullptr;
    }
    case Native::Kind::Custom:
        return r.install && r.install(r.address) ? nullptr : "could not be installed";
    }
    return "bad description";
}

} // namespace

namespace Guest {
std::uint8_t* Image() {
    return g_image;
}
} // namespace Guest

namespace Native {

void Register(const Replacement& replacement) {
    Registry().push_back(replacement);
}

void* NearData(std::size_t bytes, std::size_t align) {
    if (!g_host || !g_host->data) {
        return nullptr;
    }
    std::size_t at = (g_data_used + align - 1) / align * align;
    if (at + bytes > g_host->data_size) {
        return nullptr;
    }
    g_data_used = at + bytes;
    return g_host->data + at;
}

void WriteCode(std::uint64_t address, const void* bytes, std::size_t size) {
    std::memcpy(g_image + address, bytes, size);
}

const TheOrderGameMenu* Menu() {
    return g_menu;
}

void Pulse(Pulses kind, float large, float small, unsigned ms, unsigned rise_ms, Texture texture) {
    Effect(kind, 0, large, small, ms, rise_ms, 0, texture);
}

void Effect(Pulses kind, int id, float large, float small, unsigned ms, unsigned rise_ms, unsigned period_ms,
            Texture texture) {
    if (g_pad_effect)
        g_pad_effect(static_cast<int>(kind), id, large, small, ms, rise_ms, period_ms, static_cast<int>(texture));
}

bool IsPlayer(std::uint32_t handle) {
    // PROTOTYPE: the player's object handle (u16 index, u16 generation) is kept at 0x13491c0.
    return g_image && handle != 0xffffffffu && (handle & 0xffff) != 0xffff &&
           handle == *reinterpret_cast<const std::uint32_t*>(g_image + 0x13491c0);
}

bool PlayerExists() {
    if (!g_image) return false;
    const std::uint32_t handle = *reinterpret_cast<const volatile std::uint32_t*>(g_image + 0x13491c0);
    const std::uint32_t index = handle & 0xffff;
    if (handle == 0xffffffffu || index == 0xffff) return false;
    // The object table: 16-byte entries {object, u16 generation}.
    const auto table = *reinterpret_cast<const volatile std::uintptr_t*>(g_image + 0x1233670);
    if (!table) return false;
    const auto* entry = reinterpret_cast<const volatile std::uint8_t*>(table + std::uintptr_t(index) * 16);
    const auto object = *reinterpret_cast<const volatile std::uintptr_t*>(entry);
    const auto generation = *reinterpret_cast<const volatile std::uint16_t*>(entry + 8);
    return object != 0 && generation == (handle >> 16);
}

void SetPresenceSource(int (*source)(char* mission, int size)) {
    if (g_presence_source) g_presence_source(source);
}

} // namespace Native

extern "C" void native_game_install(const NativeHost* host) {
    if (!host || !host->image) {
        return;
    }
    g_host = host;
    g_image = host->image;
    g_menu = host->menu; // kept: the host itself is gone once the game runs
    g_pad_effect = host->pad_effect;
    g_presence_source = host->presence_source;
    unsigned installed = 0, skipped = 0;
    bool all_off = false;
    for (const auto& r : Registry()) {
        if (!Wanted(r, all_off)) {
            if (!all_off) {
                std::printf("Native: %s [%s] left out (THEORDER_NATIVE)\n", r.name, r.group);
            }
            continue;
        }
        if (const char* why = Install(r)) {
            ++skipped;
            std::printf("Native: %s [%s] at 0x%" PRIx64 " not installed: %s\n", r.name, r.group, r.address, why);
        } else {
            ++installed;
            std::printf("Native: %s [%s] at 0x%" PRIx64 "\n", r.name, r.group, r.address);
        }
    }
    if (all_off) {
        std::printf("Native: game layer off (THEORDER_NATIVE=0)\n");
    } else {
        std::printf("Native: %u replacements installed, %u not\n", installed, skipped);
    }
    std::fflush(stdout);
}
