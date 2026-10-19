// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>
#include <memory>
#include <span>
#include <vector>

class TimingEvent;
struct h8_system_t;

namespace NamcoSystem10 {

class MGEXIOPeripherals;

// MGEXIO medal game expansion board mapped at 0x1FE00000. An H8/3007 runs a
// program the game uploads into the board's 512 KiB RAM; the first 32 KiB are
// battery backed. The CPU ports drive the medal mechanics.
class MGEXIOBoard
{
public:
  // H8/3007 I/O ports wired on the board.
  enum class Port : u8
  {
    P4,
    P6,
    P7,
    P8,
    P9,
    PA,
    PB,
    Count
  };

  struct Cabinet
  {
    void* context = nullptr;
    u8 (*port_read)(void* context, Port port) = nullptr;
    void (*port_write)(void* context, Port port, u8 value) = nullptr;
    // Host output latches at 0x1FE40000 + n * 0x1000.
    void (*output_write)(void* context, u32 index, u16 value) = nullptr;
  };

  static constexpr u32 RAM_SIZE = 0x80000;
  static constexpr u32 BACKUP_SIZE = 0x8000;

  explicit MGEXIOBoard(const Cabinet& cabinet);
  ~MGEXIOBoard();

  static constexpr u8 GetIdentifier() { return 0x33; }

  void Reset();

  bool Read(u32 width, u32 offset, u32* value);
  bool Write(u32 width, u32 offset, u32 value);

  std::span<u8> GetBackupRAM() { return std::span<u8>(m_ram.data(), BACKUP_SIZE); }

private:
  friend struct MGEXIOBus;

  static constexpr u32 CPU_CLOCK_HZ = 14'745'600;

  static void SchedulerCallback(void* param, TickCount ticks, TickCount ticks_late);
  static void StartCallback(void* param, TickCount ticks, TickCount ticks_late);

  u8 ReadByte(u32 address);
  void WriteByte(u32 address, u8 value);

  bool ReadHalfword(u32 offset, u16* value);
  bool WriteHalfword(u32 offset, u16 value);

  void ReleaseReset();
  void RunCPU(TickCount ticks);
  void Execute(u32 clocks);

  Cabinet m_cabinet;
  std::vector<u8> m_ram;
  std::unique_ptr<h8_system_t> m_cpu;
  std::unique_ptr<MGEXIOPeripherals> m_peripherals;
  std::unique_ptr<TimingEvent> m_event;
  std::unique_ptr<TimingEvent> m_start_event;
  u64 m_clock_fraction = 0;
  s64 m_clock_balance = 0;

  u16 m_ctrl = 0;
  u16 m_bus_req = 0;
  bool m_running = false;
  bool m_active = false;
  bool m_fault_logged = false;
};

} // namespace NamcoSystem10
