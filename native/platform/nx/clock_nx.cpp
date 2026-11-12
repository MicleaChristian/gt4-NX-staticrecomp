// Switch monotonic clock / sleep for gt4::platform.
#include "platform/clock.hpp"

#include <thread>

#if defined(__SWITCH__)
#include <switch.h>
#include <time.h>
#else
#include <chrono>
#endif

namespace gt4::platform {

std::uint64_t clock_frequency() {
#if defined(__SWITCH__)
  return 1'000'000'000ull; // nanoseconds via armGetSystemTick conversion below uses ns sleep; expose 1e9
#else
  return 1'000'000'000ull;
#endif
}

std::int64_t clock_now() {
#if defined(__SWITCH__)
  // armGetSystemTick is the usual homebrew monotonic source; convert to nanoseconds.
  const std::uint64_t tick = armGetSystemTick();
  const std::uint64_t ns = armTicksToNs(tick);
  return static_cast<std::int64_t>(ns);
#else
  using clock = std::chrono::steady_clock;
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());
#endif
}

bool clock_high_resolution_wait() { return true; }

void sleep_ticks(std::uint64_t frequency, std::int64_t ticks) {
  if (ticks <= 0 || frequency == 0) return;
  const double seconds = static_cast<double>(ticks) / static_cast<double>(frequency);
#if defined(__SWITCH__)
  const std::uint64_t ns = static_cast<std::uint64_t>(seconds * 1e9);
  svcSleepThread(static_cast<std::int64_t>(ns));
#else
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
#endif
}

} // namespace gt4::platform
