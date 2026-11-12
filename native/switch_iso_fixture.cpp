// TASK-174: host-side check of Switch ISO discovery (no DevKitPro required).
#include "../switch/include/gt4_switch.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

int main(int argc, char **argv) {
  const fs::path work = (argc > 1) ? fs::path(argv[1]) : fs::temp_directory_path() / "gt4_switch_iso_fixture";
  std::error_code error;
  fs::remove_all(work, error);
  fs::create_directories(work, error);
  {
    std::ofstream a(work / "game.iso", std::ios::binary);
    a << "ISO9660stub";
    std::ofstream b(work / "notes.txt");
    b << "not an iso";
  }
  const auto found = gt4::switch_app::find_isos_in(work);
  if (found.size() != 1 || found.front().filename() != "game.iso") {
    std::fprintf(stderr, "FAIL: expected one game.iso, got %zu\n", found.size());
    return 1;
  }
  std::fprintf(stderr, "PASS: switch_iso_fixture\n");
  fs::remove_all(work, error);
  return 0;
}
