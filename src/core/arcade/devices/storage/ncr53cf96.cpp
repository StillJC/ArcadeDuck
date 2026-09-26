// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/storage/ncr53cf96.h"

#include "core/system.h"
#include "core/timing_event.h"

#include <algorithm>

namespace NCR53CF96 {

ControllerEvents::ControllerEvents() = default;

ControllerEvents::~ControllerEvents() = default;

void ControllerEvents::Initialize(std::string_view reset_name, EventCallback reset_callback,
                                  std::string_view selection_name, EventCallback selection_callback,
                                  std::string_view disconnect_name, EventCallback disconnect_callback,
                                  void* callback_param)
{
  if (!m_reset_event)
    m_reset_event = std::make_unique<TimingEvent>(reset_name, 1, 1, reset_callback, callback_param);
  else
    m_reset_event->Deactivate();

  if (!m_selection_event)
    m_selection_event = std::make_unique<TimingEvent>(selection_name, 1, 1, selection_callback, callback_param);
  else
    m_selection_event->Deactivate();

  if (!m_disconnect_event)
    m_disconnect_event = std::make_unique<TimingEvent>(disconnect_name, 1, 1, disconnect_callback, callback_param);
  else
    m_disconnect_event->Deactivate();
}

void ControllerEvents::DeactivateAll()
{
  DeactivateReset();
  DeactivateSelection();
  DeactivateDisconnect();
}

void ControllerEvents::DeactivateReset()
{
  if (m_reset_event)
    m_reset_event->Deactivate();
}

void ControllerEvents::DeactivateSelection()
{
  if (m_selection_event)
    m_selection_event->Deactivate();
}

void ControllerEvents::DeactivateDisconnect()
{
  if (m_disconnect_event)
    m_disconnect_event->Deactivate();
}

bool ControllerEvents::ScheduleReset(TickCount delay)
{
  if (!m_reset_event)
    return false;

  m_reset_event->Schedule(delay);
  return true;
}

bool ControllerEvents::ScheduleSelection(TickCount delay)
{
  if (!m_selection_event)
    return false;

  m_selection_event->Schedule(delay);
  return true;
}

bool ControllerEvents::ScheduleDisconnect(TickCount delay)
{
  if (!m_disconnect_event)
    return false;

  m_disconnect_event->Schedule(delay);
  return true;
}

void ControllerEvents::RestoreSelection(bool schedule, TickCount delay)
{
  DeactivateSelection();
  if (schedule)
    ScheduleSelection(delay);
}

void ControllerEvents::RestoreDisconnect(bool schedule, TickCount delay)
{
  DeactivateDisconnect();
  if (schedule)
    ScheduleDisconnect(delay);
}


u8 NormalizeRegister(u32 offset)
{
  return static_cast<u8>((offset & 0x1f) >> 1);
}

u8 GetPhaseStatusBits(Phase phase)
{
  // Status bits 2:0 expose the SCSI MSG/C-D/I-O lines, not the internal Phase enum.
  switch (phase)
  {
    case Phase::DataOut: return 0x00;
    case Phase::DataIn: return 0x01;
    case Phase::Command: return 0x02;
    case Phase::Status: return 0x03;
    case Phase::MessageOut: return 0x06;
    case Phase::MessageIn: return 0x07;
    case Phase::BusFree: return 0x00;
    default: return 0x00;
  }
}

void SetPhase(ControllerCoreState& state, Phase phase)
{
  state.phase = phase;
  state.status = static_cast<u8>((state.status & ~0x07) | GetPhaseStatusBits(phase));
}

void ClearFIFO(ControllerCoreState& state)
{
  state.fifo_read = 0;
  state.fifo_write = 0;
  state.fifo_count = 0;
}

u8 PeekFIFO(const ControllerCoreState& state, u8 index)
{
  return state.fifo[(state.fifo_read + index) % FIFO_CAPACITY];
}

FIFOReadResult ReadFIFOData(ControllerCoreState& state, u32 width)
{
  if (state.fifo_count == 0)
    return FIFOReadResult{0, true};

  const u32 byte_count = std::min<u32>(width, sizeof(u32));
  u32 value = 0;
  for (u32 i = 0; i < byte_count && state.fifo_count != 0; i++)
  {
    value |= static_cast<u32>(state.fifo[state.fifo_read]) << (i * 8);
    state.fifo_read = static_cast<u8>((state.fifo_read + 1) % FIFO_CAPACITY);
    state.fifo_count--;
  }

  return FIFOReadResult{value, false};
}

bool WriteFIFOData(ControllerCoreState& state, u8 value)
{
  if (state.fifo_count == FIFO_CAPACITY)
    return false;

  state.fifo[state.fifo_write] = value;
  state.fifo_write = static_cast<u8>((state.fifo_write + 1) % FIFO_CAPACITY);
  state.fifo_count++;
  return true;
}

RegisterReadResult PrepareRegisterRead(const ControllerCoreState& state, u8 reg)
{
  switch (reg)
  {
    case XferCountLow:
    case XferCountMid:
    case XferCountHigh: return RegisterReadResult{RegisterReadAction::Value, ReadTransferCounter(state, reg)};
    case FIFO: return RegisterReadResult{RegisterReadAction::FIFO, 0xff};
    case Command: return RegisterReadResult{RegisterReadAction::Value, state.command};
    case Status:
      return RegisterReadResult{
        RegisterReadAction::Value,
        static_cast<u8>((state.status & ~STATUS_INTERRUPT) | (state.irq ? STATUS_INTERRUPT : 0))};
    case InterruptStatus: return RegisterReadResult{RegisterReadAction::InterruptStatus, 0xff};
    case SequenceStep: return RegisterReadResult{RegisterReadAction::Value, state.sequence_step};
    case FIFOStatus: return RegisterReadResult{RegisterReadAction::Value, static_cast<u8>(state.fifo_count & 0x1f)};
    case Config1: return RegisterReadResult{RegisterReadAction::Value, state.config1};
    case Config2: return RegisterReadResult{RegisterReadAction::Value, state.config2};
    case Config3: return RegisterReadResult{RegisterReadAction::Value, state.config3};
    case Config4: return RegisterReadResult{RegisterReadAction::Value, state.config4};
    default: return RegisterReadResult{};
  }
}

RegisterWriteAction PrepareRegisterWrite(ControllerCoreState& state, u8 reg, u8 value)
{
  switch (reg)
  {
    case XferCountLow:
    case XferCountMid:
    case XferCountHigh: return RegisterWriteAction::TransferCount;
    case FIFO: return RegisterWriteAction::FIFO;
    case Status:
      state.destination_id = value & 0x07;
      return RegisterWriteAction::None;
    case InterruptStatus:
      state.selection_timeout = value;
      return RegisterWriteAction::None;
    case SequenceStep:
      state.sync_period = value & 0x1f;
      return RegisterWriteAction::None;
    case FIFOStatus:
      state.sync_offset = value & 0x0f;
      return RegisterWriteAction::None;
    case ClockFactor:
      state.clock_factor = value & 0x07;
      return RegisterWriteAction::None;
    case TestMode: return RegisterWriteAction::None;
    case DataAlignment:
      state.fifo_alignment = value;
      return RegisterWriteAction::None;
    case Config1:
      state.config1 = value;
      state.test_mode |= (value & 0x08) != 0;
      return RegisterWriteAction::None;
    case Config2:
      state.config2 = value;
      state.transfer_counter_mask = (value & CONFIG2_FEATURES_ENABLE) ? 0x00ffffff : 0x0000ffff;
      return RegisterWriteAction::None;
    case Config3:
      state.config3 = value;
      return RegisterWriteAction::None;
    case Config4:
      state.config4 = value;
      return RegisterWriteAction::None;
    case Command: return RegisterWriteAction::Command;
    default: return RegisterWriteAction::None;
  }
}

ControllerRegisterWriteResult PrepareControllerRegisterWrite(ControllerCoreState& state, u32 offset, u32 value)
{
  ControllerRegisterWriteResult result;
  result.reg = NormalizeRegister(offset);
  result.value = static_cast<u8>(value);
  result.action = PrepareRegisterWrite(state, result.reg, result.value);
  if (result.action == RegisterWriteAction::Command)
    result.command_started = BeginCommand(state, result.value);
  return result;
}

void WriteTransferCount(ControllerCoreState& state, u8 reg, u8 value)
{
  switch (reg)
  {
    case XferCountLow: state.transfer_count = (state.transfer_count & 0x00ffff00) | value; break;
    case XferCountMid:
      state.transfer_count = (state.transfer_count & 0x00ff00ff) | (static_cast<u32>(value) << 8);
      break;
    case XferCountHigh:
      state.transfer_count = (state.transfer_count & 0x0000ffff) | (static_cast<u32>(value) << 16);
      break;
    default: break;
  }
}

u8 ReadTransferCounter(const ControllerCoreState& state, u8 reg)
{
  switch (reg)
  {
    case XferCountLow: return static_cast<u8>(state.transfer_counter);
    case XferCountMid: return static_cast<u8>(state.transfer_counter >> 8);
    case XferCountHigh: return static_cast<u8>(state.transfer_counter >> 16);
    default: return 0;
  }
}

void LoadTransferCounter(ControllerCoreState& state, bool dma_command)
{
  state.dma_command = dma_command;
  if (!dma_command)
  {
    state.transfer_counter = 0;
    return;
  }

  state.transfer_counter = state.transfer_count & state.transfer_counter_mask;
  state.status &= ~STATUS_TERMINAL_COUNT;
}

void DecrementTransferCounter(ControllerCoreState& state, u32 count)
{
  if (!state.dma_command || count == 0)
    return;

  const u32 remaining =
    state.transfer_counter != 0 ? state.transfer_counter : (state.transfer_counter_mask + 1);
  const u32 transferred = std::min(count, remaining);
  state.transfer_counter = (remaining - transferred) & state.transfer_counter_mask;
  if (state.transfer_counter == 0)
    state.status |= STATUS_TERMINAL_COUNT;
}

bool QueueCommand(ControllerCoreState& state, u8 value)
{
  const u8 command = value & 0x7f;
  if (command == COMMAND_RESET_CHIP || command == COMMAND_RESET_BUS)
    state.command_queue_count = 0;
  if (state.command_queue_count == state.command_queue.size())
  {
    state.status |= STATUS_GROSS_ERROR;
    return false;
  }
  state.command_queue[state.command_queue_count++] = value;
  return state.command_queue_count == 1;
}

void CompleteCommand(ControllerCoreState& state)
{
  if (state.command_queue_count == 0)
    return;
  state.command_queue_count--;
  if (state.command_queue_count != 0)
    state.command_queue[0] = state.command_queue[1];
}

bool BeginCommand(ControllerCoreState& state, u8 value)
{
  if (!QueueCommand(state, value))
    return false;

  state.command = state.command_queue[0];
  return true;
}

CommandAction PrepareCommand(ControllerCoreState& state)
{
  LoadTransferCounter(state, (state.command & 0x80) != 0);

  switch (state.command & 0x7f)
  {
    case COMMAND_NOP: return CommandAction::NoOperation;
    case COMMAND_FLUSH_FIFO:
      ClearFIFO(state);
      return CommandAction::NoOperation;
    case COMMAND_RESET_CHIP: return CommandAction::ResetChip;
    case COMMAND_RESET_BUS: return CommandAction::ResetBus;
    case COMMAND_SELECT_WITH_ATN: return CommandAction::SelectWithATN;
    case COMMAND_ENABLE_SELECTION_RESELECTION: return CommandAction::EnableSelectionReselection;
    case COMMAND_TRANSFER_INFORMATION: return CommandAction::TransferInformation;
    case COMMAND_INITIATOR_COMMAND_COMPLETE: return CommandAction::InitiatorCommandComplete;
    case COMMAND_MESSAGE_ACCEPTED: return CommandAction::MessageAccepted;
    default: return CommandAction::Unsupported;
  }
}

u8 GetCDBLength(u8 opcode)
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

u8 ReadInterruptStatus(ControllerCoreState& state)
{
  const u8 value = state.interrupt_status;
  state.interrupt_status = 0;
  state.status &= ~(STATUS_INTERRUPT | STATUS_GROSS_ERROR);
  state.sequence_step = 0;
  return value;
}

void PrepareInitiatorCommandComplete(ControllerCoreState& state)
{
  state.sequence_step = 0x06;
}

TickCount GetResetBusDelayTicks(const ControllerCoreState& state)
{
  const u32 clock_factor = state.clock_factor != 0 ? state.clock_factor : 8;
  const u64 device_clocks = static_cast<u64>(RESET_BUS_DELAY_CYCLES) * clock_factor;
  const u64 system_ticks =
    (device_clocks * static_cast<u64>(System::MASTER_CLOCK) + NCR_CLOCK_HZ - 1) / NCR_CLOCK_HZ;
  return System::ScaleTicksToOverclock(static_cast<TickCount>(system_ticks));
}

TickCount GetSelectionDelayTicks(const ControllerCoreState& state)
{
  const u32 clock_factor = state.clock_factor != 0 ? state.clock_factor : 8;
  const u64 device_clocks =
    (static_cast<u64>(SELECT_SCALED_DELAY_CYCLES) * clock_factor) + SELECT_UNSCALED_DELAY_CYCLES;
  const u64 system_ticks =
    (device_clocks * static_cast<u64>(System::MASTER_CLOCK) + NCR_CLOCK_HZ - 1) / NCR_CLOCK_HZ;
  return std::max<TickCount>(static_cast<TickCount>(1),
                             System::ScaleTicksToOverclock(static_cast<TickCount>(system_ticks)));
}

TickCount GetDisconnectDelayTicks(const ControllerCoreState& state)
{
  const u32 clock_factor = state.clock_factor != 0 ? state.clock_factor : 8;
  const u64 device_clocks = static_cast<u64>(DISCONNECT_DELAY_CYCLES) * clock_factor;
  const u64 system_ticks =
    (device_clocks * static_cast<u64>(System::MASTER_CLOCK) + NCR_CLOCK_HZ - 1) / NCR_CLOCK_HZ;
  return std::max<TickCount>(static_cast<TickCount>(1),
                             System::ScaleTicksToOverclock(static_cast<TickCount>(system_ticks)));
}

void DeassertIRQ(ControllerCoreState& state)
{
  state.irq = false;
}

void AssertIRQ(ControllerCoreState& state, u8 cause)
{
  state.interrupt_status |= cause;
  state.status |= STATUS_INTERRUPT;
  state.irq = true;
}

} // namespace NCR53CF96
