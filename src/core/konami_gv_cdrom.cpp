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