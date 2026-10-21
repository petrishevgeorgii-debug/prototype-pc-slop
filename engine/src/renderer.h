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

struct GpuTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation_T* allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
};

/// One draw: a range of a model's index buffer with one texture.
struct Draw {
    std::uint32_t first_index = 0, index_count = 0;
    std::int32_t vertex_offset = 0;
    VkDescriptorSet texture = VK_NULL_HANDLE; ///< null: untextured
    int group = -1; ///< e.g. the cell a low-detail part stands for (the city hides it)
};

/// Geometry on the GPU: one vertex and one index buffer, drawn in parts.
struct Model {
    GpuBuffer vertices, indices;
    std::vector<Draw> draws;
    std::vector<GpuTexture> textures; ///< owned by this model
};

struct FrameView {
    float view_projection[16]; ///< column-major
    float camera[3];
    float sun[3];
    float fog_density;
};

class Renderer {
public:
    bool Init(SDL_Window* window, bool validation, std::string& error);
    void Shutdown();

    /// Uploads a texture (all its levels) and makes its descriptor set.
    bool CreateTexture(const p3d::Texture& texture, GpuTexture& out);
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
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    GpuTexture white_{};

    static constexpr int kFrames = 2;
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence done = VK_NULL_HANDLE;
    } frames_[kFrames];
    int frame_ = 0;
    std::uint32_t last_image_ = UINT32_MAX;
    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
};

} // namespace engine
