// Linux display timing (SDL3 + a monotonic clock). There is no DXGI vblank wait: the emulated
// vblank runs from the clock at the configured rate, with the game's 60 Hz rule outside missions
// and during movies preserved. VK_KHR_present_wait can refine this in the Vulkan backend.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/config.h"
#include "runtime/gpu/display.h"
#include "runtime/gpu/window.h"
#include "runtime/log.h"
#include "runtime/platform.h"

namespace rt::lv2 {
u64 system_time_us();
void sleep_us(u64 us);
}  // namespace rt::lv2

namespace rt::gpu::display {
namespace {

std::mutex g_mu;
std::atomic<bool> g_exclusive{false};
std::atomic<int> g_movies{0};
std::atomic<int> g_gameplay{0};
std::string g_pacing = "clock";
u64 g_presents = 0;
constexpr double kUncappedHz = 1000;

double target_fps() {
  double want = cfg().fps, fps;
  if (want < 0)
    fps = kUncappedHz;
  else if (want > 0)
    fps = want;
  else
    fps = refresh_hz() >= 30 ? double(refresh_hz()) : 60.0;
  bool outside = g_movies.load(std::memory_order_relaxed) > 0 || g_gameplay.load(std::memory_order_relaxed) == 0;
  return outside && std::fabs(fps - 60.0) > 0.5 ? 60.0 : fps;
}

double g_next_tick_us = 0;

void wait_clock() {
  const double fps = target_fps();
  if (fps >= kUncappedHz) {
    g_next_tick_us = 0;
    platform::sleep_ms(1);
    return;
  }
  const double period = 1e6 / fps;
  double now = double(platform::monotonic_us());
  if (g_next_tick_us == 0 || now - g_next_tick_us > 4 * period) g_next_tick_us = now;
  g_next_tick_us += period;
  for (;;) {
    now = double(platform::monotonic_us());
    double left = g_next_tick_us - now;
    if (left <= 0) return;
    if (left > 2000)
      platform::sleep_us(u64(left - 1000));
    else
      platform::yield_cpu();
  }
}

u32 current_refresh() {
  SDL_Window* w = static_cast<SDL_Window*>(window_handle());
  SDL_DisplayID d = w ? SDL_GetDisplayForWindow(w) : SDL_GetPrimaryDisplay();
  const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(d);
  return m && m->refresh_rate > 0 ? u32(std::lround(m->refresh_rate)) : 60;
}

}  // namespace

void wait_next_vblank() {
  g_pacing = "clock";
  wait_clock();
}

void movie_started() {
  if (g_movies.fetch_add(1) == 0 && cfg().fps != 60) LOG_I("display", "video playback: the emulated vblank runs at up to 60 Hz");
}
void movie_stopped() {
  if (g_movies.fetch_sub(1) == 1 && cfg().fps != 60) LOG_I("display", "video playback ended");
}
bool movie_playing() { return g_movies.load(std::memory_order_relaxed) > 0; }
void gameplay_started() {
  if (g_gameplay.fetch_add(1) == 0 && cfg().fps != 60) LOG_I("display", "mission started: frame rate target applies");
}
void gameplay_stopped() {
  if (g_gameplay.fetch_sub(1) == 1 && cfg().fps != 60) LOG_I("display", "mission ended: 60 Hz outside missions");
}

u32 present_flags() { return 0; }
void set_exclusive(bool on) { g_exclusive = on; }

u32 refresh_hz() { return current_refresh(); }

std::vector<Mode> modes() {
  std::vector<Mode> out;
  SDL_Window* w = static_cast<SDL_Window*>(window_handle());
  SDL_DisplayID d = w ? SDL_GetDisplayForWindow(w) : SDL_GetPrimaryDisplay();
  int n = 0;
  SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(d, &n);
  for (int i = 0; i < n; ++i) {
    const SDL_DisplayMode& m = *list[i];
    if (m.w < 640 || m.h < 360) continue;
    u32 hz = u32(std::lround(m.refresh_rate));
    if (hz < 30) continue;
    bool seen = std::any_of(out.begin(), out.end(), [&](const Mode& x) { return x.w == u32(m.w) && x.h == u32(m.h) && x.hz == hz; });
    if (!seen) out.push_back({u32(m.w), u32(m.h), hz});
  }
  if (list) SDL_free(list);
  std::sort(out.begin(), out.end(), [](const Mode& a, const Mode& b) {
    return a.w != b.w ? a.w > b.w : a.h != b.h ? a.h > b.h : a.hz > b.hz;
  });
  return out;
}

void swapchain_created(IDXGISwapChain1*, bool) {}
void after_present(IDXGISwapChain1*) { ++g_presents; }

std::string report() {
  u32 hz = refresh_hz();
  double fps = target_fps();
  return acfa::strprintf("display: %u Hz, %s; %llu presented", hz,
                         fps >= kUncappedHz ? "uncapped" : acfa::strprintf("%.0f Hz clock", fps).c_str(),
                         (unsigned long long)g_presents);
}

}  // namespace rt::gpu::display
