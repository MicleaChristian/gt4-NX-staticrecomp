// TASK-174: strong Switch entry — donor boot + optional CoreGS BackendDk (deko3d present).
#include "runtime_link.hpp"

#include "coregs_nx.hpp"
#include "gt4_switch.hpp"
#include "perf_nx.hpp"
#include "platform/nx/deko3d_present.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"
#include "race_display.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#endif

void register_generated_code();
void register_generated_vu();

#if defined(__SWITCH__)
// Strong when VU1 AOT overlay is linked (patch-vu-perf.py); null when stubbed.
extern "C" void gt4_vu1_aot_snapshot(std::uint64_t *, std::uint64_t *, std::uint64_t *) __attribute__((weak));
#endif

namespace {

bool file_readable(const char *path) {
  if (!path || !*path) return false;
  FILE *f = std::fopen(path, "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

void set_msg(char *message, std::size_t cap, const char *text) {
  if (!message || cap == 0) return;
  std::snprintf(message, cap, "%s", text ? text : "");
}

#if defined(__SWITCH__)
PadState g_boot_pad{};
std::vector<std::uint32_t> g_frame_rgba;
std::string g_last_app_dir;
std::vector<std::uint32_t> g_held_field;
std::uint32_t g_held_w = 0;
std::uint32_t g_held_h = 0;

constexpr std::uint16_t kBtnSelect = 1u << 0;
constexpr std::uint16_t kBtnL3 = 1u << 1;
constexpr std::uint16_t kBtnR3 = 1u << 2;
constexpr std::uint16_t kBtnStart = 1u << 3;
constexpr std::uint16_t kBtnUp = 1u << 4;
constexpr std::uint16_t kBtnRight = 1u << 5;
constexpr std::uint16_t kBtnDown = 1u << 6;
constexpr std::uint16_t kBtnLeft = 1u << 7;
constexpr std::uint16_t kBtnL2 = 1u << 8;
constexpr std::uint16_t kBtnR2 = 1u << 9;
constexpr std::uint16_t kBtnL1 = 1u << 10;
constexpr std::uint16_t kBtnR1 = 1u << 11;
constexpr std::uint16_t kBtnTriangle = 1u << 12;
constexpr std::uint16_t kBtnCircle = 1u << 13;
constexpr std::uint16_t kBtnCross = 1u << 14;
constexpr std::uint16_t kBtnSquare = 1u << 15;

void write_halt_log(const char *why) {
  if (g_last_app_dir.empty() || !why) return;
  const std::string path = g_last_app_dir + "/halt.log";
  FILE *f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "%s\n", why);
  std::fclose(f);
}

std::uint8_t stick_to_ps2(float v, bool invert) {
  float t = invert ? -v : v;
  if (t < -1.f) t = -1.f;
  if (t > 1.f) t = 1.f;
  return static_cast<std::uint8_t>((t * 0.5f + 0.5f) * 255.f + 0.5f);
}

void feed_guest_pad() {
  padUpdate(&g_boot_pad);
  const u64 down = padGetButtons(&g_boot_pad);
  std::uint16_t buttons = 0;
  if (down & HidNpadButton_Minus) buttons |= kBtnSelect;
  if (down & HidNpadButton_StickL) buttons |= kBtnL3;
  if (down & HidNpadButton_StickR) buttons |= kBtnR3;
  if (down & HidNpadButton_Plus) buttons |= kBtnStart;
  if (down & HidNpadButton_Up) buttons |= kBtnUp;
  if (down & HidNpadButton_Right) buttons |= kBtnRight;
  if (down & HidNpadButton_Down) buttons |= kBtnDown;
  if (down & HidNpadButton_Left) buttons |= kBtnLeft;
  if (down & HidNpadButton_ZL) buttons |= kBtnL2;
  if (down & HidNpadButton_ZR) buttons |= kBtnR2;
  if (down & HidNpadButton_L) buttons |= kBtnL1;
  if (down & HidNpadButton_R) buttons |= kBtnR1;
  if (down & HidNpadButton_X) buttons |= kBtnTriangle;
  if (down & HidNpadButton_A) buttons |= kBtnCircle;
  if (down & HidNpadButton_B) buttons |= kBtnCross;
  if (down & HidNpadButton_Y) buttons |= kBtnSquare;
  ps2::set_pad_buttons(buttons);

  const HidAnalogStickState left = padGetStickPos(&g_boot_pad, 0);
  const HidAnalogStickState right = padGetStickPos(&g_boot_pad, 1);
  const float lx = static_cast<float>(left.x) / 32767.f;
  const float ly = static_cast<float>(-left.y) / 32767.f;
  const float rx = static_cast<float>(right.x) / 32767.f;
  const float ry = static_cast<float>(-right.y) / 32767.f;
  ps2::set_pad_axis(0, stick_to_ps2(rx, false));
  ps2::set_pad_axis(1, stick_to_ps2(ry, true));
  ps2::set_pad_axis(2, stick_to_ps2(lx, false));
  ps2::set_pad_axis(3, stick_to_ps2(ly, true));
}

void switch_host_vblank() {
  if (!appletMainLoop()) {
    write_halt_log("host requested exit");
    ps2::halt("host requested exit");
    return;
  }
  const std::uint64_t frame = ps2::frame_count();
  feed_guest_pad();

  // Prefer progressive race index when the game exposes it (cuts 480i bob).
  static gt4::display::Force480pState force480p{};
  gt4::display::force_race_480p_at_vblank(ps2::g_ram, ps2::kRamSize, force480p);

  static int plus_held = 0;
  if (padGetButtons(&g_boot_pad) & HidNpadButton_Plus) {
    if (frame > 45) ++plus_held;
  } else {
    plus_held = 0;
  }
  if (plus_held > 90) {
    write_halt_log("plus hold abort");
    ps2::halt("plus hold abort");
    return;
  }

  const bool coregs = gt4::switch_app::coregs_active();
  gt4::perf::FrameExtras extras{};
  extras.coregs = coregs;
  if (coregs) {
    gt4::switch_app::coregs_fill_perf(extras.gif_bytes, extras.gpu_draws, extras.gpu_batches,
                                      extras.cpu_fallback, extras.tex_cache_hits, extras.wb_flushes);
  }
  if (gt4_vu1_aot_snapshot) {
    gt4_vu1_aot_snapshot(&extras.vu1_aot_compiled, &extras.vu1_aot_interpreted, &extras.vu1_identity_rejects);
  }
  gt4::perf::on_vblank(frame, ps2::vu1_interpreted_calls(), &extras);

  if (coregs) {
    gt4::perf::Scope scope(gt4::perf::kCoreGsFrame);
    if (!gt4::switch_app::coregs_vblank_present(frame)) {
      gt4::platform::nx::soft_present_heartbeat(frame);
    }
    return;
  }

  // Donor CPU GS: field-hold on odd frames (re-present last even field) to stop 480i shake.
  std::uint32_t w = 0, h = 0;
  const bool odd = (frame & 1u) != 0;
  bool fresh = false;
  if (!odd) {
    gt4::perf::Scope scope(gt4::perf::kSoftFrame);
    fresh = ps2::display_frame(g_frame_rgba, w, h) && w > 0 && h > 0;
  }
  if (fresh) {
    g_held_field.swap(g_frame_rgba);
    g_held_w = w;
    g_held_h = h;
  }
  if (!g_held_field.empty()) {
    // Odd frames re-show the held field; skip the copy when nothing new arrived.
    if (fresh || !odd) {
      gt4::platform::nx::deko3d_present_frame(reinterpret_cast<const std::uint8_t *>(g_held_field.data()),
                                              g_held_field.size() * 4u, static_cast<int>(g_held_w),
                                              static_cast<int>(g_held_h), frame);
    }
  } else {
    gt4::platform::nx::soft_present_heartbeat(frame);
  }
}
#endif

} // namespace

extern "C" int gt4_switch_runtime_probe(const char *iso_path_utf8, const char *app_dir_utf8, char *message,
                                        std::size_t message_cap) {
  if (!iso_path_utf8 || !*iso_path_utf8) {
    set_msg(message, message_cap, "Missing ISO path.");
    return 2;
  }
  if (!file_readable(iso_path_utf8)) {
    set_msg(message, message_cap, "Cannot open ISO.");
    return 2;
  }
  const std::string app = app_dir_utf8 && *app_dir_utf8 ? app_dir_utf8 : "sdmc:/switch/gt4recomp";
  const std::string elf = app + "/core.elf";
  if (!file_readable(elf.c_str())) {
    set_msg(message, message_cap, "Missing core.elf next to the NRO (copy from export/switch).");
    return 3;
  }
  set_msg(message, message_cap,
          "Runtime linked. Donor GS + deko3d present (coregs=1 to test BackendDk). Plus aborts.");
  return 0;
}

extern "C" int gt4_switch_game_main_frames(const char *iso_path_utf8, const char *app_dir_utf8,
                                           std::uint64_t max_frames) {
  if (!iso_path_utf8 || !*iso_path_utf8) return 2;
  if (!file_readable(iso_path_utf8)) return 2;

  const std::string app = app_dir_utf8 && *app_dir_utf8 ? app_dir_utf8 : "sdmc:/switch/gt4recomp";
  const std::string memcard = app + "/saves/card.ps2";
  const std::string elf = app + "/core.elf";
  if (!file_readable(elf.c_str())) return 3;

#if defined(__SWITCH__)
  g_last_app_dir = app;
  // stderr -> stderr.log next to the NRO (CoreGS / WritebackDk / donor diagnostics).
  if (std::freopen((app + "/stderr.log").c_str(), "wb", stderr)) {
    static char stderr_buffer[64 * 1024];
    std::setvbuf(stderr, stderr_buffer, _IOLBF, sizeof stderr_buffer);
  }
  gt4::perf::open_log(app.c_str());
  std::fprintf(stderr,
               "[gt4-switch] logs: perf.log (CSV/60f), session_summary.txt (on exit), stderr.log, halt.log\n");
#endif

  std::fprintf(stderr, "[gt4-switch] boot elf=%s disc=%s memcard=%s frames=%llu\n", elf.c_str(), iso_path_utf8,
               memcard.c_str(), static_cast<unsigned long long>(max_frames));

  register_generated_code();
  register_generated_vu();

  ps2::g_options = ps2::Options{};
  ps2::g_options.pal = false;
  ps2::g_options.rate_forced = true;
  ps2::g_options.disc_path = iso_path_utf8;
  ps2::g_options.memcard_path = memcard;
  ps2::g_options.max_frames = max_frames;
#if defined(__SWITCH__)
  padInitializeDefault(&g_boot_pad);
  appletUnlockExit();
  if (!gt4::platform::nx::soft_present_begin()) {
    std::fprintf(stderr, "[gt4-switch] soft_present_begin failed\n");
    write_halt_log("soft_present_begin failed");
    appletLockExit();
    return 5;
  }
  const auto settings = gt4::switch_app::load_settings(std::filesystem::path(app));
  if (settings.coregs) {
    if (!gt4::switch_app::coregs_start()) {
      std::fprintf(stderr, "[gt4-switch] CoreGS start failed; using donor CPU GS\n");
    }
  } else {
    std::fprintf(stderr, "[gt4-switch] CoreGS off (set coregs=1 or coregs.on to enable BackendDk)\n");
  }
  ps2::g_options.on_vblank = switch_host_vblank;
#endif

  std::uint32_t entry = 0;
  if (!ps2::load_elf(elf.c_str(), entry)) {
    std::fprintf(stderr, "[gt4-switch] load_elf failed for %s\n", elf.c_str());
#if defined(__SWITCH__)
    ps2::g_options.on_vblank = nullptr;
    gt4::switch_app::coregs_stop();
    gt4::platform::nx::soft_present_end();
    write_halt_log("load_elf failed");
    appletLockExit();
#endif
    return 4;
  }
  std::vector<std::string> args{elf};
  ps2::boot(entry, args);
  const int rc = ps2::run();
#if defined(__SWITCH__)
  ps2::g_options.on_vblank = nullptr;
  char why[128];
  std::snprintf(why, sizeof why, "run returned rc=%d frames=%llu halted=%d coregs=%d", rc,
                static_cast<unsigned long long>(ps2::frame_count()), ps2::halted() ? 1 : 0,
                gt4::switch_app::coregs_active() ? 1 : 0);
  write_halt_log(why);
  gt4::perf::close_log();
  std::fflush(stderr);
  gt4::switch_app::coregs_stop();
  gt4::platform::nx::soft_present_end();
  appletLockExit();
#endif
  return rc;
}

extern "C" int gt4_switch_game_main(const char *iso_path_utf8, const char *app_dir_utf8) {
  return gt4_switch_game_main_frames(iso_path_utf8, app_dir_utf8, 0);
}
