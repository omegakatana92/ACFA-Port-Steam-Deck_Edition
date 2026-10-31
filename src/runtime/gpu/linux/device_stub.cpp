// Linux stand-in for the Direct3D 11 device while the native Vulkan backend is being built.
// It keeps the Windows interface link-compatible and reports that no presentation is available,
// so the rest of the runtime (guest CPU, HLE, audio, input, saves) can be built and exercised.
#include <algorithm>

#include "runtime/gpu/d3d11_device.h"
#include "runtime/gpu/display.h"
#include "runtime/gpu/shader_warm.h"
#include "runtime/gpu/vulkan/vk_presenter.h"
#include "runtime/log.h"

namespace rt::gpu {

Device& device() {
  static Device d;
  return d;
}

bool device_init(void* hwnd) {
  std::string error;
  if (!vk::init(hwnd, &error)) {
    LOG_E("gpu", "Vulkan presenter init failed: %s", error.c_str());
    return false;
  }
  return true;
}

void shader_stats(u32* compiled, u32* cached, double* seconds) {
  if (compiled) *compiled = 0;
  if (cached) *cached = 0;
  if (seconds) *seconds = 0;
}

void present_rgba(const u8* rgba, u32 w, u32 h) {
  std::string error;
  if (!vk::present_rgba(rgba, w, h, &error)) LOG_W("gpu", "present failed: %s", error.c_str());
}

void request_exclusive(bool on, u32, u32, u32) { display::set_exclusive(on); }
bool exclusive_active() { return false; }

bool read_back_last_frame(std::vector<u8>& rgba, u32& w, u32& h) { return vk::last_rgba(rgba, w, h); }
void begin_capture(u32) {}
void poll_captures(const std::function<void(u32, std::vector<u8>&&, u32, u32)>&) {}

WarmResult warm_shaders(const std::string&, const std::string&, const std::string&) {
  WarmResult r;
  r.error = "shader warm-up needs the Linux SPIR-V shader compiler, which is not implemented yet";
  return r;
}

}  // namespace rt::gpu
