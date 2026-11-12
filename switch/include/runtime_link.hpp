// TASK-174 S3: contract for linking the translated game into the NRO.
#pragma once

#include <cstddef>
#include <cstdint>

// Probe: 0 = ready to boot, >0 = error code, -1 = runtime not linked into this NRO.
extern "C" int gt4_switch_runtime_probe(const char *iso_path_utf8, const char *app_dir_utf8, char *message,
                                        std::size_t message_cap);

// Boot the donor. max_frames 0 = until guest exit; otherwise stop after N guest frames.
extern "C" int gt4_switch_game_main_frames(const char *iso_path_utf8, const char *app_dir_utf8,
                                           std::uint64_t max_frames);

// Back-compat name used earlier: unlimited run.
extern "C" int gt4_switch_game_main(const char *iso_path_utf8, const char *app_dir_utf8);
