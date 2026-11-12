#include "gt4_switch.hpp"

#include <cstdint>
#include <cstring>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace gt4::switch_app {
namespace {
#if defined(__SWITCH__)
AudioOutBuffer g_buffer{};
alignas(0x1000) std::uint8_t g_pcm[0x1000];
bool g_audio_ok = false;
#endif
AppSettings *g_settings = nullptr;
} // namespace

void audio_init(AppSettings &settings) {
  g_settings = &settings;
#if defined(__SWITCH__)
  g_audio_ok = false;
  if (!settings.audio_enabled) return;
  if (R_FAILED(audoutInitialize()) || R_FAILED(audoutStartAudioOut())) return;
  std::memset(g_pcm, 0, sizeof(g_pcm));
  g_buffer.next = nullptr;
  g_buffer.buffer = g_pcm;
  g_buffer.buffer_size = sizeof(g_pcm);
  g_buffer.data_size = sizeof(g_pcm);
  g_buffer.data_offset = 0;
  g_audio_ok = true;
#else
  (void)settings;
#endif
}

void audio_shutdown() {
#if defined(__SWITCH__)
  if (g_audio_ok) {
    audoutStopAudioOut();
    audoutExit();
    g_audio_ok = false;
  }
#endif
  g_settings = nullptr;
}

void audio_submit_silence() {
#if defined(__SWITCH__)
  if (!g_audio_ok || !g_settings || !g_settings->audio_enabled) return;
  audoutAppendAudioOutBuffer(&g_buffer);
  AudioOutBuffer *released = nullptr;
  u32 count = 0;
  audoutGetReleasedAudioOutBuffer(&released, &count);
#else
  (void)0;
#endif
}

} // namespace gt4::switch_app
