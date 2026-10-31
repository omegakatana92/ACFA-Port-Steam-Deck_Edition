// Linux RSX renderer placeholder. The RSX front end, command FIFO, HLE and guest CPU run for
// real; drawing and presentation wait for the native Vulkan backend. Keeping this file separate
// from renderer.cpp makes the missing graphics path explicit instead of pretending to draw.
#include <atomic>

#include "runtime/gpu/display.h"
#include "runtime/gpu/window.h"
#include "runtime/log.h"
#include "runtime/rsx/rsx.h"
#include "runtime/rsx/rsx_internal.h"

namespace rt::rsx {
namespace {

std::atomic<bool> g_told{false};
std::atomic<u64> g_methods{0};

void tell_once() {
  if (g_told.exchange(true)) return;
  LOG_W("gpu", "the Linux Vulkan renderer is not implemented: RSX commands are accepted and presented as a black frame");
}

}  // namespace

u32 g_covered_put = ~0u;

void renderer_thread_init() { tell_once(); }
void renderer_idle() {}
void present_buffer(u32, const DisplayBuffer&) {}

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
