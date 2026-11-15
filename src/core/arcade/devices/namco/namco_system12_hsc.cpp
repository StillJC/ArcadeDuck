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
#include <type_traits>

Log_SetChannel(NamcoSystem12HSC);

namespace Arcade::Namco
{
namespace
{
constexpr u32 CLOCK_HZ = 14'745'600;
constexpr u32 SCHEDULER_HZ = 1000;

// Boards linked on one cabinet run interleaved in slices this long, which
// bounds how far one board's clock can lead another's.
constexpr u32 SLICE_CLOCKS = 256;

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

// Dual-port RAM mirroring between linked cabinets.
//   message: u8 kind, u8 generation, records
//   record:  u8 op, u16 offset, u16 length, data (little-endian fields)
constexpr u8 MSG_HOST = 1;  // cabinet -> cabinet 1: host writes and reset control
constexpr u8 MSG_BOARD = 2; // cabinet 1 -> cabinet: writes made by that cabinet's board
constexpr u8 OP_WRITE = 1;
constexpr u8 OP_RESET = 2;
constexpr u8 OP_REGISTER = 3;
constexpr u8 OP_STATE = 4;  // offset = chunk index
constexpr u8 OP_RESUME = 5; // board continues from the received state
constexpr size_t STATE_CHUNK = 0x2000;
constexpr size_t MAX_TRANSFER_STATE_SIZE = 0x100000;

// Registers shared by the H8 and the host, as offsets from the host reset
// register. The board raises PS1 IRQ10 by writing the mailbox; the host
// acknowledges through the following halfword.
constexpr u16 REG_MAILBOX = 0x0014;
constexpr u16 REG_MAILBOX_ACK = 0x0016;
constexpr size_t RECORD_HEADER_SIZE = 5;
constexpr size_t SNAPSHOT_CHUNK = 0x1000;

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

// Appends a one-byte write record, extending the previous record when contiguous.
void AppendWrite(std::vector<u8>& records, size_t& last_record, u16 address, u8 value)
{
  if (last_record < records.size())
  {
    u8* rec = records.data() + last_record;
    const u16 start = static_cast<u16>(rec[1] | (rec[2] << 8));
    const u16 length = static_cast<u16>(rec[3] | (rec[4] << 8));
    if (rec[0] == OP_WRITE && static_cast<u16>(start + length) == address && length < 0x1000)
    {
      records.push_back(value);
      rec = records.data() + last_record;
      rec[3] = static_cast<u8>(length + 1);
      rec[4] = static_cast<u8>((length + 1) >> 8);
      return;
    }
  }

  last_record = records.size();
  records.insert(records.end(), {OP_WRITE, static_cast<u8>(address), static_cast<u8>(address >> 8), 1, 0, value});
}

void AppendRecord(std::vector<u8>& records, size_t& last_record, u8 op, u16 offset, std::span<const u8> data)
{
  last_record = records.size();
  records.insert(records.end(), {op, static_cast<u8>(offset), static_cast<u8>(offset >> 8),
                                 static_cast<u8>(data.size()), static_cast<u8>(data.size() >> 8)});
  records.insert(records.end(), data.begin(), data.end());
}

// Sends records as one or more messages, splitting only between records.
void SendRecords(Link::Transport& link, u8 cabinet, u8 kind, u8 generation, std::vector<u8>& records)
{
  size_t pos = 0;
  std::vector<u8> message;
  while (pos < records.size())
  {
    message.assign({kind, generation});
    while (pos < records.size())
    {
      if (records.size() - pos < RECORD_HEADER_SIZE)
      {
        WARNING_LOG("HSC truncated outgoing record");
        records.clear();
        return;
      }
      const size_t length = records[pos + 3] | (records[pos + 4] << 8);
      if (length > records.size() - pos - RECORD_HEADER_SIZE ||
          RECORD_HEADER_SIZE + length > Link::Transport::MAX_MESSAGE_SIZE - 2)
      {
        WARNING_LOG("HSC invalid outgoing record length={}", length);
        records.clear();
        return;
      }
      const size_t size = RECORD_HEADER_SIZE + length;
      if (message.size() + size > Link::Transport::MAX_MESSAGE_SIZE)
        break;
      message.insert(message.end(), records.begin() + pos, records.begin() + pos + size);
      pos += size;
    }
    link.Send(cabinet, message);
  }
  records.clear();
}

template<typename Fn>
void ForEachRecord(std::span<const u8> records, Fn&& fn)
{
  size_t pos = 0;
  while (records.size() - pos >= RECORD_HEADER_SIZE)
  {
    const u8 op = records[pos];
    const u16 offset = static_cast<u16>(records[pos + 1] | (records[pos + 2] << 8));
    const size_t length = records[pos + 3] | (records[pos + 4] << 8);
    if (records.size() - pos - RECORD_HEADER_SIZE < length)
      return;
    fn(op, offset, records.subspan(pos + RECORD_HEADER_SIZE, length));
    pos += RECORD_HEADER_SIZE + length;
  }
}
} // namespace

// One HSC board: H8/3002 link CPU and the C446/C422 ring controller.
class System12HSCBoard
{
public:
  // Ring bytes: data in bits 0-7, end-of-frame mark in bit 8.
  using FrameSink = void (*)(void* opaque, const System12HSCBoard* from, std::span<const u16> bytes);
  using WriteObserver = void (*)(void* opaque, u16 address, u8 value);
  // reg is the host register offset (little-endian byte order).
  using RegisterSink = void (*)(void* opaque, u16 reg, u8 value);

  System12HSCBoard(u8 cabinet, u8* shared_ram, void* owner, FrameSink sink);

  // Calls observer for every write the board makes to the dual-port RAM.
  void SetWriteObserver(WriteObserver observer, void* opaque)
  {
    m_observer = observer;
    m_observer_opaque = opaque;
  }

  void SetRegisterSink(RegisterSink sink, void* opaque)
  {
    m_register_sink = sink;
    m_register_opaque = opaque;
  }

  void SetReset(bool asserted);
  bool IsRunning() const { return !m_reset; }
  void Run(u32 clocks);
  void Receive(std::span<const u16> bytes);

  // Running state of the board other than its dual-port RAM, for moving it to
  // another cabinet. Only states written by the same build are accepted.
  void SaveState(std::vector<u8>* out) const;
  bool LoadState(std::span<const u8> data);

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
  WriteObserver m_observer = nullptr;
  void* m_observer_opaque = nullptr;
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

namespace
{
constexpr u32 STATE_MAGIC = 0x31435348; // "HSC1"

template<typename T>
void PutState(std::vector<u8>* out, const T& value)
{
  static_assert(std::is_trivially_copyable_v<T>);
  const u8* const bytes = reinterpret_cast<const u8*>(&value);
  out->insert(out->end(), bytes, bytes + sizeof(T));
}

template<typename T>
bool GetState(std::span<const u8>* in, T* value)
{
  static_assert(std::is_trivially_copyable_v<T>);
  if (in->size() < sizeof(T))
    return false;
  std::memcpy(value, in->data(), sizeof(T));
  *in = in->subspan(sizeof(T));
  return true;
}

u32 StateLayout()
{
  return static_cast<u32>(sizeof(h8_cpu_t) ^ (sizeof(H8::H83002Intc) << 8) ^ (sizeof(H8::H83002Timer16) << 12) ^
                          (sizeof(H8::H83002Peripherals) << 20));
}
} // namespace

void System12HSCBoard::SaveState(std::vector<u8>* out) const
{
  out->clear();
  PutState(out, STATE_MAGIC);
  PutState(out, StateLayout());
  PutState(out, m_cpu->cpu);
  PutState(out, m_cpu->sleep);
  PutState(out, m_intc);
  PutState(out, m_timer16);
  PutState(out, m_peripherals);
  PutState(out, m_dmac.GetState());
  PutState(out, m_work_ram);
  PutState(out, m_packet_buffer);
  PutState(out, m_packet_mark);
  PutState(out, m_internal);
  PutState(out, m_cycle_balance);
  PutState(out, m_timer_clocks);
  PutState(out, m_rx_regs);
  PutState(out, m_tx_regs);
  PutState(out, m_misc_regs);
  PutState(out, m_rx_pointer);
  PutState(out, m_rx_frame_end);
  PutState(out, m_tx_offset);
  PutState(out, m_rx_enabled);
  PutState(out, m_rx_clocks);
  PutState(out, m_tx_clocks);
  PutState(out, static_cast<u32>(m_rx_line.size()));
  for (const u16 b : m_rx_line)
    PutState(out, b);
  PutState(out, static_cast<u32>(m_tx_jobs.size()));
  for (const TxJob& job : m_tx_jobs)
    PutState(out, job);
}

bool System12HSCBoard::LoadState(std::span<const u8> data)
{
  u32 magic = 0, layout = 0;
  if (!GetState(&data, &magic) || !GetState(&data, &layout) || magic != STATE_MAGIC || layout != StateLayout())
    return false;

  // Start from a freshly initialised core so its host pointers are valid here.
  ReleaseReset();

  H8::H83002DMAC::State dmac;
  u32 rx_count = 0;
  bool ok = GetState(&data, &m_cpu->cpu) && GetState(&data, &m_cpu->sleep) && GetState(&data, &m_intc) &&
            GetState(&data, &m_timer16) && GetState(&data, &m_peripherals) && GetState(&data, &dmac) &&
            GetState(&data, &m_work_ram) && GetState(&data, &m_packet_buffer) && GetState(&data, &m_packet_mark) &&
            GetState(&data, &m_internal) && GetState(&data, &m_cycle_balance) && GetState(&data, &m_timer_clocks) &&
            GetState(&data, &m_rx_regs) && GetState(&data, &m_tx_regs) && GetState(&data, &m_misc_regs) &&
            GetState(&data, &m_rx_pointer) && GetState(&data, &m_rx_frame_end) && GetState(&data, &m_tx_offset) &&
            GetState(&data, &m_rx_enabled) && GetState(&data, &m_rx_clocks) && GetState(&data, &m_tx_clocks) &&
            GetState(&data, &rx_count);
  for (u32 i = 0; ok && i < rx_count; i++)
  {
    u16 b = 0;
    ok = GetState(&data, &b);
    m_rx_line.push_back(b);
  }
  u32 tx_count = 0;
  ok = ok && GetState(&data, &tx_count);
  for (u32 i = 0; ok && i < tx_count; i++)
  {
    TxJob job{};
    ok = GetState(&data, &job);
    m_tx_jobs.push_back(job);
  }

  if (!ok || !data.empty())
  {
    SetReset(true);
    return false;
  }

  m_dmac.SetState(dmac);
  return true;
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
    if (m_observer)
      m_observer(m_observer_opaque, static_cast<u16>(address), value);
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

// A board run on cabinet 1 for another cabinet, with its own copy of that
// cabinet's dual-port RAM.
struct System12HSC::RemoteBoard
{
  RemoteBoard(u8 cabinet_, System12HSC* owner)
    : cabinet(cabinet_), board(cabinet_, shared_ram.data(), owner, &System12HSC::RouteFrame)
  {
    board.SetWriteObserver(
      [](void* opaque, u16 address, u8 value) {
        RemoteBoard* const self = static_cast<RemoteBoard*>(opaque);
        AppendWrite(self->pending_writes, self->last_record, address, value);
      },
      this);
    board.SetRegisterSink(
      [](void* opaque, u16 reg, u8 value) {
        RemoteBoard* const self = static_cast<RemoteBoard*>(opaque);
        AppendRecord(self->pending_writes, self->last_record, OP_REGISTER, reg, std::span<const u8>(&value, 1));
      },
      this);
  }

  u8 cabinet;
  u8 generation = 0;
  std::array<u8, SHARED_RAM_SIZE> shared_ram{};
  System12HSCBoard board;
  std::vector<u8> pending_writes;
  size_t last_record = SIZE_MAX;
  std::vector<u8> incoming_state;
  u8 incoming_state_generation = 0;
};

System12HSC::System12HSC() = default;

System12HSC::~System12HSC() = default;

void System12HSC::Initialize(std::string_view session)
{
  m_link = Link::CreateTransport(session);
  const u8 cabinet = m_link ? m_link->GetCabinetID() : 1;
  m_board = std::make_unique<System12HSCBoard>(cabinet, m_shared_ram.data(), this, &System12HSC::RouteFrame);
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
  SetBoardReset(true);
  m_pending_host_writes.clear();
  m_last_host_record = SIZE_MAX;

  const TickCount interval = std::max<TickCount>(1, System::GetTicksPerSecond() / SCHEDULER_HZ);
  m_event = std::make_unique<TimingEvent>("Namco System 12 HSC", interval, interval, SchedulerCallback, this);
  m_event->SetPeriodAndSchedule(interval);
}

void System12HSC::Shutdown()
{
  if (m_event && m_event->IsActive())
    m_event->Deactivate();
  m_event.reset();
  m_remote_boards.clear();
  m_link.reset();
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

    if (m_mirroring)
    {
      const u32 first = (width == 1) ? (base ^ 1) : base;
      for (u32 address = first; address < first + width; address++)
        AppendWrite(m_pending_host_writes, m_last_host_record, static_cast<u16>(address), m_shared_ram[address]);
    }
    return true;
  }

  if (FitsRange(offset, width, HOST_RESET_BASE, HOST_RESET_END))
  {
    // Halfword 0 holds the H8 in reset, halfword 1 releases it.
    const u32 end = offset + width - 1;
    if (offset <= HOST_RESET_BASE + 1 && end >= HOST_RESET_BASE)
      SetBoardReset(true);
    if (offset <= HOST_RESET_BASE + 3 && end >= HOST_RESET_BASE + 2)
      SetBoardReset(false);
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

void System12HSC::SetBoardReset(bool asserted)
{
  if (asserted)
  {
    if (m_board)
      m_board->SetReset(true);
    if (m_mirroring)
    {
      const u8 data = 1;
      AppendRecord(m_pending_host_writes, m_last_host_record, OP_RESET, 0, std::span<const u8>(&data, 1));
      FlushHostWrites();
    }
    m_board_reset = true;
    return;
  }

  if (!m_board_reset)
    return;
  m_board_reset = false;

  // A cabinet linked to cabinet 1 has its board run there; otherwise it runs
  // here with nothing on the ring.
  m_mirroring = false;
  m_pending_host_writes.clear();
  m_last_host_record = SIZE_MAX;
  if (!m_link || m_link->GetCabinetID() == 1 || (m_link->GetConnectedMask() & 1) == 0)
  {
    if (m_board)
      m_board->SetReset(false);
    return;
  }

  StartMirroring();
  const u8 data = 0;
  AppendRecord(m_pending_host_writes, m_last_host_record, OP_RESET, 0, std::span<const u8>(&data, 1));
  FlushHostWrites();
  INFO_LOG("HSC board for cabinet {} runs on cabinet 1", m_link->GetCabinetID());
}

void System12HSC::StartMirroring()
{
  m_mirroring = true;
  m_generation++;
  m_pending_host_writes.clear();
  m_last_host_record = SIZE_MAX;
  for (size_t pos = 0; pos < SHARED_RAM_SIZE; pos += SNAPSHOT_CHUNK)
  {
    AppendRecord(m_pending_host_writes, m_last_host_record, OP_WRITE, static_cast<u16>(pos),
                 std::span<const u8>(m_shared_ram.data() + pos, SNAPSHOT_CHUNK));
  }
}

void System12HSC::HandOverBoard()
{
  // Cabinet 1 became reachable while this board ran unconnected. The board
  // carries on there from its current state, as if the cable had been plugged
  // in.
  std::vector<u8> state;
  m_board->SaveState(&state);
  m_board->SetReset(true);

  StartMirroring();
  for (size_t pos = 0, chunk = 0; pos < state.size(); pos += STATE_CHUNK, chunk++)
  {
    AppendRecord(m_pending_host_writes, m_last_host_record, OP_STATE, static_cast<u16>(chunk),
                 std::span<const u8>(state).subspan(pos, std::min(STATE_CHUNK, state.size() - pos)));
  }
  AppendRecord(m_pending_host_writes, m_last_host_record, OP_RESUME, 0, {});
  FlushHostWrites();
  INFO_LOG("HSC board for cabinet {} moved to cabinet 1", m_link->GetCabinetID());
}

void System12HSC::FlushHostWrites()
{
  if (m_pending_host_writes.empty())
    return;
  if (m_mirroring && m_link)
    SendRecords(*m_link, 1, MSG_HOST, m_generation, m_pending_host_writes);
  m_pending_host_writes.clear();
  m_last_host_record = SIZE_MAX;
}

void System12HSC::SchedulerCallback(void* param, TickCount ticks, TickCount ticks_late)
{
  static_cast<System12HSC*>(param)->Run(ticks);
}

void System12HSC::Run(TickCount ticks)
{
  if (m_link)
  {
    if (m_link->GetCabinetID() == 1)
      UpdateRemoteBoards();
    else if (!m_mirroring && m_board->IsRunning() && (m_link->GetConnectedMask() & 1) != 0)
      HandOverBoard();
    ReceiveMessages();
  }

  const u64 ticks_per_second = static_cast<u64>(std::max<TickCount>(System::GetTicksPerSecond(), 1));
  const u64 max_ticks = (ticks_per_second / SCHEDULER_HZ) * 8;
  const u64 numerator =
    static_cast<u64>(CLOCK_HZ) * std::min<u64>(static_cast<u64>(ticks), max_ticks) + m_cycle_fraction;
  u64 clocks = numerator / ticks_per_second;
  m_cycle_fraction = numerator % ticks_per_second;

  while (clocks > 0)
  {
    const u32 slice = static_cast<u32>(std::min<u64>(clocks, SLICE_CLOCKS));
    if (m_board)
      m_board->Run(slice);
    for (const std::unique_ptr<RemoteBoard>& remote : m_remote_boards)
      remote->board.Run(slice);
    clocks -= slice;
  }

  if (m_link)
  {
    FlushHostWrites();
    for (const std::unique_ptr<RemoteBoard>& remote : m_remote_boards)
    {
      if (!remote->pending_writes.empty())
      {
        SendRecords(*m_link, remote->cabinet, MSG_BOARD, remote->generation, remote->pending_writes);
        remote->last_record = SIZE_MAX;
      }
    }
  }
}

void System12HSC::RouteFrame(void* opaque, const System12HSCBoard* from, std::span<const u16> bytes)
{
  // Boards are cabled in cabinet order; the last one's TX returns to the first.
  System12HSC* const self = static_cast<System12HSC*>(opaque);
  if (self->m_remote_boards.empty())
    return;

  if (from == self->m_board.get())
  {
    self->m_remote_boards.front()->board.Receive(bytes);
    return;
  }

  for (size_t i = 0; i < self->m_remote_boards.size(); i++)
  {
    if (&self->m_remote_boards[i]->board != from)
      continue;
    if (i + 1 < self->m_remote_boards.size())
      self->m_remote_boards[i + 1]->board.Receive(bytes);
    else if (self->m_board)
      self->m_board->Receive(bytes);
    return;
  }
}

void System12HSC::ReceiveMessages()
{
  u8 cabinet = 0;
  std::vector<u8> message;
  while (m_link->Receive(&cabinet, &message))
  {
    if (message.size() < 2)
      continue;

    if (message[0] == MSG_HOST && m_link->GetCabinetID() == 1)
    {
      if (RemoteBoard* remote = FindRemoteBoard(cabinet))
        HandleCabinetMessage(*remote, message);
    }
    else if (message[0] == MSG_BOARD && cabinet == 1)
    {
      HandleBoardMessage(message);
    }
  }
}

void System12HSC::HandleCabinetMessage(RemoteBoard& remote, std::span<const u8> message)
{
  const u8 generation = message[1];
  ForEachRecord(message.subspan(2), [&remote, generation](u8 op, u16 offset, std::span<const u8> data) {
    if (op == OP_WRITE)
    {
      const size_t length = std::min<size_t>(data.size(), SHARED_RAM_SIZE - offset);
      std::memcpy(remote.shared_ram.data() + offset, data.data(), length);
    }
    else if (op == OP_RESET && !data.empty())
    {
      if (data[0] == 0)
      {
        remote.generation = generation;
        remote.pending_writes.clear();
        remote.last_record = SIZE_MAX;
      }
      remote.board.SetReset(data[0] != 0);
    }
    else if (op == OP_STATE)
    {
      if (offset == 0)
      {
        remote.incoming_state.clear();
        remote.incoming_state_generation = generation;
      }
      if (remote.incoming_state_generation != generation || data.empty() || data.size() > STATE_CHUNK ||
          remote.incoming_state.size() != static_cast<size_t>(offset) * STATE_CHUNK ||
          remote.incoming_state.size() > MAX_TRANSFER_STATE_SIZE ||
          data.size() > MAX_TRANSFER_STATE_SIZE - remote.incoming_state.size())
      {
        remote.incoming_state.clear();
        return;
      }
      remote.incoming_state.insert(remote.incoming_state.end(), data.begin(), data.end());
    }
    else if (op == OP_RESUME)
    {
      remote.generation = generation;
      remote.pending_writes.clear();
      remote.last_record = SIZE_MAX;
      if (!remote.incoming_state.empty() && remote.incoming_state_generation == generation &&
          remote.board.LoadState(remote.incoming_state))
        INFO_LOG("HSC board for cabinet {} resumed on cabinet 1", remote.cabinet);
      else
        WARNING_LOG("HSC board state from cabinet {} was not accepted", remote.cabinet);
      remote.incoming_state.clear();
    }
  });
}

void System12HSC::HandleBoardMessage(std::span<const u8> message)
{
  if (!m_mirroring || message[1] != m_generation)
    return;

  ForEachRecord(message.subspan(2), [this](u8 op, u16 offset, std::span<const u8> data) {
    if (op == OP_WRITE)
    {
      const size_t length = std::min<size_t>(data.size(), SHARED_RAM_SIZE - offset);
      std::memcpy(m_shared_ram.data() + offset, data.data(), length);
    }
    else if (op == OP_REGISTER && !data.empty())
    {
      BoardRegisterWrite(offset, data[0]);
    }
  });
}

void System12HSC::UpdateRemoteBoards()
{
  const u32 mask = m_link->GetConnectedMask();

  std::erase_if(m_remote_boards, [mask](const std::unique_ptr<RemoteBoard>& remote) {
    if (mask & (1u << (remote->cabinet - 1)))
      return false;
    INFO_LOG("HSC board for cabinet {} removed from the ring", remote->cabinet);
    return true;
  });

  for (u8 cabinet = 2; cabinet <= Link::Transport::MAX_CABINETS; cabinet++)
  {
    if (!(mask & (1u << (cabinet - 1))) || FindRemoteBoard(cabinet))
      continue;

    auto remote = std::make_unique<RemoteBoard>(cabinet, this);
    const auto pos = std::find_if(m_remote_boards.begin(), m_remote_boards.end(),
                                  [cabinet](const std::unique_ptr<RemoteBoard>& r) { return r->cabinet > cabinet; });
    m_remote_boards.insert(pos, std::move(remote));
    INFO_LOG("HSC board for cabinet {} added to the ring", cabinet);
  }
}

System12HSC::RemoteBoard* System12HSC::FindRemoteBoard(u8 cabinet)
{
  for (const std::unique_ptr<RemoteBoard>& remote : m_remote_boards)
  {
    if (remote->cabinet == cabinet)
      return remote.get();
  }
  return nullptr;
}
} // namespace Arcade::Namco
