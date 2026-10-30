// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system10/namco_system10_pdrive.h"
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
#include <array>
#include <cstring>
#include <deque>
#include <optional>

Log_SetChannel(NamcoSystem10);

namespace NamcoSystem10 {
namespace {
// H8/3002 advanced mode: 24-bit addresses, 128 KiB external ROM at 0, 512 bytes of on-chip RAM.
constexpr u32 ADDRESS_MASK = 0xFFFFFF;
constexpr u32 ROM_SIZE = 0x20000;
constexpr u32 RAM_BASE = 0xFFFD10;
constexpr u32 RAM_END = 0xFFFF10;
constexpr u32 IO_BASE = 0xFFFF1C;

// SCI0 at 115200 baud (BRR 3): ten bits of 128 clocks per character.
constexpr u64 SCI_CHARACTER_CLOCKS = 1280;

// ITU interrupt vectors.
constexpr u32 VECTOR_IMIA1 = 28;
constexpr u32 VECTOR_IMIB1 = 29;
constexpr u32 VECTOR_OVI1 = 30;
constexpr u32 VECTOR_IMIA2 = 32;
constexpr u32 VECTOR_IMIB2 = 33;

constexpr u8 SSR_TDRE = 0x80;
constexpr u8 SSR_RDRF = 0x40;
constexpr u8 SSR_ORER = 0x20;
constexpr u8 SSR_TEND = 0x04;
constexpr u8 SCR_TE = 0x20;
constexpr u8 SCR_RE = 0x10;

constexpr u8 TCR_PRESCALER_SHIFT[8] = {0, 1, 2, 3, 0, 0, 0, 0};
} // namespace

// The on-chip peripherals the P-DRIVE firmware uses: ports, SCI0, ITU0-ITU2 and ITU4, the
// watchdog and the interrupt controller. The A/D converter is started but never read.
class PDrivePeripherals
{
public:
  bool DoState(StateWrapper& sw)
  {
    if (!m_intc.DoState(sw))
      return false;
    sw.DoArray(m_ddr.data(), m_ddr.size());
    sw.DoArray(m_dr.data(), m_dr.size());
    sw.Do(&m_syscr);
    sw.Do(&m_smr);
    sw.Do(&m_brr);
    sw.Do(&m_scr);
    sw.Do(&m_ssr);
    sw.Do(&m_rdr);
    sw.Do(&m_tdr);
    sw.Do(&m_tdr_full);
    sw.Do(&m_shifting);
    sw.Do(&m_shift_byte);
    sw.Do(&m_shift_end);
    u32 queued = static_cast<u32>(m_rx.size());
    sw.Do(&queued);
    if (sw.IsReading())
    {
      if (sw.HasError() || queued > 0x10000)
        return false;
      m_rx.assign(queued, Received{});
    }
    for (Received& r : m_rx)
    {
      sw.Do(&r.value);
      sw.Do(&r.arrival);
    }
    sw.Do(&m_tstr);
    for (ITUChannel& c : m_itu)
    {
      sw.Do(&c.tcr);
      sw.Do(&c.tior);
      sw.Do(&c.tier);
      sw.Do(&c.tsr);
      sw.Do(&c.grb);
      sw.Do(&c.stopped_count);
    }
    sw.Do(&m_tcnt1_origin);
    sw.Do(&m_tcnt1_latch);
    sw.Do(&m_next_overflow1);
    sw.Do(&m_tcnt4_origin);
    sw.Do(&m_tcnt4_base);
    sw.Do(&m_tcnt4_latch);
    sw.Do(&m_wdt_high);
    sw.Do(&m_wdt_tcsr);
    sw.Do(&m_wdt_tcnt);
    sw.Do(&m_wdt_origin);
    sw.Do(&m_sensors);
    sw.Do(&m_next_sensor_change);
    sw.Do(&m_motor_rate);
    sw.Do(&m_outputs);
    return !sw.HasError();
  }

  explicit PDrivePeripherals(PDriveBoard& board) : m_board(board) { Reset(0); }

  void Reset(u64 clock)
  {
    m_intc.Reset();
    m_ddr.fill(0);
    m_dr.fill(0);
    m_syscr = 0x0B;
    m_scr = 0;
    m_smr = 0;
    m_brr = 0xFF;
    m_ssr = SSR_TDRE | SSR_TEND;
    m_rdr = 0;
    m_tdr = 0xFF;
    m_tdr_full = false;
    m_shift_end = 0;
    m_shifting = false;
    m_tstr = 0;
    m_itu = {};
    m_tcnt1_origin = clock;
    m_tcnt4_origin = clock;
    m_tcnt4_base = 0;
    m_next_overflow1 = UINT64_MAX;
    m_wdt_tcsr = 0x18;
    m_wdt_tcnt = 0;
    m_wdt_origin = clock;
    m_sensors = 0x0F;
    m_next_sensor_change = 0;
    // Reset releases the step pulse train and the port outputs.
    const PDriveBoard::Cabinet& cabinet = m_board.m_cabinet;
    if (m_motor_rate != 0.0 && cabinet.motor)
      cabinet.motor(cabinet.context, 0.0, clock);
    m_motor_rate = 0.0;
    UpdateOutputs();
  }

  Arcade::H8::H83002Intc& Intc() { return m_intc; }
  u8 SYSCR() const { return m_syscr; }

  // Returns true when the watchdog has overflowed and the board must reset.
  bool Advance(u64 clock)
  {
    // Captured sensor edges.
    if (clock >= m_next_sensor_change)
      UpdateSensors(clock);

    // ITU1 overflow.
    if (clock >= m_next_overflow1)
    {
      m_itu[1].tsr |= 0x04;
      if (m_itu[1].tier & 0x04)
        m_intc.InternalInterrupt(VECTOR_OVI1);
      ScheduleOverflow1(clock);
    }

    // SCI0 transmit shifter.
    if (m_shifting && clock >= m_shift_end)
    {
      m_shifting = false;
      if (m_board.m_cabinet.transmit)
        m_board.m_cabinet.transmit(m_board.m_cabinet.context, m_shift_byte);
      StartTransmit(clock);
    }

    // SCI0 receiver.
    if (!m_rx.empty() && clock >= m_rx.front().arrival)
    {
      if (m_scr & SCR_RE)
      {
        if (m_ssr & SSR_RDRF)
          m_ssr |= SSR_ORER;
        else
        {
          m_rdr = m_rx.front().value;
          m_ssr |= SSR_RDRF;
        }
      }
      m_rx.pop_front();
    }

    return AdvanceWatchdog(clock);
  }

  void SensorsChanged() { m_next_sensor_change = 0; }

  void QueueReceive(u8 value, u64 arrival)
  {
    // Characters arrive back to back at the line rate.
    if (!m_rx.empty())
      arrival = std::max(arrival, m_rx.back().arrival + SCI_CHARACTER_CLOCKS);
    m_rx.push_back({value, arrival});
  }

  u8 Read(u32 address, u64 clock)
  {
    switch (address)
    {
      case 0xFFFFB0: return m_smr;
      case 0xFFFFB1: return m_brr;
      case 0xFFFFB2: return m_scr;
      case 0xFFFFB3: return m_tdr;
      case 0xFFFFB4: return m_ssr;
      case 0xFFFFB5: return m_rdr;

      case 0xFFFFC5: case 0xFFFFC9: case 0xFFFFCD: case 0xFFFFD0: case 0xFFFFD1: case 0xFFFFD4:
        return 0xFF; // DDRs are write-only
      case 0xFFFFC7: return ReadPort(PORT4, 0xFF);
      case 0xFFFFCB: return ReadPort(PORT6, 0xFF);
      case 0xFFFFCE:
        return m_board.m_cabinet.switches ? m_board.m_cabinet.switches(m_board.m_cabinet.context, clock) : 0xFF;
      case 0xFFFFCF: return ReadPort(PORT8, 0xFF);
      case 0xFFFFD2: return ReadPort(PORT9, 0xFF);
      case 0xFFFFD3:
        UpdateSensors(clock);
        return ReadPort(PORTA, static_cast<u8>(0x0F | (m_sensors << 4)));
      case 0xFFFFD6: return ReadPort(PORTB, 0xFF);

      case 0xFFFF60: return m_tstr;
      case 0xFFFF67: return m_itu[0].tsr;
      case 0xFFFF71: return m_itu[1].tsr;
      case 0xFFFF7B: return m_itu[2].tsr;
      case 0xFFFF72: return static_cast<u8>(TCNT1(clock) >> 8);
      case 0xFFFF73: return static_cast<u8>(TCNT1(clock));
      case 0xFFFF96: return static_cast<u8>(TCNT4(clock) >> 8);
      case 0xFFFF97: return static_cast<u8>(TCNT4(clock));

      case 0xFFFFA8: return m_wdt_tcsr;
      case 0xFFFFA9: return m_wdt_tcnt;
      case 0xFFFFE8: return 0x80; // A/D conversion always complete
      case 0xFFFFF2: return m_syscr;

      default:
        if (m_intc.Handles(static_cast<u16>(address)))
          return m_intc.Read(static_cast<u16>(address));
        return 0xFF;
    }
  }

  void Write(u32 address, u8 value, u64 clock)
  {
    switch (address)
    {
      case 0xFFFFB0: m_smr = value; return;
      case 0xFFFFB1: m_brr = value; return;
      case 0xFFFFB2:
        m_scr = value;
        if (!(m_scr & SCR_TE))
        {
          m_tdr_full = false;
          m_ssr |= SSR_TDRE | SSR_TEND;
        }
        return;
      case 0xFFFFB3: m_tdr = value; return;
      case 0xFFFFB4:
      {
        // Flags clear when written as zero after being read set; TDRE clearing hands TDR over.
        const bool tdre_cleared = (m_ssr & SSR_TDRE) && !(value & SSR_TDRE);
        // TDRE/RDRF/ORER/FER/PER clear on a zero write; TEND and MPB are read-only.
        m_ssr = static_cast<u8>((m_ssr & (value & 0xF8)) | (m_ssr & 0x06) | (value & 0x01));
        if (tdre_cleared && (m_scr & SCR_TE))
        {
          m_tdr_full = true;
          m_ssr &= ~(SSR_TDRE | SSR_TEND);
          if (!m_shifting)
            StartTransmit(clock);
        }
        return;
      }

      case 0xFFFFC5: m_ddr[PORT4] = value; return;
      case 0xFFFFC7: m_dr[PORT4] = value; UpdateOutputs(); return;
      case 0xFFFFC9: m_ddr[PORT6] = value; return;
      case 0xFFFFCB: m_dr[PORT6] = value; return;
      case 0xFFFFCD: m_ddr[PORT8] = value; return;
      case 0xFFFFCF: m_dr[PORT8] = value; UpdateOutputs(); return;
      case 0xFFFFD0: m_ddr[PORT9] = value; return;
      case 0xFFFFD1: m_ddr[PORTA] = value; return;
      case 0xFFFFD2: m_dr[PORT9] = value; return;
      case 0xFFFFD3: m_dr[PORTA] = value; UpdateMotor(clock); UpdateOutputs(); return;
      case 0xFFFFD4: m_ddr[PORTB] = value; return;
      case 0xFFFFD6: m_dr[PORTB] = value; UpdateOutputs(); return;

      case 0xFFFF60:
      {
        const u8 started = static_cast<u8>(value & ~m_tstr);
        if ((value & 0x02) == 0 && (m_tstr & 0x02))
          m_itu[1].stopped_count = TCNT1(clock);
        m_tstr = value;
        if (started & 0x02)
          m_tcnt1_origin = clock - static_cast<u64>(m_itu[1].stopped_count) * 2;
        ScheduleOverflow1(clock);
        UpdateMotor(clock);
        return;
      }

      case 0xFFFF64: m_itu[0].tcr = value; UpdateMotor(clock); return;
      case 0xFFFF65: m_itu[0].tior = value; UpdateMotor(clock); return;
      case 0xFFFF66: m_itu[0].tier = value; return;
      case 0xFFFF67: m_itu[0].tsr &= value; return;
      case 0xFFFF6C: m_itu[0].grb = static_cast<u16>((m_itu[0].grb & 0x00FF) | (value << 8)); return;
      case 0xFFFF6D: m_itu[0].grb = static_cast<u16>((m_itu[0].grb & 0xFF00) | value); UpdateMotor(clock); return;

      case 0xFFFF6E: m_itu[1].tcr = value; return;
      case 0xFFFF6F: m_itu[1].tior = value; return;
      case 0xFFFF70: m_itu[1].tier = value; return;
      case 0xFFFF71: m_itu[1].tsr &= value; return;
      case 0xFFFF72: m_tcnt1_latch = value; return;
      case 0xFFFF73:
        m_itu[1].stopped_count = static_cast<u16>((m_tcnt1_latch << 8) | value);
        m_tcnt1_origin = clock - static_cast<u64>(m_itu[1].stopped_count) * 2;
        ScheduleOverflow1(clock);
        return;

      case 0xFFFF78: m_itu[2].tcr = value; return;
      case 0xFFFF79: m_itu[2].tior = value; return;
      case 0xFFFF7A: m_itu[2].tier = value; return;
      case 0xFFFF7B: m_itu[2].tsr &= value; return;

      case 0xFFFF96: m_tcnt4_latch = value; return;
      case 0xFFFF97:
        m_tcnt4_base = static_cast<u16>((m_tcnt4_latch << 8) | value);
        m_tcnt4_origin = clock;
        return;

      case 0xFFFFA8: m_wdt_high = value; return;
      case 0xFFFFA9:
        // Word writes: 5Axx loads TCNT, A5xx writes TCSR.
        if (m_wdt_high == 0x5A)
        {
          m_wdt_tcnt = value;
          m_wdt_origin = clock;
        }
        else if (m_wdt_high == 0xA5)
        {
          m_wdt_tcsr = static_cast<u8>((m_wdt_tcsr & 0x80 & value) | (value & 0x7F) | 0x18);
          m_wdt_origin = clock;
        }
        return;

      case 0xFFFFF2: m_syscr = value; return;

      default:
        if (m_intc.Handles(static_cast<u16>(address)))
          m_intc.Write(static_cast<u16>(address), value);
        return;
    }
  }

private:
  enum Port : u32
  {
    PORT4,
    PORT6,
    PORT8,
    PORT9,
    PORTA,
    PORTB,
    PORT_COUNT,
  };

  struct ITUChannel
  {
    u8 tcr = 0;
    u8 tior = 0;
    u8 tier = 0;
    u8 tsr = 0;
    u16 grb = 0xFFFF;
    u16 stopped_count = 0;
  };

  struct Received
  {
    u8 value;
    u64 arrival;
  };

  u8 ReadPort(Port port, u8 pins) const
  {
    return static_cast<u8>((m_dr[port] & m_ddr[port]) | (pins & ~m_ddr[port]));
  }

  u16 TCNT1(u64 clock) const
  {
    if (!(m_tstr & 0x02))
      return m_itu[1].stopped_count;
    return static_cast<u16>((clock - m_tcnt1_origin) / 2);
  }

  u16 TCNT4(u64 clock) const
  {
    return static_cast<u16>(m_tcnt4_base + (clock - m_tcnt4_origin) / 2);
  }

  void ScheduleOverflow1(u64 clock)
  {
    if (!(m_tstr & 0x02))
    {
      m_next_overflow1 = UINT64_MAX;
      return;
    }
    // TCNT1 counts phi/2 and overflows every 65536 counts.
    const u64 elapsed = (clock - m_tcnt1_origin) / 2;
    m_next_overflow1 = m_tcnt1_origin + ((elapsed / 0x10000) + 1) * 0x10000 * 2;
  }

  void StartTransmit(u64 clock)
  {
    if (!m_tdr_full)
    {
      m_ssr |= SSR_TEND;
      return;
    }
    m_tdr_full = false;
    m_shift_byte = m_tdr;
    m_shifting = true;
    m_shift_end = clock + SCI_CHARACTER_CLOCKS;
    m_ssr |= SSR_TDRE;
    m_ssr &= ~SSR_TEND;
  }

  void UpdateSensors(u64 clock)
  {
    const PDriveBoard::Cabinet& cabinet = m_board.m_cabinet;
    if (!cabinet.sensors)
      return;

    const u8 sensors = cabinet.sensors(cabinet.context, clock) & 0x0F;
    const u8 changed = sensors ^ m_sensors;
    m_sensors = sensors;
    m_next_sensor_change = cabinet.next_sensor_change ? cabinet.next_sensor_change(cabinet.context, clock) : UINT64_MAX;
    if (!changed)
      return;

    // Both-edge input capture: PA4/PA5 on ITU1 A/B, PA6/PA7 on ITU2 A/B.
    static constexpr std::array<u32, 4> CHANNEL = {1, 1, 2, 2};
    static constexpr std::array<u8, 4> FLAG = {0x01, 0x02, 0x01, 0x02};
    static constexpr std::array<u32, 4> VECTOR = {VECTOR_IMIA1, VECTOR_IMIB1, VECTOR_IMIA2, VECTOR_IMIB2};
    for (u32 i = 0; i < 4; i++)
    {
      if (!((changed >> i) & 1))
        continue;
      ITUChannel& itu = m_itu[CHANNEL[i]];
      const u8 io = (i & 1) ? static_cast<u8>((itu.tior >> 4) & 7) : static_cast<u8>(itu.tior & 7);
      const bool rising = ((sensors >> i) & 1) != 0;
      const bool capture = (io == 4 && rising) || (io == 5 && !rising) || io >= 6;
      if (!capture || !(m_tstr & (1u << CHANNEL[i])))
        continue;
      itu.tsr |= FLAG[i];
      if (itu.tier & FLAG[i])
        m_intc.InternalInterrupt(VECTOR[i]);
    }
  }

  void UpdateMotor(u64 clock)
  {
    // TIOCB0 toggles on every GRB0 compare match (TIOR0 IOB = 011), giving one step pulse per
    // two matches; the counter clears on GRB0 and counts phi/(1, 2, 4, 8).
    const ITUChannel& itu = m_itu[0];
    double rate = 0.0;
    if ((m_tstr & 0x01) && ((itu.tior >> 4) & 7) == 3)
    {
      const u32 prescale = 1u << TCR_PRESCALER_SHIFT[itu.tcr & 7];
      rate = static_cast<double>(PDriveBoard::CPU_CLOCK_HZ) / prescale / (static_cast<double>(itu.grb) + 1.0) / 2.0;
    }
    if (!(m_dr[PORTA] & m_ddr[PORTA] & 0x04))
      rate = -rate;
    if (rate == m_motor_rate)
      return;
    m_motor_rate = rate;
    const PDriveBoard::Cabinet& cabinet = m_board.m_cabinet;
    if (cabinet.motor)
      cabinet.motor(cabinet.context, rate, clock);
  }

  void UpdateOutputs()
  {
    // The output word as the firmware reads it back (0x11A4): PB0-7 in bits 15-8, P4.4-7 in
    // bits 7-4, P8.0 in bit 1 and PA0 in bit 0.
    u16 word = 0;
    for (u32 i = 0; i < 8; i++)
      word |= static_cast<u16>(((m_dr[PORTB] >> i) & 1) << (15 - i));
    for (u32 i = 4; i < 8; i++)
      word |= static_cast<u16>(((m_dr[PORT4] >> i) & 1) << (11 - i));
    word |= static_cast<u16>((m_dr[PORT8] & 1) << 1);
    word |= static_cast<u16>(m_dr[PORTA] & 1);
    if (word == m_outputs)
      return;
    m_outputs = word;
    const PDriveBoard::Cabinet& cabinet = m_board.m_cabinet;
    if (cabinet.outputs)
      cabinet.outputs(cabinet.context, word);
  }

  bool AdvanceWatchdog(u64 clock)
  {
    if (!(m_wdt_tcsr & 0x20))
      return false;
    static constexpr std::array<u32, 8> DIVIDER = {2, 32, 64, 128, 256, 512, 2048, 4096};
    const u64 counts = (clock - m_wdt_origin) / DIVIDER[m_wdt_tcsr & 7];
    if (m_wdt_tcnt + counts < 0x100)
      return false;
    // Watchdog mode: the overflow output resets the board.
    return (m_wdt_tcsr & 0x40) != 0;
  }

  PDriveBoard& m_board;
  Arcade::H8::H83002Intc m_intc;
  std::array<u8, PORT_COUNT> m_ddr{};
  std::array<u8, PORT_COUNT> m_dr{};
  u8 m_syscr = 0x0B;

  u8 m_smr = 0;
  u8 m_brr = 0xFF;
  u8 m_scr = 0;
  u8 m_ssr = 0;
  u8 m_rdr = 0;
  u8 m_tdr = 0;
  bool m_tdr_full = false;
  bool m_shifting = false;
  u8 m_shift_byte = 0;
  u64 m_shift_end = 0;
  std::deque<Received> m_rx;

  u8 m_tstr = 0;
  std::array<ITUChannel, 3> m_itu{};
  u64 m_tcnt1_origin = 0;
  u8 m_tcnt1_latch = 0;
  u64 m_next_overflow1 = UINT64_MAX;
  u64 m_tcnt4_origin = 0;
  u16 m_tcnt4_base = 0;
  u8 m_tcnt4_latch = 0;

  u8 m_wdt_high = 0;
  u8 m_wdt_tcsr = 0x18;
  u8 m_wdt_tcnt = 0;
  u64 m_wdt_origin = 0;

  u8 m_sensors = 0x0F;
  u64 m_next_sensor_change = 0;
  double m_motor_rate = 0.0;
  u16 m_outputs = 0;
};

struct PDriveBus
{
  static h8_bool Read(void* opaque, unsigned address, h8_byte_t* value)
  {
    value->u = static_cast<PDriveBoard*>(opaque)->ReadByte(address & ADDRESS_MASK);
    return TRUE;
  }

  static h8_bool Write(void* opaque, unsigned address, h8_byte_t value)
  {
    static_cast<PDriveBoard*>(opaque)->WriteByte(address & ADDRESS_MASK, value.u);
    return TRUE;
  }
};

PDriveBoard::PDriveBoard(std::span<const u8> firmware, const Cabinet& cabinet)
  : m_cabinet(cabinet), m_rom(ROM_SIZE, 0xFF), m_cpu(std::make_unique<h8_system_t>()),
    m_peripherals(std::make_unique<PDrivePeripherals>(*this))
{
  std::copy_n(firmware.begin(), std::min<size_t>(firmware.size(), ROM_SIZE), m_rom.begin());
  m_ram.fill(0);
  const TickCount interval = std::max<TickCount>(1, System::GetTicksPerSecond() / SCHEDULER_HZ);
  m_event = std::make_unique<TimingEvent>("Namco System 10 P-DRIVE", interval, interval, SchedulerCallback, this);
  m_event->SetPeriodAndSchedule(interval);
  ResetCPU();
}

PDriveBoard::~PDriveBoard()
{
  if (m_event)
    m_event->Deactivate();
}

void PDriveBoard::Reset()
{
  ResetCPU();
}

void PDriveBoard::ResetCPU()
{
  m_peripherals->Reset(m_clock);
  m_ram.fill(0);
  std::memset(m_cpu.get(), 0, sizeof(h8_system_t));
  m_cpu->bus_opaque = this;
  m_cpu->bus_read = PDriveBus::Read;
  m_cpu->bus_write = PDriveBus::Write;
  m_cpu->instruction_fetch_data = m_rom.data();
  m_cpu->instruction_fetch_base = 0;
  m_cpu->instruction_fetch_size = ROM_SIZE;
  h8_init(m_cpu.get());
  m_running = true;
  m_fault_logged = false;
}

u64 PDriveBoard::Now() const
{
  const u64 ticks_per_second = static_cast<u64>(std::max<TickCount>(System::GetTicksPerSecond(), 1));
  const u64 pending = static_cast<u64>(std::max<TickCount>(m_event->GetTicksSinceLastExecution(), 0));
  const s64 target = static_cast<s64>(m_clock) + m_clock_balance;
  return static_cast<u64>(std::max<s64>(target, 0)) + pending * CPU_CLOCK_HZ / ticks_per_second;
}

void PDriveBoard::SensorsChanged()
{
  m_peripherals->SensorsChanged();
}

void PDriveBoard::ReceiveSerial(u8 value)
{
  m_peripherals->QueueReceive(value, Now() + SCI_CHARACTER_CLOCKS);
}

void PDriveBoard::SchedulerCallback(void* param, TickCount ticks, TickCount)
{
  PDriveBoard* self = static_cast<PDriveBoard*>(param);
  const u64 ticks_per_second = static_cast<u64>(std::max<TickCount>(System::GetTicksPerSecond(), 1));
  const u64 numerator =
    static_cast<u64>(CPU_CLOCK_HZ) * static_cast<u64>(std::max<TickCount>(ticks, 0)) + self->m_clock_fraction;
  self->m_clock_fraction = numerator % ticks_per_second;
  self->Execute(static_cast<u32>(numerator / ticks_per_second));
}

void PDriveBoard::Execute(u32 clocks)
{
  h8_system_t& h8 = *m_cpu;
  Arcade::H8::H83002Intc& intc = m_peripherals->Intc();
  m_clock_balance += clocks;
  while (m_clock_balance > 0 && m_running)
  {
    u32 step_clocks;
    if (h8.sleep)
    {
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
          ERROR_LOG("P-DRIVE H8/3002 fault error={} line={} pc={:06X}", static_cast<u32>(h8.error_code), h8.error_line, pc);
        }
        m_running = false;
        break;
      }
      step_clocks = (h8.cycles > 0) ? static_cast<u32>(h8.cycles) : 2u;
    }

    m_clock += step_clocks;
    m_clock_balance -= static_cast<s64>(step_clocks);
    if (m_peripherals->Advance(m_clock))
    {
      DEV_LOG("P-DRIVE watchdog reset");
      ResetCPU();
      continue;
    }

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
    if (h8.cycles > 0)
    {
      m_clock += static_cast<u32>(h8.cycles);
      m_clock_balance -= h8.cycles;
    }
  }
}

u8 PDriveBoard::ReadByte(u32 address)
{
  if (address < ROM_SIZE)
    return m_rom[address];
  if (address >= RAM_BASE && address < RAM_END)
    return m_ram[address - RAM_BASE];
  if (address >= IO_BASE)
    return m_peripherals->Read(address, m_clock);
  return 0xFF;
}

void PDriveBoard::WriteByte(u32 address, u8 value)
{
  if (address >= RAM_BASE && address < RAM_END)
  {
    m_ram[address - RAM_BASE] = value;
    return;
  }
  if (address >= IO_BASE)
    m_peripherals->Write(address, value, m_clock);
}

// Deadlines are restored by TimingEvents; only activation is established here.
static bool DoPDriveEventActivation(StateWrapper& sw, TimingEvent* event)
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

// The firmware ROM is content. Timestamps are on the board clock, which is stored.
bool PDriveBoard::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("S10PDrive"))
    return false;

  sw.DoArray(m_ram.data(), m_ram.size());
  sw.Do(&m_clock);
  sw.Do(&m_clock_fraction);
  sw.Do(&m_clock_balance);
  sw.Do(&m_running);
  if (!DoPDriveEventActivation(sw, m_event.get()) || !Arcade::H8::DoCoreState(sw, *m_cpu) ||
      !m_peripherals->DoState(sw))
  {
    return false;
  }

  return !sw.HasError();
}

} // namespace NamcoSystem10
