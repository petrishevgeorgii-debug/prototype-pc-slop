// SPDX-License-Identifier: GPL-2.0-or-later
// port: the scene capture, step 3 of docs/RENDERER.md (towards ray tracing). Every so often a
// frame's solid draws (world, roads, Alex, people, vehicles: the render map's kinds, from the
// game's shader templates) use a variant of their vertex shader that also stores each vertex's
// clip position (EmitVertexCapture). The positions and the draws' indices make that frame's
// triangles, as the game placed and animated them. For now they are only checked and logged,
// and the first captures are written to scene_capture_<n>.ply (binary PLY: x, y and w of the
// clip position, so the camera's view up to a scale per axis; a face's kind as a property).
// THEORDER_SCENE_CAPTURE=1 (with THEORDER_RENDER_MAP=1, as test builds of the launcher set).

#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

class Instance;
class Scheduler;

class SceneCapture {
public:
    SceneCapture(const Instance& instance, Scheduler& scheduler);
    ~SceneCapture();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }
    /// A capture frame is being drawn (the draw pipeline is off meanwhile: the capture decides
    /// per draw, on the command processor's thread).
    [[nodiscard]] bool Active() const noexcept {
        return active;
    }
    /// Whether the draw just parsed is to be captured (the render map's current kind).
    [[nodiscard]] bool Wanted() const;
    /// A wanted draw drew without capturing: its capture pipeline was still building, or the
    /// draw cannot have one (not a triangle list of a plain vertex shader).
    void NoteMissed(bool building);

    struct Draw {
        const void* indices; ///< guest index data (nullptr: not indexed)
        u32 index_size;      ///< 2 or 4
        u32 count;           ///< indices (or vertices when not indexed)
        s32 base_vertex;
        bool restart;
        u32 first_instance;
        u32 instances;
    };
    /// A captured draw: its parameter index for the vertex shader (push constant), 0 when it
    /// does not fit.
    u32 PrepareDraw(const Draw& draw);

    /// A flip parsed (the command processor's thread): starts, ends and checks captures.
    void OnFlip();

private:
    struct Record {
        int kind;
        u32 store_base;
        u32 vertices;
        u32 instances;
        std::vector<u32> triangles; ///< position indices (store base included), 3 per triangle
    };
    void Analyze();

    const Instance& instance;
    Scheduler& scheduler;
    bool enabled = false;
    bool active = false;
    bool pending = false;
    u64 pending_tick = 0;
    u64 frames = 0;
    u64 next_capture = 900;
    u32 captures = 0;

    static constexpr u32 MaxParams = 16384;
    static constexpr u32 MaxVertices = 2u << 20;
    vk::Buffer params_buffer{};
    VmaAllocation params_allocation{};
    u32* params{};
    vk::Buffer positions_buffer{};
    VmaAllocation positions_allocation{};
    float* positions{};
    u32 params_used = 1;
    u32 vertices_used = 1;
    std::vector<Record> records;
    u32 missed_building = 0, missed_other = 0, full = 0, skipped = 0;
    std::atomic<bool> writing{false};
};

} // namespace Vulkan
