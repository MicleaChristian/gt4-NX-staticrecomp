// TASK-174 S4: CoreGS present path skeleton for deko3d.
// Full CoreGS GPU binding lands here later; for now we present a console status framebuffer via libnx gfx.
#include "gt4_switch.hpp"

#include "platform/nx/deko3d_present.hpp"

#include <cstdio>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace gt4::switch_app {
namespace {
AppSettings g_present_settings{};
std::vector<std::string> g_lines;
} // namespace

void present_init(AppSettings &settings) {
  g_present_settings = settings;
  gt4::platform::nx::PresentConfig cfg;
  cfg.scale = settings.scale;
  cfg.performance_mode = settings.performance_mode;
  gt4::platform::nx::deko3d_present_create(cfg);
#if defined(__SWITCH__)
  // Console UI for the hello / boot-status NRO. When CoreGS+deko3d is wired, this becomes a swapchain present.
  consoleInit(nullptr);
#endif
  g_lines.clear();
  g_lines.push_back("GT4Recomp Switch");
  g_lines.push_back(std::string("scale=") + std::to_string(settings.scale) +
                    " performance=" + (settings.performance_mode ? "on" : "off"));
}

void present_shutdown() {
#if defined(__SWITCH__)
  // Drop text console before a soft/game framebuffer takes the default window.
  // Do not wipe PresentConfig / g_alive here — soft_present_begin() reuses them.
  consoleExit(nullptr);
#endif
  gt4::platform::nx::soft_present_end();
}

void present_clear(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  (void)r;
  (void)g;
  (void)b;
#if defined(__SWITCH__)
  consoleClear();
#endif
}

void present_status_lines(const std::vector<std::string> &lines) {
  g_lines = lines;
  present_clear(0, 0, 0);
#if defined(__SWITCH__)
  for (const auto &line : g_lines) {
    std::printf("%s\n", line.c_str());
  }
  consoleUpdate(nullptr);
#else
  for (const auto &line : g_lines) {
    std::fprintf(stderr, "%s\n", line.c_str());
  }
#endif
}

} // namespace gt4::switch_app
