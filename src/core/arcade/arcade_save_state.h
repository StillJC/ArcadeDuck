// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Error;

namespace Arcade::SaveState {

using Hash = std::array<u8, 16>;

/// Largest metadata block accepted in a save state header (stored in place of the media path).
constexpr u32 MAX_METADATA_SIZE = 64 * 1024;

enum : s32
{
  SLOT_RESUME = -1,
  SLOT_NONE = 0,
};

struct NVRAMFingerprint
{
  std::string id;
  u32 size = 0;
  Hash hash = {};
};

/// Identity and compatibility record stored uncompressed ahead of the state data. Filenames carry no meaning.
struct Metadata
{
  std::string handler_id;
  u32 handler_state_version = 0;
  std::string game_id;
  std::string game_title;
  std::string archive_base;
  Hash identity = {};
  u64 created_time = 0;
  s32 slot = SLOT_NONE;
  std::string label;
  Hash save_id = {};
  std::vector<NVRAMFingerprint> nvram;

  /// CPUExecutionMode the state was saved with. Loading requires the same mode.
  u8 cpu_execution_mode = 0;
};

Hash HashBytes(std::span<const u8> data);
std::string FormatHash(const Hash& hash);
Hash GenerateSaveID();

/// Fails without producing output when the metadata does not fit in MAX_METADATA_SIZE.
bool SerializeMetadata(const Metadata& metadata, std::vector<u8>* out, Error* error);
bool ParseMetadata(std::span<const u8> data, Metadata* metadata, Error* error);

/// Tracks the bytes last read from or written to one persistence file, so a file changed outside the
/// emulator while the machine is running is detected instead of being validated against stale contents.
class PersistentFile
{
public:
  void SetPath(std::string path);
  const std::string& GetPath() const { return m_path; }

  /// Records the exact contents now present on disk after a successful read or write.
  void RecordDiskContents(std::span<const u8> data);

  /// Records that the file is known to be absent.
  void RecordDiskAbsent();

  /// Fails when the file no longer holds the contents last recorded by the emulator.
  bool CheckDiskUnchanged(std::string_view component_id, Error* error) const;

private:
  std::string m_path;
  Hash m_disk_hash = {};
  bool m_disk_known = false;
  bool m_disk_present = false;
};

/// One persistent NVRAM component of the running machine. Contents are the live device memory, which is the
/// authoritative copy while the machine runs.
struct NVRAMComponent
{
  std::string_view id;
  std::span<const u8> contents;
  const PersistentFile* file = nullptr;
};

std::vector<NVRAMFingerprint> FingerprintNVRAM(std::span<const NVRAMComponent> components);

/// Compares current NVRAM against a save's fingerprints. Every difference is logged; returns false on any.
bool CompareNVRAM(std::span<const NVRAMComponent> components, std::span<const NVRAMFingerprint> saved, Error* error);

/// True while a state is being restored or rolled back. Board persistence writers must not run meanwhile.
bool IsRestoring();

class RestoreScope
{
public:
  RestoreScope();
  ~RestoreScope();
  RestoreScope(const RestoreScope&) = delete;
  RestoreScope& operator=(const RestoreScope&) = delete;
};

struct Entry
{
  std::string path;
  Metadata metadata;
  u64 file_time = 0;
  bool duplicate_slot = false;
};

/// Reads only the header and metadata of an arcade save. Returns false for non-arcade or malformed files.
bool ReadFileMetadata(const char* path, Metadata* metadata, Error* error);

/// Lists every arcade save in the save-state folder whose internal identity names this game.
std::vector<Entry> FindSaves(std::string_view game_id);

/// Default filename for a new save: <base>.slotNN.sav, <base>.resume.sav or <base>.<label>.sav.
std::string GetDefaultFileName(std::string_view archive_base, s32 slot, std::string_view label);

/// Returns the owning file for a slot, or the default path for a new one. When more than one save claims the
/// slot, returns the first owner and sets duplicate; the duplicate must be resolved before the slot is used.
std::string ResolveSlotPath(std::string_view game_id, std::string_view archive_base, s32 slot,
                            bool* duplicate = nullptr);

/// Infers the slot a new file represents from the default naming convention.
s32 GetSlotFromFileName(std::string_view path);

/// Rewrites a save's slot/label metadata in place, keeping its state data. Used for duplicate resolution.
bool RewriteSlot(const char* path, s32 slot, std::string_view label, Error* error);

std::string GetSlotDisplayName(s32 slot);

} // namespace Arcade::SaveState
