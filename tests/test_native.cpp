// SPDX-License-Identifier: GPL-2.0-or-later
// Native game layer (game/): installation into a stand-in of PROTOTYPE's eboot that holds only
// the original bytes the replacements check, and the native shadow distance on a stand-in
// pure3d::Shadow. Linux (build.sh --test).

#include <sys/mman.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "game/guest.h"
#include "game/native.h"
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

constexpr std::size_t kImage = 0x1300000, kCode = 0x10000, kData = 0x1000;
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

void PutFloat(std::uint64_t at, float v) { std::memcpy(g_base + at, &v, 4); }
float GetFloat(std::uint64_t at) { float v; std::memcpy(&v, g_base + at, 4); return v; }

bool Near(float a, float b) { return std::fabs(a - b) <= 0.01f * std::max(1.0f, std::fabs(b)); }

void BuildGame() {
    std::memset(g_base, 0xcc, kImage);
    Put(0x330dca, {0xe8, 0x01, 0x6b, 0xf9, 0xff, 0x41, 0x80, 0x7e, 0x58, 0x00}); // call Shadow::Update
    Put(0x344eb3, {0xe8, 0x18, 0x2a, 0xf8, 0xff, 0x80, 0x7b, 0x58, 0x00});       // call Shadow::Update
    Put(0x2c95db, {0xc5, 0xe2, 0x58, 0x1d, 0x35, 0x62, 0x9d, 0x00});             // vaddss [rip -> -200]
    // Shadow::Update stand-in: marks the shadow (byte +0x58 = 1) and returns.
    Put(0x2c78d0, {0xc6, 0x47, 0x58, 0x01, 0xc3});
    PutFloat(0x1218930, 200.0f);
    PutFloat(0xc9f818, -200.0f);
}

using Update = std::uint64_t(GUEST_ABI*)(void*, void*, void*, void*);

Update CallTarget(std::uint64_t site) {
    std::int32_t rel;
    std::memcpy(&rel, g_base + site + 1, 4);
    return reinterpret_cast<Update>(g_base + site + 5 + rel);
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
    CHECK(g_base[0x330dca] == 0xe8 && g_base[0x330dcb] == 0x01);
    CHECK(g_base[0x2c95df] == 0x35);

    // On.
    BuildGame();
    unsetenv("THEORDER_NATIVE");
    native_game_install(&host);
    CHECK(reinterpret_cast<std::uint8_t*>(CallTarget(0x330dca)) >= g_base + kImage); // to a native thunk
    CHECK(reinterpret_cast<std::uint8_t*>(CallTarget(0x344eb3)) >= g_base + kImage);
    std::int32_t disp;
    std::memcpy(&disp, g_base + 0x2c95df, 4);
    float* check = reinterpret_cast<float*>(g_base + 0x2c95e3 + disp);
    CHECK(reinterpret_cast<std::uint8_t*>(check) >= g_base + kImage + kCode && *check == -200.0f);

    // A stand-in sun shadow: 4 cascades, extents 1 2 4 24, unit divisor 40, scale 1.
    alignas(16) std::uint8_t shadow[0x400] = {};
    std::uint32_t count = 4;
    std::memcpy(shadow + 0x300, &count, 4);
    const float extents[5] = {1, 2, 4, 24, 25}, divisor = 40, scale = 1;
    std::memcpy(shadow + 0x318, extents, sizeof(extents));
    std::memcpy(shadow + 0x32c, &divisor, 4);
    std::memcpy(shadow + 0x330, &scale, 4);
    auto field = [&](std::size_t off) { float v; std::memcpy(&v, shadow + off, 4); return v; };

    // GAME: the original runs, nothing changes.
    CallTarget(0x330dca)(shadow, nullptr, nullptr, nullptr);
    CHECK(shadow[0x58] == 1);
    CHECK(field(0x318) == 1 && field(0x324) == 24 && field(0x32c) == 40 && GetFloat(0x1218930) == 200.0f);

    // 1500 m: cascades end at 5, 33.5, 224, 1500 m (extent = end / 5), box +- 1875 m.
    shadow[0x58] = 0;
    PortSettings::Get().shadow_distance = 1500;
    CallTarget(0x344eb3)(shadow, nullptr, nullptr, nullptr);
    CHECK(shadow[0x58] == 1);
    CHECK(Near(field(0x318), 1.0f) && Near(field(0x31c), 6.69f) && Near(field(0x320), 44.8f) && Near(field(0x324), 300.0f));
    CHECK(field(0x328) == 25.0f); // cascades the shadow does not use are left alone
    CHECK(Near(field(0x32c), 375.0f));
    CHECK(Near(GetFloat(0x1218930), 1875.0f) && Near(*check, -1875.0f));
    // The game's unit: 2 x range x scale / divisor = 10 m, so cascade 3 ends at 0.5 x 10 x 300.
    CHECK(Near(2.0f * GetFloat(0x1218930) * field(0x330) / field(0x32c), 10.0f));

    // Back to GAME: the game's values return.
    PortSettings::Get().shadow_distance = 0;
    CallTarget(0x330dca)(shadow, nullptr, nullptr, nullptr);
    CHECK(field(0x318) == 1 && field(0x31c) == 2 && field(0x324) == 24 && field(0x32c) == 40);
    CHECK(GetFloat(0x1218930) == 200.0f && *check == -200.0f);

    // A shadow without cascades (a spot light's) is never touched.
    alignas(16) std::uint8_t spot[0x400] = {};
    std::memcpy(spot + 0x318, extents, sizeof(extents));
    PortSettings::Get().shadow_distance = 1000;
    CallTarget(0x330dca)(spot, nullptr, nullptr, nullptr);
    float spot0;
    std::memcpy(&spot0, spot + 0x318, 4);
    CHECK(spot0 == 1.0f);

    // A build whose code differs: nothing is installed there.
    BuildGame();
    g_base[0x330dcb] = 0x02;
    native_game_install(&host);
    CHECK(g_base[0x330dca] == 0xe8 && g_base[0x330dcb] == 0x02);

    if (failures) {
        std::fprintf(stderr, "native test: %d failures\n", failures);
        return 1;
    }
    std::puts("native test: ok");
    return 0;
}
