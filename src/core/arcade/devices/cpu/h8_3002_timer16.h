#pragma once

#include "common/types.h"

#include <array>
#include <cstddef>
#include <optional>

namespace Arcade::H8 {

// H8/3002 five-channel Timer16.
//
// This is a framework-independent adaptation of the H8H Timer16 behavior used
// by MAME's H83002 device:
//   - TSTR reset 0xE0, five channels
//   - channel bases FF64/FF6E/FF78/FF82/FF92
//   - two active TGR compare registers per channel
//   - TBR shadow registers on channels 3/4
//   - H8H TCR divider/phase/auto-clear behavior
//   - H8H TIER/TSR semantics
//   - interrupt vectors 24/28/32/36/40
class H83002Timer16
{
public:
  static constexpr u16 REGISTER_BASE = UINT16_C(0xFF60);
  static constexpr u16 REGISTER_END = UINT16_C(0xFF9F);

  H83002Timer16() { Reset(); }

  void Reset()
  {
    m_total_clocks = 0;
    m_tstr = UINT8_C(0xE0);

    for (size_t i = 0; i < m_channels.size(); i++)
    {
      Channel& c = m_channels[i];
      c = {};
      c.tgr.fill(UINT16_C(0xFFFF));
      c.tbr.fill(UINT16_C(0xFFFF));
      c.counter_cycle = UINT32_C(0x10000);
      c.incrementing = true;
      c.enabled = ((m_tstr >> i) & 1) != 0;
    }
  }

  bool Handles(u16 reg) const
  {
    if (reg >= UINT16_C(0xFF60) && reg <= UINT16_C(0xFF63))
      return true;
    if (reg >= UINT16_C(0xFF90) && reg <= UINT16_C(0xFF91))
      return true;

    for (size_t i = 0; i < CHANNEL_BASES.size(); i++)
    {
      if (reg >= CHANNEL_BASES[i] && reg <= CHANNEL_ENDS[i])
        return true;
    }

    return false;
  }

  u8 Read(u16 reg)
  {
    switch (reg)
    {
      case UINT16_C(0xFF60):
        return m_tstr;
      case UINT16_C(0xFF61):
      case UINT16_C(0xFF62):
      case UINT16_C(0xFF63):
      case UINT16_C(0xFF90):
      case UINT16_C(0xFF91):
        return 0;
      default:
        break;
    }

    size_t index = 0;
    u16 offset = 0;
    if (!DecodeChannelRegister(reg, index, offset))
      return UINT8_C(0xFF);

    Channel& c = m_channels[index];
    UpdateChannel(index, m_total_clocks);

    switch (offset)
    {
      case 0: return c.tcr;
      case 1: return 0; // TIOR is read-zero in MAME's H8H Timer16 baseline.
      case 2: return c.tier;
      case 3:
        return static_cast<u8>(
          UINT8_C(0xF8) |
          ((c.isr & IRQ_V) ? UINT8_C(0x04) : 0) |
          ((c.isr & IRQ_B) ? UINT8_C(0x02) : 0) |
          ((c.isr & IRQ_A) ? UINT8_C(0x01) : 0));
      case 4: return static_cast<u8>(c.tcnt >> 8);
      case 5: return static_cast<u8>(c.tcnt);
      default:
        break;
    }

    if (offset >= 6 && offset <= 9)
    {
      const size_t tgr = static_cast<size_t>((offset - 6) >> 1);
      const u16 value = c.tgr[tgr];
      return ((offset - 6) & 1) ? static_cast<u8>(value) : static_cast<u8>(value >> 8);
    }

    // Channels 3 and 4 expose two TBR shadow registers after TGRB.
    if (index >= 3 && offset >= 10 && offset <= 13)
    {
      const size_t tbr = static_cast<size_t>((offset - 10) >> 1);
      const u16 value = c.tbr[tbr];
      return ((offset - 10) & 1) ? static_cast<u8>(value) : static_cast<u8>(value >> 8);
    }

    return UINT8_C(0xFF);
  }

  void Write(u16 reg, u8 data)
  {
    if (reg == UINT16_C(0xFF60))
    {
      for (size_t i = 0; i < m_channels.size(); i++)
        UpdateChannel(i, m_total_clocks);

      m_tstr = data;
      for (size_t i = 0; i < m_channels.size(); i++)
      {
        Channel& c = m_channels[i];
        const bool new_enable = ((m_tstr >> i) & 1) != 0;
        if (new_enable != c.enabled)
        {
          c.enabled = new_enable;
          c.last_clock_update = m_total_clocks;
        }
      }
      return;
    }

    if ((reg >= UINT16_C(0xFF61) && reg <= UINT16_C(0xFF63)) ||
        reg == UINT16_C(0xFF90) || reg == UINT16_C(0xFF91))
    {
      return;
    }

    size_t index = 0;
    u16 offset = 0;
    if (!DecodeChannelRegister(reg, index, offset))
      return;

    Channel& c = m_channels[index];
    UpdateChannel(index, m_total_clocks);

    switch (offset)
    {
      case 0:
        c.tcr = data;
        UpdateTCR(c);
        return;

      case 1:
        return;

      case 2:
        c.tier = static_cast<u8>(data | UINT8_C(0xF8));
        c.ier =
          static_cast<u8>(
            ((c.tier & UINT8_C(0x01)) ? IRQ_A : 0) |
            ((c.tier & UINT8_C(0x02)) ? IRQ_B : 0) |
            ((c.tier & UINT8_C(0x04)) ? IRQ_V : 0));
        return;

      case 3:
        if ((data & UINT8_C(0x01)) == 0)
          c.isr = static_cast<u8>(c.isr & static_cast<u8>(~IRQ_A));
        if ((data & UINT8_C(0x02)) == 0)
          c.isr = static_cast<u8>(c.isr & static_cast<u8>(~IRQ_B));
        if ((data & UINT8_C(0x04)) == 0)
          c.isr = static_cast<u8>(c.isr & static_cast<u8>(~IRQ_V));
        return;

      case 4:
        c.tcnt = static_cast<u16>((c.tcnt & UINT16_C(0x00FF)) |
                                  (static_cast<u16>(data) << 8));
        return;

      case 5:
        c.tcnt = static_cast<u16>((c.tcnt & UINT16_C(0xFF00)) |
                                  static_cast<u16>(data));
        return;

      default:
        break;
    }

    if (offset >= 6 && offset <= 9)
    {
      const size_t tgr = static_cast<size_t>((offset - 6) >> 1);
      if (((offset - 6) & 1) == 0)
        c.tgr[tgr] = static_cast<u16>((c.tgr[tgr] & UINT16_C(0x00FF)) |
                                      (static_cast<u16>(data) << 8));
      else
        c.tgr[tgr] = static_cast<u16>((c.tgr[tgr] & UINT16_C(0xFF00)) |
                                      static_cast<u16>(data));
      RecalculateCounterCycle(c);
      return;
    }

    if (index >= 3 && offset >= 10 && offset <= 13)
    {
      const size_t tbr = static_cast<size_t>((offset - 10) >> 1);
      if (((offset - 10) & 1) == 0)
        c.tbr[tbr] = static_cast<u16>((c.tbr[tbr] & UINT16_C(0x00FF)) |
                                      (static_cast<u16>(data) << 8));
      else
        c.tbr[tbr] = static_cast<u16>((c.tbr[tbr] & UINT16_C(0xFF00)) |
                                      static_cast<u16>(data));
    }
  }

  void Advance(u64 clocks)
  {
    if (clocks == 0)
      return;

    const u64 new_time = m_total_clocks + clocks;
    for (size_t i = 0; i < m_channels.size(); i++)
      UpdateChannel(i, new_time);

    m_total_clocks = new_time;
  }

  std::optional<u32> TakePendingInterrupt()
  {
    // MAME's H8H channel constructor maps:
    //   A=base, B=base+1, overflow=base+2.
    for (size_t i = 0; i < m_channels.size(); i++)
    {
      Channel& c = m_channels[i];
      const u32 base = IRQ_BASES[i];

      if ((c.pending & IRQ_A) != 0)
      {
        c.pending = static_cast<u8>(c.pending & static_cast<u8>(~IRQ_A));
        return base;
      }
      if ((c.pending & IRQ_B) != 0)
      {
        c.pending = static_cast<u8>(c.pending & static_cast<u8>(~IRQ_B));
        return base + 1;
      }
      if ((c.pending & IRQ_V) != 0)
      {
        c.pending = static_cast<u8>(c.pending & static_cast<u8>(~IRQ_V));
        return base + 2;
      }
    }

    return std::nullopt;
  }

private:
  static constexpr u8 IRQ_A = UINT8_C(0x01);
  static constexpr u8 IRQ_B = UINT8_C(0x02);
  static constexpr u8 IRQ_V = UINT8_C(0x10);

  static constexpr int CLEAR_NONE = -1;
  static constexpr int CLEAR_EXTERNAL = -2;

  struct Channel
  {
    u8 tcr = 0;
    u8 tier = 0;
    u8 ier = 0;
    u8 isr = 0;
    u8 pending = 0;
    u16 tcnt = 0;
    std::array<u16, 2> tgr{};
    std::array<u16, 2> tbr{};
    u64 last_clock_update = 0;
    u32 phase = 0;
    u32 counter_cycle = UINT32_C(0x10000);
    int clear_source = CLEAR_NONE;
    u8 clock_divider = 0;
    bool internal_clock = true;
    bool incrementing = true;
    bool enabled = false;
  };

  static constexpr std::array<u16, 5> CHANNEL_BASES = {
    UINT16_C(0xFF64), UINT16_C(0xFF6E), UINT16_C(0xFF78),
    UINT16_C(0xFF82), UINT16_C(0xFF92)
  };

  static constexpr std::array<u16, 5> CHANNEL_ENDS = {
    UINT16_C(0xFF6D), UINT16_C(0xFF77), UINT16_C(0xFF81),
    UINT16_C(0xFF8F), UINT16_C(0xFF9F)
  };

  static constexpr std::array<u32, 5> IRQ_BASES = {24, 28, 32, 36, 40};

  bool DecodeChannelRegister(u16 reg, size_t& index, u16& offset) const
  {
    for (size_t i = 0; i < CHANNEL_BASES.size(); i++)
    {
      if (reg >= CHANNEL_BASES[i] && reg <= CHANNEL_ENDS[i])
      {
        index = i;
        offset = static_cast<u16>(reg - CHANNEL_BASES[i]);
        return true;
      }
    }
    return false;
  }

  void UpdateTCR(Channel& c)
  {
    switch (c.tcr & UINT8_C(0x60))
    {
      case UINT8_C(0x00): c.clear_source = CLEAR_NONE; break;
      case UINT8_C(0x20): c.clear_source = 0; break;
      case UINT8_C(0x40): c.clear_source = 1; break;
      case UINT8_C(0x60): c.clear_source = CLEAR_EXTERNAL; break;
    }

    const u8 count_type = static_cast<u8>(c.tcr & UINT8_C(0x07));
    if (count_type < 4)
    {
      c.internal_clock = true;
      c.clock_divider = count_type;

      if (count_type <= 1)
      {
        c.phase = 0;
      }
      else
      {
        switch (c.tcr & UINT8_C(0x18))
        {
          case UINT8_C(0x00):
            c.phase = 0;
            break;
          case UINT8_C(0x08):
            c.phase = UINT32_C(1) << (c.clock_divider - 1);
            break;
          case UINT8_C(0x10):
          case UINT8_C(0x18):
            c.phase = 0;
            c.clock_divider--;
            break;
        }
      }
    }
    else
    {
      // INPUT_A..INPUT_D. The System 12 H8 firmware uses internal clock modes;
      // preserve the external-input mode state instead of inventing pulses.
      c.internal_clock = false;
      c.clock_divider = 0;
      c.phase = 0;
    }

    RecalculateCounterCycle(c);
  }

  void RecalculateCounterCycle(Channel& c)
  {
    if (c.clear_source >= 0)
      c.counter_cycle = static_cast<u32>(c.tgr[static_cast<size_t>(c.clear_source)]) + 1;
    else
      c.counter_cycle = UINT32_C(0x10000);
  }

  void RequestEvent(Channel& c, u8 source)
  {
    c.isr = static_cast<u8>(c.isr | source);
    if ((c.ier & source) != 0)
      c.pending = static_cast<u8>(c.pending | source);
  }

  void UpdateChannel(size_t index, u64 new_time)
  {
    Channel& c = m_channels[index];

    if (!c.enabled || !c.internal_clock || !c.incrementing)
    {
      c.last_clock_update = new_time;
      return;
    }

    u64 base_time = c.last_clock_update;
    c.last_clock_update = new_time;

    if (new_time <= base_time)
      return;

    u64 scaled_base = base_time;
    u64 scaled_new = new_time;

    if (c.clock_divider)
    {
      scaled_base = (scaled_base + c.phase) >> c.clock_divider;
      scaled_new = (scaled_new + c.phase) >> c.clock_divider;
    }

    if (scaled_new == scaled_base)
      return;

    RecalculateCounterCycle(c);

    const u16 previous = c.tcnt;
    const u64 delta = scaled_new - scaled_base;
    const u64 total = static_cast<u64>(c.tcnt) + delta;

    if (previous >= c.counter_cycle)
    {
      if (total >= UINT64_C(0x10000))
        c.tcnt = static_cast<u16>((total - UINT64_C(0x10000)) % c.counter_cycle);
      else
        c.tcnt = static_cast<u16>(total);
    }
    else
    {
      c.tcnt = static_cast<u16>(total % c.counter_cycle);
    }

    for (size_t i = 0; i < c.tgr.size(); i++)
    {
      const u16 compare = static_cast<u16>(c.tgr[i] + 1);
      bool match =
        c.tcnt == compare ||
        (total == compare && total == c.counter_cycle);

      if (!match)
      {
        if (previous >= c.counter_cycle)
        {
          match =
            (compare > previous && total >= compare) ||
            (compare <= c.counter_cycle && c.tcnt < c.counter_cycle &&
             (delta - (UINT64_C(0x10000) - previous)) >= compare);
        }
        else if (compare <= c.counter_cycle)
        {
          match =
            delta >= c.counter_cycle ||
            (previous < compare && total >= compare) ||
            (c.tcnt <= previous && c.tcnt >= compare);
        }
      }

      if (match)
        RequestEvent(c, i == 0 ? IRQ_A : IRQ_B);
    }

    if (total >= UINT64_C(0x10000) &&
        (c.counter_cycle == UINT32_C(0x10000) || previous >= c.counter_cycle))
    {
      RequestEvent(c, IRQ_V);
    }
  }

  std::array<Channel, 5> m_channels{};
  u64 m_total_clocks = 0;
  u8 m_tstr = UINT8_C(0xE0);
};

} // namespace Arcade::H8