#include "vulkan_renderer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "vk_shaders_spv.h"

namespace gbemu::frontend {

namespace {

constexpr int kMaxFramesInFlight = 2;

struct QueueFamilies {
  std::optional<std::uint32_t> graphics;
  std::optional<std::uint32_t> present;

  bool complete() const { return graphics.has_value() && present.has_value(); }
};

struct SwapchainSupport {
  VkSurfaceCapabilitiesKHR capabilities{};
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> present_modes;
};

std::string vk_result_string(VkResult result) {
  return std::to_string(static_cast<int>(result));
}

QueueFamilies find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) {
  QueueFamilies families;
  std::uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> props(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, props.data());
  for (std::uint32_t i = 0; i < count; ++i) {
    if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      families.graphics = i;
    }
    VkBool32 present_support = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_support);
    if (present_support == VK_TRUE) {
      families.present = i;
    }
    if (families.complete()) {
      break;
    }
  }
  return families;
}

SwapchainSupport query_swapchain_support(VkPhysicalDevice device, VkSurfaceKHR surface) {
  SwapchainSupport support;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &support.capabilities);

  std::uint32_t format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
  if (format_count > 0) {
    support.formats.resize(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, support.formats.data());
  }

  std::uint32_t mode_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &mode_count, nullptr);
  if (mode_count > 0) {
    support.present_modes.resize(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &mode_count,
                                              support.present_modes.data());
  }

  return support;
}

bool has_required_extensions(VkPhysicalDevice device) {
  std::uint32_t ext_count = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> extensions(ext_count);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, extensions.data());

  bool has_swapchain = false;
  for (const auto& ext : extensions) {
    if (std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
      has_swapchain = true;
      break;
    }
  }
  return has_swapchain;
}

VkSurfaceFormatKHR pick_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) {
  for (const auto& format : formats) {
    if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
        format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      return format;
    }
  }
  return formats[0];
}

VkPresentModeKHR pick_present_mode(const std::vector<VkPresentModeKHR>& modes) {
  for (auto mode : modes) {
    if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
      return mode;
    }
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D pick_extent(const VkSurfaceCapabilitiesKHR& caps, std::uint32_t width,
                       std::uint32_t height) {
  if (caps.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
    return caps.currentExtent;
  }
  VkExtent2D extent = {width, height};
  extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
  extent.height =
      std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  return extent;
}

}  // namespace

struct VulkanRenderer::Impl {
  SDL_Window* window = nullptr;

  VkInstance instance = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphics_queue = VK_NULL_HANDLE;
  VkQueue present_queue = VK_NULL_HANDLE;
  QueueFamilies queue_families;

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swap_format = VK_FORMAT_UNDEFINED;
  VkExtent2D swap_extent{};
  std::vector<VkImage> swap_images;
  std::vector<VkImageView> swap_views;

  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;

  VkCommandPool command_pool = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kMaxFramesInFlight> command_buffers{};

  std::array<VkSemaphore, kMaxFramesInFlight> image_available{};
  std::array<VkSemaphore, kMaxFramesInFlight> render_finished{};
  std::array<VkFence, kMaxFramesInFlight> in_flight_fences{};
  std::uint32_t frame_index = 0;

  VkImage frame_image = VK_NULL_HANDLE;
  VkDeviceMemory frame_image_memory = VK_NULL_HANDLE;
  VkImageView frame_image_view = VK_NULL_HANDLE;
  VkSampler frame_sampler = VK_NULL_HANDLE;
  bool frame_image_initialized = false;
  int frame_width = 0;
  int frame_height = 0;

  VkBuffer staging_buffer = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  std::size_t staging_size = 0;

  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

  std::vector<VkFramebuffer> framebuffers;
  std::atomic_bool framebuffer_resized{false};

  std::uint32_t find_memory_type(std::uint32_t filter, VkMemoryPropertyFlags props,
                                 std::string* error) {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      if ((filter & (1u << i)) != 0 && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
        return i;
      }
    }
    if (error) {
      *error = "No suitable Vulkan memory type found";
    }
    return std::numeric_limits<std::uint32_t>::max();
  }

  bool create_instance(std::string* error) {
    unsigned ext_count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &ext_count, nullptr)) {
      if (error) {
        *error = std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError();
      }
      return false;
    }
    std::vector<const char*> extensions(ext_count);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &ext_count, extensions.data())) {
      if (error) {
        *error = std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError();
      }
      return false;
    }

    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "GBEmu";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
    app_info.pEngineName = "GBEmu";
    app_info.engineVersion = VK_MAKE_VERSION(0, 0, 1);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    VkResult result = vkCreateInstance(&create_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreateInstance failed: " + vk_result_string(result);
      }
      return false;
    }

    if (!SDL_Vulkan_CreateSurface(window, instance, &surface)) {
      if (error) {
        *error = std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError();
      }
      return false;
    }

    return true;
  }

  bool pick_physical_device(std::string* error) {
    std::uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count == 0) {
      if (error) {
        *error = "No Vulkan physical devices found";
      }
      return false;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    for (auto dev : devices) {
      auto families = find_queue_families(dev, surface);
      if (!families.complete()) {
        continue;
      }
      if (!has_required_extensions(dev)) {
        continue;
      }
      auto swap = query_swapchain_support(dev, surface);
      if (swap.formats.empty() || swap.present_modes.empty()) {
        continue;
      }
      physical = dev;
      queue_families = families;
      break;
    }

    if (physical == VK_NULL_HANDLE) {
      if (error) {
        *error = "No suitable Vulkan GPU found";
      }
      return false;
    }

    return true;
  }

  bool create_device(std::string* error) {
    std::set<std::uint32_t> unique_queues = {*queue_families.graphics, *queue_families.present};
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    queue_infos.reserve(unique_queues.size());
    float priority = 1.0f;
    for (auto q : unique_queues) {
      VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queue_info.queueFamilyIndex = q;
      queue_info.queueCount = 1;
      queue_info.pQueuePriorities = &priority;
      queue_infos.push_back(queue_info);
    }

    VkPhysicalDeviceFeatures features{};

    const char* device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create_info.queueCreateInfoCount = static_cast<std::uint32_t>(queue_infos.size());
    create_info.pQueueCreateInfos = queue_infos.data();
    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = device_exts;
    create_info.pEnabledFeatures = &features;

    VkResult result = vkCreateDevice(physical, &create_info, nullptr, &device);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreateDevice failed: " + vk_result_string(result);
      }
      return false;
    }

    vkGetDeviceQueue(device, *queue_families.graphics, 0, &graphics_queue);
    vkGetDeviceQueue(device, *queue_families.present, 0, &present_queue);
    return true;
  }

  bool create_swapchain(std::string* error) {
    int w = 0;
    int h = 0;
    SDL_Vulkan_GetDrawableSize(window, &w, &h);
    if (w <= 0 || h <= 0) {
      w = 1;
      h = 1;
    }

    auto support = query_swapchain_support(physical, surface);
    VkSurfaceFormatKHR surface_format = pick_surface_format(support.formats);
    VkPresentModeKHR present_mode = pick_present_mode(support.present_modes);
    VkExtent2D extent = pick_extent(support.capabilities, static_cast<std::uint32_t>(w),
                                    static_cast<std::uint32_t>(h));

    std::uint32_t image_count = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 && image_count > support.capabilities.maxImageCount) {
      image_count = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR create_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create_info.surface = surface;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    std::uint32_t queue_indices[] = {*queue_families.graphics, *queue_families.present};
    if (*queue_families.graphics != *queue_families.present) {
      create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
      create_info.queueFamilyIndexCount = 2;
      create_info.pQueueFamilyIndices = queue_indices;
    } else {
      create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    create_info.preTransform = support.capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = VK_NULL_HANDLE;

    VkResult result = vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreateSwapchainKHR failed: " + vk_result_string(result);
      }
      return false;
    }

    vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
    swap_images.resize(image_count);
    vkGetSwapchainImagesKHR(device, swapchain, &image_count, swap_images.data());
    swap_format = surface_format.format;
    swap_extent = extent;
    return true;
  }

  bool create_swap_views(std::string* error) {
    swap_views.resize(swap_images.size());
    for (std::size_t i = 0; i < swap_images.size(); ++i) {
      VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_info.image = swap_images[i];
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_info.format = swap_format;
      view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      view_info.subresourceRange.baseMipLevel = 0;
      view_info.subresourceRange.levelCount = 1;
      view_info.subresourceRange.baseArrayLayer = 0;
      view_info.subresourceRange.layerCount = 1;

      VkResult result = vkCreateImageView(device, &view_info, nullptr, &swap_views[i]);
      if (result != VK_SUCCESS) {
        if (error) {
          *error = "vkCreateImageView failed: " + vk_result_string(result);
        }
        return false;
      }
    }
    return true;
  }

  bool create_render_pass(std::string* error) {
    VkAttachmentDescription color{};
    color.format = swap_format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &color;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 1;
    pass_info.pDependencies = &dep;

    VkResult result = vkCreateRenderPass(device, &pass_info, nullptr, &render_pass);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreateRenderPass failed: " + vk_result_string(result);
      }
      return false;
    }
    return true;
  }

  VkShaderModule create_shader_module(const std::uint32_t* code, std::size_t words,
                                      std::string* error) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words * sizeof(std::uint32_t);
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult result = vkCreateShaderModule(device, &info, nullptr, &module);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreateShaderModule failed: " + vk_result_string(result);
      }
      return VK_NULL_HANDLE;
    }
    return module;
  }

  bool create_pipeline(std::string* error) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo set_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout_info.bindingCount = 1;
    set_layout_info.pBindings = &binding;

    VkResult result =
        vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &descriptor_set_layout);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreateDescriptorSetLayout failed: " + vk_result_string(result);
      }
      return false;
    }

    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &descriptor_set_layout;
    result = vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreatePipelineLayout failed: " + vk_result_string(result);
      }
      return false;
    }

    VkShaderModule vert = create_shader_module(vkshader::kFullscreenVert.data(),
                                               vkshader::kFullscreenVert.size(), error);
    if (vert == VK_NULL_HANDLE) {
      return false;
    }
    VkShaderModule frag = create_shader_module(vkshader::kFullscreenFrag.data(),
                                               vkshader::kFullscreenFrag.size(), error);
    if (frag == VK_NULL_HANDLE) {
      vkDestroyShaderModule(device, vert, nullptr);
      return false;
    }

    VkPipelineShaderStageCreateInfo shader_stages[2] = {};
    shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stages[0].module = vert;
    shader_stages[0].pName = "main";
    shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shader_stages[1].module = frag;
    shader_stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swap_extent.width);
    viewport.height = static_cast<float>(swap_extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swap_extent;

    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msaa{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = shader_stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &msaa;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;

    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &pipeline);

    vkDestroyShaderModule(device, frag, nullptr);
    vkDestroyShaderModule(device, vert, nullptr);

    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreateGraphicsPipelines failed: " + vk_result_string(result);
      }
      return false;
    }
    return true;
  }

  bool create_command_pool(std::string* error) {
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = *queue_families.graphics;
    VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &command_pool);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreateCommandPool failed: " + vk_result_string(result);
      }
      return false;
    }

    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = static_cast<std::uint32_t>(command_buffers.size());
    result = vkAllocateCommandBuffers(device, &alloc, command_buffers.data());
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkAllocateCommandBuffers failed: " + vk_result_string(result);
      }
      return false;
    }

    return true;
  }

  bool create_sync(std::string* error) {
    VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < kMaxFramesInFlight; ++i) {
      if (vkCreateSemaphore(device, &sem_info, nullptr, &image_available[i]) != VK_SUCCESS ||
          vkCreateSemaphore(device, &sem_info, nullptr, &render_finished[i]) != VK_SUCCESS ||
          vkCreateFence(device, &fence_info, nullptr, &in_flight_fences[i]) != VK_SUCCESS) {
        if (error) {
          *error = "Failed to create Vulkan sync primitives";
        }
        return false;
      }
    }
    return true;
  }

  bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                     VkBuffer* buffer, VkDeviceMemory* memory, std::string* error) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(device, &info, nullptr, buffer);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreateBuffer failed: " + vk_result_string(result);
      }
      return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, *buffer, &req);

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits, props, error);
    if (alloc.memoryTypeIndex == std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }

    result = vkAllocateMemory(device, &alloc, nullptr, memory);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkAllocateMemory(buffer) failed: " + vk_result_string(result);
      }
      return false;
    }

    vkBindBufferMemory(device, *buffer, *memory, 0);
    return true;
  }

  bool create_image(std::uint32_t width, std::uint32_t height, VkFormat format,
                    VkImageUsageFlags usage, VkImage* image, VkDeviceMemory* memory,
                    std::string* error) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent.width = width;
    info.extent.height = height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.format = format;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = usage;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateImage(device, &info, nullptr, image);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreateImage failed: " + vk_result_string(result);
      }
      return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, *image, &req);

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, error);
    if (alloc.memoryTypeIndex == std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }

    result = vkAllocateMemory(device, &alloc, nullptr, memory);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkAllocateMemory(image) failed: " + vk_result_string(result);
      }
      return false;
    }

    vkBindImageMemory(device, *image, *memory, 0);
    return true;
  }

  bool create_frame_resources(int width, int height, std::string* error) {
    destroy_frame_resources();

    frame_width = width;
    frame_height = height;
    frame_image_initialized = false;

    if (!create_image(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                      VK_FORMAT_B8G8R8A8_UNORM,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &frame_image,
                      &frame_image_memory, error)) {
      return false;
    }

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = frame_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    VkResult result = vkCreateImageView(device, &view_info, nullptr, &frame_image_view);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreateImageView(frame) failed: " + vk_result_string(result);
      }
      return false;
    }

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.maxLod = 1.0f;
    result = vkCreateSampler(device, &sampler_info, nullptr, &frame_sampler);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkCreateSampler failed: " + vk_result_string(result);
      }
      return false;
    }

    VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
    if (!create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &staging_buffer, &staging_memory, error)) {
      return false;
    }
    staging_size = static_cast<std::size_t>(bytes);

    if (descriptor_pool == VK_NULL_HANDLE) {
      VkDescriptorPoolSize pool_size{};
      pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      pool_size.descriptorCount = 1;
      VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      pool_info.maxSets = 1;
      pool_info.poolSizeCount = 1;
      pool_info.pPoolSizes = &pool_size;
      result = vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool);
      if (result != VK_SUCCESS) {
        if (error) {
          *error = "vkCreateDescriptorPool failed: " + vk_result_string(result);
        }
        return false;
      }

      VkDescriptorSetAllocateInfo alloc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      alloc_info.descriptorPool = descriptor_pool;
      alloc_info.descriptorSetCount = 1;
      alloc_info.pSetLayouts = &descriptor_set_layout;
      result = vkAllocateDescriptorSets(device, &alloc_info, &descriptor_set);
      if (result != VK_SUCCESS) {
        if (error) {
          *error = "vkAllocateDescriptorSets failed: " + vk_result_string(result);
        }
        return false;
      }
    }

    VkDescriptorImageInfo image_info{};
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = frame_image_view;
    image_info.sampler = frame_sampler;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    return true;
  }

  void destroy_frame_resources() {
    if (device == VK_NULL_HANDLE) {
      return;
    }
    if (staging_buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(device, staging_buffer, nullptr);
      staging_buffer = VK_NULL_HANDLE;
    }
    if (staging_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, staging_memory, nullptr);
      staging_memory = VK_NULL_HANDLE;
    }
    staging_size = 0;

    if (frame_sampler != VK_NULL_HANDLE) {
      vkDestroySampler(device, frame_sampler, nullptr);
      frame_sampler = VK_NULL_HANDLE;
    }
    if (frame_image_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device, frame_image_view, nullptr);
      frame_image_view = VK_NULL_HANDLE;
    }
    if (frame_image != VK_NULL_HANDLE) {
      vkDestroyImage(device, frame_image, nullptr);
      frame_image = VK_NULL_HANDLE;
    }
    if (frame_image_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, frame_image_memory, nullptr);
      frame_image_memory = VK_NULL_HANDLE;
    }
    frame_image_initialized = false;
    frame_width = 0;
    frame_height = 0;
  }

  bool create_framebuffers(std::string* error) {
    framebuffers.resize(swap_views.size());
    for (std::size_t i = 0; i < swap_views.size(); ++i) {
      VkImageView attachments[] = {swap_views[i]};
      VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      info.renderPass = render_pass;
      info.attachmentCount = 1;
      info.pAttachments = attachments;
      info.width = swap_extent.width;
      info.height = swap_extent.height;
      info.layers = 1;
      VkResult result = vkCreateFramebuffer(device, &info, nullptr, &framebuffers[i]);
      if (result != VK_SUCCESS) {
        if (error) {
          *error = "vkCreateFramebuffer failed: " + vk_result_string(result);
        }
        return false;
      }
    }
    return true;
  }

  void destroy_swapchain_objects() {
    if (device == VK_NULL_HANDLE) {
      return;
    }
    for (auto fb : framebuffers) {
      vkDestroyFramebuffer(device, fb, nullptr);
    }
    framebuffers.clear();

    for (auto view : swap_views) {
      vkDestroyImageView(device, view, nullptr);
    }
    swap_views.clear();
    swap_images.clear();

    if (swapchain != VK_NULL_HANDLE) {
      vkDestroySwapchainKHR(device, swapchain, nullptr);
      swapchain = VK_NULL_HANDLE;
    }
  }

  bool recreate_swapchain(std::string* error) {
    int w = 0;
    int h = 0;
    SDL_Vulkan_GetDrawableSize(window, &w, &h);
    while (w == 0 || h == 0) {
      SDL_WaitEvent(nullptr);
      SDL_Vulkan_GetDrawableSize(window, &w, &h);
    }

    vkDeviceWaitIdle(device);

    destroy_swapchain_objects();

    if (!create_swapchain(error)) {
      return false;
    }
    if (!create_swap_views(error)) {
      return false;
    }
    if (!create_framebuffers(error)) {
      return false;
    }

    framebuffer_resized.store(false, std::memory_order_relaxed);
    return true;
  }

  bool record_command_buffer(VkCommandBuffer cmd, std::uint32_t image_index, std::string* error) {
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkResult result = vkBeginCommandBuffer(cmd, &begin);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkBeginCommandBuffer failed: " + vk_result_string(result);
      }
      return false;
    }

    VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.oldLayout =
        frame_image_initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = frame_image;
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.baseMipLevel = 0;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.baseArrayLayer = 0;
    to_transfer.subresourceRange.layerCount = 1;
    to_transfer.srcAccessMask = frame_image_initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
                         frame_image_initialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                                 : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_transfer);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {static_cast<std::uint32_t>(frame_width),
                          static_cast<std::uint32_t>(frame_height), 1};

    vkCmdCopyBufferToImage(cmd, staging_buffer, frame_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);

    VkImageMemoryBarrier to_shader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader.image = frame_image;
    to_shader.subresourceRange = to_transfer.subresourceRange;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_shader);

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderPassBeginInfo pass_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass_begin.renderPass = render_pass;
    pass_begin.framebuffer = framebuffers[image_index];
    pass_begin.renderArea.offset = {0, 0};
    pass_begin.renderArea.extent = swap_extent;
    pass_begin.clearValueCount = 1;
    pass_begin.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                            &descriptor_set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    result = vkEndCommandBuffer(cmd);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkEndCommandBuffer failed: " + vk_result_string(result);
      }
      return false;
    }

    frame_image_initialized = true;
    return true;
  }

  bool init(SDL_Window* in_window, int initial_width, int initial_height, std::string* error) {
    window = in_window;
    if (!create_instance(error) || !pick_physical_device(error) || !create_device(error) ||
        !create_swapchain(error) || !create_swap_views(error) || !create_render_pass(error) ||
        !create_pipeline(error) || !create_command_pool(error) || !create_sync(error) ||
        !create_framebuffers(error) ||
        !create_frame_resources(std::max(1, initial_width), std::max(1, initial_height), error)) {
      return false;
    }
    return true;
  }

  bool draw(const std::uint32_t* pixels, int width, int height, std::string* error) {
    if (!pixels || width <= 0 || height <= 0) {
      return true;
    }

    if (framebuffer_resized.load(std::memory_order_relaxed)) {
      if (!recreate_swapchain(error)) {
        return false;
      }
    }

    if (width != frame_width || height != frame_height) {
      vkDeviceWaitIdle(device);
      if (!create_frame_resources(width, height, error)) {
        return false;
      }
    }

    std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    if (bytes > staging_size) {
      if (error) {
        *error = "Staging buffer too small";
      }
      return false;
    }

    void* mapped = nullptr;
    VkResult result = vkMapMemory(device, staging_memory, 0, bytes, 0, &mapped);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkMapMemory failed: " + vk_result_string(result);
      }
      return false;
    }
    std::memcpy(mapped, pixels, bytes);
    vkUnmapMemory(device, staging_memory);

    result = vkWaitForFences(device, 1, &in_flight_fences[frame_index], VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkWaitForFences failed: " + vk_result_string(result);
      }
      return false;
    }

    std::uint32_t image_index = 0;
    result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, image_available[frame_index],
                                   VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      if (!recreate_swapchain(error)) {
        return false;
      }
      return true;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
      if (error) {
        *error = "vkAcquireNextImageKHR failed: " + vk_result_string(result);
      }
      return false;
    }

    vkResetFences(device, 1, &in_flight_fences[frame_index]);
    vkResetCommandBuffer(command_buffers[frame_index], 0);

    if (!record_command_buffer(command_buffers[frame_index], image_index, error)) {
      return false;
    }

    VkSemaphore wait_sems[] = {image_available[frame_index]};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signal_sems[] = {render_finished[frame_index]};

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = wait_sems;
    submit.pWaitDstStageMask = wait_stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffers[frame_index];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = signal_sems;

    result = vkQueueSubmit(graphics_queue, 1, &submit, in_flight_fences[frame_index]);
    if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkQueueSubmit failed: " + vk_result_string(result);
      }
      return false;
    }

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = signal_sems;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &image_index;

    result = vkQueuePresentKHR(present_queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        framebuffer_resized.load(std::memory_order_relaxed)) {
      if (!recreate_swapchain(error)) {
        return false;
      }
    } else if (result != VK_SUCCESS) {
      if (error) {
        *error = "vkQueuePresentKHR failed: " + vk_result_string(result);
      }
      return false;
    }

    frame_index = (frame_index + 1) % kMaxFramesInFlight;
    return true;
  }

  void shutdown() {
    if (device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device);
    }

    destroy_frame_resources();
    destroy_swapchain_objects();

    if (device != VK_NULL_HANDLE) {
      if (descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        descriptor_pool = VK_NULL_HANDLE;
      }
      if (descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
        descriptor_set_layout = VK_NULL_HANDLE;
      }
      if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
      }
      if (pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        pipeline_layout = VK_NULL_HANDLE;
      }
      if (render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, render_pass, nullptr);
        render_pass = VK_NULL_HANDLE;
      }
      if (command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, command_pool, nullptr);
        command_pool = VK_NULL_HANDLE;
      }
      for (int i = 0; i < kMaxFramesInFlight; ++i) {
        if (image_available[i] != VK_NULL_HANDLE) {
          vkDestroySemaphore(device, image_available[i], nullptr);
          image_available[i] = VK_NULL_HANDLE;
        }
        if (render_finished[i] != VK_NULL_HANDLE) {
          vkDestroySemaphore(device, render_finished[i], nullptr);
          render_finished[i] = VK_NULL_HANDLE;
        }
        if (in_flight_fences[i] != VK_NULL_HANDLE) {
          vkDestroyFence(device, in_flight_fences[i], nullptr);
          in_flight_fences[i] = VK_NULL_HANDLE;
        }
      }

      vkDestroyDevice(device, nullptr);
      device = VK_NULL_HANDLE;
    }

    if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
      vkDestroySurfaceKHR(instance, surface, nullptr);
      surface = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
      vkDestroyInstance(instance, nullptr);
      instance = VK_NULL_HANDLE;
    }
  }
};

VulkanRenderer::~VulkanRenderer() { shutdown(); }

bool VulkanRenderer::init(SDL_Window* window, int initial_width, int initial_height,
                          std::string* error) {
  shutdown();
  impl_ = new Impl();
  if (!impl_->init(window, initial_width, initial_height, error)) {
    shutdown();
    return false;
  }
  return true;
}

void VulkanRenderer::shutdown() {
  if (!impl_) {
    return;
  }
  impl_->shutdown();
  delete impl_;
  impl_ = nullptr;
}

bool VulkanRenderer::draw_frame(const std::uint32_t* pixels, int width, int height,
                                std::string* error) {
  if (!impl_) {
    if (error) {
      *error = "Vulkan renderer is not initialized";
    }
    return false;
  }
  return impl_->draw(pixels, width, height, error);
}

void VulkanRenderer::notify_resize() {
  if (impl_) {
    impl_->framebuffer_resized.store(true, std::memory_order_relaxed);
  }
}

}  // namespace gbemu::frontend
