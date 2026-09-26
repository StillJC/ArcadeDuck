// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/storage/chd_hard_disk.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include "libchdr/chd.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

Log_SetChannel(ArcadeCHDHardDisk);

namespace Arcade::Storage {
namespace {

constexpr u32 SECTOR_SIZE = 512;

} // namespace

struct CHDHardDisk::Impl
{
  chd_file* chd = nullptr;
  std::string path;
  Geometry geometry;
  u32 block_count = 0;
  u32 hunk_bytes = 0;
  u32 hunk_count = 0;
  u32 sectors_per_hunk = 0;
  std::vector<u8> hunk_buffer;
  u32 current_hunk = UINT32_MAX;
  std::unordered_map<u32, std::array<u8, SECTOR_SIZE>> write_overlay;

  bool LoadHunk(u32 hunk)
  {
    if (!chd || hunk >= hunk_count)
      return false;

    if (current_hunk == hunk)
      return true;

    const chd_error err = chd_read(chd, hunk, hunk_buffer.data());
    if (err != CHDERR_NONE)
    {
      ERROR_LOG("Arcade CHD hard-disk hunk read failed path='{}' hunk={} error='{}'", path, hunk,
                chd_error_string(err));
      return false;
    }

    current_hunk = hunk;
    return true;
  }
};

CHDHardDisk::CHDHardDisk() = default;

CHDHardDisk::~CHDHardDisk()
{
  Close();
}

CHDHardDisk::CHDHardDisk(CHDHardDisk&& other) noexcept = default;

CHDHardDisk& CHDHardDisk::operator=(CHDHardDisk&& other) noexcept = default;

bool CHDHardDisk::Open(const char* path, Error* error)
{
  Close();

  auto fp = FileSystem::OpenManagedSharedCFile(path, "rb", FileSystem::FileShareMode::DenyWrite, error);
  if (!fp)
    return false;

  chd_file* chd = nullptr;
  const chd_error open_error = chd_open_file(fp.get(), CHD_OPEN_READ | CHD_OPEN_TRANSFER_FILE, nullptr, &chd);
  if (open_error != CHDERR_NONE)
  {
    Error::SetStringFmt(error, "Failed to open arcade hard-disk CHD '{}': {}", Path::GetFileName(path),
                        chd_error_string(open_error));
    return false;
  }
  fp.release();

  const chd_header* const header = chd_get_header(chd);
  if (!header || header->hunkbytes == 0 || header->logicalbytes == 0)
  {
    chd_close(chd);
    Error::SetStringFmt(error, "Arcade hard-disk CHD '{}' has an invalid header.", Path::GetFileName(path));
    return false;
  }

  std::array<char, 256> metadata = {};
  u32 metadata_length = 0;
  const chd_error metadata_error =
    chd_get_metadata(chd, HARD_DISK_METADATA_TAG, 0, metadata.data(), static_cast<u32>(metadata.size() - 1),
                     &metadata_length, nullptr, nullptr);
  if (metadata_error != CHDERR_NONE)
  {
    chd_close(chd);
    Error::SetStringFmt(error, "Arcade hard-disk CHD '{}' is missing hard-disk geometry metadata: {}",
                        Path::GetFileName(path), chd_error_string(metadata_error));
    return false;
  }
  metadata[std::min<u32>(metadata_length, static_cast<u32>(metadata.size() - 1))] = '\0';

  int cylinders = 0;
  int heads = 0;
  int sectors = 0;
  int bytes_per_sector = 0;
  if (std::sscanf(metadata.data(), HARD_DISK_METADATA_FORMAT, &cylinders, &heads, &sectors, &bytes_per_sector) != 4 ||
      cylinders <= 0 || heads <= 0 || sectors <= 0 || bytes_per_sector <= 0)
  {
    chd_close(chd);
    Error::SetStringFmt(error, "Arcade hard-disk CHD '{}' has malformed geometry metadata: {}",
                        Path::GetFileName(path), metadata.data());
    return false;
  }

  const Geometry geometry = {
    .cylinders = static_cast<u32>(cylinders),
    .heads = static_cast<u32>(heads),
    .sectors = static_cast<u32>(sectors),
    .bytes_per_sector = static_cast<u32>(bytes_per_sector),
  };

  if (geometry.bytes_per_sector != SECTOR_SIZE || (header->logicalbytes % SECTOR_SIZE) != 0 ||
      (header->hunkbytes % SECTOR_SIZE) != 0)
  {
    chd_close(chd);
    Error::SetStringFmt(error,
                        "Arcade hard-disk CHD '{}' has unsupported geometry ({} cylinders, {} heads, {} sectors, "
                        "{} bytes/sector, {} bytes/hunk).",
                        Path::GetFileName(path), geometry.cylinders, geometry.heads, geometry.sectors,
                        geometry.bytes_per_sector, header->hunkbytes);
    return false;
  }

  const u64 block_count64 = header->logicalbytes / SECTOR_SIZE;
  const u64 geometry_block_count = static_cast<u64>(geometry.cylinders) * geometry.heads * geometry.sectors;
  const u32 hunk_count = header->hunkcount != 0 ? header->hunkcount : header->totalhunks;
  if (block_count64 == 0 || block_count64 > UINT32_MAX || geometry_block_count != block_count64 || hunk_count == 0)
  {
    chd_close(chd);
    Error::SetStringFmt(error,
                        "Arcade hard-disk CHD '{}' has inconsistent geometry or logical size "
                        "(geometry blocks={}, logical blocks={}, hunks={}).",
                        Path::GetFileName(path), geometry_block_count, block_count64, hunk_count);
    return false;
  }

  auto impl = std::make_unique<Impl>();
  impl->chd = chd;
  impl->path = path;
  impl->geometry = geometry;
  impl->block_count = static_cast<u32>(block_count64);
  impl->hunk_bytes = header->hunkbytes;
  impl->hunk_count = hunk_count;
  impl->sectors_per_hunk = header->hunkbytes / SECTOR_SIZE;
  impl->hunk_buffer.resize(header->hunkbytes);
  impl->current_hunk = UINT32_MAX;
  m_impl = std::move(impl);

  VERBOSE_LOG("Arcade CHD hard-disk opened path='{}' cylinders={} heads={} sectors={} bytes_per_sector={} blocks={} "
           "hunk_bytes={} hunks={} sectors_per_hunk={}",
           m_impl->path, m_impl->geometry.cylinders, m_impl->geometry.heads, m_impl->geometry.sectors,
           m_impl->geometry.bytes_per_sector, m_impl->block_count, m_impl->hunk_bytes, m_impl->hunk_count,
           m_impl->sectors_per_hunk);
  return true;
}

void CHDHardDisk::Close()
{
  if (!m_impl)
    return;

  if (m_impl->chd)
  {
    VERBOSE_LOG("Arcade CHD hard-disk closed path='{}' overlay_sectors={}", m_impl->path, m_impl->write_overlay.size());
    chd_close(m_impl->chd);
    m_impl->chd = nullptr;
  }
  m_impl.reset();
}

bool CHDHardDisk::IsOpen() const
{
  return m_impl && m_impl->chd;
}

std::string_view CHDHardDisk::GetPath() const
{
  return m_impl ? std::string_view(m_impl->path) : std::string_view{};
}

const CHDHardDisk::Geometry& CHDHardDisk::GetGeometry() const
{
  static constexpr Geometry empty_geometry{};
  return m_impl ? m_impl->geometry : empty_geometry;
}

u32 CHDHardDisk::GetBlockCount() const
{
  return m_impl ? m_impl->block_count : 0;
}

bool CHDHardDisk::ReadSector(u32 lba, u8* buffer)
{
  if (!m_impl || !m_impl->chd || !buffer || lba >= m_impl->block_count)
    return false;

  if (const auto it = m_impl->write_overlay.find(lba); it != m_impl->write_overlay.end())
  {
    std::memcpy(buffer, it->second.data(), SECTOR_SIZE);
    return true;
  }

  const u64 byte_offset = static_cast<u64>(lba) * SECTOR_SIZE;
  const u32 hunk = static_cast<u32>(byte_offset / m_impl->hunk_bytes);
  const u32 hunk_offset = static_cast<u32>(byte_offset % m_impl->hunk_bytes);
  if ((hunk_offset + SECTOR_SIZE) > m_impl->hunk_buffer.size() || !m_impl->LoadHunk(hunk))
    return false;

  std::memcpy(buffer, m_impl->hunk_buffer.data() + hunk_offset, SECTOR_SIZE);
  return true;
}

bool CHDHardDisk::WriteSector(u32 lba, const u8* buffer)
{
  if (!m_impl || !m_impl->chd || !buffer || lba >= m_impl->block_count)
    return false;

  auto& sector = m_impl->write_overlay[lba];
  std::memcpy(sector.data(), buffer, sector.size());
  return true;
}

void CHDHardDisk::ClearWriteOverlay()
{
  if (!m_impl)
    return;

  if (!m_impl->write_overlay.empty())
    VERBOSE_LOG("Arcade CHD hard-disk overlay cleared sectors={}", m_impl->write_overlay.size());
  m_impl->write_overlay.clear();
}

} // namespace Arcade::Storage
