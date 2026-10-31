// Linux window, keyboard, mouse and monitor layer (SDL3). The Windows implementation is
// gpu/window.cpp; both provide the same gpu::window_* interface.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/config.h"
#include "runtime/game/frame_policy.h"
#include "runtime/game/menu_mouse.h"
#include "runtime/gpu/display.h"
#include "runtime/gpu/input_hooks.h"
#include "runtime/gpu/window.h"
#include "runtime/input.h"
#include "runtime/keycodes.h"
#include "runtime/log.h"
#include "runtime/settings.h"
#include "runtime/sysdialog.h"
#include "runtime/ui/overlay.h"

namespace rt::gpu {
namespace {

SDL_Window* g_win = nullptr;
SDL_WindowID g_win_id = 0;
std::atomic<bool> g_close{false};
std::atomic<bool> g_fullscreen{false};
std::atomic<bool> g_mouse_captured{false};
bool g_alive = false;
bool g_user_released = false;
std::thread::id g_window_thread;
std::mutex g_call_mu;
std::condition_variable g_call_cv;
struct Call {
  std::function<void()> fn;
  bool done = false;
};
std::deque<std::shared_ptr<Call>> g_calls;

u32 vk_from_key(SDL_Keycode k) {
  if (k >= SDLK_A && k <= SDLK_Z) return u32('A' + (k - SDLK_A));
  if (k >= SDLK_0 && k <= SDLK_9) return u32('0' + (k - SDLK_0));
  if (k >= SDLK_F1 && k <= SDLK_F24) return VK_F1 + u32(k - SDLK_F1);
  if (k >= SDLK_KP_0 && k <= SDLK_KP_9) return VK_NUMPAD0 + u32(k - SDLK_KP_0);
  switch (k) {
    case SDLK_BACKSPACE: return VK_BACK;
    case SDLK_TAB: return VK_TAB;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return VK_RETURN;
    case SDLK_ESCAPE: return VK_ESCAPE;
    case SDLK_SPACE: return VK_SPACE;
    case SDLK_DELETE: return VK_DELETE;
    case SDLK_INSERT: return VK_INSERT;
    case SDLK_HOME: return VK_HOME;
    case SDLK_END: return VK_END;
    case SDLK_PAGEUP: return VK_PRIOR;
    case SDLK_PAGEDOWN: return VK_NEXT;
    case SDLK_LEFT: return VK_LEFT;
    case SDLK_RIGHT: return VK_RIGHT;
    case SDLK_UP: return VK_UP;
    case SDLK_DOWN: return VK_DOWN;
    case SDLK_CAPSLOCK: return VK_CAPITAL;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT: return VK_SHIFT;
    case SDLK_LCTRL:
    case SDLK_RCTRL: return VK_CONTROL;
    case SDLK_LALT:
    case SDLK_RALT: return VK_MENU;
    case SDLK_KP_MULTIPLY: return VK_MULTIPLY;
    case SDLK_KP_PLUS: return VK_ADD;
    case SDLK_KP_MINUS: return VK_SUBTRACT;
    case SDLK_KP_PERIOD: return VK_DECIMAL;
    case SDLK_KP_DIVIDE: return VK_DIVIDE;
    case SDLK_SEMICOLON: return VK_OEM_1;
    case SDLK_EQUALS: return VK_OEM_PLUS;
    case SDLK_COMMA: return VK_OEM_COMMA;
    case SDLK_MINUS: return VK_OEM_MINUS;
    case SDLK_PERIOD: return VK_OEM_PERIOD;
    case SDLK_SLASH: return VK_OEM_2;
    case SDLK_GRAVE: return VK_OEM_3;
    case SDLK_LEFTBRACKET: return VK_OEM_4;
    case SDLK_BACKSLASH: return VK_OEM_5;
    case SDLK_RIGHTBRACKET: return VK_OEM_6;
    case SDLK_APOSTROPHE: return VK_OEM_7;
    default: return 0;
  }
}

void refresh_cursor() {
  if (g_mouse_captured) {
    SDL_HideCursor();
    return;
  }
  if (g_win && (sysdlg::shown() || ui::is_open() || game::menu_cursor_visible()))
    SDL_ShowCursor();
  else
    SDL_ShowCursor();
}

void set_mouse_capture(bool on) {
  if (!g_win) return;
  g_mouse_captured = on;
  SDL_SetWindowRelativeMouseMode(g_win, on);
  if (on) {
    SDL_Rect r{0, 0, 0, 0};
    int w = 0, h = 0;
    SDL_GetWindowSize(g_win, &w, &h);
    r.w = w;
    r.h = h;
    SDL_SetWindowMouseRect(g_win, &r);
    SDL_HideCursor();
  } else {
    SDL_SetWindowMouseRect(g_win, nullptr);
    SDL_ShowCursor();
  }
  input_mouse_capture(on);
  LOG_I("input", "mouse %s", on ? "captured (F1 releases it)" : "released");
}

void capture_for_gameplay() {
  if (g_mouse_captured || g_user_released || ui::is_open() || sysdlg::shown() ||
      game::input_context() != game::InputContext::Gameplay || game::controller_used_last())
    return;
  if (SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS) set_mouse_capture(true);
}

int button_index(u8 b) {
  switch (b) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    case SDL_BUTTON_X1: return 3;
    case SDL_BUTTON_X2: return 4;
    default: return -1;
  }
}

void on_focus(bool focused) {
  LOG_I("input", "window focus %s", focused ? "back" : "lost");
  if (focused) {
    input_focus(true);
    capture_for_gameplay();
  } else {
    set_mouse_capture(false);
    input_focus(false);
  }
}

void toggle_borderless() { settings::toggle_fullscreen(); }

}  // namespace

void window_dpi_aware() {}

bool window_create(const std::string& title, int client_w, int client_h, bool visible) {
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    LOG_E("window", "SDL video init failed: %s", SDL_GetError());
    return false;
  }
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
  if (!visible) flags |= SDL_WINDOW_HIDDEN;
  g_win = SDL_CreateWindow(title.c_str(), client_w, client_h, flags);
  if (!g_win) {
    LOG_E("window", "SDL_CreateWindow failed: %s", SDL_GetError());
    return false;
  }
  g_win_id = SDL_GetWindowID(g_win);
  g_alive = true;
  g_window_thread = std::this_thread::get_id();
  SDL_StartTextInput(g_win);
  LOG_I("window", "SDL3 window %dx%d: %s", client_w, client_h, title.c_str());
  return true;
}

void* window_handle() { return g_win; }

bool window_pump() {
  if (!g_win) return false;
  {
    std::lock_guard lk(g_call_mu);
    for (auto& c : g_calls) {
      c->fn();
      c->done = true;
    }
    g_calls.clear();
  }
  g_call_cv.notify_all();
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    const bool dialog = sysdlg::shown();
    const bool overlay = dialog || ui::is_open();
    switch (e.type) {
      case SDL_EVENT_QUIT:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e.window.windowID == g_win_id) {
          if (dialog) sysdlg::abort();
          g_close = true;
        }
        break;
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (e.window.windowID == g_win_id) on_focus(true);
        break;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (e.window.windowID == g_win_id) on_focus(false);
        break;
      case SDL_EVENT_WINDOW_RESIZED:
        if (e.window.windowID == g_win_id && !g_fullscreen && !g_mouse_captured && e.window.data1 >= 320 &&
            e.window.data2 >= 180)
          settings::note_window_size(e.window.data1, e.window.data2);
        break;
      case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        if (e.window.windowID == g_win_id) settings::apply_window_mode();
        break;
      case SDL_EVENT_MOUSE_MOTION: {
        int b = -1;
        (void)b;
        if (g_mouse_captured) {
          input_mouse_motion(int(e.motion.xrel), int(e.motion.yrel));
          break;
        }
        int x = int(e.motion.x), y = int(e.motion.y);
        if (dialog) sysdlg::on_mouse_move(x, y);
        else if (overlay) ui::on_mouse_move(x, y);
        if (!overlay && !input_isolated()) {
          bool shown = game::menu_cursor_visible();
          game::menu_mouse_moved();
          if (!shown && game::input_context() == game::InputContext::Menu) refresh_cursor();
          capture_for_gameplay();
        }
        break;
      }
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP: {
        bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        int button = button_index(e.button.button);
        if (button < 0) break;
        int x = int(e.button.x), y = int(e.button.y);
        if (g_mouse_captured) {
          input_mouse_button_event(button, down);
          break;
        }
        if (dialog) {
          sysdlg::on_mouse_button(button, down, x, y);
          break;
        }
        if (overlay) {
          ui::on_mouse_button(button, down, x, y);
          break;
        }
        if (!input_isolated()) {
          if (game::input_context() != game::InputContext::Gameplay) {
            if (button <= 1) game::menu_mouse_button(button, down);
            if (down) refresh_cursor();
            break;
          }
          if (down && button == 0) {
            g_user_released = false;
            set_mouse_capture(true);
          }
        }
        break;
      }
      case SDL_EVENT_MOUSE_WHEEL: {
        int delta = int(e.wheel.y * 120.0f);
        if (g_mouse_captured) {
          input_mouse_wheel(delta);
          break;
        }
        if (dialog) {
          sysdlg::on_wheel(delta);
          break;
        }
        if (overlay) {
          ui::on_wheel(delta);
          break;
        }
        if (game::input_context() == game::InputContext::Menu) {
          static int rest = 0;
          rest += delta;
          int notches = rest / 120;
          rest -= notches * 120;
          if (notches) game::menu_mouse_wheel(notches);
        }
        break;
      }
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP: {
        if (e.key.windowID != g_win_id) break;
        bool down = e.type == SDL_EVENT_KEY_DOWN;
        bool repeat = e.key.repeat;
        u32 vk = vk_from_key(e.key.key);
        if (!vk) break;
        if (down && !repeat) {
          if (vk == VK_F11 || (vk == VK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
            toggle_borderless();
            break;
          }
          if (vk == VK_F1 && !input_isolated()) {
            if (!overlay) {
              g_user_released = g_mouse_captured;
              set_mouse_capture(!g_mouse_captured);
            }
            break;
          }
        }
        if (dialog) {
          sysdlg::on_key(vk, down, repeat);
          break;
        }
        if (overlay) {
          ui::on_key(vk, down, repeat);
          break;
        }
        input_key(vk, down);
        break;
      }
      case SDL_EVENT_TEXT_INPUT:
        if (e.text.windowID == g_win_id && sysdlg::shown()) {
          // UTF-8 -> code points -> UTF-16 is done by sysdialog's on_char (one key at a time).
          const char* s = e.text.text;
          while (*s) {
            u32 cp = u32(static_cast<unsigned char>(*s++));
            if (cp < 0x80) sysdlg::on_char(cp);
          }
        }
        break;
      default: break;
    }
  }
  return g_alive;
}

bool window_close_requested() { return g_close.load(); }

void window_set_title(const std::string& title) {
  if (g_win) SDL_SetWindowTitle(g_win, title.c_str());
}

void window_set_fullscreen(bool on) {
  if (!g_win) return;
  SDL_SetWindowFullscreen(g_win, on);
  g_fullscreen = on;
}

bool window_is_fullscreen() { return g_fullscreen.load(); }

void window_set_client_size(int w, int h) {
  if (g_win) SDL_SetWindowSize(g_win, w, h);
}

void window_client_size(int* w, int* h) {
  if (g_win) {
    SDL_GetWindowSize(g_win, w, h);
    return;
  }
  *w = *h = 0;
}

bool window_cursor_position(int* x, int* y) {
  if (!g_win) return false;
  float fx = 0, fy = 0;
  SDL_MouseButtonFlags b = SDL_GetMouseState(&fx, &fy);
  (void)b;
  if (!(SDL_GetWindowFlags(g_win) & SDL_WINDOW_MOUSE_FOCUS)) return false;
  *x = int(fx);
  *y = int(fy);
  return true;
}

void window_work_area(int* w, int* h) {
  *w = *h = 0;
  SDL_DisplayID d = g_win ? SDL_GetDisplayForWindow(g_win) : SDL_GetPrimaryDisplay();
  SDL_Rect r{};
  if (SDL_GetDisplayUsableBounds(d, &r)) {
    *w = r.w;
    *h = r.h;
  }
}

void window_monitor_size(int* w, int* h) {
  *w = *h = 0;
  SDL_DisplayID d = g_win ? SDL_GetDisplayForWindow(g_win) : SDL_GetPrimaryDisplay();
  SDL_Rect r{};
  if (SDL_GetDisplayBounds(d, &r)) {
    *w = r.w;
    *h = r.h;
  }
}

std::vector<Monitor> window_monitors() {
  std::vector<Monitor> out;
  int n = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&n);
  SDL_DisplayID primary = SDL_GetPrimaryDisplay();
  for (int i = 0; i < n; ++i) {
    SDL_Rect r{};
    if (!SDL_GetDisplayBounds(ids[i], &r)) continue;
    Monitor m;
    m.device = SDL_GetDisplayName(ids[i]) ? SDL_GetDisplayName(ids[i]) : ("display-" + std::to_string(i));
    m.x = r.x;
    m.y = r.y;
    m.w = r.w;
    m.h = r.h;
    m.primary = ids[i] == primary;
    out.push_back(m);
  }
  if (ids) SDL_free(ids);
  return out;
}

void window_move_to_monitor(const std::string& device) {
  if (!g_win || device.empty()) return;
  auto monitors = window_monitors();
  for (const Monitor& m : monitors) {
    if (m.device != device) continue;
    if (g_fullscreen)
      SDL_SetWindowPosition(g_win, m.x, m.y);
    else
      SDL_SetWindowPosition(g_win, m.x + (m.w - 1280) / 2, m.y + (m.h - 720) / 2);
    return;
  }
}

void window_set_mouse_capture(bool on) { set_mouse_capture(on); }
bool window_mouse_captured() { return g_mouse_captured.load(); }
void window_request_mouse_capture(bool on) {
  g_user_released = !on;
  run_on_window_thread([on] {
    if (on) capture_for_gameplay();
    else set_mouse_capture(false);
    refresh_cursor();
  });
}
void window_refresh_cursor() { run_on_window_thread([] { refresh_cursor(); }); }

bool run_on_window_thread(const std::function<void()>& fn) {
  if (!g_win || !g_alive) return false;
  if (std::this_thread::get_id() == g_window_thread) {
    fn();
    return true;
  }
  auto item = std::make_shared<Call>();
  item->fn = fn;
  {
    std::lock_guard lk(g_call_mu);
    g_calls.push_back(item);
  }
  SDL_Event e{};
  e.type = SDL_EVENT_USER;
  SDL_PushEvent(&e);
  std::unique_lock lk(g_call_mu);
  g_call_cv.wait(lk, [&] { return item->done; });
  return true;
}

}  // namespace rt::gpu
