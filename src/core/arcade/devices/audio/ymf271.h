// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>
#include <cstddef>
#include <vector>

namespace YMF271 {

class Chip
{
public:
  static constexpr u32 OUTPUT_DIVIDER = 384;

  Chip() = default;

  void Initialize(std::vector<u8> rom, u32 clock_hz);
  void Reset();
  void Shutdown();

  u8 Read(u32 selection);
  void Write(u32 selection, u8 value);

  void AdvanceCycles(u32 source_cycles, u32 source_clock_hz);
  void GetOutput(s32* left, s32* right) const;

  u32 GetClockHz() const { return m_clock_hz; }
  u32 GetNativeSampleRate() const { return m_native_sample_rate; }
  size_t GetROMSize() const { return m_rom.size(); }

private:
  enum class EnvelopeState : u8
  {
    Off,
    Attack,
    Decay1,
    Decay2,
    Release,
  };

  struct Slot
  {
    bool active = false;
    bool key_on = false;

    u8 ext_enable = 0;
    u8 ext_output = 0;
    u8 lfo_frequency = 0;
    u8 lfo_waveform = 0;
    u8 pms = 0;
    u8 ams = 0;
    u8 multiple = 1;
    u8 detune = 0;
    u8 total_level = 0x7f;
    u8 key_scale = 0;
    u8 attack_rate = 0;
    u8 decay1_rate = 0;
    u8 decay2_rate = 0;
    u8 release_rate = 0;
    u8 decay1_level = 0;
    u8 block = 0;
    u8 fns_high = 0;
    u16 fns = 0;
    u8 feedback = 0;
    u8 waveform = 0;
    bool accumulate = false;
    u8 algorithm = 0;
    u8 channel_level[4] = {15, 15, 15, 15};

    u32 start_address = 0;
    u32 end_address = 0;
    u32 loop_address = 0;
    bool alternate_loop = false;
    u8 fs = 0;
    bool pcm_12bit = false;
    u8 source_note = 0;
    u8 source_block = 0;

    u64 phase = 0;
    u32 step = 0;
    u8 key_code = 0;
    u32 lfo_counter = 0;
    u8 lfo_position = 0;
    bool pcm_ended = false;

    u8 modulator_count = 0;
    std::array<u8, 3> modulator_slots{};
    bool feedback_head = false;
    s8 feedback_target = -1;
    bool carrier = false;
    s32 output = 0;
    s32 accumulation = 0;
    std::array<s32, 2> feedback_history{};

    EnvelopeState envelope_state = EnvelopeState::Off;
    s32 envelope_attenuation = 0x3ff;
  };

  struct Group
  {
    u8 sync = 0;
    bool pfm = false;
    bool dirty = true;
  };

  static int DecodeGroup(u8 address);
  static int DecodePCMSlot(u8 address);
  static bool IsSynchronizedRegister(u8 reg);

  u8 ReadROMByte(u32 address) const;
  s16 ReadPCMSample(const Slot& slot, u32 sample_index) const;

  void WriteFunctionBank(u32 bank, u8 address, u8 value);
  void WriteSlotRegister(u32 slot_index, u8 reg, u8 value);
  void WritePCMRegister(u8 address, u8 value);
  void WriteUtilityRegister(u8 address, u8 value);

  void KeyOn(u32 slot_index);
  void KeyOff(u32 slot_index);
  void UpdateKeyCode(u32 slot_index);
  static u32 LFOPeriod(u8 value);
  static void TickLFO(Slot& slot);
  static s32 LFOPhaseModulation(const Slot& slot);
  static s32 LFOAmplitudeModulation(const Slot& slot);
  u32 CalculatePCMStep(const Slot& slot, s32 lfo_pm) const;
  u32 CalculateFMPhaseIncrement(const Slot& slot, s32 lfo_pm) const;
  void UpdateStep(Slot& slot);
  void InitializeTables();
  static u32 EnvelopeRate(u32 rate2, u32 rate_key_scale);
  void StartEnvelope(Slot& slot);
  void TickEnvelope(Slot& slot);
  s32 ApplyEnvelope(s32 sample, u32 attenuation) const;
  s32 GenerateWaveOperator(u32 phase, u8 waveform, u32 attenuation) const;
  static s32 PanOutput(s32 sample, u8 level);

  void ConnectAlgorithm(const std::array<u8, 4>& modulator_masks, u8 carrier_mask, u8 feedback_source,
                        const std::array<u32, 4>& slots, u32 count);
  void RebuildGroup(u32 group_index);

  void SetEndStatus(u32 slot_index, bool state);
  void AdvanceTimers(u64 master_cycles);
  void GenerateNativeSample();

  std::vector<u8> m_rom;
  std::array<Slot, 48> m_slots{};
  std::array<Group, 12> m_groups{};
  std::array<u8, 6> m_address{};

  u32 m_clock_hz = 0;
  u32 m_native_sample_rate = 0;
  u64 m_native_phase = 0;
  u64 m_eg_counter = 0;
  bool m_eg_phase = false;
  std::array<u16, 256> m_log_sine_table{};
  std::array<u16, 256> m_exp_table{};

  u16 m_end_status = 0;
  u8 m_timer_status = 0;
  u16 m_timer_a = 0;
  u8 m_timer_b = 0;
  u8 m_timer_control = 0;
  bool m_timer_a_running = false;
  bool m_timer_b_running = false;
  u64 m_timer_a_cycles = 0;
  u64 m_timer_b_cycles = 0;
  u64 m_master_cycle_fraction = 0;
  u32 m_external_address = 0;
  bool m_external_read = false;
  u8 m_external_read_latch = 0xff;

  s32 m_last_left = 0;
  s32 m_last_right = 0;

  bool m_altloop_logged = false;
  bool m_pfm_logged = false;
};

} // namespace YMF271
