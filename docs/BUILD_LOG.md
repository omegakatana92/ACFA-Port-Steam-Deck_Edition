# Build log — Linux port

Every build or link error encountered while porting, its cause and the fix. Entries are appended,
never rewritten. A later fix supersedes an earlier one only by a new entry.

Format: `## N. <date> — <target> — <platform/toolchain>`, then `Command`, `Error`, `Cause`,
`Fix`, `Verified`.

---

## 1. 2026-10-31 — configure — Ubuntu 22.04 / GCC 11.4 / CMake 4.3

**Command**

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

**Error**

```
CMake Error at CMakeLists.txt:2 (project):
  No CMAKE_RC_COMPILER could be found.
```

**Cause**

`project(acfa LANGUAGES C CXX RC)` and `.rc` sources are Windows-only.

**Fix**

Select `RC` and add `.rc` sources only on `WIN32`; Linux declares `C CXX`.

**Verified**

Configure succeeds. Windows keeps the original target graph.

---

## 2. 2026-10-31 — `acfa_common` link — Ubuntu 22.04 / GCC 11.4

**Error**

```
/usr/bin/ld: cannot find -lbcrypt
```

**Cause**

`acfa_common` links `bcrypt` unconditionally; CNG does not exist on Linux.

**Fix**

Keep BCrypt on Windows; implement SHA-1/SHA-256 in `common/crypto.cpp` for POSIX. No OpenSSL or
other new dependency. Host tests cover empty input, `"abc"` and one million bytes against NIST
vectors.

**Verified**

`ctest` passes; `sha256_file_hex` used by `acfa-prep` works on Linux.

---

## 3. 2026-10-31 — common paths/INI — Ubuntu 22.04 / GCC 11.4

**Error**

Compilation of `common/file.cpp`, `common/ini.cpp`, `common/privacy.cpp` failed on
`<windows.h>`, `MoveFileExW`, `GetEnvironmentVariableW`, `towlower`/wide paths.

**Cause**

Windows path normalisation, atomic replace and environment lookup.

**Fix**

POSIX branches: `generic` path spelling, case-sensitive protected-root comparison (a differently
cased symlink must not bypass the guard), `rename(2)`, `getenv("HOME"/"USER")`.

**Verified**

Host tests for the protected-root guard (including the sibling-prefix case) and INI save/load.

---

## 4. 2026-10-31 — runtime compile — Ubuntu 22.04 / GCC 11.4

**Error**

```
fatal error: windows.h: No such file or directory
```

in the runtime core, HLE, LV2, RSX and input files.

**Cause**

Each file used Win32 APIs directly.

**Fix**

Add `runtime/platform.{h,cpp}` and port the core primitives (time, futex waits, thread names,
affinity/priority, guest `mmap` commit/decommit, `/proc` stats, XDG paths, `_exit`) plus POSIX
branches in VFS, PPU/SPU threads, LV2 time, RSX FIFO, save-data mtime and time zones. Windows
paths stay behind `_WIN32`.

**Verified**

All Linux runtime translation units compile. `acfa` links and starts, reserves guest memory, and
stops cleanly with `cannot open .../eboot.elf` when no game data is present.

---

## 5. 2026-10-31 — `rsx_core.cpp` — Ubuntu 22.04 / GCC 11.4 / libstdc++ 11

**Error**

```
error: 'struct std::atomic<unsigned int>' has no member named 'wait_for'
error: 'Sleep' was not declared in this scope
```

**Cause**

libstdc++ 11 does not implement C++20 `atomic::wait`; `Sleep(0)` was Win32.

**Fix**

`platform::wait_atomic_u32` / `notify_all_atomic_u32` use futex on Linux and
`WaitOnAddress`/`WakeByAddressAll` on Windows; `Sleep(0)` becomes `sched_yield`.

**Verified**

Compiles and links; FIFO fragment waiting retains its 2 ms bounded poll.

---

## 6. 2026-10-31 — SDL3 configure — Ubuntu 22.04 / CMake 4.3 in WSL

**Error**

```
/usr/include/c++/11/bits/atomic_wait.h: error: 'syscall' was not declared in this scope
/mnt/c/msys64/mingw64/include/wchar.h: ... _errno was not declared
```

**Cause**

`find_package(SDL3 QUIET CONFIG)` found a **Windows** SDL3 package under `/mnt/c/msys64` and added
`/mnt/c/msys64/mingw64/include` to the Linux compile line. WSL exposes the Windows filesystem
through `/mnt`, so CMake's prefix search crossed platforms.

**Fix**

Fetch the pinned SDL3 3.4.18 source by default on Linux; an installed SDL3 is used only with
`-DACFA_USE_SYSTEM_SDL3=ON` and must be a Linux package.

**Verified**

Clean configure/build on Ubuntu 22.04 with SDL3 static, X11/Wayland/KMSDRM backends.

---

## 7. 2026-10-31 — `acfa` link (synthetic table) — Ubuntu 22.04 / GCC 11.4

**Error**

```
undefined reference to `ppu::g_funcs'
undefined reference to `ppu::g_func_count'
undefined reference to `f_00ede7e8(ppu::Context&)`
```

**Cause**

The link-check fixture in the git-ignored `local/gen/ppu/` declared `const` tables without a prior
`extern` declaration (internal linkage), and placed the game-hook stubs inside `namespace ppu`
while the emitter defines `f_XXXXXXXX` at global scope.

**Fix**

Add `extern` declarations before the definitions; move the hook stubs to the global namespace.
The fixture is not game-derived and is never committed or shipped.

**Verified**

`acfa` links (exit 0). A real build still needs the user's generated table.

---

## 8. 2026-10-31 — Windows build attempt — Windows 11 / LLVM 22 + MinGW Makefiles

**Error**

```
lld-link: error: duplicate resource: type MANIFEST ... in utf8.rc.res and acfa.exe.manifest.res
lld-link: error: undefined symbol: __cpu_indicator_init
```

**Cause**

The host has LLVM/Clang and MinGW-w64, but not the project's pinned llvm-mingw toolchain. The
mixed Clang/MSVC/MinGW environment produces unrelated manifest and compiler-rt link failures.

**Fix**

Not fixed in the source: this is an environment limitation. The Windows code paths were kept
behind `_WIN32`, and the changed common files compile under the Windows headers in a partial
build. **A full Windows build with `data/toolchain.txt` is still required before release.**

**Verified**

Not verified. Recorded as a blocker for the release checklist.

---

## 9. 2026-10-31 — Linux runtime run — Ubuntu 22.04 / GCC 11.4

**Command**

```bash
./acfa --data /tmp/acfa-data
```

**Result**

```
[0.0096 W vm] guest write tracking unavailable; textures are re-checked by hashing
[0.0101 E main] load failed: cannot open /tmp/acfa-data/game/BLUS30187/eboot.elf
```

**Interpretation**

Expected without the user's game data. It verifies platform init, the 4 GiB guest reservation,
logging, error handling and process exit. It does **not** verify guest execution.

**Verified**

Run performed; no game data present.

---

## 10. 2026-10-31 — `acfa-vk-smoke` — Ubuntu 22.04 / GCC 11.4 / Mesa lavapipe

**Command**

```bash
cmake -S . -B /tmp/acfa-linux ... -DACFA_BUILD_TESTS=ON
cmake --build /tmp/acfa-linux -j8
ctest --test-dir /tmp/acfa-linux --output-on-failure
```

**Result**

```
Vulkan device: llvmpipe (LLVM 15.0.7, 256 bits) (API 1.3.255)
clear/readback OK (64 x 64, first pixel 64,128,191,255)
2/2 tests passed
```

**Cause / purpose**

Stage 1 foundation check: instance, physical/logical device, queue, image, memory, command buffer,
clear, copy and readback. It deliberately does not create a swapchain or compile shaders yet.

**Fix**

None needed. The test is built only on Linux when `find_package(Vulkan)` succeeds.

**Verified**

Passed on llvmpipe. This is not GPU-driver testing and not game rendering; the Steam Deck's AMD
driver remains untested.

