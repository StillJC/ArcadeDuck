// SPDX-FileCopyrightText: 2026 ArcadeDuck Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "konami.h"
#include "konami_gv_scsi.h"

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
#include "sio.h"
#include "system.h"
#include "util/state_wrapper.h"
#include "util/cd_image.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

Log_SetChannel(Konami);

namespace Konami {

namespace {

constexpr u32 GV_SHARP_FLASH_SIZE = 0x80000;
constexpr u32 GV_SHARP_FLASH_SECTOR_SIZE = 0x10000;
constexpr u8 GV_SHARP_FLASH_ERASED_VALUE = 0xff;
constexpr u32 GV_FUJITSU_FLASH_SIZE = 0x200000;
constexpr u32 GV_FUJITSU_FLASH_SECTOR_SIZE = 0x10000;
constexpr u32 GV_FUJITSU_FLASH_CHIP_COUNT = 4;
constexpr u32 GV_FUJITSU_FLASH_CHIPS_PER_PAIR = 2;
constexpr u8 GV_FUJITSU_FLASH_ERASED_VALUE = 0xff;
constexpr u32 GV_FUJITSU_FLASH_PAIR_SECTOR_COUNT = GV_FUJITSU_FLASH_SIZE / (CDImage::DATA_SECTOR_SIZE / 2);
constexpr u32 GV_WATCHDOG_TIMEOUT_FRAMES = 120;
constexpr u32 TOKIMEKI_DEFAULT_HEARTBEAT_RATE = 70;
constexpr u32 TOKIMEKI_HEARTBEAT_RATE_STEP = 5;
constexpr u32 TOKIMEKI_HEARTBEAT_PULSE_FRAMES = 4;
constexpr u32 TOKIMEKI_MAX_EXCITEMENT_LEVEL = 7;
constexpr u8 TOKIMEKI_DEFAULT_GSR_VALUE = 0x20;
constexpr u8 TOKIMEKI_MAX_GSR_VALUE = 0x80;
constexpr u32 TOKIMEKI_PRINTER_BUSY_FRAMES = 60;
constexpr u64 TOKIMEKI_PRINTER_TIMER_DIVIDER = 8;
constexpr u64 TOKIMEKI_PRINTER_TRANSFER_START_MIN_TICKS = 9000;
constexpr u64 TOKIMEKI_PRINTER_ONE_MIN_TICKS = 4000;
constexpr u64 TOKIMEKI_PRINTER_ONE_MAX_TICKS = 5600;
constexpr u16 LIGHTGUN_X_MIN = 0x004c;
constexpr u16 LIGHTGUN_X_MAX = 0x01bb;
constexpr u16 LIGHTGUN_Y_MIN = 0x0000;
constexpr u16 LIGHTGUN_Y_MAX = 0x00ef;
constexpr u16 LIGHTGUN_X_CENTER = 0x0100;
constexpr u16 LIGHTGUN_Y_CENTER = 0x0077;
constexpr u16 LIGHTGUN_X_OFFSCREEN = 0x0000;
constexpr u16 LIGHTGUN_Y_OFFSCREEN = 0x00f0;

std::mutex s_trackball_mutex;
std::mutex s_lightgun_mutex;

enum class SharpFlashMode : u8
{
  ReadArray,
  ReadStatus,
  ReadID,
  Program,
  EraseSetup,
  SetMaster,
};

enum class BtChampFirstBootState : u8
{
  Inactive,
  WaitingForValidationEnd,
  WaitingForValidationRestart,
  ResetRequested,
  HoldingTest,
};

enum class FujitsuFlashMode : u8
{
  ReadArray,
  Unlock1,
  Unlock2,
  Autoselect,
  Program,
  EraseUnlock1,
  EraseUnlock2,
  EraseSelect,
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
  std::unique_ptr<CDImage> media;
  u32 data_track_number = 0;
  CDImage::LBA data_track_start_lba = 0;
  u32 data_track_length = 0;
  std::string persistence_directory;
  std::vector<u8> sharp_flash;
  std::string sharp_flash_path;
  SharpFlashMode sharp_flash_mode = SharpFlashMode::ReadArray;
  u16 sharp_flash_status = 0x0080;
  u64 sharp_flash_busy_until_ticks = 0;
  bool sharp_flash_master_lock = false;
  bool sharp_flash_active = false;
  bool sharp_flash_dirty = false;
  BtChampFirstBootState btchamp_first_boot_state = BtChampFirstBootState::Inactive;
  std::array<std::vector<u8>, GV_FUJITSU_FLASH_CHIP_COUNT> fujitsu_flash;
  std::array<std::string, GV_FUJITSU_FLASH_CHIP_COUNT> fujitsu_flash_paths;
  std::array<FujitsuFlashMode, GV_FUJITSU_FLASH_CHIP_COUNT> fujitsu_flash_modes = {};
  std::array<bool, GV_FUJITSU_FLASH_CHIP_COUNT> fujitsu_flash_dirty = {};
  u32 fujitsu_flash_address = 0;
  bool fujitsu_flash_active = false;
  bool fujitsu_flash_dirty_logged = false;
  std::array<u32, 2> buttons = {{0xffffffff, 0xffffffff}};
  std::array<double, 2> trackball_pending_x = {};
  std::array<double, 2> trackball_pending_y = {};
  std::array<u16, 2> trackball_x = {};
  std::array<u16, 2> trackball_y = {};
  s32 trackball_counter_x = 0;
  s32 trackball_counter_y = 0;
  s32 trackball_start_x = 0;
  s32 trackball_start_y = 0;
  bool trackball_reset_active = false;
  float trackball_sensitivity = 1.0f;
  std::array<float, 2> lightgun_normalized_x = {{0.5f, 0.5f}};
  std::array<float, 2> lightgun_normalized_y = {{0.5f, 0.5f}};
  std::array<bool, 2> lightgun_trigger = {};
  std::array<bool, 2> lightgun_offscreen = {};
  std::array<bool, 2> lightgun_shoot_offscreen = {};
  u32 watchdog_frames_remaining = 0;
  bool tokimeki_enabled = false;
  u16 tokimeki_device_check_value = 0;
  bool tokimeki_device_check_clock = false;
  u32 tokimeki_excitement_level = 0;
  u32 tokimeki_heartbeat_rate = TOKIMEKI_DEFAULT_HEARTBEAT_RATE;
  bool tokimeki_heartbeat_signal = true;
  u32 tokimeki_heartbeat_frames_remaining = 0;
  u32 tokimeki_heartbeat_pulse_frames_remaining = 0;
  u8 tokimeki_gsr_value = TOKIMEKI_DEFAULT_GSR_VALUE;
  u8 tokimeki_serial_value = 0;
  u8 tokimeki_serial_length = 0;
  bool tokimeki_serial_clock = false;
  u8 tokimeki_serial_sensor_id = 0;
  u16 tokimeki_serial_sensor_data = 0;
  bool tokimeki_printer_bit = false;
  std::array<u8, 6> tokimeki_printer_data = {};
  u8 tokimeki_printer_current_bit = 0;
  u8 tokimeki_printer_current_byte = 0;
  u64 tokimeki_printer_pulse_start_ticks = 0;
  bool tokimeki_printer_busy = false;
  u32 tokimeki_printer_busy_frames_remaining = 0;
  bool eeprom_access_logged = false;
  bool sharp_flash_access_logged = false;
  bool fujitsu_flash_access_logged = false;
  bool input_access_logged = false;
  bool watchdog_access_logged = false;
  bool game_specific_access_logged = false;
  bool bios_installed = false;
  bool active = false;
};

std::optional<GVRuntimeState> s_gv_runtime;

static bool IsSet(const GVRuntimeState& runtime, std::string_view set_name);
static bool IsSharpFlashFilled(const GVRuntimeState& runtime, u8 value);

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

static u16 SwapEEPROMWord16(u16 value)
{
  return static_cast<u16>((value >> 8) | (value << 8));
}

static void ConvertEEPROMDumpToMAMEOrder(std::array<u8, 0x80>& eeprom)
{
  for (size_t i = 0; i < eeprom.size(); i += 2)
    std::swap(eeprom[i], eeprom[i + 1]);
}

static bool LoadEEPROM(GVRuntimeState& runtime, Error* error)
{
  runtime.eeprom_path = Path::Combine(runtime.persistence_directory, "eeprom");
  const bool persistence_exists = FileSystem::FileExists(runtime.eeprom_path.c_str());
  bool persistence_changed = false;

  if (!persistence_exists)
  {
    runtime.eeprom = runtime.default_eeprom;

    // The *.25c member is a raw ROM dump. The working PoC converts every
    // 16-bit word once when seeding NVRAM so the persisted file uses MAME's
    // 93C46 word order.
    ConvertEEPROMDumpToMAMEOrder(runtime.eeprom);
    persistence_changed = true;
    INFO_LOG("KonamiGV.EEPROM converted_default_to_mame_order canonical_set='{}'", runtime.set_name);
  }
  else
  {
    std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(runtime.eeprom_path.c_str(), error);
    if (!data || data->size() != runtime.eeprom.size())
    {
      Error::SetStringFmt(error, "Konami GV EEPROM '{}' has invalid size; expected {} bytes.", runtime.eeprom_path,
                          static_cast<u32>(runtime.eeprom.size()));
      ERROR_LOG("KonamiGV.EEPROM malformed_persistence path='{}'", runtime.eeprom_path);
      return false;
    }

    std::memcpy(runtime.eeprom.data(), data->data(), runtime.eeprom.size());
  }

  if (IsSet(runtime, "kdeadeye") && IsSharpFlashFilled(runtime, GV_SHARP_FLASH_ERASED_VALUE))
  {
    // Exact working-PoC first-boot state. Dead Eye enters its real Sharp-flash
    // initializer when persisted EEPROM bytes 2/3 are 47 56. The game restores
    // the normal 56 47 value after flash programming completes.
    if (runtime.eeprom[2] == 0x56 && runtime.eeprom[3] == 0x47)
    {
      std::swap(runtime.eeprom[2], runtime.eeprom[3]);
      persistence_changed = true;
      INFO_LOG("KonamiGV.EEPROM prepared_kdeadeye_sharp_first_boot canonical_set='{}'", runtime.set_name);
    }
    else if (runtime.eeprom[2] != 0x47 || runtime.eeprom[3] != 0x56)
    {
      WARNING_LOG("KonamiGV.EEPROM unexpected_kdeadeye_first_boot_state canonical_set='{}' bytes={:02X}{:02X}",
                  runtime.set_name, runtime.eeprom[2], runtime.eeprom[3]);
    }
  }

  if (persistence_changed)
  {
    const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
    if (!FileSystem::CreateDirectory(nvram_root.c_str(), false) ||
        !FileSystem::CreateDirectory(runtime.persistence_directory.c_str(), false) ||
        !FileSystem::WriteBinaryFile(runtime.eeprom_path.c_str(), runtime.eeprom.data(), runtime.eeprom.size()))
    {
      Error::SetStringFmt(error, "Failed to create Konami GV EEPROM persistence '{}'.", runtime.eeprom_path);
      ERROR_LOG("KonamiGV.EEPROM persistence_write_failed canonical_set='{}' path='{}'", runtime.set_name,
                runtime.eeprom_path);
      return false;
    }
  }

  INFO_LOG("KonamiGV.EEPROM {} canonical_set='{}' path='{}'",
           persistence_exists ? "loaded_persistence" : "initialized_default_persistence", runtime.set_name,
           runtime.eeprom_path);
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
  runtime.sharp_flash_busy_until_ticks = 0;
  INFO_LOG("KonamiGV.SharpFlash reset canonical_set='{}'", runtime.set_name);
}

static void MarkSharpFlashDirty(GVRuntimeState& runtime)
{
  if (!runtime.sharp_flash_dirty)
    INFO_LOG("KonamiGV.SharpFlash dirty canonical_set='{}'", runtime.set_name);
  runtime.sharp_flash_dirty = true;
}

static bool IsSharpFlashFilled(const GVRuntimeState& runtime, u8 value)
{
  return std::all_of(runtime.sharp_flash.begin(), runtime.sharp_flash.end(),
                     [value](u8 flash_byte) { return flash_byte == value; });
}

static bool WriteSharpFlashPersistence(const GVRuntimeState& runtime, Error* error)
{
  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  if (!FileSystem::CreateDirectory(nvram_root.c_str(), false) ||
      !FileSystem::CreateDirectory(runtime.persistence_directory.c_str(), false) ||
      !FileSystem::WriteBinaryFile(runtime.sharp_flash_path.c_str(), runtime.sharp_flash.data(),
                                   runtime.sharp_flash.size()))
  {
    Error::SetStringFmt(error, "Failed to write Konami GV Sharp flash persistence '{}'.", runtime.sharp_flash_path);
    ERROR_LOG("KonamiGV.SharpFlash persistence_write_failed canonical_set='{}' path='{}'", runtime.set_name,
              runtime.sharp_flash_path);
    return false;
  }

  return true;
}

static void ArmBtChampFirstBootRecovery(GVRuntimeState& runtime)
{
  runtime.btchamp_first_boot_state = BtChampFirstBootState::Inactive;
  if (IsSet(runtime, "btchamp") && IsSharpFlashFilled(runtime, GV_SHARP_FLASH_ERASED_VALUE))
  {
    runtime.btchamp_first_boot_state = BtChampFirstBootState::WaitingForValidationEnd;
    INFO_LOG("KonamiGV.SharpFlash btchamp_first_boot_recovery_armed canonical_set='{}'", runtime.set_name);
  }
}

static void ObserveBtChampBlankFlashRead(GVRuntimeState& runtime, u32 size, u32 relative_offset, u32 value)
{
  if (!IsSet(runtime, "btchamp") || size != 2 || value != 0xffff)
    return;

  switch (runtime.btchamp_first_boot_state)
  {
    case BtChampFirstBootState::WaitingForValidationEnd:
      if (relative_offset == 0x000007a6)
        runtime.btchamp_first_boot_state = BtChampFirstBootState::WaitingForValidationRestart;
      break;

    case BtChampFirstBootState::WaitingForValidationRestart:
      if (relative_offset == 0x00000008)
      {
        runtime.btchamp_first_boot_state = BtChampFirstBootState::ResetRequested;
        INFO_LOG("KonamiGV.SharpFlash btchamp_blank_validation_complete canonical_set='{}'", runtime.set_name);
      }
      break;

    default:
      break;
  }
}

static bool InitializeSharpFlash(GVRuntimeState& runtime, Error* error)
{
  if (!UsesSharpFlash(runtime))
    return true;

  runtime.sharp_flash_active = true;
  runtime.sharp_flash.assign(GV_SHARP_FLASH_SIZE, GV_SHARP_FLASH_ERASED_VALUE);
  runtime.sharp_flash_path = Path::Combine(runtime.persistence_directory, "flash");
  runtime.sharp_flash_dirty = false;
  runtime.btchamp_first_boot_state = BtChampFirstBootState::Inactive;
  ResetSharpFlash(runtime);
  INFO_LOG("KonamiGV.SharpFlash selected canonical_set='{}' hardware_profile='{}' path='{}'", runtime.set_name,
           runtime.hardware_profile, runtime.sharp_flash_path);

  const bool persistence_exists = FileSystem::FileExists(runtime.sharp_flash_path.c_str());
  if (!persistence_exists && !WriteSharpFlashPersistence(runtime, error))
    return false;

  std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(runtime.sharp_flash_path.c_str(), error);
  if (!data || data->size() != GV_SHARP_FLASH_SIZE)
  {
    const size_t actual_size = data ? data->size() : 0;
    Error::SetStringFmt(error, "Konami GV Sharp flash '{}' has invalid size {}; required {} bytes.",
                        runtime.sharp_flash_path, actual_size, GV_SHARP_FLASH_SIZE);
    ERROR_LOG("KonamiGV.SharpFlash malformed_persistence path='{}' actual_size={} required_size={}",
              runtime.sharp_flash_path, actual_size, GV_SHARP_FLASH_SIZE);
    return false;
  }

  std::memcpy(runtime.sharp_flash.data(), data->data(), runtime.sharp_flash.size());

  // Preserve initialized flash, but normalize the all-zero placeholder produced
  // by early PoC builds to the erased state used by the actual Sharp device.
  if (IsSharpFlashFilled(runtime, 0x00))
  {
    std::fill(runtime.sharp_flash.begin(), runtime.sharp_flash.end(), GV_SHARP_FLASH_ERASED_VALUE);
    if (!WriteSharpFlashPersistence(runtime, error))
      return false;
    INFO_LOG("KonamiGV.SharpFlash normalized_zero_persistence canonical_set='{}' path='{}'", runtime.set_name,
             runtime.sharp_flash_path);
  }
  ArmBtChampFirstBootRecovery(runtime);

  INFO_LOG("KonamiGV.SharpFlash {} canonical_set='{}' path='{}'",
           persistence_exists ? "loaded_persistence" : "initialized_erased_persistence", runtime.set_name,
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

static u16 ReadSharpFlashID16Impl(const GVRuntimeState& runtime, u32 relative_offset)
{
  switch ((relative_offset >> 1) & 0xff)
  {
    case 0: return 0x00b0;
    case 1: return 0x00ed;
    case 2: return 0x0000;
    case 3: return runtime.sharp_flash_master_lock ? 0x0001 : 0x0000;
    default: return 0x0000;
  }
}

static void UpdateSharpFlashBusyStatus(GVRuntimeState& runtime)
{
  if (runtime.sharp_flash_status != 0x0000 || runtime.sharp_flash_busy_until_ticks == 0)
    return;

  if (static_cast<u64>(System::GetGlobalTickCounter()) >= runtime.sharp_flash_busy_until_ticks)
  {
    runtime.sharp_flash_status = 0x0080;
    runtime.sharp_flash_busy_until_ticks = 0;
  }
}

static u32 ReadSharpFlashImpl(GVRuntimeState& runtime, u32 size, u32 offset)
{
  const u32 relative_offset = (offset >= 0x00380000) ? (offset - 0x00380000) : offset;
  UpdateSharpFlashBusyStatus(runtime);

  if (runtime.sharp_flash_active && runtime.sharp_flash_mode == SharpFlashMode::ReadStatus)
  {
    u32 value = runtime.sharp_flash_status;
    if (size == 4)
      value |= value << 16;
    return value;
  }

  if (!runtime.sharp_flash_active)
    return (size == 1) ? 0xff : (size == 2) ? 0xffff : 0xffffffff;

  const bool read_id = (runtime.sharp_flash_mode == SharpFlashMode::ReadID);
  u32 value = 0xffffffff;
  switch (size)
  {
    case 1:
    {
      if (read_id)
      {
        const u16 id = ReadSharpFlashID16Impl(runtime, relative_offset);
        value = (relative_offset & 1) ? static_cast<u8>(id) : static_cast<u8>(id >> 8);
      }
      else
      {
        value = runtime.sharp_flash[relative_offset & (GV_SHARP_FLASH_SIZE - 1)];
      }
      break;
    }

    case 2:
      value = read_id ? ReadSharpFlashID16Impl(runtime, relative_offset) :
                        ReadSharpFlashArray16Impl(runtime, relative_offset);
      break;

    case 4:
    {
      const u16 low = read_id ? ReadSharpFlashID16Impl(runtime, relative_offset) :
                                ReadSharpFlashArray16Impl(runtime, relative_offset);
      const u16 high = read_id ? ReadSharpFlashID16Impl(runtime, relative_offset + 2) :
                                 ReadSharpFlashArray16Impl(runtime, relative_offset + 2);
      value = static_cast<u32>(low) | (static_cast<u32>(high) << 16);
      break;
    }

    default:
      break;
  }

  if (!read_id)
    ObserveBtChampBlankFlashRead(runtime, size, relative_offset, value);
  return value;
}

static void WriteSharpFlashImpl(GVRuntimeState& runtime, u32 size, u32 offset, u32 value)
{
  if (!runtime.sharp_flash_active)
    return;

  UpdateSharpFlashBusyStatus(runtime);

  const u32 relative_offset = (offset >= 0x00380000) ? (offset - 0x00380000) : offset;
  const u32 byte_offset = relative_offset & (GV_SHARP_FLASH_SIZE - 1);
  const u8 command = static_cast<u8>(value);

  if (runtime.sharp_flash_mode == SharpFlashMode::Program)
  {
    bool changed = false;
    const auto write_byte = [&runtime, &changed](u32 target_offset, u8 byte_value) {
      if (runtime.sharp_flash[target_offset] != byte_value)
      {
        runtime.sharp_flash[target_offset] = byte_value;
        changed = true;
      }
    };

    switch (size)
    {
      case 1:
        write_byte(byte_offset, static_cast<u8>(value));
        break;

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
        const u16 low_word = static_cast<u16>(value);
        const u16 high_word = static_cast<u16>(value >> 16);
        write_byte(first_offset, static_cast<u8>(low_word >> 8));
        write_byte((first_offset + 1) & (GV_SHARP_FLASH_SIZE - 1), static_cast<u8>(low_word));
        write_byte(second_offset, static_cast<u8>(high_word >> 8));
        write_byte((second_offset + 1) & (GV_SHARP_FLASH_SIZE - 1), static_cast<u8>(high_word));
        break;
      }

      default:
        break;
    }

    if (changed)
      MarkSharpFlashDirty(runtime);
    runtime.sharp_flash_status = 0x0080;
    runtime.sharp_flash_busy_until_ticks = 0;
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

      if (runtime.btchamp_first_boot_state == BtChampFirstBootState::HoldingTest)
      {
        runtime.btchamp_first_boot_state = BtChampFirstBootState::Inactive;
        INFO_LOG("KonamiGV.SharpFlash btchamp_flash_initialization_entered canonical_set='{}' automatic_test_released=true",
                 runtime.set_name);
      }

      if (changed)
        MarkSharpFlashDirty(runtime);

      runtime.sharp_flash_status = 0x0000;
      runtime.sharp_flash_busy_until_ticks =
        static_cast<u64>(System::GetGlobalTickCounter()) + static_cast<u64>(System::GetTicksPerSecond());
    }
    else
    {
      runtime.sharp_flash_status = 0x00b0;
      runtime.sharp_flash_busy_until_ticks = 0;
    }

    runtime.sharp_flash_mode = SharpFlashMode::ReadStatus;
    return;
  }

  if (runtime.sharp_flash_mode == SharpFlashMode::SetMaster)
  {
    if (command == 0xf1)
      runtime.sharp_flash_master_lock = true;
    else if (command == 0xd0)
      runtime.sharp_flash_master_lock = false;

    runtime.sharp_flash_status = 0x0080;
    runtime.sharp_flash_busy_until_ticks = 0;
    runtime.sharp_flash_mode = SharpFlashMode::ReadStatus;
    return;
  }

  switch (command)
  {
    case 0xf0:
    case 0xff:
      runtime.sharp_flash_mode = SharpFlashMode::ReadArray;
      runtime.sharp_flash_status = 0x0080;
      runtime.sharp_flash_busy_until_ticks = 0;
      break;

    case 0x90:
      runtime.sharp_flash_mode = SharpFlashMode::ReadID;
      break;

    case 0x70:
      runtime.sharp_flash_mode = SharpFlashMode::ReadStatus;
      break;

    case 0x50:
      runtime.sharp_flash_status = 0x0080;
      runtime.sharp_flash_busy_until_ticks = 0;
      runtime.sharp_flash_mode = SharpFlashMode::ReadStatus;
      break;

    case 0x40:
    case 0x10:
      runtime.sharp_flash_mode = SharpFlashMode::Program;
      runtime.sharp_flash_status = 0x0080;
      runtime.sharp_flash_busy_until_ticks = 0;
      break;

    case 0x20:
      runtime.sharp_flash_mode = SharpFlashMode::EraseSetup;
      runtime.sharp_flash_status = 0x0080;
      runtime.sharp_flash_busy_until_ticks = 0;
      break;

    case 0x60:
      runtime.sharp_flash_mode = SharpFlashMode::SetMaster;
      break;

    case 0xb0:
    case 0xd0:
      runtime.sharp_flash_mode = SharpFlashMode::ReadStatus;
      runtime.sharp_flash_status = 0x0080;
      runtime.sharp_flash_busy_until_ticks = 0;
      break;

    default:
      break;
  }
}

static bool UsesFujitsuFlash(const GVRuntimeState& runtime)
{
  return runtime.hardware_profile == "konamigv_simpbowl";
}

static void ResetFujitsuFlash(GVRuntimeState& runtime)
{
  if (!runtime.fujitsu_flash_active)
    return;

  runtime.fujitsu_flash_address = 0;
  runtime.fujitsu_flash_modes.fill(FujitsuFlashMode::ReadArray);
  INFO_LOG("KonamiGV.FujitsuFlash reset canonical_set='{}'", runtime.set_name);
}

static void MarkFujitsuFlashDirty(GVRuntimeState& runtime, u32 chip)
{
  if (!runtime.fujitsu_flash_dirty_logged)
  {
    INFO_LOG("KonamiGV.FujitsuFlash dirty canonical_set='{}'", runtime.set_name);
    runtime.fujitsu_flash_dirty_logged = true;
  }
  runtime.fujitsu_flash_dirty[chip] = true;
}

static bool IsFujitsuFlashErased(const std::vector<u8>& flash)
{
  return std::all_of(flash.begin(), flash.end(), [](u8 value) { return value == GV_FUJITSU_FLASH_ERASED_VALUE; });
}

static bool PopulateSimpsonsFujitsuFlashFromMedia(GVRuntimeState& runtime, Error* error)
{
  constexpr std::array<u32, 2> pair_start_lbas = {{202, 2250}};
  std::array<u8, CDImage::DATA_SECTOR_SIZE> sector;

  for (u32 pair = 0; pair < pair_start_lbas.size(); pair++)
  {
    u32 output_offset = 0;
    for (u32 sector_index = 0; sector_index < GV_FUJITSU_FLASH_PAIR_SECTOR_COUNT; sector_index++)
    {
      if (!runtime.media->Seek(runtime.data_track_number, pair_start_lbas[pair] + sector_index) ||
          runtime.media->Read(CDImage::ReadMode::DataOnly, 1, sector.data()) != 1)
      {
        Error::SetStringFmt(error, "Failed reading Konami GV Fujitsu flash seed sector {}.",
                            pair_start_lbas[pair] + sector_index);
        ERROR_LOG("KonamiGV.FujitsuFlash media_seed_failed canonical_set='{}' pair={} lba={}", runtime.set_name,
                  pair, pair_start_lbas[pair] + sector_index);
        return false;
      }

      for (u32 byte_index = 0; byte_index < sector.size(); byte_index += 2)
      {
        runtime.fujitsu_flash[pair * 2][output_offset] = sector[byte_index];
        runtime.fujitsu_flash[(pair * 2) + 1][output_offset] = sector[byte_index + 1];
        output_offset++;
      }
    }
  }

  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  if (!FileSystem::CreateDirectory(nvram_root.c_str(), false) ||
      !FileSystem::CreateDirectory(runtime.persistence_directory.c_str(), false))
  {
    Error::SetStringFmt(error, "Failed to create Konami GV Fujitsu flash persistence directory '{}'.",
                        runtime.persistence_directory);
    ERROR_LOG("KonamiGV.FujitsuFlash media_seed_persistence_directory_failed canonical_set='{}' path='{}'",
              runtime.set_name, runtime.persistence_directory);
    return false;
  }
  for (u32 chip = 0; chip < GV_FUJITSU_FLASH_CHIP_COUNT; chip++)
  {
    if (!FileSystem::WriteBinaryFile(runtime.fujitsu_flash_paths[chip].c_str(), runtime.fujitsu_flash[chip].data(),
                                     runtime.fujitsu_flash[chip].size()))
    {
      Error::SetStringFmt(error, "Failed to create Konami GV Fujitsu flash persistence '{}'.",
                          runtime.fujitsu_flash_paths[chip]);
      ERROR_LOG("KonamiGV.FujitsuFlash media_seed_persistence_write_failed canonical_set='{}' path='{}'",
                runtime.set_name, runtime.fujitsu_flash_paths[chip]);
      return false;
    }
    std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(runtime.fujitsu_flash_paths[chip].c_str(), error);
    if (!data || data->size() != runtime.fujitsu_flash[chip].size())
    {
      Error::SetStringFmt(error, "Failed to validate generated Konami GV Fujitsu flash persistence '{}'.",
                          runtime.fujitsu_flash_paths[chip]);
      ERROR_LOG("KonamiGV.FujitsuFlash media_seed_persistence_read_failed canonical_set='{}' path='{}'",
                runtime.set_name, runtime.fujitsu_flash_paths[chip]);
      return false;
    }
    std::memcpy(runtime.fujitsu_flash[chip].data(), data->data(), runtime.fujitsu_flash[chip].size());
  }
  INFO_LOG("KonamiGV.FujitsuFlash generated_persistence canonical_set='{}' pair01_lba=202 pair23_lba=2250",
           runtime.set_name);
  return true;
}

static bool InitializeFujitsuFlash(GVRuntimeState& runtime, Error* error)
{
  if (!UsesFujitsuFlash(runtime))
    return true;

  runtime.fujitsu_flash_active = true;
  bool needs_media_seed = false;
  for (u32 chip = 0; chip < GV_FUJITSU_FLASH_CHIP_COUNT; chip++)
  {
    runtime.fujitsu_flash[chip].assign(GV_FUJITSU_FLASH_SIZE, GV_FUJITSU_FLASH_ERASED_VALUE);
    runtime.fujitsu_flash_paths[chip] = Path::Combine(runtime.persistence_directory, std::string("flash") +
                                                                                  static_cast<char>('0' + chip));
    if (!FileSystem::FileExists(runtime.fujitsu_flash_paths[chip].c_str()))
    {
      needs_media_seed = true;
      INFO_LOG("KonamiGV.FujitsuFlash initialized_erased canonical_set='{}' path='{}'", runtime.set_name,
               runtime.fujitsu_flash_paths[chip]);
      continue;
    }

    std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(runtime.fujitsu_flash_paths[chip].c_str(), error);
    if (!data || data->size() != GV_FUJITSU_FLASH_SIZE)
    {
      Error::SetStringFmt(error, "Konami GV Fujitsu flash '{}' has invalid size; expected {} bytes.",
                          runtime.fujitsu_flash_paths[chip], GV_FUJITSU_FLASH_SIZE);
      ERROR_LOG("KonamiGV.FujitsuFlash malformed_persistence path='{}' expected_size={}",
                runtime.fujitsu_flash_paths[chip], GV_FUJITSU_FLASH_SIZE);
      return false;
    }
    std::memcpy(runtime.fujitsu_flash[chip].data(), data->data(), runtime.fujitsu_flash[chip].size());
    needs_media_seed |= IsFujitsuFlashErased(runtime.fujitsu_flash[chip]);
    INFO_LOG("KonamiGV.FujitsuFlash loaded_persistence canonical_set='{}' path='{}'", runtime.set_name,
             runtime.fujitsu_flash_paths[chip]);
  }

  if (needs_media_seed && !PopulateSimpsonsFujitsuFlashFromMedia(runtime, error))
    return false;

  ResetFujitsuFlash(runtime);
  INFO_LOG("KonamiGV.FujitsuFlash selected canonical_set='{}' hardware_profile='{}' path0='{}'", runtime.set_name,
           runtime.hardware_profile, runtime.fujitsu_flash_paths[0]);
  return true;
}

static void SaveFujitsuFlash(GVRuntimeState& runtime)
{
  if (!runtime.fujitsu_flash_active)
    return;

  bool needs_save = false;
  for (const bool dirty : runtime.fujitsu_flash_dirty)
    needs_save |= dirty;
  if (!needs_save)
    return;

  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  if (!FileSystem::CreateDirectory(nvram_root.c_str(), false) ||
      !FileSystem::CreateDirectory(runtime.persistence_directory.c_str(), false))
  {
    ERROR_LOG("KonamiGV.FujitsuFlash save_failed reason='directory' canonical_set='{}'", runtime.set_name);
    return;
  }

  for (u32 chip = 0; chip < GV_FUJITSU_FLASH_CHIP_COUNT; chip++)
  {
    if (!runtime.fujitsu_flash_dirty[chip])
      continue;
    if (!FileSystem::WriteBinaryFile(runtime.fujitsu_flash_paths[chip].c_str(), runtime.fujitsu_flash[chip].data(),
                                     runtime.fujitsu_flash[chip].size()))
    {
      ERROR_LOG("KonamiGV.FujitsuFlash save_failed path='{}'", runtime.fujitsu_flash_paths[chip]);
      continue;
    }
    runtime.fujitsu_flash_dirty[chip] = false;
    INFO_LOG("KonamiGV.FujitsuFlash saved path='{}'", runtime.fujitsu_flash_paths[chip]);
  }
}

static bool IsFujitsuUnlockAAAddress(u32 address)
{
  return ((address & 0x0fff) == 0x0555) || ((address & 0x0fff) == 0x0aaa) || ((address & 0xffff) == 0x5555);
}

static bool IsFujitsuUnlock55Address(u32 address)
{
  return ((address & 0xffff) == 0x02aa) || ((address & 0xffff) == 0x2aaa) || ((address & 0x0fff) == 0x0555);
}

static bool IsFujitsuCommandAddress(u32 address)
{
  return ((address & 0xffff) == 0x0555) || ((address & 0xffff) == 0x5555) || ((address & 0x0fff) == 0x0aaa);
}

static u8 ReadFujitsuFlashChip(const GVRuntimeState& runtime, u32 chip, u32 address)
{
  address &= (GV_FUJITSU_FLASH_SIZE - 1);
  if (runtime.fujitsu_flash_modes[chip] == FujitsuFlashMode::Autoselect)
  {
    switch (address & 0xff)
    {
      case 0: return 0x04;
      case 1: return 0xad;
      case 2: return 0x00;
      default: return 0xff;
    }
  }
  return runtime.fujitsu_flash[chip][address];
}

static void EraseFujitsuFlashSector(GVRuntimeState& runtime, u32 chip, u32 address)
{
  const u32 sector_base = address & ~(GV_FUJITSU_FLASH_SECTOR_SIZE - 1);
  bool changed = false;
  for (u32 i = 0; i < GV_FUJITSU_FLASH_SECTOR_SIZE; i++)
  {
    u8& byte = runtime.fujitsu_flash[chip][(sector_base + i) & (GV_FUJITSU_FLASH_SIZE - 1)];
    if (byte != GV_FUJITSU_FLASH_ERASED_VALUE)
    {
      byte = GV_FUJITSU_FLASH_ERASED_VALUE;
      changed = true;
    }
  }
  if (changed)
    MarkFujitsuFlashDirty(runtime, chip);
}

static void EraseFujitsuFlashChip(GVRuntimeState& runtime, u32 chip)
{
  bool changed = false;
  for (u8& byte : runtime.fujitsu_flash[chip])
  {
    if (byte != GV_FUJITSU_FLASH_ERASED_VALUE)
    {
      byte = GV_FUJITSU_FLASH_ERASED_VALUE;
      changed = true;
    }
  }
  if (changed)
    MarkFujitsuFlashDirty(runtime, chip);
}

static void WriteFujitsuFlashChip(GVRuntimeState& runtime, u32 chip, u32 address, u8 data)
{
  address &= (GV_FUJITSU_FLASH_SIZE - 1);
  if (data == 0xf0 || data == 0xff)
  {
    runtime.fujitsu_flash_modes[chip] = FujitsuFlashMode::ReadArray;
    return;
  }

  switch (runtime.fujitsu_flash_modes[chip])
  {
    case FujitsuFlashMode::ReadArray:
    case FujitsuFlashMode::Autoselect:
      if (data == 0xaa && IsFujitsuUnlockAAAddress(address))
        runtime.fujitsu_flash_modes[chip] = FujitsuFlashMode::Unlock1;
      break;
    case FujitsuFlashMode::Unlock1:
      runtime.fujitsu_flash_modes[chip] =
        (data == 0x55 && IsFujitsuUnlock55Address(address)) ? FujitsuFlashMode::Unlock2 : FujitsuFlashMode::ReadArray;
      break;
    case FujitsuFlashMode::Unlock2:
      if (!IsFujitsuCommandAddress(address))
      {
        runtime.fujitsu_flash_modes[chip] = FujitsuFlashMode::ReadArray;
        break;
      }
      switch (data)
      {
        case 0x90: runtime.fujitsu_flash_modes[chip] = FujitsuFlashMode::Autoselect; break;
        case 0xa0: runtime.fujitsu_flash_modes[chip] = FujitsuFlashMode::Program; break;
        case 0x80: runtime.fujitsu_flash_modes[chip] = FujitsuFlashMode::EraseUnlock1; break;
        default: runtime.fujitsu_flash_modes[chip] = FujitsuFlashMode::ReadArray; break;
      }
      break;
    case FujitsuFlashMode::Program:
    {
      const u8 programmed = runtime.fujitsu_flash[chip][address] & data;
      if (programmed != runtime.fujitsu_flash[chip][address])
      {
        runtime.fujitsu_flash[chip][address] = programmed;
        MarkFujitsuFlashDirty(runtime, chip);
      }
      runtime.fujitsu_flash_modes[chip] = FujitsuFlashMode::ReadArray;
      break;
    }
    case FujitsuFlashMode::EraseUnlock1:
      runtime.fujitsu_flash_modes[chip] = (data == 0xaa && IsFujitsuUnlockAAAddress(address)) ?
        FujitsuFlashMode::EraseUnlock2 : FujitsuFlashMode::ReadArray;
      break;
    case FujitsuFlashMode::EraseUnlock2:
      runtime.fujitsu_flash_modes[chip] = (data == 0x55 && IsFujitsuUnlock55Address(address)) ?
        FujitsuFlashMode::EraseSelect : FujitsuFlashMode::ReadArray;
      break;
    case FujitsuFlashMode::EraseSelect:
      if (data == 0x10 && IsFujitsuCommandAddress(address))
        EraseFujitsuFlashChip(runtime, chip);
      else if (data == 0x30)
        EraseFujitsuFlashSector(runtime, chip, address);
      runtime.fujitsu_flash_modes[chip] = FujitsuFlashMode::ReadArray;
      break;
  }
}

static u32 ReadFujitsuFlashImpl(GVRuntimeState& runtime, u32 size, u32 offset)
{
  static_cast<void>(size);
  if (!runtime.fujitsu_flash_active)
    return 0xffffffff;

  switch (offset & 0x0f)
  {
    case 0:
    {
      const u32 chip = (runtime.fujitsu_flash_address >= GV_FUJITSU_FLASH_SIZE) ? GV_FUJITSU_FLASH_CHIPS_PER_PAIR : 0;
      const u32 address = runtime.fujitsu_flash_address & (GV_FUJITSU_FLASH_SIZE - 1);
      const u32 value = static_cast<u32>(ReadFujitsuFlashChip(runtime, chip, address)) |
                        (static_cast<u32>(ReadFujitsuFlashChip(runtime, chip + 1, address)) << 8);
      runtime.fujitsu_flash_address++;
      return value;
    }
    case 8: runtime.fujitsu_flash_address |= 1; return 0;
    default: return 0;
  }
}

static void WriteFujitsuFlashImpl(GVRuntimeState& runtime, u32 size, u32 offset, u32 value)
{
  static_cast<void>(size);
  if (!runtime.fujitsu_flash_active)
    return;

  switch (offset & 0x0f)
  {
    case 0:
    {
      const u32 chip = (runtime.fujitsu_flash_address >= GV_FUJITSU_FLASH_SIZE) ? GV_FUJITSU_FLASH_CHIPS_PER_PAIR : 0;
      const u32 address = runtime.fujitsu_flash_address & (GV_FUJITSU_FLASH_SIZE - 1);
      WriteFujitsuFlashChip(runtime, chip, address, static_cast<u8>(value));
      WriteFujitsuFlashChip(runtime, chip + 1, address, static_cast<u8>(value >> 8));
      break;
    }
    case 2: runtime.fujitsu_flash_address = value << 1; break;
    case 4: runtime.fujitsu_flash_address = (runtime.fujitsu_flash_address & 0xff00ff) | (value << 8); break;
    case 6: runtime.fujitsu_flash_address = (runtime.fujitsu_flash_address & 0x00ffff) | (value << 15); break;
    default: break;
  }
}

// Source authority: simpsons-bowling-duckstation dc6720ae7 and MAME's konamigv.cpp.

static bool IsSet(const GVRuntimeState& runtime, std::string_view set_name)
{
  return std::string_view(runtime.set_name) == set_name;
}

static bool IsTokimeki(const GVRuntimeState& runtime)
{
  return runtime.hardware_profile == "konamigv_tokimeki";
}

static u32 GetTokimekiHeartbeatPeriodFrames(const GVRuntimeState& runtime)
{
  return (3600U + runtime.tokimeki_heartbeat_rate - 1U) / runtime.tokimeki_heartbeat_rate;
}

static void UpdateTokimekiExcitementValues(GVRuntimeState& runtime)
{
  runtime.tokimeki_heartbeat_rate =
    TOKIMEKI_DEFAULT_HEARTBEAT_RATE + (runtime.tokimeki_excitement_level * TOKIMEKI_HEARTBEAT_RATE_STEP);
  runtime.tokimeki_gsr_value = static_cast<u8>(TOKIMEKI_DEFAULT_GSR_VALUE +
    (((TOKIMEKI_MAX_GSR_VALUE - TOKIMEKI_DEFAULT_GSR_VALUE) * runtime.tokimeki_excitement_level) /
     TOKIMEKI_MAX_EXCITEMENT_LEVEL));
}

static void ResetCabinetIO(GVRuntimeState& runtime)
{
  runtime.buttons = {{0xffffffff, 0xffffffff}};
  runtime.watchdog_frames_remaining = 0;

  {
    std::lock_guard<std::mutex> lock(s_trackball_mutex);
    runtime.trackball_pending_x = {};
    runtime.trackball_pending_y = {};
    runtime.trackball_x = {};
    runtime.trackball_y = {};
    runtime.trackball_counter_x = 0;
    runtime.trackball_counter_y = 0;
    runtime.trackball_start_x = 0;
    runtime.trackball_start_y = 0;
    runtime.trackball_reset_active = false;
  }

  {
    std::lock_guard<std::mutex> lock(s_lightgun_mutex);
    runtime.lightgun_normalized_x = {{0.5f, 0.5f}};
    runtime.lightgun_normalized_y = {{0.5f, 0.5f}};
    runtime.lightgun_trigger = {};
    runtime.lightgun_offscreen = {};
    runtime.lightgun_shoot_offscreen = {};
  }

  runtime.tokimeki_enabled = IsTokimeki(runtime);
  runtime.tokimeki_device_check_value = 0;
  if (IsSet(runtime, "tmoshs"))
    runtime.tokimeki_device_check_value = 0xf073;
  else if (IsSet(runtime, "tmoshsp") || IsSet(runtime, "tmoshspa"))
    runtime.tokimeki_device_check_value = 0xf0ba;
  runtime.tokimeki_device_check_clock = false;
  runtime.tokimeki_excitement_level = 0;
  UpdateTokimekiExcitementValues(runtime);
  runtime.tokimeki_heartbeat_signal = true;
  runtime.tokimeki_heartbeat_frames_remaining = GetTokimekiHeartbeatPeriodFrames(runtime);
  runtime.tokimeki_heartbeat_pulse_frames_remaining = 0;
  runtime.tokimeki_serial_value = 0;
  runtime.tokimeki_serial_length = 0;
  runtime.tokimeki_serial_clock = false;
  runtime.tokimeki_serial_sensor_id = 0;
  runtime.tokimeki_serial_sensor_data = 0;
  runtime.tokimeki_printer_bit = false;
  runtime.tokimeki_printer_data = {};
  runtime.tokimeki_printer_current_bit = 0;
  runtime.tokimeki_printer_current_byte = 0;
  runtime.tokimeki_printer_pulse_start_ticks = 0;
  runtime.tokimeki_printer_busy = false;
  runtime.tokimeki_printer_busy_frames_remaining = 0;
}

static s32 ConsumeTrackballAxis(double& pending)
{
  const s32 whole = std::clamp(static_cast<s32>(std::trunc(pending)), -2048, 2047);
  pending -= static_cast<double>(whole);
  return whole;
}

static u16 EncodeTrackball12(s32 value)
{
  return static_cast<u16>(value) & 0x0fff;
}

static void LatchTrackball(GVRuntimeState& runtime, u32 player)
{
  if (player >= runtime.trackball_x.size())
    return;
  std::lock_guard<std::mutex> lock(s_trackball_mutex);
  runtime.trackball_x[player] = EncodeTrackball12(ConsumeTrackballAxis(runtime.trackball_pending_x[player]));
  runtime.trackball_y[player] = EncodeTrackball12(ConsumeTrackballAxis(runtime.trackball_pending_y[player]));
}

static u16 ScaleLightgunAxis(float normalized, u16 min_value, u16 max_value)
{
  normalized = std::clamp(normalized, 0.0f, 1.0f);
  return static_cast<u16>(static_cast<float>(min_value) +
                          (normalized * static_cast<float>(max_value - min_value)));
}

static u16 GetLightgunX(GVRuntimeState& runtime, u32 player)
{
  if (player >= runtime.lightgun_normalized_x.size())
    return LIGHTGUN_X_CENTER;
  std::lock_guard<std::mutex> lock(s_lightgun_mutex);
  if (runtime.lightgun_offscreen[player] || runtime.lightgun_shoot_offscreen[player])
    return LIGHTGUN_X_OFFSCREEN;
  return ScaleLightgunAxis(runtime.lightgun_normalized_x[player], LIGHTGUN_X_MIN, LIGHTGUN_X_MAX);
}

static u16 GetLightgunY(GVRuntimeState& runtime, u32 player)
{
  if (player >= runtime.lightgun_normalized_y.size())
    return LIGHTGUN_Y_CENTER;
  std::lock_guard<std::mutex> lock(s_lightgun_mutex);
  if (runtime.lightgun_offscreen[player] || runtime.lightgun_shoot_offscreen[player])
    return LIGHTGUN_Y_OFFSCREEN;
  return ScaleLightgunAxis(runtime.lightgun_normalized_y[player], LIGHTGUN_Y_MIN, LIGHTGUN_Y_MAX);
}

static void HandleTokimekiPrinterPacket(GVRuntimeState& runtime)
{
  const bool repeated = runtime.tokimeki_printer_data[0] == runtime.tokimeki_printer_data[2] &&
                        runtime.tokimeki_printer_data[0] == runtime.tokimeki_printer_data[4] &&
                        runtime.tokimeki_printer_data[1] == runtime.tokimeki_printer_data[3] &&
                        runtime.tokimeki_printer_data[1] == runtime.tokimeki_printer_data[5];
  if (repeated && runtime.tokimeki_printer_data[1] == 0xf9)
  {
    switch (runtime.tokimeki_printer_data[0])
    {
      case 0x10: // memory-in/capture
        break;
      case 0x11: // print
        runtime.tokimeki_printer_busy = true;
        runtime.tokimeki_printer_busy_frames_remaining = TOKIMEKI_PRINTER_BUSY_FRAMES;
        break;
      case 0x17: // source/reset
      case 0x0b: // stop/cancel
      case 0x62: // menu
      case 0x63: // execute
      case 0x64: // up
      case 0x65: // down
      case 0x66: // left
      case 0x67: // right
      default:
        break;
    }
  }
  runtime.tokimeki_printer_current_byte = 0;
  runtime.tokimeki_printer_current_bit = 0;
}

static u32 ReadTokimekiSerial(GVRuntimeState& runtime)
{
  u32 value = runtime.tokimeki_heartbeat_signal ? 0x0004U : 0x0000U;
  if (runtime.tokimeki_serial_sensor_id != 0)
    value |= static_cast<u32>((runtime.tokimeki_serial_sensor_data >> 8) & 1U) << 3;
  if (runtime.tokimeki_printer_busy)
    value |= 0x0040U;
  return value;
}

static void WriteTokimekiSerial(GVRuntimeState& runtime, u32 value)
{
  const bool new_printer_bit = (value & 0x01U) != 0;
  const u64 current_ticks = static_cast<u64>(System::GetGlobalTickCounter());
  if (new_printer_bit && !runtime.tokimeki_printer_bit)
  {
    runtime.tokimeki_printer_pulse_start_ticks = current_ticks;
  }
  else if (!new_printer_bit && runtime.tokimeki_printer_bit)
  {
    const u64 printer_ticks = (current_ticks - runtime.tokimeki_printer_pulse_start_ticks) /
                              TOKIMEKI_PRINTER_TIMER_DIVIDER;
    if (printer_ticks >= TOKIMEKI_PRINTER_TRANSFER_START_MIN_TICKS)
    {
      runtime.tokimeki_printer_current_bit = 0;
    }
    else if (runtime.tokimeki_printer_current_byte < runtime.tokimeki_printer_data.size())
    {
      const u8 bit = (printer_ticks >= TOKIMEKI_PRINTER_ONE_MIN_TICKS &&
                      printer_ticks <= TOKIMEKI_PRINTER_ONE_MAX_TICKS) ? 1 : 0;
      if (runtime.tokimeki_printer_current_bit == 0)
        runtime.tokimeki_printer_data[runtime.tokimeki_printer_current_byte] = 0;
      runtime.tokimeki_printer_data[runtime.tokimeki_printer_current_byte] |=
        static_cast<u8>(bit << runtime.tokimeki_printer_current_bit);
      runtime.tokimeki_printer_current_bit++;
      const bool seven_bit_byte = (runtime.tokimeki_printer_current_byte & 1U) == 0;
      if ((seven_bit_byte && runtime.tokimeki_printer_current_bit == 7) ||
          (!seven_bit_byte && runtime.tokimeki_printer_current_bit == 8))
      {
        runtime.tokimeki_printer_current_byte++;
        runtime.tokimeki_printer_current_bit = 0;
      }
    }
    if (runtime.tokimeki_printer_current_byte >= runtime.tokimeki_printer_data.size())
      HandleTokimekiPrinterPacket(runtime);
  }
  runtime.tokimeki_printer_bit = new_printer_bit;

  const bool new_serial_clock = (value & 0x20U) != 0;
  if ((value & 0x02U) != 0)
  {
    runtime.tokimeki_serial_sensor_data = 0;
    runtime.tokimeki_serial_sensor_id = 0;
    runtime.tokimeki_serial_value = 0;
    runtime.tokimeki_serial_length = 0;
  }
  else if (!runtime.tokimeki_serial_clock && new_serial_clock)
  {
    if (runtime.tokimeki_serial_length < 5)
    {
      runtime.tokimeki_serial_value |= static_cast<u8>(((value >> 4) & 1U) << (4 - runtime.tokimeki_serial_length));
      if (runtime.tokimeki_serial_length == 4)
      {
        runtime.tokimeki_serial_sensor_id = runtime.tokimeki_serial_value;
        runtime.tokimeki_serial_sensor_data =
          (runtime.tokimeki_serial_sensor_id == 0x1a) ? runtime.tokimeki_gsr_value : 0;
      }
    }
    else if (runtime.tokimeki_serial_length >= 6)
    {
      runtime.tokimeki_serial_sensor_data <<= 1;
    }
    runtime.tokimeki_serial_length++;
  }
  runtime.tokimeki_serial_clock = new_serial_clock;
}

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
  runtime.media = CDImage::Open(runtime.chd_path.c_str(), false, error);
  if (!runtime.media)
    return false;
  for (const CDImage::Track& track : runtime.media->GetTracks())
  {
    if (track.mode == CDImage::TrackMode::Audio)
      continue;
    runtime.data_track_number = track.track_number;
    runtime.data_track_start_lba = track.start_lba;
    runtime.data_track_length = track.length;
    break;
  }
  if (runtime.data_track_number == 0)
  {
    Error::SetStringFmt(error, "Konami GV CHD '{}' has no readable data track.", runtime.chd_path);
    return false;
  }
  runtime.persistence_directory = Path::Combine(Path::Combine(EmuFolders::DataRoot, "nvram"), runtime.set_name);
  if (!InitializeSharpFlash(runtime, error))
    return false;
  if (!LoadEEPROM(runtime, error))
    return false;
  ResetEEPROMProtocol(runtime);
  if (!InitializeFujitsuFlash(runtime, error))
    return false;
  ResetCabinetIO(runtime);
  std::memcpy(Bus::g_bios, runtime.bios.data(), runtime.bios.size());
  runtime.bios_installed = true;
  runtime.active = true;
  s_gv_runtime.emplace(std::move(runtime));
  SIO::Reset();
  KonamiGVScsi::Initialize();

  INFO_LOG("KonamiGV.Lifecycle initialized canonical_set='{}' title='{}' hardware_profile='{}' persistence_directory='{}'",
           s_gv_runtime->set_name, s_gv_runtime->title, s_gv_runtime->hardware_profile,
           s_gv_runtime->persistence_directory);
  INFO_LOG("KonamiGV.Lifecycle bios_installed canonical_set='{}'", s_gv_runtime->set_name);
  INFO_LOG("KonamiGV.Media opened canonical_set='{}' tracks={} lba_count={} data_track={} start_lba={} length={}",
           s_gv_runtime->set_name, s_gv_runtime->media->GetTrackCount(), s_gv_runtime->media->GetLBACount(),
           s_gv_runtime->data_track_number, s_gv_runtime->data_track_start_lba, s_gv_runtime->data_track_length);
  INFO_LOG("KonamiGV.EXP1 dispatch_activated canonical_set='{}' scsi='0x1F000000-0x1F00001F' inputs='0x1F100000-0x1F100007' eeprom='0x1F180000-0x1F1800FF' sharp='0x1F380000-0x1F3FFFFF' board_io='0x1F680000-0x1F6800FF' watchdog='0x1F780000-0x1F780003'",
           s_gv_runtime->set_name);
  return true;
}

void ResetGV()
{
  if (s_gv_runtime && s_gv_runtime->active)
  {
    ResetEEPROMProtocol(*s_gv_runtime);
    ResetSharpFlash(*s_gv_runtime);
    ResetFujitsuFlash(*s_gv_runtime);
    ResetCabinetIO(*s_gv_runtime);
    KonamiGVScsi::Reset();
    s_gv_runtime->eeprom_access_logged = false;
    s_gv_runtime->sharp_flash_access_logged = false;
    s_gv_runtime->fujitsu_flash_access_logged = false;
    s_gv_runtime->input_access_logged = false;
    s_gv_runtime->watchdog_access_logged = false;
    s_gv_runtime->game_specific_access_logged = false;
    INFO_LOG("KonamiGV.Lifecycle reset canonical_set='{}'", s_gv_runtime->set_name);
  }
}

void ShutdownGV()
{
  if (s_gv_runtime)
  {
    KonamiGVScsi::Shutdown();
    SaveSharpFlash(*s_gv_runtime);
    SaveFujitsuFlash(*s_gv_runtime);
    SaveEEPROM(*s_gv_runtime);
    INFO_LOG("KonamiGV.EXP1 dispatch_shutdown canonical_set='{}'", s_gv_runtime->set_name);
    INFO_LOG("KonamiGV.Lifecycle shutdown canonical_set='{}'", s_gv_runtime->set_name);
  }
  s_gv_runtime.reset();
}

bool IsGVActive()
{
  return s_gv_runtime.has_value() && s_gv_runtime->active;
}

bool HasValidGVDiscContent()
{
  return IsGVActive() && !s_gv_runtime->chd_path.empty();
}

bool ReadGVDataSector(u32 lba, u8* buffer, u32* cdimage_lba, u32* track_number)
{
  if (!s_gv_runtime || !s_gv_runtime->media || !buffer || lba >= s_gv_runtime->data_track_length)
    return false;
  const CDImage::LBA translated_lba = s_gv_runtime->data_track_start_lba + static_cast<CDImage::LBA>(lba);
  if (cdimage_lba)
    *cdimage_lba = translated_lba;
  if (track_number)
    *track_number = s_gv_runtime->data_track_number;

  // DuckStation's CHD reader can expose a GV data track in either cooked
  // 2048-byte form (payload begins at byte 0) or raw Mode 1/Mode 2 form.
  // Detect the standard raw-sector sync/header instead of assuming one layout
  // for every disc. Mode 1 user data begins at byte 16; Mode 2 begins at 24.
  std::array<u8, CDImage::RAW_SECTOR_SIZE> raw_sector;
  if (!s_gv_runtime->media->Seek(translated_lba) ||
      s_gv_runtime->media->Read(CDImage::ReadMode::RawSector, 1, raw_sector.data()) != 1)
  {
    return false;
  }

  bool has_raw_sync = raw_sector[0] == 0x00 && raw_sector[11] == 0x00;
  for (u32 i = 1; has_raw_sync && i < 11; i++)
    has_raw_sync = raw_sector[i] == 0xff;

  u32 data_offset = 0;
  if (has_raw_sync)
  {
    if (raw_sector[15] == 0x01)
      data_offset = 16;
    else if (raw_sector[15] == 0x02)
      data_offset = 24;
    else
      return false;
  }

  std::memcpy(buffer, raw_sector.data() + data_offset, CDImage::DATA_SECTOR_SIZE);
  return true;
}

u32 BuildGVTOC(bool msf, u8 requested_track, u8* response, u32 response_size)
{
  if (!s_gv_runtime || !s_gv_runtime->media || !response || response_size < 4)
    return 0;
  const u8 first_track = static_cast<u8>(s_gv_runtime->media->GetFirstTrackNumber());
  const u8 last_track = static_cast<u8>(s_gv_runtime->media->GetLastTrackNumber());
  std::memset(response, 0, response_size);
  response[2] = first_track;
  response[3] = last_track;
  u32 write_offset = 4;
  const auto append = [&](u8 track_number, u8 control_bits, CDImage::LBA lba) {
    if ((write_offset + 8) > response_size)
      return;
    // Target CDImage metadata may omit ADR and retain only CONTROL in the high nibble.
    const u8 adr = (control_bits & 0x0f) != 0 ? (control_bits & 0x0f) : 0x01;
    const u8 control = (control_bits >> 4) & 0x0f;
    response[write_offset + 1] = static_cast<u8>((adr << 4) | control);
    response[write_offset + 2] = track_number;
    if (msf)
    {
      const CDImage::Position position = CDImage::Position::FromLBA(lba);
      response[write_offset + 5] = position.minute;
      response[write_offset + 6] = position.second;
      response[write_offset + 7] = position.frame;
    }
    else
    {
      response[write_offset + 4] = static_cast<u8>(lba >> 24);
      response[write_offset + 5] = static_cast<u8>(lba >> 16);
      response[write_offset + 6] = static_cast<u8>(lba >> 8);
      response[write_offset + 7] = static_cast<u8>(lba);
    }
    write_offset += 8;
  };
  if (requested_track != CDImage::LEAD_OUT_TRACK_NUMBER)
  {
    const u8 start_track = (requested_track == 0 || requested_track < first_track) ? first_track : requested_track;
    for (u8 track = start_track; track <= last_track; track++)
    {
      const CDImage::Track& info = s_gv_runtime->media->GetTrack(track);
      append(track, info.control.bits, info.start_lba);
      if ((write_offset + 8) > response_size)
        break;
    }
  }
  if ((write_offset + 8) <= response_size)
  {
    const CDImage::Track& info = s_gv_runtime->media->GetTrack(last_track);
    append(CDImage::LEAD_OUT_TRACK_NUMBER, info.control.bits, s_gv_runtime->media->GetLBACount());
  }
  const u16 length = static_cast<u16>(write_offset - 2);
  response[0] = static_cast<u8>(length >> 8);
  response[1] = static_cast<u8>(length);
  return write_offset;
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

std::string_view GetGVCHDPath()
{
  return s_gv_runtime ? std::string_view(s_gv_runtime->chd_path) : std::string_view{};
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
  if (!runtime.eeprom_access_logged)
  {
    INFO_LOG("KonamiGV.EXP1 first_eeprom_access canonical_set='{}'", runtime.set_name);
    runtime.eeprom_access_logged = true;
  }

  const bool di = (value & 0x01) != 0;
  const bool cs = (value & 0x02) != 0;
  const bool clk = (value & 0x04) != 0;
  const bool device_check_clock = (value & 0x10) != 0;

  if (runtime.tokimeki_enabled && !runtime.tokimeki_device_check_clock && device_check_clock)
  {
    runtime.tokimeki_device_check_value = static_cast<u16>((runtime.tokimeki_device_check_value << 1) |
                                                           (runtime.tokimeki_device_check_value >> 15));
  }
  runtime.tokimeki_device_check_clock = device_check_clock;

  if (!cs)
  {
    ResetEEPROMProtocol(runtime);
    runtime.eeprom_di = di;
    runtime.eeprom_clk = clk;
    return;
  }

  if (!runtime.eeprom_cs)
    ResetEEPROMProtocol(runtime);

  // Rising edge clocks data, matching the working PoC 93C46 path.
  if (!runtime.eeprom_clk && clk)
  {
    if (runtime.eeprom_read_bits > 0)
    {
      runtime.eeprom_do = ((runtime.eeprom_read_shift >> 15) & 1) != 0;
      runtime.eeprom_read_shift <<= 1;
      runtime.eeprom_read_bits--;
    }
    else if (runtime.eeprom_write_bits > 0)
    {
      runtime.eeprom_write_shift = static_cast<u16>((runtime.eeprom_write_shift << 1) | (di ? 1 : 0));
      runtime.eeprom_write_bits--;

      if (runtime.eeprom_write_bits == 0)
      {
        const u16 stored_value = SwapEEPROMWord16(runtime.eeprom_write_shift);

        if (runtime.eeprom_write_enabled)
        {
          if (runtime.eeprom_write_all)
          {
            for (u8 i = 0; i < 64; i++)
              SetEEPROMWord(runtime, i, stored_value);
          }
          else
          {
            SetEEPROMWord(runtime, runtime.eeprom_write_address, stored_value);
          }

          SaveEEPROM(runtime);
        }

        runtime.eeprom_shift = 0;
        runtime.eeprom_shift_count = 0;
        runtime.eeprom_write_all = false;
        runtime.eeprom_write_address = 0;
        runtime.eeprom_write_shift = 0;
        runtime.eeprom_do = true;
      }
    }
    else
    {
      // 93C46 ignores leading zeroes until the start bit.
      if (runtime.eeprom_shift_count != 0 || di)
      {
        runtime.eeprom_shift = (runtime.eeprom_shift << 1) | (di ? 1 : 0);
        runtime.eeprom_shift_count++;

        // start bit 1, opcode 2 bits, 6-bit address = 9 bits total.
        if (runtime.eeprom_shift_count == 9)
        {
          const u8 start = static_cast<u8>((runtime.eeprom_shift >> 8) & 1);
          const u8 opcode = static_cast<u8>((runtime.eeprom_shift >> 6) & 0x03);
          const u8 address = static_cast<u8>(runtime.eeprom_shift & 0x3f);
          const u16 wire_value = SwapEEPROMWord16(GetEEPROMWord(runtime, address));

          if (!start)
          {
            runtime.eeprom_shift = 0;
            runtime.eeprom_shift_count = 0;
          }
          else if (opcode == 0x02)
          {
            // A 93C46 outputs a dummy zero immediately after the command,
            // followed by the 16-bit word MSB-first.
            runtime.eeprom_read_address = address;
            runtime.eeprom_read_shift = wire_value;
            runtime.eeprom_read_bits = 16;
            runtime.eeprom_do = false;
          }
          else if (opcode == 0x01)
          {
            runtime.eeprom_write_all = false;
            runtime.eeprom_write_address = address;
            runtime.eeprom_write_shift = 0;
            runtime.eeprom_write_bits = 16;
            runtime.eeprom_do = false;
          }
          else if (opcode == 0x03)
          {
            if (runtime.eeprom_write_enabled)
            {
              SetEEPROMWord(runtime, address, 0xffff);
              SaveEEPROM(runtime);
            }

            runtime.eeprom_do = true;
            runtime.eeprom_shift = 0;
            runtime.eeprom_shift_count = 0;
          }
          else
          {
            switch ((address >> 4) & 0x03)
            {
              case 0:
                runtime.eeprom_write_enabled = false;
                runtime.eeprom_do = true;
                runtime.eeprom_shift = 0;
                runtime.eeprom_shift_count = 0;
                break;

              case 1:
                runtime.eeprom_write_all = true;
                runtime.eeprom_write_address = 0;
                runtime.eeprom_write_shift = 0;
                runtime.eeprom_write_bits = 16;
                runtime.eeprom_do = false;
                break;

              case 2:
                if (runtime.eeprom_write_enabled)
                {
                  for (u8 i = 0; i < 64; i++)
                    SetEEPROMWord(runtime, i, 0xffff);
                  SaveEEPROM(runtime);
                }

                runtime.eeprom_do = true;
                runtime.eeprom_shift = 0;
                runtime.eeprom_shift_count = 0;
                break;

              case 3:
                runtime.eeprom_write_enabled = true;
                runtime.eeprom_do = true;
                runtime.eeprom_shift = 0;
                runtime.eeprom_shift_count = 0;
                break;
            }
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

u32 ReadGVPlayer1Status(u32 size, u32 offset)
{
  static_cast<void>(size);
  static_cast<void>(offset);
  if (!s_gv_runtime)
    return 0xffffffff;
  GVRuntimeState& runtime = *s_gv_runtime;
  if (!runtime.eeprom_access_logged)
  {
    INFO_LOG("KonamiGV.EXP1 first_eeprom_access canonical_set='{}'", runtime.set_name);
    runtime.eeprom_access_logged = true;
  }
  u32 value = runtime.buttons[0];
  if (runtime.btchamp_first_boot_state == BtChampFirstBootState::HoldingTest)
    value &= ~(1U << 12);
  if (runtime.eeprom_do)
    value |= (1U << 13);
  else
    value &= ~(1U << 13);
  return value;
}

u32 ReadGVPlayer2Status(u32 size, u32 offset)
{
  static_cast<void>(size);
  static_cast<void>(offset);
  if (!s_gv_runtime)
    return 0xffffffff;
  GVRuntimeState& runtime = *s_gv_runtime;
  u32 value = runtime.buttons[1];
  if (runtime.tokimeki_enabled)
  {
    value &= ~0x00000400U;
    value |= static_cast<u32>((runtime.tokimeki_device_check_value >> 15) & 1U) << 10;
  }
  return value;
}

void WriteGVPlayerStatus(u32 size, u32 offset, u32 value)
{
  static_cast<void>(size);
  static_cast<void>(offset);
  static_cast<void>(value);
}

u32 ReadGVSpecialIO(u32 size, u32 offset)
{
  static_cast<void>(size);
  if (!s_gv_runtime)
    return 0xffffffff;
  GVRuntimeState& runtime = *s_gv_runtime;

  if (IsSet(runtime, "kdeadeye"))
  {
    if (offset >= 0x00680000 && offset <= 0x0068001f)
      return 0;
    if (offset >= 0x00680080 && offset <= 0x00680083)
      return GetLightgunX(runtime, 0);
    if (offset >= 0x00680090 && offset <= 0x00680093)
      return GetLightgunY(runtime, 0);
    if (offset >= 0x006800a0 && offset <= 0x006800a3)
      return GetLightgunX(runtime, 1);
    if (offset >= 0x006800b0 && offset <= 0x006800b3)
      return GetLightgunY(runtime, 1);
    if (offset >= 0x006800c0 && offset <= 0x006800c3)
    {
      std::lock_guard<std::mutex> lock(s_lightgun_mutex);
      u32 value = 0xffff;
      if (runtime.lightgun_trigger[0])
        value &= ~0x0001U;
      if (runtime.lightgun_trigger[1])
        value &= ~0x0002U;
      return value;
    }
    if (offset >= 0x006800e0 && offset <= 0x006800e3)
      return 0;
  }

  if (runtime.tokimeki_enabled && offset >= 0x00680080 && offset < 0x00680082)
    return ReadTokimekiSerial(runtime);

  if (IsSet(runtime, "btchamp") && offset >= 0x00680080 && offset < 0x00680090)
  {
    if (offset == 0x00680086)
    {
      LatchTrackball(runtime, 0);
      LatchTrackball(runtime, 1);
    }
    switch (offset & ~1U)
    {
      case 0x00680080: return ((runtime.trackball_x[0] & 0x00ff) << 8) | (runtime.trackball_x[1] & 0x00ff);
      case 0x00680082: return (((runtime.trackball_x[0] >> 8) & 0x000f) << 8) | ((runtime.trackball_x[1] >> 8) & 0x000f);
      case 0x00680084: return ((runtime.trackball_y[0] & 0x00ff) << 8) | (runtime.trackball_y[1] & 0x00ff);
      case 0x00680086: return (((runtime.trackball_y[0] >> 8) & 0x000f) << 8) | ((runtime.trackball_y[1] >> 8) & 0x000f);
      default: return 0;
    }
  }

  if (offset >= 0x006800c0 && offset <= 0x006800c9)
  {
    if (offset == 0x006800c0)
      LatchTrackball(runtime, 0);
    switch (offset & ~1U)
    {
      case 0x006800c0: return (runtime.trackball_x[0] & 0x00ff) << 8;
      case 0x006800c2: return runtime.trackball_x[0] & 0x0f00;
      case 0x006800c4: return (runtime.trackball_y[0] & 0x00ff) << 8;
      case 0x006800c6: return runtime.trackball_y[0] & 0x0f00;
      default: return 0;
    }
  }

  return 0xffffffff;
}

void WriteGVSpecialIO(u32 size, u32 offset, u32 value)
{
  static_cast<void>(size);
  if (!s_gv_runtime)
    return;
  GVRuntimeState& runtime = *s_gv_runtime;

  if (runtime.tokimeki_enabled && offset >= 0x00680090 && offset < 0x00680092)
  {
    WriteTokimekiSerial(runtime, value);
    return;
  }

  if (IsSet(runtime, "btchamp") && (offset == 0x00680088 || offset == 0x0068008a))
  {
    const bool reset_active = (value & 0x0001) == 0;
    if (reset_active && !runtime.trackball_reset_active)
    {
      std::lock_guard<std::mutex> lock(s_trackball_mutex);
      runtime.trackball_start_x = runtime.trackball_counter_x;
      runtime.trackball_start_y = runtime.trackball_counter_y;
      runtime.trackball_x = {};
      runtime.trackball_y = {};
    }
    runtime.trackball_reset_active = reset_active;
    return;
  }

  // Dead Eye DUART/control and lightgun writes are valid no-ops in the PoC.
  if (IsSet(runtime, "kdeadeye") && offset >= 0x00680000 && offset <= 0x006800e3)
    return;
}

void WriteGVWatchdog(u32 value)
{
  static_cast<void>(value);
  if (s_gv_runtime)
    s_gv_runtime->watchdog_frames_remaining = GV_WATCHDOG_TIMEOUT_FRAMES;
}

bool ProcessGVFrame()
{
  if (!s_gv_runtime)
    return false;
  GVRuntimeState& runtime = *s_gv_runtime;
  if (runtime.tokimeki_enabled)
  {
    if (runtime.tokimeki_heartbeat_pulse_frames_remaining > 0 &&
        --runtime.tokimeki_heartbeat_pulse_frames_remaining == 0)
    {
      runtime.tokimeki_heartbeat_signal = true;
    }
    if (runtime.tokimeki_heartbeat_frames_remaining > 0)
      runtime.tokimeki_heartbeat_frames_remaining--;
    if (runtime.tokimeki_heartbeat_frames_remaining == 0)
    {
      runtime.tokimeki_heartbeat_signal = false;
      runtime.tokimeki_heartbeat_pulse_frames_remaining = TOKIMEKI_HEARTBEAT_PULSE_FRAMES;
      runtime.tokimeki_heartbeat_frames_remaining = GetTokimekiHeartbeatPeriodFrames(runtime);
    }
    if (runtime.tokimeki_printer_busy_frames_remaining > 0 &&
        --runtime.tokimeki_printer_busy_frames_remaining == 0)
    {
      runtime.tokimeki_printer_busy = false;
    }
  }
  if (runtime.btchamp_first_boot_state == BtChampFirstBootState::ResetRequested)
  {
    runtime.btchamp_first_boot_state = BtChampFirstBootState::HoldingTest;
    runtime.watchdog_frames_remaining = 0;
    INFO_LOG("KonamiGV.SharpFlash btchamp_first_boot_recovery_started canonical_set='{}'", runtime.set_name);
    return true;
  }

  if (runtime.watchdog_frames_remaining == 0)
    return false;
  runtime.watchdog_frames_remaining--;
  return runtime.watchdog_frames_remaining == 0;
}

void SetGVButtons(u32 player, u32 buttons)
{
  if (!s_gv_runtime || player >= s_gv_runtime->buttons.size())
    return;
  s_gv_runtime->buttons[player] = buttons;
}

void SetGVControllerButtons(u32 player, u32 active_low_psx_buttons)
{
  if (!s_gv_runtime || player >= s_gv_runtime->buttons.size())
    return;

  u32 value = 0xffffffff;
  const auto map_button = [&value, active_low_psx_buttons](u32 psx_mask, u32 gv_mask) {
    if ((active_low_psx_buttons & psx_mask) == 0)
      value &= ~gv_mask;
  };

  map_button(0x0080, 1U << 0);  // Left
  map_button(0x0020, 1U << 1);  // Right
  map_button(0x0010, 1U << 2);  // Up
  map_button(0x0040, 1U << 3);  // Down
  map_button(0x4000, 1U << 4);  // Button 1 / Cross
  map_button(0x2000, 1U << 5);  // Button 2 / Circle
  map_button(0x0200, 1U << 6);  // Button 3 / R2
  map_button(0x8000, 1U << 7);  // Button 4 / Square
  map_button(0x0008, 1U << 9);  // Start
  map_button(0x0001, 1U << 10); // Coin / Select

  s_gv_runtime->buttons[player] = value;
}

void SetGVArcadeButton(u32 player, u32 button_mask, bool pressed)
{
  if (!s_gv_runtime || player >= s_gv_runtime->buttons.size())
    return;
  if (pressed)
    s_gv_runtime->buttons[player] &= ~button_mask;
  else
    s_gv_runtime->buttons[player] |= button_mask;
}

void SetGVTrackballPosition(u16 x, u16 y)
{
  if (!s_gv_runtime)
    return;
  std::lock_guard<std::mutex> lock(s_trackball_mutex);
  s_gv_runtime->trackball_x[0] = x & 0x0fff;
  s_gv_runtime->trackball_y[0] = y & 0x0fff;
}

void AddGVTrackballDelta(u32 player, s32 x, s32 y)
{
  if (!s_gv_runtime || player >= s_gv_runtime->trackball_pending_x.size())
    return;
  GVRuntimeState& runtime = *s_gv_runtime;
  std::lock_guard<std::mutex> lock(s_trackball_mutex);
  const s32 adjusted_x = IsSet(runtime, "btchamp") ? x : -x;
  runtime.trackball_pending_x[player] += static_cast<double>(adjusted_x) * runtime.trackball_sensitivity;
  runtime.trackball_pending_y[player] += static_cast<double>(y) * runtime.trackball_sensitivity;
}

void AddGVTrackballDelta(s32 x, s32 y)
{
  AddGVTrackballDelta(0, x, y);
}

void SetGVLightgunPosition(u32 player, float x, float y)
{
  if (!s_gv_runtime || player >= s_gv_runtime->lightgun_normalized_x.size())
    return;
  GVRuntimeState& runtime = *s_gv_runtime;
  std::lock_guard<std::mutex> lock(s_lightgun_mutex);
  runtime.lightgun_offscreen[player] = x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f;
  runtime.lightgun_normalized_x[player] = std::clamp(x, 0.0f, 1.0f);
  runtime.lightgun_normalized_y[player] = std::clamp(y, 0.0f, 1.0f);
}

void SetGVLightgunTrigger(u32 player, bool pressed)
{
  if (!s_gv_runtime || player >= s_gv_runtime->lightgun_trigger.size())
    return;
  std::lock_guard<std::mutex> lock(s_lightgun_mutex);
  s_gv_runtime->lightgun_trigger[player] = pressed;
}

void SetGVLightgunShootOffscreen(u32 player, bool pressed)
{
  if (!s_gv_runtime || player >= s_gv_runtime->lightgun_shoot_offscreen.size())
    return;
  std::lock_guard<std::mutex> lock(s_lightgun_mutex);
  s_gv_runtime->lightgun_shoot_offscreen[player] = pressed;
}

void AdjustGVTokimekiExcitement(s32 direction)
{
  if (!s_gv_runtime || !s_gv_runtime->tokimeki_enabled || direction == 0)
    return;
  GVRuntimeState& runtime = *s_gv_runtime;
  const u32 previous = runtime.tokimeki_excitement_level;
  if (direction > 0 && runtime.tokimeki_excitement_level < TOKIMEKI_MAX_EXCITEMENT_LEVEL)
    runtime.tokimeki_excitement_level++;
  else if (direction < 0 && runtime.tokimeki_excitement_level > 0)
    runtime.tokimeki_excitement_level--;
  if (runtime.tokimeki_excitement_level != previous)
  {
    UpdateTokimekiExcitementValues(runtime);
    runtime.tokimeki_heartbeat_signal = true;
    runtime.tokimeki_heartbeat_frames_remaining = GetTokimekiHeartbeatPeriodFrames(runtime);
    runtime.tokimeki_heartbeat_pulse_frames_remaining = 0;
  }
}

u32 ReadGVEEPROM(u32 size, u32 offset)
{
  static_cast<void>(size);
  if (!s_gv_runtime)
    return 0xffffffff;
  if (offset >= 0x00180080 && offset < 0x00180100)
  {
    const u8 address = static_cast<u8>(((offset - 0x00180080) & 0x7f) >> 1);
    return GetEEPROMWord(*s_gv_runtime, address);
  }
  return 0;
}

void WriteGVEEPROM(u32 size, u32 offset, u32 value)
{
  static_cast<void>(size);
  if (!s_gv_runtime)
    return;
  if (offset >= 0x00180000 && offset < 0x00180004)
  {
    WriteEEPROMControl(value);
    return;
  }
  if (offset >= 0x00180080 && offset < 0x00180100)
  {
    const u8 address = static_cast<u8>(((offset - 0x00180080) & 0x7f) >> 1);
    SetEEPROMWord(*s_gv_runtime, address, static_cast<u16>(value));
  }
}

u32 ReadSharpFlash(u32 size, u32 offset)
{
  if (s_gv_runtime && s_gv_runtime->sharp_flash_active && !s_gv_runtime->sharp_flash_access_logged)
  {
    INFO_LOG("KonamiGV.EXP1 first_sharp_flash_access canonical_set='{}'", s_gv_runtime->set_name);
    s_gv_runtime->sharp_flash_access_logged = true;
  }
  return s_gv_runtime ? ReadSharpFlashImpl(*s_gv_runtime, size, offset) : 0xffffffff;
}

void WriteSharpFlash(u32 size, u32 offset, u32 value)
{
  if (s_gv_runtime)
  {
    if (s_gv_runtime->sharp_flash_active && !s_gv_runtime->sharp_flash_access_logged)
    {
      INFO_LOG("KonamiGV.EXP1 first_sharp_flash_access canonical_set='{}'", s_gv_runtime->set_name);
      s_gv_runtime->sharp_flash_access_logged = true;
    }
    WriteSharpFlashImpl(*s_gv_runtime, size, offset, value);
  }
}

u32 ReadFujitsuFlash(u32 size, u32 offset)
{
  if (s_gv_runtime && s_gv_runtime->fujitsu_flash_active && !s_gv_runtime->fujitsu_flash_access_logged)
  {
    INFO_LOG("KonamiGV.EXP1 first_fujitsu_flash_access canonical_set='{}'", s_gv_runtime->set_name);
    s_gv_runtime->fujitsu_flash_access_logged = true;
  }
  return s_gv_runtime ? ReadFujitsuFlashImpl(*s_gv_runtime, size, offset) : 0xffffffff;
}

void WriteFujitsuFlash(u32 size, u32 offset, u32 value)
{
  if (s_gv_runtime)
  {
    if (s_gv_runtime->fujitsu_flash_active && !s_gv_runtime->fujitsu_flash_access_logged)
    {
      INFO_LOG("KonamiGV.EXP1 first_fujitsu_flash_access canonical_set='{}'", s_gv_runtime->set_name);
      s_gv_runtime->fujitsu_flash_access_logged = true;
    }
    WriteFujitsuFlashImpl(*s_gv_runtime, size, offset, value);
  }
}

void NotifyGVDeferredEXP1Access(GVDeferredEXP1Range range, u32 physical_address, u32 width, bool is_write, u32 value)
{
  if (!s_gv_runtime)
    return;

  bool* logged = nullptr;
  const char* category = nullptr;
  switch (range)
  {
    case GVDeferredEXP1Range::Input: logged = &s_gv_runtime->input_access_logged; category = "input"; break;
    case GVDeferredEXP1Range::Watchdog: logged = &s_gv_runtime->watchdog_access_logged; category = "watchdog"; break;
    case GVDeferredEXP1Range::GameSpecific: logged = &s_gv_runtime->game_specific_access_logged; category = "game_specific"; break;
  }
  if (logged && !*logged)
  {
    INFO_LOG("KonamiGV.EXP1 deferred_access canonical_set='{}' category='{}' address=0x{:08X} width={} direction='{}' value=0x{:08X}",
             s_gv_runtime->set_name, category, physical_address, width, is_write ? "write" : "read", value);
    *logged = true;
  }
}

bool DoGVState(StateWrapper& sw)
{
  if (!s_gv_runtime)
    return true;
  GVRuntimeState& r = *s_gv_runtime;
  if (!KonamiGVScsi::DoState(sw))
    return false;
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
    sw.Do(&r.sharp_flash_busy_until_ticks);
    sw.Do(&r.sharp_flash_master_lock);
    sw.Do(&r.sharp_flash_dirty);
    u8 btchamp_first_boot_state = static_cast<u8>(r.btchamp_first_boot_state);
    sw.Do(&btchamp_first_boot_state);
    if (sw.IsReading())
    {
      if (sharp_flash_mode > static_cast<u8>(SharpFlashMode::SetMaster))
      {
        ResetSharpFlash(r);
      }
      else
      {
        r.sharp_flash_mode = static_cast<SharpFlashMode>(sharp_flash_mode);
      }

      if (btchamp_first_boot_state > static_cast<u8>(BtChampFirstBootState::HoldingTest))
        r.btchamp_first_boot_state = BtChampFirstBootState::Inactive;
      else
        r.btchamp_first_boot_state = static_cast<BtChampFirstBootState>(btchamp_first_boot_state);
    }
  }
  if (r.fujitsu_flash_active)
  {
    std::array<u8, GV_FUJITSU_FLASH_CHIP_COUNT> fujitsu_flash_modes;
    for (u32 chip = 0; chip < GV_FUJITSU_FLASH_CHIP_COUNT; chip++)
    {
      sw.DoBytes(r.fujitsu_flash[chip].data(), r.fujitsu_flash[chip].size());
      fujitsu_flash_modes[chip] = static_cast<u8>(r.fujitsu_flash_modes[chip]);
      sw.Do(&fujitsu_flash_modes[chip]);
      sw.Do(&r.fujitsu_flash_dirty[chip]);
    }
    sw.Do(&r.fujitsu_flash_address);
    if (sw.IsReading())
    {
      bool invalid_mode = false;
      for (u32 chip = 0; chip < GV_FUJITSU_FLASH_CHIP_COUNT; chip++)
      {
        if (fujitsu_flash_modes[chip] > static_cast<u8>(FujitsuFlashMode::EraseSelect))
          invalid_mode = true;
        else
          r.fujitsu_flash_modes[chip] = static_cast<FujitsuFlashMode>(fujitsu_flash_modes[chip]);
      }
      if (invalid_mode)
        ResetFujitsuFlash(r);
    }
  }
  sw.DoBytes(r.buttons.data(), sizeof(r.buttons));
  sw.DoBytes(r.trackball_pending_x.data(), sizeof(r.trackball_pending_x));
  sw.DoBytes(r.trackball_pending_y.data(), sizeof(r.trackball_pending_y));
  sw.DoBytes(r.trackball_x.data(), sizeof(r.trackball_x));
  sw.DoBytes(r.trackball_y.data(), sizeof(r.trackball_y));
  sw.Do(&r.trackball_counter_x);
  sw.Do(&r.trackball_counter_y);
  sw.Do(&r.trackball_start_x);
  sw.Do(&r.trackball_start_y);
  sw.Do(&r.trackball_reset_active);
  sw.Do(&r.trackball_sensitivity);
  sw.DoBytes(r.lightgun_normalized_x.data(), sizeof(r.lightgun_normalized_x));
  sw.DoBytes(r.lightgun_normalized_y.data(), sizeof(r.lightgun_normalized_y));
  sw.DoBytes(r.lightgun_trigger.data(), sizeof(r.lightgun_trigger));
  sw.DoBytes(r.lightgun_offscreen.data(), sizeof(r.lightgun_offscreen));
  sw.DoBytes(r.lightgun_shoot_offscreen.data(), sizeof(r.lightgun_shoot_offscreen));
  sw.Do(&r.watchdog_frames_remaining);
  sw.Do(&r.tokimeki_enabled);
  sw.Do(&r.tokimeki_device_check_value);
  sw.Do(&r.tokimeki_device_check_clock);
  sw.Do(&r.tokimeki_excitement_level);
  sw.Do(&r.tokimeki_heartbeat_rate);
  sw.Do(&r.tokimeki_heartbeat_signal);
  sw.Do(&r.tokimeki_heartbeat_frames_remaining);
  sw.Do(&r.tokimeki_heartbeat_pulse_frames_remaining);
  sw.Do(&r.tokimeki_gsr_value);
  sw.Do(&r.tokimeki_serial_value);
  sw.Do(&r.tokimeki_serial_length);
  sw.Do(&r.tokimeki_serial_clock);
  sw.Do(&r.tokimeki_serial_sensor_id);
  sw.Do(&r.tokimeki_serial_sensor_data);
  sw.Do(&r.tokimeki_printer_bit);
  sw.DoBytes(r.tokimeki_printer_data.data(), r.tokimeki_printer_data.size());
  sw.Do(&r.tokimeki_printer_current_bit);
  sw.Do(&r.tokimeki_printer_current_byte);
  sw.Do(&r.tokimeki_printer_pulse_start_ticks);
  sw.Do(&r.tokimeki_printer_busy);
  sw.Do(&r.tokimeki_printer_busy_frames_remaining);
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
