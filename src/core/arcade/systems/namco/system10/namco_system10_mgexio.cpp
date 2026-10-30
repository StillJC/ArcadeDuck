// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-FileCopyrightText: windyfairy
// SPDX-FileCopyrightText: Olivier Galibert
// SPDX-License-Identifier: GPL-3.0-only
//
// MGEXIO register map, CPU wiring and start-up timing follow MAME's
// BSD-3-Clause namcos10_exio.cpp and namcos10.cpp; the H8/3007 internal
// register map follows MAME's h83006.cpp. See THIRD-PARTY-LICENSES.txt.

#include "core/arcade/systems/namco/system10/namco_system10_mgexio.h"
#include "core/arcade/devices/cpu/h8_300h_state.h"
#include "util/state_wrapper.h"

#include "core/arcade/devices/cpu/h8_3002_intc.h"
#include "core/system.h"
#include "core/timing_event.h"

#include "common/log.h"

extern "C" {
#include "core/arcade/third_party/h8_300h/system.h"
}

#include <algorithm>
#include <bitset>
#include <cstring>
#include <optional>

Log_SetChannel(NamcoSystem10);

namespace NamcoSystem10 {
namespace {
// Main CPU offsets within the 0x1FE00000 window.
constexpr u32 RAM_WINDOW_END = 0x10000;
constexpr u32 CTRL = 0x10000;
constexpr u32 BUS_REQ = 0x18000;
constexpr u32 ERROR_STATUS = 0x20000;
constexpr u32 CPU_STATUS = 0x28000;
constexpr u32 OUTPUT_BASE = 0x40000;
constexpr u32 OUTPUT_STRIDE = 0x1000;
constexpr u32 OUTPUT_COUNT = 8;
constexpr u32 WINDOW_SIZE = 0x100000;

constexpr u32 SCHEDULER_HZ = 2000;

// The CPU starts this long after the host sets the control bit; pacmball times
// the start-up and fails when the CPU answers too early or too late.
constexpr u32 START_DELAY_MS = 40;

// H8/3007 in a 1 MiB mode: 20-bit addresses.
constexpr u32 ADDRESS_MASK = 0xFFFFF;
constexpr u32 IO_LOW_BASE = 0xEE000;
constexpr u32 IO_LOW_END = 0xEE0FF;
constexpr u32 INTERNAL_RAM_BASE = 0xFEF20;
constexpr u32 INTERNAL_RAM_END = 0xFFF1F;

constexpr u32 WATCHDOG_VECTOR = 20;
constexpr u32 REFRESH_VECTOR = 21;
constexpr u32 TIMER16_VECTOR_BASE = 24;
constexpr u32 TIMER8_CMIA_VECTOR = 36;
constexpr u32 TIMER8_CMIB_VECTOR = 38;
constexpr u32 TIMER8_OVI_VECTOR = 39;
constexpr u32 TIMER8_PAIR_VECTOR_STRIDE = 4;
} // namespace

// H8/3007 on-chip peripherals used by the MGEXIO programs.
class MGEXIOPeripherals
{
public:
  bool DoState(StateWrapper& sw)
  {
    if (!m_intc.DoState(sw))
      return false;
    sw.DoArray(m_ddr.data(), m_ddr.size());
    sw.DoArray(m_dr.data(), m_dr.size());
    sw.DoArray(m_internal_ram.data(), m_internal_ram.size());
    sw.DoArray(m_unknown.data(), m_unknown.size());
    sw.Do(&m_syscr);
    sw.Do(&m_rtmcsr);
    sw.Do(&m_rtcnt);
    sw.Do(&m_rtcor);
    sw.Do(&m_refresh_clocks);
    for (Timer8& t : m_timer8)
    {
      sw.Do(&t.tcr);
      sw.Do(&t.tcsr);
      sw.Do(&t.tcora);
      sw.Do(&t.tcorb);
      sw.Do(&t.tcnt);
    }
    sw.DoArray(m_timer8_clocks.data(), m_timer8_clocks.size());
    sw.Do(&m_tstr);
    for (Timer16& t : m_timer16)
    {
      sw.Do(&t.tcr);
      sw.Do(&t.tior);
      sw.Do(&t.tcnt);
      sw.Do(&t.gra);
      sw.Do(&t.grb);
    }
    sw.DoArray(m_timer16_clocks.data(), m_timer16_clocks.size());
    sw.DoArray(m_tisr.data(), m_tisr.size());
    sw.Do(&m_wdt_tcsr);
    sw.Do(&m_wdt_tcnt);
    sw.Do(&m_wdt_key);
    sw.Do(&m_wdt_clocks);
    return !sw.HasError();
  }

  explicit MGEXIOPeripherals(MGEXIOBoard::Cabinet& cabinet) : m_cabinet(cabinet) { Reset(); }

  void Reset()
  {
    m_intc.Reset();
    m_ddr.fill(0);
    m_dr.fill(0);
    m_internal_ram.fill(0);
    m_unknown.fill(0);
    m_logged.reset();
    m_syscr = 0x0B;
    m_rtmcsr = 0;
    m_rtcnt = 0;
    m_rtcor = 0xFF;
    m_refresh_clocks = 0;
    m_timer8 = {};
    m_timer8_clocks.fill(0);
    m_tstr = 0;
    m_timer16 = {};
    m_timer16_clocks.fill(0);
    m_tisr.fill(0);
    m_wdt_tcsr = 0x18;
    m_wdt_tcnt = 0;
    m_wdt_key = 0;
    m_wdt_clocks = 0;
  }

  Arcade::H8::H83002Intc& Intc() { return m_intc; }
  u8 SYSCR() const { return m_syscr; }

  u8 Read(u32 address)
  {
    if (address >= INTERNAL_RAM_BASE && address <= INTERNAL_RAM_END)
      return m_internal_ram[address - INTERNAL_RAM_BASE];

    switch (address)
    {
      // Data direction registers are write-only.
      case 0xEE003: case 0xEE005: case 0xEE007: case 0xEE008: case 0xEE009: case 0xEE00A:
        return 0xFF;

      case 0xEE012: return m_syscr;
      case 0xEE014: return m_intc.Read(Arcade::H8::H83002Intc::ISCR);
      case 0xEE015: return m_intc.Read(Arcade::H8::H83002Intc::IER);
      case 0xEE016: return m_intc.Read(Arcade::H8::H83002Intc::ISR);
      case 0xEE018: return m_intc.Read(Arcade::H8::H83002Intc::ICRL);
      case 0xEE019: return m_intc.Read(Arcade::H8::H83002Intc::ICRH);

      case 0xEE028: return static_cast<u8>(m_rtmcsr | 0x07);
      case 0xEE029: return m_rtcnt;
      case 0xEE02A: return m_rtcor;

      case 0xFFF60: return static_cast<u8>(m_tstr | 0xF8);
      case 0xFFF64: return static_cast<u8>(m_tisr[0] | 0x88);
      case 0xFFF65: return static_cast<u8>(m_tisr[1] | 0x88);
      case 0xFFF66: return static_cast<u8>(m_tisr[2] | 0x88);

      case 0xFFF8C: return m_wdt_tcsr;
      case 0xFFF8D: return m_wdt_tcnt;

      // SCI status: transmitter always empty.
      case 0xFFFB4: case 0xFFFBC: case 0xFFFC4:
        return 0x84;

      case 0xFFFD3: return ReadPort(MGEXIOBoard::Port::P4);
      case 0xFFFD5: return ReadPort(MGEXIOBoard::Port::P6);
      case 0xFFFD6: return ReadPort(MGEXIOBoard::Port::P7);
      case 0xFFFD7: return ReadPort(MGEXIOBoard::Port::P8);
      case 0xFFFD8: return ReadPort(MGEXIOBoard::Port::P9);
      case 0xFFFD9: return ReadPort(MGEXIOBoard::Port::PA);
      case 0xFFFDA: return ReadPort(MGEXIOBoard::Port::PB);

      default:
        break;
    }

    if (address >= 0xFFF68 && address <= 0xFFF7F)
      return ReadTimer16(address);
    if ((address >= 0xFFF80 && address <= 0xFFF89) || (address >= 0xFFF90 && address <= 0xFFF99))
      return ReadTimer8(address);

    LogAccess(address, false, 0);
    return UnknownSlot(address);
  }

  void Write(u32 address, u8 value)
  {
    if (address >= INTERNAL_RAM_BASE && address <= INTERNAL_RAM_END)
    {
      m_internal_ram[address - INTERNAL_RAM_BASE] = value;
      return;
    }

    switch (address)
    {
      case 0xEE003: m_ddr[Index(MGEXIOBoard::Port::P4)] = value; return;
      case 0xEE005: m_ddr[Index(MGEXIOBoard::Port::P6)] = value; return;
      case 0xEE007: m_ddr[Index(MGEXIOBoard::Port::P8)] = value; return;
      case 0xEE008: m_ddr[Index(MGEXIOBoard::Port::P9)] = value; return;
      case 0xEE009: m_ddr[Index(MGEXIOBoard::Port::PA)] = value; return;
      case 0xEE00A: m_ddr[Index(MGEXIOBoard::Port::PB)] = value; return;

      case 0xEE012: m_syscr = value; return;
      case 0xEE014: m_intc.Write(Arcade::H8::H83002Intc::ISCR, value); return;
      case 0xEE015: m_intc.Write(Arcade::H8::H83002Intc::IER, value); return;
      case 0xEE016: m_intc.Write(Arcade::H8::H83002Intc::ISR, value); return;
      case 0xEE018: m_intc.Write(Arcade::H8::H83002Intc::ICRL, value); return;
      case 0xEE019: m_intc.Write(Arcade::H8::H83002Intc::ICRH, value); return;

      case 0xEE028:
        // CMF clears by writing 0 after reading it as 1.
        m_rtmcsr = static_cast<u8>((value & 0x78) | (m_rtmcsr & value & 0x80));
        return;
      case 0xEE029: m_rtcnt = value; return;
      case 0xEE02A: m_rtcor = value; return;

      case 0xFFF60: m_tstr = static_cast<u8>(value & 0x07); return;
      case 0xFFF64: case 0xFFF65: case 0xFFF66:
      {
        // Interrupt enables in bits 4-6; flags in bits 0-2 clear by writing 0.
        u8& reg = m_tisr[address - 0xFFF64];
        reg = static_cast<u8>((value & 0x70) | (reg & value & 0x07));
        return;
      }

      // The watchdog registers are written as words: a key byte, then the value.
      case 0xFFF8C: case 0xFFF8E:
        m_wdt_key = value;
        return;
      case 0xFFF8D:
        if (m_wdt_key == 0xA5)
          m_wdt_tcsr = static_cast<u8>((value & 0x7F) | 0x18 | (m_wdt_tcsr & value & 0x80));
        else if (m_wdt_key == 0x5A)
          m_wdt_tcnt = value;
        return;
      case 0xFFF8F:
        return;

      case 0xFFFD3: WritePort(MGEXIOBoard::Port::P4, value); return;
      case 0xFFFD5: WritePort(MGEXIOBoard::Port::P6, value); return;
      case 0xFFFD7: WritePort(MGEXIOBoard::Port::P8, value); return;
      case 0xFFFD8: WritePort(MGEXIOBoard::Port::P9, value); return;
      case 0xFFFD9: WritePort(MGEXIOBoard::Port::PA, value); return;
      case 0xFFFDA: WritePort(MGEXIOBoard::Port::PB, value); return;

      default:
        break;
    }

    if (address >= 0xFFF68 && address <= 0xFFF7F)
    {
      WriteTimer16(address, value);
      return;
    }
    if ((address >= 0xFFF80 && address <= 0xFFF89) || (address >= 0xFFF90 && address <= 0xFFF99))
    {
      WriteTimer8(address, value);
      return;
    }

    LogAccess(address, true, value);
    UnknownSlot(address) = value;
  }

  void Advance(u32 clocks)
  {
    AdvanceRefresh(clocks);
    for (u32 ch = 0; ch < m_timer8.size(); ch++)
      AdvanceTimer8(ch, clocks);
    for (u32 ch = 0; ch < m_timer16.size(); ch++)
    {
      if (m_tstr & (1u << ch))
        AdvanceTimer16(ch, clocks);
    }
    AdvanceWatchdog(clocks);
  }

private:
  struct Timer8
  {
    u8 tcr = 0;
    u8 tcsr = 0;
    u8 tcora = 0xFF;
    u8 tcorb = 0xFF;
    u8 tcnt = 0;
  };

  struct Timer16
  {
    u8 tcr = 0;
    u8 tior = 0;
    u16 tcnt = 0;
    u16 gra = 0xFFFF;
    u16 grb = 0xFFFF;
  };

  static constexpr u32 Index(MGEXIOBoard::Port port) { return static_cast<u32>(port); }

  u8 ReadPort(MGEXIOBoard::Port port)
  {
    const u32 i = Index(port);
    const u8 input = m_cabinet.port_read ? m_cabinet.port_read(m_cabinet.context, port) : 0xFF;
    return static_cast<u8>((m_dr[i] & m_ddr[i]) | (input & ~m_ddr[i]));
  }

  void WritePort(MGEXIOBoard::Port port, u8 value)
  {
    const u32 i = Index(port);
    m_dr[i] = value;
    if (m_cabinet.port_write)
      m_cabinet.port_write(m_cabinet.context, port, static_cast<u8>(value & m_ddr[i]));
  }

  u8& UnknownSlot(u32 address) { return m_unknown[address & 0xFFF]; }

  void LogAccess(u32 address, bool write, u8 value)
  {
    const u32 slot = address & 0xFFF;
    if (m_logged.test(slot))
      return;
    m_logged.set(slot);
    if (write)
      DEV_LOG("MGEXIO H8/3007 unhandled write {:05X} = {:02X}", address, value);
    else
      DEV_LOG("MGEXIO H8/3007 unhandled read {:05X}", address);
  }

  void AdvanceRefresh(u32 clocks)
  {
    // CKS: stop, phi/2, /8, /32, /128, /512, /2048, /4096.
    static constexpr std::array<u32, 8> DIVIDERS = {0, 2, 8, 32, 128, 512, 2048, 4096};
    const u32 divider = DIVIDERS[(m_rtmcsr >> 3) & 7];
    if (divider == 0)
      return;

    m_refresh_clocks += clocks;
    while (m_refresh_clocks >= divider)
    {
      m_refresh_clocks -= divider;
      if (m_rtcnt == m_rtcor)
      {
        m_rtcnt = 0;
        m_rtmcsr |= 0x80;
        if (m_rtmcsr & 0x40)
          m_intc.InternalInterrupt(REFRESH_VECTOR);
      }
      else
      {
        m_rtcnt++;
      }
    }
  }

  u8 ReadTimer8(u32 address)
  {
    const u32 pair = (address >= 0xFFF90) ? 2 : 0;
    const u32 reg = (address - ((pair == 2) ? 0xFFF90 : 0xFFF80));
    switch (reg)
    {
      case 0: return m_timer8[pair].tcr;
      case 1: return m_timer8[pair + 1].tcr;
      case 2: return static_cast<u8>(m_timer8[pair].tcsr | 0x10);
      case 3: return static_cast<u8>(m_timer8[pair + 1].tcsr | 0x10);
      case 4: return m_timer8[pair].tcora;
      case 5: return m_timer8[pair + 1].tcora;
      case 6: return m_timer8[pair].tcorb;
      case 7: return m_timer8[pair + 1].tcorb;
      case 8: return m_timer8[pair].tcnt;
      case 9: return m_timer8[pair + 1].tcnt;
      default: return 0xFF;
    }
  }

  void WriteTimer8(u32 address, u8 value)
  {
    const u32 pair = (address >= 0xFFF90) ? 2 : 0;
    const u32 reg = (address - ((pair == 2) ? 0xFFF90 : 0xFFF80));
    Timer8& t = m_timer8[pair + (reg & 1)];
    switch (reg & ~1u)
    {
      case 0: t.tcr = value; break;
      case 2: t.tcsr = static_cast<u8>((value & 0x1F) | (t.tcsr & value & 0xE0)); break;
      case 4: t.tcora = value; break;
      case 6: t.tcorb = value; break;
      case 8: t.tcnt = value; break;
      default: break;
    }
  }

  void AdvanceTimer8(u32 ch, u32 clocks)
  {
    // CKS 1-3: phi/8, phi/64, phi/8192. Cascaded and external clocks are not used.
    static constexpr std::array<u32, 4> DIVIDERS = {0, 8, 64, 8192};
    Timer8& t = m_timer8[ch];
    const u32 cks = t.tcr & 7;
    if (cks == 0 || cks > 3)
      return;

    const u32 divider = DIVIDERS[cks];
    m_timer8_clocks[ch] += clocks;
    const u32 pair_base = (ch & 2) ? TIMER8_PAIR_VECTOR_STRIDE : 0;
    while (m_timer8_clocks[ch] >= divider)
    {
      m_timer8_clocks[ch] -= divider;
      const u8 cclr = (t.tcr >> 3) & 3;
      bool clear = false;
      if (t.tcnt == t.tcora)
      {
        t.tcsr |= 0x40;
        if (t.tcr & 0x40)
          m_intc.InternalInterrupt(TIMER8_CMIA_VECTOR + pair_base + (ch & 1));
        clear |= (cclr == 1);
      }
      if (t.tcnt == t.tcorb)
      {
        t.tcsr |= 0x80;
        if (t.tcr & 0x80)
          m_intc.InternalInterrupt(TIMER8_CMIB_VECTOR + pair_base);
        clear |= (cclr == 2);
      }

      if (clear)
      {
        t.tcnt = 0;
      }
      else if (++t.tcnt == 0)
      {
        t.tcsr |= 0x20;
        if (t.tcr & 0x20)
          m_intc.InternalInterrupt(TIMER8_OVI_VECTOR + pair_base);
      }
    }
  }

  u8 ReadTimer16(u32 address)
  {
    const u32 ch = (address - 0xFFF68) / 8;
    const u32 reg = (address - 0xFFF68) % 8;
    const Timer16& t = m_timer16[ch];
    switch (reg)
    {
      case 0: return t.tcr;
      case 1: return t.tior;
      case 2: return static_cast<u8>(t.tcnt >> 8);
      case 3: return static_cast<u8>(t.tcnt);
      case 4: return static_cast<u8>(t.gra >> 8);
      case 5: return static_cast<u8>(t.gra);
      case 6: return static_cast<u8>(t.grb >> 8);
      default: return static_cast<u8>(t.grb);
    }
  }

  void WriteTimer16(u32 address, u8 value)
  {
    const u32 ch = (address - 0xFFF68) / 8;
    const u32 reg = (address - 0xFFF68) % 8;
    Timer16& t = m_timer16[ch];
    const auto merge = [reg, value](u16& r) {
      r = (reg & 1) ? static_cast<u16>((r & 0xFF00) | value) : static_cast<u16>((r & 0x00FF) | (value << 8));
    };
    switch (reg)
    {
      case 0: t.tcr = value; break;
      case 1: t.tior = value; break;
      case 2: case 3: merge(t.tcnt); break;
      case 4: case 5: merge(t.gra); break;
      default: merge(t.grb); break;
    }
  }

  void AdvanceTimer16(u32 ch, u32 clocks)
  {
    // TPSC 0-3: phi, phi/2, phi/4, phi/8. External clocks are not used.
    Timer16& t = m_timer16[ch];
    const u32 tpsc = t.tcr & 7;
    if (tpsc > 3)
      return;

    const u32 divider = 1u << tpsc;
    m_timer16_clocks[ch] += clocks;
    const u32 ticks = m_timer16_clocks[ch] / divider;
    m_timer16_clocks[ch] %= divider;
    const u8 cclr = (t.tcr >> 5) & 3;
    const u32 bit = 1u << ch;
    for (u32 i = 0; i < ticks; i++)
    {
      bool clear = false;
      if (t.tcnt == t.gra)
      {
        m_tisr[0] |= bit;
        if (m_tisr[0] & (bit << 4))
          m_intc.InternalInterrupt(TIMER16_VECTOR_BASE + ch * 4);
        clear |= (cclr == 1);
      }
      if (t.tcnt == t.grb)
      {
        m_tisr[1] |= bit;
        if (m_tisr[1] & (bit << 4))
          m_intc.InternalInterrupt(TIMER16_VECTOR_BASE + ch * 4 + 1);
        clear |= (cclr == 2);
      }

      if (clear)
      {
        t.tcnt = 0;
      }
      else if (++t.tcnt == 0)
      {
        m_tisr[2] |= bit;
        if (m_tisr[2] & (bit << 4))
          m_intc.InternalInterrupt(TIMER16_VECTOR_BASE + ch * 4 + 2);
      }
    }
  }

  void AdvanceWatchdog(u32 clocks)
  {
    // TME (bit 5) runs the counter; interval timer mode (WT/IT = 0) raises WOVI.
    if (!(m_wdt_tcsr & 0x20))
      return;

    static constexpr std::array<u32, 8> DIVIDERS = {2, 32, 64, 128, 256, 512, 2048, 4096};
    const u32 divider = DIVIDERS[m_wdt_tcsr & 7];
    m_wdt_clocks += clocks;
    while (m_wdt_clocks >= divider)
    {
      m_wdt_clocks -= divider;
      if (++m_wdt_tcnt == 0)
      {
        m_wdt_tcsr |= 0x80;
        if (!(m_wdt_tcsr & 0x40))
          m_intc.InternalInterrupt(WATCHDOG_VECTOR);
        else if (!m_wdt_logged)
        {
          m_wdt_logged = true;
          WARNING_LOG("MGEXIO H8/3007 watchdog overflow in watchdog timer mode");
        }
      }
    }
  }

  MGEXIOBoard::Cabinet& m_cabinet;
  Arcade::H8::H83002Intc m_intc;
  std::array<u8, static_cast<size_t>(MGEXIOBoard::Port::Count)> m_ddr{};
  std::array<u8, static_cast<size_t>(MGEXIOBoard::Port::Count)> m_dr{};
  std::array<u8, INTERNAL_RAM_END - INTERNAL_RAM_BASE + 1> m_internal_ram{};
  std::array<u8, 0x1000> m_unknown{};
  std::bitset<0x1000> m_logged;
  u8 m_syscr = 0x0B;
  u8 m_rtmcsr = 0;
  u8 m_rtcnt = 0;
  u8 m_rtcor = 0xFF;
  u32 m_refresh_clocks = 0;
  std::array<Timer8, 4> m_timer8{};
  std::array<u32, 4> m_timer8_clocks{};
  u8 m_tstr = 0;
  std::array<Timer16, 3> m_timer16{};
  std::array<u32, 3> m_timer16_clocks{};
  std::array<u8, 3> m_tisr{};
  u8 m_wdt_tcsr = 0x18;
  u8 m_wdt_tcnt = 0;
  u8 m_wdt_key = 0;
  u32 m_wdt_clocks = 0;
  bool m_wdt_logged = false;
};

struct MGEXIOBus
{
  static h8_bool Read(void* opaque, unsigned address, h8_byte_t* value)
  {
    value->u = static_cast<MGEXIOBoard*>(opaque)->ReadByte(address & ADDRESS_MASK);
    return TRUE;
  }

  static h8_bool Write(void* opaque, unsigned address, h8_byte_t value)
  {
    static_cast<MGEXIOBoard*>(opaque)->WriteByte(address & ADDRESS_MASK, value.u);
    return TRUE;
  }
};

MGEXIOBoard::MGEXIOBoard(const Cabinet& cabinet)
  : m_cabinet(cabinet), m_ram(RAM_SIZE, 0), m_cpu(std::make_unique<h8_system_t>()),
    m_peripherals(std::make_unique<MGEXIOPeripherals>(m_cabinet))
{
  const TickCount interval = std::max<TickCount>(1, System::GetTicksPerSecond() / SCHEDULER_HZ);
  m_event = std::make_unique<TimingEvent>("Namco System 10 MGEXIO", interval, interval, SchedulerCallback, this);
  m_event->SetPeriodAndSchedule(interval);

  const TickCount delay = std::max<TickCount>(1, static_cast<TickCount>(static_cast<u64>(System::GetTicksPerSecond()) *
                                                                        START_DELAY_MS / 1000));
  m_start_event = std::make_unique<TimingEvent>("Namco System 10 MGEXIO start", delay, delay, StartCallback, this);
}

MGEXIOBoard::~MGEXIOBoard()
{
  if (m_event)
    m_event->Deactivate();
  if (m_start_event)
    m_start_event->Deactivate();
}

void MGEXIOBoard::Reset()
{
  // The battery-backed area keeps its contents.
  std::fill(m_ram.begin() + BACKUP_SIZE, m_ram.end(), 0);
  m_ctrl = 0;
  m_bus_req = 0;
  m_running = false;
  m_active = false;
  m_clock_fraction = 0;
  m_clock_balance = 0;
  m_start_event->Deactivate();
}

void MGEXIOBoard::StartCallback(void* param, TickCount, TickCount)
{
  MGEXIOBoard* self = static_cast<MGEXIOBoard*>(param);
  self->m_start_event->Deactivate();
  self->ReleaseReset();
}

void MGEXIOBoard::ReleaseReset()
{
  m_peripherals->Reset();
  std::memset(m_cpu.get(), 0, sizeof(h8_system_t));
  m_cpu->bus_opaque = this;
  m_cpu->bus_read = MGEXIOBus::Read;
  m_cpu->bus_write = MGEXIOBus::Write;
  m_cpu->instruction_fetch_data = m_ram.data();
  m_cpu->instruction_fetch_base = 0;
  m_cpu->instruction_fetch_size = RAM_SIZE;
  h8_init(m_cpu.get());

  m_running = true;
  m_active = true;
  m_fault_logged = false;
  m_clock_fraction = 0;
  m_clock_balance = 0;
  DEV_LOG("MGEXIO H8/3007 started pc={:05X}", static_cast<u32>(m_cpu->cpu.pc));
}

void MGEXIOBoard::SchedulerCallback(void* param, TickCount ticks, TickCount)
{
  static_cast<MGEXIOBoard*>(param)->RunCPU(ticks);
}

void MGEXIOBoard::RunCPU(TickCount ticks)
{
  if (!m_running)
    return;

  const u64 ticks_per_second = static_cast<u64>(std::max<TickCount>(System::GetTicksPerSecond(), 1));
  const u64 numerator =
    static_cast<u64>(CPU_CLOCK_HZ) * static_cast<u64>(std::max<TickCount>(ticks, 0)) + m_clock_fraction;
  m_clock_fraction = numerator % ticks_per_second;
  Execute(static_cast<u32>(numerator / ticks_per_second));
}

void MGEXIOBoard::Execute(u32 clocks)
{
  h8_system_t& h8 = *m_cpu;
  Arcade::H8::H83002Intc& intc = m_peripherals->Intc();
  m_clock_balance += clocks;
  while (m_clock_balance > 0 && m_running)
  {
    u32 step_clocks = 0;
    if (h8.sleep)
    {
      // Sleep until the next interrupt; peripherals keep running.
      step_clocks = 16;
    }
    else
    {
      const u32 pc = static_cast<u32>(h8.cpu.pc);
      h8_step(&h8);
      if (h8.error_code != H8_DEBUG_NO_ERROR)
      {
        if (!m_fault_logged)
        {
          m_fault_logged = true;
          ERROR_LOG("MGEXIO H8/3007 fault error={} line={} pc={:05X} opcode={:04X}", static_cast<u32>(h8.error_code),
                    h8.error_line, pc, h8.dbus.bits.u);
        }
        m_running = false;
        break;
      }
      step_clocks = (h8.cycles > 0) ? static_cast<u32>(h8.cycles) : 2u;
    }

    m_peripherals->Advance(step_clocks);
    m_clock_balance -= static_cast<s64>(step_clocks);

    if (intc.GetPendingMask() == 0)
      continue;

    const std::optional<u32> vector =
      intc.PeekNext(h8.cpu.ccr.flags.i != 0, h8.cpu.ccr.flags.ui != 0, m_peripherals->SYSCR());
    if (!vector)
      continue;

    h8.cycles = 0;
    if (!h8_interrupt(&h8, *vector))
      continue;
    if ((m_peripherals->SYSCR() & 0x08) == 0)
      h8.cpu.ccr.flags.ui = 1;
    intc.Acknowledge(*vector);
    m_clock_balance -= (h8.cycles > 0) ? h8.cycles : 0;
  }
}

u8 MGEXIOBoard::ReadByte(u32 address)
{
  if (address < RAM_SIZE)
    return m_ram[address];
  if ((address >= IO_LOW_BASE && address <= IO_LOW_END) || address >= INTERNAL_RAM_BASE)
    return m_peripherals->Read(address);
  return 0xFF;
}

void MGEXIOBoard::WriteByte(u32 address, u8 value)
{
  if (address < RAM_SIZE)
  {
    m_ram[address] = value;
    return;
  }
  if ((address >= IO_LOW_BASE && address <= IO_LOW_END) || address >= INTERNAL_RAM_BASE)
    m_peripherals->Write(address, value);
}

bool MGEXIOBoard::ReadHalfword(u32 offset, u16* value)
{
  if (offset < RAM_WINDOW_END)
  {
    // One board RAM byte per main CPU halfword, on the low data lines.
    *value = m_ram[offset / 2];
    return true;
  }

  switch (offset)
  {
    case CTRL:
      *value = m_ctrl;
      return true;

    case BUS_REQ:
      *value = (m_bus_req == 1) ? 2 : 0;
      return true;

    case CPU_STATUS:
      *value = m_active ? 2 : 0;
      return true;

    default:
      return false;
  }
}

bool MGEXIOBoard::WriteHalfword(u32 offset, u16 value)
{
  if (offset < RAM_WINDOW_END)
  {
    m_ram[offset / 2] = static_cast<u8>(value);
    return true;
  }

  if (offset >= OUTPUT_BASE && offset < OUTPUT_BASE + OUTPUT_COUNT * OUTPUT_STRIDE && (offset % OUTPUT_STRIDE) < 4)
  {
    if (m_cabinet.output_write)
      m_cabinet.output_write(m_cabinet.context, (offset - OUTPUT_BASE) / OUTPUT_STRIDE, value);
    return true;
  }

  switch (offset)
  {
    case CTRL:
      // Bit 0 starts the CPU after a delay; clearing it halts the CPU.
      if ((value & 1) && !(m_ctrl & 1))
      {
        m_start_event->Deactivate();
        m_start_event->Schedule(m_start_event->GetPeriod());
      }
      else if (!(value & 1) && (m_ctrl & 1))
      {
        m_start_event->Deactivate();
        m_running = false;
      }
      m_ctrl = value;
      return true;

    case BUS_REQ:
      m_bus_req = value;
      return true;

    case ERROR_STATUS:
      return true;

    default:
      return false;
  }
}

bool MGEXIOBoard::Read(u32 width, u32 offset, u32* value)
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

bool MGEXIOBoard::Write(u32 width, u32 offset, u32 value)
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
static bool DoMGEXIOEventActivation(StateWrapper& sw, TimingEvent* event)
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

// The battery-backed first 32 KiB of board RAM are NVRAM matched by fingerprint; the rest of the RAM is stored.
bool MGEXIOBoard::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("S10MGEXIO"))
    return false;

  const size_t backup_size = GetBackupRAM().size();
  if (m_ram.size() < backup_size)
    return false;
  sw.DoBytes(m_ram.data() + backup_size, m_ram.size() - backup_size);
  sw.Do(&m_clock_fraction);
  sw.Do(&m_clock_balance);
  sw.Do(&m_ctrl);
  sw.Do(&m_bus_req);
  sw.Do(&m_running);
  sw.Do(&m_active);
  if (!DoMGEXIOEventActivation(sw, m_event.get()) || !DoMGEXIOEventActivation(sw, m_start_event.get()) ||
      !Arcade::H8::DoCoreState(sw, *m_cpu) || !m_peripherals->DoState(sw))
  {
    return false;
  }

  return !sw.HasError();
}

} // namespace NamcoSystem10
