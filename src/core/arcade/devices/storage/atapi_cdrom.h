// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>
#include <memory>
#include <string_view>
#include <vector>

class CDImage;
class Error;
class TimingEvent;

namespace Arcade {

// ATAPI CD-ROM drive (ATA packet interface, MMC command set) backed by a CD
// image. Data is available as soon as a command is accepted; INTRQ for each
// phase is raised after a short drive delay.
class ATAPICDROM
{
public:
  struct Identity
  {
    std::string_view vendor;   // INQUIRY vendor, 8 characters
    std::string_view product;  // INQUIRY product, 16 characters
    std::string_view revision; // INQUIRY revision and IDENTIFY firmware, 4 characters
  };

  struct Callbacks
  {
    void* context = nullptr;
    void (*irq)(void* context, bool asserted) = nullptr;
    void (*dma_request)(void* context, bool asserted) = nullptr;
  };

  explicit ATAPICDROM(const Identity& identity);
  ~ATAPICDROM();

  void SetCallbacks(const Callbacks& callbacks) { m_callbacks = callbacks; }
  bool OpenMedia(const char* path, Error* error);
  bool HasMedia() const { return static_cast<bool>(m_media); }

  void Reset();

  // Command block registers 0-7 (data, error/features, sector count, LBA low,
  // LBA mid, LBA high, device, status/command).
  u16 ReadCommandBlock(u32 reg);
  void WriteCommandBlock(u32 reg, u16 value);

  // Control block: alternate status / device control.
  u8 ReadAlternateStatus() const { return m_status; }
  void WriteDeviceControl(u8 value);

  // Data word for a DMA transfer (DMACK asserted).
  u16 ReadDMA();

private:
  enum class Phase : u8
  {
    Idle,
    PacketCommand,
    DataIn,
    DataOut,
  };

  static void InterruptEventCallback(void* param, TickCount ticks, TickCount ticks_late);

  void SetIRQ(bool asserted);
  void RaiseIRQ();
  void SetDMARequest(bool asserted);

  void ExecuteATACommand(u8 command);
  void ExecutePacket();
  void Complete();
  void Fail(u8 sense_key, u8 asc, u8 ascq = 0);
  void BeginDataIn(std::vector<u8> data, size_t allocation_length);
  void StartDataInBlock();
  void BeginDataOut(size_t length);
  u16 ReadDataWord();
  void WriteDataWord(u16 value);

  void CommandInquiry();
  void CommandModeSense(bool ten_byte);
  void CommandReadCapacity();
  void CommandRead(u32 lba, u32 count);
  void CommandReadCD();
  void CommandReadTOC();
  void CommandReadSubChannel();
  std::vector<u8> BuildIdentifyData() const;

  Identity m_identity;
  Callbacks m_callbacks;
  std::unique_ptr<CDImage> m_media;
  std::unique_ptr<TimingEvent> m_interrupt_event;

  Phase m_phase = Phase::Idle;
  u8 m_error = 0;
  u8 m_features = 0;
  u8 m_sector_count = 0;
  u8 m_lba_low = 0;
  u8 m_lba_mid = 0;
  u8 m_lba_high = 0;
  u8 m_device = 0;
  u8 m_status = 0;
  u8 m_device_control = 0;
  bool m_irq = false;
  bool m_dma = false;
  bool m_dma_request = false;

  std::array<u8, 12> m_packet{};
  u32 m_packet_size = 0;

  std::vector<u8> m_data;
  size_t m_data_offset = 0;
  size_t m_block_end = 0;
  u32 m_byte_count_limit = 0;

  bool m_unit_attention = true;
  u8 m_sense_key = 0;
  u8 m_sense_asc = 0;
  u8 m_sense_ascq = 0;
};

} // namespace Arcade
