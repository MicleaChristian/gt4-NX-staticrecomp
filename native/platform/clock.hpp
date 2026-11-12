// TASK-174: monotonic wall clock and sleep used by the frame pacer on every host.
#pragma once

#include <cstdint>

namespace gt4::platform {

// Ticks of a monotonic host clock. Frequency is ticks per second.
std::uint64_t clock_frequency();
std::int64_t clock_now();

// Sleep for approximately the given ticks (may wake early). Prefer high-resolution waits when available.
void sleep_ticks(std::uint64_t frequency, std::int64_t ticks);

// True when the host exposed a high-resolution wait primitive (Windows waitable timer, Switch nanosleep, etc.).
bool clock_high_resolution_wait();

} // namespace gt4::platform
