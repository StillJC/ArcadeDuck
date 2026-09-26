// SPDX-FileCopyrightText: Olivier Galibert
// SPDX-FileCopyrightText: R. Belmont
// SPDX-FileCopyrightText: hap
// SPDX-FileCopyrightText: superctr
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: BSD-3-Clause
//
// Modified for ArcadeDuck by StillJC, 2026.
//
// Standalone ZOOM ZSG-2 wavetable core adapted from MAME's zsg2 device.

#pragma once

#include "core/types.h"

#include <array>
#include <span>
#include <vector>

namespace TaitoFX1BZSG2 {

class Chip
{
public:
  static constexpr u32 OUTPUT_COUNT = 4;
  static constexpr u32 CHANNEL_COUNT = 48;

  bool Initialize(std::span<const u8> sample_rom);
  void Reset();

  u16 Read(u32 offset);
  void Write(u32 offset, u16 data);
  void GenerateFrame(std::array<s32, OUTPUT_COUNT>* output);

private:
  static constexpr u16 STATUS_ACTIVE = 0x8000;

  struct Channel
  {
    std::array<u16, 16> v{};
    u16 status = 0;
    u32 cur_pos = 0;
    u32 step_ptr = 0;
    u32 step = 0;
    u32 start_pos = 0;
    u32 end_pos = 0;
    u32 loop_pos = 0;
    u32 page = 0;

    u16 vol = 0;
    u16 vol_initial = 0;
    u16 vol_target = 0;
    s16 vol_delta = 0;

    u16 output_cutoff = 0;
    u16 output_cutoff_initial = 0;
    u16 output_cutoff_target = 0;
    s16 output_cutoff_delta = 0;

    s32 emphasis_filter_state = 0;
    s32 output_filter_state = 0;

    std::array<u8, 4> output_gain{};
    std::array<s16, 5> samples{};
  };

  std::vector<u8> m_rom;
  std::vector<u32> m_mem_copy;
  std::vector<s16> m_full_samples;
  std::array<u16, 256> m_gain_tab{};
  std::array<u16, 32> m_reg{};
  std::array<Channel, CHANNEL_COUNT> m_chan{};
  u32 m_sample_count = 0;
  u32 m_read_address = 0;
  u32 m_mem_blocks = 0;

  u32 ReadMemory(u32 offset) const;
  s16* PrepareSamples(u32 offset);
  void FilterSamples(Channel* channel);
  void ChannelWrite(int channel, int reg, u16 data);
  u16 ChannelRead(int channel, int reg) const;
  void ControlWrite(int reg, u16 data);
  u16 ControlRead(int reg) const;
  static s16 GetRamp(u8 value);
  static u16 Ramp(u16 current, u16 target, s16 delta);
};

} // namespace TaitoFX1BZSG2
