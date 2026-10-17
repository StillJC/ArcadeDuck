// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "namco_system12_hsc.h"

#include "core/arcade/devices/cpu/h8_3002_dmac.h"
#include "core/arcade/devices/cpu/h8_3002_intc.h"
#include "core/arcade/devices/cpu/h8_3002_peripherals.h"
#include "core/arcade/devices/cpu/h8_3002_timer16.h"
#include "core/interrupt_controller.h"
#include "core/system.h"
#include "core/timing_event.h"

#include "common/log.h"

extern "C" {
#include "core/arcade/third_party/h8_300h/system.h"
}

#include <algorithm>
#include <cstring>
#include <deque>

Log_SetChannel(NamcoSystem12HSC);

namespace Arcade::Namco
{
namespace
{
constexpr u32 CLOCK_HZ = 14'745'600;
constexpr u32 SCHEDULER_HZ = 1000;

// H8 address map (MAME kartduel_state::linkcpu_program_map).
constexpr u32 H8_SHARED_END = 0x00FFFF;
constexpr u32 H8_WORK_BASE = 0x010000;
constexpr u32 H8_WORK_END = 0x01FFFF;
constexpr u32 H8_MAILBOX_BASE = 0x016014;
constexpr u32 H8_MAILBOX_END = 0x016017;
constexpr u32 H8_STATUS_BASE = 0x017000;
constexpr u32 H8_STATUS_END = 0x017003;
constexpr u32 H8_PACKET_BASE = 0x200000;
constexpr u32 H8_PACKET_END = 0x201FFF;
constexpr u32 H8_RX_BASE = 0x400000;
constexpr u32 H8_RX_END = 0x400007;
constexpr u32 H8_RX_ACK_BASE = 0x408000;
constexpr u32 H8_RX_ACK_END = 0x408007;
constexpr u32 H8_TX_BASE = 0x410000;
constexpr u32 H8_TX_END = 0x41000F;
constexpr u32 H8_MISC_BASE = 0x418000;
constexpr u32 H8_MISC_END = 0x418007;
constexpr u32 H8_INTERNAL_BASE = 0xFF0000;

// The ITU is brought up to date in steps of at most this many clocks, and
// before any access to its registers.
constexpr u32 TIMER_BATCH_CLOCKS = 128;

constexpr size_t WORK_RAM_SIZE = 0x10000;
constexpr size_t PACKET_BUFFER_SIZE = 0x2000;
constexpr u16 PACKET_OFFSET_MASK = 0x1FFF;
constexpr u32 RX_IRQ = 0;

// Packet buffer bytes carry an end-of-frame mark, set for CPU writes while bit 9
// of the receive control register is set. The firmware sets it only around the
// last byte of each frame it builds; data transfers carry two (header, trailer).
constexpr u16 RX_CONTROL_MARK = 0x0200;
constexpr u16 RING_MARK = 0x100;

// The line rate is set by the host through the board's 2061ASC clock
// generator and is not known; this is the time on the ring per byte.
constexpr u32 LINE_BYTE_CLOCKS = 64;

// Registers shared by the H8 and the host, as offsets from the host reset
// register. The board raises PS1 IRQ10 by writing the mailbox; the host
// acknowledges through the following halfword.
constexpr u16 REG_MAILBOX = 0x0014;
constexpr u16 REG_MAILBOX_ACK = 0x0016;

// Byte-swapped 13-bit buffer offset used by the pointer registers.
u16 DecodeOffset(u16 reg)
{
  return static_cast<u16>(((reg & 0xFF) << 8 | (reg >> 8)) & PACKET_OFFSET_MASK);
}

u16 EncodeOffset(u16 offset)
{
  return static_cast<u16>((offset & 0xFF) << 8 | (offset >> 8));
}

bool FitsRange(u32 offset, u32 width, u32 base, u32 end)
{
  return (width == 1 || width == 2 || width == 4) && offset >= base && offset <= end && width <= (end - offset + 1);
}

bool HostRegisterRAM(u32 offset, u32 width)
{
  return FitsRange(offset, width, 0x796004, 0x79600B) || FitsRange(offset, width, 0x796014, 0x796017) ||
         FitsRange(offset, width, 0x796020, 0x796023) || FitsRange(offset, width, 0x797000, 0x797003);
}

} // namespace

// One HSC board: H8/3002 link CPU and the C446/C422 ring controller.
class System12HSCBoard
{
public:
  // Ring bytes: data in bits 0-7, end-of-frame mark in bit 8.
  using FrameSink = void (*)(void* opaque, const System12HSCBoard* from, std::span<const u16> bytes);
  // reg is the host register offset (little-endian byte order).
  using RegisterSink = void (*)(void* opaque, u16 reg, u8 value);

  System12HSCBoard(u8 cabinet, u8* shared_ram, void* owner, FrameSink sink);

  void SetRegisterSink(RegisterSink sink, void* opaque)
  {
    m_register_sink = sink;
    m_register_opaque = opaque;
  }

  void SetReset(bool asserted);
  void Run(u32 clocks);
  void Receive(std::span<const u16> bytes);

private:
  friend struct System12HSCBus;

  u8 ReadByte(u32 address);
  void WriteByte(u32 address, u8 value);
  u8 ReadInternal(u16 reg);
  void WriteInternal(u16 reg, u8 value);

  void ReleaseReset();
  void AdvanceDevices(u32 clocks);
  void SyncTimer();
  u32 ServiceInterrupts();
  void Transmit(u32 length);
  void AdvanceTransmit(u32 clocks);
  void AdvanceReceive(u32 clocks);

  const u8 m_cabinet;
  u8* const m_shared_ram;
  void* const m_owner;
  const FrameSink m_sink;
  RegisterSink m_register_sink = nullptr;
  void* m_register_opaque = nullptr;

  std::unique_ptr<h8_system_t> m_cpu;
  H8::H83002Intc m_intc;
  H8::H83002Timer16 m_timer16;
  H8::H83002Peripherals m_peripherals;
  H8::H83002DMAC m_dmac;

  std::array<u8, WORK_RAM_SIZE> m_work_ram{};
  std::array<u8, PACKET_BUFFER_SIZE> m_packet_buffer{};
  std::array<bool, PACKET_BUFFER_SIZE> m_packet_mark{};
  std::array<u8, 0x10000> m_internal{};

  bool m_reset = true;
  bool m_fault_logged = false;
  s64 m_cycle_balance = 0;
  u32 m_dma_states = 0;
  u32 m_timer_clocks = 0;

  // Ring controller. Pointers are 13-bit packet buffer offsets.
  std::array<u16, 4> m_rx_regs{};
  std::array<u16, 8> m_tx_regs{};
  std::array<u16, 4> m_misc_regs{};
  u16 m_rx_pointer = 0;
  u16 m_rx_frame_end = 0;
  u16 m_tx_offset = 0;
  bool m_rx_enabled = false;
  u32 m_rx_clocks = 0;
  std::deque<u16> m_rx_line;

  struct TxJob
  {
    u16 offset;
    u32 remaining;
  };
  std::deque<TxJob> m_tx_jobs;
  u32 m_tx_clocks = 0;
};

struct System12HSCBus
{
  static h8_bool Read(void* opaque, unsigned address, h8_byte_t* value)
  {
    value->u = static_cast<System12HSCBoard*>(opaque)->ReadByte(address & 0xFFFFFF);
    return TRUE;
  }

  static h8_bool Write(void* opaque, unsigned address, h8_byte_t value)
  {
    static_cast<System12HSCBoard*>(opaque)->WriteByte(address & 0xFFFFFF, value.u);
    return TRUE;
  }

  static u8 DMARead(void* opaque, u32 address) { return static_cast<System12HSCBoard*>(opaque)->ReadByte(address); }

  static void DMAWrite(void* opaque, u32 address, u8 value)
  {
    static_cast<System12HSCBoard*>(opaque)->WriteByte(address, value);
  }
};

System12HSCBoard::System12HSCBoard(u8 cabinet, u8* shared_ram, void* owner, FrameSink sink)
  : m_cabinet(cabinet), m_shared_ram(shared_ram), m_owner(owner), m_sink(sink), m_cpu(std::make_unique<h8_system_t>())
{
  m_dmac.SetBus(this, System12HSCBus::DMARead, System12HSCBus::DMAWrite);

  // SRAM powers up with board-specific contents. The firmware hashes part of
  // the packet buffer and its internal RAM into the seed it uses to elect the
  // ring master, so linked boards must not start identical.
  u32 state = 0x9E3779B9u * (static_cast<u32>(cabinet) + 1);
  const auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<u8>(state);
  };
  for (u8& b : m_packet_buffer)
    b = next();
  for (u32 i = 0xFD10; i <= 0xFF0F; i++)
    m_internal[i] = next();
}

void System12HSCBoard::SetReset(bool asserted)
{
  if (asserted)
  {
    m_reset = true;
    m_rx_enabled = false;
    m_rx_line.clear();
    m_tx_jobs.clear();
  }
  else if (m_reset)
  {
    ReleaseReset();
  }
}

void System12HSCBoard::ReleaseReset()
{
  m_reset = false;
  m_fault_logged = false;
  m_cycle_balance = 0;
  m_dma_states = 0;
  m_timer_clocks = 0;

  m_intc.Reset();
  m_timer16.Reset();
  m_peripherals.Reset();
  m_dmac.Reset();

  m_rx_regs.fill(0);
  m_tx_regs.fill(0);
  m_misc_regs.fill(0);
  m_rx_pointer = 0;
  m_rx_frame_end = 0;
  m_tx_offset = 0;
  m_rx_enabled = false;
  m_rx_clocks = 0;
  m_rx_line.clear();
  m_tx_jobs.clear();
  m_tx_clocks = 0;

  std::memset(m_cpu.get(), 0, sizeof(h8_system_t));
  m_cpu->bus_opaque = this;
  m_cpu->bus_read = System12HSCBus::Read;
  m_cpu->bus_write = System12HSCBus::Write;
  h8_init(m_cpu.get());

  // The program runs from the dual-port RAM the host uploads it into.
  m_cpu->instruction_fetch_data = m_shared_ram;
  m_cpu->instruction_fetch_base = 0;
  m_cpu->instruction_fetch_size = static_cast<unsigned>(System12HSC::SHARED_RAM_SIZE);

  DEV_LOG("HSC board {} H8/3002 released from reset pc={:06X}", m_cabinet, static_cast<u32>(m_cpu->cpu.pc));
}

void System12HSCBoard::Run(u32 clocks)
{
  if (m_reset)
    return;

  h8_system_t& h8 = *m_cpu;
  if (h8.error_code != H8_DEBUG_NO_ERROR)
    return;

  m_cycle_balance += clocks;
  while (m_cycle_balance > 0 && !m_reset)
  {
    const u32 pc = static_cast<u32>(h8.cpu.pc);
    m_dma_states = 0;
    h8_step(&h8);
    if (h8.error_code != H8_DEBUG_NO_ERROR)
    {
      if (!m_fault_logged)
      {
        m_fault_logged = true;
        ERROR_LOG("HSC board {} H8/3002 fault error={} line={} pc={:06X} opcode={:04X}", m_cabinet,
                  static_cast<u32>(h8.error_code), h8.error_line, pc, h8.dbus.bits.u);
      }
      break;
    }

    const u32 step_clocks = ((h8.cycles > 0) ? static_cast<u32>(h8.cycles) : 2u) + m_dma_states;
    AdvanceDevices(step_clocks);
    m_cycle_balance -= static_cast<s64>(step_clocks);

    const u32 interrupt_clocks = ServiceInterrupts();
    if (interrupt_clocks != 0)
    {
      AdvanceDevices(interrupt_clocks);
      m_cycle_balance -= static_cast<s64>(interrupt_clocks);
    }
  }
}

void System12HSCBoard::AdvanceDevices(u32 clocks)
{
  m_timer_clocks += clocks;
  if (m_timer_clocks >= TIMER_BATCH_CLOCKS)
    SyncTimer();

  m_peripherals.Advance(clocks);
  AdvanceTransmit(clocks);
  AdvanceReceive(clocks);

  while (const std::optional<u32> vector = m_peripherals.TakePendingInterrupt())
    m_intc.InternalInterrupt(*vector);
  while (const std::optional<u32> vector = m_dmac.TakePendingInterrupt())
    m_intc.InternalInterrupt(*vector);
}

void System12HSCBoard::SyncTimer()
{
  m_timer16.Advance(m_timer_clocks);
  m_timer_clocks = 0;
  while (const std::optional<u32> vector = m_timer16.TakePendingInterrupt())
    m_intc.InternalInterrupt(*vector);
}

u32 System12HSCBoard::ServiceInterrupts()
{
  if (m_intc.GetPendingMask() == 0)
    return 0;

  h8_system_t& h8 = *m_cpu;
  const std::optional<u32> vector =
    m_intc.PeekNext(h8.cpu.ccr.flags.i != 0, h8.cpu.ccr.flags.ui != 0, m_peripherals.SYSCR());
  if (!vector)
    return 0;

  h8.cycles = 0;
  if (!h8_interrupt(&h8, *vector))
    return 0;

  if ((m_peripherals.SYSCR() & 0x08) == 0)
    h8.cpu.ccr.flags.ui = 1;

  m_intc.Acknowledge(*vector);
  return (h8.cycles > 0) ? static_cast<u32>(h8.cycles) : 0;
}

u8 System12HSCBoard::ReadByte(u32 address)
{
  if (address <= H8_SHARED_END)
    return m_shared_ram[address];
  if (address >= H8_WORK_BASE && address <= H8_WORK_END)
    return m_work_ram[address - H8_WORK_BASE];
  if (address >= H8_PACKET_BASE && address <= H8_PACKET_END)
    return m_packet_buffer[address - H8_PACKET_BASE];

  if ((address >= H8_RX_BASE && address <= H8_RX_END) || (address >= H8_RX_ACK_BASE && address <= H8_RX_ACK_END))
  {
    // The pointer reads back the end of the last complete frame received.
    const u32 reg = (address & 7) >> 1;
    const u16 value = (reg == 3) ? EncodeOffset(m_rx_frame_end) : m_rx_regs[reg];
    return static_cast<u8>((address & 1) ? value : (value >> 8));
  }

  if (address >= H8_TX_BASE && address <= H8_TX_END)
  {
    // The command register reads back the bytes still to be sent.
    const u32 reg = (address & 0xF) >> 1;
    u32 remaining = 0;
    for (const TxJob& job : m_tx_jobs)
      remaining += job.remaining;
    remaining = std::min<u32>(remaining, 0xFF);
    const u16 value = (reg == 2) ? static_cast<u16>(remaining << 8) :
                      (reg == 3) ? EncodeOffset(m_tx_offset) : m_tx_regs[reg];
    return static_cast<u8>((address & 1) ? value : (value >> 8));
  }

  if (address >= H8_MISC_BASE && address <= H8_MISC_END)
  {
    const u16 value = m_misc_regs[(address & 7) >> 1];
    return static_cast<u8>((address & 1) ? value : (value >> 8));
  }

  if (address >= H8_INTERNAL_BASE)
    return ReadInternal(static_cast<u16>(address));

  return 0xFF;
}

void System12HSCBoard::WriteByte(u32 address, u8 value)
{
  if (address <= H8_SHARED_END)
  {
    m_shared_ram[address] = value;
    return;
  }
  if (address >= H8_WORK_BASE && address <= H8_WORK_END)
  {
    m_work_ram[address - H8_WORK_BASE] = value;
    // The mailbox and status words are shared with the host at 0x1F796014 and
    // 0x1F797000.
    if (m_register_sink && ((address >= H8_MAILBOX_BASE && address <= H8_MAILBOX_END) ||
                            (address >= H8_STATUS_BASE && address <= H8_STATUS_END)))
    {
      m_register_sink(m_register_opaque, static_cast<u16>((address & 0x1FFF) ^ 1), value);
    }
    return;
  }
  if (address >= H8_PACKET_BASE && address <= H8_PACKET_END)
  {
    m_packet_buffer[address - H8_PACKET_BASE] = value;
    m_packet_mark[address - H8_PACKET_BASE] = (m_rx_regs[1] & RX_CONTROL_MARK) != 0;
    return;
  }

  const auto merge = [address, value](u16& reg) {
    reg = (address & 1) ? static_cast<u16>((reg & 0xFF00) | value) : static_cast<u16>((reg & 0x00FF) | (value << 8));
  };

  if ((address >= H8_RX_BASE && address <= H8_RX_END) || (address >= H8_RX_ACK_BASE && address <= H8_RX_ACK_END))
  {
    // The receive block is mirrored at 0x408000. The firmware writes the mirror
    // first in every receive interrupt; no further effect has been identified.
    const u32 reg = (address & 7) >> 1;
    merge(m_rx_regs[reg]);
    if (reg == 3)
    {
      // Frames are only stored once the firmware has given the receiver a
      // buffer position.
      m_rx_pointer = DecodeOffset(m_rx_regs[3]);
      m_rx_frame_end = m_rx_pointer;
      m_rx_enabled = true;
    }
    return;
  }

  if (address >= H8_TX_BASE && address <= H8_TX_END)
  {
    const u32 reg = (address & 0xF) >> 1;
    merge(m_tx_regs[reg]);
    if (reg == 3)
      m_tx_offset = DecodeOffset(m_tx_regs[3]);
    else if (reg == 2 && !(address & 1))
      Transmit(value);
    return;
  }

  if (address >= H8_MISC_BASE && address <= H8_MISC_END)
  {
    merge(m_misc_regs[(address & 7) >> 1]);
    return;
  }

  if (address >= H8_INTERNAL_BASE)
    WriteInternal(static_cast<u16>(address), value);
}

u8 System12HSCBoard::ReadInternal(u16 reg)
{
  if (m_intc.Handles(reg))
    return m_intc.Read(reg);
  if (m_dmac.Handles(reg))
    return m_dmac.Read(reg);
  if (m_timer16.Handles(reg))
  {
    SyncTimer();
    return m_timer16.Read(reg);
  }

  u8 value = 0;
  if (m_peripherals.Read(reg, value))
    return value;

  return m_internal[reg];
}

void System12HSCBoard::WriteInternal(u16 reg, u8 value)
{
  if (m_intc.Handles(reg))
  {
    m_intc.Write(reg, value);
    return;
  }
  if (m_dmac.Handles(reg))
  {
    m_dma_states += m_dmac.Write(reg, value);
    if (m_dmac.UnsupportedModeSeen() && !m_fault_logged)
    {
      m_fault_logged = true;
      ERROR_LOG("HSC board {} programmed an unsupported DMAC mode at pc={:06X}", m_cabinet,
                static_cast<u32>(m_cpu->cpu.pc));
    }
    return;
  }
  if (m_timer16.Handles(reg))
  {
    SyncTimer();
    m_timer16.Write(reg, value);
    return;
  }
  if (m_peripherals.Write(reg, value))
    return;

  m_internal[reg] = value;
}

void System12HSCBoard::Transmit(u32 length)
{
  // Writing the high byte of the command register sends that many bytes from
  // the transmit offset to the next board in the ring. Bytes are read from the
  // buffer as they go out: the firmware starts a data frame before its DMA has
  // filled the payload.
  if (length == 0)
    return;

  m_tx_jobs.push_back(TxJob{m_tx_offset, length});
}

void System12HSCBoard::AdvanceTransmit(u32 clocks)
{
  if (m_tx_jobs.empty())
  {
    m_tx_clocks = 0;
    return;
  }

  std::array<u16, 64> bytes;
  size_t count = 0;
  m_tx_clocks += clocks;
  while (m_tx_clocks >= LINE_BYTE_CLOCKS && !m_tx_jobs.empty())
  {
    m_tx_clocks -= LINE_BYTE_CLOCKS;
    TxJob& job = m_tx_jobs.front();
    const u16 offset = static_cast<u16>(job.offset & PACKET_OFFSET_MASK);
    bytes[count++] = static_cast<u16>(m_packet_buffer[offset] | (m_packet_mark[offset] ? RING_MARK : 0));
    job.offset++;
    if (--job.remaining == 0)
      m_tx_jobs.pop_front();

    if (count == bytes.size())
    {
      m_sink(m_owner, this, std::span<const u16>(bytes.data(), count));
      count = 0;
    }
  }

  if (count != 0)
    m_sink(m_owner, this, std::span<const u16>(bytes.data(), count));
}

void System12HSCBoard::Receive(std::span<const u16> bytes)
{
  if (m_rx_enabled)
    m_rx_line.insert(m_rx_line.end(), bytes.begin(), bytes.end());
}

void System12HSCBoard::AdvanceReceive(u32 clocks)
{
  if (m_rx_line.empty())
  {
    m_rx_clocks = 0;
    return;
  }

  m_rx_clocks += clocks;
  while (m_rx_clocks >= LINE_BYTE_CLOCKS && !m_rx_line.empty())
  {
    m_rx_clocks -= LINE_BYTE_CLOCKS;
    const u16 b = m_rx_line.front();
    m_rx_line.pop_front();

    m_packet_buffer[m_rx_pointer] = static_cast<u8>(b);
    m_packet_mark[m_rx_pointer] = (b & RING_MARK) != 0;
    m_rx_pointer = static_cast<u16>((m_rx_pointer + 1) & PACKET_OFFSET_MASK);
    if (!(b & RING_MARK))
      continue;

    // A marked byte completes a frame and strobes IRQ0, which the firmware sets
    // to falling-edge detection. Its restart path clears the IRQ0 flag without
    // writing the receive acknowledge, so the request is not a held level.
    m_rx_frame_end = m_rx_pointer;
    m_intc.SetInput(RX_IRQ, true);
    m_intc.SetInput(RX_IRQ, false);
  }
}

System12HSC::System12HSC() = default;

System12HSC::~System12HSC() = default;

void System12HSC::Initialize()
{
  m_board = std::make_unique<System12HSCBoard>(1, m_shared_ram.data(), this, &System12HSC::RouteFrame);
  m_board->SetRegisterSink(&System12HSC::RegisterFromBoard, this);
}

void System12HSC::Reset()
{
  if (m_event)
  {
    if (m_event->IsActive())
      m_event->Deactivate();
    m_event.reset();
  }

  m_shared_ram.fill(0);
  m_host_registers.fill(0);
  m_cycle_fraction = 0;
  SetHostInterrupt(false);
  m_board->SetReset(true);

  const TickCount interval = std::max<TickCount>(1, System::GetTicksPerSecond() / SCHEDULER_HZ);
  m_event = std::make_unique<TimingEvent>("Namco System 12 HSC", interval, interval, SchedulerCallback, this);
  m_event->SetPeriodAndSchedule(interval);
}

void System12HSC::Shutdown()
{
  if (m_event && m_event->IsActive())
    m_event->Deactivate();
  m_event.reset();
}

bool System12HSC::ReadHost(u32 width, u32 offset, u32* value) const
{
  if (FitsRange(offset, width, HOST_SHARED_BASE, HOST_SHARED_END))
  {
    // Host halfwords are little-endian on the PS1 bus.
    const u32 base = offset - HOST_SHARED_BASE;
    u32 result = 0;
    for (u32 i = 0; i < width; i++)
      result |= static_cast<u32>(m_shared_ram[(base + i) ^ 1]) << (i * 8);
    *value = result;
    return true;
  }

  if (HostRegisterRAM(offset, width))
  {
    u32 result = 0;
    for (u32 i = 0; i < width; i++)
      result |= static_cast<u32>(m_host_registers[offset - HOST_RESET_BASE + i]) << (i * 8);
    *value = result;
    return true;
  }

  return false;
}

bool System12HSC::WriteHost(u32 width, u32 offset, u32 value)
{
  if (FitsRange(offset, width, HOST_SHARED_BASE, HOST_SHARED_END))
  {
    const u32 base = offset - HOST_SHARED_BASE;
    for (u32 i = 0; i < width; i++)
      m_shared_ram[(base + i) ^ 1] = static_cast<u8>(value >> (i * 8));
    return true;
  }

  if (FitsRange(offset, width, HOST_RESET_BASE, HOST_RESET_END))
  {
    // Halfword 0 holds the H8 in reset, halfword 1 releases it.
    const u32 end = offset + width - 1;
    if (offset <= HOST_RESET_BASE + 1 && end >= HOST_RESET_BASE)
      m_board->SetReset(true);
    if (offset <= HOST_RESET_BASE + 3 && end >= HOST_RESET_BASE + 2)
      m_board->SetReset(false);
    return true;
  }

  if (HostRegisterRAM(offset, width))
  {
    const u32 reg = offset - HOST_RESET_BASE;
    for (u32 i = 0; i < width; i++)
      m_host_registers[reg + i] = static_cast<u8>(value >> (i * 8));
    if (reg <= REG_MAILBOX_ACK + 1 && reg + width > REG_MAILBOX_ACK)
      SetHostInterrupt(false);
    return true;
  }

  return false;
}

void System12HSC::RegisterFromBoard(void* opaque, u16 reg, u8 value)
{
  static_cast<System12HSC*>(opaque)->BoardRegisterWrite(reg, value);
}

void System12HSC::BoardRegisterWrite(u16 reg, u8 value)
{
  if (reg >= m_host_registers.size())
    return;

  m_host_registers[reg] = value;
  if (reg == REG_MAILBOX || reg == REG_MAILBOX + 1)
    SetHostInterrupt(true);
  else if (reg == REG_MAILBOX_ACK || reg == REG_MAILBOX_ACK + 1)
    SetHostInterrupt(false);
}

void System12HSC::SetHostInterrupt(bool asserted)
{
  if (m_host_irq == asserted)
    return;

  m_host_irq = asserted;
  InterruptController::SetLineState(InterruptController::IRQ::IRQ10, asserted);
}

void System12HSC::SchedulerCallback(void* param, TickCount ticks, TickCount ticks_late)
{
  static_cast<System12HSC*>(param)->Run(ticks);
}

void System12HSC::Run(TickCount ticks)
{
  const u64 ticks_per_second = static_cast<u64>(std::max<TickCount>(System::GetTicksPerSecond(), 1));
  const u64 max_ticks = (ticks_per_second / SCHEDULER_HZ) * 8;
  const u64 numerator =
    static_cast<u64>(CLOCK_HZ) * std::min<u64>(static_cast<u64>(ticks), max_ticks) + m_cycle_fraction;
  m_cycle_fraction = numerator % ticks_per_second;
  m_board->Run(static_cast<u32>(numerator / ticks_per_second));
}

void System12HSC::RouteFrame(void* opaque, const System12HSCBoard* from, std::span<const u16> bytes)
{
  // No other board is cabled to this one.
}
} // namespace Arcade::Namco
