# Steam Deck validation and AppImage packaging plan

Status: **not started**. No Linux build has run the game, and no AppImage exists. This document
defines the hardware/software test matrix and the packaging contract. Nothing here is a claim that
packaging or Deck testing has been performed.

Prerequisites: `docs/VULKAN_RENDERER.md` gates G1–G9 and `docs/OVERLAY.md` must pass before the
first Deck test; G9 (a real game run) must pass before any AppImage is built.

---

## 1. Target environment

| Item | Target |
| --- | --- |
| Hardware | Steam Deck LCD (Van Gogh) and OLED; x86-64-v3 |
| OS | SteamOS 3.x (Arch-based, immutable) |
| Session | Gaming Mode (gamescope) and Desktop Mode (KWin/Wayland or X11) |
| Display | 1280×800, 60 Hz; external displays optional |
| Graphics | Vulkan 1.3, Mesa RADV |
| Audio | PipeWire (ALSA/Pulse compatibility) |
| Input | built-in controls via SDL3; Bluetooth/USB gamepads; keyboard/mouse in Desktop Mode |
| Storage | microSD or internal; game data and saves on a writable volume |
| Runtime | no Proton, no Wine |

The AppImage must not assume SteamOS packages beyond the base system: Vulkan loader and drivers
come from the system; SDL3 is bundled only if not statically linked.

---

## 2. Test matrix

### 2.1 Boot and presentation

- [ ] AppImage starts from Desktop Mode by double-click / terminal.
- [ ] AppImage added to Steam launches in Gaming Mode.
- [ ] Window is 1280×800, borderless, correct aspect, no cursor in gameplay.
- [ ] F11 / Alt+Enter toggles windowed/borderless.
- [ ] External 1920×1080 display: scaling and letterboxing correct.
- [ ] Suspend/resume restores audio, video, controller and timing.
- [ ] Quit from the game's menu exits cleanly and flushes logs.

### 2.2 Content

- [ ] Title screen and intro movies (60 Hz cap, audio sync).
- [ ] Mission load, gameplay, pause, restart, mission end.
- [ ] Garage: ASSEMBLE, STABILIZER, SHOP, TUNE, DESIGN, colour editor, emblem picker.
- [ ] SYSTEM menu, PC SETTINGS open/close, settings persist.
- [ ] Save, load, delete; suspend during a mission then resume.
- [ ] ORDER MATCH / arena AI fixes setting on and off.
- [ ] Mission briefings and movies.
- [ ] Controls: sticks, triggers, bumpers, D-pad, face buttons, stick clicks, Steam/Quick Access.

### 2.3 Performance

| Metric | Target |
| --- | --- |
| Frame rate | 60 fps in missions at 1280×800, render scale 100 % |
| Frame time | p99 ≤ 20 ms; no repeated hitches during texture streaming |
| CPU | all 8 threads within thermal budget; no core pinned at 100 % in menus |
| Memory | stable working set over 30 minutes; no unbounded texture/surface growth |
| Shader compile | no multi-second stalls after warm-up; cache persists |
| Audio | no dropouts, no drift over 30 minutes |

Record `runs/<timestamp>/acfa.log` and a 10-minute capture. Do not claim a pass without logs.

### 2.4 Input

- [ ] Built-in controls recognised as a gamepad in Gaming Mode.
- [ ] Desktop Mode with Steam Input disabled/enabled (virtual pad).
- [ ] Bluetooth DualSense/DualShock 4 and Xbox controller; hotplug.
- [ ] Rumble.
- [ ] Keyboard/mouse in Desktop Mode; F1 releases the mouse.
- [ ] No stuck buttons after opening/closing PC SETTINGS or dialogs.

### 2.5 Audio/video

- [ ] Stereo output; no crackle; volume.
- [ ] Movie playback with correct A/V sync.
- [ ] Suspend during a movie and resume.
- [ ] Headphones/Bluetooth audio device change (best effort; re-open may need a restart).

### 2.6 Saves and paths

- [ ] Default `$XDG_DATA_HOME/acfa` (or `~/.local/share/acfa`) writable in Gaming Mode.
- [ ] `--data` points at a writable game-data folder on microSD/internal.
- [ ] Saves survive an AppImage update.
- [ ] PS3/RPCS3 save folders import correctly.
- [ ] No writes into the AppImage mount or the read-only game tree.

---

## 3. AppImage contract

### 3.1 Name and layout

```
ArmoredCoreForAnswer-SteamDeck-x86_64.AppImage
  AppRun
  armored-core-for-answer.desktop
  armored-core-for-answer.png
  usr/bin/acfa
  usr/lib/...            (only bundled libraries)
  usr/share/licenses/... (MIT, zlib, SDL, font)
```

- `AppRun`:
  - resolves `APPDIR`;
  - sets `LD_LIBRARY_PATH` to the bundled `usr/lib` only;
  - exports `SDL_VIDEODRIVER` only if a fallback is needed (normally unset);
  - chooses the writable data directory: `$XDG_DATA_HOME/acfa`, else `~/.local/share/acfa`;
  - passes user arguments through;
  - never writes inside `APPDIR`.
- Desktop entry: `Exec=AppRun`, `Icon=armored-core-for-answer`, `Categories=Game;`, `Terminal=false`.
- Icon: the port's own icon, not a copyrighted game asset unless the user supplies it; use the
  project icon (`data/acfa.ico` converted to PNG) or a neutral placeholder.

### 3.2 Dependencies

Bundle:

- SDL3 if dynamically linked (or link it statically);
- the overlay font and its licence;
- GCC/libstdc++ only if the target baseline requires it; prefer building against the oldest
  supported SteamOS glibc and not bundling libstdc++ unless necessary.

Do **not** bundle:

- Mesa/RADV, AMDVLK, NVIDIA drivers, or `libvulkan.so.1`;
- the Vulkan loader/ICDs;
- glibc;
- game data, firmware, keys, decrypted executables, or saves.

Check with `ldd` and `readelf -d` that the binary resolves against the system loader and the
bundled SDL only.

### 3.3 Legal

- Include the MIT licence for the port code.
- Include zlib's licence (vendored).
- Include SDL3's zlib licence and the font's OFL/permite licence.
- Include LLVM notices if compiler-rt/libc++ are statically linked.
- State clearly: no game data is included; the user must own BLUS30187 and provide their own
  legally obtained files; update 01.35 is optional and lawful.
- Attribute the original developer (`SulphurS16/ACFA-Port`) and the AC4 fork where applicable.
- Do not imply endorsement by FromSoftware, Ubisoft or Sony.

### 3.4 Build reproducibility

- Pin the toolchain, SDL3 and DXC versions; record hashes.
- Build in a clean container or chroot with a documented glibc baseline.
- `strip` the binary; keep debug symbols in a separate artifact.
- Produce a SHA-256 checksum and attach it to the release.
- Smoke-test the AppImage on a clean SteamOS user account, not only the build machine.

---

## 4. Release checklist

Before publishing `ArmoredCoreForAnswer-SteamDeck-x86_64.AppImage`:

1. Vulkan gates G1–G9 pass (`docs/VULKAN_RENDERER.md`).
2. Overlay gates pass (`docs/OVERLAY.md`).
3. Deck matrix §2 passes with logs attached.
4. AppImage runs on a clean SteamOS install in Desktop and Gaming Mode.
5. No game data, firmware, keys, decrypted executables or saves inside the AppImage.
6. Licences and attribution included.
7. Checksums and release notes published.
8. Known issues list is accurate (audio device changes, external displays, performance, etc.).

If any item fails, publish a status update, not an AppImage.

---

## 5. Current state

- No game-data run.
- No Steam Deck test.
- No AppImage, `AppRun`, desktop entry or icon conversion.
- No packaging scripts.
- The only Linux artifact is a development `acfa` ELF built against a synthetic PPU table.
