// SPDX-License-Identifier: GPL-2.0-or-later
#include "port_shadow_fade.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "port_settings.h"

namespace PortShadowFade {
namespace {

constexpr std::uint32_t kScale = 0x3ca3d70au;  // 0.02
constexpr std::uint32_t kOffset = 0xc0400000u; // -3.0
constexpr std::size_t kWindow = 8;             // dwords between the two
constexpr float kDistances[] = {0.0f, 250.0f, 500.0f, 1000.0f};

/// A literal operand: the dword before it is an instruction whose source field says "literal"
/// (0xff in its low byte: SOP1/SOP2/VOP1/VOP2 src0).
bool Literal(std::span<const std::uint32_t> code, std::size_t i) {
    return i > 0 && (code[i - 1] & 0xffu) == 0xffu;
}

/// The places of the fade's two literals (scale, offset), every pair in the code.
std::vector<std::pair<std::size_t, std::size_t>> Find(std::span<const std::uint32_t> code) {
    std::vector<std::pair<std::size_t, std::size_t>> found;
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i] != kScale || !Literal(code, i)) continue;
        const std::size_t lo = i > kWindow ? i - kWindow : 0;
        const std::size_t hi = std::min(code.size(), i + kWindow + 1);
        for (std::size_t j = lo; j < hi; ++j) {
            if (code[j] == kOffset && Literal(code, j)) {
                found.emplace_back(i, j);
                break;
            }
        }
    }
    return found;
}

std::mutex g_mutex;
std::unordered_map<std::uint64_t, bool> g_has_fade;

} // namespace

std::uint32_t Level(std::uint64_t hash, std::span<const std::uint32_t> code) {
    const int level = std::clamp(PortSettings::Get().shadow_distance.load(std::memory_order_relaxed), 0, 3);
    if (level == 0 || code.empty()) {
        return 0;
    }
    std::lock_guard lock{g_mutex};
    auto it = g_has_fade.find(hash);
    if (it == g_has_fade.end()) {
        const auto found = Find(code);
        it = g_has_fade.emplace(hash, !found.empty()).first;
        if (!found.empty()) {
            static int logged = 0;
            if (logged < 8) {
                ++logged;
                std::printf("Shadow distance: pixel shader %llx fades its shadows (%zu place%s)%s\n",
                            static_cast<unsigned long long>(hash), found.size(), found.size() == 1 ? "" : "s",
                            logged == 8 ? "; more not logged" : "");
                std::fflush(stdout);
            }
        }
    }
    return it->second ? std::uint32_t(level) : 0;
}

std::span<const std::uint32_t> Apply(std::span<const std::uint32_t> code, std::uint32_t level,
                                     std::vector<std::uint32_t>& storage) {
    if (level == 0 || level > 3) {
        return code;
    }
    const auto found = Find(code);
    if (found.empty()) {
        return code;
    }
    // saturate(d * scale + offset) from 0 at 85% of the distance to 1 at the distance.
    const float distance = kDistances[level];
    const float scale = 1.0f / (0.15f * distance);
    const float offset = -0.85f / 0.15f;
    std::uint32_t scale_bits, offset_bits;
    std::memcpy(&scale_bits, &scale, 4);
    std::memcpy(&offset_bits, &offset, 4);
    storage.assign(code.begin(), code.end());
    for (const auto& [s, o] : found) {
        storage[s] = scale_bits;
        storage[o] = offset_bits;
    }
    return storage;
}

} // namespace PortShadowFade
