// Vulkan Stage 3 gate: D3D-style pipeline state mapped to Vulkan and checked against a CPU
// reference. Cases: alpha blend, additive blend, depth LESS/GREATER, stencil write/test,
// back/front culling with a negative-height viewport, and scissor clipping.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef STAGE3_VS_SPV
#define STAGE3_VS_SPV ""
#endif
#ifndef STAGE3_PS_SPV
#define STAGE3_PS_SPV ""
#endif

namespace {

using u8 = uint8_t;
constexpr uint32_t kW = 128, kH = 128;
int g_validation_errors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    std::fprintf(stderr, "validation: %s\n", data->pMessage);
    ++g_validation_errors;
  }
  return VK_FALSE;
}

#define VK_CHECK(x)                                                                          \
  do {                                                                                       \
    VkResult r_ = (x);                                                                       \
    if (r_ != VK_SUCCESS) {                                                                  \
      std::fprintf(stderr, "%s failed: VkResult %d at %s:%d\n", #x, int(r_), __FILE__, __LINE__); \
      return 1;                                                                              \
    }                                                                                        \
  } while (0)

uint32_t memory_type(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}

struct Buffer {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  void* map = nullptr;
};

bool make_buffer(VkDevice dev, VkPhysicalDevice gpu, VkDeviceSize size, VkBufferUsageFlags usage, Buffer* out) {
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(dev, &ci, nullptr, &out->buf) != VK_SUCCESS) return false;
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(dev, out->buf, &req);
  uint32_t type = memory_type(gpu, req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (type == UINT32_MAX) return false;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  if (vkAllocateMemory(dev, &ai, nullptr, &out->mem) != VK_SUCCESS) return false;
  vkBindBufferMemory(dev, out->buf, out->mem, 0);
  return vkMapMemory(dev, out->mem, 0, size, 0, &out->map) == VK_SUCCESS;
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

VkShaderModule shader(VkDevice dev, const std::vector<char>& code) {
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = code.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule m = VK_NULL_HANDLE;
  return vkCreateShaderModule(dev, &ci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

struct Record {
  float rect[4];
  float color[4];
  float extra[4];
};
static_assert(sizeof(Record) == 48);

struct Pipe {
  bool blend = false;
  VkBlendFactor src = VK_BLEND_FACTOR_ONE;
  VkBlendFactor dst = VK_BLEND_FACTOR_ZERO;
  bool depth = false;
  VkCompareOp depth_op = VK_COMPARE_OP_LESS;
  bool stencil_write = false;
  bool stencil_test = false;
  VkCullModeFlags cull = VK_CULL_MODE_NONE;
  bool flip_front = false;  // frontFace CW
  uint32_t color_mask = 0xF;
};

VkPipeline make_pipeline(VkDevice dev, VkRenderPass rp, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule ps,
                         const Pipe& p) {
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
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vps.viewportCount = 1;
  vps.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = p.cull;
  rs.frontFace = p.flip_front ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1;

  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState ba{};
  ba.blendEnable = p.blend ? VK_TRUE : VK_FALSE;
  ba.srcColorBlendFactor = p.src;
  ba.dstColorBlendFactor = p.dst;
  ba.colorBlendOp = VK_BLEND_OP_ADD;
  ba.srcAlphaBlendFactor = p.blend ? p.src : VK_BLEND_FACTOR_ONE;
  ba.dstAlphaBlendFactor = p.blend ? p.dst : VK_BLEND_FACTOR_ZERO;
  ba.alphaBlendOp = VK_BLEND_OP_ADD;
  ba.colorWriteMask = p.color_mask;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &ba;

  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = p.depth ? VK_TRUE : VK_FALSE;
  ds.depthWriteEnable = p.depth ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = p.depth_op;
  ds.stencilTestEnable = (p.stencil_write || p.stencil_test) ? VK_TRUE : VK_FALSE;
  VkStencilOpState so{};
  so.failOp = VK_STENCIL_OP_KEEP;
  so.depthFailOp = VK_STENCIL_OP_KEEP;
  so.passOp = p.stencil_write ? VK_STENCIL_OP_REPLACE : VK_STENCIL_OP_KEEP;
  so.compareOp = p.stencil_test ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_ALWAYS;
  so.compareMask = 0xFF;
  so.writeMask = p.stencil_write ? 0xFF : 0;
  so.reference = 1;
  ds.front = so;
  ds.back = so;

  VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;

  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2;
  gp.pStages = stages;
  gp.pVertexInputState = &vin;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vps;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pDepthStencilState = &ds;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &dyn;
  gp.layout = layout;
  gp.renderPass = rp;
  gp.subpass = 0;
  VkPipeline out = VK_NULL_HANDLE;
  return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &out) == VK_SUCCESS ? out : VK_NULL_HANDLE;
}

}  // namespace

int main(int argc, char** argv) {
  const char* vs_path = STAGE3_VS_SPV;
  const char* ps_path = STAGE3_PS_SPV;
  bool want_validation = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--validate")) want_validation = true;
    else if (!vs_path[0]) vs_path = argv[i];
    else ps_path = argv[i];
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "acfa-vk-state";
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
      ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
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

  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ci.extent = {kW, kH, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage color = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImage(dev, &ci, nullptr, &color));
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(dev, color, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = memory_type(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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

  VkImageCreateInfo dci_img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  dci_img.imageType = VK_IMAGE_TYPE_2D;
  dci_img.format = VK_FORMAT_D24_UNORM_S8_UINT;
  dci_img.extent = {kW, kH, 1};
  dci_img.mipLevels = 1;
  dci_img.arrayLayers = 1;
  dci_img.samples = VK_SAMPLE_COUNT_1_BIT;
  dci_img.tiling = VK_IMAGE_TILING_OPTIMAL;
  dci_img.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  dci_img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage depth = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImage(dev, &dci_img, nullptr, &depth));
  vkGetImageMemoryRequirements(dev, depth, &req);
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = memory_type(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory depth_mem = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &depth_mem));
  VK_CHECK(vkBindImageMemory(dev, depth, depth_mem, 0));
  VkImageViewCreateInfo dvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  dvi.image = depth;
  dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  dvi.format = VK_FORMAT_D24_UNORM_S8_UINT;
  dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
  VkImageView depth_view = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImageView(dev, &dvi, nullptr, &depth_view));

  Buffer draw_buf{}, readback{};
  if (!make_buffer(dev, gpu, 48 * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &draw_buf) ||
      !make_buffer(dev, gpu, kW * kH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
    std::fprintf(stderr, "buffer allocation failed\n");
    return 1;
  }
  std::vector<Record> records;
  auto add = [&](float x0, float y0, float x1, float y1, float r, float g, float b, float a, float z) {
    Record rec{};
    rec.rect[0] = 2 * x0 / kW - 1;
    rec.rect[1] = 1 - 2 * y0 / kH;
    rec.rect[2] = 2 * x1 / kW - 1;
    rec.rect[3] = 1 - 2 * y1 / kH;
    rec.color[0] = r;
    rec.color[1] = g;
    rec.color[2] = b;
    rec.color[3] = a;
    rec.extra[0] = z;
    records.push_back(rec);
    return uint32_t(records.size() - 1);
  };

  const uint32_t r_alpha_red = add(0, 0, 32, 32, 1, 0, 0, 0.5f, 0.5f);
  const uint32_t r_alpha_green = add(0, 0, 32, 32, 0, 1, 0, 0.5f, 0.5f);
  const uint32_t r_add_red = add(32, 0, 64, 32, 1, 0, 0, 1, 0.5f);
  const uint32_t r_add_green = add(32, 0, 64, 32, 0, 1, 0, 1, 0.5f);
  const uint32_t r_less_far = add(64, 0, 96, 32, 1, 0, 0, 1, 0.75f);
  const uint32_t r_less_near = add(64, 0, 96, 32, 0, 1, 0, 1, 0.25f);
  const uint32_t r_less_prime = add(64, 0, 96, 32, 0, 0, 0, 1, 1.0f);
  const uint32_t r_greater_near = add(96, 0, 128, 32, 1, 0, 0, 1, 0.25f);
  const uint32_t r_greater_far = add(96, 0, 128, 32, 0, 1, 0, 1, 0.75f);
  const uint32_t r_stencil_mask = add(0, 32, 16, 64, 0, 0, 0, 1, 0.5f);
  const uint32_t r_stencil_fill = add(0, 32, 32, 64, 0, 1, 0, 1, 0.5f);
  const uint32_t r_cull_back = add(32, 32, 64, 64, 1, 0, 0, 1, 0.5f);
  const uint32_t r_cull_front = add(64, 32, 96, 64, 0, 1, 0, 1, 0.5f);
  const uint32_t r_scissor = add(96, 32, 128, 64, 1, 0, 0, 1, 0.5f);
  std::memcpy(draw_buf.map, records.data(), records.size() * sizeof(Record));

  auto vs_code = read_file(vs_path);
  auto ps_code = read_file(ps_path);
  if (vs_code.empty() || ps_code.empty()) {
    std::fprintf(stderr, "cannot read SPIR-V: %s / %s\n", vs_path, ps_path);
    return 1;
  }
  VkShaderModule vs = shader(dev, vs_code), ps = shader(dev, ps_code);
  if (!vs || !ps) {
    std::fprintf(stderr, "shader module creation failed\n");
    return 1;
  }

  VkAttachmentDescription atts[2]{};
  atts[0].format = VK_FORMAT_R8G8B8A8_UNORM;
  atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
  atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  atts[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  atts[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
  atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
  atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &color_ref;
  sub.pDepthStencilAttachment = &depth_ref;
  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 2;
  rpci.pAttachments = atts;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  VkRenderPass rp = VK_NULL_HANDLE;
  VK_CHECK(vkCreateRenderPass(dev, &rpci, nullptr, &rp));
  VkImageView fb_views[2] = {color_view, depth_view};
  VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbci.renderPass = rp;
  fbci.attachmentCount = 2;
  fbci.pAttachments = fb_views;
  fbci.width = kW;
  fbci.height = kH;
  fbci.layers = 1;
  VkFramebuffer fb = VK_NULL_HANDLE;
  VK_CHECK(vkCreateFramebuffer(dev, &fbci, nullptr, &fb));

  VkDescriptorSetLayoutBinding bind{};
  bind.binding = 1000;
  bind.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bind.descriptorCount = 1;
  bind.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  slci.bindingCount = 1;
  slci.pBindings = &bind;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VK_CHECK(vkCreateDescriptorSetLayout(dev, &slci, nullptr, &set_layout));
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &set_layout;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &layout));

  Pipe base{};
  Pipe alpha{};
  alpha.blend = true;
  alpha.src = VK_BLEND_FACTOR_SRC_ALPHA;
  alpha.dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  Pipe addp{};
  addp.blend = true;
  addp.src = VK_BLEND_FACTOR_ONE;
  addp.dst = VK_BLEND_FACTOR_ONE;
  Pipe less{};
  less.depth = true;
  less.depth_op = VK_COMPARE_OP_LESS;
  Pipe depth_prime{};
  depth_prime.depth = true;
  depth_prime.depth_op = VK_COMPARE_OP_ALWAYS;
  depth_prime.color_mask = 0;  // depth-only priming
  Pipe greater{};
  greater.depth = true;
  greater.depth_op = VK_COMPARE_OP_GREATER;
  Pipe stencil_write{};
  stencil_write.stencil_write = true;
  stencil_write.color_mask = 0;  // the mask pass writes stencil only
  Pipe stencil_test{};
  stencil_test.stencil_test = true;
  Pipe cull_back{};
  cull_back.cull = VK_CULL_MODE_BACK_BIT;
  Pipe cull_front{};
  cull_front.cull = VK_CULL_MODE_FRONT_BIT;

  struct NamedPipe {
    const Pipe* desc;
    VkPipeline pipe;
  };
  NamedPipe pipes[] = {
      {&base, VK_NULL_HANDLE},       {&alpha, VK_NULL_HANDLE},      {&addp, VK_NULL_HANDLE},
      {&less, VK_NULL_HANDLE},       {&depth_prime, VK_NULL_HANDLE}, {&greater, VK_NULL_HANDLE},
      {&stencil_write, VK_NULL_HANDLE}, {&stencil_test, VK_NULL_HANDLE}, {&cull_back, VK_NULL_HANDLE},
      {&cull_front, VK_NULL_HANDLE},
  };
  for (NamedPipe& np : pipes) {
    np.pipe = make_pipeline(dev, rp, layout, vs, ps, *np.desc);
    if (!np.pipe) {
      std::fprintf(stderr, "pipeline creation failed\n");
      return 1;
    }
  }
  VkPipeline pipe_base = pipes[0].pipe, pipe_alpha = pipes[1].pipe, pipe_add = pipes[2].pipe;
  VkPipeline pipe_less = pipes[3].pipe, pipe_depth_prime = pipes[4].pipe, pipe_greater = pipes[5].pipe;
  VkPipeline pipe_stencil_write = pipes[6].pipe, pipe_stencil_test = pipes[7].pipe;
  VkPipeline pipe_cull_back = pipes[8].pipe, pipe_cull_front = pipes[9].pipe;

  VkDescriptorPoolSize psize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &psize;
  VkDescriptorPool dpool = VK_NULL_HANDLE;
  VK_CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool));
  VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = dpool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &set_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
  VkDescriptorBufferInfo dbi{draw_buf.buf, 0, records.size() * sizeof(Record)};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set;
  write.dstBinding = 1000;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &dbi;
  vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpci.queueFamilyIndex = family;
  VkCommandPool cpool = VK_NULL_HANDLE;
  VK_CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &cpool));
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = cpool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
  VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &cbbi));

  VkClearValue clears[2]{};
  clears[0].color.float32[0] = 0;
  clears[0].color.float32[1] = 0;
  clears[0].color.float32[2] = 0;
  clears[0].color.float32[3] = 1;
  clears[1].depthStencil.depth = 0.0f;
  clears[1].depthStencil.stencil = 0;
  VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rbi.renderPass = rp;
  rbi.framebuffer = fb;
  rbi.renderArea = {{0, 0}, {kW, kH}};
  rbi.clearValueCount = 2;
  rbi.pClearValues = clears;
  vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport vp{0, float(kH), float(kW), -float(kH), 0, 1};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
  auto cell = [&](int x, int y, int w, int h) {
    VkRect2D r{{x, y}, {uint32_t(w), uint32_t(h)}};
    vkCmdSetScissor(cmd, 0, 1, &r);
  };
  auto draw = [&](VkPipeline p, uint32_t rec) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdDraw(cmd, 6, 1, 0, rec);
  };

  cell(0, 0, 32, 32);
  draw(pipe_alpha, r_alpha_red);
  draw(pipe_alpha, r_alpha_green);
  cell(32, 0, 32, 32);
  draw(pipe_add, r_add_red);
  draw(pipe_add, r_add_green);
  cell(64, 0, 32, 32);
  draw(pipe_depth_prime, r_less_prime);
  draw(pipe_less, r_less_far);
  draw(pipe_less, r_less_near);
  cell(96, 0, 32, 32);
  draw(pipe_greater, r_greater_near);
  draw(pipe_greater, r_greater_far);
  cell(0, 32, 32, 32);
  draw(pipe_stencil_write, r_stencil_mask);
  draw(pipe_stencil_test, r_stencil_fill);
  cell(32, 32, 32, 32);
  draw(pipe_cull_back, r_cull_back);
  cell(64, 32, 32, 32);
  draw(pipe_cull_front, r_cull_front);
  cell(96, 32, 32, 32);
  cell(96, 32, 8, 32);
  draw(pipe_base, r_scissor);
  vkCmdEndRenderPass(cmd);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {kW, kH, 1};
  vkCmdCopyImageToBuffer(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buf, 1, &region);
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
  auto close = [](u8 a, int b) { return std::abs(int(a) - b) <= 3; };
  struct Check {
    int x, y;
    const char* name;
    int r, g, b, a;
  };
  const Check checks[] = {
      {16, 16, "alpha blend", 64, 128, 0, 159},
      {48, 16, "additive blend", 255, 255, 0, 255},
      {80, 16, "depth LESS", 0, 255, 0, 255},
      {112, 16, "depth GREATER", 0, 255, 0, 255},
      {8, 48, "stencil inside", 0, 255, 0, 255},
      {24, 48, "stencil outside", 0, 0, 0, 255},
      {48, 48, "cull back keeps front", 255, 0, 0, 255},
      {80, 48, "cull front removes it", 0, 0, 0, 255},
      {100, 48, "scissor inside", 255, 0, 0, 255},
      {116, 48, "scissor outside", 0, 0, 0, 255},
  };
  bool ok = true;
  for (const Check& c : checks) {
    const u8* p = at(c.x, c.y);
    bool match = close(p[0], c.r) && close(p[1], c.g) && close(p[2], c.b) && close(p[3], c.a);
    std::printf("%s: %s (%u,%u,%u,%u)\n", c.name, match ? "OK" : "WRONG", p[0], p[1], p[2], p[3]);
    ok = ok && match;
  }
  if (have_validation) std::printf("validation errors: %d\n", g_validation_errors);
  ok = ok && g_validation_errors == 0;

  vkDestroyFence(dev, fence, nullptr);
  vkDestroyCommandPool(dev, cpool, nullptr);
  vkDestroyDescriptorPool(dev, dpool, nullptr);
  for (NamedPipe& np : pipes) vkDestroyPipeline(dev, np.pipe, nullptr);
  vkDestroyPipelineLayout(dev, layout, nullptr);
  vkDestroyDescriptorSetLayout(dev, set_layout, nullptr);
  vkDestroyFramebuffer(dev, fb, nullptr);
  vkDestroyRenderPass(dev, rp, nullptr);
  vkDestroyShaderModule(dev, vs, nullptr);
  vkDestroyShaderModule(dev, ps, nullptr);
  vkDestroyImageView(dev, depth_view, nullptr);
  vkDestroyImage(dev, depth, nullptr);
  vkFreeMemory(dev, depth_mem, nullptr);
  vkDestroyImageView(dev, color_view, nullptr);
  vkDestroyImage(dev, color, nullptr);
  vkFreeMemory(dev, color_mem, nullptr);
  for (Buffer* b : {&draw_buf, &readback}) {
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
