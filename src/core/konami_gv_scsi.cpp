#include "konami_gv_scsi.h"
#include "konami_gv_cdrom.h"

#include "bus.h"
#include "cdrom.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "konami.h"
#include "system.h"
#include "timing_event.h"

#include "common/cd_image.h"
#include "common/file_system.h"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

// Legacy Konami GV NCR53CF96/SCSI shim implementation.
//
// Existing SCSI behavior will be moved here from konami.cpp without functional
// changes before the shim is replaced with the proper NCR53CF96/CD-ROM path.

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

// DEBUG CODE
// Readable trace helpers for the legacy Konami GV SCSI shim.
// These functions are diagnostic only and must not change controller behavior.

static const char* KonamiGVScsiCommandName(u8 command)
{
  switch (command)
  {
    case 0x00:
      return "TEST UNIT READY";

    case 0x03:
      return "REQUEST SENSE";

    case 0x12:
      return "INQUIRY";

    case 0x15:
      return "MODE SELECT(6)";

    case 0x1A:
      return "MODE SENSE(6)";

    case 0x28:
      return "READ(10)";

    case 0x42:
      return "READ SUB-CHANNEL";

    case 0x43:
      return "READ TOC";

    case 0x48:
      return "PLAY AUDIO TRACK/INDEX";

    case 0x4B:
      return "PAUSE/RESUME";

    default:
      return "UNKNOWN";
  }
}

static const char* KonamiGVScsiTransferDirection(u8 command)
{
  switch (command)
  {
    case 0x03:
    case 0x12:
    case 0x1A:
    case 0x28:
    case 0x42:
    case 0x43:
      return "CD-ROM -> HOST";

    case 0x15:
      return "HOST -> CD-ROM";

    default:
      return "NONE";
  }
}

static u32 KonamiGVScsiExpectedTransferLength(const u8* cdb)
{
  switch (cdb[0])
  {
    case 0x03: // REQUEST SENSE
    case 0x12: // INQUIRY
    case 0x15: // MODE SELECT(6)
    case 0x1A: // MODE SENSE(6)
      return cdb[4];

    case 0x28: // READ(10)
    {
      const u32 sector_count = (static_cast<u32>(cdb[7]) << 8) | static_cast<u32>(cdb[8]);
      return sector_count * CDImage::DATA_SECTOR_SIZE;
    }

    case 0x42: // READ SUB-CHANNEL
    case 0x43: // READ TOC
      return (static_cast<u32>(cdb[7]) << 8) | static_cast<u32>(cdb[8]);

    default:
      return 0;
  }
}

static u32 KonamiGVScsiTransferCounter()
{
  return static_cast<u32>(ScsiRegs[REG_XFERCNTLOW]) | (static_cast<u32>(ScsiRegs[REG_XFERCNTMID]) << 8) |
         (static_cast<u32>(ScsiRegs[REG_XFERCNTHI]) << 16);
}

static void KonamiGVTraceScsiCommand()
{
  std::FILE* fp = std::fopen("konami_gv_scsi_trace.txt", "ab");
  if (!fp)
    return;

  const u8 command = ScsiCommand[0];

  std::fprintf(fp,
               "\n"
               "GAME=%s PC=0x%08X\n"
               "CDB=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n"
               "COMMAND=%s (0x%02X)\n"
               "DIRECTION=%s\n"
               "EXPECTED_TRANSFER=%u\n"
               "NCR_TRANSFER_COUNTER=%u\n"
               "STATUS=0x%02X IRQ=0x%02X INT=0x%02X FIFO=0x%02X\n",
               System::GetRunningCode().c_str(), CPU::g_state.current_instruction_pc, ScsiCommand[0], ScsiCommand[1],
               ScsiCommand[2], ScsiCommand[3], ScsiCommand[4], ScsiCommand[5], ScsiCommand[6], ScsiCommand[7],
               ScsiCommand[8], ScsiCommand[9], ScsiCommand[10], ScsiCommand[11], KonamiGVScsiCommandName(command),
               command, KonamiGVScsiTransferDirection(command), KonamiGVScsiExpectedTransferLength(ScsiCommand),
               KonamiGVScsiTransferCounter(), ScsiRegs[REG_STATUS], ScsiRegs[REG_IRQSTATE], ScsiRegs[REG_INTSTATE],
               ScsiRegs[REG_FIFOSTATE]);

  switch (command)
  {
    case 0x28: // READ(10)
    {
      const u32 lba = (static_cast<u32>(ScsiCommand[2]) << 24) | (static_cast<u32>(ScsiCommand[3]) << 16) |
                      (static_cast<u32>(ScsiCommand[4]) << 8) | static_cast<u32>(ScsiCommand[5]);

      const u32 sector_count = (static_cast<u32>(ScsiCommand[7]) << 8) | static_cast<u32>(ScsiCommand[8]);

      std::fprintf(fp, "READ10 LBA=%u SECTORS=%u\n", lba, sector_count);
      break;
    }

    case 0x42: // READ SUB-CHANNEL
    {
      const u32 allocation_length = (static_cast<u32>(ScsiCommand[7]) << 8) | static_cast<u32>(ScsiCommand[8]);

      std::fprintf(fp, "SUBCHANNEL MSF=%u SUBQ=%u FORMAT=0x%02X TRACK=%u ALLOCATION=%u\n",
                   (ScsiCommand[1] & 0x02) ? 1U : 0U, (ScsiCommand[2] & 0x40) ? 1U : 0U, ScsiCommand[3], ScsiCommand[6],
                   allocation_length);
      break;
    }

    case 0x43: // READ TOC
    {
      const u32 allocation_length = (static_cast<u32>(ScsiCommand[7]) << 8) | static_cast<u32>(ScsiCommand[8]);

      std::fprintf(fp, "TOC MSF=%u FORMAT=0x%02X START_TRACK=%u ALLOCATION=%u\n", (ScsiCommand[1] & 0x02) ? 1U : 0U,
                   ScsiCommand[2] & 0x0F, ScsiCommand[6], allocation_length);
      break;
    }

    case 0x48: // PLAY AUDIO TRACK/INDEX
      std::fprintf(fp, "PLAY START_TRACK=%u START_INDEX=%u END_TRACK=%u END_INDEX=%u\n", ScsiCommand[4], ScsiCommand[5],
                   ScsiCommand[7], ScsiCommand[8]);
      break;

    case 0x4B: // PAUSE/RESUME
      std::fprintf(fp, "AUDIO RESUME=%u\n", (ScsiCommand[8] & 0x01) ? 1U : 0U);
      break;

    default:
      break;
  }

  std::fclose(fp);
}

static std::unique_ptr<TimingEvent> ScsiIrqEvent;
static void KonamiGVScsiAssertInterrupt();
static void ScsiIrqEventCallback(void* param, TickCount ticks, TickCount ticks_late)
{
  if (ScsiIrqEvent)
    ScsiIrqEvent->Deactivate();

    KonamiGVScsiAssertInterrupt();
}

void KonamiGVScsiInitialize()
{
  KonamiGVCDROMInitialize();

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
}

void KonamiGVScsiShutdown()
{
  KonamiGVCDROMShutdown();
}

static void KonamiGVScsiAssertInterrupt()
{
  g_interrupt_controller.InterruptRequest(InterruptController::IRQ::IRQ10);
}

void KonamiScsiIrqDeassert(void)
{
  CPU::ClearExternalInterrupt((u8)InterruptController::IRQ::IRQ10);
}

static bool KonamiReadMountedDataSector(u32 lba, u8* sector)
{
  return KonamiGVCDROMReadDataSector(lba, sector);
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

  // DEBUG CODE
  // MODE SELECT(6) transfers a parameter list from system RAM to the CD-ROM.
  // Capture the payload before the legacy shim completes the DMA transfer.
  if ((Value & 1) != 0 && ScsiCommand[0] == 0x15)
  {
    const size_t parameter_length = std::min<size_t>(ReadSize, ScsiCommand[4]);

    if (std::FILE* fp = std::fopen("konami_gv_scsi_trace.txt", "ab"))
    {
      std::fprintf(fp,
                   "MODE_SELECT_PAYLOAD GAME=%s PC=0x%08X ADDRESS=0x%08X "
                   "DMA_LENGTH=%zu CDB_LENGTH=%u DATA=",
                   System::GetRunningCode().c_str(), CPU::g_state.current_instruction_pc, Address, ReadSize,
                   ScsiCommand[4]);

      for (size_t i = 0; i < parameter_length; i++)
        std::fprintf(fp, "%02X%s", Ram[Address + i], (i + 1 < parameter_length) ? " " : "");

      std::fprintf(fp, "\n");
      std::fclose(fp);
    }
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
        const u32 response_size =
          static_cast<u32>(std::min<size_t>(ReadSize, KonamiGVScsiExpectedTransferLength(ScsiCommand)));

        const u32 response_length = KonamiGVCDROMReadTOC(ScsiCommand, Ram + Address, response_size);

        if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
        {
          std::fprintf(fp, "SCSI READ TOC response address=0x%08X requested=%u returned=%u\n", Address, response_size,
                       response_length);
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
              std::fprintf(fp, "SCSI READ10 failed mounted sector read lba=%u address=0x%08X\n", ScsiSectorLba,
                           Address);
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
        // Log_WarningPrintf("Unimplemented SCSI command %02X", ScsiCmd[0]);
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
          KonamiGVScsiAssertInterrupt();
          break;
        case 0x03:
          ScsiRegs[REG_INTSTATE] = 0x04;
          KonamiGVScsiAssertInterrupt();
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

          // DEBUG CODE
          KonamiGVTraceScsiCommand();

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

                KonamiGVScsiAssertInterrupt();
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
          KonamiGVScsiAssertInterrupt();
          break;
        case 0x44:
          break;
        case 0x10:
          if (Value & 0x80U)
          {
            ScsiRegs[REG_STATUS] = (ScsiRegs[REG_STATUS] & ~0x07) | 3;
            ScsiRegs[REG_INTSTATE] = 0x00;
            KonamiGVScsiAssertInterrupt();
            break;
          }
          [[fallthrough]];

        case 0x11:
          KonamiGVScsiAssertInterrupt();
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