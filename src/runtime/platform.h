// Host platform services used by the runtime. One implementation per platform in platform.cpp;
// the rest of the runtime calls these instead of Win32/POSIX APIs directly.
#pragma once
#include <atomic>
#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

#include "common/bytes.h"

namespace rt::platform {

// ---- time ----
u64 monotonic_us();  // monotonic clock, microseconds
u64 tick_ms();       // monotonic milliseconds (coarse)
void sleep_ms(u32 ms);
void sleep_us(u64 us);
void yield_cpu();
u64 realtime_ns();   // wall clock, nanoseconds since the Unix epoch
// Local UTC offset in seconds east of UTC, and whether daylight saving is in effect.
s32 utc_offset_seconds(bool* daylight);
// Broken-down UTC time (thread-safe).
void utc_tm(std::time_t t, std::tm* out);
// Broken-down local time (thread-safe).
void local_tm(std::time_t t, std::tm* out);

// ---- threads ----
u32 current_thread_id();
void set_current_thread_name(const char* name);
// Wait for an atomic 32-bit word to change from `expected`, up to `timeout_ms`; a waiter must
// re-check its condition after returning (spurious wake-ups are allowed).
void wait_atomic_u32(std::atomic<u32>* a, u32 expected, u32 timeout_ms);
void notify_all_atomic_u32(std::atomic<u32>* a);
u32 cpu_count();                       // logical CPUs available to this process
bool set_affinity_mask(u64 mask);      // false if unsupported or rejected
bool set_process_priority(const char* cls);  // "above-normal" | "high"

// ---- guest address space ----
// Reserves [addr, addr+size) with no access. On Windows this requests write tracking; `watching`
// reports whether it was granted. Linux has no MEM_WRITE_WATCH equivalent here, so `watching` is
// false and callers use their fallback (texture hashing).
bool guest_reserve(void* addr, size_t size, bool request_write_watch, bool* watching);
bool guest_commit(void* addr, size_t size);
void guest_decommit(void* addr, size_t size);
// Pages written since the last call for [addr, addr+size), relative to `base`. False when write
// tracking is unavailable (never an error, and never a reason to skip rendering).
bool take_written_pages(void* base, size_t offset, size_t size, std::vector<u32>& out);
// Read/write memory for host structures (the recompiled-function table).
void* alloc_rw(size_t size);
void free_rw(void* p, size_t size_num);

// ---- process statistics (perf log) ----
struct ProcessStats {
  u64 working_set = 0;   // bytes
  u64 page_faults = 0;   // since start
  u64 cpu_user_us = 0, cpu_kernel_us = 0;
  u64 system_idle_us = 0, system_total_us = 0;  // machine-wide, total includes idle
};
ProcessStats process_stats();

// ---- paths ----
std::string executable_dir();
// Per-user writable data directory: %LOCALAPPDATA%/<app> on Windows, $XDG_DATA_HOME/<app>
// (default ~/.local/share/<app>) on Linux.
std::string user_data_dir(const std::string& app);

// Terminates the process immediately after flushing stdio (no destructors).
[[noreturn]] void exit_process(int code);

}  // namespace rt::platform
