// SPDX-License-Identifier: GPL-2.0-or-later
// port: the render map's GPU side (docs/RENDERER.md). game/p1/render_map.cpp marks every
// Pure3D pass in the game's command buffers (debug marker NOPs); the command processor
// (liverpool.cpp) hands them here with the draws between them and the flips. Each frame is a
// list of passes in the order they ran (nested: "RootPass > ProtoLitRootPass > ProtoOpaquePass")
// with the draws each made itself; the log shows a frame's list now and then:
//
//   Render map: frame 300: 41 passes, 2310 draws
//     RootPass > ProtoZPass                         412
//     RootPass > ProtoShadowPass                    880 ...
//
// Called from the command processor's thread only.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "port_render_map.h"

namespace PortRenderMap {
namespace {

struct Entry {
    std::string path;
    unsigned draws = 0;
    bool resumed = false; ///< the parent again after a child pass
};

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
    for (const auto& e : frame) std::printf("  %-90s %u\n", e.path.c_str(), e.draws);
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
    frame.push_back({Path(), 0, false});
    if (frame.size() > 20000) Frame(); // no flips seen: a list never ends otherwise
}

void Pop() {
    if (!stack.empty()) stack.pop_back();
    // Draws after a child pass ends belong to its parent again: a new entry for the parent.
    if (!stack.empty()) frame.push_back({Path(), 0, true});
}

void Draw() {
    if (stack.empty() || frame.empty()) {
        ++loose;
        return;
    }
    ++frame.back().draws;
}

void Frame() {
    ++frames;
    // A parent resumed between two children without drawing is left out of the list.
    frame.erase(std::remove_if(frame.begin(), frame.end(), [](const Entry& e) { return e.resumed && !e.draws; }),
                frame.end());
    if (frames == 300 || frames == 1200 || frames % 3600 == 0) Log();
    frame.clear();
    loose = 0;
    if (stack.size() > 64) stack.clear(); // markers lost: start over
    if (!stack.empty()) frame.push_back({Path(), 0, true});
}

} // namespace PortRenderMap
