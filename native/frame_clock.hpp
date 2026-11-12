#pragma once

#include <cstdint>

// Wall clock for the frame pacer. Host details live in native/platform/ (TASK-174).
class FrameClock {
public:
  FrameClock();
  ~FrameClock() = default;
  FrameClock(const FrameClock &) = delete;
  FrameClock &operator=(const FrameClock &) = delete;

  std::uint64_t frequency() const { return frequency_; }
  std::int64_t now() const;
  // Returns once now() >= target. Sleeps until shortly before the target, then spins.
  void wait_until(std::int64_t target);
  bool high_resolution_timer() const;

private:
  void sleep_ticks(std::int64_t ticks);
  std::uint64_t frequency_ = 0;
};
