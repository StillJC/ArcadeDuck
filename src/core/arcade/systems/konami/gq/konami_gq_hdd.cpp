// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/konami/gq/konami_gq_hdd.h"

#include "common/log.h"

Log_SetChannel(KonamiGQHDD);

namespace KonamiGQHDD {
namespace {

Arcade::Storage::CHDHardDisk s_disk;

} // namespace

bool Initialize(const char* path, Error* error)
{
  if (!s_disk.Open(path, error))
    return false;

  const Geometry& geometry = s_disk.GetGeometry();
  VERBOSE_LOG("KonamiGQ.HDD opened path='{}' cylinders={} heads={} sectors={} bytes_per_sector={} blocks={}",
           s_disk.GetPath(), geometry.cylinders, geometry.heads, geometry.sectors, geometry.bytes_per_sector,
           s_disk.GetBlockCount());
  return true;
}

void Shutdown()
{
  if (s_disk.IsOpen())
    VERBOSE_LOG("KonamiGQ.HDD closed path='{}'", s_disk.GetPath());
  s_disk.Close();
}

bool IsOpen()
{
  return s_disk.IsOpen();
}

std::string_view GetPath()
{
  return s_disk.GetPath();
}

const Geometry& GetGeometry()
{
  return s_disk.GetGeometry();
}

u32 GetBlockCount()
{
  return s_disk.GetBlockCount();
}

bool ReadSector(u32 lba, u8* buffer)
{
  return s_disk.ReadSector(lba, buffer);
}

bool WriteSector(u32 lba, const u8* buffer)
{
  return s_disk.WriteSector(lba, buffer);
}

void ClearWriteOverlay()
{
  s_disk.ClearWriteOverlay();
}

} // namespace KonamiGQHDD
