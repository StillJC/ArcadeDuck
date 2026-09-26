// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/arcade_database.h"

#include "core/host.h"

#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include "ryml.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

Log_SetChannel(ArcadeDatabase);

namespace Arcade::Database {
namespace {

constexpr const char* DATABASE_FILENAME = "arcadedb.yaml";
constexpr u32 SUPPORTED_SCHEMA_VERSION = 1;

bool s_load_attempted = false;
bool s_loaded = false;
std::vector<SystemDefinition> s_systems;
std::vector<GameDefinition> s_games;
std::vector<ControlLayoutDefinition> s_control_layouts;
std::unordered_map<std::string, u32> s_system_lookup;
std::unordered_map<std::string, u32> s_game_lookup;
std::unordered_map<std::string, u32> s_archive_lookup;
std::unordered_map<std::string, u32> s_control_layout_lookup;

ALWAYS_INLINE std::string_view ToStringView(const c4::csubstr& value)
{
  return std::string_view(value.data(), value.size());
}

ALWAYS_INLINE c4::csubstr ToCSubstr(std::string_view value)
{
  return c4::csubstr(value.data(), value.size());
}

std::string NormalizeKey(std::string_view value)
{
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return result;
}

bool IsValidSHA1(std::string_view value)
{
  return value.size() == 40 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

bool IsValidCRC32(std::string_view value)
{
  if (value.starts_with("0x") || value.starts_with("0X"))
    value.remove_prefix(2);
  return value.size() == 8 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

bool IsKnownROMEndianness(std::string_view value)
{
  return StringUtil::EqualNoCase(value, "native") || StringUtil::EqualNoCase(value, "little") ||
         StringUtil::EqualNoCase(value, "big");
}

bool IsKnownControllerType(std::string_view value)
{
  return StringUtil::EqualNoCase(value, "none") || StringUtil::EqualNoCase(value, "arcade") ||
         StringUtil::EqualNoCase(value, "trackball") || StringUtil::EqualNoCase(value, "lightgun") ||
         StringUtil::EqualNoCase(value, "driving") || StringUtil::EqualNoCase(value, "tokimeki");
}

bool IsKnownJoystickMode(std::string_view value)
{
  return value.empty() || StringUtil::EqualNoCase(value, "none") || StringUtil::EqualNoCase(value, "four_way") ||
         StringUtil::EqualNoCase(value, "fourway") || StringUtil::EqualNoCase(value, "eight_way") ||
         StringUtil::EqualNoCase(value, "eightway");
}

bool IsKnownBindingKind(std::string_view value)
{
  return value.empty() || StringUtil::EqualNoCase(value, "button") || StringUtil::EqualNoCase(value, "axis");
}

bool IsKnownWorkingStatus(std::string_view value)
{
  return StringUtil::EqualNoCase(value, "unknown") || StringUtil::EqualNoCase(value, "working") ||
         StringUtil::EqualNoCase(value, "preliminary") || StringUtil::EqualNoCase(value, "not_working") ||
         StringUtil::EqualNoCase(value, "notworking");
}

bool IsKnownOrientation(std::string_view value)
{
  return value.empty() || StringUtil::EqualNoCase(value, "horizontal") ||
         StringUtil::EqualNoCase(value, "vertical");
}

bool IsKnownDisplayRotation(std::string_view value)
{
  return value.empty() || StringUtil::EqualNoCase(value, "Normal") ||
         StringUtil::EqualNoCase(value, "Rotate90") || StringUtil::EqualNoCase(value, "Rotate180") ||
         StringUtil::EqualNoCase(value, "Rotate270");
}

bool GetString(const ryml::ConstNodeRef& object, std::string_view key, std::string* destination,
               bool required = false)
{
  destination->clear();
  const ryml::ConstNodeRef node = object.find_child(ToCSubstr(key));
  if (!node.valid() || !node.has_val())
  {
    if (required)
      ERROR_LOG("Arcade database field '{}' is required.", key);
    return !required;
  }

  destination->assign(ToStringView(node.val()));
  if (required && destination->empty())
  {
    ERROR_LOG("Arcade database field '{}' cannot be empty.", key);
    return false;
  }
  return true;
}

bool ParseUnsigned(std::string_view value, u32* destination)
{
  if (value.empty())
    return false;

  int base = 10;
  if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
  {
    value.remove_prefix(2);
    base = 16;
  }

  u32 parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size())
    return false;

  *destination = parsed;
  return true;
}

template<typename T>
bool GetUnsigned(const ryml::ConstNodeRef& object, std::string_view key, T* destination, bool required = false)
{
  const ryml::ConstNodeRef node = object.find_child(ToCSubstr(key));
  if (!node.valid() || !node.has_val())
  {
    if (required)
      ERROR_LOG("Arcade database numeric field '{}' is required.", key);
    return !required;
  }

  u32 value = 0;
  if (!ParseUnsigned(ToStringView(node.val()), &value) || value > static_cast<u32>(std::numeric_limits<T>::max()))
  {
    ERROR_LOG("Arcade database field '{}' has invalid numeric value '{}'.", key, ToStringView(node.val()));
    return false;
  }

  *destination = static_cast<T>(value);
  return true;
}

template<typename T>
bool GetSigned(const ryml::ConstNodeRef& object, std::string_view key, T* destination, bool required = false)
{
  const ryml::ConstNodeRef node = object.find_child(ToCSubstr(key));
  if (!node.valid() || !node.has_val())
  {
    if (required)
      ERROR_LOG("Arcade database numeric field '{}' is required.", key);
    return !required;
  }

  const std::optional<s32> value = StringUtil::FromChars<s32>(ToStringView(node.val()));
  if (!value.has_value() || *value < static_cast<s32>(std::numeric_limits<T>::min()) ||
      *value > static_cast<s32>(std::numeric_limits<T>::max()))
  {
    ERROR_LOG("Arcade database field '{}' has invalid numeric value '{}'.", key, ToStringView(node.val()));
    return false;
  }

  *destination = static_cast<T>(*value);
  return true;
}

bool GetFloat(const ryml::ConstNodeRef& object, std::string_view key, float* destination)
{
  const ryml::ConstNodeRef node = object.find_child(ToCSubstr(key));
  if (!node.valid() || !node.has_val())
    return true;

  const std::optional<float> parsed = StringUtil::FromChars<float>(ToStringView(node.val()));
  if (!parsed.has_value())
  {
    ERROR_LOG("Arcade database field '{}' has invalid floating-point value '{}'.", key, ToStringView(node.val()));
    return false;
  }

  *destination = parsed.value();
  return true;
}

bool GetBool(const ryml::ConstNodeRef& object, std::string_view key, bool* destination)
{
  const ryml::ConstNodeRef node = object.find_child(ToCSubstr(key));
  if (!node.valid() || !node.has_val())
    return true;

  const std::optional<bool> parsed = StringUtil::FromChars<bool>(ToStringView(node.val()));
  if (!parsed.has_value())
  {
    ERROR_LOG("Arcade database field '{}' has invalid boolean value '{}'.", key, ToStringView(node.val()));
    return false;
  }

  *destination = parsed.value();
  return true;
}

bool GetOptionalBool(const ryml::ConstNodeRef& object, std::string_view key, std::optional<bool>* destination)
{
  destination->reset();
  const ryml::ConstNodeRef node = object.find_child(ToCSubstr(key));
  if (!node.valid() || !node.has_val())
    return true;

  const std::optional<bool> parsed = StringUtil::FromChars<bool>(ToStringView(node.val()));
  if (!parsed.has_value())
  {
    ERROR_LOG("Arcade database field '{}' has invalid boolean value '{}'.", key, ToStringView(node.val()));
    return false;
  }

  *destination = parsed.value();
  return true;
}

ArcadeControllerType ParseControllerType(std::string_view value)
{
  if (StringUtil::EqualNoCase(value, "none"))
    return ArcadeControllerType::None;
  if (StringUtil::EqualNoCase(value, "arcade"))
    return ArcadeControllerType::Arcade;
  if (StringUtil::EqualNoCase(value, "trackball"))
    return ArcadeControllerType::Trackball;
  if (StringUtil::EqualNoCase(value, "lightgun"))
    return ArcadeControllerType::Lightgun;
  if (StringUtil::EqualNoCase(value, "driving"))
    return ArcadeControllerType::Driving;
  if (StringUtil::EqualNoCase(value, "tokimeki"))
    return ArcadeControllerType::Tokimeki;
  return ArcadeControllerType::None;
}

ArcadeJoystickMode ParseJoystickMode(std::string_view value)
{
  if (StringUtil::EqualNoCase(value, "four_way") || StringUtil::EqualNoCase(value, "fourway"))
    return ArcadeJoystickMode::FourWay;
  if (StringUtil::EqualNoCase(value, "eight_way") || StringUtil::EqualNoCase(value, "eightway"))
    return ArcadeJoystickMode::EightWay;
  return ArcadeJoystickMode::None;
}

ArcadeControlBindingKind ParseBindingKind(std::string_view value)
{
  return StringUtil::EqualNoCase(value, "axis") ? ArcadeControlBindingKind::Axis : ArcadeControlBindingKind::Button;
}

WorkingStatus ParseWorkingStatus(std::string_view value)
{
  if (StringUtil::EqualNoCase(value, "working"))
    return WorkingStatus::Working;
  if (StringUtil::EqualNoCase(value, "preliminary"))
    return WorkingStatus::Preliminary;
  if (StringUtil::EqualNoCase(value, "not_working") || StringUtil::EqualNoCase(value, "notworking"))
    return WorkingStatus::NotWorking;
  return WorkingStatus::Unknown;
}

Orientation ParseOrientation(std::string_view value)
{
  return StringUtil::EqualNoCase(value, "vertical") ? Orientation::Vertical : Orientation::Horizontal;
}

DisplayRotation ParseDisplayRotation(std::string_view value)
{
  if (StringUtil::EqualNoCase(value, "Rotate90"))
    return DisplayRotation::Rotate90;
  if (StringUtil::EqualNoCase(value, "Rotate180"))
    return DisplayRotation::Rotate180;
  if (StringUtil::EqualNoCase(value, "Rotate270"))
    return DisplayRotation::Rotate270;
  return DisplayRotation::Normal;
}

MediaType ParseMediaType(std::string_view value)
{
  if (StringUtil::EqualNoCase(value, "chd_cdrom"))
    return MediaType::CHDCDROM;
  if (StringUtil::EqualNoCase(value, "chd_hard_disk"))
    return MediaType::CHDHardDisk;
  if (StringUtil::EqualNoCase(value, "cdrom"))
    return MediaType::CDROM;
  if (StringUtil::EqualNoCase(value, "hard_disk"))
    return MediaType::HardDisk;
  if (StringUtil::EqualNoCase(value, "dvdrom"))
    return MediaType::DVDROM;
  if (StringUtil::EqualNoCase(value, "gdrom"))
    return MediaType::GDROM;
  if (StringUtil::EqualNoCase(value, "compact_flash"))
    return MediaType::CompactFlash;
  if (StringUtil::EqualNoCase(value, "nand"))
    return MediaType::NAND;
  if (StringUtil::EqualNoCase(value, "laserdisc"))
    return MediaType::LaserDisc;
  if (StringUtil::EqualNoCase(value, "security_device"))
    return MediaType::SecurityDevice;
  return MediaType::Unknown;
}

void ParseProperties(const ryml::ConstNodeRef& object, std::vector<Property>* destination)
{
  const ryml::ConstNodeRef properties = object.find_child("properties");
  if (!properties.valid() || !properties.has_children())
    return;

  destination->reserve(properties.num_children());
  for (const ryml::ConstNodeRef& child : properties.children())
  {
    if (child.key().empty() || !child.has_val())
      continue;
    destination->push_back({std::string(ToStringView(child.key())), std::string(ToStringView(child.val()))});
  }
}

bool ParseROMSegments(const ryml::ConstNodeRef& rom_node, ROMDefinition* rom)
{
  const ryml::ConstNodeRef segments = rom_node.find_child("segments");
  if (!segments.valid() || !segments.has_children())
    return true;

  rom->segments.reserve(segments.num_children());
  for (const ryml::ConstNodeRef& segment_node : segments.children())
  {
    ROMSegmentDefinition segment;
    if (!GetString(segment_node, "operation", &segment.operation, true) ||
        !GetUnsigned(segment_node, "offset", &segment.offset, true) ||
        !GetUnsigned(segment_node, "length", &segment.length, true) ||
        !GetUnsigned(segment_node, "sourceOffset", &segment.source_offset, true) ||
        !GetUnsigned(segment_node, "groupSize", &segment.group_size, true) ||
        !GetUnsigned(segment_node, "skip", &segment.skip, true) || !GetBool(segment_node, "reverse", &segment.reverse))
    {
      ERROR_LOG("Failed parsing ROM segment for '{}'.", rom->name);
      return false;
    }

    if (segment.length == 0)
    {
      ERROR_LOG("ROM '{}' segment '{}' has an invalid length of zero.", rom->name, segment.operation);
      return false;
    }
    if (segment.group_size == 0)
    {
      ERROR_LOG("ROM '{}' segment '{}' has an invalid groupSize of zero.", rom->name, segment.operation);
      return false;
    }

    rom->segments.push_back(std::move(segment));
  }

  return true;
}

bool ParseFirmwareROM(const ryml::ConstNodeRef& node, const SystemDefinition& system,
                      const FirmwareDefinition& firmware, ROMDefinition* rom)
{
  std::string crc32;
  std::string bios_variant;
  std::string variant;
  if (!GetString(node, "role", &rom->role) || !GetString(node, "region", &rom->region) ||
      !GetString(node, "name", &rom->name, true) || !GetString(node, "sha1", &rom->sha1) ||
      !GetString(node, "crc32", &crc32) || !GetString(node, "biosVariant", &bios_variant) ||
      !GetString(node, "variant", &variant) || !GetUnsigned(node, "size", &rom->size, true) ||
      !GetUnsigned(node, "offset", &rom->offset) || !GetUnsigned(node, "interleave", &rom->interleave) ||
      !GetUnsigned(node, "groupSize", &rom->group_size) || !GetUnsigned(node, "skip", &rom->skip) ||
      !GetBool(node, "required", &rom->required) || !GetBool(node, "badDump", &rom->bad_dump) ||
      !GetBool(node, "wordSwap", &rom->word_swap) || !ParseROMSegments(node, rom))
  {
    ERROR_LOG("Arcade system '{}' firmware '{}' failed parsing ROM '{}' fields.", system.id, firmware.id,
              rom->name);
    return false;
  }

  if (!bios_variant.empty() && !variant.empty() && !StringUtil::EqualNoCase(bios_variant, variant))
  {
    ERROR_LOG("Arcade system '{}' firmware '{}' ROM '{}' has contradictory 'biosVariant' '{}' and 'variant' '{}'.",
              system.id, firmware.id, rom->name, bios_variant, variant);
    return false;
  }
  rom->bios_variant = bios_variant.empty() ? std::move(variant) : std::move(bios_variant);

  if (rom->size == 0)
  {
    ERROR_LOG("Arcade system '{}' firmware '{}' ROM '{}' field 'size' must be non-zero.", system.id, firmware.id,
              rom->name);
    return false;
  }
  if (rom->interleave == 0)
  {
    ERROR_LOG("Arcade system '{}' firmware '{}' ROM '{}' field 'interleave' must be non-zero.", system.id,
              firmware.id, rom->name);
    return false;
  }
  if (rom->group_size == 0)
  {
    ERROR_LOG("Arcade system '{}' firmware '{}' ROM '{}' field 'groupSize' must be non-zero.", system.id,
              firmware.id, rom->name);
    return false;
  }

  if (!crc32.empty())
  {
    if (!IsValidCRC32(crc32) || !ParseUnsigned(crc32, &rom->crc32))
    {
      ERROR_LOG("Arcade system '{}' firmware '{}' ROM '{}' field 'crc32' has malformed value '{}'.", system.id,
                firmware.id, rom->name, crc32);
      return false;
    }
    rom->has_crc32 = true;
  }
  if (!rom->sha1.empty() && !IsValidSHA1(rom->sha1))
  {
    ERROR_LOG("Arcade system '{}' firmware '{}' ROM '{}' field 'sha1' has malformed value '{}'.", system.id,
              firmware.id, rom->name, rom->sha1);
    return false;
  }

  return true;
}

bool ParseSystemFirmware(const ryml::ConstNodeRef& system_node, SystemDefinition* system)
{
  const ryml::ConstNodeRef bios_node = system_node.find_child("bios");
  if (!bios_node.valid())
    return true;

  FirmwareDefinition firmware;
  firmware.id = system->bios_profile;
  if (firmware.id.empty())
  {
    ERROR_LOG("Arcade system '{}' field 'bios' has no stable ID because field 'biosProfile' is empty.", system->id);
    return false;
  }
  if (!GetString(bios_node, "archive", &firmware.archive_name, true))
  {
    ERROR_LOG("Arcade system '{}' firmware '{}' has invalid field 'bios.archive'.", system->id, firmware.id);
    return false;
  }

  const ryml::ConstNodeRef regions = bios_node.find_child("romRegions");
  if (regions.valid() && regions.has_children())
  {
    firmware.rom_regions.reserve(regions.num_children());
    for (const ryml::ConstNodeRef& region_node : regions.children())
    {
      ROMRegionDefinition region;
      if (!GetString(region_node, "id", &region.id, true) ||
          !GetString(region_node, "endianness", &region.endianness, true) ||
          !GetUnsigned(region_node, "size", &region.size, true) ||
          !GetUnsigned(region_node, "width", &region.width, true))
      {
        ERROR_LOG("Arcade system '{}' firmware '{}' failed parsing a 'bios.romRegions' entry.", system->id,
                  firmware.id);
        return false;
      }

      const ryml::ConstNodeRef erase_value = region_node.find_child("eraseValue");
      if (erase_value.valid())
      {
        if (!GetUnsigned(region_node, "eraseValue", &region.erase_value, true))
        {
          ERROR_LOG("Arcade system '{}' firmware '{}' ROM region '{}' field 'eraseValue' is invalid.", system->id,
                    firmware.id, region.id);
          return false;
        }
        region.has_erase_value = true;
      }

      if (region.size == 0)
      {
        ERROR_LOG("Arcade system '{}' firmware '{}' ROM region '{}' field 'size' must be non-zero.", system->id,
                  firmware.id, region.id);
        return false;
      }
      if (region.width == 0 || (region.width % 8) != 0)
      {
        ERROR_LOG("Arcade system '{}' firmware '{}' ROM region '{}' field 'width' has invalid value {}.",
                  system->id, firmware.id, region.id, region.width);
        return false;
      }
      if (!IsKnownROMEndianness(region.endianness))
      {
        ERROR_LOG("Arcade system '{}' firmware '{}' ROM region '{}' field 'endianness' has unknown value '{}'.",
                  system->id, firmware.id, region.id, region.endianness);
        return false;
      }
      firmware.rom_regions.push_back(std::move(region));
    }
  }

  const ryml::ConstNodeRef roms = bios_node.find_child("roms");
  if (roms.valid() && roms.has_children())
  {
    firmware.roms.reserve(roms.num_children());
    for (const ryml::ConstNodeRef& rom_node : roms.children())
    {
      ROMDefinition rom;
      if (!ParseFirmwareROM(rom_node, *system, firmware, &rom))
        return false;
      firmware.roms.push_back(std::move(rom));
    }
  }

  const ryml::ConstNodeRef undumped_roms = bios_node.find_child("undumpedRoms");
  if (undumped_roms.valid() && undumped_roms.has_children())
  {
    firmware.undumped_roms.reserve(undumped_roms.num_children());
    for (const ryml::ConstNodeRef& rom_node : undumped_roms.children())
    {
      UndumpedROMDefinition rom;
      if (!GetString(rom_node, "role", &rom.role) || !GetString(rom_node, "region", &rom.region) ||
          !GetString(rom_node, "name", &rom.name, true) || !GetUnsigned(rom_node, "size", &rom.size, true) ||
          !GetOptionalBool(rom_node, "required", &rom.required))
      {
        ERROR_LOG("Arcade system '{}' firmware '{}' failed parsing an 'undumpedRoms' entry '{}'.", system->id,
                  firmware.id, rom.name);
        return false;
      }
      if (rom.size == 0)
      {
        ERROR_LOG("Arcade system '{}' firmware '{}' undumped ROM '{}' field 'size' must be non-zero.", system->id,
                  firmware.id, rom.name);
        return false;
      }
      firmware.undumped_roms.push_back(std::move(rom));
    }
  }

  const bool has_usable_member = std::any_of(firmware.roms.begin(), firmware.roms.end(), [](const ROMDefinition& rom) {
    return !rom.name.empty() && rom.size > 0 && (rom.has_crc32 || !rom.sha1.empty());
  });
  if (!has_usable_member)
  {
    ERROR_LOG("Arcade system '{}' required firmware '{}' field 'bios.roms' has no usable dumped members.", system->id,
              firmware.id);
    return false;
  }

  system->firmware.push_back(std::move(firmware));
  return true;
}

bool ParseSystem(const ryml::ConstNodeRef& node)
{
  SystemDefinition definition;
  definition.id.assign(ToStringView(node.key()));
  if (definition.id.empty() || !GetString(node, "name", &definition.name, true) ||
      !GetString(node, "manufacturer", &definition.manufacturer, true) ||
      !GetString(node, "hardwareFamily", &definition.hardware_family, true) ||
      !GetString(node, "machineHandler", &definition.machine_handler, true) ||
      !GetString(node, "biosProfile", &definition.bios_profile) ||
      !GetUnsigned(node, "releaseYear", &definition.release_year) ||
      !GetUnsigned(node, "maxPlayers", &definition.max_players) ||
      !GetBool(node, "supportsLightgun", &definition.supports_lightgun) ||
      !GetBool(node, "supportsTrackball", &definition.supports_trackball) ||
      !GetBool(node, "supportsSpecialControls", &definition.supports_special_controls) ||
      !GetBool(node, "supportsNetwork", &definition.supports_network))
  {
    ERROR_LOG("Failed parsing arcade system '{}'.", definition.id);
    return false;
  }

  if (definition.max_players == 0)
  {
    ERROR_LOG("Arcade system '{}' must support at least one player.", definition.id);
    return false;
  }

  if (!ParseSystemFirmware(node, &definition))
    return false;

  ParseProperties(node, &definition.properties);
  const std::string lookup_key = NormalizeKey(definition.id);
  if (s_system_lookup.contains(lookup_key))
  {
    ERROR_LOG("Duplicate arcade system id '{}'.", definition.id);
    return false;
  }

  s_system_lookup.emplace(lookup_key, static_cast<u32>(s_systems.size()));
  s_systems.push_back(std::move(definition));
  return true;
}

bool ParseControlLayout(const ryml::ConstNodeRef& node)
{
  ControlLayoutDefinition definition;
  definition.id.assign(ToStringView(node.key()));
  std::string controller_type;
  std::string joystick_mode;
  if (definition.id.empty() || !GetString(node, "displayName", &definition.display_name, true) ||
      !GetString(node, "controllerType", &controller_type, true) ||
      !GetString(node, "joystickMode", &joystick_mode))
  {
    ERROR_LOG("Failed parsing arcade control layout '{}'.", definition.id);
    return false;
  }

  if (!IsKnownControllerType(controller_type) || !IsKnownJoystickMode(joystick_mode))
  {
    ERROR_LOG("Arcade control layout '{}' has an unknown controller type or joystick mode.", definition.id);
    return false;
  }
  definition.controller_type = ParseControllerType(controller_type);
  definition.joystick_mode = ParseJoystickMode(joystick_mode);

  const ryml::ConstNodeRef bindings = node.find_child("bindings");
  if (bindings.valid() && bindings.has_children())
  {
    definition.bindings.reserve(bindings.num_children());
    for (const ryml::ConstNodeRef& binding_node : bindings.children())
    {
      ControlLayoutBindingDefinition binding;
      std::string kind;
      if (!GetString(binding_node, "key", &binding.key, true) ||
          !GetString(binding_node, "displayName", &binding.display_name, true) ||
          !GetString(binding_node, "kind", &kind))
      {
        ERROR_LOG("Failed parsing binding in arcade layout '{}'.", definition.id);
        return false;
      }
      if (!IsKnownBindingKind(kind))
      {
        ERROR_LOG("Arcade control layout '{}' has unknown binding kind '{}'.", definition.id, kind);
        return false;
      }
      binding.kind = ParseBindingKind(kind);
      definition.bindings.push_back(std::move(binding));
    }
  }

  const std::string lookup_key = NormalizeKey(definition.id);
  if (s_control_layout_lookup.contains(lookup_key))
  {
    ERROR_LOG("Duplicate arcade control layout id '{}'.", definition.id);
    return false;
  }

  s_control_layout_lookup.emplace(lookup_key, static_cast<u32>(s_control_layouts.size()));
  s_control_layouts.push_back(std::move(definition));
  return true;
}

bool ParseROMs(const ryml::ConstNodeRef& game_node, GameDefinition* game)
{
  const ryml::ConstNodeRef roms = game_node.find_child("roms");
  if (!roms.valid() || !roms.has_children())
    return true;

  game->roms.reserve(roms.num_children());
  for (const ryml::ConstNodeRef& rom_node : roms.children())
  {
    ROMDefinition rom;
    std::string crc32;
    std::string variant;
    if (!GetString(rom_node, "role", &rom.role, true) || !GetString(rom_node, "region", &rom.region) ||
        !GetString(rom_node, "name", &rom.name, true) || !GetString(rom_node, "sha1", &rom.sha1, true) ||
        !GetString(rom_node, "crc32", &crc32) || !GetString(rom_node, "biosVariant", &rom.bios_variant) ||
        !GetString(rom_node, "variant", &variant) || !GetUnsigned(rom_node, "size", &rom.size, true) ||
        !GetUnsigned(rom_node, "offset", &rom.offset) || !GetUnsigned(rom_node, "interleave", &rom.interleave) ||
        !GetUnsigned(rom_node, "groupSize", &rom.group_size) || !GetUnsigned(rom_node, "skip", &rom.skip) ||
        !GetBool(rom_node, "required", &rom.required) || !GetBool(rom_node, "badDump", &rom.bad_dump) ||
        !GetBool(rom_node, "wordSwap", &rom.word_swap) || !ParseROMSegments(rom_node, &rom))
    {
      ERROR_LOG("Failed parsing ROM manifest for arcade game '{}'.", game->id);
      return false;
    }

    if (!IsValidSHA1(rom.sha1))
    {
      ERROR_LOG("Invalid SHA-1 '{}' for ROM '{}' in arcade game '{}'.", rom.sha1, rom.name, game->id);
      return false;
    }
    if (!variant.empty())
    {
      if (!rom.bios_variant.empty() && !StringUtil::EqualNoCase(rom.bios_variant, variant))
      {
        ERROR_LOG("ROM '{}' in arcade game '{}' has contradictory 'biosVariant' and 'variant' fields.", rom.name,
                  game->id);
        return false;
      }
      rom.bios_variant = std::move(variant);
    }
    if (rom.size == 0)
    {
      ERROR_LOG("ROM '{}' in arcade game '{}' has an invalid size of zero.", rom.name, game->id);
      return false;
    }
    if (rom.interleave == 0)
    {
      ERROR_LOG("ROM '{}' in arcade game '{}' has an invalid interleave of zero.", rom.name, game->id);
      return false;
    }
    if (rom.group_size == 0)
    {
      ERROR_LOG("ROM '{}' in arcade game '{}' has an invalid groupSize of zero.", rom.name, game->id);
      return false;
    }

    if (!crc32.empty())
    {
      if (!IsValidCRC32(crc32) || !ParseUnsigned(crc32, &rom.crc32))
      {
        ERROR_LOG("Invalid CRC32 '{}' for ROM '{}' in arcade game '{}'.", crc32, rom.name, game->id);
        return false;
      }
      rom.has_crc32 = true;
    }

    game->roms.push_back(std::move(rom));
  }
  return true;
}

bool ParseMedia(const ryml::ConstNodeRef& game_node, GameDefinition* game)
{
  const ryml::ConstNodeRef media = game_node.find_child("media");
  if (!media.valid() || !media.has_children())
    return true;

  game->media.reserve(media.num_children());
  for (const ryml::ConstNodeRef& media_node : media.children())
  {
    MediaDefinition definition;
    std::string type;
    if (!GetString(media_node, "role", &definition.role, true) || !GetString(media_node, "type", &type, true) ||
        !GetString(media_node, "name", &definition.name, true) ||
        !GetString(media_node, "sha1", &definition.sha1, true) ||
        !GetBool(media_node, "required", &definition.required) ||
        !GetBool(media_node, "readOnly", &definition.read_only) ||
        !GetBool(media_node, "companion", &definition.companion))
    {
      ERROR_LOG("Failed parsing media manifest for arcade game '{}'.", game->id);
      return false;
    }

    definition.type = ParseMediaType(type);
    if (definition.type == MediaType::Unknown)
    {
      ERROR_LOG("Unknown media type '{}' in arcade game '{}'.", type, game->id);
      return false;
    }
    if (!IsValidSHA1(definition.sha1))
    {
      ERROR_LOG("Invalid SHA-1 '{}' for media '{}' in arcade game '{}'.", definition.sha1, definition.name, game->id);
      return false;
    }
    game->media.push_back(std::move(definition));
  }
  return true;
}

bool ParsePersistentStorage(const ryml::ConstNodeRef& game_node, GameDefinition* game)
{
  const ryml::ConstNodeRef storage = game_node.find_child("persistentStorage");
  if (!storage.valid() || !storage.has_children())
    return true;

  game->persistent_storage.reserve(storage.num_children());
  for (const ryml::ConstNodeRef& storage_node : storage.children())
  {
    PersistentStorageDefinition definition;
    if (!GetString(storage_node, "id", &definition.id, true) ||
        !GetString(storage_node, "type", &definition.type, true) ||
        !GetString(storage_node, "defaultRomRole", &definition.default_rom_role) ||
        !GetUnsigned(storage_node, "size", &definition.size, true) ||
        !GetBool(storage_node, "shared", &definition.shared))
    {
      ERROR_LOG("Failed parsing persistent storage for arcade game '{}'.", game->id);
      return false;
    }
    game->persistent_storage.push_back(std::move(definition));
  }
  return true;
}

bool ParsePorts(const ryml::ConstNodeRef& game_node, GameDefinition* game)
{
  const ryml::ConstNodeRef controls = game_node.find_child("controls");
  if (!controls.valid())
    return true;

  const ryml::ConstNodeRef ports = controls.find_child("ports");
  if (!ports.valid() || !ports.has_children())
    return true;

  if (ports.num_children() > NUM_ARCADE_CONTROLLER_PORTS)
  {
    ERROR_LOG("Arcade game '{}' defines more than {} controller ports.", game->id, NUM_ARCADE_CONTROLLER_PORTS);
    return false;
  }

  u32 index = 0;
  for (const ryml::ConstNodeRef& port_node : ports.children())
  {
    std::string type;
    std::string joystick;
    PortDefinition& port = game->ports[index++];
    if (!GetString(port_node, "type", &type, true) || !GetString(port_node, "joystick", &joystick) ||
        !GetString(port_node, "layout", &port.layout))
    {
      ERROR_LOG("Failed parsing controller port for arcade game '{}'.", game->id);
      return false;
    }
    if (!IsKnownControllerType(type) || !IsKnownJoystickMode(joystick))
    {
      ERROR_LOG("Arcade game '{}' has an unknown controller type or joystick mode on port {}.", game->id, index);
      return false;
    }
    port.controller_type = ParseControllerType(type);
    port.joystick_mode = ParseJoystickMode(joystick);
  }
  return true;
}

bool ParseGame(const ryml::ConstNodeRef& node)
{
  GameDefinition game;
  game.id.assign(ToStringView(node.key()));
  std::string status;
  std::string orientation;
  std::string rotation;
  if (game.id.empty() || !GetString(node, "archive", &game.archive_name) ||
      !GetString(node, "title", &game.title, true) || !GetString(node, "sortTitle", &game.sort_title) ||
      !GetString(node, "nativeTitle", &game.native_title) || !GetString(node, "system", &game.system_id, true) ||
      !GetString(node, "parent", &game.parent) || !GetString(node, "manufacturer", &game.manufacturer, true) ||
      !GetString(node, "developer", &game.developer) || !GetString(node, "distributor", &game.distributor) ||
      !GetString(node, "genre", &game.genre) || !GetString(node, "region", &game.region) ||
      !GetString(node, "revision", &game.revision) || !GetString(node, "boardCode", &game.board_code) ||
      !GetString(node, "hardwareProfile", &game.hardware_profile, true) ||
      !GetString(node, "setFormat", &game.set_format, true) || !GetString(node, "status", &status, true) ||
      !GetUnsigned(node, "year", &game.year, true) ||
      !GetUnsigned(node, "cpuClockPercent", &game.cpu_clock_percent) ||
      !GetUnsigned(node, "minPlayers", &game.min_players) || !GetUnsigned(node, "maxPlayers", &game.max_players))
  {
    ERROR_LOG("Failed parsing arcade game '{}'.", game.id);
    return false;
  }

  if (game.archive_name.empty())
    game.archive_name = game.id + ".zip";
  if (game.sort_title.empty())
    game.sort_title = game.title;
  if (game.developer.empty())
    game.developer = game.manufacturer;
  if (game.distributor.empty())
    game.distributor = game.manufacturer;
  if (!StringUtil::EqualNoCase(game.set_format, "non_merged"))
  {
    ERROR_LOG("Arcade game '{}' must use the non_merged set format.", game.id);
    return false;
  }

  if (!IsKnownWorkingStatus(status))
  {
    ERROR_LOG("Arcade game '{}' has unknown working status '{}'.", game.id, status);
    return false;
  }
  if (game.min_players == 0 || game.max_players < game.min_players)
  {
    ERROR_LOG("Arcade game '{}' has an invalid player range {}-{}.", game.id, game.min_players, game.max_players);
    return false;
  }
  if (game.cpu_clock_percent < 10 || game.cpu_clock_percent > 1000)
  {
    ERROR_LOG("Arcade game '{}' field 'cpuClockPercent' must be between 10 and 1000.", game.id);
    return false;
  }

  game.status = ParseWorkingStatus(status);
  const ryml::ConstNodeRef display = node.find_child("display");
  if (display.valid())
  {
    if (!GetString(display, "orientation", &orientation) ||
        !GetString(display, "rotation", &rotation) ||
        !GetUnsigned(display, "aspectNumerator", &game.display.aspect_numerator) ||
        !GetUnsigned(display, "aspectDenominator", &game.display.aspect_denominator) ||
        !GetUnsigned(display, "screens", &game.display.screens) ||
        !GetUnsigned(display, "monitorCropTop", &game.display.monitor_crop_top) ||
        !GetUnsigned(display, "monitorCropBottom", &game.display.monitor_crop_bottom) ||
        !GetSigned(display, "monitorOffsetX", &game.display.monitor_offset_x) ||
        !GetFloat(display, "refreshHz", &game.display.refresh_hz))
    {
      ERROR_LOG("Failed parsing display definition for arcade game '{}'.", game.id);
      return false;
    }
    if (!IsKnownOrientation(orientation) || !IsKnownDisplayRotation(rotation) ||
        game.display.aspect_numerator == 0 || game.display.aspect_denominator == 0 || game.display.screens == 0 ||
        game.display.monitor_crop_top > 64 || game.display.monitor_crop_bottom > 64 ||
        game.display.monitor_offset_x < -64 || game.display.monitor_offset_x > 64)
    {
      ERROR_LOG("Arcade game '{}' has an invalid display definition.", game.id);
      return false;
    }
    const ryml::ConstNodeRef refresh_hz = display.find_child("refreshHz");
    if (refresh_hz.valid() &&
        (!std::isfinite(game.display.refresh_hz) || game.display.refresh_hz <= 0.0f))
    {
      ERROR_LOG("Arcade game '{}' display field 'refreshHz' must be a finite positive value.", game.id);
      return false;
    }
    game.display.orientation = ParseOrientation(orientation);
    if (!rotation.empty())
      game.display.rotation = ParseDisplayRotation(rotation);
  }

  if (!ParsePorts(node, &game) || !ParseROMs(node, &game) || !ParseMedia(node, &game) ||
      !ParsePersistentStorage(node, &game))
  {
    return false;
  }
  ParseProperties(node, &game.properties);

  const std::string game_key = NormalizeKey(game.id);
  const std::string archive_key = NormalizeKey(game.archive_name);
  if (s_game_lookup.contains(game_key) || s_archive_lookup.contains(archive_key))
  {
    ERROR_LOG("Duplicate arcade game id or archive for '{}'.", game.id);
    return false;
  }

  s_game_lookup.emplace(game_key, static_cast<u32>(s_games.size()));
  s_archive_lookup.emplace(archive_key, static_cast<u32>(s_games.size()));
  s_games.push_back(std::move(game));
  return true;
}

bool ValidateDatabase()
{
  bool valid = true;
  for (const SystemDefinition& system : s_systems)
  {
    std::unordered_set<std::string> firmware_ids;
    for (const FirmwareDefinition& firmware : system.firmware)
    {
      if (!firmware_ids.emplace(NormalizeKey(firmware.id)).second)
      {
        ERROR_LOG("Arcade system '{}' defines duplicate firmware/profile id '{}' in field 'biosProfile'.", system.id,
                  firmware.id);
        valid = false;
      }

      std::unordered_set<std::string> region_ids;
      for (const ROMRegionDefinition& region : firmware.rom_regions)
      {
        if (!region_ids.emplace(NormalizeKey(region.id)).second)
        {
          ERROR_LOG("Arcade system '{}' firmware '{}' defines duplicate ROM region id '{}' in field 'romRegions'.",
                    system.id, firmware.id, region.id);
          valid = false;
        }
      }

      for (const ROMDefinition& rom : firmware.roms)
      {
        if (rom.required && (!rom.has_crc32 || rom.sha1.empty()))
        {
          ERROR_LOG("Arcade system '{}' firmware '{}' required ROM '{}' must provide fields 'crc32' and 'sha1'.",
                    system.id, firmware.id, rom.name);
          valid = false;
        }
        if (rom.region.empty())
          continue;

        const auto region = std::find_if(firmware.rom_regions.begin(), firmware.rom_regions.end(),
                                         [&rom](const ROMRegionDefinition& candidate) {
                                           return StringUtil::EqualNoCase(candidate.id, rom.region);
                                         });
        if (region == firmware.rom_regions.end())
        {
          ERROR_LOG("Arcade system '{}' firmware '{}' ROM '{}' field 'region' references missing ROM region '{}'.",
                    system.id, firmware.id, rom.name, rom.region);
          valid = false;
        }
        else if (rom.size > region->size || rom.offset > (region->size - rom.size))
        {
          ERROR_LOG("Arcade system '{}' firmware '{}' ROM '{}' fields 'offset'/'size' exceed ROM region '{}' size {}.",
                    system.id, firmware.id, rom.name, region->id, region->size);
          valid = false;
        }
      }

      for (const UndumpedROMDefinition& rom : firmware.undumped_roms)
      {
        if (rom.region.empty())
          continue;
        const bool region_exists = std::any_of(firmware.rom_regions.begin(), firmware.rom_regions.end(),
                                               [&rom](const ROMRegionDefinition& region) {
                                                 return StringUtil::EqualNoCase(region.id, rom.region);
                                               });
        if (!region_exists)
        {
          ERROR_LOG("Arcade system '{}' firmware '{}' undumped ROM '{}' field 'region' references missing ROM region '{}'.",
                    system.id, firmware.id, rom.name, rom.region);
          valid = false;
        }
      }
    }

    if (!system.bios_profile.empty() && !ResolveFirmwareProfile(system))
    {
      WARNING_LOG("Arcade system '{}' field 'biosProfile' references '{}' but no structured field 'bios' definition exists.",
                  system.id, system.bios_profile);
    }
  }

  for (const GameDefinition& game : s_games)
  {
    const SystemDefinition* system = GetSystem(game.system_id);
    if (!system)
    {
      ERROR_LOG("Arcade game '{}' references missing system '{}'.", game.id, game.system_id);
      valid = false;
      continue;
    }
    if (game.max_players > system->max_players)
    {
      ERROR_LOG("Arcade game '{}' supports {} players, exceeding system '{}' maximum {}.", game.id,
                game.max_players, system->id, system->max_players);
      valid = false;
    }
    if (!game.parent.empty() && !GetGame(game.parent))
    {
      WARNING_LOG("Arcade game '{}' references parent '{}' which is not currently present. Parent relationships are metadata only.",
                  game.id, game.parent);
    }
    for (const PortDefinition& port : game.ports)
    {
      if (port.layout.empty())
        continue;

      const ControlLayoutDefinition* const layout = GetControlLayout(port.layout);
      if (!layout)
      {
        ERROR_LOG("Arcade game '{}' references missing control layout '{}'.", game.id, port.layout);
        valid = false;
      }
      else if (layout->controller_type != port.controller_type)
      {
        ERROR_LOG("Arcade game '{}' port layout '{}' does not match its controller type.", game.id, port.layout);
        valid = false;
      }
    }

    std::unordered_set<std::string> rom_roles;
    for (const ROMDefinition& rom : game.roms)
    {
      if (!rom_roles.emplace(NormalizeKey(rom.role)).second)
      {
        ERROR_LOG("Arcade game '{}' defines duplicate ROM role '{}'.", game.id, rom.role);
        valid = false;
      }
    }

    std::unordered_set<std::string> media_roles;
    for (const MediaDefinition& media : game.media)
    {
      if (!media_roles.emplace(NormalizeKey(media.role)).second)
      {
        ERROR_LOG("Arcade game '{}' defines duplicate media role '{}'.", game.id, media.role);
        valid = false;
      }
    }

    std::unordered_set<std::string> storage_ids;
    for (const PersistentStorageDefinition& storage : game.persistent_storage)
    {
      if (!storage_ids.emplace(NormalizeKey(storage.id)).second)
      {
        ERROR_LOG("Arcade game '{}' defines duplicate persistent storage id '{}'.", game.id, storage.id);
        valid = false;
      }
      if (!storage.default_rom_role.empty() && !GetROMByRole(game, storage.default_rom_role))
      {
        ERROR_LOG("Arcade game '{}' persistent storage '{}' references missing ROM role '{}'.", game.id, storage.id,
                  storage.default_rom_role);
        valid = false;
      }
    }
  }
  return valid;
}

void SetRymlCallbacks()
{
  ryml::Callbacks callbacks = ryml::get_callbacks();
  callbacks.m_error = [](const char* message, size_t message_length, ryml::Location location, void*) {
    ERROR_LOG("Arcade database parse error at {}:{} (offset={}): {}", location.line, location.col, location.offset,
              std::string_view(message, message_length));
  };
  ryml::set_callbacks(callbacks);
  c4::set_error_callback(
    [](const char* message, size_t message_length) { ERROR_LOG("Arcade database C4 error: {}", std::string_view(message, message_length)); });
}

bool LoadDatabase()
{
  const std::optional<std::string> data = Host::ReadResourceFileToString(DATABASE_FILENAME, false);
  if (!data.has_value())
  {
    ERROR_LOG("Failed to read universal arcade database '{}'.", DATABASE_FILENAME);
    return false;
  }

  SetRymlCallbacks();
  const ryml::Tree tree = ryml::parse_in_arena(ToCSubstr(DATABASE_FILENAME), ToCSubstr(data.value()));
  const ryml::ConstNodeRef root = tree.rootref();

  u32 schema_version = 0;
  if (!GetUnsigned(root, "schemaVersion", &schema_version, true) || schema_version != SUPPORTED_SCHEMA_VERSION)
  {
    ERROR_LOG("Unsupported arcade database schema version {} (expected {}).", schema_version, SUPPORTED_SCHEMA_VERSION);
    ryml::reset_callbacks();
    return false;
  }

  const ryml::ConstNodeRef systems = root.find_child("systems");
  const ryml::ConstNodeRef layouts = root.find_child("controlLayouts");
  const ryml::ConstNodeRef games = root.find_child("games");
  if (!systems.valid() || !systems.has_children() || !games.valid() || !games.has_children())
  {
    ERROR_LOG("Arcade database must contain non-empty systems and games maps.");
    ryml::reset_callbacks();
    return false;
  }

  s_systems.reserve(systems.num_children());
  for (const ryml::ConstNodeRef& node : systems.children())
  {
    if (!ParseSystem(node))
    {
      ryml::reset_callbacks();
      return false;
    }
  }

  if (layouts.valid() && layouts.has_children())
  {
    s_control_layouts.reserve(layouts.num_children());
    for (const ryml::ConstNodeRef& node : layouts.children())
    {
      if (!ParseControlLayout(node))
      {
        ryml::reset_callbacks();
        return false;
      }
    }
  }

  s_games.reserve(games.num_children());
  for (const ryml::ConstNodeRef& node : games.children())
  {
    if (!ParseGame(node))
    {
      ryml::reset_callbacks();
      return false;
    }
  }

  const bool valid = ValidateDatabase();
  ryml::reset_callbacks();
  if (!valid)
    return false;

  INFO_LOG("Loaded universal arcade database: {} systems, {} games, {} control layouts.", s_systems.size(),
           s_games.size(), s_control_layouts.size());
  size_t firmware_count = 0;
  size_t firmware_rom_count = 0;
  size_t rom_region_count = 0;
  size_t undumped_rom_count = 0;
  for (const SystemDefinition& system : s_systems)
  {
    firmware_count += system.firmware.size();
    for (const FirmwareDefinition& firmware : system.firmware)
    {
      firmware_rom_count += firmware.roms.size();
      rom_region_count += firmware.rom_regions.size();
      undumped_rom_count += firmware.undumped_roms.size();
    }
  }
  INFO_LOG("Loaded structured arcade firmware: {} definitions, {} ROM members, {} ROM regions, {} undumped requirements.",
           firmware_count, firmware_rom_count, rom_region_count, undumped_rom_count);
  return true;
}

} // namespace

bool EnsureLoaded()
{
  if (s_load_attempted)
    return s_loaded;

  s_load_attempted = true;
  s_loaded = LoadDatabase();
  return s_loaded;
}

void Unload()
{
  s_systems.clear();
  s_games.clear();
  s_control_layouts.clear();
  s_system_lookup.clear();
  s_game_lookup.clear();
  s_archive_lookup.clear();
  s_control_layout_lookup.clear();
  s_load_attempted = false;
  s_loaded = false;
}

bool IsLoaded()
{
  return s_loaded;
}

std::span<const SystemDefinition> GetSystems()
{
  EnsureLoaded();
  return s_systems;
}

std::span<const GameDefinition> GetGames()
{
  EnsureLoaded();
  return s_games;
}

std::span<const ControlLayoutDefinition> GetControlLayouts()
{
  EnsureLoaded();
  return s_control_layouts;
}

const SystemDefinition* GetSystem(std::string_view id)
{
  EnsureLoaded();
  const auto iterator = s_system_lookup.find(NormalizeKey(id));
  return (iterator != s_system_lookup.end()) ? &s_systems[iterator->second] : nullptr;
}

const GameDefinition* GetGame(std::string_view id)
{
  EnsureLoaded();
  const auto iterator = s_game_lookup.find(NormalizeKey(id));
  return (iterator != s_game_lookup.end()) ? &s_games[iterator->second] : nullptr;
}

const ControlLayoutDefinition* GetControlLayout(std::string_view id)
{
  EnsureLoaded();
  const auto iterator = s_control_layout_lookup.find(NormalizeKey(id));
  return (iterator != s_control_layout_lookup.end()) ? &s_control_layouts[iterator->second] : nullptr;
}

const FirmwareDefinition* GetFirmwareDefinition(const SystemDefinition& system, std::string_view id)
{
  for (const FirmwareDefinition& firmware : system.firmware)
  {
    if (StringUtil::EqualNoCase(firmware.id, id))
      return &firmware;
  }
  return nullptr;
}

const FirmwareDefinition* ResolveFirmwareProfile(const SystemDefinition& system)
{
  return system.bios_profile.empty() ? nullptr : GetFirmwareDefinition(system, system.bios_profile);
}

bool IsArchivePath(std::string_view path)
{
  return StringUtil::EqualNoCase(Path::GetExtension(path), "zip");
}

std::string_view GetSetNameFromArchivePath(std::string_view path)
{
  return IsArchivePath(path) ? Path::GetFileTitle(path) : std::string_view();
}

const GameDefinition* IdentifyArchive(std::string_view path)
{
  if (!IsArchivePath(path))
    return nullptr;

  EnsureLoaded();
  const auto iterator = s_archive_lookup.find(NormalizeKey(Path::GetFileName(path)));
  return (iterator != s_archive_lookup.end()) ? &s_games[iterator->second] : nullptr;
}

const ROMDefinition* GetROMByRole(const GameDefinition& game, std::string_view role)
{
  for (const ROMDefinition& rom : game.roms)
  {
    if (StringUtil::EqualNoCase(rom.role, role))
      return &rom;
  }
  return nullptr;
}

const MediaDefinition* GetMediaByRole(const GameDefinition& game, std::string_view role)
{
  for (const MediaDefinition& media : game.media)
  {
    if (StringUtil::EqualNoCase(media.role, role))
      return &media;
  }
  return nullptr;
}

std::string GetMediaFilename(const MediaDefinition& media)
{
  if (media.type == MediaType::CHDCDROM || media.type == MediaType::CHDHardDisk)
  {
    if (StringUtil::EndsWithNoCase(media.name, ".chd"))
      return media.name;
    return media.name + ".chd";
  }
  return media.name;
}

std::string GetCompanionMediaPath(std::string_view archive_path, const GameDefinition& game,
                                  const MediaDefinition& media)
{
  const std::string filename = GetMediaFilename(media);
  std::string path = Path::Combine(Path::Combine(Path::GetDirectory(archive_path), game.id), filename);
  if (game.parent.empty() || FileSystem::FileExists(path.c_str()))
    return path;

  // Clone sets commonly share companion media with their parent archive. Prefer
  // a clone-local copy when present, otherwise inherit the parent's companion
  // directory without adding per-game path exceptions in machine handlers.
  const std::string parent_path =
    Path::Combine(Path::Combine(Path::GetDirectory(archive_path), game.parent), filename);
  return FileSystem::FileExists(parent_path.c_str()) ? parent_path : path;
}

const char* GetWorkingStatusName(WorkingStatus status)
{
  static constexpr std::array names = {"Unknown", "Preliminary", "Working", "Not Working"};
  return names[static_cast<size_t>(status)];
}

const char* GetMediaTypeName(MediaType type)
{
  static constexpr std::array names = {"Unknown",       "CHD CD-ROM",    "CHD Hard Disk", "CD-ROM",
                                       "Hard Disk",     "DVD-ROM",       "GD-ROM",        "CompactFlash",
                                       "NAND",          "LaserDisc",     "Security Device"};
  return names[static_cast<size_t>(type)];
}

} // namespace Arcade::Database
