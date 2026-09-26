// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/konami/gq/konami_gq_sound_cpu.h"

#include "core/arcade/devices/audio/k054539.h"
#include "core/arcade/systems/konami/gq/konami_gq_k056800.h"
#include "core/arcade/systems/konami/gq/konami_gq_pcm.h"
#include "core/arcade/systems/konami/gq/konami_gq_tms57002.h"

#include "core/spu.h"
#include "core/system.h"
#include "core/timing_event.h"

#include "common/log.h"

#include "core/musashi/m68k.h"
#include "core/musashi/m68k_bus.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <memory>
#include <string_view>
#include <vector>

Log_SetChannel(KonamiGQSoundCPU);

namespace KonamiGQSoundCPU {

u32 ReadMemory8ForCore(u32 address);
u32 ReadMemory16ForCore(u32 address);
u32 ReadMemory32ForCore(u32 address);
void WriteMemory8ForCore(u32 address, u32 value);
void WriteMemory16ForCore(u32 address, u32 value);
void WriteMemory32ForCore(u32 address, u32 value);

namespace {

constexpr u32 ADDRESS_MASK = 0x00ffffff;
[[maybe_unused]] constexpr u32 ROM_BASE = 0x000000;
constexpr u32 ROM_SIZE = 0x080000;
constexpr u32 RAM_BASE = 0x100000;
constexpr u32 RAM_SIZE = 0x010000;
constexpr u32 K054539_BASE = 0x200000;
constexpr u32 K054539_END = 0x2004ff;
constexpr u32 K054539_REGISTER_COUNT = 0x280;
constexpr u32 K054539_DATA_REGISTER = 0x22d;
constexpr u32 K054539_BANK_REGISTER = 0x22e;
constexpr u32 K054539_BANK_SIZE = 0x20000;
constexpr u32 K054539_DATA_POINTER_MASK = K054539_BANK_SIZE - 1;
constexpr u32 K054539_SAMPLE_ROM_SIZE = 0x080000;
constexpr u32 TMS_DATA_ADDRESS = 0x300001;
constexpr u32 K056800_BASE = 0x400000;
constexpr u32 K056800_END = 0x40001f;

static s32 ApplyK056800CommonVolume(s32 sample)
{
  const u32 volume = KonamiGQK056800::GetCommonVolume();
  return static_cast<s32>((static_cast<s64>(sample) * static_cast<s64>(volume)) / 256);
}

// The 056602 is the downstream GQ audio module, but its exact analog transfer
// gain is not documented in the available references. Crypt Killer's factory
// Sound Volume setting (0x14) programs a K056800 position of 0x81. Use the
// current empirical 4x board-output calibration from controlled hardware-level
// comparisons; this is not claimed to be a measured 056602 transfer gain.
static s32 ApplyK056602OutputGain(s32 sample)
{
  return static_cast<s32>(static_cast<s64>(sample) * 4);
}

// Crypt Killer's exact AXDA/TMS57002 return balance through the downstream GQ
// audio path is not yet fully documented. Controlled hardware-reference
// listening consistently favored a +8 dB AXDA contribution over the baseline
// mix. Keep that empirical calibration until a clean direct hardware capture
// can replace it with a measured reference.
constexpr double CRYPT_KILLER_AXDA_GAIN = 2.5118864315095801;

constexpr u32 TMS_STATUS_BASE = 0x500000;
constexpr u32 TMS_STATUS_END = 0x500001;
constexpr u32 NRES_BASE = 0x580000;
constexpr u32 NRES_END = 0x580001;

constexpr u32 SOUND_CLOCK_HZ = 8'000'000;
constexpr u32 TMS_CLOCK_HZ = 24'000'000;
constexpr u32 TMS_SYNC_HZ = 48'000;
constexpr u32 TMS_CYCLES_PER_SOUND_CPU_CYCLE = TMS_CLOCK_HZ / SOUND_CLOCK_HZ;
constexpr u32 TMS_CYCLES_PER_SYNC = TMS_CLOCK_HZ / TMS_SYNC_HZ;
constexpr u32 K054539_CLOCK_HZ = 18'432'000;
constexpr u32 K054539_TIMER_REGISTER = 0x227;
constexpr u32 K054539_CONTROL_REGISTER = 0x22f;
constexpr u8 K054539_TIMER_OUTPUT_ENABLE = 0x20;
constexpr u64 K054539_TIMER_PHASE_DENOMINATOR = static_cast<u64>(SOUND_CLOCK_HZ) * 3u;
constexpr TickCount SCHEDULER_INTERVAL_TICKS = System::MASTER_CLOCK / 1000;
// Keep each Musashi slice close to one 48 kHz sample period so K054539 writes,
// TMS57002 processing, and the queued board output remain ordered.
constexpr int MAX_EXECUTE_CYCLES = static_cast<int>((SOUND_CLOCK_HZ + TMS_SYNC_HZ - 1) / TMS_SYNC_HZ);
constexpr size_t MAX_NATIVE_AUDIO_QUEUE_FRAMES = 4096;
constexpr u32 K054539_KEY_ON_REGISTER = 0x214;
constexpr u32 K054539_KEY_OFF_REGISTER = 0x215;
constexpr u32 K054539_ACTIVE_REGISTER = 0x22c;
constexpr u32 K054539_CHANNEL_REGISTER_STRIDE = 0x20;
constexpr u32 K054539_CHANNEL_POSITION_OFFSET = 0x0c;

[[maybe_unused]] constexpr std::string_view CRYPT_KILLER_SERIAL = "cryptklr";
constexpr u32 CRYPT_KILLER_SFX_DISPATCH_PC = 0x005826;
constexpr u32 CRYPT_KILLER_SHORT_SPEECH_1 = 0x16ea89;
constexpr u32 CRYPT_KILLER_SHORT_SPEECH_2 = 0x18ce29;
constexpr std::array<u32, 3> CRYPT_KILLER_MAN_SPEECH_ADDRESSES = {{
  0x140000, 0x14a78e, 0x15d257,
}};

constexpr u32 SLICE_LOG_LIMIT = 16;
constexpr u32 ACCESS_LOG_LIMIT = 32;
constexpr u32 K054539_WRITE_LOG_LIMIT = 32;
constexpr u32 K054539_DATA_LOG_LIMIT = 16;
constexpr u32 TMS_STATUS_POLL_LOG_LIMIT = 8;
constexpr u32 CRYPT_KILLER_ROUTE_TARGET_LOG_LIMIT = 2048;
constexpr u32 CRYPT_KILLER_ROUTE_BASELINE_LOG_LIMIT = 128;
constexpr u32 CRYPT_KILLER_ROUTE_WRITE_LOG_LIMIT = 2048;
constexpr u32 CRYPT_KILLER_SIGNAL_BASELINE_TRACK_LIMIT = 16;
constexpr u32 CRYPT_KILLER_SIGNAL_SUMMARY_LOG_LIMIT = 256;

struct NativeAudioFrame
{
  std::array<std::array<s32, 2>, 2> chip = {};
  std::array<std::array<s32, 2>, 2> dsp_send = {};
  std::array<K054539::Chip::ChannelFrame, 2> channel = {};
  std::array<K054539::Chip::RawChannelFrame, 2> raw_channel = {};
  std::array<K054539::Chip::GQSourceSlotFrame, 2> source_slot = {};
  std::array<K054539::Chip::GQOutputPlaneFrame, 2> output_plane = {};
  std::array<K054539::Chip::GQMuxFPathFrame, 2> muxf_path = {};
  std::array<K054539::Chip::GQMuxFShadowFrame, 2> muxf_shadow = {};
  std::array<K054539::Chip::GQMuxFShadowValidFrame, 2> muxf_shadow_valid = {};
  std::array<K054539::Chip::GQAXDTChannelContributionFrame, 2> axdt_channel_contribution = {};
  std::array<K054539::Chip::GQAXDTSignedChannelContributionFrame, 2> axdt_signed_channel_contribution = {};
  std::array<K054539::Chip::GQAXDTCoefficientAddressFrame, 2> axdt_coefficient_address = {};
  std::array<K054539::Chip::GQAXDTChannelMixFrame, 2> axdt_channel_mix = {};
  std::array<bool, 2> axdt_channel_mix_valid = {};
  std::array<K054539::Chip::GQAXDTFullMixFrame, 2> axdt_full_mix = {};
  std::array<bool, 2> axdt_full_mix_valid = {};
  std::array<K054539::Chip::GQAXDTClosedLoopMixFrame, 2> axdt_closed_loop_mix = {};
  std::array<bool, 2> axdt_closed_loop_mix_valid = {};
  std::array<K054539::Chip::GQAXDTRVVLoopMixFrame, 2> axdt_rvv_loop_mix = {};
  std::array<bool, 2> axdt_rvv_loop_mix_valid = {};
  std::array<s32, 4> tms = {};
  std::array<s32, 4> rvv_tms = {};
  std::array<s32, 2> output = {};
};

struct CryptKillerSignalTrace
{
  bool active = false;
  bool baseline = false;
  u32 start_address = 0;
  u32 sound_pc = 0;
  u8 volume = 0;
  u8 reverb_volume = 0;
  u8 pan = 0;
  u8 effect_param_06 = 0;
  u8 effect_param_07 = 0;
  u8 channel_control = 0;
  u8 global_volume = 0;
  u8 effects_control = 0;
  u8 effects_mux = 0;
  u64 frames = 0;
  u64 raw_peak = 0;
  std::array<u64, 2> dry_peak = {};
  std::array<u64, 2> chip_peak = {};
  std::array<u64, 2> dsp_send_peak = {};
  std::array<u64, 2> tms_return_peak = {};
  std::array<u64, 2> board_peak = {};
  std::array<u64, 8> raw_pcm_peak = {};
  std::array<u64, 12> source_slot_peak = {};
  std::array<u64, 6> output_plane_peak = {};
  u64 muxf_shadow_peak = 0;
  u64 muxf_shadow_valid_frames = 0;
  std::array<u64, 2> axdt_channel_contribution_peak = {};
  std::array<u8, 2> axdt_coefficient_address = {};
  std::array<u64, 2> axdt_channel_mix_peak = {};
  u64 axdt_channel_mix_valid_frames = 0;
  std::array<u64, 2> axdt_full_mix_peak = {};
  u64 axdt_full_mix_valid_frames = 0;
  std::array<u64, 2> axdt_closed_loop_peak = {};
  u64 axdt_closed_loop_valid_frames = 0;
  std::array<u64, 2> axdt_closed_loop_clip_frames = {};
  std::array<u64, 2> axdt_rvv_loop_peak = {};
  u64 axdt_rvv_loop_valid_frames = 0;
  std::array<u64, 2> rvv_tms_return_peak = {};
  std::array<u64, 2> axdt_rvv_loop_clip_frames = {};
  std::array<u64, 2> rvv_tms_clip_frames = {};
  std::array<u64, 2> rvv_tms_real_diff_peak = {};
  std::array<u64, 2> rvv_tms_real_absdiff_sum = {};
  // Phase 3L11 identifies the active channel(s) that make the channel-only
  // AXDT aggregate incomplete. A path-mask bit uses the GQMuxFPath enum
  // value (bit 0 DLAT_C, bit 1 DLAT_B, bit 2 P13LAT, bit 3 DLAT_A).
  std::array<u64, 8> axdt_mix_invalid_channel_frames = {};
  std::array<u8, 8> axdt_mix_invalid_path_mask = {};
  std::array<u8, 8> axdt_mix_invalid_effect06 = {};
  std::array<u8, 8> axdt_mix_invalid_effect07 = {};
  std::array<u8, 8> axdt_mix_invalid_control = {};
  std::array<u8, 8> axdt_mix_invalid_signature_seen = {};
  std::array<u8, 8> axdt_mix_invalid_signature_changed = {};
  K054539::Chip::GQMuxFPath muxf_path = K054539::Chip::GQMuxFPath::Unknown;
};

struct State
{
  std::vector<u8> rom;
  std::vector<u8> pcm_samples;
  std::unique_ptr<KonamiGQTMS57002::Core> tms57002;
  std::unique_ptr<KonamiGQTMS57002::Core> rvv_tms57002;
  std::unique_ptr<TimingEvent> timing_event;
  std::array<u8, RAM_SIZE> ram = {};
  std::array<std::array<u8, K054539_REGISTER_COUNT>, 2> k054539_registers = {};
  std::array<K054539::Chip, 2> k054539_chips = {};
  std::deque<NativeAudioFrame> native_audio_queue;
  NativeAudioFrame k054539_resample_current = {};
  NativeAudioFrame k054539_resample_next = {};
  u64 k054539_resample_phase = 0;
  bool k054539_resampler_initialized = false;
  std::array<u32, 2> k054539_data_pointer = {};
  std::array<u8, 2> k054539_data_bank = {};
  std::array<u64, 2> k054539_timer_phase = {};
  std::array<u8, 2> k054539_timer_state = {};
  std::array<bool, 2> k054539_timer_configured = {};
  u8 sound_control = 0;
  u8 nres = 0;
  bool crypt_killer_route_trace_enabled = false;
  bool active = false;
  bool reset_released = false;
  bool executing = false;
  bool first_k054539_access_logged = false;
  bool first_tms_access_logged = false;
  bool first_unmapped_access_logged = false;
  bool k054539_irq2_pending = false;
  u64 total_cycles = 0;
  u64 cycle_fraction = 0;
  u64 last_ticks_per_second = 0;
  s64 cycle_balance = 0;
  u64 scheduler_callbacks = 0;
  u64 scheduler_ticks = 0;
  u64 k054539_reads = 0;
  u64 k054539_writes = 0;
  u64 k054539_data_reads = 0;
  u64 k054539_data_writes = 0;
  u64 k054539_sample_rom_reads = 0;
  u64 k054539_pcm_ram_reads = 0;
  u64 k054539_reverb_reads = 0;
  u64 k054539_reverb_writes = 0;
  u64 k054539_unmapped_data_reads = 0;
  u64 k054539_timer_toggles = 0;
  u64 k054539_timer_rising_edges = 0;
  u64 k054539_irq2_assertions = 0;
  u64 k054539_irq2_clears = 0;
  u64 tms_reads = 0;
  u64 tms_writes = 0;
  u64 tms_scheduled_cycles = 0;
  u64 tms_syncs = 0;
  u32 tms_cycle_balance = 0;
  u64 unmapped_reads = 0;
  u64 unmapped_writes = 0;
  u32 reset_pulses = 0;
  u32 crypt_killer_route_key_logs = 0;
  u32 crypt_killer_route_target_logs = 0;
  u32 crypt_killer_route_baseline_logs = 0;
  u32 crypt_killer_route_write_logs = 0;
  u32 crypt_killer_signal_baseline_tracks = 0;
  u32 crypt_killer_signal_summary_logs = 0;
  std::array<std::array<CryptKillerSignalTrace, 8>, 2> crypt_killer_signal_trace = {};
  std::array<std::array<bool, 8>, 2> crypt_killer_gameplay_sfx_active = {};
  u32 slice_count = 0;
  u32 slice_logs = 0;
  u32 access_logs = 0;
  u32 k054539_write_logs = 0;
  u32 k054539_data_logs = 0;
  u32 tms_status_poll_logs = 0;
};

State s_state;

bool ShouldLogAccess()
{
  if (s_state.access_logs >= ACCESS_LOG_LIMIT)
    return false;

  s_state.access_logs++;
  return true;
}

bool ShouldLogK054539Data()
{
  if (s_state.k054539_data_logs >= K054539_DATA_LOG_LIMIT)
    return false;

  s_state.k054539_data_logs++;
  return true;
}

bool ShouldLogK054539IRQ(u64 count)
{
  return count <= 16 || (count & (count - 1)) == 0;
}

u8 ReadK054539ExternalData(u32 chip, u32 address)
{
  if (address < s_state.pcm_samples.size())
  {
    s_state.k054539_sample_rom_reads++;
    return s_state.pcm_samples[address];
  }

  if (address >= K054539_SAMPLE_ROM_SIZE)
  {
    const u32 pcm_offset = address - K054539_SAMPLE_ROM_SIZE;
    if (pcm_offset < KonamiGQPCM::RAM_SIZE)
    {
      s_state.k054539_pcm_ram_reads++;
      return KonamiGQPCM::ReadRAMByte(pcm_offset);
    }
  }

  s_state.k054539_unmapped_data_reads++;
  if (ShouldLogK054539Data())
  {
    DEV_LOG(
      "KonamiGQ.SoundCPU K054539_data_unmapped chip={} bank=0x{:02X} pointer=0x{:05X} address=0x{:06X}",
      chip + 1, s_state.k054539_data_bank[chip], s_state.k054539_data_pointer[chip], address);
  }
  return 0xff;
}

void ResetK054539Resampler()
{
  s_state.native_audio_queue.clear();
  s_state.k054539_resample_current = {};
  s_state.k054539_resample_next = {};
  s_state.k054539_resample_phase = 0;
  s_state.k054539_resampler_initialized = false;
}

void QueueNativeAudioFrame(const NativeAudioFrame& frame)
{
  if (s_state.native_audio_queue.size() >= MAX_NATIVE_AUDIO_QUEUE_FRAMES)
    s_state.native_audio_queue.pop_front();

  s_state.native_audio_queue.push_back(frame);
}

bool PopNativeAudioFrame(NativeAudioFrame* frame)
{
  if (!frame || s_state.native_audio_queue.empty())
    return false;

  *frame = s_state.native_audio_queue.front();
  s_state.native_audio_queue.pop_front();
  return true;
}

u32 GetK054539ChannelStartAddress(u32 chip, u32 channel)
{
  const u32 base = channel * K054539_CHANNEL_REGISTER_STRIDE + K054539_CHANNEL_POSITION_OFFSET;
  const auto& registers = s_state.k054539_registers[chip];
  return static_cast<u32>(registers[base]) |
         (static_cast<u32>(registers[base + 1]) << 8) |
         (static_cast<u32>(registers[base + 2]) << 16);
}

bool IsCryptKillerManSpeechAddress(u32 address)
{
  return std::find(
           CRYPT_KILLER_MAN_SPEECH_ADDRESSES.begin(),
           CRYPT_KILLER_MAN_SPEECH_ADDRESSES.end(),
           address) != CRYPT_KILLER_MAN_SPEECH_ADDRESSES.end();
}

// Reference-tuned Crypt Killer balance corrections. Keep these local to the
// identified voice streams and dedicated gameplay-SFX dispatch so the board
// gain, title audio, and AXDA/TMS return calibration remain unchanged.
constexpr s32 CRYPT_KILLER_GAIN_ONE_Q12 = 4096;
constexpr s32 CRYPT_KILLER_WOMAN_VOICE_GAIN_Q12 = 9192; // +7.02 dB total
constexpr s32 CRYPT_KILLER_MAN_VOICE_GAIN_Q12 = 6500;   // +4.01 dB total
constexpr s32 CRYPT_KILLER_GAMEPLAY_SFX_GAIN_Q12 = 5157; // +2.00 dB

s32 ApplyCryptKillerBalanceGain(s32 sample, s32 gain_q12)
{
  return static_cast<s32>(
    (static_cast<s64>(sample) * static_cast<s64>(gain_q12)) /
    static_cast<s64>(CRYPT_KILLER_GAIN_ONE_Q12));
}

s32 GetCryptKillerVoiceGainQ12(u32 start_address)
{
  if (start_address == CRYPT_KILLER_SHORT_SPEECH_1 ||
      start_address == CRYPT_KILLER_SHORT_SPEECH_2)
  {
    return CRYPT_KILLER_WOMAN_VOICE_GAIN_Q12;
  }

  if (IsCryptKillerManSpeechAddress(start_address))
    return CRYPT_KILLER_MAN_VOICE_GAIN_Q12;

  return CRYPT_KILLER_GAIN_ONE_Q12;
}

bool IsCryptKillerGameplaySFX(u32 start_address, u32 sound_pc)
{
  if (sound_pc != CRYPT_KILLER_SFX_DISPATCH_PC)
    return false;

  if (start_address == CRYPT_KILLER_SHORT_SPEECH_1 ||
      start_address == CRYPT_KILLER_SHORT_SPEECH_2 ||
      IsCryptKillerManSpeechAddress(start_address) ||
      start_address == 0x1e1049 ||
      start_address == 0x120000 ||
      start_address == 0x0427dd)
  {
    return false;
  }

  return true;
}

const char* ClassifyCryptKillerSound(u32 start_address, u32 sound_pc)
{
  if (start_address == CRYPT_KILLER_SHORT_SPEECH_1 ||
      start_address == CRYPT_KILLER_SHORT_SPEECH_2)
  {
    return "short_speech";
  }
  if (IsCryptKillerManSpeechAddress(start_address))
    return "man_speech";
  if (start_address == 0x1e1049)
    return "insert_coin_candidate";
  if (start_address == 0x120000)
    return "look_out_candidate";
  if (sound_pc == CRYPT_KILLER_SFX_DISPATCH_PC)
    return "gameplay_sfx";
  return "other";
}

bool IsK054539OutputRoutingRegister(u32 register_index)
{
  return (register_index >= 0x200 && register_index <= 0x213) ||
         (register_index >= 0x224 && register_index <= 0x225) ||
         (register_index >= 0x228 && register_index <= 0x22b);
}

void LogCryptKillerK054539KeyOn(u32 chip, u32 channel, u32 sound_pc)
{
  if (!s_state.crypt_killer_route_trace_enabled)
    return;

  const auto& registers = s_state.k054539_registers[chip];
  const u32 base = channel * K054539_CHANNEL_REGISTER_STRIDE;
  const u32 start_address = GetK054539ChannelStartAddress(chip, channel);
  const u32 loop_address = static_cast<u32>(registers[base + 0x08]) |
                           (static_cast<u32>(registers[base + 0x09]) << 8) |
                           (static_cast<u32>(registers[base + 0x0a]) << 16);
  const u32 pitch = static_cast<u32>(registers[base]) |
                    (static_cast<u32>(registers[base + 1]) << 8) |
                    (static_cast<u32>(registers[base + 2]) << 16);
  const u32 channel_control_register = 0x200u + (channel * 2u);
  const u8 channel_control = registers[channel_control_register];
  const u8 channel_control_odd = registers[channel_control_register + 1u];
  const u32 volume_select = (~channel_control) & 0x03u;
  const u32 global_volume_register = 0x228u + volume_select;
  const u8 global_volume = registers[global_volume_register] & 0x7fu;
  const u8 pan = registers[base + 0x05];
  const std::string_view sound_class = ClassifyCryptKillerSound(start_address, sound_pc);
  const bool targeted = sound_class != "other" || volume_select != 3u;

  if (targeted)
  {
    if (s_state.crypt_killer_route_target_logs >= CRYPT_KILLER_ROUTE_TARGET_LOG_LIMIT)
      return;
    s_state.crypt_killer_route_target_logs++;
  }
  else
  {
    if (s_state.crypt_killer_route_baseline_logs >= CRYPT_KILLER_ROUTE_BASELINE_LOG_LIMIT)
      return;
    s_state.crypt_killer_route_baseline_logs++;
  }

  s_state.crypt_killer_route_key_logs++;
  DEV_LOG(
    "KonamiGQ.SoundCPU CryptKiller_route_key index={} class='{}' chip={} channel={} "
    "start=0x{:06X} loop=0x{:06X} pc=0x{:06X} pitch=0x{:06X} volume=0x{:02X} "
    "reverb_volume=0x{:02X} rv_high={} rv_low={} effect06=0x{:02X} effect07=0x{:02X} "
    "effect_word=0x{:04X} pan=0x{:02X} pan_right={} pan_left={} "
    "channel_control=0x{:02X} channel_control_odd=0x{:02X} data_type={} reverse={} loop_enable={} "
    "route_bits={} volume_select={} global_volume_register=0x{:03X} global_volume=0x{:02X} "
    "axda_a210=0x{:02X} axda_b211=0x{:02X} muxh_a212=0x{:02X} muxh_b213=0x{:02X} "
    "global228=0x{:02X} global229=0x{:02X} global22a=0x{:02X} global22b=0x{:02X} "
    "effects224=0x{:02X} mux225=0x{:02X} control22f=0x{:02X}",
    s_state.crypt_killer_route_key_logs, sound_class, chip + 1, channel,
    start_address, loop_address, sound_pc, pitch, registers[base + 0x03], registers[base + 0x04],
    registers[base + 0x04] >> 4, registers[base + 0x04] & 0x0fu,
    registers[base + 0x06], registers[base + 0x07],
    static_cast<u32>(registers[base + 0x06]) | (static_cast<u32>(registers[base + 0x07]) << 8),
    pan, pan >> 4, pan & 0x0fu, channel_control, channel_control_odd, (channel_control >> 2) & 0x07u,
    (channel_control & 0x20u) != 0, (channel_control_odd & 0x01u) != 0, channel_control >> 6,
    volume_select, global_volume_register, global_volume,
    registers[0x210], registers[0x211], registers[0x212], registers[0x213],
    registers[0x228], registers[0x229], registers[0x22a], registers[0x22b],
    registers[0x224], registers[0x225], registers[K054539_CONTROL_REGISTER]);
}

void LogCryptKillerK054539RoutingWrite(u32 chip, u32 register_index, u8 value, u8 previous_value)
{
  if (!s_state.crypt_killer_route_trace_enabled || value == previous_value ||
      !IsK054539OutputRoutingRegister(register_index) ||
      s_state.crypt_killer_route_write_logs >= CRYPT_KILLER_ROUTE_WRITE_LOG_LIMIT)
  {
    return;
  }

  s_state.crypt_killer_route_write_logs++;
  DEV_LOG(
    "KonamiGQ.SoundCPU CryptKiller_route_write index={} chip={} register=0x{:03X} value=0x{:02X} "
    "previous=0x{:02X} pc=0x{:06X}",
    s_state.crypt_killer_route_write_logs, chip + 1, register_index, value, previous_value,
    m68k_get_reg(nullptr, M68K_REG_PC));
}

u64 SampleMagnitude(s32 value)
{
  const s64 wide = static_cast<s64>(value);
  return static_cast<u64>(wide < 0 ? -wide : wide);
}

const char* GetGQMuxFPathName(K054539::Chip::GQMuxFPath path)
{
  switch (path)
  {
    case K054539::Chip::GQMuxFPath::DLATC: return "DLAT_C";
    case K054539::Chip::GQMuxFPath::DLATB: return "DLAT_B";
    case K054539::Chip::GQMuxFPath::P13LAT: return "P13LAT";
    case K054539::Chip::GQMuxFPath::DLATA: return "DLAT_A";
    default: return "unknown";
  }
}

void FinishCryptKillerSignalTrace(u32 chip, u32 channel, const char* reason)
{
  if (!s_state.crypt_killer_route_trace_enabled ||
      chip >= s_state.crypt_killer_signal_trace.size() ||
      channel >= s_state.crypt_killer_signal_trace[chip].size())
  {
    return;
  }

  CryptKillerSignalTrace& trace = s_state.crypt_killer_signal_trace[chip][channel];
  if (!trace.active)
    return;

  if (s_state.crypt_killer_signal_summary_logs < CRYPT_KILLER_SIGNAL_SUMMARY_LOG_LIMIT)
  {
    s_state.crypt_killer_signal_summary_logs++;
    DEV_LOG(
      "KonamiGQ.SoundCPU CryptKiller_signal_summary index={} class='{}' baseline={} chip={} channel={} "
      "start=0x{:06X} pc=0x{:06X} reason='{}' frames={} volume=0x{:02X} reverb_volume=0x{:02X} "
      "rv_high={} rv_low={} effect06=0x{:02X} effect07=0x{:02X} effect_word=0x{:04X} "
      "pan=0x{:02X} channel_control=0x{:02X} global_volume=0x{:02X} effects224=0x{:02X} mux225=0x{:02X} raw_peak={} "
      "dry_peak_l={} dry_peak_r={} chip_peak_l={} chip_peak_r={} "
      "dsp_send_peak_l={} dsp_send_peak_r={} tms_return_peak_l={} tms_return_peak_r={} "
      "board_peak_l={} board_peak_r={}",
      s_state.crypt_killer_signal_summary_logs,
      ClassifyCryptKillerSound(trace.start_address, trace.sound_pc), trace.baseline, chip + 1, channel,
      trace.start_address, trace.sound_pc, reason, trace.frames, trace.volume, trace.reverb_volume,
      trace.reverb_volume >> 4, trace.reverb_volume & 0x0fu,
      trace.effect_param_06, trace.effect_param_07,
      static_cast<u32>(trace.effect_param_06) | (static_cast<u32>(trace.effect_param_07) << 8),
      trace.pan, trace.channel_control, trace.global_volume, trace.effects_control, trace.effects_mux, trace.raw_peak,
      trace.dry_peak[0], trace.dry_peak[1], trace.chip_peak[0], trace.chip_peak[1],
      trace.dsp_send_peak[0], trace.dsp_send_peak[1],
      trace.tms_return_peak[0], trace.tms_return_peak[1],
      trace.board_peak[0], trace.board_peak[1]);

    DEV_LOG(
      "KonamiGQ.SoundCPU CryptKiller_silicon_summary index={} class='{}' chip={} channel={} "
      "src_mux_a={} src_mux_b={} raw_pcm0={} raw_pcm1={} raw_pcm2={} raw_pcm3={} "
      "raw_pcm4={} raw_pcm5={} raw_pcm6={} raw_pcm7={} muxf_path='{}' "
      "muxf_shadow_peak={} muxf_shadow_valid_frames={} src_muxf_channel={} "
      "axdt_ch_e_peak={} axdt_ch_f_peak={} axdt_e_addr=0x{:02X} axdt_f_addr=0x{:02X} "
      "axdt_mix_e_peak={} axdt_mix_f_peak={} axdt_mix_valid_frames={} "
      "axdt_full_e_peak={} axdt_full_f_peak={} axdt_full_valid_frames={} "
      "axdt_closed_e_peak={} axdt_closed_f_peak={} axdt_closed_valid_frames={} "
      "axdt_closed_clip_l={} axdt_closed_clip_r={} "
      "axdt_rvv_e_peak={} axdt_rvv_f_peak={} axdt_rvv_valid_frames={} "
      "rvv_tms_l_peak={} rvv_tms_r_peak={} "
      "axdt_rvv_clip_l={} axdt_rvv_clip_r={} rvv_tms_clip_l={} rvv_tms_clip_r={} "
      "axdt_rvv_e_addr=0x{:02X} axdt_rvv_f_addr=0x{:02X} "
      "rvv_real_diff_peak_l={} rvv_real_diff_peak_r={} "
      "rvv_real_absdiff_sum_l={} rvv_real_absdiff_sum_r={} "
      "src_axda_a={} src_axda_b={} "
      "rege_a_redt={} rege_b_frdt={} rege_c_redl={} rege_d_frdl={} rege_e_axdt_a={} rege_f_axdt_b={}",
      s_state.crypt_killer_signal_summary_logs,
      ClassifyCryptKillerSound(trace.start_address, trace.sound_pc), chip + 1, channel,
      trace.source_slot_peak[0], trace.source_slot_peak[1],
      trace.raw_pcm_peak[0], trace.raw_pcm_peak[1], trace.raw_pcm_peak[2], trace.raw_pcm_peak[3],
      trace.raw_pcm_peak[4], trace.raw_pcm_peak[5], trace.raw_pcm_peak[6], trace.raw_pcm_peak[7],
      GetGQMuxFPathName(trace.muxf_path),
      trace.muxf_shadow_peak, trace.muxf_shadow_valid_frames,
      trace.source_slot_peak[2u + channel],
      trace.axdt_channel_contribution_peak[0], trace.axdt_channel_contribution_peak[1],
      trace.axdt_coefficient_address[0], trace.axdt_coefficient_address[1],
      trace.axdt_channel_mix_peak[0], trace.axdt_channel_mix_peak[1], trace.axdt_channel_mix_valid_frames,
      trace.axdt_full_mix_peak[0], trace.axdt_full_mix_peak[1], trace.axdt_full_mix_valid_frames,
      trace.axdt_closed_loop_peak[0], trace.axdt_closed_loop_peak[1], trace.axdt_closed_loop_valid_frames,
      trace.axdt_closed_loop_clip_frames[0], trace.axdt_closed_loop_clip_frames[1],
      trace.axdt_rvv_loop_peak[0], trace.axdt_rvv_loop_peak[1], trace.axdt_rvv_loop_valid_frames,
      trace.rvv_tms_return_peak[0], trace.rvv_tms_return_peak[1],
      trace.axdt_rvv_loop_clip_frames[0], trace.axdt_rvv_loop_clip_frames[1],
      trace.rvv_tms_clip_frames[0], trace.rvv_tms_clip_frames[1],
      static_cast<u8>(((trace.reverb_volume & 0x0fu) << 3) | 0x07u),
      static_cast<u8>((((trace.reverb_volume >> 4) & 0x0fu) << 3) | 0x07u),
      trace.rvv_tms_real_diff_peak[0], trace.rvv_tms_real_diff_peak[1],
      trace.rvv_tms_real_absdiff_sum[0], trace.rvv_tms_real_absdiff_sum[1],
      trace.source_slot_peak[10], trace.source_slot_peak[11],
      trace.output_plane_peak[0], trace.output_plane_peak[1], trace.output_plane_peak[2],
      trace.output_plane_peak[3], trace.output_plane_peak[4], trace.output_plane_peak[5]);

    const u64 invalid_frames = trace.frames - trace.axdt_channel_mix_valid_frames;
    if (invalid_frames != 0)
    {
      u8 invalid_channel_mask = 0;
      for (u32 invalid_channel = 0; invalid_channel < trace.axdt_mix_invalid_channel_frames.size();
           invalid_channel++)
      {
        if (trace.axdt_mix_invalid_channel_frames[invalid_channel] != 0)
          invalid_channel_mask |= static_cast<u8>(1u << invalid_channel);
      }

      DEV_LOG(
        "KonamiGQ.SoundCPU CryptKiller_mix_invalidators index={} class='{}' chip={} channel={} "
        "invalid_frames={} invalid_mask=0x{:02X} "
        "ch0={}/0x{:02X}/0x{:02X}/0x{:02X}/0x{:02X}/{} "
        "ch1={}/0x{:02X}/0x{:02X}/0x{:02X}/0x{:02X}/{} "
        "ch2={}/0x{:02X}/0x{:02X}/0x{:02X}/0x{:02X}/{} "
        "ch3={}/0x{:02X}/0x{:02X}/0x{:02X}/0x{:02X}/{} "
        "ch4={}/0x{:02X}/0x{:02X}/0x{:02X}/0x{:02X}/{} "
        "ch5={}/0x{:02X}/0x{:02X}/0x{:02X}/0x{:02X}/{} "
        "ch6={}/0x{:02X}/0x{:02X}/0x{:02X}/0x{:02X}/{} "
        "ch7={}/0x{:02X}/0x{:02X}/0x{:02X}/0x{:02X}/{}",
        s_state.crypt_killer_signal_summary_logs,
        ClassifyCryptKillerSound(trace.start_address, trace.sound_pc), chip + 1, channel,
        invalid_frames, invalid_channel_mask,
        trace.axdt_mix_invalid_channel_frames[0], trace.axdt_mix_invalid_path_mask[0],
        trace.axdt_mix_invalid_effect06[0], trace.axdt_mix_invalid_effect07[0],
        trace.axdt_mix_invalid_control[0], trace.axdt_mix_invalid_signature_changed[0],
        trace.axdt_mix_invalid_channel_frames[1], trace.axdt_mix_invalid_path_mask[1],
        trace.axdt_mix_invalid_effect06[1], trace.axdt_mix_invalid_effect07[1],
        trace.axdt_mix_invalid_control[1], trace.axdt_mix_invalid_signature_changed[1],
        trace.axdt_mix_invalid_channel_frames[2], trace.axdt_mix_invalid_path_mask[2],
        trace.axdt_mix_invalid_effect06[2], trace.axdt_mix_invalid_effect07[2],
        trace.axdt_mix_invalid_control[2], trace.axdt_mix_invalid_signature_changed[2],
        trace.axdt_mix_invalid_channel_frames[3], trace.axdt_mix_invalid_path_mask[3],
        trace.axdt_mix_invalid_effect06[3], trace.axdt_mix_invalid_effect07[3],
        trace.axdt_mix_invalid_control[3], trace.axdt_mix_invalid_signature_changed[3],
        trace.axdt_mix_invalid_channel_frames[4], trace.axdt_mix_invalid_path_mask[4],
        trace.axdt_mix_invalid_effect06[4], trace.axdt_mix_invalid_effect07[4],
        trace.axdt_mix_invalid_control[4], trace.axdt_mix_invalid_signature_changed[4],
        trace.axdt_mix_invalid_channel_frames[5], trace.axdt_mix_invalid_path_mask[5],
        trace.axdt_mix_invalid_effect06[5], trace.axdt_mix_invalid_effect07[5],
        trace.axdt_mix_invalid_control[5], trace.axdt_mix_invalid_signature_changed[5],
        trace.axdt_mix_invalid_channel_frames[6], trace.axdt_mix_invalid_path_mask[6],
        trace.axdt_mix_invalid_effect06[6], trace.axdt_mix_invalid_effect07[6],
        trace.axdt_mix_invalid_control[6], trace.axdt_mix_invalid_signature_changed[6],
        trace.axdt_mix_invalid_channel_frames[7], trace.axdt_mix_invalid_path_mask[7],
        trace.axdt_mix_invalid_effect06[7], trace.axdt_mix_invalid_effect07[7],
        trace.axdt_mix_invalid_control[7], trace.axdt_mix_invalid_signature_changed[7]);
    }
  }

  trace = {};
}

void StartCryptKillerSignalTrace(u32 chip, u32 channel, u32 sound_pc)
{
  if (!s_state.crypt_killer_route_trace_enabled ||
      chip >= s_state.crypt_killer_signal_trace.size() ||
      channel >= s_state.crypt_killer_signal_trace[chip].size())
  {
    return;
  }

  FinishCryptKillerSignalTrace(chip, channel, "rekey");

  const auto& registers = s_state.k054539_registers[chip];
  const u32 base = channel * K054539_CHANNEL_REGISTER_STRIDE;
  const u32 start_address = GetK054539ChannelStartAddress(chip, channel);
  const u8 channel_control = registers[0x200u + (channel * 2u)];
  const u32 volume_select = (~channel_control) & 0x03u;
  const char* sound_class = ClassifyCryptKillerSound(start_address, sound_pc);
  const bool targeted = std::string_view(sound_class) != "other";
  const bool baseline =
    !targeted && volume_select == 3u && start_address != 0 && registers[base + 0x03] != 0x7fu &&
    s_state.crypt_killer_signal_baseline_tracks < CRYPT_KILLER_SIGNAL_BASELINE_TRACK_LIMIT;

  if (!targeted && !baseline)
    return;

  if (baseline)
    s_state.crypt_killer_signal_baseline_tracks++;

  CryptKillerSignalTrace& trace = s_state.crypt_killer_signal_trace[chip][channel];
  trace.active = true;
  trace.baseline = baseline;
  trace.start_address = start_address;
  trace.sound_pc = sound_pc;
  trace.volume = registers[base + 0x03];
  trace.reverb_volume = registers[base + 0x04];
  trace.pan = registers[base + 0x05];
  trace.effect_param_06 = registers[base + 0x06];
  trace.effect_param_07 = registers[base + 0x07];
  trace.channel_control = channel_control;
  trace.global_volume = registers[0x228u + volume_select] & 0x7fu;
  trace.effects_control = registers[0x224];
  trace.effects_mux = registers[0x225];
}

void UpdateCryptKillerSignalTraces(const NativeAudioFrame& frame)
{
  if (!s_state.crypt_killer_route_trace_enabled)
    return;

  for (u32 chip = 0; chip < s_state.crypt_killer_signal_trace.size(); chip++)
  {
    for (u32 channel = 0; channel < s_state.crypt_killer_signal_trace[chip].size(); channel++)
    {
      CryptKillerSignalTrace& trace = s_state.crypt_killer_signal_trace[chip][channel];
      if (!trace.active)
        continue;

      trace.frames++;
      trace.raw_peak = std::max(trace.raw_peak, SampleMagnitude(frame.raw_channel[chip][channel]));
      for (u32 raw_channel = 0; raw_channel < frame.raw_channel[chip].size(); raw_channel++)
      {
        trace.raw_pcm_peak[raw_channel] =
          std::max(trace.raw_pcm_peak[raw_channel], SampleMagnitude(frame.raw_channel[chip][raw_channel]));
      }
      if (frame.muxf_path[chip][channel] != K054539::Chip::GQMuxFPath::Unknown)
        trace.muxf_path = frame.muxf_path[chip][channel];
      if (frame.muxf_shadow_valid[chip][channel])
      {
        trace.muxf_shadow_valid_frames++;
        trace.muxf_shadow_peak =
          std::max(trace.muxf_shadow_peak, SampleMagnitude(frame.muxf_shadow[chip][channel]));
        for (u32 axdt_word = 0; axdt_word < 2; axdt_word++)
        {
          trace.axdt_channel_contribution_peak[axdt_word] = std::max(
            trace.axdt_channel_contribution_peak[axdt_word],
            SampleMagnitude(frame.axdt_channel_contribution[chip][channel][axdt_word]));
          trace.axdt_coefficient_address[axdt_word] =
            frame.axdt_coefficient_address[chip][channel][axdt_word];
        }
      }
      if (frame.axdt_channel_mix_valid[chip])
      {
        trace.axdt_channel_mix_valid_frames++;
        for (u32 axdt_word = 0; axdt_word < 2; axdt_word++)
        {
          trace.axdt_channel_mix_peak[axdt_word] =
            std::max(trace.axdt_channel_mix_peak[axdt_word],
                     SampleMagnitude(frame.axdt_channel_mix[chip][axdt_word]));
        }
      }
      else
      {
        // The aggregate validity is chip-wide. Record which active channel
        // prevented an exact slots-2..9 result during this tracked sound.
        // muxf_path remains Unknown for inactive channels, while a non-Unknown
        // path with no shadow-valid flag is exactly the current unresolved
        // channel source.
        const auto& registers = s_state.k054539_registers[chip];
        for (u32 invalid_channel = 0; invalid_channel < frame.muxf_path[chip].size(); invalid_channel++)
        {
          const K054539::Chip::GQMuxFPath invalid_path = frame.muxf_path[chip][invalid_channel];
          if (invalid_path == K054539::Chip::GQMuxFPath::Unknown ||
              frame.muxf_shadow_valid[chip][invalid_channel])
          {
            continue;
          }

          trace.axdt_mix_invalid_channel_frames[invalid_channel]++;
          const u8 path_index = static_cast<u8>(invalid_path);
          if (path_index < 4u)
            trace.axdt_mix_invalid_path_mask[invalid_channel] |= static_cast<u8>(1u << path_index);

          const u32 invalid_base = invalid_channel * K054539_CHANNEL_REGISTER_STRIDE;
          const u8 effect06 = registers[invalid_base + 0x06u];
          const u8 effect07 = registers[invalid_base + 0x07u];
          const u8 control = registers[0x200u + (invalid_channel * 2u)];

          if (!trace.axdt_mix_invalid_signature_seen[invalid_channel])
          {
            trace.axdt_mix_invalid_signature_seen[invalid_channel] = 1u;
            trace.axdt_mix_invalid_effect06[invalid_channel] = effect06;
            trace.axdt_mix_invalid_effect07[invalid_channel] = effect07;
            trace.axdt_mix_invalid_control[invalid_channel] = control;
          }
          else if (trace.axdt_mix_invalid_effect06[invalid_channel] != effect06 ||
                   trace.axdt_mix_invalid_effect07[invalid_channel] != effect07 ||
                   trace.axdt_mix_invalid_control[invalid_channel] != control)
          {
            trace.axdt_mix_invalid_signature_changed[invalid_channel] = 1u;
          }
        }
      }
      if (frame.axdt_full_mix_valid[chip])
      {
        trace.axdt_full_mix_valid_frames++;
        for (u32 axdt_word = 0; axdt_word < 2; axdt_word++)
        {
          trace.axdt_full_mix_peak[axdt_word] =
            std::max(trace.axdt_full_mix_peak[axdt_word],
                     SampleMagnitude(frame.axdt_full_mix[chip][axdt_word]));
        }
      }
      if (frame.axdt_closed_loop_mix_valid[chip])
      {
        trace.axdt_closed_loop_valid_frames++;
        for (u32 axdt_word = 0; axdt_word < 2; axdt_word++)
        {
          const s32 closed_value = frame.axdt_closed_loop_mix[chip][axdt_word];
          trace.axdt_closed_loop_peak[axdt_word] =
            std::max(trace.axdt_closed_loop_peak[axdt_word], SampleMagnitude(closed_value));
          if (closed_value <= -32767 || closed_value >= 32767)
            trace.axdt_closed_loop_clip_frames[axdt_word]++;
        }
      }
      if (frame.axdt_rvv_loop_mix_valid[chip])
      {
        trace.axdt_rvv_loop_valid_frames++;
        for (u32 axdt_word = 0; axdt_word < 2; axdt_word++)
        {
          const s32 rvv_value = frame.axdt_rvv_loop_mix[chip][axdt_word];
          trace.axdt_rvv_loop_peak[axdt_word] =
            std::max(trace.axdt_rvv_loop_peak[axdt_word], SampleMagnitude(rvv_value));
          if (rvv_value <= -32767 || rvv_value >= 32767)
            trace.axdt_rvv_loop_clip_frames[axdt_word]++;
        }
      }
      for (u32 axdt_word = 0; axdt_word < 2; axdt_word++)
      {
        const s32 rvv_tms_value = frame.rvv_tms[(chip * 2u) + axdt_word];
        const s32 real_value = frame.tms[(chip * 2u) + axdt_word];
        trace.rvv_tms_return_peak[axdt_word] =
          std::max(trace.rvv_tms_return_peak[axdt_word], SampleMagnitude(rvv_tms_value));
        if (rvv_tms_value <= -32767 || rvv_tms_value >= 32767)
          trace.rvv_tms_clip_frames[axdt_word]++;

        const s64 rvv_diff = static_cast<s64>(rvv_tms_value) - static_cast<s64>(real_value);
        const u64 rvv_diff_magnitude = static_cast<u64>(rvv_diff < 0 ? -rvv_diff : rvv_diff);
        trace.rvv_tms_real_diff_peak[axdt_word] =
          std::max(trace.rvv_tms_real_diff_peak[axdt_word], rvv_diff_magnitude);
        trace.rvv_tms_real_absdiff_sum[axdt_word] += rvv_diff_magnitude;
      }
      for (u32 slot = 0; slot < frame.source_slot[chip].size(); slot++)
      {
        trace.source_slot_peak[slot] =
          std::max(trace.source_slot_peak[slot], SampleMagnitude(frame.source_slot[chip][slot]));
      }
      for (u32 plane = 0; plane < frame.output_plane[chip].size(); plane++)
      {
        trace.output_plane_peak[plane] =
          std::max(trace.output_plane_peak[plane], SampleMagnitude(frame.output_plane[chip][plane]));
      }

      for (u32 output_channel = 0; output_channel < 2; output_channel++)
      {
        trace.dry_peak[output_channel] =
          std::max(trace.dry_peak[output_channel], SampleMagnitude(frame.channel[chip][channel][output_channel]));
        trace.chip_peak[output_channel] =
          std::max(trace.chip_peak[output_channel], SampleMagnitude(frame.chip[chip][output_channel]));
        trace.dsp_send_peak[output_channel] =
          std::max(trace.dsp_send_peak[output_channel], SampleMagnitude(frame.dsp_send[chip][output_channel]));
        trace.tms_return_peak[output_channel] =
          std::max(trace.tms_return_peak[output_channel], SampleMagnitude(frame.tms[(chip * 2u) + output_channel]));
        trace.board_peak[output_channel] =
          std::max(trace.board_peak[output_channel], SampleMagnitude(frame.output[output_channel]));
      }
    }
  }
}

void FinishInactiveCryptKillerSignalTraces()
{
  if (!s_state.crypt_killer_route_trace_enabled)
    return;

  for (u32 chip = 0; chip < s_state.k054539_chips.size(); chip++)
  {
    const u8 active = s_state.k054539_chips[chip].ReadRegister(K054539_ACTIVE_REGISTER);
    for (u32 channel = 0; channel < 8; channel++)
    {
      if ((active & (1u << channel)) == 0)
        FinishCryptKillerSignalTrace(chip, channel, "sample_end");
    }
  }
}

void GenerateSynchronizedNativeAudioFrame()
{
  NativeAudioFrame frame;
  const bool tms_active = s_state.tms57002 && s_state.tms57002->IsResetReleased();
  const bool rvv_tms_active = s_state.rvv_tms57002 && s_state.rvv_tms57002->IsResetReleased();

  // Feed the TMS57002 return channels back into the two K054539 devices.
  // This is the established audible return path used by the current GQ model.
  if (tms_active)
  {
    for (u32 channel = 0; channel < frame.tms.size(); channel++)
      frame.tms[channel] = s_state.tms57002->GetSerialOutput(channel);

    s_state.k054539_chips[0].SetAuxInput(frame.tms[0], frame.tms[1]);
    s_state.k054539_chips[1].SetAuxInput(frame.tms[2], frame.tms[3]);
  }
  else
  {
    for (K054539::Chip& chip : s_state.k054539_chips)
      chip.SetAuxInput(0, 0);
  }

  // The rejected flattened-shadow TMS execution model is intentionally inactive.
  // Keep its K054539 feedback input at zero so it cannot influence the audible path.
  for (K054539::Chip& chip : s_state.k054539_chips)
    chip.SetGQShadowAuxInput(0, 0);

  // Keep the optional RVVOL/AXDT diagnostic domain isolated from the audible path.
  if (rvv_tms_active)
  {
    for (u32 channel = 0; channel < frame.rvv_tms.size(); channel++)
      frame.rvv_tms[channel] = s_state.rvv_tms57002->GetSerialOutput(channel);

    s_state.k054539_chips[0].SetGQRVVAuxInput(frame.rvv_tms[0], frame.rvv_tms[1]);
    s_state.k054539_chips[1].SetGQRVVAuxInput(frame.rvv_tms[2], frame.rvv_tms[3]);
  }
  else
  {
    for (K054539::Chip& chip : s_state.k054539_chips)
      chip.SetGQRVVAuxInput(0, 0);
  }

  for (u32 chip = 0; chip < s_state.k054539_chips.size(); chip++)
  {
    // Request only the outputs required by the current audible path. Additional
    // silicon-model capture outputs are diagnostic-only and remain disabled.
    s_state.k054539_chips[chip].GenerateFrame(
      &frame.chip[chip][0], &frame.chip[chip][1], &frame.channel[chip],
      &frame.dsp_send[chip][0], &frame.dsp_send[chip][1],
      nullptr, nullptr, nullptr, &frame.muxf_path[chip], nullptr, &frame.muxf_shadow_valid[chip],
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr, &frame.axdt_signed_channel_contribution[chip],
      CRYPT_KILLER_AXDA_GAIN);

    // Apply the reference-tuned Crypt Killer voice and gameplay-SFX balance.
    // The same gain follows each selected channel's dry contribution and
    // resolved signed-DLAT_A TMS send so its dry/effect relationship is
    // preserved. Music, title audio, and service tones remain unchanged.
    {
      const u8 active = s_state.k054539_chips[chip].ReadRegister(K054539_ACTIVE_REGISTER);
      for (u32 channel = 0; channel < frame.channel[chip].size(); channel++)
      {
        if ((active & (1u << channel)) == 0)
          continue;

        s32 gain_q12 = CRYPT_KILLER_GAIN_ONE_Q12;
        if (chip == 1)
          gain_q12 = GetCryptKillerVoiceGainQ12(GetK054539ChannelStartAddress(chip, channel));

        if (gain_q12 == CRYPT_KILLER_GAIN_ONE_Q12 &&
            s_state.crypt_killer_gameplay_sfx_active[chip][channel])
        {
          gain_q12 = CRYPT_KILLER_GAMEPLAY_SFX_GAIN_Q12;
        }

        if (gain_q12 == CRYPT_KILLER_GAIN_ONE_Q12)
          continue;

        for (u32 output_channel = 0; output_channel < 2; output_channel++)
        {
          const s32 original = frame.channel[chip][channel][output_channel];
          const s32 boosted = ApplyCryptKillerBalanceGain(original, gain_q12);
          frame.channel[chip][channel][output_channel] = boosted;
          frame.chip[chip][output_channel] += boosted - original;
        }

        for (u32 word = 0; word < 2; word++)
        {
          frame.axdt_signed_channel_contribution[chip][channel][word] =
            ApplyCryptKillerBalanceGain(
              frame.axdt_signed_channel_contribution[chip][channel][word], gain_q12);
        }
      }
    }

    frame.output[0] += frame.chip[chip][0];
    frame.output[1] += frame.chip[chip][1];
  }

  frame.output[0] = ApplyK056602OutputGain(ApplyK056800CommonVolume(frame.output[0]));
  frame.output[1] = ApplyK056602OutputGain(ApplyK056800CommonVolume(frame.output[1]));

  if (tms_active)
  {
    // The K054539-to-TMS57002 send is only partially reconstructed. For chip 2,
    // the resolved portion is the regular-channel DLAT_A path for slots 2-9
    // using signed MULB interpretation. Unresolved chip-2 paths and the special
    // MUXH/AXDA slots are intentionally omitted rather than guessed. Chip 1
    // continues to use the established stereo send. Board dry output is
    // unaffected by this partial DSP-input model.
    std::array<u16, 2> chip2_signed_dlata_accumulator = {};
    for (u32 channel = 0; channel < 8; channel++)
    {
      if (frame.muxf_shadow_valid[1][channel] == 0 ||
          frame.muxf_path[1][channel] != K054539::Chip::GQMuxFPath::DLATA)
      {
        continue;
      }

      for (u32 word = 0; word < 2; word++)
      {
        chip2_signed_dlata_accumulator[word] =
          static_cast<u16>(
            chip2_signed_dlata_accumulator[word] +
            static_cast<u16>(frame.axdt_signed_channel_contribution[1][channel][word]));
      }
    }

    const auto signed_dlata_dasp_input = [](u16 sample) {
      return static_cast<s32>(static_cast<s16>(sample)) / 2;
    };

    s_state.tms57002->SetSerialInputs(
      frame.dsp_send[0][0], frame.dsp_send[0][1],
      signed_dlata_dasp_input(chip2_signed_dlata_accumulator[0]),
      signed_dlata_dasp_input(chip2_signed_dlata_accumulator[1]));

    if ((s_state.sound_control & 0x20u) != 0)
    {
      s_state.tms57002->Sync();
      s_state.tms_syncs++;
    }

    s_state.tms57002->Execute(TMS_CYCLES_PER_SYNC);
  }

  if (rvv_tms_active)
  {
    const auto rvv_dasp_input = [](s32 sample) { return sample / 2; };
    s_state.rvv_tms57002->SetSerialInputs(
      frame.axdt_rvv_loop_mix_valid[0] ? rvv_dasp_input(frame.axdt_rvv_loop_mix[0][0]) : 0,
      frame.axdt_rvv_loop_mix_valid[0] ? rvv_dasp_input(frame.axdt_rvv_loop_mix[0][1]) : 0,
      frame.axdt_rvv_loop_mix_valid[1] ? rvv_dasp_input(frame.axdt_rvv_loop_mix[1][0]) : 0,
      frame.axdt_rvv_loop_mix_valid[1] ? rvv_dasp_input(frame.axdt_rvv_loop_mix[1][1]) : 0);

    if ((s_state.sound_control & 0x20u) != 0)
      s_state.rvv_tms57002->Sync();

    s_state.rvv_tms57002->Execute(TMS_CYCLES_PER_SYNC);
  }

  UpdateCryptKillerSignalTraces(frame);
  FinishInactiveCryptKillerSignalTraces();
  QueueNativeAudioFrame(frame);
}

u8 ReadK054539DataPort(u32 chip)
{
  const u32 pointer = s_state.k054539_data_pointer[chip];
  const u8 bank = s_state.k054539_data_bank[chip];

  // The chip owns the live register file, data pointer, and reverb RAM.
  const u8 value = s_state.k054539_chips[chip].ReadRegister(K054539_DATA_REGISTER);

  if (bank == 0x80)
    s_state.k054539_reverb_reads++;

  s_state.k054539_data_reads++;
  s_state.k054539_data_pointer[chip] = (pointer + 1) & K054539_DATA_POINTER_MASK;
  if ((pointer == 0 || s_state.k054539_data_pointer[chip] == 0) && ShouldLogK054539Data())
  {
    DEV_LOG(
      "KonamiGQ.SoundCPU K054539_data_read_{} chip={} bank=0x{:02X} pointer=0x{:05X} value=0x{:02X} "
      "total_reads={}",
      pointer == 0 ? "started" : "bank_completed", chip + 1, bank, pointer, value,
      s_state.k054539_data_reads);
  }
  return value;
}

void WriteK054539DataPort(u32 chip, u8 value)
{
  const u32 pointer = s_state.k054539_data_pointer[chip];
  const u8 bank = s_state.k054539_data_bank[chip];

  // WriteRegister() has already updated the chip-owned data pointer and reverb RAM.
  if (bank == 0x80)
    s_state.k054539_reverb_writes++;

  s_state.k054539_data_writes++;
  s_state.k054539_data_pointer[chip] = (pointer + 1) & K054539_DATA_POINTER_MASK;
  if ((pointer == 0 || s_state.k054539_data_pointer[chip] == 0) && ShouldLogK054539Data())
  {
    DEV_LOG(
      "KonamiGQ.SoundCPU K054539_data_write_{} chip={} bank=0x{:02X} pointer=0x{:05X} value=0x{:02X} "
      "reverb_selected={} total_writes={}",
      pointer == 0 ? "started" : "bank_completed", chip + 1, bank, pointer, value, bank == 0x80,
      s_state.k054539_data_writes);
  }
}


void AdvanceTMS57002(int executed_cycles)
{
  if (executed_cycles <= 0)
    return;

  const u64 new_cycles =
    static_cast<u64>(executed_cycles) * static_cast<u64>(TMS_CYCLES_PER_SOUND_CPU_CYCLE);
  s_state.tms_scheduled_cycles += new_cycles;
  s_state.tms_cycle_balance += static_cast<u32>(new_cycles);

  while (s_state.tms_cycle_balance >= TMS_CYCLES_PER_SYNC)
  {
    GenerateSynchronizedNativeAudioFrame();
    s_state.tms_cycle_balance -= TMS_CYCLES_PER_SYNC;
  }
}

u32 ReadBigEndian32(const std::vector<u8>& data, u32 offset)
{
  return (static_cast<u32>(data[offset]) << 24) | (static_cast<u32>(data[offset + 1]) << 16) |
         (static_cast<u32>(data[offset + 2]) << 8) | static_cast<u32>(data[offset + 3]);
}

bool HasPlausibleResetVectors(const std::vector<u8>& rom, u32* initial_sp, u32* initial_pc)
{
  if (rom.size() < 8)
    return false;

  const u32 sp = ReadBigEndian32(rom, 0);
  const u32 pc = ReadBigEndian32(rom, 4);
  if (initial_sp)
    *initial_sp = sp;
  if (initial_pc)
    *initial_pc = pc;

  return sp >= RAM_BASE && sp <= (RAM_BASE + RAM_SIZE) && (sp & 1u) == 0 && pc < ROM_SIZE && (pc & 1u) == 0;
}

u32 GetIRQLevel()
{
  if (s_state.k054539_irq2_pending)
    return 2;

  return KonamiGQK056800::IsSoundInterruptPending() ? 1u : 0u;
}

void UpdateIRQLine()
{
  if (s_state.active)
    m68k_set_irq(GetIRQLevel());
}

void ClearK054539IRQ2(const char* reason)
{
  if (!s_state.k054539_irq2_pending)
    return;

  s_state.k054539_irq2_pending = false;
  s_state.k054539_irq2_clears++;
  if (ShouldLogK054539IRQ(s_state.k054539_irq2_clears))
  {
    DEV_LOG("KonamiGQ.SoundCPU K054539_irq2_clear index={} reason='{}' pc=0x{:06X}",
             s_state.k054539_irq2_clears, reason, m68k_get_reg(nullptr, M68K_REG_PC));
  }
  UpdateIRQLine();
}

void AdvanceK054539Timers(int executed_cycles)
{
  if (executed_cycles <= 0 || !s_state.k054539_timer_configured[0])
    return;

  const u8 timer_value = s_state.k054539_registers[0][K054539_TIMER_REGISTER];
  const u64 rate_numerator = static_cast<u64>(38u + timer_value) * 20u;
  const u64 accumulator =
    s_state.k054539_timer_phase[0] + (static_cast<u64>(executed_cycles) * rate_numerator);
  const u64 toggles = accumulator / K054539_TIMER_PHASE_DENOMINATOR;
  s_state.k054539_timer_phase[0] = accumulator % K054539_TIMER_PHASE_DENOMINATOR;
  if (toggles == 0)
    return;

  if ((s_state.k054539_registers[0][K054539_CONTROL_REGISTER] & K054539_TIMER_OUTPUT_ENABLE) == 0)
    return;

  const u8 old_state = s_state.k054539_timer_state[0];
  const u64 rising_edges = old_state ? (toggles / 2u) : ((toggles + 1u) / 2u);
  s_state.k054539_timer_state[0] ^= static_cast<u8>(toggles & 1u);
  s_state.k054539_timer_toggles += toggles;
  s_state.k054539_timer_rising_edges += rising_edges;

  if (rising_edges != 0 && (s_state.sound_control & 0x01u) != 0 && !s_state.k054539_irq2_pending)
  {
    s_state.k054539_irq2_pending = true;
    s_state.k054539_irq2_assertions++;
    if (ShouldLogK054539IRQ(s_state.k054539_irq2_assertions))
    {
      DEV_LOG(
        "KonamiGQ.SoundCPU K054539_irq2_assert index={} timer_value=0x{:02X} toggles={} rising_edges={} "
        "timer_state={} pc=0x{:06X}",
        s_state.k054539_irq2_assertions, timer_value, toggles, rising_edges, s_state.k054539_timer_state[0],
        m68k_get_reg(nullptr, M68K_REG_PC));
    }
  }
}

u8 GetTMSStatus()
{
  if (!s_state.tms57002)
    return 0x05;

  return static_cast<u8>((s_state.tms57002->DataReady() ? 0x04u : 0u) |
                         (s_state.tms57002->ProgramCounterNonZero() ? 0x02u : 0u) |
                         (s_state.tms57002->UpdateFIFOEmpty() ? 0x01u : 0u));
}


void LogTMSStatusPoll(u8 status)
{
  const u64 read_count = s_state.tms_reads;
  if (s_state.tms_status_poll_logs >= TMS_STATUS_POLL_LOG_LIMIT ||
      (read_count != 1 && (read_count & (read_count - 1)) != 0))
  {
    return;
  }

  s_state.tms_status_poll_logs++;
  const u32 pc = m68k_get_reg(nullptr, M68K_REG_PC) & ADDRESS_MASK;
  std::array<u8, 12> opcode_bytes = {};
  for (u32 i = 0; i < opcode_bytes.size(); i++)
  {
    const u32 address = (pc + i) & ADDRESS_MASK;
    opcode_bytes[i] = (address < s_state.rom.size()) ? s_state.rom[address] : 0xff;
  }

  DEV_LOG(
    "KonamiGQ.SoundCPU TMS57002_status_poll index={} reads={} status=0x{:02X} dready={} pc0={} empty={} "
    "pc=0x{:06X} opcodes='{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}'",
    s_state.tms_status_poll_logs, read_count, status, (status & 0x04u) != 0, (status & 0x02u) != 0,
    (status & 0x01u) != 0, pc, opcode_bytes[0], opcode_bytes[1], opcode_bytes[2], opcode_bytes[3],
    opcode_bytes[4], opcode_bytes[5], opcode_bytes[6], opcode_bytes[7], opcode_bytes[8], opcode_bytes[9],
    opcode_bytes[10], opcode_bytes[11]);
}

void LogUnmapped(bool write, u32 address, u32 value)
{
  if (write)
    s_state.unmapped_writes++;
  else
    s_state.unmapped_reads++;

  if (!s_state.first_unmapped_access_logged)
  {
    s_state.first_unmapped_access_logged = true;
    DEV_LOG("KonamiGQ.SoundCPU first_unmapped_access operation='{}' address=0x{:06X} value=0x{:08X} pc=0x{:06X}",
             write ? "write" : "read", address, value, m68k_get_reg(nullptr, M68K_REG_PC));
  }

  if (ShouldLogAccess())
  {
    DEV_LOG("KonamiGQ.SoundCPU unmapped_{} address=0x{:06X} value=0x{:08X} pc=0x{:06X}",
             write ? "write" : "read", address, value, m68k_get_reg(nullptr, M68K_REG_PC));
  }
}

u8 ReadByte(u32 address)
{
  address &= ADDRESS_MASK;

  if (address < ROM_SIZE)
    return s_state.rom[address];

  if (address >= RAM_BASE && address < (RAM_BASE + RAM_SIZE))
    return s_state.ram[address - RAM_BASE];

  if (address >= K054539_BASE && address <= K054539_END)
  {
    const u32 register_index = (address - K054539_BASE) >> 1;
    const u32 chip = address & 1u;
    const u8 value =
      (register_index == K054539_DATA_REGISTER) ?
        ((s_state.k054539_registers[chip][K054539_CONTROL_REGISTER] & 0x10u) != 0 ?
           ReadK054539DataPort(chip) : 0) :
        s_state.k054539_chips[chip].ReadRegister(register_index);
    s_state.k054539_reads++;
    if (!s_state.first_k054539_access_logged)
    {
      s_state.first_k054539_access_logged = true;
      DEV_LOG("KonamiGQ.SoundCPU K054539_first_access operation='read' chip={} register=0x{:03X} value=0x{:02X}",
               chip + 1, register_index, value);
    }
    return value;
  }

  if (address == TMS_DATA_ADDRESS)
  {
    const u8 value = s_state.tms57002 ? s_state.tms57002->DataRead() : 0xff;
    if (s_state.rvv_tms57002)
      static_cast<void>(s_state.rvv_tms57002->DataRead());
    s_state.tms_reads++;
    if (!s_state.first_tms_access_logged)
    {
      s_state.first_tms_access_logged = true;
      DEV_LOG("KonamiGQ.SoundCPU TMS57002_first_access operation='data_read' value=0x{:02X}", value);
    }
    return value;
  }

  if (address >= K056800_BASE && address <= K056800_END)
  {
    if ((address & 1u) == 0)
      return 0xff;

    return KonamiGQK056800::ReadSound((address - K056800_BASE) >> 1);
  }

  if (address >= TMS_STATUS_BASE && address <= TMS_STATUS_END)
  {
    const u8 status = GetTMSStatus();
    s_state.tms_reads++;
    if (!s_state.first_tms_access_logged)
    {
      s_state.first_tms_access_logged = true;
      DEV_LOG("KonamiGQ.SoundCPU TMS57002_first_access operation='status_read' status=0x{:02X}", status);
    }
    if ((address & 1u) != 0)
      LogTMSStatusPoll(status);
    return (address & 1u) ? status : 0x00;
  }

  if (address >= NRES_BASE && address <= NRES_END)
    return 0xff;

  LogUnmapped(false, address, 0);
  return 0xff;
}

void WriteByte(u32 address, u8 value)
{
  address &= ADDRESS_MASK;

  if (address < ROM_SIZE)
  {
    LogUnmapped(true, address, value);
    return;
  }

  if (address >= RAM_BASE && address < (RAM_BASE + RAM_SIZE))
  {
    s_state.ram[address - RAM_BASE] = value;
    return;
  }

  if (address >= K054539_BASE && address <= K054539_END)
  {
    const u32 register_index = (address - K054539_BASE) >> 1;
    const u32 chip = address & 1u;
    const u8 previous_value = s_state.k054539_registers[chip][register_index];
    s_state.k054539_registers[chip][register_index] = value;
    s_state.k054539_chips[chip].WriteRegister(register_index, value);
    LogCryptKillerK054539RoutingWrite(chip, register_index, value, previous_value);
    if (register_index == K054539_KEY_ON_REGISTER)
    {
      s_state.k054539_registers[chip][K054539_ACTIVE_REGISTER] |= value;
      const u32 sound_pc = m68k_get_reg(nullptr, M68K_REG_PC);
      for (u32 channel = 0; channel < 8; channel++)
      {
        if ((value & (1u << channel)) != 0)
        {
          const u32 start_address = GetK054539ChannelStartAddress(chip, channel);
          s_state.crypt_killer_gameplay_sfx_active[chip][channel] =
            IsCryptKillerGameplaySFX(start_address, sound_pc);
          LogCryptKillerK054539KeyOn(chip, channel, sound_pc);
          StartCryptKillerSignalTrace(chip, channel, sound_pc);
        }
      }
    }
    else if (register_index == K054539_KEY_OFF_REGISTER)
    {
      s_state.k054539_registers[chip][K054539_ACTIVE_REGISTER] &= static_cast<u8>(~value);
      for (u32 channel = 0; channel < 8; channel++)
      {
        if ((value & (1u << channel)) == 0)
          continue;

        s_state.crypt_killer_gameplay_sfx_active[chip][channel] = false;
        FinishCryptKillerSignalTrace(chip, channel, "key_off");
      }
    }
    s_state.k054539_writes++;
    if (!s_state.first_k054539_access_logged)
    {
      s_state.first_k054539_access_logged = true;
      DEV_LOG("KonamiGQ.SoundCPU K054539_first_access operation='write' chip={} register=0x{:03X} value=0x{:02X}",
               chip + 1, register_index, value);
    }

    if (s_state.k054539_write_logs < K054539_WRITE_LOG_LIMIT)
    {
      s_state.k054539_write_logs++;
      DEV_LOG(
        "KonamiGQ.SoundCPU K054539_write index={} chip={} register=0x{:03X} value=0x{:02X} previous=0x{:02X} "
        "pc=0x{:06X}",
        s_state.k054539_write_logs, chip + 1, register_index, value, previous_value,
        m68k_get_reg(nullptr, M68K_REG_PC));
    }

    if (register_index == K054539_DATA_REGISTER)
    {
      WriteK054539DataPort(chip, value);
    }
    else if (register_index == K054539_BANK_REGISTER)
    {
      s_state.k054539_data_bank[chip] = value;
      s_state.k054539_data_pointer[chip] = 0;
      if (ShouldLogK054539Data())
      {
        DEV_LOG(
          "KonamiGQ.SoundCPU K054539_data_bank chip={} bank=0x{:02X} source='{}' pointer_reset=true "
          "pc=0x{:06X}",
          chip + 1, value,
          value == 0x80 ? "reverb_ram" :
            ((static_cast<u32>(value) * K054539_BANK_SIZE) < K054539_SAMPLE_ROM_SIZE ? "sample_rom" : "pcm_ram"),
          m68k_get_reg(nullptr, M68K_REG_PC));
      }
    }
    else if (register_index == K054539_TIMER_REGISTER)
    {
      s_state.k054539_timer_configured[chip] = true;
      s_state.k054539_timer_phase[chip] = 0;
      s_state.k054539_timer_state[chip] = 0;
      const double output_hz =
        static_cast<double>(38u + value) * static_cast<double>(K054539_CLOCK_HZ) /
        (384.0 * 14400.0);
      DEV_LOG(
        "KonamiGQ.SoundCPU K054539_timer_configured chip={} register_value=0x{:02X} "
        "callback_hz={:.6f} output_hz={:.6f}",
        chip + 1, value, output_hz * 2.0, output_hz);
    }
    else if (register_index == K054539_CONTROL_REGISTER)
    {
      if ((value & K054539_TIMER_OUTPUT_ENABLE) == 0)
        s_state.k054539_timer_state[chip] = 0;

      DEV_LOG("KonamiGQ.SoundCPU K054539_global_control chip={} value=0x{:02X} pcm_enabled={} "
               "timer_output_enabled={}",
               chip + 1, value, (value & 0x01u) != 0, (value & K054539_TIMER_OUTPUT_ENABLE) != 0);
    }
    return;
  }

  if (address == TMS_DATA_ADDRESS)
  {
    if (s_state.tms57002)
      s_state.tms57002->DataWrite(value);
    if (s_state.rvv_tms57002)
      s_state.rvv_tms57002->DataWrite(value);
    s_state.tms_writes++;
    if (!s_state.first_tms_access_logged)
    {
      s_state.first_tms_access_logged = true;
      DEV_LOG("KonamiGQ.SoundCPU TMS57002_first_access operation='data_write' value=0x{:02X}", value);
    }
    return;
  }

  if (address >= K056800_BASE && address <= K056800_END)
  {
    if ((address & 1u) == 0)
      return;

    const u32 reg = (address - K056800_BASE) >> 1;
    KonamiGQK056800::WriteSound(reg, value);
    if (reg == 1 && s_state.executing)
      m68k_end_timeslice();
    return;
  }

  if (address >= TMS_STATUS_BASE && address <= TMS_STATUS_END)
  {
    if ((address & 1u) == 0)
      return;

    if ((value & 0x01u) == 0)
      ClearK054539IRQ2("sound_control_bit0_clear");

    s_state.sound_control = value;
    if (s_state.tms57002)
    {
      s_state.tms57002->PLoadWrite((value & 0x04u) != 0);
      s_state.tms57002->CLoadWrite((value & 0x08u) != 0);
      s_state.tms57002->SetResetReleased((value & 0x10u) != 0);
    }
    if (s_state.rvv_tms57002)
    {
      s_state.rvv_tms57002->PLoadWrite((value & 0x04u) != 0);
      s_state.rvv_tms57002->CLoadWrite((value & 0x08u) != 0);
      s_state.rvv_tms57002->SetResetReleased((value & 0x10u) != 0);
    }
    s_state.tms_writes++;
    if (!s_state.first_tms_access_logged)
    {
      s_state.first_tms_access_logged = true;
      DEV_LOG("KonamiGQ.SoundCPU TMS57002_first_access operation='control_write' value=0x{:02X}", value);
    }
    if (ShouldLogAccess())
    {
      DEV_LOG("KonamiGQ.SoundCPU TMS57002_control value=0x{:02X} pload={} cload={} reset_released={} irq2_enabled={}",
               value, (value & 0x04u) != 0, (value & 0x08u) != 0, (value & 0x10u) != 0,
               (value & 0x01u) != 0);
    }
    return;
  }

  if (address >= NRES_BASE && address <= NRES_END)
  {
    if ((address & 1u) != 0)
    {
      s_state.nres = value;
      if (ShouldLogAccess())
        DEV_LOG("KonamiGQ.SoundCPU NRES_write value=0x{:02X}", value);
    }
    return;
  }

  LogUnmapped(true, address, value);
}

void PulseReset()
{
  m68k_set_irq(0);
  m68k_pulse_reset();
  s_state.reset_pulses++;
  DEV_LOG("KonamiGQ.SoundCPU reset_pulsed index={} sp=0x{:06X} pc=0x{:06X}", s_state.reset_pulses,
           m68k_get_reg(nullptr, M68K_REG_SP), m68k_get_reg(nullptr, M68K_REG_PC));
}

void AccumulateScheduledCycles(TickCount ticks)
{
  if (ticks <= 0)
    return;

  const u64 ticks_per_second = static_cast<u64>(std::max<TickCount>(System::GetTicksPerSecond(), 1));
  if (s_state.last_ticks_per_second != 0 && s_state.last_ticks_per_second != ticks_per_second)
  {
    s_state.cycle_fraction =
      (s_state.cycle_fraction * ticks_per_second) / s_state.last_ticks_per_second;
  }
  s_state.last_ticks_per_second = ticks_per_second;

  const u64 numerator =
    (static_cast<u64>(static_cast<u32>(ticks)) * SOUND_CLOCK_HZ) + s_state.cycle_fraction;
  const u64 cycles = numerator / ticks_per_second;
  s_state.cycle_fraction = numerator % ticks_per_second;
  s_state.cycle_balance += static_cast<s64>(cycles);
  s_state.scheduler_ticks += static_cast<u64>(static_cast<u32>(ticks));
}

void RunPendingCycles(const char* reason)
{
  if (!s_state.active || !s_state.reset_released || s_state.executing)
    return;

  while (s_state.cycle_balance > 0)
  {
    const int requested_cycles =
      static_cast<int>(std::min<s64>(s_state.cycle_balance, static_cast<s64>(MAX_EXECUTE_CYCLES)));
    const u32 irq_level = GetIRQLevel();
    m68k_set_irq(irq_level);
    const u32 pc_before = m68k_get_reg(nullptr, M68K_REG_PC);

    s_state.executing = true;
    const int executed_cycles = m68k_execute(requested_cycles);
    s_state.executing = false;
    AdvanceTMS57002(executed_cycles);
    AdvanceK054539Timers(executed_cycles);
    UpdateIRQLine();

    s_state.slice_count++;
    if (executed_cycles > 0)
    {
      s_state.total_cycles += static_cast<u64>(executed_cycles);
      s_state.cycle_balance -= static_cast<s64>(executed_cycles);
    }

    if (s_state.slice_logs < SLICE_LOG_LIMIT)
    {
      s_state.slice_logs++;
      DEV_LOG(
        "KonamiGQ.SoundCPU slice index={} reason='{}' requested_cycles={} executed_cycles={} irq={} "
        "pc_before=0x{:06X} pc_after=0x{:06X} sp=0x{:06X} cycle_balance={} total_cycles={}",
        s_state.slice_count, reason, requested_cycles, executed_cycles, irq_level, pc_before,
        m68k_get_reg(nullptr, M68K_REG_PC), m68k_get_reg(nullptr, M68K_REG_SP), s_state.cycle_balance,
        s_state.total_cycles);
    }

    if (executed_cycles <= 0 || executed_cycles < requested_cycles)
      break;
  }
}

void SchedulerCallback(void*, TickCount ticks, TickCount ticks_late)
{
  if (!s_state.active || !s_state.reset_released)
    return;

  s_state.scheduler_callbacks++;
  AccumulateScheduledCycles(ticks);
  RunPendingCycles("timing_event");

  if (s_state.scheduler_callbacks <= 8)
  {
    DEV_LOG(
      "KonamiGQ.SoundCPU scheduler index={} ticks={} ticks_late={} cycle_fraction={} cycle_balance={} "
      "total_cycles={}",
      s_state.scheduler_callbacks, ticks, ticks_late, s_state.cycle_fraction, s_state.cycle_balance,
      s_state.total_cycles);
  }
}

void SynchronizeScheduler(const char* reason)
{
  if (!s_state.active || !s_state.reset_released || s_state.executing || !s_state.timing_event ||
      !s_state.timing_event->IsActive())
  {
    return;
  }

  s_state.timing_event->InvokeEarly(true);
  RunPendingCycles(reason);
}

} // namespace

bool Initialize(const std::vector<u8>& sound_program, const std::vector<u8>& pcm_samples)
{
  Shutdown();
  if (sound_program.size() != ROM_SIZE)
  {
    ERROR_LOG("KonamiGQ.SoundCPU initialization_failed reason='invalid_rom_size' expected={} actual={}", ROM_SIZE,
              sound_program.size());
    return false;
  }
  if (pcm_samples.size() != K054539_SAMPLE_ROM_SIZE)
  {
    ERROR_LOG(
      "KonamiGQ.SoundCPU initialization_failed reason='invalid_pcm_rom_size' expected={} actual={}",
      K054539_SAMPLE_ROM_SIZE, pcm_samples.size());
    return false;
  }

  s_state.pcm_samples = pcm_samples;
  s_state.crypt_killer_route_trace_enabled = false;
  for (u32 chip = 0; chip < s_state.k054539_chips.size(); chip++)
  {
    s_state.k054539_chips[chip].Initialize(
      [chip](u32 address) { return ReadK054539ExternalData(chip, address); },
      K054539::Chip::PanEncoding::KonamiGQ);
  }
  s_state.tms57002 = std::make_unique<KonamiGQTMS57002::Core>();
  s_state.rom.resize(ROM_SIZE);
  for (u32 offset = 0; offset < ROM_SIZE; offset += 2)
  {
    s_state.rom[offset] = sound_program[offset + 1];
    s_state.rom[offset + 1] = sound_program[offset];
  }

  u32 initial_sp = 0;
  u32 initial_pc = 0;
  if (!HasPlausibleResetVectors(s_state.rom, &initial_sp, &initial_pc))
  {
    ERROR_LOG("KonamiGQ.SoundCPU initialization_failed reason='invalid_reset_vectors' sp=0x{:08X} pc=0x{:08X}",
              initial_sp, initial_pc);
    s_state = {};
    return false;
  }

  s_state.active = true;

  MusashiBus::Callbacks callbacks;
  callbacks.read8 = ReadMemory8ForCore;
  callbacks.read16 = ReadMemory16ForCore;
  callbacks.read32 = ReadMemory32ForCore;
  callbacks.write8 = WriteMemory8ForCore;
  callbacks.write16 = WriteMemory16ForCore;
  callbacks.write32 = WriteMemory32ForCore;
  MusashiBus::SetCallbacks(callbacks);

  m68k_init();
  m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  PulseReset();
  s_state.timing_event =
    std::make_unique<TimingEvent>("Konami GQ Sound CPU", SCHEDULER_INTERVAL_TICKS,
                                  SCHEDULER_INTERVAL_TICKS, SchedulerCallback, nullptr);

  VERBOSE_LOG(
    "KonamiGQ.SoundCPU initialized core='Musashi' clock_hz={} rom_size={} ram_size={} "
    "pcm_rom_size={} pcm_shared_ram_size={} tms_core='MAME-standalone' tms_clock_hz={} "
    "tms_external_ram_size={} rom_byte_order='ROM_LOAD16_WORD_SWAP' "
    "initial_sp=0x{:06X} initial_pc=0x{:06X} scheduler_interval_ticks={} "
    "crypt_killer_route_trace={} shadow_tms_model=false rvv_tms_model=false "
    "diagnostic_tms_domains=0 "
    "tms_input_model='partial' chip1_tms_input='stereo_send' "
    "chip2_tms_input='signed_dlata_regular_channels' chip2_slots='2-9' "
    "chip2_input_gain=0.5 chip2_non_dlata_input=omitted special_axdt_slots=omitted",
    SOUND_CLOCK_HZ, s_state.rom.size(), s_state.ram.size(), s_state.pcm_samples.size(),
    KonamiGQPCM::RAM_SIZE, TMS_CLOCK_HZ, KonamiGQTMS57002::Core::EXTERNAL_RAM_SIZE,
    initial_sp, initial_pc, SCHEDULER_INTERVAL_TICKS, s_state.crypt_killer_route_trace_enabled);
  return true;
}

void Reset()
{
  if (!s_state.active)
    return;

  if (s_state.timing_event && s_state.timing_event->IsActive())
    s_state.timing_event->Deactivate();

  s_state.ram.fill(0);
  for (auto& registers : s_state.k054539_registers)
    registers.fill(0);
  for (K054539::Chip& chip : s_state.k054539_chips)
    chip.Reset();
  ResetK054539Resampler();
  s_state.k054539_data_pointer.fill(0);
  s_state.k054539_data_bank.fill(0);
  s_state.k054539_timer_phase.fill(0);
  s_state.k054539_timer_state.fill(0);
  s_state.k054539_timer_configured.fill(false);
  s_state.k054539_irq2_pending = false;
  if (s_state.tms57002)
    s_state.tms57002->Reset();
  if (s_state.rvv_tms57002)
    s_state.rvv_tms57002->Reset();
  s_state.sound_control = 0;
  s_state.nres = 0;
  s_state.reset_released = false;
  s_state.executing = false;
  s_state.first_k054539_access_logged = false;
  s_state.first_tms_access_logged = false;
  s_state.first_unmapped_access_logged = false;
  s_state.total_cycles = 0;
  s_state.cycle_fraction = 0;
  s_state.last_ticks_per_second = 0;
  s_state.cycle_balance = 0;
  s_state.scheduler_callbacks = 0;
  s_state.scheduler_ticks = 0;
  s_state.k054539_reads = 0;
  s_state.k054539_writes = 0;
  s_state.k054539_data_reads = 0;
  s_state.k054539_data_writes = 0;
  s_state.k054539_sample_rom_reads = 0;
  s_state.k054539_pcm_ram_reads = 0;
  s_state.k054539_reverb_reads = 0;
  s_state.k054539_reverb_writes = 0;
  s_state.k054539_unmapped_data_reads = 0;
  s_state.k054539_timer_toggles = 0;
  s_state.k054539_timer_rising_edges = 0;
  s_state.k054539_irq2_assertions = 0;
  s_state.k054539_irq2_clears = 0;
  s_state.tms_reads = 0;
  s_state.tms_writes = 0;
  s_state.tms_scheduled_cycles = 0;
  s_state.tms_syncs = 0;
  s_state.tms_cycle_balance = 0;
  s_state.unmapped_reads = 0;
  s_state.unmapped_writes = 0;
  s_state.slice_count = 0;
  s_state.slice_logs = 0;
  s_state.access_logs = 0;
  s_state.k054539_write_logs = 0;
  s_state.k054539_data_logs = 0;
  s_state.tms_status_poll_logs = 0;
  s_state.crypt_killer_route_key_logs = 0;
  s_state.crypt_killer_route_target_logs = 0;
  s_state.crypt_killer_route_baseline_logs = 0;
  s_state.crypt_killer_route_write_logs = 0;
  s_state.crypt_killer_signal_baseline_tracks = 0;
  s_state.crypt_killer_signal_summary_logs = 0;
  s_state.crypt_killer_signal_trace = {};
  s_state.crypt_killer_gameplay_sfx_active = {};
  PulseReset();
  DEV_LOG("KonamiGQ.SoundCPU reset state='asserted'");
}

void Shutdown()
{
  if (s_state.timing_event && s_state.timing_event->IsActive())
    s_state.timing_event->Deactivate();

  if (s_state.active)
  {
    for (u32 chip = 0; chip < s_state.crypt_killer_signal_trace.size(); chip++)
    {
      for (u32 channel = 0; channel < s_state.crypt_killer_signal_trace[chip].size(); channel++)
        FinishCryptKillerSignalTrace(chip, channel, "shutdown");
    }

    VERBOSE_LOG(
      "KonamiGQ.SoundCPU shutdown reset_released={} reset_pulses={} scheduler_callbacks={} "
      "scheduler_ticks={} slices={} cycle_balance={} total_cycles={} pc=0x{:06X} sp=0x{:06X} "
      "k054539_reads={} k054539_writes={} data_reads={} data_writes={} sample_rom_reads={} "
      "pcm_ram_reads={} reverb_reads={} reverb_writes={} unmapped_data_reads={} timer_toggles={} "
      "timer_rising_edges={} irq2_assertions={} irq2_clears={} irq2_pending={} tms_reads={} "
      "tms_writes={} tms_scheduled_cycles={} tms_syncs={} tms_pc=0x{:02X} tms_core_cycles={} "
      "tms_external_reads={} tms_external_writes={} unmapped_reads={} unmapped_writes={}",
      s_state.reset_released, s_state.reset_pulses, s_state.scheduler_callbacks, s_state.scheduler_ticks,
      s_state.slice_count, s_state.cycle_balance, s_state.total_cycles,
      m68k_get_reg(nullptr, M68K_REG_PC), m68k_get_reg(nullptr, M68K_REG_SP), s_state.k054539_reads,
      s_state.k054539_writes, s_state.k054539_data_reads, s_state.k054539_data_writes,
      s_state.k054539_sample_rom_reads, s_state.k054539_pcm_ram_reads, s_state.k054539_reverb_reads,
      s_state.k054539_reverb_writes, s_state.k054539_unmapped_data_reads, s_state.k054539_timer_toggles,
      s_state.k054539_timer_rising_edges, s_state.k054539_irq2_assertions, s_state.k054539_irq2_clears,
      s_state.k054539_irq2_pending, s_state.tms_reads, s_state.tms_writes,
      s_state.tms_scheduled_cycles, s_state.tms_syncs,
      s_state.tms57002 ? s_state.tms57002->GetProgramCounter() : 0,
      s_state.tms57002 ? s_state.tms57002->GetExecutedCycles() : 0,
      s_state.tms57002 ? s_state.tms57002->GetExternalReads() : 0,
      s_state.tms57002 ? s_state.tms57002->GetExternalWrites() : 0,
      s_state.unmapped_reads, s_state.unmapped_writes);
  }

  s_state = {};
  MusashiBus::ClearCallbacks();
}

void GenerateAudioFrame(s32* left, s32* right)
{
  if (!left || !right)
    return;

  *left = 0;
  *right = 0;
  if (!s_state.active || !s_state.reset_released)
    return;

  if (s_state.native_audio_queue.size() < 2)
    SynchronizeScheduler("audio_output");

  if (!s_state.k054539_resampler_initialized)
  {
    if (!PopNativeAudioFrame(&s_state.k054539_resample_current))
    {
      return;
    }

    if (!PopNativeAudioFrame(&s_state.k054539_resample_next))
    {
      s_state.k054539_resample_next = s_state.k054539_resample_current;
    }

    s_state.k054539_resample_phase = 0;
    s_state.k054539_resampler_initialized = true;
  }

  constexpr u64 source_rate = K054539::Chip::NATIVE_SAMPLE_RATE;
  constexpr u64 output_rate = SPU::SAMPLE_RATE;
  const s64 phase = static_cast<s64>(s_state.k054539_resample_phase);
  const s64 left_value =
    static_cast<s64>(s_state.k054539_resample_current.output[0]) +
    ((static_cast<s64>(s_state.k054539_resample_next.output[0] - s_state.k054539_resample_current.output[0]) * phase) /
     static_cast<s64>(output_rate));
  const s64 right_value =
    static_cast<s64>(s_state.k054539_resample_current.output[1]) +
    ((static_cast<s64>(s_state.k054539_resample_next.output[1] - s_state.k054539_resample_current.output[1]) * phase) /
     static_cast<s64>(output_rate));

  *left = static_cast<s32>(left_value);
  *right = static_cast<s32>(right_value);

  s_state.k054539_resample_phase += source_rate;
  while (s_state.k054539_resample_phase >= output_rate)
  {
    s_state.k054539_resample_phase -= output_rate;
    s_state.k054539_resample_current = s_state.k054539_resample_next;

    if (s_state.native_audio_queue.empty())
      SynchronizeScheduler("audio_resampler");

    if (!PopNativeAudioFrame(&s_state.k054539_resample_next))
    {
      s_state.k054539_resample_next = s_state.k054539_resample_current;
    }
  }
}

bool IsActive()
{
  return s_state.active;
}

void SetResetReleased(bool released)
{
  if (!s_state.active || s_state.reset_released == released)
    return;

  if (!released)
  {
    SynchronizeScheduler("reset_assert");
    if (s_state.timing_event && s_state.timing_event->IsActive())
      s_state.timing_event->Deactivate();

    s_state.reset_released = false;
    s_state.cycle_fraction = 0;
    s_state.last_ticks_per_second = 0;
    s_state.cycle_balance = 0;
    s_state.k054539_irq2_pending = false;
    ResetK054539Resampler();
    m68k_set_irq(0);
    DEV_LOG("KonamiGQ.SoundCPU reset_line state='asserted'");
    return;
  }

  s_state.reset_released = true;
  s_state.cycle_fraction = 0;
  ResetK054539Resampler();
  s_state.last_ticks_per_second = static_cast<u64>(std::max<TickCount>(System::GetTicksPerSecond(), 1));
  s_state.cycle_balance = 0;
  PulseReset();
  if (s_state.timing_event)
    s_state.timing_event->SetPeriodAndSchedule(SCHEDULER_INTERVAL_TICKS);

  DEV_LOG("KonamiGQ.SoundCPU reset_line state='released' scheduler_interval_ticks={}",
           SCHEDULER_INTERVAL_TICKS);
}

void SynchronizeAfterHostWrite()
{
  SynchronizeScheduler("host_write");
}

void SynchronizeBeforeHostRead()
{
  SynchronizeScheduler("host_read");
}

u32 ReadMemory8ForCore(u32 address)
{
  return ReadByte(address);
}

u32 ReadMemory16ForCore(u32 address)
{
  return (static_cast<u32>(ReadByte(address)) << 8) | static_cast<u32>(ReadByte(address + 1));
}

u32 ReadMemory32ForCore(u32 address)
{
  return (ReadMemory16ForCore(address) << 16) | ReadMemory16ForCore(address + 2);
}

void WriteMemory8ForCore(u32 address, u32 value)
{
  WriteByte(address, static_cast<u8>(value));
}

void WriteMemory16ForCore(u32 address, u32 value)
{
  WriteByte(address, static_cast<u8>(value >> 8));
  WriteByte(address + 1, static_cast<u8>(value));
}

void WriteMemory32ForCore(u32 address, u32 value)
{
  WriteMemory16ForCore(address, value >> 16);
  WriteMemory16ForCore(address + 2, value);
}

} // namespace KonamiGQSoundCPU
