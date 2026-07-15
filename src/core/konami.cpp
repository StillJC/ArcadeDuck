#include "bus.h"
#include "common/cd_image.h"
#include "common/file_system.h"
#include "common/log.h"
#include "cpu_core.h"
#include "host_display.h"
#include "host_interface.h"
#include "konami.h"
#include "konami_gv_scsi.h"
#include "system.h"
#include "timers.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdio.h>
#include <vector>

Log_SetChannel(Konami);

// Buttons
static u32 CurrentButtons[2];

// Tokimeki Memorial daughterboard
static bool TokimekiDeviceCheckEnabled = false;
static u16 TokimekiDeviceCheckValue = 0;
static bool TokimekiDeviceCheckClock = false;
static constexpr u32 TOKIMEKI_DEFAULT_HEARTBEAT_RATE = 80;
static constexpr u32 TOKIMEKI_HEARTBEAT_RATE_STEP = 10;
static constexpr u32 TOKIMEKI_MAX_EXCITEMENT_LEVEL = 7;
static constexpr u8 TOKIMEKI_DEFAULT_GSR_VALUE = 0x20;
static constexpr u8 TOKIMEKI_MAX_GSR_VALUE = 0x80;

static u32 TokimekiExcitementLevel = 0;
static u32 TokimekiHeartbeatRate = TOKIMEKI_DEFAULT_HEARTBEAT_RATE;
static bool TokimekiHeartbeatSignal = true;
static u32 TokimekiHeartbeatFramesRemaining = 45;
static u8 TokimekiGSRValue = TOKIMEKI_DEFAULT_GSR_VALUE;
static u8 TokimekiSerialValue = 0;
static u8 TokimekiSerialLength = 0;
static bool TokimekiSerialClock = false;
static u8 TokimekiSerialSensorId = 0;
static u16 TokimekiSerialSensorData = 0;

static u32 KonamiTokimekiGetHeartbeatPeriodFrames()
{
  return (3600U + TokimekiHeartbeatRate - 1U) / TokimekiHeartbeatRate;
}

static void KonamiTokimekiUpdateExcitementValues()
{
  TokimekiHeartbeatRate = TOKIMEKI_DEFAULT_HEARTBEAT_RATE + (TokimekiExcitementLevel * TOKIMEKI_HEARTBEAT_RATE_STEP);

  TokimekiGSRValue = static_cast<u8>(
    TOKIMEKI_DEFAULT_GSR_VALUE + (((TOKIMEKI_MAX_GSR_VALUE - TOKIMEKI_DEFAULT_GSR_VALUE) * TokimekiExcitementLevel) /
                                  TOKIMEKI_MAX_EXCITEMENT_LEVEL));
}

// Konami GV flash state
static constexpr u32 KONAMI_GV_FUJITSU_FLASH_SIZE = 0x200000;
static constexpr u32 KONAMI_GV_FUJITSU_FLASH_SECTOR_SIZE = 0x10000;
static constexpr u32 KONAMI_GV_FUJITSU_FLASH_CHIP_COUNT = 4;
static constexpr u32 KONAMI_GV_FUJITSU_FLASH_CHIPS_PER_PAIR = 2;
static constexpr u32 KONAMI_GV_FUJITSU_FLASH_PAIR_SECTOR_COUNT =
  (KONAMI_GV_FUJITSU_FLASH_SIZE * KONAMI_GV_FUJITSU_FLASH_CHIPS_PER_PAIR) / CDImage::DATA_SECTOR_SIZE;

static constexpr u32 KONAMI_GV_FUJITSU_SIMPSONS_PAIR_01_LBA = 202;
static constexpr u32 KONAMI_GV_FUJITSU_SIMPSONS_PAIR_23_LBA = 2250;

static constexpr u32 KONAMI_GV_SHARP_FLASH_SIZE = 0x80000;
static constexpr u32 KONAMI_GV_SHARP_FLASH_SECTOR_SIZE = 0x10000;

static u8 GVFujitsuFlash[KONAMI_GV_FUJITSU_FLASH_CHIP_COUNT][KONAMI_GV_FUJITSU_FLASH_SIZE];
static u32 GVFujitsuFlashAddress;

static u8 GVSharpFlash[KONAMI_GV_SHARP_FLASH_SIZE];

// The Sharp flash chip exists on supported GV hardware even when no
// persisted flash file exists yet.
static bool GVSharpFlashPresent = false;

static bool GVSharpFlashDirty = false;
static std::string GVSharpFlashPath;

enum KonamiGVSharpFlashMode : u8
{
  KONAMI_GV_SHARP_FLASH_MODE_READ_ARRAY,
  KONAMI_GV_SHARP_FLASH_MODE_READ_STATUS,
  KONAMI_GV_SHARP_FLASH_MODE_PROGRAM,
  KONAMI_GV_SHARP_FLASH_MODE_ERASE_SETUP
};

static KonamiGVSharpFlashMode GVSharpFlashMode = KONAMI_GV_SHARP_FLASH_MODE_READ_ARRAY;
static u16 GVSharpFlashStatus = 0x0080;

enum class KonamiGVBtChampFirstBootState : u8
{
  Inactive,
  WaitingForValidationEnd,
  WaitingForValidationRestart,
  ResetRequested,
  HoldingTest
};

static KonamiGVBtChampFirstBootState GVBtChampFirstBootState = KonamiGVBtChampFirstBootState::Inactive;

// Konami GV watchdog.
// The exact hardware timeout is not documented, so use a conservative
// two-second provisional timeout until hardware timing is better established.
static constexpr u32 KONAMI_GV_WATCHDOG_TIMEOUT_FRAMES = 120;
static u32 KonamiGVWatchdogFramesRemaining = 0;

static bool KonamiGVSharpFlashIsErased()
{
  for (const u8 value : GVSharpFlash)
  {
    if (value != 0xFF)
      return false;
  }

  return true;
}

static void KonamiGVBtChampObserveBlankFlashRead(u32 size, u32 relative_offset, u32 value)
{
  if (size != 2 || value != 0xFFFF)
    return;

  switch (GVBtChampFirstBootState)
  {
    case KonamiGVBtChampFirstBootState::WaitingForValidationEnd:
      if (relative_offset == 0x000007A6)
      {
        GVBtChampFirstBootState = KonamiGVBtChampFirstBootState::WaitingForValidationRestart;
      }
      break;

    case KonamiGVBtChampFirstBootState::WaitingForValidationRestart:
      if (relative_offset == 0x00000008)
      {
        GVBtChampFirstBootState = KonamiGVBtChampFirstBootState::ResetRequested;

        Log_InfoPrintf("KonamiGV: Beat the Champ blank flash validation completed; queued automatic recovery reset");
      }
      break;

    default:
      break;
  }
}

static bool KonamiGVSharpFlashLoadFile(const std::string& path)
{
  for (u32 i = 0; i < KONAMI_GV_SHARP_FLASH_SIZE; i++)
    GVSharpFlash[i] = 0xFF;

  std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "rb");
  if (!fp)
    return false;

  std::fseek(fp, 0, SEEK_END);
  const long size = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);

  if (size != static_cast<long>(KONAMI_GV_SHARP_FLASH_SIZE))
  {
    std::fclose(fp);
    return false;
  }

  const size_t read = std::fread(GVSharpFlash, 1, KONAMI_GV_SHARP_FLASH_SIZE, fp);
  std::fclose(fp);

  return (read == KONAMI_GV_SHARP_FLASH_SIZE);
}

static void KonamiGVSharpFlashSaveFile()
{
  if (!GVSharpFlashPresent || !GVSharpFlashDirty || GVSharpFlashPath.empty())
    return;

  std::FILE* fp = FileSystem::OpenCFile(GVSharpFlashPath.c_str(), "wb");
  if (!fp)
    return;

  std::fwrite(GVSharpFlash, 1, KONAMI_GV_SHARP_FLASH_SIZE, fp);
  std::fclose(fp);

  GVSharpFlashDirty = false;
}

static u16 KonamiGVSharpFlashReadArray16(u32 relative_offset)
{
  relative_offset &= (KONAMI_GV_SHARP_FLASH_SIZE - 1);

  const u32 high_offset = relative_offset & ~1U;
  const u32 low_offset = (high_offset + 1) & (KONAMI_GV_SHARP_FLASH_SIZE - 1);

  return static_cast<u16>((GVSharpFlash[high_offset] << 8) | GVSharpFlash[low_offset]);
}

static u16 KonamiGVSharpFlashRead16(u32 relative_offset)
{
  if (!GVSharpFlashPresent)
    return 0xFFFF;

  return KonamiGVSharpFlashReadArray16(relative_offset);
}

void KonamiGVSharpFlashRead(u32 Size, u32 Offset, u32& Value)
{
  const u32 relative_offset = (Offset >= 0x00380000) ? (Offset - 0x00380000) : Offset;

  if (GVSharpFlashPresent && GVSharpFlashMode == KONAMI_GV_SHARP_FLASH_MODE_READ_STATUS)
  {
  // The Sharp LH28F400 is a 16-bit flash device.
  // Its ready status is 0x0080, not 0x8080.
  Value = GVSharpFlashStatus;

  if (Size == 4)
    Value |= Value << 16;

  return;
}

  switch (Size)
  {
    case 1:
    {
      if (GVSharpFlashPresent)
        Value = GVSharpFlash[relative_offset & (KONAMI_GV_SHARP_FLASH_SIZE - 1)];
      else
        Value = 0xFF;

      break;
    }

    case 2:
      Value = KonamiGVSharpFlashRead16(relative_offset);
      break;

    case 4:
    {
      const u16 low = KonamiGVSharpFlashRead16(relative_offset);
      const u16 high = KonamiGVSharpFlashRead16(relative_offset + 2);
      Value = static_cast<u32>(low) | (static_cast<u32>(high) << 16);
      break;
    }

    default:
      Value = 0xFFFFFFFF;
      break;
  }

  KonamiGVBtChampObserveBlankFlashRead(Size, relative_offset, Value);
}

void KonamiGVSharpFlashWrite(u32 Size, u32 Offset, u32 Value)
{
  const u32 relative_offset = (Offset >= 0x00380000) ? (Offset - 0x00380000) : Offset;
  const u32 byte_offset = relative_offset & (KONAMI_GV_SHARP_FLASH_SIZE - 1);
  const u16 command = static_cast<u16>(Value & 0xFF);

  if (!GVSharpFlashPresent)
    return;

if (GVSharpFlashMode == KONAMI_GV_SHARP_FLASH_MODE_PROGRAM)
  {
    switch (Size)
    {
      case 1:
        GVSharpFlash[byte_offset] = static_cast<u8>(Value & 0xFF);
        break;

      case 2:
      {
        const u32 high_offset = byte_offset & ~1U;
        const u32 low_offset = (high_offset + 1) & (KONAMI_GV_SHARP_FLASH_SIZE - 1);

        GVSharpFlash[high_offset] = static_cast<u8>((Value >> 8) & 0xFF);
        GVSharpFlash[low_offset] = static_cast<u8>(Value & 0xFF);
        break;
      }

      case 4:
      {
        const u32 first_offset = byte_offset & ~1U;
        const u32 second_offset = (first_offset + 2) & (KONAMI_GV_SHARP_FLASH_SIZE - 1);

        const u16 low_word = static_cast<u16>(Value & 0xFFFF);
        const u16 high_word = static_cast<u16>((Value >> 16) & 0xFFFF);

        GVSharpFlash[first_offset] = static_cast<u8>((low_word >> 8) & 0xFF);

        GVSharpFlash[(first_offset + 1) & (KONAMI_GV_SHARP_FLASH_SIZE - 1)] = static_cast<u8>(low_word & 0xFF);

        GVSharpFlash[second_offset] = static_cast<u8>((high_word >> 8) & 0xFF);

        GVSharpFlash[(second_offset + 1) & (KONAMI_GV_SHARP_FLASH_SIZE - 1)] = static_cast<u8>(high_word & 0xFF);
        break;
      }
    }

    GVSharpFlashDirty = true;
    GVSharpFlashStatus = 0x0080;
    GVSharpFlashMode = KONAMI_GV_SHARP_FLASH_MODE_READ_STATUS;
    return;
  }

  if (GVSharpFlashMode == KONAMI_GV_SHARP_FLASH_MODE_ERASE_SETUP)
  {
    if (command == 0xD0)
    {
      const u32 block_start = byte_offset & ~(KONAMI_GV_SHARP_FLASH_SECTOR_SIZE - 1);

      for (u32 i = 0; i < KONAMI_GV_SHARP_FLASH_SECTOR_SIZE; i++)
        GVSharpFlash[(block_start + i) & (KONAMI_GV_SHARP_FLASH_SIZE - 1)] = 0xFF;

      if (GVBtChampFirstBootState == KonamiGVBtChampFirstBootState::HoldingTest)
      {
        GVBtChampFirstBootState = KonamiGVBtChampFirstBootState::Inactive;

        Log_InfoPrintf("KonamiGV: Beat the Champ recovery entered flash initialization; released automatic Test input");
      }

      GVSharpFlashDirty = true;
      GVSharpFlashStatus = 0x0080;
      GVSharpFlashMode = KONAMI_GV_SHARP_FLASH_MODE_READ_STATUS;
    }
    else
    {
      GVSharpFlashStatus = 0x00B0;
      GVSharpFlashMode = KONAMI_GV_SHARP_FLASH_MODE_READ_STATUS;
    }

    return;
  }

  switch (command)
  {
    case 0xFF:
      // Read array / reset.
      GVSharpFlashMode = KONAMI_GV_SHARP_FLASH_MODE_READ_ARRAY;
      GVSharpFlashStatus = 0x0080;
      break;

    case 0x70:
      // Read status register.
      GVSharpFlashMode = KONAMI_GV_SHARP_FLASH_MODE_READ_STATUS;
      break;

    case 0x50:
      // Clear status register.
      GVSharpFlashStatus = 0x0080;
      break;

    case 0x40:
    case 0x10:
      // Program next write.
      GVSharpFlashMode = KONAMI_GV_SHARP_FLASH_MODE_PROGRAM;
      GVSharpFlashStatus = 0x0080;
      break;

    case 0x20:
      // Erase setup. Next write should be 0xD0.
      GVSharpFlashMode = KONAMI_GV_SHARP_FLASH_MODE_ERASE_SETUP;
      GVSharpFlashStatus = 0x0080;
      break;

    case 0xB0:
      // Suspend not currently needed; report ready.
      GVSharpFlashMode = KONAMI_GV_SHARP_FLASH_MODE_READ_STATUS;
      GVSharpFlashStatus = 0x0080;
      break;

    case 0xD0:
      // Resume/confirm without erase setup. Treat as ready.
      GVSharpFlashMode = KONAMI_GV_SHARP_FLASH_MODE_READ_STATUS;
      GVSharpFlashStatus = 0x0080;
      break;

default:
      break;
  }
}
enum KonamiGVFujitsuFlashMode : u8
{
  KONAMI_GV_FUJITSU_FLASH_MODE_READ_ARRAY = 0,
  KONAMI_GV_FUJITSU_FLASH_MODE_UNLOCK_1,
  KONAMI_GV_FUJITSU_FLASH_MODE_UNLOCK_2,
  KONAMI_GV_FUJITSU_FLASH_MODE_AUTOSELECT,
  KONAMI_GV_FUJITSU_FLASH_MODE_PROGRAM,
  KONAMI_GV_FUJITSU_FLASH_MODE_ERASE_UNLOCK_1,
  KONAMI_GV_FUJITSU_FLASH_MODE_ERASE_UNLOCK_2,
  KONAMI_GV_FUJITSU_FLASH_MODE_ERASE_SELECT
};

static KonamiGVFujitsuFlashMode GVFujitsuFlashModeState[KONAMI_GV_FUJITSU_FLASH_CHIP_COUNT];
static bool GVFujitsuFlashDirty[KONAMI_GV_FUJITSU_FLASH_CHIP_COUNT];

static std::FILE* EepromFp;
static uint16_t Eeprom[64];

// 93C46 serial EEPROM state.
// MAME maps this through P1 bit 13 for DO and EEPROMOUT bits 0/1/2 for DI/CS/CLK.
static bool EepromDi;
static bool EepromDo = true;
static bool EepromCs;
static bool EepromClk;
static u32 EepromShiftIn;
static u32 EepromShiftCount;
static u32 EepromReadShift;
static s32 EepromReadBits;
static bool EepromWriteEnabled;
static bool EepromWriteAll;
static u32 EepromWriteAddress;
static u32 EepromWriteShift;
static s32 EepromWriteBits;
static void KonamiSaveEepromFile();

// Trackball
static std::mutex TrackballMutex;

// Pending relative movement from the real mice / USB trackballs.
// These are not screen cursor coordinates.
static double TrackballPendingX[2];
static double TrackballPendingY[2];

// Latched 12-bit values exposed to the Konami GV input registers.
static u16 TrackballX[2];
static u16 TrackballY[2];
static s32 TrackballCounterX;
static s32 TrackballCounterY;
static s32 TrackballStartX;
static s32 TrackballStartY;
static bool TrackballResetActive;
static float TrackballSensitivity = 1.0f;

// Lightgun
static std::mutex LightgunMutex;

static float LightgunNormalizedX[2] = {0.5f, 0.5f};
static float LightgunNormalizedY[2] = {0.5f, 0.5f};
static bool LightgunTrigger[2] = {false, false};
static bool LightgunOffscreen[2] = {false, false};
static bool LightgunShootOffscreen[2] = {false, false};

static constexpr u16 LIGHTGUN_X_MIN = 0x004C;
static constexpr u16 LIGHTGUN_X_MAX = 0x01BB;
static constexpr u16 LIGHTGUN_Y_MIN = 0x0000;
static constexpr u16 LIGHTGUN_Y_MAX = 0x00EF;
static constexpr u16 LIGHTGUN_X_CENTER = 0x0100;
static constexpr u16 LIGHTGUN_Y_CENTER = 0x0077;
static constexpr u16 LIGHTGUN_X_OFFSCREEN = 0x0000;
static constexpr u16 LIGHTGUN_Y_OFFSCREEN = 0x00F0;

void KonamiTrackballReset();

// Score table
static struct {
        u8 Name[4];
        u16 Character;
        u16 Score;
} ScoreTable[2][10];

// Private API

static bool LoadEepromFile(const char* Path)
{
  if (std::FILE* fp = std::fopen("konami_gv_eeprom_debug.txt", "ab"))
  {
    std::fprintf(fp, "LoadEepromFile path='%s'\n", Path ? Path : "(null)");
    std::fclose(fp);
  }

  EepromFp = FileSystem::OpenCFile(Path, "r+b");
  if (!EepromFp)
  {
    if (std::FILE* fp = std::fopen("konami_gv_eeprom_debug.txt", "ab"))
    {
      std::fprintf(fp, "LoadEepromFile FAILED open\n");
      std::fclose(fp);
    }

    return false;
  }

  u8 raw[sizeof(Eeprom)];

  if (std::fread(raw, 1, sizeof(raw), EepromFp) != sizeof(raw))
  {
    if (std::FILE* fp = std::fopen("konami_gv_eeprom_debug.txt", "ab"))
    {
      std::fprintf(fp, "LoadEepromFile FAILED read\n");
      std::fclose(fp);
    }

    return false;
  }

  for (u32 i = 0; i < 64; i++)
  {
    Eeprom[i] = (static_cast<u16>(raw[(i * 2) + 0]) << 8) | static_cast<u16>(raw[(i * 2) + 1]);
  }

  if (std::FILE* fp = std::fopen("konami_gv_eeprom_debug.txt", "ab"))
  {
    std::fprintf(fp, "LoadEepromFile OK first_words:");
    for (int i = 0; i < 16; i++)
      std::fprintf(fp, " %04X", Eeprom[i]);
    std::fprintf(fp, "\n");
    std::fclose(fp);
  }

  return true;
}

static bool KonamiGVFujitsuFlashLoadFile(const char* path, void* buffer)
{
  std::FILE* fp = FileSystem::OpenCFile(path, "rb");
  if (!fp)
    return false;

  if (std::fread(buffer, 1, KONAMI_GV_FUJITSU_FLASH_SIZE, fp) != KONAMI_GV_FUJITSU_FLASH_SIZE)
  {
    std::fclose(fp);
    return false;
  }

  std::fclose(fp);
  return true;
}

static bool KonamiGVFujitsuFlashFileIsMissingOrErased(const std::string& path)
{
  std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "rb");
  if (!fp)
    return true;

  std::fseek(fp, 0, SEEK_END);
  const long size = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);

  if (size != static_cast<long>(KONAMI_GV_FUJITSU_FLASH_SIZE))
  {
    std::fclose(fp);
    return true;
  }

u8 buffer[4096];

  for (u32 offset = 0; offset < KONAMI_GV_FUJITSU_FLASH_SIZE; offset += sizeof(buffer))
  {
    if (std::fread(buffer, 1, sizeof(buffer), fp) != sizeof(buffer))
    {
      std::fclose(fp);
      return true;
    }

    for (u8 value : buffer)
    {
      if (value != 0xFF)
      {
        std::fclose(fp);
        return false;
      }
    }
  }

  std::fclose(fp);
  return true;
}

static bool KonamiGVFujitsuFlashReadDataSectorFromImage(CDImage* image, u32 lba, u8* sector)
{
  if (!image)
    return false;

  if (!image->Seek(1, static_cast<CDImage::LBA>(lba)))
    return false;

  return image->Read(CDImage::ReadMode::DataOnly, 1, sector) == 1;
}

static bool KonamiGVFujitsuFlashWriteFile(const std::string& path, const std::vector<u8>& data)
{
  std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "wb");
  if (!fp)
    return false;

  const bool ok = std::fwrite(data.data(), 1, data.size(), fp) == data.size();
  std::fclose(fp);
  return ok;
}

static bool KonamiGVFujitsuFlashExtractPairFromImage(CDImage* image, u32 start_lba, const std::string& low_path,
                                            const std::string& high_path)
{
  std::vector<u8> low(KONAMI_GV_FUJITSU_FLASH_SIZE);
  std::vector<u8> high(KONAMI_GV_FUJITSU_FLASH_SIZE);

  u8 sector[CDImage::DATA_SECTOR_SIZE];
  u32 output_offset = 0;

  for (u32 sector_index = 0; sector_index < KONAMI_GV_FUJITSU_FLASH_PAIR_SECTOR_COUNT; sector_index++)
  {
    const u32 lba = start_lba + sector_index;

    if (!KonamiGVFujitsuFlashReadDataSectorFromImage(image, lba, sector))
    {
      if (std::FILE* fp = std::fopen("konami_gv_flash_extract_debug.txt", "ab"))
      {
        std::fprintf(fp, "FAILED flash sector read start_lba=%u sector_index=%u lba=%u\n", start_lba, sector_index,
                     lba);
        std::fclose(fp);
      }

      return false;
    }

    for (u32 i = 0; i < CDImage::DATA_SECTOR_SIZE; i += 2)
    {
      low[output_offset] = sector[i];
      high[output_offset] = sector[i + 1];
      output_offset++;
    }
  }

  return KonamiGVFujitsuFlashWriteFile(low_path, low) && KonamiGVFujitsuFlashWriteFile(high_path, high);
}

static void KonamiCreateParentDirectoryForFile(const std::string& path)
{
  const size_t slash = path.find_last_of("/\\");

  if (slash == std::string::npos)
    return;

  const std::string directory = path.substr(0, slash);

  if (!directory.empty())
    FileSystem::CreateDirectory(directory.c_str(), false);
}

static void KonamiGVFujitsuFlashGenerateSimpsonsIfNeeded(const std::string& flash0_path, const std::string& flash1_path,
                                                const std::string& flash2_path, const std::string& flash3_path)
{
  const bool needs_flash =
    KonamiGVFujitsuFlashFileIsMissingOrErased(flash0_path) || KonamiGVFujitsuFlashFileIsMissingOrErased(flash1_path) ||
    KonamiGVFujitsuFlashFileIsMissingOrErased(flash2_path) || KonamiGVFujitsuFlashFileIsMissingOrErased(flash3_path);

  if (!needs_flash)
    return;

  KonamiCreateParentDirectoryForFile(flash0_path);
  KonamiCreateParentDirectoryForFile(flash1_path);
  KonamiCreateParentDirectoryForFile(flash2_path);
  KonamiCreateParentDirectoryForFile(flash3_path);

    auto image = CDImage::Open(System::GetRunningPath().c_str(), nullptr);
  if (!image)
  {
    if (std::FILE* fp = std::fopen("konami_gv_flash_extract_debug.txt", "ab"))
    {
      std::fprintf(fp, "FAILED opening running image for flash extraction: %s\n", System::GetRunningPath().c_str());
      std::fclose(fp);
    }

    return;
  }

  // Simpsons Bowling stores the four 29F016A flash chips as two interleaved pairs on disc.
  // Pair 0/1 starts at ISO offset 0x00065000 = LBA 202.
  // Pair 2/3 starts at ISO offset 0x00465000 = LBA 2250.
  const bool ok01 = KonamiGVFujitsuFlashExtractPairFromImage(image.get(), KONAMI_GV_FUJITSU_SIMPSONS_PAIR_01_LBA,
                                                             flash0_path, flash1_path);
  const bool ok23 = KonamiGVFujitsuFlashExtractPairFromImage(image.get(), KONAMI_GV_FUJITSU_SIMPSONS_PAIR_23_LBA,
                                                             flash2_path, flash3_path);

  if (std::FILE* fp = std::fopen("konami_gv_flash_extract_debug.txt", "ab"))
  {
    std::fprintf(fp, "Generated Simpsons flash from mounted disc: pair01=%u pair23=%u\n", ok01 ? 1 : 0, ok23 ? 1 : 0);
    std::fclose(fp);
  }
}

// Public API

bool KonamiUsesDirectGVFlash()
{
  const std::string& code = System::GetRunningCode();
  return code == "kdeadeye" || code == "btchamp";
}

void KonamiGVWatchdogWrite()
{
  KonamiGVWatchdogFramesRemaining = KONAMI_GV_WATCHDOG_TIMEOUT_FRAMES;
}

bool KonamiConsumeAutomaticResetRequest()
{
  if (TokimekiDeviceCheckEnabled)
  {
    if (TokimekiHeartbeatFramesRemaining > 0)
      TokimekiHeartbeatFramesRemaining--;

    if (TokimekiHeartbeatFramesRemaining == 0)
    {
      TokimekiHeartbeatSignal = false;
      TokimekiHeartbeatFramesRemaining = KonamiTokimekiGetHeartbeatPeriodFrames();
    }
  }

  if (GVBtChampFirstBootState == KonamiGVBtChampFirstBootState::ResetRequested)
  {
    GVBtChampFirstBootState = KonamiGVBtChampFirstBootState::HoldingTest;
    KonamiGVWatchdogFramesRemaining = 0;

    Log_InfoPrintf("KonamiGV: starting automatic Beat the Champ first-run flash recovery");

    return true;
  }

  if (KonamiGVWatchdogFramesRemaining == 0)
    return false;

  KonamiGVWatchdogFramesRemaining--;

  if (KonamiGVWatchdogFramesRemaining != 0)
    return false;

  Log_InfoPrintf("KonamiGV: watchdog expired; resetting system");

  return true;
}

void KonamiInit(void)
{
  KonamiGVWatchdogFramesRemaining = 0;

  if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
  {
    std::fprintf(fp, "KONAMI INIT CALLED\n");
    std::fclose(fp);
  }

  KonamiGVScsiInitialize();

  // ArcadeDuck uses the active MAME set name for per-game NVRAM.
  // Example:
  //   simpbowl.zip -> nvram/simpbowl
  //   kdeadeye.zip -> nvram/kdeadeye
  //   btchamp.zip  -> nvram/btchamp
  const std::string& game_name = System::GetRunningCode();

  TokimekiDeviceCheckEnabled = false;
  TokimekiDeviceCheckValue = 0;
  TokimekiDeviceCheckClock = false;
  TokimekiExcitementLevel = 0;
  KonamiTokimekiUpdateExcitementValues();

  TokimekiHeartbeatSignal = true;
  TokimekiHeartbeatFramesRemaining = KonamiTokimekiGetHeartbeatPeriodFrames();
  TokimekiSerialValue = 0;
  TokimekiSerialLength = 0;
  TokimekiSerialClock = false;
  TokimekiSerialSensorId = 0;
  TokimekiSerialSensorData = 0;

  if (game_name == "tmosh")
  {
    TokimekiDeviceCheckEnabled = true;
  }
  else if (game_name == "tmoshs")
  {
    TokimekiDeviceCheckEnabled = true;
    TokimekiDeviceCheckValue = 0xF073;
  }
  else if (game_name == "tmoshsp" || game_name == "tmoshspa")
  {
    TokimekiDeviceCheckEnabled = true;
    TokimekiDeviceCheckValue = 0xF0BA;
  }

  if (game_name.empty())
  {
    Log_ErrorPrintf("KonamiGV: missing MAME set name for NVRAM setup");
    return;
  }

  const std::string nvram_dir = std::string("nvram") + FS_OSPATH_SEPARATOR_STR + game_name;
  const std::string eeprom_path = nvram_dir + FS_OSPATH_SEPARATOR_STR "eeprom";

  FileSystem::CreateDirectory("nvram", false);
  FileSystem::CreateDirectory(nvram_dir.c_str(), false);

  const std::string flash0_path = nvram_dir + FS_OSPATH_SEPARATOR_STR "flash0";
  const std::string flash1_path = nvram_dir + FS_OSPATH_SEPARATOR_STR "flash1";
  const std::string flash2_path = nvram_dir + FS_OSPATH_SEPARATOR_STR "flash2";
  const std::string flash3_path = nvram_dir + FS_OSPATH_SEPARATOR_STR "flash3";
  const std::string gv_sharp_flash_path = nvram_dir + FS_OSPATH_SEPARATOR_STR "flash";

  // Empty flash chips should read as erased flash, not zero-filled RAM.
  for (u32 chip = 0; chip < KONAMI_GV_FUJITSU_FLASH_CHIP_COUNT; chip++)
  {
    for (u32 i = 0; i < KONAMI_GV_FUJITSU_FLASH_SIZE; i++)
      GVFujitsuFlash[chip][i] = 0xFF;
  }

  EepromDi = false;
  EepromDo = true;
  EepromCs = false;
  EepromClk = false;
  EepromShiftIn = 0;
  EepromShiftCount = 0;
  EepromReadShift = 0;
  EepromReadBits = 0;
  EepromWriteEnabled = false;
  EepromWriteAll = false;
  EepromWriteAddress = 0;
  EepromWriteShift = 0;
  EepromWriteBits = 0;

  for (u32 i = 0; i < KONAMI_GV_SHARP_FLASH_SIZE; i++)
    GVSharpFlash[i] = 0xFF;

  GVSharpFlashPresent = KonamiUsesDirectGVFlash();
  GVSharpFlashDirty = false;
  GVSharpFlashPath.clear();
  GVSharpFlashMode = KONAMI_GV_SHARP_FLASH_MODE_READ_ARRAY;
  GVSharpFlashStatus = 0x0080;
  GVBtChampFirstBootState = KonamiGVBtChampFirstBootState::Inactive;
  GVFujitsuFlashAddress = 0;

  for (u32 chip = 0; chip < KONAMI_GV_FUJITSU_FLASH_CHIP_COUNT; chip++)
  {
    GVFujitsuFlashModeState[chip] = KONAMI_GV_FUJITSU_FLASH_MODE_READ_ARRAY;
    GVFujitsuFlashDirty[chip] = false;
  }

  if (!eeprom_path.empty())
    LoadEepromFile(eeprom_path.c_str());

  if (KonamiUsesDirectGVFlash())
  {
    KonamiGVSharpFlashLoadFile(gv_sharp_flash_path);
    GVSharpFlashPath = gv_sharp_flash_path;

    if (game_name == "btchamp" && KonamiGVSharpFlashIsErased())
    {
      GVBtChampFirstBootState = KonamiGVBtChampFirstBootState::WaitingForValidationEnd;

      Log_InfoPrintf("KonamiGV: armed automatic Beat the Champ first-run flash recovery");
    }
  }

  else if (game_name == "simpbowl")
  {
    KonamiGVFujitsuFlashGenerateSimpsonsIfNeeded(flash0_path, flash1_path, flash2_path, flash3_path);

    KonamiGVFujitsuFlashLoadFile(flash0_path.c_str(), GVFujitsuFlash[0]);
    KonamiGVFujitsuFlashLoadFile(flash1_path.c_str(), GVFujitsuFlash[1]);
    KonamiGVFujitsuFlashLoadFile(flash2_path.c_str(), GVFujitsuFlash[2]);
    KonamiGVFujitsuFlashLoadFile(flash3_path.c_str(), GVFujitsuFlash[3]);
  }

  KonamiTrackballReset();
  TrackballSensitivity = g_host_interface->GetFloatSettingValue("KonamiGV", "TrackballSensitivity", 1.0f);

  CurrentButtons[0] = 0xFFFFFFFF;
  CurrentButtons[1] = 0xFFFFFFFF;

  {
    std::lock_guard<std::mutex> lock(LightgunMutex);

    LightgunNormalizedX[0] = 0.5f;
    LightgunNormalizedY[0] = 0.5f;
    LightgunTrigger[0] = false;
    LightgunOffscreen[0] = false;
    LightgunShootOffscreen[0] = false;

    LightgunNormalizedX[1] = 0.5f;
    LightgunNormalizedY[1] = 0.5f;
    LightgunTrigger[1] = false;
    LightgunOffscreen[1] = false;
    LightgunShootOffscreen[1] = false;
  }
}

void KonamiTerm(void)
{
  KonamiGVScsiShutdown();

  KonamiGVSharpFlashSaveFile();

  if (EepromFp)
  {
    KonamiSaveEepromFile();
    std::fclose(EepromFp);
    EepromFp = nullptr;
  }
}

// Player 1 controls

void KonamiP1Read(u32 Size, u32 Offset, u32& Value)
{
  Value = CurrentButtons[0];

  if (GVBtChampFirstBootState == KonamiGVBtChampFirstBootState::HoldingTest)
    Value &= ~(1U << 12);

  if (EepromDo)
    Value |= (1 << 13);
  else
    Value &= ~(1 << 13);
}

void KonamiP1Write(u32 Size, u32 Offset, u32 Value)
{
  // Ignored
}

// Player 2 controls

void KonamiP2Read(u32 Size, u32 Offset, u32& Value)
{
  Value = CurrentButtons[1];

  if (TokimekiDeviceCheckEnabled)
  {
    Value &= ~0x00000400U;
    Value |= static_cast<u32>((TokimekiDeviceCheckValue >> 15) & 1U) << 10;
  }
}

void KonamiP2Write(u32 Size, u32 Offset, u32 Value)
{
  // Ignored
}

void KonamiTokimekiAdjustExcitement(s32 Direction)
{
  if (!TokimekiDeviceCheckEnabled || Direction == 0)
    return;

  const u32 previous_level = TokimekiExcitementLevel;

  if (Direction > 0)
  {
    if (TokimekiExcitementLevel < TOKIMEKI_MAX_EXCITEMENT_LEVEL)
      TokimekiExcitementLevel++;
  }
  else
  {
    if (TokimekiExcitementLevel > 0)
      TokimekiExcitementLevel--;
  }

  if (TokimekiExcitementLevel == previous_level)
    return;

  KonamiTokimekiUpdateExcitementValues();

  TokimekiHeartbeatFramesRemaining = KonamiTokimekiGetHeartbeatPeriodFrames();
}

void KonamiTokimekiSerialRead(u32 Size, u32 Offset, u32& Value)
{
  Value = TokimekiHeartbeatSignal ? 0x0004U : 0x0000U;
  TokimekiHeartbeatSignal = true;

  if (TokimekiSerialSensorId != 0)
  {
    Value |= static_cast<u32>((TokimekiSerialSensorData >> 8) & 1U) << 3;
  }
}

void KonamiTokimekiSerialWrite(u32 Size, u32 Offset, u32 Value)
{
  const bool new_serial_clock = (Value & 0x20U) != 0;

  if ((Value & 0x02U) != 0)
  {
    TokimekiSerialSensorData = 0;
    TokimekiSerialSensorId = 0;
    TokimekiSerialValue = 0;
    TokimekiSerialLength = 0;
  }
  else if (!TokimekiSerialClock && new_serial_clock)
  {
    if (TokimekiSerialLength < 5)
    {
      const u8 serial_bit = static_cast<u8>((Value >> 4) & 1U);

      TokimekiSerialValue |= static_cast<u8>(serial_bit << (4 - TokimekiSerialLength));

      if (TokimekiSerialLength == 4)
      {
        TokimekiSerialSensorId = TokimekiSerialValue;

        switch (TokimekiSerialSensorId)
        {
          case 0x1A:
            TokimekiSerialSensorData = TokimekiGSRValue;
            break;

          case 0x18:
          case 0x19:
          default:
            TokimekiSerialSensorData = 0;
            break;
        }
      }
    }
    else if (TokimekiSerialLength >= 6)
    {
      TokimekiSerialSensorData <<= 1;
    }

    TokimekiSerialLength++;
  }

  TokimekiSerialClock = new_serial_clock;
}

// Konami GV Fujitsu flash

static bool KonamiGVFujitsuFlashIsUnlockAAAddress(u32 Address)
{
  return ((Address & 0x0FFF) == 0x0555) || ((Address & 0x0FFF) == 0x0AAA) || ((Address & 0xFFFF) == 0x5555);
}

static bool KonamiGVFujitsuFlashIsUnlock55Address(u32 Address)
{
  return ((Address & 0xFFFF) == 0x02AA) || ((Address & 0xFFFF) == 0x2AAA) || ((Address & 0x0FFF) == 0x0555);
}

static bool KonamiGVFujitsuFlashIsCommandAddress(u32 Address)
{
  return ((Address & 0xFFFF) == 0x0555) || ((Address & 0xFFFF) == 0x5555) || ((Address & 0x0FFF) == 0x0AAA);
}

static u8 KonamiGVFujitsuFlashChipRead(u8 Chip, u32 Address)
{
  Address &= (KONAMI_GV_FUJITSU_FLASH_SIZE - 1);

  if (GVFujitsuFlashModeState[Chip] == KONAMI_GV_FUJITSU_FLASH_MODE_AUTOSELECT)
  {
    switch (Address & 0xFF)
    {
      case 0:
        return 0x04; // Fujitsu manufacturer ID

      case 1:
        return 0xAD; // Fujitsu MBM29F016A device ID

      case 2:
        return 0x00;

      default:
        return 0xFF;
    }
  }

  return GVFujitsuFlash[Chip][Address];
}

static void KonamiGVFujitsuFlashEraseSector(u8 Chip, u32 Address)
{
  const u32 SectorBase = Address & ~(KONAMI_GV_FUJITSU_FLASH_SECTOR_SIZE - 1);

  for (u32 i = 0; i < KONAMI_GV_FUJITSU_FLASH_SECTOR_SIZE; i++)
    GVFujitsuFlash[Chip][(SectorBase + i) & (KONAMI_GV_FUJITSU_FLASH_SIZE - 1)] = 0xFF;

  GVFujitsuFlashDirty[Chip] = true;
}

static void KonamiGVFujitsuFlashEraseChip(u8 Chip)
{
  for (u32 i = 0; i < KONAMI_GV_FUJITSU_FLASH_SIZE; i++)
    GVFujitsuFlash[Chip][i] = 0xFF;

  GVFujitsuFlashDirty[Chip] = true;
}

static void KonamiGVFujitsuFlashChipWrite(u8 Chip, u32 Address, u8 Data)
{
  Address &= (KONAMI_GV_FUJITSU_FLASH_SIZE - 1);

  // Reset/read-array command.
  if (Data == 0xF0 || Data == 0xFF)
  {
    GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_READ_ARRAY;
    return;
  }

  switch (GVFujitsuFlashModeState[Chip])
  {
    case KONAMI_GV_FUJITSU_FLASH_MODE_READ_ARRAY:
    case KONAMI_GV_FUJITSU_FLASH_MODE_AUTOSELECT:
      if (Data == 0xAA && KonamiGVFujitsuFlashIsUnlockAAAddress(Address))
        GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_UNLOCK_1;
      break;

    case KONAMI_GV_FUJITSU_FLASH_MODE_UNLOCK_1:
      if (Data == 0x55 && KonamiGVFujitsuFlashIsUnlock55Address(Address))
        GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_UNLOCK_2;
      else
        GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_READ_ARRAY;
      break;

    case KONAMI_GV_FUJITSU_FLASH_MODE_UNLOCK_2:
      if (!KonamiGVFujitsuFlashIsCommandAddress(Address))
      {
        GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_READ_ARRAY;
        break;
      }

      switch (Data)
      {
        case 0x90:
          GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_AUTOSELECT;
          break;

        case 0xA0:
          GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_PROGRAM;
          break;

        case 0x80:
          GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_ERASE_UNLOCK_1;
          break;

        default:
          GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_READ_ARRAY;
          break;
      }
      break;

    case KONAMI_GV_FUJITSU_FLASH_MODE_PROGRAM:
      // Real flash can clear bits from 1 to 0, not set 0 back to 1 without erase.
      GVFujitsuFlash[Chip][Address] &= Data;
      GVFujitsuFlashDirty[Chip] = true;
      GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_READ_ARRAY;
      break;

    case KONAMI_GV_FUJITSU_FLASH_MODE_ERASE_UNLOCK_1:
      if (Data == 0xAA && KonamiGVFujitsuFlashIsUnlockAAAddress(Address))
        GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_ERASE_UNLOCK_2;
      else
        GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_READ_ARRAY;
      break;

    case KONAMI_GV_FUJITSU_FLASH_MODE_ERASE_UNLOCK_2:
      if (Data == 0x55 && KonamiGVFujitsuFlashIsUnlock55Address(Address))
        GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_ERASE_SELECT;
      else
        GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_READ_ARRAY;
      break;

    case KONAMI_GV_FUJITSU_FLASH_MODE_ERASE_SELECT:
      if (Data == 0x10 && KonamiGVFujitsuFlashIsCommandAddress(Address))
        KonamiGVFujitsuFlashEraseChip(Chip);
      else if (Data == 0x30)
        KonamiGVFujitsuFlashEraseSector(Chip, Address);

      GVFujitsuFlashModeState[Chip] = KONAMI_GV_FUJITSU_FLASH_MODE_READ_ARRAY;
      break;
  }
}

void KonamiGVFujitsuFlashRead(u32 Size, u32 Offset, u32& Value)
{
  static int gv_fujitsu_flash_read_debug_count = 0;

  const u32 raw_offset = Offset;
  Offset &= 0xF;

  switch (Offset)
  {
    case 0:
    {
      const u8 chip =
        (GVFujitsuFlashAddress >= KONAMI_GV_FUJITSU_FLASH_SIZE) ? KONAMI_GV_FUJITSU_FLASH_CHIPS_PER_PAIR : 0;
      const u32 address = GVFujitsuFlashAddress & (KONAMI_GV_FUJITSU_FLASH_SIZE - 1);

      const u8 low = KonamiGVFujitsuFlashChipRead(chip, address);
      const u8 high = KonamiGVFujitsuFlashChipRead(chip + 1, address);

      Value = static_cast<u32>(low) | (static_cast<u32>(high) << 8);

      if (gv_fujitsu_flash_read_debug_count < 500)
      {
        if (std::FILE* fp = std::fopen("konami_gv_flash_debug.txt", "ab"))
        {
          std::fprintf(fp, "FLASH READ raw_offset=0x%08X offset=0x%X address=0x%08X chip=%u value=0x%04X mode=%u/%u\n",
                       raw_offset, Offset, GVFujitsuFlashAddress, chip, Value & 0xFFFF, static_cast<u32>(GVFujitsuFlashModeState[chip]),
                       static_cast<u32>(GVFujitsuFlashModeState[chip + 1]));
          std::fclose(fp);
        }
        gv_fujitsu_flash_read_debug_count++;
      }

      GVFujitsuFlashAddress++;
      break;
    }

    case 8:
      GVFujitsuFlashAddress |= 1;
      Value = 0;
      break;

    default:
      Value = 0;
      break;
  }
}

void KonamiGVFujitsuFlashWrite(u32 Size, u32 Offset, u32 Value)
{
  static int gv_fujitsu_flash_write_debug_count = 0;

  const u32 raw_offset = Offset;
  Offset &= 0xF;

  if (gv_fujitsu_flash_write_debug_count < 2000)
  {
    if (std::FILE* fp = std::fopen("konami_gv_flash_debug.txt", "ab"))
    {
      std::fprintf(fp, "FLASH WRITE raw_offset=0x%08X offset=0x%X value=0x%04X old_address=0x%08X\n", raw_offset,
                   Offset, Value & 0xFFFF, GVFujitsuFlashAddress);
      std::fclose(fp);
    }
    gv_fujitsu_flash_write_debug_count++;
  }

  switch (Offset)
  {
    case 0:
    {
      const u8 chip =
        (GVFujitsuFlashAddress >= KONAMI_GV_FUJITSU_FLASH_SIZE) ? KONAMI_GV_FUJITSU_FLASH_CHIPS_PER_PAIR : 0;
      const u32 address = GVFujitsuFlashAddress & (KONAMI_GV_FUJITSU_FLASH_SIZE - 1);

      KonamiGVFujitsuFlashChipWrite(chip, address, static_cast<u8>(Value & 0xFF));
      KonamiGVFujitsuFlashChipWrite(chip + 1, address, static_cast<u8>((Value >> 8) & 0xFF));
      break;
    }

    case 2:
      GVFujitsuFlashAddress = 0;
      GVFujitsuFlashAddress |= Value << 1;
      break;

    case 4:
      GVFujitsuFlashAddress &= 0xFF00FF;
      GVFujitsuFlashAddress |= Value << 8;
      break;

    case 6:
      GVFujitsuFlashAddress &= 0x00FFFF;
      GVFujitsuFlashAddress |= Value << 15;
      break;
  }
}

// EEPROM

static u16 KonamiEepromSwap16(u16 value)
{
  return static_cast<u16>((value >> 8) | (value << 8));
}

static void KonamiSerialEepromWrite(u32 Value)
{
  static int serial_debug_count = 0;

  const bool new_di = (Value & 0x01) != 0;
  const bool new_cs = (Value & 0x02) != 0;
  const bool new_clk = (Value & 0x04) != 0;

  const bool new_device_check_clock = (Value & 0x10) != 0;

  if (TokimekiDeviceCheckEnabled && !TokimekiDeviceCheckClock && new_device_check_clock)
  {
    TokimekiDeviceCheckValue = static_cast<u16>((TokimekiDeviceCheckValue << 1) | (TokimekiDeviceCheckValue >> 15));
  }

  TokimekiDeviceCheckClock = new_device_check_clock;

  static u32 eeprom_edge_debug_count = 0;

  const bool cs_changed = (EepromCs != new_cs);
  const bool rising_edge = (!EepromClk && new_clk);

  if (eeprom_edge_debug_count < 5000 && (cs_changed || (new_cs && rising_edge)))
  {
    if (std::FILE* fp = std::fopen("konami_gv_eeprom_edges_debug.txt", "ab"))
    {
      std::fprintf(fp,
                   "pc=0x%08X value=0x%08X "
                   "old_cs=%u new_cs=%u old_clk=%u new_clk=%u di=%u do=%u "
                   "shift=0x%08X shift_count=%u read_bits=%d "
                   "write_bits=%d write_enabled=%u\n",
                   CPU::g_state.current_instruction_pc, Value, EepromCs ? 1 : 0, new_cs ? 1 : 0, EepromClk ? 1 : 0,
                   new_clk ? 1 : 0, new_di ? 1 : 0, EepromDo ? 1 : 0, EepromShiftIn, EepromShiftCount, EepromReadBits,
                   EepromWriteBits, EepromWriteEnabled ? 1 : 0);

      std::fclose(fp);
    }

    eeprom_edge_debug_count++;
  }

  if (!new_cs)
  {
    EepromDi = new_di;
    EepromCs = false;
    EepromClk = new_clk;
    EepromShiftIn = 0;
    EepromShiftCount = 0;
    EepromReadShift = 0;
    EepromReadBits = 0;
    EepromWriteAll = false;
    EepromWriteAddress = 0;
    EepromWriteShift = 0;
    EepromWriteBits = 0;
    EepromDo = true;
    return;
  }

  if (!EepromCs)
  {
    EepromShiftIn = 0;
    EepromShiftCount = 0;
    EepromReadShift = 0;
    EepromReadBits = 0;
    EepromWriteAll = false;
    EepromWriteAddress = 0;
    EepromWriteShift = 0;
    EepromWriteBits = 0;
    EepromDo = true;
  }

  // Rising edge clocks data.
  if (!EepromClk && new_clk)
  {
    if (EepromReadBits > 0)
    {
      EepromDo = ((EepromReadShift >> 15) & 1) != 0;
      EepromReadShift <<= 1;
      EepromReadBits--;
    }
    else if (EepromWriteBits > 0)
    {
      EepromWriteShift = (EepromWriteShift << 1) | (new_di ? 1 : 0);
      EepromWriteBits--;

      if (EepromWriteBits == 0)
      {
        const u16 wire_value = static_cast<u16>(EepromWriteShift & 0xFFFF);
        const u16 raw_value = KonamiEepromSwap16(wire_value);

        if (EepromWriteEnabled)
        {
          if (EepromWriteAll)
          {
            for (u32 i = 0; i < 64; i++)
              Eeprom[i] = raw_value;
          }
          else
          {
            Eeprom[EepromWriteAddress & 0x3F] = raw_value;
          }

          KonamiSaveEepromFile();
        }

        if (serial_debug_count < 300)
        {
          if (std::FILE* fp = std::fopen("konami_gv_eeprom_serial_debug.txt", "ab"))
          {
            std::fprintf(fp, "EEPROM WRITE address=%u write_all=%u enabled=%u value=0x%04X\n",
                         EepromWriteAddress & 0x3F, EepromWriteAll ? 1 : 0, EepromWriteEnabled ? 1 : 0, wire_value);
            std::fclose(fp);
          }
          serial_debug_count++;
        }

        EepromShiftIn = 0;
        EepromShiftCount = 0;
        EepromWriteAll = false;
        EepromWriteAddress = 0;
        EepromWriteShift = 0;
        EepromDo = true;
      }
    }
    else
    {
      // 93C46 ignores leading zeroes until the start bit.
      if (EepromShiftCount == 0 && !new_di)
      {
        // Still waiting for start bit.
      }
      else
      {
        EepromShiftIn = (EepromShiftIn << 1) | (new_di ? 1 : 0);
        EepromShiftCount++;

        // 93C46 16-bit command:
        // start bit 1, opcode 2 bits, 6-bit address = 9 bits total.
        if (EepromShiftCount == 9)
        {
          const u32 start = (EepromShiftIn >> 8) & 1;
          const u32 opcode = (EepromShiftIn >> 6) & 3;
          const u32 address = EepromShiftIn & 0x3F;
          const u16 eeprom_value = KonamiEepromSwap16(Eeprom[address & 0x3F]);

          if (serial_debug_count < 300)
          {
            if (std::FILE* fp = std::fopen("konami_gv_eeprom_serial_debug.txt", "ab"))
            {
              std::fprintf(fp, "EEPROM CMD shift=0x%03X start=%u opcode=%u address=%u value=0x%04X enabled=%u\n",
                           EepromShiftIn, start, opcode, address, eeprom_value, EepromWriteEnabled ? 1 : 0);
              std::fclose(fp);
            }
            serial_debug_count++;
          }

          if (!start)
          {
            EepromShiftIn = 0;
            EepromShiftCount = 0;
          }
          else if (opcode == 2)
          {
            // READ.
            // A 93C46 outputs a dummy 0 bit immediately after the command,
            // then shifts the 16-bit EEPROM word out MSB-first.
            EepromReadShift = eeprom_value;
            EepromReadBits = 16;
            EepromDo = false;
          }
          else if (opcode == 1)
          {
            // WRITE word; next 16 clocked bits are data.
            EepromWriteAll = false;
            EepromWriteAddress = address;
            EepromWriteShift = 0;
            EepromWriteBits = 16;
            EepromDo = false;
          }
          else if (opcode == 3)
          {
            // ERASE word.
            if (EepromWriteEnabled)
            {
              Eeprom[address & 0x3F] = 0xFFFF;
              KonamiSaveEepromFile();
            }

            EepromDo = true;
            EepromShiftIn = 0;
            EepromShiftCount = 0;
          }
          else
          {
            // Special commands use opcode 00 and address high bits:
            // 00xxxx = EWDS, 01xxxx = WRAL, 10xxxx = ERAL, 11xxxx = EWEN.
            const u32 special = (address >> 4) & 3;

            switch (special)
            {
              case 0:
                // EWDS
                EepromWriteEnabled = false;
                EepromDo = true;
                EepromShiftIn = 0;
                EepromShiftCount = 0;
                break;

              case 1:
                // WRAL; next 16 clocked bits are written to all words.
                EepromWriteAll = true;
                EepromWriteAddress = 0;
                EepromWriteShift = 0;
                EepromWriteBits = 16;
                EepromDo = false;
                break;

              case 2:
                // ERAL
                if (EepromWriteEnabled)
                {
                  for (u32 i = 0; i < 64; i++)
                    Eeprom[i] = 0xFFFF;

                  KonamiSaveEepromFile();
                }

                EepromDo = true;
                EepromShiftIn = 0;
                EepromShiftCount = 0;
                break;

              case 3:
                // EWEN
                EepromWriteEnabled = true;
                EepromDo = true;
                EepromShiftIn = 0;
                EepromShiftCount = 0;
                break;
            }
          }
        }
      }
    }
  }

  EepromDi = new_di;
  EepromCs = new_cs;
  EepromClk = new_clk;
}
void KonamiEepromRead(u32 Size, u32 Offset, u32& Value)
{
  if (Offset >= 0x00180080 && Offset < 0x00180100)
  {
    const u32 relative_offset = (Offset - 0x00180080) & 0x7F;
    const u32 word_index = relative_offset >> 1;

    Value = Eeprom[word_index];

    static u32 direct_eeprom_read_log_count = 0;
    if (direct_eeprom_read_log_count < 2000)
    {
      if (std::FILE* fp = std::fopen("konami_gv_eeprom_direct_debug.txt", "ab"))
      {
        std::fprintf(fp,
                     "DIRECT EEPROM READ "
                     "game=%s pc=0x%08X size=%u offset=0x%08X "
                     "relative=0x%02X index=%u value=0x%08X word=0x%04X\n",
                     System::GetRunningCode().c_str(), CPU::g_state.current_instruction_pc, Size, Offset,
                     relative_offset, word_index, Value, Eeprom[word_index]);

        std::fclose(fp);
      }

      direct_eeprom_read_log_count++;
    }
  }
  else
  {
    Log_WarningPrintf("%s: %08X", __FUNCTION__, Offset);
    Value = 0;
  }
}

static void KonamiSaveEepromFile()
{
  if (!EepromFp)
    return;

  u8 raw[sizeof(Eeprom)];

  for (u32 i = 0; i < 64; i++)
  {
    raw[(i * 2) + 0] = static_cast<u8>(Eeprom[i] >> 8);
    raw[(i * 2) + 1] = static_cast<u8>(Eeprom[i] & 0xFF);
  }

  std::fseek(EepromFp, 0, SEEK_SET);
  std::fwrite(raw, 1, sizeof(raw), EepromFp);
  std::fflush(EepromFp);
}

void KonamiEepromWrite(u32 Size, u32 Offset, u32 Value)
{
  if (Offset >= 0x00180000 && Offset < 0x00180004)
  {
    KonamiSerialEepromWrite(Value);
  }
  else if (Offset >= 0x00180080 && Offset < 0x00180100)
  {
    const u32 relative_offset = (Offset - 0x00180080) & 0x7F;
    const u32 word_index = relative_offset >> 1;
    const u16 old_value = Eeprom[word_index];
    const u16 new_value = static_cast<u16>(Value);

    if (std::FILE* fp = std::fopen("konami_gv_eeprom_direct_debug.txt", "ab"))
    {
      std::fprintf(fp,
                   "DIRECT EEPROM WRITE "
                   "game=%s pc=0x%08X size=%u offset=0x%08X "
                   "relative=0x%02X index=%u raw_value=0x%08X "
                   "old_word=0x%04X new_word=0x%04X\n",
                   System::GetRunningCode().c_str(), CPU::g_state.current_instruction_pc, Size, Offset, relative_offset,
                   word_index, Value, old_value, new_value);

      std::fclose(fp);
    }

    Eeprom[word_index] = new_value;
    KonamiSaveEepromFile();
  }
  else
  {
    Log_WarningPrintf("%s: %08X %08X", __FUNCTION__, Offset, Value);
  }
}

static s32 KonamiConsumeTrackballAxis(double& pending)
{
  const s32 whole = std::clamp(static_cast<s32>(std::trunc(pending)), -2048, 2047);
  pending -= static_cast<double>(whole);
  return whole;
}

static u16 KonamiEncodeTrackball12(s32 value)
{
  return static_cast<u16>(value) & 0x0FFF;
}

static void KonamiLatchTrackball(u32 Player)
{
  if (Player >= 2)
    return;

  std::lock_guard<std::mutex> lock(TrackballMutex);

  const s32 x = KonamiConsumeTrackballAxis(TrackballPendingX[Player]);
  const s32 y = KonamiConsumeTrackballAxis(TrackballPendingY[Player]);

  TrackballX[Player] = KonamiEncodeTrackball12(x);
  TrackballY[Player] = KonamiEncodeTrackball12(y);
}

// Trackball
void KonamiTrackballRead(u32 Size, u32 Offset, u32& Value)
{
  static int btchamp_trackball_debug_count = 0;

  if (System::GetRunningCode() == "btchamp" && btchamp_trackball_debug_count < 500)
  {
    if (std::FILE* fp = std::fopen("btchamp_trackball_debug.txt", "ab"))
    {
      std::fprintf(fp, "TRACKBALL READ offset=0x%08X\n", Offset);
      std::fclose(fp);
    }

    btchamp_trackball_debug_count++;
  }
  if (System::GetRunningCode() == "btchamp" && Offset == 0x00680086)
  {
    KonamiLatchTrackball(0);
    KonamiLatchTrackball(1);
  }
  else if (Offset == 0x006800C0)
  {
    KonamiLatchTrackball(0);
  }

  switch (Offset)
  {
    case 0x00680080:
      // P1 X low byte in high byte, P2 X low byte in low byte.
      Value = ((TrackballX[0] & 0x00FF) << 8) | (TrackballX[1] & 0x00FF);
      break;

    case 0x00680082:
      // P1 X high nibble in high byte, P2 X high nibble in low byte.
      Value = (((TrackballX[0] >> 8) & 0x000F) << 8) | ((TrackballX[1] >> 8) & 0x000F);
      break;

    case 0x00680084:
      // P1 Y low byte in high byte, P2 Y low byte in low byte.
      Value = ((TrackballY[0] & 0x00FF) << 8) | (TrackballY[1] & 0x00FF);
      break;

    case 0x00680086:
      // P1 Y high nibble in high byte, P2 Y high nibble in low byte.
      Value = (((TrackballY[0] >> 8) & 0x000F) << 8) | ((TrackballY[1] >> 8) & 0x000F);
      break;

    case 0x006800C0:
      Value = (TrackballX[0] & 0x0FF) << 8;
      break;

    case 0x006800C2:
      Value = TrackballX[0] & 0xF00;
      break;

    case 0x006800C4:
      Value = (TrackballY[0] & 0x0FF) << 8;
      break;

    case 0x006800C6:
      Value = TrackballY[0] & 0xF00;
      break;

    default:
      Value = 0;
      break;
  }

  if (System::GetRunningCode() == "btchamp")
  {
    static int value_debug_count = 0;

    if (value_debug_count < 300 &&
        (TrackballX[0] != 0 || TrackballY[0] != 0 || TrackballX[1] != 0 || TrackballY[1] != 0))
    {
      if (std::FILE* fp = std::fopen("btchamp_trackball_values.txt", "ab"))
      {
        std::fprintf(fp,
                     "offset=0x%08X value=0x%04X "
                     "p1_x=0x%04X p1_y=0x%04X "
                     "p2_x=0x%04X p2_y=0x%04X\n",
                     Offset, Value & 0xFFFF, TrackballX[0], TrackballY[0], TrackballX[1], TrackballY[1]);

        std::fclose(fp);
      }

      value_debug_count++;
    }
  }
}

void KonamiTrackballWrite(u32 Size, u32 Offset, u32 Value)
{
  if (System::GetRunningCode() == "btchamp" && (Offset == 0x00680088 || Offset == 0x0068008A))
  {
    static int btchamp_trackball_write_debug_count = 0;

    if (btchamp_trackball_write_debug_count < 200)
    {
      if (std::FILE* fp = std::fopen("btchamp_trackball_writes.txt", "ab"))
      {
        std::fprintf(fp,
                     "offset=0x%08X value=0x%08X bit0_reset=%u bit1_cs=%u counter_x=0x%03X counter_y=0x%03X "
                     "start_x=0x%03X start_y=0x%03X out_x=0x%03X out_y=0x%03X\n",
                     Offset, Value, Value & 1, (Value >> 1) & 1, TrackballCounterX & 0x0FFF, TrackballCounterY & 0x0FFF,
                     TrackballStartX & 0x0FFF, TrackballStartY & 0x0FFF, TrackballX[0] & 0x0FFF,
                     TrackballY[0] & 0x0FFF);
        std::fclose(fp);
      }

      btchamp_trackball_write_debug_count++;
    }

    const bool reset_active = ((Value & 0x0001) == 0);

    if (reset_active && !TrackballResetActive)
    {
      std::lock_guard<std::mutex> lock(TrackballMutex);

      TrackballStartX = TrackballCounterX;
      TrackballStartY = TrackballCounterY;

      TrackballX[0] = 0;
      TrackballY[0] = 0;
      TrackballX[1] = 0;
      TrackballY[1] = 0;
    }

    TrackballResetActive = reset_active;

    return;
  }
}

void KonamiButtonsSet(u32 Player, u32 Buttons)
{
  if (Player >= 2)
    return;

  u32 value = 0xFFFFFFFF;

  // Convert PS1 controller active-low bits to Konami GV active-low JAMMA bits.
  const auto map_button = [&value, Buttons](u32 psx_mask, u32 gv_mask) {
    if ((Buttons & psx_mask) == 0)
      value &= ~gv_mask;
  };

  map_button(0x0080, 1 << 0); // Left
  map_button(0x0020, 1 << 1); // Right
  map_button(0x0010, 1 << 2); // Up
  map_button(0x0040, 1 << 3); // Down

  map_button(0x4000, 1 << 4); // Button 1 / Cross
  map_button(0x2000, 1 << 5); // Button 2 / Circle
  map_button(0x0200, 1 << 6); // Button 3 / R2
  map_button(0x8000, 1 << 7); // Button 4 / Square

  map_button(0x0008, 1 << 9);  // Start
  map_button(0x0001, 1 << 10); // Coin / Select

  CurrentButtons[Player] = value;
}

void KonamiArcadeButtonSet(u32 Player, u32 ButtonMask, bool Pressed)
{
  if (Player >= 2)
    return;

  if (Pressed)
    CurrentButtons[Player] &= ~ButtonMask;
  else
    CurrentButtons[Player] |= ButtonMask;
}

void KonamiTrackballSetXY(u16 X, u16 Y)
{
  std::lock_guard<std::mutex> lock(TrackballMutex);

  TrackballX[0] = X & 0x0FFF;
  TrackballY[0] = Y & 0x0FFF;
}

void KonamiTrackballAddDelta(u32 Player, s32 X, s32 Y)
{
  if (Player >= 2)
    return;

  std::lock_guard<std::mutex> lock(TrackballMutex);

  const s32 adjusted_x = (System::GetRunningCode() == "btchamp") ? X : -X;

  TrackballPendingX[Player] += static_cast<double>(adjusted_x) * static_cast<double>(TrackballSensitivity);

  TrackballPendingY[Player] += static_cast<double>(Y) * static_cast<double>(TrackballSensitivity);
}

void KonamiTrackballAddDelta(s32 X, s32 Y)
{
  KonamiTrackballAddDelta(0, X, Y);
}

void KonamiTrackballReset()
{
  std::lock_guard<std::mutex> lock(TrackballMutex);

  TrackballPendingX[0] = 0.0;
  TrackballPendingY[0] = 0.0;
  TrackballPendingX[1] = 0.0;
  TrackballPendingY[1] = 0.0;

  TrackballX[0] = 0;
  TrackballY[0] = 0;
  TrackballX[1] = 0;
  TrackballY[1] = 0;

  TrackballResetActive = false;
}

bool KonamiIsKDeadEye()
{
  return System::GetRunningCode() == "kdeadeye";
}

static u16 KonamiScaleLightgunAxis(float normalized, u16 min_value, u16 max_value)
{
  normalized = std::clamp(normalized, 0.0f, 1.0f);
  return static_cast<u16>(static_cast<float>(min_value) + (normalized * static_cast<float>(max_value - min_value)));
}

static u16 KonamiGetLightgunX(u32 player)
{
  if (player >= 2)
    return LIGHTGUN_X_CENTER;

  std::lock_guard<std::mutex> lock(LightgunMutex);

  if (LightgunOffscreen[player] || LightgunShootOffscreen[player])
    return LIGHTGUN_X_OFFSCREEN;

  return KonamiScaleLightgunAxis(LightgunNormalizedX[player], LIGHTGUN_X_MIN, LIGHTGUN_X_MAX);
}

static u16 KonamiGetLightgunY(u32 player)
{
  if (player >= 2)
    return LIGHTGUN_Y_CENTER;

  std::lock_guard<std::mutex> lock(LightgunMutex);

  if (LightgunOffscreen[player] || LightgunShootOffscreen[player])
    return LIGHTGUN_Y_OFFSCREEN;

  return KonamiScaleLightgunAxis(LightgunNormalizedY[player], LIGHTGUN_Y_MIN, LIGHTGUN_Y_MAX);
}

void KonamiLightgunX1Read(u32 Size, u32 Offset, u32& Value)
{
  Value = KonamiGetLightgunX(0);
}

void KonamiLightgunY1Read(u32 Size, u32 Offset, u32& Value)
{
  Value = KonamiGetLightgunY(0);
}

void KonamiLightgunX2Read(u32 Size, u32 Offset, u32& Value)
{
  Value = KonamiGetLightgunX(1);
}

void KonamiLightgunY2Read(u32 Size, u32 Offset, u32& Value)
{
  Value = KonamiGetLightgunY(1);
}

void KonamiLightgunButtonsRead(u32 Size, u32 Offset, u32& Value)
{
  std::lock_guard<std::mutex> lock(LightgunMutex);

  Value = 0xFFFF;

  if (LightgunTrigger[0])
    Value &= ~0x0001U;

  if (LightgunTrigger[1])
    Value &= ~0x0002U;
}

void KonamiLightgunWrite(u32 Size, u32 Offset, u32 Value)
{
  // Ignored / nop.
}

void KonamiLightgunSetPosition(u32 Player, float X, float Y)
{
  if (Player >= 2)
    return;

  std::lock_guard<std::mutex> lock(LightgunMutex);

  LightgunOffscreen[Player] = (X < 0.0f || X > 1.0f || Y < 0.0f || Y > 1.0f);
  LightgunNormalizedX[Player] = std::clamp(X, 0.0f, 1.0f);
  LightgunNormalizedY[Player] = std::clamp(Y, 0.0f, 1.0f);
}

void KonamiLightgunSetTrigger(u32 Player, bool Pressed)
{
  if (Player >= 2)
    return;

  std::lock_guard<std::mutex> lock(LightgunMutex);
  LightgunTrigger[Player] = Pressed;
}

void KonamiLightgunSetShootOffscreen(u32 Player, bool Pressed)
{
  if (Player >= 2)
    return;

  std::lock_guard<std::mutex> lock(LightgunMutex);
  LightgunShootOffscreen[Player] = Pressed;
}

// Misc

void KonamiEepromFixup(void)
{
  Eeprom[0] = 0;

  for (size_t i = 1; i < std::size(Eeprom); i++)
    Eeprom[0] = static_cast<u16>(Eeprom[0] + Eeprom[i]);

  KonamiSaveEepromFile();
}

void KonamiScoreInit(void)
{
  u8* Ram = Bus::g_ram;

  // Update the template score table
  memcpy(Ram + 0x132A8, &ScoreTable, sizeof(ScoreTable));
}

void KonamiScoreUpdate(void)
{
  u8* Ram = Bus::g_ram;

  // Copy the live score table
  memcpy(&ScoreTable, Ram + 0xE31E0, sizeof(ScoreTable));
}
