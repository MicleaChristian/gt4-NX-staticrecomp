#include "gt4_switch.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace gt4::switch_app {
namespace {
#if defined(__SWITCH__)
// The SPU2 mixes 48 kHz stereo Int16, the audout native format, so samples pass through unchanged.
// Six 1024-frame buffers (21.3 ms each) bound the queue to about 128 ms of latency.
constexpr int kBufferCount = 6;
constexpr std::size_t kFramesPerBuffer = 1024;
constexpr std::size_t kBufferBytes = kFramesPerBuffer * 2 * sizeof(std::int16_t); // 0x1000, audout alignment

alignas(0x1000) std::uint8_t g_pcm[kBufferCount][kBufferBytes];
AudioOutBuffer g_buffers[kBufferCount]{};
bool g_in_flight[kBufferCount]{};
int g_fill = -1;           // buffer being filled, -1 when none is free
std::size_t g_fill_bytes = 0;
bool g_audio_ok = false;

// Diagnostics for stderr.log: buffers dropped because all were queued (game faster than real time)
// and buffers submitted.
std::uint64_t g_dropped = 0;
std::uint64_t g_submitted = 0;

void reclaim_released() {
  for (;;) {
    AudioOutBuffer *released = nullptr;
    u32 count = 0;
    if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count)) || count == 0 || !released) return;
    for (int i = 0; i < kBufferCount; ++i)
      if (&g_buffers[i] == released) g_in_flight[i] = false;
  }
}

int take_free_buffer() {
  reclaim_released();
  for (int i = 0; i < kBufferCount; ++i)
    if (!g_in_flight[i]) return i;
  return -1;
}

void submit_fill() {
  AudioOutBuffer &b = g_buffers[g_fill];
  b.next = nullptr;
  b.buffer = g_pcm[g_fill];
  b.buffer_size = kBufferBytes;
  b.data_size = g_fill_bytes;
  b.data_offset = 0;
  if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&b))) {
    g_in_flight[g_fill] = true;
    ++g_submitted;
  }
  g_fill = -1;
  g_fill_bytes = 0;
}
#endif
AppSettings *g_settings = nullptr;
} // namespace

void audio_init(AppSettings &settings) {
  g_settings = &settings;
#if defined(__SWITCH__)
  g_audio_ok = false;
  if (!settings.audio_enabled) return;
  if (R_FAILED(audoutInitialize())) return;
  if (R_FAILED(audoutStartAudioOut())) {
    audoutExit();
    return;
  }
  std::memset(g_pcm, 0, sizeof(g_pcm));
  for (bool &f : g_in_flight) f = false;
  g_fill = -1;
  g_fill_bytes = 0;
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
  if (!g_audio_ok) return;
  const int slot = take_free_buffer();
  if (slot < 0) return;
  g_fill = slot;
  std::memset(g_pcm[slot], 0, kBufferBytes);
  g_fill_bytes = kBufferBytes;
  submit_fill();
#endif
}

void audio_push(const std::int16_t *stereo, std::size_t frames) {
#if defined(__SWITCH__)
  if (!g_audio_ok || !stereo) return;
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(stereo);
  std::size_t left = frames * 2 * sizeof(std::int16_t);
  while (left > 0) {
    if (g_fill < 0) {
      g_fill = take_free_buffer();
      g_fill_bytes = 0;
      if (g_fill < 0) {
        // Every buffer is queued: drop this block rather than stall the game.
        ++g_dropped;
        return;
      }
    }
    const std::size_t take = left < kBufferBytes - g_fill_bytes ? left : kBufferBytes - g_fill_bytes;
    std::memcpy(g_pcm[g_fill] + g_fill_bytes, bytes, take);
    g_fill_bytes += take;
    bytes += take;
    left -= take;
    if (g_fill_bytes == kBufferBytes) submit_fill();
  }
#else
  (void)stereo;
  (void)frames;
#endif
}

void audio_report(std::FILE *out) {
#if defined(__SWITCH__)
  if (!out) return;
  std::fprintf(out, "[audio] enabled=%d submitted=%llu dropped_blocks=%llu\n", g_audio_ok ? 1 : 0,
               static_cast<unsigned long long>(g_submitted), static_cast<unsigned long long>(g_dropped));
#else
  (void)out;
#endif
}

} // namespace gt4::switch_app
