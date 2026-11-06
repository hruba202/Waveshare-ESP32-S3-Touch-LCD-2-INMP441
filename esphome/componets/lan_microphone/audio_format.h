#pragma once
#include <array>
#include <cstdint>
#include <limits>

namespace esphome::lan_microphone {
constexpr int64_t scaled_sample(int32_t raw, uint16_t gain) {
  return static_cast<int64_t>(raw) * gain / 65536;
}
constexpr int16_t pcm16(int32_t raw, uint16_t gain) {
  const int64_t v = scaled_sample(raw, gain);
  return static_cast<int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}
constexpr std::array<uint8_t, 44> wav_header() {
  // 16 kHz, mono, signed PCM16 little endian; live WAV length as in VisionState tests.
  return {'R','I','F','F', 0xff,0xff,0xff,0x7f, 'W','A','V','E',
          'f','m','t',' ', 16,0,0,0, 1,0, 1,0, 0x80,0x3e,0,0,
          0,0x7d,0,0, 2,0, 16,0, 'd','a','t','a', 0xdb,0xff,0xff,0x7f};
}
static_assert(pcm16(0, 1) == 0);
static_assert(pcm16(std::numeric_limits<int32_t>::min(), 1) == -32768);
static_assert(pcm16(std::numeric_limits<int32_t>::max(), 1) == 32767);
static_assert(pcm16(65536, 1) == 1 && pcm16(-65536, 1) == -1);
static_assert(pcm16(1073741824, 4) == 32767 && pcm16(-1073741824, 4) == -32768);
static_assert(pcm16(536870912, 2) == 16384);
static_assert(wav_header()[22] == 1 && wav_header()[34] == 16);
}  // namespace esphome::lan_microphone
