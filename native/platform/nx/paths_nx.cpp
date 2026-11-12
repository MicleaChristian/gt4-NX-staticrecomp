// Switch / libnx backend for gt4::platform paths. Compiled into the NRO target, not the Windows game.
#include "platform/paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <system_error>

namespace gt4::platform {
namespace {

constexpr const char *kDefaultAppDir = "sdmc:/switch/gt4recomp";

} // namespace

std::filesystem::path executable_directory() {
  // Atmosphere's homebrew loader sets the working directory to the folder that contains the NRO.
  std::error_code error;
  const std::filesystem::path cwd = std::filesystem::current_path(error);
  if (!error && !cwd.empty()) return cwd;
  return std::filesystem::path(kDefaultAppDir);
}

std::string getenv_utf8(const char *name) {
  if (!name) return {};
  const char *value = std::getenv(name);
  return value ? std::string(value) : std::string{};
}

std::vector<std::filesystem::path> find_sibling_isos() {
  std::vector<std::filesystem::path> out;
  const std::filesystem::path directory = executable_directory();
  if (directory.empty()) return out;
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

std::filesystem::path first_sibling_iso() {
  const auto isos = find_sibling_isos();
  return isos.empty() ? std::filesystem::path{} : isos.front();
}

} // namespace gt4::platform
