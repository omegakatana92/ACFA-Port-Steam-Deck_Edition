# PC SETTINGS and system-dialog overlay — Linux design

Status: **stub**. On Linux, `src/runtime/ui/overlay_stub.cpp` provides the `ui::` API with
`is_open() == false`, no drawing and no PC SETTINGS opening. `sysdialog` remains functional by
keyboard/controller but its dialogs are not drawn because `ui::render` is a no-op.

This document specifies the 2D layer that replaces the stub. It does not claim the overlay works.

---

## 1. What exists on Windows

- `ui/overlay.cpp` (about 1,400 lines) draws the PC SETTINGS screen and system dialogs.
- Drawing uses Direct2D 1.1 and DirectWrite, with Times New Roman and Bahnschrift/Segoe UI.
- The layout is a fixed 1280×720 canvas scaled uniformly and letterboxed to the window; hit-testing
  uses the same canvas coordinates.
- The overlay renders over the presented game frame, then the composed image is copied for captures.
- Input arrives from the window: keys, mouse, wheel, controller; `sysdialog` gets first refusal.
- The model/state and input routing are platform-neutral; only the drawing backend and font access
  are Windows-specific.

## 2. Split the module

Create two layers:

```
ui/overlay_model.cpp   // state, input, settings values, hit-testing, pad suppression
ui/overlay_draw.cpp    // platform drawing interface; Windows D2D and Linux Vulkan implementations
```

Keep `ui/overlay.h` as the public contract so `sysdialog.cpp`, `bindings.cpp`, `sys_io.cpp` and the
window backends do not change:

- `is_open`, `screen_up`, `open_from_menu`, `decline_install`, `close`
- `on_key`, `on_mouse_move`, `on_mouse_button`, `on_wheel`, `tick`
- `pad_mode`, `pad_suppressed`
- `dialog_hit`
- `render` (backend-specific signature)

`overlay_model.cpp` must compile on both platforms and be unit-testable without a GPU.

## 3. Drawing interface

The Vulkan 2D layer needs only a small set of primitives, derived from the current Direct2D calls:

| Primitive | Use |
| --- | --- |
| filled rectangle | page background, selection, panels |
| rectangle outline (1 px) | framed rows, focus |
| line | separators, slider tracks |
| gradient-filled rectangle | selected row highlight |
| filled triangle | arrows, slider handles |
| text run | captions, values, rows, dialogs |
| clip rectangle | scrolling lists, text clipping |

Proposed interface:

```
struct Canvas { float scale, ox, oy; };
void frame_begin(u32 window_w, u32 window_h, VkImage target);
void fill_rect(Rect, Colour);
void stroke_rect(Rect, Colour, float width);
void line(Point, Point, Colour);
void gradient_rect(Rect, Colour top, Colour bottom);
void triangle(Point a, Point b, Point c, Colour);
void text(float x, float y, std::string_view utf8, Font, float size, Colour, Align);
void clip_push(Rect);
void clip_pop();
void frame_end();
```

Implementation:

- One pipeline with a vertex buffer of coloured triangles and a texture atlas for glyphs.
- Alpha blending: `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`.
- Scissor for clipping (rectangular only, as today).
- No depth; draw in submission order.
- Dynamic viewport/scissor; negative-height viewport consistent with the game renderer.
- A per-frame staging ring for vertices; glyph atlas uploaded once.

## 4. Fonts and licensing

The Windows implementation uses system fonts that cannot be redistributed. The Linux build must use
a redistributable font:

- **DejaVu Sans** (Bitstream Vera licence, permissive) or **Noto Sans** (SIL OFL 1.1).
- The overlay uses two families (serif captions, sans rows). Ship one font with two faces or two
  OFL fonts.
- Bundle the licence text in the AppImage and in `licenses/`.
- Rasterise glyphs at runtime with FreeType or `stb_truetype` into an R8 atlas; cache by
  `(codepoint, pixel size)`.
- UTF-8 input; the game's text is ASCII/Latin-1 in practice, but dialogs accept user text.

Do **not** copy Windows font files into the repository or AppImage.

## 5. Layout and input

- Keep the 1280×720 canvas and the existing layout math so the screen looks the same.
- Convert client pixels to canvas coordinates: `(px - ox) / scale`.
- Keep the current hit-test regions and row geometry from the model.
- Mouse: hover focuses, click activates, right-click goes back, wheel scrolls, slider drag.
- Keyboard: arrows/WASD, Enter/Space, Escape/Backspace, Page Up/Down, Home/End, Tab, Q/E, Delete.
- Controller: D-pad/left stick with 380 ms then 90 ms repeat, A accept, B back, X clear, shoulders
  switch pages, START close.
- While the screen is open, the game's pad reads idle; released buttons are suppressed until
  released (`pad_suppressed`).
- In ACFA, closing after the install question sends a timed Circle press; preserve this state
  machine.

## 6. Rendering order

1. Game frame is rendered to the swapchain image (or blitted from the game target).
2. If `screen_up()`, draw the dim layer, panel, rows and dialogs.
3. Copy the composed image for captures if requested.
4. Present.

The overlay must not disturb the game renderer's state: separate pipeline, descriptor set, vertex
buffer and scissor state. With dynamic rendering this is natural; with render passes use a second
pass with `loadOp=LOAD`.

## 7. System dialogs

`sysdialog` owns the state and input; `DialogView` describes the content. The Linux drawing code
must implement:

- List: heading, rows with title/subtitle/detail/date/new marker, focus, scroll.
- Question: text, YES/NO buttons.
- Message: text, OK.
- Text entry: prompt, edit buffer, on-screen keyboard grid (`kDialogKeyCols` × rows), selected key.
- `dialog_hit` returns Row/Button/Key/None using the same canvas geometry.

A blind dialog is not acceptable for release: save/load, delete and text entry must be visible.

## 8. Verification

Without game data:

- A synthetic overlay test opens the screen, walks every page and row kind, drags a slider, types
  text, and captures screenshots.
- Compare screenshots against a reference for layout regressions (tolerance for font metrics).
- Exercise keyboard-only, mouse-only and controller-only paths.
- Verify the game's pad stays idle while the overlay is open and that suppressed buttons do not leak.

With game data (later): open PC SETTINGS from SYSTEM, change display/fps/render scale/keys, close,
and confirm the settings file and runtime behaviour.

## 9. Current Linux state

- `ui/overlay_stub.cpp`: `is_open=false`, `open_from_menu` logs a warning, `render` no-op,
  `dialog_hit` returns None.
- PC SETTINGS cannot be opened from the game.
- System dialogs can be answered by keyboard/controller but are invisible.
- No font, atlas or Vulkan 2D pipeline exists.

This is a release blocker, not a cosmetic gap.
