// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/devices/audio/lc82310.h"
#include "core/arcade/devices/cpu/tmp95c061.h"
#include "core/types.h"

#include <memory>
#include <vector>

class TimingEvent;

namespace NamcoSystem10 {

// MEM(P3) board: a TMP95C061 sub-CPU runs a program the game uploads into its
// 8 MiB RAM, takes commands from the main CPU and streams MP3 data from that
// RAM to an LC82310 decoder. The sub-CPU's AN0-AN3 inputs carry cabinet analog
// controls.
class MemP3Board
{
public:
  struct Cabinet
  {
    void* context = nullptr;
    // 10-bit conversion results for AN0-AN3.
    u16 (*analog)(void* context, u32 channel) = nullptr;
  };

  explicit MemP3Board(const Cabinet& cabinet);
  ~MemP3Board();

  void Reset();

  // Main CPU accesses within the MEM(P3) register/RAM windows of EXP1.
  static bool HandlesOffset(u32 offset);
  bool Read(u32 width, u32 offset, u32* value);
  bool Write(u32 width, u32 offset, u32 value);

  void GenerateAudioFrame(s32* left, s32* right) { m_decoder.GenerateFrame(left, right); }

private:
  static constexpr u32 MCU_CLOCK_HZ = 50'745'600;
  static constexpr u32 RAM_SIZE = 0x800000;

  static void SchedulerCallback(void* param, TickCount ticks, TickCount ticks_late);
  static u8 MCURead(void* context, u32 address);
  static void MCUWrite(void* context, u32 address, u8 value);
  static u8 MCUPortRead(void* context, Arcade::TMP95C061::Port port);
  static void MCUPortWrite(void* context, Arcade::TMP95C061::Port port, u8 value);
  static u16 MCUAnalogRead(void* context, u32 channel);

  u16 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u16 value, u16 mask);
  void RunMCU(TickCount ticks);

  Cabinet m_cabinet;
  std::unique_ptr<Arcade::TMP95C061> m_mcu;
  Arcade::LC82310 m_decoder;
  std::unique_ptr<TimingEvent> m_event;
  u64 m_state_fraction = 0;

  // MCU RAM, little-endian bytes; the main CPU sees 16-bit words.
  std::vector<u8> m_ram;
  u16 m_ram_bank = 0;
  bool m_command_busy = false;

  u16 m_status_value = 0;
  u16 m_control_value = 0xffff;
  u16 m_mode_value = 0;
  u8 m_port7 = 0;
  u8 m_porta = 0;
  bool m_stream_available = false;
};

} // namespace NamcoSystem10
