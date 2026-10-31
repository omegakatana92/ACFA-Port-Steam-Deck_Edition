// Vulkan foundation smoke test (Linux). Creates an instance, a device, an offscreen image, clears
// it with a known colour, copies it back and verifies the pixel. It is the Stage 1 gate that can
// run on CI or a headless machine with lavapipe; it does not present or draw game geometry.
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

#define VK_CHECK(x)                                                                      \
  do {                                                                                   \
    VkResult r_ = (x);                                                                   \
    if (r_ != VK_SUCCESS) {                                                              \
      std::fprintf(stderr, "%s failed: VkResult %d at %s:%d\n", #x, int(r_), __FILE__, __LINE__); \
      return 1;                                                                          \
    }                                                                                    \
  } while (0)

constexpr unsigned kWidth = 64, kHeight = 64;
constexpr float kColor[4] = {0.25f, 0.50f, 0.75f, 1.0f};

VkDevice g_device = VK_NULL_HANDLE;

void destroy() {
  if (g_device != VK_NULL_HANDLE) vkDeviceWaitIdle(g_device);
}

}  // namespace

int main() {
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "acfa-vk-smoke";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

  uint32_t count = 0;
  VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
  if (!count) {
    std::fprintf(stderr, "no Vulkan physical device\n");
    vkDestroyInstance(instance, nullptr);
    return 1;
  }
  std::vector<VkPhysicalDevice> gpus(count);
  VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, gpus.data()));

  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  uint32_t family = 0;
  for (VkPhysicalDevice g : gpus) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g, &n, nullptr);
    std::vector<VkQueueFamilyProperties> q(n);
    vkGetPhysicalDeviceQueueFamilyProperties(g, &n, q.data());
    for (uint32_t i = 0; i < n; ++i)
      if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (q[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
        gpu = g;
        family = i;
        break;
      }
    if (gpu) break;
  }
  if (!gpu) {
    std::fprintf(stderr, "no graphics+transfer queue\n");
    vkDestroyInstance(instance, nullptr);
    return 1;
  }
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(gpu, &props);
  std::printf("Vulkan device: %s (API %u.%u.%u)\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
              VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VkDevice device = VK_NULL_HANDLE;
  VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
  g_device = device;
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(device, family, 0, &queue);

  VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  img.imageType = VK_IMAGE_TYPE_2D;
  img.format = VK_FORMAT_R8G8B8A8_UNORM;
  img.extent = {kWidth, kHeight, 1};
  img.mipLevels = 1;
  img.arrayLayers = 1;
  img.samples = VK_SAMPLE_COUNT_1_BIT;
  img.tiling = VK_IMAGE_TILING_OPTIMAL;
  img.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  img.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImage(device, &img, nullptr, &image));
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device, image, &req);
  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
    if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      type = i;
      break;
    }
  if (type == UINT32_MAX) {
    std::fprintf(stderr, "no device-local image memory\n");
    return 1;
  }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  VkDeviceMemory image_mem = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &image_mem));
  VK_CHECK(vkBindImageMemory(device, image, image_mem, 0));

  const VkDeviceSize bytes = VkDeviceSize(kWidth) * kHeight * 4;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &buffer));
  vkGetBufferMemoryRequirements(device, buffer, &req);
  type = UINT32_MAX;
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
    if ((req.memoryTypeBits & (1u << i)) &&
        (mem.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      type = i;
      break;
    }
  if (type == UINT32_MAX) {
    std::fprintf(stderr, "no host-visible coherent buffer memory\n");
    return 1;
  }
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  VkDeviceMemory buffer_mem = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &buffer_mem));
  VK_CHECK(vkBindBufferMemory(device, buffer, buffer_mem, 0));

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pci.queueFamilyIndex = family;
  VkCommandPool pool = VK_NULL_HANDLE;
  VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));

  auto barrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  };

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
  barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkClearColorValue color{};
  std::memcpy(color.float32, kColor, sizeof kColor);
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
  barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {kWidth, kHeight, 1};
  vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
  VK_CHECK(vkEndCommandBuffer(cmd));

  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  VK_CHECK(vkCreateFence(device, &fci, nullptr, &fence));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
  VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

  void* mapped = nullptr;
  VK_CHECK(vkMapMemory(device, buffer_mem, 0, bytes, 0, &mapped));
  const auto* px = static_cast<const uint8_t*>(mapped);
  const uint8_t want[4] = {64, 128, 191, 255};  // 0.25, 0.50, 0.75, 1.0
  bool ok = true;
  for (size_t i = 0; i < size_t(kWidth) * kHeight; ++i) {
    if (px[4 * i + 0] != want[0] || px[4 * i + 1] != want[1] || px[4 * i + 2] != want[2] ||
        px[4 * i + 3] != want[3]) {
      ok = false;
      break;
    }
  }
  std::printf("clear/readback %s (%u x %u, first pixel %u,%u,%u,%u)\n", ok ? "OK" : "MISMATCH", kWidth, kHeight, px[0], px[1],
              px[2], px[3]);
  vkUnmapMemory(device, buffer_mem);

  vkDestroyFence(device, fence, nullptr);
  vkDestroyCommandPool(device, pool, nullptr);
  vkDestroyBuffer(device, buffer, nullptr);
  vkFreeMemory(device, buffer_mem, nullptr);
  vkDestroyImage(device, image, nullptr);
  vkFreeMemory(device, image_mem, nullptr);
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
  return ok ? 0 : 1;
}
