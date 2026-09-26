// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/konami/gq/konami_gq_scsi.h"

#include "core/arcade/devices/storage/ncr53cf96.h"

#include "core/arcade/systems/konami/konami.h"
#include "core/arcade/systems/konami/gq/konami_gq_hdd.h"
#include "core/dma.h"
#include "core/interrupt_controller.h"
#include "util/state_wrapper.h"

#include "common/log.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

Log_SetChannel(KonamiGQScsi);

// Extremely high-volume NCR53CF96 transaction detail remains intentionally
// suppressed so diagnostic logging cannot starve the low-latency audio path.

namespace KonamiGQScsi {
namespace {

using namespace NCR53CF96;

constexpr size_t RESPONSE_CAPACITY = 0x200;
constexpr u32 SCSI_LOGICAL_BLOCK_SIZE = 512;

struct ControllerState final : NCR53CF96::ControllerState<RESPONSE_CAPACITY, SCSI_LOGICAL_BLOCK_SIZE>
{
  bool write_active = false;
  u32 write_lba = 0;
  u16 write_blocks_remaining = 0;
  u32 write_total_bytes = 0;
  u32 write_transferred_bytes = 0;
  u32 write_sector_offset = 0;
};

static void CompleteDataOut();
static void CaptureCDB(u32 pc);
static bool LoadReadSector();

struct ControllerPolicy
{
  static constexpr std::string_view RESET_EVENT_NAME = "Konami GQ NCR53CF96 Reset";
  static constexpr std::string_view SELECTION_EVENT_NAME = "Konami GQ NCR53CF96 Selection";
  static constexpr std::string_view DISCONNECT_EVENT_NAME = "Konami GQ NCR53CF96 Disconnect";
  static constexpr std::string_view LOG_PREFIX = "KonamiGQ";

  static std::string_view GetSetName()
  {
    return Konami::GetGQSetName();
  }

  template<typename... T>
  static void Info(fmt::format_string<T...>, T&&...)
  {
    // Shared Info includes routine transfer-completion traffic. GQ keeps
    // command-level diagnostics below instead of logging every transfer.
  }

  template<typename... T>
  static void Detail(fmt::format_string<T...>, T&&...)
  {
    // Intentionally suppressed: per-transaction detail is too high-volume for
    // the low-latency GQ audio path. Event-level diagnostics use Info()/DEV_LOG.
  }

  template<typename... T>
  static void Warning(fmt::format_string<T...> format, T&&... args)
  {
    WARNING_LOG(format, std::forward<T>(args)...);
  }

  template<typename... T>
  static void Error(fmt::format_string<T...> format, T&&... args)
  {
    ERROR_LOG(format, std::forward<T>(args)...);
  }

  static void DeliverDMARequest(bool request)
  {
    DMA::SetRequest(DMA::Channel::PIO, request);
  }

  static void DeliverIRQ(bool state)
  {
    InterruptController::SetLineState(InterruptController::IRQ::IRQ10, state);
  }

  static void ResetTargetProtocol(ControllerState& state)
  {
    NCR53CF96::ResetTargetProtocolState(state, Konami::HasValidGQHardDiskContent());
    state.write_active = false;
    state.write_lba = 0;
    state.write_blocks_remaining = 0;
    state.write_total_bytes = 0;
    state.write_transferred_bytes = 0;
    state.write_sector_offset = 0;
    DMA::SetRequest(DMA::Channel::PIO, false);
  }

  static void TargetInitialize(ControllerState&)
  {
  }

  static void TargetReset(ControllerState&)
  {
  }

  static void TargetShutdown(ControllerState&)
  {
  }

  static void ExecuteCDB(u32 pc)
  {
    CaptureCDB(pc);
  }

  static bool ReadSector(ControllerState&)
  {
    return LoadReadSector();
  }

  static void OnTargetDisconnect(ControllerState& state)
  {
    state.write_active = false;
  }

  template<typename Wrapper>
  static void DoStateExtraPrefix(Wrapper&, ControllerState&)
  {
  }

  template<typename Wrapper>
  static void DoStateExtraSuffix(Wrapper& sw, ControllerState& state)
  {
    sw.Do(&state.write_active);
    sw.Do(&state.write_lba);
    sw.Do(&state.write_blocks_remaining);
    sw.Do(&state.write_total_bytes);
    sw.Do(&state.write_transferred_bytes);
    sw.Do(&state.write_sector_offset);
  }

  static bool ValidateExtraState(const ControllerState& state)
  {
    return state.write_sector_offset <= SCSI_LOGICAL_BLOCK_SIZE &&
           state.write_transferred_bytes <= state.write_total_bytes;
  }
};

using SharedController = NCR53CF96::ControllerAdapter<ControllerState, ControllerPolicy>;

SharedController s_controller;
ControllerState& s_state = s_controller.GetState();
static u32 s_read_log_count = 0;

static void CompleteDataOut()
{
  s_controller.SetDMARequest(false);
  s_state.write_active = false;
  NCR53CF96::CompleteDataOutState(s_state, [](Phase phase) { s_controller.SetPhase(phase); });
  s_controller.AssertIRQ(INTERRUPT_BUS_SERVICE);
}

static void CaptureCDB(u32 pc)
{
  if (s_state.fifo_count < 2)
  {
    s_state.status |= STATUS_GROSS_ERROR;
    s_controller.RequestDeferredStop(MigrationStopReason::IncompleteCDB, "selection_without_cdb", pc);
    return;
  }

  const u8 opcode = NCR53CF96::PeekFIFO(s_state, 1);
  const u8 cdb_length = NCR53CF96::GetCDBLength(opcode);
  if (cdb_length == 0 || cdb_length > s_state.cdb.size() || s_state.fifo_count < (cdb_length + 1))
  {
    s_state.status |= STATUS_GROSS_ERROR;
    s_controller.RequestDeferredStop(MigrationStopReason::IncompleteCDB, "unsupported_or_incomplete_cdb", pc);
    return;
  }

  s_state.cdb.fill(0);
  for (u8 i = 0; i < cdb_length; i++)
    s_state.cdb[i] = NCR53CF96::PeekFIFO(s_state, static_cast<u8>(i + 1));
  s_state.cdb_length = cdb_length;
  NCR53CF96::ClearFIFO(s_state);
  s_state.initiator_connected = true;
  s_state.disconnect_pending = false;
  s_controller.DeactivateDisconnectEvent();
  s_controller.SetPhase(Phase::Command);
  s_state.sequence_step = (opcode == 0x00) ? 0x06 : 0x04;
  constexpr u8 select_completion_cause = INTERRUPT_FUNCTION_COMPLETE | INTERRUPT_BUS_SERVICE;

  auto finish_no_data = [&](u8 status = SCSI_STATUS_GOOD) {
    s_state.target_status = status;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    s_state.data_in_active = false;
    s_state.data_out_active = false;
    s_state.read_active = false;
    s_state.write_active = false;
    s_state.status_message_pending = true;
    s_state.status_message_consumption_logged = false;
    s_controller.SetPhase(Phase::BusFree);
    s_state.sequence_step = 0x06;
    s_controller.AssertIRQ(select_completion_cause);
  };

  auto start_data_in = [&](u16 length) {
    s_state.response_length = std::min<u16>(length, static_cast<u16>(s_state.response.size()));
    s_state.response_position = 0;
    s_state.target_transfer_length = s_state.response_length;
    s_state.data_in_active = s_state.response_length != 0;
    s_state.data_out_active = false;
    s_state.read_active = false;
    s_state.write_active = false;
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    s_state.status_message_pending = false;
    s_controller.SetPhase(s_state.data_in_active ? Phase::DataIn : Phase::Status);
    s_state.sequence_step = 0x04;
    s_controller.AssertIRQ(select_completion_cause);
  };

  auto start_data_out = [&](u32 length) {
    if (length > s_state.response.size())
    {
      s_controller.RequestDeferredStop(MigrationStopReason::UnsupportedTargetCommand, "oversized_data_out", pc);
      return false;
    }
    s_state.response.fill(0);
    s_state.response_length = static_cast<u16>(length);
    s_state.response_position = 0;
    s_state.target_transfer_length = s_state.response_length;
    s_state.data_in_active = false;
    s_state.data_out_active = length != 0;
    s_state.read_active = false;
    s_state.write_active = false;
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    s_state.status_message_pending = false;
    if (length == 0)
      finish_no_data();
    else
    {
      s_controller.SetPhase(Phase::DataOut);
      s_state.sequence_step = 0x04;
      s_controller.AssertIRQ(select_completion_cause);
    }
    return true;
  };

  auto set_check_condition = [&](u8 sense_key, u8 asc, u8 ascq) {
    s_state.sense_key = sense_key;
    s_state.sense_asc = asc;
    s_state.sense_ascq = ascq;
    finish_no_data(0x02);
  };

  auto validate_range = [&](u32 lba, u32 blocks) {
    const u64 end_lba = static_cast<u64>(lba) + static_cast<u64>(blocks);
    return blocks != 0 && lba < KonamiGQHDD::GetBlockCount() && end_lba <= KonamiGQHDD::GetBlockCount();
  };

  if (opcode == 0x00) // TEST UNIT READY
  {
    if (!Konami::HasValidGQHardDiskContent())
      set_check_condition(0x02, 0x3a, 0x00);
    else
    {
      s_state.sense_key = SCSI_SENSE_NO_SENSE;
      s_state.sense_asc = 0;
      s_state.sense_ascq = 0;
      finish_no_data();
    }
    DEV_LOG("KonamiGQ.NCR53CF96 test_unit_ready canonical_set='{}' ready={}", Konami::GetGQSetName(),
             Konami::HasValidGQHardDiskContent());
    return;
  }

  if (opcode == 0x03) // REQUEST SENSE
  {
    const u8 allocation_length = s_state.cdb[4];
    s_state.response.fill(0);
    const u16 length = std::min<u16>(allocation_length, 18);
    if (length > 0)
      s_state.response[0] = 0x70;
    if (length > 2)
      s_state.response[2] = s_state.sense_key;
    if (length > 7)
      s_state.response[7] = 0x0a;
    if (length > 12)
      s_state.response[12] = s_state.sense_asc;
    if (length > 13)
      s_state.response[13] = s_state.sense_ascq;
    start_data_in(length);
    s_state.sense_key = SCSI_SENSE_NO_SENSE;
    s_state.sense_asc = 0;
    s_state.sense_ascq = 0;
    DEV_LOG("KonamiGQ.NCR53CF96 request_sense canonical_set='{}' allocation_length={} transfer_length={}",
             Konami::GetGQSetName(), allocation_length, length);
    return;
  }

  if (opcode == 0x12) // INQUIRY
  {
    const u8 lun = (s_state.cdb[1] >> 5) & 0x07;
    const bool evpd = (s_state.cdb[1] & 0x01) != 0;
    const u8 page_code = s_state.cdb[2];
    const u8 allocation_length = s_state.cdb[4];
    if (evpd || page_code != 0)
    {
      set_check_condition(0x05, 0x24, 0x00);
      return;
    }

    s_state.response.fill(0);
    std::fill_n(s_state.response.data() + 8, 28, static_cast<u8>(' '));
    s_state.response[0] = lun == 0 ? 0x00 : 0x7f;
    s_state.response[1] = 0x00;
    s_state.response[2] = 0x05;
    s_state.response[3] = 0x01;
    s_state.response[4] = 52;
    static constexpr char identity[] = " SEAGATE          ST225N1.00";
    static_assert(sizeof(identity) - 1 == 28);
    std::memcpy(s_state.response.data() + 8, identity, sizeof(identity) - 1);
    s_state.response[36] = 0x00;
    s_state.response[37] = 0x08;
    s_state.response[38] = 0x00;
    s_state.response[39] = 0x99;
    s_state.response[40] = 0xa0;
    s_state.response[41] = 0x27;
    s_state.response[42] = 0x34;
    s_state.response[43] = 0x01;
    s_state.response[44] = 0x04;
    s_state.response[45] = 0xa0;
    s_state.response[46] = 0x01;
    s_state.response[47] = 0x18;
    s_state.response[48] = 0x07;
    s_state.response[49] = 0x00;
    s_state.response[50] = 0xa0;
    s_state.response[51] = 0x00;
    s_state.response[52] = 0x00;
    s_state.response[53] = 0xff;
    const u16 length = std::min<u16>(allocation_length, 56);
    start_data_in(length);
    DEV_LOG("KonamiGQ.NCR53CF96 inquiry canonical_set='{}' lun={} allocation_length={} transfer_length={}",
             Konami::GetGQSetName(), lun, allocation_length, length);
    return;
  }

  if (opcode == 0x1a) // MODE SENSE (6)
  {
    const u8 lun = (s_state.cdb[1] >> 5) & 0x07;
    const bool disable_block_descriptors = (s_state.cdb[1] & 0x08) != 0;
    const u8 page_code = s_state.cdb[2] & 0x3f;
    const u8 allocation_length = s_state.cdb[4];
    if (lun != 0)
    {
      set_check_condition(0x05, 0x25, 0x00);
      return;
    }

    s_state.response.fill(0);
    u16 pos = 4;
    const KonamiGQHDD::Geometry& geometry = KonamiGQHDD::GetGeometry();
    const u32 block_count = KonamiGQHDD::GetBlockCount();
    auto put_be16 = [&](u16 value) {
      s_state.response[pos++] = static_cast<u8>(value >> 8);
      s_state.response[pos++] = static_cast<u8>(value);
    };
    auto put_be24 = [&](u32 value) {
      s_state.response[pos++] = static_cast<u8>(value >> 16);
      s_state.response[pos++] = static_cast<u8>(value >> 8);
      s_state.response[pos++] = static_cast<u8>(value);
    };

    if (!disable_block_descriptors)
    {
      s_state.response[3] = 8;
      s_state.response[pos++] = 0;
      put_be24(std::min<u32>(block_count - 1, 0x00ffffff));
      s_state.response[pos++] = 0;
      put_be24(SCSI_LOGICAL_BLOCK_SIZE);
    }

    auto append_page = [&](u8 page) {
      switch (page)
      {
        case 0x00:
          s_state.response[pos++] = 0x80;
          s_state.response[pos++] = 0x02;
          s_state.response[pos++] = 0x00;
          s_state.response[pos++] = 0x00;
          return true;
        case 0x01:
          s_state.response[pos++] = 0x01;
          s_state.response[pos++] = 0x0a;
          s_state.response[pos++] = 0x26;
          for (u32 i = 0; i < 9; i++)
            s_state.response[pos++] = 0;
          return true;
        case 0x02:
          s_state.response[pos++] = 0x02;
          s_state.response[pos++] = 0x0e;
          for (u32 i = 0; i < 14; i++)
            s_state.response[pos++] = 0;
          return true;
        case 0x03:
          s_state.response[pos++] = 0x83;
          s_state.response[pos++] = 0x16;
          put_be16(static_cast<u16>(std::min<u32>(geometry.cylinders * geometry.heads, 0xffff)));
          for (u32 i = 0; i < 6; i++)
            s_state.response[pos++] = 0;
          put_be16(static_cast<u16>(geometry.sectors));
          put_be16(static_cast<u16>(geometry.bytes_per_sector));
          for (u32 i = 0; i < 10; i++)
            s_state.response[pos++] = 0;
          return true;
        case 0x04:
          s_state.response[pos++] = 0x84;
          s_state.response[pos++] = 0x16;
          put_be24(geometry.cylinders);
          s_state.response[pos++] = static_cast<u8>(geometry.heads);
          for (u32 i = 0; i < 14; i++)
            s_state.response[pos++] = 0;
          put_be16(10000);
          s_state.response[pos++] = 0;
          s_state.response[pos++] = 0;
          return true;
        case 0x08:
          s_state.response[pos++] = 0x08;
          s_state.response[pos++] = 0x0a;
          for (u32 i = 0; i < 10; i++)
            s_state.response[pos++] = 0;
          return true;
        default:
          return false;
      }
    };

    bool supported = true;
    if (page_code == 0x3f)
    {
      for (const u8 page : {u8(0x08), u8(0x04), u8(0x03), u8(0x02), u8(0x01), u8(0x00)})
        supported &= append_page(page);
    }
    else
    {
      supported = append_page(page_code);
    }

    if (!supported || pos > s_state.response.size())
    {
      set_check_condition(0x05, 0x24, 0x00);
      return;
    }

    s_state.response[0] = static_cast<u8>(pos - 1);
    const u16 length = std::min<u16>(allocation_length, pos);
    start_data_in(length);
    DEV_LOG("KonamiGQ.NCR53CF96 mode_sense6 canonical_set='{}' page=0x{:02X} allocation_length={} transfer_length={}",
             Konami::GetGQSetName(), page_code, allocation_length, length);
    return;
  }

  if (opcode == 0x15) // MODE SELECT (6)
  {
    if (!start_data_out(s_state.cdb[4]))
      return;
    DEV_LOG("KonamiGQ.NCR53CF96 mode_select6 canonical_set='{}' parameter_length={}", Konami::GetGQSetName(),
             s_state.cdb[4]);
    return;
  }

  // REZERO, RESERVE, RELEASE, START/STOP, PREVENT/ALLOW, SYNCHRONIZE CACHE.
  if (opcode == 0x01 || opcode == 0x16 || opcode == 0x17 || opcode == 0x1b || opcode == 0x1e || opcode == 0x35)
  {
    finish_no_data();
    DEV_LOG("KonamiGQ.NCR53CF96 no_data_command canonical_set='{}' opcode=0x{:02X}", Konami::GetGQSetName(), opcode);
    return;
  }

  if (opcode == 0x1c) // RECEIVE DIAGNOSTIC RESULTS
  {
    const u16 allocation_length = static_cast<u16>((static_cast<u16>(s_state.cdb[3]) << 8) | s_state.cdb[4]);
    s_state.response.fill(0);
    static constexpr std::array<u8, 7> result = {0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(s_state.response.data(), result.data(), result.size());
    start_data_in(static_cast<u16>(std::min<u32>(allocation_length, static_cast<u32>(result.size()))));
    DEV_LOG("KonamiGQ.NCR53CF96 receive_diagnostic canonical_set='{}' allocation_length={}",
             Konami::GetGQSetName(), allocation_length);
    return;
  }

  if (opcode == 0x1d) // SEND DIAGNOSTIC
  {
    const bool self_test = (s_state.cdb[1] & 0x04) != 0;
    const u16 parameter_length = static_cast<u16>((static_cast<u16>(s_state.cdb[3]) << 8) | s_state.cdb[4]);
    if (self_test || parameter_length == 0)
    {
      finish_no_data();
    }
    else
    {
      s_state.response.fill(0);
      static constexpr std::array<u8, 8> result = {0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
      std::memcpy(s_state.response.data(), result.data(), result.size());
      start_data_in(static_cast<u16>(std::min<u32>(parameter_length, static_cast<u32>(result.size()))));
    }
    DEV_LOG("KonamiGQ.NCR53CF96 send_diagnostic canonical_set='{}' self_test={} parameter_length={}",
             Konami::GetGQSetName(), self_test, parameter_length);
    return;
  }

  if (opcode == 0x25) // READ CAPACITY (10)
  {
    s_state.response.fill(0);
    const u32 last_lba = KonamiGQHDD::GetBlockCount() - 1;
    s_state.response[0] = static_cast<u8>(last_lba >> 24);
    s_state.response[1] = static_cast<u8>(last_lba >> 16);
    s_state.response[2] = static_cast<u8>(last_lba >> 8);
    s_state.response[3] = static_cast<u8>(last_lba);
    s_state.response[4] = static_cast<u8>(SCSI_LOGICAL_BLOCK_SIZE >> 24);
    s_state.response[5] = static_cast<u8>(SCSI_LOGICAL_BLOCK_SIZE >> 16);
    s_state.response[6] = static_cast<u8>(SCSI_LOGICAL_BLOCK_SIZE >> 8);
    s_state.response[7] = static_cast<u8>(SCSI_LOGICAL_BLOCK_SIZE);
    start_data_in(8);
    DEV_LOG("KonamiGQ.NCR53CF96 read_capacity canonical_set='{}' last_lba={} block_size={}",
             Konami::GetGQSetName(), last_lba, SCSI_LOGICAL_BLOCK_SIZE);
    return;
  }

  if (opcode == 0x08 || opcode == 0x28) // READ (6)/(10)
  {
    const u32 lba = opcode == 0x08 ?
                      ((static_cast<u32>(s_state.cdb[1] & 0x1f) << 16) |
                       (static_cast<u32>(s_state.cdb[2]) << 8) | s_state.cdb[3]) :
                      ((static_cast<u32>(s_state.cdb[2]) << 24) | (static_cast<u32>(s_state.cdb[3]) << 16) |
                       (static_cast<u32>(s_state.cdb[4]) << 8) | s_state.cdb[5]);
    u32 blocks = opcode == 0x08 ? s_state.cdb[4] :
                                  ((static_cast<u32>(s_state.cdb[7]) << 8) | s_state.cdb[8]);
    if (opcode == 0x08 && blocks == 0)
      blocks = 256;
    else if (opcode == 0x28 && blocks == 0)
    {
      finish_no_data();
      return;
    }
    if (!validate_range(lba, blocks) || blocks > UINT16_MAX)
    {
      set_check_condition(0x05, 0x21, 0x00);
      return;
    }

    s_state.read_lba = lba;
    s_state.read_blocks_remaining = static_cast<u16>(blocks);
    s_state.read_total_bytes = blocks * SCSI_LOGICAL_BLOCK_SIZE;
    s_state.read_transferred_bytes = 0;
    s_state.read_sector_offset = 0;
    s_state.sector_valid = false;
    s_state.read_active = true;
    s_state.data_in_active = false;
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    s_controller.SetPhase(Phase::DataIn);
    s_state.sequence_step = 0x04;
    s_controller.AssertIRQ(select_completion_cause);
    const u32 read_log_index = ++s_read_log_count;
    if (read_log_index <= 8 || (read_log_index & (read_log_index - 1)) == 0)
    {
      DEV_LOG(
        "KonamiGQ.NCR53CF96 read_started index={} canonical_set='{}' opcode=0x{:02X} lba={} blocks={} bytes={}",
        read_log_index, Konami::GetGQSetName(), opcode, lba, blocks, s_state.read_total_bytes);
    }
    return;
  }

  if (opcode == 0x0a || opcode == 0x2a) // WRITE (6)/(10), in-memory overlay only
  {
    const u32 lba = opcode == 0x0a ?
                      ((static_cast<u32>(s_state.cdb[1] & 0x1f) << 16) |
                       (static_cast<u32>(s_state.cdb[2]) << 8) | s_state.cdb[3]) :
                      ((static_cast<u32>(s_state.cdb[2]) << 24) | (static_cast<u32>(s_state.cdb[3]) << 16) |
                       (static_cast<u32>(s_state.cdb[4]) << 8) | s_state.cdb[5]);
    u32 blocks = opcode == 0x0a ? s_state.cdb[4] :
                                  ((static_cast<u32>(s_state.cdb[7]) << 8) | s_state.cdb[8]);
    if (opcode == 0x0a && blocks == 0)
      blocks = 256;
    else if (opcode == 0x2a && blocks == 0)
    {
      finish_no_data();
      return;
    }
    if (!validate_range(lba, blocks) || blocks > UINT16_MAX)
    {
      set_check_condition(0x05, 0x21, 0x00);
      return;
    }

    s_state.write_lba = lba;
    s_state.write_blocks_remaining = static_cast<u16>(blocks);
    s_state.write_total_bytes = blocks * SCSI_LOGICAL_BLOCK_SIZE;
    s_state.write_transferred_bytes = 0;
    s_state.write_sector_offset = 0;
    s_state.sector_buffer.fill(0);
    s_state.write_active = true;
    s_state.data_out_active = true;
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    s_state.status_message_pending = false;
    s_controller.SetPhase(Phase::DataOut);
    s_state.sequence_step = 0x04;
    s_controller.AssertIRQ(select_completion_cause);
    DEV_LOG("KonamiGQ.NCR53CF96 write_started canonical_set='{}' opcode=0x{:02X} lba={} blocks={} bytes={} persistence='memory_only'",
             Konami::GetGQSetName(), opcode, lba, blocks, s_state.write_total_bytes);
    return;
  }

  if (opcode == 0x0b || opcode == 0x2b) // SEEK (6)/(10)
  {
    finish_no_data();
    DEV_LOG("KonamiGQ.NCR53CF96 seek canonical_set='{}' opcode=0x{:02X}", Konami::GetGQSetName(), opcode);
    return;
  }

  if (opcode == 0x2f) // VERIFY (10)
  {
    if ((s_state.cdb[1] & 0x02) != 0)
      set_check_condition(0x05, 0x24, 0x00);
    else
      finish_no_data();
    return;
  }

  if (opcode == 0x04) // FORMAT UNIT - clear only the transient write overlay
  {
    KonamiGQHDD::ClearWriteOverlay();
    finish_no_data();
    DEV_LOG("KonamiGQ.NCR53CF96 format_unit canonical_set='{}' action='overlay_cleared'", Konami::GetGQSetName());
    return;
  }

  std::string bytes;
  for (u8 i = 0; i < cdb_length; i++)
  {
    char byte_text[4] = {};
    std::snprintf(byte_text, sizeof(byte_text), "%02X", s_state.cdb[i]);
    if (!bytes.empty())
      bytes += ' ';
    bytes += byte_text;
  }
  ERROR_LOG("KonamiGQ.NCR53CF96 next_unsupported_cdb canonical_set='{}' opcode=0x{:02X} cdb='{}' length={} pc=0x{:08X}",
            Konami::GetGQSetName(), opcode, bytes, cdb_length, pc);
  s_controller.RequestDeferredStop(MigrationStopReason::UnsupportedTargetCommand, "unimplemented_target_command", pc);
}

static bool LoadReadSector()
{
  if (s_state.sector_valid || s_state.read_blocks_remaining == 0)
    return s_state.sector_valid;

  const bool success = KonamiGQHDD::ReadSector(s_state.read_lba, s_state.sector_buffer.data());
  if (!success)
  {
    s_state.sector_buffer.fill(0);
    ERROR_LOG("KonamiGQ.NCR53CF96 media_read_failure canonical_set='{}' lba={}", Konami::GetGQSetName(),
              s_state.read_lba);
  }
  s_state.sector_valid = true;
  return true;
}

} // namespace

void Initialize()
{
  s_read_log_count = 0;
  s_controller.Initialize();
}

void Reset()
{
  s_read_log_count = 0;
  s_controller.Reset();
}

void Shutdown()
{
  s_controller.Shutdown();
}

bool IsActive()
{
  return s_controller.IsActive();
}

u32 ReadRegister(u32 width, u32 offset)
{
  return s_controller.ReadRegister(width, offset);
}

void WriteRegister(u32 width, u32 offset, u32 value, u32 pc)
{
  s_controller.WriteRegister(width, offset, value, pc);
}

MigrationStopReason ConsumeMigrationStopRequest()
{
  return s_controller.ConsumeMigrationStopRequest();
}

u8 GetActiveCommand()
{
  return s_controller.GetActiveCommand();
}

u8 GetTargetCommandOpcode()
{
  return s_controller.GetTargetCommandOpcode();
}

void DMARead(u32* data, u32 word_count)
{
  s_controller.DMARead(data, word_count);
}

void DMAWrite(const u32* data, u32 word_count)
{
  if (word_count == 0 || !s_state.dma_request || !s_state.data_out_active)
    return;

  const u32 byte_count = word_count * sizeof(u32);
  if (s_state.write_active)
  {
    const u8* input = reinterpret_cast<const u8*>(data);
    u32 remaining = byte_count;
    while (remaining != 0 && s_state.write_blocks_remaining != 0)
    {
      const u32 available = SCSI_LOGICAL_BLOCK_SIZE - s_state.write_sector_offset;
      const u32 copy_bytes = std::min(remaining, available);
      std::memcpy(s_state.sector_buffer.data() + s_state.write_sector_offset, input, copy_bytes);
      input += copy_bytes;
      remaining -= copy_bytes;
      s_state.write_sector_offset += copy_bytes;
      s_state.write_transferred_bytes += copy_bytes;
      if (s_state.write_sector_offset == SCSI_LOGICAL_BLOCK_SIZE)
      {
        if (!KonamiGQHDD::WriteSector(s_state.write_lba, s_state.sector_buffer.data()))
        {
          ERROR_LOG("KonamiGQ.NCR53CF96 media_write_failure canonical_set='{}' lba={}", Konami::GetGQSetName(),
                    s_state.write_lba);
          s_state.sense_key = 0x03;
          s_state.sense_asc = 0x0c;
          s_state.sense_ascq = 0x02;
          s_state.target_status = 0x02;
        }
        s_state.write_lba++;
        s_state.write_blocks_remaining--;
        s_state.write_sector_offset = 0;
        s_state.sector_buffer.fill(0);
      }
    }

    NCR53CF96::DecrementTransferCounter(s_state, byte_count);
    if (s_state.write_blocks_remaining == 0 && (s_state.status & STATUS_TERMINAL_COUNT) != 0)
    {
      DEV_LOG("KonamiGQ.NCR53CF96 write_complete canonical_set='{}' bytes={} persistence='memory_only'",
               Konami::GetGQSetName(), s_state.write_transferred_bytes);
      CompleteDataOut();
    }
    return;
  }

  NCR53CF96::WriteResponseControllerDMA(
    s_state, data, word_count,
    [](u32, u32) {},
    []() { CompleteDataOut(); });
}

bool DoState(StateWrapper& sw)
{
  return s_controller.DoState(sw);
}

} // namespace KonamiGQScsi
