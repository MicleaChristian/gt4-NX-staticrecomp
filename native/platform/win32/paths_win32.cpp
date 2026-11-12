#include "platform/paths.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <system_error>

namespace gt4::platform {

std::filesystem::path executable_directory() {
  wchar_t path[32768];
  const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  if (length == 0 || length >= std::size(path)) return {};
  return std::filesystem::path(path).parent_path();
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
    if (ext == ".iso" || ext == ".ISO") out.push_back(std::filesystem::absolute(entry.path(), error));
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::filesystem::path first_sibling_iso() {
  const auto isos = find_sibling_isos();
  return isos.empty() ? std::filesystem::path{} : isos.front();
}

} // namespace gt4::platform
