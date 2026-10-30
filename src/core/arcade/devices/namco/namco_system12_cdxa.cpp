// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "namco_system12_cdxa.h"

#include "core/interrupt_controller.h"
#include "core/cpu_core.h"
#include "core/system.h"
#include "common/log.h"
#include "common/timer.h"
#include "util/cd_image.h"
#include "util/state_wrapper.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

Log_SetChannel(NamcoSystem12CDXA);

namespace Arcade::Namco
{
namespace
{
constexpr uint32_t SH2_PSX_INT10_TRIGGER = 0x0041601Cu;
constexpr uint32_t SH2_BOARD_LATCH = 0x00417000u;
constexpr uint32_t SH2_PSX_INT10_STATUS = 0x0041C000u;

constexpr uint32_t SH2_ATA_CS0_BASE = 0x00C00000u;
constexpr uint32_t SH2_ATA_CS0_END = 0x00C1FFFFu;
constexpr uint32_t SH2_ATA_CS1_BASE = 0x00C20000u;
constexpr uint32_t SH2_ATA_CS1_END = 0x00C3FFFFu;
constexpr uint32_t SH2_ATA_REGISTER_STRIDE = 0x00004000u;

constexpr uint8_t ATA_STATUS_ERR = 0x01u;
constexpr uint8_t ATA_STATUS_DRQ = 0x08u;
constexpr uint8_t ATA_STATUS_DSC = 0x10u;
constexpr uint8_t ATA_STATUS_DRDY = 0x40u;

constexpr uint8_t ATA_COMMAND_PACKET = 0xA0u;
constexpr uint8_t ATA_COMMAND_IDENTIFY_PACKET_DEVICE = 0xA1u;
constexpr uint8_t ATA_COMMAND_IDLE_IMMEDIATE = 0xE1u;
constexpr uint8_t ATA_COMMAND_CHECK_POWER_MODE = 0xE5u;

constexpr uint8_t ATAPI_INTERRUPT_COD = 0x01u;
constexpr uint8_t ATAPI_INTERRUPT_IO = 0x02u;

constexpr uint32_t PSX_SH2_RESET = 0x007D6002u;
constexpr uint32_t PSX_CLOCKGEN_BASE = 0x007D6004u;
constexpr uint32_t PSX_CLOCKGEN_END = 0x007D600Bu;
constexpr uint32_t PSX_IDE_SH2_ENABLE = 0x007D6010u;
constexpr uint32_t PSX_IDE_PS1_ENABLE = 0x007D6012u;
constexpr uint32_t PSX_UNKNOWN_6018 = 0x007D6018u;
constexpr uint32_t PSX_SRAM_SELECT = 0x007D601Au;
constexpr uint32_t PSX_INT10_ACK = 0x007D601Eu;
constexpr uint32_t PSX_M148_STATUS = 0x007D800Au;

uint16_t ReadBE16(const uint8_t* data)
{
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

void WriteBE16(uint8_t* data, uint16_t value)
{
  data[0] = static_cast<uint8_t>(value >> 8);
  data[1] = static_cast<uint8_t>(value);
}
} // namespace

System12CDXA::System12CDXA()
{
  m_sh2.SetExternalBus(MakeSH2ExternalBus());
  m_sh2.SetSCI0TransmitCallback(
    [](void* opaque, uint8_t value) { static_cast<System12CDXA*>(opaque)->HandleSCI0Transmit(value); }, this);
  m_sh2.SetPortBWriteCallback(
    [](void* opaque, uint16_t value) { static_cast<System12CDXA*>(opaque)->HandlePortBWrite(value); }, this);
  Reset();
}

System12CDXA::~System12CDXA() = default;

bool System12CDXA::SetMedia(const char* path, Error* error)
{
  if (!path || path[0] == '\0')
  {
    m_media.reset();
    return true;
  }

  std::unique_ptr<CDImage> image = CDImage::Open(path, false, error);
  if (!image)
    return false;

  INFO_LOG("System12 CDXA CD-ROM attached path='{}' tracks={} lbas={}", path, image->GetTrackCount(),
           image->GetLBACount());
  m_media = std::move(image);
  return true;
}

void System12CDXA::Reset()
{
  m_sh2_running = false;
  m_sram_selected = false;
  m_ide_sh2_enabled = false;
  m_ide_ps1_enabled = false;
  m_psx_int10_busy = false;
  InterruptController::SetLineState(InterruptController::IRQ::IRQ10, false);
  m_boot_latch = 0;
  m_unknown_6018 = 0;
  m_sh2_417000_latch = 0;

  m_volume_latch.fill(UINT16_C(0x007F));
  m_volume_gain_q16.fill(UINT32_C(0x00010000));
  m_volume_control = 0;
  m_volume_data = 0;
  m_volume_write_phase = false;

  m_audio_word = 0;
  m_audio_word_byte = 0;
  m_audio_left_channel = true;
  m_audio_pending_left = 0;
  m_audio_muted = false;
  m_audio_queue = {};
  m_audio_queue_read = 0;
  m_audio_queue_write = 0;
  m_audio_queue_count = 0;
  m_audio_host_primed = false;
  m_audio_current = {};
  m_audio_next = {};
  m_audio_resample_phase = 0;
  m_audio_sci_bytes = 0;
  m_audio_native_frames = 0;
  m_audio_nonzero_frames = 0;
  m_audio_host_pops = 0;
  m_audio_peak_abs = 0;
  m_audio_underflows = 0;
  m_audio_reprime_count = 0;

  ResetATAPI();

  m_sh2.ClearMemory();
  m_sh2.Reset(true);
  // Four SCI bytes form one 16-bit stereo sample frame. Current MAME tunes
  // the CDXA clock generator for an exact 37.8 kHz DAC rate.
  m_sh2.SetSCI0TransmitRate(CDXA_SCI_BYTES_PER_SECOND);
}

uint64_t System12CDXA::Run(uint64_t cycles)
{
  if (!m_sh2_running)
    return 0;

  // Keep the IDE DREQ line visible to the SH7014 DMAC while the ATAPI data
  // phase is active. DMA units are now consumed inside SH7014::Step(), so
  // firmware execution, DMA0 audio streaming and DMA1 CD transfers advance
  // in the same emulated clock domain instead of transferring a full sector
  // after the SH slice has already completed.
  m_sh2.SetExternalDMARequestLine(1, m_atapi_phase == ATAPIPhase::PacketDataIn);

  return m_sh2.Run(cycles);
}

void System12CDXA::ResetATAPI()
{
  m_atapi_phase = ATAPIPhase::Idle;
  m_ata_error = 0;
  m_ata_features = 0;
  m_ata_sector_count = 1;
  m_ata_lba_mid = 0x14;
  m_ata_lba_high = 0xEB;
  m_ata_device = 0;
  m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC);
  m_ata_device_control = 0;
  m_atapi_media_change = true;
  m_atapi_sense_key = 0;
  m_atapi_sense_asc = 0;
  m_atapi_sense_ascq = 0;
  m_identify_words.fill(0);
  m_identify_word = 0;
  m_packet.fill(0);
  m_packet_size = 0;
  m_atapi_data.fill(0);
  m_atapi_data_size = 0;
  m_atapi_data_offset = 0;

  // QF checks that the ATAPI model begins with TOSHIB before accepting the drive.
  m_identify_words[0] = UINT16_C(0x8580);
  m_identify_words[49] = UINT16_C(0x0200);
  m_identify_words[53] = UINT16_C(0x0003);
  m_identify_words[63] = UINT16_C(0x0007);

  static constexpr char MODEL[] = "TOSHIBA CD-ROM XM-6402B ";
  for (size_t i = 0; i < sizeof(MODEL) - 1 && i < 40; i += 2)
  {
    const uint8_t a = static_cast<uint8_t>(MODEL[i]);
    const uint8_t b =
      (i + 1 < sizeof(MODEL) - 1) ? static_cast<uint8_t>(MODEL[i + 1]) : UINT8_C(' ');
    m_identify_words[27 + (i / 2)] =
      static_cast<uint16_t>((static_cast<uint16_t>(a) << 8) | b);
  }
  for (size_t i = 27 + ((sizeof(MODEL) - 1 + 1) / 2); i <= 46; i++)
    m_identify_words[i] = UINT16_C(0x2020);
}

uint16_t System12CDXA::ReadATAData() const
{
  System12CDXA* const self = const_cast<System12CDXA*>(this);

  if (m_atapi_phase == ATAPIPhase::IdentifyData)
  {
    if (self->m_identify_word >= m_identify_words.size())
      return UINT16_C(0xFFFF);

    const uint16_t value = m_identify_words[self->m_identify_word++];
    if (self->m_identify_word == m_identify_words.size())
      self->CompleteATAPICommand();
    return value;
  }

  if (m_atapi_phase == ATAPIPhase::PacketDataIn)
  {
    if (self->m_atapi_data_offset >= m_atapi_data_size)
      return UINT16_C(0xFFFF);

    const uint8_t first = m_atapi_data[self->m_atapi_data_offset++];
    const uint8_t second =
      (self->m_atapi_data_offset < m_atapi_data_size) ?
        m_atapi_data[self->m_atapi_data_offset++] :
        UINT8_C(0);
    const uint16_t value =
      static_cast<uint16_t>(first | (static_cast<uint16_t>(second) << 8));

    if (self->m_atapi_data_offset >= m_atapi_data_size)
      self->CompleteATAPICommand();
    return value;
  }

  return UINT16_C(0xFFFF);
}

void System12CDXA::WriteATAData(uint16_t value)
{
  if (m_atapi_phase != ATAPIPhase::PacketCommand)
    return;

  // ATA PIO words carry the first packet byte in D7-D0.
  if (m_packet_size < m_packet.size())
    m_packet[m_packet_size++] = static_cast<uint8_t>(value);
  if (m_packet_size < m_packet.size())
    m_packet[m_packet_size++] = static_cast<uint8_t>(value >> 8);

  if (m_packet_size == m_packet.size())
    ExecutePacketCommand();
}

void System12CDXA::BeginATAPIDataIn(const uint8_t* data, size_t size)
{
  m_atapi_data_size = std::min(size, m_atapi_data.size());

  m_atapi_data_offset = 0;
  if (m_atapi_data_size != 0)
    std::memcpy(m_atapi_data.data(), data, m_atapi_data_size);

  m_atapi_phase = ATAPIPhase::PacketDataIn;
  m_sh2.SetExternalDMARequestLine(1, true);
  m_ata_sector_count = ATAPI_INTERRUPT_IO;
  m_ata_lba_mid = static_cast<uint8_t>(m_atapi_data_size);
  m_ata_lba_high = static_cast<uint8_t>(m_atapi_data_size >> 8);
  m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ);
}

void System12CDXA::CompleteATAPICommand()
{
  m_atapi_phase = ATAPIPhase::Idle;
  m_sh2.SetExternalDMARequestLine(1, false);
  m_packet_size = 0;
  m_atapi_data_size = 0;
  m_atapi_data_offset = 0;
  m_ata_sector_count = static_cast<uint8_t>(ATAPI_INTERRUPT_COD | ATAPI_INTERRUPT_IO);
  m_ata_lba_mid = 0;
  m_ata_lba_high = 0;
  m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC);
}

void System12CDXA::ExecutePacketCommand()
{
  switch (m_packet[0])
  {
    case 0x00: // TEST UNIT READY
      // MAME's ATAPI CD-ROM device reports UNIT ATTENTION once after media
      // insertion/reset. namcos12 is explicitly one of the users of this path.
      if (m_atapi_media_change)
      {
        m_atapi_media_change = false;
        m_atapi_sense_key = UINT8_C(0x06); // UNIT ATTENTION
        m_atapi_sense_asc = UINT8_C(0x28); // NOT READY TO READY / MEDIUM CHANGED
        m_atapi_sense_ascq = UINT8_C(0x00);
        m_atapi_phase = ATAPIPhase::Idle;
        m_sh2.SetExternalDMARequestLine(1, false);
        m_packet_size = 0;
        m_atapi_data_size = 0;
        m_atapi_data_offset = 0;
        m_ata_error = static_cast<uint8_t>(m_atapi_sense_key << 4);
        m_ata_sector_count = static_cast<uint8_t>(ATAPI_INTERRUPT_COD | ATAPI_INTERRUPT_IO);
        m_ata_lba_mid = 0;
        m_ata_lba_high = 0;
        m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_ERR);
        return;
      }
      CompleteATAPICommand();
      return;

    case 0x1B: // START STOP UNIT
    case 0x1E: // PREVENT/ALLOW MEDIUM REMOVAL
    case 0x2B: // SEEK(10)
    case 0x4B: // PAUSE/RESUME
    case 0xBB: // SET CD SPEED
      // The Toshiba XM-6402B accepts SET CD SPEED during Truck Kyosokyoku
      // startup. MAME acknowledges the command but does not model a resulting
      // transfer-rate change, so complete it successfully here as well.
      CompleteATAPICommand();
      return;

    case 0x03: // REQUEST SENSE
    {
      std::array<uint8_t, 18> response{};
      response[0] = UINT8_C(0x70);
      response[2] = static_cast<uint8_t>(m_atapi_sense_key & UINT8_C(0x0F));
      response[7] = UINT8_C(0x0A);
      response[12] = m_atapi_sense_asc;
      response[13] = m_atapi_sense_ascq;

      const size_t allocation_length = (m_packet[4] == 0) ? size_t{4} : std::min<size_t>(m_packet[4], response.size());

      // MAME clears the stored sense after REQUEST SENSE data is prepared.
      m_atapi_sense_key = 0;
      m_atapi_sense_asc = 0;
      m_atapi_sense_ascq = 0;
      m_ata_error = 0;

      BeginATAPIDataIn(response.data(), allocation_length);
      return;
    }

    case 0x43: // READ TOC/PMA/ATIP
    {
      if (!m_media)
      {
        WARNING_LOG("System12 CDXA ATAPI READ-TOC without attached media");
        m_atapi_phase = ATAPIPhase::Idle;
        m_packet_size = 0;
        m_ata_error = UINT8_C(0x04);
        m_ata_sector_count = static_cast<uint8_t>(ATAPI_INTERRUPT_COD | ATAPI_INTERRUPT_IO);
        m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_ERR);
        return;
      }

      const bool msf = (m_packet[1] & UINT8_C(0x02)) != 0;
      const uint8_t format = static_cast<uint8_t>(m_packet[2] & UINT8_C(0x0F));
      const uint8_t start_track = m_packet[6];
      const size_t allocation_length =
        static_cast<size_t>((static_cast<uint16_t>(m_packet[7]) << 8) | m_packet[8]);

      // QF currently has a single MODE2 data track. Format 0 is the standard TOC
      // response: first/last track plus track 1 and lead-out (AA) descriptors.
      if (format != 0 || (start_track != 0 && start_track != 1 && start_track != UINT8_C(0xAA)))
      {
        WARNING_LOG("System12 CDXA ATAPI READ-TOC unsupported format={} start_track={:02X} msf={}", format, start_track, msf);
        m_atapi_phase = ATAPIPhase::Idle;
        m_packet_size = 0;
        m_ata_error = UINT8_C(0x04);
        m_ata_sector_count = static_cast<uint8_t>(ATAPI_INTERRUPT_COD | ATAPI_INTERRUPT_IO);
        m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_ERR);
        return;
      }

      std::array<uint8_t, 20> response{};
      size_t response_size = 4;
      response[2] = 1;
      response[3] = 1;

      const auto append_descriptor = [&](uint8_t track, uint32_t lba) {
        const size_t offset = response_size;
        response[offset + 1] = UINT8_C(0x14); // ADR=1, CONTROL=4 (data track)
        response[offset + 2] = track;

        if (msf)
        {
          const uint32_t absolute_frame = lba + 150;
          response[offset + 4] = 0;
          response[offset + 5] = static_cast<uint8_t>(absolute_frame / (60 * 75));
          response[offset + 6] = static_cast<uint8_t>((absolute_frame / 75) % 60);
          response[offset + 7] = static_cast<uint8_t>(absolute_frame % 75);
        }
        else
        {
          response[offset + 4] = static_cast<uint8_t>(lba >> 24);
          response[offset + 5] = static_cast<uint8_t>(lba >> 16);
          response[offset + 6] = static_cast<uint8_t>(lba >> 8);
          response[offset + 7] = static_cast<uint8_t>(lba);
        }

        response_size += 8;
      };

      if (start_track != UINT8_C(0xAA))
        append_descriptor(1, 0);
      append_descriptor(UINT8_C(0xAA), static_cast<uint32_t>(m_media->GetLBACount()));

      const uint16_t toc_data_length = static_cast<uint16_t>(response_size - 2);
      response[0] = static_cast<uint8_t>(toc_data_length >> 8);
      response[1] = static_cast<uint8_t>(toc_data_length);

      const size_t transfer_size = std::min(response_size, allocation_length);
      BeginATAPIDataIn(response.data(), transfer_size);
      return;
    }

    case 0xBE: // READ CD
    {
      const uint32_t lba = (static_cast<uint32_t>(m_packet[2]) << 24) |
                           (static_cast<uint32_t>(m_packet[3]) << 16) |
                           (static_cast<uint32_t>(m_packet[4]) << 8) | m_packet[5];
      const uint32_t sector_count = (static_cast<uint32_t>(m_packet[6]) << 16) |
                                    (static_cast<uint32_t>(m_packet[7]) << 8) | m_packet[8];
      const uint8_t sector_fields = m_packet[9];
      const uint8_t subchannel = m_packet[10];

      if (!m_media || subchannel != 0 || sector_count == 0)
      {
        WARNING_LOG(
          "System12 CDXA ATAPI READ-CD unsupported/media-missing lba={} sectors={} fields={:02X} sub={:02X} media={}",
          lba, sector_count, sector_fields, subchannel, static_cast<bool>(m_media));
        m_atapi_phase = ATAPIPhase::Idle;
        m_packet_size = 0;
        m_ata_error = UINT8_C(0x04);
        m_ata_sector_count = static_cast<uint8_t>(ATAPI_INTERRUPT_COD | ATAPI_INTERRUPT_IO);
        m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_ERR);
        return;
      }

      std::vector<uint8_t> response;
      if (sector_fields == UINT8_C(0x10))
      {
        const size_t response_size = static_cast<size_t>(sector_count) * CDImage::DATA_SECTOR_SIZE;
        if (response_size > m_atapi_data.size())
        {
          WARNING_LOG("System12 CDXA ATAPI READ-CD transfer too large lba={} sectors={} bytes={} fields={:02X}",
                      lba, sector_count, response_size, sector_fields);
          m_atapi_phase = ATAPIPhase::Idle;
          m_packet_size = 0;
          m_ata_error = UINT8_C(0x04);
          m_ata_sector_count = static_cast<uint8_t>(ATAPI_INTERRUPT_COD | ATAPI_INTERRUPT_IO);
          m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_ERR);
          return;
        }

        response.resize(response_size);
        const CDImage::LBA track1_start = m_media->GetTrack(1).start_lba;
        const CDImage::LBA seek_lba = track1_start + static_cast<CDImage::LBA>(lba);
        if (!m_media->Seek(seek_lba) ||
            m_media->Read(CDImage::ReadMode::DataOnly, sector_count, response.data()) != sector_count)
        {
          WARNING_LOG("System12 CDXA ATAPI READ-CD failed lba={} sectors={} fields={:02X}", lba, sector_count, sector_fields);
          m_atapi_phase = ATAPIPhase::Idle;
          m_packet_size = 0;
          m_ata_error = UINT8_C(0x04);
          m_ata_sector_count = static_cast<uint8_t>(ATAPI_INTERRUPT_COD | ATAPI_INTERRUPT_IO);
          m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_ERR);
          return;
        }

      }
      else if (sector_fields == UINT8_C(0x50))
      {
        // MMC READ CD field 0x50 requests the Mode 2 subheader plus user data.
        // Mode 2 Form 1 sectors contain 2048 user bytes; Form 2 sectors contain
        // 2328. The sector form is selected by submode bit 5 in the XA
        // subheader, matching MAME's T10/MMC READ CD handling.
        constexpr size_t MODE2_SUBHEADER_OFFSET = 16;
        constexpr size_t MODE2_SUBHEADER_SIZE = 8;
        constexpr size_t MODE2_USER_OFFSET = MODE2_SUBHEADER_OFFSET + MODE2_SUBHEADER_SIZE;
        constexpr size_t MODE2_FORM1_USER_SIZE = CDImage::DATA_SECTOR_SIZE;
        constexpr size_t MODE2_FORM2_USER_SIZE = 2328;
        constexpr uint8_t MODE2_SUBMODE_FORM2 = UINT8_C(0x20);

        // The largest possible 0x50 response is one Form 2 payload per sector.
        const size_t max_response_size =
          static_cast<size_t>(sector_count) * (MODE2_SUBHEADER_SIZE + MODE2_FORM2_USER_SIZE);
        if (max_response_size > m_atapi_data.size())
        {
          WARNING_LOG("System12 CDXA ATAPI READ-CD transfer too large lba={} sectors={} max_bytes={} fields={:02X}",
                      lba, sector_count, max_response_size, sector_fields);
          m_atapi_phase = ATAPIPhase::Idle;
          m_packet_size = 0;
          m_ata_error = UINT8_C(0x04);
          m_ata_sector_count = static_cast<uint8_t>(ATAPI_INTERRUPT_COD | ATAPI_INTERRUPT_IO);
          m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_ERR);
          return;
        }

        std::vector<uint8_t> raw(static_cast<size_t>(sector_count) * CDImage::RAW_SECTOR_SIZE);
        if (!m_media->Seek(m_media->GetTrack(1).start_lba + static_cast<CDImage::LBA>(lba)) ||
            m_media->Read(CDImage::ReadMode::RawSector, sector_count, raw.data()) != sector_count)
        {
          WARNING_LOG("System12 CDXA ATAPI READ-CD raw read failed lba={} sectors={} fields={:02X}", lba, sector_count,
                      sector_fields);
          m_atapi_phase = ATAPIPhase::Idle;
          m_packet_size = 0;
          m_ata_error = UINT8_C(0x04);
          m_ata_sector_count = static_cast<uint8_t>(ATAPI_INTERRUPT_COD | ATAPI_INTERRUPT_IO);
          m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_ERR);
          return;
        }

        response.clear();
        response.reserve(max_response_size);
        for (uint32_t i = 0; i < sector_count; i++)
        {
          const uint8_t* const raw_sector = raw.data() + (static_cast<size_t>(i) * CDImage::RAW_SECTOR_SIZE);
          const bool form2 = (raw_sector[UINT32_C(0x12)] & MODE2_SUBMODE_FORM2) != 0;
          const size_t user_size = form2 ? MODE2_FORM2_USER_SIZE : MODE2_FORM1_USER_SIZE;

          response.insert(response.end(), raw_sector + MODE2_SUBHEADER_OFFSET,
                          raw_sector + MODE2_SUBHEADER_OFFSET + MODE2_SUBHEADER_SIZE);
          response.insert(response.end(), raw_sector + MODE2_USER_OFFSET,
                          raw_sector + MODE2_USER_OFFSET + user_size);
        }
      }
      else
      {
        WARNING_LOG("System12 CDXA ATAPI READ-CD unsupported fields lba={} sectors={} fields={:02X} sub={:02X}",
                    lba, sector_count, sector_fields, subchannel);
        m_atapi_phase = ATAPIPhase::Idle;
        m_packet_size = 0;
        m_ata_error = UINT8_C(0x04);
        m_ata_sector_count = static_cast<uint8_t>(ATAPI_INTERRUPT_COD | ATAPI_INTERRUPT_IO);
        m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_ERR);
        return;
      }

      BeginATAPIDataIn(response.data(), response.size());
      return;
    }

    default:
      WARNING_LOG("System12 CDXA ATAPI unsupported packet opcode={:02X}", m_packet[0]);
      m_atapi_phase = ATAPIPhase::Idle;
      m_packet_size = 0;
      m_ata_error = UINT8_C(0x04);
      m_ata_sector_count = static_cast<uint8_t>(ATAPI_INTERRUPT_COD | ATAPI_INTERRUPT_IO);
      m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_ERR);
      return;
  }
}

void System12CDXA::ExecuteATACommand(uint8_t command)
{
  m_ata_error = 0;

  switch (command)
  {
    case ATA_COMMAND_IDENTIFY_PACKET_DEVICE:
      m_identify_word = 0;
      m_atapi_phase = ATAPIPhase::IdentifyData;
      m_ata_sector_count = ATAPI_INTERRUPT_IO;
      m_ata_lba_mid = 0;
      m_ata_lba_high = 2;
      m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ);
      return;

    case ATA_COMMAND_PACKET:
      m_packet.fill(0);
      m_packet_size = 0;
      m_atapi_phase = ATAPIPhase::PacketCommand;
      m_ata_sector_count = ATAPI_INTERRUPT_COD;
      m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ);
      return;

    case ATA_COMMAND_IDLE_IMMEDIATE:
      m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC);
      return;

    case ATA_COMMAND_CHECK_POWER_MODE:
      m_ata_sector_count = UINT8_C(0xFF);
      m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC);
      return;

    default:
      WARNING_LOG("System12 CDXA ATA unsupported command={:02X}", command);
      m_atapi_phase = ATAPIPhase::Idle;
      m_ata_error = UINT8_C(0x04);
      m_ata_status = static_cast<uint8_t>(ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_ERR);
      return;
  }
}

uint16_t System12CDXA::ReadATACS0(uint32_t address) const
{
  if (address < SH2_ATA_CS0_BASE || address > SH2_ATA_CS0_END)
    return UINT16_C(0xFFFF);

  const uint32_t reg = (address - SH2_ATA_CS0_BASE) / SH2_ATA_REGISTER_STRIDE;
  switch (reg)
  {
    case 0: return ReadATAData();
    case 1: return m_ata_error;
    case 2: return m_ata_sector_count;
    case 3: return 0;
    case 4: return m_ata_lba_mid;
    case 5: return m_ata_lba_high;
    case 6: return m_ata_device;
    case 7: return m_ata_status;
    default: return UINT16_C(0xFFFF);
  }
}

void System12CDXA::WriteATACS0(uint32_t address, uint16_t value)
{
  if (address < SH2_ATA_CS0_BASE || address > SH2_ATA_CS0_END)
    return;

  const uint32_t reg = (address - SH2_ATA_CS0_BASE) / SH2_ATA_REGISTER_STRIDE;
  const uint8_t data = static_cast<uint8_t>(value);
  switch (reg)
  {
    case 0: WriteATAData(value); break;
    case 1: m_ata_features = data; break;
    case 2: m_ata_sector_count = data; break;
    case 3: break;
    case 4: m_ata_lba_mid = data; break;
    case 5: m_ata_lba_high = data; break;
    case 6: m_ata_device = data; break;
    case 7: ExecuteATACommand(data); break;
    default: break;
  }
}

uint16_t System12CDXA::ReadATACS1(uint32_t address) const
{
  if (address < SH2_ATA_CS1_BASE || address > SH2_ATA_CS1_END)
    return UINT16_C(0xFFFF);

  const uint32_t reg = (address - SH2_ATA_CS1_BASE) / SH2_ATA_REGISTER_STRIDE;
  return (reg == 0) ? m_ata_status : UINT16_C(0xFFFF);
}

void System12CDXA::WriteATACS1(uint32_t address, uint16_t value)
{
  if (address < SH2_ATA_CS1_BASE || address > SH2_ATA_CS1_END)
    return;

  const uint32_t reg = (address - SH2_ATA_CS1_BASE) / SH2_ATA_REGISTER_STRIDE;
  if (reg != 0)
    return;

  m_ata_device_control = static_cast<uint8_t>(value);
}

bool System12CDXA::ReadShared(const uint8_t* data, size_t size, uint32_t width, uint32_t offset, uint32_t& value)
{
  if (!data || (width != 1 && width != 2 && width != 4) || offset > size || width > (size - offset))
    return false;

  if (width == 1)
  {
    // The PSX side is little-endian while the SH7014 side is big-endian.
    // The C448 bridge swaps byte lanes within each 16-bit word.
    value = data[offset ^ 1u];
    return true;
  }

  const uint16_t lo = ReadBE16(data + offset);
  if (width == 2)
  {
    value = lo;
    return true;
  }

  // The C448 window is a 16-bit bridge. Preserve PSX word ordering while
  // swapping bytes within each halfword for the big-endian SH7014 side.
  const uint16_t hi = ReadBE16(data + offset + 2u);
  value = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
  return true;
}

bool System12CDXA::WriteShared(uint8_t* data, size_t size, uint32_t width, uint32_t offset, uint32_t value)
{
  if (!data || (width != 1 && width != 2 && width != 4) || offset > size || width > (size - offset))
    return false;

  if (width == 1)
  {
    // Byte stores cross the 16-bit C448 bridge with the lane reversed.
    // Quest for Fame writes the SH firmware image odd/even for this wiring;
    // swapping here reconstructs the SH7014-visible firmware byte order.
    data[offset ^ 1u] = static_cast<uint8_t>(value);
    return true;
  }

  WriteBE16(data + offset, static_cast<uint16_t>(value));
  if (width == 4)
    WriteBE16(data + offset + 2u, static_cast<uint16_t>(value >> 16));
  return true;
}

void System12CDXA::HandleSCI0Transmit(uint8_t value)
{
  m_audio_sci_bytes++;

  if (m_audio_word_byte == 0)
  {
    m_audio_word = static_cast<uint16_t>(static_cast<uint16_t>(value) << 8);
    m_audio_word_byte = 1;
    return;
  }

  m_audio_word = static_cast<uint16_t>(m_audio_word | value);
  m_audio_word_byte = 0;

  const int16_t sample = static_cast<int16_t>(m_audio_word);
  if (m_audio_left_channel)
  {
    m_audio_pending_left = sample;
  }
  else
  {
    PushAudioFrame(m_audio_pending_left, sample);
  }

  m_audio_left_channel = !m_audio_left_channel;
}

void System12CDXA::HandlePortBWrite(uint16_t value)
{
  m_audio_muted = (value & UINT16_C(0x0040)) != 0;
}

void System12CDXA::WriteVolumeControl(uint8_t encoded_pins)
{
  m_volume_write_phase = !m_volume_write_phase;
  if (m_volume_write_phase)
  {
    m_volume_control = static_cast<uint8_t>(encoded_pins & UINT8_C(0x1F));
    return;
  }

  m_volume_data = static_cast<uint8_t>(encoded_pins & UINT8_C(0x3F));
  const uint32_t channel = m_volume_control & UINT8_C(0x03);
  m_volume_latch[channel] =
    static_cast<uint16_t>(((static_cast<uint16_t>(m_volume_control) << 4) & UINT16_C(0x01C0)) |
                          m_volume_data);
  RecalculateVolumeGain(channel);
}

void System12CDXA::RecalculateVolumeGain(uint32_t channel)
{
  if (channel >= m_volume_latch.size())
    return;

  const uint16_t latch = m_volume_latch[channel];
  uint32_t gain_q16 = UINT32_C(0x00010000);

  if ((latch & UINT16_C(0x0040)) == 0)
  {
    gain_q16 = 0;
  }
  else if ((latch & UINT16_C(0x0100)) != 0)
  {
    gain_q16 = static_cast<uint32_t>(std::lround(std::pow(10.0, -32.0 / 20.0) * 65536.0));
  }
  else if ((latch & UINT16_C(0x0080)) == 0)
  {
    const uint32_t attenuation_steps = static_cast<uint32_t>((~latch) & UINT16_C(0x003F));
    const double attenuation_db = -0.5 * static_cast<double>(attenuation_steps);
    gain_q16 =
      static_cast<uint32_t>(std::lround(std::pow(10.0, attenuation_db / 20.0) * 65536.0));
  }

  m_volume_gain_q16[channel] = gain_q16;
}

void System12CDXA::PushAudioFrame(int16_t left, int16_t right)
{
  m_audio_native_frames++;
  if (left != 0 || right != 0)
    m_audio_nonzero_frames++;

  const uint32_t left_abs = static_cast<uint32_t>(left < 0 ? -static_cast<int32_t>(left) : left);
  const uint32_t right_abs = static_cast<uint32_t>(right < 0 ? -static_cast<int32_t>(right) : right);
  m_audio_peak_abs = std::max(m_audio_peak_abs, std::max(left_abs, right_abs));

  if (m_audio_muted)
  {
    left = 0;
    right = 0;
  }
  else
  {
    left = static_cast<int16_t>(
      (static_cast<int64_t>(left) * static_cast<int64_t>(m_volume_gain_q16[0])) >> 16);
    right = static_cast<int16_t>(
      (static_cast<int64_t>(right) * static_cast<int64_t>(m_volume_gain_q16[1])) >> 16);
  }

  if (m_audio_queue_count == AUDIO_QUEUE_CAPACITY)
  {
    m_audio_queue_read = (m_audio_queue_read + 1) % AUDIO_QUEUE_CAPACITY;
    m_audio_queue_count--;
  }

  m_audio_queue[m_audio_queue_write] = {left, right};
  m_audio_queue_write = (m_audio_queue_write + 1) % AUDIO_QUEUE_CAPACITY;
  m_audio_queue_count++;
}

void System12CDXA::PopAudioFrame(int32_t* left, int32_t* right)
{
  if (!left || !right)
    return;

  *left = 0;
  *right = 0;
  m_audio_host_pops++;

  auto pop_native = [this]() {
    const StereoFrame frame = m_audio_queue[m_audio_queue_read];
    m_audio_queue_read = (m_audio_queue_read + 1) % AUDIO_QUEUE_CAPACITY;
    m_audio_queue_count--;
    return frame;
  };

  if (!m_audio_host_primed)
  {
    if (m_audio_queue_count < AUDIO_PRIME_FRAMES)
      return;

    m_audio_current = pop_native();
    m_audio_next = pop_native();
    m_audio_resample_phase = 0;
    m_audio_host_primed = true;
    m_audio_reprime_count++;
  }

  const uint32_t sci_bytes_per_second = m_sh2.GetSCI0TransmitBytesPerSecond();
  if (sci_bytes_per_second == 0)
    return;

  // Four SCI bytes make one stereo frame. Use the exact rational source rate
  // rather than the rounded integer SH clocks-per-byte fallback.
  const uint64_t phase_denominator =
    static_cast<uint64_t>(HOST_AUDIO_RATE_HZ) * UINT64_C(4);

  const int64_t current_weight = static_cast<int64_t>(phase_denominator - m_audio_resample_phase);
  const int64_t next_weight = static_cast<int64_t>(m_audio_resample_phase);

  int32_t mixed_left = static_cast<int32_t>(
    (static_cast<int64_t>(m_audio_current.left) * current_weight +
     static_cast<int64_t>(m_audio_next.left) * next_weight) /
    static_cast<int64_t>(phase_denominator));
  int32_t mixed_right = static_cast<int32_t>(
    (static_cast<int64_t>(m_audio_current.right) * current_weight +
     static_cast<int64_t>(m_audio_next.right) * next_weight) /
    static_cast<int64_t>(phase_denominator));

  mixed_left = (mixed_left * 3) / 10;
  mixed_right = (mixed_right * 3) / 10;

  *left = mixed_left;
  *right = mixed_right;

  m_audio_resample_phase += sci_bytes_per_second;
  if (m_audio_resample_phase >= phase_denominator)
  {
    m_audio_resample_phase -= phase_denominator;
    m_audio_current = m_audio_next;

    if (m_audio_queue_count == 0)
    {
      m_audio_underflows++;
      m_audio_host_primed = false;
      m_audio_next = m_audio_current;
      return;
    }

    m_audio_next = pop_native();
  }
}

bool System12CDXA::ReadPSX(uint32_t width, uint32_t exp1_offset, uint32_t& value) const
{
  if (exp1_offset >= PSX_SHARED_BASE && exp1_offset <= PSX_SHARED_END)
  {
    const uint32_t offset = exp1_offset - PSX_SHARED_BASE;
    const uint8_t* const memory = m_sram_selected ? m_sh2.GetSRAM() : m_sh2.GetCRAM();
    const size_t size = m_sram_selected ? SH7014::SRAM_SIZE : SH7014::CRAM_SIZE;
    return ReadShared(memory, size, width, offset, value);
  }

  if ((exp1_offset & ~UINT32_C(1)) == PSX_M148_STATUS)
  {
    // MAME maps 0x1F7D800A-0x1F7D800B as the Um Jammer Lammy NOW!
    // M148 EMI DRIVE status handshake. Game code waits for 0x20 here
    // before emitting its 0x55-prefixed I/O packet stream at 0x1F7D8000.
    value = UINT32_C(0x20);
    return true;
  }

  return false;
}

bool System12CDXA::WritePSX(uint32_t width, uint32_t exp1_offset, uint32_t value)
{
  if (exp1_offset >= PSX_SHARED_BASE && exp1_offset <= PSX_SHARED_END)
  {
    const uint32_t offset = exp1_offset - PSX_SHARED_BASE;

    // Hardware writes reach SRAM regardless of the selector. When CRAM is
    // selected they are also visible to CRAM; this matches the observed QF
    // firmware upload and the CDXA C448 behavior documented by MAME.
    if (!m_sram_selected)
    {
      if (!WriteShared(m_sh2.GetCRAM(), SH7014::CRAM_SIZE, width, offset, value))
        return false;
    }

    const bool handled = WriteShared(m_sh2.GetSRAM(), SH7014::SRAM_SIZE, width, offset, value);
    return handled;
  }

  if (exp1_offset >= UINT32_C(0x007F8000) && exp1_offset <= UINT32_C(0x007F80FF))
  {
    WriteVolumeControl(static_cast<uint8_t>(exp1_offset));
    return true;
  }

  if (exp1_offset < PSX_CONTROL_BASE || exp1_offset > PSX_CONTROL_END)
    return false;

  const uint16_t data = static_cast<uint16_t>(value);
  if (exp1_offset == PSX_CONTROL_BASE)
  {
    m_boot_latch = data;
    return true;
  }
  if (exp1_offset == PSX_SH2_RESET)
  {
    if (data == 1)
    {
      // CRAM contents survive SH7014 reset. Reset starts from vectors 0/1
      // in the loaded image (Quest for Fame: PC=0x440, SP=0x10000).
      m_sh2.Reset(true);
      m_sh2.SetSCI0TransmitRate(CDXA_SCI_BYTES_PER_SECOND);
      m_sh2_running = true;
    }
    return true;
  }
  if (exp1_offset >= PSX_CLOCKGEN_BASE && exp1_offset <= PSX_CLOCKGEN_END)
    return true;
  if (exp1_offset == PSX_IDE_SH2_ENABLE)
  {
    m_ide_sh2_enabled = data != 0;
    return true;
  }
  if (exp1_offset == PSX_IDE_PS1_ENABLE)
  {
    m_ide_ps1_enabled = data != 0;
    return true;
  }
  if (exp1_offset == PSX_UNKNOWN_6018)
  {
    m_unknown_6018 = data;
    return true;
  }
  if (exp1_offset == PSX_SRAM_SELECT)
  {
    m_sram_selected = data != 0;
    return true;
  }
  if (exp1_offset == PSX_INT10_ACK)
  {
    InterruptController::SetLineState(InterruptController::IRQ::IRQ10, false);
    m_psx_int10_busy = false;
    return true;
  }

  return false;
}

ArcadeDuck::SH2::CPU::Bus System12CDXA::MakeSH2ExternalBus()
{
  return {
    [this](uint32_t address) { return SH2ExternalRead8(address); },
    [this](uint32_t address) { return SH2ExternalRead16(address); },
    [this](uint32_t address) { return SH2ExternalRead32(address); },
    [this](uint32_t address, uint8_t value) { SH2ExternalWrite8(address, value); },
    [this](uint32_t address, uint16_t value) { SH2ExternalWrite16(address, value); },
    [this](uint32_t address, uint32_t value) { SH2ExternalWrite32(address, value); },
  };
}

uint8_t System12CDXA::SH2ExternalRead8(uint32_t address) const
{
  const uint16_t value = SH2ExternalRead16(address & ~1u);
  return (address & 1u) ? static_cast<uint8_t>(value) : static_cast<uint8_t>(value >> 8);
}

uint16_t System12CDXA::SH2ExternalRead16(uint32_t address) const
{
  if ((address & ~3u) == SH2_PSX_INT10_STATUS)
  {
    const uint16_t value = m_psx_int10_busy ? UINT16_C(0x0000) : UINT16_C(0x0800);
    return value;
  }

  if (address >= SH2_ATA_CS0_BASE && address <= SH2_ATA_CS0_END)
  {
    const uint16_t value = m_ide_sh2_enabled ? ReadATACS0(address) : UINT16_C(0xFFFF);
    return value;
  }

  if (address >= SH2_ATA_CS1_BASE && address <= SH2_ATA_CS1_END)
  {
    const uint16_t value = m_ide_sh2_enabled ? ReadATACS1(address) : UINT16_C(0xFFFF);
    return value;
  }

  return UINT16_C(0xFFFF);
}

uint32_t System12CDXA::SH2ExternalRead32(uint32_t address) const
{
  return (static_cast<uint32_t>(SH2ExternalRead16(address)) << 16) |
         SH2ExternalRead16(address + 2u);
}

void System12CDXA::SH2ExternalWrite8(uint32_t address, uint8_t value)
{
  const uint32_t aligned = address & ~1u;
  uint16_t current = 0;
  if (aligned == SH2_BOARD_LATCH)
    current = m_sh2_417000_latch;
  const uint16_t merged = (address & 1u) ?
    static_cast<uint16_t>((current & 0xFF00u) | value) :
    static_cast<uint16_t>((current & 0x00FFu) | (static_cast<uint16_t>(value) << 8));
  SH2ExternalWrite16(aligned, merged);
}

void System12CDXA::SH2ExternalWrite16(uint32_t address, uint16_t value)
{
  if ((address & ~3u) == SH2_PSX_INT10_TRIGGER)
  {
    InterruptController::SetLineState(InterruptController::IRQ::IRQ10, true);
    m_psx_int10_busy = true;
    return;
  }

  if ((address & ~1u) == SH2_BOARD_LATCH)
  {
    m_sh2_417000_latch = value;
    return;
  }

  if (address >= SH2_ATA_CS0_BASE && address <= SH2_ATA_CS0_END)
  {
    if (m_ide_sh2_enabled)
      WriteATACS0(address, value);
    return;
  }

  if (address >= SH2_ATA_CS1_BASE && address <= SH2_ATA_CS1_END)
  {
    // The CDXA SH7014 writes 0x0e to the ATA alternate-status/device-control
    // register during initialization. On the hardware path modeled by MAME,
    // forwarding this write puts the drive into software reset and breaks the
    // immediately following CHECK POWER MODE command, so SH2-side CS1 writes
    // are deliberately left unconnected. Keep CS1 reads and PSX-side CS1
    // accesses intact.
    return;
  }
}

void System12CDXA::SH2ExternalWrite32(uint32_t address, uint32_t value)
{
  SH2ExternalWrite16(address, static_cast<uint16_t>(value >> 16));
  SH2ExternalWrite16(address + 2u, static_cast<uint16_t>(value));
}

// The CD image is host media (sector reads are random access). Diagnostic audio counters are excluded. The PS1
// IRQ10 line level belongs to the interrupt controller state.
bool System12CDXA::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("S12CDXA") || !m_sh2.DoState(sw))
    return false;

  sw.Do(&m_sh2_running);
  sw.Do(&m_sram_selected);
  sw.Do(&m_ide_sh2_enabled);
  sw.Do(&m_ide_ps1_enabled);
  sw.Do(&m_psx_int10_busy);
  sw.Do(&m_boot_latch);
  sw.Do(&m_unknown_6018);
  sw.Do(&m_sh2_417000_latch);
  sw.DoArray(m_volume_latch.data(), m_volume_latch.size());
  sw.DoArray(m_volume_gain_q16.data(), m_volume_gain_q16.size());
  sw.Do(&m_volume_control);
  sw.Do(&m_volume_data);
  sw.Do(&m_volume_write_phase);
  sw.Do(&m_audio_word);
  sw.Do(&m_audio_word_byte);
  sw.Do(&m_audio_left_channel);
  sw.Do(&m_audio_pending_left);
  sw.Do(&m_audio_muted);
  for (StereoFrame& frame : m_audio_queue)
  {
    sw.Do(&frame.left);
    sw.Do(&frame.right);
  }
  u32 queue_read = static_cast<u32>(m_audio_queue_read), queue_write = static_cast<u32>(m_audio_queue_write);
  u32 queue_count = static_cast<u32>(m_audio_queue_count);
  sw.Do(&queue_read);
  sw.Do(&queue_write);
  sw.Do(&queue_count);
  sw.Do(&m_audio_host_primed);
  sw.Do(&m_audio_current.left);
  sw.Do(&m_audio_current.right);
  sw.Do(&m_audio_next.left);
  sw.Do(&m_audio_next.right);
  sw.Do(&m_audio_resample_phase);
  sw.Do(&m_audio_sci_bytes);
  sw.Do(&m_atapi_phase);
  sw.Do(&m_ata_error);
  sw.Do(&m_ata_features);
  sw.Do(&m_ata_sector_count);
  sw.Do(&m_ata_lba_mid);
  sw.Do(&m_ata_lba_high);
  sw.Do(&m_ata_device);
  sw.Do(&m_ata_status);
  sw.Do(&m_ata_device_control);
  sw.Do(&m_atapi_media_change);
  sw.Do(&m_atapi_sense_key);
  sw.Do(&m_atapi_sense_asc);
  sw.Do(&m_atapi_sense_ascq);
  sw.DoArray(m_identify_words.data(), m_identify_words.size());
  u32 identify_word = static_cast<u32>(m_identify_word), packet_size = static_cast<u32>(m_packet_size);
  u32 data_size = static_cast<u32>(m_atapi_data_size), data_offset = static_cast<u32>(m_atapi_data_offset);
  sw.Do(&identify_word);
  sw.DoArray(m_packet.data(), m_packet.size());
  sw.Do(&packet_size);
  sw.Do(&data_size);
  sw.Do(&data_offset);
  sw.DoBytes(m_atapi_data.data(), data_size <= m_atapi_data.size() ? data_size : 0);

  if (sw.IsReading())
  {
    if (sw.HasError() || queue_read >= AUDIO_QUEUE_CAPACITY || queue_write >= AUDIO_QUEUE_CAPACITY ||
        queue_count > AUDIO_QUEUE_CAPACITY || m_atapi_phase > ATAPIPhase::PacketDataIn ||
        identify_word > m_identify_words.size() || packet_size > m_packet.size() || data_size > m_atapi_data.size() ||
        data_offset > data_size)
    {
      return false;
    }
    m_audio_queue_read = queue_read;
    m_audio_queue_write = queue_write;
    m_audio_queue_count = queue_count;
    m_identify_word = identify_word;
    m_packet_size = packet_size;
    m_atapi_data_size = data_size;
    m_atapi_data_offset = data_offset;
  }

  return !sw.HasError();
}
} // namespace Arcade::Namco
