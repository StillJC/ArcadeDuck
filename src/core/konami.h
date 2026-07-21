// SPDX-FileCopyrightText: 2026 ArcadeDuck Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "types.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

class Error;
class StateWrapper;
namespace BIOS {
struct Image;
}

namespace Konami {

enum class GVBIOSProfile : u8
{
  KonamiGV,
};

struct GVGameDefinition
{
  std::string_view set_name;
  std::string_view title;
  GVBIOSProfile bios_profile;
  std::string_view hardware_profile;
  std::string_view eeprom_member_name;
  u32 eeprom_crc32;
  std::string_view eeprom_sha1;
  std::string_view chd_disk_name;
  std::string_view chd_sha1;
  bool bad_dump;
  u32 eeprom_size = 0x80;
};

struct GVLoadedContent
{
  const GVGameDefinition* definition;
  std::string_view set_name;
  std::array<u8, 0x80> default_eeprom;
  std::string chd_path;
  std::string_view chd_sha1;
};

/// Returns the immutable definition for a case-insensitive exact MAME set-name match.
const GVGameDefinition* GetGVGameDefinition(std::string_view set_name);

/// Returns true when path names a ZIP archive. The extension comparison is case-insensitive.
bool IsGVArchivePath(std::string_view path);

/// Returns the archive basename without its ZIP extension, or an empty view for non-ZIP paths.
std::string_view GetGVSetNameFromArchivePath(std::string_view path);

/// Identifies an exact Konami GV MAME set from a ZIP archive path.
const GVGameDefinition* IdentifyGVArchive(std::string_view path);

/// Returns the physical filename for a definition's extensionless MAME CHD disk basename.
std::string GetGVCHDFilename(const GVGameDefinition& definition);

/// Returns the complete companion CHD path for a selected GV ZIP archive.
std::string GetGVCompanionCHDPath(std::string_view zip_path, const GVGameDefinition& definition);

/// Loads and validates the MAME ZIP EEPROM and companion CHD for a recognized GV archive.
std::optional<GVLoadedContent> LoadGVContent(const char* archive_path, Error* error);

/// Initializes the Stage 1 GV board lifecycle and installs its already-validated BIOS.
bool InitializeGV(const BIOS::Image& bios, const GVLoadedContent& content, Error* error);
void ResetGV();
void ShutdownGV();
bool IsGVActive();
std::string_view GetGVSetName();
std::string_view GetGVTitle();
std::string_view GetGVPersistenceDirectory();
bool DoGVState(StateWrapper& sw);
void WriteEEPROMControl(u32 value);
bool GetEEPROMDataOutput();
u32 ReadSharpFlash(u32 size, u32 offset);
void WriteSharpFlash(u32 size, u32 offset, u32 value);

bool IsGVSet(std::string_view set_name);
const char* GetGVBIOSProfileName(GVBIOSProfile profile);

} // namespace Konami
