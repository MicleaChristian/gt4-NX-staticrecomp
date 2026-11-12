#include "frame_clock.hpp"

#include "platform/clock.hpp"

#include <cstdio>

namespace {
// The final stretch is spun rather than slept: a timer wake-up can be late.
constexpr double spin_seconds = 0.0005;
}

FrameClock::FrameClock() {
  frequency_ = gt4::platform::clock_frequency();
  if (!gt4::platform::clock_high_resolution_wait())
    std::fprintf(stderr, "[pacing] high-resolution wait unavailable; using coarse Sleep\n");
}

std::int64_t FrameClock::now() const { return gt4::platform::clock_now(); }

bool FrameClock::high_resolution_timer() const { return gt4::platform::clock_high_resolution_wait(); }

void FrameClock::sleep_ticks(std::int64_t ticks) {
  gt4::platform::sleep_ticks(frequency_, ticks);
}

void FrameClock::wait_until(std::int64_t target) {
  const auto spin = static_cast<std::int64_t>(spin_seconds * static_cast<double>(frequency_));
  for (;;) {
    const std::int64_t remaining = target - now();
    if (remaining <= 0) return;
    if (remaining > spin) sleep_ticks(remaining - spin);
  }
}
