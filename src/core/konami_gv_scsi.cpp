// SPDX-FileCopyrightText: 2026 ArcadeDuck Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "konami_gv_scsi.h"

#include "konami.h"
#include "konami_gv_cdrom.h"
#include "dma.h"
#include "interrupt_controller.h"
#include "system.h"
#include "timing_event.h"
#include "util/state_wrapper.h"

#include "common/log.h"

#include <algorithm>
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
constexpr u8 COMMAND_TRANSFER_INFORMATION = 0x10;
constexpr u8 CONFIG2_FEATURES_ENABLE = 0x08;
constexpr u8 CONFIG1_DISABLE_RESET_INTERRUPT = 0x40;
constexpr u8 INTERRUPT_FUNCTION_COMPLETE = 0x08;
constexpr u8 INTERRUPT_BUS_SERVICE = 0x10;
constexpr u8 INTERRUPT_DISCONNECTED = 0x20;
constexpr u8 INTERRUPT_SCSI_RESET = 0x80;
constexpr u8 SCSI_STATUS_GOOD = 0x00;
constexpr u8 SCSI_MESSAGE_COMMAND_COMPLETE = 0x00;
constexpr u8 SCSI_SENSE_NO_SENSE = 0x00;
constexpr size_t RESPONSE_CAPACITY = 0xFF;
constexpr u32 SCSI_LOGICAL_BLOCK_SIZE = 2048;
constexpr u32 NCR_CLOCK_HZ = 16'000'000;
constexpr u32 RESET_BUS_DELAY_CYCLES = 130;
constexpr u32 DISCONNECT_DELAY_CYCLES = 1;
// MAME's NCR53C90 state machine models Select-with-ATN as asynchronous bus
// arbitration/selection. The fixed portion before target handshaking is:
// delay(11), delay(6), delay_cycles(4), delay(2), delay_cycles(2).
constexpr u32 SELECT_SCALED_DELAY_CYCLES = 19;
constexpr u32 SELECT_UNSCALED_DELAY_CYCLES = 6;

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
  std::array<u8, RESPONSE_CAPACITY> response = {};
  u16 response_length = 0;
  u16 response_position = 0;
  u16 target_transfer_length = 0;
  bool data_in_active = false;
  bool data_out_active = false;
  bool dma_request = false;
  std::array<u8, 4> audio_output_channel = {{1, 2, 0, 0}};
  std::array<u8, 4> audio_output_volume = {{0xff, 0xff, 0, 0}};
  std::array<u8, SCSI_LOGICAL_BLOCK_SIZE> sector_buffer = {};
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

ControllerState s_state;
std::unique_ptr<TimingEvent> s_reset_event;
std::unique_ptr<TimingEvent> s_selection_event;
std::unique_ptr<TimingEvent> s_disconnect_event;

static u8 NormalizeRegister(u32 offset)
{
  return static_cast<u8>((offset & 0x1f) >> 1);
}

static u8 GetPhaseStatusBits(Phase phase)
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

static void SetPhase(Phase phase)
{
  if (s_state.phase != phase)
  {
    INFO_LOG("KonamiGV.NCR53CF96 phase canonical_set='{}' phase={} status_bits=0x{:02X}",
             Konami::GetGVSetName(), static_cast<u8>(phase), GetPhaseStatusBits(phase));
  }
  s_state.phase = phase;
  s_state.status = static_cast<u8>((s_state.status & ~0x07) | GetPhaseStatusBits(phase));
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
  s_state.response.fill(0);
  s_state.response_length = 0;
  s_state.response_position = 0;
  s_state.target_transfer_length = 0;
  s_state.data_in_active = false;
  s_state.data_out_active = false;
  s_state.dma_request = false;
  s_state.sector_buffer.fill(0);
  s_state.read_lba = 0;
  s_state.read_blocks_remaining = 0;
  s_state.read_total_bytes = 0;
  s_state.read_transferred_bytes = 0;
  s_state.read_sector_offset = 0;
  s_state.read_active = false;
  s_state.sector_valid = false;
  DMA::SetRequest(DMA::Channel::PIO, false);
}

static u32 ReadFIFO(u32 width)
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

  const u32 byte_count = std::min<u32>(width, sizeof(u32));
  u32 value = 0;
  for (u32 i = 0; i < byte_count && s_state.fifo_count != 0; i++)
  {
    value |= static_cast<u32>(s_state.fifo[s_state.fifo_read]) << (i * 8);
    s_state.fifo_read = static_cast<u8>((s_state.fifo_read + 1) % FIFO_CAPACITY);
    s_state.fifo_count--;
  }

  if (s_state.status_message_pending && s_state.fifo_count == 0 && !s_state.status_message_consumption_logged)
  {
    s_state.status_message_consumption_logged = true;
    INFO_LOG("KonamiGV.NCR53CF96 status_message_fifo_consumed canonical_set='{}' width={} status=0x{:02X} message=0x{:02X}",
             Konami::GetGVSetName(), width, s_state.target_status, s_state.target_message);
  }
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

static void DecrementTransferCounter(u32 count)
{
  if (!s_state.dma_command || count == 0)
    return;

  const u32 remaining = s_state.transfer_counter != 0 ? s_state.transfer_counter : (s_state.transfer_counter_mask + 1);
  const u32 transferred = std::min(count, remaining);
  s_state.transfer_counter = (remaining - transferred) & s_state.transfer_counter_mask;
  if (s_state.transfer_counter == 0)
    s_state.status |= STATUS_TERMINAL_COUNT;
}

static void SetDMARequest(bool state)
{
  if (s_state.dma_request != state)
    INFO_LOG("KonamiGV.NCR53CF96 dma5_request_{} canonical_set='{}'", state ? "asserted" : "deasserted",
             Konami::GetGVSetName());
  s_state.dma_request = state;
  DMA::SetRequest(DMA::Channel::PIO, state);
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

static void CompleteDataOut()
{
  if (s_state.cdb[0] == 0x15 && s_state.response_length >= 4)
  {
    const size_t parameter_length = std::min<size_t>(s_state.response_length, s_state.cdb[4]);
    const size_t block_descriptor_length = s_state.response[3];
    size_t page_offset = 4 + block_descriptor_length;
    while ((page_offset + 2) <= parameter_length)
    {
      const u8 page_code = s_state.response[page_offset] & 0x3f;
      const size_t page_size = 2 + s_state.response[page_offset + 1];
      if ((page_offset + page_size) > parameter_length)
        break;
      if (page_code == 0x0e && page_size >= 24 && (page_offset + 24) <= parameter_length)
      {
        for (u8 output = 0; output < 4; output++)
        {
          const size_t output_offset = page_offset + 16 + (output * 2);
          s_state.audio_output_channel[output] = s_state.response[output_offset] & 0x0f;
          s_state.audio_output_volume[output] = s_state.response[output_offset + 1];
          KonamiGVCDROM::SetAudioOutput(output, s_state.audio_output_channel[output], s_state.audio_output_volume[output]);
        }
        INFO_LOG("KonamiGV.NCR53CF96 mode_select6_audio_control canonical_set='{}' outputs='{:02X}/{:02X} {:02X}/{:02X} {:02X}/{:02X} {:02X}/{:02X}'",
                 Konami::GetGVSetName(), s_state.audio_output_channel[0], s_state.audio_output_volume[0],
                 s_state.audio_output_channel[1], s_state.audio_output_volume[1], s_state.audio_output_channel[2],
                 s_state.audio_output_volume[2], s_state.audio_output_channel[3], s_state.audio_output_volume[3]);
        break;
      }
      page_offset += page_size;
    }
  }
  SetDMARequest(false);
  s_state.data_out_active = false;
  s_state.response.fill(0);
  s_state.response_length = 0;
  s_state.response_position = 0;
  s_state.target_transfer_length = 0;
  s_state.status_message_pending = true;
  SetPhase(Phase::Status);
  s_state.sequence_step = 0;
  INFO_LOG("KonamiGV.NCR53CF96 bus_service canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(),
           INTERRUPT_BUS_SERVICE);
  AssertIRQ10(INTERRUPT_BUS_SERVICE);
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
  s_state.initiator_connected = true;
  s_state.disconnect_pending = false;
  if (s_disconnect_event)
    s_disconnect_event->Deactivate();
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

  // The working GV PoC reports Select-with-ATN completion as Function Complete | Bus Service
  // for every target CDB, regardless of whether the target next requests data, status, or bus-free.
  constexpr u8 select_completion_cause = INTERRUPT_FUNCTION_COMPLETE | INTERRUPT_BUS_SERVICE;

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
             select_completion_cause);
    AssertIRQ10(select_completion_cause);
    return;
  }

  if (opcode == 0x03)
  {
    // The source allocates exactly the CDB allocation length, zero-fills it, then supplies fixed-format no-sense data.
    const u8 allocation_length = s_state.cdb[4];
    s_state.response.fill(0);
    s_state.response_length = std::min<u16>(allocation_length, static_cast<u16>(s_state.response.size()));
    s_state.response_position = 0;
    s_state.target_transfer_length = s_state.response_length;
    if (s_state.response_length > 0)
      s_state.response[0] = 0x70;
    if (s_state.response_length > 2)
      s_state.response[2] = SCSI_SENSE_NO_SENSE;
    if (s_state.response_length > 7)
      s_state.response[7] = 0x0a;
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    s_state.data_in_active = s_state.response_length != 0;
    s_state.status_message_pending = false;
    s_state.status_message_consumption_logged = false;
    SetPhase(Phase::DataIn);
    s_state.sequence_step = 0x04;
    INFO_LOG("KonamiGV.NCR53CF96 request_sense_execute canonical_set='{}' allocation_length={}",
             Konami::GetGVSetName(), allocation_length);
    std::string response_bytes;
    for (u16 i = 0; i < s_state.response_length; i++)
    {
      char byte_text[4] = {};
      std::snprintf(byte_text, sizeof(byte_text), "%02X", s_state.response[i]);
      if (!response_bytes.empty())
        response_bytes += ' ';
      response_bytes += byte_text;
    }
    INFO_LOG("KonamiGV.NCR53CF96 request_sense_response canonical_set='{}' length={} data='{}'", Konami::GetGVSetName(),
             s_state.response_length, response_bytes);
    INFO_LOG("KonamiGV.NCR53CF96 data_in_started canonical_set='{}' phase={} sequence_step={}",
             Konami::GetGVSetName(), static_cast<u8>(s_state.phase), s_state.sequence_step);
    INFO_LOG("KonamiGV.NCR53CF96 function_complete canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(),
             select_completion_cause);
    AssertIRQ10(select_completion_cause);
    return;
  }

  if (opcode == 0x28)
  {
    const u32 lba = (static_cast<u32>(s_state.cdb[2]) << 24) | (static_cast<u32>(s_state.cdb[3]) << 16) |
                    (static_cast<u32>(s_state.cdb[4]) << 8) | s_state.cdb[5];
    const u16 blocks = static_cast<u16>((static_cast<u16>(s_state.cdb[7]) << 8) | s_state.cdb[8]);
    s_state.read_lba = lba;
    s_state.read_blocks_remaining = blocks;
    s_state.read_total_bytes = static_cast<u32>(blocks) * SCSI_LOGICAL_BLOCK_SIZE;
    s_state.read_transferred_bytes = 0;
    s_state.read_sector_offset = 0;
    s_state.sector_valid = false;
    s_state.read_active = blocks != 0;
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    SetPhase(Phase::DataIn);
    s_state.sequence_step = 0x04;
    INFO_LOG("KonamiGV.NCR53CF96 read10_execute canonical_set='{}' lba={} blocks={} bytes={}", Konami::GetGVSetName(),
             lba, blocks, s_state.read_total_bytes);
    INFO_LOG("KonamiGV.NCR53CF96 read10_data_in_started canonical_set='{}' phase={} sequence_step={}",
             Konami::GetGVSetName(), static_cast<u8>(s_state.phase), s_state.sequence_step);
    AssertIRQ10(select_completion_cause);
    return;
  }

  if (opcode == 0x15)
  {
    const u8 lun = (s_state.cdb[1] >> 5) & 0x07;
    const u8 parameter_length = s_state.cdb[4];
    if (lun != 0 || (s_state.cdb[1] & 0x10) == 0)
    {
      RequestDeferredStop(MigrationStopReason::UnsupportedTargetCommand, "unsupported_mode_select_variant", pc);
      return;
    }
    s_state.response.fill(0);
    s_state.response_length = std::min<u16>(parameter_length, static_cast<u16>(s_state.response.size()));
    s_state.response_position = 0;
    s_state.target_transfer_length = s_state.response_length;
    s_state.data_out_active = s_state.response_length != 0;
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    s_state.status_message_pending = false;
    SetPhase(Phase::DataOut);
    s_state.sequence_step = 0x04;
    INFO_LOG("KonamiGV.NCR53CF96 mode_select6_execute canonical_set='{}' lun={} parameter_length={} status_bits=0x{:02X} cause=0x{:02X}",
             Konami::GetGVSetName(), lun, parameter_length, GetPhaseStatusBits(s_state.phase),
             select_completion_cause);
    AssertIRQ10(select_completion_cause);
    return;
  }

  if (opcode == 0x43)
  {
    const u8 format = s_state.cdb[2] & 0x0f;
    const u8 starting_track = s_state.cdb[6];
    const u16 allocation_length = static_cast<u16>((static_cast<u16>(s_state.cdb[7]) << 8) | s_state.cdb[8]);
    std::array<u8, RESPONSE_CAPACITY> full_response = {};
    const u32 full_response_length = KonamiGVCDROM::ReadTOC(s_state.cdb.data(), full_response.data(),
                                                             static_cast<u32>(full_response.size()));
    s_state.response.fill(0);
    s_state.response_length = static_cast<u16>(std::min<u32>({allocation_length, full_response_length,
                                                               static_cast<u32>(s_state.response.size())}));
    s_state.response_position = 0;
    s_state.target_transfer_length = s_state.response_length;
    if (format != 0)
    {
      ERROR_LOG("KonamiGV.NCR53CF96 read_toc_invalid_format canonical_set='{}' format={}", Konami::GetGVSetName(), format);
      RequestDeferredStop(MigrationStopReason::UnsupportedTargetCommand, "unsupported_read_toc_format", pc);
      return;
    }
    std::memcpy(s_state.response.data(), full_response.data(), s_state.response_length);
    s_state.data_in_active = s_state.response_length != 0;
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    SetPhase(Phase::DataIn);
    s_state.sequence_step = 0x04;
    std::string response_bytes;
    for (u16 i = 0; i < s_state.response_length; i++)
    {
      char text[4] = {};
      std::snprintf(text, sizeof(text), "%02X", s_state.response[i]);
      if (!response_bytes.empty()) response_bytes += ' ';
      response_bytes += text;
    }
    INFO_LOG("KonamiGV.NCR53CF96 read_toc_execute canonical_set='{}' format={} msf={} starting_track={} allocation_length={} full_response_length={} toc_data_length={} transfer_length={} adr=1 control=4 adr_control=0x14 data='{}'",
             Konami::GetGVSetName(), format, (s_state.cdb[1] & 0x02) != 0, starting_track, allocation_length, full_response_length,
             full_response_length >= 2 ? full_response_length - 2 : 0, s_state.response_length, response_bytes);
    INFO_LOG("KonamiGV.NCR53CF96 data_in_started canonical_set='{}' phase={} sequence_step={}", Konami::GetGVSetName(),
             static_cast<u8>(s_state.phase), s_state.sequence_step);
    AssertIRQ10(select_completion_cause);
    return;
  }

  if (opcode == 0x42)
  {
    const u16 allocation_length = static_cast<u16>((static_cast<u16>(s_state.cdb[7]) << 8) | s_state.cdb[8]);
    s_state.response.fill(0);
    const u32 full_length = KonamiGVCDROM::ReadSubChannel(s_state.cdb.data(), s_state.response.data(),
                                                           static_cast<u32>(s_state.response.size()));
    s_state.response_length = static_cast<u16>(std::min<u32>({allocation_length, full_length,
                                                               static_cast<u32>(s_state.response.size())}));
    s_state.response_position = 0; s_state.target_transfer_length = s_state.response_length; s_state.data_in_active = s_state.response_length != 0;
    s_state.target_status = SCSI_STATUS_GOOD; s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE; SetPhase(Phase::DataIn); s_state.sequence_step = 0x04;
    AssertIRQ10(select_completion_cause); return;
  }

  if (opcode == 0x48 || opcode == 0x4b)
  {
    const bool ok = opcode == 0x48 ? KonamiGVCDROM::PlayAudioTrackIndex(s_state.cdb[4], s_state.cdb[5], s_state.cdb[7], s_state.cdb[8]) : (KonamiGVCDROM::PauseAudio((s_state.cdb[8] & 1) != 0), true);
    s_state.target_status = SCSI_STATUS_GOOD; s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE; s_state.status_message_pending = true;
    SetPhase(Phase::BusFree);
    // The working PoC leaves PLAY AUDIO and PAUSE/RESUME at selection step 4.
    s_state.sequence_step = 0x04;
    INFO_LOG("KonamiGV.NCR53CF96 audio_select_completion canonical_set='{}' opcode=0x{:02X} cause=0x{:02X}",
             Konami::GetGVSetName(), opcode, select_completion_cause);
    AssertIRQ10(select_completion_cause);
    if (!ok) WARNING_LOG("KonamiGV.NCR53CF96 audio_command_rejected opcode=0x{:02X}", opcode);
    return;
  }

  if (opcode == 0x1a)
  {
    const bool dbd = (s_state.cdb[1] & 0x08) != 0;
    const u8 page_control = s_state.cdb[2] >> 6;
    const u8 page_code = s_state.cdb[2] & 0x3f;
    const u8 allocation_length = s_state.cdb[4];
    s_state.response.fill(0);
    s_state.response_length = static_cast<u16>(std::min<u32>(allocation_length, static_cast<u32>(s_state.response.size())));
    s_state.response_position = 0;
    s_state.target_transfer_length = s_state.response_length;
    if (page_control != 0 || page_code != 0x0e)
    {
      ERROR_LOG("KonamiGV.NCR53CF96 mode_sense6_invalid canonical_set='{}' page_control={} page_code=0x{:02X}",
                Konami::GetGVSetName(), page_control, page_code);
      RequestDeferredStop(MigrationStopReason::UnsupportedTargetCommand, "unsupported_mode_sense_page", pc);
      return;
    }
    if (s_state.response_length > 0) s_state.response[0] = 0x1b;
    if (s_state.response_length > 4) s_state.response[4] = 0x0e;
    if (s_state.response_length > 5) s_state.response[5] = 0x16;
    if (s_state.response_length > 6) s_state.response[6] = 0x04;
    for (u32 output = 0; output < s_state.audio_output_channel.size() && (20 + (output * 2) + 1) < s_state.response_length;
         output++)
    {
      s_state.response[20 + (output * 2)] = s_state.audio_output_channel[output];
      s_state.response[21 + (output * 2)] = s_state.audio_output_volume[output];
    }
    s_state.data_in_active = s_state.response_length != 0;
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    SetPhase(Phase::DataIn);
    s_state.sequence_step = 0x04;
    std::string response_bytes;
    for (u16 i = 0; i < s_state.response_length; i++)
    {
      char text[4] = {};
      std::snprintf(text, sizeof(text), "%02X", s_state.response[i]);
      if (!response_bytes.empty()) response_bytes += ' ';
      response_bytes += text;
    }
    INFO_LOG("KonamiGV.NCR53CF96 mode_sense6_execute canonical_set='{}' dbd={} page_control={} page_code=0x{:02X} allocation_length={} full_response_length={} transfer_length={} header='{:02X} {:02X} {:02X} {:02X}' page='0E 16 04' ports='01 FF 02 FF 00 00 00 00' data='{}'",
             Konami::GetGVSetName(), dbd, page_control, page_code, allocation_length, allocation_length,
             s_state.response_length, s_state.response[0], s_state.response[1], s_state.response[2], s_state.response[3],
             response_bytes);
    AssertIRQ10(select_completion_cause);
    return;
  }

  if (opcode == 0x12)
  {
    const u8 lun = (s_state.cdb[1] >> 5) & 0x07;
    const bool evpd = (s_state.cdb[1] & 0x01) != 0;
    const u8 page_code = s_state.cdb[2];
    const u8 allocation_length = s_state.cdb[4];
    if (lun != 0 || evpd || page_code != 0)
    {
      ERROR_LOG("KonamiGV.NCR53CF96 inquiry_invalid canonical_set='{}' lun={} evpd={} page=0x{:02X}",
                Konami::GetGVSetName(), lun, evpd, page_code);
      RequestDeferredStop(MigrationStopReason::UnsupportedTargetCommand, "unsupported_inquiry_variant", pc);
      return;
    }
    static constexpr std::array<u8, 36> inquiry = {
      0x05, 0x80, 0x02, 0x02, 0x20, 0x00, 0x00, 0x98,
      'T', 'O', 'S', 'H', 'I', 'B', 'A', ' ',
      'C', 'D', '-', 'R', 'O', 'M', ' ', 'X', 'M', '-', '5', '4', '0', '1', 'T', 'A',
      '3', '6', '0', '5'};
    s_state.response.fill(0);
    s_state.response_length = static_cast<u16>(std::min<u32>(allocation_length, static_cast<u32>(inquiry.size())));
    std::memcpy(s_state.response.data(), inquiry.data(), s_state.response_length);
    s_state.response_position = 0;
    s_state.target_transfer_length = s_state.response_length;
    s_state.data_in_active = s_state.response_length != 0;
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    SetPhase(Phase::DataIn);
    s_state.sequence_step = 0x04;
    std::string response_bytes;
    for (u16 i = 0; i < s_state.response_length; i++)
    {
      char text[4] = {};
      std::snprintf(text, sizeof(text), "%02X", s_state.response[i]);
      if (!response_bytes.empty()) response_bytes += ' ';
      response_bytes += text;
    }
    INFO_LOG("KonamiGV.NCR53CF96 inquiry_execute canonical_set='{}' lun={} evpd={} page=0x{:02X} allocation_length={} full_response_length={} transfer_length={} vendor='TOSHIBA ' product='CD-ROM XM-5401TA' revision='3605' data='{}'",
             Konami::GetGVSetName(), lun, evpd, page_code, allocation_length, inquiry.size(), s_state.response_length,
             response_bytes);
    AssertIRQ10(select_completion_cause);
    return;
  }

  ERROR_LOG("KonamiGV.NCR53CF96 next_unsupported_cdb canonical_set='{}' opcode=0x{:02X} cdb='{}' length={} pc=0x{:08X} transfer_count=0x{:06X} fifo_count={} response_position={} response_remaining={} controller_command=0x{:02X} phase={} sequence_step={} target_status=0x{:02X} sense_key=0x{:02X} asc=0x{:02X} ascq=0x{:02X} dma_request={}",
            Konami::GetGVSetName(), opcode, bytes, cdb_length, pc, s_state.transfer_count, s_state.fifo_count,
            s_state.response_position, s_state.response_length - s_state.response_position, s_state.command,
            static_cast<u8>(s_state.phase), s_state.sequence_step, s_state.target_status, s_state.sense_key,
            s_state.sense_asc, s_state.sense_ascq, s_state.dma_request);
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

static void CompleteDataIn()
{
  SetDMARequest(false);
  s_state.data_in_active = false;
  s_state.response.fill(0);
  s_state.response_length = 0;
  s_state.response_position = 0;
  s_state.target_transfer_length = 0;
  s_state.status_message_pending = true;
  SetPhase(Phase::Status);
  s_state.sequence_step = 0x00;
  INFO_LOG("KonamiGV.NCR53CF96 data_in_complete canonical_set='{}' phase={} sequence_step={}",
           Konami::GetGVSetName(), static_cast<u8>(s_state.phase), s_state.sequence_step);
  INFO_LOG("KonamiGV.NCR53CF96 bus_service canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(),
           INTERRUPT_BUS_SERVICE);
  AssertIRQ10(INTERRUPT_BUS_SERVICE);
}

static void StartDataInTransfer()
{
  INFO_LOG("KonamiGV.NCR53CF96 transfer_information canonical_set='{}' dma=1 phase={} response_remaining={} transfer_count=0x{:06X}",
           Konami::GetGVSetName(), static_cast<u8>(s_state.phase), s_state.response_length - s_state.response_position,
           s_state.transfer_counter);
  if (s_state.read_active)
  {
    SetDMARequest(true);
    return;
  }
  if (!s_state.data_in_active || s_state.response_position >= s_state.response_length)
  {
    CompleteDataIn();
    return;
  }

  SetDMARequest(true);
}

static bool LoadReadSector()
{
  if (s_state.sector_valid || s_state.read_blocks_remaining == 0)
    return s_state.sector_valid;
  u32 cdimage_lba = 0;
  u32 track_number = 0;
  const bool success = Konami::ReadGVDataSector(s_state.read_lba, s_state.sector_buffer.data(), &cdimage_lba, &track_number);
  if (!success)
  {
    s_state.sector_buffer.fill(0);
    ERROR_LOG("KonamiGV.NCR53CF96 media_read_failure canonical_set='{}' scsi_lba={} cdimage_lba={} track={}",
              Konami::GetGVSetName(), s_state.read_lba, cdimage_lba, track_number);
  }
  s_state.sector_valid = true;
  INFO_LOG("KonamiGV.NCR53CF96 read10_sector_loaded canonical_set='{}' scsi_lba={} cdimage_lba={} track={} size={} success={} prefix={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
           Konami::GetGVSetName(), s_state.read_lba, cdimage_lba, track_number, s_state.sector_buffer.size(), success,
           s_state.sector_buffer[0], s_state.sector_buffer[1], s_state.sector_buffer[2], s_state.sector_buffer[3],
           s_state.sector_buffer[4], s_state.sector_buffer[5], s_state.sector_buffer[6], s_state.sector_buffer[7],
           s_state.sector_buffer[8], s_state.sector_buffer[9], s_state.sector_buffer[10], s_state.sector_buffer[11],
           s_state.sector_buffer[12], s_state.sector_buffer[13], s_state.sector_buffer[14], s_state.sector_buffer[15]);
  return true;
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
  if (s_selection_event)
    s_selection_event->Deactivate();
  if (s_disconnect_event)
    s_disconnect_event->Deactivate();
  DMA::SetRequest(DMA::Channel::PIO, false);
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

static TickCount GetSelectionDelayTicks()
{
  const u32 clock_factor = s_state.clock_factor != 0 ? s_state.clock_factor : 8;
  const u64 device_clocks =
    (static_cast<u64>(SELECT_SCALED_DELAY_CYCLES) * clock_factor) + SELECT_UNSCALED_DELAY_CYCLES;
  const u64 system_ticks = (device_clocks * static_cast<u64>(System::MASTER_CLOCK) + NCR_CLOCK_HZ - 1) / NCR_CLOCK_HZ;
  return std::max<TickCount>(static_cast<TickCount>(1),
                             System::ScaleTicksToOverclock(static_cast<TickCount>(system_ticks)));
}

static TickCount GetDisconnectDelayTicks()
{
  const u32 clock_factor = s_state.clock_factor != 0 ? s_state.clock_factor : 8;
  const u64 device_clocks = static_cast<u64>(DISCONNECT_DELAY_CYCLES) * clock_factor;
  const u64 system_ticks = (device_clocks * static_cast<u64>(System::MASTER_CLOCK) + NCR_CLOCK_HZ - 1) / NCR_CLOCK_HZ;
  return std::max<TickCount>(static_cast<TickCount>(1),
                             System::ScaleTicksToOverclock(static_cast<TickCount>(system_ticks)));
}

static void CompleteSelection()
{
  if (!s_state.selection_pending)
    return;

  const u32 selection_pc = s_state.selection_pc;
  s_state.selection_pending = false;
  s_state.selection_pc = 0;
  INFO_LOG("KonamiGV.NCR53CF96 selection_completed canonical_set='{}'", Konami::GetGVSetName());
  CaptureCDB(selection_pc);
}

static void SelectionEventCallback(void*, TickCount, TickCount)
{
  if (s_selection_event)
    s_selection_event->Deactivate();
  if (s_state.active)
    CompleteSelection();
}

static void CompleteTargetDisconnect()
{
  if (!s_state.disconnect_pending || !s_state.initiator_connected)
    return;

  s_state.disconnect_pending = false;
  s_state.initiator_connected = false;
  s_state.data_in_active = false;
  s_state.data_out_active = false;
  s_state.read_active = false;
  SetDMARequest(false);
  SetPhase(Phase::BusFree);
  INFO_LOG("KonamiGV.NCR53CF96 target_bsy_released canonical_set='{}'", Konami::GetGVSetName());
  INFO_LOG("KonamiGV.NCR53CF96 disconnected canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(),
           INTERRUPT_DISCONNECTED);
  AssertIRQ10(INTERRUPT_DISCONNECTED);
}

static void DisconnectEventCallback(void*, TickCount, TickCount)
{
  if (s_disconnect_event)
    s_disconnect_event->Deactivate();
  if (s_state.active)
    CompleteTargetDisconnect();
}

static void CompleteBusReset()
{
  if (s_selection_event)
    s_selection_event->Deactivate();
  if (s_disconnect_event)
    s_disconnect_event->Deactivate();
  s_state.selection_pending = false;
  s_state.selection_pc = 0;
  s_state.disconnect_pending = false;
  s_state.initiator_connected = false;
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
  KonamiGVCDROM::Initialize();
  if (!s_reset_event)
    s_reset_event = std::make_unique<TimingEvent>("Konami GV NCR53CF96 Reset", 1, 1, ResetEventCallback, nullptr);
  else
    s_reset_event->Deactivate();
  if (!s_selection_event)
    s_selection_event = std::make_unique<TimingEvent>("Konami GV NCR53CF96 Selection", 1, 1, SelectionEventCallback, nullptr);
  else
    s_selection_event->Deactivate();
  if (!s_disconnect_event)
    s_disconnect_event = std::make_unique<TimingEvent>("Konami GV NCR53CF96 Disconnect", 1, 1, DisconnectEventCallback, nullptr);
  else
    s_disconnect_event->Deactivate();
  ResetController();
  s_state.active = true;
  INFO_LOG("KonamiGV.NCR53CF96 initialized canonical_set='{}'", Konami::GetGVSetName());
}

void Reset()
{
  if (!s_state.active)
    return;
  ResetController(false, true);
  KonamiGVCDROM::Reset();
  s_state.active = true;
  INFO_LOG("KonamiGV.NCR53CF96 reset canonical_set='{}'", Konami::GetGVSetName());
}

void Shutdown()
{
  if (s_reset_event)
    s_reset_event->Deactivate();
  if (s_selection_event)
    s_selection_event->Deactivate();
  if (s_disconnect_event)
    s_disconnect_event->Deactivate();
  DMA::SetRequest(DMA::Channel::PIO, false);
  DeassertIRQ10();
  KonamiGVCDROM::Shutdown();
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
    case FIFO: return ReadFIFO(width);
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
      {
        CompleteCommand();
        INFO_LOG("KonamiGV.NCR53CF96 interrupt_status_read canonical_set='{}' value=0x{:02X}", Konami::GetGVSetName(), value);
      }
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
        case COMMAND_SELECT_WITH_ATN:
          s_state.selection_pending = true;
          s_state.selection_pc = pc;
          s_state.sequence_step = 0;
          INFO_LOG("KonamiGV.NCR53CF96 selection_started canonical_set='{}' pc=0x{:08X} delay_ticks={}",
                   Konami::GetGVSetName(), pc, GetSelectionDelayTicks());
          if (s_selection_event)
            s_selection_event->Schedule(GetSelectionDelayTicks());
          else
            CompleteSelection();
          return;
        case COMMAND_ENABLE_SELECTION_RESELECTION:
          INFO_LOG("KonamiGV.NCR53CF96 enable_selection_reselection canonical_set='{}'", Konami::GetGVSetName());
          break;
        case COMMAND_TRANSFER_INFORMATION:
          if ((s_state.command & 0x80) != 0)
          {
            if (s_state.data_in_active || s_state.read_active)
            {
              StartDataInTransfer();
            }
            else if (s_state.data_out_active)
            {
              INFO_LOG("KonamiGV.NCR53CF96 data_out_transfer_information canonical_set='{}' opcode=0x{:02X} response_remaining={} transfer_count=0x{:06X}",
                       Konami::GetGVSetName(), s_state.cdb[0],
                       s_state.response_length - s_state.response_position, s_state.transfer_counter);
              SetDMARequest(true);
            }
            else
            {
              SetPhase(Phase::Status);
              s_state.sequence_step = 0x00;
              INFO_LOG("KonamiGV.NCR53CF96 function_complete canonical_set='{}' cause=0x{:02X}",
                       Konami::GetGVSetName(), INTERRUPT_FUNCTION_COMPLETE);
              AssertIRQ10(INTERRUPT_FUNCTION_COMPLETE);
            }
            break;
          }
          [[fallthrough]];
        case COMMAND_INITIATOR_COMMAND_COMPLETE:
          // The target BIOS reads GOOD followed by Command Complete as one little-endian halfword from the FIFO.
          if (s_state.status_message_pending && s_state.fifo_count == 0)
          {
            WriteFIFO(s_state.target_status);
            WriteFIFO(s_state.target_message);
          }
          SetPhase(Phase::MessageIn);
          s_state.sequence_step = 0x06;
          INFO_LOG("KonamiGV.NCR53CF96 initiator_command_complete canonical_set='{}' phase={} sequence_step={}",
                   Konami::GetGVSetName(), static_cast<u8>(s_state.phase), s_state.sequence_step);
          if (s_state.status_message_pending)
          {
            INFO_LOG("KonamiGV.NCR53CF96 status_message_ready canonical_set='{}' status=0x{:02X} message=0x{:02X} fifo_count={}",
                     Konami::GetGVSetName(), s_state.target_status, s_state.target_message, s_state.fifo_count);
            s_state.test_unit_ready_complete = false;
          }
          INFO_LOG("KonamiGV.NCR53CF96 function_complete canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(),
                   INTERRUPT_FUNCTION_COMPLETE);
          AssertIRQ10(INTERRUPT_FUNCTION_COMPLETE);
          break;
        case COMMAND_MESSAGE_ACCEPTED:
          // Release ACK for the Command Complete message. The target then releases BSY;
          // the controller reports Disconnected only after observing that bus transition.
          s_state.sequence_step = 0x02;
          INFO_LOG("KonamiGV.NCR53CF96 message_accepted_fifo_state canonical_set='{}' fifo_count={} completion_bytes_consumed={} phase={}",
                   Konami::GetGVSetName(), s_state.fifo_count, s_state.status_message_consumption_logged,
                   static_cast<u8>(s_state.phase));
          s_state.status_message_pending = false;
          s_state.disconnect_pending = s_state.initiator_connected;
          INFO_LOG("KonamiGV.NCR53CF96 message_accepted canonical_set='{}' sequence_step={} disconnect_pending={}",
                   Konami::GetGVSetName(), s_state.sequence_step, s_state.disconnect_pending);
          if (s_state.disconnect_pending)
          {
            if (s_disconnect_event)
              s_disconnect_event->Schedule(GetDisconnectDelayTicks());
            else
              CompleteTargetDisconnect();
          }
          else
          {
            // No active initiator connection means the bus is already free. Preserve the
            // old Bus Service fallback rather than fabricating a disconnect transition.
            SetPhase(Phase::BusFree);
            INFO_LOG("KonamiGV.NCR53CF96 bus_service canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(),
                     INTERRUPT_BUS_SERVICE);
            AssertIRQ10(INTERRUPT_BUS_SERVICE);
          }
          break;
        default: RequestDeferredStop(MigrationStopReason::UnsupportedControllerCommand, "unimplemented_controller_command", pc); break;
      }
      if (!s_state.irq && !s_state.dma_request)
        CompleteCommand();
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

void DMARead(u32* data, u32 word_count)
{
  if (word_count == 0)
    return;

  const u32 byte_count = word_count * sizeof(u32);
  std::memset(data, 0, byte_count);
  if (!s_state.dma_request)
    return;

  if (s_state.read_active)
  {
    u8* output = reinterpret_cast<u8*>(data);
    u32 remaining = byte_count;
    while (remaining != 0 && s_state.read_blocks_remaining != 0)
    {
      if (!LoadReadSector())
        break;
      const u32 available = SCSI_LOGICAL_BLOCK_SIZE - s_state.read_sector_offset;
      const u32 copy_bytes = std::min(remaining, available);
      std::memcpy(output, s_state.sector_buffer.data() + s_state.read_sector_offset, copy_bytes);
      output += copy_bytes;
      remaining -= copy_bytes;
      s_state.read_sector_offset += copy_bytes;
      s_state.read_transferred_bytes += copy_bytes;
      if (s_state.read_sector_offset == SCSI_LOGICAL_BLOCK_SIZE)
      {
        INFO_LOG("KonamiGV.NCR53CF96 read10_progress canonical_set='{}' lba={} transferred={} remaining_blocks={}",
                 Konami::GetGVSetName(), s_state.read_lba, s_state.read_transferred_bytes, s_state.read_blocks_remaining - 1);
        s_state.read_lba++;
        s_state.read_blocks_remaining--;
        s_state.read_sector_offset = 0;
        s_state.sector_valid = false;
      }
    }
    DecrementTransferCounter(byte_count);
    if (s_state.read_blocks_remaining == 0 && (s_state.status & STATUS_TERMINAL_COUNT) != 0)
    {
      s_state.read_active = false;
      INFO_LOG("KonamiGV.NCR53CF96 read10_complete canonical_set='{}' bytes={}", Konami::GetGVSetName(),
               s_state.read_transferred_bytes);
      CompleteDataIn();
    }
    return;
  }

  if (!s_state.data_in_active || s_state.response_position >= s_state.response_length)
    return;

  const u16 remaining = s_state.response_length - s_state.response_position;
  const u32 copy_bytes = std::min<u32>(byte_count, remaining);
  std::memcpy(data, s_state.response.data() + s_state.response_position, copy_bytes);
  s_state.response_position = static_cast<u16>(s_state.response_position + copy_bytes);
  DecrementTransferCounter(byte_count);
  if (s_state.response_position == s_state.response_length && (s_state.status & STATUS_TERMINAL_COUNT) != 0)
    CompleteDataIn();
}

void DMAWrite(const u32* data, u32 word_count)
{
  if (word_count == 0 || !s_state.dma_request || !s_state.data_out_active)
    return;
  const u32 byte_count = word_count * sizeof(u32);
  const u16 remaining = s_state.response_length - s_state.response_position;
  const u32 copy_bytes = std::min<u32>(byte_count, remaining);
  const bool first_write = s_state.response_position == 0;
  std::memcpy(s_state.response.data() + s_state.response_position, data, copy_bytes);
  s_state.response_position = static_cast<u16>(s_state.response_position + copy_bytes);
  DecrementTransferCounter(byte_count);
  if (first_write)
  {
    INFO_LOG("KonamiGV.NCR53CF96 data_out_first_dma_write canonical_set='{}' opcode=0x{:02X} callback_bytes={} copied_bytes={} expected_bytes={}",
             Konami::GetGVSetName(), s_state.cdb[0], byte_count, copy_bytes, s_state.response_length);
  }
  if (s_state.response_position == s_state.response_length && (s_state.status & STATUS_TERMINAL_COUNT) != 0)
  {
    INFO_LOG("KonamiGV.NCR53CF96 data_out_complete canonical_set='{}' opcode=0x{:02X} bytes={}",
             Konami::GetGVSetName(), s_state.cdb[0], s_state.response_position);
    CompleteDataOut();
  }
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
  sw.DoBytes(s_state.response.data(), s_state.response.size());
  sw.Do(&s_state.response_length);
  sw.Do(&s_state.response_position);
  sw.Do(&s_state.target_transfer_length);
  sw.Do(&s_state.data_in_active);
  sw.Do(&s_state.data_out_active);
  sw.Do(&s_state.dma_request);
  sw.DoBytes(s_state.audio_output_channel.data(), s_state.audio_output_channel.size());
  sw.DoBytes(s_state.audio_output_volume.data(), s_state.audio_output_volume.size());
  sw.DoBytes(s_state.sector_buffer.data(), s_state.sector_buffer.size());
  sw.Do(&s_state.read_lba);
  sw.Do(&s_state.read_blocks_remaining);
  sw.Do(&s_state.read_total_bytes);
  sw.Do(&s_state.read_transferred_bytes);
  sw.Do(&s_state.read_sector_offset);
  sw.Do(&s_state.read_active);
  sw.Do(&s_state.sector_valid);
  sw.Do(&s_state.boundary_requested);
  sw.Do(&s_state.boundary_consumed);
  u8 boundary_reason = static_cast<u8>(s_state.boundary_reason);
  sw.Do(&boundary_reason);
  sw.Do(&s_state.access_logged);
  sw.Do(&s_state.overflow_logged);
  sw.Do(&s_state.underflow_logged);
  sw.Do(&s_state.deferred_command_logged);
  sw.Do(&s_state.reset_bus_pending);
  sw.Do(&s_state.selection_pending);
  sw.Do(&s_state.selection_pc);
  sw.Do(&s_state.initiator_connected);
  sw.Do(&s_state.disconnect_pending);
  const bool valid_phase = phase == static_cast<u8>(Phase::BusFree) || phase == static_cast<u8>(Phase::DataIn) ||
                           phase == static_cast<u8>(Phase::DataOut) || phase == static_cast<u8>(Phase::Status) ||
                           phase == static_cast<u8>(Phase::Command) || phase == static_cast<u8>(Phase::MessageOut) ||
                           phase == static_cast<u8>(Phase::MessageIn);
  const bool valid_boundary_reason = boundary_reason <= static_cast<u8>(MigrationStopReason::UnsupportedTargetCommand);
  if (sw.IsReading() && (!saved_active || s_state.fifo_read >= FIFO_CAPACITY || s_state.fifo_write >= FIFO_CAPACITY ||
                         s_state.fifo_count > FIFO_CAPACITY || s_state.command_queue_count > s_state.command_queue.size() ||
                         s_state.cdb_length > s_state.cdb.size() || s_state.response_length > s_state.response.size() ||
                         s_state.response_position > s_state.response_length || s_state.target_transfer_length > s_state.response.size() ||
                         s_state.read_sector_offset > SCSI_LOGICAL_BLOCK_SIZE || s_state.read_transferred_bytes > s_state.read_total_bytes ||
                         !valid_phase || !valid_boundary_reason))
  {
    ResetController();
  }
  else if (sw.IsReading())
  {
    s_state.phase = static_cast<Phase>(phase);
    s_state.boundary_reason = static_cast<MigrationStopReason>(boundary_reason);
    DMA::SetRequest(DMA::Channel::PIO, s_state.dma_request);
    if (s_selection_event)
    {
      s_selection_event->Deactivate();
      if (s_state.selection_pending)
        s_selection_event->Schedule(GetSelectionDelayTicks());
    }
    if (s_disconnect_event)
    {
      s_disconnect_event->Deactivate();
      if (s_state.disconnect_pending && s_state.initiator_connected)
        s_disconnect_event->Schedule(GetDisconnectDelayTicks());
    }
  }
  return !sw.HasError();
}

} // namespace KonamiGVScsi
