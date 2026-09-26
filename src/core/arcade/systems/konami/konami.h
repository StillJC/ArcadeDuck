// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

enum class GVDeferredEXP1Range : u8
{
  Input,
  Watchdog,
  GameSpecific,
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
const GVGameDefinition* GetGVGameDefinition(std::string_view set_name);

/// Returns true when path names a ZIP archive. The extension comparison is case-insensitive.
bool IsGVArchivePath(std::string_view path);

/// Returns the archive basename without its ZIP extension, or an empty view for non-ZIP paths.
std::string_view GetGVSetNameFromArchivePath(std::string_view path);

/// Identifies an exact Konami GV arcade game from a ZIP archive path.
const GVGameDefinition* IdentifyGVArchive(std::string_view path);

/// Returns the physical filename for a definition's extensionless CHD media basename.
std::string GetGVCHDFilename(const GVGameDefinition& definition);

/// Returns the complete companion CHD path for a selected GV ZIP archive.
std::string GetGVCompanionCHDPath(std::string_view zip_path, const GVGameDefinition& definition);

/// Loads and validates the non-merged ZIP EEPROM and companion CHD for a recognized GV archive.
std::optional<GVLoadedContent> LoadGVContent(const char* archive_path, Error* error);

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

/// Initializes the Stage 1 GV board lifecycle and installs its already-validated BIOS.
bool InitializeGV(const BIOS::Image& bios, const GVLoadedContent& content, std::string_view persistence_directory,
                  Error* error);
void ResetGV();
void ShutdownGV();
bool IsGVActive();

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
/// Returns whether the active GV lifecycle retained validated companion-disc content.
bool HasValidGVDiscContent();
bool ReadGVDataSector(u32 lba, u8* buffer, u32* cdimage_lba = nullptr, u32* track_number = nullptr);
u32 BuildGVTOC(bool msf, u8 requested_track, u8* response, u32 response_size);
std::string_view GetGVSetName();
std::string_view GetGVTitle();
std::string_view GetGVPersistenceDirectory();
std::string_view GetGVCHDPath();
bool DoGVState(StateWrapper& sw);
void WriteEEPROMControl(u32 value);
bool GetEEPROMDataOutput();
u32 ReadGVPlayer1Status(u32 size, u32 offset);
u32 ReadGVPlayer2Status(u32 size, u32 offset);
u32 ReadGVPlayer34Status(u32 size, u32 offset);
void WriteGVPlayerStatus(u32 size, u32 offset, u32 value);
u32 ReadGVEEPROM(u32 size, u32 offset);
void WriteGVEEPROM(u32 size, u32 offset, u32 value);
u32 ReadGVSpecialIO(u32 size, u32 offset);
void WriteGVSpecialIO(u32 size, u32 offset, u32 value);
void WriteGVWatchdog(u32 value);
bool ProcessGVFrame();
void ResetGVArcadeInputs();
void SetGVButtons(u32 player, u32 buttons);
void SetGVControllerButtons(u32 player, u32 active_low_psx_buttons);
void SetGVArcadeButton(u32 player, u32 button_mask, bool pressed);
void SetGVTrackballPosition(u16 x, u16 y);
void AddGVTrackballDelta(u32 player, s32 x, s32 y);
void AddGVTrackballDelta(s32 x, s32 y);
void SetGVLightgunPosition(u32 player, float x, float y);
void SetGVLightgunTrigger(u32 player, bool pressed);
void SetGVLightgunShootOffscreen(u32 player, bool pressed);
void AdjustGVTokimekiHeartRate(s32 amount);
void SetGVTokimekiHeartRate(s32 rate);
void SetGVTokimekiExcitement(s32 value);
void AdjustGVTokimekiGSR(s32 amount);
void SetGVTokimekiHeartbeatPulse(bool pressed);
void AdjustGVTokimekiExcitement(s32 direction);
u32 ReadSharpFlash(u32 size, u32 offset);
void WriteSharpFlash(u32 size, u32 offset, u32 value);
u32 ReadFujitsuFlash(u32 size, u32 offset);
void WriteFujitsuFlash(u32 size, u32 offset, u32 value);
void NotifyGVDeferredEXP1Access(GVDeferredEXP1Range range, u32 physical_address, u32 width, bool is_write, u32 value);

bool IsGVSet(std::string_view set_name);
bool IsGQSet(std::string_view set_name);
const char* GetGVBIOSProfileName(GVBIOSProfile profile);

} // namespace Konami
