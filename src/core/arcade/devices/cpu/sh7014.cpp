#include "sh7014.h"

#include <algorithm>
#include <cstring>

namespace Arcade
{
namespace
{
constexpr uint32_t SCI0_BASE = 0xFFFF81A0u;
constexpr uint32_t SCI0_END = 0xFFFF81A5u;
constexpr uint32_t MTU_BASE = 0xFFFF8240u;
constexpr uint32_t MTU_END = 0xFFFF82AFu;
constexpr uint32_t INTC_BASE = 0xFFFF8348u;
constexpr uint32_t INTC_END = 0xFFFF835Bu;
constexpr uint32_t PORT_BASE = 0xFFFF8382u;
constexpr uint32_t PORT_END = 0xFFFF83BBu;
constexpr uint32_t BSC_BASE = 0xFFFF8620u;
constexpr uint32_t BSC_END = 0xFFFF8633u;
constexpr uint32_t DMAC_BASE = 0xFFFF86B0u;
constexpr uint32_t DMAC_END = 0xFFFF86DFu;

constexpr uint8_t SCI_SCR_TIE = 0x80u;
constexpr uint8_t SCI_SCR_TE = 0x20u;
constexpr uint8_t SCI_SSR_TDRE = 0x80u;
constexpr uint8_t SCI_SSR_TEND = 0x04u;
constexpr uint8_t SCI0_TXI_VECTOR = 130u;

constexpr uint16_t DMAOR_DME = 0x0001u;
constexpr uint16_t DMAOR_ERROR_MASK = 0x0006u;
constexpr uint32_t CHCR_DE = 0x0001u;
constexpr uint32_t CHCR_TE = 0x0002u;
constexpr uint32_t CHCR_IE = 0x0004u;
constexpr uint8_t DMA_REQUEST_SCI_TXI0 = 12u;
constexpr uint8_t DMA0_VECTOR = 72u;
constexpr uint8_t DMA_REQUEST_NONE = 0xFFu;
constexpr uint32_t DMA_INTERNAL_START_DELAY = 4u;
constexpr uint32_t DMA_EXTERNAL_UNIT_DELAY = 2u;

uint8_t ReadWordByte(uint16_t value, uint32_t address)
{
  return (address & 1u) ? static_cast<uint8_t>(value) : static_cast<uint8_t>(value >> 8);
}

uint16_t ReplaceWordByte(uint16_t value, uint32_t address, uint8_t data)
{
  if (address & 1u)
    return static_cast<uint16_t>((value & 0xFF00u) | data);
  return static_cast<uint16_t>((value & 0x00FFu) | (static_cast<uint16_t>(data) << 8));
}
} // namespace

SH7014::SH7014() : m_cpu(MakeCPUBus())
{
  // Instruction fetches overwhelmingly execute from the SH7014's on-chip
  // memories. Map those live arrays directly into the interpreter so opcode
  // fetches do not pay a std::function bus dispatch on every instruction.
  m_cpu.SetInstructionFetchRegion(0, CRAM_BASE, CRAM_SIZE, m_cram.data());
  m_cpu.SetInstructionFetchRegion(1, SRAM_BASE, SRAM_SIZE, m_sram.data());
  m_cpu.SetInstructionFetchRegion(2, CACHE_RAM_BASE, CACHE_RAM_SIZE, m_cache_ram.data());
}

ArcadeDuck::SH2::CPU::Bus SH7014::MakeCPUBus()
{
  return {
    [this](uint32_t address) { return Read8(address); },
    [this](uint32_t address) { return Read16(address); },
    [this](uint32_t address) { return Read32(address); },
    [this](uint32_t address, uint8_t value) { Write8(address, value); },
    [this](uint32_t address, uint16_t value) { Write16(address, value); },
    [this](uint32_t address, uint32_t value) { Write32(address, value); },
  };
}

void SH7014::Reset(bool power_on)
{
  SyncPeripherals();
  m_peripheral_next_event = 0;
  m_ccr = 0;
  m_sci0 = {};
  m_dmac = {};
  m_mtu = {};
  m_internal_irq_pending.fill(false);

  // SH7014 on-chip RAM is physical SRAM and survives a CPU/peripheral reset.
  // QF installs its DMA interrupt trampoline here before the later CDXA reset.
  // ClearMemory() still clears it for a true fresh device initialization.
  m_bsc = {};
  m_ports = {};
  m_intc = {};
  m_cpu.Reset(power_on);
}

void SH7014::ClearMemory()
{
  m_peripheral_pending_cycles = 0;
  m_peripheral_next_event = 0;
  m_cram.fill(0);
  m_sram.fill(0);
  m_cache_ram.fill(0);
  m_ccr = 0;
  m_sci0 = {};
  m_dmac = {};
  m_mtu = {};
  m_internal_irq_pending.fill(false);
  m_bsc = {};
  m_ports = {};
  m_intc = {};
}

bool SH7014::LoadFirmware(const void* data, size_t size, uint32_t address)
{
  if (!data || address < CRAM_BASE)
    return false;

  const uint64_t offset = static_cast<uint64_t>(address - CRAM_BASE);
  if (offset > m_cram.size() || size > (m_cram.size() - static_cast<size_t>(offset)))
    return false;

  std::memcpy(m_cram.data() + static_cast<size_t>(offset), data, size);
  return true;
}


uint32_t SH7014::Step()
{
  const uint32_t cycles = m_cpu.Step();
  AddPeripheralCycles(cycles);
  return cycles;
}

uint64_t SH7014::Run(uint64_t cycles)
{
  uint64_t used = 0;
  while (used < cycles)
    used += Step();
  return used;
}

void SH7014::SetSCI0TransmitRate(uint32_t bytes_per_second)
{
  if (bytes_per_second == 0)
    return;

  m_sci0_tx_bytes_per_second = bytes_per_second;
  m_sci0_tx_cycle_remainder = 0;

  // Keep the rounded value as a fallback. The actual transfer cadence is
  // fractional so the programmed CDXA rate does not drift.
  m_sci0_tx_cycles_per_byte =
    std::max<uint32_t>(1u, (CLOCK_HZ + (bytes_per_second / 2u)) / bytes_per_second);
}

uint32_t SH7014::NextSCI0TransmitCycles()
{
  if (m_sci0_tx_bytes_per_second == 0)
    return m_sci0_tx_cycles_per_byte;

  uint32_t cycles = CLOCK_HZ / m_sci0_tx_bytes_per_second;
  const uint32_t remainder = CLOCK_HZ % m_sci0_tx_bytes_per_second;

  m_sci0_tx_cycle_remainder += remainder;
  if (m_sci0_tx_cycle_remainder >= m_sci0_tx_bytes_per_second)
  {
    m_sci0_tx_cycle_remainder -= m_sci0_tx_bytes_per_second;
    cycles++;
  }

  return std::max<uint32_t>(1u, cycles);
}

bool SH7014::ServiceExternalDMARequest(uint32_t channel)
{
  // CDXA IDE uses SH7014 external dual-address DMA request source 0.
  if (channel >= m_dmac.channel.size())
    return false;

  SetExternalDMARequestLine(channel, true);
  return IsDMAChannelReady(channel) && (((m_dmac.channel[channel].chcr >> 8) & 0x0Fu) == 0u);
}

void SH7014::SetExternalDMARequestLine(uint32_t channel, bool asserted)
{
  if (channel >= m_dmac.channel.size())
    return;

  // The ATAPI side changes DREQ between SH-2 instructions or from inside a DMA
  // transfer; either way the DMAC must be current first and step again after.
  BeginPeripheralAccess();
  EndPeripheralWrite();

  auto& ch = m_dmac.channel[channel];
  ch.external_request_asserted = asserted;
  if (!asserted)
  {
    if (ch.pending_request_source == 0u)
    {
      ch.pending_request_source = DMA_REQUEST_NONE;
      ch.request_cycles_remaining = 0;
    }
    return;
  }

  if (IsDMAChannelReady(channel) && (((ch.chcr >> 8) & 0x0Fu) == 0u) &&
      ch.pending_request_source == DMA_REQUEST_NONE)
  {
    ScheduleDMARequest(channel, 0u, DMA_EXTERNAL_UNIT_DELAY);
  }
}

uint8_t SH7014::ReadSCIRegister(uint32_t address) const
{
  switch (address)
  {
    case 0xFFFF81A0u: return m_sci0.smr;
    case 0xFFFF81A1u: return m_sci0.brr;
    case 0xFFFF81A2u: return m_sci0.scr;
    case 0xFFFF81A3u: return m_sci0.tdr;
    case 0xFFFF81A4u: return m_sci0.ssr;
    case 0xFFFF81A5u: return m_sci0.rdr;
    default: return 0xFF;
  }
}

void SH7014::WriteSCIRegister(uint32_t address, uint8_t value)
{
  switch (address)
  {
    case 0xFFFF81A0u: m_sci0.smr = value; break;
    case 0xFFFF81A1u: m_sci0.brr = value; break;
    case 0xFFFF81A2u:
      m_sci0.scr = value;
      if ((m_sci0.scr & SCI_SCR_TE) == 0)
      {
        m_sci0.tx_cycles_remaining = 0;
        m_sci0.ssr |= static_cast<uint8_t>(SCI_SSR_TDRE | SCI_SSR_TEND);
      }
      UpdateSCI0TransmitRequest();
      break;
    case 0xFFFF81A3u:
      m_sci0.tdr = value;
      break;
    case 0xFFFF81A4u:
    {
      constexpr uint8_t writable = 0xF8u;
      const uint8_t old = m_sci0.ssr;
      m_sci0.ssr = static_cast<uint8_t>((old & ~writable) | (old & value & writable) | (old & 0x06u) | (value & 0x01u));
      if ((m_sci0.scr & SCI_SCR_TE) == 0)
        m_sci0.ssr |= static_cast<uint8_t>(SCI_SSR_TDRE | SCI_SSR_TEND);
      UpdateSCI0TransmitRequest();
      break;
    }
    default: break;
  }
}

void SH7014::WriteSCI0TDRFromDMA(uint8_t value)
{
  m_sci0.tdr = value;
  m_sci0.tx_tdr_pending = true;
  m_sci0.ssr &= static_cast<uint8_t>(~(SCI_SSR_TDRE | SCI_SSR_TEND));
  SetInternalInterrupt(SCI0_TXI_VECTOR, false);


  if (!m_sci0.tx_active)
    StartSCI0Transmit();
}

void SH7014::StartSCI0Transmit()
{
  if ((m_sci0.scr & SCI_SCR_TE) == 0 || m_sci0.tx_active || !m_sci0.tx_tdr_pending ||
      (m_sci0.ssr & SCI_SSR_TDRE) != 0)
  {
    return;
  }

  m_sci0.tx_shift = m_sci0.tdr;
  m_sci0.tx_tdr_pending = false;
  m_sci0.tx_active = true;
  m_sci0.tx_cycles_remaining = NextSCI0TransmitCycles();
  m_sci0.ssr |= SCI_SSR_TDRE;
  m_sci0.ssr &= static_cast<uint8_t>(~SCI_SSR_TEND);


  if (m_sci0_transmit_callback)
    m_sci0_transmit_callback(m_sci0_transmit_opaque, m_sci0.tx_shift);

  UpdateSCI0TransmitRequest();
}

void SH7014::AdvanceSCI0(uint32_t cycles)
{
  uint32_t remaining_cycles = cycles;

  while (remaining_cycles != 0)
  {
    if (!m_sci0.tx_active)
    {
      StartSCI0Transmit();
      UpdateSCI0TransmitRequest();

      if (!m_sci0.tx_active)
        break;
    }

    if (remaining_cycles < m_sci0.tx_cycles_remaining)
    {
      m_sci0.tx_cycles_remaining -= remaining_cycles;
      break;
    }

    remaining_cycles -= m_sci0.tx_cycles_remaining;
    m_sci0.tx_cycles_remaining = 0;
    m_sci0.tx_active = false;

    if (m_sci0.tx_tdr_pending)
    {
      // The next byte is already buffered in TDR. Carry any instruction-cycle
      // overshoot across the byte boundary instead of discarding it.
      StartSCI0Transmit();
    }
    else
    {
      m_sci0.ssr |= SCI_SSR_TEND;
      UpdateSCI0TransmitRequest();
      break;
    }
  }
}

void SH7014::UpdateSCI0TransmitRequest()
{
  UpdateSCI0TransmitRequest(DMA_INTERNAL_START_DELAY);
}

void SH7014::UpdateSCI0TransmitRequest(uint32_t dma_delay_cycles)
{
  const bool request =
    (m_sci0.scr & (SCI_SCR_TIE | SCI_SCR_TE)) == (SCI_SCR_TIE | SCI_SCR_TE) &&
    (m_sci0.ssr & SCI_SSR_TDRE) != 0;

  if (!request)
  {
    SetInternalInterrupt(SCI0_TXI_VECTOR, false);
    return;
  }

  if (ServiceDMARequest(0, DMA_REQUEST_SCI_TXI0, dma_delay_cycles))
  {
    SetInternalInterrupt(SCI0_TXI_VECTOR, false);
    return;
  }

  SetInternalInterrupt(SCI0_TXI_VECTOR, true);
}

uint32_t SH7014::ReadDMACRegister(uint32_t address) const
{
  if (address == 0xFFFF86B0u)
    return static_cast<uint32_t>(m_dmac.dmaor & 0x0007u);

  for (uint32_t channel = 0; channel < 2; channel++)
  {
    const uint32_t base = 0xFFFF86C0u + (channel * 0x10u);
    const auto& ch = m_dmac.channel[channel];
    switch (address - base)
    {
      case 0x00u: return ch.sar;
      case 0x04u: return ch.dar;
      case 0x08u: return ch.dmatcr & 0x0000FFFFu;
      case 0x0Cu: return ch.chcr & 0x000BFF7Fu;
      default: break;
    }
  }
  return 0xFFFFFFFFu;
}

void SH7014::WriteDMACRegister(uint32_t address, uint32_t value)
{

  if (address == 0xFFFF86B0u)
  {
    m_dmac.dmaor = static_cast<uint16_t>((value & ~DMAOR_ERROR_MASK) |
                                         (m_dmac.dmaor & value & DMAOR_ERROR_MASK));
    for (uint32_t channel = 0; channel < m_dmac.channel.size(); channel++)
    {
      auto& ch = m_dmac.channel[channel];
      if (!IsDMAChannelReady(channel))
      {
        ch.pending_request_source = DMA_REQUEST_NONE;
        ch.request_cycles_remaining = 0;
      }
      else if (ch.external_request_asserted && (((ch.chcr >> 8) & 0x0Fu) == 0u) &&
               ch.pending_request_source == DMA_REQUEST_NONE)
      {
        ScheduleDMARequest(channel, 0u, DMA_EXTERNAL_UNIT_DELAY);
      }
    }
    UpdateSCI0TransmitRequest();
    return;
  }

  for (uint32_t channel = 0; channel < 2; channel++)
  {
    const uint32_t base = 0xFFFF86C0u + (channel * 0x10u);
    auto& ch = m_dmac.channel[channel];
    switch (address - base)
    {
      case 0x00u:
        ch.sar = value;
        return;
      case 0x04u:
        ch.dar = value;
        return;
      case 0x08u:
        ch.dmatcr = value & 0x0000FFFFu;
        return;
      case 0x0Cu:
      {
        const uint32_t old_chcr = ch.chcr;
        ch.chcr = value & 0x000BFF7Fu;
        if ((old_chcr & CHCR_TE) != 0 && (ch.chcr & CHCR_TE) == 0)
          SetInternalInterrupt(static_cast<uint8_t>(DMA0_VECTOR + channel * 4u), false);
        if (!IsDMAChannelReady(channel))
        {
          ch.pending_request_source = DMA_REQUEST_NONE;
          ch.request_cycles_remaining = 0;
        }
        else if (ch.external_request_asserted && (((ch.chcr >> 8) & 0x0Fu) == 0u) &&
                 ch.pending_request_source == DMA_REQUEST_NONE)
        {
          ScheduleDMARequest(channel, 0u, DMA_EXTERNAL_UNIT_DELAY);
        }
        UpdateSCI0TransmitRequest();
        return;
      }
      default: break;
    }
  }
}

bool SH7014::IsDMATransferAllowed() const
{
  return (m_dmac.dmaor & DMAOR_DME) != 0 &&
         (m_dmac.dmaor & DMAOR_ERROR_MASK) == 0;
}

bool SH7014::IsDMAChannelReady(uint32_t channel) const
{
  if (channel >= m_dmac.channel.size() || !IsDMATransferAllowed())
    return false;

  const auto& ch = m_dmac.channel[channel];
  return (ch.chcr & CHCR_DE) != 0 && (ch.chcr & CHCR_TE) == 0;
}

bool SH7014::ServiceDMARequest(uint32_t channel, uint8_t request_source, uint32_t delay_cycles)
{
  if (!IsDMAChannelReady(channel))
    return false;

  auto& ch = m_dmac.channel[channel];
  if (((ch.chcr >> 8) & 0x0Fu) != request_source)
    return false;

  if (ch.pending_request_source == DMA_REQUEST_NONE)
    ScheduleDMARequest(channel, request_source, delay_cycles);

  return true;
}

void SH7014::ScheduleDMARequest(uint32_t channel, uint8_t request_source, uint32_t delay_cycles)
{
  auto& ch = m_dmac.channel[channel];
  ch.pending_request_source = request_source;
  ch.request_cycles_remaining = delay_cycles;
}

void SH7014::AdvanceDMAC(uint32_t cycles)
{
  for (uint32_t channel = 0; channel < m_dmac.channel.size(); channel++)
  {
    auto& ch = m_dmac.channel[channel];
    uint32_t available_cycles = cycles;

    while (ch.pending_request_source != DMA_REQUEST_NONE)
    {
      if (ch.request_cycles_remaining > available_cycles)
      {
        ch.request_cycles_remaining -= available_cycles;
        break;
      }

      available_cycles -= ch.request_cycles_remaining;
      const uint8_t request_source = ch.pending_request_source;
      ch.pending_request_source = DMA_REQUEST_NONE;
      ch.request_cycles_remaining = 0;

      if (!IsDMAChannelReady(channel) || (((ch.chcr >> 8) & 0x0Fu) != request_source))
        break;

      if (!ExecuteDMAUnit(channel))
        break;

      // MAME's SH7014 DMAC model spaces external transfers by two clocks.
      // Keep servicing while DREQ remains asserted, but leave internal requests
      // one-shot so SCI0 has to assert TXI again after the transmitted byte.
      if (request_source == 0u && ch.external_request_asserted && IsDMAChannelReady(channel) &&
          (((ch.chcr >> 8) & 0x0Fu) == 0u))
      {
        ScheduleDMARequest(channel, 0u, DMA_EXTERNAL_UNIT_DELAY);
        if (available_cycles == 0)
          break;
        continue;
      }

      break;
    }
  }
}

bool SH7014::ExecuteDMAUnit(uint32_t channel)
{
  if (!IsDMAChannelReady(channel))
    return false;

  auto& ch = m_dmac.channel[channel];
  const uint32_t source_mode = (ch.chcr >> 12) & 0x03u;
  const uint32_t dest_mode = (ch.chcr >> 14) & 0x03u;
  const uint32_t unit_size = 1u << ((ch.chcr >> 3) & 0x03u);
  if (source_mode == 3u || dest_mode == 3u || unit_size > 4u)
    return false;

  uint32_t source = ch.sar;
  uint32_t dest = ch.dar;
  if (source_mode == 2u)
    source -= unit_size;
  if (dest_mode == 2u)
    dest -= unit_size;

  if (unit_size == 1u)
  {
    const uint8_t data = Read8(source);
    if (dest == 0xFFFF81A3u)
    {
      WriteSCI0TDRFromDMA(data);
    }
    else
      Write8(dest, data);
  }
  else if (unit_size == 2u)
  {
    Write16(dest, Read16(source));
  }
  else
  {
    Write32(dest, Read32(source));
  }

  if (source_mode == 1u)
    source += unit_size;
  if (dest_mode == 1u)
    dest += unit_size;

  ch.sar = source;
  ch.dar = dest;

  uint32_t remaining = ch.dmatcr & 0x0000FFFFu;
  if (remaining == 0)
    remaining = 0x10000u;
  remaining--;

  if (remaining == 0)
  {
    ch.dmatcr = 0;
    ch.chcr |= CHCR_TE;
    if ((ch.chcr & CHCR_IE) != 0)
      SetInternalInterrupt(static_cast<uint8_t>(DMA0_VECTOR + channel * 4u), true);
  }
  else
  {
    ch.dmatcr = remaining & 0x0000FFFFu;
  }

  return true;
}

uint8_t SH7014::ReadMTURegister8(uint32_t address) const
{
  if (address == 0xFFFF8240u)
  {
    uint8_t value = 0;
    for (uint32_t i = 0; i < 3; i++)
      if (m_mtu.channel[i].enabled)
        value |= static_cast<uint8_t>(1u << i);
    return value;
  }
  if (address == 0xFFFF8241u)
    return static_cast<uint8_t>(m_mtu.tsyr & 0x07u);

  for (uint32_t channel = 0; channel < 3; channel++)
  {
    const uint32_t base = 0xFFFF8260u + (channel * 0x20u);
    if (address < base || address >= base + 0x20u)
      continue;
    const auto& ch = m_mtu.channel[channel];
    switch (address - base)
    {
      case 0x00u: return (channel == 0) ? ch.tcr : static_cast<uint8_t>(ch.tcr & 0x7Fu);
      case 0x01u: return static_cast<uint8_t>(ch.tmdr | 0xC0u);
      case 0x02u: return ch.tiorh;
      case 0x03u: return (channel == 0) ? ch.tiorl : 0;
      case 0x04u: return static_cast<uint8_t>(ch.tier | 0x40u);
      case 0x05u:
      {
        uint8_t value = ch.tsr;
        if (channel == 0)
          value = static_cast<uint8_t>((value & 0x1Fu) | 0xC0u);
        else
          value = static_cast<uint8_t>((value & 0xB3u) | 0x40u);
        return value;
      }
      case 0x06u: return static_cast<uint8_t>(ch.tcnt >> 8);
      case 0x07u: return static_cast<uint8_t>(ch.tcnt);
      case 0x08u: return static_cast<uint8_t>(ch.tgr[0] >> 8);
      case 0x09u: return static_cast<uint8_t>(ch.tgr[0]);
      case 0x0Au: return static_cast<uint8_t>(ch.tgr[1] >> 8);
      case 0x0Bu: return static_cast<uint8_t>(ch.tgr[1]);
      case 0x0Cu: return (channel == 0) ? static_cast<uint8_t>(ch.tgr[2] >> 8) : 0xFF;
      case 0x0Du: return (channel == 0) ? static_cast<uint8_t>(ch.tgr[2]) : 0xFF;
      case 0x0Eu: return (channel == 0) ? static_cast<uint8_t>(ch.tgr[3] >> 8) : 0xFF;
      case 0x0Fu: return (channel == 0) ? static_cast<uint8_t>(ch.tgr[3]) : 0xFF;
      default: return 0xFF;
    }
  }
  return 0xFF;
}

void SH7014::WriteMTURegister8(uint32_t address, uint8_t value)
{
  if (address == 0xFFFF8240u)
  {
    for (uint32_t i = 0; i < 3; i++)
      m_mtu.channel[i].enabled = (value & (1u << i)) != 0;
    return;
  }
  if (address == 0xFFFF8241u)
  {
    m_mtu.tsyr = static_cast<uint8_t>(value & 0x07u);
    return;
  }

  for (uint32_t channel = 0; channel < 3; channel++)
  {
    const uint32_t base = 0xFFFF8260u + (channel * 0x20u);
    if (address < base || address >= base + 0x20u)
      continue;
    auto& ch = m_mtu.channel[channel];
    switch (address - base)
    {
      case 0x00u: ch.tcr = value; return;
      case 0x01u: ch.tmdr = value; return;
      case 0x02u: ch.tiorh = value; return;
      case 0x03u: if (channel == 0) ch.tiorl = value; return;
      case 0x04u: ch.tier = value; return;
      case 0x05u:
      {
        const uint8_t mask = (channel == 0) ? 0x1Fu : 0x33u;
        ch.tsr = static_cast<uint8_t>((ch.tsr & ~mask) | (ch.tsr & value & mask) | (ch.tsr & 0x80u));
        if ((ch.tsr & 0x01u) == 0)
        {
          static constexpr uint8_t vectors[3] = {88, 96, 104};
          SetInternalInterrupt(vectors[channel], false);
        }
        return;
      }
      case 0x06u: ch.tcnt = static_cast<uint16_t>((ch.tcnt & 0x00FFu) | (static_cast<uint16_t>(value) << 8)); return;
      case 0x07u: ch.tcnt = static_cast<uint16_t>((ch.tcnt & 0xFF00u) | value); return;
      case 0x08u: ch.tgr[0] = static_cast<uint16_t>((ch.tgr[0] & 0x00FFu) | (static_cast<uint16_t>(value) << 8)); return;
      case 0x09u: ch.tgr[0] = static_cast<uint16_t>((ch.tgr[0] & 0xFF00u) | value); return;
      case 0x0Au: ch.tgr[1] = static_cast<uint16_t>((ch.tgr[1] & 0x00FFu) | (static_cast<uint16_t>(value) << 8)); return;
      case 0x0Bu: ch.tgr[1] = static_cast<uint16_t>((ch.tgr[1] & 0xFF00u) | value); return;
      case 0x0Cu: if (channel == 0) ch.tgr[2] = static_cast<uint16_t>((ch.tgr[2] & 0x00FFu) | (static_cast<uint16_t>(value) << 8)); return;
      case 0x0Du: if (channel == 0) ch.tgr[2] = static_cast<uint16_t>((ch.tgr[2] & 0xFF00u) | value); return;
      case 0x0Eu: if (channel == 0) ch.tgr[3] = static_cast<uint16_t>((ch.tgr[3] & 0x00FFu) | (static_cast<uint16_t>(value) << 8)); return;
      case 0x0Fu: if (channel == 0) ch.tgr[3] = static_cast<uint16_t>((ch.tgr[3] & 0xFF00u) | value); return;
      default: return;
    }
  }
}

uint16_t SH7014::ReadMTURegister16(uint32_t address) const
{
  return static_cast<uint16_t>((static_cast<uint16_t>(ReadMTURegister8(address)) << 8) |
                               ReadMTURegister8(address + 1u));
}

void SH7014::WriteMTURegister16(uint32_t address, uint16_t value)
{
  WriteMTURegister8(address, static_cast<uint8_t>(value >> 8));
  WriteMTURegister8(address + 1u, static_cast<uint8_t>(value));
}

void SH7014::SetInternalInterrupt(uint8_t vector, bool asserted)
{
  if (m_internal_irq_pending[vector] == asserted)
    return;

  m_internal_irq_pending[vector] = asserted;
  UpdateInterruptLine();
}

void SH7014::UpdateInterruptLine()
{
  int best_vector = -1;
  uint8_t best_level = 0;
  for (uint32_t vector = 0; vector < m_internal_irq_pending.size(); vector++)
  {
    if (!m_internal_irq_pending[vector])
      continue;

    uint8_t level = 0;
    if (vector == 72) level = static_cast<uint8_t>((m_intc.iprc >> 12) & 0x0Fu);
    else if (vector == 76) level = static_cast<uint8_t>((m_intc.iprc >> 8) & 0x0Fu);
    else if (vector >= 88 && vector <= 92) level = static_cast<uint8_t>((m_intc.iprd >> ((vector == 92) ? 8 : 12)) & 0x0Fu);
    else if (vector == 96 || vector == 97) level = static_cast<uint8_t>((m_intc.iprd >> 4) & 0x0Fu);
    else if (vector == 100 || vector == 101) level = static_cast<uint8_t>(m_intc.iprd & 0x0Fu);
    else if (vector == 104 || vector == 105) level = static_cast<uint8_t>((m_intc.ipre >> 12) & 0x0Fu);
    else if (vector == 108 || vector == 109) level = static_cast<uint8_t>((m_intc.ipre >> 8) & 0x0Fu);
    else if (vector >= 128 && vector <= 131) level = static_cast<uint8_t>((m_intc.iprf >> 4) & 0x0Fu);

    if (level > best_level)
    {
      best_level = level;
      best_vector = static_cast<int>(vector);
    }
  }

  if (best_vector >= 0 && best_level != 0)
    m_cpu.SetInterrupt(best_level, static_cast<uint8_t>(best_vector), true);
  else
    m_cpu.SetInterrupt(0, 0, false);
}

void SH7014::AdvanceMTU(uint32_t cycles)
{
  static constexpr uint32_t dividers[3][8] = {
    {1, 4, 16, 64, 0, 0, 0, 0},
    {1, 4, 16, 64, 0, 0, 256, 0},
    {1, 4, 16, 64, 0, 0, 0, 1024},
  };
  static constexpr uint8_t tgia_vectors[3] = {88, 96, 104};

  for (uint32_t channel = 0; channel < 3; channel++)
  {
    auto& ch = m_mtu.channel[channel];
    if (!ch.enabled)
      continue;

    const uint32_t prescaler = ch.tcr & 0x07u;
    const uint32_t divider = dividers[channel][prescaler];
    if (divider == 0)
      continue; // external clock source

    const uint32_t accumulated_cycles = ch.prescale_accum + cycles;
    const uint32_t timer_ticks = accumulated_cycles / divider;
    ch.prescale_accum = accumulated_cycles % divider;
    if (timer_ticks == 0)
      continue;

    const uint32_t clear_select = (ch.tcr >> 5) & ((channel == 0) ? 0x07u : 0x03u);
    const uint32_t target = ch.tgr[0];
    const uint32_t current = ch.tcnt;
    uint32_t distance_to_match = (target - current) & 0xFFFFu;
    if (distance_to_match == 0)
      distance_to_match = 0x10000u;

    bool match_a = false;
    if (clear_select == 1u)
    {
      if (timer_ticks < distance_to_match)
      {
        ch.tcnt = static_cast<uint16_t>(current + timer_ticks);
      }
      else
      {
        match_a = true;
        const uint32_t remaining_ticks = timer_ticks - distance_to_match;
        const uint32_t period = (target != 0) ? target : 0x10000u;
        ch.tcnt = static_cast<uint16_t>(remaining_ticks % period);
      }
    }
    else
    {
      match_a = (timer_ticks >= distance_to_match);
      ch.tcnt = static_cast<uint16_t>(current + timer_ticks);
    }

    if (match_a)
    {
      ch.tsr |= 0x01u;
      if ((ch.tier & 0x01u) != 0)
        SetInternalInterrupt(tgia_vectors[channel], true);
    }
  }
}

void SH7014::AdvancePeripherals(uint32_t cycles)
{
  if (m_mtu.channel[0].enabled || m_mtu.channel[1].enabled || m_mtu.channel[2].enabled)
    AdvanceMTU(cycles);

  AdvanceSCI0(cycles);

  if (m_dmac.channel[0].pending_request_source != DMA_REQUEST_NONE ||
      m_dmac.channel[1].pending_request_source != DMA_REQUEST_NONE)
  {
    AdvanceDMAC(cycles);
  }
}

void SH7014::AddPeripheralCycles(uint32_t cycles)
{
  if (m_peripheral_pending_cycles + cycles < m_peripheral_next_event)
  {
    m_peripheral_pending_cycles += cycles;
    return;
  }

  // Bring the peripherals up to the start of this instruction, which no event
  // falls inside, then step this instruction's cycles on their own: an SCI0
  // byte that ends here schedules a DMA request that the DMAC then counts down
  // by these cycles only.
  SyncPeripherals();
  m_peripheral_sync_active = true;
  AdvancePeripherals(cycles);
  m_peripheral_sync_active = false;
  m_peripheral_next_event = m_peripheral_step_forced ? 0 : NextPeripheralEvent();
  m_peripheral_step_forced = false;
}

void SH7014::SyncPeripherals()
{
  if (m_peripheral_pending_cycles != 0)
  {
    m_peripheral_sync_active = true;
    AdvancePeripherals(static_cast<uint32_t>(m_peripheral_pending_cycles));
    m_peripheral_sync_active = false;
    m_peripheral_pending_cycles = 0;
  }
  m_peripheral_next_event = m_peripheral_step_forced ? 0 : NextPeripheralEvent();
  m_peripheral_step_forced = false;
}

void SH7014::BeginPeripheralAccess()
{
  if (!m_peripheral_sync_active)
    SyncPeripherals();
}

void SH7014::EndPeripheralWrite()
{
  // The peripherals step at the end of the instruction that wrote them.
  if (m_peripheral_sync_active)
    m_peripheral_step_forced = true;
  else
    m_peripheral_next_event = 0;
}

bool SH7014::IsSCI0IdleStable() const
{
  // True when the idle SCI0 step would neither start a byte nor change the
  // TXI request: no transmit to start, and UpdateSCI0TransmitRequest() would
  // leave the TXI line and the DMA request as they are.
  if ((m_sci0.scr & SCI_SCR_TE) != 0 && m_sci0.tx_tdr_pending && (m_sci0.ssr & SCI_SSR_TDRE) == 0)
    return false;

  const bool txi_asserted = m_internal_irq_pending[SCI0_TXI_VECTOR];
  const bool request = (m_sci0.scr & (SCI_SCR_TIE | SCI_SCR_TE)) == (SCI_SCR_TIE | SCI_SCR_TE) &&
                       (m_sci0.ssr & SCI_SSR_TDRE) != 0;
  if (!request)
    return !txi_asserted;

  const auto& ch = m_dmac.channel[0];
  if (IsDMAChannelReady(0) && ((ch.chcr >> 8) & 0x0Fu) == DMA_REQUEST_SCI_TXI0)
    return ch.pending_request_source != DMA_REQUEST_NONE && !txi_asserted;

  return txi_asserted;
}

uint64_t SH7014::NextPeripheralEvent() const
{
  // Bound the batch so the cycle count stays well inside 32 bits.
  uint64_t next = UINT64_C(1) << 24;

  for (uint32_t channel = 0; channel < 3; channel++)
  {
    const auto& ch = m_mtu.channel[channel];
    if (!ch.enabled)
      continue;

    static constexpr uint32_t dividers[3][8] = {
      {1, 4, 16, 64, 0, 0, 0, 0},
      {1, 4, 16, 64, 0, 0, 256, 0},
      {1, 4, 16, 64, 0, 0, 0, 1024},
    };
    const uint32_t divider = dividers[channel][ch.tcr & 0x07u];
    if (divider == 0)
      continue;

    // Cycles until TCNT reaches TGRA, the only MTU event modelled.
    uint32_t distance = (static_cast<uint32_t>(ch.tgr[0]) - ch.tcnt) & 0xFFFFu;
    if (distance == 0)
      distance = 0x10000u;
    const uint64_t cycles = static_cast<uint64_t>(distance) * divider;
    next = std::min<uint64_t>(next, (cycles > ch.prescale_accum) ? (cycles - ch.prescale_accum) : 0);
  }

  if (m_sci0.tx_active)
    next = std::min<uint64_t>(next, m_sci0.tx_cycles_remaining);
  else if (!IsSCI0IdleStable())
    return 0;

  for (const auto& ch : m_dmac.channel)
  {
    if (ch.pending_request_source != DMA_REQUEST_NONE)
      next = std::min<uint64_t>(next, ch.request_cycles_remaining);
  }

  return next;
}

uint16_t SH7014::ReadBSCRegister(uint32_t address) const
{
  switch (address)
  {
    case 0xFFFF8620u: return static_cast<uint16_t>((m_bsc.bcr1 & 0x010Fu) | 0x2000u);
    case 0xFFFF8622u: return m_bsc.bcr2;
    case 0xFFFF8624u: return m_bsc.wcr1;
    case 0xFFFF8626u: return static_cast<uint16_t>(m_bsc.wcr2 & 0x03FFu);
    case 0xFFFF862Au: return static_cast<uint16_t>(m_bsc.dcr & ~0x0048u);
    case 0xFFFF862Cu: return static_cast<uint16_t>(m_bsc.rtcsr & 0x007Fu);
    case 0xFFFF862Eu: return static_cast<uint16_t>(m_bsc.rtcnt & 0x00FFu);
    case 0xFFFF8630u: return static_cast<uint16_t>(m_bsc.rtcor & 0x00FFu);
    default: return 0xFFFFu;
  }
}

void SH7014::WriteBSCRegister(uint32_t address, uint16_t value)
{
  switch (address)
  {
    case 0xFFFF8620u: m_bsc.bcr1 = value; break;
    case 0xFFFF8622u: m_bsc.bcr2 = value; break;
    case 0xFFFF8624u: m_bsc.wcr1 = value; break;
    case 0xFFFF8626u: m_bsc.wcr2 = value; break;
    case 0xFFFF862Au: m_bsc.dcr = value; break;
    case 0xFFFF862Cu: m_bsc.rtcsr = value; break;
    case 0xFFFF862Eu: m_bsc.rtcnt = value; break;
    case 0xFFFF8630u: m_bsc.rtcor = value; break;
    default: break;
  }
}

uint16_t SH7014::ReadPortRegister(uint32_t address) const
{
  switch (address)
  {
    case 0xFFFF8382u: return static_cast<uint16_t>((m_ports.padr & m_ports.paior) & 0x83FFu);
    case 0xFFFF8386u: return m_ports.paior;
    case 0xFFFF838Cu: return m_ports.pacr1;
    case 0xFFFF838Eu: return m_ports.pacr2;
    case 0xFFFF8390u: return static_cast<uint16_t>((m_ports.pbdr & m_ports.pbior) & 0x03FFu);
    case 0xFFFF8394u: return m_ports.pbior;
    case 0xFFFF8398u: return m_ports.pbcr1;
    case 0xFFFF839Au: return m_ports.pbcr2;
    case 0xFFFF83B0u: return static_cast<uint16_t>(m_ports.pedr & m_ports.peior);
    case 0xFFFF83B4u: return m_ports.peior;
    case 0xFFFF83B8u: return m_ports.pecr1;
    case 0xFFFF83BAu: return m_ports.pecr2;
    default: return 0xFFFFu;
  }
}

void SH7014::WritePortRegister(uint32_t address, uint16_t value)
{
  bool portb_changed = false;

  switch (address)
  {
    case 0xFFFF8382u: m_ports.padr = static_cast<uint16_t>(value & 0x83FFu); break;
    case 0xFFFF8386u: m_ports.paior = static_cast<uint16_t>(value & 0x83FFu); break;
    case 0xFFFF838Cu: m_ports.pacr1 = static_cast<uint16_t>(value & 0x400Fu); break;
    case 0xFFFF838Eu: m_ports.pacr2 = static_cast<uint16_t>(value & 0xFD75u); break;
    case 0xFFFF8390u:
      m_ports.pbdr = value;
      portb_changed = true;
      break;
    case 0xFFFF8394u:
      m_ports.pbior = static_cast<uint16_t>(value & 0x03FCu);
      portb_changed = true;
      break;
    case 0xFFFF8398u: m_ports.pbcr1 = static_cast<uint16_t>(value & 0x000Fu); break;
    case 0xFFFF839Au: m_ports.pbcr2 = static_cast<uint16_t>(value & 0xFFF5u); break;
    case 0xFFFF83B0u: m_ports.pedr = value; break;
    case 0xFFFF83B4u: m_ports.peior = value; break;
    case 0xFFFF83B8u: m_ports.pecr1 = static_cast<uint16_t>(value & 0xF000u); break;
    case 0xFFFF83BAu: m_ports.pecr2 = static_cast<uint16_t>(value & 0x55FFu); break;
    default: break;
  }

  if (portb_changed && m_portb_write_callback)
  {
    const uint16_t output = static_cast<uint16_t>((m_ports.pbdr & m_ports.pbior) & 0x03FFu);
    m_portb_write_callback(m_portb_write_opaque, output);
  }
}

uint16_t SH7014::ReadINTCRegister(uint32_t address) const
{
  switch (address)
  {
    case 0xFFFF8348u: return m_intc.ipra;
    case 0xFFFF834Au: return static_cast<uint16_t>(m_intc.iprb & 0xFF00u);
    case 0xFFFF834Cu: return m_intc.iprc;
    case 0xFFFF834Eu: return m_intc.iprd;
    case 0xFFFF8350u: return static_cast<uint16_t>(m_intc.ipre & 0xFF00u);
    case 0xFFFF8352u: return static_cast<uint16_t>(m_intc.iprf & 0x00FFu);
    case 0xFFFF8354u: return static_cast<uint16_t>(m_intc.iprg & 0xF0FFu);
    case 0xFFFF8356u: return static_cast<uint16_t>(m_intc.iprh & 0xF000u);
    case 0xFFFF8358u: return static_cast<uint16_t>(m_intc.icr & 0x01F3u);
    case 0xFFFF835Au: return static_cast<uint16_t>(m_intc.isr & 0x00F3u);
    default: return 0xFFFFu;
  }
}

void SH7014::WriteINTCRegister(uint32_t address, uint16_t value)
{
  switch (address)
  {
    case 0xFFFF8348u: m_intc.ipra = value; break;
    case 0xFFFF834Au: m_intc.iprb = value; break;
    case 0xFFFF834Cu: m_intc.iprc = value; break;
    case 0xFFFF834Eu: m_intc.iprd = value; break;
    case 0xFFFF8350u: m_intc.ipre = value; break;
    case 0xFFFF8352u: m_intc.iprf = value; break;
    case 0xFFFF8354u: m_intc.iprg = value; break;
    case 0xFFFF8356u: m_intc.iprh = value; break;
    case 0xFFFF8358u: m_intc.icr = value; break;
    case 0xFFFF835Au:
    {
      // Edge-detected external IRQ flags can only be cleared by software.
      const uint16_t old = m_intc.isr;
      m_intc.isr = static_cast<uint16_t>((old & ~m_intc.icr) | (old & value & m_intc.icr));
      break;
    }
    default: break;
  }
  UpdateInterruptLine();
}

bool SH7014::ReadPeripheral8(uint32_t address, uint8_t& value) const
{
  const uint32_t base = address & ~1u;

  if (address >= SCI0_BASE && address <= SCI0_END)
  {
    value = ReadSCIRegister(address);
    return true;
  }

  if (address >= MTU_BASE && address <= MTU_END)
  {
    value = ReadMTURegister8(address);
    return true;
  }

  if (address >= DMAC_BASE && address <= DMAC_END)
  {
    if (address <= 0xFFFF86B1u)
    {
      const uint16_t v = static_cast<uint16_t>(ReadDMACRegister(0xFFFF86B0u));
      value = ReadWordByte(v, address);
      return true;
    }
    const uint32_t reg = address & ~3u;
    const uint32_t v = ReadDMACRegister(reg);
    const uint32_t shift = (3u - (address & 3u)) * 8u;
    value = static_cast<uint8_t>(v >> shift);
    return true;
  }

  if (address >= INTC_BASE && address <= INTC_END)
  {
    value = ReadWordByte(ReadINTCRegister(base), address);
    return true;
  }

  if (address >= PORT_BASE && address <= PORT_END)
  {
    if (address == 0xFFFF83B2u)
    {
      value = 0xFF;
      return true;
    }
    if (address == 0xFFFF83B3u)
    {
      value = m_ports.pfdr;
      return true;
    }
    switch (base)
    {
      case 0xFFFF8382u: case 0xFFFF8386u: case 0xFFFF838Cu: case 0xFFFF838Eu:
      case 0xFFFF8390u: case 0xFFFF8394u: case 0xFFFF8398u: case 0xFFFF839Au:
      case 0xFFFF83B0u: case 0xFFFF83B4u: case 0xFFFF83B8u: case 0xFFFF83BAu:
        value = ReadWordByte(ReadPortRegister(base), address);
        return true;
      default: break;
    }
  }

  if (address >= BSC_BASE && address <= BSC_END)
  {
    switch (base)
    {
      case 0xFFFF8620u: case 0xFFFF8622u: case 0xFFFF8624u: case 0xFFFF8626u:
      case 0xFFFF862Au: case 0xFFFF862Cu: case 0xFFFF862Eu: case 0xFFFF8630u:
        value = ReadWordByte(ReadBSCRegister(base), address);
        return true;
      default: break;
    }
  }

  return false;
}

bool SH7014::WritePeripheral8(uint32_t address, uint8_t value)
{
  const uint32_t base = address & ~1u;

  if (address >= SCI0_BASE && address <= SCI0_END)
  {
    WriteSCIRegister(address, value);
    return true;
  }

  if (address >= MTU_BASE && address <= MTU_END)
  {
    WriteMTURegister8(address, value);
    return true;
  }

  if (address >= DMAC_BASE && address <= DMAC_END)
  {
    if (address <= 0xFFFF86B1u)
    {
      const uint16_t old_value = static_cast<uint16_t>(ReadDMACRegister(0xFFFF86B0u));
      WriteDMACRegister(0xFFFF86B0u, ReplaceWordByte(old_value, address, value));
      return true;
    }
    const uint32_t reg = address & ~3u;
    const uint32_t old_value = ReadDMACRegister(reg);
    const uint32_t shift = (3u - (address & 3u)) * 8u;
    const uint32_t mask = 0xFFu << shift;
    WriteDMACRegister(reg, (old_value & ~mask) | (static_cast<uint32_t>(value) << shift));
    return true;
  }

  if (address >= INTC_BASE && address <= INTC_END)
  {
    const uint16_t old_value = ReadINTCRegister(base);
    WriteINTCRegister(base, ReplaceWordByte(old_value, address, value));
    return true;
  }

  if (address >= PORT_BASE && address <= PORT_END)
  {
    if (address == 0xFFFF83B2u || address == 0xFFFF83B3u)
      return true;
    switch (base)
    {
      case 0xFFFF8382u: case 0xFFFF8386u: case 0xFFFF838Cu: case 0xFFFF838Eu:
      case 0xFFFF8390u: case 0xFFFF8394u: case 0xFFFF8398u: case 0xFFFF839Au:
      case 0xFFFF83B0u: case 0xFFFF83B4u: case 0xFFFF83B8u: case 0xFFFF83BAu:
      {
        const uint16_t old_value = ReadPortRegister(base);
        WritePortRegister(base, ReplaceWordByte(old_value, address, value));
        return true;
      }
      default: break;
    }
  }

  if (address >= BSC_BASE && address <= BSC_END)
  {
    switch (base)
    {
      case 0xFFFF8620u: case 0xFFFF8622u: case 0xFFFF8624u: case 0xFFFF8626u:
      case 0xFFFF862Au: case 0xFFFF862Cu: case 0xFFFF862Eu: case 0xFFFF8630u:
      {
        const uint16_t old_value = ReadBSCRegister(base);
        WriteBSCRegister(base, ReplaceWordByte(old_value, address, value));
        return true;
      }
      default: break;
    }
  }

  return false;
}

bool SH7014::ReadPeripheral16(uint32_t address, uint16_t& value) const
{
  if (address & 1u)
    return false;

  if (address >= MTU_BASE && address + 1u <= MTU_END)
  {
    value = ReadMTURegister16(address);
    return true;
  }

  if (address >= DMAC_BASE && address + 1u <= DMAC_END)
  {
    if (address == 0xFFFF86B0u)
      value = static_cast<uint16_t>(ReadDMACRegister(address));
    else
    {
      const uint32_t reg = address & ~3u;
      const uint32_t v = ReadDMACRegister(reg);
      value = (address & 2u) ? static_cast<uint16_t>(v) : static_cast<uint16_t>(v >> 16);
    }
    return true;
  }

  if (address >= INTC_BASE && address <= INTC_END)
  {
    value = ReadINTCRegister(address);
    return true;
  }

  switch (address)
  {
    case 0xFFFF8382u: case 0xFFFF8386u: case 0xFFFF838Cu: case 0xFFFF838Eu:
    case 0xFFFF8390u: case 0xFFFF8394u: case 0xFFFF8398u: case 0xFFFF839Au:
    case 0xFFFF83B0u: case 0xFFFF83B4u: case 0xFFFF83B8u: case 0xFFFF83BAu:
      value = ReadPortRegister(address);
      return true;
    case 0xFFFF83B2u:
      value = static_cast<uint16_t>(0xFF00u | m_ports.pfdr);
      return true;
    default: break;
  }

  switch (address)
  {
    case 0xFFFF8620u: case 0xFFFF8622u: case 0xFFFF8624u: case 0xFFFF8626u:
    case 0xFFFF862Au: case 0xFFFF862Cu: case 0xFFFF862Eu: case 0xFFFF8630u:
      value = ReadBSCRegister(address);
      return true;
    default: break;
  }

  return false;
}

bool SH7014::WritePeripheral16(uint32_t address, uint16_t value)
{
  if (address & 1u)
    return false;

  if (address >= MTU_BASE && address + 1u <= MTU_END)
  {
    WriteMTURegister16(address, value);
    return true;
  }

  if (address >= DMAC_BASE && address + 1u <= DMAC_END)
  {
    if (address == 0xFFFF86B0u)
      WriteDMACRegister(address, value);
    else
    {
      const uint32_t reg = address & ~3u;
      const uint32_t old_value = ReadDMACRegister(reg);
      const uint32_t merged = (address & 2u) ? ((old_value & 0xFFFF0000u) | value) :
                                              ((old_value & 0x0000FFFFu) | (static_cast<uint32_t>(value) << 16));
      WriteDMACRegister(reg, merged);
    }
    return true;
  }

  if (address >= INTC_BASE && address <= INTC_END)
  {
    WriteINTCRegister(address, value);
    return true;
  }

  switch (address)
  {
    case 0xFFFF8382u: case 0xFFFF8386u: case 0xFFFF838Cu: case 0xFFFF838Eu:
    case 0xFFFF8390u: case 0xFFFF8394u: case 0xFFFF8398u: case 0xFFFF839Au:
    case 0xFFFF83B0u: case 0xFFFF83B4u: case 0xFFFF83B8u: case 0xFFFF83BAu:
      WritePortRegister(address, value);
      return true;
    case 0xFFFF83B2u:
      return true;
    default: break;
  }

  switch (address)
  {
    case 0xFFFF8620u: case 0xFFFF8622u: case 0xFFFF8624u: case 0xFFFF8626u:
    case 0xFFFF862Au: case 0xFFFF862Cu: case 0xFFFF862Eu: case 0xFFFF8630u:
      WriteBSCRegister(address, value);
      return true;
    default: break;
  }

  return false;
}

bool SH7014::ReadInternal8(uint32_t address, uint8_t& value) const
{
  if (address >= CRAM_BASE && address < CRAM_BASE + CRAM_SIZE)
  {
    value = m_cram[address - CRAM_BASE];
    return true;
  }

  if (address >= SRAM_BASE && address < SRAM_BASE + SRAM_SIZE)
  {
    value = m_sram[address - SRAM_BASE];
    return true;
  }

  if (ReadPeripheral8(address, value))
    return true;

  if (address == CCR_BASE)
  {
    value = 0;
    return true;
  }
  if (address == CCR_BASE + 1u)
  {
    value = static_cast<uint8_t>(m_ccr & 0x001Fu);
    return true;
  }

  if (address >= CACHE_RAM_BASE)
  {
    value = m_cache_ram[address - CACHE_RAM_BASE];
    return true;
  }

  return false;
}

bool SH7014::WriteInternal8(uint32_t address, uint8_t value)
{
  if (address >= CRAM_BASE && address < CRAM_BASE + CRAM_SIZE)
  {
    m_cram[address - CRAM_BASE] = value;
    return true;
  }

  if (address >= SRAM_BASE && address < SRAM_BASE + SRAM_SIZE)
  {
    m_sram[address - SRAM_BASE] = value;

    return true;
  }

  if (WritePeripheral8(address, value))
    return true;

  if (address == CCR_BASE)
    return true;
  if (address == CCR_BASE + 1u)
  {
    m_ccr = static_cast<uint16_t>(value & 0x1Fu);
    return true;
  }

  if (address >= CACHE_RAM_BASE)
  {
    m_cache_ram[address - CACHE_RAM_BASE] = value;
    return true;
  }

  return false;
}

namespace
{
// SCI0, MTU, INTC, ports, BSC and DMAC.
constexpr uint32_t PERIPHERAL_BASE = 0xFFFF8000u;
constexpr uint32_t PERIPHERAL_END = 0xFFFF8800u;

bool TouchesPeripherals(uint32_t address, uint32_t size)
{
  return address < PERIPHERAL_END && (address + size) > PERIPHERAL_BASE;
}
} // namespace

uint8_t SH7014::Read8(uint32_t address)
{
  if (TouchesPeripherals(address, 1))
    BeginPeripheralAccess();
  return ReadBus8(address);
}

uint16_t SH7014::Read16(uint32_t address)
{
  if (TouchesPeripherals(address, 2))
    BeginPeripheralAccess();
  return ReadBus16(address);
}

uint32_t SH7014::Read32(uint32_t address)
{
  if (TouchesPeripherals(address, 4))
    BeginPeripheralAccess();
  return ReadBus32(address);
}

void SH7014::Write8(uint32_t address, uint8_t value)
{
  if (!TouchesPeripherals(address, 1))
  {
    WriteBus8(address, value);
    return;
  }

  BeginPeripheralAccess();
  WriteBus8(address, value);
  EndPeripheralWrite();
}

void SH7014::Write16(uint32_t address, uint16_t value)
{
  if (!TouchesPeripherals(address, 2))
  {
    WriteBus16(address, value);
    return;
  }

  BeginPeripheralAccess();
  WriteBus16(address, value);
  EndPeripheralWrite();
}

void SH7014::Write32(uint32_t address, uint32_t value)
{
  if (!TouchesPeripherals(address, 4))
  {
    WriteBus32(address, value);
    return;
  }

  BeginPeripheralAccess();
  WriteBus32(address, value);
  EndPeripheralWrite();
}

uint8_t SH7014::ReadBus8(uint32_t address) const
{
  uint8_t value = 0;
  if (ReadInternal8(address, value))
    return value;
  return m_external_bus.read8 ? m_external_bus.read8(address) : UINT8_C(0xFF);
}

uint16_t SH7014::ReadBus16(uint32_t address) const
{
  // Instruction fetches overwhelmingly target on-chip CRAM. Avoid the generic
  // peripheral decoder and byte-at-a-time internal lookup on this hot path.
  if (address < CRAM_BASE + CRAM_SIZE - 1u)
  {
    const uint32_t offset = address - CRAM_BASE;
    return static_cast<uint16_t>((static_cast<uint16_t>(m_cram[offset]) << 8) | m_cram[offset + 1u]);
  }
  if (address >= SRAM_BASE && address < SRAM_BASE + SRAM_SIZE - 1u)
  {
    const uint32_t offset = address - SRAM_BASE;
    return static_cast<uint16_t>((static_cast<uint16_t>(m_sram[offset]) << 8) | m_sram[offset + 1u]);
  }
  if (address >= CACHE_RAM_BASE)
  {
    const uint32_t offset = address - CACHE_RAM_BASE;
    if (offset < CACHE_RAM_SIZE - 1u)
      return static_cast<uint16_t>((static_cast<uint16_t>(m_cache_ram[offset]) << 8) | m_cache_ram[offset + 1u]);
  }

  uint16_t peripheral = 0;
  if (ReadPeripheral16(address, peripheral))
    return peripheral;

  uint8_t hi = 0;
  uint8_t lo = 0;
  if (ReadInternal8(address, hi) && ReadInternal8(address + 1u, lo))
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
  if (m_external_bus.read16)
    return m_external_bus.read16(address);
  return static_cast<uint16_t>((static_cast<uint16_t>(ReadBus8(address)) << 8) | ReadBus8(address + 1u));
}

uint32_t SH7014::ReadBus32(uint32_t address) const
{
  if (address < CRAM_BASE + CRAM_SIZE - 3u)
  {
    const uint32_t offset = address - CRAM_BASE;
    return (static_cast<uint32_t>(m_cram[offset]) << 24) |
           (static_cast<uint32_t>(m_cram[offset + 1u]) << 16) |
           (static_cast<uint32_t>(m_cram[offset + 2u]) << 8) |
           static_cast<uint32_t>(m_cram[offset + 3u]);
  }
  if (address >= SRAM_BASE && address < SRAM_BASE + SRAM_SIZE - 3u)
  {
    const uint32_t offset = address - SRAM_BASE;
    return (static_cast<uint32_t>(m_sram[offset]) << 24) |
           (static_cast<uint32_t>(m_sram[offset + 1u]) << 16) |
           (static_cast<uint32_t>(m_sram[offset + 2u]) << 8) |
           static_cast<uint32_t>(m_sram[offset + 3u]);
  }
  if (address >= CACHE_RAM_BASE)
  {
    const uint32_t offset = address - CACHE_RAM_BASE;
    if (offset < CACHE_RAM_SIZE - 3u)
    {
      return (static_cast<uint32_t>(m_cache_ram[offset]) << 24) |
             (static_cast<uint32_t>(m_cache_ram[offset + 1u]) << 16) |
             (static_cast<uint32_t>(m_cache_ram[offset + 2u]) << 8) |
             static_cast<uint32_t>(m_cache_ram[offset + 3u]);
    }
  }

  uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
  if (ReadInternal8(address, b0) && ReadInternal8(address + 1u, b1) &&
      ReadInternal8(address + 2u, b2) && ReadInternal8(address + 3u, b3))
  {
    return (static_cast<uint32_t>(b0) << 24) | (static_cast<uint32_t>(b1) << 16) |
           (static_cast<uint32_t>(b2) << 8) | static_cast<uint32_t>(b3);
  }
  if (m_external_bus.read32)
    return m_external_bus.read32(address);
  return (static_cast<uint32_t>(ReadBus16(address)) << 16) | ReadBus16(address + 2u);
}

void SH7014::WriteBus8(uint32_t address, uint8_t value)
{
  if (!WriteInternal8(address, value) && m_external_bus.write8)
    m_external_bus.write8(address, value);
}

void SH7014::WriteBus16(uint32_t address, uint16_t value)
{
  if (WritePeripheral16(address, value))
    return;

  if (WriteInternal8(address, static_cast<uint8_t>(value >> 8)) &&
      WriteInternal8(address + 1u, static_cast<uint8_t>(value)))
    return;
  if (m_external_bus.write16)
    m_external_bus.write16(address, value);
  else
  {
    WriteBus8(address, static_cast<uint8_t>(value >> 8));
    WriteBus8(address + 1u, static_cast<uint8_t>(value));
  }
}

void SH7014::WriteBus32(uint32_t address, uint32_t value)
{
  const bool internal = WriteInternal8(address, static_cast<uint8_t>(value >> 24)) &&
                        WriteInternal8(address + 1u, static_cast<uint8_t>(value >> 16)) &&
                        WriteInternal8(address + 2u, static_cast<uint8_t>(value >> 8)) &&
                        WriteInternal8(address + 3u, static_cast<uint8_t>(value));
  if (internal)
    return;
  if (m_external_bus.write32)
    m_external_bus.write32(address, value);
  else
  {
    WriteBus16(address, static_cast<uint16_t>(value >> 16));
    WriteBus16(address + 2u, static_cast<uint16_t>(value));
  }
}
} // namespace Arcade
