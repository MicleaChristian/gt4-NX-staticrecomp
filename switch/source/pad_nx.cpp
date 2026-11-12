#include "gt4_switch.hpp"

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace gt4::switch_app {
namespace {
#if defined(__SWITCH__)
::PadState g_nx_pad{};
#endif
} // namespace

void pad_init() {
#if defined(__SWITCH__)
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_nx_pad);
#endif
}

void pad_shutdown() {}

InputState pad_poll() {
  InputState out{};
#if defined(__SWITCH__)
  padUpdate(&g_nx_pad);
  const u64 down = padGetButtons(&g_nx_pad);
  const HidAnalogStickState left = padGetStickPos(&g_nx_pad, 0);
  const HidAnalogStickState right = padGetStickPos(&g_nx_pad, 1);
  out.cross = (down & HidNpadButton_B) != 0;   // Switch B ~ PS Cross (also confirm on some UIs)
  out.circle = (down & HidNpadButton_A) != 0;  // Switch A ~ PS Circle
  out.a = out.circle;
  out.b = out.cross;
  out.square = (down & HidNpadButton_Y) != 0;
  out.triangle = (down & HidNpadButton_X) != 0;
  out.start = (down & HidNpadButton_Plus) != 0;
  out.select = (down & HidNpadButton_Minus) != 0;
  out.l1 = (down & HidNpadButton_L) != 0;
  out.r1 = (down & HidNpadButton_R) != 0;
  out.l2 = (down & HidNpadButton_ZL) ? 1.f : 0.f;
  out.r2 = (down & HidNpadButton_ZR) ? 1.f : 0.f;
  out.lx = static_cast<float>(left.x) / 32767.f;
  out.ly = static_cast<float>(-left.y) / 32767.f;
  out.rx = static_cast<float>(right.x) / 32767.f;
  out.ry = static_cast<float>(-right.y) / 32767.f;
#endif
  return out;
}

} // namespace gt4::switch_app
