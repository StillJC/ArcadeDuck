// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/arcade_control_registry.h"
#include "core/types.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcade::Database {

enum class WorkingStatus : u8
{
  Unknown,
  Preliminary,
  Working,
  NotWorking,
};

enum class Orientation : u8
{
  Horizontal,
  Vertical,
};

enum class MediaType : u8
{
  Unknown,
  CHDCDROM,
  CHDHardDisk,
  CDROM,
  HardDisk,
  DVDROM,
  GDROM,
  CompactFlash,
  NAND,
  LaserDisc,
  SecurityDevice,
};

struct Property
{
  std::string name;
  std::string value;
};

struct ROMSegmentDefinition
{
  std::string operation;
  u32 offset = 0;
  u32 length = 0;
  u32 source_offset = 0;
  u32 group_size = 1;
  u32 skip = 0;
  bool reverse = false;
};

struct ROMDefinition
{
  std::string role;
  std::string region;
  std::string name;
  std::string sha1;
  std::string bios_variant;
  u32 crc32 = 0;
  u32 size = 0;
  u32 offset = 0;
  u32 interleave = 1;
  u32 group_size = 1;
  u32 skip = 0;
  bool has_crc32 = false;
  bool required = true;
  bool bad_dump = false;
  bool word_swap = false;
  std::vector<ROMSegmentDefinition> segments;
};

struct ROMRegionDefinition
{
  std::string id;
  std::string endianness;
  u32 size = 0;
  u32 width = 0;
  u8 erase_value = 0;
  bool has_erase_value = false;
};

struct UndumpedROMDefinition
{
  std::string role;
  std::string region;
  std::string name;
  u32 size = 0;
  std::optional<bool> required;
};

struct FirmwareDefinition
{
  std::string id;
  std::string archive_name;
  std::vector<ROMRegionDefinition> rom_regions;
  std::vector<ROMDefinition> roms;
  std::vector<UndumpedROMDefinition> undumped_roms;
};

struct SystemDefinition
{
  std::string id;
  std::string name;
  std::string manufacturer;
  std::string hardware_family;
  std::string machine_handler;
  std::string bios_profile;
  u16 release_year = 0;
  u16 cpu_clock_percent = 100;
  u8 max_players = 1;
  bool supports_lightgun = false;
  bool supports_trackball = false;
  bool supports_special_controls = false;
  bool supports_network = false;
  std::vector<FirmwareDefinition> firmware;
  std::vector<Property> properties;
};

struct MediaDefinition
{
  std::string role;
  MediaType type = MediaType::Unknown;
  std::string name;
  std::string sha1;
  bool required = true;
  bool read_only = true;
  bool companion = true;
};

struct PersistentStorageDefinition
{
  std::string id;
  std::string type;
  std::string default_rom_role;
  u32 size = 0;
  bool shared = false;
};

struct PortDefinition
{
  ArcadeControllerType controller_type = ArcadeControllerType::None;
  ArcadeJoystickMode joystick_mode = ArcadeJoystickMode::None;
  std::string layout;
};

struct DisplayDefinition
{
  Orientation orientation = Orientation::Horizontal;
  std::optional<DisplayRotation> rotation;
  u16 aspect_numerator = 4;
  u16 aspect_denominator = 3;
  u8 screens = 1;
  u8 monitor_crop_top = 0;
  u8 monitor_crop_bottom = 0;
  s8 monitor_offset_x = 0;
  float refresh_hz = 0.0f;
};

struct GameDefinition
{
  std::string id;
  std::string archive_name;
  std::string title;
  std::string sort_title;
  std::string native_title;
  std::string system_id;
  std::string parent;
  std::string manufacturer;
  std::string developer;
  std::string distributor;
  std::string genre;
  std::string region;
  std::string revision;
  std::string board_code;
  std::string hardware_profile;
  std::string set_format;
  u16 year = 0;
  u16 cpu_clock_percent = 100;
  u8 min_players = 1;
  u8 max_players = 1;
  WorkingStatus status = WorkingStatus::Unknown;
  DisplayDefinition display;
  std::array<PortDefinition, NUM_ARCADE_CONTROLLER_PORTS> ports{};
  std::vector<ROMDefinition> roms;
  std::vector<MediaDefinition> media;
  std::vector<PersistentStorageDefinition> persistent_storage;
  std::vector<Property> properties;
};

struct ControlLayoutBindingDefinition
{
  std::string key;
  std::string display_name;
  ArcadeControlBindingKind kind = ArcadeControlBindingKind::Button;
};

struct ControlLayoutDefinition
{
  std::string id;
  std::string display_name;
  ArcadeControllerType controller_type = ArcadeControllerType::None;
  ArcadeJoystickMode joystick_mode = ArcadeJoystickMode::None;
  std::vector<ControlLayoutBindingDefinition> bindings;
};

bool EnsureLoaded();
void Unload();
bool IsLoaded();

std::span<const SystemDefinition> GetSystems();
std::span<const GameDefinition> GetGames();
std::span<const ControlLayoutDefinition> GetControlLayouts();

const SystemDefinition* GetSystem(std::string_view id);
const GameDefinition* GetGame(std::string_view id);
const ControlLayoutDefinition* GetControlLayout(std::string_view id);

const FirmwareDefinition* GetFirmwareDefinition(const SystemDefinition& system, std::string_view id);
const FirmwareDefinition* ResolveFirmwareProfile(const SystemDefinition& system);

bool IsArchivePath(std::string_view path);
std::string_view GetSetNameFromArchivePath(std::string_view path);
const GameDefinition* IdentifyArchive(std::string_view path);

const ROMDefinition* GetROMByRole(const GameDefinition& game, std::string_view role);
const MediaDefinition* GetMediaByRole(const GameDefinition& game, std::string_view role);
std::string GetMediaFilename(const MediaDefinition& media);
std::string GetCompanionMediaPath(std::string_view archive_path, const GameDefinition& game,
                                  const MediaDefinition& media);

const char* GetWorkingStatusName(WorkingStatus status);
const char* GetMediaTypeName(MediaType type);

} // namespace Arcade::Database
