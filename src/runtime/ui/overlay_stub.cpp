// Linux PC SETTINGS / dialog drawing placeholder. The settings model and input routing are in
// overlay.cpp (Windows, Direct2D/DirectWrite). They will be moved behind the Vulkan 2D layer; until
// then the Linux build keeps system dialogs operable by keyboard/controller and reports that the
// settings screen cannot be drawn.
#include "runtime/ui/overlay.h"

#include "runtime/log.h"
#include "runtime/sysdialog.h"

namespace rt::ui {

bool screen_up() { return sysdlg::shown(); }
bool is_open() { return false; }

void open_from_menu() {
  LOG_W("ui", "PC SETTINGS needs the Linux 2D drawing layer, which is not implemented yet");
}
void decline_install() {}
void close() {}

bool on_key(u32, bool, bool) { return false; }
void on_mouse_move(int, int) {}
void on_mouse_button(int, bool, int, int) {}
void on_wheel(int) {}
void tick() {}
PadMode pad_mode() { return PadMode::Normal; }
u16 pad_suppressed(u32, u16 buttons) { return buttons; }

DialogHit dialog_hit(const DialogView&, int, int) { return {}; }

void render(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*, u32, u32) {}

}  // namespace rt::ui
