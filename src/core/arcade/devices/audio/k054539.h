// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>
#include <functional>

namespace K054539 {

class Chip
{
public:
  enum class PanEncoding : u8
  {
    Standard,
    KonamiGQ,
  };

  static constexpr u32 REGISTER_COUNT = 0x280;
  static constexpr u32 NATIVE_SAMPLE_RATE = 48'000;

  using ReadCallback = std::function<u8(u32 address)>;
  using ChannelFrame = std::array<std::array<s32, 2>, 8>;
  using RawChannelFrame = std::array<s32, 8>;
  using GQSourceSlotFrame = std::array<s32, 12>;
  enum class GQMuxFPath : u8
  {
    DLATC = 0,
    DLATB = 1,
    P13LAT = 2,
    DLATA = 3,
    Unknown = 0xff,
  };
  using GQMuxFPathFrame = std::array<GQMuxFPath, 8>;
  using GQMuxFShadowFrame = std::array<s32, 8>;
  using GQMuxFShadowValidFrame = std::array<u8, 8>;
  using GQAXDTChannelContributionFrame = std::array<std::array<s32, 2>, 8>;
  using GQAXDTSignedChannelContributionFrame = std::array<std::array<s32, 2>, 8>;
  using GQAXDTCoefficientAddressFrame = std::array<std::array<u8, 2>, 8>;
  using GQAXDTChannelMixFrame = std::array<s32, 2>;
  using GQAXDTFullMixFrame = std::array<s32, 2>;
  using GQAXDTClosedLoopMixFrame = std::array<s32, 2>;
  using GQAXDTRVVLoopMixFrame = std::array<s32, 2>;
  using GQAXDTSignedMixFrame = std::array<s32, 2>;
  // REGEA, REGEB, REGEC, REGED, REGEE, REGEF in silicon register order.
  using GQOutputPlaneFrame = std::array<s32, 6>;

  void Initialize(ReadCallback read_callback, PanEncoding pan_encoding = PanEncoding::Standard);
  void Reset();

  u8 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u8 value);

  // GQ connects the TMS57002 serial return to the K054539 AXDA input.
  // SiliconRE resolves these as mixer source slots 10/11 (word A/word B),
  // controlled by registers 0x210/0x211. Keep them latched for the next
  // native 48 kHz frame.
  void SetAuxInput(s32 word_a, s32 word_b);

  // Separate AXDA latch for the isolated Phase-3L17 shadow-TMS loop.
  // It never participates in the audible/provisional board path.
  void SetGQShadowAuxInput(s32 word_a, s32 word_b);

  // Separate AXDA latch for the optional RVVOL-pair diagnostic loop.
  // This diagnostic never participates in the audible path.
  void SetGQRVVAuxInput(s32 word_a, s32 word_b);

  // aux_send_* exposes the pre-AXDA stereo mix used as the GQ DSP send.
  // gq_source_slots exposes the twelve silicon mixer inputs in hardware order:
  // MUXH A/B, MUXF_REG for channels 0-7, AXDA A/B. The decoded PCM sample
  // is not MUXF_REG and remains available separately through RawChannelFrame.
  // gq_muxf_paths exposes the exact odd-control source selected for each
  // channel. gq_muxf_shadow exposes the reconstructed MUXF_REG word when
  // gq_muxf_shadow_valid is set. The qualified set includes the zero-MULA
  // DLAT_A path plus the exact DLAT_B case where the even MULA byte is zero.
  // The AXDT shadow frames expose the
  // exact per-channel REGEE/REGEF contribution and the ROMB addresses used by
  // those output events. gq_axdt_channel_mix applies the reconstructed
  // active channel-slot updates in hardware slot order (channels 0-7 ==
  // slots 2-9) with exact 16-bit wrap. gq_axdt_full_mix extends that shadow
  // through all twelve slots: previous REGEE/REGEF feedback at slots 0/1,
  // every channel MUXF source including inactive feedback tails at slots 2-9,
  // and AXDA A/B at slots 10/11. These optional shadows are evaluated only
  // when requested by a diagnostic caller and never replace the audible/DSP-send
  // path. A validity flag is clear rather than guessing whenever a required
  // source is not reconstructed.
  void GenerateFrame(s32* left, s32* right, ChannelFrame* channel_output = nullptr,
                     s32* aux_send_left = nullptr, s32* aux_send_right = nullptr,
                     RawChannelFrame* raw_channel_output = nullptr,
                     GQSourceSlotFrame* gq_source_slots = nullptr,
                     GQOutputPlaneFrame* gq_output_planes = nullptr,
                     GQMuxFPathFrame* gq_muxf_paths = nullptr,
                     GQMuxFShadowFrame* gq_muxf_shadow = nullptr,
                     GQMuxFShadowValidFrame* gq_muxf_shadow_valid = nullptr,
                     GQAXDTChannelContributionFrame* gq_axdt_channel_contribution = nullptr,
                     GQAXDTCoefficientAddressFrame* gq_axdt_coefficient_address = nullptr,
                     GQAXDTChannelMixFrame* gq_axdt_channel_mix = nullptr,
                     bool* gq_axdt_channel_mix_valid = nullptr,
                     GQAXDTFullMixFrame* gq_axdt_full_mix = nullptr,
                     bool* gq_axdt_full_mix_valid = nullptr,
                     GQAXDTClosedLoopMixFrame* gq_axdt_closed_loop_mix = nullptr,
                     bool* gq_axdt_closed_loop_mix_valid = nullptr,
                     GQAXDTRVVLoopMixFrame* gq_axdt_rvv_loop_mix = nullptr,
                     bool* gq_axdt_rvv_loop_mix_valid = nullptr,
                     GQAXDTSignedMixFrame* gq_axdt_signed_mix = nullptr,
                     bool* gq_axdt_signed_mix_valid = nullptr,
                     GQAXDTSignedChannelContributionFrame* gq_axdt_signed_channel_contribution = nullptr,
                     double gq_audible_aux_gain = 1.0);

private:
  struct Channel
  {
    u32 position = 0;
    s32 position_fraction = 0;
    s32 current_value = 0;
    s32 previous_value = 0;
  };

  // Global arithmetic pipeline state shared while the silicon walks its
  // twelve 32-cycle source slots. P11/P13 are stored separately per channel
  // because the physical chip writes those feedback words to internal RAM.
  struct GQEffectShadowState
  {
    u32 dlat_a = 0;
    u32 dlat_b = 0;
    u32 dlat_c = 0;
    u32 dlat_d = 0;
    u32 previous_mula_out = 0;
    u32 adde = 0;
    u8 mula_lat = 0;
  };

  bool RegisterUpdatesEnabled() const;
  void KeyOn(u32 channel);
  void KeyOff(u32 channel);
  u8 ReadSampleByte(u32 address) const;
  s16 ReadReverbSample(u32 index) const;
  void WriteReverbSample(u32 index, s16 value);
  double GetGQMixerSlotGain(u32 slot) const;

  ReadCallback m_read_callback;
  PanEncoding m_pan_encoding = PanEncoding::Standard;

  std::array<u8, REGISTER_COUNT> m_registers = {};
  std::array<std::array<u8, 3>, 8> m_position_latch = {};
  std::array<Channel, 8> m_channels = {};
  std::array<u8, 0x8000> m_reverb_ram = {};
  std::array<double, 256> m_volume_table = {};
  std::array<double, 128> m_gq_volume_table = {};
  std::array<double, 15> m_pan_table = {};
  std::array<s32, 2> m_aux_input = {};
  std::array<s32, 2> m_gq_shadow_aux_input = {};
  std::array<s32, 2> m_gq_rvv_aux_input = {};
  // CPU access to 0x000-0x1FF reaches the same 0x100-byte internal RAM because
  // K054539 A8 is not decoded. Keep an alias-resolved GQ shadow for the
  // special mixer parameter words without changing the established PCM path.
  std::array<u8, 0x100> m_gq_internal_ram_cpu_shadow = {};
  // Full twelve-slot REGEE/REGEF shadow output from the previous native frame.
  // REGEEB/REGEFB latch these words at the final-output load edge and MUXH
  // selects them as source slots 0/1 on the following frame.
  std::array<u16, 2> m_gq_axdt_full_feedback = {};
  bool m_gq_axdt_full_feedback_valid = true;
  // Separate previous-frame REGEE/REGEF feedback for the isolated shadow loop.
  std::array<u16, 2> m_gq_axdt_closed_loop_feedback = {};
  bool m_gq_axdt_closed_loop_feedback_valid = true;
  // Independent previous-frame feedback for the optional RVVOL-pair
  // diagnostic. Keeping it separate prevents diagnostic state from contaminating
  // either the established shadow or audible path.
  std::array<u16, 2> m_gq_axdt_rvv_loop_feedback = {};
  bool m_gq_axdt_rvv_loop_feedback_valid = true;
  // Independent previous-frame feedback for the optional signed-MULB
  // diagnostic. This isolates multiplier signedness from the established
  // unsigned shadow and from the audible DSP path.
  std::array<u16, 2> m_gq_axdt_signed_feedback = {};
  bool m_gq_axdt_signed_feedback_valid = true;
  // Physical REGEA-F state is 16-bit. Keep raw words here and sign-extend only
  // when exposing an audio/sample-domain value.
  std::array<u16, 6> m_gq_output_latches = {};
  std::array<u32, 8> m_gq_effect_feedback_p11 = {};
  std::array<u32, 8> m_gq_effect_feedback_p13 = {};
  GQEffectShadowState m_gq_effect_shadow = {};
  u32 m_reverb_position = 0;
  u32 m_data_pointer = 0;
  u8 m_data_bank = 0;
};

} // namespace K054539