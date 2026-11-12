#include "gt4_switch.hpp"

#include <algorithm>
#include <system_error>

namespace gt4::switch_app {

std::filesystem::path resolve_app_directory() {
  std::error_code error;
  const std::filesystem::path cwd = std::filesystem::current_path(error);
  if (!error && !cwd.empty()) return cwd;
  return std::filesystem::path("sdmc:/switch/gt4recomp");
}

std::vector<std::filesystem::path> find_isos_in(const std::filesystem::path &directory) {
  std::vector<std::filesystem::path> out;
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) return out;
  for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
    if (error) break;
    if (!entry.is_regular_file(error)) continue;
    const auto ext = entry.path().extension().string();
    if (ext == ".iso" || ext == ".ISO") out.push_back(entry.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace gt4::switch_app
