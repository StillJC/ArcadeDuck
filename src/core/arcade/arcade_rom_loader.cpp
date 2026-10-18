// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/arcade_rom_loader.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/path.h"
#include "common/sha1_digest.h"
#include "common/string_util.h"

#include "7z.h"
#include "7zCrc.h"
#include "Alloc.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>

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
  bool has_crc32 = false;
};

struct SevenZipFileStream
{
  ISeekInStream vt = {};
  std::FILE* file = nullptr;
};

static std::once_flag s_seven_zip_crc_once;

void EnsureSevenZipCRCTable()
{
  std::call_once(s_seven_zip_crc_once, []() { CrcGenerateTable(); });
}

SRes SevenZipRead(const ISeekInStream* stream, void* buffer, size_t* size)
{
  const auto* file_stream = reinterpret_cast<const SevenZipFileStream*>(stream);
  const size_t requested = *size;
  *size = std::fread(buffer, 1, requested, file_stream->file);
  return (*size == requested || !std::ferror(file_stream->file)) ? SZ_OK : SZ_ERROR_READ;
}

SRes SevenZipSeek(const ISeekInStream* stream, Int64* position, ESzSeek origin)
{
  const auto* file_stream = reinterpret_cast<const SevenZipFileStream*>(stream);
  int whence;
  switch (origin)
  {
    case SZ_SEEK_SET:
      whence = SEEK_SET;
      break;
    case SZ_SEEK_CUR:
      whence = SEEK_CUR;
      break;
    case SZ_SEEK_END:
      whence = SEEK_END;
      break;
    default:
      return SZ_ERROR_PARAM;
  }

  if (FileSystem::FSeek64(file_stream->file, static_cast<s64>(*position), whence) != 0)
    return SZ_ERROR_READ;

  const s64 new_position = FileSystem::FTell64(file_stream->file);
  if (new_position < 0)
    return SZ_ERROR_READ;

  *position = static_cast<Int64>(new_position);
  return SZ_OK;
}

void AppendUTF8CodePoint(std::string* output, u32 code_point)
{
  if (code_point <= 0x7f)
  {
    output->push_back(static_cast<char>(code_point));
  }
  else if (code_point <= 0x7ff)
  {
    output->push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
  else if (code_point <= 0xffff)
  {
    output->push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
  else
  {
    output->push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    output->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
}

std::string UTF16ToUTF8(const UInt16* input, size_t length)
{
  std::string output;
  output.reserve(length);
  for (size_t i = 0; i < length && input[i] != 0; i++)
  {
    u32 code_point = input[i];
    if (code_point >= 0xd800 && code_point <= 0xdbff && (i + 1) < length)
    {
      const u32 low = input[i + 1];
      if (low >= 0xdc00 && low <= 0xdfff)
      {
        code_point = 0x10000 + ((code_point - 0xd800) << 10) + (low - 0xdc00);
        i++;
      }
      else
      {
        code_point = 0xfffd;
      }
    }
    else if (code_point >= 0xdc00 && code_point <= 0xdfff)
    {
      code_point = 0xfffd;
    }

    AppendUTF8CodePoint(&output, code_point);
  }

  return output;
}

class SevenZipArchive
{
public:
  SevenZipArchive() = default;
  ~SevenZipArchive()
  {
    ISzAlloc_Free(&g_Alloc, m_out_buffer);
    SzArEx_Free(&m_archive, &g_Alloc);
    if (m_file)
      std::fclose(m_file);
  }

  SevenZipArchive(const SevenZipArchive&) = delete;
  SevenZipArchive& operator=(const SevenZipArchive&) = delete;

  bool Open(std::string_view archive_path, Error* error)
  {
    const std::string path(archive_path);
    auto file = FileSystem::OpenManagedCFile(path.c_str(), "rb", error);
    if (!file)
      return false;

    m_file = file.release();
    m_file_stream.vt.Read = SevenZipRead;
    m_file_stream.vt.Seek = SevenZipSeek;
    m_file_stream.file = m_file;

    m_look_buffer.resize(1 << 18);
    LookToRead2_CreateVTable(&m_look_stream, False);
    m_look_stream.buf = m_look_buffer.data();
    m_look_stream.bufSize = m_look_buffer.size();
    m_look_stream.realStream = &m_file_stream.vt;
    LookToRead2_Init(&m_look_stream);

    EnsureSevenZipCRCTable();
    SzArEx_Init(&m_archive);
    const SRes result =
      SzArEx_Open(&m_archive, &m_look_stream.vt, &g_Alloc, &g_Alloc);
    if (result != SZ_OK)
    {
      Error::SetStringFmt(error, "Failed to open 7z arcade ROM archive '{}' (7z error {}).", archive_path, result);
      return false;
    }

    return true;
  }

  u32 GetMemberCount() const { return m_archive.NumFiles; }
  bool IsDirectory(u32 index) const { return SzArEx_IsDir(&m_archive, index) != 0; }

  ArchiveMemberInfo GetMemberInfo(u32 index) const
  {
    const size_t name_length = SzArEx_GetFileNameUtf16(&m_archive, index, nullptr);
    std::vector<UInt16> name(name_length);
    if (name_length != 0)
      SzArEx_GetFileNameUtf16(&m_archive, index, name.data());

    ArchiveMemberInfo info;
    info.name = UTF16ToUTF8(name.data(), name.size());
    info.size = static_cast<u64>(SzArEx_GetFileSize(&m_archive, index));
    info.has_crc32 = SzBitWithVals_Check(&m_archive.CRCs, index);
    if (info.has_crc32)
      info.crc32 = static_cast<u32>(m_archive.CRCs.Vals[index]);
    return info;
  }

  bool ExtractMember(u32 index, std::vector<u8>* data, Error* error)
  {
    size_t offset = 0;
    size_t extracted_size = 0;
    const SRes result =
      SzArEx_Extract(&m_archive, &m_look_stream.vt, index, &m_block_index, &m_out_buffer, &m_out_buffer_size,
                     &offset, &extracted_size, &g_Alloc, &g_Alloc);
    if (result != SZ_OK)
    {
      Error::SetStringFmt(error, "Failed to extract 7z arcade ROM member (7z error {}).", result);
      return false;
    }

    if (extracted_size == 0)
      data->clear();
    else
      data->assign(m_out_buffer + offset, m_out_buffer + offset + extracted_size);
    return true;
  }

private:
  std::FILE* m_file = nullptr;
  SevenZipFileStream m_file_stream;
  CLookToRead2 m_look_stream = {};
  CSzArEx m_archive = {};
  std::vector<Byte> m_look_buffer;
  UInt32 m_block_index = 0xffffffff;
  Byte* m_out_buffer = nullptr;
  size_t m_out_buffer_size = 0;
};

std::unordered_map<std::string, std::unique_ptr<SevenZipArchive>> s_seven_zip_archive_cache;

bool IsSevenZipPath(std::string_view path)
{
  return StringUtil::EqualNoCase(Path::GetExtension(path), "7z");
}

bool ROMMetadataMatches(std::string_view member_name, u64 member_size, u32 member_crc32, bool member_has_crc32,
                        const Database::ROMDefinition& rom)
{
  const bool name_matches = StringUtil::EqualNoCase(member_name, rom.name) ||
                            StringUtil::EqualNoCase(Path::GetFileName(member_name), rom.name);
  const bool size_matches = member_size == rom.size;
  const bool crc_matches = !rom.has_crc32 || !member_has_crc32 || member_crc32 == rom.crc32;
  const bool crc_fallback_matches =
    rom.has_crc32 && member_has_crc32 && size_matches && member_crc32 == rom.crc32;
  return (name_matches && size_matches && crc_matches) || crc_fallback_matches;
}

bool ValidateROMData(const Database::ROMDefinition& rom, std::span<const u8> data, Error* error)
{
  if (rom.has_crc32)
  {
    EnsureSevenZipCRCTable();
    const u32 crc32 = static_cast<u32>(CrcCalc(data.data(), data.size()));
    if (crc32 != rom.crc32)
    {
      Error::SetStringFmt(error, "Arcade ROM '{}' has CRC32 {:08x}, expected {:08x}.", rom.name, crc32, rom.crc32);
      return false;
    }
  }

  if (rom.sha1.empty())
    return true;

  auto digest = SHA1Digest::GetDigest(data);
  const std::string digest_string = SHA1Digest::DigestToString(digest);
  if (StringUtil::EqualNoCase(digest_string, rom.sha1))
    return true;

  Error::SetStringFmt(error, "Arcade ROM '{}' has SHA-1 {}, expected {}.", rom.name, digest_string, rom.sha1);
  return false;
}

bool ReadZipArchiveMembers(std::string_view archive_path, std::vector<ArchiveMemberInfo>* members)
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
    members->push_back({member_name, static_cast<u64>(file_info.uncompressed_size),
                        static_cast<u32>(file_info.crc), true});

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

bool ReadSevenZipArchiveMembers(std::string_view archive_path, std::vector<ArchiveMemberInfo>* members)
{
  SevenZipArchive archive;
  if (!archive.Open(archive_path, nullptr))
    return false;

  for (u32 index = 0; index < archive.GetMemberCount(); index++)
  {
    if (!archive.IsDirectory(index))
      members->push_back(archive.GetMemberInfo(index));
  }
  return true;
}

bool ReadArchiveMembers(std::string_view archive_path, std::vector<ArchiveMemberInfo>* members)
{
  return IsSevenZipPath(archive_path) ? ReadSevenZipArchiveMembers(archive_path, members) :
                                       ReadZipArchiveMembers(archive_path, members);
}

LoadResult TryLoadROMFromZipArchive(std::string_view archive_path, const Database::ROMDefinition& rom,
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
                           static_cast<u32>(file_info.crc), true, rom))
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

      if (!ValidateROMData(rom, std::span<const u8>(data->data(), data->size()), error))
      {
        unzClose(zf);
        return LoadResult::Error;
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

SevenZipArchive* GetCachedSevenZipArchive(std::string_view archive_path, Error* error)
{
  const std::string path(archive_path);
  if (const auto iter = s_seven_zip_archive_cache.find(path); iter != s_seven_zip_archive_cache.end())
    return iter->second.get();

  auto archive = std::make_unique<SevenZipArchive>();
  if (!archive->Open(archive_path, error))
    return nullptr;

  SevenZipArchive* const result = archive.get();
  s_seven_zip_archive_cache.emplace(path, std::move(archive));
  return result;
}

LoadResult TryLoadROMFromSevenZipArchive(std::string_view archive_path, const Database::ROMDefinition& rom,
                                         std::vector<u8>* data, Error* error)
{
  SevenZipArchive* const archive = GetCachedSevenZipArchive(archive_path, error);
  if (!archive)
    return LoadResult::Error;

  for (u32 index = 0; index < archive->GetMemberCount(); index++)
  {
    if (archive->IsDirectory(index))
      continue;

    const ArchiveMemberInfo member = archive->GetMemberInfo(index);
    if (!ROMMetadataMatches(member.name, member.size, member.crc32, member.has_crc32, rom))
      continue;

    if (!archive->ExtractMember(index, data, error))
    {
      Error::AddPrefixFmt(error, "Failed to decompress arcade ROM '{}' from '{}': ", rom.name, archive_path);
      return LoadResult::Error;
    }

    if (data->size() != rom.size)
    {
      Error::SetStringFmt(error, "Arcade ROM '{}' in '{}' has size {}, expected {} bytes.", rom.name, archive_path,
                          data->size(), rom.size);
      return LoadResult::Error;
    }

    if (!ValidateROMData(rom, std::span<const u8>(data->data(), data->size()), error))
      return LoadResult::Error;

    VERBOSE_LOG("Loaded arcade ROM '{}' from '{}' member='{}'.", rom.name, archive_path, member.name);
    return LoadResult::Loaded;
  }

  return LoadResult::NotFound;
}

LoadResult TryLoadROMFromArchive(std::string_view archive_path, const Database::ROMDefinition& rom,
                                 std::vector<u8>* data, Error* error)
{
  return IsSevenZipPath(archive_path) ? TryLoadROMFromSevenZipArchive(archive_path, rom, data, error) :
                                       TryLoadROMFromZipArchive(archive_path, rom, data, error);
}

} // namespace

void ClearROMArchiveCache()
{
  s_seven_zip_archive_cache.clear();
}

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
  bool had_error = false;
  for (const std::string& archive_path : archive_paths)
  {
    const LoadResult result = TryLoadROMFromArchive(archive_path, rom, data, error);
    if (result == LoadResult::Loaded)
      return true;
    if (result == LoadResult::Error)
      had_error = true;
  }

  if (had_error)
    return false;

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

bool LoadGameROMByRole(std::string_view archive_path, const Database::GameDefinition& game, std::string_view role,
                       std::vector<u8>* data, Error* error)
{
  const Database::GameDefinition* current = &game;
  std::unordered_set<std::string> visited;
  while (current && visited.emplace(current->id).second)
  {
    if (const Database::ROMDefinition* const rom = Database::GetROMByRole(*current, role))
      return LoadGameROM(archive_path, game, *rom, data, error);

    if (current->parent.empty())
      break;
    current = Database::GetGame(current->parent);
  }

  Error::SetStringFmt(error, "Arcade game '{}' and its parents are missing required ROM role '{}'.", game.id, role);
  return false;
}

bool CanResolveGameROMs(std::string_view archive_path, const Database::GameDefinition& game)
{
  const std::vector<std::string> archive_paths = Database::GetROMArchiveSearchPaths(archive_path, game);
  if (archive_paths.empty())
    return false;

  const Database::SystemDefinition* const system = Database::GetSystem(game.system_id);
  const Database::FirmwareDefinition* const firmware =
    system ? Database::ResolveFirmwareProfile(*system) : nullptr;

  std::vector<std::vector<ArchiveMemberInfo>> archive_members;
  archive_members.reserve(archive_paths.size());
  for (const std::string& path : archive_paths)
  {
    std::vector<ArchiveMemberInfo> members;
    if (!ReadArchiveMembers(path, &members))
    {
      VERBOSE_LOG("Skipping unreadable arcade ROM archive '{}' while resolving game '{}'.", path, game.id);
      continue;
    }

    archive_members.push_back(std::move(members));
  }

  if (archive_members.empty())
  {
    VERBOSE_LOG("No readable arcade ROM archives are available while resolving game '{}'.", game.id);
    return false;
  }

  for (const Database::ROMDefinition& rom : game.roms)
  {
    if (!rom.required)
      continue;

    if (firmware && std::any_of(firmware->roms.begin(), firmware->roms.end(),
                                [&rom](const Database::ROMDefinition& firmware_rom) {
                                  return rom.size == firmware_rom.size &&
                                         StringUtil::EqualNoCase(rom.sha1, firmware_rom.sha1);
                                }))
    {
      continue;
    }

    bool found = false;
    for (const std::vector<ArchiveMemberInfo>& members : archive_members)
    {
      if (std::any_of(members.begin(), members.end(), [&rom](const ArchiveMemberInfo& member) {
            return ROMMetadataMatches(member.name, member.size, member.crc32, member.has_crc32, rom);
          }))
      {
        found = true;
        break;
      }
    }

    if (!found)
    {
      VERBOSE_LOG("Required arcade ROM '{}' is unavailable while resolving game '{}'.", rom.name, game.id);
      return false;
    }
  }

  return true;
}

} // namespace Arcade
