// LV2 PPU thread, time, timer and TTY syscalls.
#include <ctime>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "runtime/lv2/lv2.h"
#include "runtime/platform.h"

namespace rt::lv2 {

u64 system_time_us() {
  static u64 t0 = platform::monotonic_us();
  return platform::monotonic_us() - t0;
}

// High-resolution sleep (sys_timer_usleep): waitable timer per thread on Windows.
void sleep_us(u64 us) {
  note_block();
#ifdef _WIN32
  thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!us) {
    SwitchToThread();
    return;
  }
  if (!timer) {
    Sleep(DWORD((us + 999) / 1000));
    return;
  }
  LARGE_INTEGER due;
  due.QuadPart = -s64(us * 10);  // 100 ns units, relative
  SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
  WaitForSingleObject(timer, INFINITE);
#else
  platform::sleep_us(us);
#endif
}

namespace {

void sys_ppu_thread_exit(u64 val) { thread_exit(val); }
s32 sys_ppu_thread_yield() {
  platform::yield_cpu();
  return CELL_OK;
}
s32 sys_ppu_thread_join(u64 id, Ptr<be64> status) {
  u64 st = 0;
  s32 r = thread_join(u32(id), &st);
  if (r == CELL_OK && status) *status = st;
  return r;
}
s32 sys_ppu_thread_detach(u64 id) { return thread_detach(u32(id)); }
s32 sys_ppu_thread_set_priority(u64 id, s32 prio) {
  PpuThread* t = thread_get(u32(id));
  if (!t) return CELL_ESRCH;
  if (prio < 0 || prio > 3071) return CELL_EINVAL;
  t->prio = prio;
  return CELL_OK;
}
s32 sys_ppu_thread_get_priority(u64 id, Ptr<bes32> prio) {
  PpuThread* t = thread_get(u32(id));
  if (!t) return CELL_ESRCH;
  *prio = t->prio;
  return CELL_OK;
}

s32 sys_timer_usleep(u64 us) {
  sleep_us(us);
  return CELL_OK;
}
s32 sys_timer_sleep(u32 sec) {
  sleep_us(u64(sec) * 1000000);
  return CELL_OK;
}

s32 sys_time_get_timezone(Ptr<bes32> timezone, Ptr<bes32> summertime) {
  bool daylight = false;
  s32 offset_seconds = platform::utc_offset_seconds(&daylight);
  // Same units as the Windows path: minutes west of UTC, and the daylight bias in minutes.
  if (timezone) *timezone = -offset_seconds / 60;
  if (summertime) *summertime = daylight ? 60 : 0;
  return CELL_OK;
}
s32 sys_time_get_current_time(Ptr<bes64> sec, Ptr<bes64> nsec) {
  u64 t = platform::realtime_ns();
  if (sec) *sec = s64(t / 1000000000ull);
  if (nsec) *nsec = s64(t % 1000000000ull);
  return CELL_OK;
}
u64 sys_time_get_timebase_frequency() { return 79800000ull; }

s32 sys_tty_write(s32 ch, u32 buf, u32 len, Ptr<be32> written) {
  if (ch < 0 || ch > 15) return CELL_EINVAL;
  std::string s(reinterpret_cast<const char*>(ppu::mem() + buf), len);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  if (!s.empty()) LOG_I("tty", "%s", s.c_str());
  if (written) *written = len;
  return CELL_OK;
}
s32 sys_tty_read(s32 ch, u32 buf, u32 len, Ptr<be32> read) {
  if (read) *read = 0;
  return CELL_OK;
}

hle::SyscallTable s_thread({
    SYSCALL(41, sys_ppu_thread_exit),
    SYSCALL(43, sys_ppu_thread_yield),
    SYSCALL(44, sys_ppu_thread_join),
    SYSCALL(45, sys_ppu_thread_detach),
    SYSCALL(47, sys_ppu_thread_set_priority),
    SYSCALL(48, sys_ppu_thread_get_priority),
    SYSCALL(141, sys_timer_usleep),
    SYSCALL(142, sys_timer_sleep),
    SYSCALL(144, sys_time_get_timezone),
    SYSCALL(145, sys_time_get_current_time),
    SYSCALL(147, sys_time_get_timebase_frequency),
    SYSCALL(402, sys_tty_read),
    SYSCALL(403, sys_tty_write),
});

}  // namespace
}  // namespace rt::lv2
