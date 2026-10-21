// SPDX-License-Identifier: GPL-2.0-or-later
// Manhattan Engine: the Vulkan renderer. One forward pass: the city's geometry with its colour
// textures, a sun, sky light and fog; reverse-Z depth with an infinite far plane.

#include "renderer.h"

#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "vma.h"

#include "miniz.h"

#include "world_frag.h"
#include "world_vert.h"

namespace engine {

namespace {

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr std::uint32_t kMaxTextures = 32768;

struct PushConstants {
    float view_projection[16];
    float camera[4]; ///< xyz, fog density
    float sun[4];    ///< xyz, material flags (bits: 1 textured)
};
static_assert(sizeof(PushConstants) == 96);

VKAPI_ATTR VkBool32 VKAPI_CALL OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                            void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        Log("Vulkan: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

VkFormat FormatOf(p3d::TextureFormat f) {
    switch (f) {
    case p3d::TextureFormat::BC1: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case p3d::TextureFormat::BC2: return VK_FORMAT_BC2_SRGB_BLOCK;
    case p3d::TextureFormat::BC3: return VK_FORMAT_BC3_SRGB_BLOCK;
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

    if (!CreateSwapchain(error) || !CreatePipeline(error)) {
        return false;
    }

    // A white texture for untextured parts (the shader ignores it, the layout wants one).
    p3d::Texture white;
    white.name = "white";
    white.format = p3d::TextureFormat::BC1;
    white.width = white.height = 4;
    // BC1 block: both colours white, all indices 0.
    white.levels.push_back({0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0});
    if (!CreateTexture(white, white_)) {
        error = "cannot create textures";
        return false;
    }
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
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    vkCreateDescriptorSetLayout(device_, &li, nullptr, &set_layout_);
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    vkCreatePipelineLayout(device_, &pli, nullptr, &pipeline_layout_);

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = kMaxTextures;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    vkCreateDescriptorPool(device_, &pi, nullptr, &pool_);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.anisotropyEnable = max_anisotropy_ > 1.0f ? VK_TRUE : VK_FALSE;
    sci.maxAnisotropy = max_anisotropy_;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(device_, &sci, nullptr, &sampler_);

    VkShaderModule vs = Module(device_, world_vert, sizeof(world_vert));
    VkShaderModule fs = Module(device_, world_frag, sizeof(world_frag));
    if (!vs || !fs) {
        error = "cannot load the shaders";
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    VkVertexInputBindingDescription binding{0, sizeof(p3d::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(p3d::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(p3d::Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(p3d::Vertex, normal)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(p3d::Vertex, colour)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // the game's winding is not known yet
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER; // reverse Z
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
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
    gi.renderPass = render_pass_;
    const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gi, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    if (r != VK_SUCCESS) {
        error = "cannot create the pipeline";
        return false;
    }
    return true;
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
    ii.format = FormatOf(t.format);
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
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(device_, &dai, &out.set) != VK_SUCCESS) return false;
    VkDescriptorImageInfo image{sampler_, out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = out.set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &image;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    return true;
}

void Renderer::DestroyModel(Model& m) {
    vkDeviceWaitIdle(device_);
    for (auto& t : m.textures) {
        if (t.set) vkFreeDescriptorSets(device_, pool_, 1, &t.set);
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
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
    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    VkClearValue clear[2];
    clear[0].color = {{0.30f, 0.38f, 0.48f, 1.0f}}; // the sky (linear)
    clear[1].depthStencil = {0.0f, 0};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = render_pass_;
    rp.framebuffer = framebuffers_[image];
    rp.renderArea = {{0, 0}, extent_};
    rp.clearValueCount = 2;
    rp.pClearValues = clear;
    vkCmdBeginRenderPass(f.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    VkViewport viewport{0, 0, float(extent_.width), float(extent_.height), 0, 1};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(f.cmd, 0, 1, &viewport);
    vkCmdSetScissor(f.cmd, 0, 1, &scissor);
    PushConstants pc{};
    std::memcpy(pc.view_projection, view.view_projection, sizeof(pc.view_projection));
    pc.camera[0] = view.camera[0];
    pc.camera[1] = view.camera[1];
    pc.camera[2] = view.camera[2];
    pc.camera[3] = view.fog_density;
    pc.sun[0] = view.sun[0];
    pc.sun[1] = view.sun[1];
    pc.sun[2] = view.sun[2];
    vkCmdPushConstants(f.cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    // A model whose bounds are wholly outside one side of the view is left out.
    const auto visible = [&](const Model& m) {
        if (m.max[0] - m.min[0] > 1e29f) return true;
        int outside[5] = {0, 0, 0, 0, 0};
        for (int corner = 0; corner < 8; ++corner) {
            const float p[3] = {corner & 1 ? m.max[0] : m.min[0], corner & 2 ? m.max[1] : m.min[1], corner & 4 ? m.max[2] : m.min[2]};
            float c[4];
            for (int r = 0; r < 4; ++r) {
                c[r] = view.view_projection[r] * p[0] + view.view_projection[4 + r] * p[1] + view.view_projection[8 + r] * p[2] +
                       view.view_projection[12 + r];
            }
            outside[0] += c[0] < -c[3];
            outside[1] += c[0] > c[3];
            outside[2] += c[1] < -c[3];
            outside[3] += c[1] > c[3];
            outside[4] += c[3] <= 0.0f; // behind the camera
        }
        for (int k : outside) {
            if (k == 8) return false;
        }
        return true;
    };
    for (const Model* m : models) {
        if (!visible(*m)) continue;
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(f.cmd, 0, 1, &m->vertices.buffer, &offset);
        vkCmdBindIndexBuffer(f.cmd, m->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        VkDescriptorSet bound = VK_NULL_HANDLE;
        float flags = -1.0f;
        for (const Draw& d : m->draws) {
            if (d.group >= 0 && d.group < static_cast<int>(hidden.size()) && hidden[d.group]) continue;
            const VkDescriptorSet set = d.texture ? d.texture : white_.set;
            if (set != bound) {
                vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
                bound = set;
            }
            const float want = d.texture ? 1.0f : 0.0f;
            if (want != flags) {
                vkCmdPushConstants(f.cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   offsetof(PushConstants, sun) + 12, 4, &want);
                flags = want;
            }
            vkCmdDrawIndexed(f.cmd, d.index_count, 1, d.first_index, d.vertex_offset, 0);
        }
    }
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
    Model white_model;
    white_model.textures.push_back(white_);
    DestroyModel(white_model);
    DestroySwapchain();
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
    for (auto& f : frames_) {
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
