// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_screen_reflections.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "port_settings.h"
#include "video_core/host_shaders/proto_ssr_comp.h"
#include "video_core/host_shaders/proto_water_mask_comp.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

struct MaskConstants {
    s32 size[2];
};

struct SsrConstants {
    std::array<float, 4> proj; ///< x scale, y scale, z scale, z offset
    std::array<float, 4> up;   ///< world up in view space, strength
    float size[2];
    float max_distance;
    u32 mode; ///< 0 reflections, 1 water mask, 2 hits (THEORDER_SSR_DEBUG)
};

constexpr vk::ImageUsageFlags NeededUsage =
    vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc;

u32 DebugMode() {
    const char* env = std::getenv("THEORDER_SSR_DEBUG");
    return env && env[0] ? u32(std::atoi(env)) : 0u;
}

float EnvFloat(const char* name, float fallback) {
    const char* env = std::getenv(name);
    return env && env[0] ? float(std::atof(env)) : fallback;
}

vk::UniqueDescriptorSetLayout MakeLayout(vk::Device device,
                                          std::initializer_list<vk::DescriptorType> types) {
    std::array<vk::DescriptorSetLayoutBinding, 4> bindings{};
    u32 count = 0;
    for (const auto type : types) {
        bindings[count] = {.binding = count,
                           .descriptorType = type,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        ++count;
    }
    return Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = count,
        .pBindings = bindings.data(),
    }));
}

void ImageBarrier(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout old_layout,
                  vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                  vk::ImageLayout new_layout, vk::PipelineStageFlags2 dst_stage,
                  vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

} // namespace

ScreenReflections::ScreenReflections(const Instance& instance_, Scheduler& scheduler_,
                                     VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_} {
    const auto device = instance.GetDevice();
    const auto make_pipeline = [&](vk::UniqueDescriptorSetLayout& layout,
                                   vk::UniquePipelineLayout& pipeline_layout,
                                   vk::UniquePipeline& pipeline, u32 push_size,
                                   std::span<const u32> code) {
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = push_size};
        pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto module = CompileSPV(code, device);
        pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *pipeline_layout,
                }));
        device.destroyShaderModule(module);
    };
    mask_layout = MakeLayout(device, {vk::DescriptorType::eStorageImage,
                                      vk::DescriptorType::eStorageImage,
                                      vk::DescriptorType::eStorageImage});
    make_pipeline(mask_layout, mask_pipeline_layout, mask_pipeline, sizeof(MaskConstants),
                  PROTO_WATER_MASK_COMP);
    ssr_layout = MakeLayout(device, {vk::DescriptorType::eStorageImage,
                                     vk::DescriptorType::eSampledImage,
                                     vk::DescriptorType::eStorageImage,
                                     vk::DescriptorType::eStorageImage});
    make_pipeline(ssr_layout, ssr_pipeline_layout, ssr_pipeline, sizeof(SsrConstants),
                  PROTO_SSR_COMP);
    if (const char* env = std::getenv("THEORDER_SSR_WATER"); env && env[0]) {
        const char* p = env;
        while (*p && extra_count < extra_water.size()) {
            char* end = nullptr;
            const u64 hash = std::strtoull(p, &end, 16);
            if (end == p) {
                ++p;
                continue;
            }
            extra_water[extra_count++] = hash;
            std::printf("Water reflections: shader %08llx is water too\n",
                        static_cast<unsigned long long>(hash));
            p = end;
        }
    }
    ready = true;
    std::printf("Water reflections: ready (%s)\n", PortSettings::Get().ssr ? "on" : "off");
}

ScreenReflections::~ScreenReflections() {
    const auto device = instance.GetDevice();
    for (auto& entry : views) {
        if (entry.view) {
            device.destroyImageView(entry.view);
        }
    }
}

bool ScreenReflections::Enabled() const noexcept {
    return ready && PortSettings::Get().ssr;
}

void ScreenReflections::OnFrameStart() {
    in_water = false;
    copied = false;
    mask_ready = false;
    water_color = {};
    ++frames;
    if (frames % 600 == 0 && reports < 6) {
        ++reports;
        std::printf("Water reflections (600 frames): water in %llu, applied %llu; post trigger %llu, "
                    "skipped: no water %llu, other colour %llu, no camera %llu, size %llu\n",
                    static_cast<unsigned long long>(masks), static_cast<unsigned long long>(applied),
                    static_cast<unsigned long long>(calls), static_cast<unsigned long long>(no_mask),
                    static_cast<unsigned long long>(other_color),
                    static_cast<unsigned long long>(no_camera),
                    static_cast<unsigned long long>(bad_size));
        masks = applied = calls = no_mask = other_color = no_camera = bad_size = 0;
    }
}

vk::ImageView ScreenReflections::View(const VideoCore::Image& image, vk::Format format,
                                      vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    ViewEntry* oldest = &views[0];
    for (auto& entry : views) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
            entry.last_use = ++view_uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }
    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++view_uses,
    };
    return oldest->view;
}

bool ScreenReflections::EnsureImages(u32 w, u32 h) {
    if (copy_image && w == width && h == height) {
        return true;
    }
    const auto device = instance.GetDevice();
    scheduler.Finish();
    const auto make = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                          vk::Format format, vk::ImageUsageFlags usage) {
        view.reset();
        image = VideoCore::UniqueImage(device, instance.GetAllocator());
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    };
    make(copy_image, copy_view, vk::Format::eR8G8B8A8Unorm,
         vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst);
    make(mask_image, mask_view, vk::Format::eR8Unorm, vk::ImageUsageFlagBits::eStorage);
    width = w;
    height = h;
    std::printf("Water reflections: %ux%u\n", w, h);
    return true;
}

bool ScreenReflections::IsWater(u64 vs_hash, u64 fs_hash) const {
    if (fs_hash == WaterPixelShader || vs_hash == WaterVertexShader) {
        return true;
    }
    for (u32 i = 0; i < extra_count; ++i) {
        if (extra_water[i] == vs_hash || extra_water[i] == fs_hash) {
            return true;
        }
    }
    return false;
}

void ScreenReflections::NoteCubeDraw(u64 vs_hash, u64 fs_hash) {
    const u64 key = vs_hash * 0x9e3779b97f4a7c15ull ^ fs_hash;
    for (u32 i = 0; i < noted_count; ++i) {
        if (noted[i] == key) {
            return;
        }
    }
    if (noted_count == noted.size()) {
        return;
    }
    noted[noted_count++] = key;
    std::printf("Water reflections: scene draw vs %08llx ps %08llx reflects a cube map%s\n",
                static_cast<unsigned long long>(vs_hash), static_cast<unsigned long long>(fs_hash),
                IsWater(vs_hash, fs_hash) ? " (water)" : "");
}

bool ScreenReflections::OnDraw(u64 vs_hash, u64 fs_hash, VideoCore::ImageId color) {
    if (!Enabled()) {
        return false;
    }
    const bool water = color && IsWater(vs_hash, fs_hash);
    if (water && !in_water) {
        in_water = true;
        if (copied) {
            return false; // the frame's first water run decides
        }
        auto& image = texture_cache.GetImage(color);
        if (image.info.pixel_format != vk::Format::eR8G8B8A8Unorm ||
            (image.usage_flags & NeededUsage) != NeededUsage) {
            static bool told = false;
            if (!told) {
                told = true;
                std::printf("Water reflections: scene colour format %d / usage %x not usable\n",
                            int(image.info.pixel_format), u32(image.usage_flags));
            }
            return false;
        }
        if (!EnsureImages(image.info.size.width, image.info.size.height)) {
            return false;
        }
        scheduler.EndRendering();
        runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
        const auto cmd = scheduler.CommandBuffer();
        ImageBarrier(cmd, vk::Image(copy_image), vk::ImageLayout::eUndefined,
                     vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eNone,
                     vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .extent = {width, height, 1},
        };
        cmd.copyImage(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                      vk::Image(copy_image), vk::ImageLayout::eTransferDstOptimal, region);
        ImageBarrier(cmd, vk::Image(copy_image), vk::ImageLayout::eTransferDstOptimal,
                     vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                     vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderRead);
        copied = true;
        water_color = color;
        return true;
    }
    if (!water && in_water) {
        in_water = false;
        if (copied && !mask_ready) {
            BuildMask();
            return true;
        }
    }
    return false;
}

void ScreenReflections::BuildMask() {
    auto& image = texture_cache.GetImage(water_color);
    if (image.info.size.width != width || image.info.size.height != height) {
        return;
    }
    scheduler.EndRendering();
    runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    const auto cmd = scheduler.CommandBuffer();
    ImageBarrier(cmd, vk::Image(mask_image), vk::ImageLayout::eUndefined,
                 vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eNone,
                 vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                 vk::AccessFlagBits2::eShaderWrite);
    const vk::DescriptorImageInfo before_info{.imageView = *copy_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{
        .imageView = View(image, vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor),
        .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo mask_info{.imageView = *mask_view,
                                            .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes = {{
        {.dstBinding = 0, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &before_info},
        {.dstBinding = 1, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &color_info},
        {.dstBinding = 2, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &mask_info},
    }};
    const MaskConstants push{.size = {s32(width), s32(height)}};
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *mask_pipeline);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *mask_pipeline_layout, 0, writes);
    cmd.pushConstants(*mask_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                      &push);
    cmd.dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ImageBarrier(cmd, vk::Image(mask_image), vk::ImageLayout::eGeneral,
                 vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                 vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                 vk::AccessFlagBits2::eShaderRead);
    // copy_image is read here and rewritten by Apply: the copy waits for this dispatch.
    ImageBarrier(cmd, vk::Image(copy_image), vk::ImageLayout::eGeneral,
                 vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead,
                 vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                 vk::AccessFlagBits2::eShaderRead);
    mask_ready = true;
    ++masks;
}

void ScreenReflections::Apply(VideoCore::ImageId color, VideoCore::ImageId depth,
                              const CameraMotion& camera) {
    if (!Enabled() || !color || !depth) {
        return;
    }
    ++calls;
    if (!mask_ready) {
        ++no_mask;
        return;
    }
    if (color != water_color) {
        ++other_color;
        return;
    }
    mask_ready = false;
    if (!camera.CurrentValid()) {
        ++no_camera;
        return;
    }
    auto& color_image = texture_cache.GetImage(color);
    auto& depth_image = texture_cache.GetImage(depth);
    const auto depth_format = depth_image.info.pixel_format;
    if (color_image.info.size.width != width || color_image.info.size.height != height ||
        depth_image.info.size.width != width || depth_image.info.size.height != height ||
        (depth_format != vk::Format::eD32Sfloat && depth_format != vk::Format::eD32SfloatS8Uint)) {
        ++bad_size;
        return;
    }
    // The world's up axis (y) in view space: column 1 of the view rotation.
    const auto view = camera.CurrentView();
    const float strength = EnvFloat("THEORDER_SSR_STRENGTH", 1.0f);
    const SsrConstants push{
        .proj = camera.CurrentProj(),
        .up = {view[1], view[5], view[9], strength},
        .size = {float(width), float(height)},
        .max_distance = EnvFloat("THEORDER_SSR_DISTANCE", 1500.0f),
        .mode = DebugMode(),
    };

    scheduler.EndRendering();
    // The rays read the colour as it was before this pass: a copy.
    runtime.Transit(&color_image, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
    runtime.Transit(&depth_image, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    auto cmd = scheduler.CommandBuffer();
    ImageBarrier(cmd, vk::Image(copy_image), vk::ImageLayout::eGeneral,
                 vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead,
                 vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eCopy,
                 vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {width, height, 1},
    };
    cmd.copyImage(color_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                  vk::Image(copy_image), vk::ImageLayout::eTransferDstOptimal, region);
    ImageBarrier(cmd, vk::Image(copy_image), vk::ImageLayout::eTransferDstOptimal,
                 vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                 vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                 vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color_image, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    runtime.FlushBarriers();
    cmd = scheduler.CommandBuffer();

    const vk::DescriptorImageInfo source_info{.imageView = *copy_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo depth_info{
        .imageView = View(depth_image, depth_format, vk::ImageAspectFlagBits::eDepth),
        .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo mask_info{.imageView = *mask_view,
                                            .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{
        .imageView = View(color_image, vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor),
        .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        {.dstBinding = 0, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &source_info},
        {.dstBinding = 1, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &depth_info},
        {.dstBinding = 2, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &mask_info},
        {.dstBinding = 3, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &color_info},
    }};
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *ssr_pipeline);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *ssr_pipeline_layout, 0, writes);
    cmd.pushConstants(*ssr_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                      &push);
    cmd.dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ++applied;
}

} // namespace Vulkan
