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

enum class KonamiGVNCR53CF96Mode : u8
{
  Disconnected,
  Target,
  Initiator
};

enum class KonamiGVNCR53CF96DmaDirection : u8
{
  None,
  DeviceToHost,
  HostToDevice
};

struct KonamiGVNCR53CF96State
{
  // Existing legacy register/FIFO/CDB state.
  // These remain connected to the current command path until each controller
  // behavior is replaced in later checkpoints.
  u8 registers[16];
  u8 fifo[16];
  u8 fifo_count;
  u8 identify_message;
  u8 cdb[12];
  bool data_in;
  u32 sector_lba;

  // NCR53CF96 controller foundation.
  u8 command_queue[2];
  u8 command_queue_count;

  u8 status;
  u8 interrupt_status;
  u8 config1;
  u8 config2;
  u8 config3;
  u8 config4;
  u8 fifo_alignment;

  bool test_mode;

  u32 transfer_count;
  u32 transfer_counter;
  u32 transfer_counter_mask;

  u8 sequence_step;
  u8 clock_factor;
  u8 sync_period;
  u8 sync_offset;

  u8 destination_id;
  u8 selection_timeout;

  u32 controller_state;
  u8 transfer_phase;

  KonamiGVNCR53CF96Mode mode;
  KonamiGVNCR53CF96DmaDirection dma_direction;

  bool dma_command;
  bool irq;
  bool drq;
};

static KonamiGVNCR53CF96State ScsiController;

// Temporary compatibility aliases.
//
// These preserve the existing legacy command/response behavior while its state
// is migrated into ScsiController one checkpoint at a time.
static u8 (&ScsiRegs)[16] = ScsiController.registers;
static u8 (&ScsiFifo)[16] = ScsiController.fifo;
static u8 (&ScsiCommand)[12] = ScsiController.cdb;
static bool& ScsiIsRead = ScsiController.data_in;
static u32& ScsiSectorLba = ScsiController.sector_lba;

static constexpr u8 NCR53CF96_FIFO_CAPACITY = 16;

static constexpr u8 NCR53CF96_STATUS_VALID_GROUP_CODE = 0x08;
static constexpr u8 NCR53CF96_STATUS_TERMINAL_COUNT = 0x10;
static constexpr u8 NCR53CF96_STATUS_PARITY_ERROR = 0x20;
static constexpr u8 NCR53CF96_STATUS_GROSS_ERROR = 0x40;
static constexpr u8 NCR53CF96_STATUS_INTERRUPT = 0x80;

static constexpr u8 NCR53CF96_INTERRUPT_FUNCTION_COMPLETE = 0x08;

static constexpr u8 NCR53CF96_COMMAND_NOP = 0x00;
static constexpr u8 NCR53CF96_COMMAND_FLUSH_FIFO = 0x01;
static constexpr u8 NCR53CF96_COMMAND_RESET_CHIP = 0x02;
static constexpr u8 NCR53CF96_COMMAND_RESET_BUS = 0x03;
static constexpr u8 NCR53CF96_COMMAND_SELECT_WITH_ATN = 0x42;
static constexpr u8 NCR53CF96_CONFIG2_FEATURES_ENABLE = 0x08;

static constexpr u32 NCR53CF96_FAMILY_ID = 0x04;
static constexpr u32 NCR53CF96_REVISION_LEVEL = 0x02;

static void KonamiGVScsiWriteStatus(u8 Value)
{
  ScsiController.status = Value;

  // Preserve the legacy backing-register mirror while Status ownership moves
  // into ScsiController.
  ScsiRegs[REG_STATUS] = Value;
}

static void KonamiGVScsiSetStatusBits(u8 Bits)
{
  KonamiGVScsiWriteStatus(ScsiController.status | Bits);
}

static void KonamiGVScsiClearStatusBits(u8 Bits)
{
  KonamiGVScsiWriteStatus(ScsiController.status & ~Bits);
}

static void KonamiGVScsiSetPhase(u8 Phase)
{
  KonamiGVScsiWriteStatus(static_cast<u8>((ScsiController.status & ~0x07U) | (Phase & 0x07U)));
}

static u8 KonamiGVScsiReadTransferCounter(u8 Register)
{
  switch (Register)
  {
    case REG_XFERCNTLOW:
      return static_cast<u8>(ScsiController.transfer_counter);

    case REG_XFERCNTMID:
      return static_cast<u8>(ScsiController.transfer_counter >> 8);

    case REG_XFERCNTHI:
      return static_cast<u8>(ScsiController.transfer_counter >> 16);

    default:
      return 0;
  }
}

static void KonamiGVScsiWriteTransferCount(u8 Register, u8 Value)
{
  switch (Register)
  {
    case REG_XFERCNTLOW:
      ScsiController.transfer_count = (ScsiController.transfer_count & 0x00FFFF00U) | static_cast<u32>(Value);
      break;

    case REG_XFERCNTMID:
      ScsiController.transfer_count = (ScsiController.transfer_count & 0x00FF00FFU) | (static_cast<u32>(Value) << 8);
      break;

    case REG_XFERCNTHI:
      ScsiController.transfer_count = (ScsiController.transfer_count & 0x0000FFFFU) | (static_cast<u32>(Value) << 16);
      break;

    default:
      break;
  }
}

static void KonamiGVScsiLoadTransferCounter(bool DmaCommand)
{
  ScsiController.dma_command = DmaCommand;

  if (!DmaCommand)
  {
    ScsiController.transfer_counter = 0;
    return;
  }

  ScsiController.transfer_counter = ScsiController.transfer_count & ScsiController.transfer_counter_mask;

  // Terminal Count is cleared when a DMA command loads the active counter,
  // not merely when software writes the transfer-count registers.
  KonamiGVScsiClearStatusBits(NCR53CF96_STATUS_TERMINAL_COUNT);

  // Match the 53CF94/96 identification behavior modeled by MAME.
  // With SCSI-2 Features disabled, the next DMA counter load can expose
  // the controller family and revision through the 24-bit counter.
  if ((ScsiController.config2 & NCR53CF96_CONFIG2_FEATURES_ENABLE) == 0)
  {
    ScsiController.transfer_count = (1U << 23) | (NCR53CF96_FAMILY_ID << 19) | (NCR53CF96_REVISION_LEVEL << 16) |
                                    (ScsiController.transfer_count & 0x0000FFFFU);
  }
}

static u8 KonamiGVScsiReadFIFO()
{
  if (ScsiController.fifo_count == 0)
    return 0;

  const u8 Value = ScsiController.fifo[0];

  ScsiController.fifo_count--;

  if (ScsiController.fifo_count != 0)
  {
    std::memmove(ScsiController.fifo, ScsiController.fifo + 1, ScsiController.fifo_count);
  }

  return Value;
}

static void KonamiGVScsiWriteFIFO(u8 Value)
{
  if (ScsiController.fifo_count >= NCR53CF96_FIFO_CAPACITY)
    return;

  ScsiController.fifo[ScsiController.fifo_count] = Value;
  ScsiController.fifo_count++;
}

static void KonamiGVScsiClearFIFO()
{
  ScsiController.fifo_count = 0;
}

static u8 KonamiGVScsiReadFIFOFlags()
{
  return ScsiController.fifo_count & 0x1FU;
}

static u8 KonamiGVScsiReadCommand()
{
  return ScsiController.command_queue[0];
}

static bool KonamiGVScsiQueueCommand(u8 Value)
{
  const u8 Command = Value & 0x7FU;

  // Reset commands execute from the front of the command register.
  if (Command == NCR53CF96_COMMAND_RESET_CHIP || Command == NCR53CF96_COMMAND_RESET_BUS)
    ScsiController.command_queue_count = 0;

  if (ScsiController.command_queue_count >= 2)
  {
    KonamiGVScsiSetStatusBits(NCR53CF96_STATUS_GROSS_ERROR);
    return false;
  }

  ScsiController.command_queue[ScsiController.command_queue_count] = Value;
  ScsiController.command_queue_count++;

  // The current legacy controller executes commands synchronously, so only
  // the command at the front of the queue starts during this checkpoint.
  return ScsiController.command_queue_count == 1;
}

static void KonamiGVScsiCompleteCommand()
{
  if (ScsiController.command_queue_count == 0)
    return;

  ScsiController.command_queue_count--;

  if (ScsiController.command_queue_count != 0)
    ScsiController.command_queue[0] = ScsiController.command_queue[1];
}

static void KonamiGVScsiConsumeSelectionFIFO()
{
  std::memset(ScsiController.cdb, 0, sizeof(ScsiController.cdb));

  ScsiController.identify_message = (ScsiController.fifo_count != 0) ? ScsiController.fifo[0] : 0;

  const u32 CdbSize =
    (ScsiController.fifo_count > 1) ? std::min<u32>(sizeof(ScsiController.cdb), ScsiController.fifo_count - 1) : 0;

  if (CdbSize != 0)
    std::memcpy(ScsiController.cdb, ScsiController.fifo + 1, CdbSize);

  KonamiGVScsiClearFIFO();
}

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
  return ScsiController.transfer_counter;
}

static void KonamiGVTraceScsiCommand()
{
  // DEBUG CODE
  // Beat the Champ polls READ SUB-CHANNEL thousands of times per second.
  // Do not perform synchronous file logging for every poll.
  if (ScsiCommand[0] == 0x42)
    return;

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
               KonamiGVScsiTransferCounter(), ScsiController.status, ScsiController.interrupt_status,
               ScsiController.sequence_step, KonamiGVScsiReadFIFOFlags());

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
static void KonamiGVScsiResetController()
{
  ScsiController = {};

  // NCR53CF96 reset defaults used as the foundation for the new controller.
  ScsiController.transfer_counter_mask = 0xFFFF;
  ScsiController.clock_factor = 2;
  ScsiController.sync_period = 5;

  ScsiController.mode = KonamiGVNCR53CF96Mode::Disconnected;
  ScsiController.dma_direction = KonamiGVNCR53CF96DmaDirection::None;
}
static void KonamiGVScsiAssertInterrupt(u8 Cause = NCR53CF96_INTERRUPT_FUNCTION_COMPLETE);
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

  KonamiGVScsiResetController();
}

void KonamiGVScsiShutdown()
{
  KonamiGVCDROMShutdown();
}

static void KonamiGVScsiAssertInterrupt(u8 Cause)
{
  ScsiController.interrupt_status |= Cause;
  ScsiController.irq = true;

  KonamiGVScsiSetStatusBits(NCR53CF96_STATUS_INTERRUPT);

  g_interrupt_controller.InterruptRequest(InterruptController::IRQ::IRQ10);
}

static u8 KonamiGVScsiReadStatus()
{
  // Status bit 7 reflects the controller's actual interrupt-output state.
  return static_cast<u8>((ScsiController.status & ~NCR53CF96_STATUS_INTERRUPT) |
                         (ScsiController.irq ? NCR53CF96_STATUS_INTERRUPT : 0));
}

static void KonamiGVScsiSetSequenceStep(u8 Value)
{
  ScsiController.sequence_step = Value & 0x07U;
}

static u8 KonamiGVScsiReadInterruptStatus()
{
  const u8 Value = ScsiController.interrupt_status;

  if (Value != 0)
  {
    // Reading the Interrupt register acknowledges the pending controller
    // interrupt and clears the latched Status/Sequence information.
    ScsiController.interrupt_status = 0;
    KonamiGVScsiSetSequenceStep(0);
    ScsiController.irq = false;

    KonamiGVScsiClearStatusBits(NCR53CF96_STATUS_INTERRUPT | NCR53CF96_STATUS_GROSS_ERROR |
                                NCR53CF96_STATUS_PARITY_ERROR | NCR53CF96_STATUS_VALID_GROUP_CODE);

    CPU::ClearExternalInterrupt(static_cast<u8>(InterruptController::IRQ::IRQ10));
  }

  return Value;
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

  // Some Konami GV READ10 transfers use a DMA block count of 0.
  // On PS1-style DMA, this can represent 0x10000 words, but for SCSI READ10
  // we can recover the intended byte count directly from the command's sector count.
  if (ReadSize == 0 && ScsiCommand[0] == 0x28)
  {
    const u32 read10_sector_count = (static_cast<u32>(ScsiCommand[7]) << 8) | ScsiCommand[8];

    if (read10_sector_count != 0)
      ReadSize = static_cast<size_t>(read10_sector_count) * CDImage::DATA_SECTOR_SIZE;
  }

  // MODE SELECT(6) transfers a parameter list from system RAM to the CD-ROM.
  if ((Value & 1) != 0 && ScsiCommand[0] == 0x15)
  {
    const size_t parameter_length = std::min<size_t>(ReadSize, ScsiCommand[4]);

    const u8* const parameter_data = Ram + Address;

    // DEBUG CODE
    if (std::FILE* fp = std::fopen("konami_gv_scsi_trace.txt", "ab"))
    {
      std::fprintf(fp,
                   "MODE_SELECT_PAYLOAD GAME=%s PC=0x%08X "
                   "ADDRESS=0x%08X DMA_LENGTH=%zu CDB_LENGTH=%u DATA=",
                   System::GetRunningCode().c_str(), CPU::g_state.current_instruction_pc, Address, ReadSize,
                   ScsiCommand[4]);

      for (size_t i = 0; i < parameter_length; i++)
      {
        std::fprintf(fp, "%02X%s", parameter_data[i], (i + 1 < parameter_length) ? " " : "");
      }

      std::fprintf(fp, "\n");
      std::fclose(fp);
    }

    if (parameter_length >= 4)
    {
      const size_t block_descriptor_length = parameter_data[3];

      size_t page_offset = 4 + block_descriptor_length;

      while ((page_offset + 2) <= parameter_length)
      {
        const u8 page_code = parameter_data[page_offset] & 0x3F;

        const size_t page_length = parameter_data[page_offset + 1];

        const size_t page_size = 2 + page_length;

        if ((page_offset + page_size) > parameter_length)
          break;

        if (page_code == 0x0E && page_length >= 0x0E)
        {
          const size_t audio_output_offset = page_offset + 16;

          if ((audio_output_offset + 8) > parameter_length)
            break;

          for (u8 output = 0; output < 4; output++)
          {
            const size_t output_offset = audio_output_offset + (output * 2);

            KonamiGVCDROMSetAudioOutput(output, parameter_data[output_offset], parameter_data[output_offset + 1]);
          }

          // DEBUG CODE
          if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
          {
            std::fprintf(fp,
                         "SCSI MODE SELECT CDDA "
                         "out0=%u/%u out1=%u/%u "
                         "out2=%u/%u out3=%u/%u\n",
                         parameter_data[audio_output_offset + 0] & 0x0F, parameter_data[audio_output_offset + 1],
                         parameter_data[audio_output_offset + 2] & 0x0F, parameter_data[audio_output_offset + 3],
                         parameter_data[audio_output_offset + 4] & 0x0F, parameter_data[audio_output_offset + 5],
                         parameter_data[audio_output_offset + 6] & 0x0F, parameter_data[audio_output_offset + 7]);

            std::fclose(fp);
          }
          break;
        }

        page_offset += page_size;
      }
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
        // MODE SENSE(6). Return CD Audio Control page 0x0E using
        // the GV CD-ROM layer's current output routing and volume state.
        std::memset(Ram + Address, 0, 28);

        Ram[Address + 0] = 0x1B; // mode data length: bytes after this field
        Ram[Address + 1] = 0x00; // medium type
        Ram[Address + 2] = 0x00; // device-specific parameter
        Ram[Address + 3] = 0x00; // block descriptor length

        Ram[Address + 4] = 0x0E; // page code: CD audio control
        Ram[Address + 5] = 0x16; // page length

        Ram[Address + 6] = 0x04;

        for (u8 output = 0; output < 4; output++)
        {
          u8 channel = 0;
          u8 volume = 0;

          KonamiGVCDROMGetAudioOutput(output, &channel, &volume);

          const u32 output_offset = Address + 20 + (output * 2);

          Ram[output_offset + 0] = channel;
          Ram[output_offset + 1] = volume;
        }

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
        while (ReadSize >= 2048)
        {
          if (KonamiReadMountedDataSector(ScsiSectorLba, Sector))
          {
            std::memcpy(Ram + Address, Sector, 2048);
            CPU::CodeCache::InvalidateCodePages(Address, 2048 / sizeof(u32));
          }
          else
          {
            std::memset(Ram + Address, 0, 2048);
          }

          ScsiSectorLba++;
          Address += 2048;
          ReadSize -= 2048;
        }
        ControlBits &= 0xFFFF;
        break;
      case 0x42:
      {
        const u32 response_size =
          static_cast<u32>(std::min<size_t>(ReadSize, KonamiGVScsiExpectedTransferLength(ScsiCommand)));

        KonamiGVCDROMReadSubChannel(ScsiCommand, Ram + Address, response_size);
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
  KonamiGVScsiSetPhase(0);
}

// SCSI

void KonamiScsiRead(u32 Size, u32 Offset, u32& Value)
{
  const u8 Register = (Offset & 0x1F) >> 1;

  Value = ScsiRegs[Register];

  switch (Register)
  {
    case REG_XFERCNTLOW:
    case REG_XFERCNTMID:
    case REG_XFERCNTHI:
      Value = KonamiGVScsiReadTransferCounter(Register);
      break;

    case REG_FIFO:
      Value = KonamiGVScsiReadFIFO();
      break;

    case REG_COMMAND:
      Value = KonamiGVScsiReadCommand();
      break;

    case REG_STATUS:
      Value = KonamiGVScsiReadStatus();
      break;

    case REG_IRQSTATE:
      Value = KonamiGVScsiReadInterruptStatus();
      break;

    case REG_INTSTATE:
      Value = ScsiController.sequence_step;
      break;

    case REG_FIFOSTATE:
      Value = KonamiGVScsiReadFIFOFlags();
      break;

    case REG_CLOCKFCTR:
    case REG_TESTMODE:
    case REG_DATAALIGN:
      Value = 0xFFU;
      break;

    case REG_CTRL1:
      Value = ScsiController.config1;
      break;

    case REG_CTRL2:
      Value = ScsiController.config2;
      break;

    case REG_CTRL3:
      Value = ScsiController.config3;
      break;

    case REG_CTRL4:
      Value = ScsiController.config4;
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
      KonamiGVScsiWriteTransferCount(Register, static_cast<u8>(Value));
      break;
    case REG_FIFO:
      KonamiGVScsiWriteFIFO(static_cast<u8>(Value));
      break;

    case REG_STATUS:
      ScsiController.destination_id = static_cast<u8>(Value & 0x07U);
      break;

    case REG_IRQSTATE:
      ScsiController.selection_timeout = static_cast<u8>(Value);
      break;

    case REG_INTSTATE:
      ScsiController.sync_period = static_cast<u8>(Value & 0x1FU);
      break;

    case REG_FIFOSTATE:
      ScsiController.sync_offset = static_cast<u8>(Value & 0x0FU);
      break;

    case REG_CLOCKFCTR:
      ScsiController.clock_factor = static_cast<u8>(Value & 0x07U);
      break;

    case REG_TESTMODE:
      // NCR test operations are not implemented.
      break;

    case REG_DATAALIGN:
      ScsiController.fifo_alignment = static_cast<u8>(Value);
      break;

    case REG_CTRL1:
      ScsiController.config1 = static_cast<u8>(Value);

      // Test mode remains latched until a controller reset.
      if ((ScsiController.config1 & 0x08U) != 0)
        ScsiController.test_mode = true;

      break;

    case REG_CTRL2:
      ScsiController.config2 = static_cast<u8>(Value);
      ScsiController.transfer_counter_mask =
        (ScsiController.config2 & NCR53CF96_CONFIG2_FEATURES_ENABLE) ? 0x00FFFFFFU : 0x0000FFFFU;
      break;

    case REG_CTRL3:
      ScsiController.config3 = static_cast<u8>(Value);
      break;

    case REG_CTRL4:
      ScsiController.config4 = static_cast<u8>(Value);
      break;

    case REG_COMMAND:
    {
      if (!KonamiGVScsiQueueCommand(static_cast<u8>(Value)))
        break;

      const u8 ActiveCommand = ScsiController.command_queue[0];

      KonamiGVScsiLoadTransferCounter((ActiveCommand & 0x80U) != 0);

      switch (ActiveCommand & 0x7FU)
      {
        case NCR53CF96_COMMAND_NOP:
          break;

        case NCR53CF96_COMMAND_FLUSH_FIFO:
          KonamiGVScsiClearFIFO();
          break;

        case NCR53CF96_COMMAND_RESET_CHIP:
          // Preserve the current legacy reset response for this checkpoint.
          // Full Reset Chip behavior will be connected separately.
          KonamiGVScsiClearFIFO();
          KonamiGVScsiAssertInterrupt();
          break;
        case 0x03:
          KonamiGVScsiSetSequenceStep(0x04);
          KonamiGVScsiAssertInterrupt();
          break;
        case NCR53CF96_COMMAND_SELECT_WITH_ATN:
          KonamiGVScsiConsumeSelectionFIFO();

          if (ScsiCommand[0] == 0 || ScsiCommand[0] == 0x48 || ScsiCommand[0] == 0x4B)
          {
            KonamiGVScsiSetSequenceStep(0x06);
          }
          else
          {
            KonamiGVScsiSetSequenceStep(0x04);
          }

          // DEBUG CODE
          KonamiGVTraceScsiCommand();

          ScsiIsRead = false;
          switch (ScsiCommand[0])
          {
            case 0x03:
            case 0x12:
            case 0x1A:
            case 0x42:
            case 0x43:
              KonamiGVScsiSetPhase(1);
              break;

            case 0x00:
            case 0x15:
              if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
              {
                std::fprintf(fp, "SCSI simple command complete cmd=%02X status_phase=0\n", ScsiCommand[0]);
                std::fclose(fp);
              }

              KonamiGVScsiSetPhase(0);
              break;

            case 0x48: // PLAY AUDIO TRACK/INDEX
            {
              const bool started =
                KonamiGVCDROMPlayAudioTrackIndex(ScsiCommand[4], ScsiCommand[5], ScsiCommand[7], ScsiCommand[8]);

              KonamiGVScsiClearStatusBits(NCR53CF96_STATUS_INTERRUPT | 0x07U);
              KonamiGVScsiSetSequenceStep(0x04U);

              if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
              {
                std::fprintf(fp, "SCSI PLAY AUDIO start=%u/%u end=%u/%u result=%s\n", ScsiCommand[4], ScsiCommand[5],
                             ScsiCommand[7], ScsiCommand[8], started ? "started" : "rejected");
                std::fclose(fp);
              }

              break;
            }

            case 0x4B: // PAUSE/RESUME
            {
              const bool resume = (ScsiCommand[8] & 0x01) != 0;

              KonamiGVCDROMPauseAudio(resume);

              KonamiGVScsiClearStatusBits(NCR53CF96_STATUS_INTERRUPT | 0x07U);
              KonamiGVScsiSetSequenceStep(0x04U);

              if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
              {
                std::fprintf(fp, "SCSI PAUSE/RESUME resume=%u\n", resume ? 1U : 0U);
                std::fclose(fp);
              }

              break;
            }

            case 0x28:
              ScsiSectorLba = (static_cast<u32>(ScsiCommand[2]) << 24) | (static_cast<u32>(ScsiCommand[3]) << 16) |
                              (static_cast<u32>(ScsiCommand[4]) << 8) | static_cast<u32>(ScsiCommand[5]);
              ScsiIsRead = true;
              KonamiGVScsiSetPhase(1);
              break;
          }

          KonamiGVScsiAssertInterrupt();
          break;

        case 0x44:
          break;
        case 0x10:
          if (Value & 0x80U)
          {
            KonamiGVScsiSetPhase(3);
            KonamiGVScsiSetSequenceStep(0x00);
            KonamiGVScsiAssertInterrupt();
            break;
          }
          [[fallthrough]];

        case 0x11:
          KonamiGVScsiAssertInterrupt();
          KonamiGVScsiClearStatusBits(NCR53CF96_STATUS_INTERRUPT | 0x07U);
          KonamiGVScsiSetSequenceStep(0x00);
          [[fallthrough]]; // ?? Why ??

        case 0x12:
          KonamiGVScsiSetSequenceStep(0x06U);
          KonamiGVScsiAssertInterrupt();
          break;

        default:
          // Log_WarningPrintf("Unknown command %02X!", value);
          break;
      }

      KonamiGVScsiCompleteCommand();
      break;
    }
  }
  if (Register != REG_STATUS && Register != REG_INTSTATE && Register != REG_IRQSTATE && Register != REG_FIFOSTATE &&
      Register != REG_CTRL1 && Register != REG_CLOCKFCTR && Register != REG_TESTMODE && Register != REG_CTRL2 &&
      Register != REG_CTRL3 && Register != REG_CTRL4 && Register != REG_DATAALIGN)
  {
    ScsiRegs[Register] = (uint8_t)Value;
  }
}