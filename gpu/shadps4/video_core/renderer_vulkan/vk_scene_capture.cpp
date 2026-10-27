// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_scene_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <type_traits>

#include "port_render_map.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

bool CreateBuffer(const Instance& instance, vk::DeviceSize size, VmaAllocationCreateFlags flags,
                  vk::Buffer& buffer, VmaAllocation& allocation, void*& mapped, u64& address) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = flags | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    };
    VkBuffer raw{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &raw, &allocation, &info) !=
        VK_SUCCESS) {
        return false;
    }
    buffer = raw;
    mapped = info.pMappedData;
    address = instance.GetDevice().getBufferAddress({.buffer = buffer});
    return mapped && address;
}

bool Written(const float* p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && std::isfinite(p[3]);
}

} // namespace

SceneCapture::SceneCapture(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const char* env = std::getenv("THEORDER_SCENE_CAPTURE");
    if (!env || std::strcmp(env, "1") != 0) {
        return;
    }
    if (!PortRenderMap::Enabled()) {
        std::printf("Scene capture: off (it needs the render map, THEORDER_RENDER_MAP=1)\n");
        return;
    }
    if (!instance.GetPhysicalDevice().getFeatures().vertexPipelineStoresAndAtomics) {
        std::printf("Scene capture: off (vertexPipelineStoresAndAtomics unsupported)\n");
        return;
    }
    void* mapped_params = nullptr;
    void* mapped_positions = nullptr;
    u64 params_address = 0, positions_address = 0;
    if (!CreateBuffer(instance, vk::DeviceSize(MaxParams) * 16,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, params_buffer,
                      params_allocation, mapped_params, params_address) ||
        !CreateBuffer(instance, vk::DeviceSize(MaxVertices) * 16,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, positions_buffer,
                      positions_allocation, mapped_positions, positions_address)) {
        std::printf("Scene capture: off (buffer creation failed)\n");
        return;
    }
    params = static_cast<u32*>(mapped_params);
    positions = static_cast<float*>(mapped_positions);
    std::memset(params, 0, 16); // parameter 0: off
    Shader::SceneCapture::params_address = params_address;
    Shader::SceneCapture::positions_address = positions_address;
    enabled = true;
    std::printf("Scene capture: on (a frame every 30 s from frame 900, up to %u vertices)\n",
                MaxVertices);
    std::fflush(stdout);
}

SceneCapture::~SceneCapture() {
    while (writing.load()) {
        std::this_thread::yield();
    }
    const auto allocator = instance.GetAllocator();
    if (params_buffer) {
        vmaDestroyBuffer(allocator, params_buffer, params_allocation);
    }
    if (positions_buffer) {
        vmaDestroyBuffer(allocator, positions_buffer, positions_allocation);
    }
}

bool SceneCapture::Wanted() const {
    return active && PortRenderMap::Solid(PortRenderMap::CurrentKind());
}

void SceneCapture::NoteMissed(bool building) {
    ++(building ? missed_building : missed_other);
}

u32 SceneCapture::PrepareDraw(const Draw& draw) {
    if (!active) {
        return 0;
    }
    if (params_used >= MaxParams) {
        ++full;
        return 0;
    }
    // The vertices this draw runs (indexed: only those its indices reach).
    s64 first = draw.base_vertex;
    u32 count = draw.count;
    if (draw.indices) {
        u32 lo = std::numeric_limits<u32>::max(), hi = 0;
        const auto scan = [&](const auto* data) {
            using Index = std::remove_cvref_t<decltype(*data)>;
            for (u32 i = 0; i < draw.count; ++i) {
                const u32 index = data[i];
                if (draw.restart && index == std::numeric_limits<Index>::max()) continue;
                lo = std::min(lo, index);
                hi = std::max(hi, index);
            }
        };
        if (draw.index_size == 2) {
            scan(static_cast<const u16*>(draw.indices));
        } else {
            scan(static_cast<const u32*>(draw.indices));
        }
        if (lo > hi) {
            ++skipped;
            return 0;
        }
        first = s64(lo) + draw.base_vertex;
        count = hi - lo + 1;
    }
    if (count == 0 || count > (1u << 20) || first < 0 || first > std::numeric_limits<s32>::max()) {
        ++skipped;
        return 0;
    }
    if (u64(vertices_used) + count > MaxVertices) {
        ++full;
        return 0;
    }
    Record record{PortRenderMap::CurrentKind(), vertices_used, count, draw.instances, {}};
    record.triangles.reserve(draw.count - draw.count % 3);
    for (u32 i = 0; i + 2 < draw.count; i += 3) {
        u32 slot[3];
        bool ok = true;
        for (u32 k = 0; k < 3; ++k) {
            s64 vertex = i + k;
            if (draw.indices) {
                const u32 index = draw.index_size == 2 ? static_cast<const u16*>(draw.indices)[i + k]
                                                       : static_cast<const u32*>(draw.indices)[i + k];
                if (draw.restart && index == (draw.index_size == 2 ? 0xffffu : 0xffffffffu)) ok = false;
                vertex = s64(index) + draw.base_vertex - first;
            }
            if (vertex < 0 || vertex >= count) ok = false;
            slot[k] = record.store_base + u32(vertex);
        }
        if (ok) record.triangles.insert(record.triangles.end(), slot, slot + 3);
    }
    const u32 index = params_used++;
    params[index * 4 + 0] = vertices_used;
    params[index * 4 + 1] = count;
    params[index * 4 + 2] = u32(first);
    params[index * 4 + 3] = draw.first_instance;
    vertices_used += count;
    records.push_back(std::move(record));
    return index;
}

void SceneCapture::OnFlip() {
    if (!enabled) {
        return;
    }
    ++frames;
    if (active) {
        active = false;
        // The vertex shaders' stores, for the host to read once this work is done.
        scheduler.EndRendering();
        scheduler.Record([](vk::CommandBuffer cmd) {
            const vk::MemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            };
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        });
        pending_tick = scheduler.CurrentTick();
        pending = true;
        return;
    }
    if (pending) {
        if (scheduler.IsFree(pending_tick)) {
            pending = false;
            Analyze();
        }
        return;
    }
    if (frames >= next_capture && !writing.load()) {
        // Unwritten positions stay NaN.
        std::memset(positions, 0xff, size_t(MaxVertices) * 16);
        params_used = 1;
        vertices_used = 1;
        records.clear();
        missed_building = missed_other = full = skipped = 0;
        active = true;
        next_capture = frames + 1800;
    }
}

void SceneCapture::Analyze() {
    vmaInvalidateAllocation(instance.GetAllocator(), positions_allocation, 0, VK_WHOLE_SIZE);
    ++captures;
    const int kinds = PortRenderMap::KindCount();
    struct Stats {
        u32 draws = 0, instanced = 0;
        u64 vertices = 0, written = 0, triangles = 0, complete = 0, front = 0, in_view = 0;
        float w_min = std::numeric_limits<float>::max(), w_max = 0.0f;
    };
    std::vector<Stats> stats(kinds);
    std::vector<u8> vertex_written(vertices_used, 0);
    for (u32 v = 1; v < vertices_used; ++v) {
        vertex_written[v] = Written(positions + size_t(v) * 4);
    }
    for (const auto& r : records) {
        if (r.kind < 0 || r.kind >= kinds) continue;
        auto& s = stats[r.kind];
        ++s.draws;
        s.instanced += r.instances > 1;
        s.vertices += r.vertices;
        for (u32 v = r.store_base; v < r.store_base + r.vertices; ++v) {
            s.written += vertex_written[v];
        }
        for (size_t t = 0; t + 2 < r.triangles.size(); t += 3) {
            ++s.triangles;
            const u32* tri = &r.triangles[t];
            if (!vertex_written[tri[0]] || !vertex_written[tri[1]] || !vertex_written[tri[2]]) continue;
            ++s.complete;
            bool front = true, inside = false;
            for (u32 k = 0; k < 3; ++k) {
                const float* p = positions + size_t(tri[k]) * 4;
                if (p[3] <= 0.0f) {
                    front = false;
                    continue;
                }
                s.w_min = std::min(s.w_min, p[3]);
                s.w_max = std::max(s.w_max, p[3]);
                inside |= std::fabs(p[0]) <= p[3] && std::fabs(p[1]) <= p[3];
            }
            s.front += front;
            s.in_view += front && inside;
        }
    }
    u64 total_triangles = 0;
    std::printf("Scene capture %u (frame %llu): %zu draws, %u vertices; missed %u (pipeline building) + "
                "%u (no capture variant), %u skipped, %u over capacity\n",
                captures, static_cast<unsigned long long>(frames), records.size(), vertices_used - 1,
                missed_building, missed_other, skipped, full);
    for (int k = 0; k < kinds; ++k) {
        const auto& s = stats[k];
        if (!s.draws) continue;
        total_triangles += s.complete;
        std::printf("  %-14s %5u draws (%u instanced), %7llu vertices (%llu written), %7llu triangles "
                    "(%llu complete, %llu in front, %llu in view), w %.2f..%.2f\n",
                    PortRenderMap::KindName(k), s.draws, s.instanced,
                    static_cast<unsigned long long>(s.vertices), static_cast<unsigned long long>(s.written),
                    static_cast<unsigned long long>(s.triangles), static_cast<unsigned long long>(s.complete),
                    static_cast<unsigned long long>(s.front), static_cast<unsigned long long>(s.in_view),
                    s.w_max > 0 ? s.w_min : 0.0f, s.w_max);
    }
    std::fflush(stdout);
    if (captures > 2 || total_triangles == 0) {
        return;
    }
    // The first two captures as files, written off this thread: x, y, w of each vertex, faces
    // (in front of the camera, all three vertices written) with their kind.
    std::vector<float> xyz(size_t(vertices_used) * 3, 0.0f);
    for (u32 v = 1; v < vertices_used; ++v) {
        if (!vertex_written[v]) continue;
        const float* p = positions + size_t(v) * 4;
        xyz[size_t(v) * 3 + 0] = p[0];
        xyz[size_t(v) * 3 + 1] = p[1];
        xyz[size_t(v) * 3 + 2] = p[3];
    }
    std::vector<u32> faces;
    std::vector<u8> face_kinds;
    for (const auto& r : records) {
        if (r.kind < 0 || r.kind >= kinds) continue;
        for (size_t t = 0; t + 2 < r.triangles.size(); t += 3) {
            const u32* tri = &r.triangles[t];
            bool ok = true;
            for (u32 k = 0; k < 3; ++k) {
                ok &= vertex_written[tri[k]] && positions[size_t(tri[k]) * 4 + 3] > 0.0f;
            }
            if (!ok) continue;
            faces.insert(faces.end(), tri, tri + 3);
            face_kinds.push_back(u8(r.kind));
        }
    }
    std::string kind_names;
    for (int k = 0; k < kinds; ++k) {
        kind_names += "comment kind " + std::to_string(k) + " " + PortRenderMap::KindName(k) + "\n";
    }
    const std::string path = "scene_capture_" + std::to_string(captures) + ".ply";
    writing = true;
    std::thread([this, path, kind_names, xyz = std::move(xyz), faces = std::move(faces),
                 face_kinds = std::move(face_kinds)] {
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f,
                         "ply\nformat binary_little_endian 1.0\ncomment PROTOTYPE PC port scene "
                         "capture: x, y, w of the clip position\n%selement vertex %zu\nproperty "
                         "float x\nproperty float y\nproperty float z\nelement face %zu\nproperty "
                         "list uchar uint vertex_indices\nproperty uchar kind\nend_header\n",
                         kind_names.c_str(), xyz.size() / 3, face_kinds.size());
            std::fwrite(xyz.data(), sizeof(float), xyz.size(), f);
            for (size_t i = 0; i < face_kinds.size(); ++i) {
                const u8 three = 3;
                std::fwrite(&three, 1, 1, f);
                std::fwrite(&faces[i * 3], sizeof(u32), 3, f);
                std::fwrite(&face_kinds[i], 1, 1, f);
            }
            std::fclose(f);
            std::printf("Scene capture: %s written (%zu vertices, %zu triangles)\n", path.c_str(),
                        xyz.size() / 3, face_kinds.size());
        } else {
            std::printf("Scene capture: %s could not be written\n", path.c_str());
        }
        std::fflush(stdout);
        writing = false;
    }).detach();
}

} // namespace Vulkan
