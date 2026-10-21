// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: the Vulkan renderer (docs/ENGINE.md).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "p3d.h"

struct SDL_Window;
struct VmaAllocator_T;
struct VmaAllocation_T;

namespace engine {

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation_T* allocation = nullptr;
};

/// A texture with two views of its data: as colour (sRGB) and as data (normal maps).
struct GpuTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation_T* allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;   ///< sRGB
    VkImageView linear = VK_NULL_HANDLE; ///< UNORM
};

/// How a draw's surface is shaded (world.frag): bits.
namespace Material {
constexpr std::uint32_t Textured = 1, NormalMap = 2, Tint = 4, AlphaTest = 8, Decal = 16;
}

/// One draw: a range of a model's index buffer with one material.
struct Draw {
    std::uint32_t first_index = 0, index_count = 0;
    std::int32_t vertex_offset = 0;
    VkDescriptorSet material = VK_NULL_HANDLE; ///< its textures (Renderer::CreateMaterial); null: none
    std::uint32_t flags = 0;                   ///< Material bits
    int group = -1; ///< e.g. the cell a low-detail part stands for (the city hides it)
};

/// Geometry on the GPU: one vertex and one index buffer, drawn in parts.
struct Model {
    GpuBuffer vertices, indices;
    std::vector<Draw> draws;
    std::vector<GpuTexture> textures;       ///< owned by this model
    std::vector<VkDescriptorSet> materials; ///< owned by this model
    float min[3] = {-1e30f, -1e30f, -1e30f}, max[3] = {1e30f, 1e30f, 1e30f}; ///< world bounds, for culling
};

/// The lights of the scene (the city's light rig, manhattan_tod).
struct Lighting {
    float sun[3] = {0.576f, 0.817f, -0.034f}; ///< towards the sun
    float sun_colour[3] = {1.01f, 1.0f, 0.85f};
    float ambient[3] = {0.29f, 0.39f, 0.55f};
    float fill[2][3] = {{-0.39f, 0.62f, -0.68f}, {0.08f, 0.83f, 0.55f}}; ///< towards each fill light
    float fill_colour[2][3] = {{0.37f, 0.37f, 0.42f}, {0.29f, 0.36f, 0.40f}};
    float exposure = 0.85f;
    float fog_density = 0.0008f;
    bool shadows = true;
};

struct FrameView {
    float view_projection[16]; ///< column-major
    float camera[3];
    float forward[3];
    float right[3]; ///< scaled by tan(horizontal fov / 2); negative when the view is mirrored
    float up[3];    ///< scaled by tan(vertical fov / 2)
    Lighting lighting;
};

class Renderer {
public:
    bool Init(SDL_Window* window, bool validation, std::string& error);
    void Shutdown();

    /// Uploads a texture (all its levels).
    bool CreateTexture(const p3d::Texture& texture, GpuTexture& out);
    /// A material's descriptor set: its colour texture and normal map (either may be null).
    /// Freed with the model that lists it in `materials`.
    VkDescriptorSet CreateMaterial(const GpuTexture* colour, const GpuTexture* normal);
    /// Uploads geometry; `draws` index into it.
    bool CreateModel(const std::vector<p3d::Vertex>& vertices, const std::vector<std::uint32_t>& indices, Model& out);
    /// Frees a model and its textures (waits for the GPU).
    void DestroyModel(Model& model);

    /// Draws the models; `hidden` groups are left out. False when the window cannot be drawn
    /// to right now (minimised).
    bool DrawFrame(const FrameView& view, const std::vector<const Model*>& models, const std::vector<bool>& hidden);

    /// The size the next frame is drawn at.
    void Size(std::uint32_t& width, std::uint32_t& height) const;
    /// Writes the last frame drawn to a PNG (--screenshot).
    bool SaveScreenshot(const std::string& path, std::string& error);
    std::string DeviceName() const { return device_name_; }

private:
    bool CreateSwapchain(std::string& error);
    void DestroySwapchain();
    bool CreatePipeline(std::string& error);
    bool CreateShadowMap(std::string& error);
    VkPipeline MakePipeline(const std::uint32_t* vs, std::size_t vs_bytes, const std::uint32_t* fs, std::size_t fs_bytes,
                            VkRenderPass pass, int kind);
    /// The sun's cascades for this view: matrices, ends and texel sizes.
    void Cascades(const FrameView& view, float matrices[4][16], float splits[4], float texels[4]) const;
    bool Upload(const void* data, std::size_t size, VkBuffer dst);
    VkCommandBuffer BeginOneShot();
    void EndOneShot(VkCommandBuffer cmd);

    SDL_Window* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VmaAllocator_T* allocator_ = nullptr;
    std::string device_name_;
    float max_anisotropy_ = 1.0f;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> render_done_; ///< per swapchain image
    VkImage depth_ = VK_NULL_HANDLE;
    VmaAllocation_T* depth_allocation_ = nullptr;
    VkImageView depth_view_ = VK_NULL_HANDLE;
    bool resized_ = false;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;       ///< set 0: a material
    VkDescriptorSetLayout frame_set_layout_ = VK_NULL_HANDLE; ///< set 1: the frame's uniforms, the shadow map
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;        ///< opaque and cut-out surfaces
    VkPipeline decal_pipeline_ = VK_NULL_HANDLE;  ///< blended surfaces
    VkPipeline sky_pipeline_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;
    GpuTexture white_{}, flat_normal_{};
    VkDescriptorSet default_material_ = VK_NULL_HANDLE;

    static constexpr std::uint32_t kShadowSize = 4096; ///< the atlas: 2 x 2 cascades
    VkRenderPass shadow_pass_ = VK_NULL_HANDLE;
    VkImage shadow_ = VK_NULL_HANDLE;
    VmaAllocation_T* shadow_allocation_ = nullptr;
    VkImageView shadow_view_ = VK_NULL_HANDLE;
    VkFramebuffer shadow_framebuffer_ = VK_NULL_HANDLE;

    static constexpr int kFrames = 2;
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence done = VK_NULL_HANDLE;
        GpuBuffer uniforms; ///< FrameUniforms, mapped
        void* mapped = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
    } frames_[kFrames];
    int frame_ = 0;
    std::uint32_t last_image_ = UINT32_MAX;
    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
};

} // namespace engine
