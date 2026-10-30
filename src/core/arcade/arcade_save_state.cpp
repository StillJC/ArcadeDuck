// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/arcade_save_state.h"
#include "core/save_state_version.h"
#include "core/settings.h"
#include "core/system.h"

#include "common/binary_reader_writer.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include "xxhash.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <random>

Log_SetChannel(ArcadeSaveState);

namespace Arcade::SaveState {
namespace {

constexpr u32 METADATA_VERSION = 2;
constexpr u32 MAX_NVRAM_COMPONENTS = 64;

u32 s_restore_depth = 0;

bool ReadHeader(std::FILE* fp, SAVE_STATE_HEADER* header, s64* file_size, Error* error)
{
  *file_size = FileSystem::FSize64(fp, error);
  if (*file_size < static_cast<s64>(sizeof(SAVE_STATE_HEADER)))
  {
    Error::SetStringView(error, "File is too small to be a save state.");
    return false;
  }

  if (!FileSystem::FSeek64(fp, 0, SEEK_SET, error) || std::fread(header, sizeof(*header), 1, fp) != 1)
  {
    Error::SetStringView(error, "Failed to read save state header.");
    return false;
  }

  if (header->magic != ARCADE_SAVE_STATE_MAGIC)
  {
    Error::SetStringView(error, "Not an arcade save state.");
    return false;
  }

  if (header->media_path_length == 0 || header->media_path_length > MAX_METADATA_SIZE ||
      static_cast<s64>(header->offset_to_media_path) + header->media_path_length > *file_size ||
      static_cast<s64>(header->offset_to_screenshot) + header->screenshot_compressed_size > *file_size ||
      static_cast<s64>(header->offset_to_data) + header->data_compressed_size > *file_size)
  {
    Error::SetStringView(error, "Save state header is corrupted.");
    return false;
  }

  return true;
}

bool ReadRegion(std::FILE* fp, u32 offset, u32 size, std::vector<u8>* out, Error* error)
{
  out->resize(size);
  if (size == 0)
    return true;

  if (!FileSystem::FSeek64(fp, offset, SEEK_SET, error) || std::fread(out->data(), size, 1, fp) != 1)
  {
    Error::SetStringView(error, "Failed to read save state contents.");
    return false;
  }

  return true;
}

} // namespace

Hash HashBytes(std::span<const u8> data)
{
  const XXH128_hash_t h = XXH3_128bits(data.data(), data.size());
  Hash out;
  std::memcpy(out.data(), &h.low64, sizeof(h.low64));
  std::memcpy(out.data() + sizeof(h.low64), &h.high64, sizeof(h.high64));
  return out;
}

std::string FormatHash(const Hash& hash)
{
  std::string out;
  out.reserve(hash.size() * 2);
  for (const u8 b : hash)
    out += fmt::format("{:02x}", b);
  return out;
}

Hash GenerateSaveID()
{
  std::random_device rd;
  Hash id;
  for (size_t i = 0; i < id.size(); i += sizeof(u32))
  {
    const u32 v = rd();
    std::memcpy(id.data() + i, &v, sizeof(v));
  }
  return id;
}

bool SerializeMetadata(const Metadata& metadata, std::vector<u8>* out, Error* error)
{
  std::vector<u8> buffer(MAX_METADATA_SIZE);
  BinarySpanWriter writer(buffer);
  bool ok =
    writer.WriteU32(METADATA_VERSION) && writer.WriteSizePrefixedString(metadata.handler_id) &&
    writer.WriteU32(metadata.handler_state_version) && writer.WriteSizePrefixedString(metadata.game_id) &&
    writer.WriteSizePrefixedString(metadata.game_title) && writer.WriteSizePrefixedString(metadata.archive_base) &&
    writer.Write(metadata.identity.data(), metadata.identity.size()) && writer.WriteU64(metadata.created_time) &&
    writer.WriteS32(metadata.slot) && writer.WriteSizePrefixedString(metadata.label) &&
    writer.Write(metadata.save_id.data(), metadata.save_id.size()) && metadata.nvram.size() <= MAX_NVRAM_COMPONENTS &&
    writer.WriteU32(static_cast<u32>(metadata.nvram.size()));
  for (size_t i = 0; ok && i < metadata.nvram.size(); i++)
  {
    const NVRAMFingerprint& fp = metadata.nvram[i];
    ok =
      writer.WriteSizePrefixedString(fp.id) && writer.WriteU32(fp.size) && writer.Write(fp.hash.data(), fp.hash.size());
  }
  ok = ok && writer.WriteU8(metadata.cpu_execution_mode);
  if (!ok)
  {
    Error::SetStringView(error, "The save state information is too large to store.");
    return false;
  }

  buffer.resize(writer.GetBufferWritten());
  *out = std::move(buffer);
  return true;
}

bool ParseMetadata(std::span<const u8> data, Metadata* metadata, Error* error)
{
  BinarySpanReader reader(data);
  u32 version = 0;
  if (!reader.ReadU32(&version) || version != METADATA_VERSION)
  {
    Error::SetStringFmt(error, "Unsupported arcade save state metadata version {}.", version);
    return false;
  }

  u32 nvram_count = 0;
  if (!reader.ReadSizePrefixedString(&metadata->handler_id) || !reader.ReadU32(&metadata->handler_state_version) ||
      !reader.ReadSizePrefixedString(&metadata->game_id) || !reader.ReadSizePrefixedString(&metadata->game_title) ||
      !reader.ReadSizePrefixedString(&metadata->archive_base) ||
      !reader.Read(metadata->identity.data(), metadata->identity.size()) || !reader.ReadU64(&metadata->created_time) ||
      !reader.ReadS32(&metadata->slot) || !reader.ReadSizePrefixedString(&metadata->label) ||
      !reader.Read(metadata->save_id.data(), metadata->save_id.size()) || !reader.ReadU32(&nvram_count) ||
      nvram_count > MAX_NVRAM_COMPONENTS)
  {
    Error::SetStringView(error, "Arcade save state metadata is truncated or corrupted.");
    return false;
  }

  metadata->nvram.clear();
  metadata->nvram.resize(nvram_count);
  for (NVRAMFingerprint& fp : metadata->nvram)
  {
    if (!reader.ReadSizePrefixedString(&fp.id) || !reader.ReadU32(&fp.size) ||
        !reader.Read(fp.hash.data(), fp.hash.size()) || fp.id.empty())
    {
      Error::SetStringView(error, "Arcade save state NVRAM metadata is truncated or corrupted.");
      return false;
    }
  }

  if (!reader.ReadU8(&metadata->cpu_execution_mode) ||
      metadata->cpu_execution_mode >= static_cast<u8>(CPUExecutionMode::Count))
  {
    Error::SetStringView(error, "Arcade save state metadata is truncated or corrupted.");
    return false;
  }

  if (metadata->handler_id.empty() || metadata->game_id.empty() || metadata->slot < SLOT_RESUME ||
      metadata->slot > System::PER_GAME_SAVE_STATE_SLOTS)
  {
    Error::SetStringView(error, "Arcade save state metadata is missing required fields.");
    return false;
  }

  return true;
}

void PersistentFile::SetPath(std::string path)
{
  m_path = std::move(path);
  m_disk_known = false;
  m_disk_present = false;
}

void PersistentFile::RecordDiskContents(std::span<const u8> data)
{
  m_disk_hash = HashBytes(data);
  m_disk_known = true;
  m_disk_present = true;
}

void PersistentFile::RecordDiskAbsent()
{
  m_disk_known = true;
  m_disk_present = false;
}

bool PersistentFile::CheckDiskUnchanged(std::string_view component_id, Error* error) const
{
  if (!m_disk_known || m_path.empty())
  {
    ERROR_LOG("NVRAM component '{}' has no recorded persistence state.", component_id);
    Error::SetStringFmt(error, "NVRAM component '{}' has no recorded persistence state.", component_id);
    return false;
  }

  std::optional<DynamicHeapArray<u8>> data;
  if (FileSystem::FileExists(m_path.c_str()))
  {
    Error read_error;
    data = FileSystem::ReadBinaryFile(m_path.c_str(), &read_error);
    if (!data.has_value())
    {
      ERROR_LOG("NVRAM component '{}' persistence '{}' is unreadable: {}", component_id, m_path,
                read_error.GetDescription());
      Error::SetStringFmt(error, "NVRAM persistence for '{}' could not be read.", component_id);
      return false;
    }
  }

  if (data.has_value() != m_disk_present ||
      (data.has_value() && HashBytes(std::span<const u8>(data->data(), data->size())) != m_disk_hash))
  {
    ERROR_LOG("NVRAM component '{}' persistence '{}' was changed outside ArcadeDuck ({}).", component_id, m_path,
              data.has_value() ? (m_disk_present ? "contents differ" : "file appeared") : "file removed");
    Error::SetStringFmt(error, "NVRAM persistence for '{}' was changed outside ArcadeDuck.", component_id);
    return false;
  }

  return true;
}

std::vector<NVRAMFingerprint> FingerprintNVRAM(std::span<const NVRAMComponent> components)
{
  std::vector<NVRAMFingerprint> out;
  out.reserve(components.size());
  for (const NVRAMComponent& c : components)
    out.push_back(NVRAMFingerprint{std::string(c.id), static_cast<u32>(c.contents.size()), HashBytes(c.contents)});
  return out;
}

bool CompareNVRAM(std::span<const NVRAMComponent> components, std::span<const NVRAMFingerprint> saved, Error* error)
{
  bool matches = true;
  for (const NVRAMComponent& c : components)
  {
    const auto it =
      std::find_if(saved.begin(), saved.end(), [&c](const NVRAMFingerprint& fp) { return fp.id == c.id; });
    if (it == saved.end())
    {
      ERROR_LOG("NVRAM mismatch: component '{}' is present now but not in the save.", c.id);
      matches = false;
      continue;
    }

    if (it->size != c.contents.size())
    {
      ERROR_LOG("NVRAM mismatch: component '{}' size {} in save, {} now.", c.id, it->size, c.contents.size());
      matches = false;
      continue;
    }

    const Hash current = HashBytes(c.contents);
    if (current != it->hash)
    {
      ERROR_LOG("NVRAM mismatch: component '{}' contents differ (save {} current {}).", c.id,
                FormatHash(it->hash).substr(0, 16), FormatHash(current).substr(0, 16));
      matches = false;
    }
  }

  for (const NVRAMFingerprint& fp : saved)
  {
    if (std::none_of(components.begin(), components.end(), [&fp](const NVRAMComponent& c) { return c.id == fp.id; }))
    {
      ERROR_LOG("NVRAM mismatch: component '{}' is in the save but not present now.", fp.id);
      matches = false;
    }
  }

  if (!matches)
    Error::SetStringView(error, "NVRAM contents differ from the save state.");

  return matches;
}

bool IsRestoring()
{
  return (s_restore_depth != 0);
}

RestoreScope::RestoreScope()
{
  s_restore_depth++;
}

RestoreScope::~RestoreScope()
{
  s_restore_depth--;
}

bool ReadFileMetadata(const char* path, Metadata* metadata, Error* error)
{
  auto fp = FileSystem::OpenManagedCFile(path, "rb", error);
  if (!fp)
    return false;

  SAVE_STATE_HEADER header;
  s64 file_size;
  std::vector<u8> data;
  return ReadHeader(fp.get(), &header, &file_size, error) &&
         ReadRegion(fp.get(), header.offset_to_media_path, header.media_path_length, &data, error) &&
         ParseMetadata(data, metadata, error);
}

std::vector<Entry> FindSaves(std::string_view game_id)
{
  std::vector<Entry> entries;
  FileSystem::FindResultsArray files;
  if (!FileSystem::FindFiles(EmuFolders::SaveStates.c_str(), "*.sav", FILESYSTEM_FIND_FILES, &files))
    return entries;

  for (FILESYSTEM_FIND_DATA& file : files)
  {
    Entry entry;
    if (!ReadFileMetadata(file.FileName.c_str(), &entry.metadata, nullptr) ||
        !StringUtil::EqualNoCase(entry.metadata.game_id, game_id))
    {
      continue;
    }

    entry.path = std::move(file.FileName);
    entry.file_time = static_cast<u64>(file.ModificationTime);
    entries.push_back(std::move(entry));
  }

  for (Entry& entry : entries)
  {
    if (entry.metadata.slot == SLOT_NONE)
      continue;

    entry.duplicate_slot = std::count_if(entries.begin(), entries.end(), [&entry](const Entry& other) {
                             return other.metadata.slot == entry.metadata.slot;
                           }) > 1;
  }

  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if ((a.metadata.slot == SLOT_NONE) != (b.metadata.slot == SLOT_NONE))
      return (b.metadata.slot == SLOT_NONE);
    if (a.metadata.slot != b.metadata.slot)
      return a.metadata.slot < b.metadata.slot;
    return a.metadata.created_time > b.metadata.created_time;
  });

  return entries;
}

std::string GetDefaultFileName(std::string_view archive_base, s32 slot, std::string_view label)
{
  if (slot == SLOT_RESUME)
    return fmt::format("{}.resume.sav", archive_base);
  if (slot > 0)
    return fmt::format("{}.slot{:02d}.sav", archive_base, slot);

  std::string name = Path::SanitizeFileName(label);
  std::replace(name.begin(), name.end(), ' ', '_');
  return fmt::format("{}.{}.sav", archive_base, name.empty() ? std::string("save") : name);
}

std::string ResolveSlotPath(std::string_view game_id, std::string_view archive_base, s32 slot, bool* duplicate)
{
  if (duplicate)
    *duplicate = false;

  const std::vector<Entry> entries = FindSaves(game_id);
  const auto owner =
    std::find_if(entries.begin(), entries.end(), [slot](const Entry& e) { return e.metadata.slot == slot; });
  if (owner != entries.end())
  {
    if (duplicate)
      *duplicate = owner->duplicate_slot;
    return owner->path;
  }

  // A file already holding the default name but not owning this slot is never replaced.
  const std::string base_name = GetDefaultFileName(archive_base, slot, {});
  std::string path = Path::Combine(EmuFolders::SaveStates, base_name);
  for (u32 n = 2; FileSystem::FileExists(path.c_str()); n++)
  {
    path = Path::Combine(EmuFolders::SaveStates, fmt::format("{}-{}.sav", Path::GetFileTitle(base_name), n));
  }

  return path;
}

s32 GetSlotFromFileName(std::string_view path)
{
  const std::string_view name = Path::GetFileName(path);
  if (StringUtil::EndsWithNoCase(name, ".resume.sav"))
    return SLOT_RESUME;

  const std::string_view::size_type pos = name.rfind(".slot");
  if (pos == std::string_view::npos || !StringUtil::EndsWithNoCase(name, ".sav"))
    return SLOT_NONE;

  const std::string_view digits = name.substr(pos + 5, name.size() - pos - 5 - 4);
  const std::optional<s32> slot = StringUtil::FromChars<s32>(digits);
  return (slot.has_value() && slot.value() >= 1 && slot.value() <= System::PER_GAME_SAVE_STATE_SLOTS) ? slot.value() :
                                                                                                        SLOT_NONE;
}

bool RewriteSlot(const char* path, s32 slot, std::string_view label, Error* error)
{
  SAVE_STATE_HEADER header;
  std::vector<u8> metadata_bytes, screenshot, data;
  Metadata metadata;
  {
    auto fp = FileSystem::OpenManagedCFile(path, "rb", error);
    s64 file_size;
    if (!fp || !ReadHeader(fp.get(), &header, &file_size, error) ||
        !ReadRegion(fp.get(), header.offset_to_media_path, header.media_path_length, &metadata_bytes, error) ||
        !ParseMetadata(metadata_bytes, &metadata, error) ||
        !ReadRegion(fp.get(), header.offset_to_screenshot, header.screenshot_compressed_size, &screenshot, error) ||
        !ReadRegion(fp.get(), header.offset_to_data, header.data_compressed_size, &data, error))
    {
      return false;
    }
  }

  if (slot != SLOT_NONE)
  {
    for (const Entry& entry : FindSaves(metadata.game_id))
    {
      if (entry.metadata.slot == slot &&
          !StringUtil::EqualNoCase(Path::Canonicalize(entry.path), Path::Canonicalize(path)))
      {
        Error::SetStringFmt(error, "{} is already used by '{}'.", GetSlotDisplayName(slot),
                            Path::GetFileName(entry.path));
        return false;
      }
    }
  }

  metadata.slot = slot;
  metadata.label = label;
  metadata.save_id = GenerateSaveID();
  if (!SerializeMetadata(metadata, &metadata_bytes, error))
    return false;

  u32 offset = sizeof(header);
  header.offset_to_media_path = offset;
  header.media_path_length = static_cast<u32>(metadata_bytes.size());
  offset += header.media_path_length;
  header.offset_to_screenshot = screenshot.empty() ? 0 : offset;
  offset += static_cast<u32>(screenshot.size());
  header.offset_to_data = offset;

  auto out = FileSystem::CreateAtomicRenamedFile(path, "wb", error);
  if (!out)
    return false;

  if (std::fwrite(&header, sizeof(header), 1, out.get()) != 1 ||
      std::fwrite(metadata_bytes.data(), metadata_bytes.size(), 1, out.get()) != 1 ||
      (!screenshot.empty() && std::fwrite(screenshot.data(), screenshot.size(), 1, out.get()) != 1) ||
      (!data.empty() && std::fwrite(data.data(), data.size(), 1, out.get()) != 1) || std::fflush(out.get()) != 0)
  {
    Error::SetErrno(error, "fwrite() failed: ", errno);
    FileSystem::DiscardAtomicRenamedFile(out);
    return false;
  }

  // Closing commits the rename; confirm the file at the path is the one just written.
  out.reset();
  Metadata written;
  if (!ReadFileMetadata(path, &written, nullptr) || written.save_id != metadata.save_id)
  {
    Error::SetStringFmt(error, "Failed to write '{}'. The save there is unchanged.", Path::GetFileName(path));
    return false;
  }

  INFO_LOG("Rewrote arcade save '{}' as {}.", Path::GetFileName(path), GetSlotDisplayName(slot));
  return true;
}

std::string GetSlotDisplayName(s32 slot)
{
  if (slot == SLOT_RESUME)
    return "resume";
  if (slot > 0)
    return fmt::format("slot {}", slot);
  return "named save";
}

} // namespace Arcade::SaveState
