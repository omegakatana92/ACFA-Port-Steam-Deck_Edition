// Vulkan Stage 1 presenter. See docs/VULKAN_RENDERER.md (gate G1).
#include "runtime/gpu/vulkan/vk_presenter.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace rt::gpu::vk {
namespace {

struct State {
  SDL_Window* window = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkExtent2D extent{};
  std::vector<VkImage> images;
  std::vector<VkImageView> views;
  std::vector<VkCommandBuffer> cmds;
  std::vector<VkSemaphore> available;
  std::vector<VkSemaphore> rendered;
  std::vector<VkFence> fences;

  VkCommandPool pool = VK_NULL_HANDLE;
  VkImage src = VK_NULL_HANDLE;
  VkDeviceMemory src_mem = VK_NULL_HANDLE;
  VkExtent2D src_extent{};
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  void* staging_ptr = nullptr;
  VkDeviceSize staging_size = 0;

  VkBuffer readback = VK_NULL_HANDLE;
  VkDeviceMemory readback_mem = VK_NULL_HANDLE;
  void* readback_ptr = nullptr;
  VkDeviceSize readback_size = 0;

  std::mutex mu;
  bool ok = false;
  bool verify_ok = false;
  u32 frame = 0;
  std::vector<u8> last;
  u32 last_w = 0, last_h = 0;
};

State g;
constexpr const char* kName = "acfa-vulkan";
constexpr u32 kFrames = 2;

void set_error(std::string* error, const char* what, VkResult r) {
  if (error) *error = std::string(what) + " (VkResult " + std::to_string(int(r)) + ")";
}

bool find_memory(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags want, uint32_t* index) {
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
      *index = i;
      return true;
    }
  return false;
}

bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer* buf,
                   VkDeviceMemory* mem, void** mapped, std::string* error) {
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult r = vkCreateBuffer(g.device, &ci, nullptr, buf);
  if (r != VK_SUCCESS) {
    set_error(error, "vkCreateBuffer", r);
    return false;
  }
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(g.device, *buf, &req);
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(g.gpu, &mp);
  uint32_t type = 0;
  if (!find_memory(mp, req.memoryTypeBits, props, &type)) {
    if (error) *error = "no buffer memory type";
    return false;
  }
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  r = vkAllocateMemory(g.device, &ai, nullptr, mem);
  if (r != VK_SUCCESS) {
    set_error(error, "vkAllocateMemory(buffer)", r);
    return false;
  }
  vkBindBufferMemory(g.device, *buf, *mem, 0);
  if (mapped) {
    r = vkMapMemory(g.device, *mem, 0, size, 0, mapped);
    if (r != VK_SUCCESS) {
      set_error(error, "vkMapMemory", r);
      return false;
    }
  }
  return true;
}

void destroy_buffer(VkBuffer* buf, VkDeviceMemory* mem) {
  if (*buf) vkDestroyBuffer(g.device, *buf, nullptr);
  if (*mem) vkFreeMemory(g.device, *mem, nullptr);
  *buf = VK_NULL_HANDLE;
  *mem = VK_NULL_HANDLE;
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                   VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b.srcAccessMask = src_access;
  b.dstAccessMask = dst_access;
  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool choose_surface_format(VkPhysicalDevice gpu, VkSurfaceKHR surface, VkFormat* format) {
  uint32_t n = 0;
  if (vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, nullptr) != VK_SUCCESS || !n) return false;
  std::vector<VkSurfaceFormatKHR> list(n);
  if (vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, list.data()) != VK_SUCCESS) return false;
  for (const auto& f : list)
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      *format = f.format;
      return true;
    }
  for (const auto& f : list)
    if (f.format == VK_FORMAT_R8G8B8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      *format = f.format;
      return true;
    }
  *format = list[0].format;
  return true;
}

bool create_swapchain(std::string* error) {
  VkSurfaceCapabilitiesKHR caps{};
  VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.gpu, g.surface, &caps);
  if (r != VK_SUCCESS) {
    set_error(error, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
    return false;
  }
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g.window, &w, &h);
    extent.width = std::clamp<uint32_t>(uint32_t(std::max(w, 1)), caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp<uint32_t>(uint32_t(std::max(h, 1)), caps.minImageExtent.height, caps.maxImageExtent.height);
  }
  if (!extent.width || !extent.height) {
    if (error) *error = "zero-sized swapchain";
    return false;
  }
  VkFormat format = VK_FORMAT_UNDEFINED;
  if (!choose_surface_format(g.gpu, g.surface, &format)) {
    if (error) *error = "no surface format";
    return false;
  }

  uint32_t count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(g.gpu, g.surface, &count, nullptr);
  std::vector<VkPresentModeKHR> modes(count);
  if (count) vkGetPhysicalDeviceSurfacePresentModesKHR(g.gpu, g.surface, &count, modes.data());
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  for (VkPresentModeKHR m : modes)
    if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;

  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = g.surface;
  ci.minImageCount = std::clamp(caps.minImageCount + 1, caps.minImageCount, caps.maxImageCount ? caps.maxImageCount : caps.minImageCount + 1);
  ci.imageFormat = format;
  ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  ci.imageExtent = extent;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = mode;
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = g.swapchain;

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  r = vkCreateSwapchainKHR(g.device, &ci, nullptr, &swapchain);
  if (r != VK_SUCCESS) {
    set_error(error, "vkCreateSwapchainKHR", r);
    return false;
  }

  // Tear down the previous swapchain only after the new one exists.
  for (VkImageView v : g.views) vkDestroyImageView(g.device, v, nullptr);
  for (VkSemaphore s : g.available) vkDestroySemaphore(g.device, s, nullptr);
  for (VkSemaphore s : g.rendered) vkDestroySemaphore(g.device, s, nullptr);
  for (VkFence f : g.fences) vkDestroyFence(g.device, f, nullptr);
  if (!g.cmds.empty()) vkFreeCommandBuffers(g.device, g.pool, uint32_t(g.cmds.size()), g.cmds.data());
  if (g.swapchain) vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);

  g.swapchain = swapchain;
  g.format = format;
  g.extent = extent;
  g.views.clear();
  g.available.clear();
  g.rendered.clear();
  g.fences.clear();
  g.cmds.clear();

  uint32_t n = 0;
  vkGetSwapchainImagesKHR(g.device, g.swapchain, &n, nullptr);
  g.images.resize(n);
  vkGetSwapchainImagesKHR(g.device, g.swapchain, &n, g.images.data());
  g.views.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g.images[i];
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    r = vkCreateImageView(g.device, &vi, nullptr, &g.views[i]);
    if (r != VK_SUCCESS) {
      set_error(error, "vkCreateImageView", r);
      return false;
    }
  }

  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = g.pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = kFrames;
  g.cmds.resize(kFrames);
  r = vkAllocateCommandBuffers(g.device, &cai, g.cmds.data());
  if (r != VK_SUCCESS) {
    set_error(error, "vkAllocateCommandBuffers", r);
    return false;
  }
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  g.available.resize(kFrames);
  g.rendered.resize(kFrames);
  g.fences.resize(kFrames);
  for (uint32_t i = 0; i < kFrames; ++i) {
    if (vkCreateSemaphore(g.device, &si, nullptr, &g.available[i]) != VK_SUCCESS ||
        vkCreateSemaphore(g.device, &si, nullptr, &g.rendered[i]) != VK_SUCCESS ||
        vkCreateFence(g.device, &fi, nullptr, &g.fences[i]) != VK_SUCCESS) {
      if (error) *error = "cannot create swapchain synchronisation";
      return false;
    }
  }
  g.frame = 0;
  g.readback_size = VkDeviceSize(extent.width) * extent.height * 4;
  if (g.readback_ptr) vkUnmapMemory(g.device, g.readback_mem);
  g.readback_ptr = nullptr;
  destroy_buffer(&g.readback, &g.readback_mem);
  if (!create_buffer(g.readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g.readback,
                     &g.readback_mem, &g.readback_ptr, error))
    return false;
  return true;
}

bool ensure_source(u32 w, u32 h, std::string* error) {
  if (g.src && g.src_extent.width == w && g.src_extent.height == h) return true;
  vkDeviceWaitIdle(g.device);
  if (g.src) {
    vkDestroyImage(g.device, g.src, nullptr);
    vkFreeMemory(g.device, g.src_mem, nullptr);
    g.src = VK_NULL_HANDLE;
    g.src_mem = VK_NULL_HANDLE;
  }
  if (g.staging_ptr) vkUnmapMemory(g.device, g.staging_mem);
  g.staging_ptr = nullptr;
  destroy_buffer(&g.staging, &g.staging_mem);
  g.staging_ptr = nullptr;

  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = g.format;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkResult r = vkCreateImage(g.device, &ci, nullptr, &g.src);
  if (r != VK_SUCCESS) {
    set_error(error, "vkCreateImage(source)", r);
    return false;
  }
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(g.device, g.src, &req);
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(g.gpu, &mp);
  uint32_t type = 0;
  if (!find_memory(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
    if (error) *error = "no source image memory";
    return false;
  }
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  r = vkAllocateMemory(g.device, &ai, nullptr, &g.src_mem);
  if (r != VK_SUCCESS) {
    set_error(error, "vkAllocateMemory(source)", r);
    return false;
  }
  vkBindImageMemory(g.device, g.src, g.src_mem, 0);
  g.src_extent = {w, h};

  g.staging_size = VkDeviceSize(w) * h * 4;
  if (!create_buffer(g.staging_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g.staging, &g.staging_mem,
                     &g.staging_ptr, error))
    return false;

  g.readback_size = VkDeviceSize(g.extent.width) * g.extent.height * 4;
  destroy_buffer(&g.readback, &g.readback_mem);
  g.readback_ptr = nullptr;
  if (!create_buffer(g.readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g.readback, &g.readback_mem,
                     &g.readback_ptr, error))
    return false;
  return true;
}

bool upload(VkCommandBuffer cmd, const u8* rgba, u32 w, u32 h, std::string* error) {
  auto* dst = static_cast<u8*>(g.staging_ptr);
  const bool bgra = g.format == VK_FORMAT_B8G8R8A8_UNORM || g.format == VK_FORMAT_B8G8R8A8_SRGB;
  for (u32 y = 0; y < h; ++y) {
    const u8* s = rgba + size_t(y) * w * 4;
    u8* d = dst + size_t(y) * w * 4;
    if (!bgra) {
      std::memcpy(d, s, size_t(w) * 4);
    } else {
      for (u32 x = 0; x < w; ++x) {
        d[4 * x + 0] = s[4 * x + 2];
        d[4 * x + 1] = s[4 * x + 1];
        d[4 * x + 2] = s[4 * x + 0];
        d[4 * x + 3] = s[4 * x + 3];
      }
    }
  }
  image_barrier(cmd, g.src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  vkCmdCopyBufferToImage(cmd, g.staging, g.src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  image_barrier(cmd, g.src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
  (void)error;
  return true;
}

bool check_readback(u32 src_w, u32 src_h, VkRect2D dst, const u8* rgba) {
  if (!g.readback_ptr) return false;
  auto* p = static_cast<const u8*>(g.readback_ptr);
  const u32 cx = dst.offset.x + dst.extent.width / 2;
  const u32 cy = dst.offset.y + dst.extent.height / 2;
  if (cx >= g.extent.width || cy >= g.extent.height) return false;
  const u8* got = p + (size_t(cy) * g.extent.width + cx) * 4;
  // Reverse the blit: destination centre maps to source centre.
  const u32 sx = std::min(src_w - 1, uint32_t((uint64_t(cx - dst.offset.x) * src_w) / std::max(1u, dst.extent.width)));
  const u32 sy = std::min(src_h - 1, uint32_t((uint64_t(cy - dst.offset.y) * src_h) / std::max(1u, dst.extent.height)));
  const u8* want = rgba + (size_t(sy) * src_w + sx) * 4;
  if (g.format == VK_FORMAT_B8G8R8A8_UNORM || g.format == VK_FORMAT_B8G8R8A8_SRGB) {
    return got[0] == want[2] && got[1] == want[1] && got[2] == want[0] && got[3] == want[3];
  }
  return got[0] == want[0] && got[1] == want[1] && got[2] == want[2] && got[3] == want[3];
}

}  // namespace

bool init(void* sdl_window, std::string* error) {
  std::lock_guard lk(g.mu);
  if (g.ok) return true;
  g.window = static_cast<SDL_Window*>(sdl_window);
  if (!g.window) {
    if (error) *error = "no SDL window";
    return false;
  }
  if (!SDL_Vulkan_LoadLibrary(nullptr)) {
    if (error) *error = std::string("SDL_Vulkan_LoadLibrary: ") + SDL_GetError();
    return false;
  }
  uint32_t ext_count = 0;
  const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
  if (!exts) {
    if (error) *error = std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError();
    return false;
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = kName;
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = ext_count;
  ici.ppEnabledExtensionNames = exts;
  VkResult r = vkCreateInstance(&ici, nullptr, &g.instance);
  if (r != VK_SUCCESS) {
    set_error(error, "vkCreateInstance", r);
    return false;
  }
  if (!SDL_Vulkan_CreateSurface(g.window, g.instance, nullptr, &g.surface)) {
    if (error) *error = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
    return false;
  }

  uint32_t gpu_count = 0;
  vkEnumeratePhysicalDevices(g.instance, &gpu_count, nullptr);
  std::vector<VkPhysicalDevice> gpus(gpu_count);
  if (gpu_count) vkEnumeratePhysicalDevices(g.instance, &gpu_count, gpus.data());
  for (VkPhysicalDevice gpu : gpus) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
    std::vector<VkQueueFamilyProperties> q(n);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, q.data());
    for (uint32_t i = 0; i < n; ++i) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, g.surface, &present);
      if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
        g.gpu = gpu;
        g.family = i;
        break;
      }
    }
    if (g.gpu) break;
  }
  if (!g.gpu) {
    if (error) *error = "no Vulkan device with graphics and presentation";
    return false;
  }

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = g.family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char* dev_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dev_ext;
  r = vkCreateDevice(g.gpu, &dci, nullptr, &g.device);
  if (r != VK_SUCCESS) {
    set_error(error, "vkCreateDevice", r);
    return false;
  }
  vkGetDeviceQueue(g.device, g.family, 0, &g.queue);

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = g.family;
  r = vkCreateCommandPool(g.device, &pci, nullptr, &g.pool);
  if (r != VK_SUCCESS) {
    set_error(error, "vkCreateCommandPool", r);
    return false;
  }
  if (!create_swapchain(error)) return false;
  g.ok = true;
  return true;
}

bool ready() { return g.ok; }
u32 width() { return g.extent.width; }
u32 height() { return g.extent.height; }

bool present_rgba(const u8* rgba, u32 w, u32 h, std::string* error, bool verify) {
  std::lock_guard lk(g.mu);
  if (!g.ok || !rgba || !w || !h) {
    if (error) *error = "presenter not ready";
    return false;
  }
  int ww = 0, wh = 0;
  SDL_GetWindowSizeInPixels(g.window, &ww, &wh);
  if (ww > 0 && wh > 0 && (uint32_t(ww) != g.extent.width || uint32_t(wh) != g.extent.height)) {
    if (vkDeviceWaitIdle(g.device) != VK_SUCCESS || !create_swapchain(error)) return false;
  }
  if (!ensure_source(w, h, error)) return false;

  const u32 frame = g.frame;
  g.frame = (g.frame + 1) % kFrames;
  vkWaitForFences(g.device, 1, &g.fences[frame], VK_TRUE, UINT64_MAX);
  vkResetFences(g.device, 1, &g.fences[frame]);

  uint32_t index = 0;
  VkResult r = VK_SUCCESS;
  for (int attempt = 0; attempt < 4; ++attempt) {
    r = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, g.available[frame], VK_NULL_HANDLE, &index);
    if (r != VK_ERROR_OUT_OF_DATE_KHR && r != VK_SUBOPTIMAL_KHR) break;
    if (vkDeviceWaitIdle(g.device) != VK_SUCCESS || !create_swapchain(error)) return false;
  }
  if (r != VK_SUCCESS) {
    set_error(error, "vkAcquireNextImageKHR", r);
    return false;
  }
  VkCommandBuffer cmd = g.cmds[frame];
  vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);
  upload(cmd, rgba, w, h, error);

  image_barrier(cmd, g.images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkClearColorValue black{};
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmd, g.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

  // Aspect fit: the game/present path letterboxes rather than stretching.
  const double sw = double(g.extent.width) / double(w);
  const double sh = double(g.extent.height) / double(h);
  const double scale = std::min(sw, sh);
  const uint32_t dw = std::max(1u, uint32_t(double(w) * scale));
  const uint32_t dh = std::max(1u, uint32_t(double(h) * scale));
  VkRect2D dst{{int32_t((g.extent.width - dw) / 2), int32_t((g.extent.height - dh) / 2)}, {dw, dh}};
  // Integer alignment keeps the readback check exact.
  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[0] = {0, 0, 0};
  blit.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstOffsets[0] = {int32_t(dst.offset.x), int32_t(dst.offset.y), 0};
  blit.dstOffsets[1] = {int32_t(dst.offset.x + dst.extent.width), int32_t(dst.offset.y + dst.extent.height), 1};
  vkCmdBlitImage(cmd, g.src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                 &blit, VK_FILTER_LINEAR);

  if (verify) {
    image_barrier(cmd, g.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {g.extent.width, g.extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, g.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.readback, 1, &region);
    image_barrier(cmd, g.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  } else {
    image_barrier(cmd, g.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  }
  vkEndCommandBuffer(cmd);

  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &g.available[frame];
  si.pWaitDstStageMask = &wait_stage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &g.rendered[frame];
  r = vkQueueSubmit(g.queue, 1, &si, g.fences[frame]);
  if (r != VK_SUCCESS) {
    set_error(error, "vkQueueSubmit", r);
    return false;
  }
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &g.rendered[frame];
  pi.swapchainCount = 1;
  pi.pSwapchains = &g.swapchain;
  pi.pImageIndices = &index;
  r = vkQueuePresentKHR(g.queue, &pi);
  if (r != VK_SUCCESS && r != VK_ERROR_OUT_OF_DATE_KHR) {
    set_error(error, "vkQueuePresentKHR", r);
    return false;
  }
  if (verify) {
    vkWaitForFences(g.device, 1, &g.fences[frame], VK_TRUE, UINT64_MAX);
    g.verify_ok = check_readback(w, h, dst, rgba);
  }
  g.last.assign(rgba, rgba + size_t(w) * h * 4);
  g.last_w = w;
  g.last_h = h;
  return true;
}

bool present_test_pattern(double seconds, std::string* error, bool verify) {
  int w = 0, h = 0;
  if (g.window) SDL_GetWindowSizeInPixels(g.window, &w, &h);
  if (w <= 0 || h <= 0) {
    if (error) *error = "zero-sized window";
    return false;
  }
  std::vector<u8> img(size_t(w) * h * 4);
  const int bar = int(std::fmod(seconds * 160.0, double(w)));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      u8* p = &img[(size_t(y) * w + x) * 4];
      p[0] = u8((x * 255) / std::max(1, w - 1));
      p[1] = u8((y * 255) / std::max(1, h - 1));
      p[2] = u8(((x / 64 + y / 64) & 1) ? 190 : 40);
      p[3] = 255;
      if (x >= bar && x < bar + 32) {
        p[0] = p[1] = p[2] = 255;
      }
      // Corner markers: red top-left, green top-right, blue bottom-left, yellow bottom-right.
      if (x < 48 && y < 48) p[0] = 255, p[1] = 0, p[2] = 0;
      if (x >= w - 48 && y < 48) p[0] = 0, p[1] = 255, p[2] = 0;
      if (x < 48 && y >= h - 48) p[0] = 0, p[1] = 0, p[2] = 255;
      if (x >= w - 48 && y >= h - 48) p[0] = 255, p[1] = 255, p[2] = 0;
    }
  }
  return present_rgba(img.data(), u32(w), u32(h), error, verify);
}

bool last_rgba(std::vector<u8>& rgba, u32& w, u32& h) {
  std::lock_guard lk(g.mu);
  if (g.last.empty()) return false;
  rgba = g.last;
  w = g.last_w;
  h = g.last_h;
  return true;
}

bool last_verify_ok() { return g.verify_ok; }

void shutdown() {
  std::lock_guard lk(g.mu);
  if (g.device) vkDeviceWaitIdle(g.device);
  for (VkImageView v : g.views) vkDestroyImageView(g.device, v, nullptr);
  for (VkSemaphore s : g.available) vkDestroySemaphore(g.device, s, nullptr);
  for (VkSemaphore s : g.rendered) vkDestroySemaphore(g.device, s, nullptr);
  for (VkFence f : g.fences) vkDestroyFence(g.device, f, nullptr);
  if (!g.cmds.empty() && g.pool) vkFreeCommandBuffers(g.device, g.pool, uint32_t(g.cmds.size()), g.cmds.data());
  if (g.swapchain) vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
  if (g.pool) vkDestroyCommandPool(g.device, g.pool, nullptr);
  if (g.src) vkDestroyImage(g.device, g.src, nullptr);
  if (g.src_mem) vkFreeMemory(g.device, g.src_mem, nullptr);
  if (g.staging_ptr) vkUnmapMemory(g.device, g.staging_mem);
  if (g.readback_ptr) vkUnmapMemory(g.device, g.readback_mem);
  destroy_buffer(&g.staging, &g.staging_mem);
  destroy_buffer(&g.readback, &g.readback_mem);
  if (g.device) vkDestroyDevice(g.device, nullptr);
  if (g.surface) vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
  if (g.instance) vkDestroyInstance(g.instance, nullptr);
  if (g.window) SDL_Vulkan_UnloadLibrary();

  g.window = nullptr;
  g.instance = VK_NULL_HANDLE;
  g.surface = VK_NULL_HANDLE;
  g.gpu = VK_NULL_HANDLE;
  g.device = VK_NULL_HANDLE;
  g.queue = VK_NULL_HANDLE;
  g.family = 0;
  g.swapchain = VK_NULL_HANDLE;
  g.format = VK_FORMAT_UNDEFINED;
  g.extent = {};
  g.src_extent = {};
  g.images.clear();
  g.views.clear();
  g.cmds.clear();
  g.available.clear();
  g.rendered.clear();
  g.fences.clear();
  g.pool = VK_NULL_HANDLE;
  g.src = VK_NULL_HANDLE;
  g.src_mem = VK_NULL_HANDLE;
  g.staging = VK_NULL_HANDLE;
  g.staging_mem = VK_NULL_HANDLE;
  g.staging_ptr = nullptr;
  g.staging_size = 0;
  g.readback = VK_NULL_HANDLE;
  g.readback_mem = VK_NULL_HANDLE;
  g.readback_ptr = nullptr;
  g.readback_size = 0;
  g.ok = false;
  g.verify_ok = false;
  g.frame = 0;
  g.last.clear();
  g.last_w = g.last_h = 0;
}

}  // namespace rt::gpu::vk
