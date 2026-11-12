#include "gt4_switch.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>

namespace gt4::switch_app {
namespace {

std::filesystem::path settings_path(const std::filesystem::path &app_dir) {
  return app_dir / "gt4recomp.ini";
}

} // namespace

AppSettings load_settings(const std::filesystem::path &app_dir) {
  AppSettings s{};
  std::ifstream in(settings_path(app_dir));
  if (!in) return s;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("scale=", 0) == 0) s.scale = std::max(1, std::atoi(line.c_str() + 6));
    if (line.rfind("performance_mode=", 0) == 0) s.performance_mode = line.find('1') != std::string::npos;
    if (line.rfind("audio_enabled=", 0) == 0) s.audio_enabled = line.find('0') == std::string::npos;
  }
  if (s.scale > 2) s.scale = 2; // Switch v1: keep host memory small
  if (std::filesystem::exists(app_dir / "coregs.off")) s.coregs = false;
  if (std::filesystem::exists(app_dir / "gpudraw.off")) s.gpu_draw = false;
  return s;
}

void save_settings(const std::filesystem::path &app_dir, const AppSettings &settings) {
  std::error_code error;
  std::filesystem::create_directories(app_dir, error);
  std::ofstream out(settings_path(app_dir), std::ios::trunc);
  if (!out) return;
  out << "scale=" << settings.scale << '\n';
  out << "performance_mode=" << (settings.performance_mode ? 1 : 0) << '\n';
  out << "audio_enabled=" << (settings.audio_enabled ? 1 : 0) << '\n';
}

void settings_cycle_scale(AppSettings &settings) {
  settings.scale = settings.scale >= 2 ? 1 : settings.scale + 1;
}

void settings_toggle_performance(AppSettings &settings) {
  settings.performance_mode = !settings.performance_mode;
}

} // namespace gt4::switch_app
