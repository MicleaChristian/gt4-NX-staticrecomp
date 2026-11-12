#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gt4::switch_app {

struct AppSettings {
  int scale = 1; // CoreGS internal scale; Switch v1 default 1x
  bool performance_mode = true;
  bool audio_enabled = true;
  // Off by default: CoreGS cpuref on device still corrupts some scenes (SCEA splash).
  // Set coregs=1 in gt4recomp.ini (or create coregs.on) to test BackendDk.
  bool coregs = false;
};

std::vector<std::filesystem::path> find_isos_in(const std::filesystem::path &directory);
std::filesystem::path resolve_app_directory();

// Named InputState so it does not collide with libnx's ::PadState.
struct InputState {
  bool cross = false;
  bool circle = false;
  bool square = false;
  bool triangle = false;
  bool start = false;
  bool select = false;
  bool l1 = false;
  bool r1 = false;
  bool a = false; // Switch A (confirm) — same as circle mapping side
  bool b = false;
  float lx = 0.f;
  float ly = 0.f;
  float rx = 0.f;
  float ry = 0.f;
  float l2 = 0.f;
  float r2 = 0.f;
};

void pad_init();
void pad_shutdown();
InputState pad_poll();

void audio_init(AppSettings &settings);
void audio_shutdown();
void audio_submit_silence();

void present_init(AppSettings &settings);
void present_shutdown();
void present_clear(std::uint8_t r, std::uint8_t g, std::uint8_t b);
void present_status_lines(const std::vector<std::string> &lines);

enum class RuntimeStatus {
  NotLinked,
  MissingIso,
  MissingElf,
  Ready,   // ISO + core.elf + linked runtime — not started yet
  Failed,
  Ran,     // returned from a boot attempt
};

// Host / applet info for the status screen.
struct HostInfo {
  bool application_mode = false;
  std::string mode_label; // "Application (full)" or "Applet (limited RAM)"
};

HostInfo query_host_info();

// Probe only — never enters the game loop (so the console UI stays visible).
RuntimeStatus runtime_probe(const std::filesystem::path &iso, const AppSettings &settings, std::string &message);

// Enter the donor. frames==0 runs until the guest exits; >0 is a smoke test then returns.
int runtime_boot_game(const std::filesystem::path &iso, const std::filesystem::path &app_dir, std::uint64_t frames,
                      std::string &message);

AppSettings load_settings(const std::filesystem::path &app_dir);
void save_settings(const std::filesystem::path &app_dir, const AppSettings &settings);
void settings_cycle_scale(AppSettings &settings);
void settings_toggle_performance(AppSettings &settings);

} // namespace gt4::switch_app
