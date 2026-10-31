# ARMORED CORE for Answer — Native Linux / Steam Deck Port: Audit and Plan

**Phase 1 deliverable.** This is the dependency and portability audit that precedes the Linux
runtime work, plus the concrete plan for the later phases. It records what exists, what is
Windows-specific, what has already been ported, what is still missing, and how each claim was
verified.

- **Audited revision:** `main` @ `322f750` (`ACFA-Port 0.7.4`)
- **Fork:** `omegakatana92/ACFA-Port-Steam-Deck_Edition` (upstream `SulphurS16/ACFA-Port`, read-only)
- **Working branch:** `linux-native-port`
- **Audit host:** Windows 11 + WSL2 `Ubuntu-22.04` (GCC 11.4.0, CMake 4.3.3, Ninja 1.10.1,
  x86-64, 8 logical CPUs); no Steam Deck or PS3 hardware was available for this audit.
- **Method:** full read of the build system and every runtime subsystem; per-file Win32/D3D11/
  XInput/WASAPI/CNG/COM symbol inventory; comparison against the AC4 fork's tree; real Linux
  configure and compile runs, including a link check with a synthetic PPU table (see
  `docs/BUILD_LOG.md`).
- **Date:** 2026-10-31

> **Status honesty.** At the time of writing the Linux runtime **links and starts**, reserves the
> 4 GiB guest address space, initialises the platform layer and then stops because no game data is
> present. **No game has been run, no frame has been rendered, and no AppImage exists.** The Linux
> graphics backend is an explicit placeholder. Nothing in this document claims otherwise.

---

## 1. Executive summary

| Measure | Value |
| --- | --- |
| Tracked files in the audit revision | 216 |
| C/C++ source (`.cpp` + `.h`, no vendored zlib) | ~200 files, ~35,400 lines |
| Runtime (`src/runtime/**`) | ~27,000 lines |
| Preparation / recompilers (`prep`, `recomp`, `ppu`, `spu`, `spurecomp`) | ~4,300 lines |
| Launcher (Windows GUI) | ~3,400 lines |
| Files including `<windows.h>` (runtime + common, before port) | 30 |
| Direct3D 11 call sites | 222 in `runtime/gpu` (131 in `renderer.cpp`) + ~74 Direct2D/DirectWrite calls in `overlay.cpp` |
| Recompilers and shared libraries on Linux | **build and run** (verified) |
| Linux runtime translation units | **all compile** (verified) |
| Linux runtime executable | **links and starts** (verified with a synthetic PPU table) |
| Windows full build with the pinned llvm-mingw toolchain | **verified** (runtime, launcher, tools, host tests; start-up to the no-data error) |

**Headline findings**

1. **The preparation pipeline is already native.** `acfa-prep`, `acfa-recomp` and
   `acfa-spurecomp` build with GCC/Clang on Linux after only build-system and common-library
   changes. The PPU decoder, SPU ISA, recompiler and emitter contain no Windows API calls. This is
   the strongest part of the port and is now verified.
2. **The guest CPU/runtime core is portable in design.** The fixed 4 GiB guest mapping, recompiled
   function table, OPD dispatch, 128-byte reservations, LV2 synchronisation, HLE registration,
   save layout and MPEG-2 decoder are portable C++. The Windows dependency is a small set of host
   primitives: virtual memory, threads, signals, clocks, file I/O and the window/audio/controller
   backends.
3. **Direct3D 11 is the hard blocker, not the guest core.** `renderer.cpp` is a 2,830-line
   immediate-mode D3D11 renderer with no backend abstraction; `submit.cpp` mirrors a subset of
   `ID3D11DeviceContext`; `overlay.cpp` draws with Direct2D/DirectWrite. A Vulkan backend is a
   re-implementation of the draw path, not a recompile.
4. **The generated HLSL is a real portability asset but not a drop-in.** It is SM5 HLSL
   (`vs_5_0`/`ps_5_0`) using `ByteAddressBuffer.Load4`, `f16tof32`, `[branch]` and D3D's y-up NDC.
   Compiling it to SPIR-V requires DXC with SM 6.2, explicit Vulkan binding-space shifts (the
   generated code reuses `t0..t3`, `b0..b3`, `s0..s15` across stages), and a decided Y-flip
   strategy. Details in §8.
5. **ACFA and AC4 share most of the runtime but have diverged.** The movie stack, SPU, PPU,
   audio mixer, input, system dialogs and save code are byte-identical or near-identical; the
   renderer, frame policy, flip entry points, overlay pages and settings differ. AC4 has no
   committed Linux code (only a Phase 1 document), so there is no working Linux implementation to
   copy — only analysis to reuse. Details in §9.
6. **Steam Deck is a good target.** Van Gogh (Zen 2 + RDNA 2) satisfies the existing
   `-march=haswell` requirement, Vulkan is present in SteamOS, and 1280×800/60 fps/gamepad-only
   play are already first-class in the settings layer. The missing pieces are graphics, write
   tracking performance and packaging, not CPU compatibility.

---

## 2. Repository inventory

```
CMakeLists.txt            single build file
VERSION                   0.7.4
LICENSE                   MIT (port code only)
THIRD_PARTY.md            zlib, llvm-mingw/CMake/Ninja, Windows runtime components
data/                     acfa.ico, shader_manifest.txt, spu_manifest.txt, toolchain.txt
licenses/                 LLVM license, MinGW-w64 runtime notices
third_party/zlib/         zlib 1.3.1, unmodified
tools/                    PowerShell bootstrap/build scripts (Windows)
src/common/      (18)     bytes, SHA-1/256, files, INI, privacy, settings, SFO, known inputs
src/prep/        (6)      acfa-prep: keyless import, PRX relocation, SFO, SPU code records
src/ppu/         (4)      PPU decode and instruction semantics
src/recomp/      (7)      acfa-recomp: ELF image, control-flow analysis, C++ emitter
src/spu/         (5)      SPU ISA, chunk discovery, generated-code interface
src/spurecomp/   (1)      acfa-spurecomp: SPU records -> C++
src/runtime/     (110+)   host: loader, PPU threads, LV2, HLE, SPU, RSX, GPU, audio, media,
                          input, UI, VFS, settings, logging
src/launcher/    (16)     Windows GUI launcher (setup, downloads, RPCS3, install)
```

Subsystem portability at the audit revision:

| Subsystem | Lines | Portability |
| --- | --- | --- |
| `common` | ~1,400 | portable after the POSIX hash/path/INI/privacy changes |
| tools (`prep`,`recomp`,`ppu`,`spu`,`spurecomp`) | ~4,300 | portable; verified Linux binaries |
| `runtime` core (loader, LV2, HLE, SPU, media, saves) | ~20,000 | portable C++; host primitives shimmed |
| `runtime/gpu` | ~6,300 | D3D11-specific; Linux backend missing |
| `runtime/ui/overlay.cpp` | 1,400 | Direct2D/DirectWrite; Linux 2D layer missing |
| `runtime/audio/audio_out.cpp` | 93 | WASAPI replaced by SDL3 |
| `runtime/gamepads.cpp` | 367 | XInput/HID replaced by SDL3 |
| `launcher` | ~3,400 | Windows GUI; not needed to build/run the game |

---

## 3. Build system audit

### 3.1 Original state

The audit revision declares `project(acfa LANGUAGES C CXX RC)` and adds `.rc` files to every
executable. It also adds Windows defines globally, links `bcrypt` into `acfa_common`, links
`d3d11 dxgi d3dcompiler d2d1 dwrite ole32 psapi synchronization hid setupapi` into the game, and
builds `ACFA-Launcher` with `WIN32`, `-municode`, COM and WinHTTP. On Linux this fails immediately
at configure time: there is no `CMAKE_RC_COMPILER`.

The runtime target is already conditional on generated PPU code existing (`local/gen/ppu/*.cpp`),
which is the correct property for lawful distribution: the repository contains no game-derived
code; each user builds from their own disc.

### 3.2 Current build shape (this branch)

```
if(WIN32)  project(... LANGUAGES C CXX RC)   # unchanged Windows target graph
else()     project(... LANGUAGES C CXX)      # no .rc, no RC
```

- Windows keeps every link library and the launcher unchanged.
- Linux links `Threads::Threads` and `dl`; `bcrypt` is replaced by the in-tree SHA-1/SHA-256.
- `.rc` sources and `-municode` exist only on Windows.
- `ACFA_BUILD_LINUX_RUNTIME` (default OFF) builds the runtime; `ACFA_BUILD_TESTS` builds portable
  host tests; `ACFA_USE_SYSTEM_SDL3` selects an installed SDL3 instead of the pinned fetch.
- The Linux runtime excludes the D3D11/DXGI files and adds `window_sdl.cpp`, `display_clock.cpp`,
  `device_stub.cpp`, `renderer_stub.cpp` and `overlay_stub.cpp`. The exclusions disappear as the
  Vulkan backend replaces the placeholders.

### 3.3 Verified Linux build

On Ubuntu 22.04 / GCC 11.4 / CMake 4.3 / Ninja, with the stock tree plus this branch:

```
cmake -S . -B /tmp/acfa-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DACFA_BUILD_LINUX_RUNTIME=ON -DACFA_BUILD_TESTS=ON
cmake --build /tmp/acfa-linux -j8          # exit 0 (492 objects incl. SDL3)
ctest --test-dir /tmp/acfa-linux           # 1/1 passed
```

Produced: `acfa-prep`, `acfa-recomp`, `acfa-spurecomp`, `acfa-host-tests`, `acfa` (Linux ELF).
The runtime was linked with a **synthetic, non-game PPU function table** in the git-ignored
`local/gen/ppu/` directory solely to prove that every runtime symbol resolves. A real build
requires the user's own recompiled data.

Run evidence:

```
$ ./acfa --data /tmp/acfa-data
[   0.0096 W ... vm] guest write tracking unavailable; textures are re-checked by hashing
[   0.0101 E ... main] load failed: cannot open /tmp/acfa-data/game/BLUS30187/eboot.elf
```

That is the expected stop without game data: guest memory reservation, platform init, logging and
error handling work. It is **not** evidence that any game code runs.

### 3.4 Windows validation

The pinned llvm-mingw 20260908 toolchain from `data/toolchain.txt` was downloaded and used for a
full Windows build. `acfa.exe`, `acfa-prep.exe`, `acfa-recomp.exe`, `acfa-spurecomp.exe`,
`ACFA-Launcher.exe` and `acfa-host-tests.exe` build; the host tests pass on Windows; `acfa.exe`
starts, writes settings and a run log, and stops at the expected missing-`eboot.elf` error. The
fixes required (a missing Windows include in `save_data.cpp`, a volatile-qualified wake pointer,
a missing Windows include in `rsx_core.cpp`, and excluding the Linux overlay stub from the Windows
glob) are recorded in `docs/BUILD_LOG.md` entry 11. Real game data and the D3D11 renderer with real
shaders remain untested.

---

## 4. Already-portable core

Verified by inspection and Linux compilation:

- Byte helpers, SFO, known-input hashes, settings model, INI parser.
- PPU decoder and instruction semantics (SSE4.1/AVX2/FMA intrinsics, x86-64).
- SPU ISA, chunk discovery, interpreter, channels, local-store images.
- Recompiler: ELF image, analysis, emitter (deterministic output).
- SPU recompiler.
- Preparation: import, PRX relocation, SFO, SPU records.
- ELF loader, guest backtrace/printf, HLE registry/dispatch.
- LV2 mutex, cond, semaphore, rwlock, event queue/port, event flags, memory.
- Guest VM allocator bookkeeping and page bitmap (only the OS mapping changed).
- Reservation stripes (x86 pause).
- MPEG-2 decoder and `cellVdec` HLE.
- `cellPamf`, `cellSail`, `cellAtrac`: HLE stubs; the real implementations are LLE firmware
  modules and are portable.
- Save-data layout and transactional commit (`std::filesystem`); only mtime lookup is host-specific.
- Most game hooks (hard-coded guest addresses, no OS calls).
- Shader text generation (`vp_hlsl.cpp`, `fp_hlsl.cpp`) — pure string emission.

Caveats carried into the port:

- **Little-endian host is assumed** by every guest accessor (`__builtin_bswap*` after a raw load).
  Correct on x86-64 Linux and Steam Deck; not portable to big-endian.
- **x86-64 is assumed** by `ppu_recomp.h`, `spu_ops.h`, `reservation.h` and `spu_interp.cpp`
  (SSE/AVX, `_mm_pause`, MXCSR). Fine for SteamOS; an ARM port would need a vector abstraction.
- **Case sensitivity:** mount prefixes are matched case-insensitively (preserved), but guest path
  components are appended verbatim. A case-sensitive Linux filesystem will not find a file whose
  spelling differs from the disc tree. This is documented and must be tested with real data.
- `vfs::utime` ignores timestamps; `sys_fs_fsync` does not flush. Unchanged from Windows.

---

## 5. Windows-only inventory and replacements

Classification: **B** small shim, **C** Linux implementation behind an existing interface,
**D** Windows-only/replaced.

### 5.1 Runtime core

| File | Dependency | Linux replacement | State |
| --- | --- | --- | --- |
| `memory.cpp` | `VirtualAlloc/Free`, `MEM_WRITE_WATCH`, `GetWriteWatch` | `mmap(MAP_FIXED_NOREPLACE, PROT_NONE)`, `mprotect`, `madvise` | **done**; write watch intentionally reports unavailable (hash fallback) |
| `ppu_runtime.cpp` | VEH, `EXCEPTION_*`, `Rip`, `WaitOnAddress`, `WakeByAddressAll`, QPC, `VirtualAlloc` | `sigaction` + `ucontext`, futex, `clock_gettime`, `mmap` | **done** |
| `ppu_thread.cpp` | `CreateThread`, `SetThreadDescription`, `ExitProcess` | `std::thread`/pthread, `pthread_setname_np`, `_exit` | **done** |
| `log.cpp` | `GetCurrentThreadId`, `TerminateProcess` | `gettid`, `_exit` | **done** |
| `vfs.cpp` | `CreateFileW`, `ReadFile`, `WriteFile`, `SetFilePointerEx`, `GetFileInformationByHandle`, `GetFileAttributesExW`, `SetFileInformationByHandle`, `MoveFileExW`, `RemoveDirectoryW`, `DeleteFileW` | `open`, `read`, `write`, `pread`, `pwrite`, `lseek`, `fstat`, `stat`, `ftruncate`, `rename`, `rmdir`, `unlink` | **done** |
| `lv2/thread.cpp` | QPC, high-resolution waitable timer, `Sleep`, `SwitchToThread`, time zone, `FILETIME` | `clock_gettime`, `nanosleep`, `sched_yield`, `localtime_r`, `clock_gettime(CLOCK_REALTIME)` | **done** |
| `lv2/spu.cpp` | `CreateThread`, `WaitForSingleObject`, `CloseHandle`, `QueryThreadCycleTime` | `std::thread`, join, optional CPU-time counter | **done** (SPU thread cycle diagnostics disabled) |
| `rsx/rsx_core.cpp` | thread names, `OpenThread`, `WaitOnAddress`, `WakeByAddressAll`, `Sleep(0)` | `pthread_setname_np`, profile no-op, futex wait/wake, `sched_yield` | **done** |
| `spu/spu_thread.cpp` | `Sleep`, `SwitchToThread` | `platform::sleep_ms`, `yield_cpu` | **done** |
| `hle/cellRtc.cpp`, `cellSysutil.cpp` | `GetTimeZoneInformation`, `FILETIME` | `localtime_r`, `tm_gmtoff`, `clock_gettime` | **done** (original minute-based units preserved) |
| `hle/save_data.cpp` | `GetFileAttributesExW` | `stat` | **done** |
| `hle/cellAudio.cpp` | `SetThreadDescription` | `pthread_setname_np` | **done** |
| `hle/sysPrxForUser.cpp` | `SwitchToThread` | `sched_yield` | **done** |
| `hle/cellGcmSys.cpp` | unused `<windows.h>` | removed | **done** |
| `main.cpp` | affinity, priority, psapi/`GetSystemTimes`, `gmtime_s`, `ExitProcess` | `sched_setaffinity`, `setpriority`, `/proc`, `gmtime_r`, `_exit` | **done** |
| `input.cpp` | raw-input flags, `GetSystemMetrics` | neutral SDL motion/button/wheel events; Windows path kept | **done** |
| `bindings.cpp` | `<windows.h>` only for `VK_*` | `runtime/keycodes.h`, identical numeric values | **done** |
| `sysdialog.cpp` | `MultiByteToWideChar`, `localtime_s`, XInput, `GetDoubleClickTime`, VK | in-tree UTF-8→UTF-16, `localtime_r`, neutral pad struct, 500 ms constant | **done** |
| `gamepads.{h,cpp}` | XInput DLL + HID/SetupAPI, manually parsed DS4/DualSense reports | SDL3 Gamepad API, same `XINPUT_GAMEPAD` layout | **done** |
| `audio/audio_out.cpp` | WASAPI shared mode | SDL3 audio stream (48 kHz float stereo, 100 ms queue) | **done** |
| `game/input_hooks.cpp` | QPC | monotonic microseconds | **done** |
| `game/menu_mouse.cpp` | `GetCursorPos`/`ScreenToClient` | `gpu::window_cursor_position` (SDL backend) | **done** |
| `ui/overlay.cpp` | D3D11 + Direct2D + DirectWrite + system fonts | Vulkan 2D layer + redistributable font | **missing** (Linux stub cannot draw PC SETTINGS) |
| `gpu/window.cpp` | Win32 window/message loop/raw input | SDL3 window/events | **done** (`window_sdl.cpp`) |
| `gpu/display.cpp` | DXGI vblank, mode enumeration, frame statistics | monotonic clock pacing + SDL display modes | **done** (clock-only; present statistics reduced) |
| `gpu/d3d11_device.cpp` | D3D11/DXGI device, swap chain, HLSL compiler, captures | Vulkan device/swapchain, DXC→SPIR-V | **missing** (stub) |
| `gpu/renderer.cpp` | D3D11 RSX renderer | Vulkan RSX backend | **missing** (stub accepts commands, presents nothing) |
| `gpu/submit.cpp` | D3D11 command recorder | Vulkan command recording | **missing** |
| `gpu/rsx_texture.cpp` | `DXGI_FORMAT` in `TexData` | neutral format enum → `VkFormat` | **not ported** (excluded) |
| `gpu/shader_warm.cpp` | `D3DCompile` | DXC `-spirv` | **not ported** (stub reports unavailable) |
| `gpu/png.cpp` | portable | keep | excluded with the renderer |
| `launcher/**` | Win32 GUI/COM/WinHTTP/registry | Linux setup CLI + docs (not yet written) | **missing** (not needed to build) |

### 5.2 Consolidated mappings

| Windows facility | Linux replacement | Risk |
| --- | --- | --- |
| Fixed 4 GiB reservation | `mmap` at `0x20000000000` | low; address is free on x86-64 |
| Write watch | none yet; hash fallback | **high for performance** |
| Vectored exceptions | `sigaction` + `ucontext` (`REG_RIP`, `si_addr`) | medium: handler is diagnostic-only and uses non-async-signal-safe helpers |
| `WaitOnAddress`/`WakeByAddressAll` | futex private wait/wake | done |
| `CreateThread` + 64 MiB guest/host stacks | `std::thread` (default host stack) | medium: host stack size differs from Windows reservation; guest stack is separate |
| File I/O | POSIX `open`/`pread`/`pwrite`/`stat` | done; case sensitivity remains |
| XInput/HID | SDL3 Gamepad | done; raw report parser unused on Linux |
| WASAPI | SDL3 audio stream | done; device changes are not re-opened at runtime yet |
| DXGI vblank | monotonic clock | low; no display-locked pacing yet |
| D3D11 | Vulkan | **XL** |
| Direct2D/DirectWrite | Vulkan 2D + FreeType/stb_truetype + OFL font | **L** |
| CNG | in-tree SHA-1/256 | done, tested |

---

## 6. PPU and SPU toolchain audit

The pipeline is unchanged by the port:

1. `acfa-prep import` reads the user's decrypted disc files and firmware, relocates PRX modules,
   rebuilds SPU code records and writes `local/game/BLUS30187`.
2. `acfa-recomp analyze|emit` parses the ELF, performs reachability analysis, resolves indirect
   jumps and emits one C++ function per guest function plus a function table.
3. `acfa-spurecomp` translates SPU records into C++.
4. The runtime table drives `ppu::icall`; unknown addresses are fatal.

Contract that must stay exact: `PPU_MEM_BASE = 0x0000_2000_0000_0000`; 32 GPR/FPR/VMX registers;
CR/XER; 128-byte reservation lines; `icall`, `syscall`, `trap`, `bad_switch`, `timebase`,
`read_spr`, `write_spr`; guest setjmp/longjmp via `GuestLongjmp`.

Linux verification: all three tools build and print their usage. No recompilation of real game
code was performed (no lawful game data in this environment), so **byte-identical output between
Windows and Linux is not yet demonstrated**.

---

## 7. Runtime architecture notes for the port

- **Guest memory:** 4 GiB reserved once; commit/decommit map to `mprotect` plus a 4 GiB page
  bitmap. Allocation areas and guard pages are unchanged.
- **Threads:** one host thread per PPU/SPU guest thread. Priority handoff is emulated with a
  condition variable (20 ms wait for a higher-priority new thread to block) exactly as on Windows.
- **Reservations:** striped 128-byte line locks; `lwarx/stwcx` compare the whole line; futex
  wakeups replace `WakeByAddress*`.
- **Signals:** SIGSEGV/SIGBUS/SIGILL/SIGFPE on an alternate stack report host RIP, fault address,
  the nearest recompiled function and the executing guest thread. The handler writes to stderr and
  exits; it cannot safely dump the full guest context like the Windows VEH path yet.
- **VFS:** mount table (`/dev_bdvd`, `/app_home`, `/dev_hdd0`, `/dev_hdd1`, `/dev_flash`,
  save-data) with the same longest-prefix resolution and protected-root guard. POSIX paths are
  compared case-sensitively and use `/`.
- **Saves:** unchanged `PARAM.SFO` + files, transactional directory swap. On Linux settings and
  saves default to `$XDG_DATA_HOME/acfa` (or `~/.local/share/acfa`); `--data`/`--user` still work
  exactly as before. Verified: a run with `XDG_DATA_HOME=/tmp/xdg` wrote
  `/tmp/xdg/acfa/settings.ini`.
- **Audio:** `cellAudio` business logic unchanged; SDL3 consumes 48 kHz float stereo frames.
- **Video:** `cellVdec` + MPEG-2 decoder unchanged; `cellPamf`/`cellSail`/`cellAtrac` remain LLE
  firmware paths. Movie pacing still forces ≤60 Hz through `display`.
- **Input:** Windows VK numbers remain the binding format; SDL keycodes are translated 1:1. Pads
  are XInput-shaped, so `sys_io`, dialogs and the overlay keep their existing code.
- **Settings:** the same `settings.ini` schema; monitor identity changes from `\\.\DISPLAYn` to
  SDL display names (migration needed for Windows-written files).

---

## 8. Graphics audit and Vulkan plan

### 8.1 Current architecture

```
guest FIFO thread
  -> renderer_method / renderer_transform_constants
  -> flush_draw: targets, shaders, textures, vertices, state, constants
  -> gpu::rec() recorder (D3D11-shaped calls)
  -> submit thread executes ID3D11DeviceContext calls
  -> present_buffer -> present_texture -> Present
```

- 222 D3D11 call sites in `gpu/`; 131 in `renderer.cpp`.
- Surfaces keyed by `(location, offset)`, storage size = game size × render scale, format map to
  DXGI, depth/stencil views, feedback copies, depth-as-colour images.
- Textures decoded from RSX registers into upload formats (DXT, swizzled/Morton, remap), cached by
  content hash and guest page-dirty state.
- Vertex data is uploaded as raw big-endian guest bytes and decoded **in the vertex shader** from
  four `ByteAddressBuffer`s; there is no input layout.
- Four constant buffers: VS `b0/b1/b2` (`vc[512]`, viewport state, vertex format), PS `b2/b3`
  (`FpState`, `fc[256]`).
- Shaders: generated HLSL, compiled with `D3DCompile` to `vs_5_0`/`ps_5_0`, cached as DXBC.
- Presentation: aspect-fit blit, black clear, optional PC SETTINGS overlay, captures.
- Occlusion: D3D11 occlusion queries; conditional draws are decided on the CPU.

### 8.2 What a Vulkan backend must reproduce

Device/swapchain, shader compilation, resource creation (2D/3D textures, RTV/DSV/SRV/UAV, raw and
dynamic buffers, samplers, blend/depth/raster state, queries, staging), the recorded command set
(render targets, states, viewports/scissors, shaders, constant buffers, SRVs, samplers, draws,
clears, copies, updates, releases, immediate callbacks), presentation and display timing.

### 8.3 Shader path: concrete blockers

1. **Binding collisions.** Vulkan shares one binding number per set across descriptor types and
   stages; the generated HLSL reuses `t0..t3`, `b0..b3`, `s0..s15` and uses `t0..t3` as
   `ByteAddressBuffer` in the VS and `Texture2D` in the PS. DXC must be invoked with
   `-fvk-b-shift`, `-fvk-t-shift`, `-fvk-s-shift` (or the generator must emit `[[vk::binding]]`),
   and a reflection test must assert no `(set,binding)` has two types.
2. **`Load4` requires SM 6.2.** Target `vs_6_2`/`ps_6_2`; verify the chosen DXC build supports it
   in the SPIR-V backend.
3. **Y convention.** The VS bakes D3D's y-up NDC. Vulkan is y-down. Use a negative-height viewport
   and flip `frontFace`, or add a backend epilogue to the generator. This must be proven with a
   synthetic triangle before any game rendering.
4. **Out-of-range raw reads.** D3D returns 0 past a bound range; Vulkan storage-buffer reads are
   out of bounds. Pad every bound range by at least 32 bytes or clamp in the shader.
5. **No `WRITE_NO_OVERWRITE`/`WRITE_DISCARD`.** Replace the ring with per-frame slots and fences.
6. **`call()` escape hatch.** Six sites record lambdas against an immediate context. Replace with
   explicit fullscreen-pass/copy/query operations in the backend interface.
7. **Formats.** `R32G32B32A32_SFLOAT` and `R16` colour attachments are optional; query
   `VkFormatProperties` and log a fallback.
8. **Write tracking.** Without `GetWriteWatch`, textures are hashed every frame. Correct but slow;
   implement `mprotect`/soft-dirty tracking before performance claims.

### 8.4 Staged plan and gates

**Verified foundation (2026-10-31).** `acfa-vk-smoke` creates a Vulkan instance and device, allocates
an offscreen image, clears it, copies it to a host-visible buffer and verifies the pixels. It ran
successfully on Mesa lavapipe (`llvmpipe`, Vulkan 1.3.255) and is a CTest test when
`-DACFA_BUILD_TESTS=ON`. This covers device/queue/image/memory/command-buffer/readback only — not a
swapchain, shaders or the game renderer.

| Stage | Work | Gate (no game data required) |
| --- | --- | --- |
| 0 | Freeze a backend interface; remove `DXGI_FORMAT`/D3D types from shared structs | D3D11 still builds; descriptor hashes stable |
| 1 | Vulkan instance/device/swapchain, SDL surface, present blit | animated test pattern at 1280×800, resize, borderless toggle |
| 2 | Buffers, constant updates, one texture format, one draw, shifted bindings | synthetic triangle/quad orientation matches reference |
| 3 | Pipeline state mapping (blend/depth/raster/scissor) | synthetic state matrix vs CPU reference |
| 4 | Surfaces, clears, render-scale recreation, feedback copies | offscreen scene at 50/100/200 %, content preserved |
| 5 | Textures and samplers | synthetic swizzled/DXT/float textures vs CPU converter |
| 6 | Full draw path, MRT, depth-as-colour, occlusion | synthetic multi-target scene + query counts |
| 7 | Vulkan 2D overlay + redistributable font | settings/dialog walkthrough screenshots |
| 8 | Performance: dirty tracking, pacing, present wait | 1280×800/60 fps measurement on Deck |

Stages 0–2 alone do not make the game renderable; they are the prerequisite for every later
claim. Until stage 6 passes, this project will not publish an AppImage.

---

## 9. Comparison with the AC4 Steam Deck fork

Audited tree: `omegakatana92/AC4-Port-Steam-Deck_Edition`, `linux-native-port` @ `64d7c28`.

- **Committed:** only `docs/LINUX_PORT_PLAN.md` and `docs/BUILD_LOG.md`. There is no committed
  Linux code, so there is no working implementation to copy.
- **Uncommitted working tree:** AC4 has a Phase 2 tools/tests port (CMake split, portable SHA,
  tests, shell scripts, MinGW toolchain file, CI). ACFA independently has the same build-system
  changes plus a runtime platform layer; the hash/path/INI/privacy changes are equivalent but not
  identical in file layout.
- **Identical or near-identical runtime code:** `cellVdec`, `cellPamf`, `cellSail`, `cellAtrac`,
  `media/mpeg2_video`, `spu/*`, `rsx/rsx.h`, `rsx/methods.h`, `rsx/rsx_internal.h`,
  `rsx/rsx_methods.cpp`, `input.*`, `gamepads.*`, `sysdialog.*`, `audio/audio_out.*`,
  `memory.h`, `ppu_runtime.h`, `ppu_thread.h`, `common/settings_file.*` (AC4 drops `[game]`).
  The movie stack is byte-identical, so Linux work there can be shared after review.
- **Divergent and must not be shared blindly:**
  - `renderer.cpp`: ACFA has the constant-depth viewport/`zconst` path; AC4 removes it and adds
    dynamic movie textures, depth-copy throttling, `written_plain` and portable texture permute.
  - `d3d11_device.cpp`/`display.cpp`/`window.cpp`: AC4 adds exclusive-mode sizing, numpad key
    normalisation and different vblank handling; ACFA has movie pacing and early-vblank guards.
  - `hle/cellGcmSys.cpp`: AC4 adds an SDK 1.x flip entry point (`cellGcmSetFlip`/`WaitFlip`);
    ACFA uses inline flips.
  - `game/frame_policy.cpp`: different gameplay/movie detection.
  - `ui/overlay.cpp`: ACFA has a GAME page and an install-question state; AC4 does not.
  - AC4-only: `cellL10n`, `cellResc`, `system_menu`, `frame_hooks`, `lua_hooks`, `native_hooks`.
  - ACFA-only: `pc_settings_entry`, `script_fixes`.
- **Conclusion:** share the platform layer, SDL3 backends, shader generator, movie/audio/SPU code
  and tests **as separate copies** after diffing; never share `renderer.cpp` without reconciling
  the two feature sets. Keep both games independently buildable.

---

## 10. Linux paths, saves and Steam Deck behaviour

- Data layout stays `--data DIR` (`game/BLUS30187`, `hdd0`, `hdd1`, `runs`, caches); `--user DIR`
  places `settings.ini` and `savedata` in a writable location. The recommended Linux location is
  `$XDG_DATA_HOME/acfa` or `~/.local/share/acfa`, with game data on the same writable volume.
- Saves are unencrypted directories in the console layout; PS3/RPCS3 saves work after copying.
- Steam Deck: 1280×800 borderless, 30/60/120 fps targets, render scale 50–200 %, SDL3 gamepad
  (Steam Deck controls appear as a standard gamepad in Gaming Mode; Desktop Mode may present Steam
  Input's virtual pad). Analog sticks/triggers use XInput ranges; rumble uses SDL.
- Audio via SDL3 (PipeWire/PulseAudio/ALSA on SteamOS).
- Movies remain 60 Hz-capped; missions use the configured rate.
- Input: Steam Deck's built-in controls, Bluetooth/USB controllers; mouse/keyboard remain for
  Desktop Mode.

---

## 11. AppImage plan (not started)

Only after the Vulkan renderer and a real game run are verified:

- `ArmoredCoreForAnswer-SteamDeck-x86_64.AppImage` with `AppRun`, `.desktop`, icon.
- Bundle SDL3 (if not using the system copy), libstdc++ only if the target baseline requires it,
  and the OFL font used by the overlay. Do **not** bundle Mesa/AMD/NVIDIA drivers or the Vulkan
  loader; use the system's.
- AppRun resolves the writable game-data directory (`$XDG_DATA_HOME/acfa`, or a user-selected
  folder), sets `LD_LIBRARY_PATH` to the bundle only, and launches `acfa`.
- Test in Desktop Mode and Gaming Mode on SteamOS; verify audio, controller, saves, fullscreen,
  suspend/resume and a mission load.
- No game data, firmware, decrypted executables or keys in the AppImage or release assets.

---

## 12. Known limitations and risks

1. **No renderer.** The Linux binary accepts RSX commands and presents nothing. This is the
   dominant blocker for a playable port.
2. **No write tracking.** Texture updates hash guest memory; performance is unknown and likely
   insufficient on Deck until dirty tracking lands.
3. **No PC SETTINGS drawing.** The Linux overlay stub cannot open/draw the settings screen; system
   dialogs are operable only by keyboard/controller and are not drawn.
4. **No Windows build validation** on this host.
5. **No game-data run.** Boot, HLE coverage and timing are unverified against the real game.
6. **Signal handler** is less informative than the Windows VEH path.
7. **Host stack size** for PPU threads differs from the Windows 64 MiB reservation; deep guest
   call chains must be tested.
8. **Case-sensitive guest paths** may expose disc-tree spelling mismatches.
9. **Monitor identity migration** from `\\.\DISPLAYn` is not implemented.
10. **AppImage/release** intentionally not produced.

---

## 13. Phase status

| Phase | Status |
| --- | --- |
| 1 — Audit and this document | **complete** |
| 2 — AC4 comparison | **complete** (documented; no code copied) |
| 3 — Linux build system and tools | **complete and verified**; Windows build also verified with the pinned toolchain |
| 4 — Native Linux runtime | **core compiles and starts**; game-data run untested; input/audio/window implemented; settings overlay missing |
| 5 — Native graphics | **foundation only**: verified headless Vulkan clear/readback; no swapchain, shaders or game rendering |
| 6 — Steam Deck integration | **not started**; prerequisites not met |
| 7 — AppImage | **not started**; must not start before Phase 5/6 verification |
| 8 — GitHub release | **not started** |

The next milestone is Stage 1 of §8.4: a Vulkan swapchain and test-pattern presentation. The
headless clear/readback gate already passes; no packaging work will be published before the
renderer and a real game run are verified.

Companion documents: `docs/VULKAN_RENDERER.md` (backend design and gates), `docs/WRITE_TRACKING.md`
(dirty-tracking options), `docs/OVERLAY.md` (PC SETTINGS drawing), and
`docs/STEAM_DECK_AND_APPIMAGE.md` (hardware matrix and packaging contract).
