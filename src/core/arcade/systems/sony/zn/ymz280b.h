// SPDX-FileCopyrightText: 2026 StillJC

// SPDX-License-Identifier: GPL-3.0-only



#pragma once



#include "core/types.h"



#include <array>

#include <cstddef>

#include <vector>



namespace SonyZN {



class YMZ280B

{

public:

  using IRQCallback = void (*)(void* userdata, bool state);



  YMZ280B() = default;



  void Initialize(const std::vector<u8>& rom, u32 clock_hz, IRQCallback irq_callback = nullptr,

                  void* irq_userdata = nullptr);

  void Reset();

  void Shutdown();



  void SetIRQCallback(IRQCallback callback, void* userdata = nullptr);



  void WriteAddress(u8 value);

  void WriteData(u8 value);

  u8 ReadData();

  u8 ReadStatus();



  void AdvanceCycles(u32 source_cycles, u32 source_clock_hz);

  void GetOutput(s32* left, s32* right) const;



  u32 GetClockHz() const { return m_clock_hz; }

  u32 GetNativeSampleRate() const { return m_native_sample_rate; }

  size_t GetROMSize() const { return m_rom.size(); }



private:

  static constexpr u32 FRAC_BITS = 9;

  static constexpr u32 FRAC_ONE = 1u << FRAC_BITS;



  struct Voice

  {

    bool playing = false;

    bool keyon = false;

    bool looping = false;

    bool loop_state_captured = false;

    u8 mode = 0;

    u16 fnum = 0;

    u8 level = 0;

    u8 pan = 0;



    u32 start = 0;

    u32 stop = 0;

    u32 loop_start = 0;

    u32 loop_end = 0;

    u32 position = 0;



    s32 signal = 0;

    s32 step = 0x7f;

    s32 loop_signal = 0;

    s32 loop_step = 0x7f;



    u16 output_step = 1;

    u16 output_left = 0;

    u16 output_right = 0;

    u32 output_pos = FRAC_ONE;

    s16 last_sample = 0;

    s16 curr_sample = 0;

  };



  u8 ReadROMByte(u32 address) const;

  void UpdateIRQState();

  void UpdateVoiceStep(Voice& voice);

  void UpdateVoiceVolumes(Voice& voice);

  void StartVoice(Voice& voice);

  void EndVoice(u32 index);

  s16 DecaySample(s16 sample) const;

  s16 DecodeADPCM(u32 index, Voice& voice);

  s16 DecodePCM8(u32 index, Voice& voice);

  s16 DecodePCM16(u32 index, Voice& voice);

  s16 NextSourceSample(u32 index, Voice& voice);

  s32 GenerateVoiceSample(u32 index, Voice& voice);

  void GenerateNativeSample();

  void WriteRegister(u8 reg, u8 value);



  std::vector<u8> m_rom;

  std::array<Voice, 8> m_voice{};



  u32 m_clock_hz = 0;

  u32 m_native_sample_rate = 0;

  u8 m_register = 0;

  u8 m_status = 0;

  u8 m_irq_mask = 0;

  bool m_irq_enable = false;

  bool m_irq_state = false;

  bool m_keyon_enable = false;

  bool m_ext_mem_enable = false;

  u32 m_ext_mem_address = 0;

  u32 m_ext_mem_address_hi = 0;

  u32 m_ext_mem_address_mid = 0;

  u8 m_ext_read_latch = 0;

  u64 m_native_phase = 0;

  s32 m_last_left = 0;

  s32 m_last_right = 0;



  IRQCallback m_irq_callback = nullptr;

  void* m_irq_userdata = nullptr;



  bool m_unknown_register_logged = false;

};



} // namespace SonyZN