// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/storage/atapi_cdrom.h"
#include "util/state_wrapper.h"

#include "core/system.h"
#include "core/timing_event.h"

#include "util/cd_image.h"

#include "common/error.h"
#include "common/log.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

Log_SetChannel(ATAPICDROM);

namespace Arcade {
namespace {
constexpr u8 STATUS_ERR = 0x01;
constexpr u8 STATUS_DRQ = 0x08;
constexpr u8 STATUS_DSC = 0x10;
constexpr u8 STATUS_DRDY = 0x40;

constexpr u8 ERROR_ABRT = 0x04;

// Interrupt reason (sector count register) during packet commands.
constexpr u8 REASON_COD = 0x01;
constexpr u8 REASON_IO = 0x02;

constexpr u8 CONTROL_NIEN = 0x02;
constexpr u8 CONTROL_SRST = 0x04;

constexpr u8 FEATURE_DMA = 0x01;

constexpr u8 SENSE_NOT_READY = 0x02;
constexpr u8 SENSE_MEDIUM_ERROR = 0x03;
constexpr u8 SENSE_ILLEGAL_REQUEST = 0x05;
constexpr u8 SENSE_UNIT_ATTENTION = 0x06;

// Delay between a drive phase change and INTRQ. Hosts acknowledge the board
// interrupt after starting a transfer and then wait for the drive's interrupt.
constexpr u32 INTERRUPT_DELAY_US = 100;

// MMC logical block 0 is MSF 00:02:00.
constexpr CDImage::LBA LBA_OFFSET = 150;

void PutBE16(u8* p, u32 v)
{
  p[0] = static_cast<u8>(v >> 8);
  p[1] = static_cast<u8>(v);
}

void PutBE32(u8* p, u32 v)
{
  p[0] = static_cast<u8>(v >> 24);
  p[1] = static_cast<u8>(v >> 16);
  p[2] = static_cast<u8>(v >> 8);
  p[3] = static_cast<u8>(v);
}

u32 GetBE16(const u8* p)
{
  return (static_cast<u32>(p[0]) << 8) | p[1];
}

u32 GetBE32(const u8* p)
{
  return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
}

void PutAddress(u8* p, CDImage::LBA absolute, bool msf)
{
  if (msf)
  {
    p[0] = 0;
    p[1] = static_cast<u8>(absolute / (60 * 75));
    p[2] = static_cast<u8>((absolute / 75) % 60);
    p[3] = static_cast<u8>(absolute % 75);
  }
  else
  {
    PutBE32(p, static_cast<u32>(absolute - LBA_OFFSET));
  }
}

void PutPadded(u8* p, std::string_view s, size_t length)
{
  for (size_t i = 0; i < length; i++)
    p[i] = (i < s.size()) ? static_cast<u8>(s[i]) : ' ';
}

bool IsDataTrack(const CDImage::Track& track)
{
  return track.mode != CDImage::TrackMode::Audio;
}
} // namespace

ATAPICDROM::ATAPICDROM(const Identity& identity) : m_identity(identity)
{
  const TickCount delay = std::max<TickCount>(
    1, static_cast<TickCount>(static_cast<u64>(System::GetTicksPerSecond()) * INTERRUPT_DELAY_US / 1000000));
  m_interrupt_event =
    std::make_unique<TimingEvent>("ATAPI CD-ROM interrupt", delay, delay, InterruptEventCallback, this);
  Reset();
}

ATAPICDROM::~ATAPICDROM()
{
  m_interrupt_event->Deactivate();
}

void ATAPICDROM::InterruptEventCallback(void* param, TickCount, TickCount)
{
  ATAPICDROM* self = static_cast<ATAPICDROM*>(param);
  self->m_interrupt_event->Deactivate();
  self->SetIRQ(true);
}

void ATAPICDROM::RaiseIRQ()
{
  m_interrupt_event->Deactivate();
  m_interrupt_event->Schedule(m_interrupt_event->GetPeriod());
}

bool ATAPICDROM::OpenMedia(const char* path, Error* error)
{
  m_media = CDImage::Open(path, false, error);
  m_unit_attention = true;
  return static_cast<bool>(m_media);
}

void ATAPICDROM::Reset()
{
  m_phase = Phase::Idle;
  m_error = 0x01;
  m_features = 0;
  // ATAPI device signature.
  m_sector_count = 0x01;
  m_lba_low = 0x01;
  m_lba_mid = 0x14;
  m_lba_high = 0xEB;
  m_device = 0;
  m_status = STATUS_DRDY | STATUS_DSC;
  m_dma = false;
  m_packet_size = 0;
  m_data.clear();
  m_data_offset = 0;
  m_block_end = 0;
  m_unit_attention = true;
  m_sense_key = m_sense_asc = m_sense_ascq = 0;
  m_interrupt_event->Deactivate();
  SetDMARequest(false);
  SetIRQ(false);
}

void ATAPICDROM::SetIRQ(bool asserted)
{
  const bool line = asserted && !(m_device_control & CONTROL_NIEN);
  m_irq = asserted;
  if (m_callbacks.irq)
    m_callbacks.irq(m_callbacks.context, line);
}

void ATAPICDROM::SetDMARequest(bool asserted)
{
  if (m_dma_request == asserted)
    return;
  m_dma_request = asserted;
  if (m_callbacks.dma_request)
    m_callbacks.dma_request(m_callbacks.context, asserted);
}

void ATAPICDROM::WriteDeviceControl(u8 value)
{
  const bool reset = (value & CONTROL_SRST) && !(m_device_control & CONTROL_SRST);
  m_device_control = value;
  if (reset)
    Reset();
  else if (m_callbacks.irq)
    m_callbacks.irq(m_callbacks.context, m_irq && !(m_device_control & CONTROL_NIEN));
}

u16 ATAPICDROM::ReadCommandBlock(u32 reg)
{
  switch (reg & 7)
  {
    case 0: return ReadDataWord();
    case 1: return m_error;
    case 2: return m_sector_count;
    case 3: return m_lba_low;
    case 4: return m_lba_mid;
    case 5: return m_lba_high;
    case 6: return m_device;
    default:
      // Reading the status register acknowledges INTRQ.
      if (m_irq)
        SetIRQ(false);
      return m_status;
  }
}

void ATAPICDROM::WriteCommandBlock(u32 reg, u16 value)
{
  const u8 data = static_cast<u8>(value);
  switch (reg & 7)
  {
    case 0: WriteDataWord(value); break;
    case 1: m_features = data; break;
    case 2: m_sector_count = data; break;
    case 3: m_lba_low = data; break;
    case 4: m_lba_mid = data; break;
    case 5: m_lba_high = data; break;
    case 6: m_device = data; break;
    default: ExecuteATACommand(data); break;
  }
}

std::vector<u8> ATAPICDROM::BuildIdentifyData() const
{
  std::array<u16, 256> words{};
  // ATAPI, CD-ROM, removable, 12-byte packets.
  words[0] = 0x8580;
  const auto put_string = [&words](u32 first_word, std::string_view s, size_t chars) {
    for (size_t i = 0; i < chars; i += 2)
    {
      const u8 a = (i < s.size()) ? static_cast<u8>(s[i]) : ' ';
      const u8 b = (i + 1 < s.size()) ? static_cast<u8>(s[i + 1]) : ' ';
      words[first_word + i / 2] = static_cast<u16>((a << 8) | b);
    }
  };
  put_string(23, m_identity.revision, 8);
  std::string model(m_identity.vendor);
  model.resize(8, ' ');
  model += m_identity.product;
  put_string(27, model, 40);
  words[49] = 0x0300; // DMA and LBA supported
  words[53] = 0x0006; // words 64-70 and 88 valid
  words[63] = 0x0007; // multiword DMA modes 0-2
  words[64] = 0x0003; // PIO modes 3-4
  words[80] = 0x001E; // ATA/ATAPI-1..4

  std::vector<u8> data(512);
  for (size_t i = 0; i < words.size(); i++)
  {
    data[i * 2] = static_cast<u8>(words[i]);
    data[i * 2 + 1] = static_cast<u8>(words[i] >> 8);
  }
  return data;
}

void ATAPICDROM::ExecuteATACommand(u8 command)
{
  DEV_LOG("ATA command {:02X} features={:02X} byte count={:02X}{:02X}", command, m_features, m_lba_high, m_lba_mid);
  m_error = 0;
  SetDMARequest(false);

  switch (command)
  {
    case 0x08: // DEVICE RESET
      Reset();
      m_status = 0;
      return;

    case 0xA0: // PACKET
      m_dma = (m_features & FEATURE_DMA) != 0;
      m_byte_count_limit = (static_cast<u32>(m_lba_high) << 8) | m_lba_mid;
      if (m_byte_count_limit == 0)
        m_byte_count_limit = 0x10000;
      m_byte_count_limit &= ~1u;
      m_packet.fill(0);
      m_packet_size = 0;
      m_phase = Phase::PacketCommand;
      m_sector_count = REASON_COD;
      m_status = STATUS_DRDY | STATUS_DSC | STATUS_DRQ;
      return;

    case 0xA1: // IDENTIFY PACKET DEVICE
      m_dma = false;
      m_packet_size = 0;
      m_byte_count_limit = 512;
      m_data = BuildIdentifyData();
      m_data_offset = 0;
      m_block_end = m_data.size();
      m_phase = Phase::DataIn;
      m_status = STATUS_DRDY | STATUS_DSC | STATUS_DRQ;
      RaiseIRQ();
      return;

    case 0xEF: // SET FEATURES
    case 0xE0: // STANDBY IMMEDIATE
    case 0xE1: // IDLE IMMEDIATE
    case 0xE2: // STANDBY
    case 0xE3: // IDLE
    case 0xE6: // SLEEP
      m_status = STATUS_DRDY | STATUS_DSC;
      RaiseIRQ();
      return;

    case 0xE5: // CHECK POWER MODE
      m_sector_count = 0xFF;
      m_status = STATUS_DRDY | STATUS_DSC;
      RaiseIRQ();
      return;

    default:
      // IDENTIFY DEVICE and other ATA commands are aborted with the packet
      // device signature.
      if (command != 0xEC)
        WARNING_LOG("ATAPI CD-ROM unsupported ATA command {:02X}", command);
      m_sector_count = 0x01;
      m_lba_low = 0x01;
      m_lba_mid = 0x14;
      m_lba_high = 0xEB;
      m_error = ERROR_ABRT;
      m_status = STATUS_DRDY | STATUS_DSC | STATUS_ERR;
      m_phase = Phase::Idle;
      RaiseIRQ();
      return;
  }
}

void ATAPICDROM::Complete()
{
  m_phase = Phase::Idle;
  m_data.clear();
  m_data_offset = 0;
  m_block_end = 0;
  m_packet_size = 0;
  m_sector_count = REASON_COD | REASON_IO;
  m_status = STATUS_DRDY | STATUS_DSC;
  SetDMARequest(false);
  RaiseIRQ();
}

void ATAPICDROM::Fail(u8 sense_key, u8 asc, u8 ascq)
{
  m_sense_key = sense_key;
  m_sense_asc = asc;
  m_sense_ascq = ascq;
  Complete();
  m_error = static_cast<u8>((sense_key << 4) | ERROR_ABRT);
  m_status |= STATUS_ERR;
}

void ATAPICDROM::BeginDataIn(std::vector<u8> data, size_t allocation_length)
{
  if (data.size() > allocation_length)
    data.resize(allocation_length);
  if (data.empty())
  {
    Complete();
    return;
  }

  m_data = std::move(data);
  m_data_offset = 0;
  m_phase = Phase::DataIn;
  if (m_dma)
  {
    m_block_end = m_data.size();
    m_sector_count = REASON_IO;
    m_status = STATUS_DRDY | STATUS_DSC | STATUS_DRQ;
    SetDMARequest(true);
    return;
  }

  StartDataInBlock();
}

void ATAPICDROM::StartDataInBlock()
{
  const size_t length = std::min<size_t>(m_data.size() - m_data_offset, m_byte_count_limit);
  m_block_end = m_data_offset + length;
  m_sector_count = REASON_IO;
  m_lba_mid = static_cast<u8>(length);
  m_lba_high = static_cast<u8>(length >> 8);
  m_status = STATUS_DRDY | STATUS_DSC | STATUS_DRQ;
  RaiseIRQ();
}

void ATAPICDROM::BeginDataOut(size_t length)
{
  if (length == 0)
  {
    Complete();
    return;
  }

  m_data.assign(length, 0);
  m_data_offset = 0;
  m_block_end = length;
  m_phase = Phase::DataOut;
  m_sector_count = 0;
  m_lba_mid = static_cast<u8>(length);
  m_lba_high = static_cast<u8>(length >> 8);
  m_status = STATUS_DRDY | STATUS_DSC | STATUS_DRQ;
  RaiseIRQ();
}

u16 ATAPICDROM::ReadDataWord()
{
  if (m_phase != Phase::DataIn || m_data_offset >= m_block_end)
    return 0xFFFF;

  const u8 low = m_data[m_data_offset++];
  const u8 high = (m_data_offset < m_block_end) ? m_data[m_data_offset++] : 0;
  if (m_data_offset >= m_block_end)
  {
    if (m_data_offset >= m_data.size())
    {
      // IDENTIFY PACKET DEVICE ends without a status interrupt.
      if (m_packet_size == 0)
      {
        m_phase = Phase::Idle;
        m_status = STATUS_DRDY | STATUS_DSC;
        m_data.clear();
      }
      else
      {
        Complete();
      }
    }
    else
    {
      StartDataInBlock();
    }
  }
  return static_cast<u16>(low | (high << 8));
}

u16 ATAPICDROM::ReadDMA()
{
  if (m_phase != Phase::DataIn || !m_dma)
    return 0xFFFF;
  return ReadDataWord();
}

void ATAPICDROM::WriteDataWord(u16 value)
{
  if (m_phase == Phase::PacketCommand)
  {
    // The first packet byte is on D7-D0.
    m_packet[m_packet_size++] = static_cast<u8>(value);
    m_packet[m_packet_size++] = static_cast<u8>(value >> 8);
    if (m_packet_size >= m_packet.size())
    {
      m_status = STATUS_DRDY | STATUS_DSC;
      ExecutePacket();
    }
    return;
  }

  if (m_phase == Phase::DataOut && m_data_offset < m_block_end)
  {
    m_data[m_data_offset++] = static_cast<u8>(value);
    if (m_data_offset < m_block_end)
      m_data[m_data_offset++] = static_cast<u8>(value >> 8);
    // Parameter data (MODE SELECT) is accepted without effect.
    if (m_data_offset >= m_block_end)
      Complete();
  }
}

void ATAPICDROM::ExecutePacket()
{
  const u8 opcode = m_packet[0];
  DEV_LOG("ATAPI packet {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}{}", m_packet[0],
          m_packet[1], m_packet[2], m_packet[3], m_packet[4], m_packet[5], m_packet[6], m_packet[7], m_packet[8],
          m_packet[9], m_packet[10], m_packet[11], m_dma ? " (DMA)" : "");

  // Commands that report status without media or unit attention.
  if (opcode == 0x03)
  {
    std::vector<u8> sense(18);
    sense[0] = 0x70;
    sense[2] = m_sense_key;
    sense[7] = 10;
    sense[12] = m_sense_asc;
    sense[13] = m_sense_ascq;
    m_sense_key = m_sense_asc = m_sense_ascq = 0;
    BeginDataIn(std::move(sense), m_packet[4]);
    return;
  }
  if (opcode == 0x12)
  {
    CommandInquiry();
    return;
  }

  if (!m_media)
  {
    Fail(SENSE_NOT_READY, 0x3A);
    return;
  }

  if (m_unit_attention && opcode == 0x00)
  {
    m_unit_attention = false;
    Fail(SENSE_UNIT_ATTENTION, 0x28);
    return;
  }

  switch (opcode)
  {
    case 0x00: // TEST UNIT READY
    case 0x1B: // START STOP UNIT
    case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL
    case 0x2B: // SEEK(10)
    case 0x45: // PLAY AUDIO(10)
    case 0x47: // PLAY AUDIO MSF
    case 0x4B: // PAUSE/RESUME
    case 0x4E: // STOP PLAY/SCAN
    case 0xA5: // PLAY AUDIO(12)
    case 0xBB: // SET CD SPEED
      Complete();
      return;

    case 0x15: // MODE SELECT(6)
      BeginDataOut(m_packet[4]);
      return;

    case 0x55: // MODE SELECT(10)
      BeginDataOut(GetBE16(&m_packet[7]));
      return;

    case 0x1A:
      CommandModeSense(false);
      return;

    case 0x5A:
      CommandModeSense(true);
      return;

    case 0x25:
      CommandReadCapacity();
      return;

    case 0x28: // READ(10)
      CommandRead(GetBE32(&m_packet[2]), GetBE16(&m_packet[7]));
      return;

    case 0xA8: // READ(12)
      CommandRead(GetBE32(&m_packet[2]), GetBE32(&m_packet[6]));
      return;

    case 0xBE:
      CommandReadCD();
      return;

    case 0x43:
      CommandReadTOC();
      return;

    case 0x42:
      CommandReadSubChannel();
      return;

    default:
      WARNING_LOG("ATAPI CD-ROM unsupported packet {:02X}", opcode);
      Fail(SENSE_ILLEGAL_REQUEST, 0x20);
      return;
  }
}

void ATAPICDROM::CommandInquiry()
{
  std::vector<u8> data(96);
  data[0] = 0x05; // CD-ROM
  data[1] = 0x80; // removable
  data[3] = 0x21; // ATAPI, response format 1
  data[4] = static_cast<u8>(data.size() - 5);
  PutPadded(&data[8], m_identity.vendor, 8);
  PutPadded(&data[16], m_identity.product, 16);
  PutPadded(&data[32], m_identity.revision, 4);
  BeginDataIn(std::move(data), m_packet[4]);
}

void ATAPICDROM::CommandModeSense(bool ten_byte)
{
  const u8 page = m_packet[2] & 0x3F;
  const size_t header = ten_byte ? 8 : 4;
  std::vector<u8> data(header);

  const auto add_page = [&data](std::initializer_list<u8> bytes) { data.insert(data.end(), bytes); };
  if (page == 0x01 || page == 0x3F)
    add_page({0x01, 0x06, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00});
  if (page == 0x0E || page == 0x3F)
    add_page({0x0E, 0x0E, 0x04, 0x00, 0x00, 0x00, 0x00, 0x4B, 0x01, 0xFF, 0x02, 0xFF, 0x00, 0x00, 0x00, 0x00});
  if (page == 0x2A || page == 0x3F)
  {
    // Capabilities: CD-DA, audio play, maximum speed 8x (1412 KB/s), current 8x.
    add_page({0x2A, 0x12, 0x00, 0x00, 0x71, 0x60, 0x29, 0x00, 0x05, 0x84, 0x00, 0x10, 0x00, 0x80, 0x05, 0x84, 0x00,
              0x00, 0x00, 0x00});
  }
  if (data.size() == header)
  {
    Fail(SENSE_ILLEGAL_REQUEST, 0x24);
    return;
  }

  data[ten_byte ? 2 : 1] = 0x01; // medium type: 120 mm data disc
  if (ten_byte)
    PutBE16(&data[0], static_cast<u32>(data.size() - 2));
  else
    data[0] = static_cast<u8>(data.size() - 1);

  BeginDataIn(std::move(data), ten_byte ? GetBE16(&m_packet[7]) : m_packet[4]);
}

void ATAPICDROM::CommandReadCapacity()
{
  std::vector<u8> data(8);
  PutBE32(&data[0], static_cast<u32>(m_media->GetLBACount() - LBA_OFFSET - 1));
  PutBE32(&data[4], CDImage::DATA_SECTOR_SIZE);
  BeginDataIn(std::move(data), SIZE_MAX);
}

// Reads sectors as complete 2352-byte CD sectors. Images store cooked tracks
// (Mode 1 or Mode 2 without sync/header) with the stored data first, so the
// sync and header are rebuilt from the track mode; EDC/ECC stay zero.
bool ATAPICDROM::ReadRawSectors(u32 lba, u32 count, std::vector<u8>* raw)
{
  const CDImage::LBA start = static_cast<CDImage::LBA>(lba) + LBA_OFFSET;
  if (start + count > m_media->GetLBACount() || !m_media->Seek(start))
    return false;

  raw->assign(static_cast<size_t>(count) * CDImage::RAW_SECTOR_SIZE, 0);
  for (u32 i = 0; i < count; i++)
  {
    u8* sector = raw->data() + static_cast<size_t>(i) * CDImage::RAW_SECTOR_SIZE;
    const CDImage::TrackMode mode = m_media->GetTrackMode(static_cast<u8>(m_media->GetTrackNumber()));
    u8 stored[CDImage::RAW_SECTOR_SIZE];
    if (m_media->Read(CDImage::ReadMode::RawSector, 1, stored) != 1)
      return false;

    size_t data_offset;
    u8 sector_mode;
    switch (mode)
    {
      case CDImage::TrackMode::Audio:
      case CDImage::TrackMode::Mode1Raw:
      case CDImage::TrackMode::Mode2Raw:
        std::memcpy(sector, stored, CDImage::RAW_SECTOR_SIZE);
        continue;

      case CDImage::TrackMode::Mode1:
        data_offset = 16;
        sector_mode = 1;
        break;

      case CDImage::TrackMode::Mode2Form1:
        // Subheader stays zero: form 1.
        data_offset = 24;
        sector_mode = 2;
        break;

      default:
        // Mode 2 images stored from the subheader on.
        data_offset = 16;
        sector_mode = 2;
        break;
    }

    std::memcpy(sector + data_offset, stored, CDImage::RAW_SECTOR_SIZE - data_offset);
    std::memset(sector + 1, 0xFF, 10);
    const CDImage::LBA absolute = start + static_cast<CDImage::LBA>(i);
    const auto bcd = [](u32 v) { return static_cast<u8>(((v / 10) << 4) | (v % 10)); };
    sector[12] = bcd(absolute / (60 * 75));
    sector[13] = bcd((absolute / 75) % 60);
    sector[14] = bcd(absolute % 75);
    sector[15] = sector_mode;
  }
  return true;
}

void ATAPICDROM::CommandRead(u32 lba, u32 count)
{
  if (count == 0)
  {
    Complete();
    return;
  }

  if (static_cast<CDImage::LBA>(lba) + LBA_OFFSET + count > m_media->GetLBACount())
  {
    Fail(SENSE_ILLEGAL_REQUEST, 0x21);
    return;
  }

  std::vector<u8> raw;
  if (!ReadRawSectors(lba, count, &raw))
  {
    Fail(SENSE_MEDIUM_ERROR, 0x11); // unrecovered read error
    return;
  }

  std::vector<u8> data(static_cast<size_t>(count) * CDImage::DATA_SECTOR_SIZE);
  for (u32 i = 0; i < count; i++)
  {
    const u8* sector = raw.data() + static_cast<size_t>(i) * CDImage::RAW_SECTOR_SIZE;
    const size_t offset = (sector[15] == 2) ? 24 : 16;
    std::memcpy(data.data() + static_cast<size_t>(i) * CDImage::DATA_SECTOR_SIZE, sector + offset,
                CDImage::DATA_SECTOR_SIZE);
  }

  BeginDataIn(std::move(data), SIZE_MAX);
}

void ATAPICDROM::CommandReadCD()
{
  const u32 lba = GetBE32(&m_packet[2]);
  const u32 count = (static_cast<u32>(m_packet[6]) << 16) | GetBE16(&m_packet[7]);
  const u8 fields = m_packet[9];
  if (count == 0)
  {
    Complete();
    return;
  }

  if (static_cast<CDImage::LBA>(lba) + LBA_OFFSET + count > m_media->GetLBACount() || m_packet[10] != 0)
  {
    Fail(SENSE_ILLEGAL_REQUEST, 0x21);
    return;
  }

  std::vector<u8> raw;
  if (!ReadRawSectors(lba, count, &raw))
  {
    Fail(SENSE_MEDIUM_ERROR, 0x11); // unrecovered read error
    return;
  }

  std::vector<u8> data;
  data.reserve(raw.size());
  for (u32 i = 0; i < count; i++)
  {
    const u8* sector = raw.data() + static_cast<size_t>(i) * CDImage::RAW_SECTOR_SIZE;
    const u8 mode = sector[15];
    const bool form2 = (mode == 2) && (sector[18] & 0x20);
    size_t user_offset = 16;
    size_t user_size = CDImage::DATA_SECTOR_SIZE;
    if (mode == 2)
    {
      user_offset = 24;
      user_size = form2 ? 2324 : CDImage::DATA_SECTOR_SIZE;
    }

    // Field selection: sync (bit 7), header (bit 5), subheader (bit 6), user
    // data (bit 4), EDC/ECC (bit 3).
    if (fields & 0x80)
      data.insert(data.end(), sector, sector + 12);
    if (fields & 0x20)
      data.insert(data.end(), sector + 12, sector + 16);
    if ((fields & 0x40) && mode == 2)
      data.insert(data.end(), sector + 16, sector + 24);
    if (fields & 0x10)
      data.insert(data.end(), sector + user_offset, sector + user_offset + user_size);
    if (fields & 0x08)
      data.insert(data.end(), sector + user_offset + user_size, sector + CDImage::RAW_SECTOR_SIZE);
  }

  BeginDataIn(std::move(data), SIZE_MAX);
}

void ATAPICDROM::CommandReadTOC()
{
  const bool msf = (m_packet[1] & 0x02) != 0;
  u8 format = m_packet[2] & 0x0F;
  if (format == 0)
    format = static_cast<u8>(m_packet[9] >> 6); // SFF-8020 format field
  const u8 start_track = m_packet[6];
  const u32 track_count = m_media->GetTrackCount();

  std::vector<u8> data(4);
  data[2] = 1;
  data[3] = static_cast<u8>(track_count);

  const auto descriptor = [&](u8 number, u8 control, CDImage::LBA absolute) {
    u8 d[8] = {0, static_cast<u8>(0x10 | control), number, 0};
    PutAddress(&d[4], absolute, msf);
    data.insert(data.end(), d, d + 8);
  };

  if (format == 0)
  {
    for (u32 n = 1; n <= track_count; n++)
    {
      if (n < start_track && start_track != 0xAA)
        continue;
      if (start_track == 0xAA)
        break;
      const CDImage::Track& track = m_media->GetTrack(n);
      descriptor(static_cast<u8>(n), IsDataTrack(track) ? 0x04 : 0x00, track.start_lba);
    }
    descriptor(0xAA, IsDataTrack(m_media->GetTrack(track_count)) ? 0x04 : 0x00, m_media->GetLBACount());
  }
  else if (format == 1)
  {
    // Session information: one session starting at track 1.
    const CDImage::Track& track = m_media->GetTrack(1);
    descriptor(1, IsDataTrack(track) ? 0x04 : 0x00, track.start_lba);
    data[3] = 1;
  }
  else
  {
    Fail(SENSE_ILLEGAL_REQUEST, 0x24);
    return;
  }

  PutBE16(&data[0], static_cast<u32>(data.size() - 2));
  BeginDataIn(std::move(data), GetBE16(&m_packet[7]));
}

void ATAPICDROM::CommandReadSubChannel()
{
  // No audio playback is modelled: report "no current audio status" and the
  // start of track 1.
  const bool msf = (m_packet[1] & 0x02) != 0;
  const bool subq = (m_packet[2] & 0x40) != 0;
  std::vector<u8> data(4);
  data[1] = 0x15;
  if (subq)
  {
    const CDImage::Track& track = m_media->GetTrack(1);
    u8 q[12] = {m_packet[3], static_cast<u8>(0x10 | (IsDataTrack(track) ? 0x04 : 0x00)), 1, 1};
    PutAddress(&q[4], track.start_lba, msf);
    PutAddress(&q[8], LBA_OFFSET, msf);
    data.insert(data.end(), q, q + 12);
  }
  PutBE16(&data[2], static_cast<u32>(data.size() - 4));
  BeginDataIn(std::move(data), GetBE16(&m_packet[7]));
}

bool ATAPICDROM::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("ATAPICDROM"))
    return false;

  sw.Do(&m_phase);
  sw.Do(&m_error);
  sw.Do(&m_features);
  sw.Do(&m_sector_count);
  sw.Do(&m_lba_low);
  sw.Do(&m_lba_mid);
  sw.Do(&m_lba_high);
  sw.Do(&m_device);
  sw.Do(&m_status);
  sw.Do(&m_device_control);
  sw.Do(&m_irq);
  sw.Do(&m_dma);
  sw.Do(&m_dma_request);
  sw.DoArray(m_packet.data(), m_packet.size());
  sw.Do(&m_packet_size);
  u32 data_size = static_cast<u32>(m_data.size());
  sw.Do(&data_size);
  if (sw.IsReading())
  {
    if (sw.HasError() || data_size > 0x1000000)
      return false;
    m_data.resize(data_size);
  }
  sw.DoBytes(m_data.data(), m_data.size());
  u64 data_offset = m_data_offset, block_end = m_block_end;
  sw.Do(&data_offset);
  sw.Do(&block_end);
  sw.Do(&m_byte_count_limit);
  sw.Do(&m_unit_attention);
  sw.Do(&m_sense_key);
  sw.Do(&m_sense_asc);
  sw.Do(&m_sense_ascq);

  bool event_active = m_interrupt_event && m_interrupt_event->IsActive();
  sw.Do(&event_active);
  if (sw.IsReading())
  {
    if (sw.HasError() || m_phase > Phase::DataOut || data_offset > m_data.size() || block_end > m_data.size() ||
        m_packet_size > m_packet.size() || (event_active && !m_interrupt_event))
    {
      return false;
    }
    m_data_offset = static_cast<size_t>(data_offset);
    m_block_end = static_cast<size_t>(block_end);
    // Deadlines are restored by TimingEvents; only activation is established here.
    if (event_active)
      m_interrupt_event->Schedule(1);
    else if (m_interrupt_event)
      m_interrupt_event->Deactivate();
  }

  return !sw.HasError();
}

} // namespace Arcade
