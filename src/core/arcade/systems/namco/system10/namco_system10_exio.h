// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/devices/cpu/tmp95c061.h"
#include "core/arcade/systems/namco/system10/namco_system10_protection.h"
#include "core/types.h"

#include <array>
#include <memory>

class TimingEvent;

namespace NamcoSystem10 {

// EXIO expansion boards mapped at 0x1FE00000. Full EXIO carries a TMP95C061
// running a program the game uploads into the shared RAM; EXIO(G) only has the
// CPLD with the gun inputs.
class EXIOBoard
{
public:
  struct Cabinet
  {
    void* context = nullptr;
    // Eight 10-bit analog inputs, selected four pairs at a time by MCU port 7.
    u16 (*analog)(void* context, u32 index) = nullptr;
    // Gun axes P1 X, P1 Y, P2 X, P2 Y as 22-bit counter values.
    u32 (*lightgun)(void* context, u32 index) = nullptr;
    // EXIO_IN1/EXIO_IN2 input ports, active low.
    u32 (*input)(void* context, u32 index) = nullptr;
    // A byte left MCU serial channel n.
    void (*serial_transmit)(void* context, u32 channel, u8 value) = nullptr;
    // JVS sense level as a 10-bit conversion of the J2 sense line (AN2).
    u16 (*jvs_sense)(void* context) = nullptr;
  };

  static constexpr u32 SERIAL_CHANNELS = 2;

  EXIOBoard(ExpansionBoard type, const Cabinet& cabinet);
  ~EXIOBoard();

  u8 GetIdentifier() const;
  bool HasCPU() const { return static_cast<bool>(m_mcu); }

  void Reset();

  bool Read(u32 width, u32 offset, u32* value);
  bool Write(u32 width, u32 offset, u32 value);

  // Delivers a byte to MCU serial channel n.
  void ReceiveSerial(u32 channel, u8 value);

private:
  static constexpr u32 MCU_CLOCK_HZ = 22'118'400;
  static constexpr u32 MCU_WORK_RAM_BASE = 0x000100;
  static constexpr u32 MCU_WORK_RAM_END = 0x003000;
  static constexpr u32 MCU_SHARED_RAM_BASE = 0x003000;
  static constexpr u32 MCU_SHARED_RAM_END = 0x008000;

  static void SchedulerCallback(void* param, TickCount ticks, TickCount ticks_late);
  static u8 MCURead(void* context, u32 address);
  static void MCUWrite(void* context, u32 address, u8 value);
  static u8 MCUPortRead(void* context, Arcade::TMP95C061::Port port);
  static void MCUPortWrite(void* context, Arcade::TMP95C061::Port port, u8 value);
  static u16 MCUAnalogRead(void* context, u32 channel);
  static void MCUSerialTransmit(void* context, u32 channel, u8 value);

  bool ReadHalfword(u32 offset, u16* value);
  bool WriteHalfword(u32 offset, u16 value);
  void RunMCU(TickCount ticks);

  ExpansionBoard m_type;
  Cabinet m_cabinet;
  std::unique_ptr<Arcade::TMP95C061> m_mcu;
  std::unique_ptr<TimingEvent> m_event;
  u64 m_state_fraction = 0;

  std::array<u8, MCU_WORK_RAM_END - MCU_WORK_RAM_BASE> m_work_ram{};
  std::array<u8, MCU_SHARED_RAM_END - MCU_SHARED_RAM_BASE> m_shared_ram{};
  u16 m_ctrl = 0;
  u16 m_bus_req = 0;
  bool m_active = false;
  u8 m_analog_select = 0;
};

} // namespace NamcoSystem10
