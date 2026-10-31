// The controller state layout every consumer reads (the pad emulation, system dialogs and PC
// SETTINGS). Windows uses XInput's own types; Linux uses the same struct and constants so the
// guest-facing code and settings are identical on both platforms.
#pragma once

#include "common/bytes.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#else
struct XINPUT_GAMEPAD {
  u16 wButtons = 0;
  u8 bLeftTrigger = 0;
  u8 bRightTrigger = 0;
  s16 sThumbLX = 0;
  s16 sThumbLY = 0;
  s16 sThumbRX = 0;
  s16 sThumbRY = 0;
};

struct XINPUT_STATE {
  u32 dwPacketNumber = 0;
  XINPUT_GAMEPAD Gamepad;
};

constexpr u16 XINPUT_GAMEPAD_DPAD_UP = 0x0001;
constexpr u16 XINPUT_GAMEPAD_DPAD_DOWN = 0x0002;
constexpr u16 XINPUT_GAMEPAD_DPAD_LEFT = 0x0004;
constexpr u16 XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008;
constexpr u16 XINPUT_GAMEPAD_START = 0x0010;
constexpr u16 XINPUT_GAMEPAD_BACK = 0x0020;
constexpr u16 XINPUT_GAMEPAD_LEFT_THUMB = 0x0040;
constexpr u16 XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080;
constexpr u16 XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100;
constexpr u16 XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200;
constexpr u16 XINPUT_GAMEPAD_A = 0x1000;
constexpr u16 XINPUT_GAMEPAD_B = 0x2000;
constexpr u16 XINPUT_GAMEPAD_X = 0x4000;
constexpr u16 XINPUT_GAMEPAD_Y = 0x8000;
#endif
