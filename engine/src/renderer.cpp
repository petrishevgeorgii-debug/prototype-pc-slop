// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: the Vulkan renderer. The sun's cascaded shadow map, then one forward pass:
// the city's surfaces (colour and normal maps, vertex tint) lit by the game's light rig, the
// sky, then blended decals; haze and tone mapping in the shaders. Reverse-Z depth with an infinite
// far plane.

#include "renderer.h"

#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "vma.h"

#include "miniz.h"

#include "shadow_frag.h"
#include "shadow_vert.h"
#include "sky_frag.h"
#include "sky_vert.h"
#include "world_frag.h"
#include "world_vert.h"

namespace engine {

namespace {

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr std::uint32_t kMaxTextures = 32768;

struct PushConstants {
    float view_projection[16];
    float camera[4];          ///< xyz, fog density
    std::uint32_t material[4]; ///< flags (Material)
};
static_assert(sizeof(PushConstants) == 96);
constexpr std::uint32_t kFlagsOffset = offsetof(PushConstants, material);
constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

/// shaders/common.glsl, Frame (std140).
struct FrameUniforms {
    float cascades[4][16];
    float splits[4];
    float cascade_texel[4];
    float camera_forward[4];
    float camera_right[4];
    float camera_up[4];
    float camera_position[4];
    float sun_direction[4];
    float sun_colour[4];
    float ambient[4];
    float fill_direction[2][4];
    float fill_colour[2][4];
    float sky_zenith[4];
    float sky_horizon[4];
};

enum PipelineKind { kWorld, kDecal, kSky, kShadow };
constexpr std::uint32_t kCascadeSize = 2048;

float Dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

void Normalize3(float v[3]) {
    const float l = std::sqrt(Dot(v, v));
    if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
}

/// A model whose bounds are wholly outside one side of `m`'s clip space is left out.
bool Visible(const Model& model, const float m[16]) {
    if (model.max[0] - model.min[0] > 1e29f) return true;
    int outside[6] = {0, 0, 0, 0, 0, 0};
    for (int corner = 0; corner < 8; ++corner) {
        const float p[3] = {corner & 1 ? model.max[0] : model.min[0], corner & 2 ? model.max[1] : model.min[1],
                            corner & 4 ? model.max[2] : model.min[2]};
        float c[4];
        for (int r = 0; r < 4; ++r) c[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
        outside[0] += c[0] < -c[3];
        outside[1] += c[0] > c[3];
        outside[2] += c[1] < -c[3];
        outside[3] += c[1] > c[3];
        outside[4] += c[3] <= 0.0f; // behind the camera
        outside[5] += c[2] > c[3];  // beyond a shadow cascade's far side
    }
    for (int k : outside) {
        if (k == 8) return false;
    }
    return true;
}

VKAPI_ATTR VkBool32 VKAPI_CALL OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                            void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        Log("Vulkan: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

VkFormat FormatOf(p3d::TextureFormat f, bool srgb) {
    switch (f) {
    case p3d::TextureFormat::BC1: return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case p3d::TextureFormat::BC2: return srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
    case p3d::TextureFormat::BC3: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    }
    return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
}

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
             VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
             std::uint32_t levels = 1, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, levels, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkShaderModule Module(VkDevice device, const std::uint32_t* code, std::size_t bytes) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &m);
    return m;
}

} // namespace

bool Renderer::Init(SDL_Window* window, bool validation, std::string& error) {
    window_ = window;
    Uint32 count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!sdl_extensions) {
        error = std::string("SDL has no Vulkan: ") + SDL_GetError();
        return false;
    }
    std::vector<const char*> extensions(sdl_extensions, sdl_extensions + count);
    std::vector<const char*> layers;
    if (validation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Manhattan Engine";
    app.pEngineName = "Manhattan Engine";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    ii.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    ii.ppEnabledExtensionNames = extensions.data();
    ii.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ii.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ii, nullptr, &instance_) != VK_SUCCESS) {
        if (!validation) {
            error = "cannot create a Vulkan instance (is a Vulkan 1.2 driver installed?)";
            return false;
        }
        ii.enabledLayerCount = 0;
        extensions.pop_back();
        ii.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        validation = false;
        if (vkCreateInstance(&ii, nullptr, &instance_) != VK_SUCCESS) {
            error = "cannot create a Vulkan instance";
            return false;
        }
    }
    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = OnValidation;
        if (create) create(instance_, &mi, nullptr, &messenger_);
    }
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
        error = std::string("cannot create the window's Vulkan surface: ") + SDL_GetError();
        return false;
    }

    // A GPU with graphics + present and BC textures; a discrete one first.
    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
    int best_score = -1;
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu, &props);
        VkPhysicalDeviceFeatures features;
        vkGetPhysicalDeviceFeatures(gpu, &features);
        if (!features.textureCompressionBC) continue;
        std::uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, qs.data());
        for (std::uint32_t q = 0; q < qn; ++q) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, q, surface_, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                              : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > best_score) {
                best_score = score;
                gpu_ = gpu;
                queue_family_ = q;
                device_name_ = props.deviceName;
                max_anisotropy_ = features.samplerAnisotropy ? std::min(16.0f, props.limits.maxSamplerAnisotropy) : 1.0f;
            }
            break;
        }
    }
    if (!gpu_) {
        error = "no GPU with Vulkan, BC textures and presentation to this window";
        return false;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = queue_family_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled{};
    enabled.textureCompressionBC = VK_TRUE;
    enabled.samplerAnisotropy = max_anisotropy_ > 1.0f ? VK_TRUE : VK_FALSE;
    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = device_extensions;
    di.pEnabledFeatures = &enabled;
    if (vkCreateDevice(gpu_, &di, nullptr, &device_) != VK_SUCCESS) {
        error = "cannot create the Vulkan device";
        return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VmaAllocatorCreateInfo ai{};
    ai.physicalDevice = gpu_;
    ai.device = device_;
    ai.instance = instance_;
    ai.vulkanApiVersion = VK_API_VERSION_1_2;
    if (vmaCreateAllocator(&ai, &allocator_) != VK_SUCCESS) {
        error = "cannot create the memory allocator";
        return false;
    }

    for (auto& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = queue_family_;
        vkCreateCommandPool(device_, &pi, nullptr, &f.pool);
        VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ci.commandPool = f.pool;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        vkAllocateCommandBuffers(device_, &ci, &f.cmd);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_, &si, nullptr, &f.acquired);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device_, &fi, nullptr, &f.done);
    }
    VkCommandPoolCreateInfo upi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    upi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    upi.queueFamilyIndex = queue_family_;
    vkCreateCommandPool(device_, &upi, nullptr, &upload_pool_);

    if (!CreateSwapchain(error) || !CreateShadowMap(error) || !CreatePipeline(error)) {
        return false;
    }

    // Stand-ins for a material without a colour texture or a normal map: white and flat.
    p3d::Texture white;
    white.name = "white";
    white.format = p3d::TextureFormat::BC1;
    white.width = white.height = 4;
    // BC1 block: both colours white, all indices 0.
    white.levels.push_back({0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0});
    p3d::Texture flat = white;
    flat.name = "flat normal";
    flat.levels[0] = {0x1f, 0x84, 0x1f, 0x84, 0, 0, 0, 0}; // (128, 128, 255) in RGB565
    if (!CreateTexture(white, white_) || !CreateTexture(flat, flat_normal_)) {
        error = "cannot create textures";
        return false;
    }
    default_material_ = CreateMaterial(nullptr, nullptr);

    // Each frame in flight: its uniforms and their descriptor set (with the shadow map).
    for (auto& f : frames_) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sizeof(FrameUniforms);
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo uai{};
        uai.usage = VMA_MEMORY_USAGE_AUTO;
        uai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(allocator_, &bi, &uai, &f.uniforms.buffer, &f.uniforms.allocation, &info) != VK_SUCCESS) {
            error = "cannot create the frame's uniforms";
            return false;
        }
        f.mapped = info.pMappedData;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = pool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &frame_set_layout_;
        if (vkAllocateDescriptorSets(device_, &dai, &f.set) != VK_SUCCESS) {
            error = "cannot allocate the frame's descriptors";
            return false;
        }
        VkDescriptorBufferInfo buffer{f.uniforms.buffer, 0, sizeof(FrameUniforms)};
        VkDescriptorImageInfo shadow{shadow_sampler_, shadow_view_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = f.set;
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &buffer;
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[1].dstSet = f.set;
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &shadow;
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    }
    return true;
}

bool Renderer::CreateShadowMap(std::string& error) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = kDepthFormat;
    ii.extent = {kShadowSize, kShadowSize, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(allocator_, &ii, &ai, &shadow_, &shadow_allocation_, nullptr) != VK_SUCCESS) {
        error = "cannot create the shadow map";
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = shadow_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kDepthFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCreateImageView(device_, &vi, nullptr, &shadow_view_);

    VkAttachmentDescription a{};
    a.format = kDepthFormat;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference depth{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &depth;
    VkSubpassDependency deps[2]{};
    // The last frame's reads of the map, before this frame writes it; this frame's writes, before
    // the main pass reads them.
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ri.attachmentCount = 1;
    ri.pAttachments = &a;
    ri.subpassCount = 1;
    ri.pSubpasses = &sub;
    ri.dependencyCount = 2;
    ri.pDependencies = deps;
    if (vkCreateRenderPass(device_, &ri, nullptr, &shadow_pass_) != VK_SUCCESS) {
        error = "cannot create the shadow pass";
        return false;
    }
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = shadow_pass_;
    fi.attachmentCount = 1;
    fi.pAttachments = &shadow_view_;
    fi.width = fi.height = kShadowSize;
    fi.layers = 1;
    vkCreateFramebuffer(device_, &fi, nullptr, &shadow_framebuffer_);
    return true;
}

bool Renderer::CreateSwapchain(std::string& error) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    std::uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
    if (formats.empty()) {
        error = "the window offers no image formats";
        return false;
    }
    VkSurfaceFormatKHR format = formats[0];
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            format = f;
            break;
        }
    }
    swapchain_format_ = format.format;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    extent_ = caps.currentExtent.width != UINT32_MAX
                  ? caps.currentExtent
                  : VkExtent2D{std::clamp<std::uint32_t>(w, caps.minImageExtent.width, caps.maxImageExtent.width),
                               std::clamp<std::uint32_t>(h, caps.minImageExtent.height, caps.maxImageExtent.height)};
    if (extent_.width == 0 || extent_.height == 0) {
        return true; // minimised: made when the window has a size again
    }
    std::uint32_t images = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) images = std::min(images + 1, caps.maxImageCount);
    VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    si.surface = surface_;
    si.minImageCount = images;
    si.imageFormat = format.format;
    si.imageColorSpace = format.colorSpace;
    si.imageExtent = extent_;
    si.imageArrayLayers = 1;
    si.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    si.preTransform = caps.currentTransform;
    si.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    si.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    si.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device_, &si, nullptr, &swapchain_) != VK_SUCCESS) {
        error = "cannot create the swapchain";
        return false;
    }
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());

    VkImageCreateInfo di{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    di.imageType = VK_IMAGE_TYPE_2D;
    di.format = kDepthFormat;
    di.extent = {extent_.width, extent_.height, 1};
    di.mipLevels = di.arrayLayers = 1;
    di.samples = VK_SAMPLE_COUNT_1_BIT;
    di.tiling = VK_IMAGE_TILING_OPTIMAL;
    di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VmaAllocationCreateInfo dai{};
    dai.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(allocator_, &di, &dai, &depth_, &depth_allocation_, nullptr) != VK_SUCCESS) {
        error = "cannot create the depth buffer";
        return false;
    }
    VkImageViewCreateInfo dvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    dvi.image = depth_;
    dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvi.format = kDepthFormat;
    dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCreateImageView(device_, &dvi, nullptr, &depth_view_);

    if (!render_pass_) {
        VkAttachmentDescription a[2]{};
        a[0].format = swapchain_format_;
        a[0].samples = VK_SAMPLE_COUNT_1_BIT;
        a[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        a[1].format = kDepthFormat;
        a[1].samples = VK_SAMPLE_COUNT_1_BIT;
        a[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &color;
        sub.pDepthStencilAttachment = &depth;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ri.attachmentCount = 2;
        ri.pAttachments = a;
        ri.subpassCount = 1;
        ri.pSubpasses = &sub;
        ri.dependencyCount = 1;
        ri.pDependencies = &dep;
        if (vkCreateRenderPass(device_, &ri, nullptr, &render_pass_) != VK_SUCCESS) {
            error = "cannot create the render pass";
            return false;
        }
    }
    for (VkImage image : images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swapchain_format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(device_, &vi, nullptr, &view);
        image_views_.push_back(view);
        VkImageView attachments[] = {view, depth_view_};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = render_pass_;
        fi.attachmentCount = 2;
        fi.pAttachments = attachments;
        fi.width = extent_.width;
        fi.height = extent_.height;
        fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        vkCreateFramebuffer(device_, &fi, nullptr, &fb);
        framebuffers_.push_back(fb);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        vkCreateSemaphore(device_, &sci, nullptr, &s);
        render_done_.push_back(s);
    }
    last_image_ = UINT32_MAX;
    return true;
}

void Renderer::DestroySwapchain() {
    vkDeviceWaitIdle(device_);
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto v : image_views_) vkDestroyImageView(device_, v, nullptr);
    for (auto s : render_done_) vkDestroySemaphore(device_, s, nullptr);
    framebuffers_.clear();
    image_views_.clear();
    render_done_.clear();
    images_.clear();
    if (depth_view_) vkDestroyImageView(device_, depth_view_, nullptr);
    if (depth_) vmaDestroyImage(allocator_, depth_, depth_allocation_);
    depth_view_ = VK_NULL_HANDLE;
    depth_ = VK_NULL_HANDLE;
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool Renderer::CreatePipeline(std::string& error) {
    // Set 0: a material (colour, normal map, mask). Set 1: the frame (uniforms, the shadow map).
    VkDescriptorSetLayoutBinding material[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        material[i].binding = i;
        material[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        material[i].descriptorCount = 1;
        material[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = material;
    vkCreateDescriptorSetLayout(device_, &li, nullptr, &set_layout_);
    li.bindingCount = 2;
    VkDescriptorSetLayoutBinding frame[2]{};
    frame[0].binding = 0;
    frame[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    frame[0].descriptorCount = 1;
    frame[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    frame[1].binding = 1;
    frame[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    frame[1].descriptorCount = 1;
    frame[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    li.pBindings = frame;
    vkCreateDescriptorSetLayout(device_, &li, nullptr, &frame_set_layout_);
    const VkDescriptorSetLayout sets[] = {set_layout_, frame_set_layout_};
    VkPushConstantRange range{kPushStages, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    vkCreatePipelineLayout(device_, &pli, nullptr, &pipeline_layout_);

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kMaxTextures + kFrames},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFrames}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = kMaxTextures + kFrames;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    vkCreateDescriptorPool(device_, &pi, nullptr, &pool_);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.anisotropyEnable = max_anisotropy_ > 1.0f ? VK_TRUE : VK_FALSE;
    sci.maxAnisotropy = max_anisotropy_;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(device_, &sci, nullptr, &sampler_);
    VkSamplerCreateInfo shi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    shi.magFilter = shi.minFilter = VK_FILTER_LINEAR;
    shi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    shi.addressModeU = shi.addressModeV = shi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shi.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    shi.compareEnable = VK_TRUE;
    shi.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCreateSampler(device_, &shi, nullptr, &shadow_sampler_);

    pipeline_ = MakePipeline(world_vert, sizeof(world_vert), world_frag, sizeof(world_frag), render_pass_, kWorld);
    decal_pipeline_ = MakePipeline(world_vert, sizeof(world_vert), world_frag, sizeof(world_frag), render_pass_, kDecal);
    sky_pipeline_ = MakePipeline(sky_vert, sizeof(sky_vert), sky_frag, sizeof(sky_frag), render_pass_, kSky);
    shadow_pipeline_ = MakePipeline(shadow_vert, sizeof(shadow_vert), shadow_frag, sizeof(shadow_frag), shadow_pass_, kShadow);
    if (!pipeline_ || !decal_pipeline_ || !sky_pipeline_ || !shadow_pipeline_) {
        error = "cannot create the pipelines";
        return false;
    }
    return true;
}

VkPipeline Renderer::MakePipeline(const std::uint32_t* vs_code, std::size_t vs_bytes, const std::uint32_t* fs_code,
                                  std::size_t fs_bytes, VkRenderPass pass, int kind) {
    VkShaderModule vs = Module(device_, vs_code, vs_bytes);
    VkShaderModule fs = Module(device_, fs_code, fs_bytes);
    if (!vs || !fs) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    VkVertexInputBindingDescription binding{0, sizeof(p3d::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(p3d::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(p3d::Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(p3d::Vertex, uv1)},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(p3d::Vertex, normal)},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(p3d::Vertex, tangent)},
        {5, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(p3d::Vertex, colour)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (kind != kSky) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = kind == kShadow ? 2 : 6;
        vi.pVertexAttributeDescriptions = attributes;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // the game's winding is not known yet
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    if (kind == kShadow) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = 2.0f;
        rs.depthBiasSlopeFactor = 2.5f;
    } else if (kind == kDecal) {
        rs.depthBiasEnable = VK_TRUE; // towards the camera (reverse Z)
        rs.depthBiasConstantFactor = 4.0f;
        rs.depthBiasSlopeFactor = 1.0f;
    }
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = kind == kWorld || kind == kShadow ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = kind == kShadow  ? VK_COMPARE_OP_LESS_OR_EQUAL
                        : kind == kWorld ? VK_COMPARE_OP_GREATER // reverse Z
                                         : VK_COMPARE_OP_GREATER_OR_EQUAL; // decals; the sky where nothing was drawn
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (kind == kDecal) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = kind == kShadow ? 0 : 1;
    cb.pAttachments = &blend;
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dyn;
    gi.layout = pipeline_layout_;
    gi.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gi, nullptr, &pipeline);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    return r == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
}

VkCommandBuffer Renderer::BeginOneShot() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = upload_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void Renderer::EndOneShot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, upload_pool_, 1, &cmd);
}

bool Renderer::Upload(const void* data, std::size_t size, VkBuffer dst) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator_, &bi, &ai, &staging, &allocation, &info) != VK_SUCCESS) return false;
    std::memcpy(info.pMappedData, data, size);
    vmaFlushAllocation(allocator_, allocation, 0, VK_WHOLE_SIZE);
    VkCommandBuffer cmd = BeginOneShot();
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd, staging, dst, 1, &copy);
    EndOneShot(cmd);
    vmaDestroyBuffer(allocator_, staging, allocation);
    return true;
}

bool Renderer::CreateModel(const std::vector<p3d::Vertex>& vertices, const std::vector<std::uint32_t>& indices, Model& out) {
    if (vertices.empty() || indices.empty()) return false;
    const auto make = [&](std::size_t size, VkBufferUsageFlags usage, GpuBuffer& b) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        return vmaCreateBuffer(allocator_, &bi, &ai, &b.buffer, &b.allocation, nullptr) == VK_SUCCESS;
    };
    const std::size_t vb = vertices.size() * sizeof(p3d::Vertex), ib = indices.size() * 4;
    if (!make(vb, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, out.vertices) || !make(ib, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, out.indices)) {
        return false;
    }
    return Upload(vertices.data(), vb, out.vertices.buffer) && Upload(indices.data(), ib, out.indices.buffer);
}

bool Renderer::CreateTexture(const p3d::Texture& t, GpuTexture& out) {
    if (t.levels.empty()) return false;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; // read as colour (sRGB) or as data
    ii.format = FormatOf(t.format, true);
    ii.extent = {t.width, t.height, 1};
    ii.mipLevels = static_cast<std::uint32_t>(t.levels.size());
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(allocator_, &ii, &ai, &out.image, &out.allocation, nullptr) != VK_SUCCESS) return false;

    std::size_t total = 0;
    for (const auto& l : t.levels) total += (l.size() + 15) & ~std::size_t(15);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = total;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo sai{};
    sai.usage = VMA_MEMORY_USAGE_AUTO;
    sai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation sa = nullptr;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator_, &bi, &sai, &staging, &sa, &info) != VK_SUCCESS) return false;
    std::vector<VkBufferImageCopy> copies;
    std::size_t at = 0;
    for (std::uint32_t level = 0; level < t.levels.size(); ++level) {
        std::memcpy(static_cast<std::uint8_t*>(info.pMappedData) + at, t.levels[level].data(), t.levels[level].size());
        VkBufferImageCopy c{};
        c.bufferOffset = at;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        c.imageExtent = {std::max(1u, t.width >> level), std::max(1u, t.height >> level), 1};
        copies.push_back(c);
        at += (t.levels[level].size() + 15) & ~std::size_t(15);
    }
    vmaFlushAllocation(allocator_, sa, 0, VK_WHOLE_SIZE);
    VkCommandBuffer cmd = BeginOneShot();
    Barrier(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, ii.mipLevels);
    vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(copies.size()), copies.data());
    Barrier(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, ii.mipLevels);
    EndOneShot(cmd);
    vmaDestroyBuffer(allocator_, staging, sa);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, ii.mipLevels, 0, 1};
    vkCreateImageView(device_, &vi, nullptr, &out.view);
    vi.format = FormatOf(t.format, false);
    vkCreateImageView(device_, &vi, nullptr, &out.linear);
    return out.view && out.linear;
}

VkDescriptorSet Renderer::CreateMaterial(const GpuTexture* colour, const GpuTexture* normal, const GpuTexture* mask) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(device_, &dai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo images[3] = {
        {sampler_, colour && colour->view ? colour->view : white_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler_, normal && normal->linear ? normal->linear : flat_normal_.linear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler_, mask && mask->linear ? mask->linear : white_.linear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet w[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
    return set;
}

void Renderer::DestroyModel(Model& m) {
    vkDeviceWaitIdle(device_);
    for (VkDescriptorSet set : m.materials) {
        if (set) vkFreeDescriptorSets(device_, pool_, 1, &set);
    }
    for (auto& t : m.textures) {
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
        if (t.linear) vkDestroyImageView(device_, t.linear, nullptr);
        if (t.image) vmaDestroyImage(allocator_, t.image, t.allocation);
    }
    if (m.vertices.buffer) vmaDestroyBuffer(allocator_, m.vertices.buffer, m.vertices.allocation);
    if (m.indices.buffer) vmaDestroyBuffer(allocator_, m.indices.buffer, m.indices.allocation);
    m = Model{};
}

void Renderer::Size(std::uint32_t& width, std::uint32_t& height) const {
    width = extent_.width;
    height = extent_.height;
}

void Renderer::Cascades(const FrameView& v, float m[4][16], float splits[4], float texels[4]) const {
    // Four slices of the view, each in a 2048 x 2048 quarter of the atlas. Each is fitted with a
    // sphere (its size does not change as the camera turns) and moved in whole texels, so the
    // shadows' edges stay still.
    static constexpr float kEnds[4] = {18.0f, 60.0f, 200.0f, 650.0f};
    const float k2 = Dot(v.right, v.right) + Dot(v.up, v.up);
    float l[3] = {-v.lighting.sun[0], -v.lighting.sun[1], -v.lighting.sun[2]}; // where the light goes
    Normalize3(l);
    const float ref[3] = {0.0f, std::fabs(l[1]) < 0.99f ? 1.0f : 0.0f, std::fabs(l[1]) < 0.99f ? 0.0f : 1.0f};
    float r[3], u[3];
    Cross3(ref, l, r);
    Normalize3(r);
    Cross3(l, r, u);
    constexpr float kDepth = 1500.0f; // metres each side of a cascade's centre, along the light
    float near_end = 0.25f;
    for (int i = 0; i < 4; ++i) {
        const float n = near_end, f = kEnds[i];
        float c = 0.5f * (f + n) * (1.0f + k2);
        if (c > f) c = f;
        const float radius = std::ceil(std::sqrt((f - c) * (f - c) + f * f * k2));
        const float texel = 2.0f * radius / float(kCascadeSize);
        const float centre[3] = {v.camera[0] + v.forward[0] * c, v.camera[1] + v.forward[1] * c, v.camera[2] + v.forward[2] * c};
        const float x = std::floor(Dot(r, centre) / texel) * texel;
        const float y = std::floor(Dot(u, centre) / texel) * texel;
        const float z = Dot(l, centre);
        float* o = m[i];
        std::fill(o, o + 16, 0.0f);
        for (int k = 0; k < 3; ++k) {
            o[k * 4 + 0] = r[k] / radius;
            o[k * 4 + 1] = u[k] / radius;
            o[k * 4 + 2] = l[k] / (2.0f * kDepth);
        }
        o[12] = -x / radius;
        o[13] = -y / radius;
        o[14] = (kDepth - z) / (2.0f * kDepth);
        o[15] = 1.0f;
        splits[i] = f;
        texels[i] = texel;
        near_end = f;
    }
}

bool Renderer::DrawFrame(const FrameView& view, const std::vector<const Model*>& models, const std::vector<bool>& hidden) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (w == 0 || h == 0) return false;
    if (resized_ || !swapchain_ || static_cast<std::uint32_t>(w) != extent_.width || static_cast<std::uint32_t>(h) != extent_.height) {
        DestroySwapchain();
        std::string error;
        if (!CreateSwapchain(error) || !swapchain_) return false;
        resized_ = false;
    }
    Frame& f = frames_[frame_];
    vkWaitForFences(device_, 1, &f.done, VK_TRUE, UINT64_MAX);
    std::uint32_t image = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        resized_ = true;
        return false;
    }
    vkResetFences(device_, 1, &f.done);

    // The frame's uniforms.
    const Lighting& lit = view.lighting;
    FrameUniforms u{};
    Cascades(view, u.cascades, u.splits, u.cascade_texel);
    const auto set4 = [](float out[4], const float in[3], float w) {
        out[0] = in[0], out[1] = in[1], out[2] = in[2], out[3] = w;
    };
    float sun[3] = {lit.sun[0], lit.sun[1], lit.sun[2]};
    Normalize3(sun);
    set4(u.camera_forward, view.forward, lit.exposure);
    set4(u.camera_right, view.right, 0);
    set4(u.camera_up, view.up, 0);
    set4(u.camera_position, view.camera, lit.fog_density);
    set4(u.sun_direction, sun, lit.shadows ? 1.0f : 0.0f);
    set4(u.sun_colour, lit.sun_colour, 0);
    set4(u.ambient, lit.ambient, 0);
    for (int k = 0; k < 2; ++k) {
        float d[3] = {lit.fill[k][0], lit.fill[k][1], lit.fill[k][2]};
        Normalize3(d);
        set4(u.fill_direction[k], d, 0);
        set4(u.fill_colour[k], lit.fill_colour[k], 0);
    }
    // The sky: the ambient light's colour overhead, a pale haze at the horizon.
    for (int k = 0; k < 3; ++k) {
        u.sky_zenith[k] = lit.ambient[k] * 0.75f;
        u.sky_horizon[k] = lit.ambient[k] * 0.5f + 0.24f * lit.sun_colour[k];
    }
    std::memcpy(f.mapped, &u, sizeof(u));
    vmaFlushAllocation(allocator_, f.uniforms.allocation, 0, VK_WHOLE_SIZE);

    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);

    PushConstants pc{};
    std::copy(view.camera, view.camera + 3, pc.camera);
    pc.camera[3] = lit.fog_density;
    std::uint32_t pushed_flags = UINT32_MAX;
    VkDescriptorSet bound = VK_NULL_HANDLE;
    const auto draw_model = [&](const Model& m, bool decals, bool shadow) {
        bool buffers = false;
        for (const Draw& d : m.draws) {
            if (d.group >= 0 && d.group < static_cast<int>(hidden.size()) && hidden[d.group]) continue;
            if (((d.flags & Material::Decal) != 0) != decals) continue;
            if (!buffers) {
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(f.cmd, 0, 1, &m.vertices.buffer, &offset);
                vkCmdBindIndexBuffer(f.cmd, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                buffers = true;
            }
            const std::uint32_t flags = shadow ? d.flags & Material::AlphaTest : d.flags;
            const VkDescriptorSet set = d.material && (!shadow || flags) ? d.material : default_material_;
            if (set != bound) {
                vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
                bound = set;
            }
            if (flags != pushed_flags) {
                vkCmdPushConstants(f.cmd, pipeline_layout_, kPushStages, kFlagsOffset, 4, &flags);
                pushed_flags = flags;
            }
            vkCmdDrawIndexed(f.cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
        }
    };

    // The sun's shadow map: each cascade into its quarter of the atlas.
    {
        VkClearValue clear{};
        clear.depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = shadow_pass_;
        rp.framebuffer = shadow_framebuffer_;
        rp.renderArea = {{0, 0}, {kShadowSize, kShadowSize}};
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(f.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        if (lit.shadows) {
            vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_);
            vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1, &f.set, 0, nullptr);
            for (int i = 0; i < 4; ++i) {
                const std::int32_t x = (i & 1) * kCascadeSize, y = (i >> 1) * kCascadeSize;
                VkViewport viewport{float(x), float(y), float(kCascadeSize), float(kCascadeSize), 0, 1};
                VkRect2D scissor{{x, y}, {kCascadeSize, kCascadeSize}};
                vkCmdSetViewport(f.cmd, 0, 1, &viewport);
                vkCmdSetScissor(f.cmd, 0, 1, &scissor);
                std::memcpy(pc.view_projection, u.cascades[i], sizeof(pc.view_projection));
                pc.material[0] = 0;
                vkCmdPushConstants(f.cmd, pipeline_layout_, kPushStages, 0, sizeof(pc), &pc);
                pushed_flags = 0;
                for (const Model* m : models) {
                    if (Visible(*m, u.cascades[i])) draw_model(*m, false, true);
                }
            }
        }
        vkCmdEndRenderPass(f.cmd);
    }

    // The view: the surfaces, the sky behind them, the decals over them.
    VkClearValue clear[2];
    clear[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clear[1].depthStencil = {0.0f, 0};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = render_pass_;
    rp.framebuffer = framebuffers_[image];
    rp.renderArea = {{0, 0}, extent_};
    rp.clearValueCount = 2;
    rp.pClearValues = clear;
    vkCmdBeginRenderPass(f.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, float(extent_.width), float(extent_.height), 0, 1};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(f.cmd, 0, 1, &viewport);
    vkCmdSetScissor(f.cmd, 0, 1, &scissor);
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    bound = VK_NULL_HANDLE;
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1, &f.set, 0, nullptr);
    std::memcpy(pc.view_projection, view.view_projection, sizeof(pc.view_projection));
    pc.material[0] = 0;
    vkCmdPushConstants(f.cmd, pipeline_layout_, kPushStages, 0, sizeof(pc), &pc);
    pushed_flags = 0;
    std::vector<const Model*> shown;
    for (const Model* m : models) {
        if (Visible(*m, view.view_projection)) shown.push_back(m);
    }
    for (const Model* m : shown) draw_model(*m, false, false);
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline_);
    vkCmdDraw(f.cmd, 3, 1, 0, 0);
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, decal_pipeline_);
    for (const Model* m : shown) draw_model(*m, true, false);
    vkCmdEndRenderPass(f.cmd);
    vkEndCommandBuffer(f.cmd);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_done_[image];
    vkQueueSubmit(queue_, 1, &si, f.done);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_done_[image];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image;
    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) resized_ = true;
    last_image_ = image;
    frame_ = (frame_ + 1) % kFrames;
    return true;
}

bool Renderer::SaveScreenshot(const std::string& path, std::string& error) {
    if (last_image_ == UINT32_MAX) {
        error = "no frame drawn yet";
        return false;
    }
    vkDeviceWaitIdle(device_);
    const std::size_t size = std::size_t(extent_.width) * extent_.height * 4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator_, &bi, &ai, &buffer, &allocation, &info) != VK_SUCCESS) {
        error = "no memory for the screenshot";
        return false;
    }
    VkImage image = images_[last_image_];
    VkCommandBuffer cmd = BeginOneShot();
    Barrier(cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &c);
    Barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    EndOneShot(cmd);
    vmaInvalidateAllocation(allocator_, allocation, 0, VK_WHOLE_SIZE);
    std::vector<std::uint8_t> rgb(std::size_t(extent_.width) * extent_.height * 3);
    const auto* src = static_cast<const std::uint8_t*>(info.pMappedData);
    const bool bgr = swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB || swapchain_format_ == VK_FORMAT_B8G8R8A8_UNORM;
    for (std::size_t i = 0; i < std::size_t(extent_.width) * extent_.height; ++i) {
        rgb[i * 3 + 0] = src[i * 4 + (bgr ? 2 : 0)];
        rgb[i * 3 + 1] = src[i * 4 + 1];
        rgb[i * 3 + 2] = src[i * 4 + (bgr ? 0 : 2)];
    }
    vmaDestroyBuffer(allocator_, buffer, allocation);
    std::size_t png_size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(rgb.data(), static_cast<int>(extent_.width),
                                                        static_cast<int>(extent_.height), 3, &png_size);
    if (!png) {
        error = "cannot encode the PNG";
        return false;
    }
    FILE* file = std::fopen(path.c_str(), "wb");
    const bool ok = file && std::fwrite(png, 1, png_size, file) == png_size;
    if (file) std::fclose(file);
    mz_free(png);
    if (!ok) error = "cannot write " + path;
    return ok;
}

void Renderer::Shutdown() {
    if (!device_) {
        if (surface_) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
        return;
    }
    vkDeviceWaitIdle(device_);
    Model own;
    own.textures = {white_, flat_normal_};
    own.materials = {default_material_};
    DestroyModel(own);
    DestroySwapchain();
    for (VkPipeline p : {pipeline_, decal_pipeline_, sky_pipeline_, shadow_pipeline_}) {
        if (p) vkDestroyPipeline(device_, p, nullptr);
    }
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (frame_set_layout_) vkDestroyDescriptorSetLayout(device_, frame_set_layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    if (shadow_sampler_) vkDestroySampler(device_, shadow_sampler_, nullptr);
    if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
    if (shadow_framebuffer_) vkDestroyFramebuffer(device_, shadow_framebuffer_, nullptr);
    if (shadow_pass_) vkDestroyRenderPass(device_, shadow_pass_, nullptr);
    if (shadow_view_) vkDestroyImageView(device_, shadow_view_, nullptr);
    if (shadow_) vmaDestroyImage(allocator_, shadow_, shadow_allocation_);
    for (auto& f : frames_) {
        if (f.uniforms.buffer) vmaDestroyBuffer(allocator_, f.uniforms.buffer, f.uniforms.allocation);
        vkDestroyFence(device_, f.done, nullptr);
        vkDestroySemaphore(device_, f.acquired, nullptr);
        vkDestroyCommandPool(device_, f.pool, nullptr);
    }
    vkDestroyCommandPool(device_, upload_pool_, nullptr);
    vmaDestroyAllocator(allocator_);
    vkDestroyDevice(device_, nullptr);
    if (messenger_) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance_, messenger_, nullptr);
    }
    SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
    vkDestroyInstance(instance_, nullptr);
}

} // namespace engine
