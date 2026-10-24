#pragma once

#include "core/arcade/third_party/sh2/sh2.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace Arcade
{
class SH7014
{
public:
  using SCI0TransmitCallback = void (*)(void* opaque, uint8_t value);
  using PortBWriteCallback = void (*)(void* opaque, uint16_t value);

  static constexpr uint32_t CLOCK_HZ = 14'745'600u;
  static constexpr uint32_t CRAM_BASE = 0x00000000u;
  static constexpr uint32_t CRAM_SIZE = 0x00040000u;
  static constexpr uint32_t SRAM_BASE = 0x00400000u;
  static constexpr uint32_t SRAM_SIZE = 0x00010000u;
  static constexpr uint32_t CCR_BASE = 0xFFFF8740u;
  static constexpr uint32_t CACHE_RAM_BASE = 0xFFFFF000u;
  static constexpr uint32_t CACHE_RAM_SIZE = 0x00001000u;

  SH7014();

  void Reset(bool power_on = true);
  void ClearMemory();
  bool LoadFirmware(const void* data, size_t size, uint32_t address = CRAM_BASE);

  uint32_t Step();
  uint64_t Run(uint64_t cycles);

  void SetExternalBus(ArcadeDuck::SH2::CPU::Bus bus) { m_external_bus = std::move(bus); }
  void SetSCI0TransmitRate(uint32_t bytes_per_second);
  uint32_t GetSCI0TransmitBytesPerSecond() const { return m_sci0_tx_bytes_per_second; }
  void SetSCI0TransmitCallback(SCI0TransmitCallback callback, void* opaque)
  {
    m_sci0_transmit_callback = callback;
    m_sci0_transmit_opaque = opaque;
  }
  void SetPortBWriteCallback(PortBWriteCallback callback, void* opaque)
  {
    m_portb_write_callback = callback;
    m_portb_write_opaque = opaque;
  }
  bool ServiceExternalDMARequest(uint32_t channel);
  void SetExternalDMARequestLine(uint32_t channel, bool asserted);

  uint8_t* GetCRAM() { return m_cram.data(); }
  const uint8_t* GetCRAM() const { return m_cram.data(); }
  uint8_t* GetSRAM() { return m_sram.data(); }
  const uint8_t* GetSRAM() const { return m_sram.data(); }

private:
  struct BSCState
  {
    uint16_t bcr1 = 0;
    uint16_t bcr2 = 0xFFFF;
    uint16_t wcr1 = 0xFFFF;
    uint16_t wcr2 = 0x000F;
    uint16_t dcr = 0;
    uint16_t rtcsr = 0;
    uint16_t rtcnt = 0;
    uint16_t rtcor = 0;
  };

  struct PortState
  {
    uint16_t padr = 0;
    uint16_t paior = 0;
    uint16_t pacr1 = 0;
    uint16_t pacr2 = 0;
    uint16_t pbdr = 0;
    uint16_t pbior = 0;
    uint16_t pbcr1 = 0;
    uint16_t pbcr2 = 0;
    uint16_t pedr = 0;
    uint16_t peior = 0;
    uint16_t pecr1 = 0;
    uint16_t pecr2 = 0;
    uint8_t pfdr = 0;
  };


  struct SCIState
  {
    uint8_t smr = 0;
    uint8_t brr = 0xFF;
    uint8_t scr = 0;
    uint8_t tdr = 0xFF;
    uint8_t ssr = 0x84; // TDRE | TEND
    uint8_t rdr = 0;
    uint8_t tx_shift = 0xFF;
    uint32_t tx_cycles_remaining = 0;
    bool tx_active = false;
    bool tx_tdr_pending = false;
  };

  struct DMACChannelState
  {
    uint32_t sar = 0;
    uint32_t dar = 0;
    uint32_t dmatcr = 0;
    uint32_t chcr = 0;
    uint32_t request_cycles_remaining = 0;
    uint8_t pending_request_source = 0xFF;
    bool external_request_asserted = false;
  };

  struct DMACState
  {
    uint16_t dmaor = 0;
    std::array<DMACChannelState, 2> channel{};
  };

  struct MTUChannelState
  {
    uint8_t tcr = 0;
    uint8_t tmdr = 0;
    uint8_t tiorh = 0;
    uint8_t tiorl = 0;
    uint8_t tier = 0;
    uint8_t tsr = 0x80;
    uint16_t tcnt = 0;
    std::array<uint16_t, 4> tgr{};
    uint64_t prescale_accum = 0;
    bool enabled = false;
  };

  struct MTUState
  {
    uint8_t tsyr = 0;
    std::array<MTUChannelState, 3> channel{};
  };

  struct INTCState
  {
    uint16_t ipra = 0;
    uint16_t iprb = 0;
    uint16_t iprc = 0;
    uint16_t iprd = 0;
    uint16_t ipre = 0;
    uint16_t iprf = 0;
    uint16_t iprg = 0;
    uint16_t iprh = 0;
    uint16_t icr = 0;
    uint16_t isr = 0;
  };

  ArcadeDuck::SH2::CPU::Bus MakeCPUBus();

  bool ReadInternal8(uint32_t address, uint8_t& value) const;
  bool WriteInternal8(uint32_t address, uint8_t value);
  bool ReadPeripheral8(uint32_t address, uint8_t& value) const;
  bool WritePeripheral8(uint32_t address, uint8_t value);
  bool ReadPeripheral16(uint32_t address, uint16_t& value) const;
  bool WritePeripheral16(uint32_t address, uint16_t value);

  uint8_t ReadSCIRegister(uint32_t address) const;
  void WriteSCIRegister(uint32_t address, uint8_t value);
  uint32_t NextSCI0TransmitCycles();
  void WriteSCI0TDRFromDMA(uint8_t value);
  void StartSCI0Transmit();
  void AdvanceSCI0(uint32_t cycles);
  void UpdateSCI0TransmitRequest();
  void UpdateSCI0TransmitRequest(uint32_t dma_delay_cycles);

  uint32_t ReadDMACRegister(uint32_t address) const;
  void WriteDMACRegister(uint32_t address, uint32_t value);
  bool IsDMATransferAllowed() const;
  bool IsDMAChannelReady(uint32_t channel) const;
  bool ServiceDMARequest(uint32_t channel, uint8_t request_source, uint32_t delay_cycles);
  bool ExecuteDMAUnit(uint32_t channel);
  void AdvanceDMAC(uint32_t cycles);
  void ScheduleDMARequest(uint32_t channel, uint8_t request_source, uint32_t delay_cycles);
  uint8_t ReadMTURegister8(uint32_t address) const;
  void WriteMTURegister8(uint32_t address, uint8_t value);
  uint16_t ReadMTURegister16(uint32_t address) const;
  void WriteMTURegister16(uint32_t address, uint16_t value);
  void AdvancePeripherals(uint32_t cycles);
  void AddPeripheralCycles(uint32_t cycles);
  void SyncPeripherals();
  void BeginPeripheralAccess();
  void EndPeripheralWrite();
  uint64_t NextPeripheralEvent() const;
  bool IsSCI0IdleStable() const;
  void AdvanceMTU(uint32_t cycles);
  void UpdateInterruptLine();
  void SetInternalInterrupt(uint8_t vector, bool asserted);

  uint16_t ReadBSCRegister(uint32_t address) const;
  void WriteBSCRegister(uint32_t address, uint16_t value);
  uint16_t ReadPortRegister(uint32_t address) const;
  void WritePortRegister(uint32_t address, uint16_t value);
  uint16_t ReadINTCRegister(uint32_t address) const;
  void WriteINTCRegister(uint32_t address, uint16_t value);

  uint8_t Read8(uint32_t address);
  uint16_t Read16(uint32_t address);
  uint32_t Read32(uint32_t address);
  void Write8(uint32_t address, uint8_t value);
  void Write16(uint32_t address, uint16_t value);
  void Write32(uint32_t address, uint32_t value);
  uint8_t ReadBus8(uint32_t address) const;
  uint16_t ReadBus16(uint32_t address) const;
  uint32_t ReadBus32(uint32_t address) const;
  void WriteBus8(uint32_t address, uint8_t value);
  void WriteBus16(uint32_t address, uint16_t value);
  void WriteBus32(uint32_t address, uint32_t value);

  std::array<uint8_t, CRAM_SIZE> m_cram{};
  std::array<uint8_t, SRAM_SIZE> m_sram{};
  std::array<uint8_t, CACHE_RAM_SIZE> m_cache_ram{};
  uint16_t m_ccr = 0;
  SCIState m_sci0{};
  uint32_t m_sci0_tx_cycles_per_byte = 98;
  uint32_t m_sci0_tx_bytes_per_second = 0;
  uint32_t m_sci0_tx_cycle_remainder = 0;
  DMACState m_dmac{};
  MTUState m_mtu{};
  std::array<bool, 256> m_internal_irq_pending{};
  BSCState m_bsc{};
  PortState m_ports{};
  INTCState m_intc{};

  // Cycles the MTU, SCI0 and DMAC have not been advanced through yet, and how
  // many may pass before one of them has work to do.
  uint64_t m_peripheral_pending_cycles = 0;
  uint64_t m_peripheral_next_event = 0;
  bool m_peripheral_sync_active = false;
  bool m_peripheral_step_forced = false;

  SCI0TransmitCallback m_sci0_transmit_callback = nullptr;
  void* m_sci0_transmit_opaque = nullptr;
  PortBWriteCallback m_portb_write_callback = nullptr;
  void* m_portb_write_opaque = nullptr;

  ArcadeDuck::SH2::CPU::Bus m_external_bus{};
  ArcadeDuck::SH2::CPU m_cpu;
};
} // namespace Arcade
