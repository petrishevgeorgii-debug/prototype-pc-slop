// SPDX-License-Identifier: GPL-2.0-or-later
// port: the render map's GPU side (docs/RENDERER.md). game/p1/render_map.cpp marks every
// Pure3D pass in the game's command buffers (debug marker NOPs); the command processor
// (liverpool.cpp) hands them here with the draws between them and the flips. Each frame is a
// list of passes in the order they ran (nested: "RootPass > ProtoLitRootPass > ProtoOpaquePass")
// with the draws each made itself and the pixel shaders they used (by the hash shader mods are
// named after, the most used first); the log shows a frame's list now and then:
//
//   Render map: frame 300: 41 passes, 2310 draws
//     RootPass > ProtoShadowPass > Shadows          688  (3 shaders: 8aaeb600 x650 ...)
//
// Called from the command processor's thread only.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "port_render_map.h"

namespace PortRenderMap {
namespace {

/// Draws from one caller of ps4Context's DrawPrimGroup(s) in one pass.
struct Caller {
    std::uint32_t slot = 0, caller = 0; ///< 4 or 5; the return address (image-relative)
    unsigned draws = 0, calls = 0;
    std::vector<std::uint64_t> materials; ///< distinct primitive group shaders (up to 256)
    std::vector<std::pair<std::uint32_t, unsigned>> classes; ///< their vtables, calls each
    std::vector<std::pair<std::uint64_t, unsigned>> templates; ///< shader templates (name hashes), draws each
    std::vector<std::pair<std::uint64_t, unsigned>> vertex;    ///< vertex shader hashes, draws each
};

struct Entry {
    std::string path;
    unsigned draws = 0;
    bool resumed = false; ///< the parent again after a child pass
    std::vector<std::pair<std::uint64_t, unsigned>> shaders; ///< pixel shader hash, its draws
    std::vector<Caller> callers;       ///< the draw tags' callers (game/p1/render_map.cpp)
    unsigned untagged = 0;              ///< draws outside every tag
};

struct Tagged {
    bool on = false;
    std::uint32_t slot = 0, caller = 0, shader_class = 0, template_name = 0;
    std::uint64_t shader = 0;
} tag; ///< the draw tag open now

void Count(std::vector<std::pair<std::uint64_t, unsigned>>& counts, std::uint64_t key) {
    for (auto& [k, n] : counts) {
        if (k == key) {
            ++n;
            return;
        }
    }
    counts.emplace_back(key, 1);
}

std::unordered_map<std::uint32_t, std::string> names; ///< shader template name hash -> name

/// What a draw is, from its shader template's name (the kinds the log sums up per frame).
enum Kind { kWorld, kRoads, kAlex, kPeople, kVehicles, kDecals, kDistant, kSky, kWater, kEffects, kUnlit, kBatched,
            kUnnamed, kKinds };
constexpr const char* kKindNames[kKinds] = {"world",  "roads", "Alex", "people", "vehicles", "decals", "distant city",
                                            "sky",    "water", "effects", "unlit", "depth batches", "unnamed"};
std::unordered_map<std::uint32_t, int> kinds; ///< template name hash -> Kind
unsigned frame_kinds[kKinds];
int last_kind = -1; ///< the last draw's kind (-1: not tagged), for the scene capture

int KindOf(std::uint32_t slot, std::uint32_t hash) {
    if (slot == 5) return kBatched; // one call, many groups: the tag has the first group's template only
    const auto known = kinds.find(hash);
    if (known != kinds.end()) return known->second;
    const auto it = names.find(hash);
    if (it == names.end()) return kUnnamed;
    std::string n = it->second;
    for (auto& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (n.rfind("zcbv2_", 0) == 0) n = n.substr(6);
    auto has = [&](const char* t) { return n.find(t) != std::string::npos; };
    int k = kWorld;
    if (n.rfind("char_alex", 0) == 0) k = kAlex;
    else if (n.rfind("char_", 0) == 0) k = kPeople;
    else if (has("vehicle") || has("car_tire")) k = kVehicles;
    else if (has("decal")) k = kDecals;
    else if (n.rfind("env_midground", 0) == 0) k = kDistant;
    else if (n.rfind("env_sky", 0) == 0) k = kSky;
    else if (n.rfind("env_water", 0) == 0) k = kWater;
    else if (n.rfind("fx_", 0) == 0 || n.rfind("pfx_", 0) == 0 || n.rfind("pp_", 0) == 0) k = kEffects;
    else if (has("road") || has("sidewalk")) k = kRoads;
    else if (n == "unlit") k = kUnlit;
    kinds.emplace(hash, k);
    return k;
}

void Top(const char* what, std::vector<std::pair<std::uint64_t, unsigned>> counts, size_t most, bool named = false) {
    std::sort(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::printf("        %s (%zu):", what, counts.size());
    for (size_t i = 0; i < counts.size() && i < most; ++i) {
        const auto name = named ? names.find(std::uint32_t(counts[i].first)) : names.end();
        if (name != names.end()) {
            std::printf(" %s x%u", name->second.c_str(), counts[i].second);
        } else {
            std::printf(" %llx x%u", static_cast<unsigned long long>(counts[i].first), counts[i].second);
        }
    }
    std::printf("%s\n", counts.size() > most ? " ..." : "");
}

/// The caller entry for the open tag in this entry.
Caller& CallerOf(Entry& entry) {
    for (auto& c : entry.callers) {
        if (c.slot == tag.slot && c.caller == tag.caller) return c;
    }
    entry.callers.push_back({});
    auto& c = entry.callers.back();
    c.slot = tag.slot;
    c.caller = tag.caller;
    return c;
}

std::unordered_map<std::uint64_t, std::uint64_t> hashes; ///< pixel shader address -> its hash

/// The shader's hash (the one shader mods are named after: mods\<hash>_p0.frag), from the binary
/// information block after its code ("OrbShdr", the hash 16 bytes in), as the shader cache reads it.
std::uint64_t Hash(std::uint64_t address) {
    if (!address) return 0;
    const auto known = hashes.find(address);
    if (known != hashes.end()) return known->second;
    const auto* code = reinterpret_cast<const std::uint32_t*>(address);
    std::uint64_t hash = 0;
    auto at = [&](const std::uint32_t* p) {
        if (std::memcmp(p, "OrbShdr", 7) != 0) return false;
        std::memcpy(&hash, reinterpret_cast<const std::uint8_t*>(p) + 16, sizeof(hash));
        return true;
    };
    if (!(code[0] == 0xBEEB03FFu && at(code + (code[1] + 1) * 2))) {
        for (const std::uint32_t* p = code; p < code + 0x4000; ++p) {
            if (at(p)) break;
        }
    }
    hashes.emplace(address, hash);
    return hash;
}

std::vector<std::string> stack;  ///< the passes open now
std::vector<Entry> frame;        ///< this frame's passes, in order
unsigned loose = 0;              ///< draws outside every pass
unsigned frames = 0;

std::string Path() {
    std::string path;
    for (const auto& name : stack) {
        if (!path.empty()) path += " > ";
        path += name;
    }
    return path;
}

void Log() {
    unsigned draws = loose;
    for (const auto& e : frame) draws += e.draws;
    std::printf("Render map: frame %u: %zu passes, %u draws (%u outside them)\n", frames, frame.size(), draws, loose);
    std::printf("Render map: frame %u by kind:", frames);
    for (int k = 0; k < kKinds; ++k) {
        if (frame_kinds[k]) std::printf(" %s %u,", kKindNames[k], frame_kinds[k]);
    }
    std::printf(" the rest %u (full-screen passes, HUD, menus)\n", draws - [] {
        unsigned n = 0;
        for (unsigned k : frame_kinds) n += k;
        return n;
    }());
    for (auto e : frame) {
        std::printf("  %-90s %u", e.path.c_str(), e.draws);
        if (!e.shaders.empty()) {
            std::sort(e.shaders.begin(), e.shaders.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::printf("  (%zu shaders:", e.shaders.size());
            for (size_t i = 0; i < e.shaders.size() && i < 4; ++i) {
                std::printf(" %llx x%u", static_cast<unsigned long long>(e.shaders[i].first), e.shaders[i].second);
            }
            std::printf("%s)", e.shaders.size() > 4 ? " ..." : "");
        }
        std::printf("\n");
        std::sort(e.callers.begin(), e.callers.end(), [](const Caller& a, const Caller& b) { return a.draws > b.draws; });
        for (auto& c : e.callers) {
            std::sort(c.classes.begin(), c.classes.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            std::printf("      draw %u from %x: %u draws in %u calls, %zu%s materials, class", c.slot, c.caller, c.draws,
                        c.calls, c.materials.size(), c.materials.size() >= 256 ? "+" : "");
            for (size_t i = 0; i < c.classes.size() && i < 3; ++i) {
                std::printf(" %x x%u", c.classes[i].first, c.classes[i].second);
            }
            std::printf("\n");
            if (!c.templates.empty()) Top(c.slot == 5 ? "template (of the first group)" : "templates", c.templates, 8, true);
            if (!c.vertex.empty()) Top("vertex shaders", c.vertex, 8);
        }
        if (e.untagged && !e.callers.empty()) std::printf("      untagged: %u draws\n", e.untagged);
    }
    std::fflush(stdout);
}

} // namespace

bool Enabled() {
    static const bool on = [] {
        const char* env = std::getenv("THEORDER_RENDER_MAP");
        return env && std::strcmp(env, "1") == 0;
    }();
    return on;
}

void Push(std::string_view name) {
    const auto end = name.find('\0');
    if (end != std::string_view::npos) name = name.substr(0, end);
    stack.emplace_back(name);
    frame.push_back({Path(), 0, false, {}, {}, 0});
    tag.on = false;
    if (frame.size() > 20000) Frame(); // no flips seen: a list never ends otherwise
}

void Pop() {
    if (!stack.empty()) stack.pop_back();
    // Draws after a child pass ends belong to its parent again: a new entry for the parent.
    if (!stack.empty()) frame.push_back({Path(), 0, true, {}, {}, 0});
    tag.on = false;
}

void Tag(std::uint32_t slot, std::uint32_t caller, std::uint64_t shader, std::uint32_t shader_class,
         std::uint32_t template_name) {
    tag = {true, slot, caller, shader_class, template_name, shader};
    if (stack.empty() || frame.empty()) return;
    auto& c = CallerOf(frame.back());
    ++c.calls;
    if (c.materials.size() < 256 && std::find(c.materials.begin(), c.materials.end(), shader) == c.materials.end()) {
        c.materials.push_back(shader);
    }
    for (auto& [k, n] : c.classes) {
        if (k == shader_class) {
            ++n;
            return;
        }
    }
    c.classes.emplace_back(shader_class, 1);
}

void TagEnd() {
    tag.on = false;
}

int CurrentKind() {
    return last_kind;
}

const char* KindName(int kind) {
    return kind >= 0 && kind < kKinds ? kKindNames[kind] : "?";
}

int KindCount() {
    return kKinds;
}

bool Solid(int kind) {
    return kind == kWorld || kind == kRoads || kind == kAlex || kind == kPeople || kind == kVehicles;
}

void Name(std::uint32_t hash, std::string_view name) {
    const auto end = name.find('\0');
    if (end != std::string_view::npos) name = name.substr(0, end);
    if (!hash || name.empty()) return;
    names[hash] = std::string(name);
    kinds.erase(hash);
}

void Draw(std::uint64_t ps_address, std::uint64_t vs_address) {
    last_kind = -1;
    if (stack.empty() || frame.empty()) {
        ++loose;
        return;
    }
    auto& entry = frame.back();
    ++entry.draws;
    if (tag.on) {
        auto& c = CallerOf(entry);
        ++c.draws;
        Count(c.templates, tag.template_name);
        last_kind = KindOf(tag.slot, tag.template_name);
        ++frame_kinds[last_kind];
        Count(c.vertex, Hash(vs_address));
    } else {
        ++entry.untagged;
    }
    const std::uint64_t hash = Hash(ps_address);
    for (auto& [h, draws] : entry.shaders) {
        if (h == hash) {
            ++draws;
            return;
        }
    }
    entry.shaders.emplace_back(hash, 1);
}

void Frame() {
    ++frames;
    // A parent resumed between two children without drawing is left out of the list.
    frame.erase(std::remove_if(frame.begin(), frame.end(), [](const Entry& e) { return e.resumed && !e.draws; }),
                frame.end());
    if (frames == 300 || frames == 1200 || frames == 2400 || frames % 3600 == 0) Log();
    frame.clear();
    loose = 0;
    for (auto& k : frame_kinds) k = 0;
    if (stack.size() > 64) stack.clear(); // markers lost: start over
    if (!stack.empty()) frame.push_back({Path(), 0, true, {}, {}, 0});
    tag.on = false;
}

} // namespace PortRenderMap
