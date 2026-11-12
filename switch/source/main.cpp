// TASK-174: status UI first. Game boot is opt-in (A / X) so we never sit on an unexplained black screen.
#include "gt4_switch.hpp"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#endif

using gt4::switch_app::AppSettings;
using gt4::switch_app::HostInfo;
using gt4::switch_app::InputState;
using gt4::switch_app::RuntimeStatus;

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

#if defined(__SWITCH__)
  appletLockExit();
#endif

  const auto app_dir = gt4::switch_app::resolve_app_directory();
  AppSettings settings = gt4::switch_app::load_settings(app_dir);
  gt4::switch_app::present_init(settings);
  gt4::switch_app::pad_init();
  gt4::switch_app::audio_init(settings);

  std::error_code error;
  std::filesystem::create_directories(app_dir / "saves", error);

  const HostInfo host = gt4::switch_app::query_host_info();
  const auto isos = gt4::switch_app::find_isos_in(app_dir);
  const std::filesystem::path iso = isos.empty() ? std::filesystem::path{} : isos.front();
  std::string runtime_message;
  RuntimeStatus status = gt4::switch_app::runtime_probe(iso, settings, runtime_message);

  bool running = true;
  bool a_prev = false;
  bool triangle_prev = false;

  while (running) {
#if defined(__SWITCH__)
    if (!appletMainLoop()) break;
#endif
    const InputState pad = gt4::switch_app::pad_poll();
    if (pad.start) running = false;

    if (pad.select) {
      gt4::switch_app::settings_toggle_performance(settings);
      gt4::switch_app::save_settings(app_dir, settings);
    }
    if (pad.l1 && pad.r1) {
      gt4::switch_app::settings_cycle_scale(settings);
      gt4::switch_app::save_settings(app_dir, settings);
    }

    // Switch A edge: longer smoke (~5s at 60) so the cyan OSD is obviously moving.
    if (pad.a && !a_prev && (status == RuntimeStatus::Ready || status == RuntimeStatus::Ran)) {
      std::vector<std::string> notice{
          "Smoke boot: 300 frames (~5s). Cyan bar must keep growing.",
          "If it freezes early, check last_boot.txt next to the NRO.",
          host.application_mode ? "Mode OK (Application)." : "WARNING: Applet mode - likely too little RAM.",
      };
      gt4::switch_app::present_status_lines(notice);
#if defined(__SWITCH__)
      svcSleepThread(1'000'000'000ull);
#endif
      // Release text console so the soft framebuffer can own the window.
      gt4::switch_app::present_shutdown();
      const int rc = gt4::switch_app::runtime_boot_game(iso, app_dir, 300, runtime_message);
      status = (rc == 0) ? RuntimeStatus::Ready : RuntimeStatus::Failed;
      gt4::switch_app::present_init(settings);
    }
    a_prev = pad.a;

    // Switch X: long run. Soft FB + cyan OSD. Plus aborts (after a short grace). Home exits.
    if (pad.triangle && !triangle_prev && (status == RuntimeStatus::Ready || status == RuntimeStatus::Ran)) {
      std::vector<std::string> notice{
          "Long run: CPU GS + pad wired. B=Cross(OK), A=Circle, D-pad move.",
          "Hold Plus ~1.5s to abort. Choppy until CoreGS GPU.",
          host.application_mode ? "Mode: Application." : "WARNING: Applet mode - may OOM silently.",
      };
      gt4::switch_app::present_status_lines(notice);
#if defined(__SWITCH__)
      svcSleepThread(1'000'000'000ull);
#endif
      gt4::switch_app::present_shutdown();
      const int rc = gt4::switch_app::runtime_boot_game(iso, app_dir, 0, runtime_message);
      status = (rc == 0) ? RuntimeStatus::Ready : RuntimeStatus::Failed;
      gt4::switch_app::present_init(settings);
    }
    triangle_prev = pad.triangle;

    std::vector<std::string> lines;
    lines.push_back("GT4Recomp Switch (TASK-174)");
    lines.push_back("Launch: " + host.mode_label);
    lines.push_back("App dir: " + app_dir.string());
    lines.push_back(std::string("ISO: ") + (iso.empty() ? "(none)" : iso.filename().string()));
    lines.push_back(runtime_message);
    lines.push_back("scale=" + std::to_string(settings.scale) +
                    "  performance=" + std::string(settings.performance_mode ? "on" : "off"));
    lines.push_back("A: smoke 300f (~5s) | X: long run | Plus: exit menu");
    lines.push_back("Minus: Performance | L+R: scale");
    if (!host.application_mode)
      lines.push_back("TIP: hold R when launching a title to force full RAM, or use hbmenu title override.");

    switch (status) {
      case RuntimeStatus::Ready:
        lines.push_back("Status: Ready (waiting for A/X)");
        break;
      case RuntimeStatus::Ran:
        lines.push_back("Status: Ready (boot returned)");
        break;
      case RuntimeStatus::MissingIso:
        lines.push_back("Status: MissingIso");
        break;
      case RuntimeStatus::MissingElf:
        lines.push_back("Status: MissingElf");
        break;
      case RuntimeStatus::NotLinked:
        lines.push_back("Status: NotLinked");
        break;
      default:
        lines.push_back("Status: Failed");
        break;
    }

    gt4::switch_app::present_status_lines(lines);
    gt4::switch_app::audio_submit_silence();

#if defined(__SWITCH__)
    svcSleepThread(33'000'000); // ~30 Hz UI
#endif
  }

  gt4::switch_app::audio_shutdown();
  gt4::switch_app::pad_shutdown();
  gt4::switch_app::present_shutdown();
#if defined(__SWITCH__)
  appletUnlockExit();
#endif
  return (status == RuntimeStatus::MissingIso) ? 1 : 0;
}
