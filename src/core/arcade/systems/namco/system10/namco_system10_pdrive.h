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

class PDrivePeripherals;
struct PDriveBus;

// Namco P-DRIVE PCB ("Stepping Motor Driver"): an H8/3002 JVS I/O node used by Knock Down 2001.
// The firmware answers JVS on SCI0, drives a stepper driver with a step pulse train from ITU0
// (TIOCB0) and a direction line (PA2), sets lamp outputs, reads two limit switches on P7.0/P7.1,
// and times how long each of four active-low sensors (PA4-PA7, captured on ITU1/ITU2) stays blocked.
class PDriveBoard
{
public:
  static constexpr u32 CPU_CLOCK_HZ = 14'745'600;

  struct Cabinet
  {
    void* context = nullptr;
    // PA4-PA7 sensor levels in bits 0-3 (1 = clear) at board clock `clock`.
    u8 (*sensors)(void* context, u64 clock) = nullptr;
    // First board clock after `clock` at which a sensor level may change, or UINT64_MAX.
    u64 (*next_sensor_change)(void* context, u64 clock) = nullptr;
    // Port 7 pin levels (limit switches on P7.0/P7.1, active low) at board clock `clock`.
    u8 (*switches)(void* context, u64 clock) = nullptr;
    // Step pulse rate (steps per second, negative = PA2 clear) from board clock `clock`.
    void (*motor)(void* context, double steps_per_second, u64 clock) = nullptr;
    // General-purpose output word as the JVS output commands set it.
    void (*outputs)(void* context, u16 value) = nullptr;
    // A JVS byte the board transmitted.
    void (*transmit)(void* context, u8 value) = nullptr;
  };

  PDriveBoard(std::span<const u8> firmware, const Cabinet& cabinet);
  ~PDriveBoard();

  void Reset();

  // A JVS byte from the host, arriving one character time after the current emulated time.
  void ReceiveSerial(u8 value);

  // Board clock that corresponds to the current emulated time.
  u64 Now() const;

  // The cabinet's sensor timeline changed; sensors are sampled again on the next step.
  void SensorsChanged();

private:
  friend class PDrivePeripherals;
  friend struct PDriveBus;

  static constexpr u32 SCHEDULER_HZ = 4000;

  static void SchedulerCallback(void* param, TickCount ticks, TickCount ticks_late);
  void Execute(u32 clocks);
  void ResetCPU();

  u8 ReadByte(u32 address);
  void WriteByte(u32 address, u8 value);

  Cabinet m_cabinet;
  std::vector<u8> m_rom;
  std::array<u8, 0x200> m_ram{};
  std::unique_ptr<h8_system_t> m_cpu;
  std::unique_ptr<PDrivePeripherals> m_peripherals;
  std::unique_ptr<TimingEvent> m_event;
  u64 m_clock = 0;
  u64 m_clock_fraction = 0;
  s64 m_clock_balance = 0;
  bool m_running = false;
  bool m_fault_logged = false;
};

} // namespace NamcoSystem10
