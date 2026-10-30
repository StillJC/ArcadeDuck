// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>
#include <memory>

class StateWrapper;

namespace Arcade {

// Sanyo LC82310 MP3 decoder. A host CPU sends 8-bit commands (LSB first) over
// a clocked serial control port and feeds the MPEG stream byte by byte while
// the decoder requests data. Decoded audio is produced at 44.1 kHz.
class LC82310
{
public:
  bool DoState(StateWrapper& sw);

  static constexpr u32 OUTPUT_RATE = 44100;

  LC82310();
  ~LC82310();

  void Reset();

  // Serial control port: chip select (active low), clock, data in, data out.
  void SetChipSelect(bool state) { m_cs = state; }
  void SetClock(bool state);
  void SetDataIn(bool state) { m_di = state; }
  bool GetDataOut() const { return m_do; }

  // DEMAND output: the decoder accepts more stream data. It is also released
  // once each time the decoder starts a frame; ReadDemand() is a host poll,
  // which observes that release.
  bool GetDemand() const;
  bool ReadDemand();
  void WriteData(u8 value);

  // Discards buffered stream data and decoded samples.
  void ResetPlayback();

  // One output frame at OUTPUT_RATE.
  void GenerateFrame(s32* left, s32* right);

private:
  struct Decoder;

  static constexpr u32 BUFFER_SIZE = 0x1000;
  static constexpr u32 MAX_FRAME_SAMPLES = 1152;

  void HandleCommand(u8 command, u8 parameter);
  bool FindStream();
  bool DecodeFrame();

  std::unique_ptr<Decoder> m_decoder;

  std::array<u8, BUFFER_SIZE> m_buffer{};
  u32 m_buffer_count = 0;
  bool m_stream_found = false;
  bool m_frame_release = false;

  std::array<s16, MAX_FRAME_SAMPLES * 2> m_samples{};
  u32 m_sample_count = 0;
  u32 m_sample_index = 0;
  u32 m_frame_rate = OUTPUT_RATE;
  u32 m_rate_phase = 0;
  std::array<float, 2> m_gain{};

  bool m_cs = true;
  bool m_clock = false;
  bool m_di = false;
  bool m_do = false;
  bool m_accepting_parameter = false;
  u8 m_command = 0;
  u8 m_shift = 0;
  u32 m_bits = 0;
  u8 m_out_shift = 0;
};

} // namespace Arcade
