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

#include "core/pch.h"
#include "core/arcade/systems/sony/zn/taito_fx1b_zsg2.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace TaitoFX1BZSG2 {

namespace {

constexpr s32 EMPHASIS_INITIAL_BIAS = 0;
constexpr u32 EMPHASIS_FILTER_SHIFT = 6;
constexpr s32 EMPHASIS_ROUNDING = 0x20;
constexpr u32 EMPHASIS_OUTPUT_SHIFT = 1;

} // namespace

bool Chip::Initialize(std::span<const u8> sample_rom)
{
  if (sample_rom.empty() || (sample_rom.size() & 3) != 0)
    return false;

  m_rom.assign(sample_rom.begin(), sample_rom.end());
  m_mem_blocks = static_cast<u32>(m_rom.size() / 4);
  m_mem_copy.assign(m_mem_blocks, 0);
  m_full_samples.assign(static_cast<size_t>(m_mem_blocks) * 4 + 4, 0);

  m_gain_tab.fill(0);
  for (int i = 1; i < 32; i++)
  {
    const double value = std::pow(10.0, -(31 - i) / 20.0) * 65535.0;
    m_gain_tab[i] = static_cast<u16>(value);
  }

  Reset();
  return true;
}

void Chip::Reset()
{
  m_reg.fill(0);
  m_read_address = 0;
  m_sample_count = 0;
  for (Channel& channel : m_chan)
    channel = {};

  // MAME's device reset explicitly keys off all 48 channels.
  ControlWrite(4, UINT16_C(0xffff));
  ControlWrite(5, UINT16_C(0xffff));
  ControlWrite(6, UINT16_C(0xffff));
}

u32 Chip::ReadMemory(u32 offset) const
{
  if (offset >= m_mem_blocks)
    return 0;

  const size_t byte = static_cast<size_t>(offset) * 4;
  return static_cast<u32>(m_rom[byte]) |
         (static_cast<u32>(m_rom[byte + 1]) << 8) |
         (static_cast<u32>(m_rom[byte + 2]) << 16) |
         (static_cast<u32>(m_rom[byte + 3]) << 24);
}

s16* Chip::PrepareSamples(u32 offset)
{
  if (offset >= m_mem_blocks)
    return &m_full_samples[static_cast<size_t>(m_mem_blocks) * 4];

  const u32 block = ReadMemory(offset);
  if (block == 0)
    return &m_full_samples[static_cast<size_t>(m_mem_blocks) * 4];

  if (block == m_mem_copy[offset])
    return &m_full_samples[static_cast<size_t>(offset) * 4];

  m_mem_copy[offset] = block;
  size_t base = static_cast<size_t>(offset) * 4;

  m_full_samples[base | 0] = static_cast<s16>((block >> 8) & 0x7f);
  m_full_samples[base | 1] = static_cast<s16>((block >> 16) & 0x7f);
  m_full_samples[base | 2] = static_cast<s16>((block >> 24) & 0x7f);
  m_full_samples[base | 3] = static_cast<s16>(((block >> 9) & 0x40) | ((block >> 18) & 0x20) |
                                              ((block >> 27) & 0x10) | (block & 0x0f));

  const u8 shift = static_cast<u8>((block >> 4) & 0x0f);
  for (size_t i = base; i < base + 4; i++)
  {
    // The ZSG-2 code is a signed 7-bit sample. Left-align it into the
    // signed 16-bit sample domain before scaling. This intentionally
    // mirrors the reference core's int16_t narrowing before >> shift.
    s32 sample = (static_cast<s32>(m_full_samples[i]) << 9) & 0xffff;
    if (sample & 0x8000)
      sample -= 0x10000;
    sample >>= shift;
    m_full_samples[i] = static_cast<s16>(sample);
  }

  return &m_full_samples[base];
}

void Chip::FilterSamples(Channel* channel)
{
  s16* raw = PrepareSamples(channel->page | channel->cur_pos);
  channel->samples[0] = channel->samples[4];

  for (int i = 0; i < 4; i++)
  {
    channel->emphasis_filter_state +=
      static_cast<s32>(raw[i]) - ((channel->emphasis_filter_state + EMPHASIS_ROUNDING) >> EMPHASIS_FILTER_SHIFT);

    const s32 sample = channel->emphasis_filter_state >> EMPHASIS_OUTPUT_SHIFT;
    channel->samples[i + 1] = static_cast<s16>(std::clamp<s32>(sample, -32768, 32767));
  }
}

void Chip::GenerateFrame(std::array<s32, OUTPUT_COUNT>* output)
{
  if (!output)
    return;

  output->fill(0);

  for (Channel& channel : m_chan)
  {
    if ((channel.status & STATUS_ACTIVE) == 0)
      continue;

    channel.step_ptr += channel.step;
    if (channel.step_ptr & 0xffff0000u)
    {
      if (++channel.cur_pos >= channel.end_pos)
      {
        channel.cur_pos = channel.loop_pos;
        if ((channel.cur_pos + 1) >= channel.end_pos)
        {
          channel.vol = 0;
          channel.status &= ~STATUS_ACTIVE;
          continue;
        }
      }

      if (channel.cur_pos == channel.start_pos)
        channel.emphasis_filter_state = EMPHASIS_INITIAL_BIAS;

      channel.step_ptr &= 0xffffu;
      FilterSamples(&channel);
    }

    const u8 sample_pos = static_cast<u8>((channel.step_ptr >> 14) & 3);
    s32 sample = channel.samples[sample_pos];
    sample +=
      (static_cast<u16>(channel.step_ptr << 2) *
       static_cast<s16>(channel.samples[sample_pos + 1] - sample)) >> 16;

    channel.output_filter_state +=
      (sample - (channel.output_filter_state >> 16)) * channel.output_cutoff;
    sample = channel.output_filter_state >> 16;

    if (!channel.output_cutoff)
      channel.output_filter_state >>= 1;

    sample = (sample * channel.vol) >> 16;

    for (u32 index = 0; index < OUTPUT_COUNT; index++)
    {
      const u8 gain_register = channel.output_gain[index];
      s32 output_sample = sample;
      if (gain_register & 0x80)
        output_sample = -output_sample;

      (*output)[index] +=
        (output_sample * static_cast<s32>(m_gain_tab[gain_register & 0x1f])) >> 16;
    }

    // The standalone backend asks the ZSG-2 for one native frame per update,
    // making this equivalent to MAME's "every other update" ramp cadence.
    if (m_sample_count & 1)
    {
      channel.vol = Ramp(channel.vol, channel.vol_target, channel.vol_delta);
      channel.output_cutoff =
        Ramp(channel.output_cutoff, channel.output_cutoff_target, channel.output_cutoff_delta);
    }
  }

  // ZSG-2 exposes four signed 16-bit stream outputs. The reference
  // device clamps the accumulated 48-channel mix at this boundary
  // before routing it to the TMS57002.
  for (s32& mixed : *output)
    mixed = std::clamp<s32>(mixed, -32768, 32767);

  m_sample_count++;
}

void Chip::ChannelWrite(int ch, int reg, u16 data)
{
  Channel& channel = m_chan[ch];

  switch (reg)
  {
    case 0x0:
      channel.start_pos = (channel.start_pos & 0xff00u) | ((data >> 8) & 0xffu);
      break;
    case 0x1:
      channel.start_pos = (channel.start_pos & 0x00ffu) | ((static_cast<u32>(data) << 8) & 0xff00u);
      channel.page = (static_cast<u32>(data) << 8) & 0xff0000u;
      break;
    case 0x2:
      break;
    case 0x3:
      channel.status = static_cast<u16>((channel.status & 0x8000u) | (data & 0x7fffu));
      break;
    case 0x4:
      channel.step = static_cast<u32>(data) + 1;
      break;
    case 0x5:
      channel.loop_pos = (channel.loop_pos & 0xff00u) | (data & 0xffu);
      channel.output_gain[3] = static_cast<u8>(data >> 8);
      break;
    case 0x6:
      channel.end_pos = data;
      break;
    case 0x7:
      channel.loop_pos = (channel.loop_pos & 0x00ffu) | ((static_cast<u32>(data) << 8) & 0xff00u);
      channel.output_gain[2] = static_cast<u8>(data >> 8);
      break;
    case 0x8:
      channel.output_cutoff_initial = data;
      break;
    case 0x9:
      channel.output_cutoff = data;
      break;
    case 0xa:
      channel.vol_initial = data;
      break;
    case 0xb:
      channel.vol = data;
      break;
    case 0xc:
      channel.output_cutoff_target = data;
      break;
    case 0xd:
      channel.output_gain[1] = static_cast<u8>(data >> 8);
      channel.output_cutoff_delta = GetRamp(static_cast<u8>(data));
      break;
    case 0xe:
      channel.vol_target = data;
      break;
    case 0xf:
      channel.output_gain[0] = static_cast<u8>(data >> 8);
      channel.vol_delta = GetRamp(static_cast<u8>(data));
      break;
  }

  channel.v[reg] = data;
}

u16 Chip::ChannelRead(int ch, int reg) const
{
  const Channel& channel = m_chan[ch];

  switch (reg)
  {
    case 0x3: return channel.status;
    case 0x9: return channel.output_cutoff;
    case 0xb: return channel.vol;
    default: return channel.v[reg];
  }
}

s16 Chip::GetRamp(u8 value)
{
  s16 fraction = static_cast<s16>(static_cast<u16>(value) << 12);
  fraction = static_cast<s16>(((fraction >> 12) ^ 8) << (value >> 4));
  return static_cast<s16>(fraction >> 4);
}

u16 Chip::Ramp(u16 current, u16 target, s16 delta)
{
  s32 value = static_cast<s32>(current) + delta;
  if (delta < 0 && value < target)
    value = target;
  else if (delta >= 0 && value > target)
    value = target;
  return static_cast<u16>(value);
}

void Chip::ControlWrite(int reg, u16 data)
{
  switch (reg)
  {
    case 0x00:
    case 0x01:
    case 0x02:
    {
      const int base = (reg & 3) << 4;
      for (int i = 0; i < 16; i++)
      {
        if (data & (1u << i))
        {
          Channel& channel = m_chan[base | i];
          channel.status |= STATUS_ACTIVE;
          channel.cur_pos = channel.start_pos - 1;
          channel.step_ptr = 0x10000;
          channel.vol = 0;
          channel.vol_delta = 0x0400;
          channel.output_cutoff = channel.output_cutoff_initial;
          channel.output_filter_state = 0;
        }
      }
      break;
    }

    case 0x04:
    case 0x05:
    case 0x06:
    {
      const int base = (reg & 3) << 4;
      for (int i = 0; i < 16; i++)
      {
        if (data & (1u << i))
        {
          Channel& channel = m_chan[base | i];
          channel.vol = 0;
          channel.status &= ~STATUS_ACTIVE;
        }
      }
      break;
    }

    case 0x1c:
      m_read_address = (m_read_address & 0x3fffc000u) | ((data >> 2) & 0x00003fffu);
      break;

    case 0x1d:
      m_read_address = (m_read_address & 0x00003fffu) | ((static_cast<u32>(data) << 14) & 0x3fffc000u);
      break;

    default:
      if (reg < 0x20)
        m_reg[reg] = data;
      break;
  }
}

u16 Chip::ControlRead(int reg) const
{
  switch (reg)
  {
    case 0x14:
      return 0;
    case 0x1e:
      return static_cast<u16>(ReadMemory(m_read_address));
    case 0x1f:
      return static_cast<u16>(ReadMemory(m_read_address) >> 16);
    default:
      if (reg < 0x20)
        return m_reg[reg];
      return 0;
  }
}

void Chip::Write(u32 offset, u16 data)
{
  if (offset < 0x300)
    ChannelWrite(static_cast<int>(offset >> 4), static_cast<int>(offset & 0x0f), data);
  else
    ControlWrite(static_cast<int>(offset - 0x300), data);
}

u16 Chip::Read(u32 offset)
{
  if (offset < 0x300)
    return ChannelRead(static_cast<int>(offset >> 4), static_cast<int>(offset & 0x0f));
  return ControlRead(static_cast<int>(offset - 0x300));
}

} // namespace TaitoFX1BZSG2
