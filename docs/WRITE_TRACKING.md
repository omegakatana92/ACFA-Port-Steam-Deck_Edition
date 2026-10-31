# Linux guest-memory write tracking — options and plan

Status: **not implemented**. On Windows, `memory.cpp` reserves the 4 GiB guest address space with
`MEM_WRITE_WATCH` and asks `GetWriteWatch` which pages changed. The RSX texture cache uses that to
avoid re-decoding and re-uploading textures that did not change. On Linux,
`platform::guest_reserve` reports that write tracking is unavailable
(`src/runtime/platform.cpp`, POSIX branch), and `platform::take_written_pages` always returns
`false`. The renderer would therefore fall back to hashing every bound texture every frame.

This document compares the implementation options, states the correctness requirements and defines
the benchmark gate. It does not claim any implementation exists.

---

## 1. Why it matters

Textures are decoded from guest memory into GPU formats (DXT, swizzled/Morton, channel remap). A
single frame can bind hundreds of textures; some are large. Re-hashing them every frame is
`O(texture bytes)` of CPU work and cache traffic on top of the decode. On a Steam Deck, memory
bandwidth and CPU time are the scarce resources, so this can turn a playable frame into a stall.

Correctness matters more than speed:

- A missed write means a stale texture (wrong image).
- A false positive means an unnecessary decode (slow but correct).
- Page granularity is 4 KiB; writes to any byte mark the page dirty.
- Guest writes come from PPU threads, SPU DMA, and host-side transfers.
- The guest fault handler already uses signals; any new signal-based tracking must not conflict.

---

## 2. Options

### 2.1 Optimised hashing (current fallback)

Hash only textures that are actually bound this frame, cache the hash, and re-hash at a bounded
interval. Existing `renderer.cpp` already has a hashing fallback.

**Pros:** trivial, portable, no privileges, no signal handlers, no kernel features.
**Cons:** still reads every bound texture's bytes; large scenes remain expensive.
**Use:** baseline and safety net for every other option.

### 2.2 `/proc/self/pagemap` soft-dirty

The kernel maintains a soft-dirty bit per page (bit 55). Clear it by writing `4` to
`/proc/self/clear_refs`, then read `pagemap` for the watched ranges.

**Pros:** no fault overhead; can scan only watched pages; works for all writers.
**Cons:**
- `clear_refs` is **process-wide**, so clearing affects every watcher and must be serialised.
- Scanning a 4 GiB range is expensive; only scan texture/surface ranges.
- `pagemap` access rules vary by kernel; soft-dirty must be enabled.
- The bit is a hint: it can report pages dirty that were only read, and needs testing on SteamOS.
- Concurrent clearing while other threads write can lose a dirty page between clear and scan; the
  protocol must quiesce or accept a conservative rescan.

**Verdict:** best fallback if `userfaultfd` is unavailable, but needs a carefully serialised
collector and a benchmark.

### 2.3 `userfaultfd` write-protect

Register watched ranges, write-protect them, and handle `UFFD_EVENT_PAGEFAULT` with `UFFD_PAGEFAULT_
FLAG_WP`; record the page dirty and remove the write-protect.

**Pros:** precise write faults; no scanning; can cover large ranges cheaply.
**Cons:**
- Requires `vm.unprivileged_userfaultfd=1` or `CAP_SYS_PTRACE`; SteamOS defaults may forbid it.
- A monitor thread and per-range registration are needed.
- Fault latency is high; textures written every frame would fault constantly.
- Interaction with the guest SIGSEGV handler must be defined (different signal, but both on the
  same threads).

**Verdict:** implement only if the Deck configuration allows it and measurements justify it.

### 2.4 `mprotect(PROT_READ)` + `SIGSEGV`

Remove write permission from watched pages. On a write fault, the SIGSEGV handler marks the page
dirty, restores write permission and returns.

**Pros:** no privileges; precise; works on any Linux.
**Cons:**
- Faults are expensive; a texture written per frame faults every frame.
- The handler must be async-signal-safe and must distinguish guest faults from tracking faults.
- Guest code may write to protected pages from any thread; the handler must update a lock-free
  bitmap.
- Restoring protection per page causes TLB shootdowns.

**Verdict:** viable for large, rarely written surfaces; not for hot textures. It also conflicts
conceptually with the existing guest fault handler, which treats SIGSEGV as fatal.

### 2.5 Hybrid (recommended)

1. Keep hashing as the correctness fallback.
2. Add a **watched-range registry** with page bitmaps (per texture/surface).
3. Use soft-dirty for large, rarely written ranges if the benchmark shows a win.
4. Use explicit invalidation where the runtime already knows about writes: `renderer_memory_written`,
   transfer-engine writes, and surface writes. This is free and covers the common cases.
5. Re-hash only on a miss or on an interval, not every frame.

The cheapest large win is often **explicit invalidation**, because the runtime already knows when
a transfer engine or the CPU writes guest memory. Use that before any kernel mechanism.

---

## 3. Correctness contract

- A texture is re-decoded if any byte of its guest range changed since the last decode.
- A surface is re-read if any byte changed, except where the renderer itself wrote it (feedback).
- Writes from all PPU threads, SPU DMA and host transfers count.
- The dirty set is per-frame and may be conservative (extra dirty pages are allowed).
- No dirty information may be lost between a write and the next texture validation without a
  rescan; if in doubt, report the whole range dirty.
- The implementation must be tested with a multi-threaded writer and a reader that validates
  concurrently.

---

## 4. API shape

Keep the existing `vm::take_written_pages` contract and add explicit invalidation:

```
bool take_written_pages(u32 begin, u32 size, std::vector<u32>& pages);  // current
void note_written(u32 ea, u32 size);                                   // new, cheap
```

`note_written` is called by:
- `renderer_memory_written` (already exists),
- transfer-engine writes in `rsx_methods.cpp`/`rsx_core.cpp`,
- any HLE path that writes guest memory through a helper (audit `memcpy` sites),
- the renderer when it writes a surface it also samples.

Then `take_written_pages` can combine explicit notes with the kernel mechanism.

---

## 5. Benchmark gate

No renderer exists yet, so this is a plan. Once G5/G6 of `docs/VULKAN_RENDERER.md` pass:

1. Instrument per-frame texture validation time (existing `RenderStats` has `tex_hashed`,
   `tex_uploaded`).
2. Measure with:
   - hashing every bound texture,
   - hashing with explicit invalidation,
   - soft-dirty (if available),
   - userfaultfd/mprotect if enabled.
3. Use a real mission scene on Steam Deck at 1280×800, 60 fps, with the frame limiter off.
4. Acceptance: validation CPU time below 1 ms/frame at 60 fps and no visual regressions over a
   10-minute mission.
5. If no option reaches the budget, keep hashing and optimise the decode/cache path instead; do not
   ship a fragile mechanism for a marginal gain.

Record results in `docs/BUILD_LOG.md` or a new `docs/PERFORMANCE.md`.

---

## 6. SteamOS notes

- SteamOS is immutable; the AppImage cannot install kernel modules or change sysctls.
- `userfaultfd` availability must be probed, not assumed.
- `pagemap` may be restricted; probe and fall back.
- Do not require root.
- The tracking mechanism must survive suspend/resume (re-probe after resume).

---

## 7. Current state

- Windows: `MEM_WRITE_WATCH`/`GetWriteWatch` works and is used.
- Linux: not implemented; `take_written_pages` returns false; hashing is the only path.
- No benchmark has been run because there is no renderer.
- This document defines the work; it does not claim any of it is done.
