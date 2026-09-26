// SPDX-FileCopyrightText: Olivier Galibert
// SPDX-FileCopyrightText: R. Belmont
// SPDX-FileCopyrightText: hap
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: BSD-3-Clause
//
// Modified for ArcadeDuck by StillJC, 2026.
//
// Standalone Panasonic MN1020012A core adapted from MAME's MN10200 CPU device.

#pragma once

#include "core/types.h"

#include <array>
#include <functional>

namespace MN10200 {

class Core
{
public:
  static constexpr u32 NUM_EXT_IRQS = 4;

  using Read8Callback = std::function<u8(u32)>;
  using Read16Callback = std::function<u16(u32)>;
  using Write8Callback = std::function<void(u32, u8)>;
  using Write16Callback = std::function<void(u32, u16)>;
  using PortReadCallback = std::function<u8(u32)>;
  using PortWriteCallback = std::function<void(u32, u8)>;

  Core() = default;

  void Initialize(Read8Callback read8, Read16Callback read16, Write8Callback write8, Write16Callback write16,
                  PortReadCallback port_read, PortWriteCallback port_write);
  void Reset();
  int Execute(int cycles);

  // MAME's external-input API uses ASSERT_LINE as a low physical pin.
  void SetInput(u32 irq, bool asserted);
  void PulseIRQ(u32 irq);

  u32 GetPC() const { return m_pc & 0x00ffffffu; }
  u16 GetPSW() const { return m_psw; }

private:
  static constexpr u32 NUM_PRESCALERS = 2;
  static constexpr u32 NUM_TIMERS_8BIT = 10;
  static constexpr u32 NUM_IRQ_GROUPS = 31;

  enum Flag : u16
  {
    FLAG_ZF  = 0x0001,
    FLAG_NF  = 0x0002,
    FLAG_CF  = 0x0004,
    FLAG_VF  = 0x0008,
    FLAG_ZX  = 0x0010,
    FLAG_NX  = 0x0020,
    FLAG_CX  = 0x0040,
    FLAG_VX  = 0x0080,
    FLAG_IM0 = 0x0100,
    FLAG_IM1 = 0x0200,
    FLAG_IM2 = 0x0400,
    FLAG_IE  = 0x0800,
    FLAG_S0  = 0x1000,
    FLAG_S1  = 0x2000,
    FLAG_D14 = 0x4000,
    FLAG_D15 = 0x8000
  };

  Read8Callback m_read8;
  Read16Callback m_read16;
  Write8Callback m_write8;
  Write16Callback m_write16;
  PortReadCallback m_port_read;
  PortWriteCallback m_port_write;

  int m_cycles = 0;
  u32 m_pc = 0;
  std::array<u32, 4> m_d{};
  std::array<u32, 4> m_a{};
  u16 m_psw = 0;
  u16 m_mdr = 0;

  std::array<u8, NUM_IRQ_GROUPS> m_icrl{};
  std::array<u8, NUM_IRQ_GROUPS> m_icrh{};
  u8 m_nmicr = 0;
  u8 m_iagr = 0;
  u8 m_extmdl = 0;
  u8 m_extmdh = 0;
  bool m_possible_irq = false;

  struct SimpleTimer
  {
    u8 mode = 0;
    u8 base = 0;
    u8 cur = 0;
    s64 cycles_until_tick = -1;
  };
  std::array<SimpleTimer, NUM_TIMERS_8BIT> m_simple_timer{};

  struct Prescaler
  {
    u8 mode = 0;
    u8 base = 0;
    u8 cur = 0;
  };
  std::array<Prescaler, NUM_PRESCALERS> m_prescaler{};

  struct DMAState
  {
    u32 adr = 0;
    u32 count = 0;
    u16 iadr = 0;
    u8 ctrll = 0;
    u8 ctrlh = 0;
    u8 irq = 0;
  };
  std::array<DMAState, 8> m_dma{};

  struct SerialState
  {
    u8 ctrll = 0;
    u8 ctrlh = 0;
    u8 buf = 0;
    u8 recv = 0;
  };
  std::array<SerialState, 2> m_serial{};

  u8 m_pplul = 0;
  u8 m_ppluh = 0;
  u8 m_p3md = 0;
  u8 m_p4 = 0x0f;

  struct PortState
  {
    u8 out = 0;
    u8 dir = 0;
  };
  std::array<PortState, 4> m_port{};

  u8 ReadByte(u32 address);
  u16 ReadWord(u32 address);
  void WriteByte(u32 address, u8 value);
  void WriteWord(u32 address, u16 value);
  u8 ReadPort(u32 port) const;
  void WritePort(u32 port, u8 value);

  inline u8 read_arg8(u32 address) { return ReadByte(address); }
  inline u16 read_arg16(u32 address)
  {
    return static_cast<u16>(ReadByte(address) | (static_cast<u16>(ReadByte(address + 1)) << 8));
  }
  inline u32 read_arg24(u32 address)
  {
    return static_cast<u32>(ReadByte(address)) | (static_cast<u32>(ReadByte(address + 1)) << 8) |
           (static_cast<u32>(ReadByte(address + 2)) << 16);
  }

  inline u8 read_mem8(u32 address) { return ReadByte(address); }
  inline u16 read_mem16(u32 address) { return ReadWord(address & ~1u); }
  inline u32 read_mem24(u32 address)
  {
    address &= ~1u;
    return static_cast<u32>(ReadWord(address)) | (static_cast<u32>(ReadByte(address + 2)) << 16);
  }

  inline void write_mem8(u32 address, u8 data) { WriteByte(address, data); }
  inline void write_mem16(u32 address, u16 data) { WriteWord(address & ~1u, data); }
  inline void write_mem24(u32 address, u32 data)
  {
    address &= ~1u;
    WriteWord(address, static_cast<u16>(data));
    WriteByte(address + 2, static_cast<u8>(data >> 16));
  }

  inline void change_pc(u32 pc) { m_pc = pc & 0x00ffffffu; }

  void take_irq(int level, int group);
  void check_irq();
  void check_ext_irq();

  int TimerTickSimple(int timer);
  void RefreshTimer(int timer);
  void RefreshAllTimers();
  void AdvanceTimers(int cycles);

  u8 ReadInternalIO(u32 offset);
  void WriteInternalIO(u32 offset, u8 data);

  void illegal(u8 prefix, u8 op);
  u32 do_add(u32 a, u32 b, u32 c = 0);
  u32 do_sub(u32 a, u32 b, u32 c = 0);
  void test_nz16(u16 value);
  void do_jsr(u32 to, u32 ret);
  void do_branch(int condition = 1);
};

} // namespace MN10200
