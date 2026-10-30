// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-FileCopyrightText: windyfairy
// SPDX-License-Identifier: GPL-3.0-only
//
// MEM(P3) register map, sub-CPU clock and decoder wiring follow MAME's
// BSD-3-Clause namcos10.cpp (namcos10_memp3_state). See THIRD-PARTY-LICENSES.txt.

#include "core/arcade/systems/namco/system10/namco_system10_memp3.h"
#include "util/state_wrapper.h"

#include "core/system.h"
#include "core/timing_event.h"

#include "common/log.h"

#include <algorithm>

Log_SetChannel(NamcoSystem10);

namespace NamcoSystem10 {
namespace {
// Main CPU offsets within EXP1 (0x1F000000).
constexpr u32 HOST_FIRMWARE_CONTROL = 0x300000;
constexpr u32 HOST_COMMAND = 0x300006;
constexpr u32 HOST_RAM_BANK = 0x30000c;
constexpr u32 HOST_UPLOAD_READY = 0x30000e;
constexpr u32 HOST_REGISTERS_END = 0x300010;
constexpr u32 HOST_RAM_BASE = 0x320000;
constexpr u32 HOST_RAM_END = 0x340000;
constexpr u32 HOST_RAM_BANK_WORDS = 0x10000;

// Sub-CPU address map.
constexpr u32 MCU_STATUS = 0x100000;
constexpr u32 MCU_DIPSW = 0x100002;
constexpr u32 MCU_CONTROL = 0x100004;
constexpr u32 MCU_COMMAND_ACK = 0x100008;
constexpr u32 MCU_DECODE_PLD = 0x10000e;
constexpr u32 MCU_MODE = 0x100010;
constexpr u32 MCU_PLD_VERSION_BASE = 0x10001e;
constexpr u32 MCU_PLD_VERSION_END = 0x100030;
constexpr u32 MCU_MP3_DATA = 0x100030;
constexpr u32 MCU_STREAM_STATUS = 0x100032;
constexpr u32 MCU_RAM_BASE = 0x800000;

// Port 7: decoder control-port lines. Port B bit 0: decoder DEMAND.
constexpr u8 P7_DATA_OUT = 0x01;
constexpr u8 P7_DATA_IN = 0x02;
constexpr u8 P7_CLOCK = 0x04;
constexpr u8 P7_CHIP_SELECT = 0x08;

constexpr u32 SCHEDULER_HZ = 2000;
} // namespace

MemP3Board::MemP3Board(const Cabinet& cabinet) : m_cabinet(cabinet), m_ram(RAM_SIZE, 0)
{
  m_mcu = std::make_unique<Arcade::TMP95C061>(MCU_CLOCK_HZ);
  Arcade::TMP95C061::Callbacks callbacks;
  callbacks.context = this;
  callbacks.read = &MemP3Board::MCURead;
  callbacks.write = &MemP3Board::MCUWrite;
  callbacks.port_read = &MemP3Board::MCUPortRead;
  callbacks.port_write = &MemP3Board::MCUPortWrite;
  callbacks.analog_read = &MemP3Board::MCUAnalogRead;
  m_mcu->SetCallbacks(callbacks);
  m_mcu->SetDirectMemory(MCU_RAM_BASE, m_ram.data(), RAM_SIZE);

  const TickCount interval = std::max<TickCount>(1, System::GetTicksPerSecond() / SCHEDULER_HZ);
  m_event = std::make_unique<TimingEvent>("Namco System 10 MEM(P3)", interval, interval, SchedulerCallback, this);
  m_event->SetPeriodAndSchedule(interval);
}

MemP3Board::~MemP3Board()
{
  m_event->Deactivate();
}

void MemP3Board::Reset()
{
  std::fill(m_ram.begin(), m_ram.end(), 0);
  m_ram_bank = 0;
  m_command_busy = false;
  m_status_value = 0;
  m_control_value = 0xffff;
  m_mode_value = 0;
  m_port7 = 0;
  m_porta = 0;
  m_stream_available = false;
  m_state_fraction = 0;
  m_decoder.Reset();
  m_mcu->SetInterruptLine(Arcade::TMP95C061::InterruptLine::INT5, false);
  m_mcu->SetReset(true);
}

void MemP3Board::SchedulerCallback(void* param, TickCount ticks, TickCount)
{
  static_cast<MemP3Board*>(param)->RunMCU(ticks);
}

void MemP3Board::RunMCU(TickCount ticks)
{
  if (m_mcu->IsInReset())
    return;

  const u64 ticks_per_second = static_cast<u64>(std::max<TickCount>(System::GetTicksPerSecond(), 1));
  const u64 numerator = static_cast<u64>(m_mcu->GetStatesPerSecond()) * static_cast<u64>(std::max<TickCount>(ticks, 0)) +
                        m_state_fraction;
  m_state_fraction = numerator % ticks_per_second;
  m_mcu->Run(static_cast<u32>(numerator / ticks_per_second));
}

bool MemP3Board::HandlesOffset(u32 offset)
{
  return (offset >= HOST_FIRMWARE_CONTROL && offset < HOST_REGISTERS_END) ||
         (offset >= HOST_RAM_BASE && offset < HOST_RAM_END);
}

bool MemP3Board::Read(u32 width, u32 offset, u32* value)
{
  const u32 base = offset & ~1u;
  u16 word;
  if (base >= HOST_RAM_BASE && base < HOST_RAM_END)
  {
    const u32 address = (static_cast<u32>(m_ram_bank) * HOST_RAM_BANK_WORDS + (base - HOST_RAM_BASE) / 2) * 2;
    word = (address + 1 < RAM_SIZE) ? static_cast<u16>(m_ram[address] | (m_ram[address + 1] << 8)) : 0xffff;
  }
  else if (base == HOST_COMMAND)
  {
    // Clear once the sub-CPU has acknowledged the previous command.
    word = m_command_busy ? 0 : 1;
  }
  else if (base == HOST_UPLOAD_READY)
  {
    word = 1;
  }
  else if (base >= HOST_FIRMWARE_CONTROL && base < HOST_REGISTERS_END)
  {
    word = 0;
  }
  else
  {
    return false;
  }

  *value = (width == 1) ? ((word >> ((offset & 1) * 8)) & 0xff) : word;
  return true;
}

bool MemP3Board::Write(u32 width, u32 offset, u32 value)
{
  const u32 base = offset & ~1u;
  if (base >= HOST_RAM_BASE && base < HOST_RAM_END)
  {
    const u32 address = (static_cast<u32>(m_ram_bank) * HOST_RAM_BANK_WORDS + (base - HOST_RAM_BASE) / 2) * 2;
    if (address + 1 >= RAM_SIZE)
      return true;
    if (width == 1)
      m_ram[address + (offset & 1)] = static_cast<u8>(value);
    else
    {
      m_ram[address] = static_cast<u8>(value);
      m_ram[address + 1] = static_cast<u8>(value >> 8);
    }
    return true;
  }

  switch (base)
  {
    case HOST_FIRMWARE_CONTROL:
      // Writing 1 restarts the sub-CPU on the uploaded program.
      if ((value & 0xffff) == 1)
      {
        m_mcu->SetReset(true);
        m_mcu->SetReset(false);
        m_state_fraction = 0;
        DEV_LOG("MEM(P3) sub-CPU started pc={:06X}", m_mcu->GetPC());
      }
      return true;

    case HOST_COMMAND:
      // Commands are processed on INT5; the sub-CPU acknowledges through 0x100008.
      m_command_busy = true;
      m_mcu->SetInterruptLine(Arcade::TMP95C061::InterruptLine::INT5, true);
      return true;

    case HOST_RAM_BANK:
      m_ram_bank = static_cast<u16>(value);
      return true;

    default:
      return base >= HOST_FIRMWARE_CONTROL && base < HOST_REGISTERS_END;
  }
}

u16 MemP3Board::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case MCU_STATUS: return m_status_value;
    case MCU_DIPSW: return 0xffff; // all switches off (debug mode off)
    case MCU_CONTROL: return m_control_value;
    case MCU_DECODE_PLD: return 1;
    case MCU_MODE: return m_mode_value;
    case MCU_STREAM_STATUS: return m_stream_available ? 1 : 0;
    default: break;
  }
  // DRAM-C and NAND-C PLD versions.
  if (offset >= MCU_PLD_VERSION_BASE && offset < MCU_PLD_VERSION_END)
    return 1;
  return 0xffff;
}

void MemP3Board::WriteRegister(u32 offset, u16 value, u16 mask)
{
  const auto merge = [value, mask](u16& reg) { reg = static_cast<u16>((reg & ~mask) | (value & mask)); };
  switch (offset)
  {
    case MCU_STATUS:
      merge(m_status_value);
      break;

    case MCU_CONTROL:
      merge(m_control_value);
      break;

    case MCU_COMMAND_ACK:
      m_command_busy = false;
      m_mcu->SetInterruptLine(Arcade::TMP95C061::InterruptLine::INT5, false);
      break;

    case MCU_MODE:
      merge(m_mode_value);
      break;

    case MCU_MP3_DATA:
      if (mask & 0x00ff)
        m_decoder.WriteData(static_cast<u8>(value));
      break;

    case MCU_STREAM_STATUS:
      if (mask & 0x00ff)
      {
        // Cleared and set again whenever a new stream starts.
        const bool available = (value & 1) != 0;
        if (m_stream_available && !available)
          m_decoder.ResetPlayback();
        m_stream_available = available;
      }
      break;

    default:
      break;
  }
}

u8 MemP3Board::MCURead(void* context, u32 address)
{
  MemP3Board* self = static_cast<MemP3Board*>(context);
  if (address >= MCU_RAM_BASE)
    return self->m_ram[(address - MCU_RAM_BASE) & (RAM_SIZE - 1)];
  if (address >= MCU_STATUS && address < MCU_STATUS + 0x40)
  {
    const u16 word = self->ReadRegister(address & ~1u);
    return static_cast<u8>(word >> ((address & 1) * 8));
  }
  return 0xff;
}

void MemP3Board::MCUWrite(void* context, u32 address, u8 value)
{
  MemP3Board* self = static_cast<MemP3Board*>(context);
  if (address >= MCU_RAM_BASE)
  {
    self->m_ram[(address - MCU_RAM_BASE) & (RAM_SIZE - 1)] = value;
    return;
  }
  if (address >= MCU_STATUS && address < MCU_STATUS + 0x40)
  {
    const u32 shift = (address & 1) * 8;
    self->WriteRegister(address & ~1u, static_cast<u16>(value << shift), static_cast<u16>(0xff << shift));
  }
}

u8 MemP3Board::MCUPortRead(void* context, Arcade::TMP95C061::Port port)
{
  MemP3Board* self = static_cast<MemP3Board*>(context);
  switch (port)
  {
    case Arcade::TMP95C061::Port::P7:
      return static_cast<u8>((self->m_port7 & ~P7_DATA_OUT) | (self->m_decoder.GetDataOut() ? P7_DATA_OUT : 0));

    case Arcade::TMP95C061::Port::PA:
      return self->m_porta;

    case Arcade::TMP95C061::Port::PB:
      // Decoder DEMAND. The program streams while it reads high and does its
      // decoder housekeeping, including moving to the next part of a track, on
      // a pass where it reads low.
      return self->m_decoder.ReadDemand() ? 1 : 0;

    default:
      return 0;
  }
}

void MemP3Board::MCUPortWrite(void* context, Arcade::TMP95C061::Port port, u8 value)
{
  MemP3Board* self = static_cast<MemP3Board*>(context);
  switch (port)
  {
    case Arcade::TMP95C061::Port::P7:
      self->m_decoder.SetChipSelect((value & P7_CHIP_SELECT) != 0);
      self->m_decoder.SetDataIn((value & P7_DATA_IN) != 0);
      self->m_decoder.SetClock((value & P7_CLOCK) != 0);
      self->m_port7 = value;
      break;

    case Arcade::TMP95C061::Port::PA:
      self->m_porta = value;
      break;

    default:
      break;
  }
}

u16 MemP3Board::MCUAnalogRead(void* context, u32 channel)
{
  MemP3Board* self = static_cast<MemP3Board*>(context);
  return self->m_cabinet.analog ? (self->m_cabinet.analog(self->m_cabinet.context, channel) & 0x3ff) : 0;
}

// Deadlines are restored by TimingEvents; only activation is established here.
static bool DoMemP3EventActivation(StateWrapper& sw, TimingEvent* event)
{
  bool active = event && event->IsActive();
  sw.Do(&active);
  if (!sw.IsReading())
    return !sw.HasError();
  if (sw.HasError() || (active && !event))
    return false;
  if (event)
  {
    if (active)
      event->Schedule(1);
    else
      event->Deactivate();
  }
  return true;
}

bool MemP3Board::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("S10MemP3"))
    return false;

  sw.Do(&m_state_fraction);
  sw.DoBytes(m_ram.data(), m_ram.size());
  sw.Do(&m_ram_bank);
  sw.Do(&m_command_busy);
  sw.Do(&m_status_value);
  sw.Do(&m_control_value);
  sw.Do(&m_mode_value);
  sw.Do(&m_port7);
  sw.Do(&m_porta);
  sw.Do(&m_stream_available);
  if (!DoMemP3EventActivation(sw, m_event.get()) || !m_mcu->DoState(sw) || !m_decoder.DoState(sw))
    return false;

  return !sw.HasError();
}

} // namespace NamcoSystem10
