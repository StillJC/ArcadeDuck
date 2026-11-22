// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-FileCopyrightText: Wilbert Pol
// SPDX-FileCopyrightText: Felipe Sanches
// SPDX-License-Identifier: GPL-3.0-only
//
// Register map, interrupt priority/vector table, timer, A/D and micro-DMA
// behavior follow MAME's BSD-3-Clause tmp95c061.cpp and the Toshiba TMP95C061
// data book. Serial transfer timing follows the data book.

#include "core/arcade/devices/cpu/tmp95c061.h"

#include "core/arcade/third_party/tlcs900h/tlcs900h_arcadeduck.h"

#include "common/assert.h"

#include "util/state_wrapper.h"

#include <algorithm>
#include <cstring>

namespace Arcade
{
namespace
{
enum IntReg : u8
{
  INTE0AD = 0,
  INTE45,
  INTE67,
  INTET10,
  INTET32,
  INTET54,
  INTET76,
  INTES0,
  INTES1,
  INTETC10,
  INTETC32,
};

struct IrqSource
{
  u8 reg;
  u8 flag;
  u8 vector;
};

// Maskable sources in descending default priority.
constexpr std::array<IrqSource, 22> IRQ_SOURCES = {{
  {INTETC32, 0x80, 0x80}, // INTTC3
  {INTETC32, 0x08, 0x7c}, // INTTC2
  {INTETC10, 0x80, 0x78}, // INTTC1
  {INTETC10, 0x08, 0x74}, // INTTC0
  {INTE0AD, 0x80, 0x70},  // INTAD
  {INTES1, 0x80, 0x6c},   // INTTX1
  {INTES1, 0x08, 0x68},   // INTRX1
  {INTES0, 0x80, 0x64},   // INTTX0
  {INTES0, 0x08, 0x60},   // INTRX0
  {INTET76, 0x80, 0x5c},  // INTTR7
  {INTET76, 0x08, 0x58},  // INTTR6
  {INTET54, 0x80, 0x54},  // INTTR5
  {INTET54, 0x08, 0x50},  // INTTR4
  {INTET32, 0x80, 0x4c},  // INTT3
  {INTET32, 0x08, 0x48},  // INTT2
  {INTET10, 0x80, 0x44},  // INTT1
  {INTET10, 0x08, 0x40},  // INTT0
  {INTE67, 0x80, 0x38},   // INT7
  {INTE67, 0x08, 0x34},   // INT6
  {INTE45, 0x80, 0x30},   // INT5
  {INTE45, 0x08, 0x2c},   // INT4
  {INTE0AD, 0x08, 0x28},  // INT0
}};

// IRQ_SOURCES index for each vector / 4, or -1.
constexpr std::array<s8, 64> MakeVectorSources()
{
  std::array<s8, 64> table{};
  for (s8& entry : table)
    entry = -1;
  for (size_t i = 0; i < IRQ_SOURCES.size(); i++)
    table[IRQ_SOURCES[i].vector >> 2] = static_cast<s8>(i);
  return table;
}
constexpr std::array<s8, 64> VECTOR_SOURCES = MakeVectorSources();

constexpr size_t PortIndex(TMP95C061::Port port)
{
  return static_cast<size_t>(port);
}

// Prescaler taps in fc periods: phiT1 = fc/8, phiT4 = fc/32, phiT16 = fc/128, phiT256 = fc/2048.
constexpr u32 T1_SHIFT = 3;
constexpr u32 T4_SHIFT = 5;
constexpr u32 T16_SHIFT = 7;
constexpr u32 T256_SHIFT = 11;

enum : u32
{
  FF_CLEAR,
  FF_SET,
  FF_INVERT,
};

TMP95C061* s_bound_device = nullptr;
} // namespace

TMP95C061::TMP95C061(u32 clock_hz) : m_clock_hz(clock_hz)
{
  ResetPeripherals();
}

TMP95C061::~TMP95C061()
{
  if (s_bound_device == this)
    s_bound_device = nullptr;
}

void TMP95C061::SetCallbacks(const Callbacks& callbacks)
{
  m_callbacks = callbacks;
}

void TMP95C061::SaveCoreContext()
{
  m_core_context.clear();
  ArcadeDuckTLCS900HDoState(
    [](void* context, void* data, size_t size) {
      std::vector<u8>& buffer = *static_cast<std::vector<u8>*>(context);
      const u8* const bytes = static_cast<const u8*>(data);
      buffer.insert(buffer.end(), bytes, bytes + size);
    },
    &m_core_context);
}

void TMP95C061::LoadCoreContext()
{
  if (m_core_context.empty())
    return;

  struct Cursor
  {
    const std::vector<u8>* buffer;
    size_t offset;
  } cursor = {&m_core_context, 0};
  ArcadeDuckTLCS900HDoState(
    [](void* context, void* data, size_t size) {
      Cursor& c = *static_cast<Cursor*>(context);
      std::memcpy(data, c.buffer->data() + c.offset, size);
      c.offset += size;
    },
    &cursor);
}

void TMP95C061::BindCore()
{
  if (s_bound_device == this)
    return;

  // The TLCS-900/H interpreter is a single global instance: park the previous device's CPU context and resume this
  // device's own.
  if (s_bound_device)
    s_bound_device->SaveCoreContext();
  LoadCoreContext();

  ArcadeDuckTLCS900HCallbacks callbacks = {};
  callbacks.context = this;
  callbacks.read8 = &TMP95C061::BusRead8;
  callbacks.read16 = &TMP95C061::BusRead16;
  callbacks.read32 = &TMP95C061::BusRead32;
  callbacks.write8 = &TMP95C061::BusWrite8;
  callbacks.write16 = &TMP95C061::BusWrite16;
  callbacks.write32 = &TMP95C061::BusWrite32;
  callbacks.control_read = &TMP95C061::ControlRead;
  callbacks.control_write = &TMP95C061::ControlWrite;
  ArcadeDuckTLCS900HSetCallbacks(&callbacks);
  s_bound_device = this;
}

void TMP95C061::SetReset(bool asserted)
{
  if (asserted)
  {
    m_in_reset = true;
    return;
  }

  if (!m_in_reset)
    return;

  BindCore();
  ResetPeripherals();
  m_in_reset = false;
  m_state_balance = 0;
  m_irq_inhibit = false;

  const u32 reset_pc = static_cast<u32>(Read8(0xffff00)) | (static_cast<u32>(Read8(0xffff01)) << 8) |
                       (static_cast<u32>(Read8(0xffff02)) << 16);
  ArcadeDuckTLCS900HReset(reset_pc, 0x000100);
}

u32 TMP95C061::GetPC() const
{
  return (s_bound_device == this) ? ArcadeDuckTLCS900HGetPC() : 0;
}

void TMP95C061::ResetPeripherals()
{
  m_to1 = 0;
  m_to3 = 0;
  m_prescaler = 0;
  m_ad_states_left = 0;

  m_port_latch[PortIndex(Port::P1)] = 0x00;
  m_port_latch[PortIndex(Port::P2)] = 0xff;
  m_port_latch[PortIndex(Port::P5)] = 0x3d;
  m_port_latch[PortIndex(Port::P6)] = 0x3b;
  m_port_latch[PortIndex(Port::P7)] = 0xff;
  m_port_latch[PortIndex(Port::P8)] = 0x3f;
  m_port_latch[PortIndex(Port::P9)] = 0x00;
  m_port_latch[PortIndex(Port::PA)] = 0x0f;
  m_port_latch[PortIndex(Port::PB)] = 0xff;
  m_port_control.fill(0);
  m_port_function.fill(0);

  m_trun = 0;
  m_t8_reg.fill(0);
  m_t8_count.fill(0);
  m_t8_pending.fill(0);
  m_t8_mode.fill(0);
  m_tffcr = 0xcc;
  m_trdc = 0;
  m_t16_reg.fill(0);
  m_t16_cap.fill(0);
  m_t16_count.fill(0);
  m_t16_mode.fill(0x20);
  m_t16_ffcr.fill(0);
  m_t45cr = 0;
  m_pgreg[0] &= 0x0f;
  m_pgreg[1] &= 0x0f;
  m_pg01cr = 0;
  m_watchdog_mode = 0x80;
  m_od_enable = 0;

  for (SerialChannel& channel : m_serial)
  {
    channel.mode &= 0x80;
    channel.control &= 0x80;
    channel.baud_rate = 0;
    channel.rx_full = false;
    channel.tx_busy = false;
    channel.tx_ticks_left = 0;
    channel.rx_queue.clear();
    channel.rx_busy = false;
    channel.rx_ticks_left = 0;
  }

  m_ad_mode = 0;
  m_int_reg.fill(0);
  m_iimc = 0;
  m_dma_vector.fill(0);
  m_block_cs = {0x00, 0x00, 0x10, 0x00};
  m_external_cs = 0;
  m_mem_start.fill(0xff);
  m_mem_mask.fill(0xff);
  m_dram_refresh = 0;
  m_dram_access = 0x80;
}

void TMP95C061::Run(u32 states)
{
  if (m_in_reset)
    return;

  BindCore();
  m_state_balance += states;
  while (m_state_balance > 0)
  {
    u32 elapsed = 0;
    if (m_irq_inhibit)
      m_irq_inhibit = false;
    else if (CheckInterrupts())
      elapsed += 18;

    const bool halted = ArcadeDuckTLCS900HIsHalted();
    elapsed += static_cast<u32>(std::max<s32>(ArcadeDuckTLCS900HStep(), 1));

    // Acceptance is deferred for one instruction after EI and RETI.
    if (!halted)
    {
      const u8 opcode = ArcadeDuckTLCS900HGetLastOpcode();
      m_irq_inhibit = (opcode == 0x06 || opcode == 0x07);
    }

    UpdateAnalog(elapsed);
    UpdateTimers(elapsed * 2);
    for (u32 channel = 0; channel < NUM_SERIAL_CHANNELS; channel++)
    {
      const SerialChannel& ch = m_serial[channel];
      if (ch.tx_busy || ch.rx_busy || !ch.rx_queue.empty())
        UpdateSerial(channel, elapsed * 2);
    }
    if (ProcessMicroDMA())
      elapsed += 8;

    m_state_balance -= elapsed;
  }
}

bool TMP95C061::CheckInterrupts()
{
  const u8 mask = ArcadeDuckTLCS900HGetIFF();
  if (m_irq_idle.Matches(m_int_reg, m_dma_vector, mask))
    return false;

  u8 requests = 0;
  for (const u8 reg : m_int_reg)
    requests |= reg;
  if ((requests & 0x88) == 0)
  {
    m_irq_idle.Store(m_int_reg, m_dma_vector, mask);
    return false;
  }

  std::array<s32, 8> by_priority;
  by_priority.fill(-1);
  for (size_t i = 0; i < IRQ_SOURCES.size(); i++)
  {
    const IrqSource& source = IRQ_SOURCES[i];
    const u8 reg = m_int_reg[source.reg];
    if ((reg & source.flag) == 0)
      continue;

    const u32 priority = (source.flag == 0x80) ? ((reg >> 4) & 0x07) : (reg & 0x07);
    by_priority[priority] = static_cast<s32>(i);
  }

  // Priority 0 disables a source and 7 is reserved for non-maskable requests.
  s32 selected = -1;
  u32 level = 0;
  for (u32 priority = std::max<u32>(1, mask); priority < 7; priority++)
  {
    if (by_priority[priority] >= 0)
    {
      selected = by_priority[priority];
      level = priority + 1;
    }
  }

  if (selected < 0)
  {
    m_irq_idle.Store(m_int_reg, m_dma_vector, mask);
    return false;
  }

  const IrqSource& source = IRQ_SOURCES[static_cast<size_t>(selected)];
  m_int_reg[source.reg] &= static_cast<u8>(~source.flag);
  ArcadeDuckTLCS900HTakeInterrupt(static_cast<u8>(level), source.vector);
  return true;
}

bool TMP95C061::ProcessMicroDMA()
{
  // Micro-DMA requests are serviced only while maskable interrupts are enabled.
  const u8 mask = ArcadeDuckTLCS900HGetIFF();
  if (mask == 7 || m_dma_idle.Matches(m_int_reg, m_dma_vector, mask))
    return false;

  for (u32 channel = 0; channel < 4; channel++)
  {
    if (ProcessMicroDMAChannel(channel))
      return true;
  }
  m_dma_idle.Store(m_int_reg, m_dma_vector, mask);
  return false;
}

bool TMP95C061::ProcessMicroDMAChannel(u32 channel)
{
  const u8 vector = static_cast<u8>((m_dma_vector[channel] & 0x1f) << 2);
  if (vector < 0x28 || vector == 0x3c || vector >= 0x74)
    return false;

  const s8 source_index = VECTOR_SOURCES[vector >> 2];
  if (source_index < 0)
    return false;
  const IrqSource* it = &IRQ_SOURCES[static_cast<size_t>(source_index)];
  if ((m_int_reg[it->reg] & it->flag) == 0)
    return false;

  u32& src = m_dma_source[channel];
  u32& dst = m_dma_dest[channel];
  const auto copy = [this, &src, &dst](u32 width) {
    for (u32 i = 0; i < width; i++)
      Write8((dst + i) & 0xffffff, Read8((src + i) & 0xffffff));
  };

  switch (m_dma_mode[channel] & 0x1f)
  {
    case 0x00: copy(1); dst += 1; break;
    case 0x01: copy(2); dst += 2; break;
    case 0x02: copy(4); dst += 4; break;
    case 0x04: copy(1); dst -= 1; break;
    case 0x05: copy(2); dst -= 2; break;
    case 0x06: copy(4); dst -= 4; break;
    case 0x08: copy(1); src += 1; break;
    case 0x09: copy(2); src += 2; break;
    case 0x0a: copy(4); src += 4; break;
    case 0x0c: copy(1); src -= 1; break;
    case 0x0d: copy(2); src -= 2; break;
    case 0x0e: copy(4); src -= 4; break;
    case 0x10: copy(1); break;
    case 0x11: copy(2); break;
    case 0x12: copy(4); break;
    case 0x14: src += 1; break;
    default: break;
  }

  m_dma_count[channel]--;
  if (m_dma_count[channel] == 0)
  {
    m_dma_vector[channel] = 0;
    m_int_reg[(channel < 2) ? INTETC10 : INTETC32] |= (channel & 1) ? 0x80 : 0x08;
  }

  m_int_reg[it->reg] &= static_cast<u8>(~it->flag);
  return true;
}

void TMP95C061::UpdateAnalog(u32 states)
{
  if (m_ad_states_left <= 0)
    return;

  m_ad_states_left -= static_cast<s32>(states);
  if (m_ad_states_left > 0)
    return;

  const auto convert = [this](u32 channel) {
    m_ad_result[channel] = m_callbacks.analog_read ? (m_callbacks.analog_read(m_callbacks.context, channel) & 0x3ff) : 0;
  };

  if ((m_ad_mode & 0x10) == 0)
  {
    convert(m_ad_mode & 0x03);
  }
  else
  {
    // Channel scan converts AN0 up to the selected channel.
    for (u32 channel = 0; channel <= (m_ad_mode & 0x03u); channel++)
      convert(channel);
  }

  m_ad_mode = static_cast<u8>((m_ad_mode & ~0x40) | 0x80);
  m_int_reg[INTE0AD] |= 0x80;

  // Repeat mode starts the next conversion immediately.
  if (m_ad_mode & 0x20)
    m_ad_states_left = (m_ad_mode & 0x08) ? 320 : 160;
}

void TMP95C061::ChangeTimerFlipFlop(u32 which, u32 change)
{
  u8& ff = (which == 1) ? m_to1 : m_to3;
  switch (change)
  {
    case FF_CLEAR: ff = 0; break;
    case FF_SET: ff = 1; break;
    case FF_INVERT: ff ^= 1; break;
    default: break;
  }
  UpdatePortA();
}

void TMP95C061::UpdateTimers(u32 fc_ticks)
{
  if ((m_trun & 0x80) == 0)
    return;

  const u32 old_prescaler = m_prescaler;
  m_prescaler += fc_ticks;
  const auto ticks = [old_prescaler, this](u32 shift) { return (m_prescaler >> shift) - (old_prescaler >> shift); };

  // 8-bit timers 0-3. Timers 0/2 can cascade into 1/3 (TO0TRG/TO2TRG) or pair
  // into a 16-bit timer (TxxMOD bits 7-6 = 01).
  for (u32 pair = 0; pair < 2; pair++)
  {
    const u32 low = pair * 2;
    const u32 high = low + 1;
    const u8 mode = m_t8_mode[pair];
    const u8 low_int = (pair == 0) ? INTET10 : INTET32;

    if (m_trun & (1u << low))
    {
      switch (mode & 0x03)
      {
        case 0x00: if (pair == 1) m_t8_pending[low] += ticks(T1_SHIFT); break; // TI0 is not connected
        case 0x01: m_t8_pending[low] += ticks(T1_SHIFT); break;
        case 0x02: m_t8_pending[low] += ticks(T4_SHIFT); break;
        case 0x03: m_t8_pending[low] += ticks(T16_SHIFT); break;
      }

      for (; m_t8_pending[low] > 0; m_t8_pending[low]--)
      {
        m_t8_count[low]++;
        if (m_t8_count[low] != m_t8_reg[low])
          continue;

        if ((m_trun & (1u << high)) && (mode & 0x0c) == 0x00)
          m_t8_pending[high]++;

        if ((mode & 0xc0) != 0x40)
        {
          m_t8_count[low] = 0;
          m_int_reg[low_int] |= 0x08;
        }
      }
    }

    if (m_trun & (1u << high))
    {
      switch ((mode >> 2) & 0x03)
      {
        case 0x00: break;
        case 0x01: m_t8_pending[high] += ticks(T1_SHIFT); break;
        case 0x02: m_t8_pending[high] += ticks(T16_SHIFT); break;
        case 0x03: m_t8_pending[high] += ticks(T256_SHIFT); break;
      }

      for (; m_t8_pending[high] > 0; m_t8_pending[high]--)
      {
        m_t8_count[high]++;
        if (m_t8_count[high] != m_t8_reg[high])
          continue;

        m_t8_count[high] = 0;
        m_int_reg[low_int] |= 0x80;
        if (m_tffcr & ((pair == 0) ? 0x02 : 0x20))
          ChangeTimerFlipFlop(high, FF_INVERT);
        if ((mode & 0xc0) == 0x40)
          m_t8_count[low] = 0;
      }
    }
  }

  // 16-bit timers 4/5. TREGn+1 match clears the up-counter.
  for (u32 timer = 0; timer < 2; timer++)
  {
    if ((m_trun & (0x10u << timer)) == 0)
      continue;

    u32 count = 0;
    switch (m_t16_mode[timer] & 0x03)
    {
      case 0x01: count = ticks(T1_SHIFT); break;
      case 0x02: count = ticks(T4_SHIFT); break;
      case 0x03: count = ticks(T16_SHIFT); break;
      default: break;
    }

    const u8 int_reg = (timer == 0) ? INTET54 : INTET76;
    for (; count > 0; count--)
    {
      m_t16_count[timer]++;
      if (m_t16_count[timer] == m_t16_reg[timer * 2 + 1])
      {
        m_t16_count[timer] = 0;
        m_int_reg[int_reg] |= 0x80;
      }
      else if (m_t16_count[timer] == m_t16_reg[timer * 2])
      {
        m_int_reg[int_reg] |= 0x08;
      }
    }
  }

  m_prescaler &= 0xffffff;
}

s64 TMP95C061::GetSerialFrameTicks(u32 channel) const
{
  const SerialChannel& ch = m_serial[channel];
  static constexpr std::array<u32, 4> PRESCALE = {4, 16, 64, 256};
  const u32 divisor = (ch.baud_rate & 0x0f) ? (ch.baud_rate & 0x0f) : 16;
  const u32 baud_period = PRESCALE[(ch.baud_rate >> 4) & 0x03] * divisor;

  const u32 sm = (ch.mode >> 2) & 0x03;
  if (sm == 0)
  {
    // I/O interface mode shifts one bit per two baud generator periods.
    return static_cast<s64>(8 * 2 * baud_period);
  }

  u32 sioclk_period;
  switch (ch.mode & 0x03)
  {
    case 0x02: sioclk_period = 2; break;
    case 0x00: sioclk_period = 8u * std::max<u32>(m_t8_reg[0], 1); break; // timer 0 match
    default: sioclk_period = baud_period; break;
  }

  const u32 data_bits = (sm == 1) ? 7 : ((sm == 2) ? 8 : 9);
  const u32 parity_bits = (sm != 3 && (ch.control & 0x20)) ? 1 : 0;
  const u32 bits = 1 + data_bits + parity_bits + 1;
  return static_cast<s64>(bits * 16 * sioclk_period);
}

void TMP95C061::UpdateSerial(u32 channel, u32 fc_ticks)
{
  SerialChannel& ch = m_serial[channel];
  const u8 tx_flag = 0x80;
  const u8 rx_flag = 0x08;
  const u8 int_reg = (channel == 0) ? INTES0 : INTES1;

  if (ch.tx_busy)
  {
    ch.tx_ticks_left -= fc_ticks;
    if (ch.tx_ticks_left <= 0)
    {
      ch.tx_busy = false;
      m_int_reg[int_reg] |= tx_flag;
      if (m_callbacks.serial_transmit)
        m_callbacks.serial_transmit(m_callbacks.context, channel, ch.tx_shift);
    }
  }

  if (!ch.rx_busy)
  {
    StartSerialReceive(channel);
    return;
  }

  ch.rx_ticks_left -= fc_ticks;
  if (ch.rx_ticks_left > 0)
    return;

  ch.rx_busy = false;
  const u8 value = ch.rx_queue.front();
  ch.rx_queue.pop_front();
  if (ch.rx_full)
  {
    ch.control |= 0x10; // OERR
  }
  else
  {
    ch.rx_buffer = value;
    ch.rx_full = true;
    m_int_reg[int_reg] |= rx_flag;
  }
  StartSerialReceive(channel);
}

void TMP95C061::StartSerialReceive(u32 channel)
{
  SerialChannel& ch = m_serial[channel];
  if (ch.rx_busy || ch.rx_queue.empty())
    return;

  // A peer only starts its frame once the receiver is enabled (half-duplex turnaround).
  if ((ch.mode & 0x20) == 0)
    return;

  ch.rx_busy = true;
  ch.rx_ticks_left = GetSerialFrameTicks(channel);
}

void TMP95C061::ReceiveSerial(u32 channel, u8 value)
{
  DebugAssert(channel < NUM_SERIAL_CHANNELS);
  m_serial[channel].rx_queue.push_back(value);
}

void TMP95C061::SetInterruptLine(InterruptLine line, bool asserted)
{
  bool& level = m_int_line[static_cast<size_t>(line)];
  const bool rising = asserted && !level;
  level = asserted;

  switch (line)
  {
    case InterruptLine::INT0:
      if (m_iimc & 0x04)
      {
        if (m_iimc & 0x02)
        {
          if (rising)
            m_int_reg[INTE0AD] |= 0x08;
        }
        else if (asserted)
        {
          m_int_reg[INTE0AD] |= 0x08;
        }
        else
        {
          m_int_reg[INTE0AD] &= static_cast<u8>(~0x08);
        }
      }
      break;

    case InterruptLine::INT4:
      if ((m_port_control[PortIndex(Port::PB)] & 0x01) == 0 && rising)
        m_int_reg[INTE45] |= 0x08;
      break;

    case InterruptLine::INT5:
      if ((m_port_control[PortIndex(Port::PB)] & 0x02) == 0 && rising)
        m_int_reg[INTE45] |= 0x80;
      break;
  }
}

u8 TMP95C061::ReadPort(Port port)
{
  const size_t index = PortIndex(port);
  const u8 pins = m_callbacks.port_read ? m_callbacks.port_read(m_callbacks.context, port) : 0x00;
  switch (port)
  {
    case Port::P2:
      return m_port_latch[index];
    case Port::P9:
      return pins;
    case Port::P6:
      return static_cast<u8>((pins & ~m_port_function[index]) | (m_port_latch[index] & m_port_function[index]));
    default:
      return static_cast<u8>((pins & ~m_port_control[index]) | (m_port_latch[index] & m_port_control[index]));
  }
}

void TMP95C061::WritePort(Port port, u8 value)
{
  m_port_latch[PortIndex(port)] = value;
  if (port == Port::PA)
  {
    UpdatePortA();
    return;
  }

  if (m_callbacks.port_write)
    m_callbacks.port_write(m_callbacks.context, port, value);
}

void TMP95C061::UpdatePortA()
{
  const size_t index = PortIndex(Port::PA);
  const u8 timer_outputs = static_cast<u8>((m_to1 << 2) | (m_to3 << 3));
  const u8 value = static_cast<u8>(((timer_outputs & m_port_function[index]) |
                                    (m_port_latch[index] & ~m_port_function[index])) &
                                   m_port_control[index]);
  if (m_callbacks.port_write)
    m_callbacks.port_write(m_callbacks.context, Port::PA, value);
}

u8 TMP95C061::ReadInternal(u32 address)
{
  switch (address)
  {
    case 0x01: return ReadPort(Port::P1);
    case 0x06: return ReadPort(Port::P2);
    case 0x0d: return ReadPort(Port::P5);
    case 0x12: return ReadPort(Port::P6);
    case 0x13: return ReadPort(Port::P7);
    case 0x18: return ReadPort(Port::P8);
    case 0x19: return ReadPort(Port::P9);
    case 0x1e: return ReadPort(Port::PA);
    case 0x1f: return ReadPort(Port::PB);
    case 0x20: return m_trun;
    case 0x25: return m_tffcr;
    case 0x29: return m_trdc;
    case 0x34: case 0x35: case 0x36: case 0x37:
      return static_cast<u8>(m_t16_cap[(address - 0x34) >> 1] >> ((address & 1) * 8));
    case 0x38: return m_t16_mode[0];
    case 0x39: return m_t16_ffcr[0];
    case 0x3a: return m_t45cr;
    case 0x3c: case 0x3d: case 0x3e: case 0x3f:
      return (address & 1) ? m_mem_mask[(address - 0x3c) >> 1] : m_mem_start[(address - 0x3c) >> 1];
    case 0x44: case 0x45: case 0x46: case 0x47:
      return static_cast<u8>(m_t16_cap[2 + ((address - 0x44) >> 1)] >> ((address & 1) * 8));
    case 0x48: return m_t16_mode[1];
    case 0x49: return m_t16_ffcr[1];
    case 0x4c: case 0x4d: return m_pgreg[address - 0x4c];
    case 0x4e: return m_pg01cr;

    case 0x50: case 0x54:
    {
      // Reading receive buffer 2 also clears the INTRX request.
      const u32 channel = (address == 0x50) ? 0 : 1;
      SerialChannel& ch = m_serial[channel];
      ch.rx_full = false;
      m_int_reg[(channel == 0) ? INTES0 : INTES1] &= static_cast<u8>(~0x08);
      return ch.rx_buffer;
    }

    case 0x51: case 0x55:
    {
      // Error flags clear when SCnCR is read.
      SerialChannel& ch = m_serial[(address == 0x51) ? 0 : 1];
      const u8 value = ch.control;
      ch.control &= 0xe3;
      return value;
    }

    case 0x52: return m_serial[0].mode;
    case 0x53: return m_serial[0].baud_rate;
    case 0x56: return m_serial[1].mode;
    case 0x57: return m_serial[1].baud_rate;
    case 0x58: return m_od_enable;
    case 0x5a: return m_dram_refresh;
    case 0x5b: return m_dram_access;
    case 0x5c: case 0x5d: case 0x5e: case 0x5f:
      return (address & 1) ? m_mem_mask[2 + ((address - 0x5c) >> 1)] : m_mem_start[2 + ((address - 0x5c) >> 1)];

    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    {
      // Reading any ADREG clears EOCF; reading the upper byte clears the A/D request.
      const u32 channel = (address - 0x60) >> 1;
      m_ad_mode &= static_cast<u8>(~0x80);
      if (address & 1)
        return static_cast<u8>(m_ad_result[channel] >> 2);
      m_int_reg[INTE0AD] &= static_cast<u8>(~0x80);
      return static_cast<u8>((m_ad_result[channel] << 6) | 0x3f);
    }

    case 0x6d: return m_ad_mode;
    case 0x6e: return m_watchdog_mode;

    default:
      if (address >= 0x70 && address <= 0x7a)
        return m_int_reg[address - 0x70];
      return 0x00;
  }
}

void TMP95C061::WriteInternal(u32 address, u8 value)
{
  switch (address)
  {
    case 0x01: WritePort(Port::P1, value); return;
    case 0x04: m_port_control[PortIndex(Port::P1)] = value; return;
    case 0x06: WritePort(Port::P2, value); return;
    case 0x09: m_port_function[PortIndex(Port::P2)] = value; return;
    case 0x0d: WritePort(Port::P5, value); return;
    case 0x10: m_port_control[PortIndex(Port::P5)] = value; return;
    case 0x11: m_port_function[PortIndex(Port::P5)] = value; return;
    case 0x12: WritePort(Port::P6, value); return;
    case 0x13: WritePort(Port::P7, value); return;
    case 0x15: m_port_function[PortIndex(Port::P6)] = value; return;
    case 0x16: m_port_control[PortIndex(Port::P7)] = value; return;
    case 0x17: m_port_function[PortIndex(Port::P7)] = value; return;
    case 0x18: WritePort(Port::P8, value); return;
    case 0x1a: m_port_control[PortIndex(Port::P8)] = value; return;
    case 0x1b: m_port_function[PortIndex(Port::P8)] = value; return;
    case 0x1e: WritePort(Port::PA, value); return;
    case 0x1f: WritePort(Port::PB, value); return;

    case 0x20:
      for (u32 i = 0; i < 4; i++)
      {
        if ((value & (1u << i)) == 0)
        {
          m_t8_count[i] = 0;
          m_t8_pending[i] = 0;
        }
      }
      if ((value & 0x10) == 0)
        m_t16_count[0] = 0;
      if ((value & 0x20) == 0)
        m_t16_count[1] = 0;
      m_trun = value;
      return;

    case 0x22: case 0x23: m_t8_reg[address - 0x22] = value; return;
    case 0x24: m_t8_mode[0] = value; return;

    case 0x25:
      switch (value & 0x0c)
      {
        case 0x00: ChangeTimerFlipFlop(1, FF_INVERT); break;
        case 0x04: ChangeTimerFlipFlop(1, FF_SET); break;
        case 0x08: ChangeTimerFlipFlop(1, FF_CLEAR); break;
        default: break;
      }
      switch (value & 0xc0)
      {
        case 0x00: ChangeTimerFlipFlop(3, FF_INVERT); break;
        case 0x40: ChangeTimerFlipFlop(3, FF_SET); break;
        case 0x80: ChangeTimerFlipFlop(3, FF_CLEAR); break;
        default: break;
      }
      m_tffcr = value | 0xcc;
      return;

    case 0x26: case 0x27: m_t8_reg[2 + address - 0x26] = value; return;
    case 0x28: m_t8_mode[1] = value; return;
    case 0x29: m_trdc = value; return;
    case 0x2c: m_port_control[PortIndex(Port::PA)] = value; UpdatePortA(); return;
    case 0x2d: m_port_function[PortIndex(Port::PA)] = value; UpdatePortA(); return;
    case 0x2e: m_port_control[PortIndex(Port::PB)] = value; return;
    case 0x2f: m_port_function[PortIndex(Port::PB)] = value; return;

    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x40: case 0x41: case 0x42: case 0x43:
    {
      const u32 index = ((address & 0x0f) >> 1) + ((address >= 0x40) ? 2 : 0);
      u16& reg = m_t16_reg[index];
      reg = (address & 1) ? static_cast<u16>((reg & 0x00ff) | (value << 8)) : static_cast<u16>((reg & 0xff00) | value);
      return;
    }

    case 0x38:
      // Writing 0 to CAPIN latches the timer 4 up-counter into CAP1; the bit reads as 1.
      if ((value & 0x20) == 0)
        m_t16_cap[0] = m_t16_count[0];
      m_t16_mode[0] = value | 0x20;
      return;
    case 0x39: m_t16_ffcr[0] = value | 0xc3; return;
    case 0x3a: m_t45cr = value; return;
    case 0x3c: case 0x3d: case 0x3e: case 0x3f:
      ((address & 1) ? m_mem_mask : m_mem_start)[(address - 0x3c) >> 1] = value;
      return;
    case 0x48:
      // Writing 0 to CAP3IN latches the timer 5 up-counter into CAP3.
      if ((value & 0x20) == 0)
        m_t16_cap[2] = m_t16_count[1];
      m_t16_mode[1] = value | 0x20;
      return;
    case 0x49: m_t16_ffcr[1] = value | 0xc3; return;
    case 0x4c: case 0x4d: m_pgreg[address - 0x4c] = value; return;
    case 0x4e: m_pg01cr = value; return;

    case 0x50: case 0x54:
    {
      const u32 channel = (address == 0x50) ? 0 : 1;
      SerialChannel& ch = m_serial[channel];
      // Single transmit buffer: INTTX is raised once the frame has been shifted out.
      ch.tx_shift = value;
      ch.tx_busy = true;
      ch.tx_ticks_left = GetSerialFrameTicks(channel);
      return;
    }

    case 0x51: m_serial[0].control = static_cast<u8>((m_serial[0].control & 0x9c) | (value & 0x63)); return;
    case 0x52: m_serial[0].mode = value; return;
    case 0x53: m_serial[0].baud_rate = value; return;
    case 0x55: m_serial[1].control = static_cast<u8>((m_serial[1].control & 0x9c) | (value & 0x63)); return;
    case 0x56: m_serial[1].mode = value; return;
    case 0x57: m_serial[1].baud_rate = value; return;
    case 0x58: m_od_enable = value; return;
    case 0x5a: m_dram_refresh = value; return;
    case 0x5b: m_dram_access = value; return;
    case 0x5c: case 0x5d: case 0x5e: case 0x5f:
      ((address & 1) ? m_mem_mask : m_mem_start)[2 + ((address - 0x5c) >> 1)] = value;
      return;
    case 0x68: case 0x69: case 0x6a: case 0x6b: m_block_cs[address - 0x68] = value; return;
    case 0x6c: m_external_cs = value; return;

    case 0x6d:
    {
      u8 data = static_cast<u8>((m_ad_mode & 0xc0) | (value & 0x3f));
      if (data & 0x04)
      {
        data = static_cast<u8>((data & ~0x04) | 0x40);
        m_ad_states_left = (data & 0x08) ? 320 : 160;
      }
      m_ad_mode = data;
      return;
    }

    case 0x6e: m_watchdog_mode = value; return;
    case 0x6f: return;

    case 0x7b: m_iimc = value; return;
    case 0x7c: case 0x7d: case 0x7e: case 0x7f: m_dma_vector[address - 0x7c] = value; return;

    default:
      if (address >= 0x70 && address <= 0x7a)
      {
        // Writing 1 to a request flag leaves it unchanged; writing 0 clears it.
        u8& reg = m_int_reg[address - 0x70];
        u8 data = value;
        if (data & 0x80)
          data = static_cast<u8>((data & 0x7f) | (reg & 0x80));
        if (data & 0x08)
          data = static_cast<u8>((data & 0xf7) | (reg & 0x08));
        reg = data;
      }
      return;
  }
}

void TMP95C061::SetDirectMemory(u32 base, u8* data, u32 size)
{
  m_direct_data = data;
  m_direct_base = base;
  m_direct_size = data ? size : 0;
}

u8 TMP95C061::Read8(u32 address)
{
  address &= 0xffffff;
  if ((address - m_direct_base) < m_direct_size)
    return m_direct_data[address - m_direct_base];
  if (address < 0x80)
    return ReadInternal(address);
  return m_callbacks.read ? m_callbacks.read(m_callbacks.context, address) : 0xff;
}

void TMP95C061::Write8(u32 address, u8 value)
{
  address &= 0xffffff;
  if ((address - m_direct_base) < m_direct_size)
  {
    m_direct_data[address - m_direct_base] = value;
    return;
  }
  if (address < 0x80)
  {
    WriteInternal(address, value);
    return;
  }
  if (m_callbacks.write)
    m_callbacks.write(m_callbacks.context, address, value);
}

u8 TMP95C061::BusRead8(void* context, u32 address)
{
  return static_cast<TMP95C061*>(context)->Read8(address);
}

u16 TMP95C061::BusRead16(void* context, u32 address)
{
  TMP95C061* self = static_cast<TMP95C061*>(context);
  return static_cast<u16>(self->Read8(address) | (self->Read8(address + 1) << 8));
}

u32 TMP95C061::BusRead32(void* context, u32 address)
{
  TMP95C061* self = static_cast<TMP95C061*>(context);
  return static_cast<u32>(self->Read8(address)) | (static_cast<u32>(self->Read8(address + 1)) << 8) |
         (static_cast<u32>(self->Read8(address + 2)) << 16) | (static_cast<u32>(self->Read8(address + 3)) << 24);
}

void TMP95C061::BusWrite8(void* context, u32 address, u8 value)
{
  static_cast<TMP95C061*>(context)->Write8(address, value);
}

void TMP95C061::BusWrite16(void* context, u32 address, u16 value)
{
  TMP95C061* self = static_cast<TMP95C061*>(context);
  self->Write8(address, static_cast<u8>(value));
  self->Write8(address + 1, static_cast<u8>(value >> 8));
}

void TMP95C061::BusWrite32(void* context, u32 address, u32 value)
{
  TMP95C061* self = static_cast<TMP95C061*>(context);
  for (u32 i = 0; i < 4; i++)
    self->Write8(address + i, static_cast<u8>(value >> (i * 8)));
}

// Control register space: DMAS0-3 at 0x00-0x0F, DMAD0-3 at 0x10-0x1F,
// DMAC0-3 (16-bit) at 0x20/0x24/0x28/0x2C and DMAM0-3 (8-bit) at 0x22/0x26/0x2A/0x2E.
u32 TMP95C061::ControlRead(void* context, u8 reg, unsigned size)
{
  TMP95C061* self = static_cast<TMP95C061*>(context);
  if (reg < 0x20)
  {
    const std::array<u32, 4>& regs = (reg < 0x10) ? self->m_dma_source : self->m_dma_dest;
    const u32 value = regs[(reg >> 2) & 3] >> ((reg & 3) * 8);
    return (size == 4) ? value : ((size == 2) ? (value & 0xffff) : (value & 0xff));
  }

  if (reg < 0x30)
  {
    const u32 channel = (reg - 0x20) >> 2;
    if ((reg & 3) == 2)
      return self->m_dma_mode[channel];
    return (size == 1) ? (self->m_dma_count[channel] >> ((reg & 1) * 8)) & 0xff : self->m_dma_count[channel];
  }

  return 0;
}

void TMP95C061::ControlWrite(void* context, u8 reg, unsigned size, u32 value)
{
  TMP95C061* self = static_cast<TMP95C061*>(context);
  if (reg < 0x20)
  {
    u32& target = ((reg < 0x10) ? self->m_dma_source : self->m_dma_dest)[(reg >> 2) & 3];
    const u32 shift = (reg & 3) * 8;
    const u32 mask = (size == 4) ? 0xffffffffu : ((size == 2) ? 0xffffu : 0xffu);
    target = ((target & ~(mask << shift)) | ((value & mask) << shift)) & 0xffffff;
    return;
  }

  if (reg < 0x30)
  {
    const u32 channel = (reg - 0x20) >> 2;
    if ((reg & 3) == 2)
    {
      self->m_dma_mode[channel] = static_cast<u8>(value);
    }
    else if (size == 1)
    {
      const u32 shift = (reg & 1) * 8;
      self->m_dma_count[channel] =
        static_cast<u16>((self->m_dma_count[channel] & ~(0xffu << shift)) | ((value & 0xff) << shift));
    }
    else
    {
      self->m_dma_count[channel] = static_cast<u16>(value);
    }
  }
}

static void DoTLCSStateBytes(void* context, void* data, size_t size)
{
  static_cast<StateWrapper*>(context)->DoBytes(data, size);
}

bool TMP95C061::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("TMP95C061"))
    return false;

  BindCore();
  ArcadeDuckTLCS900HDoState(DoTLCSStateBytes, &sw);

  sw.Do(&m_in_reset);
  sw.Do(&m_irq_inhibit);
  sw.Do(&m_state_balance);
  sw.DoArray(m_port_latch.data(), m_port_latch.size());
  sw.DoArray(m_port_control.data(), m_port_control.size());
  sw.DoArray(m_port_function.data(), m_port_function.size());
  sw.Do(&m_trun);
  sw.DoArray(m_t8_reg.data(), m_t8_reg.size());
  sw.DoArray(m_t8_count.data(), m_t8_count.size());
  sw.DoArray(m_t8_pending.data(), m_t8_pending.size());
  sw.DoArray(m_t8_mode.data(), m_t8_mode.size());
  sw.Do(&m_tffcr);
  sw.Do(&m_trdc);
  sw.Do(&m_to1);
  sw.Do(&m_to3);
  sw.DoArray(m_t16_reg.data(), m_t16_reg.size());
  sw.DoArray(m_t16_cap.data(), m_t16_cap.size());
  sw.DoArray(m_t16_count.data(), m_t16_count.size());
  sw.DoArray(m_t16_mode.data(), m_t16_mode.size());
  sw.DoArray(m_t16_ffcr.data(), m_t16_ffcr.size());
  sw.Do(&m_t45cr);
  sw.Do(&m_prescaler);
  sw.DoArray(m_pgreg.data(), m_pgreg.size());
  sw.Do(&m_pg01cr);
  sw.Do(&m_watchdog_mode);
  sw.Do(&m_od_enable);
  for (SerialChannel& ch : m_serial)
  {
    sw.Do(&ch.mode);
    sw.Do(&ch.control);
    sw.Do(&ch.baud_rate);
    sw.Do(&ch.rx_buffer);
    sw.Do(&ch.rx_full);
    sw.Do(&ch.tx_shift);
    sw.Do(&ch.tx_busy);
    sw.Do(&ch.tx_ticks_left);
    u32 queued = static_cast<u32>(ch.rx_queue.size());
    sw.Do(&queued);
    if (sw.IsReading())
    {
      if (sw.HasError() || queued > 0x10000)
        return false;
      ch.rx_queue.assign(queued, 0);
    }
    for (u8& value : ch.rx_queue)
      sw.Do(&value);
    sw.Do(&ch.rx_busy);
    sw.Do(&ch.rx_ticks_left);
  }
  sw.DoArray(m_ad_result.data(), m_ad_result.size());
  sw.Do(&m_ad_mode);
  sw.Do(&m_ad_states_left);
  sw.DoArray(m_int_reg.data(), m_int_reg.size());
  sw.Do(&m_iimc);
  for (bool& line : m_int_line)
    sw.Do(&line);
  sw.DoArray(m_dma_vector.data(), m_dma_vector.size());
  for (IdleState* idle : {&m_irq_idle, &m_dma_idle})
  {
    sw.DoArray(idle->int_reg.data(), idle->int_reg.size());
    sw.DoArray(idle->dma_vector.data(), idle->dma_vector.size());
    sw.Do(&idle->iff);
    sw.Do(&idle->valid);
  }
  sw.DoArray(m_dma_source.data(), m_dma_source.size());
  sw.DoArray(m_dma_dest.data(), m_dma_dest.size());
  sw.DoArray(m_dma_count.data(), m_dma_count.size());
  sw.DoArray(m_dma_mode.data(), m_dma_mode.size());
  sw.DoArray(m_block_cs.data(), m_block_cs.size());
  sw.Do(&m_external_cs);
  sw.DoArray(m_mem_start.data(), m_mem_start.size());
  sw.DoArray(m_mem_mask.data(), m_mem_mask.size());
  sw.Do(&m_dram_refresh);
  sw.Do(&m_dram_access);
  return !sw.HasError();
}
} // namespace Arcade
