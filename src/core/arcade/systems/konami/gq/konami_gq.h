// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Error;
namespace BIOS {
struct Image;
}

namespace Konami {

struct GQGameDefinition
{
  std::string_view set_name;
  std::string_view title;
  std::string_view hardware_profile;
  std::string_view sound_program_member_name;
  u32 sound_program_crc32;
  std::string_view sound_program_sha1;
  std::string_view pcm_member_name;
  u32 pcm_crc32;
  std::string_view pcm_sha1;
  std::string_view chd_disk_name;
  std::string_view chd_sha1;
  u32 rom_size = 0x80000;
};

struct GQLoadedContent
{
  const GQGameDefinition* definition;
  std::string_view set_name;
  std::vector<u8> sound_program;
  std::vector<u8> pcm_samples;
  std::string chd_path;
  std::string_view chd_sha1;
};

/// Returns the immutable definition for a case-insensitive exact arcade game-id match.
const GQGameDefinition* GetGQGameDefinition(std::string_view set_name);

/// Returns true when path names a ZIP archive. The extension comparison is case-insensitive.
bool IsGQArchivePath(std::string_view path);

/// Returns the archive basename without its ZIP extension, or an empty view for non-ZIP paths.
std::string_view GetGQSetNameFromArchivePath(std::string_view path);

/// Identifies an exact Konami GQ arcade game from a ZIP archive path.
const GQGameDefinition* IdentifyGQArchive(std::string_view path);

/// Returns the physical filename for a definition's extensionless CHD media basename.
std::string GetGQCHDFilename(const GQGameDefinition& definition);

/// Returns the complete companion CHD path for a selected GQ ZIP archive.
std::string GetGQCompanionCHDPath(std::string_view zip_path, const GQGameDefinition& definition);

/// Loads and validates the GQ sound ROMs and companion hard-disk CHD.
std::optional<GQLoadedContent> LoadGQContent(const char* archive_path, Error* error);

/// Initializes the GQ board lifecycle, hard-disk CHD, and NCR53CF96 host adapter.
bool InitializeGQ(const BIOS::Image& bios, GQLoadedContent content, std::string_view persistence_directory,
                  Error* error);
void ResetGQ();
void ShutdownGQ();
bool IsGQActive();
bool HasValidGQHardDiskContent();
std::string_view GetGQSetName();
std::string_view GetGQTitle();
std::string_view GetGQCHDPath();
void WriteGQCabinetOutput(u32 size, u32 offset, u32 value);
u32 ReadGQInput(u32 size, u32 offset);
void ProcessGQFrame();
void ResetGQArcadeInputs();
void SetGQArcadeButton(u32 player, u32 button_mask, bool pressed);
void SetGQTest(bool pressed);
void SetGQLightgunPosition(u32 player, float x, float y);

bool IsGQSet(std::string_view set_name);

} // namespace Konami
