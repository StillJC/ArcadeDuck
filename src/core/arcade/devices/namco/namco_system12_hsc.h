// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>

class TimingEvent;

namespace Arcade::Namco
{
class System12HSCBoard;

// SYSTEM12 HSC PCB 8661960601: network board fitted between the System 12 mother
// board and CPU board. An H8/3002 runs a program uploaded by the game into the
// 64 KiB dual-port RAM and drives the C446/C422 ring controller. Cabinets are
// cabled in a ring, TX of one board to RX of the next.
class System12HSC
{
public:
  // Host offsets within the System 12 EXP1 window (0x1F000000).
  static constexpr u32 HOST_SHARED_BASE = 0x780000;
  static constexpr u32 HOST_SHARED_END = 0x78FFFF;
  static constexpr u32 HOST_RESET_BASE = 0x796000;
  static constexpr u32 HOST_RESET_END = 0x796003;

  static constexpr size_t SHARED_RAM_SIZE = 0x10000;

  System12HSC();
  ~System12HSC();

  void Initialize();
  void Reset();
  void Shutdown();

  bool ReadHost(u32 width, u32 offset, u32* value) const;
  bool WriteHost(u32 width, u32 offset, u32 value);

private:
  static void SchedulerCallback(void* param, TickCount ticks, TickCount ticks_late);
  static void RouteFrame(void* opaque, const System12HSCBoard* from, std::span<const u16> bytes);
  static void RegisterFromBoard(void* opaque, u16 reg, u8 value);

  void Run(TickCount ticks);
  void BoardRegisterWrite(u16 reg, u8 value);
  void SetHostInterrupt(bool asserted);

  // Host view of the dual-port RAM, H8 (big-endian) byte order.
  std::array<u8, SHARED_RAM_SIZE> m_shared_ram{};
  std::array<u8, 0x1004> m_host_registers{};

  std::unique_ptr<System12HSCBoard> m_board;
  std::unique_ptr<TimingEvent> m_event;
  u64 m_cycle_fraction = 0;
  bool m_host_irq = false;
};
} // namespace Arcade::Namco
