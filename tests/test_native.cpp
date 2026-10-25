// SPDX-License-Identifier: GPL-2.0-or-later
// Native game layer (game/): installation into a stand-in game image. A test replacement takes
// over one call: it is installed only where the original bytes are found, can be turned off with
// THEORDER_NATIVE, and calls the original through the address it is handed. Linux
// (build.sh --test).

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

constexpr std::size_t kImage = 0x10000, kCode = 0x10000, kData = 0x1000;
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

void BuildGame() {
    std::memset(g_base, 0xcc, kImage);
    Put(kCallee, {0xc6, 0x07, 0x01, 0xc3});                     // mov byte [rdi], 1; ret
    Put(kSite, {0xe8, 0xfb, 0xef, 0xff, 0xff, 0x90, 0x90});     // call kCallee
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
    NativeHost host{g_base, kImage, AllocCode, g_base + kImage + kCode, kData};

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

    if (failures) {
        std::fprintf(stderr, "native test: %d failures\n", failures);
        return 1;
    }
    std::puts("native test: ok");
    return 0;
}
