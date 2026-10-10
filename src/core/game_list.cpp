// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "game_list.h"
#include "host.h"
#include "core/arcade/arcade_database.h"
#include "settings.h"

#include "util/http_downloader.h"
#include "util/ini_settings_interface.h"

#include "common/binary_reader_writer.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/heterogeneous_containers.h"
#include "common/log.h"
#include "common/path.h"
#include "common/progress_callback.h"
#include "common/string_util.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <string_view>
#include <unordered_map>
#include <utility>

Log_SetChannel(GameList);

#ifdef _WIN32
#include "common/windows_headers.h"
#endif

namespace GameList {
namespace {

enum : u32
{
  GAME_LIST_CACHE_SIGNATURE = 0x45434C48,
  GAME_LIST_CACHE_VERSION = 40,

  PLAYED_TIME_SERIAL_LENGTH = 32,
  PLAYED_TIME_LAST_TIME_LENGTH = 20,  // uint64
  PLAYED_TIME_TOTAL_TIME_LENGTH = 20, // uint64
  PLAYED_TIME_LINE_LENGTH =
    PLAYED_TIME_SERIAL_LENGTH + 1 + PLAYED_TIME_LAST_TIME_LENGTH + 1 + PLAYED_TIME_TOTAL_TIME_LENGTH,
};

struct PlayedTimeEntry
{
  std::time_t last_played_time;
  std::time_t total_played_time;
};

} // namespace

using CacheMap = PreferUnorderedStringMap<Entry>;
using PlayedTimeMap = PreferUnorderedStringMap<PlayedTimeEntry>;

static bool GetArcadeListEntry(const std::string& path, Entry* entry);

static void ApplyCustomAttributes(const std::string& path, Entry* entry,
                                  const INISettingsInterface& custom_attributes_ini);
static bool RescanCustomAttributesForPath(const std::string& path, const INISettingsInterface& custom_attributes_ini);
static bool GetGameListEntryFromCache(const std::string& path, Entry* entry,
                                      const INISettingsInterface& custom_attributes_ini);
static Entry* GetMutableEntryForPath(std::string_view path);
static void ScanDirectory(const char* path, bool recursive, bool only_cache,
                          const std::vector<std::string>& excluded_paths, const PlayedTimeMap& played_time_map,
                          const INISettingsInterface& custom_attributes_ini, BinaryFileWriter& cache_writer,
                          ProgressCallback* progress);
static bool AddFileFromCache(const std::string& path, std::time_t timestamp, const PlayedTimeMap& played_time_map,
                             const INISettingsInterface& custom_attributes_ini);
static bool ScanFile(std::string path, std::time_t timestamp, std::unique_lock<std::recursive_mutex>& lock,
                     const PlayedTimeMap& played_time_map, const INISettingsInterface& custom_attributes_ini,
                     BinaryFileWriter& cache_writer);

static bool LoadOrInitializeCache(std::FILE* fp, bool invalidate_cache);
static bool LoadEntriesFromCache(BinaryFileReader& reader);
static bool WriteEntryToCache(const Entry* entry, BinaryFileWriter& writer);
static std::string GetPlayedTimeFile();
static bool ParsePlayedTimeLine(char* line, std::string& serial, PlayedTimeEntry& entry);
static std::string MakePlayedTimeLine(const std::string& serial, const PlayedTimeEntry& entry);
static PlayedTimeMap LoadPlayedTimeMap(const std::string& path);
static PlayedTimeEntry UpdatePlayedTimeFile(const std::string& path, const std::string& serial, std::time_t last_time,
                                            std::time_t add_time);

static std::string GetCustomPropertiesFile();

static EntryList s_entries;
static std::recursive_mutex s_mutex;
static CacheMap s_cache_map;

static bool s_game_list_loaded = false;

} // namespace GameList

bool GameList::IsGameListLoaded()
{
  return s_game_list_loaded;
}

bool GameList::IsScannableFilename(std::string_view path)
{
  return Arcade::Database::IsArchivePath(path);
}

bool GameList::GetArcadeListEntry(const std::string& path, Entry* entry)
{
  const Arcade::Database::GameDefinition* const game = Arcade::Database::IdentifyArchive(path);
  if (!game)
  {
    INFO_LOG("ArcadeDatabase.GameList ignored_unknown_archive archive='{}'", Path::GetFileName(path));
    return false;
  }

  entry->path = path;
  // This legacy-named field is the canonical arcade set ID.
  entry->serial = game->id;
  entry->title = game->title;
  entry->genre = game->genre;
  entry->publisher = game->manufacturer;
  entry->developer = game->developer;
  entry->min_players = game->min_players;
  entry->max_players = game->max_players;
  if (game->year != 0)
  {
    std::tm release_date = {};
    release_date.tm_year = static_cast<int>(game->year) - 1900;
    release_date.tm_mon = 0;
    release_date.tm_mday = 1;
#ifdef _WIN32
    entry->release_date = static_cast<u64>(_mkgmtime(&release_date));
#else
    entry->release_date = static_cast<u64>(timegm(&release_date));
#endif
  }

  const Arcade::Database::SystemDefinition* const system = Arcade::Database::GetSystem(game->system_id);
  entry->system = system ? system->name : game->system_id;
  INFO_LOG("ArcadeDatabase.GameList added_archive canonical_set='{}' title='{}' system='{}'", entry->serial,
           entry->title, entry->system);
  return true;
}

bool GameList::PopulateEntryFromPath(const std::string& path, Entry* entry)
{
  return Arcade::Database::IsArchivePath(path) && GetArcadeListEntry(path, entry);
}

bool GameList::GetGameListEntryFromCache(const std::string& path, Entry* entry,
                                         const INISettingsInterface& custom_attributes_ini)
{
  auto iter = s_cache_map.find(path);
  if (iter == s_cache_map.end())
    return false;

  *entry = std::move(iter->second);
  s_cache_map.erase(iter);
  ApplyCustomAttributes(path, entry, custom_attributes_ini);
  return true;
}

bool GameList::LoadEntriesFromCache(BinaryFileReader& reader)
{
  u32 file_signature, file_version;
  if (!reader.ReadU32(&file_signature) || !reader.ReadU32(&file_version) ||
      file_signature != GAME_LIST_CACHE_SIGNATURE || file_version != GAME_LIST_CACHE_VERSION)
  {
    WARNING_LOG("Game list cache is corrupted");
    return false;
  }

  while (!reader.IsAtEnd())
  {
    std::string path;
    Entry ge;

    if (!reader.ReadSizePrefixedString(&path) || !reader.ReadSizePrefixedString(&ge.serial) ||
        !reader.ReadSizePrefixedString(&ge.title) || !reader.ReadSizePrefixedString(&ge.system) ||
        !reader.ReadSizePrefixedString(&ge.genre) || !reader.ReadSizePrefixedString(&ge.publisher) ||
        !reader.ReadSizePrefixedString(&ge.developer) ||
        !reader.ReadS64(&ge.file_size) || !reader.ReadU64(&ge.uncompressed_size) ||
        !reader.ReadU64(reinterpret_cast<u64*>(&ge.last_modified_time)) || !reader.ReadU64(&ge.release_date) ||
        !reader.ReadU8(&ge.min_players) || !reader.ReadU8(&ge.max_players))
    {
      WARNING_LOG("Game list cache entry is corrupted");
      return false;
    }

    ge.path = path;
    auto iter = s_cache_map.find(ge.path);
    if (iter != s_cache_map.end())
      iter->second = std::move(ge);
    else
      s_cache_map.emplace(std::move(path), std::move(ge));
  }

  return true;
}

bool GameList::WriteEntryToCache(const Entry* entry, BinaryFileWriter& writer)
{
  writer.WriteSizePrefixedString(entry->path);
  writer.WriteSizePrefixedString(entry->serial);
  writer.WriteSizePrefixedString(entry->title);
  writer.WriteSizePrefixedString(entry->system);
  writer.WriteSizePrefixedString(entry->genre);
  writer.WriteSizePrefixedString(entry->publisher);
  writer.WriteSizePrefixedString(entry->developer);
  writer.WriteS64(entry->file_size);
  writer.WriteU64(entry->uncompressed_size);
  writer.WriteU64(entry->last_modified_time);
  writer.WriteU64(entry->release_date);
  writer.WriteU8(entry->min_players);
  writer.WriteU8(entry->max_players);
  return writer.IsGood();
}

bool GameList::LoadOrInitializeCache(std::FILE* fp, bool invalidate_cache)
{
  BinaryFileReader reader(fp);
  if (!invalidate_cache && !reader.IsAtEnd() && LoadEntriesFromCache(reader))
  {
    // Prepare for writing.
    return (FileSystem::FSeek64(fp, 0, SEEK_END) == 0);
  }

  WARNING_LOG("Initializing game list cache.");
  s_cache_map.clear();

  // Truncate file, and re-write header.
  Error error;
  if (!FileSystem::FSeek64(fp, 0, SEEK_SET, &error) || !FileSystem::FTruncate64(fp, 0, &error))
  {
    ERROR_LOG("Failed to truncate game list cache: {}", error.GetDescription());
    return false;
  }

  BinaryFileWriter writer(fp);
  writer.WriteU32(GAME_LIST_CACHE_SIGNATURE);
  writer.WriteU32((GAME_LIST_CACHE_VERSION));
  if (!writer.Flush(&error))
  {
    ERROR_LOG("Failed to write game list cache header: {}", error.GetDescription());
    return false;
  }

  return true;
}

static bool IsPathExcluded(const std::vector<std::string>& excluded_paths, const std::string& path)
{
  return std::find_if(excluded_paths.begin(), excluded_paths.end(),
                      [&path](const std::string& entry) { return path.starts_with(entry); }) != excluded_paths.end();
}

void GameList::ScanDirectory(const char* path, bool recursive, bool only_cache,
                             const std::vector<std::string>& excluded_paths, const PlayedTimeMap& played_time_map,
                             const INISettingsInterface& custom_attributes_ini, BinaryFileWriter& cache_writer,
                             ProgressCallback* progress)
{
  INFO_LOG("Scanning {}{}", path, recursive ? " (recursively)" : "");

  progress->SetStatusText(SmallString::from_format(TRANSLATE_FS("GameList", "Scanning directory '{}'..."), path));

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(path, "*",
                        recursive ? (FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RECURSIVE) :
                                    (FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES),
                        &files);
  if (files.empty())
    return;

  progress->PushState();
  progress->SetProgressRange(static_cast<u32>(files.size()));
  progress->SetProgressValue(0);

  u32 files_scanned = 0;
  for (FILESYSTEM_FIND_DATA& ffd : files)
  {
    files_scanned++;

    if (progress->IsCancelled() || !GameList::IsScannableFilename(ffd.FileName) ||
        IsPathExcluded(excluded_paths, ffd.FileName))
    {
      continue;
    }

    if (!Arcade::Database::IdentifyArchive(ffd.FileName))
    {
      INFO_LOG("ArcadeDatabase.GameList ignored_unknown_archive archive='{}'", Path::GetFileName(ffd.FileName));
      continue;
    }

    std::unique_lock lock(s_mutex);
    if (GetEntryForPath(ffd.FileName) ||
        AddFileFromCache(ffd.FileName, ffd.ModificationTime, played_time_map, custom_attributes_ini) || only_cache)
    {
      continue;
    }

    progress->SetStatusText(SmallString::from_format(TRANSLATE_FS("GameList", "Scanning '{}'..."),
                                                     FileSystem::GetDisplayNameFromPath(ffd.FileName)));
    ScanFile(std::move(ffd.FileName), ffd.ModificationTime, lock, played_time_map, custom_attributes_ini, cache_writer);
    progress->SetProgressValue(files_scanned);
  }

  progress->SetProgressValue(files_scanned);
  progress->PopState();
}

bool GameList::AddFileFromCache(const std::string& path, std::time_t timestamp, const PlayedTimeMap& played_time_map,
                                const INISettingsInterface& custom_attributes_ini)
{
  Entry entry;
  if (!GetGameListEntryFromCache(path, &entry, custom_attributes_ini) || entry.last_modified_time != timestamp)
    return false;

  auto iter = played_time_map.find(entry.serial);
  if (iter != played_time_map.end())
  {
    entry.last_played_time = iter->second.last_played_time;
    entry.total_played_time = iter->second.total_played_time;
  }

  s_entries.push_back(std::move(entry));
  return true;
}

bool GameList::ScanFile(std::string path, std::time_t timestamp, std::unique_lock<std::recursive_mutex>& lock,
                        const PlayedTimeMap& played_time_map, const INISettingsInterface& custom_attributes_ini,
                        BinaryFileWriter& cache_writer)
{
  // don't block UI while scanning
  lock.unlock();

  DEV_LOG("Scanning '{}'...", path);

  Entry entry;
  if (!PopulateEntryFromPath(path, &entry))
    return false;

  entry.path = std::move(path);
  entry.last_modified_time = timestamp;

  if (cache_writer.IsOpen() && !WriteEntryToCache(&entry, cache_writer)) [[unlikely]]
    WARNING_LOG("Failed to write entry '{}' to cache", entry.path);

  const auto iter = played_time_map.find(entry.serial);
  if (iter != played_time_map.end())
  {
    entry.last_played_time = iter->second.last_played_time;
    entry.total_played_time = iter->second.total_played_time;
  }

  ApplyCustomAttributes(entry.path, &entry, custom_attributes_ini);

  lock.lock();

  // replace if present
  auto it = std::find_if(s_entries.begin(), s_entries.end(),
                         [&entry](const Entry& existing_entry) { return (existing_entry.path == entry.path); });
  if (it != s_entries.end())
    *it = std::move(entry);
  else
    s_entries.push_back(std::move(entry));

  return true;
}

bool GameList::RescanCustomAttributesForPath(const std::string& path, const INISettingsInterface& custom_attributes_ini)
{
  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
    return false;

  {
    // cancel if excluded
    const std::vector<std::string> excluded_paths(Host::GetBaseStringListSetting("GameList", "ExcludedPaths"));
    if (IsPathExcluded(excluded_paths, path))
      return false;
  }

  Entry entry;
  if (!PopulateEntryFromPath(path, &entry))
    return false;

  entry.path = path;
  entry.last_modified_time = sd.ModificationTime;

  const PlayedTimeMap played_time_map(LoadPlayedTimeMap(GetPlayedTimeFile()));
  const auto iter = played_time_map.find(entry.serial);
  if (iter != played_time_map.end())
  {
    entry.last_played_time = iter->second.last_played_time;
    entry.total_played_time = iter->second.total_played_time;
  }

  ApplyCustomAttributes(entry.path, &entry, custom_attributes_ini);

  std::unique_lock lock(s_mutex);

  // replace if present
  auto it = std::find_if(s_entries.begin(), s_entries.end(),
                         [&entry](const Entry& existing_entry) { return (existing_entry.path == entry.path); });
  if (it != s_entries.end())
    *it = std::move(entry);
  else
    s_entries.push_back(std::move(entry));

  return true;
}

void GameList::ApplyCustomAttributes(const std::string& path, Entry* entry,
                                     const INISettingsInterface& custom_attributes_ini)
{
  std::optional<std::string> custom_title = custom_attributes_ini.GetOptionalStringValue(path.c_str(), "Title");
  if (custom_title.has_value())
  {
    entry->title = std::move(custom_title.value());
    entry->has_custom_title = true;
  }
}

std::unique_lock<std::recursive_mutex> GameList::GetLock()
{
  return std::unique_lock<std::recursive_mutex>(s_mutex);
}

const GameList::Entry* GameList::GetEntryByIndex(u32 index)
{
  return (index < s_entries.size()) ? &s_entries[index] : nullptr;
}

const GameList::Entry* GameList::GetEntryForPath(std::string_view path)
{
  return GetMutableEntryForPath(path);
}

GameList::Entry* GameList::GetMutableEntryForPath(std::string_view path)
{
  for (Entry& entry : s_entries)
  {
    // Use case-insensitive compare on Windows, since it's the same file.
#ifdef _WIN32
    if (StringUtil::EqualNoCase(entry.path, path))
      return &entry;
#else
    if (entry.path == path)
      return &entry;
#endif
  }

  return nullptr;
}

const GameList::Entry* GameList::GetEntryBySerial(std::string_view serial)
{
  for (const Entry& entry : s_entries)
  {
    if (entry.serial == serial)
      return &entry;
  }

  return nullptr;
}

u32 GameList::GetEntryCount()
{
  return static_cast<u32>(s_entries.size());
}

void GameList::Refresh(bool invalidate_cache, bool only_cache, ProgressCallback* progress /* = nullptr */)
{
  s_game_list_loaded = true;

  if (!progress)
    progress = ProgressCallback::NullProgressCallback;

  Error error;
  FileSystem::ManagedCFilePtr cache_file =
    FileSystem::OpenExistingOrCreateManagedCFile(Path::Combine(EmuFolders::Cache, "gamelist.cache").c_str(), 0, &error);
  if (!cache_file)
    ERROR_LOG("Failed to open game list cache: {}", error.GetDescription());

#ifndef _WIN32
  // Lock cache file for multi-instance on Linux. Implicitly done on Windows.
  std::optional<FileSystem::POSIXLock> cache_file_lock;
  if (cache_file)
    cache_file_lock.emplace(cache_file.get());
  if (!LoadOrInitializeCache(cache_file.get(), invalidate_cache))
  {
    cache_file_lock.reset();
    cache_file.reset();
  }
#else
  if (!LoadOrInitializeCache(cache_file.get(), invalidate_cache))
    cache_file.reset();
#endif
  BinaryFileWriter cache_writer(cache_file.get());

  // don't delete the old entries, since the frontend might still access them
  std::vector<Entry> old_entries;
  {
    std::unique_lock lock(s_mutex);
    old_entries.swap(s_entries);
  }

  const std::vector<std::string> excluded_paths(Host::GetBaseStringListSetting("GameList", "ExcludedPaths"));
  const std::vector<std::string> dirs(Host::GetBaseStringListSetting("GameList", "Paths"));
  std::vector<std::string> recursive_dirs(Host::GetBaseStringListSetting("GameList", "RecursivePaths"));
  const PlayedTimeMap played_time(LoadPlayedTimeMap(GetPlayedTimeFile()));
  INISettingsInterface custom_attributes_ini(GetCustomPropertiesFile());
  custom_attributes_ini.Load();

  if (!dirs.empty() || !recursive_dirs.empty())
  {
    progress->SetProgressRange(static_cast<u32>(dirs.size() + recursive_dirs.size()));
    progress->SetProgressValue(0);

    // we manually count it here, because otherwise pop state updates it itself
    int directory_counter = 0;
    for (const std::string& dir : dirs)
    {
      if (progress->IsCancelled())
        break;

      ScanDirectory(dir.c_str(), false, only_cache, excluded_paths, played_time, custom_attributes_ini, cache_writer,
                    progress);
      progress->SetProgressValue(++directory_counter);
    }
    for (const std::string& dir : recursive_dirs)
    {
      if (progress->IsCancelled())
        break;

      ScanDirectory(dir.c_str(), true, only_cache, excluded_paths, played_time, custom_attributes_ini, cache_writer,
                    progress);
      progress->SetProgressValue(++directory_counter);
    }
  }

  // don't need unused cache entries
  s_cache_map.clear();

}

GameList::EntryList GameList::TakeEntryList()
{
  EntryList ret = std::move(s_entries);
  s_entries = {};
  return ret;
}

static std::string GetFullCoverPath(std::string_view filename, std::string_view extension)
{
  return fmt::format("{}" FS_OSPATH_SEPARATOR_STR "{}.{}", EmuFolders::Covers, filename, extension);
}

static constexpr std::array<const char*, 3> s_artwork_directory_names = {"clear-logo", "box-2d", "box-3d"};
static constexpr std::array<const char*, 3> s_artwork_extensions = {"png", "jpg", "png"};
static constexpr std::array<const char*, 3> s_artwork_display_names = {"Clear Logo", "2D Box", "3D Box"};
static constexpr const char* ARCADEDUCK_ARTWORK_BASE_URL =
  "https://raw.githubusercontent.com/StillJC/ArcadeDuck-Artwork/master";

static size_t GetArtworkTypeIndex(GameList::ArtworkType type)
{
  return static_cast<size_t>(type);
}

static std::string GetDownloadedArtworkPath(std::string_view serial, GameList::ArtworkType type)
{
  const size_t index = GetArtworkTypeIndex(type);
  if (serial.empty() || index >= s_artwork_directory_names.size())
    return {};

  return fmt::format("{}" FS_OSPATH_SEPARATOR_STR "arcadeduck" FS_OSPATH_SEPARATOR_STR "{}"
                     FS_OSPATH_SEPARATOR_STR "{}.{}",
                     EmuFolders::Covers, s_artwork_directory_names[index], serial, s_artwork_extensions[index]);
}

static std::string GetManualCoverImagePath(const std::string& path, const std::string& serial, const std::string& title)
{
  static constexpr const std::array extensions = {"jpg", "jpeg", "png", "webp"};

  for (const char* extension : extensions)
  {
    // Prioritize lookup by serial (most specific).
    if (!serial.empty())
    {
      const std::string cover_path(GetFullCoverPath(serial, extension));
      if (FileSystem::FileExists(cover_path.c_str()))
        return cover_path;
    }

    // Try file title for specifically assigned/modded entries.
    const std::string_view file_title(Path::GetFileTitle(path));
    if (!file_title.empty() && title != file_title)
    {
      const std::string cover_path(GetFullCoverPath(file_title, extension));
      if (FileSystem::FileExists(cover_path.c_str()))
        return cover_path;
    }

    // Last resort for manually assigned legacy covers: game title.
    if (!title.empty())
    {
      const std::string cover_path(GetFullCoverPath(title, extension));
      if (FileSystem::FileExists(cover_path.c_str()))
        return cover_path;
    }
  }

  return {};
}

GameList::ArtworkType GameList::GetSelectedArtworkType()
{
  const int default_type = static_cast<int>(DEFAULT_ARTWORK_TYPE);
  const int configured_type = Host::GetBaseIntSettingValue("GameList", "DefaultArtworkType", default_type);
  if (configured_type < 0 || configured_type >= static_cast<int>(ArtworkType::Count))
    return DEFAULT_ARTWORK_TYPE;

  return static_cast<ArtworkType>(configured_type);
}

std::string GameList::GetManualCoverImagePathForEntry(const Entry* entry)
{
  return GetManualCoverImagePath(entry->path, entry->serial, entry->title);
}

std::string GameList::GetCoverImagePathForEntry(const Entry* entry)
{
  return GetCoverImagePath(entry->path, entry->serial, entry->title);
}

std::string GameList::GetCoverImagePath(const std::string& path, const std::string& serial, const std::string& title)
{
  std::string manual_cover(GetManualCoverImagePath(path, serial, title));
  if (!manual_cover.empty())
    return manual_cover;

  const std::string artwork_path(GetDownloadedArtworkPath(serial, GetSelectedArtworkType()));
  if (!artwork_path.empty() && FileSystem::FileExists(artwork_path.c_str()))
    return artwork_path;

  return {};
}

std::string GameList::GetNewCoverImagePathForEntry(const Entry* entry, const char* new_filename, bool use_serial)
{
  const char* extension = std::strrchr(new_filename, '.');
  if (!extension)
    return {};

  std::string existing_filename = GetManualCoverImagePathForEntry(entry);
  if (!existing_filename.empty())
  {
    std::string::size_type pos = existing_filename.rfind('.');
    if (pos != std::string::npos && existing_filename.compare(pos, std::strlen(extension), extension) == 0)
      return existing_filename;
  }

  // Check for illegal characters, use serial instead.
  const std::string sanitized_name(Path::SanitizeFileName(entry->title));

  std::string name;
  if (sanitized_name != entry->title || use_serial)
    name = fmt::format("{}{}", entry->serial, extension);
  else
    name = fmt::format("{}{}", entry->title, extension);

  return Path::Combine(EmuFolders::Covers, Path::SanitizeFileName(name));
}

size_t GameList::Entry::GetReleaseDateString(char* buffer, size_t buffer_size) const
{
  if (release_date == 0)
    return StringUtil::Strlcpy(buffer, "Unknown", buffer_size);

  std::time_t date_as_time = static_cast<std::time_t>(release_date);
#ifdef _WIN32
  tm date_tm = {};
  gmtime_s(&date_tm, &date_as_time);
#else
  tm date_tm = {};
  gmtime_r(&date_as_time, &date_tm);
#endif

  return std::strftime(buffer, buffer_size, "%d %B %Y", &date_tm);
}

std::string GameList::GetPlayedTimeFile()
{
  return Path::Combine(EmuFolders::DataRoot, "playtime.dat");
}

bool GameList::ParsePlayedTimeLine(char* line, std::string& serial, PlayedTimeEntry& entry)
{
  size_t len = std::strlen(line);
  if (len != (PLAYED_TIME_LINE_LENGTH + 1)) // \n
  {
    WARNING_LOG("Malformed line: '{}'", line);
    return false;
  }

  const std::string_view serial_tok(StringUtil::StripWhitespace(std::string_view(line, PLAYED_TIME_SERIAL_LENGTH)));
  const std::string_view total_played_time_tok(
    StringUtil::StripWhitespace(std::string_view(line + PLAYED_TIME_SERIAL_LENGTH + 1, PLAYED_TIME_LAST_TIME_LENGTH)));
  const std::string_view last_played_time_tok(StringUtil::StripWhitespace(std::string_view(
    line + PLAYED_TIME_SERIAL_LENGTH + 1 + PLAYED_TIME_LAST_TIME_LENGTH + 1, PLAYED_TIME_TOTAL_TIME_LENGTH)));

  const std::optional<u64> total_played_time(StringUtil::FromChars<u64>(total_played_time_tok));
  const std::optional<u64> last_played_time(StringUtil::FromChars<u64>(last_played_time_tok));
  if (serial_tok.empty() || !last_played_time.has_value() || !total_played_time.has_value())
  {
    WARNING_LOG("Malformed line: '{}'", line);
    return false;
  }

  serial = serial_tok;
  entry.last_played_time = static_cast<std::time_t>(last_played_time.value());
  entry.total_played_time = static_cast<std::time_t>(total_played_time.value());
  return true;
}

std::string GameList::MakePlayedTimeLine(const std::string& serial, const PlayedTimeEntry& entry)
{
  return fmt::format("{:<{}} {:<{}} {:<{}}\n", serial, static_cast<unsigned>(PLAYED_TIME_SERIAL_LENGTH),
                     entry.total_played_time, static_cast<unsigned>(PLAYED_TIME_TOTAL_TIME_LENGTH),
                     entry.last_played_time, static_cast<unsigned>(PLAYED_TIME_LAST_TIME_LENGTH));
}

GameList::PlayedTimeMap GameList::LoadPlayedTimeMap(const std::string& path)
{
  PlayedTimeMap ret;

  // Use write mode here, even though we're not writing, so we can lock the file from other updates.
  Error error;
  auto fp = FileSystem::OpenExistingOrCreateManagedCFile(path.c_str(), 0, &error);
  if (!fp)
  {
    ERROR_LOG("Failed to open '{}' for load: {}", Path::GetFileName(path), error.GetDescription());
    return ret;
  }

#ifndef _WIN32
  FileSystem::POSIXLock flock(fp.get());
#endif

  char line[256];
  while (std::fgets(line, sizeof(line), fp.get()))
  {
    std::string serial;
    PlayedTimeEntry entry;
    if (!ParsePlayedTimeLine(line, serial, entry))
      continue;

    if (ret.find(serial) != ret.end())
    {
      WARNING_LOG("Duplicate entry: '{}'", serial);
      continue;
    }

    ret.emplace(std::move(serial), entry);
  }

  return ret;
}

GameList::PlayedTimeEntry GameList::UpdatePlayedTimeFile(const std::string& path, const std::string& serial,
                                                         std::time_t last_time, std::time_t add_time)
{
  const PlayedTimeEntry new_entry{last_time, add_time};

  Error error;
  auto fp = FileSystem::OpenExistingOrCreateManagedCFile(path.c_str(), 0, &error);
  if (!fp)
  {
    ERROR_LOG("Failed to open '{}' for update: {}", Path::GetFileName(path), error.GetDescription());
    return new_entry;
  }

#ifndef _WIN32
  FileSystem::POSIXLock flock(fp.get());
#endif

  for (;;)
  {
    char line[256];
    const s64 line_pos = FileSystem::FTell64(fp.get());
    if (!std::fgets(line, sizeof(line), fp.get()))
      break;

    std::string line_serial;
    PlayedTimeEntry line_entry;
    if (!ParsePlayedTimeLine(line, line_serial, line_entry))
      continue;

    if (line_serial != serial)
      continue;

    // found it!
    line_entry.last_played_time = (last_time != 0) ? last_time : 0;
    line_entry.total_played_time = (last_time != 0) ? (line_entry.total_played_time + add_time) : 0;

    std::string new_line(MakePlayedTimeLine(serial, line_entry));
    if (FileSystem::FSeek64(fp.get(), line_pos, SEEK_SET) != 0 ||
        std::fwrite(new_line.data(), new_line.length(), 1, fp.get()) != 1)
    {
      ERROR_LOG("Failed to update '{}'.", path);
    }

    return line_entry;
  }

  if (last_time != 0)
  {
    // new entry.
    std::string new_line(MakePlayedTimeLine(serial, new_entry));
    if (FileSystem::FSeek64(fp.get(), 0, SEEK_END) != 0 ||
        std::fwrite(new_line.data(), new_line.length(), 1, fp.get()) != 1)
    {
      ERROR_LOG("Failed to write '{}'.", path);
    }
  }

  return new_entry;
}

void GameList::AddPlayedTimeForSerial(const std::string& serial, std::time_t last_time, std::time_t add_time)
{
  if (serial.empty())
    return;

  const PlayedTimeEntry pt(UpdatePlayedTimeFile(GetPlayedTimeFile(), serial, last_time, add_time));
  VERBOSE_LOG("Add {} seconds play time to {} -> now {}", static_cast<unsigned>(add_time), serial.c_str(),
              static_cast<unsigned>(pt.total_played_time));

  std::unique_lock<std::recursive_mutex> lock(s_mutex);
  for (GameList::Entry& entry : s_entries)
  {
    if (entry.serial != serial)
      continue;

    entry.last_played_time = pt.last_played_time;
    entry.total_played_time = pt.total_played_time;
  }

}

void GameList::ClearPlayedTimeForSerial(const std::string& serial)
{
  if (serial.empty())
    return;

  UpdatePlayedTimeFile(GetPlayedTimeFile(), serial, 0, 0);

  std::unique_lock<std::recursive_mutex> lock(s_mutex);
  for (GameList::Entry& entry : s_entries)
  {
    if (entry.serial != serial)
      continue;

    entry.last_played_time = 0;
    entry.total_played_time = 0;
  }
}

std::time_t GameList::GetCachedPlayedTimeForSerial(const std::string& serial)
{
  if (serial.empty())
    return 0;

  std::unique_lock<std::recursive_mutex> lock(s_mutex);
  for (GameList::Entry& entry : s_entries)
  {
    if (entry.serial == serial)
      return entry.total_played_time;
  }

  return 0;
}

TinyString GameList::FormatTimestamp(std::time_t timestamp)
{
  TinyString ret;

  if (timestamp == 0)
  {
    ret = TRANSLATE("GameList", "Never");
  }
  else
  {
    struct tm ctime = {};
    struct tm ttime = {};
    const std::time_t ctimestamp = std::time(nullptr);
#ifdef _MSC_VER
    localtime_s(&ctime, &ctimestamp);
    localtime_s(&ttime, &timestamp);
#else
    localtime_r(&ctimestamp, &ctime);
    localtime_r(&timestamp, &ttime);
#endif

    if (ctime.tm_year == ttime.tm_year && ctime.tm_yday == ttime.tm_yday)
    {
      ret = TRANSLATE("GameList", "Today");
    }
    else if ((ctime.tm_year == ttime.tm_year && ctime.tm_yday == (ttime.tm_yday + 1)) ||
             (ctime.tm_yday == 0 && (ctime.tm_year - 1) == ttime.tm_year))
    {
      ret = TRANSLATE("GameList", "Yesterday");
    }
    else
    {
      char buf[128];
      std::strftime(buf, std::size(buf), "%x", &ttime);
      ret.assign(buf);
    }
  }

  return ret;
}

TinyString GameList::FormatTimespan(std::time_t timespan, bool long_format)
{
  const u32 hours = static_cast<u32>(timespan / 3600);
  const u32 minutes = static_cast<u32>((timespan % 3600) / 60);
  const u32 seconds = static_cast<u32>((timespan % 3600) % 60);

  TinyString ret;
  if (!long_format)
  {
    if (hours >= 100)
      ret.format(TRANSLATE_FS("GameList", "{}h {}m"), hours, minutes);
    else if (hours > 0)
      ret.format(TRANSLATE_FS("GameList", "{}h {}m {}s"), hours, minutes, seconds);
    else if (minutes > 0)
      ret.format(TRANSLATE_FS("GameList", "{}m {}s"), minutes, seconds);
    else if (seconds > 0)
      ret.format(TRANSLATE_FS("GameList", "{}s"), seconds);
    else
      ret = TRANSLATE_SV("GameList", "None");
  }
  else
  {
    if (hours > 0)
      ret.assign(TRANSLATE_PLURAL_STR("GameList", "%n hours", "", hours));
    else
      ret.assign(TRANSLATE_PLURAL_STR("GameList", "%n minutes", "", minutes));
  }

  return ret;
}

std::vector<std::pair<std::string, const GameList::Entry*>>
GameList::GetMatchingEntriesForSerial(const std::span<const std::string> serials)
{
  std::vector<std::pair<std::string, const GameList::Entry*>> ret;
  ret.reserve(serials.size());

  for (const std::string& serial : serials)
  {
    const Entry* matching_entry = nullptr;
    bool has_multiple_entries = false;

    for (const Entry& entry : s_entries)
    {
      if (entry.serial != serial)
        continue;

      if (!matching_entry)
        matching_entry = &entry;
      else
        has_multiple_entries = true;
    }

    if (!matching_entry)
      continue;

    if (!has_multiple_entries)
    {
      ret.emplace_back(matching_entry->title, matching_entry);
      continue;
    }

    // Have to add all matching files.
    for (const Entry& entry : s_entries)
    {
      if (entry.serial != serial)
        continue;

      ret.emplace_back(Path::GetFileName(entry.path), &entry);
    }
  }

  return ret;
}

bool GameList::DownloadArcadeArtwork(ArtworkType type, ProgressCallback* progress,
                                      std::function<void(const Entry*, std::string)> save_callback)
{
  if (!progress)
    progress = ProgressCallback::NullProgressCallback;

  const size_t type_index = GetArtworkTypeIndex(type);
  if (type_index >= s_artwork_directory_names.size())
  {
    progress->DisplayError("Invalid ArcadeDuck artwork type.");
    return false;
  }

  const std::string artwork_directory =
    fmt::format("{}" FS_OSPATH_SEPARATOR_STR "arcadeduck" FS_OSPATH_SEPARATOR_STR "{}",
                EmuFolders::Covers, s_artwork_directory_names[type_index]);
  if (!FileSystem::EnsureDirectoryExists(artwork_directory.c_str(), true))
  {
    progress->FormatStatusText("Failed to create artwork directory: {}", artwork_directory);
    return false;
  }

  struct ArtworkDownload
  {
    std::string entry_path;
    std::string url;
    std::string write_path;
  };

  std::vector<ArtworkDownload> downloads;
  {
    std::unique_lock lock(s_mutex);
    for (const GameList::Entry& entry : s_entries)
    {
      if (entry.serial.empty() || !GetManualCoverImagePathForEntry(&entry).empty())
        continue;

      std::string write_path(GetDownloadedArtworkPath(entry.serial, type));
      if (write_path.empty() || FileSystem::FileExists(write_path.c_str()))
        continue;

      downloads.push_back(
        {entry.path,
         fmt::format("{}/{}/{}.{}", ARCADEDUCK_ARTWORK_BASE_URL, s_artwork_directory_names[type_index],
                     Path::URLEncode(entry.serial), s_artwork_extensions[type_index]),
         std::move(write_path)});
    }
  }

  if (downloads.empty())
    return true;

  std::unique_ptr<HTTPDownloader> downloader(HTTPDownloader::Create(Host::GetHTTPUserAgent()));
  if (!downloader)
  {
    progress->DisplayError("Failed to create HTTP downloader.");
    return false;
  }

  progress->SetCancellable(true);
  progress->SetProgressRange(static_cast<u32>(downloads.size()));

  for (ArtworkDownload& download : downloads)
  {
    if (progress->IsCancelled())
      break;

    {
      std::unique_lock lock(s_mutex);
      const GameList::Entry* entry = GetEntryForPath(download.entry_path);
      if (!entry || !GetManualCoverImagePathForEntry(entry).empty() ||
          FileSystem::FileExists(download.write_path.c_str()))
      {
        progress->IncrementProgressValue();
        continue;
      }

      progress->FormatStatusText("Downloading {} artwork for {}...", s_artwork_display_names[type_index], entry->title);
    }

    downloader->CreateRequest(
      std::move(download.url),
      [&save_callback, entry_path = download.entry_path, write_path = std::move(download.write_path)](
        s32 status_code, const std::string&, HTTPDownloader::Request::Data data) mutable {
        if (status_code != HTTPDownloader::HTTP_STATUS_OK || data.empty())
          return;

        std::unique_lock lock(s_mutex);
        const GameList::Entry* entry = GetEntryForPath(entry_path);
        if (!entry || !GetManualCoverImagePathForEntry(entry).empty() || FileSystem::FileExists(write_path.c_str()))
          return;

        if (FileSystem::WriteBinaryFile(write_path.c_str(), data.data(), data.size()) && save_callback)
          save_callback(entry, std::move(write_path));
      });
    downloader->WaitForAllRequests();
    progress->IncrementProgressValue();
  }

  return true;
}

std::string GameList::GetCustomPropertiesFile()
{
  return Path::Combine(EmuFolders::DataRoot, "custom_properties.ini");
}

void GameList::SaveCustomTitleForPath(const std::string& path, const std::string& custom_title)
{
  INISettingsInterface custom_attributes_ini(GetCustomPropertiesFile());
  custom_attributes_ini.Load();

  if (!custom_title.empty())
  {
    custom_attributes_ini.SetStringValue(path.c_str(), "Title", custom_title.c_str());
  }
  else
  {
    custom_attributes_ini.DeleteValue(path.c_str(), "Title");
    custom_attributes_ini.RemoveEmptySections();
  }

  Error error;
  if (!custom_attributes_ini.Save(&error))
  {
    ERROR_LOG("Failed to save custom attributes: {}", error.GetDescription());
    return;
  }

  if (!custom_title.empty())
  {
    // Can skip the rescan and just update the value directly.
    auto lock = GetLock();
    Entry* entry = GetMutableEntryForPath(path);
    if (entry)
    {
      entry->title = custom_title;
      entry->has_custom_title = true;
    }
  }
  else
  {
    // Let the cache update by rescanning. Only need to do this on deletion, to get the original value.
    RescanCustomAttributesForPath(path, custom_attributes_ini);
  }
}

std::string GameList::GetCustomTitleForPath(const std::string_view path)
{
  std::string ret;

  std::unique_lock lock(s_mutex);
  const GameList::Entry* entry = GetEntryForPath(path);
  if (entry && entry->has_custom_title)
    ret = entry->title;

  return ret;
}

std::string GameList::GetGameIconPath(std::string_view serial)
{
  if (serial.empty())
    return {};

  const std::string path = Path::Combine(EmuFolders::GameIcons, TinyString::from_format("{}.png", serial));
  return FileSystem::FileExists(path.c_str()) ? path : std::string();
}
