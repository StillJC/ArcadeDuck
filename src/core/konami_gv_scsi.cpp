// SPDX-FileCopyrightText: 2026 ArcadeDuck Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "konami_gv_scsi.h"

#include "konami.h"
#include "interrupt_controller.h"
#include "system.h"
#include "timing_event.h"
#include "util/state_wrapper.h"

#include "common/log.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

Log_SetChannel(KonamiGVScsi);

namespace KonamiGVScsi {
namespace {

enum Register : u8
{
  XferCountLow = 0,
  XferCountMid,
  FIFO,
  Command,
  Status,
  InterruptStatus,
  SequenceStep,
  FIFOStatus,
  Config1,
  ClockFactor,
  TestMode,
  Config2,
  Config3,
  Config4,
  XferCountHigh,
  DataAlignment,
};

enum class Phase : u8
{
  BusFree = 0,
  DataIn = 1,
  DataOut = 2,
  Status = 3,
  Command = 4,
  MessageOut = 6,
  MessageIn = 7,
};

constexpr u8 FIFO_CAPACITY = 16;
constexpr u8 STATUS_TERMINAL_COUNT = 0x10;
constexpr u8 STATUS_GROSS_ERROR = 0x40;
constexpr u8 STATUS_INTERRUPT = 0x80;
constexpr u8 COMMAND_NOP = 0x00;
constexpr u8 COMMAND_FLUSH_FIFO = 0x01;
constexpr u8 COMMAND_RESET_CHIP = 0x02;
constexpr u8 COMMAND_RESET_BUS = 0x03;
constexpr u8 COMMAND_SELECT_WITH_ATN = 0x42;
constexpr u8 COMMAND_ENABLE_SELECTION_RESELECTION = 0x44;
constexpr u8 COMMAND_INITIATOR_COMMAND_COMPLETE = 0x11;
constexpr u8 COMMAND_MESSAGE_ACCEPTED = 0x12;
constexpr u8 CONFIG2_FEATURES_ENABLE = 0x08;
constexpr u8 CONFIG1_DISABLE_RESET_INTERRUPT = 0x40;
constexpr u8 INTERRUPT_FUNCTION_COMPLETE = 0x08;
constexpr u8 INTERRUPT_SCSI_RESET = 0x80;
constexpr u8 SCSI_STATUS_GOOD = 0x00;
constexpr u8 SCSI_MESSAGE_COMMAND_COMPLETE = 0x00;
constexpr u8 SCSI_SENSE_NO_SENSE = 0x00;
constexpr u32 NCR_CLOCK_HZ = 16'000'000;
constexpr u32 RESET_BUS_DELAY_CYCLES = 130;

struct ControllerState
{
  bool active = false;
  std::array<u8, FIFO_CAPACITY> fifo = {};
  u8 fifo_read = 0;
  u8 fifo_write = 0;
  u8 fifo_count = 0;
  u32 transfer_count = 0;
  u32 transfer_counter = 0;
  u32 transfer_counter_mask = 0x0000ffff;
  u8 command = 0;
  std::array<u8, 2> command_queue = {};
  u8 command_queue_count = 0;
  u8 status = 0;
  u8 interrupt_status = 0;
  u8 sequence_step = 0;
  u8 config1 = 0;
  u8 config2 = 0;
  u8 config3 = 0;
  u8 config4 = 0;
  u8 clock_factor = 2;
  u8 sync_period = 5;
  u8 sync_offset = 0;
  u8 destination_id = 0;
  u8 selection_timeout = 0;
  u8 fifo_alignment = 0;
  bool test_mode = false;
  bool dma_command = false;
  bool irq = false;
  Phase phase = Phase::BusFree;
  std::array<u8, 12> cdb = {};
  u8 cdb_length = 0;
  bool target_ready = false;
  u8 target_status = SCSI_STATUS_GOOD;
  u8 target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
  u8 sense_key = SCSI_SENSE_NO_SENSE;
  u8 sense_asc = 0;
  u8 sense_ascq = 0;
  bool test_unit_ready_complete = false;
  bool status_message_pending = false;
  bool status_message_consumption_logged = false;
  bool boundary_requested = false;
  bool boundary_consumed = false;
  MigrationStopReason boundary_reason = MigrationStopReason::None;
  bool access_logged = false;
  bool overflow_logged = false;
  bool underflow_logged = false;
  bool deferred_command_logged = false;
  bool reset_bus_pending = false;
};

ControllerState s_state;
std::unique_ptr<TimingEvent> s_reset_event;

static u8 NormalizeRegister(u32 offset)
{
  return static_cast<u8>((offset & 0x1f) >> 1);
}

static void SetPhase(Phase phase)
{
  if (s_state.phase != phase)
    INFO_LOG("KonamiGV.NCR53CF96 phase canonical_set='{}' phase={}", Konami::GetGVSetName(), static_cast<u8>(phase));
  s_state.phase = phase;
  s_state.status = static_cast<u8>((s_state.status & ~0x07) | (static_cast<u8>(phase) & 0x07));
}

static void ClearFIFO()
{
  s_state.fifo_read = 0;
  s_state.fifo_write = 0;
  s_state.fifo_count = 0;
}

static void ResetTargetProtocol()
{
  s_state.cdb.fill(0);
  s_state.cdb_length = 0;
  s_state.target_ready = Konami::HasValidGVDiscContent();
  s_state.target_status = SCSI_STATUS_GOOD;
  s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
  s_state.sense_key = SCSI_SENSE_NO_SENSE;
  s_state.sense_asc = 0;
  s_state.sense_ascq = 0;
  s_state.test_unit_ready_complete = false;
  s_state.status_message_pending = false;
  s_state.status_message_consumption_logged = false;
}

static u8 ReadFIFO()
{
  if (s_state.fifo_count == 0)
  {
    if (!s_state.underflow_logged)
    {
      WARNING_LOG("KonamiGV.NCR53CF96 fifo_underflow canonical_set='{}'", Konami::GetGVSetName());
      s_state.underflow_logged = true;
    }
    return 0;
  }

  const u8 value = s_state.fifo[s_state.fifo_read];
  s_state.fifo_read = static_cast<u8>((s_state.fifo_read + 1) % FIFO_CAPACITY);
  s_state.fifo_count--;
  return value;
}

static void WriteFIFO(u8 value)
{
  if (s_state.fifo_count == FIFO_CAPACITY)
  {
    if (!s_state.overflow_logged)
    {
      WARNING_LOG("KonamiGV.NCR53CF96 fifo_overflow canonical_set='{}'", Konami::GetGVSetName());
      s_state.overflow_logged = true;
    }
    return;
  }

  s_state.fifo[s_state.fifo_write] = value;
  s_state.fifo_write = static_cast<u8>((s_state.fifo_write + 1) % FIFO_CAPACITY);
  s_state.fifo_count++;
}

static u8 PeekFIFO(u8 index)
{
  return s_state.fifo[(s_state.fifo_read + index) % FIFO_CAPACITY];
}

static void WriteTransferCount(u8 reg, u8 value)
{
  switch (reg)
  {
    case XferCountLow: s_state.transfer_count = (s_state.transfer_count & 0x00ffff00) | value; break;
    case XferCountMid: s_state.transfer_count = (s_state.transfer_count & 0x00ff00ff) | (static_cast<u32>(value) << 8); break;
    case XferCountHigh: s_state.transfer_count = (s_state.transfer_count & 0x0000ffff) | (static_cast<u32>(value) << 16); break;
    default: break;
  }
  INFO_LOG("KonamiGV.NCR53CF96 transfer_count_set canonical_set='{}' count=0x{:06X}", Konami::GetGVSetName(),
           s_state.transfer_count);
}

static u8 ReadTransferCounter(u8 reg)
{
  switch (reg)
  {
    case XferCountLow: return static_cast<u8>(s_state.transfer_counter);
    case XferCountMid: return static_cast<u8>(s_state.transfer_counter >> 8);
    case XferCountHigh: return static_cast<u8>(s_state.transfer_counter >> 16);
    default: return 0;
  }
}

static void LoadTransferCounter(bool dma_command)
{
  s_state.dma_command = dma_command;
  if (!dma_command)
  {
    s_state.transfer_counter = 0;
    return;
  }

  s_state.transfer_counter = s_state.transfer_count & s_state.transfer_counter_mask;
  s_state.status &= ~STATUS_TERMINAL_COUNT;
}

static bool QueueCommand(u8 value)
{
  const u8 command = value & 0x7f;
  if (command == COMMAND_RESET_CHIP || command == COMMAND_RESET_BUS)
    s_state.command_queue_count = 0;
  if (s_state.command_queue_count == s_state.command_queue.size())
  {
    s_state.status |= STATUS_GROSS_ERROR;
    return false;
  }
  s_state.command_queue[s_state.command_queue_count++] = value;
  return s_state.command_queue_count == 1;
}

static void CompleteCommand()
{
  if (s_state.command_queue_count == 0)
    return;
  s_state.command_queue_count--;
  if (s_state.command_queue_count != 0)
    s_state.command_queue[0] = s_state.command_queue[1];
}

static u8 GetCDBLength(u8 opcode)
{
  switch ((opcode >> 5) & 0x07)
  {
    case 0: return 6;
    case 1:
    case 2: return 10;
    case 5: return 12;
    default: return 0;
  }
}

static void AssertIRQ10(u8 cause);

static void RequestDeferredStop(MigrationStopReason boundary_reason, const char* reason, u32 pc)
{
  if (!s_state.deferred_command_logged)
  {
    ERROR_LOG("KonamiGV.NCR53CF96 deferred_command canonical_set='{}' reason='{}' command=0x{:02X} phase={} pc=0x{:08X}",
              Konami::GetGVSetName(), reason, s_state.command, static_cast<u8>(s_state.phase), pc);
    s_state.deferred_command_logged = true;
  }
  s_state.boundary_reason = boundary_reason;
  s_state.boundary_requested = true;
}

static void CaptureCDB(u32 pc)
{
  if (s_state.fifo_count < 2)
  {
    s_state.status |= STATUS_GROSS_ERROR;
    RequestDeferredStop(MigrationStopReason::IncompleteCDB, "selection_without_cdb", pc);
    return;
  }

  const u8 opcode = PeekFIFO(1);
  const u8 cdb_length = GetCDBLength(opcode);
  if (cdb_length == 0 || cdb_length > s_state.cdb.size() || s_state.fifo_count < (cdb_length + 1))
  {
    s_state.status |= STATUS_GROSS_ERROR;
    RequestDeferredStop(MigrationStopReason::IncompleteCDB, "unsupported_or_incomplete_cdb", pc);
    return;
  }

  s_state.cdb.fill(0);
  for (u8 i = 0; i < cdb_length; i++)
    s_state.cdb[i] = PeekFIFO(static_cast<u8>(i + 1));
  s_state.cdb_length = cdb_length;
  ClearFIFO();
  SetPhase(Phase::Command);
  s_state.sequence_step = (opcode == 0x00 || opcode == 0x48 || opcode == 0x4b) ? 0x06 : 0x04;

  std::string bytes;
  for (u8 i = 0; i < cdb_length; i++)
  {
    char byte_text[4] = {};
    std::snprintf(byte_text, sizeof(byte_text), "%02X", s_state.cdb[i]);
    if (!bytes.empty())
      bytes += ' ';
    bytes += byte_text;
  }
  INFO_LOG("KonamiGV.NCR53CF96 cdb_captured canonical_set='{}' opcode=0x{:02X} cdb='{}' length={} pc=0x{:08X}",
           Konami::GetGVSetName(), opcode, bytes, cdb_length, pc);

  if (opcode == 0x00)
  {
    s_state.target_ready = Konami::HasValidGVDiscContent();
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    s_state.sense_key = SCSI_SENSE_NO_SENSE;
    s_state.sense_asc = 0;
    s_state.sense_ascq = 0;
    s_state.test_unit_ready_complete = true;
    s_state.status_message_pending = true;
    s_state.status_message_consumption_logged = false;
    INFO_LOG("KonamiGV.NCR53CF96 test_unit_ready_execute canonical_set='{}' ready={}", Konami::GetGVSetName(),
             s_state.target_ready);
    if (!s_state.target_ready)
    {
      ERROR_LOG("KonamiGV.NCR53CF96 test_unit_ready_not_ready canonical_set='{}'", Konami::GetGVSetName());
      RequestDeferredStop(MigrationStopReason::IncompleteCDB, "test_unit_ready_without_valid_media", pc);
      return;
    }

    // The authoritative target completes no-data commands internally: no status/message bytes are placed in the FIFO.
    SetPhase(Phase::BusFree);
    INFO_LOG("KonamiGV.NCR53CF96 target_phase canonical_set='{}' phase={}", Konami::GetGVSetName(),
             static_cast<u8>(s_state.phase));
    s_state.sequence_step = 0x06;
    INFO_LOG("KonamiGV.NCR53CF96 test_unit_ready_good canonical_set='{}' status=0x{:02X} message=0x{:02X}",
             Konami::GetGVSetName(), s_state.target_status, s_state.target_message);
    INFO_LOG("KonamiGV.NCR53CF96 select_command_completion canonical_set='{}' phase={} sequence_step={}",
             Konami::GetGVSetName(), static_cast<u8>(s_state.phase), s_state.sequence_step);
    INFO_LOG("KonamiGV.NCR53CF96 function_complete canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(),
             INTERRUPT_FUNCTION_COMPLETE);
    AssertIRQ10(INTERRUPT_FUNCTION_COMPLETE);
    return;
  }

  ERROR_LOG("KonamiGV.NCR53CF96 next_unsupported_cdb canonical_set='{}' opcode=0x{:02X} cdb='{}' length={} pc=0x{:08X} transfer_count=0x{:06X} fifo_count={} controller_command=0x{:02X} phase={} sequence_step={} target_status=0x{:02X} sense_key=0x{:02X} asc=0x{:02X} ascq=0x{:02X}",
            Konami::GetGVSetName(), opcode, bytes, cdb_length, pc, s_state.transfer_count, s_state.fifo_count,
            s_state.command, static_cast<u8>(s_state.phase), s_state.sequence_step, s_state.target_status,
            s_state.sense_key, s_state.sense_asc, s_state.sense_ascq);
  RequestDeferredStop(MigrationStopReason::UnsupportedTargetCommand, "unimplemented_target_command", pc);
}

static void DeassertIRQ10()
{
  if (s_state.irq)
    INFO_LOG("KonamiGV.NCR53CF96 irq10_deasserted canonical_set='{}'", Konami::GetGVSetName());
  s_state.irq = false;
  InterruptController::SetLineState(InterruptController::IRQ::IRQ10, false);
}

static void AssertIRQ10(u8 cause)
{
  const bool was_pending = s_state.interrupt_status != 0;
  s_state.interrupt_status |= cause;
  s_state.status |= STATUS_INTERRUPT;
  s_state.irq = true;
  if (!was_pending)
  {
    INFO_LOG("KonamiGV.NCR53CF96 irq10_asserted canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(), cause);
    InterruptController::SetLineState(InterruptController::IRQ::IRQ10, true);
  }
}

static void ResetController(bool retain_destination = false, bool preserve_diagnostics = false)
{
  const u8 destination_id = s_state.destination_id;
  const u8 selection_timeout = s_state.selection_timeout;
  const u8 config1_id = s_state.config1 & 0x07;
  const bool access_logged = s_state.access_logged;
  const bool overflow_logged = s_state.overflow_logged;
  const bool underflow_logged = s_state.underflow_logged;
  const bool deferred_command_logged = s_state.deferred_command_logged;
  const bool boundary_requested = s_state.boundary_requested;
  const bool boundary_consumed = s_state.boundary_consumed;
  if (s_reset_event)
    s_reset_event->Deactivate();
  DeassertIRQ10();
  s_state = {};
  s_state.transfer_counter_mask = 0x0000ffff;
  s_state.clock_factor = 2;
  s_state.sync_period = 5;
  s_state.config1 = config1_id;
  if (retain_destination)
  {
    s_state.destination_id = destination_id;
    s_state.selection_timeout = selection_timeout;
  }
  if (preserve_diagnostics)
  {
    s_state.access_logged = access_logged;
    s_state.overflow_logged = overflow_logged;
    s_state.underflow_logged = underflow_logged;
    s_state.deferred_command_logged = deferred_command_logged;
    s_state.boundary_requested = boundary_requested;
    s_state.boundary_consumed = boundary_consumed;
  }
  SetPhase(Phase::BusFree);
}

static TickCount GetResetBusDelayTicks()
{
  const u32 clock_factor = s_state.clock_factor != 0 ? s_state.clock_factor : 8;
  const u64 device_clocks = static_cast<u64>(RESET_BUS_DELAY_CYCLES) * clock_factor;
  const u64 system_ticks = (device_clocks * static_cast<u64>(System::MASTER_CLOCK) + NCR_CLOCK_HZ - 1) / NCR_CLOCK_HZ;
  return System::ScaleTicksToOverclock(static_cast<TickCount>(system_ticks));
}

static void CompleteBusReset()
{
  s_state.reset_bus_pending = false;
  s_state.dma_command = false;
  s_state.command = 0;
  s_state.command_queue_count = 0;
  ClearFIFO();
  ResetTargetProtocol();
  SetPhase(Phase::BusFree);
  INFO_LOG("KonamiGV.NCR53CF96 reset_bus_completed canonical_set='{}'", Konami::GetGVSetName());
  if ((s_state.config1 & CONFIG1_DISABLE_RESET_INTERRUPT) == 0)
  {
    INFO_LOG("KonamiGV.NCR53CF96 reset_interrupt_asserted canonical_set='{}'", Konami::GetGVSetName());
    AssertIRQ10(INTERRUPT_SCSI_RESET);
  }
  else
  {
    INFO_LOG("KonamiGV.NCR53CF96 reset_interrupt_suppressed canonical_set='{}'", Konami::GetGVSetName());
  }
}

static void ResetEventCallback(void*, TickCount, TickCount)
{
  if (s_reset_event)
    s_reset_event->Deactivate();
  if (s_state.active && s_state.reset_bus_pending)
    CompleteBusReset();
}

} // namespace

void Initialize()
{
  if (!s_reset_event)
    s_reset_event = std::make_unique<TimingEvent>("Konami GV NCR53CF96 Reset", 1, 1, ResetEventCallback, nullptr);
  else
    s_reset_event->Deactivate();
  ResetController();
  s_state.active = true;
  INFO_LOG("KonamiGV.NCR53CF96 initialized canonical_set='{}'", Konami::GetGVSetName());
}

void Reset()
{
  if (!s_state.active)
    return;
  ResetController(false, true);
  s_state.active = true;
  INFO_LOG("KonamiGV.NCR53CF96 reset canonical_set='{}'", Konami::GetGVSetName());
}

void Shutdown()
{
  if (s_reset_event)
    s_reset_event->Deactivate();
  DeassertIRQ10();
  if (s_state.active)
    INFO_LOG("KonamiGV.NCR53CF96 shutdown canonical_set='{}'", Konami::GetGVSetName());
  s_state = {};
}

bool IsActive()
{
  return s_state.active;
}

u32 ReadRegister(u32 width, u32 offset)
{
  static_cast<void>(width);
  if (!s_state.active)
    return 0xffffffff;
  if (!s_state.access_logged)
  {
    INFO_LOG("KonamiGV.NCR53CF96 first_register_access canonical_set='{}'", Konami::GetGVSetName());
    s_state.access_logged = true;
  }

  switch (NormalizeRegister(offset))
  {
    case XferCountLow:
    case XferCountMid:
    case XferCountHigh: return ReadTransferCounter(NormalizeRegister(offset));
    case FIFO: return ReadFIFO();
    case Command: return s_state.command;
    case Status: return static_cast<u8>((s_state.status & ~STATUS_INTERRUPT) | (s_state.irq ? STATUS_INTERRUPT : 0));
    case InterruptStatus:
    {
      const u8 value = s_state.interrupt_status;
      s_state.interrupt_status = 0;
      s_state.status &= ~(STATUS_INTERRUPT | STATUS_GROSS_ERROR);
      s_state.sequence_step = 0;
      DeassertIRQ10();
      if (value != 0)
        INFO_LOG("KonamiGV.NCR53CF96 interrupt_status_read canonical_set='{}' value=0x{:02X}", Konami::GetGVSetName(), value);
      return value;
    }
    case SequenceStep: return s_state.sequence_step;
    case FIFOStatus: return s_state.fifo_count & 0x1f;
    case Config1: return s_state.config1;
    case Config2: return s_state.config2;
    case Config3: return s_state.config3;
    case Config4: return s_state.config4;
    default: return 0xff;
  }
}

void WriteRegister(u32 width, u32 offset, u32 value, u32 pc)
{
  static_cast<void>(width);
  if (!s_state.active)
    return;
  if (!s_state.access_logged)
  {
    INFO_LOG("KonamiGV.NCR53CF96 first_register_access canonical_set='{}'", Konami::GetGVSetName());
    s_state.access_logged = true;
  }

  const u8 reg = NormalizeRegister(offset);
  const u8 byte_value = static_cast<u8>(value);
  switch (reg)
  {
    case XferCountLow:
    case XferCountMid:
    case XferCountHigh: WriteTransferCount(reg, byte_value); break;
    case FIFO: WriteFIFO(byte_value); break;
    case Status: s_state.destination_id = byte_value & 0x07; break;
    case InterruptStatus: s_state.selection_timeout = byte_value; break;
    case SequenceStep: s_state.sync_period = byte_value & 0x1f; break;
    case FIFOStatus: s_state.sync_offset = byte_value & 0x0f; break;
    case ClockFactor: s_state.clock_factor = byte_value & 0x07; break;
    case TestMode: break;
    case DataAlignment: s_state.fifo_alignment = byte_value; break;
    case Config1: s_state.config1 = byte_value; s_state.test_mode |= (byte_value & 0x08) != 0; break;
    case Config2:
      s_state.config2 = byte_value;
      s_state.transfer_counter_mask = (byte_value & CONFIG2_FEATURES_ENABLE) ? 0x00ffffff : 0x0000ffff;
      break;
    case Config3: s_state.config3 = byte_value; break;
    case Config4: s_state.config4 = byte_value; break;
    case Command:
    {
      if (!QueueCommand(byte_value))
        break;

      do
      {
        s_state.command = s_state.command_queue[0];
        const u8 command = s_state.command & 0x7f;
        INFO_LOG("KonamiGV.NCR53CF96 command canonical_set='{}' command=0x{:02X}", Konami::GetGVSetName(), s_state.command);
        LoadTransferCounter((s_state.command & 0x80) != 0);
        switch (command)
        {
          case COMMAND_NOP: break;
          case COMMAND_FLUSH_FIFO: ClearFIFO(); break;
          case COMMAND_RESET_CHIP: ResetController(true, true); s_state.active = true; break;
          case COMMAND_RESET_BUS:
            s_state.reset_bus_pending = true;
            INFO_LOG("KonamiGV.NCR53CF96 reset_bus_started canonical_set='{}' pc=0x{:08X} delay_ticks={}",
                     Konami::GetGVSetName(), pc, GetResetBusDelayTicks());
            if (s_reset_event)
              s_reset_event->Schedule(GetResetBusDelayTicks());
            else
              CompleteBusReset();
            return;
          case COMMAND_SELECT_WITH_ATN: CaptureCDB(pc); break;
          case COMMAND_ENABLE_SELECTION_RESELECTION:
            INFO_LOG("KonamiGV.NCR53CF96 enable_selection_reselection canonical_set='{}'", Konami::GetGVSetName());
            break;
          case COMMAND_INITIATOR_COMMAND_COMPLETE:
            // Source behavior: the no-data target path already consumed the status/message response.
            SetPhase(Phase::BusFree);
            s_state.sequence_step = 0x06;
            INFO_LOG("KonamiGV.NCR53CF96 initiator_command_complete canonical_set='{}' phase={} sequence_step={}",
                     Konami::GetGVSetName(), static_cast<u8>(s_state.phase), s_state.sequence_step);
            if (s_state.test_unit_ready_complete && !s_state.status_message_consumption_logged)
            {
              INFO_LOG("KonamiGV.NCR53CF96 status_message_ready canonical_set='{}' status=0x{:02X} message=0x{:02X} fifo_count={}",
                       Konami::GetGVSetName(), s_state.target_status, s_state.target_message, s_state.fifo_count);
              INFO_LOG("KonamiGV.NCR53CF96 status_message_fifo_consumed canonical_set='{}' source_preconsumed=1", Konami::GetGVSetName());
              s_state.status_message_consumption_logged = true;
              s_state.status_message_pending = false;
              s_state.test_unit_ready_complete = false;
              INFO_LOG("KonamiGV.NCR53CF96 bus_free_disconnect canonical_set='{}'", Konami::GetGVSetName());
            }
            INFO_LOG("KonamiGV.NCR53CF96 function_complete canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(),
                     INTERRUPT_FUNCTION_COMPLETE);
            AssertIRQ10(INTERRUPT_FUNCTION_COMPLETE);
            break;
          case COMMAND_MESSAGE_ACCEPTED:
            // Source behavior: Message Accepted only updates the sequence state and raises Function Complete.
            s_state.sequence_step = 0x06;
            INFO_LOG("KonamiGV.NCR53CF96 message_accepted canonical_set='{}' sequence_step={}",
                     Konami::GetGVSetName(), s_state.sequence_step);
            INFO_LOG("KonamiGV.NCR53CF96 function_complete canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(),
                     INTERRUPT_FUNCTION_COMPLETE);
            AssertIRQ10(INTERRUPT_FUNCTION_COMPLETE);
            break;
          default: RequestDeferredStop(MigrationStopReason::UnsupportedControllerCommand, "unimplemented_controller_command", pc); break;
        }
        CompleteCommand();
        if (s_state.command_queue_count != 0)
          INFO_LOG("KonamiGV.NCR53CF96 next_queued_command_started canonical_set='{}' command=0x{:02X}",
                   Konami::GetGVSetName(), s_state.command_queue[0]);
      }
      while (s_state.command_queue_count != 0);
      break;
    }
    default: break;
  }
}

MigrationStopReason ConsumeMigrationStopRequest()
{
  if (!s_state.boundary_requested || s_state.boundary_consumed)
    return MigrationStopReason::None;
  s_state.boundary_consumed = true;
  ERROR_LOG("KonamiGV.NCR53CF96 controlled_stop canonical_set='{}' reason={}", Konami::GetGVSetName(),
            static_cast<u8>(s_state.boundary_reason));
  return s_state.boundary_reason;
}

u8 GetActiveCommand()
{
  return s_state.command;
}

u8 GetTargetCommandOpcode()
{
  return s_state.cdb[0];
}

bool DoState(StateWrapper& sw)
{
  bool saved_active = s_state.active;
  sw.Do(&saved_active);
  sw.DoBytes(s_state.fifo.data(), s_state.fifo.size());
  sw.Do(&s_state.fifo_read);
  sw.Do(&s_state.fifo_write);
  sw.Do(&s_state.fifo_count);
  sw.Do(&s_state.transfer_count);
  sw.Do(&s_state.transfer_counter);
  sw.Do(&s_state.transfer_counter_mask);
  sw.Do(&s_state.command);
  sw.DoBytes(s_state.command_queue.data(), s_state.command_queue.size());
  sw.Do(&s_state.command_queue_count);
  sw.Do(&s_state.status);
  sw.Do(&s_state.interrupt_status);
  sw.Do(&s_state.sequence_step);
  sw.Do(&s_state.config1);
  sw.Do(&s_state.config2);
  sw.Do(&s_state.config3);
  sw.Do(&s_state.config4);
  sw.Do(&s_state.clock_factor);
  sw.Do(&s_state.sync_period);
  sw.Do(&s_state.sync_offset);
  sw.Do(&s_state.destination_id);
  sw.Do(&s_state.selection_timeout);
  sw.Do(&s_state.fifo_alignment);
  sw.Do(&s_state.test_mode);
  sw.Do(&s_state.dma_command);
  sw.Do(&s_state.irq);
  u8 phase = static_cast<u8>(s_state.phase);
  sw.Do(&phase);
  sw.DoBytes(s_state.cdb.data(), s_state.cdb.size());
  sw.Do(&s_state.cdb_length);
  sw.Do(&s_state.target_ready);
  sw.Do(&s_state.target_status);
  sw.Do(&s_state.target_message);
  sw.Do(&s_state.sense_key);
  sw.Do(&s_state.sense_asc);
  sw.Do(&s_state.sense_ascq);
  sw.Do(&s_state.test_unit_ready_complete);
  sw.Do(&s_state.status_message_pending);
  sw.Do(&s_state.status_message_consumption_logged);
  sw.Do(&s_state.boundary_requested);
  sw.Do(&s_state.boundary_consumed);
  u8 boundary_reason = static_cast<u8>(s_state.boundary_reason);
  sw.Do(&boundary_reason);
  sw.Do(&s_state.access_logged);
  sw.Do(&s_state.overflow_logged);
  sw.Do(&s_state.underflow_logged);
  sw.Do(&s_state.deferred_command_logged);
  sw.Do(&s_state.reset_bus_pending);
  const bool valid_phase = phase == static_cast<u8>(Phase::BusFree) || phase == static_cast<u8>(Phase::DataIn) ||
                           phase == static_cast<u8>(Phase::DataOut) || phase == static_cast<u8>(Phase::Status) ||
                           phase == static_cast<u8>(Phase::Command) || phase == static_cast<u8>(Phase::MessageOut) ||
                           phase == static_cast<u8>(Phase::MessageIn);
  const bool valid_boundary_reason = boundary_reason <= static_cast<u8>(MigrationStopReason::UnsupportedTargetCommand);
  if (sw.IsReading() && (!saved_active || s_state.fifo_read >= FIFO_CAPACITY || s_state.fifo_write >= FIFO_CAPACITY ||
                         s_state.fifo_count > FIFO_CAPACITY || s_state.command_queue_count > s_state.command_queue.size() ||
                         s_state.cdb_length > s_state.cdb.size() || !valid_phase || !valid_boundary_reason))
  {
    ResetController();
  }
  else if (sw.IsReading())
  {
    s_state.phase = static_cast<Phase>(phase);
    s_state.boundary_reason = static_cast<MigrationStopReason>(boundary_reason);
  }
  return !sw.HasError();
}

} // namespace KonamiGVScsi
