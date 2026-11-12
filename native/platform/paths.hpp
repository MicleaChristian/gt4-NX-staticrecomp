// TASK-174: portable executable / app directory helpers (Windows + Switch).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gt4::platform {

// Directory that contains the running executable (PE or NRO), or empty on failure.
std::filesystem::path executable_directory();

// UTF-8 getenv; empty optional when unset. Never returns a null pointer from the raw C API to callers.
std::string getenv_utf8(const char *name);

// Look for plain *.iso / *.ISO files next to the executable. Returns absolute paths, sorted.
std::vector<std::filesystem::path> find_sibling_isos();

// First sibling ISO, or empty if none.
std::filesystem::path first_sibling_iso();

} // namespace gt4::platform
