// Vulkan Stage 1 gate: SDL window + swapchain + animated CPU test pattern, with resize and
// borderless fullscreen transitions. Run on a real display or Xvfb; no game data.
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "runtime/gpu/vulkan/vk_presenter.h"

int main(int argc, char** argv) {
  int frames = 180;
  bool verify = false;
  int w = 1280, h = 800;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--frames" && i + 1 < argc) frames = std::max(1, std::atoi(argv[++i]));
    else if (a == "--verify") verify = true;
    else if (a == "--size" && i + 2 < argc) {
      w = std::atoi(argv[++i]);
      h = std::atoi(argv[++i]);
    }
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s (skipping; a display or Xvfb is required)\n", SDL_GetError());
    return 77;
  }
  SDL_Window* window = SDL_CreateWindow("ACFA Vulkan Stage 1", w, h,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    SDL_Quit();
    return 2;
  }
  std::string error;
  if (!rt::gpu::vk::init(window, &error)) {
    std::fprintf(stderr, "Vulkan init: %s\n", error.c_str());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 2;
  }
  std::printf("presenter ready: %ux%u\n", rt::gpu::vk::width(), rt::gpu::vk::height());

  const auto start = std::chrono::steady_clock::now();
  bool ok = true;
  for (int i = 0; i < frames && ok; ++i) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        frames = i;
        break;
      }
    }
    if (i == 30) SDL_SetWindowSize(window, 960, 600);
    if (i == 60) SDL_SetWindowSize(window, 1280, 800);
    if (i == 90) SDL_SetWindowFullscreen(window, true);
    if (i == 120) SDL_SetWindowFullscreen(window, false);

    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!rt::gpu::vk::present_test_pattern(t, &error, verify && i == 0)) {
      std::fprintf(stderr, "present frame %d: %s\n", i, error.c_str());
      ok = false;
    }
    SDL_Delay(16);
  }
  if (verify) {
    std::printf("frame verification: %s\n", rt::gpu::vk::last_verify_ok() ? "OK" : "MISMATCH");
    if (!rt::gpu::vk::last_verify_ok()) ok = false;
  }
  rt::gpu::vk::shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  std::printf("presented %d frames\n", frames);
  return ok ? 0 : 1;
}
