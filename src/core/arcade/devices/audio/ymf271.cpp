// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/audio/ymf271.h"

#include "common/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

Log_SetChannel(YMF271);

namespace YMF271 {
namespace {

static constexpr u32 PHASE_BITS = 16;

// OPX function/utility register addresses leave every fourth low-nibble code
// unused. The twelve remaining codes select groups 0-11.
static constexpr std::array<s8, 16> GROUP_DECODE = {
  0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1,
};

// PCM attribute bank 5 addresses the twelve PCM-capable slots: groups 0,4,8
// across each of the four function-register banks.
static constexpr std::array<s8, 16> PCM_SLOT_DECODE = {
  0, 4, 8, -1, 12, 16, 20, -1, 24, 28, 32, -1, 36, 40, 44, -1,
};

// OPM-style detune table indexed by the 5-bit key code and DT1 magnitude.
// Values are in OPX phase-generator units (fs / 2^20).
static constexpr std::array<std::array<u8, 4>, 32> DETUNE_TABLE = {{
  {{0, 0, 1, 2}}, {{0, 0, 1, 2}}, {{0, 0, 1, 2}}, {{0, 0, 1, 2}},
  {{0, 1, 2, 2}}, {{0, 1, 2, 3}}, {{0, 1, 2, 3}}, {{0, 1, 2, 3}},
  {{0, 1, 2, 4}}, {{0, 1, 3, 4}}, {{0, 1, 3, 4}}, {{0, 1, 3, 5}},
  {{0, 2, 4, 5}}, {{0, 2, 4, 6}}, {{0, 2, 4, 6}}, {{0, 2, 5, 7}},
  {{0, 2, 5, 8}}, {{0, 3, 6, 8}}, {{0, 3, 6, 9}}, {{0, 3, 7, 10}},
  {{0, 4, 8, 11}}, {{0, 4, 8, 12}}, {{0, 4, 9, 13}}, {{0, 5, 10, 14}},
  {{0, 5, 11, 16}}, {{0, 6, 12, 17}}, {{0, 6, 13, 19}}, {{0, 7, 14, 20}},
  {{0, 8, 16, 22}}, {{0, 8, 16, 22}}, {{0, 8, 16, 22}}, {{0, 8, 16, 22}},
}};

// LFO pitch-modulation depth from the OPX application manual.
// Maximum deviation is F-number * value / 1024.
static constexpr std::array<u8, 8> PMS_DEPTH = {0, 2, 3, 4, 6, 12, 24, 48};

static constexpr std::array<u16, 8> MODULATION_LEVEL = {128, 64, 32, 16, 8, 256, 512, 1024};

struct AlgorithmModel
{
  std::array<u8, 4> modulators;
  u8 carriers;
  u8 feedback_source;
};

static constexpr std::array<AlgorithmModel, 16> ALGORITHM_4OP = {{
  {{{0x0, 0x4, 0x1, 0x2}}, 0x8, 0},
  {{{0x0, 0x4, 0x1, 0x2}}, 0x8, 2},
  {{{0x0, 0x5, 0x0, 0x2}}, 0x8, 0},
  {{{0x0, 0x4, 0x0, 0x3}}, 0x8, 0},
  {{{0x0, 0x0, 0x1, 0x6}}, 0x8, 0},
  {{{0x0, 0x0, 0x1, 0x6}}, 0x8, 2},
  {{{0x0, 0x0, 0x1, 0x2}}, 0xc, 0},
  {{{0x0, 0x0, 0x1, 0x2}}, 0xc, 2},
  {{{0x0, 0x4, 0x0, 0x2}}, 0x9, 0},
  {{{0x0, 0x0, 0x0, 0x6}}, 0x9, 0},
  {{{0x0, 0x0, 0x1, 0x0}}, 0xe, 0},
  {{{0x0, 0x0, 0x1, 0x0}}, 0xe, 2},
  {{{0x0, 0x1, 0x1, 0x1}}, 0xe, 0},
  {{{0x0, 0x4, 0x0, 0x0}}, 0xb, 0},
  {{{0x0, 0x0, 0x1, 0x2}}, 0xd, 0},
  {{{0x0, 0x0, 0x0, 0x0}}, 0xf, 0},
}};

static constexpr std::array<AlgorithmModel, 4> ALGORITHM_2OP = {{
  {{{0x0, 0x1, 0x0, 0x0}}, 0x2, 0},
  {{{0x0, 0x1, 0x0, 0x0}}, 0x2, 1},
  {{{0x0, 0x0, 0x0, 0x0}}, 0x3, 0},
  {{{0x0, 0x1, 0x0, 0x0}}, 0x3, 0},
}};

static constexpr std::array<AlgorithmModel, 8> ALGORITHM_3OP = {{
  {{{0x0, 0x4, 0x1, 0x0}}, 0x2, 0},
  {{{0x0, 0x4, 0x1, 0x0}}, 0x2, 2},
  {{{0x0, 0x5, 0x0, 0x0}}, 0x2, 0},
  {{{0x0, 0x4, 0x0, 0x0}}, 0x3, 0},
  {{{0x0, 0x0, 0x1, 0x0}}, 0x6, 0},
  {{{0x0, 0x0, 0x1, 0x0}}, 0x6, 2},
  {{{0x0, 0x0, 0x0, 0x0}}, 0x7, 0},
  {{{0x0, 0x0, 0x1, 0x0}}, 0x7, 0},
}};

static constexpr AlgorithmModel ALGORITHM_SINGLE = {{{0x0, 0x0, 0x0, 0x0}}, 0x1, 0};

// Hardware-style envelope increment patterns. Each nibble selects the
// attenuation increment for one of the eight EG substeps at that rate.
static constexpr std::array<u32, 64> EG_INCREMENT = {
  0x00000000u, 0x00000000u, 0x10101010u, 0x10101010u,
  0x10101010u, 0x10111010u, 0x11101110u, 0x11111110u,
  0x10101010u, 0x10111010u, 0x11101110u, 0x11111110u,
  0x10101010u, 0x10111010u, 0x11101110u, 0x11111110u,
  0x10101010u, 0x10111010u, 0x11101110u, 0x11111110u,
  0x10101010u, 0x10111010u, 0x11101110u, 0x11111110u,
  0x10101010u, 0x10111010u, 0x11101110u, 0x11111110u,
  0x10101010u, 0x10111010u, 0x11101110u, 0x11111110u,
  0x10101010u, 0x10111010u, 0x11101110u, 0x11111110u,
  0x10101010u, 0x10111010u, 0x11101110u, 0x11111110u,
  0x10101010u, 0x10111010u, 0x11101110u, 0x11111110u,
  0x10101010u, 0x10111010u, 0x11101110u, 0x11111110u,
  0x11111111u, 0x21112111u, 0x21212121u, 0x22212221u,
  0x22222222u, 0x42224222u, 0x42424242u, 0x44424442u,
  0x44444444u, 0x84448444u, 0x84848484u, 0x88848884u,
  0x88888888u, 0x88888888u, 0x88888888u, 0x88888888u,
};

// Rate-key scaling from the OPX application manual, indexed by key code and KS.
static constexpr std::array<std::array<u8, 8>, 32> RATE_KEY_SCALE = {{
  {{0, 0, 0, 0, 0, 2, 4, 8}}, {{0, 0, 0, 0, 1, 3, 5, 9}},
  {{0, 0, 0, 1, 2, 4, 6, 10}}, {{0, 0, 0, 1, 3, 5, 7, 11}},
  {{0, 0, 1, 2, 4, 6, 8, 12}}, {{0, 0, 1, 2, 5, 7, 9, 13}},
  {{0, 0, 1, 3, 6, 8, 10, 14}}, {{0, 0, 1, 3, 7, 9, 11, 15}},
  {{0, 1, 2, 4, 8, 10, 12, 16}}, {{0, 1, 2, 4, 9, 11, 13, 17}},
  {{0, 1, 2, 5, 10, 12, 14, 18}}, {{0, 1, 2, 5, 11, 13, 15, 19}},
  {{0, 1, 3, 6, 12, 14, 16, 20}}, {{0, 1, 3, 6, 13, 15, 17, 21}},
  {{0, 1, 3, 7, 14, 16, 18, 22}}, {{0, 1, 3, 7, 15, 17, 19, 23}},
  {{0, 2, 4, 8, 16, 18, 20, 24}}, {{0, 2, 4, 8, 17, 19, 21, 25}},
  {{0, 2, 4, 9, 18, 20, 22, 26}}, {{0, 2, 4, 9, 19, 21, 23, 27}},
  {{0, 2, 5, 10, 20, 22, 24, 28}}, {{0, 2, 5, 10, 21, 23, 25, 29}},
  {{0, 2, 5, 11, 22, 24, 26, 30}}, {{0, 2, 5, 11, 23, 25, 27, 31}},
  {{0, 3, 6, 12, 24, 26, 28, 31}}, {{0, 3, 6, 12, 25, 27, 29, 31}},
  {{0, 3, 6, 13, 26, 28, 30, 31}}, {{0, 3, 6, 13, 27, 29, 31, 31}},
  {{0, 3, 7, 14, 28, 30, 31, 31}}, {{0, 3, 7, 14, 29, 31, 31, 31}},
  {{0, 3, 7, 15, 30, 31, 31, 31}}, {{0, 3, 7, 15, 31, 31, 31, 31}},
}};
s32 ClampS32(s64 value)
{
  if (value > std::numeric_limits<s32>::max())
    return std::numeric_limits<s32>::max();
  if (value < std::numeric_limits<s32>::min())
    return std::numeric_limits<s32>::min();
  return static_cast<s32>(value);
}

} // namespace

void Chip::Initialize(std::vector<u8> rom, u32 clock_hz)
{
  m_rom = std::move(rom);
  m_clock_hz = clock_hz;
  m_native_sample_rate = clock_hz / OUTPUT_DIVIDER;
  InitializeTables();
  Reset();
}

void Chip::Reset()
{
  m_slots = {};
  m_groups = {};
  m_address.fill(0);

  // Power-on state is silent. Register writes from the sound program establish
  // the actual slot levels before key-on.
  for (Slot& slot : m_slots)
  {
    slot.multiple = 1;
    slot.total_level = 0x7f;
    slot.channel_level[0] = 15;
    slot.channel_level[1] = 15;
    slot.channel_level[2] = 15;
    slot.channel_level[3] = 15;
    slot.feedback_target = -1;
  }
  for (Group& group : m_groups)
    group.dirty = true;

  m_native_phase = 0;
  m_eg_counter = 0;
  m_eg_phase = false;
  m_end_status = 0;
  m_timer_status = 0;
  m_timer_a = 0;
  m_timer_b = 0;
  m_timer_control = 0;
  m_timer_a_running = false;
  m_timer_b_running = false;
  m_timer_a_cycles = 0;
  m_timer_b_cycles = 0;
  m_master_cycle_fraction = 0;
  m_external_address = 0;
  m_external_read = false;
  m_external_read_latch = 0xff;
  m_last_left = 0;
  m_last_right = 0;
  m_altloop_logged = false;
  m_pfm_logged = false;
}

void Chip::Shutdown()
{
  m_rom.clear();
  m_clock_hz = 0;
  m_native_sample_rate = 0;
  Reset();
}

int Chip::DecodeGroup(u8 address)
{
  return GROUP_DECODE[address & 0x0f];
}

int Chip::DecodePCMSlot(u8 address)
{
  return PCM_SLOT_DECODE[address & 0x0f];
}

bool Chip::IsSynchronizedRegister(u8 reg)
{
  switch (reg)
  {
    case 0x0: // key on/off
    case 0x9: // FNS low
    case 0xa: // FNS high / block
    case 0xc: // algorithm
    case 0xd: // output levels 0/1
    case 0xe: // output levels 2/3
      return true;

    default:
      return false;
  }
}

u8 Chip::ReadROMByte(u32 address) const
{
  address &= UINT32_C(0x007fffff);
  return (address < m_rom.size()) ? m_rom[address] : UINT8_C(0xff);
}

s16 Chip::ReadPCMSample(const Slot& slot, u32 sample_index) const
{
  if (!slot.pcm_12bit)
    return static_cast<s16>(static_cast<s32>(static_cast<s8>(ReadROMByte(slot.start_address + sample_index))) * 256);

  const u32 pair = sample_index >> 1;
  const u32 address = slot.start_address + (pair * 3);
  const u8 b0 = ReadROMByte(address);
  const u8 b1 = ReadROMByte(address + 1);
  const u8 b2 = ReadROMByte(address + 2);

  // Packed 12-bit samples use three bytes for two words:
  // b0 = even bits 11..4, b1 low nibble = even bits 3..0,
  // b1 high nibble = odd bits 3..0, b2 = odd bits 11..4.
  // Keep samples left-aligned in the signed 16-bit output domain.
  const u16 packed = ((sample_index & 1u) == 0) ?
                       static_cast<u16>((static_cast<u16>(b0) << 8) |
                                        (static_cast<u16>(b1 & 0x0fu) << 4)) :
                       static_cast<u16>((static_cast<u16>(b2) << 8) | (b1 & 0xf0u));
  return static_cast<s16>(packed);
}

void Chip::UpdateKeyCode(u32 slot_index)
{
  Slot& slot = m_slots[slot_index];

  int note_bits;
  int key_code;

  if (slot.waveform == 7 && (slot_index & 3u) == 0)
  {
    const u16 fnum = slot.fns & 0x07ffu;
    if (fnum < 0x0100u)
      note_bits = 0;
    else if (fnum < 0x0300u)
      note_bits = 1;
    else if (fnum < 0x0500u)
      note_bits = 2;
    else
      note_bits = 3;

    key_code = (static_cast<int>(slot.source_block) * 4) + static_cast<int>(slot.source_note) +
               (static_cast<int>(slot.block & 7u) * 4) + note_bits;
  }
  else
  {
    if (slot.fns < 0x0780u)
      note_bits = 0;
    else if (slot.fns < 0x0900u)
      note_bits = 1;
    else if (slot.fns < 0x0a80u)
      note_bits = 2;
    else
      note_bits = 3;

    // The signed Block nibble is -8..7. Hardware recordings used by the
    // current OPX model indicate that negative internal-wave blocks use the
    // lowest detune/key-scaling octave rather than wrapping to octave 7.
    const s8 signed_block = static_cast<s8>(static_cast<s32>(slot.block ^ 8u) - 8);
    key_code = ((signed_block < 0) ? 0 : (static_cast<int>(slot.block & 7u) * 4)) + note_bits;
  }

  slot.key_code = static_cast<u8>(key_code & 31);
}

u32 Chip::LFOPeriod(u8 value)
{
  if (value >= 240u)
    return 16u - (value & 0x0fu);

  return (32u - (value & 0x0fu)) << (14u - (value >> 4));
}

void Chip::TickLFO(Slot& slot)
{
  if (slot.lfo_waveform == 0)
    return;

  slot.lfo_counter++;
  if (slot.lfo_counter >= LFOPeriod(slot.lfo_frequency))
  {
    slot.lfo_counter = 0;
    slot.lfo_position = static_cast<u8>((slot.lfo_position + 1u) & 0x7fu);
  }
}

s32 Chip::LFOPhaseModulation(const Slot& slot)
{
  const s32 position = static_cast<s32>(slot.lfo_position);

  switch (slot.lfo_waveform)
  {
    case 1: // saw
      return (((position + 64) & 127) * 2) - 128;

    case 2: // square
      return (position < 64) ? 127 : -128;

    case 3: // triangle
      if (position < 32)
        return position * 4;
      if (position < 96)
        return 128 - ((position - 32) * 4);
      return ((position - 96) * 4) - 128;

    default:
      return 0;
  }
}

u32 Chip::CalculatePCMStep(const Slot& slot, s32 lfo_pm) const
{
  const s64 base_fnum = static_cast<s64>((slot.fns & 0x07ffu) | 0x0800u);
  s64 fnum_q7 = base_fnum << 7;

  if (lfo_pm != 0)
    fnum_q7 += (base_fnum * static_cast<s64>(PMS_DEPTH[slot.pms & 7u]) * static_cast<s64>(lfo_pm)) >> 10;

  const s8 signed_block = static_cast<s8>(static_cast<s32>(slot.block ^ 8u) - 8);
  s64 increment = (fnum_q7 << 16) >> (18 - signed_block);

  const s64 detune = static_cast<s64>(DETUNE_TABLE[slot.key_code & 31u][slot.detune & 3u]) << 6;
  if ((slot.detune & 4u) != 0)
    increment -= detune;
  else
    increment += detune;

  if (increment < 0)
    increment = 0;

  if (slot.multiple == 0)
    increment >>= 1;
  else
    increment *= static_cast<s64>(slot.multiple);

  increment >>= (slot.fs & 3u);
  return static_cast<u32>(std::min<s64>(increment, static_cast<s64>(std::numeric_limits<u32>::max())));
}

void Chip::UpdateStep(Slot& slot)
{
  slot.step = CalculatePCMStep(slot, 0);
}

void Chip::InitializeTables()
{
  static constexpr double PI = 3.1415926535897932384626433832795;

  for (u32 i = 0; i < m_exp_table.size(); i++)
  {
    m_log_sine_table[i] = static_cast<u16>(
      std::floor((-std::log2(std::sin((static_cast<double>(i) + 0.5) * PI / 512.0)) * 256.0) + 0.5));
    m_exp_table[i] = static_cast<u16>(
      std::floor(std::pow(2.0, -(static_cast<double>(i) + 1.0) / 256.0) * 2048.0 + 0.5));
  }
}

u32 Chip::EnvelopeRate(u32 rate2, u32 rate_key_scale)
{
  if (rate2 == 0)
    return 0;

  return std::min<u32>(rate2 + rate_key_scale, 63u);
}

void Chip::StartEnvelope(Slot& slot)
{
  slot.envelope_state = EnvelopeState::Attack;

  // OPM-style retriggering keeps the current attenuation. Only the maximum
  // effective attack rate jumps directly to 0 dB.
  const u32 rks = RATE_KEY_SCALE[slot.key_code & 31u][slot.key_scale & 7u];
  if (EnvelopeRate(static_cast<u32>(slot.attack_rate) * 2u, rks) >= 63u)
    slot.envelope_attenuation = 0;
}

void Chip::TickEnvelope(Slot& slot)
{
  if (slot.envelope_state == EnvelopeState::Off)
    return;

  // State transitions are evaluated before the current EG increment.
  if (slot.envelope_state == EnvelopeState::Attack && slot.envelope_attenuation <= 0)
  {
    slot.envelope_attenuation = 0;
    slot.envelope_state = EnvelopeState::Decay1;
  }

  if (slot.envelope_state == EnvelopeState::Decay1)
  {
    const s32 decay1_target =
      (slot.decay1_level == 15u) ? static_cast<s32>(31u << 5) : static_cast<s32>(slot.decay1_level << 5);
    if (slot.envelope_attenuation >= decay1_target)
      slot.envelope_state = EnvelopeState::Decay2;
  }

  const u32 rks = RATE_KEY_SCALE[slot.key_code & 31u][slot.key_scale & 7u];
  u32 rate;
  switch (slot.envelope_state)
  {
    case EnvelopeState::Attack:
      rate = EnvelopeRate(static_cast<u32>(slot.attack_rate) * 2u, rks);
      break;
    case EnvelopeState::Decay1:
      rate = EnvelopeRate(static_cast<u32>(slot.decay1_rate) * 2u, rks);
      break;
    case EnvelopeState::Decay2:
      rate = EnvelopeRate(static_cast<u32>(slot.decay2_rate) * 2u, rks);
      break;
    case EnvelopeState::Release:
      rate = EnvelopeRate(static_cast<u32>(slot.release_rate) * 4u, rks);
      break;
    case EnvelopeState::Off:
    default:
      return;
  }

  u32 index;
  if (rate < 48u)
  {
    const u32 shift = 11u - (rate >> 2);
    if ((m_eg_counter & ((UINT64_C(1) << shift) - 1u)) != 0)
      return;
    index = static_cast<u32>((m_eg_counter >> shift) & 7u);
  }
  else
  {
    index = static_cast<u32>(m_eg_counter & 7u);
  }

  const s32 increment = static_cast<s32>((EG_INCREMENT[rate] >> (index * 4u)) & 0x0fu);
  if (increment == 0)
    return;

  if (slot.envelope_state == EnvelopeState::Attack)
  {
    slot.envelope_attenuation += ((~slot.envelope_attenuation) * increment) >> 4;
    if (slot.envelope_attenuation <= 0)
    {
      slot.envelope_attenuation = 0;
      slot.envelope_state = EnvelopeState::Decay1;
    }
  }
  else
  {
    slot.envelope_attenuation += increment;
    if (slot.envelope_attenuation >= 0x3ff)
    {
      slot.envelope_attenuation = 0x3ff;
      if (slot.envelope_state == EnvelopeState::Release)
      {
        slot.envelope_state = EnvelopeState::Off;
        slot.active = false;
      }
    }
  }
}

s32 Chip::LFOAmplitudeModulation(const Slot& slot)
{
  const s32 position = static_cast<s32>(slot.lfo_position);

  switch (slot.lfo_waveform)
  {
    case 1: // saw: full attenuation down to zero
      return 127 - position;

    case 2: // square: attenuated first half
      return (position < 64) ? 127 : 0;

    case 3: // triangle: full -> minimum -> full
      return (position < 64) ? (127 - (position * 2)) : (((position - 64) * 2) + 1);

    default:
      return 0;
  }
}

s32 Chip::ApplyEnvelope(s32 sample, u32 attenuation) const
{
  attenuation = std::min<u32>(attenuation, 0x3ffu);
  const u32 table_index = (attenuation & 63u) << 2;
  const u32 shift = 11u + (attenuation >> 6);
  return static_cast<s32>(
    (static_cast<s64>(sample) * static_cast<s64>(m_exp_table[table_index])) >> shift);
}

s32 Chip::PanOutput(s32 sample, u8 level)
{
  // OPX channel-level law: odd levels apply a 0.75 factor before the
  // power-of-two attenuation; levels 13-15 are muted.
  if (level >= 13u)
    return 0;

  if ((level & 1u) != 0)
    sample = (sample * 3) >> 2;

  return sample >> (level >> 1);
}

void Chip::SetEndStatus(u32 slot_index, bool state)
{
  if ((slot_index & 3u) != 0)
    return;

  const u32 subbit = slot_index / 12u;
  const u32 bankbit = ((slot_index % 12u) >> 2u);
  const u16 mask = static_cast<u16>(1u << (subbit + (bankbit * 4u)));
  if (state)
    m_end_status |= mask;
  else
    m_end_status &= static_cast<u16>(~mask);
}

void Chip::KeyOn(u32 slot_index)
{
  Slot& slot = m_slots[slot_index];
  slot.key_on = true;
  slot.phase = 0;
  slot.lfo_counter = 0;
  slot.lfo_position = 0;
  UpdateKeyCode(slot_index);
  UpdateStep(slot);
  SetEndStatus(slot_index, false);
  slot.pcm_ended = false;
  slot.accumulation = 0;
  StartEnvelope(slot);

  // Every waveform now has a synthesis path (wave 7 fetches external PCM;
  // wave 6 and waves 0-5 use the internal operator pipeline).
  slot.active = true;
}

void Chip::KeyOff(u32 slot_index)
{
  Slot& slot = m_slots[slot_index];
  slot.key_on = false;
  if (slot.envelope_state != EnvelopeState::Off)
    slot.envelope_state = EnvelopeState::Release;
}

void Chip::WriteSlotRegister(u32 slot_index, u8 reg, u8 value)
{
  Slot& slot = m_slots[slot_index];

  switch (reg)
  {
    case 0x0:
      slot.ext_enable = (value >> 7) & 1u;
      slot.ext_output = (value >> 3) & 0x0fu;
      if (value & 1u)
        KeyOn(slot_index);
      else
        KeyOff(slot_index);
      break;

    case 0x1:
      slot.lfo_frequency = value;
      break;

    case 0x2:
      slot.lfo_waveform = value & 0x03u;
      slot.pms = (value >> 3) & 0x07u;
      slot.ams = (value >> 6) & 0x03u;
      break;

    case 0x3:
      slot.multiple = value & 0x0fu;
      slot.detune = (value >> 4) & 0x07u;
      UpdateStep(slot);
      break;

    case 0x4:
      slot.total_level = value & 0x7fu;
      break;

    case 0x5:
      slot.attack_rate = value & 0x1fu;
      slot.key_scale = (value >> 5) & 0x07u;
      break;

    case 0x6:
      slot.decay1_rate = value & 0x1fu;
      break;

    case 0x7:
      slot.decay2_rate = value & 0x1fu;
      break;

    case 0x8:
      slot.release_rate = value & 0x0fu;
      slot.decay1_level = (value >> 4) & 0x0fu;
      break;

    case 0x9:
      slot.fns = static_cast<u16>(((static_cast<u16>(slot.fns_high) & 0x0fu) << 8) | value);
      slot.block = (slot.fns_high >> 4) & 0x0fu;
      UpdateKeyCode(slot_index);
      UpdateStep(slot);
      break;

    case 0xa:
      // Block/FNS-high is latched and becomes active when FNS-low (0x9) is written.
      slot.fns_high = value;
      break;

    case 0xb:
      slot.waveform = value & 0x07u;
      slot.feedback = (value >> 4) & 0x07u;
      slot.accumulate = (value & 0x80u) != 0;
      UpdateKeyCode(slot_index);
      UpdateStep(slot);
      break;

    case 0xc:
      slot.algorithm = value & 0x0fu;
      m_groups[slot_index % 12u].dirty = true;
      break;

    case 0xd:
      slot.channel_level[0] = value >> 4;
      slot.channel_level[1] = value & 0x0fu;
      break;

    case 0xe:
      slot.channel_level[2] = value >> 4;
      slot.channel_level[3] = value & 0x0fu;
      break;

    default:
      break;
  }
}

void Chip::WriteFunctionBank(u32 bank, u8 address, u8 value)
{
  const int group = DecodeGroup(address);
  if (group < 0 || bank >= 4)
    return;

  const u8 reg = address >> 4;
  const u32 group_index = static_cast<u32>(group);
  const Group& group_state = m_groups[group_index];

  if (!IsSynchronizedRegister(reg) || group_state.sync == 3)
  {
    WriteSlotRegister((bank * 12u) + group_index, reg, value);
    return;
  }

  switch (group_state.sync)
  {
    case 0: // four-slot FM mode: bank 0 (S1) is the synchronized control slot
      if (bank == 0)
      {
        for (u32 slot_bank = 0; slot_bank < 4; slot_bank++)
          WriteSlotRegister((slot_bank * 12u) + group_index, reg, value);
      }
      else
      {
        WriteSlotRegister((bank * 12u) + group_index, reg, value);
      }
      break;

    case 1: // two independent two-slot pairs
      if (bank == 0 || bank == 1)
      {
        WriteSlotRegister((bank * 12u) + group_index, reg, value);
        WriteSlotRegister(((bank + 2u) * 12u) + group_index, reg, value);
      }
      else
      {
        WriteSlotRegister((bank * 12u) + group_index, reg, value);
      }
      break;

    case 2: // three-slot FM plus independent fourth slot
      if (bank == 0)
      {
        for (u32 slot_bank = 0; slot_bank < 3; slot_bank++)
          WriteSlotRegister((slot_bank * 12u) + group_index, reg, value);
      }
      else
      {
        WriteSlotRegister((bank * 12u) + group_index, reg, value);
      }
      break;

    default:
      WriteSlotRegister((bank * 12u) + group_index, reg, value);
      break;
  }
}

void Chip::WritePCMRegister(u8 address, u8 value)
{
  const int decoded_slot = DecodePCMSlot(address);
  if (decoded_slot < 0)
    return;

  Slot& slot = m_slots[static_cast<u32>(decoded_slot)];
  switch ((address >> 4) & 0x0fu)
  {
    case 0x0:
      slot.start_address = (slot.start_address & 0x7fff00u) | value;
      break;
    case 0x1:
      slot.start_address = (slot.start_address & 0x7f00ffu) | (static_cast<u32>(value) << 8);
      break;
    case 0x2:
      slot.start_address = (slot.start_address & 0x00ffffu) | (static_cast<u32>(value & 0x7f) << 16);
      slot.alternate_loop = (value & 0x80u) != 0;
      if (slot.alternate_loop && !m_altloop_logged)
      {
        m_altloop_logged = true;
        DEV_LOG("YMF271 alternate-loop requested; currently treated as normal forward loop.");
      }
      break;
    case 0x3:
      slot.end_address = (slot.end_address & 0x7fff00u) | value;
      break;
    case 0x4:
      slot.end_address = (slot.end_address & 0x7f00ffu) | (static_cast<u32>(value) << 8);
      break;
    case 0x5:
      slot.end_address = (slot.end_address & 0x00ffffu) | (static_cast<u32>(value & 0x7f) << 16);
      break;
    case 0x6:
      slot.loop_address = (slot.loop_address & 0x7fff00u) | value;
      break;
    case 0x7:
      slot.loop_address = (slot.loop_address & 0x7f00ffu) | (static_cast<u32>(value) << 8);
      break;
    case 0x8:
      slot.loop_address = (slot.loop_address & 0x00ffffu) | (static_cast<u32>(value & 0x7f) << 16);
      break;
    case 0x9:
      slot.fs = value & 0x03u;
      slot.pcm_12bit = (value & 0x04u) != 0;
      slot.source_note = (value >> 3) & 0x03u;
      slot.source_block = (value >> 5) & 0x07u;
      UpdateKeyCode(static_cast<u32>(decoded_slot));
      UpdateStep(slot);
      break;
    default:
      break;
  }
}

void Chip::WriteUtilityRegister(u8 address, u8 value)
{
  if ((address & 0xf0u) == 0)
  {
    const int group = DecodeGroup(address);
    if (group >= 0)
    {
      Group& group_state = m_groups[static_cast<u32>(group)];
      group_state.sync = value & 0x03u;
      group_state.pfm = (value & 0x80u) != 0;
      group_state.dirty = true;
      if (group_state.pfm && group_state.sync != 3 && !m_pfm_logged)
      {
        m_pfm_logged = true;
        DEV_LOG("YMF271 PFM mode requested for group {}; external-wave FM is not yet synthesized.", group);
      }
    }
    return;
  }

  switch (address)
  {
    case 0x10:
      // Timer A is a 10-bit period value. Beastorizer's 68000 program writes
      // the upper eight bits here and the low two bits to 0x11.
      m_timer_a = static_cast<u16>((m_timer_a & 0x0003u) | (static_cast<u16>(value) << 2));
      break;

    case 0x11:
      m_timer_a = static_cast<u16>((m_timer_a & 0x03fcu) | (value & 0x03u));
      break;

    case 0x12:
      m_timer_b = value;
      break;

    case 0x13:
    {
      const u8 previous = m_timer_control;

      // Timer load is edge-sensitive. Once loaded, each timer free-runs and
      // reloads on expiry. The RA9701 board does not require OPX /IRQ wiring:
      // Beastorizer polls the timer status bits to drive its sound sequencer.
      if ((previous & 0x01u) == 0 && (value & 0x01u) != 0)
      {
        m_timer_a_running = true;
        m_timer_a_cycles = 0;
      }
      if ((previous & 0x02u) == 0 && (value & 0x02u) != 0)
      {
        m_timer_b_running = true;
        m_timer_b_cycles = 0;
      }

      if (value & 0x10u)
        m_timer_status &= ~UINT8_C(0x01);
      if (value & 0x20u)
        m_timer_status &= ~UINT8_C(0x02);

      m_timer_control = value;
      break;
    }

    case 0x14:
      m_external_address = (m_external_address & 0x7fff00u) | value;
      break;
    case 0x15:
      m_external_address = (m_external_address & 0x7f00ffu) | (static_cast<u32>(value) << 8);
      break;
    case 0x16:
      m_external_address = (m_external_address & 0x00ffffu) | (static_cast<u32>(value & 0x7f) << 16);
      m_external_read = (value & 0x80u) != 0;
      if (m_external_read)
        m_external_read_latch = ReadROMByte(m_external_address);
      break;
    case 0x17:
      // External SRAM writes are not present on RA9701's sample-ROM path.
      if (!m_external_read)
        m_external_address = (m_external_address + 1) & 0x007fffffu;
      break;
    default:
      break;
  }

}

void Chip::Write(u32 selection, u8 value)
{
  selection &= 0x0fu;

  switch (selection)
  {
    case 0x0:
      m_address[0] = value;
      break;
    case 0x1:
      WriteFunctionBank(0, m_address[0], value);
      break;
    case 0x2:
      m_address[1] = value;
      break;
    case 0x3:
      WriteFunctionBank(1, m_address[1], value);
      break;
    case 0x4:
      m_address[2] = value;
      break;
    case 0x5:
      WriteFunctionBank(2, m_address[2], value);
      break;
    case 0x6:
      m_address[3] = value;
      break;
    case 0x7:
      WriteFunctionBank(3, m_address[3], value);
      break;
    case 0x8:
      m_address[4] = value;
      break;
    case 0x9:
      WritePCMRegister(m_address[4], value);
      break;
    case 0xc:
      m_address[5] = value;
      break;
    case 0xd:
      WriteUtilityRegister(m_address[5], value);
      break;
    default:
      break;
  }
}

u8 Chip::Read(u32 selection)
{
  selection &= 0x0fu;

  switch (selection)
  {
    case 0x0:
    {
      const u8 result = static_cast<u8>(m_timer_status | ((m_end_status & 0x000fu) << 3));
      // PCM End flags are read-to-clear. This is required by software which
      // copies the OPX status and later reuses the same PCM slots.
      m_end_status &= static_cast<u16>(~UINT16_C(0x000f));
      return result;
    }
    case 0x1:
    {
      const u8 result = static_cast<u8>(m_end_status >> 4);
      m_end_status &= UINT16_C(0x000f);
      return result;
    }
    case 0x2:
    {
      if (!m_external_read)
        return UINT8_C(0xff);

      const u8 result = m_external_read_latch;
      m_external_address = (m_external_address + 1) & 0x007fffffu;
      m_external_read_latch = ReadROMByte(m_external_address);
      return result;
    }
    default:
      return UINT8_C(0xff);
  }
}

void Chip::AdvanceTimers(u64 master_cycles)
{
  if (master_cycles == 0)
    return;

  if (m_timer_a_running)
  {
    const u64 period = UINT64_C(384) * (UINT64_C(1024) - static_cast<u64>(m_timer_a & 0x03ffu));
    m_timer_a_cycles += master_cycles;
    while (m_timer_a_cycles >= period)
    {
      m_timer_a_cycles -= period;
      m_timer_status |= UINT8_C(0x01);

    }
  }

  if (m_timer_b_running)
  {
    const u64 period =
      UINT64_C(384) * UINT64_C(16) * (UINT64_C(256) - static_cast<u64>(m_timer_b));
    m_timer_b_cycles += master_cycles;
    while (m_timer_b_cycles >= period)
    {
      m_timer_b_cycles -= period;
      m_timer_status |= UINT8_C(0x02);

    }
  }
}

u32 Chip::CalculateFMPhaseIncrement(const Slot& slot, s32 lfo_pm) const
{
  const s8 signed_block = static_cast<s8>(static_cast<s32>(slot.block ^ 8u) - 8);
  const u32 shift = static_cast<u32>(signed_block + 11);
  s64 fnum_q7 = static_cast<s64>(slot.fns) << 7;

  if (lfo_pm != 0)
  {
    fnum_q7 +=
      (static_cast<s64>(slot.fns) * static_cast<s64>(PMS_DEPTH[slot.pms & 7u]) * static_cast<s64>(lfo_pm)) >> 10;
  }

  s64 increment = (fnum_q7 << shift) >> 7;
  const s64 detune = static_cast<s64>(DETUNE_TABLE[slot.key_code & 31u][slot.detune & 3u]) << 12;
  if ((slot.detune & 4u) != 0)
    increment -= detune;
  else
    increment += detune;

  if (increment < 0)
    increment = 0;

  if (slot.multiple == 0)
    increment >>= 1;
  else
    increment *= static_cast<s64>(slot.multiple);

  // The FM phase accumulator is a 32-bit wrapping counter. Values above
  // Nyquist intentionally alias as they do on the OPX/OPM-style PG.
  return static_cast<u32>(increment);
}

s32 Chip::GenerateWaveOperator(u32 phase, u8 waveform, u32 attenuation) const
{
  const u32 wrapped_phase = phase & 0x03ffu;
  u32 index = wrapped_phase & 0x00ffu;
  if ((wrapped_phase & 0x0100u) != 0)
    index ^= 0x00ffu;

  u32 log_attenuation = 0;
  bool negative = false;

  switch (waveform & 7u)
  {
    case 0: // sine
      log_attenuation = m_log_sine_table[index];
      negative = (wrapped_phase & 0x0200u) != 0;
      break;

    case 1: // +/-sin^2
      log_attenuation = static_cast<u32>(m_log_sine_table[index]) << 1;
      negative = (wrapped_phase & 0x0200u) != 0;
      break;

    case 2: // |sin|
      log_attenuation = m_log_sine_table[index];
      break;

    case 3: // positive half sine, silent during the second half-cycle
      if ((wrapped_phase & 0x0200u) != 0)
        return 0;
      log_attenuation = m_log_sine_table[index];
      break;

    case 4: // sin(2wt) during the first half-cycle
    case 5: // |sin(2wt)| during the first half-cycle
      if ((wrapped_phase & 0x0200u) != 0)
        return 0;
      index = (wrapped_phase << 1) & 0x00ffu;
      if ((wrapped_phase & 0x0080u) != 0)
        index ^= 0x00ffu;
      log_attenuation = m_log_sine_table[index];
      negative = ((waveform & 7u) == 4u) && ((wrapped_phase & 0x0100u) != 0);
      break;

    default:
      // Wave 6 is the linear/modulation-through waveform and wave 7 is PCM.
      // Both need data other than the oscillator phase and are handled by the
      // native-sample loop.
      return 0;
  }

  log_attenuation += std::min<u32>(attenuation, 0x03ffu) << 2;
  if (log_attenuation >= 4096u)
    return 0;

  const s32 output =
    static_cast<s32>((static_cast<u32>(m_exp_table[log_attenuation & 0x00ffu]) << 2) >>
                     (log_attenuation >> 8));
  return negative ? -output : output;
}

void Chip::ConnectAlgorithm(const std::array<u8, 4>& modulator_masks, u8 carrier_mask, u8 feedback_source,
                            const std::array<u32, 4>& slots, u32 count)
{
  for (u32 position = 0; position < count; position++)
  {
    Slot& slot = m_slots[slots[position]];
    slot.modulator_count = 0;

    for (u32 source = 0; source < count; source++)
    {
      if ((modulator_masks[position] & (1u << source)) != 0)
        slot.modulator_slots[slot.modulator_count++] = static_cast<u8>(slots[source]);
    }

    slot.carrier = ((carrier_mask >> position) & 1u) != 0;
    slot.feedback_head = (position == 0);
    slot.feedback_target =
      (position == static_cast<u32>(feedback_source)) ? static_cast<s8>(slots[0]) : static_cast<s8>(-1);
  }
}

void Chip::RebuildGroup(u32 group_index)
{
  Group& group = m_groups[group_index];
  group.dirty = false;

  std::array<u32, 4> slots = {};

  switch (group.sync)
  {
    case 0: // S1-S2-S3-S4
    {
      slots = {group_index, group_index + 12u, group_index + 24u, group_index + 36u};
      const AlgorithmModel& algorithm = ALGORITHM_4OP[m_slots[group_index].algorithm & 15u];
      ConnectAlgorithm(algorithm.modulators, algorithm.carriers, algorithm.feedback_source, slots, 4);
      break;
    }

    case 1: // S1-S3 and S2-S4
    {
      slots = {group_index, group_index + 24u, 0, 0};
      const AlgorithmModel& first = ALGORITHM_2OP[m_slots[group_index].algorithm & 3u];
      ConnectAlgorithm(first.modulators, first.carriers, first.feedback_source, slots, 2);

      slots = {group_index + 12u, group_index + 36u, 0, 0};
      const AlgorithmModel& second = ALGORITHM_2OP[m_slots[group_index + 12u].algorithm & 3u];
      ConnectAlgorithm(second.modulators, second.carriers, second.feedback_source, slots, 2);
      break;
    }

    case 2: // S1-S2-S3 FM plus independent S4/PCM
    {
      slots = {group_index, group_index + 12u, group_index + 24u, 0};
      const AlgorithmModel& fm = ALGORITHM_3OP[m_slots[group_index].algorithm & 7u];
      ConnectAlgorithm(fm.modulators, fm.carriers, fm.feedback_source, slots, 3);

      slots = {group_index + 36u, 0, 0, 0};
      ConnectAlgorithm(ALGORITHM_SINGLE.modulators, ALGORITHM_SINGLE.carriers, ALGORITHM_SINGLE.feedback_source,
                       slots, 1);
      break;
    }

    default: // four independent slots
      for (u32 bank = 0; bank < 4; bank++)
      {
        slots = {group_index + (bank * 12u), 0, 0, 0};
        ConnectAlgorithm(ALGORITHM_SINGLE.modulators, ALGORITHM_SINGLE.carriers, ALGORITHM_SINGLE.feedback_source,
                         slots, 1);
      }
      break;
  }
}

void Chip::GenerateNativeSample()
{
  s64 left = 0;
  s64 right = 0;

  for (u32 group_index = 0; group_index < m_groups.size(); group_index++)
  {
    if (m_groups[group_index].dirty)
      RebuildGroup(group_index);
  }

  // The envelope generator advances at half the native output sample rate.
  const bool eg_clock = m_eg_phase;
  m_eg_phase = !m_eg_phase;
  if (eg_clock)
    m_eg_counter++;

  // Slots are evaluated in hardware order. A modulator with a lower slot
  // number therefore contributes its current-frame output, while a
  // higher-numbered modulator still contains its previous-frame output.
  for (u32 slot_index = 0; slot_index < m_slots.size(); slot_index++)
  {
    Slot& slot = m_slots[slot_index];

    if (eg_clock)
      TickEnvelope(slot);
    TickLFO(slot);

    if (!slot.active || slot.envelope_state == EnvelopeState::Off)
    {
      slot.output = 0;
      slot.accumulation = 0;
      continue;
    }

    s32 modulation = 0;
    if (slot.feedback_head)
    {
      const u32 feedback = slot.feedback & 7u;
      if (feedback != 0)
      {
        modulation =
          (slot.feedback_history[0] + slot.feedback_history[1]) >> static_cast<s32>(10u - feedback);
      }
    }
    else
    {
      s32 sum = 0;
      for (u32 i = 0; i < slot.modulator_count; i++)
        sum += m_slots[slot.modulator_slots[i]].output;

      modulation =
        static_cast<s32>((static_cast<s64>(sum) * MODULATION_LEVEL[slot.feedback & 7u]) >> 8);
    }

    u32 attenuation =
      static_cast<u32>(std::max<s32>(slot.envelope_attenuation, 0)) +
      (static_cast<u32>(slot.total_level) << 3);

    if (slot.ams != 0 && slot.lfo_waveform != 0)
    {
      const s32 am = LFOAmplitudeModulation(slot);
      attenuation += (slot.ams == 1u) ? static_cast<u32>(am >> 1) :
                     (slot.ams == 2u) ? static_cast<u32>(am) :
                                       static_cast<u32>(am << 1);
    }
    attenuation = std::min<u32>(attenuation, 0x03ffu);

    const s32 lfo_pm = (slot.pms != 0 && slot.lfo_waveform != 0) ? LFOPhaseModulation(slot) : 0;
    s32 output = 0;

    if (slot.waveform == 7)
    {
      // External PCM fetch is physically available only on slots 0,4,...,44.
      if ((slot_index & 3u) == 0)
      {
        u32 sample_index = static_cast<u32>(slot.phase >> PHASE_BITS);
        if (sample_index >= slot.end_address)
        {
          if (!slot.pcm_ended)
          {
            SetEndStatus(slot_index, true);
            slot.pcm_ended = true;
          }

          if (slot.end_address > slot.loop_address)
          {
            const u64 loop_length = static_cast<u64>(slot.end_address - slot.loop_address) << PHASE_BITS;
            const u64 overshoot = slot.phase - (static_cast<u64>(slot.end_address) << PHASE_BITS);
            slot.phase = (static_cast<u64>(slot.loop_address) << PHASE_BITS) + (overshoot % loop_length);
          }
          else
          {
            slot.phase = static_cast<u64>(slot.loop_address) << PHASE_BITS;
          }

          sample_index = static_cast<u32>(slot.phase >> PHASE_BITS);
        }

        const s32 sample_a = ReadPCMSample(slot, sample_index);
        const s32 sample_b = ReadPCMSample(slot, sample_index + 1u);
        // The current reference model uses the upper eight bits of the 16-bit
        // fractional PCM position as the linear-interpolation weight.
        const u32 fraction = static_cast<u32>((slot.phase >> 8) & 0xffu);
        const s32 sample = static_cast<s32>(
          ((static_cast<s64>(sample_a) * static_cast<s64>(256u - fraction)) +
           (static_cast<s64>(sample_b) * static_cast<s64>(fraction))) >> 8);

        // ArcadeDuck keeps PCM words left-aligned in a 16-bit domain. Convert
        // the enveloped result to the OPX's 14-bit operator domain before it
        // participates in FM routing/feedback.
        output = ApplyEnvelope(sample, attenuation) >> 2;

        slot.phase += (lfo_pm != 0) ? CalculatePCMStep(slot, lfo_pm) : slot.step;
      }
    }
    else
    {
      if (slot.waveform == 6)
      {
        // OPX waveform 6 is phase-independent. It outputs a half-scale DC
        // level plus the modulation input passed through as a 9-bit wrapping
        // ramp. MUL scales the modulation input first (MUL=0 means one half).
        const s32 scaled_modulation =
          (slot.multiple != 0) ? static_cast<s32>(static_cast<s64>(modulation) * slot.multiple) :
                                 ((modulation >= 0) ? (modulation / 2) :
                                                      -static_cast<s32>((-static_cast<s64>(modulation) + 1) / 2));
        const u32 ramp = static_cast<u32>(static_cast<s64>(scaled_modulation) * 64) & 0x7fffu;
        const s32 linear = 8192 + static_cast<s32>(ramp);
        output = ApplyEnvelope(linear, attenuation);
      }
      else
      {
        const u32 phase = (static_cast<u32>(slot.phase) >> 22) + static_cast<u32>(modulation);
        output = GenerateWaveOperator(phase, slot.waveform, attenuation);
      }

      // The phase generator continues to run for every internal waveform,
      // including waveform 6 even though waveform 6 itself is phase-independent.
      slot.phase =
        static_cast<u32>(static_cast<u32>(slot.phase) + CalculateFMPhaseIncrement(slot, lfo_pm));
    }

    if (slot.accumulate)
    {
      const s64 accumulated = static_cast<s64>(slot.accumulation) + static_cast<s64>(output);
      slot.accumulation = static_cast<s32>(std::clamp<s64>(accumulated, -8192, 8191));
      output = slot.accumulation;
    }

    slot.output = output;

    if (slot.feedback_target >= 0)
    {
      Slot& target = m_slots[static_cast<u32>(slot.feedback_target)];
      target.feedback_history[1] = target.feedback_history[0];
      target.feedback_history[0] = output;
    }

    if (slot.carrier)
    {
      left += static_cast<s64>(PanOutput(output, slot.channel_level[0]));
      right += static_cast<s64>(PanOutput(output, slot.channel_level[1]));
    }
  }

  // One full-level carrier is a 14-bit +/-8192 signal, inherently leaving
  // roughly 12 dB of headroom in the 16-bit-equivalent device mix domain.
  // RA9701 uses the first stereo pair of the OPX main output buses.
  m_last_left = ClampS32(left);
  m_last_right = ClampS32(right);
}

void Chip::AdvanceCycles(u32 source_cycles, u32 source_clock_hz)
{
  if (source_cycles == 0 || source_clock_hz == 0 || m_native_sample_rate == 0 || m_clock_hz == 0)
    return;

  // Convert elapsed 68000 cycles to OPX master-clock cycles. Timer A/B are
  // clocked from the 16.9344 MHz OPX master clock, independently of audio rate.
  const u64 timer_numerator =
    (static_cast<u64>(source_cycles) * static_cast<u64>(m_clock_hz)) + m_master_cycle_fraction;
  const u64 master_cycles = timer_numerator / source_clock_hz;
  m_master_cycle_fraction = timer_numerator % source_clock_hz;
  AdvanceTimers(master_cycles);

  m_native_phase += static_cast<u64>(source_cycles) * m_native_sample_rate;
  while (m_native_phase >= source_clock_hz)
  {
    m_native_phase -= source_clock_hz;
    GenerateNativeSample();
  }
}

void Chip::GetOutput(s32* left, s32* right) const
{
  if (left)
    *left = m_last_left;
  if (right)
    *right = m_last_right;
}

} // namespace YMF271
