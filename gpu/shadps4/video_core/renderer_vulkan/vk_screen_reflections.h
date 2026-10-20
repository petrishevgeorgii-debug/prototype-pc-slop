// SPDX-License-Identifier: GPL-2.0-or-later
// port: PROTOTYPE screen-space reflections on the river (docs/PROTOTYPE.md).
//
// The water is found by what its draws change: the scene colour pass tests depth-equal against
// the depth pre-pass, so every pixel is written only by its front-most surface. The scene colour
// is copied before the first draw of the water's pixel shader, and once the water's draws are
// done a compute pass marks the pixels they changed. Before post-processing (where FSR also
// runs), rays from those pixels are reflected about the water plane, marched through the scene
// depth, and the colour they hit is blended over the game's own sky reflection.

#pragma once

#include <array>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class CameraMotion;

class ScreenReflections {
public:
    /// The river's pixel shader (CUSA06545 01.00 eboot.bin).
    static constexpr u64 WaterShader = 0xa71ff1ce;

    ScreenReflections(const Instance& instance, Scheduler& scheduler,
                      VideoCore::TextureCache& texture_cache, Runtime& runtime);
    ~ScreenReflections();

    /// The menu setting (THEORDER_SSR) and the pipelines are ready.
    [[nodiscard]] bool Enabled() const noexcept;

    /// A new frame: last frame's water is forgotten.
    void OnFrameStart();

    /// Before each colour draw (outside rendering): the draw's pixel shader and colour target.
    /// The first water draw copies the target, the first other draw after the water marks the
    /// water's pixels. Returns true when it recorded work (rendering was ended).
    bool OnDraw(u64 fs_hash, VideoCore::ImageId color);

    /// Before post-processing: reflections into `color` (the scene colour) from `depth`.
    void Apply(VideoCore::ImageId color, VideoCore::ImageId depth, const CameraMotion& camera);

private:
    bool EnsureImages(u32 width, u32 height);
    void BuildMask();
    vk::ImageView View(const VideoCore::Image& image, vk::Format format,
                       vk::ImageAspectFlags aspect);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;

    bool ready = false;
    vk::UniqueDescriptorSetLayout mask_layout;
    vk::UniquePipelineLayout mask_pipeline_layout;
    vk::UniquePipeline mask_pipeline;
    vk::UniqueDescriptorSetLayout ssr_layout;
    vk::UniquePipelineLayout ssr_pipeline_layout;
    vk::UniquePipeline ssr_pipeline;

    // Own images: the scene colour before the water (then the colour the rays read) and the
    // water mask.
    VideoCore::UniqueImage copy_image;
    vk::UniqueImageView copy_view;
    VideoCore::UniqueImage mask_image;
    vk::UniqueImageView mask_view;
    u32 width = 0, height = 0;

    // This frame.
    bool in_water = false;  ///< the last colour draw was the water's
    bool copied = false;    ///< copy_image holds the scene colour before the water
    bool mask_ready = false;
    VideoCore::ImageId water_color{};

    struct ViewEntry {
        vk::Image image{};
        u64 uid = 0;
        vk::Format format{};
        vk::ImageAspectFlags aspect{};
        vk::ImageView view{};
        u64 last_use = 0;
    };
    std::array<ViewEntry, 6> views{};
    u64 view_uses = 0;
    u64 frames = 0, applied = 0;
    /// Diagnostics (last_run.log, the first minutes): frames with a water mask, Apply calls,
    /// and why Apply did nothing.
    u64 masks = 0, calls = 0, no_mask = 0, other_color = 0, no_camera = 0, bad_size = 0;
    u32 reports = 0;
};

} // namespace Vulkan
