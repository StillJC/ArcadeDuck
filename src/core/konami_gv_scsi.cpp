#include "konami_gv_scsi.h"
#include "konami_gv_cdrom.h"
#include "cdrom.h"
#include "cpu_core.h"
#include "dma.h"
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
#include <vector>

// Konami GV NCR53CF96/SCSI implementation.

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
  // FIFO/CDB state.
  u8 fifo[16];
  u8 fifo_count;
  u8 identify_message;
  u8 cdb[12];
  u32 sector_lba;

  std::vector<u8> dma_buffer;
  u32 dma_buffer_offset;

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

static u8 (&ScsiFifo)[16] = ScsiController.fifo;
static u8 (&ScsiCommand)[12] = ScsiController.cdb;
static u32& ScsiSectorLba = ScsiController.sector_lba;

static constexpr u8 NCR53CF96_FIFO_CAPACITY = 16;

static constexpr u8 NCR53CF96_STATUS_VALID_GROUP_CODE = 0x08;
static constexpr u8 NCR53CF96_STATUS_TERMINAL_COUNT = 0x10;
static constexpr u8 NCR53CF96_STATUS_PARITY_ERROR = 0x20;
static constexpr u8 NCR53CF96_STATUS_GROSS_ERROR = 0x40;
static constexpr u8 NCR53CF96_STATUS_INTERRUPT = 0x80;

static constexpr u8 NCR53CF96_INTERRUPT_FUNCTION_COMPLETE = 0x08;
static constexpr u8 NCR53CF96_INTERRUPT_SCSI_RESET = 0x80;

static constexpr u8 NCR53CF96_CONFIG1_DISABLE_RESET_INTERRUPT = 0x40;

static constexpr u8 NCR53CF96_COMMAND_NOP = 0x00;
static constexpr u8 NCR53CF96_COMMAND_FLUSH_FIFO = 0x01;
static constexpr u8 NCR53CF96_COMMAND_RESET_CHIP = 0x02;
static constexpr u8 NCR53CF96_COMMAND_RESET_BUS = 0x03;
static constexpr u8 NCR53CF96_COMMAND_SELECT_WITH_ATN = 0x42;
static constexpr u8 NCR53CF96_CONFIG2_FEATURES_ENABLE = 0x08;

static constexpr u32 NCR53CF96_FAMILY_ID = 0x04;
static constexpr u32 NCR53CF96_REVISION_LEVEL = 0x02;

static constexpr u32 NCR53CF96_CLOCK_HZ = 16'000'000U;
static constexpr u32 NCR53CF96_RESET_BUS_DELAY_CYCLES = 130U;

static void KonamiGVScsiWriteStatus(u8 Value)
{
  ScsiController.status = Value;
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
  ScsiController.transfer_phase = Phase & 0x07U;

  KonamiGVScsiWriteStatus(static_cast<u8>((ScsiController.status & ~0x07U) | ScsiController.transfer_phase));
}

static void KonamiGVScsiSetDmaDirection(KonamiGVNCR53CF96DmaDirection Direction)
{
  ScsiController.dma_direction = Direction;
}

static void KonamiGVScsiSetDRQ(bool State)
{
  ScsiController.drq = State;

  // Always mirror the current NCR DRQ state to DMA5. DMA::Reset() clears its
  // request state independently, so this must not early-return on a match.
  g_dma.SetRequest(DMA::Channel::PIO, State);
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

static void KonamiGVScsiDecrementTransferCounter(u32 Count)
{
  if (!ScsiController.dma_command || Count == 0)
    return;

  if ((ScsiController.status & NCR53CF96_STATUS_TERMINAL_COUNT) != 0)
  {
    ScsiController.transfer_counter = 0;
    return;
  }

  // A loaded count of zero represents the controller's maximum transfer
  // length: 0x10000 bytes in 16-bit mode or 0x1000000 bytes in 24-bit mode.
  const u32 Remaining = (ScsiController.transfer_counter != 0) ? ScsiController.transfer_counter :
                                                                 (ScsiController.transfer_counter_mask + 1U);

  // DMA callbacks stop NCR DRQ when Terminal Count is reached.
  const u32 Transferred = std::min(Count, Remaining);

  ScsiController.transfer_counter = (Remaining - Transferred) & ScsiController.transfer_counter_mask;

  if (ScsiController.transfer_counter == 0)
    KonamiGVScsiSetStatusBits(NCR53CF96_STATUS_TERMINAL_COUNT);
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

  ScsiController.dma_buffer.clear();
  ScsiController.dma_buffer_offset = 0;

  ScsiController.identify_message = (ScsiController.fifo_count != 0) ? ScsiController.fifo[0] : 0;

  const u32 CdbSize =
    (ScsiController.fifo_count > 1) ? std::min<u32>(sizeof(ScsiController.cdb), ScsiController.fifo_count - 1) : 0;

  if (CdbSize != 0)
    std::memcpy(ScsiController.cdb, ScsiController.fifo + 1, CdbSize);

  KonamiGVScsiClearFIFO();
}

// DEBUG CODE
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
static bool ScsiBusResetPending;

static void KonamiGVScsiResetController()
{
  ScsiController = {};
  ScsiBusResetPending = false;

  // NCR53CF96 reset defaults used as the foundation for the new controller.
  ScsiController.transfer_counter_mask = 0xFFFF;
  ScsiController.clock_factor = 2;
  ScsiController.sync_period = 5;

  ScsiController.mode = KonamiGVNCR53CF96Mode::Disconnected;
  ScsiController.dma_direction = KonamiGVNCR53CF96DmaDirection::None;
}

static void KonamiGVScsiResetChip()
{
  // Destination ID and selection timeout are not changed by controller reset.
  const u8 DestinationId = ScsiController.destination_id;
  const u8 SelectionTimeout = ScsiController.selection_timeout;

  // Config 1 retains only the controller's encoded SCSI ID.
  const u8 Config1Id = ScsiController.config1 & 0x07U;

  if (ScsiIrqEvent)
    ScsiIrqEvent->Deactivate();

  ScsiBusResetPending = false;

  std::memset(ScsiController.fifo, 0, sizeof(ScsiController.fifo));
  ScsiController.fifo_count = 0;

  ScsiController.identify_message = 0;
  std::memset(ScsiController.cdb, 0, sizeof(ScsiController.cdb));
  ScsiController.sector_lba = 0;

  ScsiController.dma_buffer.clear();
  ScsiController.dma_buffer_offset = 0;

  std::memset(ScsiController.command_queue, 0, sizeof(ScsiController.command_queue));
  ScsiController.command_queue_count = 0;

  ScsiController.status = 0;
  ScsiController.interrupt_status = 0;

  ScsiController.config1 = Config1Id;
  ScsiController.config2 = 0;
  ScsiController.config3 = 0;
  ScsiController.config4 = 0;
  ScsiController.fifo_alignment = 0;
  ScsiController.test_mode = false;

  ScsiController.transfer_count = 0;
  ScsiController.transfer_counter = 0;
  ScsiController.transfer_counter_mask = 0x0000FFFFU;

  ScsiController.sequence_step = 0;
  ScsiController.clock_factor = 2;
  ScsiController.sync_period = 5;
  ScsiController.sync_offset = 0;

  ScsiController.destination_id = DestinationId;
  ScsiController.selection_timeout = SelectionTimeout;

  ScsiController.controller_state = 0;
  ScsiController.transfer_phase = 0;

  ScsiController.mode = KonamiGVNCR53CF96Mode::Disconnected;
  ScsiController.dma_direction = KonamiGVNCR53CF96DmaDirection::None;

  ScsiController.dma_command = false;
  ScsiController.irq = false;
  KonamiGVScsiSetDRQ(false);

  CPU::ClearExternalInterrupt(static_cast<u8>(InterruptController::IRQ::IRQ10));
}

static void KonamiGVScsiAssertInterrupt(u8 Cause = NCR53CF96_INTERRUPT_FUNCTION_COMPLETE);

static TickCount KonamiGVScsiGetResetBusDelayTicks()
{
  const u32 ClockFactor = (ScsiController.clock_factor != 0) ? ScsiController.clock_factor : 8U;

  const u64 DeviceClocks = static_cast<u64>(NCR53CF96_RESET_BUS_DELAY_CYCLES) * static_cast<u64>(ClockFactor);

  const u64 SystemTicks =
    ((DeviceClocks * static_cast<u64>(System::MASTER_CLOCK)) + NCR53CF96_CLOCK_HZ - 1U) / NCR53CF96_CLOCK_HZ;

  return System::ScaleTicksToOverclock(static_cast<TickCount>(SystemTicks));
}

static void KonamiGVScsiCompleteBusReset()
{
  ScsiBusResetPending = false;

  std::memset(ScsiController.command_queue, 0, sizeof(ScsiController.command_queue));
  ScsiController.command_queue_count = 0;

  ScsiController.dma_buffer.clear();
  ScsiController.dma_buffer_offset = 0;

  ScsiController.controller_state = 0;
  ScsiController.mode = KonamiGVNCR53CF96Mode::Disconnected;

  ScsiController.dma_direction = KonamiGVNCR53CF96DmaDirection::None;
  ScsiController.dma_command = false;
  KonamiGVScsiSetDRQ(false);

  KonamiGVScsiSetPhase(0);

  if ((ScsiController.config1 & NCR53CF96_CONFIG1_DISABLE_RESET_INTERRUPT) == 0)
    KonamiGVScsiAssertInterrupt(NCR53CF96_INTERRUPT_SCSI_RESET);
}

static void ScsiIrqEventCallback(void* param, TickCount ticks, TickCount ticks_late)
{
  if (ScsiIrqEvent)
    ScsiIrqEvent->Deactivate();

  if (ScsiBusResetPending)
  {
    KonamiGVScsiCompleteBusReset();
    return;
  }

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

// NCR53CF96 DMA payload handling.
//
// The PSX DMA engine owns RAM addressing, block progress, channel completion,
// and DMA interrupts. These callbacks only exchange payload bytes with the
// emulated SCSI controller and update NCR transfer state.

static void KonamiGVScsiProcessModeSelectData()
{
  const size_t ParameterLength = std::min<size_t>(ScsiController.dma_buffer.size(), ScsiCommand[4]);

  if (ParameterLength == 0)
    return;

  const u8* const ParameterData = ScsiController.dma_buffer.data();

  // DEBUG CODE
  if (std::FILE* fp = std::fopen("konami_gv_scsi_trace.txt", "ab"))
  {
    std::fprintf(fp,
                 "MODE_SELECT_PAYLOAD GAME=%s PC=0x%08X "
                 "DMA_LENGTH=%zu CDB_LENGTH=%u DATA=",
                 System::GetRunningCode().c_str(), CPU::g_state.current_instruction_pc,
                 ScsiController.dma_buffer.size(), ScsiCommand[4]);

    for (size_t i = 0; i < ParameterLength; i++)
      std::fprintf(fp, "%02X%s", ParameterData[i], (i + 1 < ParameterLength) ? " " : "");

    std::fprintf(fp, "\n");
    std::fclose(fp);
  }

  if (ParameterLength < 4)
    return;

  const size_t BlockDescriptorLength = ParameterData[3];
  size_t PageOffset = 4 + BlockDescriptorLength;

  while ((PageOffset + 2) <= ParameterLength)
  {
    const u8 PageCode = ParameterData[PageOffset] & 0x3FU;
    const size_t PageLength = ParameterData[PageOffset + 1];
    const size_t PageSize = 2 + PageLength;

    if ((PageOffset + PageSize) > ParameterLength)
      break;

    if (PageCode == 0x0E && PageLength >= 0x0E)
    {
      const size_t AudioOutputOffset = PageOffset + 16;

      if ((AudioOutputOffset + 8) > ParameterLength)
        break;

      for (u8 Output = 0; Output < 4; Output++)
      {
        const size_t OutputOffset = AudioOutputOffset + (Output * 2);

        KonamiGVCDROMSetAudioOutput(Output, ParameterData[OutputOffset], ParameterData[OutputOffset + 1]);
      }

      // DEBUG CODE
      if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
      {
        std::fprintf(fp,
                     "SCSI MODE SELECT CDDA "
                     "out0=%u/%u out1=%u/%u "
                     "out2=%u/%u out3=%u/%u\n",
                     ParameterData[AudioOutputOffset + 0] & 0x0F, ParameterData[AudioOutputOffset + 1],
                     ParameterData[AudioOutputOffset + 2] & 0x0F, ParameterData[AudioOutputOffset + 3],
                     ParameterData[AudioOutputOffset + 4] & 0x0F, ParameterData[AudioOutputOffset + 5],
                     ParameterData[AudioOutputOffset + 6] & 0x0F, ParameterData[AudioOutputOffset + 7]);

        std::fclose(fp);
      }

      break;
    }

    PageOffset += PageSize;
  }
}

static void KonamiGVScsiPrepareDmaTransfer()
{
  const u32 TransferLength = KonamiGVScsiExpectedTransferLength(ScsiCommand);

  ScsiController.dma_buffer.assign(TransferLength, 0);
  ScsiController.dma_buffer_offset = 0;

  if (ScsiController.dma_direction != KonamiGVNCR53CF96DmaDirection::DeviceToHost || TransferLength == 0)
    return;

  switch (ScsiCommand[0])
  {
    case 0x03:
    {
      // REQUEST SENSE. Return a valid fixed-format "no sense / no error" packet.
      if (ScsiController.dma_buffer.size() > 0)
        ScsiController.dma_buffer[0] = 0x70;

      if (ScsiController.dma_buffer.size() > 2)
        ScsiController.dma_buffer[2] = 0x00;

      if (ScsiController.dma_buffer.size() > 7)
        ScsiController.dma_buffer[7] = 0x0A;

      // DEBUG CODE
      if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
      {
        std::fprintf(fp, "SCSI REQUEST SENSE response length=%u\n", TransferLength);
        std::fclose(fp);
      }

      break;
    }

    case 0x12:
    {
      // INQUIRY. Return a minimal valid CD-ROM device identity.
      if (ScsiController.dma_buffer.size() > 0)
        ScsiController.dma_buffer[0] = 0x05;

      if (ScsiController.dma_buffer.size() > 1)
        ScsiController.dma_buffer[1] = 0x80;

      if (ScsiController.dma_buffer.size() > 2)
        ScsiController.dma_buffer[2] = 0x02;

      if (ScsiController.dma_buffer.size() > 3)
        ScsiController.dma_buffer[3] = 0x02;

      if (ScsiController.dma_buffer.size() > 4)
        ScsiController.dma_buffer[4] = 0x1B;

      if (ScsiController.dma_buffer.size() > 8)
      {
        const size_t CopyLength = std::min<size_t>(8, ScsiController.dma_buffer.size() - 8);
        std::memcpy(ScsiController.dma_buffer.data() + 8, "KONAMI  ", CopyLength);
      }

      if (ScsiController.dma_buffer.size() > 16)
      {
        const size_t CopyLength = std::min<size_t>(16, ScsiController.dma_buffer.size() - 16);
        std::memcpy(ScsiController.dma_buffer.data() + 16, "GV CD-ROM       ", CopyLength);
      }

      // DEBUG CODE
      if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
      {
        std::fprintf(fp, "SCSI INQUIRY response length=%u\n", TransferLength);
        std::fclose(fp);
      }

      break;
    }

    case 0x1A:
    {
      // MODE SENSE(6). Return CD Audio Control page 0x0E using
      // the GV CD-ROM layer's current output routing and volume state.
      if (ScsiController.dma_buffer.size() > 0)
        ScsiController.dma_buffer[0] = 0x1B;

      if (ScsiController.dma_buffer.size() > 4)
        ScsiController.dma_buffer[4] = 0x0E;

      if (ScsiController.dma_buffer.size() > 5)
        ScsiController.dma_buffer[5] = 0x16;

      if (ScsiController.dma_buffer.size() > 6)
        ScsiController.dma_buffer[6] = 0x04;

      for (u8 Output = 0; Output < 4; Output++)
      {
        u8 Channel = 0;
        u8 Volume = 0;

        KonamiGVCDROMGetAudioOutput(Output, &Channel, &Volume);

        const size_t OutputOffset = 20 + (Output * 2);

        if ((OutputOffset + 1) >= ScsiController.dma_buffer.size())
          break;

        ScsiController.dma_buffer[OutputOffset + 0] = Channel;
        ScsiController.dma_buffer[OutputOffset + 1] = Volume;
      }

      // DEBUG CODE
      if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
      {
        std::fprintf(fp, "SCSI MODE SENSE response length=%u\n", TransferLength);
        std::fclose(fp);
      }

      break;
    }

    case 0x28:
    {
      std::array<u8, CDImage::DATA_SECTOR_SIZE> Sector = {};

      size_t BufferOffset = 0;

      while (BufferOffset < ScsiController.dma_buffer.size())
      {
        const size_t SectorBytes =
          std::min<size_t>(CDImage::DATA_SECTOR_SIZE, ScsiController.dma_buffer.size() - BufferOffset);

        if (KonamiReadMountedDataSector(ScsiSectorLba, Sector.data()))
        {
          std::memcpy(ScsiController.dma_buffer.data() + BufferOffset, Sector.data(), SectorBytes);
        }

        ScsiSectorLba++;
        BufferOffset += SectorBytes;
      }

      break;
    }

    case 0x42:
      KonamiGVCDROMReadSubChannel(ScsiCommand, ScsiController.dma_buffer.data(), TransferLength);
      break;

    case 0x43:
    {
      const u32 ResponseLength = KonamiGVCDROMReadTOC(ScsiCommand, ScsiController.dma_buffer.data(), TransferLength);

      // DEBUG CODE
      if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
      {
        std::fprintf(fp, "SCSI READ TOC response requested=%u returned=%u\n", TransferLength, ResponseLength);
        std::fclose(fp);
      }

      break;
    }

    default:
      break;
  }
}

static void KonamiGVScsiCompleteDmaTransfer()
{
  if (ScsiController.dma_direction == KonamiGVNCR53CF96DmaDirection::HostToDevice && ScsiCommand[0] == 0x15)
    KonamiGVScsiProcessModeSelectData();

  KonamiGVScsiSetDRQ(false);
  KonamiGVScsiSetDmaDirection(KonamiGVNCR53CF96DmaDirection::None);

  KonamiGVScsiSetPhase(3);
  KonamiGVScsiSetSequenceStep(0x00U);

  ScsiController.dma_buffer.clear();
  ScsiController.dma_buffer_offset = 0;

  KonamiGVScsiAssertInterrupt();
}

void KonamiGVScsiDmaRead(u32* Data, u32 WordCount)
{
  if (WordCount == 0)
    return;

  const u32 ByteCount = WordCount * sizeof(u32);

  std::memset(Data, 0, ByteCount);

  if (ScsiController.dma_direction == KonamiGVNCR53CF96DmaDirection::DeviceToHost &&
      ScsiController.dma_buffer_offset < ScsiController.dma_buffer.size())
  {
    const size_t AvailableBytes = ScsiController.dma_buffer.size() - ScsiController.dma_buffer_offset;

    const size_t CopyBytes = std::min<size_t>(ByteCount, AvailableBytes);

    std::memcpy(Data, ScsiController.dma_buffer.data() + ScsiController.dma_buffer_offset, CopyBytes);
  }

  ScsiController.dma_buffer_offset += ByteCount;

  KonamiGVScsiDecrementTransferCounter(ByteCount);

  if ((ScsiController.status & NCR53CF96_STATUS_TERMINAL_COUNT) != 0)
    KonamiGVScsiCompleteDmaTransfer();
}

void KonamiGVScsiDmaWrite(const u32* Data, u32 WordCount)
{
  if (WordCount == 0)
    return;

  const u32 ByteCount = WordCount * sizeof(u32);

  if (ScsiController.dma_direction == KonamiGVNCR53CF96DmaDirection::HostToDevice &&
      ScsiController.dma_buffer_offset < ScsiController.dma_buffer.size())
  {
    const size_t AvailableBytes = ScsiController.dma_buffer.size() - ScsiController.dma_buffer_offset;

    const size_t CopyBytes = std::min<size_t>(ByteCount, AvailableBytes);

    std::memcpy(ScsiController.dma_buffer.data() + ScsiController.dma_buffer_offset, Data, CopyBytes);
  }

  ScsiController.dma_buffer_offset += ByteCount;

  KonamiGVScsiDecrementTransferCounter(ByteCount);

  if ((ScsiController.status & NCR53CF96_STATUS_TERMINAL_COUNT) != 0)
    KonamiGVScsiCompleteDmaTransfer();
}

// SCSI

void KonamiScsiRead(u32 Size, u32 Offset, u32& Value)
{
  const u8 Register = (Offset & 0x1F) >> 1;

  Value = 0xFFU;

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
          KonamiGVScsiResetChip();
          break;
        case NCR53CF96_COMMAND_RESET_BUS:
          ScsiBusResetPending = true;

          if (ScsiIrqEvent)
          {
            ScsiIrqEvent->Schedule(KonamiGVScsiGetResetBusDelayTicks());
          }
          else
          {
            KonamiGVScsiCompleteBusReset();
          }

          // Reset SCSI Bus remains at the front of the command queue until
          // delayed reset completion clears the queue.
          return;
        case NCR53CF96_COMMAND_SELECT_WITH_ATN:
          KonamiGVScsiConsumeSelectionFIFO();
          ScsiController.mode = KonamiGVNCR53CF96Mode::Initiator;

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

          KonamiGVScsiSetDmaDirection(KonamiGVNCR53CF96DmaDirection::None);

          switch (ScsiCommand[0])
          {
            case 0x03:
            case 0x12:
            case 0x1A:
            case 0x42:
            case 0x43:
              KonamiGVScsiSetDmaDirection(KonamiGVNCR53CF96DmaDirection::DeviceToHost);
              KonamiGVScsiSetPhase(1);
              break;

            case 0x15:
              KonamiGVScsiSetDmaDirection(KonamiGVNCR53CF96DmaDirection::HostToDevice);
              [[fallthrough]];

            case 0x00:
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

              KonamiGVScsiClearStatusBits(NCR53CF96_STATUS_INTERRUPT);
              KonamiGVScsiSetPhase(0);
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

              KonamiGVScsiClearStatusBits(NCR53CF96_STATUS_INTERRUPT);
              KonamiGVScsiSetPhase(0);
              KonamiGVScsiSetSequenceStep(0x04U);

              if (std::FILE* fp = std::fopen("konami_gv_scsi_debug.txt", "ab"))
              {
                std::fprintf(fp, "SCSI PAUSE/RESUME resume=%u\n", resume ? 1U : 0U);
                std::fclose(fp);
              }

              break;
            }

            case 0x28:
              KonamiGVScsiSetDmaDirection(KonamiGVNCR53CF96DmaDirection::DeviceToHost);
              ScsiSectorLba = (static_cast<u32>(ScsiCommand[2]) << 24) | (static_cast<u32>(ScsiCommand[3]) << 16) |
                              (static_cast<u32>(ScsiCommand[4]) << 8) | static_cast<u32>(ScsiCommand[5]);
              KonamiGVScsiSetPhase(1);
              break;
          }

          KonamiGVScsiSetDRQ(ScsiController.dma_command &&
                             ScsiController.dma_direction != KonamiGVNCR53CF96DmaDirection::None);

          KonamiGVScsiAssertInterrupt();
          break;

        case 0x44:
          break;
        case 0x10:
          if ((ActiveCommand & 0x80U) != 0)
          {
            if (ScsiController.dma_direction == KonamiGVNCR53CF96DmaDirection::None)
            {
              KonamiGVScsiSetPhase(3);
              KonamiGVScsiSetSequenceStep(0x00);
              KonamiGVScsiAssertInterrupt();
            }
            else
            {
              KonamiGVScsiPrepareDmaTransfer();

              if (ScsiController.dma_buffer.empty())
                KonamiGVScsiCompleteDmaTransfer();
              else
                KonamiGVScsiSetDRQ(true);
            }

            break;
          }
          [[fallthrough]];

        case 0x11:
          KonamiGVScsiAssertInterrupt();
          KonamiGVScsiClearStatusBits(NCR53CF96_STATUS_INTERRUPT);
          KonamiGVScsiSetPhase(0);
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
}
