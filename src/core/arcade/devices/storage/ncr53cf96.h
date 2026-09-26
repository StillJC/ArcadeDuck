// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

class TimingEvent;

namespace NCR53CF96 {

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

enum class MigrationStopReason : u8
{
  None,
  FirstCDB,
  UnsupportedControllerCommand,
  IncompleteCDB,
  UnsupportedTargetCommand,
};

enum class AdapterLogEvent : u8
{
  PhaseChanged,
  FIFOUnderflow,
  FIFOOverflow,
  StatusMessageConsumed,
  TransferCountSet,
  DMARequestChanged,
  DeferredCommand,
  IRQDeasserted,
  IRQAsserted,
  DataInComplete,
  TransferInformation,
  BusService,
  SelectionCompleted,
  TargetBSYReleased,
  Disconnected,
  ResetBusCompleted,
  ResetInterruptAsserted,
  ResetInterruptSuppressed,
  Initialized,
  Reset,
  Shutdown,
  FirstRegisterAccess,
  InterruptStatusRead,
  ControllerCommand,
  ResetBusStarted,
  SelectionStarted,
  EnableSelectionReselection,
  DataOutTransferInformation,
  FunctionComplete,
  InitiatorCommandComplete,
  StatusMessageReady,
  MessageAcceptedFIFOState,
  MessageAccepted,
  ControlledStop,
  ReadProgress,
  ReadComplete,
};

enum class CommandAction : u8
{
  NoOperation,
  ResetChip,
  ResetBus,
  SelectWithATN,
  EnableSelectionReselection,
  TransferInformation,
  InitiatorCommandComplete,
  MessageAccepted,
  Unsupported,
};

enum class RegisterReadAction : u8
{
  Value,
  FIFO,
  InterruptStatus,
};

struct RegisterReadResult
{
  RegisterReadAction action = RegisterReadAction::Value;
  u8 value = 0xff;
};

enum class RegisterWriteAction : u8
{
  None,
  TransferCount,
  FIFO,
  Command,
};

struct ControllerRegisterWriteResult
{
  RegisterWriteAction action = RegisterWriteAction::None;
  u8 reg = 0;
  u8 value = 0;
  bool command_started = false;
};

enum class ControllerCommandExecutionAction : u8
{
  NoOperation,
  ResetChip,
  ResetBus,
  SelectWithATN,
  EnableSelectionReselection,
  StartDataIn,
  StartDataOut,
  FunctionComplete,
  InitiatorCommandComplete,
  MessageAccepted,
  Unsupported,
};

struct FIFOReadResult
{
  u32 value = 0;
  bool underflow = false;
};

enum class DataInTransferAction : u8
{
  RequestDMA,
  Complete,
};

struct ResponseTransferResult
{
  u32 copied_bytes = 0;
  bool first_transfer = false;
  bool complete = false;
};

struct ControllerRegisterReadResult
{
  u32 value = 0xff;
  bool interrupt_status_read = false;
};

struct RuntimeRestoreActions
{
  bool dma_request = false;
  bool schedule_selection = false;
  bool schedule_disconnect = false;
};

struct SerializedStateValues
{
  bool saved_active = false;
  u8 phase = static_cast<u8>(Phase::BusFree);
  u8 boundary_reason = static_cast<u8>(MigrationStopReason::None);
};

using EventCallback = void (*)(void* param, TickCount ticks, TickCount ticks_late);

class ControllerEvents final
{
public:
  ControllerEvents();
  ~ControllerEvents();

  ControllerEvents(const ControllerEvents&) = delete;
  ControllerEvents& operator=(const ControllerEvents&) = delete;

  void Initialize(std::string_view reset_name, EventCallback reset_callback, std::string_view selection_name,
                  EventCallback selection_callback, std::string_view disconnect_name,
                  EventCallback disconnect_callback, void* callback_param);

  void DeactivateAll();
  void DeactivateReset();
  void DeactivateSelection();
  void DeactivateDisconnect();

  bool ScheduleReset(TickCount delay);
  bool ScheduleSelection(TickCount delay);
  bool ScheduleDisconnect(TickCount delay);

  void RestoreSelection(bool schedule, TickCount delay);
  void RestoreDisconnect(bool schedule, TickCount delay);

private:
  std::unique_ptr<TimingEvent> m_reset_event;
  std::unique_ptr<TimingEvent> m_selection_event;
  std::unique_ptr<TimingEvent> m_disconnect_event;
};

inline constexpr u8 FIFO_CAPACITY = 16;
inline constexpr u8 STATUS_TERMINAL_COUNT = 0x10;
inline constexpr u8 STATUS_GROSS_ERROR = 0x40;
inline constexpr u8 STATUS_INTERRUPT = 0x80;
inline constexpr u8 COMMAND_NOP = 0x00;
inline constexpr u8 COMMAND_FLUSH_FIFO = 0x01;
inline constexpr u8 COMMAND_RESET_CHIP = 0x02;
inline constexpr u8 COMMAND_RESET_BUS = 0x03;
inline constexpr u8 COMMAND_SELECT_WITH_ATN = 0x42;
inline constexpr u8 COMMAND_ENABLE_SELECTION_RESELECTION = 0x44;
inline constexpr u8 COMMAND_INITIATOR_COMMAND_COMPLETE = 0x11;
inline constexpr u8 COMMAND_MESSAGE_ACCEPTED = 0x12;
inline constexpr u8 COMMAND_TRANSFER_INFORMATION = 0x10;
inline constexpr u8 CONFIG2_FEATURES_ENABLE = 0x08;
inline constexpr u8 CONFIG1_DISABLE_RESET_INTERRUPT = 0x40;
inline constexpr u8 INTERRUPT_FUNCTION_COMPLETE = 0x08;
inline constexpr u8 INTERRUPT_BUS_SERVICE = 0x10;
inline constexpr u8 INTERRUPT_DISCONNECTED = 0x20;
inline constexpr u8 INTERRUPT_SCSI_RESET = 0x80;
inline constexpr u8 SCSI_STATUS_GOOD = 0x00;
inline constexpr u8 SCSI_MESSAGE_COMMAND_COMPLETE = 0x00;
inline constexpr u8 SCSI_SENSE_NO_SENSE = 0x00;

inline constexpr u32 NCR_CLOCK_HZ = 16'000'000;
inline constexpr u32 RESET_BUS_DELAY_CYCLES = 130;
inline constexpr u32 DISCONNECT_DELAY_CYCLES = 1;

// MAME's NCR53C90 state machine models Select-with-ATN as asynchronous bus
// arbitration/selection. The fixed portion before target handshaking is:
// delay(11), delay(6), delay_cycles(4), delay(2), delay_cycles(2).
inline constexpr u32 SELECT_SCALED_DELAY_CYCLES = 19;
inline constexpr u32 SELECT_UNSCALED_DELAY_CYCLES = 6;

struct ControllerCoreState
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
};

template<std::size_t ResponseCapacity, std::size_t LogicalBlockSize>
struct ControllerState : ControllerCoreState
{
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
  std::array<u8, ResponseCapacity> response = {};
  u16 response_length = 0;
  u16 response_position = 0;
  u16 target_transfer_length = 0;
  bool data_in_active = false;
  bool data_out_active = false;
  bool dma_request = false;
  std::array<u8, LogicalBlockSize> sector_buffer = {};
  u32 read_lba = 0;
  u16 read_blocks_remaining = 0;
  u32 read_total_bytes = 0;
  u32 read_transferred_bytes = 0;
  u32 read_sector_offset = 0;
  bool read_active = false;
  bool sector_valid = false;
  bool boundary_requested = false;
  bool boundary_consumed = false;
  MigrationStopReason boundary_reason = MigrationStopReason::None;
  bool access_logged = false;
  bool overflow_logged = false;
  bool underflow_logged = false;
  bool deferred_command_logged = false;
  bool reset_bus_pending = false;
  bool selection_pending = false;
  u32 selection_pc = 0;
  bool initiator_connected = false;
  bool disconnect_pending = false;
};

u8 NormalizeRegister(u32 offset);
u8 GetPhaseStatusBits(Phase phase);
void SetPhase(ControllerCoreState& state, Phase phase);
void ClearFIFO(ControllerCoreState& state);
u8 PeekFIFO(const ControllerCoreState& state, u8 index);
FIFOReadResult ReadFIFOData(ControllerCoreState& state, u32 width);
bool WriteFIFOData(ControllerCoreState& state, u8 value);
RegisterReadResult PrepareRegisterRead(const ControllerCoreState& state, u8 reg);
RegisterWriteAction PrepareRegisterWrite(ControllerCoreState& state, u8 reg, u8 value);
void WriteTransferCount(ControllerCoreState& state, u8 reg, u8 value);
u8 ReadTransferCounter(const ControllerCoreState& state, u8 reg);
void LoadTransferCounter(ControllerCoreState& state, bool dma_command);
void DecrementTransferCounter(ControllerCoreState& state, u32 count);
bool QueueCommand(ControllerCoreState& state, u8 value);
void CompleteCommand(ControllerCoreState& state);
bool BeginCommand(ControllerCoreState& state, u8 value);
CommandAction PrepareCommand(ControllerCoreState& state);
u8 GetCDBLength(u8 opcode);
u8 ReadInterruptStatus(ControllerCoreState& state);
void PrepareInitiatorCommandComplete(ControllerCoreState& state);
TickCount GetResetBusDelayTicks(const ControllerCoreState& state);
TickCount GetSelectionDelayTicks(const ControllerCoreState& state);
TickCount GetDisconnectDelayTicks(const ControllerCoreState& state);
void DeassertIRQ(ControllerCoreState& state);
void AssertIRQ(ControllerCoreState& state, u8 cause);

template<typename State>
bool SetDMARequestState(State& state, bool request)
{
  const bool changed = state.dma_request != request;
  state.dma_request = request;
  return changed;
}

template<typename State, typename TransitionCallback, typename DeliveryCallback>
void SetDMARequest(State& state, bool request, TransitionCallback on_transition, DeliveryCallback deliver)
{
  if (SetDMARequestState(state, request))
    on_transition(request);

  deliver(request);
}

template<typename State, typename TransitionCallback, typename DeliveryCallback>
void DeassertControllerIRQ(State& state, TransitionCallback on_transition, DeliveryCallback deliver)
{
  if (state.irq)
    on_transition();

  DeassertIRQ(state);
  deliver(false);
}

template<typename State, typename TransitionCallback, typename DeliveryCallback>
void AssertControllerIRQ(State& state, u8 cause, TransitionCallback on_transition, DeliveryCallback deliver)
{
  const bool was_pending = state.interrupt_status != 0;
  AssertIRQ(state, cause);
  if (!was_pending)
  {
    on_transition(cause);
    deliver(true);
  }
}

template<typename State>
void CompleteDataInState(State& state)
{
  state.data_in_active = false;
  state.response.fill(0);
  state.response_length = 0;
  state.response_position = 0;
  state.target_transfer_length = 0;
  state.status_message_pending = true;
  SetPhase(state, Phase::Status);
  state.sequence_step = 0;
}

template<typename State>
DataInTransferAction PrepareDataInTransfer(const State& state)
{
  if (state.read_active)
    return DataInTransferAction::RequestDMA;

  return (!state.data_in_active || state.response_position >= state.response_length) ?
           DataInTransferAction::Complete :
           DataInTransferAction::RequestDMA;
}

template<typename State>
ResponseTransferResult ReadResponseDMA(State& state, void* data, u32 byte_count)
{
  ResponseTransferResult result;
  if (!state.data_in_active || state.response_position >= state.response_length)
    return result;

  const u16 remaining = state.response_length - state.response_position;
  result.copied_bytes = byte_count < remaining ? byte_count : remaining;
  std::memcpy(data, state.response.data() + state.response_position, result.copied_bytes);
  state.response_position = static_cast<u16>(state.response_position + result.copied_bytes);
  DecrementTransferCounter(state, byte_count);
  result.complete =
    state.response_position == state.response_length && (state.status & STATUS_TERMINAL_COUNT) != 0;
  return result;
}

template<typename State>
ResponseTransferResult WriteResponseDMA(State& state, const void* data, u32 byte_count)
{
  ResponseTransferResult result;
  if (!state.data_out_active)
    return result;

  const u16 remaining = state.response_length - state.response_position;
  result.copied_bytes = byte_count < remaining ? byte_count : remaining;
  result.first_transfer = state.response_position == 0;
  std::memcpy(state.response.data() + state.response_position, data, result.copied_bytes);
  state.response_position = static_cast<u16>(state.response_position + result.copied_bytes);
  DecrementTransferCounter(state, byte_count);
  result.complete =
    state.response_position == state.response_length && (state.status & STATUS_TERMINAL_COUNT) != 0;
  return result;
}

template<typename State>
void ResetTargetProtocolState(State& state, bool target_ready)
{
  state.cdb.fill(0);
  state.cdb_length = 0;
  state.target_ready = target_ready;
  state.target_status = SCSI_STATUS_GOOD;
  state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
  state.sense_key = SCSI_SENSE_NO_SENSE;
  state.sense_asc = 0;
  state.sense_ascq = 0;
  state.test_unit_ready_complete = false;
  state.status_message_pending = false;
  state.status_message_consumption_logged = false;
  state.response.fill(0);
  state.response_length = 0;
  state.response_position = 0;
  state.target_transfer_length = 0;
  state.data_in_active = false;
  state.data_out_active = false;
  state.dma_request = false;
  state.sector_buffer.fill(0);
  state.read_lba = 0;
  state.read_blocks_remaining = 0;
  state.read_total_bytes = 0;
  state.read_transferred_bytes = 0;
  state.read_sector_offset = 0;
  state.read_active = false;
  state.sector_valid = false;
}

template<typename State, typename LoadSector, typename OnSectorComplete>
bool ReadMediaDMA(State& state, void* data, u32 byte_count, LoadSector load_sector,
                  OnSectorComplete on_sector_complete)
{
  u8* output = static_cast<u8*>(data);
  u32 remaining = byte_count;
  while (remaining != 0 && state.read_blocks_remaining != 0)
  {
    if (!load_sector())
      break;

    const u32 available = static_cast<u32>(state.sector_buffer.size()) - state.read_sector_offset;
    const u32 copy_bytes = remaining < available ? remaining : available;
    std::memcpy(output, state.sector_buffer.data() + state.read_sector_offset, copy_bytes);
    output += copy_bytes;
    remaining -= copy_bytes;
    state.read_sector_offset += copy_bytes;
    state.read_transferred_bytes += copy_bytes;

    if (state.read_sector_offset == state.sector_buffer.size())
    {
      on_sector_complete(state.read_lba, state.read_transferred_bytes,
                         static_cast<u16>(state.read_blocks_remaining - 1));
      state.read_lba++;
      state.read_blocks_remaining--;
      state.read_sector_offset = 0;
      state.sector_valid = false;
    }
  }

  DecrementTransferCounter(state, byte_count);
  if (state.read_blocks_remaining != 0 || (state.status & STATUS_TERMINAL_COUNT) == 0)
    return false;

  state.read_active = false;
  return true;
}

template<typename State>
MigrationStopReason ConsumeMigrationStopRequestState(State& state)
{
  if (!state.boundary_requested || state.boundary_consumed)
    return MigrationStopReason::None;

  state.boundary_consumed = true;
  return state.boundary_reason;
}

inline u8 GetActiveCommand(const ControllerCoreState& state)
{
  return state.command;
}

template<typename State>
u8 GetTargetCommandOpcode(const State& state)
{
  return state.cdb[0];
}

template<typename State>
bool MarkRegisterAccess(State& state)
{
  if (state.access_logged)
    return false;

  state.access_logged = true;
  return true;
}

template<typename State, typename ReadFIFOCallback>
ControllerRegisterReadResult ReadControllerRegister(State& state, u32 width, u32 offset,
                                                    ReadFIFOCallback read_fifo)
{
  ControllerRegisterReadResult result;
  if (!state.active)
  {
    result.value = 0xffffffff;
    return result;
  }

  const RegisterReadResult prepared = PrepareRegisterRead(state, NormalizeRegister(offset));
  switch (prepared.action)
  {
    case RegisterReadAction::Value:
      result.value = prepared.value;
      break;

    case RegisterReadAction::FIFO:
      result.value = read_fifo(width);
      break;

    case RegisterReadAction::InterruptStatus:
      result.value = ReadInterruptStatus(state);
      result.interrupt_status_read = true;
      break;
  }

  return result;
}

template<typename State>
void BeginBusReset(State& state);

template<typename State>
void BeginSelection(State& state, u32 pc);

ControllerRegisterWriteResult PrepareControllerRegisterWrite(ControllerCoreState& state, u32 offset, u32 value);

template<typename State, typename WriteFIFOCallback, typename SetPhaseCallback>
ControllerCommandExecutionAction PrepareControllerCommandExecution(State& state, u32 pc,
                                                                   WriteFIFOCallback write_fifo,
                                                                   SetPhaseCallback set_phase)
{
  switch (PrepareCommand(state))
  {
    case CommandAction::NoOperation:
      return ControllerCommandExecutionAction::NoOperation;

    case CommandAction::ResetChip:
      return ControllerCommandExecutionAction::ResetChip;

    case CommandAction::ResetBus:
      BeginBusReset(state);
      return ControllerCommandExecutionAction::ResetBus;

    case CommandAction::SelectWithATN:
      BeginSelection(state, pc);
      return ControllerCommandExecutionAction::SelectWithATN;

    case CommandAction::EnableSelectionReselection:
      return ControllerCommandExecutionAction::EnableSelectionReselection;

    case CommandAction::TransferInformation:
      if ((state.command & 0x80) != 0)
      {
        if (state.data_in_active || state.read_active)
          return ControllerCommandExecutionAction::StartDataIn;

        if (state.data_out_active)
          return ControllerCommandExecutionAction::StartDataOut;

        set_phase(Phase::Status);
        state.sequence_step = 0x00;
        return ControllerCommandExecutionAction::FunctionComplete;
      }
      [[fallthrough]];

    case CommandAction::InitiatorCommandComplete:
      if (state.status_message_pending && state.fifo_count == 0)
      {
        write_fifo(state.target_status);
        write_fifo(state.target_message);
      }
      set_phase(Phase::MessageIn);
      PrepareInitiatorCommandComplete(state);
      if (state.status_message_pending)
        state.test_unit_ready_complete = false;
      return ControllerCommandExecutionAction::InitiatorCommandComplete;

    case CommandAction::MessageAccepted:
      return ControllerCommandExecutionAction::MessageAccepted;

    case CommandAction::Unsupported:
      return ControllerCommandExecutionAction::Unsupported;
  }

  return ControllerCommandExecutionAction::Unsupported;
}

template<typename State, typename DeassertIRQCallback, typename InterruptReadCallback>
void CompleteControllerRegisterRead(State& state, const ControllerRegisterReadResult& result,
                                    DeassertIRQCallback deassert_irq, InterruptReadCallback interrupt_read)
{
  if (!result.interrupt_status_read)
    return;

  deassert_irq();
  if (result.value == 0)
    return;

  CompleteCommand(state);
  interrupt_read(static_cast<u8>(result.value));
}

template<typename State>
void CompleteControllerCommandIfIdle(State& state, bool command_started)
{
  if (command_started && !state.irq && !state.dma_request)
    CompleteCommand(state);
}

template<typename State, typename FirstTransferCallback, typename CompleteCallback>
void WriteResponseControllerDMA(State& state, const u32* data, u32 word_count,
                                FirstTransferCallback first_transfer, CompleteCallback complete)
{
  if (word_count == 0 || !state.dma_request || !state.data_out_active)
    return;

  const u32 byte_count = word_count * sizeof(u32);
  const ResponseTransferResult result = WriteResponseDMA(state, data, byte_count);
  if (result.first_transfer)
    first_transfer(byte_count, result.copied_bytes);
  if (result.complete)
    complete();
}

template<typename State, typename LogCallback>
void RequestDeferredStopState(State& state, MigrationStopReason boundary_reason, LogCallback log_first_request)
{
  if (!state.deferred_command_logged)
  {
    log_first_request();
    state.deferred_command_logged = true;
  }

  state.boundary_reason = boundary_reason;
  state.boundary_requested = true;
}

template<typename State, typename SetPhaseCallback>
void CompleteDataOutState(State& state, SetPhaseCallback set_phase)
{
  state.data_out_active = false;
  state.response.fill(0);
  state.response_length = 0;
  state.response_position = 0;
  state.target_transfer_length = 0;
  state.status_message_pending = true;
  set_phase(Phase::Status);
  state.sequence_step = 0;
}

template<typename State, typename LoadSectorCallback, typename SectorCompleteCallback,
         typename MediaCompleteCallback, typename DataInCompleteCallback>
void ReadControllerDMA(State& state, u32* data, u32 word_count, LoadSectorCallback load_sector,
                       SectorCompleteCallback on_sector_complete, MediaCompleteCallback on_media_complete,
                       DataInCompleteCallback complete_data_in)
{
  if (word_count == 0)
    return;

  const u32 byte_count = word_count * sizeof(u32);
  std::memset(data, 0, byte_count);
  if (!state.dma_request)
    return;

  if (state.read_active)
  {
    const bool complete = ReadMediaDMA(state, data, byte_count, load_sector, on_sector_complete);
    if (complete)
    {
      on_media_complete();
      complete_data_in();
    }
    return;
  }

  const ResponseTransferResult result = ReadResponseDMA(state, data, byte_count);
  if (result.complete)
    complete_data_in();
}

template<typename Wrapper, typename State>
SerializedStateValues DoStateControllerPrefix(Wrapper& sw, State& state)
{
  SerializedStateValues values;
  values.saved_active = state.active;
  sw.Do(&values.saved_active);
  sw.DoBytes(state.fifo.data(), state.fifo.size());
  sw.Do(&state.fifo_read);
  sw.Do(&state.fifo_write);
  sw.Do(&state.fifo_count);
  sw.Do(&state.transfer_count);
  sw.Do(&state.transfer_counter);
  sw.Do(&state.transfer_counter_mask);
  sw.Do(&state.command);
  sw.DoBytes(state.command_queue.data(), state.command_queue.size());
  sw.Do(&state.command_queue_count);
  sw.Do(&state.status);
  sw.Do(&state.interrupt_status);
  sw.Do(&state.sequence_step);
  sw.Do(&state.config1);
  sw.Do(&state.config2);
  sw.Do(&state.config3);
  sw.Do(&state.config4);
  sw.Do(&state.clock_factor);
  sw.Do(&state.sync_period);
  sw.Do(&state.sync_offset);
  sw.Do(&state.destination_id);
  sw.Do(&state.selection_timeout);
  sw.Do(&state.fifo_alignment);
  sw.Do(&state.test_mode);
  sw.Do(&state.dma_command);
  sw.Do(&state.irq);
  values.phase = static_cast<u8>(state.phase);
  sw.Do(&values.phase);
  sw.DoBytes(state.cdb.data(), state.cdb.size());
  sw.Do(&state.cdb_length);
  sw.Do(&state.target_ready);
  sw.Do(&state.target_status);
  sw.Do(&state.target_message);
  sw.Do(&state.sense_key);
  sw.Do(&state.sense_asc);
  sw.Do(&state.sense_ascq);
  sw.Do(&state.test_unit_ready_complete);
  sw.Do(&state.status_message_pending);
  sw.Do(&state.status_message_consumption_logged);
  sw.DoBytes(state.response.data(), state.response.size());
  sw.Do(&state.response_length);
  sw.Do(&state.response_position);
  sw.Do(&state.target_transfer_length);
  sw.Do(&state.data_in_active);
  sw.Do(&state.data_out_active);
  sw.Do(&state.dma_request);
  return values;
}

template<typename Wrapper, typename State>
void DoStateReadTransfer(Wrapper& sw, State& state)
{
  sw.DoBytes(state.sector_buffer.data(), state.sector_buffer.size());
  sw.Do(&state.read_lba);
  sw.Do(&state.read_blocks_remaining);
  sw.Do(&state.read_total_bytes);
  sw.Do(&state.read_transferred_bytes);
  sw.Do(&state.read_sector_offset);
  sw.Do(&state.read_active);
}

template<typename Wrapper, typename State>
void DoStateControllerSuffix(Wrapper& sw, State& state, SerializedStateValues& values)
{
  sw.Do(&state.sector_valid);
  sw.Do(&state.boundary_requested);
  sw.Do(&state.boundary_consumed);
  values.boundary_reason = static_cast<u8>(state.boundary_reason);
  sw.Do(&values.boundary_reason);
  sw.Do(&state.access_logged);
  sw.Do(&state.overflow_logged);
  sw.Do(&state.underflow_logged);
  sw.Do(&state.deferred_command_logged);
  sw.Do(&state.reset_bus_pending);
  sw.Do(&state.selection_pending);
  sw.Do(&state.selection_pc);
  sw.Do(&state.initiator_connected);
  sw.Do(&state.disconnect_pending);
}

template<typename State>
bool IsControllerStateValid(const State& state, const SerializedStateValues& values)
{
  const bool valid_phase =
    values.phase == static_cast<u8>(Phase::BusFree) ||
    values.phase == static_cast<u8>(Phase::DataIn) ||
    values.phase == static_cast<u8>(Phase::DataOut) ||
    values.phase == static_cast<u8>(Phase::Status) ||
    values.phase == static_cast<u8>(Phase::Command) ||
    values.phase == static_cast<u8>(Phase::MessageOut) ||
    values.phase == static_cast<u8>(Phase::MessageIn);
  const bool valid_boundary_reason =
    values.boundary_reason <= static_cast<u8>(MigrationStopReason::UnsupportedTargetCommand);

  return values.saved_active && state.fifo_read < FIFO_CAPACITY && state.fifo_write < FIFO_CAPACITY &&
         state.fifo_count <= FIFO_CAPACITY && state.command_queue_count <= state.command_queue.size() &&
         state.cdb_length <= state.cdb.size() && state.response_length <= state.response.size() &&
         state.response_position <= state.response_length && state.target_transfer_length <= state.response.size() &&
         state.read_sector_offset <= state.sector_buffer.size() &&
         state.read_transferred_bytes <= state.read_total_bytes && valid_phase && valid_boundary_reason;
}

template<typename State>
void RestoreControllerStateEnums(State& state, const SerializedStateValues& values)
{
  state.phase = static_cast<Phase>(values.phase);
  state.boundary_reason = static_cast<MigrationStopReason>(values.boundary_reason);
}

template<typename State>
RuntimeRestoreActions RestoreControllerRuntimeState(State& state, const SerializedStateValues& values)
{
  RestoreControllerStateEnums(state, values);

  RuntimeRestoreActions actions;
  actions.dma_request = state.dma_request;
  actions.schedule_selection = state.selection_pending;
  actions.schedule_disconnect = state.disconnect_pending && state.initiator_connected;
  return actions;
}

template<typename State>
void ResetControllerState(State& state, bool retain_destination, bool preserve_diagnostics)
{
  const u8 destination_id = state.destination_id;
  const u8 selection_timeout = state.selection_timeout;
  const u8 config1_id = state.config1 & 0x07;
  const bool access_logged = state.access_logged;
  const bool overflow_logged = state.overflow_logged;
  const bool underflow_logged = state.underflow_logged;
  const bool deferred_command_logged = state.deferred_command_logged;
  const bool boundary_requested = state.boundary_requested;
  const bool boundary_consumed = state.boundary_consumed;

  state = {};
  state.transfer_counter_mask = 0x0000ffff;
  state.clock_factor = 2;
  state.sync_period = 5;
  state.config1 = config1_id;
  if (retain_destination)
  {
    state.destination_id = destination_id;
    state.selection_timeout = selection_timeout;
  }
  if (preserve_diagnostics)
  {
    state.access_logged = access_logged;
    state.overflow_logged = overflow_logged;
    state.underflow_logged = underflow_logged;
    state.deferred_command_logged = deferred_command_logged;
    state.boundary_requested = boundary_requested;
    state.boundary_consumed = boundary_consumed;
  }
}

template<typename State>
void BeginBusReset(State& state)
{
  state.reset_bus_pending = true;
}

template<typename State>
bool CompleteBusResetState(State& state)
{
  state.selection_pending = false;
  state.selection_pc = 0;
  state.disconnect_pending = false;
  state.initiator_connected = false;
  state.reset_bus_pending = false;
  state.dma_command = false;
  state.command = 0;
  state.command_queue_count = 0;
  ClearFIFO(state);
  return (state.config1 & CONFIG1_DISABLE_RESET_INTERRUPT) == 0;
}

template<typename State>
void BeginSelection(State& state, u32 pc)
{
  state.selection_pending = true;
  state.selection_pc = pc;
  state.sequence_step = 0;
}

template<typename State>
bool ConsumePendingSelection(State& state, u32* pc)
{
  if (!state.selection_pending)
    return false;

  *pc = state.selection_pc;
  state.selection_pending = false;
  state.selection_pc = 0;
  return true;
}

template<typename State>
bool CompleteTargetDisconnectState(State& state)
{
  if (!state.disconnect_pending || !state.initiator_connected)
    return false;

  state.disconnect_pending = false;
  state.initiator_connected = false;
  state.data_in_active = false;
  state.data_out_active = false;
  state.read_active = false;
  return true;
}

template<typename State>
bool PrepareMessageAccepted(State& state)
{
  state.status_message_pending = false;
  state.disconnect_pending = state.initiator_connected;
  return state.disconnect_pending;
}

template<typename State, typename Policy>
struct ControllerAdapterLogger
{
  static void Log(AdapterLogEvent event, const State& state, u64 arg0 = 0, u64 arg1 = 0, u64 arg2 = 0,
                  const char* text = nullptr)
  {
    switch (event)
    {
      case AdapterLogEvent::PhaseChanged:
        Policy::Detail("{}.NCR53CF96 phase canonical_set='{}' phase={} status_bits=0x{:02X}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), static_cast<u8>(arg0),
                       NCR53CF96::GetPhaseStatusBits(static_cast<Phase>(arg0)));
        break;

      case AdapterLogEvent::FIFOUnderflow:
        Policy::Warning("{}.NCR53CF96 fifo_underflow canonical_set='{}'", Policy::LOG_PREFIX,
                        Policy::GetSetName());
        break;

      case AdapterLogEvent::FIFOOverflow:
        Policy::Warning("{}.NCR53CF96 fifo_overflow canonical_set='{}'", Policy::LOG_PREFIX,
                        Policy::GetSetName());
        break;

      case AdapterLogEvent::StatusMessageConsumed:
        Policy::Detail("{}.NCR53CF96 status_message_fifo_consumed canonical_set='{}' width={} status=0x{:02X} "
                       "message=0x{:02X}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), static_cast<u32>(arg0), state.target_status,
                       state.target_message);
        break;

      case AdapterLogEvent::TransferCountSet:
        Policy::Detail("{}.NCR53CF96 transfer_count_set canonical_set='{}' count=0x{:06X}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), state.transfer_count);
        break;

      case AdapterLogEvent::DMARequestChanged:
        Policy::Detail("{}.NCR53CF96 dma5_request_{} canonical_set='{}'", Policy::LOG_PREFIX,
                       arg0 != 0 ? "asserted" : "deasserted", Policy::GetSetName());
        break;

      case AdapterLogEvent::DeferredCommand:
        Policy::Error("{}.NCR53CF96 deferred_command canonical_set='{}' reason='{}' command=0x{:02X} "
                      "phase={} pc=0x{:08X}",
                      Policy::LOG_PREFIX, Policy::GetSetName(), text, state.command,
                      static_cast<u8>(state.phase), static_cast<u32>(arg0));
        break;

      case AdapterLogEvent::IRQDeasserted:
        Policy::Detail("{}.NCR53CF96 irq10_deasserted canonical_set='{}'", Policy::LOG_PREFIX,
                       Policy::GetSetName());
        break;

      case AdapterLogEvent::IRQAsserted:
        Policy::Detail("{}.NCR53CF96 irq10_asserted canonical_set='{}' cause=0x{:02X}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), static_cast<u8>(arg0));
        break;

      case AdapterLogEvent::DataInComplete:
        Policy::Detail("{}.NCR53CF96 data_in_complete canonical_set='{}' phase={} sequence_step={}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), static_cast<u8>(state.phase),
                       state.sequence_step);
        break;

      case AdapterLogEvent::TransferInformation:
        Policy::Detail("{}.NCR53CF96 transfer_information canonical_set='{}' dma=1 phase={} "
                       "response_remaining={} transfer_count=0x{:06X}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), static_cast<u8>(state.phase),
                       state.response_length - state.response_position, state.transfer_counter);
        break;

      case AdapterLogEvent::BusService:
        Policy::Detail("{}.NCR53CF96 bus_service canonical_set='{}' cause=0x{:02X}", Policy::LOG_PREFIX,
                       Policy::GetSetName(), static_cast<u8>(arg0));
        break;

      case AdapterLogEvent::SelectionCompleted:
        Policy::Detail("{}.NCR53CF96 selection_completed canonical_set='{}'", Policy::LOG_PREFIX,
                       Policy::GetSetName());
        break;

      case AdapterLogEvent::TargetBSYReleased:
        Policy::Detail("{}.NCR53CF96 target_bsy_released canonical_set='{}'", Policy::LOG_PREFIX,
                       Policy::GetSetName());
        break;

      case AdapterLogEvent::Disconnected:
        Policy::Detail("{}.NCR53CF96 disconnected canonical_set='{}' cause=0x{:02X}", Policy::LOG_PREFIX,
                       Policy::GetSetName(), static_cast<u8>(arg0));
        break;

      case AdapterLogEvent::ResetBusCompleted:
        Policy::Info("{}.NCR53CF96 reset_bus_completed canonical_set='{}'", Policy::LOG_PREFIX,
                     Policy::GetSetName());
        break;

      case AdapterLogEvent::ResetInterruptAsserted:
        Policy::Info("{}.NCR53CF96 reset_interrupt_asserted canonical_set='{}'", Policy::LOG_PREFIX,
                     Policy::GetSetName());
        break;

      case AdapterLogEvent::ResetInterruptSuppressed:
        Policy::Info("{}.NCR53CF96 reset_interrupt_suppressed canonical_set='{}'", Policy::LOG_PREFIX,
                     Policy::GetSetName());
        break;

      case AdapterLogEvent::Initialized:
        Policy::Info("{}.NCR53CF96 initialized canonical_set='{}'", Policy::LOG_PREFIX, Policy::GetSetName());
        break;

      case AdapterLogEvent::Reset:
        Policy::Info("{}.NCR53CF96 reset canonical_set='{}'", Policy::LOG_PREFIX, Policy::GetSetName());
        break;

      case AdapterLogEvent::Shutdown:
        Policy::Info("{}.NCR53CF96 shutdown canonical_set='{}'", Policy::LOG_PREFIX, Policy::GetSetName());
        break;

      case AdapterLogEvent::FirstRegisterAccess:
        Policy::Info("{}.NCR53CF96 first_register_access canonical_set='{}'", Policy::LOG_PREFIX,
                     Policy::GetSetName());
        break;

      case AdapterLogEvent::InterruptStatusRead:
        Policy::Detail("{}.NCR53CF96 interrupt_status_read canonical_set='{}' value=0x{:02X}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), static_cast<u8>(arg0));
        break;

      case AdapterLogEvent::ControllerCommand:
        Policy::Detail("{}.NCR53CF96 command canonical_set='{}' command=0x{:02X}", Policy::LOG_PREFIX,
                       Policy::GetSetName(), state.command);
        break;

      case AdapterLogEvent::ResetBusStarted:
        Policy::Detail("{}.NCR53CF96 reset_bus_started canonical_set='{}' pc=0x{:08X} delay_ticks={}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), static_cast<u32>(arg0), arg1);
        break;

      case AdapterLogEvent::SelectionStarted:
        Policy::Detail("{}.NCR53CF96 selection_started canonical_set='{}' pc=0x{:08X} delay_ticks={}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), static_cast<u32>(arg0), arg1);
        break;

      case AdapterLogEvent::EnableSelectionReselection:
        Policy::Detail("{}.NCR53CF96 enable_selection_reselection canonical_set='{}'", Policy::LOG_PREFIX,
                       Policy::GetSetName());
        break;

      case AdapterLogEvent::DataOutTransferInformation:
        Policy::Detail("{}.NCR53CF96 data_out_transfer_information canonical_set='{}' opcode=0x{:02X} "
                       "response_remaining={} transfer_count=0x{:06X}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), state.cdb[0],
                       state.response_length - state.response_position, state.transfer_counter);
        break;

      case AdapterLogEvent::FunctionComplete:
        Policy::Detail("{}.NCR53CF96 function_complete canonical_set='{}' cause=0x{:02X}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), static_cast<u8>(arg0));
        break;

      case AdapterLogEvent::InitiatorCommandComplete:
        Policy::Detail("{}.NCR53CF96 initiator_command_complete canonical_set='{}' phase={} sequence_step={}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), static_cast<u8>(state.phase),
                       state.sequence_step);
        break;

      case AdapterLogEvent::StatusMessageReady:
        Policy::Detail("{}.NCR53CF96 status_message_ready canonical_set='{}' status=0x{:02X} "
                       "message=0x{:02X} fifo_count={}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), state.target_status, state.target_message,
                       state.fifo_count);
        break;

      case AdapterLogEvent::MessageAcceptedFIFOState:
        Policy::Detail("{}.NCR53CF96 message_accepted_fifo_state canonical_set='{}' fifo_count={} "
                       "completion_bytes_consumed={} phase={}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), state.fifo_count,
                       state.status_message_consumption_logged, static_cast<u8>(state.phase));
        break;

      case AdapterLogEvent::MessageAccepted:
        Policy::Detail("{}.NCR53CF96 message_accepted canonical_set='{}' sequence_step={} "
                       "disconnect_pending={}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), state.sequence_step,
                       state.disconnect_pending);
        break;

      case AdapterLogEvent::ControlledStop:
        Policy::Error("{}.NCR53CF96 controlled_stop canonical_set='{}' reason={}", Policy::LOG_PREFIX,
                      Policy::GetSetName(), static_cast<u8>(arg0));
        break;

      case AdapterLogEvent::ReadProgress:
        Policy::Detail("{}.NCR53CF96 read10_progress canonical_set='{}' lba={} transferred={} "
                       "remaining_blocks={}",
                       Policy::LOG_PREFIX, Policy::GetSetName(), static_cast<u32>(arg0),
                       static_cast<u32>(arg1), static_cast<u16>(arg2));
        break;

      case AdapterLogEvent::ReadComplete:
        Policy::Info("{}.NCR53CF96 read10_complete canonical_set='{}' bytes={}", Policy::LOG_PREFIX,
                     Policy::GetSetName(), state.read_transferred_bytes);
        break;
    }
  }
};

template<typename State, typename Policy>
class ControllerAdapter final
{
public:
  State& GetState() { return m_state; }
  const State& GetState() const { return m_state; }

  void DeactivateDisconnectEvent()
  {
    m_events.DeactivateDisconnect();
  }

  void SetPhase(Phase phase)
  {
    if (m_state.phase != phase)
      ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::PhaseChanged, m_state, static_cast<u64>(phase));

    NCR53CF96::SetPhase(m_state, phase);
  }

  void SetDMARequest(bool request)
  {
    NCR53CF96::SetDMARequest(
      m_state, request,
      [this](bool changed_request) {
        ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::DMARequestChanged, m_state, static_cast<u64>(changed_request));
      },
      [](bool delivered_request) { Policy::DeliverDMARequest(delivered_request); });
  }

  void RequestDeferredStop(MigrationStopReason boundary_reason, const char* reason, u32 pc)
  {
    NCR53CF96::RequestDeferredStopState(m_state, boundary_reason, [this, reason, pc]() {
      ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::DeferredCommand, m_state, pc, 0, 0, reason);
    });
  }

  void AssertIRQ(u8 cause)
  {
    NCR53CF96::AssertControllerIRQ(
      m_state, cause,
      [this](u8 interrupt_cause) {
        ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::IRQAsserted, m_state, static_cast<u64>(interrupt_cause));
      },
      [](bool state) { Policy::DeliverIRQ(state); });
  }

  void Initialize()
  {
    Policy::TargetInitialize(m_state);
    m_events.Initialize(Policy::RESET_EVENT_NAME, ResetEventCallback, Policy::SELECTION_EVENT_NAME,
                        SelectionEventCallback, Policy::DISCONNECT_EVENT_NAME, DisconnectEventCallback, this);
    ResetController();
    m_state.active = true;
    ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::Initialized, m_state);
  }

  void Reset()
  {
    if (!m_state.active)
      return;

    ResetController(false, true);
    Policy::TargetReset(m_state);
    m_state.active = true;
    ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::Reset, m_state);
  }

  void Shutdown()
  {
    m_events.DeactivateAll();
    Policy::DeliverDMARequest(false);
    DeassertIRQ();
    Policy::TargetShutdown(m_state);
    if (m_state.active)
      ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::Shutdown, m_state);
    m_state = {};
  }

  bool IsActive() const
  {
    return m_state.active;
  }

  u32 ReadRegister(u32 width, u32 offset)
  {
    if (!m_state.active)
      return 0xffffffff;

    if (NCR53CF96::MarkRegisterAccess(m_state))
      ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::FirstRegisterAccess, m_state);

    const ControllerRegisterReadResult result =
      NCR53CF96::ReadControllerRegister(m_state, width, offset, [this](u32 fifo_width) {
        return ReadFIFO(fifo_width);
      });
    NCR53CF96::CompleteControllerRegisterRead(
      m_state, result, [this]() { DeassertIRQ(); }, [this](u8 interrupt_status) {
        ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::InterruptStatusRead, m_state, static_cast<u64>(interrupt_status));
      });
    return result.value;
  }

  void WriteRegister(u32 width, u32 offset, u32 value, u32 pc)
  {
    static_cast<void>(width);
    if (!m_state.active)
      return;

    if (NCR53CF96::MarkRegisterAccess(m_state))
      ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::FirstRegisterAccess, m_state);

    const ControllerRegisterWriteResult result =
      NCR53CF96::PrepareControllerRegisterWrite(m_state, offset, value);
    switch (result.action)
    {
      case RegisterWriteAction::None:
        break;

      case RegisterWriteAction::TransferCount:
        WriteTransferCount(result.reg, result.value);
        break;

      case RegisterWriteAction::FIFO:
        WriteFIFO(result.value);
        break;

      case RegisterWriteAction::Command:
      {
        if (!result.command_started)
          break;

        ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::ControllerCommand, m_state);
        switch (NCR53CF96::PrepareControllerCommandExecution(
          m_state, pc, [this](u8 fifo_value) { WriteFIFO(fifo_value); },
          [this](Phase phase) { SetPhase(phase); }))
        {
          case ControllerCommandExecutionAction::NoOperation:
            break;

          case ControllerCommandExecutionAction::ResetChip:
            ResetController(true, true);
            m_state.active = true;
            break;

          case ControllerCommandExecutionAction::ResetBus:
          {
            const TickCount delay = NCR53CF96::GetResetBusDelayTicks(m_state);
            ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::ResetBusStarted, m_state, pc, static_cast<u64>(delay));
            if (!m_events.ScheduleReset(delay))
              CompleteBusReset();
            return;
          }

          case ControllerCommandExecutionAction::SelectWithATN:
          {
            const TickCount delay = NCR53CF96::GetSelectionDelayTicks(m_state);
            ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::SelectionStarted, m_state, pc, static_cast<u64>(delay));
            if (!m_events.ScheduleSelection(delay))
              CompleteSelection();
            return;
          }

          case ControllerCommandExecutionAction::EnableSelectionReselection:
            ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::EnableSelectionReselection, m_state);
            break;

          case ControllerCommandExecutionAction::StartDataIn:
            StartDataInTransfer();
            break;

          case ControllerCommandExecutionAction::StartDataOut:
            ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::DataOutTransferInformation, m_state);
            SetDMARequest(true);
            break;

          case ControllerCommandExecutionAction::FunctionComplete:
            ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::FunctionComplete, m_state, INTERRUPT_FUNCTION_COMPLETE);
            AssertIRQ(INTERRUPT_FUNCTION_COMPLETE);
            break;

          case ControllerCommandExecutionAction::InitiatorCommandComplete:
            ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::InitiatorCommandComplete, m_state);
            if (m_state.status_message_pending)
              ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::StatusMessageReady, m_state);
            ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::FunctionComplete, m_state, INTERRUPT_FUNCTION_COMPLETE);
            AssertIRQ(INTERRUPT_FUNCTION_COMPLETE);
            break;

          case ControllerCommandExecutionAction::MessageAccepted:
            // Release ACK for the Command Complete message. The target then releases BSY;
            // the controller reports Disconnected only after observing that bus transition.
            m_state.sequence_step = 0x02;
            ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::MessageAcceptedFIFOState, m_state);
            NCR53CF96::PrepareMessageAccepted(m_state);
            ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::MessageAccepted, m_state);
            if (m_state.disconnect_pending)
            {
              if (!m_events.ScheduleDisconnect(NCR53CF96::GetDisconnectDelayTicks(m_state)))
                CompleteTargetDisconnect();
            }
            else
            {
              // No active initiator connection means the bus is already free. Preserve the
              // old Bus Service fallback rather than fabricating a disconnect transition.
              SetPhase(Phase::BusFree);
              ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::BusService, m_state, INTERRUPT_BUS_SERVICE);
              AssertIRQ(INTERRUPT_BUS_SERVICE);
            }
            break;

          case ControllerCommandExecutionAction::Unsupported:
            RequestDeferredStop(MigrationStopReason::UnsupportedControllerCommand,
                                "unimplemented_controller_command", pc);
            break;
        }

        NCR53CF96::CompleteControllerCommandIfIdle(m_state, result.command_started);
        break;
      }
    }
  }

  MigrationStopReason ConsumeMigrationStopRequest()
  {
    const MigrationStopReason reason = NCR53CF96::ConsumeMigrationStopRequestState(m_state);
    if (reason != MigrationStopReason::None)
      ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::ControlledStop, m_state, static_cast<u64>(reason));
    return reason;
  }

  u8 GetActiveCommand() const
  {
    return NCR53CF96::GetActiveCommand(m_state);
  }

  u8 GetTargetCommandOpcode() const
  {
    return NCR53CF96::GetTargetCommandOpcode(m_state);
  }

  void DMARead(u32* data, u32 word_count)
  {
    NCR53CF96::ReadControllerDMA(
      m_state, data, word_count, [this]() { return Policy::ReadSector(m_state); },
      [this](u32 lba, u32 transferred_bytes, u16 remaining_blocks) {
        ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::ReadProgress, m_state, lba, transferred_bytes, remaining_blocks);
      },
      [this]() { ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::ReadComplete, m_state); },
      [this]() { CompleteDataIn(); });
  }

  template<typename Wrapper>
  bool DoState(Wrapper& sw)
  {
    SerializedStateValues values = NCR53CF96::DoStateControllerPrefix(sw, m_state);
    Policy::DoStateExtraPrefix(sw, m_state);
    NCR53CF96::DoStateReadTransfer(sw, m_state);
    Policy::DoStateExtraSuffix(sw, m_state);
    NCR53CF96::DoStateControllerSuffix(sw, m_state, values);

    if (sw.IsReading() &&
        (!NCR53CF96::IsControllerStateValid(m_state, values) || !Policy::ValidateExtraState(m_state)))
    {
      ResetController();
    }
    else if (sw.IsReading())
    {
      const RuntimeRestoreActions actions = NCR53CF96::RestoreControllerRuntimeState(m_state, values);
      Policy::DeliverDMARequest(actions.dma_request);
      m_events.RestoreSelection(actions.schedule_selection, NCR53CF96::GetSelectionDelayTicks(m_state));
      m_events.RestoreDisconnect(actions.schedule_disconnect, NCR53CF96::GetDisconnectDelayTicks(m_state));
    }
    return !sw.HasError();
  }

private:
  static void ResetEventCallback(void* param, TickCount, TickCount)
  {
    static_cast<ControllerAdapter*>(param)->HandleResetEvent();
  }

  static void SelectionEventCallback(void* param, TickCount, TickCount)
  {
    static_cast<ControllerAdapter*>(param)->HandleSelectionEvent();
  }

  static void DisconnectEventCallback(void* param, TickCount, TickCount)
  {
    static_cast<ControllerAdapter*>(param)->HandleDisconnectEvent();
  }

  void DeassertIRQ()
  {
    NCR53CF96::DeassertControllerIRQ(
      m_state, [this]() { ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::IRQDeasserted, m_state); },
      [](bool state) { Policy::DeliverIRQ(state); });
  }

  u32 ReadFIFO(u32 width)
  {
    const FIFOReadResult result = NCR53CF96::ReadFIFOData(m_state, width);
    if (result.underflow)
    {
      if (!m_state.underflow_logged)
      {
        ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::FIFOUnderflow, m_state);
        m_state.underflow_logged = true;
      }
      return 0;
    }

    if (m_state.status_message_pending && m_state.fifo_count == 0 &&
        !m_state.status_message_consumption_logged)
    {
      m_state.status_message_consumption_logged = true;
      ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::StatusMessageConsumed, m_state, width);
    }
    return result.value;
  }

  void WriteFIFO(u8 value)
  {
    if (!NCR53CF96::WriteFIFOData(m_state, value) && !m_state.overflow_logged)
    {
      ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::FIFOOverflow, m_state);
      m_state.overflow_logged = true;
    }
  }

  void WriteTransferCount(u8 reg, u8 value)
  {
    NCR53CF96::WriteTransferCount(m_state, reg, value);
    ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::TransferCountSet, m_state);
  }

  void CompleteDataIn()
  {
    SetDMARequest(false);
    NCR53CF96::CompleteDataInState(m_state);
    ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::DataInComplete, m_state);
    ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::BusService, m_state, INTERRUPT_BUS_SERVICE);
    AssertIRQ(INTERRUPT_BUS_SERVICE);
  }

  void StartDataInTransfer()
  {
    ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::TransferInformation, m_state);
    if (NCR53CF96::PrepareDataInTransfer(m_state) == DataInTransferAction::Complete)
      CompleteDataIn();
    else
      SetDMARequest(true);
  }

  void ResetController(bool retain_destination = false, bool preserve_diagnostics = false)
  {
    m_events.DeactivateAll();
    Policy::DeliverDMARequest(false);
    DeassertIRQ();
    NCR53CF96::ResetControllerState(m_state, retain_destination, preserve_diagnostics);
    SetPhase(Phase::BusFree);
  }

  void CompleteSelection()
  {
    u32 selection_pc;
    if (!NCR53CF96::ConsumePendingSelection(m_state, &selection_pc))
      return;

    ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::SelectionCompleted, m_state);
    Policy::ExecuteCDB(selection_pc);
  }

  void HandleSelectionEvent()
  {
    m_events.DeactivateSelection();
    if (m_state.active)
      CompleteSelection();
  }

  void CompleteTargetDisconnect()
  {
    if (!NCR53CF96::CompleteTargetDisconnectState(m_state))
      return;

    Policy::OnTargetDisconnect(m_state);
    SetDMARequest(false);
    SetPhase(Phase::BusFree);
    ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::TargetBSYReleased, m_state);
    ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::Disconnected, m_state, INTERRUPT_DISCONNECTED);
    AssertIRQ(INTERRUPT_DISCONNECTED);
  }

  void HandleDisconnectEvent()
  {
    m_events.DeactivateDisconnect();
    if (m_state.active)
      CompleteTargetDisconnect();
  }

  void CompleteBusReset()
  {
    m_events.DeactivateSelection();
    m_events.DeactivateDisconnect();
    const bool assert_reset_interrupt = NCR53CF96::CompleteBusResetState(m_state);
    Policy::ResetTargetProtocol(m_state);
    SetPhase(Phase::BusFree);
    ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::ResetBusCompleted, m_state);
    if (assert_reset_interrupt)
    {
      ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::ResetInterruptAsserted, m_state);
      AssertIRQ(INTERRUPT_SCSI_RESET);
    }
    else
    {
      ControllerAdapterLogger<State, Policy>::Log(AdapterLogEvent::ResetInterruptSuppressed, m_state);
    }
  }

  void HandleResetEvent()
  {
    m_events.DeactivateReset();
    if (m_state.active && m_state.reset_bus_pending)
      CompleteBusReset();
  }

  State m_state;
  ControllerEvents m_events;
};

} // namespace NCR53CF96
