// Vulkan Stage 1 presenter: SDL surface, swapchain and a CPU-generated test image.
// Linux only. The game renderer will later submit its own images through this presenter.
#pragma once

#include <string>
#include <vector>

#include "common/bytes.h"

namespace rt::gpu::vk {

// Creates the instance, surface, device and swapchain for an SDL window created with
// SDL_WINDOW_VULKAN. Safe to call from any thread, but SDL prefers the window thread.
bool init(void* sdl_window, std::string* error);
bool ready();
u32 width();
u32 height();

// Uploads RGBA8 and presents it aspect-fitted on a black background. Recreates the swapchain when
// the window size changed. When `verify` is true, copies the presented image back and checks the
// centre pixel (slow; tests only). `error` is set on failure.
bool present_rgba(const u8* rgba, u32 w, u32 h, std::string* error, bool verify = false);

// Builds an animated test pattern at the current window size and presents it.
bool present_test_pattern(double seconds, std::string* error, bool verify = false);

// Last presented pattern (CPU copy), for captures/diagnostics.
bool last_rgba(std::vector<u8>& rgba, u32& w, u32& h);
bool last_verify_ok();

void shutdown();

}  // namespace rt::gpu::vk
