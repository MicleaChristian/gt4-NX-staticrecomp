// TASK-174 S3: placeholder strong symbol until EE/VU archives are linked.
// Prefer leaving this weak-unresolved so a real lib can override; this TU is optional
// (not in the default Makefile). Build with EXTRA_GAME_MAIN=1 to force a linked stub.
#include "runtime_link.hpp"

#include <cstdio>

extern "C" int gt4_switch_game_main(const char *iso_path_utf8, const char *app_dir_utf8) {
  std::fprintf(stderr, "[gt4-switch] stub game_main iso=%s app=%s\n",
               iso_path_utf8 ? iso_path_utf8 : "(null)", app_dir_utf8 ? app_dir_utf8 : "(null)");
  return -1;
}
