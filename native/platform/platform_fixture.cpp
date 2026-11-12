// TASK-174: smoke test for the portable platform seam on Windows.
#include "platform/clock.hpp"
#include "platform/paths.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void require(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

} // namespace

int main() {
  const auto dir = gt4::platform::executable_directory();
  require(!dir.empty(), "executable_directory is non-empty");
  std::fprintf(stderr, "executable_directory=%s\n", dir.string().c_str());

  const std::uint64_t freq = gt4::platform::clock_frequency();
  require(freq > 0, "clock_frequency > 0");
  const std::int64_t a = gt4::platform::clock_now();
  gt4::platform::sleep_ticks(freq, static_cast<std::int64_t>(freq / 200)); // ~5 ms
  const std::int64_t b = gt4::platform::clock_now();
  require(b > a, "clock advances after sleep");

  // getenv of a known Windows / process variable should not crash; value may be empty.
  const std::string path_env = gt4::platform::getenv_utf8("PATH");
  require(!path_env.empty(), "PATH is set");

  // Sibling ISO discovery must not throw when the folder has no ISOs.
  const auto isos = gt4::platform::find_sibling_isos();
  std::fprintf(stderr, "sibling_isos=%zu\n", isos.size());

  std::fprintf(stderr, "PASS: native_platform\n");
  return 0;
}
