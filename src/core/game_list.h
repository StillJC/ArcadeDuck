// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once

#include "types.h"

#include "common/small_string.h"

#include <ctime>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

class ByteStream;
class ProgressCallback;

struct SystemBootParameters;

namespace GameList {
struct Entry
{
  std::string path;
  std::string serial;
  std::string title;
  std::string system;
  std::string genre;
  std::string publisher;
  std::string developer;
  s64 file_size = 0;
  u64 uncompressed_size = 0;
  std::time_t last_modified_time = 0;
  std::time_t last_played_time = 0;
  std::time_t total_played_time = 0;

  u64 release_date = 0;
  u8 min_players = 1;
  u8 max_players = 1;
  bool has_custom_title = false;

  size_t GetReleaseDateString(char* buffer, size_t buffer_size) const;
};

using EntryList = std::vector<Entry>;

enum class ArtworkType : u8
{
  ClearLogo = 0,
  Box2D,
  Box3D,
  Count
};

constexpr ArtworkType DEFAULT_ARTWORK_TYPE = ArtworkType::Box3D;

bool IsScannableFilename(std::string_view path);

/// Populates a game list entry struct with information from the iso/elf.
/// Do *not* call while the system is running, it will mess with CDVD state.
bool PopulateEntryFromPath(const std::string& path, Entry* entry);

// Game list access. It's the caller's responsibility to hold the lock while manipulating the entry in any way.
std::unique_lock<std::recursive_mutex> GetLock();
const Entry* GetEntryByIndex(u32 index);
const Entry* GetEntryForPath(std::string_view path);
const Entry* GetEntryBySerial(std::string_view serial);
u32 GetEntryCount();

bool IsGameListLoaded();

/// Populates the game list with files in the configured directories.
/// If invalidate_cache is set, all files will be re-scanned.
/// If only_cache is set, no new files will be scanned, only those present in the cache.
void Refresh(bool invalidate_cache, bool only_cache = false, ProgressCallback* progress = nullptr);

/// Moves the current game list, which can be temporarily displayed in the UI until refresh completes.
/// The caller **must** call Refresh() afterward, otherwise it will be permanently lost.
EntryList TakeEntryList();

/// Add played time for the specified serial.
void AddPlayedTimeForSerial(const std::string& serial, std::time_t last_time, std::time_t add_time);
void ClearPlayedTimeForSerial(const std::string& serial);

/// Returns the total time played for a game. Requires the game to be scanned in the list.
std::time_t GetCachedPlayedTimeForSerial(const std::string& serial);

/// Formats a timestamp to something human readable (e.g. Today, Yesterday, 10/11/12).
TinyString FormatTimestamp(std::time_t timestamp);

/// Formats a timespan to something human readable (e.g. 1h2m3s or 1 hour).
TinyString FormatTimespan(std::time_t timespan, bool long_format = false);

ArtworkType GetSelectedArtworkType();
std::string GetManualCoverImagePathForEntry(const Entry* entry);
std::string GetCoverImagePathForEntry(const Entry* entry);
std::string GetCoverImagePath(const std::string& path, const std::string& serial, const std::string& title);
std::string GetNewCoverImagePathForEntry(const Entry* entry, const char* new_filename, bool use_serial);

/// Returns a list of (title, entry) for entries matching serials. Titles will match the gamedb title,
/// except when two files have the same serial, in which case the filename will be used instead.
std::vector<std::pair<std::string, const Entry*>>
GetMatchingEntriesForSerial(const std::span<const std::string> serials);

/// Downloads missing default artwork from the ArcadeDuck artwork repository.
/// Manually assigned covers always take priority and are never overwritten.
bool DownloadArcadeArtwork(ArtworkType type, ProgressCallback* progress = nullptr,
                           std::function<void(const Entry*, std::string)> save_callback = {});

// Custom properties support
void SaveCustomTitleForPath(const std::string& path, const std::string& custom_title);
std::string GetCustomTitleForPath(const std::string_view path);
std::string GetGameIconPath(std::string_view serial);

}; // namespace GameList

namespace Host {
/// Asynchronously starts refreshing the game list.
void RefreshGameListAsync(bool invalidate_cache);

/// Cancels game list refresh, if there is one in progress.
void CancelGameListRefresh();
} // namespace Host
