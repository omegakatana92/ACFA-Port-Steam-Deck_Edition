# Building on Linux (development)

This page describes the **current** Linux development build. It is not a release guide: the
renderer is not implemented, so the executable cannot play the game yet.

## Requirements

- x86-64 Linux (Ubuntu 22.04 or newer, SteamOS 3.x). The runtime and generated code require
  x86-64-v3 (AVX2, FMA, BMI1/2).
- GCC 11+ or Clang 14+, CMake 3.25+, Ninja.
- Vulkan headers and loader (`libvulkan-dev` on Debian/Ubuntu). A Vulkan ICD is needed only for
  the smoke test (Mesa lavapipe is enough on a headless machine).
- SDL3 is fetched automatically (pinned 3.4.18). Set `-DACFA_USE_SYSTEM_SDL3=ON` to use a Linux
  system package instead. Do not point CMake at a Windows SDL3 package.

## Build

```bash
git clone https://github.com/omegakatana92/ACFA-Port-Steam-Deck_Edition.git
cd ACFA-Port-Steam-Deck_Edition
git checkout linux-native-port

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DACFA_BUILD_LINUX_RUNTIME=ON -DACFA_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Targets:

| Target | Purpose |
| --- | --- |
| `acfa-prep` | import the user's decrypted files, relocate firmware modules, rebuild SPU records |
| `acfa-recomp` | PPU analysis and C++ emission |
| `acfa-spurecomp` | SPU record → C++ |
| `acfa` | runtime (only when generated PPU code exists in `local/gen/ppu`) |
| `acfa-host-tests` | hashes, path guard, INI (no game data) |
| `acfa-vk-smoke` | headless Vulkan clear/readback gate (no game data) |
| `acfa-vk-present` | SDL swapchain + animated test pattern; resize/fullscreen (no game data) |

Helper scripts:

```bash
scripts/check-linux.sh src/runtime/foo.cpp   # syntax-check selected files
scripts/check-linux-all.sh                   # syntax-check the Linux runtime set
scripts/configure-linux.sh /tmp/acfa-linux -DACFA_BUILD_LINUX_RUNTIME=ON
scripts/build-linux.sh /tmp/acfa-linux
```

## Game data and generated code

The repository contains no game data or game-derived code. You must own the US disc
(BLUS30187). Preparation produces decrypted inputs; then:

```bash
./build/acfa-prep cpu
./build/acfa-prep import --disc <disc> --eboot <EBOOT.elf> \
    --modules-dir <decrypted-modules> --flash-file <flashATRAC.pic> \
    --out local/game/BLUS30187
./build/acfa-recomp emit local/game/BLUS30187/eboot.elf local/gen/ppu \
    --hooks src/runtime/game/hooks.txt
./build/acfa-spurecomp local/gen/spu <spu-records>
```

The exact inputs come from the port's established preparation flow (RPCS3 once, for the user's
own files). The Windows launcher automates it; a Linux setup CLI does not exist yet. **Do not use
this section to claim a working game run**: the Linux renderer is a placeholder and no real-data
run has been performed.

## Vulkan presentation gate (no game data)

```bash
# On a desktop:
./build/acfa-vk-present --frames 180 --verify

# Headless CI with Mesa lavapipe:
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
SDL_VIDEODRIVER=x11 xvfb-run -a -s '-screen 0 1280x800x24' \
  ./build/acfa-vk-present --frames 180 --verify
```

Expected: `presenter ready`, `frame verification: OK`, `presented 180 frames`. The same path is
available from the runtime as `./build/acfa --test-present` (300 frames, then exit).

This validates the SDL surface, swapchain, upload, aspect-fit blit, readback, resize and
borderless fullscreen. It does **not** validate shaders, game rendering or the Steam Deck GPU.

## Stage 2 shader/draw gate (DXC)

Requires DXC with the SPIR-V backend. The gate was tested with DXC 1.8.2502 (the current 1.9
release needs glibc 2.38; Ubuntu 22.04 has 2.35).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DACFA_BUILD_LINUX_RUNTIME=ON -DACFA_BUILD_TESTS=ON \
  -DACFA_DXC_EXECUTABLE=/path/to/dxc/bin/dxc \
  -DACFA_DXC_LIB_DIR=/path/to/dxc/lib
cmake --build build -j"$(nproc)"
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/acfa-vk-draw --validate
```

Expected: four coloured quadrants `OK`, `validation errors: 0`. `--no-flip` must report
`flip sensitivity: detected`; `scripts/check-stage2-bindings.sh` checks the emitted bindings.

The gate compiles `tests/shaders/stage2.hlsl` at build time with:

```
-fspv-target-env=vulkan1.1 -fspv-entrypoint-name=main -fvk-use-dx-layout
-fvk-b-shift 0 0 -fvk-t-shift 1000 0 -fvk-s-shift 2000 0 -fvk-u-shift 3000 0
```

If DXC is not configured the Stage 2 target and tests are skipped; the rest of the build is
unaffected.

## Run (expected current behaviour)

```bash
./build/acfa --data /path/to/writable/data
```

On Linux, without `--user`, settings and saves go to `$XDG_DATA_HOME/acfa` (default
`~/.local/share/acfa`); `--user DIR` overrides that. Game data still comes from `--data`.

Without prepared game data the runtime stops with:

```
load failed: cannot open .../game/BLUS30187/eboot.elf
```

With data it would run guest code, HLE, audio and input, but the RSX output is not drawn or
presented yet.

## Current limitations

- Vulkan Stage 1 only: swapchain + test pattern; no shaders, surfaces or game rendering.
- No texture write tracking on Linux; the renderer fallback hashes textures.
- No Linux setup GUI, AppImage or release.
- Windows builds are preserved in the source and verified with the pinned toolchain.
