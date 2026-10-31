// Keyboard, mouse and focus events from the window to the input layer.
#pragma once
#include "common/bytes.h"

namespace rt::gpu {

void input_key(u32 vk, bool down);
void input_focus(bool focused);
// A raw input mouse report (RAWMOUSE fields; Windows).
void input_mouse_raw(u16 flags, s32 x, s32 y, u16 button_flags, u16 button_data);
// Neutral mouse events (Linux/SDL): relative motion in pixels, a button change (0 left, 1 right,
// 2 middle, 3 back, 4 forward) and wheel notches (120 per detent).
void input_mouse_motion(s32 dx, s32 dy);
void input_mouse_button_event(int button, bool down);
void input_mouse_wheel(s32 delta);
void input_mouse_capture(bool captured);

}  // namespace rt::gpu
