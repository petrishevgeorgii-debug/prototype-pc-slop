// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_camera_motion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "port_toggles.h"

#include "video_core/host_shaders/camera_motion_comp.h"
#include "video_core/host_shaders/camera_motion_debug_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

struct PushConstants {
    std::array<float, 12> reproject;
    std::array<float, 4> proj;
    std::array<float, 4> prev_proj;
    std::array<float, 2> size;
    std::array<float, 2> jitter;
    std::array<float, 2> previous_jitter;
    u32 mode;
};

/// a * b for 3x4 affine matrices (rows [R | t]).
std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

} // namespace

CameraMotion::CameraMotion(const Instance& instance_, Scheduler& scheduler_,
                           VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_}, runtime{runtime_} {
    const char* env = std::getenv("THEORDER_DEBUG_MOTION");
    debug_overlay = env && env[0] == '1';
    const char* upscaler = std::getenv("THEORDER_UPSCALER");
    // The upscaler can be switched on from the menu at any time: the camera is always tracked
    // unless THEORDER_UPSCALER=none.
    for_upscaler = !(upscaler && std::strcmp(upscaler, "none") == 0);
    const auto device = instance.GetDevice();
    if (for_upscaler) {
        const std::array<vk::DescriptorSetLayoutBinding, 3> motion_bindings = {{
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 2,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        motion_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(motion_bindings.size()),
            .pBindings = motion_bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(PushConstants)};
        motion_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*motion_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto motion_module = CompileSPV(CAMERA_MOTION_COMP, device);
        motion_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = motion_module,
                              .pName = "main"},
                    .layout = *motion_pipeline_layout,
                }));
        device.destroyShaderModule(motion_module);
    }
    if (!debug_overlay) {
        return;
    }
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 3,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push_range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    }));
    const auto module = CompileSPV(CAMERA_MOTION_DEBUG_COMP, device);
    overlay_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
    for (auto& frame : frames) {
        frame = std::make_unique<VideoCore::Buffer>(instance, 0, 3840ull * 2160 * 4,
                                                    VideoCore::MemoryType::DeviceLocal);
    }
    std::printf("GPU: camera motion debug overlay on\n");
}

CameraMotion::~CameraMotion() = default;

float CameraMotion::VerticalFov() const noexcept {
    return 2.0f * std::atan(1.0f / current.proj[1]);
}

float CameraMotion::Near() const noexcept {
    // depth = zs + zo / z is 0 at the near plane.
    return -current.proj[3] / current.proj[2];
}

std::array<std::array<float, 4>, 3> CameraMotion::TaaDepthParameters() const noexcept {
    const auto transform = Multiply(previous.view, current.inv_view);
    return {current.proj, previous.proj,
            {transform[8], transform[9], transform[10], transform[11]}};
}

vk::ImageView CameraMotion::ObjectMotionView() const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->View() : vk::ImageView{};
}

vk::Image CameraMotion::ObjectMotionImage(u32 width, u32 height) const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->Image(width, height)
                                                      : vk::Image{};
}

void CameraMotion::RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                                vk::ImageView motion_view, u32 width, u32 height) {
    bool object_valid = false;
    const vk::ImageView object_view = object_motion && object_motion->Enabled()
        ? object_motion->PrepareRead(cmdbuf, width, height, object_valid) : depth_view;
    // Toggle 1 << 28 (PROTOTYPE menu, diagnostics): clip-space y pointing down the screen.
    const float flip_y = PortToggle::Disabled(1u << 28) ? -1.0f : 1.0f;
    const PushConstants push{
        .reproject = Multiply(previous.view, current.inv_view),
        .proj = {current.proj[0], current.proj[1] * flip_y, current.proj[2], current.proj[3]},
        .prev_proj = {previous.proj[0], previous.proj[1] * flip_y, previous.proj[2],
                      previous.proj[3]},
        .size = {float(width), float(height)},
        .jitter = jitter,
        .previous_jitter = previous_jitter,
        .mode = object_valid ? 1u : 0u,
    };
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo motion_info{.imageView = motion_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo object_info{.imageView = object_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &motion_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &object_info},
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *motion_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *motion_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*motion_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

void CameraMotion::OnConstants(const float* data) {
    // Scene constants: far plane 3000, 1/far, and the render size.
    if (data[0] != 3000.0f || data[4] < 64.0f || data[5] < 64.0f ||
        std::abs(data[1] * data[0] - 1.0f) > 1e-3f) {
        return;
    }
    if (frame_has_camera) {
        return; // the first one of a frame is the main camera
    }
    previous = current;
    std::memcpy(current.view.data(), data + 8, 12 * sizeof(float));
    std::memcpy(current.inv_view.data(), data + 180, 12 * sizeof(float));
    current.proj = {data[52], data[57], data[62], data[63]};
    current.valid = current.proj[0] != 0.0f && current.proj[1] != 0.0f;
    const std::array<u32, 2> size{u32(data[4]), u32(data[5])};
    if (size != render_size) {
        std::printf("Camera motion: scene render size %ux%u\n", size[0], size[1]);
        render_size = size;
    }
    frame_has_camera = true;
}

namespace {
using Mat4 = std::array<float, 16>; // column-major: m[col * 4 + row]

float At(const Mat4& m, int row, int col) {
    return m[col * 4 + row];
}

// Inverse of an affine matrix (last row 0 0 0 1); false when singular.
bool InvertAffine(const Mat4& m, Mat4& out) {
    const float a = At(m, 0, 0), b = At(m, 0, 1), c = At(m, 0, 2);
    const float d = At(m, 1, 0), e = At(m, 1, 1), f = At(m, 1, 2);
    const float g = At(m, 2, 0), h = At(m, 2, 1), i = At(m, 2, 2);
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-12f) {
        return false;
    }
    const float inv = 1.0f / det;
    float r[3][3] = {{(e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv},
                     {(f * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * f) * inv},
                     {(d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv}};
    const float t[3] = {At(m, 0, 3), At(m, 1, 3), At(m, 2, 3)};
    out = {};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out[col * 4 + row] = r[row][col];
        }
        out[12 + row] = -(r[row][0] * t[0] + r[row][1] * t[1] + r[row][2] * t[2]);
    }
    out[15] = 1.0f;
    return true;
}

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) {
                v += At(a, row, k) * At(b, k, col);
            }
            out[col * 4 + row] = v;
        }
    }
    return out;
}
} // namespace

void CameraMotion::OnObjectConstants(const float* data, u64 vs_hash, u32 size, int source) {
    if (frame_has_camera) {
        return;
    }
    ++diag_calls;
    (source ? diag_guest : diag_ring)++;
    // Material constants hold World and WVP (column-major), in either order: World first
    // (e.g. vertex shader 0xf1958b0b) or WVP first (most others).
    Mat4 world, wvp, inv, vp;
    bool found = false;
    for (int order = 0; order < 2 && !found; ++order) {
        std::memcpy(world.data(), data + (order ? 16 : 0), sizeof(world));
        std::memcpy(wvp.data(), data + (order ? 0 : 16), sizeof(wvp));
        // Object-to-world: affine.
        if (world[3] != 0.0f || world[7] != 0.0f || world[11] != 0.0f ||
            std::abs(world[15] - 1.0f) > 1e-4f) {
            continue;
        }
        bool finite = true;
        for (const float v : wvp) {
            finite = finite && std::isfinite(v);
        }
        if (!finite || !InvertAffine(world, inv)) {
            continue;
        }
        vp = Mul(wvp, inv);
        const float x = At(vp, 3, 0), y = At(vp, 3, 1), z = At(vp, 3, 2);
        const float len = std::sqrt(x * x + y * y + z * z);
        found = len >= 0.9f && len <= 1.1f;
    }
    if (!found) {
        ++diag_affine;
        // Diagnostics: the first floats of a few rejected buffers (one per shader and size).
        static u32 dumps = 0;
        static std::array<u64, 24> seen{};
        const u64 key = vs_hash * 4099 + size * 2 + u64(source);
        if (dumps < seen.size() && std::find(seen.begin(), seen.begin() + dumps, key) ==
                                       seen.begin() + dumps) {
            seen[dumps++] = key;
            std::printf("Camera: rejected constants of vs %08llx (%u bytes, %s):",
                        static_cast<unsigned long long>(vs_hash), size, source ? "guest" : "ring");
            for (int i = 0; i < 32 && i * 4 < int(size); ++i) {
                std::printf("%s%g", i % 4 ? " " : (i ? " | " : " "), data[i]);
            }
            std::printf("\n");
        }
        return;
    }
    // Perspective with w = distance along the view axis: row 3 is that axis (unit length).
    const float zx = At(vp, 3, 0), zy = At(vp, 3, 1), zz = At(vp, 3, 2), zw = At(vp, 3, 3);
    const float zlen = std::sqrt(zx * zx + zy * zy + zz * zz);
    diag_last_zlen = zlen;
    if (zlen < 0.9f || zlen > 1.1f) {
        ++diag_zlen;
        return;
    }
    // Group the draws' view-projections: the frame's camera is the one most draws use
    // (decided in FinalizeFrame). PROTOTYPE draws some objects with another projection (a
    // nearer near plane); taking whichever came first alternated the two between frames.
    for (u32 c = 0; c < candidate_count; ++c) {
        float diff = 0.0f, scale = 0.0f;
        for (int i = 0; i < 16; ++i) {
            diff = std::max(diff, std::abs(vp[i] - candidates[c][i]));
            scale = std::max(scale, std::abs(candidates[c][i]));
        }
        if (diff <= 1e-4f * std::max(scale, 1.0f)) {
            ++candidate_votes[c];
            return;
        }
    }
    ++diag_disagree;
    if (candidate_count < candidates.size()) {
        candidates[candidate_count] = vp;
        candidate_votes[candidate_count] = 1;
        ++candidate_count;
    }
}

void CameraMotion::FinalizeFrame() {
    if (frame_has_camera) {
        return;
    }
    u32 best = candidate_count, votes = 1, groups = 0;
    for (u32 c = 0; c < candidate_count; ++c) {
        groups += candidate_votes[c] >= 2;
        if (candidate_votes[c] > votes) {
            votes = candidate_votes[c];
            best = c;
        }
    }
    diag_max_groups = std::max(diag_max_groups, groups);
    if (best == candidate_count) {
        return;
    }
    const Mat4 vp = candidates[best];
    candidate_count = 0;
    const float zx = At(vp, 3, 0), zy = At(vp, 3, 1), zz = At(vp, 3, 2), zw = At(vp, 3, 3);
    const float zlen = std::sqrt(zx * zx + zy * zy + zz * zz);
    // Rows of the view matrix from the view-projection: z = row 3 / |row 3|, then x and y with
    // their projection centres removed; depth = a + b / z (D3D, 0 at the near plane).
    const float inv_len = 1.0f / zlen;
    std::array<float, 4> vz = {zx * inv_len, zy * inv_len, zz * inv_len, zw * inv_len};
    std::array<float, 4> rows[3];
    float scales[2];
    for (int r = 0; r < 2; ++r) {
        std::array<float, 4> row = {At(vp, r, 0), At(vp, r, 1), At(vp, r, 2), At(vp, r, 3)};
        const float centre = row[0] * vz[0] + row[1] * vz[1] + row[2] * vz[2];
        for (int k = 0; k < 4; ++k) {
            row[k] -= centre * vz[k];
        }
        const float len = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        if (len < 1e-6f) {
            return;
        }
        for (int k = 0; k < 4; ++k) {
            row[k] /= len;
        }
        rows[r] = row;
        scales[r] = len / zlen;
    }
    rows[2] = vz;
    const float a = (At(vp, 2, 0) * vz[0] + At(vp, 2, 1) * vz[1] + At(vp, 2, 2) * vz[2]) / zlen;
    const float b = (At(vp, 2, 3) - a * zw) / zlen;
    previous = current;
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 4; ++k) {
            current.view[r * 4 + k] = rows[r][k];
        }
    }
    // View to world: transposed rotation, rotated negative translation.
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
            current.inv_view[r * 4 + k] = rows[k][r];
        }
        current.inv_view[r * 4 + 3] =
            -(rows[0][r] * rows[0][3] + rows[1][r] * rows[1][3] + rows[2][r] * rows[2][3]);
    }
    current.proj = {scales[0], scales[1], a, b};
    current.valid = scales[0] > 0.0f && scales[1] > 0.0f && b < 0.0f;
    ++diag_commits;
    diag_invalid += !current.valid;
    // Diagnostics: the largest camera movement between frames in this window (world units,
    // and the angle of the view axis in degrees).
    if (previous.valid && current.valid) {
        float move = 0.0f, dot = 0.0f;
        for (int r = 0; r < 3; ++r) {
            const float d = current.inv_view[r * 4 + 3] - previous.inv_view[r * 4 + 3];
            move += d * d;
            dot += current.view[8 + r] * previous.view[8 + r];
        }
        diag_max_move = std::max(diag_max_move, std::sqrt(move));
        diag_max_turn = std::max(diag_max_turn,
                                 std::acos(std::clamp(dot, -1.0f, 1.0f)) * 57.29578f);
    }
    diag_last_b = b;
    frame_has_camera = true;
    candidate_count = 0;
}

float CameraMotion::Far() const noexcept {
    // depth = a + b / z reaches 1 at the far plane.
    // D3D projection: a = f / (f - n) > 1, b = -f n / (f - n) < 0.
    const float a = current.proj[2], b = current.proj[3];
    if (current.valid && a > 1.0f && b < 0.0f) {
        return std::min(b / (1.0f - a), 1.0e6f);
    }
    return 3000.0f;
}

void CameraMotion::OnGBufferPass(VideoCore::ImageId depth) {
    depth_id = depth;
}

void CameraMotion::OnDisplayPass(VideoCore::ImageId frame) {
    if (debug_overlay && frame && depth_id && current.valid && previous.valid) {
        Overlay(frame);
    }
    FinalizeFrame();
    if (!frame_has_camera) {
        InvalidateHistory();
        ++diag_history_resets;
    }
    frame_has_camera = false;
    candidate_count = 0;
    depth_id = {};
}

void CameraMotion::Overlay(VideoCore::ImageId frame) {
    auto& depth = texture_cache.GetImage(depth_id);
    auto& color = texture_cache.GetImage(frame);
    const auto depth_format = depth.info.pixel_format;
    if ((depth_format != vk::Format::eD32Sfloat && depth_format != vk::Format::eD32SfloatS8Uint) ||
        color.info.pixel_format != vk::Format::eR8G8B8A8Unorm ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
        color.info.size.width != depth.info.size.width ||
        color.info.size.height != depth.info.size.height) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("Camera motion: overlay skipped (depth %s, frame %s %ux%u)\n",
                        vk::to_string(depth_format).c_str(),
                        vk::to_string(color.info.pixel_format).c_str(), color.info.size.width,
                        color.info.size.height);
        }
        return;
    }
    const auto device = instance.GetDevice();
    const auto depth_view = Check(device.createImageView({
        .image = vk::Image(depth.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = depth_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    const auto color_view = Check(device.createImageView({
        .image = vk::Image(color.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    runtime.FlushBarriers();

    // Toggle 1 << 28 (PROTOTYPE menu, diagnostics): clip-space y pointing down the screen.
    const float flip_y = PortToggle::Disabled(1u << 28) ? -1.0f : 1.0f;
    const PushConstants push{
        .reproject = Multiply(previous.view, current.inv_view),
        .proj = {current.proj[0], current.proj[1] * flip_y, current.proj[2], current.proj[3]},
        .prev_proj = {previous.proj[0], previous.proj[1] * flip_y, previous.proj[2],
                      previous.proj[3]},
        .size = {float(color.info.size.width), float(color.info.size.height)},
        .jitter = jitter,
        .previous_jitter = previous_jitter,
        .mode = PortToggle::Disabled(1u << 20)   ? 1u
                : PortToggle::Disabled(1u << 21) ? 2u
                : PortToggle::Disabled(1u << 22) ? 3u
                : PortToggle::Disabled(1u << 23) ? 4u
                                               : 0u,
    };
    static u32 log_counter = 0;
    if (++log_counter % 200 == 0) {
        const auto& m = push.reproject;
        std::printf("Camera motion: proj %g %g %g %g prev %g %g %g %g\n"
                    "  view  %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f\n"
                    "  reproj %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f\n"
                    "  depth %s %ux%u, frame %ux%u\n",
                    push.proj[0], push.proj[1], push.proj[2], push.proj[3], push.prev_proj[0],
                    push.prev_proj[1], push.prev_proj[2], push.prev_proj[3], current.view[0],
                    current.view[1], current.view[2], current.view[3], current.view[4],
                    current.view[5], current.view[6], current.view[7], current.view[8],
                    current.view[9], current.view[10], current.view[11], m[0], m[1], m[2], m[3],
                    m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
                    vk::to_string(depth_format).c_str(), depth.info.size.width,
                    depth.info.size.height, color.info.size.width, color.info.size.height);
    }
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{.imageView = color_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const auto& prev_frame = *frames[frame_index ^ 1];
    const auto& next_frame = *frames[frame_index];
    frame_index ^= 1;
    const vk::DescriptorBufferInfo prev_info{prev_frame.Handle(), 0, prev_frame.SizeBytes()};
    const vk::DescriptorBufferInfo next_info{next_frame.Handle(), 0, next_frame.SizeBytes()};
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &color_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &prev_info},
        {.dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &next_info},
    }};
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *overlay_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                         &push);
    // The previous frame buffer was written by the last dispatch.
    const vk::MemoryBarrier2 frame_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &frame_barrier});
    cmdbuf.dispatch((color.info.size.width + 7) / 8, (color.info.size.height + 7) / 8, 1);

    scheduler.DeferOperation([device, depth_view, color_view] {
        device.destroyImageView(depth_view);
        device.destroyImageView(color_view);
    });
}

} // namespace Vulkan
