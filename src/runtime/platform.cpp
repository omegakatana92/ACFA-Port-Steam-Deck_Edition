#include "runtime/platform.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <fstream>

namespace rt::platform {
namespace {

u64 ft_us(const FILETIME& t) {
  u64 v = (u64(t.dwHighDateTime) << 32) | t.dwLowDateTime;  // 100 ns
  return v / 10;
}

std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

}  // namespace

u64 monotonic_us() {
  static LARGE_INTEGER f, t0;
  static bool init = [] {
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    return true;
  }();
  (void)init;
  LARGE_INTEGER n;
  QueryPerformanceCounter(&n);
  u64 d = u64(n.QuadPart - t0.QuadPart);
  return (d / u64(f.QuadPart)) * 1000000ull + (d % u64(f.QuadPart)) * 1000000ull / u64(f.QuadPart);
}

u64 tick_ms() { return GetTickCount64(); }

void sleep_ms(u32 ms) { Sleep(DWORD(ms)); }

void sleep_us(u64 us) {
  if (!us) {
    SwitchToThread();
    return;
  }
  Sleep(DWORD((us + 999) / 1000));
}

void yield_cpu() { SwitchToThread(); }

u64 realtime_ns() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  u64 t = (u64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // since 1601
  t -= 116444736000000000ull;                                 // since 1970
  return t * 100ull;
}

s32 utc_offset_seconds(bool* daylight) {
  TIME_ZONE_INFORMATION tzi{};
  DWORD r = GetTimeZoneInformation(&tzi);
  if (daylight) *daylight = r == TIME_ZONE_ID_DAYLIGHT;
  return -s32(tzi.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias : 0)) * 60;
}

void utc_tm(std::time_t t, std::tm* out) { gmtime_s(out, &t); }
void local_tm(std::time_t t, std::tm* out) { localtime_s(out, &t); }

u32 current_thread_id() { return u32(GetCurrentThreadId()); }

void set_current_thread_name(const char* name) {
  std::wstring w(name, name + std::strlen(name));
  SetThreadDescription(GetCurrentThread(), w.c_str());
}

void wait_atomic_u32(std::atomic<u32>* a, u32 expected, u32 timeout_ms) {
  WaitOnAddress(reinterpret_cast<volatile void*>(a), &expected, sizeof expected, timeout_ms);
}

void notify_all_atomic_u32(std::atomic<u32>* a) { WakeByAddressAll(reinterpret_cast<volatile void*>(a)); }

u32 cpu_count() {
  DWORD_PTR mask = 0, sys = 0;
  u32 n = GetProcessAffinityMask(GetCurrentProcess(), &mask, &sys) ? u32(__builtin_popcountll(u64(mask))) : 0;
  if (!n) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    n = si.dwNumberOfProcessors;
  }
  return n ? n : 1;
}

bool set_affinity_mask(u64 mask) { return SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(mask)) != 0; }

bool set_process_priority(const char* cls) {
  DWORD c = std::strcmp(cls, "high") == 0 ? HIGH_PRIORITY_CLASS
            : std::strcmp(cls, "above-normal") == 0 ? ABOVE_NORMAL_PRIORITY_CLASS
                                                    : 0;
  return c && SetPriorityClass(GetCurrentProcess(), c);
}

bool guest_reserve(void* addr, size_t size, bool request_write_watch, bool* watching) {
  void* p = VirtualAlloc(addr, size, MEM_RESERVE | (request_write_watch ? MEM_WRITE_WATCH : 0), PAGE_NOACCESS);
  bool watch = p == addr && request_write_watch;
  if (request_write_watch && p != addr) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    p = VirtualAlloc(addr, size, MEM_RESERVE, PAGE_NOACCESS);
    watch = false;
  }
  if (watching) *watching = watch;
  return p == addr;
}

bool guest_commit(void* addr, size_t size) { return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr; }

void guest_decommit(void* addr, size_t size) { VirtualFree(addr, size, MEM_DECOMMIT); }

bool take_written_pages(void* base, size_t offset, size_t size, std::vector<u32>& out) {
  static std::vector<void*> buf;
  if (buf.size() < size / 0x1000 + 1) buf.resize(size / 0x1000 + 1);
  ULONG_PTR count = size / 0x1000 + 1;
  DWORD gran = 0;
  if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, static_cast<u8*>(base) + offset, size, buf.data(), &count, &gran) != 0) return false;
  for (ULONG_PTR i = 0; i < count; ++i) out.push_back(u32(static_cast<u8*>(buf[i]) - static_cast<u8*>(base)));
  return true;
}

void* alloc_rw(size_t size) { return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
void free_rw(void* p, size_t) { if (p) VirtualFree(p, 0, MEM_RELEASE); }

ProcessStats process_stats() {
  ProcessStats s;
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
    s.working_set = pmc.WorkingSetSize;
    s.page_faults = pmc.PageFaultCount;
  }
  FILETIME pc, pe, pk, pu, si, sk, su;
  if (GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk, &pu)) {
    s.cpu_kernel_us = ft_us(pk);
    s.cpu_user_us = ft_us(pu);
  }
  if (GetSystemTimes(&si, &sk, &su)) {
    s.system_idle_us = ft_us(si);
    s.system_total_us = ft_us(sk) + ft_us(su);
  }
  return s;
}

std::string executable_dir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  if (!n || n >= std::size(buf)) return {};
  return std::filesystem::path(std::wstring(buf, n)).parent_path().string();
}

std::string user_data_dir(const std::string& app) {
  const char* p = std::getenv("LOCALAPPDATA");
  std::filesystem::path base = p && *p ? std::filesystem::path(p) : std::filesystem::current_path();
  return (base / app).string();
}

[[noreturn]] void exit_process(int code) {
  std::fflush(nullptr);
  ExitProcess(UINT(code));
}

}  // namespace rt::platform

#else  // POSIX

#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <climits>
#include <ctime>

namespace rt::platform {
namespace {

u64 ts_us(const timespec& t) { return u64(t.tv_sec) * 1000000ull + u64(t.tv_nsec) / 1000ull; }

bool read_token(const char* path, const char* token, u64* value) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  char line[256];
  bool ok = false;
  while (std::fgets(line, sizeof line, f)) {
    if (std::strncmp(line, token, std::strlen(token)) != 0) continue;
    *value = std::strtoull(line + std::strlen(token), nullptr, 10);
    ok = true;
    break;
  }
  std::fclose(f);
  return ok;
}

}  // namespace

u64 monotonic_us() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return ts_us(t);
}

u64 tick_ms() { return monotonic_us() / 1000ull; }

void sleep_ms(u32 ms) {
  if (!ms) return;
  timespec t{time_t(u64(ms) / 1000ull), (long(ms) % 1000) * 1000000L};
  while (nanosleep(&t, &t) != 0 && errno == EINTR) {
  }
}

void sleep_us(u64 us) {
  if (!us) {
    sched_yield();
    return;
  }
  timespec t{time_t(us / 1000000ull), long(us % 1000000ull) * 1000L};
  while (nanosleep(&t, &t) != 0 && errno == EINTR) {
  }
}

void yield_cpu() { sched_yield(); }

u64 realtime_ns() {
  timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return u64(t.tv_sec) * 1000000000ull + u64(t.tv_nsec);
}

s32 utc_offset_seconds(bool* daylight) {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  if (daylight) *daylight = tm.tm_isdst > 0;
  return s32(tm.tm_gmtoff);
}

void utc_tm(std::time_t t, std::tm* out) { gmtime_r(&t, out); }
void local_tm(std::time_t t, std::tm* out) { localtime_r(&t, out); }

u32 current_thread_id() { return u32(::syscall(SYS_gettid)); }

void set_current_thread_name(const char* name) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%s", name);
  pthread_setname_np(pthread_self(), buf);
}

void wait_atomic_u32(std::atomic<u32>* a, u32 expected, u32 timeout_ms) {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  u64 ns = u64(ts.tv_nsec) + u64(timeout_ms) * 1000000ull;
  ts.tv_sec += time_t(ns / 1000000000ull);
  ts.tv_nsec = long(ns % 1000000000ull);
  syscall(SYS_futex, reinterpret_cast<u32*>(a), FUTEX_WAIT_PRIVATE, expected, &ts, nullptr, 0);
}

void notify_all_atomic_u32(std::atomic<u32>* a) {
  syscall(SYS_futex, reinterpret_cast<u32*>(a), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}

u32 cpu_count() {
  cpu_set_t set;
  if (sched_getaffinity(0, sizeof set, &set) == 0) {
    int n = CPU_COUNT(&set);
    if (n > 0) return u32(n);
  }
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? u32(n) : 1;
}

bool set_affinity_mask(u64 mask) {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int i = 0; i < 64; ++i)
    if (mask & (1ull << i)) CPU_SET(i, &set);
  return sched_setaffinity(0, sizeof set, &set) == 0;
}

bool set_process_priority(const char* cls) {
  int p = std::strcmp(cls, "high") == 0 ? -10 : std::strcmp(cls, "above-normal") == 0 ? -5 : 0;
  return p != 0 && setpriority(PRIO_PROCESS, 0, p) == 0;
}

bool guest_reserve(void* addr, size_t size, bool request_write_watch, bool* watching) {
  (void)request_write_watch;  // no MEM_WRITE_WATCH equivalent; the renderer hashes instead
  if (watching) *watching = false;
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
  void* p = mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
  if (p == MAP_FAILED) {
    // Kernels before MAP_FIXED_NOREPLACE: reserve anywhere and check (never clobber).
    p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != addr) {
      munmap(p, size);
      return false;
    }
  }
  return p == addr;
}

bool guest_commit(void* addr, size_t size) { return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0; }

void guest_decommit(void* addr, size_t size) {
  mprotect(addr, size, PROT_NONE);
  madvise(addr, size, MADV_DONTNEED);
}

bool take_written_pages(void*, size_t, size_t, std::vector<u32>&) { return false; }

void* alloc_rw(size_t size) {
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}
void free_rw(void* p, size_t size) {
  if (p) munmap(p, size);
}

ProcessStats process_stats() {
  ProcessStats s;
  long page = sysconf(_SC_PAGESIZE);
  long hz = sysconf(_SC_CLK_TCK);
  FILE* f = std::fopen("/proc/self/statm", "rb");
  if (f) {
    unsigned long size = 0, resident = 0;
    if (std::fscanf(f, "%lu %lu", &size, &resident) == 2) s.working_set = u64(resident) * u64(page);
    std::fclose(f);
  }
  f = std::fopen("/proc/self/stat", "rb");
  if (f) {
    char buf[4096];
    size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    std::fclose(f);
    char* close = std::strrchr(buf, ')');
    if (close) {
      // Fields after the command: state=3. minflt=10, majflt=12, utime=14, stime=15.
      char* p = close + 2;
      unsigned long long vals[16] = {};
      for (int i = 0; i < 16 && p && *p; ++i) {
        vals[i] = std::strtoull(p, &p, 10);
        while (*p == ' ') ++p;
      }
      s.page_faults = vals[7] + vals[9];
      s.cpu_user_us = u64(double(vals[11]) * 1000000.0 / double(hz ? hz : 100));
      s.cpu_kernel_us = u64(double(vals[12]) * 1000000.0 / double(hz ? hz : 100));
    }
  }
  f = std::fopen("/proc/stat", "rb");
  if (f) {
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
      if (std::strncmp(line, "cpu ", 4) != 0) continue;
      char* p = line + 4;
      u64 total = 0;
      for (int i = 0; i < 8 && *p; ++i) {
        u64 v = std::strtoull(p, &p, 10);
        total += v;
        while (*p == ' ') ++p;
      }
      u64 idle = 0, iowait = 0;
      p = line + 4;
      for (int i = 0; i < 5 && *p; ++i) {
        u64 v = std::strtoull(p, &p, 10);
        if (i == 3) idle = v;
        if (i == 4) iowait = v;
        while (*p == ' ') ++p;
      }
      s.system_total_us = total * (1000000ull / u64(hz ? hz : 100));
      s.system_idle_us = (idle + iowait) * (1000000ull / u64(hz ? hz : 100));
      break;
    }
    std::fclose(f);
  }
  return s;
}

std::string executable_dir() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return {};
  buf[n] = 0;
  return std::filesystem::path(buf).parent_path().string();
}

std::string user_data_dir(const std::string& app) {
  const char* xdg = std::getenv("XDG_DATA_HOME");
  std::filesystem::path base;
  if (xdg && *xdg)
    base = xdg;
  else {
    const char* home = std::getenv("HOME");
    base = home && *home ? std::filesystem::path(home) / ".local" / "share" : std::filesystem::current_path();
  }
  return (base / app).string();
}

[[noreturn]] void exit_process(int code) {
  std::fflush(nullptr);
  _exit(code);
}

}  // namespace rt::platform

#endif
