// Linux RSX renderer placeholder. The RSX front end, command FIFO, HLE and guest CPU run for
// real; drawing and presentation wait for the native Vulkan backend. Stage 1 presents an animated
// test pattern so the window/swapchain path can be verified without game rendering.
#include <atomic>
#include <string>

#include "runtime/config.h"
#include "runtime/gpu/display.h"
#include "runtime/gpu/d3d11_device.h"
#include "runtime/gpu/vulkan/vk_presenter.h"
#include "runtime/gpu/window.h"
#include "runtime/log.h"
#include "runtime/platform.h"
#include "runtime/rsx/rsx.h"
#include "runtime/rsx/rsx_internal.h"

namespace rt::rsx {
namespace {

std::atomic<bool> g_told{false};
std::atomic<bool> g_device{false};
std::atomic<u64> g_frames{0};
std::atomic<u64> g_methods{0};

void tell_once() {
  if (g_told.exchange(true)) return;
  LOG_W("gpu", "the Linux RSX renderer is not implemented: presenting the Stage 1 test pattern");
}

}  // namespace

u32 g_covered_put = ~0u;

void renderer_thread_init() {
  tell_once();
  if (cfg().headless) return;
  void* hwnd = nullptr;
  for (int i = 0; i < 500 && !(hwnd = gpu::window_handle()); ++i) platform::sleep_ms(10);
  if (!hwnd) {
    LOG_E("gpu", "no window: presentation disabled");
    return;
  }
  bool ok = false;
  // Surface creation is safest on the window thread (Wayland/X11).
  if (!gpu::run_on_window_thread([&] { ok = gpu::device_init(hwnd); })) ok = gpu::device_init(hwnd);
  g_device = ok;
  if (!ok) LOG_E("gpu", "Vulkan presentation disabled");
}

void renderer_idle() {}
void present_buffer(u32, const DisplayBuffer&) {
  if (!g_device || !gpu::vk::ready()) return;
  std::string error;
  double t = double(g_frames.fetch_add(1, std::memory_order_relaxed)) / 60.0;
  if (!gpu::vk::present_test_pattern(t, &error)) LOG_W("gpu", "test pattern present failed: %s", error.c_str());
}

void renderer_method(u32, u32, u32, u32) {
  g_methods.fetch_add(1, std::memory_order_relaxed);
  tell_once();
}

void renderer_transform_constants(const u8*, u32, u32) { g_methods.fetch_add(1, std::memory_order_relaxed); }
void renderer_memory_written(u32, u32) {}
void renderer_guest_sync() {}
int phase_switch(int p) { return p; }
void renderer_flush() {}
u32 renderer_report_value(u32, u32) { return 0; }
void renderer_mark_special(void (*)(u32, u32)) {}

}  // namespace rt::rsx
