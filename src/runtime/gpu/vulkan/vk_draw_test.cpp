// Vulkan Stage 2 gate: DXC-compiled HLSL with shifted bindings and a synthetic draw.
// Proves, without game data:
//   * DXC -> SPIR-V for SM 6.2 ByteAddressBuffer.Load4,
//   * buffer at t0 -> Vulkan binding 1000, texture at t1 -> 1001, sampler at s0 -> 2000,
//     UBO b0 -> 0 and b1 -> 1,
//   * a negative-height viewport reproduces D3D's y-up convention (texture orientation),
//   * texture sampling + uniform tint through the descriptor set.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef STAGE2_VS_SPV
#define STAGE2_VS_SPV ""
#endif
#ifndef STAGE2_PS_SPV
#define STAGE2_PS_SPV ""
#endif

namespace {

using u8 = uint8_t;
int g_validation_errors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    std::fprintf(stderr, "validation: %s\n", data->pMessage);
    ++g_validation_errors;
  }
  return VK_FALSE;
}

#define VK_CHECK(x)                                                                        \
  do {                                                                                     \
    VkResult r_ = (x);                                                                     \
    if (r_ != VK_SUCCESS) {                                                                \
      std::fprintf(stderr, "%s failed: VkResult %d at %s:%d\n", #x, int(r_), __FILE__, __LINE__); \
      return 1;                                                                            \
    }                                                                                      \
  } while (0)

struct Buffer {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  void* map = nullptr;
};

uint32_t find_memory(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}

bool make_buffer(VkDevice dev, VkPhysicalDevice gpu, VkDeviceSize size, VkBufferUsageFlags usage, Buffer* out) {
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(dev, &ci, nullptr, &out->buf) != VK_SUCCESS) return false;
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(dev, out->buf, &req);
  uint32_t type = find_memory(gpu, req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (type == UINT32_MAX) return false;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  if (vkAllocateMemory(dev, &ai, nullptr, &out->mem) != VK_SUCCESS) return false;
  vkBindBufferMemory(dev, out->buf, out->mem, 0);
  if (vkMapMemory(dev, out->mem, 0, size, 0, &out->map) != VK_SUCCESS) return false;
  return true;
}

std::vector<char> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  std::streamsize n = f.tellg();
  f.seekg(0);
  std::vector<char> d;
  d.resize(size_t(n));
  if (n && !f.read(d.data(), n)) return {};
  return d;
}

VkShaderModule module(VkDevice dev, const std::vector<char>& code) {
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = code.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule m = VK_NULL_HANDLE;
  if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
  return m;
}

bool near(u8 a, u8 b) { return std::abs(int(a) - int(b)) <= 2; }

}  // namespace

int main(int argc, char** argv) {
  const char* vs_path = STAGE2_VS_SPV;
  const char* ps_path = STAGE2_PS_SPV;
  bool want_validation = false;
  bool no_flip = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--validate")) want_validation = true;
    else if (!std::strcmp(argv[i], "--no-flip")) no_flip = true;
    else if (!vs_path[0]) vs_path = argv[i];
    else ps_path = argv[i];
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "acfa-vk-draw";
  app.apiVersion = VK_API_VERSION_1_1;

  bool have_validation = false;
  uint32_t layer_count = 0;
  vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
  std::vector<VkLayerProperties> layers(layer_count);
  vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
  for (const auto& l : layers)
    if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) have_validation = true;
  have_validation = have_validation && want_validation;

  const char* exts[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  const char* layer_names[] = {"VK_LAYER_KHRONOS_validation"};
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  if (have_validation) {
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layer_names;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = exts;
  }
  VkInstance instance = VK_NULL_HANDLE;
  VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  if (have_validation) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (create) {
      VkDebugUtilsMessengerCreateInfoEXT ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
      ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
      ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      ci.pfnUserCallback = debug_cb;
      create(instance, &ci, nullptr, &messenger);
    }
  }

  uint32_t gpu_count = 0;
  vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
  if (!gpu_count) {
    std::fprintf(stderr, "no Vulkan device\n");
    return 1;
  }
  std::vector<VkPhysicalDevice> gpus(gpu_count);
  vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
  VkPhysicalDevice gpu = gpus[0];
  uint32_t family = 0;
  {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
    std::vector<VkQueueFamilyProperties> q(n);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, q.data());
    for (uint32_t i = 0; i < n; ++i)
      if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        family = i;
        break;
      }
  }
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VkDevice dev = VK_NULL_HANDLE;
  VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &dev));
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(dev, family, 0, &queue);

  constexpr uint32_t kW = 64, kH = 64;
  VkImageCreateInfo img_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  img_ci.imageType = VK_IMAGE_TYPE_2D;
  img_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
  img_ci.extent = {kW, kH, 1};
  img_ci.mipLevels = 1;
  img_ci.arrayLayers = 1;
  img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
  img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage color = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImage(dev, &img_ci, nullptr, &color));
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(dev, color, &req);
  uint32_t type = find_memory(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == UINT32_MAX) {
    std::fprintf(stderr, "no device memory\n");
    return 1;
  }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  VkDeviceMemory color_mem = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &color_mem));
  VK_CHECK(vkBindImageMemory(dev, color, color_mem, 0));
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = color;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView color_view = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &color_view));

  // 2x2 texture: TL red, TR green, BL blue, BR white (row 0 is the top).
  const u8 texels[2 * 2 * 4] = {
      255, 0,   0,   255, 0,   255, 0, 255,
      0,   0,   255, 255, 255, 255, 255, 255,
  };
  VkImageCreateInfo ti{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ti.imageType = VK_IMAGE_TYPE_2D;
  ti.format = VK_FORMAT_R8G8B8A8_UNORM;
  ti.extent = {2, 2, 1};
  ti.mipLevels = 1;
  ti.arrayLayers = 1;
  ti.samples = VK_SAMPLE_COUNT_1_BIT;
  ti.tiling = VK_IMAGE_TILING_OPTIMAL;
  ti.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ti.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage tex = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImage(dev, &ti, nullptr, &tex));
  vkGetImageMemoryRequirements(dev, tex, &req);
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = find_memory(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX) {
    std::fprintf(stderr, "no device memory for texture\n");
    return 1;
  }
  VkDeviceMemory tex_mem = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &tex_mem));
  VK_CHECK(vkBindImageMemory(dev, tex, tex_mem, 0));
  VkImageViewCreateInfo tvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  tvi.image = tex;
  tvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  tvi.format = VK_FORMAT_R8G8B8A8_UNORM;
  tvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView tex_view = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImageView(dev, &tvi, nullptr, &tex_view));

  Buffer tex_stage{}, vb{}, ubo_ps{}, ubo_vs{}, readback{};
  if (!make_buffer(dev, gpu, sizeof texels, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &tex_stage) ||
      !make_buffer(dev, gpu, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &vb) ||
      !make_buffer(dev, gpu, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ubo_ps) ||
      !make_buffer(dev, gpu, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ubo_vs) ||
      !make_buffer(dev, gpu, kW * kH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
    std::fprintf(stderr, "buffer allocation failed\n");
    return 1;
  }
  std::memcpy(tex_stage.map, texels, sizeof texels);
  // D3D-style clip rectangle: x0, y0, x1, y1 (y up). Load4 in the VS reads all four.
  const float corners[4] = {-0.8f, 0.8f, 0.8f, -0.2f};
  std::memcpy(vb.map, corners, sizeof corners);
  const float tint[4] = {1, 1, 1, 1};
  std::memcpy(ubo_ps.map, tint, sizeof tint);
  const float offset[4] = {0, 0, 0, 0};
  std::memcpy(ubo_vs.map, offset, sizeof offset);

  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = VK_FILTER_NEAREST;
  sci.minFilter = VK_FILTER_NEAREST;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler = VK_NULL_HANDLE;
  VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &sampler));

  VkDescriptorSetLayoutBinding binds[5]{};
  binds[0].binding = 0;
  binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  binds[0].descriptorCount = 1;
  binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  binds[1].binding = 1;
  binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  binds[1].descriptorCount = 1;
  binds[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  binds[2].binding = 1000;
  binds[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binds[2].descriptorCount = 1;
  binds[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  binds[3].binding = 1001;
  binds[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  binds[3].descriptorCount = 1;
  binds[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  binds[4].binding = 2000;
  binds[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  binds[4].descriptorCount = 1;
  binds[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  slci.bindingCount = 5;
  slci.pBindings = binds;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VK_CHECK(vkCreateDescriptorSetLayout(dev, &slci, nullptr, &set_layout));
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &set_layout;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &pipeline_layout));

  auto vs_code = read_file(vs_path);
  auto ps_code = read_file(ps_path);
  if (vs_code.empty() || ps_code.empty()) {
    std::fprintf(stderr, "cannot read SPIR-V: %s / %s\n", vs_path, ps_path);
    return 1;
  }
  VkShaderModule vs = module(dev, vs_code), ps = module(dev, ps_code);
  if (!vs || !ps) {
    std::fprintf(stderr, "shader module creation failed\n");
    return 1;
  }

  VkAttachmentDescription att{};
  att.format = VK_FORMAT_R8G8B8A8_UNORM;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 1;
  rpci.pAttachments = &att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  VkRenderPass rp = VK_NULL_HANDLE;
  VK_CHECK(vkCreateRenderPass(dev, &rpci, nullptr, &rp));
  VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbci.renderPass = rp;
  fbci.attachmentCount = 1;
  fbci.pAttachments = &color_view;
  fbci.width = kW;
  fbci.height = kH;
  fbci.layers = 1;
  VkFramebuffer fb = VK_NULL_HANDLE;
  VK_CHECK(vkCreateFramebuffer(dev, &fbci, nullptr, &fb));

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = ps;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkViewport vp = no_flip ? VkViewport{0, 0, float(kW), float(kH), 0, 1}
                          : VkViewport{0, float(kH), float(kW), -float(kH), 0, 1};  // negative height: D3D y-up
  VkRect2D sc{{0, 0}, {kW, kH}};
  VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vps.viewportCount = 1;
  vps.pViewports = &vp;
  vps.scissorCount = 1;
  vps.pScissors = &sc;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState ba{};
  ba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &ba;
  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2;
  gp.pStages = stages;
  gp.pVertexInputState = &vin;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vps;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb;
  gp.layout = pipeline_layout;
  gp.renderPass = rp;
  gp.subpass = 0;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline));

  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
  };
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 4;
  dpci.pPoolSizes = sizes;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VK_CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &pool));
  VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &set_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));

  VkDescriptorBufferInfo bi_ps{ubo_ps.buf, 0, 16}, bi_vs{ubo_vs.buf, 0, 16}, bi_vb{vb.buf, 0, 16};
  VkDescriptorImageInfo ii_tex{};
  ii_tex.imageView = tex_view;
  ii_tex.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo ii_smp{};
  ii_smp.sampler = sampler;
  VkWriteDescriptorSet writes[5]{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = set;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[0].pBufferInfo = &bi_ps;
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = set;
  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[1].pBufferInfo = &bi_vs;
  writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet = set;
  writes[2].dstBinding = 1000;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[2].pBufferInfo = &bi_vb;
  writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[3].dstSet = set;
  writes[3].dstBinding = 1001;
  writes[3].descriptorCount = 1;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[3].pImageInfo = &ii_tex;
  writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[4].dstSet = set;
  writes[4].dstBinding = 2000;
  writes[4].descriptorCount = 1;
  writes[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  writes[4].pImageInfo = &ii_smp;
  vkUpdateDescriptorSets(dev, 5, writes, 0, nullptr);

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.queueFamilyIndex = family;
  VkCommandPool pool_cmd = VK_NULL_HANDLE;
  VK_CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool_cmd));
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = pool_cmd;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
  VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &cbbi));

  auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                     VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
  };

  // Upload the texture.
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {2, 2, 1};
  barrier(tex, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkCmdCopyBufferToImage(cmd, tex_stage.buf, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  barrier(tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  VkClearValue clear{};
  clear.color.float32[0] = 0;
  clear.color.float32[1] = 0;
  clear.color.float32[2] = 0;
  clear.color.float32[3] = 1;
  VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rbi.renderPass = rp;
  rbi.framebuffer = fb;
  rbi.renderArea = {{0, 0}, {kW, kH}};
  rbi.clearValueCount = 1;
  rbi.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &set, 0, nullptr);
  vkCmdDraw(cmd, 4, 1, 0, 0);
  vkCmdEndRenderPass(cmd);

  VkBufferImageCopy rb_region{};
  rb_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  rb_region.imageExtent = {kW, kH, 1};
  vkCmdCopyImageToBuffer(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buf, 1, &rb_region);
  VK_CHECK(vkEndCommandBuffer(cmd));

  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  VK_CHECK(vkCreateFence(dev, &fci, nullptr, &fence));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
  VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

  auto* px = static_cast<const u8*>(readback.map);
  auto at = [&](int x, int y) { return px + (size_t(y) * kW + x) * 4; };
  struct Check {
    int x, y;
    const char* name;
    u8 rgba[4];
  };
  const Check checks[] = {
      {16, 16, "top-left red", {255, 0, 0, 255}},
      {48, 16, "top-right green", {0, 255, 0, 255}},
      {16, 32, "bottom-left blue", {0, 0, 255, 255}},
      {48, 32, "bottom-right white", {255, 255, 255, 255}},
  };
  bool ok = true;
  int wrong = 0;
  for (const Check& c : checks) {
    const u8* p = at(c.x, c.y);
    bool match = near(p[0], c.rgba[0]) && near(p[1], c.rgba[1]) && near(p[2], c.rgba[2]) && near(p[3], c.rgba[3]);
    std::printf("%s: %s (%u,%u,%u,%u)\n", c.name, match ? "OK" : "WRONG", p[0], p[1], p[2], p[3]);
    if (!match) ++wrong;
    ok = ok && match;
  }
  if (have_validation) std::printf("validation errors: %d\n", g_validation_errors);
  if (no_flip) {
    // Sensitivity check: a normal Vulkan viewport must invert the D3D y-up image.
    std::printf("flip sensitivity: %s (%d wrong quadrant%s)\n", wrong > 0 ? "detected" : "MISSED", wrong,
                wrong == 1 ? "" : "s");
    ok = wrong > 0 && g_validation_errors == 0;
  } else {
    ok = ok && g_validation_errors == 0;
  }

  vkDestroyFence(dev, fence, nullptr);
  vkDestroyCommandPool(dev, pool_cmd, nullptr);
  vkDestroyDescriptorPool(dev, pool, nullptr);
  vkDestroyPipeline(dev, pipeline, nullptr);
  vkDestroyFramebuffer(dev, fb, nullptr);
  vkDestroyRenderPass(dev, rp, nullptr);
  vkDestroyShaderModule(dev, vs, nullptr);
  vkDestroyShaderModule(dev, ps, nullptr);
  vkDestroySampler(dev, sampler, nullptr);
  vkDestroyDescriptorSetLayout(dev, set_layout, nullptr);
  vkDestroyPipelineLayout(dev, pipeline_layout, nullptr);
  vkDestroyImageView(dev, tex_view, nullptr);
  vkDestroyImage(dev, tex, nullptr);
  vkFreeMemory(dev, tex_mem, nullptr);
  vkDestroyImageView(dev, color_view, nullptr);
  vkDestroyImage(dev, color, nullptr);
  vkFreeMemory(dev, color_mem, nullptr);
  for (Buffer* b : {&tex_stage, &vb, &ubo_ps, &ubo_vs, &readback}) {
    if (b->map) vkUnmapMemory(dev, b->mem);
    if (b->buf) vkDestroyBuffer(dev, b->buf, nullptr);
    if (b->mem) vkFreeMemory(dev, b->mem, nullptr);
  }
  vkDestroyDevice(dev, nullptr);
  if (messenger) {
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy) destroy(instance, messenger, nullptr);
  }
  vkDestroyInstance(instance, nullptr);
  return ok ? 0 : 1;
}
