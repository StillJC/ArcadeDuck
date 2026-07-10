#include "bus.h"
#include "cdrom.h"
#include "common/cd_image.h"
#include "common/file_system.h"
#include "common/log.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "host_display.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "konami.h"
#include "system.h"
#include "timers.h"
#include "timing_event.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdio.h>
#include <vector>

Log_SetChannel(Konami);

// Legacy Konami GV SCSI shim.
//
// This is currently a hand-written approximation of the NCR53CF96/CD-ROM path.
// MAME models Konami GV as NCR53CF96 + NSCSI CD-ROM + PSX DMA channel 5.
// These globals are the fake register/FIFO/command state used by the current shim.
// Beat the Champ is exposing why this needs to become a real NCR53CF96-style path.
//
// Current call flow:
//   bus.cpp 0x1F000000-0x1F00001F
//     -> KonamiScsiRead()/KonamiScsiWrite()
//     -> ScsiRegs/ScsiFifo/ScsiCommand
//     -> KonamiDmaControlWrite()
//     -> fake REQUEST SENSE / INQUIRY / MODE SENSE / READ10 / READ TOC responses.
//
// Do not add more game-specific SCSI hacks here unless they are temporary diagnostics.
// Long-term target: replace this shim with a proper GV SCSI controller layer.
enum
{
  REG_XFERCNTLOW = 0, // read = current xfer count lo byte, write = set xfer count lo byte
  REG_XFERCNTMID,     // read = current xfer count mid byte, write = set xfer count mid byte
  REG_FIFO,           // read/write = FIFO
  REG_COMMAND,        // read/write = command
  REG_STATUS,         // read = status, write = destination SCSI ID (4)
  REG_IRQSTATE,       // read = IRQ status, write = timeout         (5)
  REG_INTSTATE,       // read = internal state, write = sync xfer period (6)
  REG_FIFOSTATE,      // read = FIFO status, write = sync offset
  REG_CTRL1,          // read/write = control 1
  REG_CLOCKFCTR,      // clock factor (write only)
  REG_TESTMODE,       // test mode (write only)
  REG_CTRL2,          // read/write = control 2
  REG_CTRL3,          // read/write = control 3
  REG_CTRL4,          // read/write = control 4
  REG_XFERCNTHI,      // read = current xfer count hi byte, write = set xfer count hi byte
  REG_DATAALIGN       // data alignment (write only)
};

static u8 ScsiRegs[16];
static u8 ScsiFifo[16];
static u8 ScsiFifoPtr;
static u8 ScsiCommand[12];
static bool ScsiIsRead;
static u32 ScsiSectorLba;
static bool ScsiAudioPlaying;
static bool ScsiAudioPaused;
static u8 ScsiAudioTrack;
static u8 ScsiAudioIndex;
static u32 ScsiAudioRelativeLba;

static std::unique_ptr<TimingEvent> ScsiIrqEvent;

// Buttons
static u32 CurrentButtons[2];

// FLASH
static constexpr u32 FLASH_SIZE = 0x200000;
static constexpr u32 FLASH_SECTOR_SIZE = 0x10000;

static constexpr u32 KDEADEYE_FLASH_SIZE = 0x80000;

static u8 Flash[4][FLASH_SIZE];
static u32 FlashAddress;

static u8 KDeadEyeFlash[KDEADEYE_FLASH_SIZE];
static bool KDeadEyeFlashValid = false;
static bool KDeadEyeFlashDirty = false;
static std::string KDeadEyeFlashPath;

enum KonamiKDeadEyeFlashMode : u8
{
  KDEADEYE_FLASH_MODE_READ_ARRAY,
  KDEADEYE_FLASH_MODE_READ_STATUS,
  KDEADEYE_FLASH_MODE_PROGRAM,
  KDEADEYE_FLASH_MODE_ERASE_SETUP
};

static KonamiKDeadEyeFlashMode KDeadEyeFlashMode = KDEADEYE_FLASH_MODE_READ_ARRAY;
static u16 KDeadEyeFlashStatus = 0x0080;

static bool KonamiLoadKDeadEyeFlashFile(const std::string& path)
{
  for (u32 i = 0; i < KDEADEYE_FLASH_SIZE; i++)
    KDeadEyeFlash[i] = 0xFF;

  KDeadEyeFlashValid = false;

  std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "rb");
  if (!fp)
    return false;

  std::fseek(fp, 0, SEEK_END);
  const long size = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);

  if (size != static_cast<long>(KDEADEYE_FLASH_SIZE))
  {
    std::fclose(fp);
    return false;
  }

  const size_t read = std::fread(KDeadEyeFlash, 1, KDEADEYE_FLASH_SIZE, fp);
  std::fclose(fp);

  KDeadEyeFlashValid = (read == KDEADEYE_FLASH_SIZE);
  return KDeadEyeFlashValid;
}

static void KonamiSaveKDeadEyeFlashFile()
{
  if (!KDeadEyeFlashValid || !KDeadEyeFlashDirty || KDeadEyeFlashPath.empty())
    return;

  std::FILE* fp = FileSystem::OpenCFile(KDeadEyeFlashPath.c_str(), "wb");
  if (!fp)
    return;

  std::fwrite(KDeadEyeFlash, 1, KDEADEYE_FLASH_SIZE, fp);
  std::fclose(fp);

  KDeadEyeFlashDirty = false;
}

static u16 KonamiKDeadEyeSingleFlashRead16(u32 relative_offset)
{
  relative_offset &= (KDEADEYE_FLASH_SIZE - 1);

  const u32 high_offset = relative_offset & ~1U;
  const u32 low_offset = (high_offset + 1) & (KDEADEYE_FLASH_SIZE - 1);

  return static_cast<u16>((KDeadEyeFlash[high_offset] << 8) | KDeadEyeFlash[low_offset]);
}

static u16 KonamiKDeadEyeFallbackFlashRead16(u32 relative_offset)
{
  const u32 word_offset = (relative_offset >> 1) & 0x3FFFFF;
  const u32 chip = (word_offset >= FLASH_SIZE) ? 2 : 0;
  const u32 chip_offset = word_offset & (FLASH_SIZE - 1);

  return static_cast<u16>(Flash[chip][chip_offset] | (Flash[chip + 1][chip_offset] << 8));
}

static u16 KonamiKDeadEyeFlashRead16(u32 relative_offset)
{
  if (KDeadEyeFlashValid)
    return KonamiKDeadEyeSingleFlashRead16(relative_offset);

  return KonamiKDeadEyeFallbackFlashRead16(relative_offset);
}

void KonamiKDeadEyeFlashRead(u32 Size, u32 Offset, u32& Value)
{
  static int kdeadeye_flash_read_debug_count = 0;

  const u32 relative_offset = (Offset >= 0x00380000) ? (Offset - 0x00380000) : Offset;

  if (KDeadEyeFlashValid && KDeadEyeFlashMode == KDEADEYE_FLASH_MODE_READ_STATUS)
  {
    Value = KDeadEyeFlashStatus & 0xFF;

    if (Size == 2)
      Value |= Value << 8;
    else if (Size == 4)
      Value |= Value << 8 | Value << 16 | Value << 24;

    return;
  }

  switch (Size)
  {
    case 1:
    {
      if (KDeadEyeFlashValid)
        Value = KDeadEyeFlash[relative_offset & (KDEADEYE_FLASH_SIZE - 1)];
      else
      {
        const u16 word = KonamiKDeadEyeFlashRead16(relative_offset);
        Value = (relative_offset & 1) ? (word >> 8) : (word & 0xFF);
      }

      break;
    }

    case 2:
      Value = KonamiKDeadEyeFlashRead16(relative_offset);
      break;

    case 4:
    {
      const u16 low = KonamiKDeadEyeFlashRead16(relative_offset);
      const u16 high = KonamiKDeadEyeFlashRead16(relative_offset + 2);
      Value = static_cast<u32>(low) | (static_cast<u32>(high) << 16);
      break;
    }

    default:
      Value = 0xFFFFFFFF;
      break;
  }

  if (kdeadeye_flash_read_debug_count < 1000)
  {
    if (std::FILE* fp = std::fopen("konami_gv_direct_flash_debug.txt", "ab"))
    {
      std::fprintf(fp, "READ size=%u offset=0x%08X relative=0x%08X single=%u value=0x%08X\n", Size, Offset,
                   relative_offset, KDeadEyeFlashValid ? 1 : 0, Value);
      std::fclose(fp);
    }

    kdeadeye_flash_read_debug_count++;
  }
}

void KonamiKDeadEyeFlashWrite(u32 Size, u32 Offset, u32 Value)
{
  static int kdeadeye_flash_write_debug_count = 0;

  const u32 relative_offset = (Offset >= 0x00380000) ? (Offset - 0x00380000) : Offset;
  const u32 byte_offset = relative_offset & (KDEADEYE_FLASH_SIZE - 1);
  const u16 command = static_cast<u16>(Value & 0xFF);

  if (kdeadeye_flash_write_debug_count < 1000)
  {
    if (std::FILE* fp = std::fopen("konami_gv_direct_flash_debug.txt", "ab"))
    {
      std::fprintf(fp, "WRITE size=%u offset=0x%08X relative=0x%08X single=%u mode=%u value=0x%08X\n", Size, Offset,
                   relative_offset, KDeadEyeFlashValid ? 1 : 0, static_cast<u32>(KDeadEyeFlashMode), Value);
      std::fclose(fp);
    }

    kdeadeye_flash_write_debug_count++;
  }

  if (!KDeadEyeFlashValid)
    return;

  if (KDeadEyeFlashMode == KDEADEYE_FLASH_MODE_PROGRAM)
  {
    switch (Size)
    {
      case 1:
        KDeadEyeFlash[byte_offset] &= static_cast<u8>(Value & 0xFF);
        break;

      case 2:
      {
        const u32 high_offset = byte_offset & ~1U;
        const u32 low_offset = (high_offset + 1) & (KDEADEYE_FLASH_SIZE - 1);

        KDeadEyeFlash[high_offset] &= static_cast<u8>((Value >> 8) & 0xFF);
        KDeadEyeFlash[low_offset] &= static_cast<u8>(Value & 0xFF);
        break;
      }

      case 4:
      {
        const u32 first_offset = byte_offset & ~1U;
        const u32 second_offset = (first_offset + 2) & (KDEADEYE_FLASH_SIZE - 1);

        const u16 low_word = static_cast<u16>(Value & 0xFFFF);
        const u16 high_word = static_cast<u16>((Value >> 16) & 0xFFFF);

        KDeadEyeFlash[first_offset] &= static_cast<u8>((low_word >> 8) & 0xFF);
        KDeadEyeFlash[(first_offset + 1) & (KDEADEYE_FLASH_SIZE - 1)] &= static_cast<u8>(low_word & 0xFF);
        KDeadEyeFlash[second_offset] &= static_cast<u8>((high_word >> 8) & 0xFF);
        KDeadEyeFlash[(second_offset + 1) & (KDEADEYE_FLASH_SIZE - 1)] &= static_cast<u8>(high_word & 0xFF);
        break;
      }
    }

    KDeadEyeFlashDirty = true;
    KDeadEyeFlashStatus = 0x0080;
    KDeadEyeFlashMode = KDEADEYE_FLASH_MODE_READ_STATUS;
    return;
  }

  if (KDeadEyeFlashMode == KDEADEYE_FLASH_MODE_ERASE_SETUP)
  {
    if (command == 0xD0)
    {
      const u32 block_start = byte_offset & ~(FLASH_SECTOR_SIZE - 1);

      for (u32 i = 0; i < FLASH_SECTOR_SIZE; i++)
        KDeadEyeFlash[(block_start + i) & (KDEADEYE_FLASH_SIZE - 1)] = 0xFF;

      KDeadEyeFlashDirty = true;
      KDeadEyeFlashStatus = 0x0080;
      KDeadEyeFlashMode = KDEADEYE_FLASH_MODE_READ_STATUS;
    }
    else
    {
      KDeadEyeFlashStatus = 0x00B0;
      KDeadEyeFlashMode = KDEADEYE_FLASH_MODE_READ_STATUS;
    }

    return;
  }

  switch (command)
  {
    case 0xFF:
      // Read array / reset.
      KDeadEyeFlashMode = KDEADEYE_FLASH_MODE_READ_ARRAY;
      KDeadEyeFlashStatus = 0x0080;
      break;

    case 0x70:
      // Read status register.
      KDeadEyeFlashMode = KDEADEYE_FLASH_MODE_READ_STATUS;
      break;

    case 0x50:
      // Clear status register.
      KDeadEyeFlashStatus = 0x0080;
      break;

    case 0x40:
    case 0x10:
      // Program next write.
      KDeadEyeFlashMode = KDEADEYE_FLASH_MODE_PROGRAM;
      KDeadEyeFlashStatus = 0x0080;
      break;

    case 0x20:
      // Erase setup. Next write should be 0xD0.
      KDeadEyeFlashMode = KDEADEYE_FLASH_MODE_ERASE_SETUP;
      KDeadEyeFlashStatus = 0x0080;
      break;

    case 0xB0:
      // Suspend not currently needed; report ready.
      KDeadEyeFlashMode = KDEADEYE_FLASH_MODE_READ_STATUS;
      KDeadEyeFlashStatus = 0x0080;
      break;

    case 0xD0:
      // Resume/confirm without erase setup. Treat as ready.
      KDeadEyeFlashMode = KDEADEYE_FLASH_MODE_READ_STATUS;
      KDeadEyeFlashStatus = 0x0080;
      break;

    default:
      if (kdeadeye_flash_write_debug_count < 1000)
      {
        if (std::FILE* fp = std::fopen("konami_gv_direct_flash_debug.txt", "ab"))
        {
          std::fprintf(fp, "UNKNOWN KDeadEye flash command offset=0x%08X command=0x%02X value=0x%08X\n", Offset,
                       command, Value);
          std::fclose(fp);
        }

        kdeadeye_flash_write_debug_count++;
      }

      break;
  }
}
enum KonamiFlashMode : u8
{
  KONAMI_FLASH_MODE_READ_ARRAY = 0,
  KONAMI_FLASH_MODE_UNLOCK_1,
  KONAMI_FLASH_MODE_UNLOCK_2,
  KONAMI_FLASH_MODE_AUTOSELECT,
  KONAMI_FLASH_MODE_PROGRAM,
  KONAMI_FLASH_MODE_ERASE_UNLOCK_1,
  KONAMI_FLASH_MODE_ERASE_UNLOCK_2,
  KONAMI_FLASH_MODE_ERASE_SELECT
};

static KonamiFlashMode FlashModeState[4];
static bool FlashDirty[4];

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
static u16 KonamiEepromSwap16(u16 value);
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

  if (System::GetRunningCode() == "btchamp")
  {
    for (size_t i = 0; i < std::size(Eeprom); i++)
      Eeprom[i] = static_cast<u16>((static_cast<u16>(raw[i * 2]) << 8) | raw[(i * 2) + 1]);
  }
  else
  {
    std::memcpy(Eeprom, raw, sizeof(Eeprom));
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

static bool LoadFlashFile(const char *Path, void *Buffer)
{
  std::FILE* fp;

  fp = FileSystem::OpenCFile(Path, "rb");
  if (!fp)
  {
    return false;
  }
  if (fread(Buffer, 1, 0x200000, fp) != 0x200000)
  {
    return false;
  }
  fclose(fp);

  return true;
}

static void AssertScsiInterrupt(void)
{
  g_interrupt_controller.InterruptRequest(InterruptController::IRQ::IRQ10);
}

static void ScsiIrqEventCallback(void* param, TickCount ticks, TickCount ticks_late)
{
  if (ScsiIrqEvent)
    ScsiIrqEvent->Deactivate();

  AssertScsiInterrupt();
}

static void QueueScsiInterruptTicks(TickCount ticks)
{
  if (!ScsiIrqEvent)
  {
    ScsiIrqEvent = TimingEvents::CreateTimingEvent("Konami GV SCSI IRQ", 1, 1, ScsiIrqEventCallback, nullptr, false);
  }

  if (ticks == 0)
    AssertScsiInterrupt();
  else
    ScsiIrqEvent->Schedule(ticks);
}

static bool KonamiFlashFileIsMissingOrErased(const std::string& path)
{
  std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "rb");
  if (!fp)
    return true;

  std::fseek(fp, 0, SEEK_END);
  const long size = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);

  if (size != static_cast<long>(FLASH_SIZE))
  {
    std::fclose(fp);
    return true;
  }

  u8 buffer[4096];

  while (!std::feof(fp))
  {
    const size_t read = std::fread(buffer, 1, sizeof(buffer), fp);

    for (size_t i = 0; i < read; i++)
    {
      if (buffer[i] != 0xFF)
      {
        std::fclose(fp);
        return false;
      }
    }
  }

  std::fclose(fp);
  return true;
}

static bool KonamiReadMountedDataSector(u32 lba, u8* sector)
{
  const CDImage* media = g_cdrom.GetMedia();
  CDImage* image = const_cast<CDImage*>(media);
  if (!image)
    return false;

  if (!image->Seek(1, static_cast<CDImage::LBA>(lba)))
    return false;

  if (System::GetRunningCode() == "btchamp")
  {
    u8 raw_sector[CDImage::RAW_SECTOR_SIZE];

    if (image->Read(CDImage::ReadMode::RawSector, 1, raw_sector) != 1)
      return false;

    // Beat the Champ's GV CHD presents its useful 2048-byte payload at the
    // beginning of DuckStation's raw-sector buffer.
    std::memcpy(sector, raw_sector, CDImage::DATA_SECTOR_SIZE);
    return true;
  }

  return image->Read(CDImage::ReadMode::DataOnly, 1, sector) == 1;
}

static bool KonamiReadDataSectorFromImage(CDImage* image, u32 lba, u8* sector)
{
  if (!image)
    return false;

  if (!image->Seek(1, static_cast<CDImage::LBA>(lba)))
    return false;

  return image->Read(CDImage::ReadMode::DataOnly, 1, sector) == 1;
}

static bool KonamiWriteFlashFile(const std::string& path, const std::vector<u8>& data)
{
  std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "wb");
  if (!fp)
    return false;

  const bool ok = std::fwrite(data.data(), 1, data.size(), fp) == data.size();
  std::fclose(fp);
  return ok;
}

static bool KonamiExtractFlashPairFromImage(CDImage* image, u32 start_lba, const std::string& low_path,
                                            const std::string& high_path)
{
  std::vector<u8> low(FLASH_SIZE);
  std::vector<u8> high(FLASH_SIZE);

  u8 sector[CDImage::DATA_SECTOR_SIZE];
  u32 output_offset = 0;

  for (u32 sector_index = 0; sector_index < 2048; sector_index++)
  {
    const u32 lba = start_lba + sector_index;

    if (!KonamiReadDataSectorFromImage(image, lba, sector))
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

  return KonamiWriteFlashFile(low_path, low) && KonamiWriteFlashFile(high_path, high);
}

static bool KonamiExtractFlashPairFromIso(std::FILE* iso_fp, u32 iso_offset, const std::string& low_path,
                                          const std::string& high_path)
{
  std::FILE* low_fp = FileSystem::OpenCFile(low_path.c_str(), "wb");
  if (!low_fp)
    return false;

  std::FILE* high_fp = FileSystem::OpenCFile(high_path.c_str(), "wb");
  if (!high_fp)
  {
    std::fclose(low_fp);
    return false;
  }

  if (std::fseek(iso_fp, iso_offset, SEEK_SET) != 0)
  {
    std::fclose(low_fp);
    std::fclose(high_fp);
    return false;
  }

  for (u32 i = 0; i < FLASH_SIZE; i++)
  {
    const int low = std::fgetc(iso_fp);
    const int high = std::fgetc(iso_fp);

    if (low < 0 || high < 0)
    {
      std::fclose(low_fp);
      std::fclose(high_fp);
      return false;
    }

    const u8 low_byte = static_cast<u8>(low);
    const u8 high_byte = static_cast<u8>(high);

    std::fwrite(&low_byte, 1, 1, low_fp);
    std::fwrite(&high_byte, 1, 1, high_fp);
  }

  std::fclose(low_fp);
  std::fclose(high_fp);
  return true;
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

static bool KonamiImageIsRaw2048Iso(std::FILE* fp)
{
  if (std::fseek(fp, 0x8001, SEEK_SET) != 0)
    return false;

  char magic[5];
  const bool is_iso = std::fread(magic, 1, sizeof(magic), fp) == sizeof(magic) && magic[0] == 'C' && magic[1] == 'D' &&
                      magic[2] == '0' && magic[3] == '0' && magic[4] == '1';

  std::fseek(fp, 0, SEEK_SET);
  return is_iso;
}

static void KonamiGenerateSimpsonsFlashIfNeeded(const std::string& flash0_path, const std::string& flash1_path,
                                                const std::string& flash2_path, const std::string& flash3_path)
{
  const bool needs_flash =
    KonamiFlashFileIsMissingOrErased(flash0_path) || KonamiFlashFileIsMissingOrErased(flash1_path) ||
    KonamiFlashFileIsMissingOrErased(flash2_path) || KonamiFlashFileIsMissingOrErased(flash3_path);

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
  const bool ok01 = KonamiExtractFlashPairFromImage(image.get(), 202, flash0_path, flash1_path);
  const bool ok23 = KonamiExtractFlashPairFromImage(image.get(), 2250, flash2_path, flash3_path);

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

void KonamiInit(void)
{
  if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
  {
    std::fprintf(fp, "KONAMI INIT CALLED\n");
    std::fclose(fp);
  }

  if (!ScsiIrqEvent)
  {
    ScsiIrqEvent = TimingEvents::CreateTimingEvent("Konami GV SCSI IRQ", 1, 1, ScsiIrqEventCallback, nullptr, false);
  }
  else
  {
    ScsiIrqEvent->Deactivate();
  }

  ScsiAudioPlaying = false;
  ScsiAudioPaused = false;
  ScsiAudioTrack = 1;
  ScsiAudioIndex = 1;
  ScsiAudioRelativeLba = 0;

  // ArcadeDuck uses the active MAME set name for per-game NVRAM.
  // Example:
  //   simpbowl.zip -> nvram/simpbowl
  //   kdeadeye.zip -> nvram/kdeadeye
  //   btchamp.zip  -> nvram/btchamp
  const std::string& game_name = System::GetRunningCode();

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
  const std::string kdeadeye_flash_path = nvram_dir + FS_OSPATH_SEPARATOR_STR "flash";

  // Empty flash chips should read as erased flash, not zero-filled RAM.
  for (u32 chip = 0; chip < 4; chip++)
  {
    for (u32 i = 0; i < FLASH_SIZE; i++)
      Flash[chip][i] = 0xFF;
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

  for (u32 i = 0; i < KDEADEYE_FLASH_SIZE; i++)
    KDeadEyeFlash[i] = 0xFF;

  KDeadEyeFlashValid = false;
  KDeadEyeFlashDirty = false;
  KDeadEyeFlashPath.clear();
  KDeadEyeFlashMode = KDEADEYE_FLASH_MODE_READ_ARRAY;
  KDeadEyeFlashStatus = 0x0080;
  FlashAddress = 0;

  for (u32 chip = 0; chip < 4; chip++)
  {
    FlashModeState[chip] = KONAMI_FLASH_MODE_READ_ARRAY;
    FlashDirty[chip] = false;
  }

  if (!eeprom_path.empty())
    LoadEepromFile(eeprom_path.c_str());

  if (KonamiUsesDirectGVFlash())
  {
    KonamiLoadKDeadEyeFlashFile(kdeadeye_flash_path);
    KDeadEyeFlashPath = kdeadeye_flash_path;

    if (std::FILE* fp = std::fopen("konami_gv_direct_flash_debug.txt", "ab"))
    {
      std::fprintf(fp, "Loaded single direct GV flash file: %s valid=%u\n", kdeadeye_flash_path.c_str(),
                   KDeadEyeFlashValid ? 1 : 0);
      std::fclose(fp);
    }
  }
  else
  {
    KonamiGenerateSimpsonsFlashIfNeeded(flash0_path, flash1_path, flash2_path, flash3_path);

    LoadFlashFile(flash0_path.c_str(), Flash[0]);
    LoadFlashFile(flash1_path.c_str(), Flash[1]);
    LoadFlashFile(flash2_path.c_str(), Flash[2]);
    LoadFlashFile(flash3_path.c_str(), Flash[3]);
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
  KonamiSaveKDeadEyeFlashFile();

  if (EepromFp)
  {
    KonamiSaveEepromFile();
    std::fclose(EepromFp);
    EepromFp = nullptr;
  }
}

// Legacy SCSI DMA / fake command execution.
// This is where the current shim manually answers SCSI commands like
// REQUEST SENSE, INQUIRY, MODE SENSE, READ10, READ TOC, and READ SUB-CHANNEL.
// MAME does this through NCR53CF96 + NSCSI CD-ROM behavior instead.

void KonamiDmaControlWrite(u32& ControlBits, u32& Address, u32 Value)
{
  size_t ReadSize = (ControlBits >> 16) * 4;
  u8* Ram = Bus::g_ram;
  static u8 Sector[2048];

  if (std::FILE* fp = std::fopen("konami_gv_scsi_dma_debug.txt", "ab"))
  {
    std::fprintf(fp,
                 "SCSI DMA ENTER pc=0x%08X control=0x%08X address=0x%08X value=0x%08X "
                 "cmd=0x%02X read_size_initial=%zu status=0x%02X irq=0x%02X int=0x%02X fifo_state=0x%02X\n",
                 CPU::g_state.current_instruction_pc, ControlBits, Address, Value, ScsiCommand[0], ReadSize,
                 ScsiRegs[REG_STATUS], ScsiRegs[REG_IRQSTATE], ScsiRegs[REG_INTSTATE], ScsiRegs[REG_FIFOSTATE]);
    std::fclose(fp);
  }

  // Some Konami GV READ10 transfers use a DMA block count of 0.
  // On PS1-style DMA, this can represent 0x10000 words, but for SCSI READ10
  // we can recover the intended byte count directly from the command's sector count.
  if (ReadSize == 0 && ScsiCommand[0] == 0x28)
  {
    const u32 read10_sector_count = (static_cast<u32>(ScsiCommand[7]) << 8) | ScsiCommand[8];

    if (read10_sector_count != 0)
      ReadSize = static_cast<size_t>(read10_sector_count) * CDImage::DATA_SECTOR_SIZE;
  }
    if ((Value & 1) == 0)
  {
    switch (ScsiCommand[0])
    {
      case 0x03:
        // REQUEST SENSE. Return a valid fixed-format "no sense / no error" packet.
        std::memset(Ram + Address, 0, 12);

        Ram[Address + 0] = 0x70; // fixed-format current error response
        Ram[Address + 2] = 0x00; // sense key: no sense
        Ram[Address + 7] = 0x0A; // additional sense length

        if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
        {
          std::fprintf(fp, "SCSI REQUEST SENSE response address=0x%08X response=0x%02X sense=0x%02X length=%u\n",
                       Address, Ram[Address + 0], Ram[Address + 2], Ram[Address + 7]);
          std::fclose(fp);
        }
        break;
      case 0x12:
      {
        // INQUIRY. Return a minimal valid CD-ROM device identity.
        std::memset(Ram + Address, 0, 32);

        Ram[Address + 0] = 0x05; // peripheral device type: CD/DVD device
        Ram[Address + 1] = 0x80; // removable media
        Ram[Address + 2] = 0x02; // ISO/ECMA/ANSI version-ish
        Ram[Address + 3] = 0x02; // response data format
        Ram[Address + 4] = 0x1B; // additional length for 32-byte response

        std::memcpy(Ram + Address + 8, "KONAMI  ", 8);
        std::memcpy(Ram + Address + 16, "GV CD-ROM       ", 16);

        if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
        {
          std::fprintf(fp, "SCSI INQUIRY response address=0x%08X type=0x%02X removable=0x%02X length=%u\n", Address,
                       Ram[Address + 0], Ram[Address + 1], Ram[Address + 4]);
          std::fclose(fp);
        }
        break;
      }
      case 0x1A:
      {
        // MODE SENSE(6). Dead Eye requests page 0x0E, CD audio control.
        // Return a minimal valid mode page instead of all zeroes.
        std::memset(Ram + Address, 0, 28);

        Ram[Address + 0] = 0x12; // mode data length: bytes after this field
        Ram[Address + 1] = 0x00; // medium type
        Ram[Address + 2] = 0x00; // device-specific parameter
        Ram[Address + 3] = 0x00; // block descriptor length

        Ram[Address + 4] = 0x0E; // page code: CD audio control
        Ram[Address + 5] = 0x0E; // page length

        // Conservative/default CD audio control values.
        Ram[Address + 6] = 0x04;
        Ram[Address + 7] = 0x00;
        Ram[Address + 8] = 0x00;
        Ram[Address + 9] = 0x00;
        Ram[Address + 10] = 0x00;
        Ram[Address + 11] = 0x00;
        Ram[Address + 12] = 0x00;
        Ram[Address + 13] = 0x00;
        Ram[Address + 14] = 0x00;
        Ram[Address + 15] = 0x00;
        Ram[Address + 16] = 0x00;
        Ram[Address + 17] = 0x00;
        Ram[Address + 18] = 0x00;
        Ram[Address + 19] = 0x00;

        if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
        {
          std::fprintf(fp, "SCSI MODE SENSE response address=0x%08X page=0x%02X length=%u mode_length=%u\n", Address,
                       Ram[Address + 4], Ram[Address + 5], Ram[Address + 0]);
          std::fclose(fp);
        }
        break;
      }
      case 0x43:
      {
        // READ TOC. Return a minimal fake TOC with track 1 data and track 2 audio.
        std::memset(Ram + Address, 0, 12);

        Ram[Address + 0] = 0x00;
        Ram[Address + 1] = 0x1A;
        Ram[Address + 2] = 0x01;
        Ram[Address + 3] = (System::GetRunningCode() == "btchamp") ? 0x0C : 0x02;

        Ram[Address + 4] = 0x00;
        Ram[Address + 5] = 0x14;
        Ram[Address + 6] = 0x01;
        Ram[Address + 7] = 0x00;

        Ram[Address + 8] = 0x00;
        Ram[Address + 9] = 0x00;
        Ram[Address + 10] = 0x00;
        Ram[Address + 11] = 0x00;

        if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
        {
          std::fprintf(fp, "SCSI READ TOC response address=0x%08X length=12 first=1 last=2\n", Address);
          std::fclose(fp);
        }
        break;
      }
      case 0x28:
        if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
        {
          std::fprintf(fp, "SCSI READ10 start_lba=%u read_size=%zu address=0x%08X\n", ScsiSectorLba, ReadSize, Address);
          std::fclose(fp);
        }

          while (ReadSize >= 2048)
        {
            if (KonamiReadMountedDataSector(ScsiSectorLba, Sector))
            {
              static int read10_data_debug_count = 0;

              if (read10_data_debug_count < 20000)
              {
                u32 checksum = 0;

                for (u32 i = 0; i < 2048; i++)
                  checksum = (checksum + Sector[i]) & 0xFFFFFFFFU;

                if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
                {
                  std::fprintf(fp,
                               "SCSI READ10 DATA count=%d lba=%u sectors=%u address=0x%08X first=%02X %02X %02X %02X "
                               "checksum=0x%08X\n",
                               read10_data_debug_count, ScsiSectorLba,
                               (static_cast<u32>(ScsiCommand[7]) << 8) | ScsiCommand[8], Address, Sector[0], Sector[1],
                               Sector[2], Sector[3], checksum);
                  std::fclose(fp);
                }

                read10_data_debug_count++;
              }

              std::memcpy(Ram + Address, Sector, 2048);
              CPU::CodeCache::InvalidateCodePages(Address, 2048 / sizeof(u32));

              if (Address <= 0x00056D2C && (Address + 2048) > 0x00056D2C)
              {
                const u32 target_offset = 0x00056D2C - Address;
                const u32 word_before = static_cast<u32>(Sector[target_offset - 4]) |
                                        (static_cast<u32>(Sector[target_offset - 3]) << 8) |
                                        (static_cast<u32>(Sector[target_offset - 2]) << 16) |
                                        (static_cast<u32>(Sector[target_offset - 1]) << 24);
                const u32 word_at = static_cast<u32>(Sector[target_offset + 0]) |
                                    (static_cast<u32>(Sector[target_offset + 1]) << 8) |
                                    (static_cast<u32>(Sector[target_offset + 2]) << 16) |
                                    (static_cast<u32>(Sector[target_offset + 3]) << 24);
                const u32 word_after = static_cast<u32>(Sector[target_offset + 4]) |
                                       (static_cast<u32>(Sector[target_offset + 5]) << 8) |
                                       (static_cast<u32>(Sector[target_offset + 6]) << 16) |
                                       (static_cast<u32>(Sector[target_offset + 7]) << 24);

                if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
                {
                  std::fprintf(fp,
                               "SCSI TARGET EPC LOAD lba=%u address=0x%08X target_offset=0x%X "
                               "before=0x%08X at=0x%08X after=0x%08X\n",
                               ScsiSectorLba, Address, target_offset, word_before, word_at, word_after);
                  std::fclose(fp);
                }
              }
            }
          else
          {
            std::memset(Ram + Address, 0, 2048);

            if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
            {
              std::fprintf(fp, "SCSI READ10 failed mounted sector read lba=%u address=0x%08X\n", ScsiSectorLba, Address);
              std::fclose(fp);
            }
          }

          ScsiSectorLba++;
          Address += 2048;
          ReadSize -= 2048;
        }
        ControlBits &= 0xFFFF;
        break;
      case 0x42:
      {
        // READ SUB-CHANNEL. Match the earlier KDeadEye branch stub that got the title/display path moving.
        const u8 response[16] = {0x00, 0x11, 0x00, 0x0C, 0x01, 0x00, 0x02, 0x01,
                                 0x00, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};

        const size_t copy_size = std::min<size_t>(ReadSize, sizeof(response));
        std::memcpy(Ram + Address, response, copy_size);

        if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
        {
          std::fprintf(fp, "SCSI READ SUBCHANNEL OLD STUB copy=%u status_byte=%02X track=%u index=%u\n",
                       static_cast<u32>(copy_size), response[1], response[6], response[7]);
          std::fclose(fp);
        }

        break;
      }
        default:
        //Log_WarningPrintf("Unimplemented SCSI command %02X", ScsiCmd[0]);
        break;
    }
  }

  // End the fake SCSI DMA transfer.
  // MAME drives this through NCR53CF96 DRQ + PSX DMA channel 5; our legacy shim
  // currently completes the transfer synchronously, so make sure the DMA channel
  // is no longer marked active after command data has been copied.
  ControlBits &= 0xFFFF;
  ScsiRegs[REG_STATUS] &= ~0x07U;
}

// SCSI

void KonamiScsiRead(u32 Size, u32 Offset, u32& Value)
{
  const u8 Register = (Offset & 0x1F) >> 1;

  Value = ScsiRegs[Register];

  switch (Register)
  {
    case REG_FIFO:
      Value = 0;
      break;
    case REG_IRQSTATE:
      ScsiRegs[REG_STATUS] &= ~0x80U;
      break;
  }
}

void KonamiScsiWrite(u32 Size, u32 Offset, u32 Value)
{
  const u8 Register = (Offset & 0x1F) >> 1;

  switch (Register)
  {
    case REG_XFERCNTLOW:
    case REG_XFERCNTMID:
    case REG_XFERCNTHI:
      ScsiRegs[REG_STATUS] &= ~0x10;
      break;
    case REG_FIFO:
      ScsiFifo[ScsiFifoPtr++] = (u8)Value;
      if (ScsiFifoPtr > 15)
      {
        ScsiFifoPtr = 15;
      }
      break;
    case REG_COMMAND:
      ScsiFifoPtr = 0;

      switch (Value & 0x7F)
      {
        case 0x00:
          ScsiRegs[REG_IRQSTATE] = 8;
          break;
        case 0x02:
          ScsiRegs[REG_IRQSTATE] = 8;
          ScsiRegs[REG_STATUS] |= 0x80;
          AssertScsiInterrupt();
          break;
        case 0x03:
          ScsiRegs[REG_INTSTATE] = 0x04;
          AssertScsiInterrupt();
          break;
        case 0x42:
          if (ScsiFifo[1] == 0 || ScsiFifo[1] == 0x48 || ScsiFifo[1] == 0x4B)
          {
            ScsiRegs[REG_INTSTATE] = 0x06;
          }
          else
          {
            ScsiRegs[REG_INTSTATE] = 0x04;
          }

          for (int i = 0; i < 12; i++)
          {
            ScsiCommand[i] = ScsiFifo[1 + i];
          }

          if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
          {
            std::fprintf(fp, "SCSI cmd=%02X fifo=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                         ScsiCommand[0], ScsiCommand[0], ScsiCommand[1], ScsiCommand[2], ScsiCommand[3], ScsiCommand[4],
                         ScsiCommand[5], ScsiCommand[6], ScsiCommand[7], ScsiCommand[8], ScsiCommand[9],
                         ScsiCommand[10], ScsiCommand[11]);
            std::fclose(fp);
          }

          ScsiIsRead = false;
          ScsiIsRead = false;
          switch (ScsiCommand[0])
          {
            case 0x03:
            case 0x12:
            case 0x1A:
            case 0x42:
            case 0x43:
              ScsiRegs[REG_STATUS] = (ScsiRegs[REG_STATUS] & ~0x07) | 1;
              break;

            case 0x00:
            case 0x15:
              if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
              {
                std::fprintf(fp, "SCSI simple command complete cmd=%02X status_phase=0\n", ScsiCommand[0]);
                std::fclose(fp);
              }

              ScsiRegs[REG_STATUS] = (ScsiRegs[REG_STATUS] & ~0x07) | 0;
              break;

            case 0x48: // PLAY AUDIO TRACK/INDEX
            case 0x4B: // PAUSE/RESUME
            {
              if (KonamiIsKDeadEye() || System::GetRunningCode() == "btchamp")
              {
                ScsiAudioPlaying = false;
                ScsiAudioPaused = false;

                ScsiRegs[REG_STATUS] = (ScsiRegs[REG_STATUS] & ~0x87U) | 0x00U;
                ScsiRegs[REG_INTSTATE] = 0x04U;

                if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
                {
                  std::fprintf(fp, "SCSI AUDIO STUB cmd=%02X no-op success game=%s\n", ScsiCommand[0],
                               System::GetRunningCode().c_str());
                  std::fclose(fp);
                }

                AssertScsiInterrupt();
                break;
              }
              ScsiRegs[REG_STATUS] = (ScsiRegs[REG_STATUS] & ~0x07) | 0;
              break;
            }
            case 0x28:
              ScsiSectorLba = (ScsiFifo[3] << 24) | (ScsiFifo[4] << 16) | (ScsiFifo[5] << 8) | ScsiFifo[6];
              ScsiIsRead = true;
              ScsiRegs[REG_STATUS] = (ScsiRegs[REG_STATUS] & ~0x07) | 1;
              break;
          }
          AssertScsiInterrupt();
          break;
        case 0x44:
          break;
        case 0x10:
          if (Value & 0x80U)
          {
            ScsiRegs[REG_STATUS] = (ScsiRegs[REG_STATUS] & ~0x07) | 3;
            ScsiRegs[REG_INTSTATE] = 0x00;
            AssertScsiInterrupt();
            break;
          }
          [[fallthrough]];

        case 0x11:
          AssertScsiInterrupt();
          ScsiRegs[REG_STATUS] &= ~0x87;
          ScsiRegs[REG_INTSTATE] = 0x00;
          [[fallthrough]]; // ?? Why ??

        case 0x12:
          ScsiRegs[REG_STATUS] |= 0x80U;
          ScsiRegs[REG_INTSTATE] = 0x06U;
          break;

        default:
          // Log_WarningPrintf("Unknown command %02X!", value);
          break;
      }
      break;
  }
  if (Register != REG_STATUS && Register != REG_INTSTATE && Register != REG_IRQSTATE && Register != REG_FIFOSTATE)
  {
    ScsiRegs[Register] = (uint8_t)Value;
  }
}

// Player 1 controls

void KonamiP1Read(u32 Size, u32 Offset, u32& Value)
{
  Value = CurrentButtons[0];

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
}

void KonamiP2Write(u32 Size, u32 Offset, u32 Value)
{
  // Ignored
}

// FLASH

static bool KonamiFlashIsUnlockAAAddress(u32 Address)
{
  return ((Address & 0x0FFF) == 0x0555) || ((Address & 0x0FFF) == 0x0AAA) || ((Address & 0xFFFF) == 0x5555);
}

static bool KonamiFlashIsUnlock55Address(u32 Address)
{
  return ((Address & 0xFFFF) == 0x02AA) || ((Address & 0xFFFF) == 0x2AAA) || ((Address & 0x0FFF) == 0x0555);
}

static bool KonamiFlashIsCommandAddress(u32 Address)
{
  return ((Address & 0xFFFF) == 0x0555) || ((Address & 0xFFFF) == 0x5555) || ((Address & 0x0FFF) == 0x0AAA);
}

static u8 KonamiFlashChipRead(u8 Chip, u32 Address)
{
  Address &= (FLASH_SIZE - 1);

  if (FlashModeState[Chip] == KONAMI_FLASH_MODE_AUTOSELECT)
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

  return Flash[Chip][Address];
}

static void KonamiFlashEraseSector(u8 Chip, u32 Address)
{
  const u32 SectorBase = Address & ~(FLASH_SECTOR_SIZE - 1);

  for (u32 i = 0; i < FLASH_SECTOR_SIZE; i++)
    Flash[Chip][(SectorBase + i) & (FLASH_SIZE - 1)] = 0xFF;

  FlashDirty[Chip] = true;
}

static void KonamiFlashEraseChip(u8 Chip)
{
  for (u32 i = 0; i < FLASH_SIZE; i++)
    Flash[Chip][i] = 0xFF;

  FlashDirty[Chip] = true;
}

static void KonamiFlashChipWrite(u8 Chip, u32 Address, u8 Data)
{
  Address &= (FLASH_SIZE - 1);

  // Reset/read-array command.
  if (Data == 0xF0 || Data == 0xFF)
  {
    FlashModeState[Chip] = KONAMI_FLASH_MODE_READ_ARRAY;
    return;
  }

  switch (FlashModeState[Chip])
  {
    case KONAMI_FLASH_MODE_READ_ARRAY:
    case KONAMI_FLASH_MODE_AUTOSELECT:
      if (Data == 0xAA && KonamiFlashIsUnlockAAAddress(Address))
        FlashModeState[Chip] = KONAMI_FLASH_MODE_UNLOCK_1;
      break;

    case KONAMI_FLASH_MODE_UNLOCK_1:
      if (Data == 0x55 && KonamiFlashIsUnlock55Address(Address))
        FlashModeState[Chip] = KONAMI_FLASH_MODE_UNLOCK_2;
      else
        FlashModeState[Chip] = KONAMI_FLASH_MODE_READ_ARRAY;
      break;

    case KONAMI_FLASH_MODE_UNLOCK_2:
      if (!KonamiFlashIsCommandAddress(Address))
      {
        FlashModeState[Chip] = KONAMI_FLASH_MODE_READ_ARRAY;
        break;
      }

      switch (Data)
      {
        case 0x90:
          FlashModeState[Chip] = KONAMI_FLASH_MODE_AUTOSELECT;
          break;

        case 0xA0:
          FlashModeState[Chip] = KONAMI_FLASH_MODE_PROGRAM;
          break;

        case 0x80:
          FlashModeState[Chip] = KONAMI_FLASH_MODE_ERASE_UNLOCK_1;
          break;

        default:
          FlashModeState[Chip] = KONAMI_FLASH_MODE_READ_ARRAY;
          break;
      }
      break;

    case KONAMI_FLASH_MODE_PROGRAM:
      // Real flash can clear bits from 1 to 0, not set 0 back to 1 without erase.
      Flash[Chip][Address] &= Data;
      FlashDirty[Chip] = true;
      FlashModeState[Chip] = KONAMI_FLASH_MODE_READ_ARRAY;
      break;

    case KONAMI_FLASH_MODE_ERASE_UNLOCK_1:
      if (Data == 0xAA && KonamiFlashIsUnlockAAAddress(Address))
        FlashModeState[Chip] = KONAMI_FLASH_MODE_ERASE_UNLOCK_2;
      else
        FlashModeState[Chip] = KONAMI_FLASH_MODE_READ_ARRAY;
      break;

    case KONAMI_FLASH_MODE_ERASE_UNLOCK_2:
      if (Data == 0x55 && KonamiFlashIsUnlock55Address(Address))
        FlashModeState[Chip] = KONAMI_FLASH_MODE_ERASE_SELECT;
      else
        FlashModeState[Chip] = KONAMI_FLASH_MODE_READ_ARRAY;
      break;

    case KONAMI_FLASH_MODE_ERASE_SELECT:
      if (Data == 0x10 && KonamiFlashIsCommandAddress(Address))
        KonamiFlashEraseChip(Chip);
      else if (Data == 0x30)
        KonamiFlashEraseSector(Chip, Address);

      FlashModeState[Chip] = KONAMI_FLASH_MODE_READ_ARRAY;
      break;
  }
}

void KonamiFlashRead(u32 Size, u32 Offset, u32& Value)
{
  static int flash_read_debug_count = 0;

  const u32 raw_offset = Offset;
  Offset &= 0xF;

  switch (Offset)
  {
    case 0:
    {
      const u8 chip = (FlashAddress >= 0x200000) ? 2 : 0;
      const u32 address = FlashAddress & 0x1FFFFF;

      const u8 low = KonamiFlashChipRead(chip, address);
      const u8 high = KonamiFlashChipRead(chip + 1, address);

      Value = static_cast<u32>(low) | (static_cast<u32>(high) << 8);

      if (flash_read_debug_count < 500)
      {
        if (std::FILE* fp = std::fopen("konami_gv_flash_debug.txt", "ab"))
        {
          std::fprintf(fp, "FLASH READ raw_offset=0x%08X offset=0x%X address=0x%08X chip=%u value=0x%04X mode=%u/%u\n",
                       raw_offset, Offset, FlashAddress, chip, Value & 0xFFFF, static_cast<u32>(FlashModeState[chip]),
                       static_cast<u32>(FlashModeState[chip + 1]));
          std::fclose(fp);
        }
        flash_read_debug_count++;
      }

      FlashAddress++;
      break;
    }

    case 8:
      FlashAddress |= 1;
      Value = 0;
      break;

    default:
      Value = 0;
      break;
  }
}

void KonamiFlashWrite(u32 Size, u32 Offset, u32 Value)
{
  static int flash_write_debug_count = 0;

  const u32 raw_offset = Offset;
  Offset &= 0xF;

  if (flash_write_debug_count < 2000)
  {
    if (std::FILE* fp = std::fopen("konami_gv_flash_debug.txt", "ab"))
    {
      std::fprintf(fp, "FLASH WRITE raw_offset=0x%08X offset=0x%X value=0x%04X old_address=0x%08X\n", raw_offset,
                   Offset, Value & 0xFFFF, FlashAddress);
      std::fclose(fp);
    }
    flash_write_debug_count++;
  }

  switch (Offset)
  {
    case 0:
    {
      const u8 chip = (FlashAddress >= 0x200000) ? 2 : 0;
      const u32 address = FlashAddress & 0x1FFFFF;

      KonamiFlashChipWrite(chip, address, static_cast<u8>(Value & 0xFF));
      KonamiFlashChipWrite(chip + 1, address, static_cast<u8>((Value >> 8) & 0xFF));
      break;
    }

    case 2:
      FlashAddress = 0;
      FlashAddress |= Value << 1;
      break;

    case 4:
      FlashAddress &= 0xFF00FF;
      FlashAddress |= Value << 8;
      break;

    case 6:
      FlashAddress &= 0x00FFFF;
      FlashAddress |= Value << 15;
      break;
  }
}

// EEPROM

static void KonamiSerialEepromWrite(u32 Value)
{
  static int serial_debug_count = 0;

  const bool new_di = (Value & 0x01) != 0;
  const bool new_cs = (Value & 0x02) != 0;
  const bool new_clk = (Value & 0x04) != 0;

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
            std::fprintf(fp, "EEPROM WRITE address=%u write_all=%u enabled=%u wire=0x%04X raw=0x%04X\n",
                         EepromWriteAddress & 0x3F, EepromWriteAll ? 1 : 0, EepromWriteEnabled ? 1 : 0, wire_value,
                         raw_value);
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
          const u16 raw_value = Eeprom[address & 0x3F];
          const u16 swapped_value = KonamiEepromSwap16(raw_value);

          if (serial_debug_count < 300)
          {
            if (std::FILE* fp = std::fopen("konami_gv_eeprom_serial_debug.txt", "ab"))
            {
              std::fprintf(
                fp, "EEPROM CMD shift=0x%03X start=%u opcode=%u address=%u raw=0x%04X swapped=0x%04X enabled=%u\n",
                EepromShiftIn, start, opcode, address, raw_value, swapped_value, EepromWriteEnabled ? 1 : 0);
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
            // READ
            EepromReadShift = swapped_value;
            EepromReadBits = 16;
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
    Value = Eeprom[((Offset - 0x80) & 0x7F) >> 1];
  }
  else
  {
    Log_WarningPrintf("%s: %08X", __FUNCTION__, Offset);
    Value = 0;
  }
}

static u16 KonamiEepromSwap16(u16 value)
{
  return static_cast<u16>((value >> 8) | (value << 8));
}

static void KonamiSaveEepromFile()
{
  if (!EepromFp)
    return;

  std::fseek(EepromFp, 0, SEEK_SET);

if (System::GetRunningCode() == "btchamp")
  {
    u8 raw[sizeof(Eeprom)];

    for (size_t i = 0; i < std::size(Eeprom); i++)
    {
      raw[i * 2] = static_cast<u8>((Eeprom[i] >> 8) & 0xFF);
      raw[(i * 2) + 1] = static_cast<u8>(Eeprom[i] & 0xFF);
    }

    std::fwrite(raw, 1, sizeof(raw), EepromFp);
  }
  else
  {
    std::fwrite(Eeprom, 1, sizeof(Eeprom), EepromFp);
  }

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
    u8 RelativeOffset = (Offset - 0x80) & 0x7F;
    Eeprom[RelativeOffset >> 1] = (u16)Value;

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
  map_button(0x8000, 1 << 6); // Button 3 / Square
  map_button(0x1000, 1 << 7); // Button 4 / Triangle

  map_button(0x0008, 1 << 9);  // Start
  map_button(0x0001, 1 << 10); // Coin / Select

  if (Player == 0)
  {
    map_button(0x0002, 1 << 11); // Service / L3
    map_button(0x0004, 1 << 12); // Test / R3
  }

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

void KonamiScsiIrqDeassert(void)
{
  CPU::ClearExternalInterrupt((u8)InterruptController::IRQ::IRQ10);
}

void KonamiEepromFixup(void)
{
        size_t i;

        Eeprom[0] = 0;
        for (i = 1; i < 64; i++) {
                Eeprom[0] += Eeprom[i];
        }
        std::fseek(EepromFp, 0, SEEK_SET);
        fwrite(Eeprom, 1, sizeof(Eeprom), EepromFp);
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
