// Keyboard codes shared with the settings file and the guest's key bindings. Windows virtual-key
// numbers are kept as the on-disk/on-wire values so existing settings.ini files stay valid; on
// Linux the window backend translates SDL keycodes into these numbers.
#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "common/bytes.h"

constexpr u32 VK_BACK = 0x08;
constexpr u32 VK_TAB = 0x09;
constexpr u32 VK_RETURN = 0x0D;
constexpr u32 VK_SHIFT = 0x10;
constexpr u32 VK_CONTROL = 0x11;
constexpr u32 VK_MENU = 0x12;
constexpr u32 VK_CAPITAL = 0x14;
constexpr u32 VK_ESCAPE = 0x1B;
constexpr u32 VK_SPACE = 0x20;
constexpr u32 VK_PRIOR = 0x21;  // Page Up
constexpr u32 VK_NEXT = 0x22;   // Page Down
constexpr u32 VK_END = 0x23;
constexpr u32 VK_HOME = 0x24;
constexpr u32 VK_LEFT = 0x25;
constexpr u32 VK_UP = 0x26;
constexpr u32 VK_RIGHT = 0x27;
constexpr u32 VK_DOWN = 0x28;
constexpr u32 VK_INSERT = 0x2D;
constexpr u32 VK_DELETE = 0x2E;
constexpr u32 VK_NUMPAD0 = 0x60;
constexpr u32 VK_NUMPAD9 = 0x69;
constexpr u32 VK_MULTIPLY = 0x6A;
constexpr u32 VK_ADD = 0x6B;
constexpr u32 VK_SUBTRACT = 0x6D;
constexpr u32 VK_DECIMAL = 0x6E;
constexpr u32 VK_DIVIDE = 0x6F;
constexpr u32 VK_F1 = 0x70;
constexpr u32 VK_F2 = 0x71;
constexpr u32 VK_F3 = 0x72;
constexpr u32 VK_F4 = 0x73;
constexpr u32 VK_F5 = 0x74;
constexpr u32 VK_F6 = 0x75;
constexpr u32 VK_F7 = 0x76;
constexpr u32 VK_F8 = 0x77;
constexpr u32 VK_F9 = 0x78;
constexpr u32 VK_F10 = 0x79;
constexpr u32 VK_F11 = 0x7A;
constexpr u32 VK_F12 = 0x7B;
constexpr u32 VK_F24 = 0x87;
constexpr u32 VK_OEM_1 = 0xBA;       // ;:
constexpr u32 VK_OEM_PLUS = 0xBB;    // =+
constexpr u32 VK_OEM_COMMA = 0xBC;   // ,<
constexpr u32 VK_OEM_MINUS = 0xBD;   // -_
constexpr u32 VK_OEM_PERIOD = 0xBE;  // .>
constexpr u32 VK_OEM_2 = 0xBF;       // /?
constexpr u32 VK_OEM_3 = 0xC0;       // `~
constexpr u32 VK_OEM_4 = 0xDB;       // [{
constexpr u32 VK_OEM_5 = 0xDC;       // \|
constexpr u32 VK_OEM_6 = 0xDD;       // ]}
constexpr u32 VK_OEM_7 = 0xDE;       // '"
#endif
