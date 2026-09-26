// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/konami/gv/konami_gv_scsi.h"

#include "core/arcade/devices/storage/ncr53cf96.h"

#include "core/arcade/systems/konami/konami.h"
#include "core/arcade/systems/konami/gv/konami_gv_cdrom.h"
#include "core/dma.h"
#include "core/interrupt_controller.h"
#include "util/state_wrapper.h"

#include "common/log.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

Log_SetChannel(KonamiGVScsi);

namespace KonamiGVScsi {
namespace {

using namespace NCR53CF96;

constexpr size_t RESPONSE_CAPACITY = 0xFF;
constexpr u32 SCSI_LOGICAL_BLOCK_SIZE = 2048;

struct ControllerState final : NCR53CF96::ControllerState<RESPONSE_CAPACITY, SCSI_LOGICAL_BLOCK_SIZE>
{
  std::array<u8, 4> audio_output_channel = {{1, 2, 0, 0}};
  std::array<u8, 4> audio_output_volume = {{0xff, 0xff, 0, 0}};
};

static void CompleteDataOut();
static void CaptureCDB(u32 pc);
static bool LoadReadSector();

struct ControllerPolicy
{
  static constexpr std::string_view RESET_EVENT_NAME = "Konami GV NCR53CF96 Reset";
  static constexpr std::string_view SELECTION_EVENT_NAME = "Konami GV NCR53CF96 Selection";
  static constexpr std::string_view DISCONNECT_EVENT_NAME = "Konami GV NCR53CF96 Disconnect";
  static constexpr std::string_view LOG_PREFIX = "KonamiGV";

  static std::string_view GetSetName()
  {
    return Konami::GetGVSetName();
  }

  template<typename... T>
  static void Info(fmt::format_string<T...> format, T&&... args)
  {
    DEV_LOG(format, std::forward<T>(args)...);
  }

  template<typename... T>
  static void Detail(fmt::format_string<T...>, T&&...)
  {
    // Routine NCR53CF96 phase/IRQ/DMA/transfer bookkeeping is intentionally
    // suppressed. Command-level GV diagnostics remain available through
    // CaptureCDB(), while Info() retains lifecycle/reset/read-complete events.
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
    NCR53CF96::ResetTargetProtocolState(state, Konami::HasValidGVDiscContent());
    DMA::SetRequest(DMA::Channel::PIO, false);
  }

  static void TargetInitialize(ControllerState&)
  {
    KonamiGVCDROM::Initialize();
  }

  static void TargetReset(ControllerState&)
  {
    KonamiGVCDROM::Reset();
  }

  static void TargetShutdown(ControllerState&)
  {
    KonamiGVCDROM::Shutdown();
  }

  static void ExecuteCDB(u32 pc)
  {
    CaptureCDB(pc);
  }

  static bool ReadSector(ControllerState&)
  {
    return LoadReadSector();
  }

  static void OnTargetDisconnect(ControllerState&)
  {
  }

  template<typename Wrapper>
  static void DoStateExtraPrefix(Wrapper& sw, ControllerState& state)
  {
    sw.DoBytes(state.audio_output_channel.data(), state.audio_output_channel.size());
    sw.DoBytes(state.audio_output_volume.data(), state.audio_output_volume.size());
  }

  template<typename Wrapper>
  static void DoStateExtraSuffix(Wrapper&, ControllerState&)
  {
  }

  static bool ValidateExtraState(const ControllerState&)
  {
    return true;
  }
};

using SharedController = NCR53CF96::ControllerAdapter<ControllerState, ControllerPolicy>;

SharedController s_controller;
ControllerState& s_state = s_controller.GetState();

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
      if (page_code == 0x0e && page_size >= 16 && (page_offset + 16) <= parameter_length)
      {
        for (u8 output = 0; output < 4; output++)
        {
          const size_t output_offset = page_offset + 8 + (output * 2);
          s_state.audio_output_channel[output] = s_state.response[output_offset] & 0x0f;
          s_state.audio_output_volume[output] = s_state.response[output_offset + 1];
          KonamiGVCDROM::SetAudioOutput(output, s_state.audio_output_channel[output], s_state.audio_output_volume[output]);
        }
        break;
      }
      page_offset += page_size;
    }
  }
  s_controller.SetDMARequest(false);
  NCR53CF96::CompleteDataOutState(s_state, [](Phase phase) { s_controller.SetPhase(phase); });
  DEV_LOG("KonamiGV.NCR53CF96 bus_service canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(),
           INTERRUPT_BUS_SERVICE);
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
  DEV_LOG("KonamiGV.NCR53CF96 cdb_captured canonical_set='{}' opcode=0x{:02X} cdb='{}' length={} pc=0x{:08X}",
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
    DEV_LOG("KonamiGV.NCR53CF96 test_unit_ready_execute canonical_set='{}' ready={}", Konami::GetGVSetName(),
             s_state.target_ready);
    if (!s_state.target_ready)
    {
      ERROR_LOG("KonamiGV.NCR53CF96 test_unit_ready_not_ready canonical_set='{}'", Konami::GetGVSetName());
      s_controller.RequestDeferredStop(MigrationStopReason::IncompleteCDB, "test_unit_ready_without_valid_media", pc);
      return;
    }

    // The authoritative target completes no-data commands internally: no status/message bytes are placed in the FIFO.
    s_controller.SetPhase(Phase::BusFree);
    DEV_LOG("KonamiGV.NCR53CF96 target_phase canonical_set='{}' phase={}", Konami::GetGVSetName(),
             static_cast<u8>(s_state.phase));
    s_state.sequence_step = 0x06;
    DEV_LOG("KonamiGV.NCR53CF96 test_unit_ready_good canonical_set='{}' status=0x{:02X} message=0x{:02X}",
             Konami::GetGVSetName(), s_state.target_status, s_state.target_message);
    DEV_LOG("KonamiGV.NCR53CF96 select_command_completion canonical_set='{}' phase={} sequence_step={}",
             Konami::GetGVSetName(), static_cast<u8>(s_state.phase), s_state.sequence_step);
    DEV_LOG("KonamiGV.NCR53CF96 function_complete canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(),
             select_completion_cause);
    s_controller.AssertIRQ(select_completion_cause);
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
    s_controller.SetPhase(Phase::DataIn);
    s_state.sequence_step = 0x04;
    DEV_LOG("KonamiGV.NCR53CF96 request_sense_execute canonical_set='{}' allocation_length={}",
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
    DEV_LOG("KonamiGV.NCR53CF96 request_sense_response canonical_set='{}' length={} data='{}'", Konami::GetGVSetName(),
             s_state.response_length, response_bytes);
    DEV_LOG("KonamiGV.NCR53CF96 data_in_started canonical_set='{}' phase={} sequence_step={}",
             Konami::GetGVSetName(), static_cast<u8>(s_state.phase), s_state.sequence_step);
    DEV_LOG("KonamiGV.NCR53CF96 function_complete canonical_set='{}' cause=0x{:02X}", Konami::GetGVSetName(),
             select_completion_cause);
    s_controller.AssertIRQ(select_completion_cause);
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
    s_controller.SetPhase(Phase::DataIn);
    s_state.sequence_step = 0x04;
    DEV_LOG("KonamiGV.NCR53CF96 read10_execute canonical_set='{}' lba={} blocks={} bytes={}", Konami::GetGVSetName(),
             lba, blocks, s_state.read_total_bytes);
    DEV_LOG("KonamiGV.NCR53CF96 read10_data_in_started canonical_set='{}' phase={} sequence_step={}",
             Konami::GetGVSetName(), static_cast<u8>(s_state.phase), s_state.sequence_step);
    s_controller.AssertIRQ(select_completion_cause);
    return;
  }

  if (opcode == 0x15)
  {
    const u8 lun = (s_state.cdb[1] >> 5) & 0x07;
    const u8 parameter_length = s_state.cdb[4];
    if (lun != 0 || (s_state.cdb[1] & 0x10) == 0)
    {
      s_controller.RequestDeferredStop(MigrationStopReason::UnsupportedTargetCommand, "unsupported_mode_select_variant", pc);
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
    s_controller.SetPhase(Phase::DataOut);
    s_state.sequence_step = 0x04;
    DEV_LOG("KonamiGV.NCR53CF96 mode_select6_execute canonical_set='{}' lun={} parameter_length={} status_bits=0x{:02X} cause=0x{:02X}",
             Konami::GetGVSetName(), lun, parameter_length, NCR53CF96::GetPhaseStatusBits(s_state.phase),
             select_completion_cause);
    s_controller.AssertIRQ(select_completion_cause);
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
      s_controller.RequestDeferredStop(MigrationStopReason::UnsupportedTargetCommand, "unsupported_read_toc_format", pc);
      return;
    }
    std::memcpy(s_state.response.data(), full_response.data(), s_state.response_length);
    s_state.data_in_active = s_state.response_length != 0;
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    s_controller.SetPhase(Phase::DataIn);
    s_state.sequence_step = 0x04;
    std::string response_bytes;
    for (u16 i = 0; i < s_state.response_length; i++)
    {
      char text[4] = {};
      std::snprintf(text, sizeof(text), "%02X", s_state.response[i]);
      if (!response_bytes.empty()) response_bytes += ' ';
      response_bytes += text;
    }
    DEV_LOG("KonamiGV.NCR53CF96 read_toc_execute canonical_set='{}' format={} msf={} starting_track={} allocation_length={} full_response_length={} toc_data_length={} transfer_length={} adr=1 control=4 adr_control=0x14 data='{}'",
             Konami::GetGVSetName(), format, (s_state.cdb[1] & 0x02) != 0, starting_track, allocation_length, full_response_length,
             full_response_length >= 2 ? full_response_length - 2 : 0, s_state.response_length, response_bytes);
    DEV_LOG("KonamiGV.NCR53CF96 data_in_started canonical_set='{}' phase={} sequence_step={}", Konami::GetGVSetName(),
             static_cast<u8>(s_state.phase), s_state.sequence_step);
    s_controller.AssertIRQ(select_completion_cause);
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
    s_state.target_status = SCSI_STATUS_GOOD; s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE; s_controller.SetPhase(Phase::DataIn); s_state.sequence_step = 0x04;
    s_controller.AssertIRQ(select_completion_cause); return;
  }

  if (opcode == 0x48 || opcode == 0x4b)
  {
    const bool ok = opcode == 0x48 ? KonamiGVCDROM::PlayAudioTrackIndex(s_state.cdb[4], s_state.cdb[5], s_state.cdb[7], s_state.cdb[8]) : (KonamiGVCDROM::PauseAudio((s_state.cdb[8] & 1) != 0), true);
    s_state.target_status = SCSI_STATUS_GOOD; s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE; s_state.status_message_pending = true;
    s_controller.SetPhase(Phase::BusFree);
    // The working PoC leaves PLAY AUDIO and PAUSE/RESUME at selection step 4.
    s_state.sequence_step = 0x04;
    DEV_LOG("KonamiGV.NCR53CF96 audio_select_completion canonical_set='{}' opcode=0x{:02X} cause=0x{:02X}",
             Konami::GetGVSetName(), opcode, select_completion_cause);
    s_controller.AssertIRQ(select_completion_cause);
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
    if (page_control != 0 || page_code != 0x0e)
    {
      ERROR_LOG("KonamiGV.NCR53CF96 mode_sense6_invalid canonical_set='{}' page_control={} page_code=0x{:02X}",
                Konami::GetGVSetName(), page_control, page_code);
      s_controller.RequestDeferredStop(MigrationStopReason::UnsupportedTargetCommand, "unsupported_mode_sense_page", pc);
      return;
    }

    // Match the Toshiba XM-5401 MODE SENSE(6) CD Audio Control page used by MAME:
    // 4-byte mode parameter header, optional 8-byte block descriptor, then the
    // 16-byte page 0x0E. The old PoC-compatible 0x16-byte pseudo-page placed the
    // same output bytes at absolute offsets 20-27, but reported the wrong page
    // length/PS bit and omitted the descriptor when DBD was clear.
    constexpr u32 mode_header_size = 4;
    constexpr u32 block_descriptor_size = 8;
    constexpr u32 audio_control_page_size = 16;
    const u32 descriptor_length = dbd ? 0 : block_descriptor_size;
    const u32 page_offset = mode_header_size + descriptor_length;
    const u32 full_response_length = page_offset + audio_control_page_size;
    s_state.response_length = static_cast<u16>(
      std::min<u32>({allocation_length, full_response_length, static_cast<u32>(s_state.response.size())}));
    s_state.response_position = 0;
    s_state.target_transfer_length = s_state.response_length;

    s_state.response[0] = static_cast<u8>(full_response_length - 1);
    s_state.response[1] = 0x00;
    s_state.response[2] = 0x80;
    s_state.response[3] = static_cast<u8>(descriptor_length);
    if (!dbd)
    {
      const u32 lba_count = KonamiGVCDROM::GetLBACount();
      const u32 last_block = lba_count ? (lba_count - 1) : 0;
      s_state.response[4] = 0x00;
      s_state.response[5] = static_cast<u8>(last_block >> 16);
      s_state.response[6] = static_cast<u8>(last_block >> 8);
      s_state.response[7] = static_cast<u8>(last_block);
      s_state.response[8] = 0x00;
      s_state.response[9] = 0x00;
      s_state.response[10] = static_cast<u8>(SCSI_LOGICAL_BLOCK_SIZE >> 8);
      s_state.response[11] = static_cast<u8>(SCSI_LOGICAL_BLOCK_SIZE);
    }

    s_state.response[page_offset] = 0x8e;
    s_state.response[page_offset + 1] = 0x0e;
    s_state.response[page_offset + 2] = 0x04;
    for (u32 output = 0; output < s_state.audio_output_channel.size(); output++)
    {
      const u32 output_offset = page_offset + 8 + (output * 2);
      s_state.response[output_offset] = s_state.audio_output_channel[output];
      s_state.response[output_offset + 1] = s_state.audio_output_volume[output];
    }

    s_state.data_in_active = s_state.response_length != 0;
    s_state.target_status = SCSI_STATUS_GOOD;
    s_state.target_message = SCSI_MESSAGE_COMMAND_COMPLETE;
    s_controller.SetPhase(Phase::DataIn);
    s_state.sequence_step = 0x04;
    s_controller.AssertIRQ(select_completion_cause);
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
      s_controller.RequestDeferredStop(MigrationStopReason::UnsupportedTargetCommand, "unsupported_inquiry_variant", pc);
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
    s_controller.SetPhase(Phase::DataIn);
    s_state.sequence_step = 0x04;
    std::string response_bytes;
    for (u16 i = 0; i < s_state.response_length; i++)
    {
      char text[4] = {};
      std::snprintf(text, sizeof(text), "%02X", s_state.response[i]);
      if (!response_bytes.empty()) response_bytes += ' ';
      response_bytes += text;
    }
    DEV_LOG("KonamiGV.NCR53CF96 inquiry_execute canonical_set='{}' lun={} evpd={} page=0x{:02X} allocation_length={} full_response_length={} transfer_length={} vendor='TOSHIBA ' product='CD-ROM XM-5401TA' revision='3605' data='{}'",
             Konami::GetGVSetName(), lun, evpd, page_code, allocation_length, inquiry.size(), s_state.response_length,
             response_bytes);
    s_controller.AssertIRQ(select_completion_cause);
    return;
  }

  ERROR_LOG("KonamiGV.NCR53CF96 next_unsupported_cdb canonical_set='{}' opcode=0x{:02X} cdb='{}' length={} pc=0x{:08X} transfer_count=0x{:06X} fifo_count={} response_position={} response_remaining={} controller_command=0x{:02X} phase={} sequence_step={} target_status=0x{:02X} sense_key=0x{:02X} asc=0x{:02X} ascq=0x{:02X} dma_request={}",
            Konami::GetGVSetName(), opcode, bytes, cdb_length, pc, s_state.transfer_count, s_state.fifo_count,
            s_state.response_position, s_state.response_length - s_state.response_position, s_state.command,
            static_cast<u8>(s_state.phase), s_state.sequence_step, s_state.target_status, s_state.sense_key,
            s_state.sense_asc, s_state.sense_ascq, s_state.dma_request);
  s_controller.RequestDeferredStop(MigrationStopReason::UnsupportedTargetCommand, "unimplemented_target_command", pc);
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
  return true;
}

} // namespace

void Initialize()
{
  s_controller.Initialize();
}

void Reset()
{
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
  NCR53CF96::WriteResponseControllerDMA(
    s_state, data, word_count,
    [](u32 byte_count, u32 copied_bytes) {
      DEV_LOG("KonamiGV.NCR53CF96 data_out_first_dma_write canonical_set='{}' opcode=0x{:02X} callback_bytes={} "
               "copied_bytes={} expected_bytes={}",
               Konami::GetGVSetName(), s_state.cdb[0], byte_count, copied_bytes, s_state.response_length);
    },
    []() {
      DEV_LOG("KonamiGV.NCR53CF96 data_out_complete canonical_set='{}' opcode=0x{:02X} bytes={}",
               Konami::GetGVSetName(), s_state.cdb[0], s_state.response_position);
      CompleteDataOut();
    });
}

bool DoState(StateWrapper& sw)
{
  return s_controller.DoState(sw);
}

} // namespace KonamiGVScsi
