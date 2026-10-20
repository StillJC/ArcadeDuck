// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>
#include <deque>

namespace Arcade
{
// Toshiba TMP95C061 TLCS-900/H microcontroller: the CPU core plus the on-chip
// ports, 8/16-bit timers, two serial channels, A/D converter, interrupt
// controller and micro-DMA. Internal registers occupy 0x000000-0x00007F; every
// other address is forwarded to the board.
//
// fc is the oscillator frequency. One CPU state is two fc periods.
class TMP95C061
{
public:
  enum class Port : u8
  {
    P1,
    P2,
    P5,
    P6,
    P7,
    P8,
    P9,
    PA,
    PB,
    Count
  };

  enum class InterruptLine : u8
  {
    INT0,
    INT4,
    INT5,
  };

  struct Callbacks
  {
    void* context = nullptr;
    u8 (*read)(void* context, u32 address) = nullptr;
    void (*write)(void* context, u32 address, u8 value) = nullptr;
    // Pin levels of input port bits; output bits read back the port latch.
    u8 (*port_read)(void* context, Port port) = nullptr;
    void (*port_write)(void* context, Port port, u8 value) = nullptr;
    // 10-bit conversion result for AN0-AN3.
    u16 (*analog_read)(void* context, u32 channel) = nullptr;
    // A complete frame left TXDn.
    void (*serial_transmit)(void* context, u32 channel, u8 value) = nullptr;
  };

  static constexpr u32 NUM_SERIAL_CHANNELS = 2;

  explicit TMP95C061(u32 clock_hz);
  ~TMP95C061();

  void SetCallbacks(const Callbacks& callbacks);

  // Plain memory the CPU reaches without going through the read/write
  // callbacks: size bytes at base, which must lie above the internal I/O.
  void SetDirectMemory(u32 base, u8* data, u32 size);

  u32 GetClock() const { return m_clock_hz; }
  u32 GetStatesPerSecond() const { return m_clock_hz / 2; }

  // RESET input. While held the CPU does not run; releasing it loads the reset
  // vector at 0xFFFF00.
  void SetReset(bool asserted);
  bool IsInReset() const { return m_in_reset; }

  // Runs for the given number of states. Overrun is carried into the next call.
  void Run(u32 states);

  // Queues a frame for RXDn. Queued frames are shifted in at the channel's
  // programmed bit rate once the receiver is enabled.
  void ReceiveSerial(u32 channel, u8 value);

  void SetInterruptLine(InterruptLine line, bool asserted);

  u8 GetPortLatch(Port port) const { return m_port_latch[static_cast<size_t>(port)]; }
  u32 GetPC() const;

private:
  struct SerialChannel
  {
    u8 mode = 0;
    u8 control = 0;
    u8 baud_rate = 0;
    u8 rx_buffer = 0;
    bool rx_full = false;
    u8 tx_shift = 0;
    bool tx_busy = false;
    s64 tx_ticks_left = 0;
    std::deque<u8> rx_queue;
    bool rx_busy = false;
    s64 rx_ticks_left = 0;
  };

  static u8 BusRead8(void* context, u32 address);
  static u16 BusRead16(void* context, u32 address);
  static u32 BusRead32(void* context, u32 address);
  static void BusWrite8(void* context, u32 address, u8 value);
  static void BusWrite16(void* context, u32 address, u16 value);
  static void BusWrite32(void* context, u32 address, u32 value);
  static u32 ControlRead(void* context, u8 reg, unsigned size);
  static void ControlWrite(void* context, u8 reg, unsigned size, u32 value);

  void ResetPeripherals();
  void BindCore();

  u8 Read8(u32 address);
  void Write8(u32 address, u8 value);
  u8 ReadInternal(u32 address);
  void WriteInternal(u32 address, u8 value);
  u8 ReadPort(Port port);
  void WritePort(Port port, u8 value);
  void UpdatePortA();

  bool CheckInterrupts();
  bool ProcessMicroDMA();
  bool ProcessMicroDMAChannel(u32 channel);
  void UpdateTimers(u32 fc_ticks);
  void ChangeTimerFlipFlop(u32 which, u32 change);
  void UpdateAnalog(u32 states);
  void UpdateSerial(u32 channel, u32 fc_ticks);
  s64 GetSerialFrameTicks(u32 channel) const;
  void StartSerialReceive(u32 channel);

  u32 m_clock_hz;
  Callbacks m_callbacks;
  u8* m_direct_data = nullptr;
  u32 m_direct_base = 0;
  u32 m_direct_size = 0;
  bool m_in_reset = true;
  bool m_irq_inhibit = false;
  s64 m_state_balance = 0;

  std::array<u8, static_cast<size_t>(Port::Count)> m_port_latch{};
  std::array<u8, static_cast<size_t>(Port::Count)> m_port_control{};
  std::array<u8, static_cast<size_t>(Port::Count)> m_port_function{};

  u8 m_trun = 0;
  std::array<u8, 4> m_t8_reg{};
  std::array<u8, 4> m_t8_count{};
  std::array<u32, 4> m_t8_pending{};
  std::array<u8, 2> m_t8_mode{};
  u8 m_tffcr = 0;
  u8 m_trdc = 0;
  u8 m_to1 = 0;
  u8 m_to3 = 0;
  std::array<u16, 4> m_t16_reg{};
  std::array<u16, 4> m_t16_cap{};
  std::array<u16, 2> m_t16_count{};
  std::array<u8, 2> m_t16_mode{};
  std::array<u8, 2> m_t16_ffcr{};
  u8 m_t45cr = 0;
  u32 m_prescaler = 0;

  std::array<u8, 2> m_pgreg{};
  u8 m_pg01cr = 0;
  u8 m_watchdog_mode = 0;
  u8 m_od_enable = 0;

  std::array<SerialChannel, NUM_SERIAL_CHANNELS> m_serial{};

  std::array<u16, 4> m_ad_result{};
  u8 m_ad_mode = 0;
  s32 m_ad_states_left = 0;

  std::array<u8, 11> m_int_reg{};
  u8 m_iimc = 0;
  std::array<bool, 3> m_int_line{};

  std::array<u8, 4> m_dma_vector{};

  // Interrupt and micro-DMA state last found to have nothing to service.
  // Both checks run before every instruction and only change outcome when
  // a request flag, a DMA vector or the CPU's interrupt mask changes.
  struct IdleState
  {
    std::array<u8, 11> int_reg{};
    std::array<u8, 4> dma_vector{};
    u8 iff = 0;
    bool valid = false;

    bool Matches(const std::array<u8, 11>& regs, const std::array<u8, 4>& vectors, u8 mask) const
    {
      return valid && iff == mask && int_reg == regs && dma_vector == vectors;
    }
    void Store(const std::array<u8, 11>& regs, const std::array<u8, 4>& vectors, u8 mask)
    {
      int_reg = regs;
      dma_vector = vectors;
      iff = mask;
      valid = true;
    }
  };
  IdleState m_irq_idle;
  IdleState m_dma_idle;
  std::array<u32, 4> m_dma_source{};
  std::array<u32, 4> m_dma_dest{};
  std::array<u16, 4> m_dma_count{};
  std::array<u8, 4> m_dma_mode{};

  std::array<u8, 4> m_block_cs{};
  u8 m_external_cs = 0;
  std::array<u8, 4> m_mem_start{};
  std::array<u8, 4> m_mem_mask{};
  u8 m_dram_refresh = 0;
  u8 m_dram_access = 0;
};
} // namespace Arcade
