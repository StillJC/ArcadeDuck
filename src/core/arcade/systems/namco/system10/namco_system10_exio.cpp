// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-FileCopyrightText: windyfairy
// SPDX-License-Identifier: GPL-3.0-only
//
// EXIO register map and identifiers follow MAME's BSD-3-Clause namcos10_exio.cpp
// and namcos10.cpp. See THIRD-PARTY-LICENSES.txt.

#include "core/arcade/systems/namco/system10/namco_system10_exio.h"
#include "util/state_wrapper.h"

#include "core/system.h"
#include "core/timing_event.h"

#include "common/log.h"

#include <algorithm>

Log_SetChannel(NamcoSystem10);

namespace NamcoSystem10 {
namespace {
// Main CPU offsets within the 0x1FE00000 window.
constexpr u32 SHARED_RAM_BASE = 0x06000;
constexpr u32 SHARED_RAM_END = 0x10000;
constexpr u32 CTRL = 0x10000;
constexpr u32 BUS_REQ = 0x18000;
constexpr u32 CPU_STATUS = 0x28000;
constexpr u32 OUTPUT_LATCH = 0x48000;
constexpr u32 INPUT1 = 0x50000;
constexpr u32 INPUT2 = 0x58000;
constexpr u32 LIGHTGUN_BASE = 0x80000;
constexpr u32 LIGHTGUN_END = 0xc0000;
constexpr u32 LIGHTGUN_STRIDE = 0x8000;
constexpr u32 WINDOW_SIZE = 0x100000;

constexpr u32 SCHEDULER_HZ = 2000;
} // namespace

EXIOBoard::EXIOBoard(ExpansionBoard type, const Cabinet& cabinet) : m_type(type), m_cabinet(cabinet)
{
  if (m_type != ExpansionBoard::EXIO)
    return;

  m_mcu = std::make_unique<Arcade::TMP95C061>(MCU_CLOCK_HZ);
  Arcade::TMP95C061::Callbacks callbacks;
  callbacks.context = this;
  callbacks.read = &EXIOBoard::MCURead;
  callbacks.write = &EXIOBoard::MCUWrite;
  callbacks.port_read = &EXIOBoard::MCUPortRead;
  callbacks.port_write = &EXIOBoard::MCUPortWrite;
  callbacks.analog_read = &EXIOBoard::MCUAnalogRead;
  callbacks.serial_transmit = &EXIOBoard::MCUSerialTransmit;
  m_mcu->SetCallbacks(callbacks);

  const TickCount interval = std::max<TickCount>(1, System::GetTicksPerSecond() / SCHEDULER_HZ);
  m_event = std::make_unique<TimingEvent>("Namco System 10 EXIO", interval, interval, SchedulerCallback, this);
  m_event->SetPeriodAndSchedule(interval);
}

EXIOBoard::~EXIOBoard()
{
  if (m_event)
    m_event->Deactivate();
}

u8 EXIOBoard::GetIdentifier() const
{
  switch (m_type)
  {
    case ExpansionBoard::EXIO:
      return 0x30;
    case ExpansionBoard::EXIOG:
      return 0x32;
    default:
      return 0xff;
  }
}

void EXIOBoard::Reset()
{
  m_work_ram.fill(0);
  m_shared_ram.fill(0);
  m_ctrl = 0;
  m_bus_req = 0;
  m_active = false;
  m_analog_select = 0;
  m_state_fraction = 0;
  if (m_mcu)
    m_mcu->SetReset(true);
}

void EXIOBoard::SchedulerCallback(void* param, TickCount ticks, TickCount)
{
  static_cast<EXIOBoard*>(param)->RunMCU(ticks);
}

void EXIOBoard::RunMCU(TickCount ticks)
{
  if (!m_mcu || m_mcu->IsInReset())
    return;

  const u64 ticks_per_second = static_cast<u64>(std::max<TickCount>(System::GetTicksPerSecond(), 1));
  const u64 numerator = static_cast<u64>(m_mcu->GetStatesPerSecond()) * static_cast<u64>(std::max<TickCount>(ticks, 0)) +
                        m_state_fraction;
  m_state_fraction = numerator % ticks_per_second;
  m_mcu->Run(static_cast<u32>(numerator / ticks_per_second));
}

void EXIOBoard::ReceiveSerial(u32 channel, u8 value)
{
  if (m_mcu && !m_mcu->IsInReset())
    m_mcu->ReceiveSerial(channel, value);
}

u8 EXIOBoard::MCURead(void* context, u32 address)
{
  EXIOBoard* self = static_cast<EXIOBoard*>(context);
  if (address >= MCU_WORK_RAM_BASE && address < MCU_WORK_RAM_END)
    return self->m_work_ram[address - MCU_WORK_RAM_BASE];

  // The shared RAM mirrors through the whole space above 0x8000, which places
  // the uploaded vector table at 0xFFFF00.
  const u32 mirrored = address & 0x7fff;
  if (mirrored >= MCU_SHARED_RAM_BASE)
    return self->m_shared_ram[mirrored - MCU_SHARED_RAM_BASE];

  return 0xff;
}

void EXIOBoard::MCUWrite(void* context, u32 address, u8 value)
{
  EXIOBoard* self = static_cast<EXIOBoard*>(context);
  if (address >= MCU_WORK_RAM_BASE && address < MCU_WORK_RAM_END)
  {
    self->m_work_ram[address - MCU_WORK_RAM_BASE] = value;
    return;
  }

  const u32 mirrored = address & 0x7fff;
  if (mirrored >= MCU_SHARED_RAM_BASE)
    self->m_shared_ram[mirrored - MCU_SHARED_RAM_BASE] = value;
}

u8 EXIOBoard::MCUPortRead(void*, Arcade::TMP95C061::Port)
{
  // No board inputs are wired to the MCU ports.
  return 0x00;
}

void EXIOBoard::MCUPortWrite(void* context, Arcade::TMP95C061::Port port, u8 value)
{
  EXIOBoard* self = static_cast<EXIOBoard*>(context);
  switch (port)
  {
    case Arcade::TMP95C061::Port::P7:
      // The CPLD routes one of four analog input pairs to AN0/AN1.
      self->m_analog_select = value;
      break;

    case Arcade::TMP95C061::Port::P8:
      // The status register reads the MCU as running once the uploaded program
      // drives port 8; the exact CPLD wiring is undocumented (as in MAME).
      self->m_active |= (value != 0);
      break;

    default:
      break;
  }
}

u16 EXIOBoard::MCUAnalogRead(void* context, u32 channel)
{
  EXIOBoard* self = static_cast<EXIOBoard*>(context);
  if (channel == 2)
  {
    // The JVS sense line divider is measured on AN2. Without a connected
    // device the line is pulled up to 5 V.
    return self->m_cabinet.jvs_sense ? (self->m_cabinet.jvs_sense(self->m_cabinet.context) & 0x3ff) : 0x3ff;
  }

  if (channel >= 2 || !self->m_cabinet.analog)
    return 0;

  return self->m_cabinet.analog(self->m_cabinet.context, (self->m_analog_select & 3) * 2 + channel) & 0x3ff;
}

void EXIOBoard::MCUSerialTransmit(void* context, u32 channel, u8 value)
{
  EXIOBoard* self = static_cast<EXIOBoard*>(context);
  if (self->m_cabinet.serial_transmit)
    self->m_cabinet.serial_transmit(self->m_cabinet.context, channel, value);
}

bool EXIOBoard::ReadHalfword(u32 offset, u16* value)
{
  if (offset >= SHARED_RAM_BASE && offset < SHARED_RAM_END)
  {
    // One shared RAM byte per main CPU halfword, on the low data lines.
    *value = m_mcu ? m_shared_ram[(offset - SHARED_RAM_BASE) / 2] : 0x00ff;
    return true;
  }

  if (offset >= LIGHTGUN_BASE && offset < LIGHTGUN_END)
  {
    // Each gun axis is read as two registers: bits 0-5, then bits 6-21.
    const u32 reg = (offset - LIGHTGUN_BASE) / LIGHTGUN_STRIDE;
    const u32 sub = (offset - LIGHTGUN_BASE) % LIGHTGUN_STRIDE;
    if (sub > 2)
      return false;

    const u32 axis = m_cabinet.lightgun ? m_cabinet.lightgun(m_cabinet.context, reg / 2) : 0;
    const u16 data = (reg & 1) ? static_cast<u16>((axis >> 6) & 0xffff) : static_cast<u16>(axis & 0x3f);
    *value = (sub == 0) ? data : 0;
    return true;
  }

  switch (offset)
  {
    case CTRL:
      *value = m_mcu ? m_ctrl : 0;
      return true;

    case BUS_REQ:
      *value = (m_mcu && m_bus_req == 1) ? 2 : 0;
      return true;

    case CPU_STATUS:
      *value = (m_mcu && m_active) ? 0x0101 : 0x0000;
      return true;

    case INPUT1:
    case INPUT1 + 2:
    case INPUT2:
    case INPUT2 + 2:
    {
      const u32 port = m_cabinet.input ? m_cabinet.input(m_cabinet.context, (offset >= INPUT2) ? 1 : 0) : 0xffffffff;
      *value = static_cast<u16>(port >> ((offset & 2) * 8));
      return true;
    }

    default:
      return false;
  }
}

bool EXIOBoard::WriteHalfword(u32 offset, u16 value)
{
  if (offset >= SHARED_RAM_BASE && offset < SHARED_RAM_END)
  {
    if (m_mcu)
      m_shared_ram[(offset - SHARED_RAM_BASE) / 2] = static_cast<u8>(value);
    return true;
  }

  switch (offset)
  {
    case CTRL:
      if (!m_mcu)
        return true;

      // Bit 0 releases the MCU from reset; clearing it halts the MCU again.
      if ((value & 1) && !(m_ctrl & 1))
      {
        m_mcu->SetReset(true);
        m_mcu->SetReset(false);
        m_state_fraction = 0;
        DEV_LOG("System10 EXIO MCU released, reset PC=0x{:06X}", m_mcu->GetPC());
      }
      else if (!(value & 1) && (m_ctrl & 1))
      {
        m_mcu->SetReset(true);
      }
      m_ctrl = value;
      return true;

    case BUS_REQ:
      m_bus_req = value;
      return true;

    case OUTPUT_LATCH:
      if (m_cabinet.output)
        m_cabinet.output(m_cabinet.context, value);
      return true;

    // Strobes written by gun games before reading the gun registers.
    case 0x30000:
    case 0x40000:
    case 0xc0000:
    case 0xc8000:
      return true;

    default:
      return false;
  }
}

bool EXIOBoard::Read(u32 width, u32 offset, u32* value)
{
  if (offset >= WINDOW_SIZE)
    return false;

  u16 low = 0;
  if (!ReadHalfword(offset & ~1u, &low))
    return false;

  switch (width)
  {
    case 1:
      *value = (low >> ((offset & 1) * 8)) & 0xff;
      return true;

    case 2:
      *value = low;
      return true;

    case 4:
    {
      u16 high = 0;
      if (!ReadHalfword(offset + 2, &high))
        high = 0;
      *value = static_cast<u32>(low) | (static_cast<u32>(high) << 16);
      return true;
    }

    default:
      return false;
  }
}

bool EXIOBoard::Write(u32 width, u32 offset, u32 value)
{
  if (offset >= WINDOW_SIZE)
    return false;

  switch (width)
  {
    case 1:
    {
      u16 current = 0;
      ReadHalfword(offset & ~1u, &current);
      const u32 shift = (offset & 1) * 8;
      return WriteHalfword(offset & ~1u,
                           static_cast<u16>((current & ~(0xffu << shift)) | ((value & 0xff) << shift)));
    }

    case 2:
      return WriteHalfword(offset, static_cast<u16>(value));

    case 4:
    {
      const bool low = WriteHalfword(offset, static_cast<u16>(value));
      WriteHalfword(offset + 2, static_cast<u16>(value >> 16));
      return low;
    }

    default:
      return false;
  }
}

// Deadlines are restored by TimingEvents; only activation is established here.
static bool DoEXIOEventActivation(StateWrapper& sw, TimingEvent* event)
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

// Cabinet callbacks are host configuration; output latches are republished by the next write.
bool EXIOBoard::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("S10EXIO"))
    return false;

  bool has_cpu = static_cast<bool>(m_mcu);
  sw.Do(&has_cpu);
  if (has_cpu != static_cast<bool>(m_mcu))
    return false;

  sw.Do(&m_state_fraction);
  sw.DoArray(m_work_ram.data(), m_work_ram.size());
  sw.DoArray(m_shared_ram.data(), m_shared_ram.size());
  sw.Do(&m_ctrl);
  sw.Do(&m_bus_req);
  sw.Do(&m_active);
  sw.Do(&m_analog_select);
  if (!DoEXIOEventActivation(sw, m_event.get()))
    return false;

  return (!m_mcu || m_mcu->DoState(sw)) && !sw.HasError();
}

} // namespace NamcoSystem10
