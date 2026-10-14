// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/arcade_rom_loader.h"

#include "common/error.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/path.h"
#include "common/sha1_digest.h"
#include "common/string_util.h"

#include <algorithm>
#include <span>

Log_SetChannel(ArcadeROMLoader);

namespace Arcade {
namespace {

enum class LoadResult
{
  NotFound,
  Loaded,
  Error,
};

struct ArchiveMemberInfo
{
  std::string name;
  u64 size = 0;
  u32 crc32 = 0;
};

bool ROMMetadataMatches(std::string_view member_name, u64 member_size, u32 member_crc32,
                        const Database::ROMDefinition& rom)
{
  const bool name_matches = StringUtil::EqualNoCase(member_name, rom.name) ||
                            StringUtil::EqualNoCase(Path::GetFileName(member_name), rom.name);
  const bool size_matches = member_size == rom.size;
  const bool crc_matches = !rom.has_crc32 || member_crc32 == rom.crc32;
  return (name_matches && size_matches && crc_matches) || (rom.has_crc32 && size_matches && crc_matches);
}

bool ReadArchiveMembers(std::string_view archive_path, std::vector<ArchiveMemberInfo>* members)
{
  const std::string archive_path_string(archive_path);
  unzFile zf = MinizipHelpers::OpenUnzFile(archive_path_string.c_str());
  if (!zf)
    return false;

  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    return false;
  }

  for (;;)
  {
    unz_file_info64 file_info = {};
    char member_name[512] = {};
    if (unzGetCurrentFileInfo64(zf, &file_info, member_name, sizeof(member_name), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      unzClose(zf);
      return false;
    }

    member_name[sizeof(member_name) - 1] = '\0';
    members->push_back({member_name, static_cast<u64>(file_info.uncompressed_size), static_cast<u32>(file_info.crc)});

    const int next_result = unzGoToNextFile(zf);
    if (next_result == UNZ_END_OF_LIST_OF_FILE)
      break;
    if (next_result != UNZ_OK)
    {
      unzClose(zf);
      return false;
    }
  }

  unzClose(zf);
  return true;
}

LoadResult TryLoadROMFromArchive(std::string_view archive_path, const Database::ROMDefinition& rom,
                                 std::vector<u8>* data, Error* error)
{
  const std::string archive_path_string(archive_path);
  unzFile zf = MinizipHelpers::OpenUnzFile(archive_path_string.c_str());
  if (!zf)
  {
    Error::SetStringFmt(error, "Failed to open arcade ROM archive '{}'.", archive_path);
    return LoadResult::Error;
  }

  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    Error::SetStringFmt(error, "Arcade ROM archive '{}' is empty or unreadable.", archive_path);
    return LoadResult::Error;
  }

  for (;;)
  {
    unz_file_info64 file_info = {};
    char member_name[512] = {};
    if (unzGetCurrentFileInfo64(zf, &file_info, member_name, sizeof(member_name), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed to read member information from arcade ROM archive '{}'.", archive_path);
      return LoadResult::Error;
    }

    member_name[sizeof(member_name) - 1] = '\0';
    if (ROMMetadataMatches(member_name, static_cast<u64>(file_info.uncompressed_size),
                           static_cast<u32>(file_info.crc), rom))
    {
      if (unzOpenCurrentFile(zf) != UNZ_OK)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Failed to decompress arcade ROM '{}' from '{}'.", rom.name, archive_path);
        return LoadResult::Error;
      }

      data->resize(rom.size);
      size_t read_offset = 0;
      while (read_offset < data->size())
      {
        const int bytes_read = unzReadCurrentFile(zf, data->data() + read_offset,
                                                  static_cast<unsigned>(data->size() - read_offset));
        if (bytes_read <= 0)
        {
          unzCloseCurrentFile(zf);
          unzClose(zf);
          Error::SetStringFmt(error, "Failed reading arcade ROM '{}' from '{}'.", rom.name, archive_path);
          return LoadResult::Error;
        }
        read_offset += static_cast<size_t>(bytes_read);
      }

      const int close_result = unzCloseCurrentFile(zf);
      if (close_result != UNZ_OK)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "CRC validation failed for arcade ROM '{}' in '{}'.", rom.name, archive_path);
        return LoadResult::Error;
      }

      if (!rom.sha1.empty())
      {
        auto digest = SHA1Digest::GetDigest(std::span<const u8>(data->data(), data->size()));
        const std::string digest_string = SHA1Digest::DigestToString(digest);
        if (!StringUtil::EqualNoCase(digest_string, rom.sha1))
        {
          unzClose(zf);
          Error::SetStringFmt(error, "Arcade ROM '{}' has SHA-1 {}, expected {}.", rom.name, digest_string, rom.sha1);
          return LoadResult::Error;
        }
      }

      unzClose(zf);
      VERBOSE_LOG("Loaded arcade ROM '{}' from '{}' member='{}'.", rom.name, archive_path, member_name);
      return LoadResult::Loaded;
    }

    const int next_result = unzGoToNextFile(zf);
    if (next_result == UNZ_END_OF_LIST_OF_FILE)
      break;
    if (next_result != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed while scanning arcade ROM archive '{}'.", archive_path);
      return LoadResult::Error;
    }
  }

  unzClose(zf);
  return LoadResult::NotFound;
}

} // namespace

bool LoadROMFromArchive(std::string_view archive_path, const Database::ROMDefinition& rom, std::vector<u8>* data,
                        Error* error)
{
  const LoadResult result = TryLoadROMFromArchive(archive_path, rom, data, error);
  if (result == LoadResult::Loaded)
    return true;
  if (result == LoadResult::Error)
    return false;

  Error::SetStringFmt(error, "Arcade ROM archive '{}' does not contain required ROM '{}'.", archive_path, rom.name);
  return false;
}

bool LoadROMFromArchives(std::span<const std::string> archive_paths, const Database::ROMDefinition& rom,
                         std::vector<u8>* data, Error* error)
{
  for (const std::string& archive_path : archive_paths)
  {
    const LoadResult result = TryLoadROMFromArchive(archive_path, rom, data, error);
    if (result == LoadResult::Loaded)
      return true;
    if (result == LoadResult::Error)
      return false;
  }

  std::string searched_paths;
  for (const std::string& archive_path : archive_paths)
  {
    if (!searched_paths.empty())
      searched_paths.append("\n");
    searched_paths.append("  ").append(archive_path);
  }

  Error::SetStringFmt(error, "Required arcade ROM '{}' was not found in:\n{}", rom.name, searched_paths);
  return false;
}

bool LoadGameROM(std::string_view archive_path, const Database::GameDefinition& game,
                 const Database::ROMDefinition& rom, std::vector<u8>* data, Error* error)
{
  const std::vector<std::string> archive_paths = Database::GetROMArchiveSearchPaths(archive_path, game);
  return LoadROMFromArchives(archive_paths, rom, data, error);
}

bool CanResolveGameROMs(std::string_view archive_path, const Database::GameDefinition& game)
{
  const std::vector<std::string> archive_paths = Database::GetROMArchiveSearchPaths(archive_path, game);
  if (archive_paths.empty())
    return false;

  std::vector<std::vector<ArchiveMemberInfo>> archive_members;
  archive_members.reserve(archive_paths.size());
  for (const std::string& path : archive_paths)
  {
    std::vector<ArchiveMemberInfo>& members = archive_members.emplace_back();
    if (!ReadArchiveMembers(path, &members))
      return false;
  }

  for (const Database::ROMDefinition& rom : game.roms)
  {
    if (!rom.required)
      continue;

    bool found = false;
    for (const std::vector<ArchiveMemberInfo>& members : archive_members)
    {
      if (std::any_of(members.begin(), members.end(), [&rom](const ArchiveMemberInfo& member) {
            return ROMMetadataMatches(member.name, member.size, member.crc32, rom);
          }))
      {
        found = true;
        break;
      }
    }

    if (!found)
      return false;
  }

  return true;
}

} // namespace Arcade
