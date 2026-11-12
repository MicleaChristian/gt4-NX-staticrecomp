// TASK-174: probe vs boot. Never auto-enter the game loop (that was the black screen).
#include "gt4_switch.hpp"
#include "runtime_link.hpp"

#include <cstdio>

#if defined(__clang__) || defined(__GNUC__)
extern "C" __attribute__((weak)) int gt4_switch_runtime_probe(const char *iso_path_utf8, const char *app_dir_utf8,
                                                              char *message, std::size_t message_cap) {
  (void)iso_path_utf8;
  (void)app_dir_utf8;
  if (message && message_cap)
    std::snprintf(message, message_cap, "%s", "Runtime not linked into this NRO.");
  return -1;
}
extern "C" __attribute__((weak)) int gt4_switch_game_main_frames(const char *iso_path_utf8, const char *app_dir_utf8,
                                                                 std::uint64_t max_frames) {
  (void)iso_path_utf8;
  (void)app_dir_utf8;
  (void)max_frames;
  return -1;
}
extern "C" __attribute__((weak)) int gt4_switch_game_main(const char *iso_path_utf8, const char *app_dir_utf8) {
  return gt4_switch_game_main_frames(iso_path_utf8, app_dir_utf8, 0);
}
#endif

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace gt4::switch_app {

HostInfo query_host_info() {
  HostInfo info;
#if defined(__SWITCH__)
  const AppletType type = appletGetAppletType();
  info.application_mode = (type == AppletType_Application || type == AppletType_SystemApplication);
  if (info.application_mode) {
    info.mode_label = "Application (full memory) - OK";
  } else {
    info.mode_label = "Applet (limited RAM) - use title override / full mode; SLaunch Apps may be applet";
  }
#else
  info.application_mode = true;
  info.mode_label = "host";
#endif
  return info;
}

RuntimeStatus runtime_probe(const std::filesystem::path &iso, const AppSettings &settings, std::string &message) {
  (void)settings;
  if (iso.empty()) {
    message = "No .iso next to the NRO.";
    return RuntimeStatus::MissingIso;
  }
  char buf[512];
  const auto app_dir = resolve_app_directory();
  const int rc = gt4_switch_runtime_probe(iso.string().c_str(), app_dir.string().c_str(), buf, sizeof buf);
  message = buf;
  if (rc == -1) return RuntimeStatus::NotLinked;
  if (rc == 3) return RuntimeStatus::MissingElf;
  if (rc == 2) return RuntimeStatus::MissingIso;
  if (rc != 0) return RuntimeStatus::Failed;
  return RuntimeStatus::Ready;
}

int runtime_boot_game(const std::filesystem::path &iso, const std::filesystem::path &app_dir, std::uint64_t frames,
                      std::string &message) {
  const int rc = gt4_switch_game_main_frames(iso.string().c_str(), app_dir.string().c_str(), frames);
  if (rc == -1) {
    message = "Boot failed: runtime not linked.";
  } else if (rc == 0) {
    message = frames ? ("Smoke boot finished OK (" + std::to_string(frames) + " frames). See last_boot.txt.")
                     : "Long run returned. See last_boot.txt next to the NRO.";
  } else if (rc == 5) {
    message = "Boot failed: soft framebuffer init (console/FB conflict). Restart app.";
  } else {
    message = "Boot exited with code " + std::to_string(rc) + ". See last_boot.txt.";
  }
  return rc;
}

} // namespace gt4::switch_app
