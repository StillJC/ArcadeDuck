// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/jvs/namco_cyberlead_led.h"

extern "C" {
#include "core/arcade/third_party/h8_300h/system.h"
}

#include "common/log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

Log_SetChannel(NamcoSystem12);

namespace Arcade::JVS {
namespace {

static constexpr u8 SCI_SMR_CA = UINT8_C(0x80);
static constexpr u8 SCI_SMR_CKS = UINT8_C(0x03);
static constexpr u8 SCI_SCR_TIE = UINT8_C(0x80);
static constexpr u8 SCI_SCR_RIE = UINT8_C(0x40);
static constexpr u8 SCI_SCR_TE = UINT8_C(0x20);
static constexpr u8 SCI_SCR_RE = UINT8_C(0x10);
static constexpr u8 SCI_SCR_TEIE = UINT8_C(0x04);
static constexpr u8 SCI_SSR_TDRE = UINT8_C(0x80);
static constexpr u8 SCI_SSR_RDRF = UINT8_C(0x40);
static constexpr u8 SCI_SSR_ORER = UINT8_C(0x20);
static constexpr u8 SCI_SSR_FER = UINT8_C(0x10);
static constexpr u8 SCI_SSR_PER = UINT8_C(0x08);
static constexpr u8 SCI_SSR_TEND = UINT8_C(0x04);
static constexpr u8 SCI_SSR_MPB = UINT8_C(0x02);
static constexpr u8 SCI_SSR_MPBT = UINT8_C(0x01);

static constexpr u8 TIMER_TCR_CKS = UINT8_C(0x07);
static constexpr u8 TIMER_TCR_CCLR = UINT8_C(0x18);
static constexpr u8 TIMER_TCR_OVIE = UINT8_C(0x20);
static constexpr u8 TIMER_TCR_CMIEA = UINT8_C(0x40);
static constexpr u8 TIMER_TCR_CMIEB = UINT8_C(0x80);
static constexpr u8 TIMER_TCSR_OVF = UINT8_C(0x20);
static constexpr u8 TIMER_TCSR_CMFA = UINT8_C(0x40);
static constexpr u8 TIMER_TCSR_CMFB = UINT8_C(0x80);

static constexpr u32 IRQ_VECTOR_EXTERNAL_BASE = 4;
static constexpr u32 IRQ_VECTOR_VBLANK = IRQ_VECTOR_EXTERNAL_BASE + 6;
static constexpr u32 IRQ_VECTOR_TIMER_A = 23;
static constexpr u32 IRQ_VECTOR_TIMER_B = 24;
static constexpr u32 IRQ_VECTOR_TIMER_OVERFLOW = 25;
static constexpr u32 IRQ_VECTOR_SCI1_ERROR = 33;
static constexpr u32 IRQ_VECTOR_SCI1_RX = 34;
static constexpr u32 IRQ_VECTOR_SCI1_TX = 35;
static constexpr u32 IRQ_VECTOR_SCI1_TE = 36;

static constexpr u32 LED_VBLANK_HZ = 60;
static constexpr u32 RX_QUEUE_SIZE = 4096;

} // namespace

void NamcoCyberLeadLEDLink::Reset()
{
  m_address = DEFAULT_ADDRESS;
  m_last_frame = {};
}

bool NamcoCyberLeadLEDLink::AppendEscapedByte(Frame& frame, u8 value)
{
  if (value == UINT8_C(0xE0) || value == UINT8_C(0xD0))
  {
    if ((frame.wire_size + 2) > frame.wire.size())
      return false;

    frame.wire[frame.wire_size++] = UINT8_C(0xD0);
    frame.wire[frame.wire_size++] = static_cast<u8>(value - 1);
    return true;
  }

  if (frame.wire_size >= frame.wire.size())
    return false;

  frame.wire[frame.wire_size++] = value;
  return true;
}

bool NamcoCyberLeadLEDLink::QueueCommand(std::span<const u8> command)
{
  if (command.empty() || command.size() > MAX_COMMAND_SIZE)
    return false;

  Frame candidate{};
  candidate.generation = m_last_frame.generation + 1;

  const u8 length = static_cast<u8>(command.size() + 1); // command bytes + checksum
  candidate.logical[candidate.logical_size++] = m_address;
  candidate.logical[candidate.logical_size++] = length;

  u8 checksum = static_cast<u8>(m_address + length);
  for (const u8 value : command)
  {
    candidate.logical[candidate.logical_size++] = value;
    checksum = static_cast<u8>(checksum + value);
  }

  candidate.logical[candidate.logical_size++] = checksum;
  candidate.checksum = checksum;

  candidate.wire[candidate.wire_size++] = UINT8_C(0xE0);
  for (u32 i = 0; i < candidate.logical_size; i++)
  {
    if (!AppendEscapedByte(candidate, candidate.logical[i]))
      return false;
  }

  candidate.valid = true;
  m_last_frame = candidate;
  return true;
}

struct NamcoCyberLeadLEDDevice::Impl
{
  std::array<u8, FIRMWARE_SIZE> firmware{};
  std::array<u8, DISPLAY_RAM_SIZE> display_ram{};
  std::array<u8, DISPLAY_CONTROL_SIZE> display_control{};
  std::array<u8, SCROLL_SIZE> scroll{};
  std::array<u8, 0x4000> work_ram{}; // C000-FFFF shadow, overridden by mapped registers.

  std::array<u8, RX_QUEUE_SIZE> rx_queue{};
  u32 rx_head = 0;
  u32 rx_tail = 0;
  u32 rx_count = 0;

  h8_system_t cpu{};
  Stats stats{};
  FrameState frame_state{};

  bool firmware_loaded = false;
  bool fault_logged = false;
  u32 last_step_pc = 0;
  s64 cycle_balance = 0;
  u64 source_clock_fraction = 0;
  u64 vblank_clock_accumulator = 0;
  u64 next_vblank_start_clock = 0;
  u64 vblank_end_clock = 0;
  bool vblank_active = false;
  bool irq6_latched = false;
  u64 timer_clock_accumulator = 0;
  u32 flash_bank = 0;

  std::array<bool, 64> pending_interrupts{};

  std::array<u8, 9> port_ddr{};
  std::array<u8, 9> port_dr{};
  u8 wscr = 0;
  u8 stcr = 0;
  u8 syscr = UINT8_C(0x09);
  u8 mdcr = 0;
  u8 iscr = 0;
  u8 ier = 0;
  u8 fff1 = 0;

  u8 timer_tcr = 0;
  u8 timer_tcsr = UINT8_C(0x10);
  std::array<u8, 2> timer_tcor{UINT8_C(0xFF), UINT8_C(0xFF)};
  u8 timer_tcnt = 0;
  bool timer_extra_clock = false;

  u8 sci0_smr = 0;
  u8 sci0_brr = UINT8_C(0xFF);
  u8 sci0_scr = 0;
  u8 sci0_tdr = UINT8_C(0xFF);
  u8 sci0_ssr = UINT8_C(0x84);
  u8 sci0_rdr = 0;

  u8 sci1_smr = 0;
  u8 sci1_brr = UINT8_C(0xFF);
  u8 sci1_scr = 0;
  u8 sci1_tdr = UINT8_C(0xFF);
  u8 sci1_ssr = UINT8_C(0x84);
  u8 sci1_ssr_read = 0;
  u8 sci1_rdr = 0;
  bool sci1_rx_busy = false;
  u32 sci1_rx_clocks_remaining = 0;
  bool sci1_tx_busy = false;
  u8 sci1_tx_shift = UINT8_C(0xFF);
  u32 sci1_tx_clocks_remaining = 0;
  bool sci1_tx_pending = false;
  u8 sci1_tx_pending_byte = UINT8_C(0xFF);

  u32 display_diag_count = 0;
  u32 control_diag_count = 0;
  u32 frame_diag_count = 0;
  u32 nonzero_write_diag_count = 0;
  u32 scan_probe_vblank_count = 0;
  u32 scan_probe_write_count = 0;
  u32 rx_diag_count = 0;
  u32 tx_diag_count = 0;

  static h8_bool BusRead(void* opaque, unsigned address, h8_byte_t* value)
  {
    Impl& self = *static_cast<Impl*>(opaque);
    value->u = self.Read(static_cast<u16>(address));
    return TRUE;
  }

  static h8_bool BusWrite(void* opaque, unsigned address, h8_byte_t value)
  {
    Impl& self = *static_cast<Impl*>(opaque);
    self.Write(static_cast<u16>(address), value.u);
    return TRUE;
  }

  void ResetRuntime()
  {
    display_ram.fill(0);
    display_control.fill(0);
    scroll.fill(0);
    work_ram.fill(0);
    rx_head = 0;
    rx_tail = 0;
    rx_count = 0;
    stats = {};
    frame_state = {};
    fault_logged = false;
    last_step_pc = 0;
    cycle_balance = 0;
    source_clock_fraction = 0;
    vblank_clock_accumulator = 0;
    next_vblank_start_clock = CLOCK_HZ / LED_VBLANK_HZ;
    vblank_end_clock = 0;
    vblank_active = false;
    irq6_latched = false;
    timer_clock_accumulator = 0;
    flash_bank = 0;
    pending_interrupts.fill(false);
    port_ddr.fill(0);
    port_dr.fill(0);
    wscr = 0;
    stcr = 0;
    syscr = UINT8_C(0x09);
    mdcr = 0;
    iscr = 0;
    ier = 0;
    fff1 = 0;
    timer_tcr = 0;
    timer_tcsr = UINT8_C(0x10);
    timer_tcor = {UINT8_C(0xFF), UINT8_C(0xFF)};
    timer_tcnt = 0;
    timer_extra_clock = false;
    sci0_smr = 0;
    sci0_brr = UINT8_C(0xFF);
    sci0_scr = 0;
    sci0_tdr = UINT8_C(0xFF);
    sci0_ssr = UINT8_C(0x84);
    sci0_rdr = 0;
    sci1_smr = 0;
    sci1_brr = UINT8_C(0xFF);
    sci1_scr = 0;
    sci1_tdr = UINT8_C(0xFF);
    sci1_ssr = UINT8_C(0x84);
    sci1_ssr_read = 0;
    sci1_rdr = 0;
    sci1_rx_busy = false;
    sci1_rx_clocks_remaining = 0;
    sci1_tx_busy = false;
    sci1_tx_shift = UINT8_C(0xFF);
    sci1_tx_clocks_remaining = 0;
    sci1_tx_pending = false;
    sci1_tx_pending_byte = UINT8_C(0xFF);
    display_diag_count = 0;
    control_diag_count = 0;
    frame_diag_count = 0;
    nonzero_write_diag_count = 0;
    scan_probe_vblank_count = 0;
    scan_probe_write_count = 0;
    rx_diag_count = 0;
    tx_diag_count = 0;

    std::memset(&cpu, 0, sizeof(cpu));
    cpu.normal_mode = TRUE;
    cpu.bus_opaque = this;
    cpu.bus_read = &BusRead;
    cpu.bus_write = &BusWrite;

    if (firmware_loaded)
    {
      h8_init(&cpu);
      INFO_LOG("CyberLead LED C77 reset_pc={:04X} clock_hz={} firmware_size={}",
               static_cast<u32>(cpu.cpu.pc), CLOCK_HZ, firmware.size());
    }
  }

  u8 ReadPort(u32 index) const
  {
    if (index == 5) // Port 6 callback on the physical LED PCB resolves high.
      return UINT8_C(0xFF);
    return static_cast<u8>(port_dr[index] | static_cast<u8>(~port_ddr[index]));
  }

  u32 TimerDivider() const
  {
    switch (timer_tcr & TIMER_TCR_CKS)
    {
      case 1:
        return timer_extra_clock ? 2U : 8U;
      case 2:
        return timer_extra_clock ? 32U : 64U;
      case 3:
        return timer_extra_clock ? 256U : 1024U;
      default:
        return 0;
    }
  }

  u32 SCI1FrameClocks() const
  {
    // MAME H8 SCI: divider=(2 << (2*CKS))*(BRR+1), with 16 internal
    // clock ticks per asynchronous data bit. The LED firmware programs
    // SMR=00/BRR=2F, yielding 9600 bps and 15360 C77 clocks per 8N1 byte.
    if ((sci1_smr & SCI_SMR_CA) != 0)
      return 0;

    const u32 cks = sci1_smr & SCI_SMR_CKS;
    const u64 divider = static_cast<u64>(2U << (2U * cks)) *
                        static_cast<u64>(static_cast<u32>(sci1_brr) + 1U);
    const u64 frame_clocks = divider * 16U * 10U;
    return static_cast<u32>(std::min<u64>(frame_clocks, UINT32_MAX));
  }

  void RaiseInterrupt(u32 vector)
  {
    if (vector < pending_interrupts.size())
      pending_interrupts[vector] = true;
  }

  bool IsVBlankIRQEdgeTriggered() const
  {
    return (iscr & UINT8_C(0x40)) != 0;
  }

  void UpdateVBlankInterruptPending()
  {
    const bool enabled = (ier & UINT8_C(0x40)) != 0;
    const bool asserted = IsVBlankIRQEdgeTriggered() ? irq6_latched : vblank_active;
    pending_interrupts[IRQ_VECTOR_VBLANK] = enabled && asserted;
  }

  void SetVBlankIRQLine(bool asserted)
  {
    if (vblank_active == asserted)
      return;

    const bool was_asserted = vblank_active;
    vblank_active = asserted;

    if (asserted)
    {
      if (!was_asserted)
        irq6_latched = true;
    }
    else if (!IsVBlankIRQEdgeTriggered())
    {
      // Base H8 level-sensitive external IRQs are not latched once the
      // physical active-low input is released.
      irq6_latched = false;
    }

    UpdateVBlankInterruptPending();
  }

  bool ServiceInterrupt()
  {
    if (cpu.cpu.ccr.flags.i != 0)
      return false;

    for (u32 vector = 0; vector < pending_interrupts.size(); vector++)
    {
      if (!pending_interrupts[vector])
        continue;

      pending_interrupts[vector] = false;
      cpu.cycles = 0;
      if (!h8_interrupt(&cpu, vector))
      {
        if (vector == IRQ_VECTOR_VBLANK)
          UpdateVBlankInterruptPending();
        return false;
      }

      if (vector == IRQ_VECTOR_VBLANK)
      {
        if (IsVBlankIRQEdgeTriggered() || !vblank_active)
          irq6_latched = false;
        stats.vblank_interrupts++;
        UpdateVBlankInterruptPending();
      }

      AdvancePeripherals(static_cast<u32>(std::max(cpu.cycles, 0)));
      cycle_balance -= static_cast<s64>(std::max(cpu.cycles, 0));
      return true;
    }

    return false;
  }

  void AdvanceTimer(u32 clocks)
  {
    const u32 divider = TimerDivider();
    if (divider == 0)
    {
      timer_clock_accumulator = 0;
      return;
    }

    timer_clock_accumulator += clocks;
    while (timer_clock_accumulator >= divider)
    {
      timer_clock_accumulator -= divider;
      const u8 previous = timer_tcnt;
      timer_tcnt = static_cast<u8>(timer_tcnt + 1);

      const u8 compare_a = static_cast<u8>(timer_tcor[0] + 1);
      const u8 compare_b = static_cast<u8>(timer_tcor[1] + 1);
      const bool hit_a = (timer_tcnt == compare_a);
      const bool hit_b = (timer_tcnt == compare_b);
      const bool overflow = (previous == UINT8_C(0xFF));

      if (hit_a)
      {
        if ((timer_tcsr & TIMER_TCSR_CMFA) == 0)
        {
          timer_tcsr |= TIMER_TCSR_CMFA;
          if ((timer_tcr & TIMER_TCR_CMIEA) != 0)
          {
            RaiseInterrupt(IRQ_VECTOR_TIMER_A);
            stats.timer_interrupts++;
          }
        }

        if ((timer_tcr & TIMER_TCR_CCLR) == UINT8_C(0x08))
          timer_tcnt = 0;
      }

      if (hit_b)
      {
        if ((timer_tcsr & TIMER_TCSR_CMFB) == 0)
        {
          timer_tcsr |= TIMER_TCSR_CMFB;
          if ((timer_tcr & TIMER_TCR_CMIEB) != 0)
            RaiseInterrupt(IRQ_VECTOR_TIMER_B);
        }

        if ((timer_tcr & TIMER_TCR_CCLR) == UINT8_C(0x10))
          timer_tcnt = 0;
      }

      if (overflow && (timer_tcsr & TIMER_TCSR_OVF) == 0)
      {
        timer_tcsr |= TIMER_TCSR_OVF;
        if ((timer_tcr & TIMER_TCR_OVIE) != 0)
          RaiseInterrupt(IRQ_VECTOR_TIMER_OVERFLOW);
      }
    }
  }

  void TryStartSCI1Receive()
  {
    if (sci1_rx_busy || rx_count == 0 || (sci1_scr & SCI_SCR_RE) == 0)
      return;

    const u32 clocks = SCI1FrameClocks();
    if (clocks == 0)
      return;

    sci1_rx_busy = true;
    sci1_rx_clocks_remaining = clocks;
  }

  void CompleteSCI1Receive()
  {
    sci1_rx_busy = false;
    sci1_rx_clocks_remaining = 0;
    if (rx_count == 0)
      return;

    const u8 value = rx_queue[rx_head];
    rx_head = (rx_head + 1) % rx_queue.size();
    rx_count--;
    stats.received_wire_bytes++;

    if ((sci1_ssr & SCI_SSR_RDRF) != 0)
    {
      sci1_ssr |= SCI_SSR_ORER;
      if ((sci1_scr & SCI_SCR_RIE) != 0)
        RaiseInterrupt(IRQ_VECTOR_SCI1_ERROR);
    }
    else
    {
      sci1_rdr = value;
      sci1_ssr |= SCI_SSR_RDRF;
      if ((sci1_scr & SCI_SCR_RIE) != 0)
      {
        RaiseInterrupt(IRQ_VECTOR_SCI1_RX);
        stats.sci_rx_interrupts++;
      }
    }

    if (rx_diag_count < 32)
    {
      INFO_LOG("CyberLead LED C77 SCI1 RX #{} pc={:04X} data={:02X} ssr={:02X} queued={}",
               stats.received_wire_bytes, static_cast<u32>(cpu.cpu.pc), value, sci1_ssr, rx_count);
      rx_diag_count++;
    }

    TryStartSCI1Receive();
  }

  void StartSCI1Transmit(u8 value)
  {
    sci1_tx_shift = value;
    sci1_tx_busy = true;
    sci1_tx_clocks_remaining = std::max<u32>(SCI1FrameClocks(), 1U);
  }

  void CompleteSCI1Transmit()
  {
    sci1_tx_busy = false;
    sci1_tx_clocks_remaining = 0;
    stats.transmitted_wire_bytes++;

    if (tx_diag_count < 32)
    {
      INFO_LOG("CyberLead LED C77 SCI1 TX #{} pc={:04X} data={:02X}",
               stats.transmitted_wire_bytes, static_cast<u32>(cpu.cpu.pc), sci1_tx_shift);
      tx_diag_count++;
    }

    if (sci1_tx_pending)
    {
      const u8 next = sci1_tx_pending_byte;
      sci1_tx_pending = false;
      sci1_ssr |= SCI_SSR_TDRE;
      if ((sci1_scr & SCI_SCR_TIE) != 0)
        RaiseInterrupt(IRQ_VECTOR_SCI1_TX);
      StartSCI1Transmit(next);
    }
    else
    {
      sci1_ssr |= static_cast<u8>(SCI_SSR_TDRE | SCI_SSR_TEND);
      if ((sci1_scr & SCI_SCR_TEIE) != 0)
        RaiseInterrupt(IRQ_VECTOR_SCI1_TE);
    }
  }

  void AdvanceSCI1(u32 clocks)
  {
    TryStartSCI1Receive();

    u32 remaining = clocks;
    while (remaining != 0 && sci1_rx_busy)
    {
      if (remaining < sci1_rx_clocks_remaining)
      {
        sci1_rx_clocks_remaining -= remaining;
        remaining = 0;
      }
      else
      {
        remaining -= sci1_rx_clocks_remaining;
        CompleteSCI1Receive();
      }
    }

    remaining = clocks;
    while (remaining != 0 && sci1_tx_busy)
    {
      if (remaining < sci1_tx_clocks_remaining)
      {
        sci1_tx_clocks_remaining -= remaining;
        remaining = 0;
      }
      else
      {
        remaining -= sci1_tx_clocks_remaining;
        CompleteSCI1Transmit();
      }
    }
  }

  void UpdateFrame()
  {
    FrameState candidate{};
    candidate.start_address = static_cast<u16>(
      ((static_cast<u16>(display_control[7]) << 8) | display_control[6]) & UINT16_C(0x1FFF));

    // MAME cyberlead.cpp screen_update():
    //   scroll = ~RAM[D0BF] & 3
    //   scrollx = ((start & 0x1f) * 4) + scroll
    //   scrolly = start >> 5
    const u32 scroll = static_cast<u32>(~work_ram[UINT16_C(0xD0BF) - UINT16_C(0xC000)]) & 3U;
    candidate.scroll_x = ((candidate.start_address & UINT16_C(0x001F)) * 4U) + scroll;
    candidate.scroll_y = candidate.start_address >> 5;
    candidate.valid = true;

    static constexpr u32 ROW_BYTES = 16U * sizeof(u16);
    static constexpr u32 ROW_COUNT = DISPLAY_RAM_SIZE / ROW_BYTES;
    static_assert(ROW_COUNT != 0);

    for (u32 source_y = 0; source_y < FRAME_HEIGHT; source_y++)
    {
      // Aplarail's observed controller start values remain within the mapped
      // SED1351F RAM. Retain deterministic wrapping at the physical RAM size
      // rather than permitting an out-of-bounds read.
      const u32 row = (candidate.scroll_y + source_y) % ROW_COUNT;
      const u32 row_base = row * ROW_BYTES;

      for (u32 source_x = 0; source_x < FRAME_WIDTH; source_x++)
      {
        const u32 p = (candidate.scroll_x + source_x) & 127U;
        const u32 word_index = p >> 3;
        const u32 byte_index = row_base + (word_index * 2U);

        // MAME's H8 program space/share is big-endian. Recreate the
        // big-endian 16-bit shared-RAM word from our logical byte-addressed RAM.
        const u16 source_word =
          static_cast<u16>(static_cast<u16>(display_ram[byte_index]) << 8) |
          static_cast<u16>(display_ram[byte_index + 1]);

        const u32 shift = ((~p) & 7U) * 2U;
        const u8 intensity = static_cast<u8>((source_word >> shift) & 3U);

        // MAME marks the LED screen ROT180. Store the public 96x16 frame in
        // physical cabinet orientation so the frontend does not need board-
        // specific orientation knowledge.
        const u32 x = (FRAME_WIDTH - 1U) - source_x;
        const u32 y = (FRAME_HEIGHT - 1U) - source_y;
        candidate.pixels[(y * FRAME_WIDTH) + x] = intensity;
        candidate.intensity_counts[intensity]++;
      }
    }

    stats.frame_refreshes++;

    const bool changed =
      !frame_state.valid ||
      candidate.pixels != frame_state.pixels ||
      candidate.start_address != frame_state.start_address ||
      candidate.scroll_x != frame_state.scroll_x ||
      candidate.scroll_y != frame_state.scroll_y;

    if (changed)
    {
      candidate.generation = frame_state.generation + 1;
      frame_state = candidate;
      stats.frame_changes++;

      const u32 nonzero =
        frame_state.intensity_counts[1] +
        frame_state.intensity_counts[2] +
        frame_state.intensity_counts[3];

      if (frame_diag_count < 16)
      {
        INFO_LOG(
          "CyberLead LED frame #{} start={:04X} scroll_x={} scroll_y={} nonzero={} "
          "levels=[{},{},{},{}] display_writes={} control_writes={}",
          frame_state.generation, frame_state.start_address, frame_state.scroll_x,
          frame_state.scroll_y, nonzero, frame_state.intensity_counts[0],
          frame_state.intensity_counts[1], frame_state.intensity_counts[2],
          frame_state.intensity_counts[3], stats.display_ram_writes,
          stats.display_control_writes);
        frame_diag_count++;
      }
    }
  }

  void AdvanceVBlank(u32 clocks)
  {
    const u64 frame_clocks = CLOCK_HZ / LED_VBLANK_HZ;
    const u64 vblank_clocks =
      (static_cast<u64>(CLOCK_HZ) * UINT64_C(2500)) / UINT64_C(1000000);
    const u64 target_clock = vblank_clock_accumulator + clocks;

    while (true)
    {
      const bool end_is_next =
        vblank_active && vblank_end_clock != 0 && vblank_end_clock < next_vblank_start_clock;
      const u64 next_event_clock = end_is_next ? vblank_end_clock : next_vblank_start_clock;
      if (next_event_clock > target_clock)
        break;

      vblank_clock_accumulator = next_event_clock;

      if (end_is_next)
      {
        SetVBlankIRQLine(false);
        vblank_end_clock = 0;
        continue;
      }

      if (scan_probe_vblank_count < 120)
      {
        const u16 start = static_cast<u16>(
          ((static_cast<u16>(display_control[7]) << 8) | display_control[6]) & UINT16_C(0x1FFF));
        const u32 fine_scroll =
          static_cast<u32>(~work_ram[UINT16_C(0xD0BF) - UINT16_C(0xC000)]) & 3U;
        INFO_LOG(
          "CyberLead LED SCAN-PROBE vblank #{} pc={:04X} start={:04X} ctrl6={:02X} ctrl7={:02X} "
          "fine={} scroll_x={} scroll_y={} timer={:02X} cpu_steps={}",
          scan_probe_vblank_count + 1, static_cast<u32>(cpu.cpu.pc), start,
          display_control[6], display_control[7], fine_scroll,
          ((start & UINT16_C(0x001F)) * 4U) + fine_scroll, start >> 5,
          timer_tcnt, stats.cpu_steps);
        scan_probe_vblank_count++;
      }

      // MAME updates the screen immediately before asserting the vblank
      // callback. Preserve that ordering, then hold C77 IRQ6 active-low for
      // the configured 2.5 ms vblank interval.
      UpdateFrame();
      SetVBlankIRQLine(true);
      vblank_end_clock = next_event_clock + vblank_clocks;
      next_vblank_start_clock = next_event_clock + frame_clocks;
    }

    vblank_clock_accumulator = target_clock;
  }

  void AdvancePeripherals(u32 clocks)
  {
    if (clocks == 0)
      return;
    AdvanceTimer(clocks);
    AdvanceSCI1(clocks);
    AdvanceVBlank(clocks);
  }

  u8 Read(u16 address)
  {
    if (address <= UINT16_C(0x3FFF))
      return firmware[address];
    if (address >= UINT16_C(0x4000) && address <= UINT16_C(0x5FFF))
      return display_ram[address - UINT16_C(0x4000)];
    if (address >= UINT16_C(0x6000) && address <= UINT16_C(0x600F))
      return display_control[address - UINT16_C(0x6000)];
    if (address >= UINT16_C(0x7000) && address <= UINT16_C(0x7001))
      return scroll[address - UINT16_C(0x7000)];
    if (address >= UINT16_C(0x8000) && address <= UINT16_C(0xBFFF))
    {
      const u32 offset = flash_bank + (address - UINT16_C(0x8000));
      return (offset < firmware.size()) ? firmware[offset] : UINT8_C(0xFF);
    }

    switch (address)
    {
      case UINT16_C(0xFFB0): return port_ddr[0];
      case UINT16_C(0xFFB1): return port_ddr[1];
      case UINT16_C(0xFFB2): return ReadPort(0);
      case UINT16_C(0xFFB3): return ReadPort(1);
      case UINT16_C(0xFFB4): return port_ddr[2];
      case UINT16_C(0xFFB5): return port_ddr[3];
      case UINT16_C(0xFFB6): return ReadPort(2);
      case UINT16_C(0xFFB7): return ReadPort(3);
      case UINT16_C(0xFFB8): return port_ddr[4];
      case UINT16_C(0xFFB9): return port_ddr[5];
      case UINT16_C(0xFFBA): return ReadPort(4);
      case UINT16_C(0xFFBB): return ReadPort(5);
      case UINT16_C(0xFFBC): return port_ddr[6];
      case UINT16_C(0xFFBD): return port_ddr[7];
      case UINT16_C(0xFFBE): return ReadPort(6);
      case UINT16_C(0xFFBF): return ReadPort(7);
      case UINT16_C(0xFFC0): return port_ddr[8];
      case UINT16_C(0xFFC1): return ReadPort(8);
      case UINT16_C(0xFFC2): return UINT8_C(0x00);
      case UINT16_C(0xFFC3): return UINT8_C(0x00);
      case UINT16_C(0xFFC4): return syscr;
      case UINT16_C(0xFFC5): return UINT8_C(0x00);
      case UINT16_C(0xFFC6): return iscr;
      case UINT16_C(0xFFC7): return ier;
      case UINT16_C(0xFFC8): return timer_tcr;
      case UINT16_C(0xFFC9): return timer_tcsr;
      case UINT16_C(0xFFCA): return timer_tcor[0];
      case UINT16_C(0xFFCB): return timer_tcor[1];
      case UINT16_C(0xFFCC): return timer_tcnt;

      case UINT16_C(0xFFD8): return sci0_smr;
      case UINT16_C(0xFFD9): return sci0_brr;
      case UINT16_C(0xFFDA): return sci0_scr;
      case UINT16_C(0xFFDB): return sci0_tdr;
      case UINT16_C(0xFFDC): return sci0_ssr;
      case UINT16_C(0xFFDD): return sci0_rdr;

      case UINT16_C(0xFFE0): return sci1_smr;
      case UINT16_C(0xFFE1): return sci1_brr;
      case UINT16_C(0xFFE2): return sci1_scr;
      case UINT16_C(0xFFE3): return sci1_tdr;
      case UINT16_C(0xFFE4):
        sci1_ssr_read = sci1_ssr;
        return sci1_ssr;
      case UINT16_C(0xFFE5): return sci1_rdr;
      case UINT16_C(0xFFF1): return fff1;
      default: break;
    }

    if (address >= UINT16_C(0xC000) && address <= UINT16_C(0xFF7F))
      return work_ram[address - UINT16_C(0xC000)];

    return UINT8_C(0xFF);
  }

  void WriteSCI1SSR(u8 data)
  {
    const bool request_tx =
      (sci1_scr & SCI_SCR_TE) != 0 &&
      (sci1_ssr & sci1_ssr_read & SCI_SSR_TDRE) != 0 &&
      (data & SCI_SSR_TDRE) == 0;

    if (request_tx)
      sci1_ssr &= static_cast<u8>(~(SCI_SSR_TDRE | SCI_SSR_TEND));

    sci1_ssr = static_cast<u8>(
      (sci1_ssr & static_cast<u8>(~sci1_ssr_read | data | SCI_SSR_TDRE | SCI_SSR_TEND | SCI_SSR_MPB) &
       static_cast<u8>(~SCI_SSR_MPBT)) |
      (data & SCI_SSR_MPBT));
    sci1_ssr_read &= sci1_ssr;

    if (request_tx)
    {
      if (!sci1_tx_busy)
      {
        sci1_ssr |= SCI_SSR_TDRE;
        if ((sci1_scr & SCI_SCR_TIE) != 0)
          RaiseInterrupt(IRQ_VECTOR_SCI1_TX);
        StartSCI1Transmit(sci1_tdr);
      }
      else
      {
        sci1_tx_pending = true;
        sci1_tx_pending_byte = sci1_tdr;
      }
    }
  }

  void Write(u16 address, u8 value)
  {
    if (address <= UINT16_C(0x3FFF))
    {
      // 29C020 programming is not required for the first firmware-runner
      // milestone. Preserve the supplied ROM image and ignore writes.
      return;
    }
    if (address >= UINT16_C(0x4000) && address <= UINT16_C(0x5FFF))
    {
      display_ram[address - UINT16_C(0x4000)] = value;
      stats.display_ram_writes++;
      if (display_diag_count < 16)
      {
        INFO_LOG("CyberLead LED display RAM write #{} pc={:04X} addr={:04X} data={:02X}",
                 stats.display_ram_writes, static_cast<u32>(cpu.cpu.pc), address, value);
        display_diag_count++;
      }
      if (value != 0 && nonzero_write_diag_count < 16)
      {
        INFO_LOG("CyberLead LED nonzero RAM write #{} pc={:04X} addr={:04X} data={:02X} total_writes={}",
                 nonzero_write_diag_count + 1, static_cast<u32>(cpu.cpu.pc), address, value,
                 stats.display_ram_writes);
        nonzero_write_diag_count++;
      }
      return;
    }
    if (address >= UINT16_C(0x6000) && address <= UINT16_C(0x600F))
    {
      display_control[address - UINT16_C(0x6000)] = value;
      stats.display_control_writes++;

      if ((address == UINT16_C(0x6006) || address == UINT16_C(0x6007)) &&
          scan_probe_write_count < 120)
      {
        const u16 start = static_cast<u16>(
          ((static_cast<u16>(display_control[7]) << 8) | display_control[6]) & UINT16_C(0x1FFF));
        const u32 fine_scroll =
          static_cast<u32>(~work_ram[UINT16_C(0xD0BF) - UINT16_C(0xC000)]) & 3U;
        INFO_LOG(
          "CyberLead LED SCAN-PROBE start-write #{} pc={:04X} addr={:04X} data={:02X} "
          "ctrl6={:02X} ctrl7={:02X} start={:04X} fine={} scroll_x={} scroll_y={} cpu_steps={}",
          scan_probe_write_count + 1, static_cast<u32>(cpu.cpu.pc), address, value,
          display_control[6], display_control[7], start, fine_scroll,
          ((start & UINT16_C(0x001F)) * 4U) + fine_scroll, start >> 5, stats.cpu_steps);
        scan_probe_write_count++;
      }

      if (control_diag_count < 16)
      {
        INFO_LOG("CyberLead LED control write #{} pc={:04X} addr={:04X} data={:02X}",
                 stats.display_control_writes, static_cast<u32>(cpu.cpu.pc), address, value);
        control_diag_count++;
      }
      return;
    }
    if (address >= UINT16_C(0x7000) && address <= UINT16_C(0x7001))
    {
      scroll[address - UINT16_C(0x7000)] = value;
      stats.scroll_writes++;
      return;
    }
    if (address >= UINT16_C(0x8000) && address <= UINT16_C(0xBFFF))
    {
      // Banked flash programming is intentionally deferred; reads are fully
      // mapped so the unmodified cl1-leda.ic5 can execute.
      return;
    }
    if (address >= UINT16_C(0xFF84) && address <= UINT16_C(0xFF87))
    {
      const u32 bank_index = static_cast<u32>(address - UINT16_C(0xFF84)) |
                             (static_cast<u32>(value) << 2);
      flash_bank = UINT32_C(0x4000) * bank_index;
      return;
    }

    switch (address)
    {
      case UINT16_C(0xFFB0): port_ddr[0] = value; return;
      case UINT16_C(0xFFB1): port_ddr[1] = value; return;
      case UINT16_C(0xFFB2): port_dr[0] = value; return;
      case UINT16_C(0xFFB3): port_dr[1] = value; return;
      case UINT16_C(0xFFB4): port_ddr[2] = value; return;
      case UINT16_C(0xFFB5): port_ddr[3] = value; return;
      case UINT16_C(0xFFB6): port_dr[2] = value; return;
      case UINT16_C(0xFFB7): port_dr[3] = value; return;
      case UINT16_C(0xFFB8): port_ddr[4] = value; return;
      case UINT16_C(0xFFB9): port_ddr[5] = value; return;
      case UINT16_C(0xFFBA): port_dr[4] = value; return;
      case UINT16_C(0xFFBB): port_dr[5] = value; return;
      case UINT16_C(0xFFBC): port_ddr[6] = value; return;
      case UINT16_C(0xFFBD): port_ddr[7] = value; return;
      case UINT16_C(0xFFBE): port_dr[6] = value; return;
      case UINT16_C(0xFFBF): port_dr[7] = value; return;
      case UINT16_C(0xFFC0): port_ddr[8] = value; return;
      case UINT16_C(0xFFC1): port_dr[8] = value; return;
      case UINT16_C(0xFFC2): wscr = value; return;
      case UINT16_C(0xFFC3):
        stcr = value;
        timer_extra_clock = (value & UINT8_C(0x01)) != 0;
        return;
      case UINT16_C(0xFFC4): syscr = value; return;
      case UINT16_C(0xFFC5): mdcr = value; return;
      case UINT16_C(0xFFC6):
      {
        const bool was_edge = IsVBlankIRQEdgeTriggered();
        iscr = value;
        const bool is_edge = IsVBlankIRQEdgeTriggered();

        // MAME's base H8 INTC clears an existing level latch when that
        // input is changed to edge-triggered, then re-evaluates level inputs.
        if (!was_edge && is_edge)
          irq6_latched = false;
        else if (was_edge && !is_edge)
          irq6_latched = vblank_active;

        UpdateVBlankInterruptPending();
        return;
      }
      case UINT16_C(0xFFC7):
        ier = value;
        UpdateVBlankInterruptPending();
        return;
      case UINT16_C(0xFFC8):
      {
        // MAME H8 timer8 behavior: if an interrupt-enable bit is raised while
        // the corresponding status flag is already set, the pending interrupt
        // is generated immediately. cl1-leda.ic5 depends on this ordering.
        const u8 previous = timer_tcr;
        timer_tcr = value;

        if ((previous & TIMER_TCR_CMIEA) == 0 && (timer_tcr & TIMER_TCR_CMIEA) != 0 &&
            (timer_tcsr & TIMER_TCSR_CMFA) != 0)
          RaiseInterrupt(IRQ_VECTOR_TIMER_A);
        if ((previous & TIMER_TCR_CMIEB) == 0 && (timer_tcr & TIMER_TCR_CMIEB) != 0 &&
            (timer_tcsr & TIMER_TCSR_CMFB) != 0)
          RaiseInterrupt(IRQ_VECTOR_TIMER_B);
        if ((previous & TIMER_TCR_OVIE) == 0 && (timer_tcr & TIMER_TCR_OVIE) != 0 &&
            (timer_tcsr & TIMER_TCSR_OVF) != 0)
          RaiseInterrupt(IRQ_VECTOR_TIMER_OVERFLOW);
        return;
      }
      case UINT16_C(0xFFC9):
        timer_tcsr = static_cast<u8>((timer_tcsr & UINT8_C(0xF0)) | (value & UINT8_C(0x0F)));
        timer_tcsr &= static_cast<u8>(value | UINT8_C(0x1F));
        return;
      case UINT16_C(0xFFCA): timer_tcor[0] = value; return;
      case UINT16_C(0xFFCB): timer_tcor[1] = value; return;
      case UINT16_C(0xFFCC): timer_tcnt = value; return;

      case UINT16_C(0xFFD8): sci0_smr = value; return;
      case UINT16_C(0xFFD9): sci0_brr = value; return;
      case UINT16_C(0xFFDA): sci0_scr = value; return;
      case UINT16_C(0xFFDB): sci0_tdr = value; return;
      case UINT16_C(0xFFDC): sci0_ssr = value; return;

      case UINT16_C(0xFFE0): sci1_smr = value; return;
      case UINT16_C(0xFFE1): sci1_brr = value; return;
      case UINT16_C(0xFFE2):
      {
        const u8 old = sci1_scr;
        sci1_scr = value;
        if ((old & SCI_SCR_RE) == 0 && (sci1_scr & SCI_SCR_RE) != 0)
          TryStartSCI1Receive();
        if ((old & SCI_SCR_RIE) == 0 && (sci1_scr & SCI_SCR_RIE) != 0)
        {
          if ((sci1_ssr & SCI_SSR_RDRF) != 0)
            RaiseInterrupt(IRQ_VECTOR_SCI1_RX);
          if ((sci1_ssr & (SCI_SSR_ORER | SCI_SSR_FER | SCI_SSR_PER)) != 0)
            RaiseInterrupt(IRQ_VECTOR_SCI1_ERROR);
        }
        return;
      }
      case UINT16_C(0xFFE3): sci1_tdr = value; return;
      case UINT16_C(0xFFE4): WriteSCI1SSR(value); return;
      case UINT16_C(0xFFF1): fff1 = value; return;
      default: break;
    }

    if (address >= UINT16_C(0xC000) && address <= UINT16_C(0xFF7F))
    {
      work_ram[address - UINT16_C(0xC000)] = value;

      if (address == UINT16_C(0xD0BF) && scan_probe_write_count < 120)
      {
        const u16 start = static_cast<u16>(
          ((static_cast<u16>(display_control[7]) << 8) | display_control[6]) & UINT16_C(0x1FFF));
        const u32 fine_scroll = static_cast<u32>(~value) & 3U;
        INFO_LOG(
          "CyberLead LED SCAN-PROBE fine-write #{} pc={:04X} data={:02X} start={:04X} "
          "fine={} scroll_x={} scroll_y={} cpu_steps={}",
          scan_probe_write_count + 1, static_cast<u32>(cpu.cpu.pc), value, start,
          fine_scroll, ((start & UINT16_C(0x001F)) * 4U) + fine_scroll,
          start >> 5, stats.cpu_steps);
        scan_probe_write_count++;
      }
    }
  }

  bool QueueWire(std::span<const u8> wire)
  {
    if (!firmware_loaded)
      return true;
    if (wire.size() > (rx_queue.size() - rx_count))
      return false;

    if (stats.queued_frames == 0)
    {
      // The first physical LED-link frame is the game's initial 0x71/02 setup.
      // Re-arm the bounded scanout diagnostics here so we capture game-driven
      // display behavior rather than only the LED firmware's startup animation.
      frame_diag_count = 0;
      scan_probe_vblank_count = 0;
      scan_probe_write_count = 0;

      const u16 start = static_cast<u16>(
        ((static_cast<u16>(display_control[7]) << 8) | display_control[6]) & UINT16_C(0x1FFF));
      const u32 fine_scroll =
        static_cast<u32>(~work_ram[UINT16_C(0xD0BF) - UINT16_C(0xC000)]) & 3U;
      INFO_LOG(
        "CyberLead LED SCAN-PROBE link-rearm wire_bytes={} pc={:04X} start={:04X} "
        "fine={} scroll_x={} scroll_y={} iscr={:02X} ier={:02X} timer={:02X} cpu_steps={}",
        wire.size(), static_cast<u32>(cpu.cpu.pc), start, fine_scroll,
        ((start & UINT16_C(0x001F)) * 4U) + fine_scroll, start >> 5,
        iscr, ier, timer_tcnt, stats.cpu_steps);
    }

    for (const u8 value : wire)
    {
      rx_queue[rx_tail] = value;
      rx_tail = (rx_tail + 1) % rx_queue.size();
      rx_count++;
    }

    stats.queued_frames++;
    stats.queued_wire_bytes += wire.size();
    TryStartSCI1Receive();
    return true;
  }

  void AdvanceLEDClocks(u32 clocks)
  {
    if (!firmware_loaded || clocks == 0 || cpu.error_code != H8_DEBUG_NO_ERROR)
      return;

    cycle_balance += static_cast<s64>(clocks);
    while (cycle_balance > 0 && cpu.error_code == H8_DEBUG_NO_ERROR)
    {
      if (ServiceInterrupt())
        continue;

      last_step_pc = static_cast<u32>(cpu.cpu.pc);
      h8_step(&cpu);
      stats.cpu_steps++;

      const u32 instruction_clocks = static_cast<u32>(std::max(cpu.cycles, 0));
      AdvancePeripherals(instruction_clocks);
      cycle_balance -= static_cast<s64>(instruction_clocks);
    }

    if (cpu.error_code != H8_DEBUG_NO_ERROR && !fault_logged)
    {
      fault_logged = true;
      stats.cpu_fault_code = static_cast<u32>(cpu.error_code);
      stats.cpu_fault_line = cpu.error_line;
      stats.cpu_fault_pc = last_step_pc;
      ERROR_LOG("CyberLead LED C77 fault error={} core_line={} pc={:04X} opcode={:04X} steps={}",
                static_cast<u32>(cpu.error_code), cpu.error_line, last_step_pc,
                static_cast<u32>(cpu.dbus.bits.u), stats.cpu_steps);
    }
  }
};

NamcoCyberLeadLEDDevice::NamcoCyberLeadLEDDevice() : m_impl(std::make_unique<Impl>())
{
}

NamcoCyberLeadLEDDevice::~NamcoCyberLeadLEDDevice() = default;
NamcoCyberLeadLEDDevice::NamcoCyberLeadLEDDevice(NamcoCyberLeadLEDDevice&&) noexcept = default;
NamcoCyberLeadLEDDevice& NamcoCyberLeadLEDDevice::operator=(NamcoCyberLeadLEDDevice&&) noexcept = default;

bool NamcoCyberLeadLEDDevice::SetFirmware(std::span<const u8> firmware)
{
  if (firmware.size() != FIRMWARE_SIZE)
    return false;

  std::copy(firmware.begin(), firmware.end(), m_impl->firmware.begin());
  m_impl->firmware_loaded = true;
  m_impl->ResetRuntime();
  return (m_impl->cpu.error_code == H8_DEBUG_NO_ERROR &&
          static_cast<u32>(m_impl->cpu.cpu.pc) == UINT32_C(0x01E4));
}

bool NamcoCyberLeadLEDDevice::HasFirmware() const
{
  return m_impl->firmware_loaded;
}

void NamcoCyberLeadLEDDevice::Reset()
{
  m_impl->ResetRuntime();
}

bool NamcoCyberLeadLEDDevice::QueueWireBytes(std::span<const u8> wire)
{
  return m_impl->QueueWire(wire);
}

void NamcoCyberLeadLEDDevice::Advance(u32 source_clocks, u32 source_clock_hz)
{
  if (!m_impl->firmware_loaded || source_clocks == 0 || source_clock_hz == 0)
    return;

  const u64 scaled = m_impl->source_clock_fraction +
                     static_cast<u64>(source_clocks) * CLOCK_HZ;
  const u32 led_clocks = static_cast<u32>(scaled / source_clock_hz);
  m_impl->source_clock_fraction = scaled % source_clock_hz;
  m_impl->AdvanceLEDClocks(led_clocks);
}

const NamcoCyberLeadLEDDevice::Stats& NamcoCyberLeadLEDDevice::GetStats() const
{
  return m_impl->stats;
}

const NamcoCyberLeadLEDDevice::FrameState& NamcoCyberLeadLEDDevice::GetFrameState() const
{
  return m_impl->frame_state;
}

std::span<const u8> NamcoCyberLeadLEDDevice::GetDisplayRAM() const
{
  return m_impl->display_ram;
}

std::span<const u8> NamcoCyberLeadLEDDevice::GetDisplayControl() const
{
  return m_impl->display_control;
}

std::span<const u8> NamcoCyberLeadLEDDevice::GetScroll() const
{
  return m_impl->scroll;
}

} // namespace Arcade::JVS