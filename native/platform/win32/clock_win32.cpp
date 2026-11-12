#include "platform/clock.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

#include <stdexcept>

#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace gt4::platform {
namespace {

struct WaitState {
  HANDLE timer = nullptr;
  bool period_raised = false;
  bool tried = false;
};

WaitState &wait_state() {
  static WaitState state;
  if (!state.tried) {
    state.tried = true;
    state.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!state.timer) {
      if (timeBeginPeriod(1) == TIMERR_NOERROR) state.period_raised = true;
    }
  }
  return state;
}

struct WaitStateCleanup {
  ~WaitStateCleanup() {
    WaitState &state = wait_state();
    if (state.timer) {
      CloseHandle(state.timer);
      state.timer = nullptr;
    }
    if (state.period_raised) {
      timeEndPeriod(1);
      state.period_raised = false;
    }
  }
};

// Keep the process-wide timer period / handle until exit (same lifetime as the old FrameClock singleton use).
WaitStateCleanup g_wait_cleanup;

} // namespace

std::uint64_t clock_frequency() {
  LARGE_INTEGER frequency{};
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    throw std::runtime_error("QueryPerformanceFrequency unavailable");
  return static_cast<std::uint64_t>(frequency.QuadPart);
}

std::int64_t clock_now() {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return counter.QuadPart;
}

bool clock_high_resolution_wait() { return wait_state().timer != nullptr; }

void sleep_ticks(std::uint64_t frequency, std::int64_t ticks) {
  if (ticks <= 0 || frequency == 0) return;
  WaitState &state = wait_state();
  if (state.timer) {
    const double seconds = static_cast<double>(ticks) / static_cast<double>(frequency);
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(seconds * 10000000.0);
    if (due.QuadPart >= 0) return;
    if (!SetWaitableTimer(state.timer, &due, 0, nullptr, nullptr, FALSE))
      throw std::runtime_error("SetWaitableTimer failed");
    WaitForSingleObject(state.timer, INFINITE);
    return;
  }
  const double seconds = static_cast<double>(ticks) / static_cast<double>(frequency);
  const DWORD ms = static_cast<DWORD>(seconds * 1000.0);
  if (ms > 0) Sleep(ms);
}

} // namespace gt4::platform
