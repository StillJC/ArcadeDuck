#include "konami_gv_cdrom.h"

#include "cdrom.h"
#include "system.h"

#include "common/cd_image.h"

#include <cstring>
#include <memory>

static std::unique_ptr<CDImage> s_konami_gv_cdrom_media;

static bool KonamiGVCDROMOpenMountedMedia()
{
  if (s_konami_gv_cdrom_media)
    return true;

  const std::string& filename = g_cdrom.GetMediaFileName();
  if (filename.empty())
    return false;

  s_konami_gv_cdrom_media = CDImage::Open(filename.c_str(), nullptr);
  return static_cast<bool>(s_konami_gv_cdrom_media);
}

void KonamiGVCDROMInitialize()
{
  s_konami_gv_cdrom_media.reset();
}

void KonamiGVCDROMShutdown()
{
  s_konami_gv_cdrom_media.reset();
}

bool KonamiGVCDROMReadDataSector(u32 lba, u8* sector)
{
  if (!KonamiGVCDROMOpenMountedMedia())
    return false;

  if (!s_konami_gv_cdrom_media->Seek(1, static_cast<CDImage::LBA>(lba)))
    return false;

  if (System::GetRunningCode() == "btchamp")
  {
    u8 raw_sector[CDImage::RAW_SECTOR_SIZE];

    if (s_konami_gv_cdrom_media->Read(CDImage::ReadMode::RawSector, 1, raw_sector) != 1)
      return false;

    // Beat the Champ's GV CHD presents its useful 2048-byte payload at the
    // beginning of DuckStation's raw-sector buffer.
    std::memcpy(sector, raw_sector, CDImage::DATA_SECTOR_SIZE);
    return true;
  }

  return s_konami_gv_cdrom_media->Read(CDImage::ReadMode::DataOnly, 1, sector) == 1;
}

u32 KonamiGVCDROMReadTOC(const u8* cdb, u8* response, u32 response_size)
{
  if (!KonamiGVCDROMOpenMountedMedia() || !cdb || !response || response_size < 4)
    return 0;

  const bool msf = (cdb[1] & 0x02) != 0;
  const u8 requested_track = cdb[6];
  const u8 first_track = static_cast<u8>(s_konami_gv_cdrom_media->GetFirstTrackNumber());
  const u8 last_track = static_cast<u8>(s_konami_gv_cdrom_media->GetLastTrackNumber());

  std::memset(response, 0, response_size);

  response[2] = first_track;
  response[3] = last_track;

  u32 write_offset = 4;

  const auto append_descriptor = [&](u8 track_number, u8 control_bits, CDImage::LBA lba) {
    if ((write_offset + 8) > response_size)
      return;

    response[write_offset + 0] = 0x00;

    // CDImage stores ADR in the low nibble and CONTROL in the high nibble.
    // SCSI TOC descriptors use ADR in the high nibble and CONTROL in the low nibble.
    response[write_offset + 1] = static_cast<u8>(((control_bits & 0x0F) << 4) | ((control_bits >> 4) & 0x0F));

    response[write_offset + 2] = track_number;
    response[write_offset + 3] = 0x00;

    if (msf)
    {
      const CDImage::Position position = CDImage::Position::FromLBA(lba);

      response[write_offset + 4] = 0x00;
      response[write_offset + 5] = position.minute;
      response[write_offset + 6] = position.second;
      response[write_offset + 7] = position.frame;
    }
    else
    {
      response[write_offset + 4] = static_cast<u8>(lba >> 24);
      response[write_offset + 5] = static_cast<u8>(lba >> 16);
      response[write_offset + 6] = static_cast<u8>(lba >> 8);
      response[write_offset + 7] = static_cast<u8>(lba);
    }

    write_offset += 8;
  };

  if (requested_track != CDImage::LEAD_OUT_TRACK_NUMBER)
  {
    const u8 start_track = (requested_track == 0 || requested_track < first_track) ? first_track : requested_track;

    for (u8 track = start_track; track <= last_track; track++)
    {
      const CDImage::Track& track_info = s_konami_gv_cdrom_media->GetTrack(track);

      append_descriptor(track, track_info.control.bits, track_info.start_lba);

      if ((write_offset + 8) > response_size)
        break;
    }
  }

  if ((write_offset + 8) <= response_size)
  {
    const CDImage::Track& last_track_info = s_konami_gv_cdrom_media->GetTrack(last_track);

    append_descriptor(CDImage::LEAD_OUT_TRACK_NUMBER, last_track_info.control.bits,
                      s_konami_gv_cdrom_media->GetLBACount());
  }

  const u16 toc_data_length = static_cast<u16>(write_offset - 2);

  response[0] = static_cast<u8>(toc_data_length >> 8);
  response[1] = static_cast<u8>(toc_data_length);

  return write_offset;
}