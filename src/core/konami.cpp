// SPDX-FileCopyrightText: 2026 ArcadeDuck Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "konami.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/path.h"
#include "common/sha1_digest.h"
#include "common/string_util.h"
#include "bios.h"
#include "bus.h"
#include "settings.h"
#include "util/state_wrapper.h"
#include "util/cd_image.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <vector>

Log_SetChannel(Konami);

namespace Konami {

namespace {

constexpr u32 GV_SHARP_FLASH_SIZE = 0x80000;
constexpr u32 GV_SHARP_FLASH_SECTOR_SIZE = 0x10000;
constexpr u8 GV_SHARP_FLASH_ERASED_VALUE = 0xff;

enum class SharpFlashMode : u8
{
  ReadArray,
  ReadStatus,
  Program,
  EraseSetup,
};

struct GVRuntimeState
{
  const GVGameDefinition* definition = nullptr;
  std::string set_name;
  std::string title;
  std::string hardware_profile;
  std::array<u8, BIOS::BIOS_SIZE> bios;
  std::array<u8, 0x80> default_eeprom;
  std::array<u8, 0x80> eeprom;
  std::string eeprom_path;
  bool eeprom_dirty = false;
  bool eeprom_write_enabled = false;
  bool eeprom_di = false;
  bool eeprom_do = true;
  bool eeprom_cs = false;
  bool eeprom_clk = false;
  u32 eeprom_shift = 0;
  u32 eeprom_shift_count = 0;
  u16 eeprom_read_shift = 0;
  s32 eeprom_read_bits = 0;
  u8 eeprom_read_address = 0;
  u16 eeprom_write_shift = 0;
  s32 eeprom_write_bits = 0;
  u8 eeprom_write_address = 0;
  bool eeprom_write_all = false;
  std::string chd_path;
  std::string persistence_directory;
  std::vector<u8> sharp_flash;
  std::string sharp_flash_path;
  SharpFlashMode sharp_flash_mode = SharpFlashMode::ReadArray;
  u16 sharp_flash_status = 0x0080;
  bool sharp_flash_active = false;
  bool sharp_flash_dirty = false;
  bool bios_installed = false;
  bool active = false;
};

std::optional<GVRuntimeState> s_gv_runtime;

static void ResetEEPROMProtocol(GVRuntimeState& runtime)
{
  runtime.eeprom_di = false;
  runtime.eeprom_do = true;
  runtime.eeprom_cs = false;
  runtime.eeprom_clk = false;
  runtime.eeprom_shift = 0;
  runtime.eeprom_shift_count = 0;
  runtime.eeprom_read_shift = 0;
  runtime.eeprom_read_bits = 0;
  runtime.eeprom_read_address = 0;
  runtime.eeprom_write_shift = 0;
  runtime.eeprom_write_bits = 0;
  runtime.eeprom_write_address = 0;
  runtime.eeprom_write_all = false;
}

static void MarkEEPROMDirty(GVRuntimeState& runtime)
{
  if (!runtime.eeprom_dirty)
    INFO_LOG("KonamiGV.EEPROM dirty canonical_set='{}'", runtime.set_name);
  runtime.eeprom_dirty = true;
}

static bool LoadEEPROM(GVRuntimeState& runtime, Error* error)
{
  runtime.eeprom_path = Path::Combine(runtime.persistence_directory, "eeprom");
  if (!FileSystem::FileExists(runtime.eeprom_path.c_str()))
  {
    runtime.eeprom = runtime.default_eeprom;
    INFO_LOG("KonamiGV.EEPROM initialized_default canonical_set='{}' path='{}'", runtime.set_name, runtime.eeprom_path);
    return true;
  }
  std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(runtime.eeprom_path.c_str(), error);
  if (!data || data->size() != runtime.eeprom.size())
  {
    Error::SetStringFmt(error, "Konami GV EEPROM '{}' has invalid size; expected {} bytes.", runtime.eeprom_path,
                        static_cast<u32>(runtime.eeprom.size()));
    ERROR_LOG("KonamiGV.EEPROM malformed_persistence path='{}'", runtime.eeprom_path);
    return false;
  }
  std::memcpy(runtime.eeprom.data(), data->data(), runtime.eeprom.size());
  INFO_LOG("KonamiGV.EEPROM loaded_persistence canonical_set='{}' path='{}'", runtime.set_name, runtime.eeprom_path);
  return true;
}

static void SaveEEPROM(GVRuntimeState& runtime)
{
  if (!runtime.eeprom_dirty)
    return;
  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  if (!FileSystem::CreateDirectory(nvram_root.c_str(), false) ||
      !FileSystem::CreateDirectory(runtime.persistence_directory.c_str(), false) ||
      !FileSystem::WriteBinaryFile(runtime.eeprom_path.c_str(), runtime.eeprom.data(), runtime.eeprom.size()))
  {
    ERROR_LOG("KonamiGV.EEPROM save_failed path='{}'", runtime.eeprom_path);
    return;
  }
  runtime.eeprom_dirty = false;
  INFO_LOG("KonamiGV.EEPROM saved path='{}'", runtime.eeprom_path);
}

static bool UsesSharpFlash(const GVRuntimeState& runtime)
{
  return runtime.hardware_profile == "konamigv_btchamp" || runtime.hardware_profile == "konamigv_kdeadeye";
}

static void ResetSharpFlash(GVRuntimeState& runtime)
{
  if (!runtime.sharp_flash_active)
    return;

  runtime.sharp_flash_mode = SharpFlashMode::ReadArray;
  runtime.sharp_flash_status = 0x0080;
  INFO_LOG("KonamiGV.SharpFlash reset canonical_set='{}'", runtime.set_name);
}

static void MarkSharpFlashDirty(GVRuntimeState& runtime)
{
  if (!runtime.sharp_flash_dirty)
    INFO_LOG("KonamiGV.SharpFlash dirty canonical_set='{}'", runtime.set_name);
  runtime.sharp_flash_dirty = true;
}

static bool InitializeSharpFlash(GVRuntimeState& runtime, Error* error)
{
  if (!UsesSharpFlash(runtime))
    return true;

  runtime.sharp_flash_active = true;
  runtime.sharp_flash.assign(GV_SHARP_FLASH_SIZE, GV_SHARP_FLASH_ERASED_VALUE);
  runtime.sharp_flash_path = Path::Combine(runtime.persistence_directory, "flash");
  ResetSharpFlash(runtime);
  INFO_LOG("KonamiGV.SharpFlash selected canonical_set='{}' hardware_profile='{}' path='{}'", runtime.set_name,
           runtime.hardware_profile, runtime.sharp_flash_path);

  if (!FileSystem::FileExists(runtime.sharp_flash_path.c_str()))
  {
    INFO_LOG("KonamiGV.SharpFlash initialized_erased canonical_set='{}' path='{}'", runtime.set_name,
             runtime.sharp_flash_path);
    return true;
  }

  std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(runtime.sharp_flash_path.c_str(), error);
  if (!data || data->size() != GV_SHARP_FLASH_SIZE)
  {
    Error::SetStringFmt(error, "Konami GV Sharp flash '{}' has invalid size; expected {} bytes.", runtime.sharp_flash_path,
                        GV_SHARP_FLASH_SIZE);
    ERROR_LOG("KonamiGV.SharpFlash malformed_persistence path='{}' expected_size={}", runtime.sharp_flash_path,
              GV_SHARP_FLASH_SIZE);
    return false;
  }

  std::memcpy(runtime.sharp_flash.data(), data->data(), runtime.sharp_flash.size());
  INFO_LOG("KonamiGV.SharpFlash loaded_persistence canonical_set='{}' path='{}'", runtime.set_name,
           runtime.sharp_flash_path);
  return true;
}

static void SaveSharpFlash(GVRuntimeState& runtime)
{
  if (!runtime.sharp_flash_active || !runtime.sharp_flash_dirty)
    return;

  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  if (!FileSystem::CreateDirectory(nvram_root.c_str(), false) ||
      !FileSystem::CreateDirectory(runtime.persistence_directory.c_str(), false) ||
      !FileSystem::WriteBinaryFile(runtime.sharp_flash_path.c_str(), runtime.sharp_flash.data(), runtime.sharp_flash.size()))
  {
    ERROR_LOG("KonamiGV.SharpFlash save_failed path='{}'", runtime.sharp_flash_path);
    return;
  }

  runtime.sharp_flash_dirty = false;
  INFO_LOG("KonamiGV.SharpFlash saved path='{}'", runtime.sharp_flash_path);
}

static u16 ReadSharpFlashArray16Impl(const GVRuntimeState& runtime, u32 relative_offset)
{
  const u32 high_offset = (relative_offset & (GV_SHARP_FLASH_SIZE - 1)) & ~1U;
  const u32 low_offset = (high_offset + 1) & (GV_SHARP_FLASH_SIZE - 1);
  return static_cast<u16>((runtime.sharp_flash[high_offset] << 8) | runtime.sharp_flash[low_offset]);
}

static u32 ReadSharpFlashImpl(const GVRuntimeState& runtime, u32 size, u32 offset)
{
  const u32 relative_offset = (offset >= 0x00380000) ? (offset - 0x00380000) : offset;
  if (runtime.sharp_flash_active && runtime.sharp_flash_mode == SharpFlashMode::ReadStatus)
  {
    u32 value = runtime.sharp_flash_status;
    if (size == 4)
      value |= value << 16;
    return value;
  }

  if (!runtime.sharp_flash_active)
    return (size == 1) ? 0xff : (size == 2) ? 0xffff : 0xffffffff;

  switch (size)
  {
    case 1: return runtime.sharp_flash[relative_offset & (GV_SHARP_FLASH_SIZE - 1)];
    case 2: return ReadSharpFlashArray16Impl(runtime, relative_offset);
    case 4:
    {
      const u16 low = ReadSharpFlashArray16Impl(runtime, relative_offset);
      const u16 high = ReadSharpFlashArray16Impl(runtime, relative_offset + 2);
      return static_cast<u32>(low) | (static_cast<u32>(high) << 16);
    }
    default: return 0xffffffff;
  }
}

static void WriteSharpFlashImpl(GVRuntimeState& runtime, u32 size, u32 offset, u32 value)
{
  if (!runtime.sharp_flash_active)
    return;

  const u32 relative_offset = (offset >= 0x00380000) ? (offset - 0x00380000) : offset;
  const u32 byte_offset = relative_offset & (GV_SHARP_FLASH_SIZE - 1);
  const u8 command = static_cast<u8>(value);
  if (runtime.sharp_flash_mode == SharpFlashMode::Program)
  {
    bool changed = false;
    const auto write_byte = [&runtime, &changed](u32 byte_offset, u8 byte_value) {
      if (runtime.sharp_flash[byte_offset] != byte_value)
      {
        runtime.sharp_flash[byte_offset] = byte_value;
        changed = true;
      }
    };

    switch (size)
    {
      case 1: write_byte(byte_offset, static_cast<u8>(value)); break;
      case 2:
      {
        const u32 high_offset = byte_offset & ~1U;
        write_byte(high_offset, static_cast<u8>(value >> 8));
        write_byte((high_offset + 1) & (GV_SHARP_FLASH_SIZE - 1), static_cast<u8>(value));
        break;
      }
      case 4:
      {
        const u32 first_offset = byte_offset & ~1U;
        const u32 second_offset = (first_offset + 2) & (GV_SHARP_FLASH_SIZE - 1);
        write_byte(first_offset, static_cast<u8>(value >> 8));
        write_byte((first_offset + 1) & (GV_SHARP_FLASH_SIZE - 1), static_cast<u8>(value));
        write_byte(second_offset, static_cast<u8>(value >> 24));
        write_byte((second_offset + 1) & (GV_SHARP_FLASH_SIZE - 1), static_cast<u8>(value >> 16));
        break;
      }
      default: break;
    }

    if (changed)
      MarkSharpFlashDirty(runtime);
    runtime.sharp_flash_status = 0x0080;
    runtime.sharp_flash_mode = SharpFlashMode::ReadStatus;
    return;
  }

  if (runtime.sharp_flash_mode == SharpFlashMode::EraseSetup)
  {
    if (command == 0xd0)
    {
      const u32 block_start = byte_offset & ~(GV_SHARP_FLASH_SECTOR_SIZE - 1);
      bool changed = false;
      for (u32 i = 0; i < GV_SHARP_FLASH_SECTOR_SIZE; i++)
      {
        u8& byte = runtime.sharp_flash[(block_start + i) & (GV_SHARP_FLASH_SIZE - 1)];
        if (byte != GV_SHARP_FLASH_ERASED_VALUE)
        {
          byte = GV_SHARP_FLASH_ERASED_VALUE;
          changed = true;
        }
      }
      if (changed)
        MarkSharpFlashDirty(runtime);
      runtime.sharp_flash_status = 0x0080;
    }
    else
    {
      runtime.sharp_flash_status = 0x00b0;
    }
    runtime.sharp_flash_mode = SharpFlashMode::ReadStatus;
    return;
  }

  switch (command)
  {
    case 0xff: runtime.sharp_flash_mode = SharpFlashMode::ReadArray; runtime.sharp_flash_status = 0x0080; break;
    case 0x70: runtime.sharp_flash_mode = SharpFlashMode::ReadStatus; break;
    case 0x50: runtime.sharp_flash_status = 0x0080; break;
    case 0x40:
    case 0x10: runtime.sharp_flash_mode = SharpFlashMode::Program; runtime.sharp_flash_status = 0x0080; break;
    case 0x20: runtime.sharp_flash_mode = SharpFlashMode::EraseSetup; runtime.sharp_flash_status = 0x0080; break;
    case 0xb0:
    case 0xd0: runtime.sharp_flash_mode = SharpFlashMode::ReadStatus; runtime.sharp_flash_status = 0x0080; break;
    default: break;
  }
}

// Source authority: simpsons-bowling-duckstation dc6720ae7 and MAME's konamigv.cpp.
constexpr std::array<GVGameDefinition, 14> s_gv_game_definitions = {{
  {"powyak96", "Jikkyou Powerful Pro Yakyuu '96", GVBIOSProfile::KonamiGV, "konamigv_standard", "powyak96.25c", 0x405a7fc9, "e2d978f49748ba3c4a425188abcd3d272ec23907", "powyak96", "ebd0ea18ff9ce300ea1e30d66a739a96acfb0621", true},
  {"hyperath", "Hyper Athlete", GVBIOSProfile::KonamiGV, "konamigv_standard", "hyperath.25c", 0x20a8c435, "a0f203a999757fba68b391c525ac4b9684a57ba9", "gv021-j1", "579442444025b18da658cd6455c51459fbc3de0e", false},
  {"lacrazyc", "Let's Attack Crazy Cross", GVBIOSProfile::KonamiGV, "konamigv_standard", "lacrazyc.25c", 0xe20e5730, "066b49236c658a4ef2930f7bacc4b2354dd7f240", "gv027-a1", "840d0d4876cf1b814c9d8db975aa6c92e1fe4039", true},
  {"susume", "Susume! Taisen Puzzle-Dama", GVBIOSProfile::KonamiGV, "konamigv_standard", "susume.25c", 0x52f17df7, "b8ad7787b0692713439d7d9bebfa0c801c806006", "gv027j1", "e7e6749ac65de7771eb8fed7d5eefaec3f902255", true},
  {"btchamp", "Beat the Champ", GVBIOSProfile::KonamiGV, "konamigv_btchamp", "btchmp.25c", 0x6d02ea54, "d3babf481fd89db3aec17f589d0d3d999a2aa6e1", "btchamp", "c9c858e9034826e1a12c3c003dd068a49a3577e1", true},
  {"kdeadeye", "Dead Eye", GVBIOSProfile::KonamiGV, "konamigv_kdeadeye", "kdeadeye.25c", 0x3935d2df, "cbb855c475269077803c380dbc3621e522efe51e", "054uaa01", "a05079e4e5024ca66b7f6b81de74695d86c62dd8", false},
  {"weddingr", "Wedding Rhapsody", GVBIOSProfile::KonamiGV, "konamigv_standard", "weddingr.25c", 0xb90509a0, "41510a0ceded81dcb26a70eba97636d38d3742c3", "weddingr", "4e7122b191747ab7220fe4ce1b4483d62ab579af", true},
  {"tmosh", "Tokimeki Memorial Oshiete Your Heart", GVBIOSProfile::KonamiGV, "konamigv_tokimeki", "tmosh.25c", 0x2f6a27fc, "4ead9313f07e9bf7aa0272dba59db6b21510e00b", "673jaa01", "eaa76073749f9db48c1bee3dff9bea955683c8a8", true},
  {"tmoshs", "Tokimeki Memorial Oshiete Your Heart Seal Version", GVBIOSProfile::KonamiGV, "konamigv_tokimeki", "tmoshs.25c", 0xe57b833f, "f18a0974a6be69dc179706643aab837ff61c2738", "755jaa01", "fc742a0b763ba38350ba7eb5d775948632aafd9d", false},
  {"tmoshsp", "Tokimeki Memorial Oshiete Your Heart Seal Version Plus", GVBIOSProfile::KonamiGV, "konamigv_tokimeki", "tmoshsp.25c", 0xaf4cdd87, "97041e287e4c80066043967450779b81b62b2b8e", "756jab01", "b2c59b9801debccbbd986728152f314535c67e53", false},
  {"tmoshspa", "Tokimeki Memorial Oshiete Your Heart Seal Version Plus", GVBIOSProfile::KonamiGV, "konamigv_tokimeki", "tmoshsp.25c", 0xaf4cdd87, "97041e287e4c80066043967450779b81b62b2b8e", "756jaa01", "5e6d349ad1a22c0dbb1ec26aa05febc830254339", true},
  {"nagano98", "Nagano Winter Olympics '98", GVBIOSProfile::KonamiGV, "konamigv_standard", "nagano98.25c", 0xb64b7451, "a77a37e0cc580934d1e7e05d523bae0acd2c1480", "nagano98", "1be7bd4531f249ff2233dd40a206c8d60054a8c6", true},
  {"naganoj", "Hyper Olympics in Nagano", GVBIOSProfile::KonamiGV, "konamigv_standard", "720ja.25c", 0x34c473ba, "768225b04a293bdbc114a092d14dee28d52044e9", "720jaa01", "437160996551ef4dfca43899d1d14beca62eb4c9", false},
  {"simpbowl", "The Simpsons Bowling", GVBIOSProfile::KonamiGV, "konamigv_simpbowl", "simpbowl.25c", 0x2c61050c, "16ae7f81cbe841c429c5c7326cf83e87db1782bf", "829uaa02", "2ec4cc608d5582e478ee047b60ccee67b52f060c", false},
}};

} // namespace

const GVGameDefinition* GetGVGameDefinition(std::string_view set_name)
{
  for (const GVGameDefinition& definition : s_gv_game_definitions)
  {
    if (StringUtil::EqualNoCase(definition.set_name, set_name))
      return &definition;
  }

  return nullptr;
}

bool IsGVArchivePath(std::string_view path)
{
  return StringUtil::EqualNoCase(Path::GetExtension(path), "zip");
}

std::string_view GetGVSetNameFromArchivePath(std::string_view path)
{
  return IsGVArchivePath(path) ? Path::GetFileTitle(path) : std::string_view{};
}

const GVGameDefinition* IdentifyGVArchive(std::string_view path)
{
  return GetGVGameDefinition(GetGVSetNameFromArchivePath(path));
}

std::string GetGVCHDFilename(const GVGameDefinition& definition)
{
  return Path::GetExtension(definition.chd_disk_name).empty() ? std::string(definition.chd_disk_name) + ".chd" :
                                                               std::string(definition.chd_disk_name);
}

std::string GetGVCompanionCHDPath(std::string_view zip_path, const GVGameDefinition& definition)
{
  const std::string zip_directory(Path::GetDirectory(zip_path));
  return Path::Combine(Path::Combine(zip_directory, definition.set_name), GetGVCHDFilename(definition));
}

bool InitializeGV(const BIOS::Image& bios, const GVLoadedContent& content, Error* error)
{
  ShutdownGV();
  if (!content.definition || content.set_name != content.definition->set_name || content.chd_path.empty() ||
      bios.data.size() != BIOS::BIOS_SIZE)
  {
    Error::SetStringView(error, "Invalid Konami GV lifecycle initialization data.");
    ERROR_LOG("KonamiGV.Lifecycle initialization_failed reason='invalid_content'");
    return false;
  }

  GVRuntimeState runtime;
  runtime.definition = content.definition;
  runtime.set_name.assign(content.definition->set_name);
  runtime.title.assign(content.definition->title);
  runtime.hardware_profile.assign(content.definition->hardware_profile);
  std::memcpy(runtime.bios.data(), bios.data.data(), runtime.bios.size());
  runtime.default_eeprom = content.default_eeprom;
  runtime.chd_path = content.chd_path;
  runtime.persistence_directory = Path::Combine(Path::Combine(EmuFolders::DataRoot, "nvram"), runtime.set_name);
  if (!LoadEEPROM(runtime, error))
    return false;
  ResetEEPROMProtocol(runtime);
  if (!InitializeSharpFlash(runtime, error))
    return false;
  std::memcpy(Bus::g_bios, runtime.bios.data(), runtime.bios.size());
  runtime.bios_installed = true;
  runtime.active = true;
  s_gv_runtime.emplace(std::move(runtime));

  INFO_LOG("KonamiGV.Lifecycle initialized canonical_set='{}' title='{}' hardware_profile='{}' persistence_directory='{}'",
           s_gv_runtime->set_name, s_gv_runtime->title, s_gv_runtime->hardware_profile,
           s_gv_runtime->persistence_directory);
  INFO_LOG("KonamiGV.Lifecycle bios_installed canonical_set='{}'", s_gv_runtime->set_name);
  return true;
}

void ResetGV()
{
  if (s_gv_runtime && s_gv_runtime->active)
  {
    ResetEEPROMProtocol(*s_gv_runtime);
    ResetSharpFlash(*s_gv_runtime);
    INFO_LOG("KonamiGV.Lifecycle reset canonical_set='{}'", s_gv_runtime->set_name);
  }
}

void ShutdownGV()
{
  if (s_gv_runtime)
  {
    SaveSharpFlash(*s_gv_runtime);
    SaveEEPROM(*s_gv_runtime);
    INFO_LOG("KonamiGV.Lifecycle shutdown canonical_set='{}'", s_gv_runtime->set_name);
  }
  s_gv_runtime.reset();
}

bool IsGVActive()
{
  return s_gv_runtime.has_value() && s_gv_runtime->active;
}

std::string_view GetGVSetName()
{
  return s_gv_runtime ? std::string_view(s_gv_runtime->set_name) : std::string_view{};
}

std::string_view GetGVTitle()
{
  return s_gv_runtime ? std::string_view(s_gv_runtime->title) : std::string_view{};
}

std::string_view GetGVPersistenceDirectory()
{
  return s_gv_runtime ? std::string_view(s_gv_runtime->persistence_directory) : std::string_view{};
}

static u16 GetEEPROMWord(const GVRuntimeState& runtime, u8 address)
{
  const size_t offset = static_cast<size_t>(address & 0x3f) * 2;
  return static_cast<u16>((runtime.eeprom[offset] << 8) | runtime.eeprom[offset + 1]);
}

static void SetEEPROMWord(GVRuntimeState& runtime, u8 address, u16 value)
{
  const size_t offset = static_cast<size_t>(address & 0x3f) * 2;
  if (GetEEPROMWord(runtime, address) == value)
    return;
  runtime.eeprom[offset] = static_cast<u8>(value >> 8);
  runtime.eeprom[offset + 1] = static_cast<u8>(value);
  MarkEEPROMDirty(runtime);
}

void WriteEEPROMControl(u32 value)
{
  if (!s_gv_runtime || !s_gv_runtime->active)
    return;
  GVRuntimeState& runtime = *s_gv_runtime;
  const bool di = (value & 0x01) != 0;
  const bool cs = (value & 0x02) != 0;
  const bool clk = (value & 0x04) != 0;
  if (!cs)
  {
    ResetEEPROMProtocol(runtime);
    runtime.eeprom_di = di;
    runtime.eeprom_clk = clk;
    return;
  }
  if (!runtime.eeprom_cs)
    ResetEEPROMProtocol(runtime);

  if (!runtime.eeprom_clk && clk)
  {
    if (runtime.eeprom_read_bits > 0)
    {
      runtime.eeprom_do = ((runtime.eeprom_read_shift >> 15) & 1) != 0;
      runtime.eeprom_read_shift <<= 1;
      if (--runtime.eeprom_read_bits == 0)
      {
        runtime.eeprom_read_address = static_cast<u8>((runtime.eeprom_read_address + 1) & 0x3f);
        runtime.eeprom_read_shift = GetEEPROMWord(runtime, runtime.eeprom_read_address);
        runtime.eeprom_read_bits = 16;
      }
    }
    else if (runtime.eeprom_write_bits > 0)
    {
      runtime.eeprom_write_shift = static_cast<u16>((runtime.eeprom_write_shift << 1) | (di ? 1 : 0));
      if (--runtime.eeprom_write_bits == 0)
      {
        if (runtime.eeprom_write_enabled)
        {
          if (runtime.eeprom_write_all)
            for (u8 i = 0; i < 64; i++) SetEEPROMWord(runtime, i, runtime.eeprom_write_shift);
          else
            SetEEPROMWord(runtime, runtime.eeprom_write_address, runtime.eeprom_write_shift);
        }
        runtime.eeprom_shift = 0;
        runtime.eeprom_shift_count = 0;
      }
    }
    else
    {
      if (runtime.eeprom_shift_count == 0 && !di)
      {
        runtime.eeprom_di = di;
        runtime.eeprom_cs = cs;
        runtime.eeprom_clk = clk;
        return;
      }
      runtime.eeprom_shift = (runtime.eeprom_shift << 1) | (di ? 1 : 0);
      if (++runtime.eeprom_shift_count == 9)
      {
        const u8 opcode = static_cast<u8>((runtime.eeprom_shift >> 6) & 0x03);
        const u8 address = static_cast<u8>(runtime.eeprom_shift & 0x3f);
        runtime.eeprom_shift = 0;
        runtime.eeprom_shift_count = 0;
        if (opcode == 0x02)
        {
          runtime.eeprom_read_address = address;
          runtime.eeprom_read_shift = GetEEPROMWord(runtime, address);
          runtime.eeprom_read_bits = 16;
        }
        else if (opcode == 0x01)
        {
          runtime.eeprom_write_address = address;
          runtime.eeprom_write_shift = 0;
          runtime.eeprom_write_bits = 16;
        }
        else if (opcode == 0x03 && runtime.eeprom_write_enabled)
        {
          SetEEPROMWord(runtime, address, 0xffff);
        }
        else if (opcode == 0x00)
        {
          switch ((address >> 4) & 0x03)
          {
            case 0: runtime.eeprom_write_enabled = false; break;
            case 1: runtime.eeprom_write_all = true; runtime.eeprom_write_shift = 0; runtime.eeprom_write_bits = 16; break;
            case 2: if (runtime.eeprom_write_enabled) for (u8 i = 0; i < 64; i++) SetEEPROMWord(runtime, i, 0xffff); break;
            case 3: runtime.eeprom_write_enabled = true; break;
          }
        }
      }
    }
  }
  runtime.eeprom_di = di;
  runtime.eeprom_cs = cs;
  runtime.eeprom_clk = clk;
}

bool GetEEPROMDataOutput()
{
  return s_gv_runtime && s_gv_runtime->eeprom_do;
}

u32 ReadSharpFlash(u32 size, u32 offset)
{
  return s_gv_runtime ? ReadSharpFlashImpl(*s_gv_runtime, size, offset) : 0xffffffff;
}

void WriteSharpFlash(u32 size, u32 offset, u32 value)
{
  if (s_gv_runtime)
    WriteSharpFlashImpl(*s_gv_runtime, size, offset, value);
}

bool DoGVState(StateWrapper& sw)
{
  if (!s_gv_runtime)
    return true;
  GVRuntimeState& r = *s_gv_runtime;
  sw.DoBytes(r.eeprom.data(), r.eeprom.size());
  sw.Do(&r.eeprom_dirty);
  sw.Do(&r.eeprom_write_enabled);
  sw.Do(&r.eeprom_di);
  sw.Do(&r.eeprom_do);
  sw.Do(&r.eeprom_cs);
  sw.Do(&r.eeprom_clk);
  sw.Do(&r.eeprom_shift);
  sw.Do(&r.eeprom_shift_count);
  sw.Do(&r.eeprom_read_shift);
  sw.Do(&r.eeprom_read_bits);
  sw.Do(&r.eeprom_read_address);
  sw.Do(&r.eeprom_write_shift);
  sw.Do(&r.eeprom_write_bits);
  sw.Do(&r.eeprom_write_address);
  sw.Do(&r.eeprom_write_all);
  if (r.sharp_flash_active)
  {
    sw.DoBytes(r.sharp_flash.data(), r.sharp_flash.size());
    u8 sharp_flash_mode = static_cast<u8>(r.sharp_flash_mode);
    sw.Do(&sharp_flash_mode);
    sw.Do(&r.sharp_flash_status);
    sw.Do(&r.sharp_flash_dirty);
    if (sw.IsReading())
    {
      if (sharp_flash_mode > static_cast<u8>(SharpFlashMode::EraseSetup))
      {
        ResetSharpFlash(r);
      }
      else
      {
        r.sharp_flash_mode = static_cast<SharpFlashMode>(sharp_flash_mode);
      }
    }
  }
  if (sw.IsReading() && (r.eeprom_shift_count > 9 || r.eeprom_read_bits < 0 || r.eeprom_read_bits > 16 ||
                         r.eeprom_write_bits < 0 || r.eeprom_write_bits > 16))
  {
    ResetEEPROMProtocol(r);
  }
  return !sw.HasError();
}

std::optional<GVLoadedContent> LoadGVContent(const char* archive_path, Error* error)
{
  const GVGameDefinition* const definition = IdentifyGVArchive(archive_path);
  if (!definition)
  {
    Error::SetStringFmt(error, "Konami GV content archive '{}' is not a recognized MAME set.", archive_path);
    return std::nullopt;
  }

  INFO_LOG("KonamiGV.Content opening_zip canonical_set='{}' eeprom_member='{}'", definition->set_name,
           definition->eeprom_member_name);
  unzFile zf = MinizipHelpers::OpenUnzFile(archive_path);
  if (!zf)
  {
    Error::SetStringFmt(error, "Failed to open Konami GV set ZIP '{}'.", archive_path);
    return std::nullopt;
  }

  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    Error::SetStringFmt(error, "Konami GV set ZIP '{}' is empty or unreadable.", archive_path);
    return std::nullopt;
  }

  GVLoadedContent content = {definition, definition->set_name, {}, {}, definition->chd_sha1};
  bool found_eeprom = false;
  for (;;)
  {
    unz_file_info64 file_info = {};
    char member_name[512] = {};
    if (unzGetCurrentFileInfo64(zf, &file_info, member_name, sizeof(member_name), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed to read Konami GV ZIP member information from '{}'.", archive_path);
      return std::nullopt;
    }

    if (StringUtil::EqualNoCase(member_name, definition->eeprom_member_name))
    {
      found_eeprom = true;
      if (file_info.uncompressed_size != definition->eeprom_size ||
          content.default_eeprom.size() != definition->eeprom_size)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Konami GV EEPROM '{}' has size {}, expected {} bytes.", definition->eeprom_member_name,
                            file_info.uncompressed_size, definition->eeprom_size);
        return std::nullopt;
      }
      if (static_cast<u32>(file_info.crc) != definition->eeprom_crc32)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Konami GV EEPROM '{}' CRC32 mismatch.", definition->eeprom_member_name);
        return std::nullopt;
      }
      if (unzOpenCurrentFile(zf) != UNZ_OK)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Failed to decompress Konami GV EEPROM '{}'.", definition->eeprom_member_name);
        return std::nullopt;
      }
      const int bytes_read = unzReadCurrentFile(zf, content.default_eeprom.data(),
                                                 static_cast<unsigned>(content.default_eeprom.size()));
      if (bytes_read != static_cast<int>(content.default_eeprom.size()) || unzCloseCurrentFile(zf) != UNZ_OK)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Failed reading or validating Konami GV EEPROM '{}'.", definition->eeprom_member_name);
        return std::nullopt;
      }
      auto digest = SHA1Digest::GetDigest(std::span<const u8>(content.default_eeprom));
      const std::string digest_string = SHA1Digest::DigestToString(digest);
      if (!StringUtil::EqualNoCase(digest_string, definition->eeprom_sha1))
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Konami GV EEPROM '{}' SHA-1 mismatch.", definition->eeprom_member_name);
        return std::nullopt;
      }
      INFO_LOG("KonamiGV.Content eeprom_validated canonical_set='{}' size={} crc32={:08x} sha1='{}' bad_dump={}",
               definition->set_name, definition->eeprom_size, definition->eeprom_crc32,
               definition->eeprom_sha1, definition->bad_dump);
      break;
    }

    const int next_result = unzGoToNextFile(zf);
    if (next_result == UNZ_END_OF_LIST_OF_FILE)
      break;
    if (next_result != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed while scanning Konami GV set ZIP '{}'.", archive_path);
      return std::nullopt;
    }
  }
  unzClose(zf);
  if (!found_eeprom)
  {
    Error::SetStringFmt(error, "Konami GV set ZIP '{}' is missing EEPROM '{}'.", archive_path, definition->eeprom_member_name);
    return std::nullopt;
  }

  content.chd_path = GetGVCompanionCHDPath(archive_path, *definition);
  INFO_LOG("KonamiGV.Content expected_chd canonical_set='{}' disk_basename='{}' candidate_path='{}'", definition->set_name,
           definition->chd_disk_name, content.chd_path);
  if (!FileSystem::FileExists(content.chd_path.c_str()))
  {
    Error::SetStringFmt(error, "Konami GV CHD for '{}' was not found:\n{}", definition->set_name, content.chd_path);
    return std::nullopt;
  }
  INFO_LOG("KonamiGV.Content validating_chd canonical_set='{}' path='{}'", definition->set_name, content.chd_path);
  if (!CDImage::Open(content.chd_path.c_str(), false, error))
    return std::nullopt;
  std::array<u8, SHA1Digest::DIGEST_SIZE> chd_sha1;
  if (!CDImage::GetCHDImageSHA1(content.chd_path.c_str(), &chd_sha1, error))
    return std::nullopt;
  const std::string chd_sha1_string = SHA1Digest::DigestToString(chd_sha1);
  if (!StringUtil::EqualNoCase(chd_sha1_string, definition->chd_sha1))
  {
    Error::SetStringFmt(error, "Konami GV CHD '{}' SHA-1 mismatch.", content.chd_path);
    return std::nullopt;
  }
  INFO_LOG("KonamiGV.Content preflight_complete canonical_set='{}' chd_sha1='{}'", definition->set_name,
           definition->chd_sha1);
  return content;
}

bool IsGVSet(std::string_view set_name)
{
  return (GetGVGameDefinition(set_name) != nullptr);
}

const char* GetGVBIOSProfileName(GVBIOSProfile profile)
{
  switch (profile)
  {
    case GVBIOSProfile::KonamiGV:
      return "konamigv";

    default:
      return "unknown";
  }
}

} // namespace Konami
