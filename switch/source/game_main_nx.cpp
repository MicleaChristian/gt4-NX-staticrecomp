// TASK-174 S3/S4: strong Switch entry — donor boot + soft framebuffer present.
#include "runtime_link.hpp"

#include "platform/nx/deko3d_present.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#endif

void register_generated_code();
void register_generated_vu();

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

// DualShock digital bits (pressed = 1), matching the Windows pad path / sio2.
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
  if (g_last_app_dir.empty()) return;
  const std::string path = g_last_app_dir + "/last_boot.txt";
  if (FILE *f = std::fopen(path.c_str(), "wb")) {
    std::fprintf(f, "frames=%llu\nhalted=%d\nwhy=%s\n",
                 static_cast<unsigned long long>(ps2::frame_count()), ps2::halted() ? 1 : 0, why ? why : "");
    std::fclose(f);
  }
}

std::uint8_t stick_to_ps2(float v, bool invert) {
  const float s = invert ? -v : v;
  const int t = static_cast<int>(128.f + s * 127.f);
  if (t < 0) return 0;
  if (t > 255) return 255;
  return static_cast<std::uint8_t>(t);
}

void feed_guest_pad() {
  padUpdate(&g_boot_pad);
  const u64 down = padGetButtons(&g_boot_pad);
  std::uint16_t mask = 0;
  if (down & HidNpadButton_B) mask |= kBtnCross;      // Switch B = Cross (US confirm)
  if (down & HidNpadButton_A) mask |= kBtnCircle;     // Switch A = Circle
  if (down & HidNpadButton_Y) mask |= kBtnSquare;
  if (down & HidNpadButton_X) mask |= kBtnTriangle;
  if (down & HidNpadButton_Plus) mask |= kBtnStart;
  if (down & HidNpadButton_Minus) mask |= kBtnSelect;
  if (down & HidNpadButton_L) mask |= kBtnL1;
  if (down & HidNpadButton_R) mask |= kBtnR1;
  if (down & HidNpadButton_ZL) mask |= kBtnL2;
  if (down & HidNpadButton_ZR) mask |= kBtnR2;
  if (down & HidNpadButton_StickL) mask |= kBtnL3;
  if (down & HidNpadButton_StickR) mask |= kBtnR3;
  if (down & HidNpadButton_Up) mask |= kBtnUp;
  if (down & HidNpadButton_Down) mask |= kBtnDown;
  if (down & HidNpadButton_Left) mask |= kBtnLeft;
  if (down & HidNpadButton_Right) mask |= kBtnRight;

  ps2::set_pad_buttons(mask);
  // Pressures: full when digital button is down (menus need Cross pressure on some paths).
  for (int b = 0; b < 16; ++b)
    ps2::set_pad_pressure(b, (mask & (1u << b)) ? 255 : 0);

  const HidAnalogStickState left = padGetStickPos(&g_boot_pad, 0);
  const HidAnalogStickState right = padGetStickPos(&g_boot_pad, 1);
  const float lx = static_cast<float>(left.x) / 32767.f;
  const float ly = static_cast<float>(-left.y) / 32767.f; // up positive
  const float rx = static_cast<float>(right.x) / 32767.f;
  const float ry = static_cast<float>(-right.y) / 32767.f;
  // PS2 order: RX, RY, LX, LY — 0 = left/up, 255 = right/down.
  ps2::set_pad_axis(0, stick_to_ps2(rx, false));
  ps2::set_pad_axis(1, stick_to_ps2(ry, true));
  ps2::set_pad_axis(2, stick_to_ps2(lx, false));
  ps2::set_pad_axis(3, stick_to_ps2(ly, true));
}

// Pump Atmosphere, feed pad into the guest, allow hold-Plus abort, blit GS + cyan OSD.
void switch_host_vblank() {
  if (!appletMainLoop()) {
    write_halt_log("host requested exit");
    ps2::halt("host requested exit");
    return;
  }
  const std::uint64_t frame = ps2::frame_count();
  feed_guest_pad();
  // Hold Plus for abort (Start is also Plus for the guest; short taps stay in-game Start).
  static int plus_held = 0;
  if (padGetButtons(&g_boot_pad) & HidNpadButton_Plus) {
    if (frame > 45) ++plus_held;
  } else {
    plus_held = 0;
  }
  if (plus_held > 90) { // ~1.5s hold
    write_halt_log("plus hold abort");
    ps2::halt("plus hold abort");
    return;
  }

  // Always refresh the cyan OSD every guest frame. GS blit is expensive on CPU raster,
  // so only pull a guest frame every 4th vblank (OSD still moves every frame).
  std::uint32_t w = 0, h = 0;
  const bool try_gs = (frame <= 3) || ((frame % 4) == 0);
  if (try_gs && ps2::display_frame(g_frame_rgba, w, h) && w > 0 && h > 0) {
    gt4::platform::nx::deko3d_present_frame(reinterpret_cast<const std::uint8_t *>(g_frame_rgba.data()),
                                            g_frame_rgba.size() * 4u, static_cast<int>(w), static_cast<int>(h),
                                            frame);
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
          "Runtime linked. A=300f, X=long. libnx present (CPU GS). Plus aborts.");
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
  ps2::g_options.on_vblank = switch_host_vblank;
#endif

  std::uint32_t entry = 0;
  if (!ps2::load_elf(elf.c_str(), entry)) {
    std::fprintf(stderr, "[gt4-switch] load_elf failed for %s\n", elf.c_str());
#if defined(__SWITCH__)
    ps2::g_options.on_vblank = nullptr;
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
  std::snprintf(why, sizeof why, "run returned rc=%d frames=%llu halted=%d", rc,
                static_cast<unsigned long long>(ps2::frame_count()), ps2::halted() ? 1 : 0);
  write_halt_log(why);
  gt4::platform::nx::soft_present_end();
  appletLockExit();
#endif
  return rc;
}

extern "C" int gt4_switch_game_main(const char *iso_path_utf8, const char *app_dir_utf8) {
  return gt4_switch_game_main_frames(iso_path_utf8, app_dir_utf8, 0);
}
