// Vulkan Stage 4 gate: surface lifecycle, clears, render-scale recreation and feedback copies.
// Synthetic reference only; no game data.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#ifndef STAGE4_VS_SPV
#define STAGE4_VS_SPV ""
#endif
#ifndef STAGE4_PS_SPV
#define STAGE4_PS_SPV ""
#endif

namespace {

using u8 = uint8_t;
int g_errors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    std::fprintf(stderr, "validation: %s\n", data->pMessage);
    ++g_errors;
  }
  return VK_FALSE;
}

#define VK_CHECK(x)                                                                          \
  do {                                                                                       \
    VkResult r_ = (x);                                                                       \
    if (r_ != VK_SUCCESS) {                                                                  \
      std::fprintf(stderr, "%s failed: VkResult %d at %s:%d\n", #x, int(r_), __FILE__, __LINE__); \
      return false;                                                                          \
    }                                                                                        \
  } while (0)

struct Buffer {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  void* map = nullptr;
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t w = 0, h = 0;
};

struct Surface {
  Image color;
  VkImage depth = VK_NULL_HANDLE;
  VkDeviceMemory depth_mem = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  VkFramebuffer fb = VK_NULL_HANDLE;
  uint32_t w = 0, h = 0;
};

VkDevice dev = VK_NULL_HANDLE;
VkPhysicalDevice gpu = VK_NULL_HANDLE;
VkQueue queue = VK_NULL_HANDLE;
uint32_t family = 0;
VkCommandPool cmd_pool = VK_NULL_HANDLE;
VkCommandBuffer cmd = VK_NULL_HANDLE;
VkFence fence = VK_NULL_HANDLE;
VkRenderPass rp = VK_NULL_HANDLE;
VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
VkDescriptorPool desc_pool = VK_NULL_HANDLE;
VkSampler sampler = VK_NULL_HANDLE;
Buffer records_buf{}, readback{};
VkDescriptorSet set_white = VK_NULL_HANDLE;
Image white{};
VkPipeline pipe_base = VK_NULL_HANDLE, pipe_mask = VK_NULL_HANDLE, pipe_depth = VK_NULL_HANDLE,
           pipe_stencil = VK_NULL_HANDLE;

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}

bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* out) {
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(dev, &ci, nullptr, &out->buf) != VK_SUCCESS) return false;
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(dev, out->buf, &req);
  uint32_t type = memory_type(req.memoryTypeBits,
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

VkShaderModule shader(const std::vector<char>& code) {
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = code.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
  VkShaderModule m = VK_NULL_HANDLE;
  return vkCreateShaderModule(dev, &ci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

bool begin_cmd() {
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkResetCommandBuffer(cmd, 0);
  return vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS;
}

bool submit() {
  VkResult r = vkEndCommandBuffer(cmd);
  if (r != VK_SUCCESS) return false;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkResetFences(dev, 1, &fence);
  if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) return false;
  return vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
             VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {aspect, 0, 1, 0, 1};
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool make_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, Image* out) {
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(dev, &ci, nullptr, &out->image) != VK_SUCCESS) return false;
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(dev, out->image, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, nullptr, &out->mem) != VK_SUCCESS) return false;
  vkBindImageMemory(dev, out->image, out->mem, 0);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = out->image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(dev, &vi, nullptr, &out->view) != VK_SUCCESS) return false;
  out->w = w;
  out->h = h;
  return true;
}

Surface make_surface(uint32_t w, uint32_t h) {
  Surface s;
  if (!make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  &s.color))
    return {};
  VkImageCreateInfo dci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  dci.imageType = VK_IMAGE_TYPE_2D;
  dci.format = VK_FORMAT_D24_UNORM_S8_UINT;
  dci.extent = {w, h, 1};
  dci.mipLevels = 1;
  dci.arrayLayers = 1;
  dci.samples = VK_SAMPLE_COUNT_1_BIT;
  dci.tiling = VK_IMAGE_TILING_OPTIMAL;
  dci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  dci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(dev, &dci, nullptr, &s.depth) != VK_SUCCESS) return {};
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(dev, s.depth, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, nullptr, &s.depth_mem) != VK_SUCCESS) return {};
  vkBindImageMemory(dev, s.depth, s.depth_mem, 0);
  VkImageViewCreateInfo dvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  dvi.image = s.depth;
  dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  dvi.format = VK_FORMAT_D24_UNORM_S8_UINT;
  dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(dev, &dvi, nullptr, &s.depth_view) != VK_SUCCESS) return {};
  VkImageView views[2] = {s.color.view, s.depth_view};
  VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbci.renderPass = rp;
  fbci.attachmentCount = 2;
  fbci.pAttachments = views;
  fbci.width = w;
  fbci.height = h;
  fbci.layers = 1;
  if (vkCreateFramebuffer(dev, &fbci, nullptr, &s.fb) != VK_SUCCESS) return {};
  s.w = w;
  s.h = h;
  return s;
}

void destroy_surface(Surface& s) {
  if (s.fb) vkDestroyFramebuffer(dev, s.fb, nullptr);
  if (s.depth_view) vkDestroyImageView(dev, s.depth_view, nullptr);
  if (s.depth) vkDestroyImage(dev, s.depth, nullptr);
  if (s.depth_mem) vkFreeMemory(dev, s.depth_mem, nullptr);
  if (s.color.view) vkDestroyImageView(dev, s.color.view, nullptr);
  if (s.color.image) vkDestroyImage(dev, s.color.image, nullptr);
  if (s.color.mem) vkFreeMemory(dev, s.color.mem, nullptr);
  s = Surface{};
}

bool recreate(Surface* s, uint32_t w, uint32_t h) {
  vkDeviceWaitIdle(dev);
  Surface n = make_surface(w, h);
  if (!n.fb) return false;
  if (!begin_cmd()) return false;
  barrier(n.color.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[1] = {int32_t(s->w), int32_t(s->h), 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstOffsets[1] = {int32_t(w), int32_t(h), 1};
  vkCmdBlitImage(cmd, s->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, n.color.image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
  barrier(n.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
  if (!submit()) return false;
  destroy_surface(*s);
  *s = n;
  return true;
}

struct Record {
  float tint[4];
  float uv[4];
  float extra[4];
};
static_assert(sizeof(Record) == 48);

uint32_t add_record(std::vector<Record>& rs, float r, float g, float b, float a, float z) {
  Record rec{};
  rec.tint[0] = r;
  rec.tint[1] = g;
  rec.tint[2] = b;
  rec.tint[3] = a;
  rec.extra[0] = z;
  rs.push_back(rec);
  return uint32_t(rs.size() - 1);
}

bool render(Surface& s, float cr, float cg, float cb, float ca, float depth, uint32_t stencil,
            const std::function<void()>& body) {
  if (!begin_cmd()) return false;
  VkClearValue clears[2]{};
  clears[0].color.float32[0] = cr;
  clears[0].color.float32[1] = cg;
  clears[0].color.float32[2] = cb;
  clears[0].color.float32[3] = ca;
  clears[1].depthStencil.depth = depth;
  clears[1].depthStencil.stencil = stencil;
  VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = rp;
  bi.framebuffer = s.fb;
  bi.renderArea = {{0, 0}, {s.w, s.h}};
  bi.clearValueCount = 2;
  bi.pClearValues = clears;
  vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vp{0, float(s.h), float(s.w), -float(s.h), 0, 1};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout, 0, 1, &set_white, 0, nullptr);
  body();
  vkCmdEndRenderPass(cmd);
  return submit();
}

bool read_surface(Surface& s, std::vector<u8>& out) {
  if (!begin_cmd()) return false;
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {s.w, s.h, 1};
  vkCmdCopyImageToBuffer(cmd, s.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buf, 1, &region);
  if (!submit()) return false;
  const auto* p = static_cast<const u8*>(readback.map);
  out.assign(p, p + size_t(s.w) * s.h * 4);
  return true;
}

void draw_record(VkPipeline pipe, uint32_t rec) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdDraw(cmd, 6, 1, 0, rec);
}

void scissor(int x, int y, int w, int h) {
  VkRect2D r{{x, y}, {uint32_t(w), uint32_t(h)}};
  vkCmdSetScissor(cmd, 0, 1, &r);
}

bool make_pipe(VkShaderModule vs, VkShaderModule ps, bool blur, VkBlendFactor src, VkBlendFactor dst,
               bool depth_test, VkCompareOp depth_op, bool stencil_test, uint32_t write_mask, VkPipeline* out) {
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
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState ba{};
  ba.blendEnable = blur ? VK_TRUE : VK_FALSE;
  ba.srcColorBlendFactor = src;
  ba.dstColorBlendFactor = dst;
  ba.colorBlendOp = VK_BLEND_OP_ADD;
  ba.srcAlphaBlendFactor = src;
  ba.dstAlphaBlendFactor = dst;
  ba.alphaBlendOp = VK_BLEND_OP_ADD;
  ba.colorWriteMask = write_mask;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &ba;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
  ds.depthWriteEnable = depth_test ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = depth_op;
  ds.stencilTestEnable = stencil_test ? VK_TRUE : VK_FALSE;
  VkStencilOpState so{};
  so.failOp = VK_STENCIL_OP_KEEP;
  so.passOp = VK_STENCIL_OP_KEEP;
  so.depthFailOp = VK_STENCIL_OP_KEEP;
  so.compareOp = VK_COMPARE_OP_EQUAL;
  so.compareMask = 0xFF;
  so.writeMask = 0;
  so.reference = 7;
  ds.front = so;
  ds.back = so;
  VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyns;
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
  gp.layout = pipe_layout;
  gp.renderPass = rp;
  gp.subpass = 0;
  return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, out) == VK_SUCCESS;
}

bool make_set(VkImageView tex, VkDescriptorSet* out) {
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = desc_pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &set_layout;
  if (vkAllocateDescriptorSets(dev, &ai, out) != VK_SUCCESS) return false;
  VkDescriptorBufferInfo bi{records_buf.buf, 0, 48 * 64};
  VkDescriptorImageInfo ii{};
  ii.imageView = tex;
  ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo si{};
  si.sampler = sampler;
  VkWriteDescriptorSet writes[3]{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = *out;
  writes[0].dstBinding = 1000;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &bi;
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = *out;
  writes[1].dstBinding = 1001;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[1].pImageInfo = &ii;
  writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet = *out;
  writes[2].dstBinding = 2000;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  writes[2].pImageInfo = &si;
  vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);
  return true;
}

bool check_pixel(const std::vector<u8>& px, int w, int x, int y, int r, int g, int b, int a, const char* what) {
  const u8* p = &px[(size_t(y) * w + x) * 4];
  bool ok = std::abs(int(p[0]) - r) <= 3 && std::abs(int(p[1]) - g) <= 3 && std::abs(int(p[2]) - b) <= 3 &&
            std::abs(int(p[3]) - a) <= 3;
  std::printf("%s: %s (%u,%u,%u,%u)\n", what, ok ? "OK" : "WRONG", p[0], p[1], p[2], p[3]);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const char* vs_path = STAGE4_VS_SPV;
  const char* ps_path = STAGE4_PS_SPV;
  bool want_validation = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--validate")) want_validation = true;
    else if (!vs_path[0]) vs_path = argv[i];
    else ps_path = argv[i];
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "acfa-vk-surface";
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
  if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return 1;
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

  uint32_t n = 0;
  vkEnumeratePhysicalDevices(instance, &n, nullptr);
  if (!n) return 1;
  std::vector<VkPhysicalDevice> gpus(n);
  vkEnumeratePhysicalDevices(instance, &n, gpus.data());
  gpu = gpus[0];
  {
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> q(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, q.data());
    for (uint32_t i = 0; i < qn; ++i)
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
  if (vkCreateDevice(gpu, &dci, nullptr, &dev) != VK_SUCCESS) return 1;
  vkGetDeviceQueue(dev, family, 0, &queue);
  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = family;
  if (vkCreateCommandPool(dev, &cpci, nullptr, &cmd_pool) != VK_SUCCESS) return 1;
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = cmd_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(dev, &cbai, &cmd) != VK_SUCCESS) return 1;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) return 1;

  // Render pass: colour clear + depth/stencil clear, colour final TRANSFER_SRC.
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
  VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &cref;
  sub.pDepthStencilAttachment = &dref;
  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 2;
  rpci.pAttachments = atts;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  if (vkCreateRenderPass(dev, &rpci, nullptr, &rp) != VK_SUCCESS) return 1;

  VkDescriptorSetLayoutBinding binds[3]{};
  binds[0].binding = 1000;
  binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binds[0].descriptorCount = 1;
  binds[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  binds[1].binding = 1001;
  binds[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  binds[1].descriptorCount = 1;
  binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  binds[2].binding = 2000;
  binds[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  binds[2].descriptorCount = 1;
  binds[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  slci.bindingCount = 3;
  slci.pBindings = binds;
  if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &set_layout) != VK_SUCCESS) return 1;
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &set_layout;
  if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipe_layout) != VK_SUCCESS) return 1;
  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = VK_FILTER_NEAREST;
  sci.minFilter = VK_FILTER_NEAREST;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(dev, &sci, nullptr, &sampler) != VK_SUCCESS) return 1;

  VkDescriptorPoolSize psz[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                 {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2},
                                 {VK_DESCRIPTOR_TYPE_SAMPLER, 2}};
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 2;
  dpci.poolSizeCount = 3;
  dpci.pPoolSizes = psz;
  if (vkCreateDescriptorPool(dev, &dpci, nullptr, &desc_pool) != VK_SUCCESS) return 1;

  if (!make_buffer(48 * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &records_buf) ||
      !make_buffer(128 * 128 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback))
    return 1;

  // 1x1 white texture for solid-colour passes.
  if (!make_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &white))
    return 1;
  Buffer stage{};
  if (!make_buffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stage)) return 1;
  const u8 white_px[4] = {255, 255, 255, 255};
  std::memcpy(stage.map, white_px, 4);
  if (!begin_cmd()) return 1;
  barrier(white.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy reg{};
  reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  reg.imageExtent = {1, 1, 1};
  vkCmdCopyBufferToImage(cmd, stage.buf, white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
  barrier(white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  if (!submit()) return 1;
  if (!make_set(white.view, &set_white)) return 1;

  auto vs_code = read_file(vs_path);
  auto ps_code = read_file(ps_path);
  if (vs_code.empty() || ps_code.empty()) {
    std::fprintf(stderr, "cannot read SPIR-V: %s / %s\n", vs_path, ps_path);
    return 1;
  }
  VkShaderModule vs = shader(vs_code), ps = shader(ps_code);
  if (!vs || !ps) return 1;
  if (!make_pipe(vs, ps, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, false, VK_COMPARE_OP_ALWAYS,
                 false, 0xF, &pipe_base) ||
      !make_pipe(vs, ps, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, false, VK_COMPARE_OP_ALWAYS,
                 false, 0x9, &pipe_mask) ||
      !make_pipe(vs, ps, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, true, VK_COMPARE_OP_LESS,
                 false, 0xF, &pipe_depth) ||
      !make_pipe(vs, ps, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, false, VK_COMPARE_OP_ALWAYS,
                 true, 0xF, &pipe_stencil))
    return 1;

  std::vector<Record> records;
  std::vector<u8> px;
  bool ok = true;

  Surface s = make_surface(64, 64);
  if (!s.fb) return 1;

  // 1. Full clear.
  if (!render(s, 10 / 255.f, 20 / 255.f, 30 / 255.f, 1, 1.0f, 0, [] {}) || !read_surface(s, px)) return 1;
  ok &= check_pixel(px, 64, 32, 32, 10, 20, 30, 255, "full clear");

  // 2. Masked clear: clear blue, then write R/A only with red.
  records.clear();
  uint32_t rec = add_record(records, 1, 0, 0, 1, 0.5f);
  std::memcpy(records_buf.map, records.data(), records.size() * sizeof(Record));
  auto mask_body = [&] {
    scissor(0, 0, 64, 64);
    draw_record(pipe_mask, rec);
  };
  if (!render(s, 0, 0, 1, 1, 1.0f, 0, mask_body) || !read_surface(s, px)) return 1;
  ok &= check_pixel(px, 64, 32, 32, 255, 0, 255, 255, "masked clear (R/A)");

  // 3. Scissored clear: black, red only inside the rect.
  records.clear();
  rec = add_record(records, 1, 0, 0, 1, 0.5f);
  std::memcpy(records_buf.map, records.data(), records.size() * sizeof(Record));
  auto scissor_body = [&] {
    scissor(16, 16, 16, 16);
    draw_record(pipe_base, rec);
  };
  if (!render(s, 0, 0, 0, 1, 1.0f, 0, scissor_body) || !read_surface(s, px)) return 1;
  ok &= check_pixel(px, 64, 24, 24, 255, 0, 0, 255, "scissor inside");
  ok &= check_pixel(px, 64, 8, 8, 0, 0, 0, 255, "scissor outside");

  // 4. Depth clear 0.5: 0.75 fails LESS, 0.25 passes.
  records.clear();
  uint32_t far = add_record(records, 1, 0, 0, 1, 0.75f);
  uint32_t near = add_record(records, 0, 1, 0, 1, 0.25f);
  std::memcpy(records_buf.map, records.data(), records.size() * sizeof(Record));
  auto depth_body = [&] {
    scissor(0, 0, 64, 64);
    draw_record(pipe_depth, far);
    draw_record(pipe_depth, near);
  };
  if (!render(s, 0, 0, 0, 1, 0.5f, 0, depth_body) || !read_surface(s, px)) return 1;
  ok &= check_pixel(px, 64, 32, 32, 0, 255, 0, 255, "depth clear 0.5 + LESS");

  // 5. Stencil clear 7: EQUAL 7 passes.
  records.clear();
  rec = add_record(records, 0, 1, 0, 1, 0.5f);
  std::memcpy(records_buf.map, records.data(), records.size() * sizeof(Record));
  auto stencil_body = [&] {
    scissor(0, 0, 64, 64);
    draw_record(pipe_stencil, rec);
  };
  if (!render(s, 0, 0, 0, 1, 1.0f, 7, stencil_body) || !read_surface(s, px)) return 1;
  ok &= check_pixel(px, 64, 32, 32, 0, 255, 0, 255, "stencil clear 7 + EQUAL");

  // 6. Render-scale recreation: four quadrants at 64, 128 and 32.
  records.clear();
  uint32_t r = add_record(records, 1, 0, 0, 1, 0.5f);
  uint32_t g = add_record(records, 0, 1, 0, 1, 0.5f);
  uint32_t b = add_record(records, 0, 0, 1, 1, 0.5f);
  uint32_t w = add_record(records, 1, 1, 1, 1, 0.5f);
  std::memcpy(records_buf.map, records.data(), records.size() * sizeof(Record));
  auto quad_body = [&] {
    scissor(0, 0, 32, 32);
    draw_record(pipe_base, r);
    scissor(32, 0, 32, 32);
    draw_record(pipe_base, g);
    scissor(0, 32, 32, 32);
    draw_record(pipe_base, b);
    scissor(32, 32, 32, 32);
    draw_record(pipe_base, w);
  };
  if (!render(s, 0, 0, 0, 1, 1.0f, 0, quad_body) || !read_surface(s, px)) return 1;
  ok &= check_pixel(px, 64, 16, 16, 255, 0, 0, 255, "scale 64 TL");
  if (!recreate(&s, 128, 128) || !read_surface(s, px)) return 1;
  ok &= check_pixel(px, 128, 32, 32, 255, 0, 0, 255, "scale 128 TL");
  ok &= check_pixel(px, 128, 96, 32, 0, 255, 0, 255, "scale 128 TR");
  ok &= check_pixel(px, 128, 32, 96, 0, 0, 255, 255, "scale 128 BL");
  ok &= check_pixel(px, 128, 96, 96, 255, 255, 255, 255, "scale 128 BR");
  if (!recreate(&s, 32, 32) || !read_surface(s, px)) return 1;
  ok &= check_pixel(px, 32, 8, 8, 255, 0, 0, 255, "scale 32 TL");
  ok &= check_pixel(px, 32, 24, 24, 255, 255, 255, 255, "scale 32 BR");

  // 7. Feedback: render quadrants, copy to B, sample B while rendering A.
  Surface fb = make_surface(64, 64);
  if (!fb.fb) return 1;
  if (!render(fb, 0, 0, 0, 1, 1.0f, 0, quad_body) || !read_surface(fb, px)) return 1;
  Image copy{};
  if (!make_image(64, 64, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &copy))
    return 1;
  if (!begin_cmd()) return 1;
  barrier(copy.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkImageCopy cpy{};
  cpy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  cpy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  cpy.extent = {64, 64, 1};
  vkCmdCopyImage(cmd, fb.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy.image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
  barrier(copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  if (!submit()) return 1;
  VkDescriptorSet feedback_set = VK_NULL_HANDLE;
  if (!make_set(copy.view, &feedback_set)) return 1;
  records.clear();
  rec = add_record(records, 0.5f, 0.5f, 0.5f, 1, 0.5f);
  std::memcpy(records_buf.map, records.data(), records.size() * sizeof(Record));
  auto feedback_body = [&] {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout, 0, 1, &feedback_set, 0, nullptr);
    scissor(0, 0, 64, 64);
    draw_record(pipe_base, rec);
  };
  if (!render(fb, 0, 0, 0, 1, 1.0f, 0, feedback_body) || !read_surface(fb, px)) return 1;
  ok &= check_pixel(px, 64, 16, 16, 128, 0, 0, 255, "feedback TL 50%");
  ok &= check_pixel(px, 64, 48, 48, 128, 128, 128, 255, "feedback BR 50%");

  if (have_validation) std::printf("validation errors: %d\n", g_errors);
  ok &= g_errors == 0;

  destroy_surface(s);
  destroy_surface(fb);
  vkDestroyImageView(dev, copy.view, nullptr);
  vkDestroyImage(dev, copy.image, nullptr);
  vkFreeMemory(dev, copy.mem, nullptr);
  vkDestroyPipeline(dev, pipe_base, nullptr);
  vkDestroyPipeline(dev, pipe_mask, nullptr);
  vkDestroyPipeline(dev, pipe_depth, nullptr);
  vkDestroyPipeline(dev, pipe_stencil, nullptr);
  vkDestroyShaderModule(dev, vs, nullptr);
  vkDestroyShaderModule(dev, ps, nullptr);
  vkDestroySampler(dev, sampler, nullptr);
  vkDestroyDescriptorPool(dev, desc_pool, nullptr);
  vkDestroyDescriptorSetLayout(dev, set_layout, nullptr);
  vkDestroyPipelineLayout(dev, pipe_layout, nullptr);
  vkDestroyRenderPass(dev, rp, nullptr);
  vkDestroyImageView(dev, white.view, nullptr);
  vkDestroyImage(dev, white.image, nullptr);
  vkFreeMemory(dev, white.mem, nullptr);
  for (Buffer* b : {&stage, &records_buf, &readback}) {
    if (b->map) vkUnmapMemory(dev, b->mem);
    if (b->buf) vkDestroyBuffer(dev, b->buf, nullptr);
    if (b->mem) vkFreeMemory(dev, b->mem, nullptr);
  }
  vkDestroyFence(dev, fence, nullptr);
  vkDestroyCommandPool(dev, cmd_pool, nullptr);
  vkDestroyDevice(dev, nullptr);
  if (messenger) {
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy) destroy(instance, messenger, nullptr);
  }
  vkDestroyInstance(instance, nullptr);
  return ok ? 0 : 1;
}
