// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/audio/k054539.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace K054539 {

namespace {

constexpr u32 ACTIVE_REGISTER = 0x22c;
constexpr u32 DATA_REGISTER = 0x22d;
constexpr u32 BANK_REGISTER = 0x22e;
constexpr u32 CONTROL_REGISTER = 0x22f;
constexpr u32 KEY_ON_REGISTER = 0x214;
constexpr u32 KEY_OFF_REGISTER = 0x215;
constexpr u32 GQ_EFFECTS_MUX_REGISTER = 0x225;
constexpr u32 GQ_GLOBAL_VOLUME_REGISTER_BASE = 0x228;
constexpr u32 GQ_MUXH_SLOT_A = 0;
constexpr u32 GQ_MUXH_SLOT_B = 1;
constexpr u32 GQ_MUXF_SLOT_BASE = 2;
constexpr u32 GQ_AXDA_SLOT_A = 10;
constexpr u32 GQ_AXDA_SLOT_B = 11;
constexpr u32 GQ_MIXER_CYCLES_PER_SLOT = 32;
constexpr u32 GQ_MIXER_SLOT_COUNT = 12;
constexpr u32 GQ_MIXER_CYCLES_PER_FRAME = GQ_MIXER_CYCLES_PER_SLOT * GQ_MIXER_SLOT_COUNT;

// SiliconRE output accumulator / serial-output identity.
// REGEA -> REDT, REGEB -> FRDT, REGEC -> REDL, REGED -> FRDL,
// REGEE/REGEF -> the two serialized AXDT words.
constexpr u32 GQ_REGEA_PLANE = 0;
constexpr u32 GQ_REGEB_PLANE = 1;
constexpr u32 GQ_REGEC_PLANE = 2;
constexpr u32 GQ_REGED_PLANE = 3;
constexpr u32 GQ_REGEE_PLANE = 4;
constexpr u32 GQ_REGEF_PLANE = 5;

// SiliconRE's 384-cycle mixer schedule has twelve 32-cycle source slots.
// Slots 0/1 are MUXH special/feedback sources controlled by 0x212/0x213,
// slots 2-9 are PCM channels 0-7 controlled by 0x200-0x20E even, and
// slots 10/11 are the two deserialized AXDA words controlled by 0x210/0x211.
constexpr std::array<u16, 12> GQ_MIXER_CONTROL_REGISTERS = {{
  0x212, 0x213,
  0x200, 0x202, 0x204, 0x206, 0x208, 0x20A, 0x20C, 0x20E,
  0x210, 0x211,
}};

static_assert(GQ_MIXER_CONTROL_REGISTERS[GQ_AXDA_SLOT_A] == 0x210);
static_assert(GQ_MIXER_CONTROL_REGISTERS[GQ_AXDA_SLOT_B] == 0x211);

// SiliconRE ROMA/RAM timing loads the RVVOL/PAN pair for the four non-channel
// source slots from internal RAM words 0x5F, 0x7F, 0x1F and 0x3F respectively.
// CPU byte addresses are word*2 for RAMB (RVVOL) and word*2+1 for RAMA (PAN).
// A8 is not decoded on K054539 internal-RAM accesses, so Crypt Killer's writes
// at 0x13E/13F etc. alias these physical 0x00-0xFF byte addresses.
constexpr u8 GQ_MUXH_SLOT_A_RVVOL_BYTE = 0xbe;
constexpr u8 GQ_MUXH_SLOT_A_PAN_BYTE = 0xbf;
constexpr u8 GQ_MUXH_SLOT_B_RVVOL_BYTE = 0xfe;
constexpr u8 GQ_MUXH_SLOT_B_PAN_BYTE = 0xff;
constexpr u8 GQ_AXDA_SLOT_A_RVVOL_BYTE = 0x3e;
constexpr u8 GQ_AXDA_SLOT_A_PAN_BYTE = 0x3f;
constexpr u8 GQ_AXDA_SLOT_B_RVVOL_BYTE = 0x7e;
constexpr u8 GQ_AXDA_SLOT_B_PAN_BYTE = 0x7f;

// Exact active 384-entry SiliconRE ROMA microsequence. ROMA is a synchronous
// ROM clocked by the main 18.432 MHz K054539 clock, so the value consumed by
// mixer cycle N is the address presented on cycle N-1. Keeping that one-cycle
// delay explicit is important when the DLAT/MULA shadow pipeline is activated.
constexpr std::array<u8, GQ_MIXER_CYCLES_PER_FRAME> GQ_ROMA = {{
  0x07, 0x04, 0x00, 0x05, 0x08, 0x09, 0x00, 0x00, 0x01, 0x7F, 0x00, 0x0A, 0x03, 0x0B, 0x00, 0x0C,
  0x0D, 0x0E, 0x00, 0x06, 0x07, 0x08, 0x00, 0x09, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16,
  0x17, 0x14, 0x10, 0x15, 0x18, 0x19, 0x10, 0x10, 0x11, 0x02, 0x10, 0x1A, 0x13, 0x1B, 0x10, 0x1C,
  0x1D, 0x1E, 0x10, 0x16, 0x17, 0x18, 0x10, 0x19, 0x1A, 0x0B, 0x10, 0x0C, 0x0D, 0x0E, 0x10, 0x26,
  0x27, 0x24, 0x20, 0x25, 0x28, 0x29, 0x20, 0x20, 0x21, 0x12, 0x20, 0x2A, 0x23, 0x2B, 0x20, 0x2C,
  0x2D, 0x2E, 0x20, 0x26, 0x27, 0x28, 0x20, 0x29, 0x2A, 0x1B, 0x20, 0x1C, 0x1D, 0x1E, 0x20, 0x36,
  0x37, 0x34, 0x30, 0x35, 0x38, 0x39, 0x30, 0x30, 0x31, 0x22, 0x30, 0x3A, 0x33, 0x3B, 0x30, 0x3C,
  0x3D, 0x3E, 0x30, 0x36, 0x37, 0x38, 0x30, 0x39, 0x3A, 0x2B, 0x30, 0x2C, 0x2D, 0x2E, 0x30, 0x46,
  0x47, 0x44, 0x40, 0x45, 0x48, 0x49, 0x40, 0x40, 0x41, 0x32, 0x40, 0x4A, 0x43, 0x4B, 0x40, 0x4C,
  0x4D, 0x4E, 0x40, 0x46, 0x47, 0x48, 0x40, 0x49, 0x4A, 0x3B, 0x40, 0x3C, 0x3D, 0x3E, 0x40, 0x56,
  0x57, 0x54, 0x50, 0x55, 0x58, 0x59, 0x50, 0x50, 0x51, 0x42, 0x50, 0x5A, 0x53, 0x5B, 0x50, 0x5C,
  0x5D, 0x5E, 0x50, 0x56, 0x57, 0x58, 0x50, 0x59, 0x5A, 0x4B, 0x50, 0x4C, 0x4D, 0x4E, 0x50, 0x66,
  0x67, 0x64, 0x60, 0x65, 0x68, 0x69, 0x60, 0x60, 0x61, 0x52, 0x60, 0x6A, 0x63, 0x6B, 0x60, 0x6C,
  0x6D, 0x6E, 0x60, 0x66, 0x67, 0x68, 0x60, 0x69, 0x6A, 0x5B, 0x60, 0x5C, 0x5D, 0x5E, 0x60, 0x76,
  0x77, 0x74, 0x70, 0x75, 0x78, 0x79, 0x70, 0x70, 0x71, 0x62, 0x70, 0x7A, 0x73, 0x7B, 0x70, 0x7C,
  0x7D, 0x7E, 0x70, 0x76, 0x77, 0x78, 0x70, 0x79, 0x7A, 0x6B, 0x60, 0x6C, 0x6D, 0x6E, 0x70, 0x70,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7B, 0x00, 0x7C, 0x7D, 0x7E, 0x7F, 0x00,
  0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x2F, 0x1F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
  0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
  0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x4F, 0x3F, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
  0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
  0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x6F, 0x5F, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
  0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x06,
}};

constexpr u8 GQGetDelayedROMA(u32 mixer_cycle)
{
  return GQ_ROMA[(mixer_cycle + GQ_MIXER_CYCLES_PER_FRAME - 1u) % GQ_MIXER_CYCLES_PER_FRAME];
}

static_assert(GQ_MIXER_CYCLES_PER_FRAME == 384);
static_assert(GQGetDelayedROMA(0) == 0x06);
static_assert(GQGetDelayedROMA(1) == 0x07);
static_assert(GQGetDelayedROMA(2) == 0x04);
static_assert(GQGetDelayedROMA(14) == 0x0B);
static_assert(GQGetDelayedROMA(32) == 0x16);

// Exact 0x00-0xBF active portion of the decapped K054539 ROMB.
// 0x00-0x7F is the attenuation table. 0x80-0xBF contains the silicon pan
// coefficient tables. Keep the raw words intact: the real mixer consumes
// these through its 16x16 MULB path rather than a normalized floating curve.
constexpr std::array<u16, 192> GQ_ROMB = {{
  0x7FFF, 0x7568, 0x6BB1, 0x62C8, 0x5A9C, 0x531D, 0x4C3C, 0x45EE,
  0x4025, 0x3AD6, 0x35F8, 0x3181, 0x2D68, 0x29A6, 0x2634, 0x230B,
  0x2025, 0x1D7C, 0x1B0C, 0x18CF, 0x16C1, 0x14DF, 0x1325, 0x1190,
  0x101C, 0x0EC7, 0x0D8E, 0x0C6F, 0x0B67, 0x0A76, 0x0998, 0x08CD,
  0x0813, 0x0768, 0x06CB, 0x063B, 0x05B7, 0x053E, 0x04CF, 0x0469,
  0x040C, 0x03B6, 0x0368, 0x0320, 0x02DD, 0x02A1, 0x0269, 0x0236,
  0x0207, 0x01DC, 0x01B5, 0x0191, 0x0170, 0x0151, 0x0136, 0x011C,
  0x0105, 0x00EF, 0x00DB, 0x00C9, 0x00B9, 0x00A9, 0x009B, 0x008F,
  0x0083, 0x0078, 0x006E, 0x0065, 0x005D, 0x0055, 0x004E, 0x0048,
  0x0042, 0x003C, 0x0037, 0x0033, 0x002F, 0x002B, 0x0027, 0x0024,
  0x0021, 0x001F, 0x001C, 0x001A, 0x0018, 0x0016, 0x0014, 0x0012,
  0x0011, 0x0010, 0x000E, 0x000D, 0x000C, 0x000B, 0x000A, 0x0009,
  0x0009, 0x0008, 0x0007, 0x0007, 0x0006, 0x0006, 0x0005, 0x0005,
  0x0005, 0x0004, 0x0004, 0x0004, 0x0003, 0x0003, 0x0003, 0x0003,
  0x0003, 0x0002, 0x0002, 0x0002, 0x0002, 0x0002, 0x0002, 0x0002,
  0x0002, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0000,
  0x0000, 0x7FFF, 0x7BE0, 0x776D, 0x7297, 0x6D4A, 0x676F, 0x60E3,
  0x5977, 0x577E, 0x46C2, 0x3A5B, 0x2A5D, 0x13D3, 0x12B4, 0x0000,
  0x0000, 0x0000, 0x12B4, 0x13D3, 0x2A5D, 0x3A5B, 0x46C2, 0x577E,
  0x5977, 0x60E3, 0x676F, 0x6D4A, 0x7297, 0x776D, 0x7BE0, 0x7FFF,
  0x0000, 0x8001, 0x8420, 0x8893, 0x8D69, 0x92B6, 0x9891, 0x9F1D,
  0xA689, 0xA882, 0xB93E, 0xC5A5, 0xD5A3, 0xEC2D, 0xED4C, 0x0000,
  0x0000, 0x0000, 0xED4C, 0xEC2D, 0xD5A3, 0xC5A5, 0xB93E, 0xA882,
  0xA689, 0x9F1D, 0x9891, 0x92B6, 0x8D69, 0x8893, 0x8420, 0x8001,
}};
constexpr double GQ_ROMB_MIX_DENOMINATOR = 131072.0;

static_assert(GQ_ROMB[0x0B] == 0x3181);
static_assert(GQ_ROMB[0x1F] == 0x08CD);
static_assert(GQ_ROMB[0x80] == 0x0000 && GQ_ROMB[0x81] == 0x7FFF);
static_assert(GQ_ROMB[0x90] == 0x0000 && GQ_ROMB[0x9F] == 0x7FFF);
static_assert(GQ_ROMB[0xA1] == 0x8001 && GQ_ROMB[0xBF] == 0x8001);


constexpr std::array<s16, 16> DPCM_DELTA = {{
  0 * 0x100,   1 * 0x100,   2 * 0x100,   4 * 0x100,
  8 * 0x100,  16 * 0x100,  32 * 0x100,  64 * 0x100,
  0 * 0x100, -64 * 0x100, -32 * 0x100, -16 * 0x100,
 -8 * 0x100,  -4 * 0x100,  -2 * 0x100,  -1 * 0x100,
}};

ALWAYS_INLINE s32 Clamp16(s32 value)
{
  return std::clamp<s32>(value, -32768, 32767);
}

// Bit-exact K054539 output-mixer arithmetic from the SiliconRE netlist/HDL.
// MULB is a 16x16 multiplier whose output stage consumes the upper 16 bits.
// REGE accumulation then adds an arithmetic-right-shifted MULB result plus
// its discarded LSB. All operations are 16-bit and therefore wrap naturally.
constexpr u16 GQMulBHigh(u16 coefficient, u16 source)
{
  return static_cast<u16>((static_cast<u32>(coefficient) * static_cast<u32>(source)) >> 16);
}

// Phase-3L23 candidate: the SiliconRE HDL expresses MULB operands as raw
// 16-bit vectors, but ROMB 0xA0-0xBF is plainly the two's-complement companion
// pan table. Keep the established unsigned interpretation intact and evaluate
// a fully signed 16x16 high-word interpretation in a separate shadow domain.
// Extracting bits 31:16 from the two's-complement product avoids relying on
// implementation-defined right shift of a negative signed integer.
constexpr u16 GQMulBHighSigned(u16 coefficient, u16 source)
{
  const s32 product = static_cast<s32>(static_cast<s16>(coefficient)) *
                      static_cast<s32>(static_cast<s16>(source));
  return static_cast<u16>(static_cast<u32>(product) >> 16);
}

constexpr u16 GQAccumulateREGE(u16 accumulator, u16 mulb_output)
{
  const u16 arithmetic_half = static_cast<u16>((mulb_output >> 1) | (mulb_output & 0x8000u));
  return static_cast<u16>(accumulator + arithmetic_half + (mulb_output & 0x0001u));
}

constexpr u16 GQMixREGE(u16 accumulator, u16 coefficient, u16 source)
{
  return GQAccumulateREGE(accumulator, GQMulBHigh(coefficient, source));
}

static_assert(GQMulBHigh(0x7fffu, 0x4000u) == 0x1fffu);
static_assert(GQMulBHighSigned(0x7fffu, 0x4000u) == 0x1fffu);
static_assert(GQMulBHighSigned(0xa689u, 0x3f80u) == 0xe9ceu);
static_assert(GQAccumulateREGE(0x0000u, 0x0003u) == 0x0002u);
static_assert(GQAccumulateREGE(0x0000u, 0xfffdu) == 0xffffu);
static_assert(GQAccumulateREGE(0xffffu, 0x0001u) == 0x0000u);

// Exact 31-bit effect/interpolation datapath primitives from SiliconRE.
// These are deliberately separate from the audible path until the complete
// 32-cycle DLAT sequence is exercised as a shadow pipeline.
constexpr u32 GQ_EFFECT_WORD_MASK = 0x7fffffffu;

constexpr u32 GQMulA(u8 coefficient, u16 source)
{
  return (static_cast<u32>(coefficient) * static_cast<u32>(source)) & 0x00ffffffu;
}

constexpr u32 GQAddE(u32 previous_mula_out, u32 current_mula_out)
{
  return ((previous_mula_out & 0x00ffffffu) + ((current_mula_out >> 14) & 0x03ffu)) & 0x00ffffffu;
}

constexpr u32 GQBuildMulaDInput(u32 adde, u32 current_mula_out)
{
  return (((adde & 0x00ffffffu) << 7) | ((current_mula_out >> 7) & 0x7fu)) & GQ_EFFECT_WORD_MASK;
}

constexpr u32 GQBuildSampleDInput(s16 sample)
{
  const u32 sample_bits = static_cast<u32>(static_cast<u16>(sample)) << 14;
  const u32 sign_bit = (sample < 0) ? 0x40000000u : 0u;
  return (sign_bit | sample_bits) & GQ_EFFECT_WORD_MASK;
}

constexpr u32 GQAddD(u32 input_a, u32 input_b, bool invert_a, bool invert_b, bool carry_in)
{
  const u32 a = invert_a ? ((~input_a) & GQ_EFFECT_WORD_MASK) : (input_a & GQ_EFFECT_WORD_MASK);
  const u32 b = invert_b ? ((~input_b) & GQ_EFFECT_WORD_MASK) : (input_b & GQ_EFFECT_WORD_MASK);
  return (a + b + (carry_in ? 1u : 0u)) & GQ_EFFECT_WORD_MASK;
}

struct GQFeedbackWords
{
  u16 low = 0;
  u16 high = 0;
};

constexpr GQFeedbackWords GQPackFeedback(u32 value)
{
  value &= GQ_EFFECT_WORD_MASK;
  return {
    static_cast<u16>((((value >> 7) & 0xffu) << 8) | ((value & 0x7fu) << 1)),
    static_cast<u16>((value >> 15) & 0xffffu),
  };
}

constexpr u32 GQUnpackFeedback(GQFeedbackWords words)
{
  const u32 low = ((static_cast<u32>(words.low >> 8) & 0xffu) << 7) |
                  ((static_cast<u32>(words.low) >> 1) & 0x7fu);
  const u32 high = static_cast<u32>(words.high) << 15;
  return (high | low) & GQ_EFFECT_WORD_MASK;
}

static_assert(GQMulA(0x00u, 0xffffu) == 0x000000u);
static_assert(GQMulA(0xffu, 0xffffu) == 0xfeff01u);
static_assert(GQBuildSampleDInput(static_cast<s16>(0x4000)) == 0x10000000u);
static_assert(GQBuildSampleDInput(static_cast<s16>(-1)) == 0x7fffc000u);
static_assert(GQUnpackFeedback(GQPackFeedback(0x00000000u)) == 0x00000000u);
static_assert(GQUnpackFeedback(GQPackFeedback(0x12345678u)) == 0x12345678u);
static_assert(GQUnpackFeedback(GQPackFeedback(0x7fffffffu)) == 0x7fffffffu);

// Exact reduced DLAT_A microsequence for the runtime-confirmed Crypt Killer
// target case: MULA parameter bytes 0x06/0x07 are both zero and MUXF selects
// DLAT_A. SiliconRE's 32-cycle sequence then deterministically flushes ADDE
// to zero before DLAT_B opens, captures zero into DLAT_B at relative cycle 17,
// captures sign-extended CH_SAMPLE + DLAT_B into DLAT_A at relative cycle 27,
// and the subsequent MUXF latch exposes DLAT_A[30:15]. No feedback/P11/P13
// term participates in this specific path.
constexpr u32 GQRunZeroMulaDLATABMicrosequence()
{
  const u32 mula_out = GQMulA(0x00u, 0x0000u);
  const u32 adde_after_second_zero_mula_edge = GQAddE(mula_out, mula_out);
  const u32 mula_d = GQBuildMulaDInput(adde_after_second_zero_mula_edge, mula_out);
  return GQAddD(mula_d, mula_d, false, false, false);
}

constexpr u32 GQRunZeroMulaDLATAMicrosequence(s16 sample)
{
  const u32 dlat_b = GQRunZeroMulaDLATABMicrosequence();
  return GQAddD(GQBuildSampleDInput(sample), dlat_b, false, false, false);
}

// With the even MULA byte (register 0x06 / MULA_A_LATB) at zero, the
// channel's effect microsequence has an exact reduced recurrence even when the
// odd MULA byte (register 0x07 / MULA_A_LATA) is nonzero:
//
//   DLAT_C  = sign-extended CH_SAMPLE
//   P13'    = previous P11
//   product = REG07 * DLAT_A[30:15] (unsigned 8x16)
//   P11'    = DLAT_C + (product << 7), 31-bit wrap
//   DLAT_B  = 0
//   DLAT_A  = sign-extended CH_SAMPLE
//
// The product occurs while TRIGE selects MULA_A_LATA. By the TRIGB window,
// L81 holds that product while the current multiplier output is zero, so ADDE
// is the full 24-bit product and MULA_LAT is zero. This makes the D-input term
// exactly product<<7. P13 is written earlier, during TRIGC, before that odd
// MULA product reaches the ADDD path, so it is exactly the previous P11 word.
constexpr u32 GQRunZeroEvenMulaDLATCMicrosequence(s16 sample)
{
  return GQBuildSampleDInput(sample);
}

constexpr u32 GQRunZeroEvenMulaP11FeedbackMicrosequence(s16 sample, u8 odd_mula)
{
  const u32 dlat_c = GQRunZeroEvenMulaDLATCMicrosequence(sample);
  const u32 dlat_a = GQRunZeroMulaDLATAMicrosequence(sample);
  const u16 dlat_a_high = static_cast<u16>((dlat_a >> 15) & 0xffffu);
  const u32 odd_mula_out = GQMulA(odd_mula, dlat_a_high);
  const u32 feedback_term = GQBuildMulaDInput(odd_mula_out, 0);
  return GQAddD(dlat_c, feedback_term, false, false, false);
}

constexpr u32 GQRunZeroEvenMulaP13FeedbackMicrosequence(u32 previous_p11)
{
  return previous_p11 & GQ_EFFECT_WORD_MASK;
}

constexpr s16 GQExtractMuxFWord(u32 dlat)
{
  return static_cast<s16>(static_cast<u16>((dlat >> 15) & 0xffffu));
}

// Exact SiliconRE timing for channel/MUXF slots 2-9 resolves the two AXDT
// accumulator contributions without reconstructing the still-unresolved A-D
// output planes. REGEE (AXDT A) updates at slot-relative cycle 13 from the
// multiplier result whose ROMB coefficient was sampled at phase 9. Due to the
// same-edge AK50A latch timing, that lookup is RVVOL-high, not PAN. REGEF
// (AXDT B) updates at cycle 15 from the phase-11 PAN lookup. At both events
// MULB_B is MUXG_REG, which is the channel's MUXF_REG source in slots 2-9.
constexpr u8 GQGetChannelREGEECoefficientAddress(u8 reverb_volume)
{
  // SiliconRE latches RVVOL from RAMB_DO[6:0]; bit 7 is not part of the
  // physical reverb-volume value. Mask it before selecting the high nibble.
  return static_cast<u8>((((reverb_volume & 0x70u) >> 4) << 3) | 0x07u);
}

constexpr u8 GQGetChannelREGEFCoefficientAddress(u8 channel_control, u8 pan, u8 control_22f)
{
  if (control_22f & 0x02u)
    return 0x98u;

  const bool negative_table = (channel_control & 0x80u) == 0;
  return static_cast<u8>(0x90u | (negative_table ? 0x20u : 0x00u) | (pan & 0x0fu));
}

// Diagnostic-only reconstruction of the four physical speaker-output
// accumulators documented by SiliconRE. This is intentionally not used by
// the audible path; it exists only to identify the downstream GQ fold.
constexpr u8 GQGetChannelOutputPlaneCoefficientAddress(u32 plane, u8 channel_control, u8 pan)
{
  const u8 high = static_cast<u8>((pan >> 4) & 0x0fu);
  const u8 low = static_cast<u8>(pan & 0x0fu);

  switch (plane)
  {
    case GQ_REGEA_PLANE:
      return static_cast<u8>(0x90u | high);
    case GQ_REGEB_PLANE:
      return static_cast<u8>((channel_control & 0x40u ? 0x80u : 0xa0u) | low);
    case GQ_REGEC_PLANE:
      return static_cast<u8>((channel_control & 0x80u ? 0x90u : 0xb0u) | low);
    case GQ_REGED_PLANE:
      return static_cast<u8>(0x80u | high);
    default:
      return 0;
  }
}

static_assert(GQGetChannelOutputPlaneCoefficientAddress(GQ_REGEA_PLANE, 0x00u, 0x68u) == 0x96u);
static_assert(GQGetChannelOutputPlaneCoefficientAddress(GQ_REGEB_PLANE, 0x00u, 0x68u) == 0xa8u);
static_assert(GQGetChannelOutputPlaneCoefficientAddress(GQ_REGEC_PLANE, 0x00u, 0x68u) == 0xb8u);
static_assert(GQGetChannelOutputPlaneCoefficientAddress(GQ_REGED_PLANE, 0x00u, 0x68u) == 0x86u);

constexpr s16 GQGetChannelAXDTContribution(s16 muxf_word, u8 coefficient_address)
{
  return static_cast<s16>(
    GQAccumulateREGE(0, GQMulBHigh(GQ_ROMB[coefficient_address], static_cast<u16>(muxf_word))));
}

static_assert(GQRunZeroMulaDLATABMicrosequence() == 0x00000000u);
static_assert(GQExtractMuxFWord(GQRunZeroMulaDLATAMicrosequence(static_cast<s16>(0x4000))) == 0x2000);
static_assert(GQExtractMuxFWord(GQRunZeroMulaDLATAMicrosequence(static_cast<s16>(-1))) == -1);
static_assert(GQExtractMuxFWord(GQRunZeroMulaDLATAMicrosequence(static_cast<s16>(-32768))) == -16384);
static_assert(GQRunZeroEvenMulaP13FeedbackMicrosequence(0x12345678u) == 0x12345678u);
static_assert(GQRunZeroEvenMulaP11FeedbackMicrosequence(static_cast<s16>(0x4000), 0x00u) == 0x10000000u);
static_assert(GQRunZeroEvenMulaP11FeedbackMicrosequence(static_cast<s16>(0x4000), 0xe0u) == 0x1e000000u);
// Phase-3L20 candidate: SiliconRE explicitly identifies RVVOL as the reverb
// volume parameter and presents its low/high nibbles as the two non-pan ROMB
// lookup cases. Test those two RVVOL attenuation words as the AXDT E/F pair
// while retaining the older flattened shadow for A/B comparison. This remains
// a diagnostic candidate until the full registered MULB/MUXC/SRA pipeline is
// reproduced.
constexpr u8 GQGetRVVOLLowCoefficientAddress(u8 reverb_volume)
{
  return static_cast<u8>(((reverb_volume & 0x0fu) << 3) | 0x07u);
}

constexpr u8 GQGetRVVOLHighCoefficientAddress(u8 reverb_volume)
{
  // RVVOL is physically 7-bit (RAMB_DO[6:0]); ignore CPU-visible bit 7.
  return static_cast<u8>((((reverb_volume & 0x70u) >> 4) << 3) | 0x07u);
}

static_assert(GQGetRVVOLLowCoefficientAddress(0x33u) == 0x1fu);
static_assert(GQGetRVVOLHighCoefficientAddress(0x33u) == 0x1fu);
static_assert(GQGetRVVOLLowCoefficientAddress(0x11u) == 0x0fu);
static_assert(GQGetRVVOLHighCoefficientAddress(0x11u) == 0x0fu);
static_assert(GQGetRVVOLLowCoefficientAddress(0x23u) == 0x1fu);
static_assert(GQGetRVVOLHighCoefficientAddress(0x23u) == 0x17u);
static_assert(GQGetRVVOLLowCoefficientAddress(0x32u) == 0x17u);
static_assert(GQGetRVVOLHighCoefficientAddress(0x32u) == 0x1fu);
static_assert(GQGetRVVOLHighCoefficientAddress(0xffu) == 0x3fu);

static_assert(GQGetChannelREGEECoefficientAddress(0x33u) == 0x1fu);
static_assert(GQGetChannelREGEECoefficientAddress(0x11u) == 0x0fu);
static_assert(GQGetChannelREGEECoefficientAddress(0x7fu) == 0x3fu);
static_assert(GQGetChannelREGEECoefficientAddress(0xffu) == 0x3fu);
static_assert(GQGetChannelREGEFCoefficientAddress(0x00u, 0x00u, 0x01u) == 0xb0u);
static_assert(GQGetChannelREGEFCoefficientAddress(0x03u, 0x18u, 0x01u) == 0xb8u);
static_assert(GQGetChannelREGEFCoefficientAddress(0x09u, 0x18u, 0x01u) == 0xb8u);
static_assert(GQGetChannelREGEFCoefficientAddress(0x89u, 0x18u, 0x01u) == 0x98u);
static_assert(GQGetChannelREGEFCoefficientAddress(0x09u, 0x18u, 0x03u) == 0x98u);

} // namespace

void Chip::Initialize(ReadCallback read_callback, PanEncoding pan_encoding)
{
  m_read_callback = std::move(read_callback);
  m_pan_encoding = pan_encoding;

  for (u32 i = 0; i < m_volume_table.size(); i++)
    m_volume_table[i] = std::pow(10.0, (-36.0 * static_cast<double>(i) / 64.0) / 20.0) / 4.0;

  // Use the exact decapped ROMB attenuation coefficients for the current
  // GQ channel-volume stage. The full ROMB is retained above because the
  // silicon pan/output mixer uses its 0x80-0xBF coefficient tables directly.
  for (u32 i = 0; i < m_gq_volume_table.size(); i++)
    m_gq_volume_table[i] = static_cast<double>(GQ_ROMB[i]) / GQ_ROMB_MIX_DENOMINATOR;

  for (u32 i = 0; i < m_pan_table.size(); i++)
    m_pan_table[i] = std::sqrt(static_cast<double>(i)) / std::sqrt(14.0);

  Reset();
}

void Chip::Reset()
{
  m_registers.fill(0);
  m_position_latch = {};
  m_channels = {};
  m_reverb_ram.fill(0);
  m_aux_input.fill(0);
  m_gq_shadow_aux_input.fill(0);
  m_gq_rvv_aux_input.fill(0);
  m_gq_internal_ram_cpu_shadow.fill(0);
  m_gq_axdt_full_feedback.fill(0);
  m_gq_axdt_full_feedback_valid = true;
  m_gq_axdt_closed_loop_feedback.fill(0);
  m_gq_axdt_closed_loop_feedback_valid = true;
  m_gq_axdt_rvv_loop_feedback.fill(0);
  m_gq_axdt_rvv_loop_feedback_valid = true;
  m_gq_axdt_signed_feedback.fill(0);
  m_gq_axdt_signed_feedback_valid = true;
  m_gq_output_latches.fill(0);
  m_gq_effect_feedback_p11.fill(0);
  m_gq_effect_feedback_p13.fill(0);
  m_gq_effect_shadow = {};
  m_reverb_position = 0;
  m_data_pointer = 0;
  m_data_bank = 0;
}

bool Chip::RegisterUpdatesEnabled() const
{
  return (m_registers[CONTROL_REGISTER] & 0x80u) == 0;
}

void Chip::KeyOn(u32 channel)
{
  if (RegisterUpdatesEnabled())
    m_registers[ACTIVE_REGISTER] |= static_cast<u8>(1u << channel);
}

void Chip::KeyOff(u32 channel)
{
  if (RegisterUpdatesEnabled())
    m_registers[ACTIVE_REGISTER] &= static_cast<u8>(~(1u << channel));
}

u8 Chip::ReadSampleByte(u32 address) const
{
  return m_read_callback ? m_read_callback(address) : 0xff;
}

s16 Chip::ReadReverbSample(u32 index) const
{
  index &= 0x1fffu;
  s16 value;
  std::memcpy(&value, &m_reverb_ram[index * sizeof(s16)], sizeof(value));
  return value;
}

void Chip::WriteReverbSample(u32 index, s16 value)
{
  index &= 0x1fffu;
  std::memcpy(&m_reverb_ram[index * sizeof(s16)], &value, sizeof(value));
}

void Chip::SetAuxInput(s32 word_a, s32 word_b)
{
  m_aux_input[0] = word_a;
  m_aux_input[1] = word_b;
}

void Chip::SetGQShadowAuxInput(s32 word_a, s32 word_b)
{
  m_gq_shadow_aux_input[0] = word_a;
  m_gq_shadow_aux_input[1] = word_b;
}

void Chip::SetGQRVVAuxInput(s32 word_a, s32 word_b)
{
  m_gq_rvv_aux_input[0] = word_a;
  m_gq_rvv_aux_input[1] = word_b;
}

double Chip::GetGQMixerSlotGain(u32 slot) const
{
  if (m_pan_encoding != PanEncoding::KonamiGQ || slot >= GQ_MIXER_CONTROL_REGISTERS.size())
    return 0.0;

  // Every one of the twelve silicon mixer source slots owns the same control
  // shape: inverted bits 1:0 select one of 0x228-0x22B. This helper now uses
  // the exact slot-to-control schedule instead of treating 0x210/0x211 as an
  // isolated AUX convention. Bits 7:6 also feed the per-output ROMB/pan route
  // logic; those become active when the six REGE accumulator schedule is ported.
  const u32 control_register = GQ_MIXER_CONTROL_REGISTERS[slot];
  const u32 volume_select = (~m_registers[control_register]) & 0x03u;
  const u32 attenuation_index = m_registers[GQ_GLOBAL_VOLUME_REGISTER_BASE + volume_select] & 0x7fu;
  return static_cast<double>(GQ_ROMB[attenuation_index]) / GQ_ROMB_MIX_DENOMINATOR;
}

u8 Chip::ReadRegister(u32 offset)
{
  if (offset >= m_registers.size())
    return 0xff;

  if (offset != DATA_REGISTER)
    return m_registers[offset];

  if ((m_registers[CONTROL_REGISTER] & 0x10u) == 0)
    return 0;

  u8 value;
  if (m_data_bank == 0x80)
  {
    const u32 address = (m_data_pointer & 0x3fffu) | ((m_data_pointer & 0x10000u) >> 2);
    value = m_reverb_ram[address];
  }
  else
  {
    value = ReadSampleByte((static_cast<u32>(m_data_bank) * 0x20000u) + m_data_pointer);
  }

  m_data_pointer = (m_data_pointer + 1u) & 0x1ffffu;
  return value;
}

void Chip::WriteRegister(u32 offset, u8 value)
{
  if (offset >= m_registers.size())
    return;

  // K054539 internal-RAM CPU accesses use {A9,A7:0}; A8 is physically ignored.
  // Preserve the existing register-file behavior while also keeping the exact
  // alias-resolved RAM byte needed by the silicon special-slot shadow.
  if (m_pan_encoding == PanEncoding::KonamiGQ && (offset & 0x200u) == 0)
    m_gq_internal_ram_cpu_shadow[offset & 0xffu] = value;

  if (m_pan_encoding == PanEncoding::KonamiGQ && offset == CONTROL_REGISTER &&
      ((m_registers[CONTROL_REGISTER] ^ value) & 0x01u) != 0)
  {
    // The established GQ frame path treats control-bit0 deassertion as a
    // silent/reset interval. Keep the diagnostic REGEEB/REGEFB shadow aligned
    // across the corresponding disable/re-enable boundary.
    m_gq_axdt_full_feedback.fill(0);
    m_gq_axdt_full_feedback_valid = true;
    m_gq_axdt_closed_loop_feedback.fill(0);
    m_gq_axdt_closed_loop_feedback_valid = true;
    m_gq_axdt_rvv_loop_feedback.fill(0);
    m_gq_axdt_rvv_loop_feedback_valid = true;
    m_gq_axdt_signed_feedback.fill(0);
    m_gq_axdt_signed_feedback_valid = true;
  }

  const bool latch_position = (m_registers[CONTROL_REGISTER] & 0x01u) != 0;
  if (latch_position && offset < 0x100)
  {
    const s32 position_byte = static_cast<s32>(offset & 0x1fu) - 0x0c;
    const u32 channel = offset >> 5;
    if (channel < 8 && position_byte >= 0 && position_byte <= 2)
    {
      m_position_latch[channel][static_cast<u32>(position_byte)] = value;
      return;
    }
  }

  switch (offset)
  {
    case KEY_ON_REGISTER:
      for (u32 channel = 0; channel < 8; channel++)
      {
        if ((value & (1u << channel)) == 0)
          continue;

        if (latch_position)
        {
          const u32 base = channel * 0x20u;
          m_registers[base + 0x0c] = m_position_latch[channel][0];
          m_registers[base + 0x0d] = m_position_latch[channel][1];
          m_registers[base + 0x0e] = m_position_latch[channel][2];
        }
        KeyOn(channel);
      }
      break;

    case KEY_OFF_REGISTER:
      for (u32 channel = 0; channel < 8; channel++)
      {
        if (value & (1u << channel))
          KeyOff(channel);
      }
      break;

    case DATA_REGISTER:
      if (m_data_bank == 0x80)
      {
        const u32 address = (m_data_pointer & 0x3fffu) | ((m_data_pointer & 0x10000u) >> 2);
        m_reverb_ram[address] = value;
      }
      m_data_pointer = (m_data_pointer + 1u) & 0x1ffffu;
      break;

    case BANK_REGISTER:
      m_data_bank = value;
      m_data_pointer = 0;
      break;

    default:
      break;
  }

  m_registers[offset] = value;
}

void Chip::GenerateFrame(s32* left, s32* right, ChannelFrame* channel_output,
                         s32* aux_send_left, s32* aux_send_right,
                         RawChannelFrame* raw_channel_output, GQSourceSlotFrame* gq_source_slots,
                         GQOutputPlaneFrame* gq_output_planes, GQMuxFPathFrame* gq_muxf_paths,
                         GQMuxFShadowFrame* gq_muxf_shadow,
                         GQMuxFShadowValidFrame* gq_muxf_shadow_valid,
                         GQAXDTChannelContributionFrame* gq_axdt_channel_contribution,
                         GQAXDTCoefficientAddressFrame* gq_axdt_coefficient_address,
                         GQAXDTChannelMixFrame* gq_axdt_channel_mix,
                         bool* gq_axdt_channel_mix_valid,
                         GQAXDTFullMixFrame* gq_axdt_full_mix,
                         bool* gq_axdt_full_mix_valid,
                         GQAXDTClosedLoopMixFrame* gq_axdt_closed_loop_mix,
                         bool* gq_axdt_closed_loop_mix_valid,
                         GQAXDTRVVLoopMixFrame* gq_axdt_rvv_loop_mix,
                         bool* gq_axdt_rvv_loop_mix_valid,
                         GQAXDTSignedMixFrame* gq_axdt_signed_mix,
                         bool* gq_axdt_signed_mix_valid,
                         GQAXDTSignedChannelContributionFrame* gq_axdt_signed_channel_contribution,
                         double gq_audible_aux_gain)
{
  double left_value = 0.0;
  double right_value = 0.0;

  // GQ dry/output domain. Keep the established stereo synthesis as the DSP/AXDT
  // send while independently building the audible dry mix from the two 4-bit
  // PAN fields documented by SiliconRE. The downstream REGEA-D -> 056602 fold
  // remains separate from this reconstruction.
  double gq_independent_pan_left_value = 0.0;
  double gq_independent_pan_right_value = 0.0;
  std::array<u16, 2> gq_axdt_channel_accumulator = {};
  std::array<s16, 8> gq_muxf_slot_source = {};
  std::array<bool, 8> gq_muxf_slot_exact = {};

  // Passive GQ A-D discriminator. SiliconRE's output-plane pipeline does not
  // take the final MUXF_REG word directly at every REGEA-D update: an earlier
  // MULB/MUXC stage supplies the second multiplier input. Sound Scale proves
  // that the current MUXF shadow can legitimately be zero while audible PCM is
  // active, so retain CH_SAMPLE/current PCM independently as the first-stage
  // source candidate. This never participates in the audible stereo or AXDT
  // paths.
  std::array<s16, 8> gq_output_plane_pcm_source = {};

  bool gq_axdt_channels_exact = true;
  if (aux_send_left)
    *aux_send_left = 0;
  if (aux_send_right)
    *aux_send_right = 0;
  if (channel_output)
  {
    for (auto& channel : *channel_output)
      channel.fill(0);
  }
  if (raw_channel_output)
    raw_channel_output->fill(0);
  if (gq_source_slots)
    gq_source_slots->fill(0);
  if (gq_output_planes)
    gq_output_planes->fill(0);
  if (gq_muxf_paths)
    gq_muxf_paths->fill(GQMuxFPath::Unknown);
  if (gq_muxf_shadow)
    gq_muxf_shadow->fill(0);
  if (gq_muxf_shadow_valid)
    gq_muxf_shadow_valid->fill(0);
  if (gq_axdt_channel_contribution)
  {
    for (auto& contribution : *gq_axdt_channel_contribution)
      contribution.fill(0);
  }
  if (gq_axdt_signed_channel_contribution)
  {
    for (auto& contribution : *gq_axdt_signed_channel_contribution)
      contribution.fill(0);
  }
  if (gq_axdt_coefficient_address)
  {
    for (auto& address : *gq_axdt_coefficient_address)
      address.fill(0);
  }
  if (gq_axdt_channel_mix)
    gq_axdt_channel_mix->fill(0);
  if (gq_axdt_channel_mix_valid)
    *gq_axdt_channel_mix_valid = true;
  if (gq_axdt_full_mix)
    gq_axdt_full_mix->fill(0);
  if (gq_axdt_full_mix_valid)
    *gq_axdt_full_mix_valid = true;
  if (gq_axdt_closed_loop_mix)
    gq_axdt_closed_loop_mix->fill(0);
  if (gq_axdt_closed_loop_mix_valid)
    *gq_axdt_closed_loop_mix_valid = true;
  if (gq_axdt_rvv_loop_mix)
    gq_axdt_rvv_loop_mix->fill(0);
  if (gq_axdt_rvv_loop_mix_valid)
    *gq_axdt_rvv_loop_mix_valid = true;
  if (gq_axdt_signed_mix)
    gq_axdt_signed_mix->fill(0);
  if (gq_axdt_signed_mix_valid)
    *gq_axdt_signed_mix_valid = true;
  if ((m_registers[CONTROL_REGISTER] & 0x01u) == 0)
  {
    if (m_pan_encoding == PanEncoding::KonamiGQ)
    {
      m_gq_output_latches.fill(0);
      m_gq_axdt_full_feedback.fill(0);
      m_gq_axdt_full_feedback_valid = true;
      m_gq_axdt_closed_loop_feedback.fill(0);
      m_gq_axdt_closed_loop_feedback_valid = true;
      m_gq_axdt_rvv_loop_feedback.fill(0);
      m_gq_axdt_rvv_loop_feedback_valid = true;
      m_gq_axdt_signed_feedback.fill(0);
      m_gq_axdt_signed_feedback_valid = true;
    }
    *left = 0;
    *right = 0;
    return;
  }

  // The legacy software reverb model is retained only for the generic K054539
  // path. SiliconRE shows that GQ register 0x04 is two independent mixer
  // coefficients and registers 0x06/0x07 feed MULA; they are not the simple
  // reverb-volume-plus-delay model used by the old emulator approximation.
  const bool use_legacy_software_reverb = (m_pan_encoding != PanEncoding::KonamiGQ);
  if (use_legacy_software_reverb)
  {
    left_value = right_value = static_cast<double>(ReadReverbSample(m_reverb_position));
    WriteReverbSample(m_reverb_position, 0);
  }

  for (u32 channel_index = 0; channel_index < 8; channel_index++)
  {
    u8* const primary = &m_registers[channel_index * 0x20u];
    u8* const secondary = &m_registers[0x200u + channel_index * 2u];

    if ((m_registers[ACTIVE_REGISTER] & (1u << channel_index)) == 0)
    {
      // SiliconRE still walks every channel mixer slot while the PCM channel is
      // inactive. For the exact zero-even-MULA subset CH_SAMPLE is zero, so
      // DLAT_A/B/C are zero while P13 can retain one frame of feedback state.
      // Preserve that P13 tail as a real MUXF source instead of treating every
      // inactive channel slot as silent.
      if (m_pan_encoding == PanEncoding::KonamiGQ && primary[0x06] == 0x00u)
      {
        const u8 odd_control = secondary[1];
        const u8 p12 = (odd_control & 0x10u) ? 0u : 1u;
        const u8 p8 = (odd_control & 0x20u) ? 0u : 1u;
        const GQMuxFPath muxf_path = static_cast<GQMuxFPath>((p12 << 1) | p8);
        const u32 previous_p11 = m_gq_effect_feedback_p11[channel_index];
        const u32 previous_p13 = m_gq_effect_feedback_p13[channel_index];

        gq_muxf_slot_source[channel_index] =
          (muxf_path == GQMuxFPath::P13LAT) ? GQExtractMuxFWord(previous_p13) : 0;
        gq_muxf_slot_exact[channel_index] = true;

        m_gq_effect_feedback_p13[channel_index] =
          GQRunZeroEvenMulaP13FeedbackMicrosequence(previous_p11);
        m_gq_effect_feedback_p11[channel_index] =
          GQRunZeroEvenMulaP11FeedbackMicrosequence(0, primary[0x07]);
      }
      continue;
    }

    Channel& channel = m_channels[channel_index];

    const u32 raw_delta = static_cast<u32>(primary[0]) |
                          (static_cast<u32>(primary[1]) << 8) |
                          (static_cast<u32>(primary[2]) << 16);
    const s32 pitch_delta = static_cast<s32>(raw_delta);
    const u32 volume = primary[3];

    const u8 raw_pan = primary[5];
    u32 pan = raw_pan;
    if (m_pan_encoding == PanEncoding::KonamiGQ && pan >= 0x61 && pan <= 0x6f)
      pan -= 0x61;
    else if (pan >= 0x81 && pan <= 0x8f)
      pan -= 0x81;
    else if (pan >= 0x11 && pan <= 0x1f)
      pan -= 0x11;
    else
      pan = 7;

    constexpr double VOLUME_CAP = 1.80;
    const double channel_volume =
      (m_pan_encoding == PanEncoding::KonamiGQ) ?
        m_gq_volume_table[volume & 0x7fu] :
        m_volume_table[volume];
    const double left_volume = std::min(channel_volume * m_pan_table[pan], VOLUME_CAP);
    const double right_volume = std::min(channel_volume * m_pan_table[14u - pan], VOLUME_CAP);

    double gq_independent_left_volume = 0.0;
    double gq_independent_right_volume = 0.0;
    if (m_pan_encoding == PanEncoding::KonamiGQ)
    {
      // SiliconRE identifies PAN[7:4] as right-volume information and PAN[3:0]
      // as left-volume information. The two positive ROMB pan tables run in
      // opposite directions for codes 1-15; code 0 is zero in both tables.
      //
      // Use those fields directly for the current GQ audible dry fold:
      //   right -> ROMB 0x80..0x8F
      //   left  -> ROMB 0x90..0x9F
      //
      // The real chip feeds these coefficients into four REGEA-D output
      // accumulators. Until the downstream 056602 fold is established, this
      // two-channel projection remains provisional rather than a final silicon model.
      constexpr double GQ_PAN_COEFFICIENT_DENOMINATOR = 32767.0;
      const u8 right_pan_code = static_cast<u8>((raw_pan >> 4) & 0x0fu);
      const u8 left_pan_code = static_cast<u8>(raw_pan & 0x0fu);
      const double right_pan_gain =
        static_cast<double>(GQ_ROMB[0x80u | right_pan_code]) / GQ_PAN_COEFFICIENT_DENOMINATOR;
      const double left_pan_gain =
        static_cast<double>(GQ_ROMB[0x90u | left_pan_code]) / GQ_PAN_COEFFICIENT_DENOMINATOR;

      gq_independent_left_volume = std::min(channel_volume * left_pan_gain, VOLUME_CAP);
      gq_independent_right_volume = std::min(channel_volume * right_pan_gain, VOLUME_CAP);
    }

    u32 current_position = static_cast<u32>(primary[0x0c]) |
                           (static_cast<u32>(primary[0x0d]) << 8) |
                           (static_cast<u32>(primary[0x0e]) << 16);

    if (current_position != channel.position)
    {
      channel.position = current_position;
      channel.position_fraction = 0;
      channel.current_value = 0;
      channel.previous_value = 0;
    }

    s32 position_fraction = channel.position_fraction;
    s32 current_value = channel.current_value;
    s32 previous_value = channel.previous_value;
    const bool reverse = (secondary[0] & 0x20u) != 0;
    const s32 byte_step = reverse ? -1 : 1;

    switch (secondary[0] & 0x0cu)
    {
      case 0x00: // 8-bit PCM
      {
        position_fraction += reverse ? -pitch_delta : pitch_delta;
        while (reverse ? (position_fraction < 0) : (position_fraction >= 0x10000))
        {
          position_fraction += reverse ? 0x10000 : -0x10000;
          current_position = static_cast<u32>(static_cast<s32>(current_position) + byte_step) & 0x00ffffffu;
          previous_value = current_value;
          current_value = static_cast<s16>(static_cast<u16>(ReadSampleByte(current_position)) << 8);
          if (current_value == -32768 && (secondary[1] & 0x01u))
          {
            current_position = static_cast<u32>(primary[0x08]) |
                               (static_cast<u32>(primary[0x09]) << 8) |
                               (static_cast<u32>(primary[0x0a]) << 16);
            current_value = static_cast<s16>(static_cast<u16>(ReadSampleByte(current_position)) << 8);
          }
          if (current_value == -32768)
          {
            KeyOff(channel_index);
            current_value = 0;
            break;
          }
        }
        break;
      }

      case 0x04: // 16-bit little-endian PCM
      {
        const s32 sample_step = byte_step * 2;
        position_fraction += reverse ? -pitch_delta : pitch_delta;
        while (reverse ? (position_fraction < 0) : (position_fraction >= 0x10000))
        {
          position_fraction += reverse ? 0x10000 : -0x10000;
          current_position = static_cast<u32>(static_cast<s32>(current_position) + sample_step) & 0x00ffffffu;
          previous_value = current_value;
          current_value = static_cast<s16>(static_cast<u16>(ReadSampleByte(current_position)) |
                                           (static_cast<u16>(ReadSampleByte(current_position + 1u)) << 8));
          if (current_value == -32768 && (secondary[1] & 0x01u))
          {
            current_position = static_cast<u32>(primary[0x08]) |
                               (static_cast<u32>(primary[0x09]) << 8) |
                               (static_cast<u32>(primary[0x0a]) << 16);
            current_value = static_cast<s16>(static_cast<u16>(ReadSampleByte(current_position)) |
                                             (static_cast<u16>(ReadSampleByte(current_position + 1u)) << 8));
          }
          if (current_value == -32768)
          {
            KeyOff(channel_index);
            current_value = 0;
            break;
          }
        }
        break;
      }

      case 0x08: // 4-bit DPCM
      {
        s32 nibble_position = static_cast<s32>(current_position << 1);
        s32 nibble_fraction = position_fraction << 1;
        if (nibble_fraction & 0x10000)
        {
          nibble_fraction &= 0xffff;
          nibble_position |= 1;
        }

        nibble_fraction += reverse ? -pitch_delta : pitch_delta;
        while (reverse ? (nibble_fraction < 0) : (nibble_fraction >= 0x10000))
        {
          nibble_fraction += reverse ? 0x10000 : -0x10000;
          nibble_position += byte_step;
          previous_value = current_value;
          u8 encoded = ReadSampleByte(static_cast<u32>(nibble_position) >> 1);
          if (encoded == 0x88 && (secondary[1] & 0x01u))
          {
            nibble_position = static_cast<s32>((static_cast<u32>(primary[0x08]) |
                                                (static_cast<u32>(primary[0x09]) << 8) |
                                                (static_cast<u32>(primary[0x0a]) << 16)) << 1);
            encoded = ReadSampleByte(static_cast<u32>(nibble_position) >> 1);
          }
          if (encoded == 0x88)
          {
            KeyOff(channel_index);
            current_value = 0;
            break;
          }

          const u8 nibble = (nibble_position & 1) ? (encoded >> 4) : (encoded & 0x0f);
          current_value = Clamp16(previous_value + DPCM_DELTA[nibble]);
        }

        position_fraction = nibble_fraction >> 1;
        if (nibble_position & 1)
          position_fraction |= 0x8000;
        current_position = static_cast<u32>(nibble_position >> 1) & 0x00ffffffu;
        break;
      }

      default:
        break;
    }

    if (raw_channel_output)
      (*raw_channel_output)[channel_index] = current_value;

    if (m_pan_encoding == PanEncoding::KonamiGQ)
      gq_output_plane_pcm_source[channel_index] = static_cast<s16>(current_value);

    if (m_pan_encoding == PanEncoding::KonamiGQ)
    {
      // SiliconRE start/stop logic stores the complements of odd-control
      // bits 4/5. The final MUXF selector uses {P12,P8}:
      //   00 -> DLAT_C[30:15]
      //   01 -> DLAT_B[30:15]
      //   10 -> P13LAT[30:15]
      //   11 -> DLAT_A[30:15]
      const u8 odd_control = secondary[1];
      const u8 p12 = (odd_control & 0x10u) ? 0u : 1u;
      const u8 p8 = (odd_control & 0x20u) ? 0u : 1u;
      const GQMuxFPath muxf_path = static_cast<GQMuxFPath>((p12 << 1) | p8);
      if (gq_muxf_paths)
        (*gq_muxf_paths)[channel_index] = muxf_path;

      // Phase 3L12 established that every observed unresolved channel still
      // has the even MULA byte (register 0x06 / MULA_A_LATB) at zero. The
      // exact SiliconRE recurrence above now covers DLAT_A, DLAT_B, DLAT_C,
      // and the persistent P13 feedback source for that zero-even-MULA subset.
      //
      // P13 must be sampled before advancing the feedback RAM recurrence:
      // hardware reads the stored P13 word in the channel slot, then writes
      // the next P11/P13 words during the following slot.
      const bool zero_even_mula = (primary[0x06] == 0x00u);
      const u32 previous_p11 = m_gq_effect_feedback_p11[channel_index];
      const u32 previous_p13 = m_gq_effect_feedback_p13[channel_index];

      bool muxf_exact = false;
      u32 muxf_source_word = 0;
      if (zero_even_mula)
      {
        m_gq_effect_shadow.previous_mula_out = 0;
        m_gq_effect_shadow.adde = 0;
        m_gq_effect_shadow.mula_lat = 0;
        m_gq_effect_shadow.dlat_c =
          GQRunZeroEvenMulaDLATCMicrosequence(static_cast<s16>(current_value));
        m_gq_effect_shadow.dlat_b = GQRunZeroMulaDLATABMicrosequence();
        m_gq_effect_shadow.dlat_a =
          GQRunZeroMulaDLATAMicrosequence(static_cast<s16>(current_value));
        m_gq_effect_shadow.dlat_d =
          GQAddD(previous_p13, previous_p11, false, true, true);

        switch (muxf_path)
        {
          case GQMuxFPath::DLATC:
            muxf_source_word = m_gq_effect_shadow.dlat_c;
            muxf_exact = true;
            break;

          case GQMuxFPath::DLATB:
            muxf_source_word = m_gq_effect_shadow.dlat_b;
            muxf_exact = true;
            break;

          case GQMuxFPath::P13LAT:
            muxf_source_word = previous_p13;
            muxf_exact = true;
            break;

          case GQMuxFPath::DLATA:
            muxf_source_word = m_gq_effect_shadow.dlat_a;
            muxf_exact = true;
            break;

          default:
            break;
        }

        // Advance the per-channel feedback words only after the current MUXF
        // source has consumed the old P13 state.
        m_gq_effect_feedback_p13[channel_index] =
          GQRunZeroEvenMulaP13FeedbackMicrosequence(previous_p11);
        m_gq_effect_feedback_p11[channel_index] =
          GQRunZeroEvenMulaP11FeedbackMicrosequence(static_cast<s16>(current_value), primary[0x07]);
      }

      if (muxf_exact)
      {
        const s16 muxf_word = GQExtractMuxFWord(muxf_source_word);
        gq_muxf_slot_source[channel_index] = muxf_word;
        gq_muxf_slot_exact[channel_index] = true;

        const u8 regee_coefficient_address = GQGetChannelREGEECoefficientAddress(primary[0x04]);
        const u8 regef_coefficient_address =
          GQGetChannelREGEFCoefficientAddress(secondary[0], primary[0x05], m_registers[0x22fu]);
        if (gq_axdt_coefficient_address)
        {
          (*gq_axdt_coefficient_address)[channel_index][0] = regee_coefficient_address;
          (*gq_axdt_coefficient_address)[channel_index][1] = regef_coefficient_address;
        }
        const s16 regee_contribution =
          GQGetChannelAXDTContribution(muxf_word, regee_coefficient_address);
        const s16 regef_contribution =
          GQGetChannelAXDTContribution(muxf_word, regef_coefficient_address);
        if (gq_axdt_channel_contribution)
        {
          (*gq_axdt_channel_contribution)[channel_index][0] = regee_contribution;
          (*gq_axdt_channel_contribution)[channel_index][1] = regef_contribution;
        }
        if (gq_axdt_signed_channel_contribution)
        {
          (*gq_axdt_signed_channel_contribution)[channel_index][0] =
            static_cast<s16>(GQAccumulateREGE(
              0, GQMulBHighSigned(GQ_ROMB[regee_coefficient_address],
                                  static_cast<u16>(muxf_word))));
          (*gq_axdt_signed_channel_contribution)[channel_index][1] =
            static_cast<s16>(GQAccumulateREGE(
              0, GQMulBHighSigned(GQ_ROMB[regef_coefficient_address],
                                  static_cast<u16>(muxf_word))));
        }

        // Channels 0-7 are silicon slots 2-9. Each REGE update adds the
        // rounded multiplier term to the existing 16-bit register, so the
        // contribution computed from a zero accumulator can be wrap-added in
        // slot order without changing the recurrence.
        gq_axdt_channel_accumulator[0] =
          static_cast<u16>(gq_axdt_channel_accumulator[0] + static_cast<u16>(regee_contribution));
        gq_axdt_channel_accumulator[1] =
          static_cast<u16>(gq_axdt_channel_accumulator[1] + static_cast<u16>(regef_contribution));

        if (gq_muxf_shadow)
          (*gq_muxf_shadow)[channel_index] = muxf_word;
        if (gq_muxf_shadow_valid)
          (*gq_muxf_shadow_valid)[channel_index] = 1u;
        if (gq_source_slots)
          (*gq_source_slots)[GQ_MUXF_SLOT_BASE + channel_index] = muxf_word;
      }
      else
      {
        // The aggregate is exact only while every active channel uses a MUXF
        // source we have reconstructed. Nonzero-even-MULA paths remain
        // deliberately invalid rather than approximated.
        gq_axdt_channels_exact = false;
      }
    }

    const double channel_left = static_cast<double>(current_value) * left_volume;
    const double channel_right = static_cast<double>(current_value) * right_volume;
    left_value += channel_left;
    right_value += channel_right;

    if (m_pan_encoding == PanEncoding::KonamiGQ)
    {
      gq_independent_pan_left_value +=
        static_cast<double>(current_value) * gq_independent_left_volume;
      gq_independent_pan_right_value +=
        static_cast<double>(current_value) * gq_independent_right_volume;
    }

    // Optional post-volume, post-pan dry output for board-level mixing.
    if (channel_output)
    {
      (*channel_output)[channel_index][0] = static_cast<s32>(std::lrint(channel_left));
      (*channel_output)[channel_index][1] = static_cast<s32>(std::lrint(channel_right));
    }

    if (use_legacy_software_reverb)
    {
      const u32 legacy_reverb_volume = std::min<u32>(volume + primary[4], 255u);
      const double legacy_reverb_gain = std::min(m_volume_table[legacy_reverb_volume] / 2.0, VOLUME_CAP);
      const u32 legacy_reverb_delay =
        (static_cast<u32>(primary[6]) | (static_cast<u32>(primary[7]) << 8)) >> 3;
      const u32 legacy_reverb_destination = (legacy_reverb_delay + m_reverb_position) & 0x3fffu;
      const s16 old_reverb =
        ReadReverbSample((legacy_reverb_destination + m_reverb_position) & 0x1fffu);
      const s16 new_reverb =
        static_cast<s16>(old_reverb + static_cast<s16>(current_value * legacy_reverb_gain));
      WriteReverbSample((legacy_reverb_destination + m_reverb_position) & 0x1fffu, new_reverb);
    }

    channel.position = current_position;
    channel.position_fraction = position_fraction;
    channel.previous_value = previous_value;
    channel.current_value = current_value;

    if (RegisterUpdatesEnabled())
    {
      primary[0x0c] = static_cast<u8>(current_position);
      primary[0x0d] = static_cast<u8>(current_position >> 8);
      primary[0x0e] = static_cast<u8>(current_position >> 16);
    }
  }

  if (m_pan_encoding == PanEncoding::KonamiGQ)
  {
    // Build the complete REGEE/REGEF shadow in the actual twelve-slot order.
    const u8 effects_mux = m_registers[GQ_EFFECTS_MUX_REGISTER];
    if (gq_source_slots || gq_output_planes || gq_axdt_full_mix || gq_axdt_full_mix_valid)
    {
      // Output accumulators are cleared at the final-output load edge. REGEEB
      // and REGEFB then retain the completed previous-frame words for MUXH.
      std::array<u16, 2> gq_axdt_full_accumulator = {};
      std::array<u16, 4> gq_output_plane_accumulator = {};
      bool gq_axdt_full_exact = m_gq_axdt_full_feedback_valid;

      const auto mix_axdt_slot =
        [this, &gq_axdt_full_accumulator](u32 slot, s16 source, u8 reverb_volume, u8 pan)
      {
        const u8 control = m_registers[GQ_MIXER_CONTROL_REGISTERS[slot]];
        const u8 regee_address = GQGetChannelREGEECoefficientAddress(reverb_volume);
        const u8 regef_address =
          GQGetChannelREGEFCoefficientAddress(control, pan, m_registers[0x22fu]);
        gq_axdt_full_accumulator[0] =
          GQMixREGE(gq_axdt_full_accumulator[0], GQ_ROMB[regee_address], static_cast<u16>(source));
        gq_axdt_full_accumulator[1] =
          GQMixREGE(gq_axdt_full_accumulator[1], GQ_ROMB[regef_address], static_cast<u16>(source));
      };

      const auto mix_output_plane_slot =
        [this, gq_output_planes, &gq_output_plane_accumulator](u32 slot, s16 source, u8 pan)
      {
        if (!gq_output_planes || (m_registers[CONTROL_REGISTER] & 0x02u) != 0)
          return;

        s16 plane_source = source;
        if (slot >= GQ_MUXF_SLOT_BASE && slot < (GQ_MUXF_SLOT_BASE + 8u))
        {
          // Regular slots 2-9 still use the v4 discriminator approximation.
          // Their silicon MUXG source is MUXF_REG, whose earlier DLAT/MULB/MUXC
          // history is not yet reproduced exactly by the emulator shadow.
          //
          // Sound Scale proved that current PCM -> CH_VOL is the useful
          // first-stage boundary candidate for these slots:
          //   CH_SAMPLE/current PCM -> CH_VOL ROMB -> MUXC/MUXF history ->
          //   per-plane PAN/control ROMB -> REGE half-step.
          const u32 channel_index = slot - GQ_MUXF_SLOT_BASE;
          const u8 channel_volume = m_registers[(channel_index * 0x20u) + 0x03u] & 0x7fu;
          plane_source = static_cast<s16>(
            GQMulBHighSigned(GQ_ROMB[channel_volume], static_cast<u16>(source)));
        }
        else if (slot != GQ_MUXH_SLOT_A && slot != GQ_MUXH_SLOT_B &&
                 slot != GQ_AXDA_SLOT_A && slot != GQ_AXDA_SLOT_B)
        {
          return;
        }
        else
        {
          // SiliconRE static timing closes the special-slot first-stage
          // question. Slots 0/1 select MUXH and slots 10/11 select AXWORD
          // directly into MUXG, then MUXG_REG. During the REGEA-D update
          // phases AS70=1 and AJ106=1, so MULB_B is MUXG_REG; neither MULA nor
          // MUXC nor the CH_VOL/RVVOL attenuation path is inserted ahead of
          // the per-plane ROMB multiply.
          //
          // Therefore the v5 least-assumptive special candidate is the actual
          // silicon boundary for A-D:
          //   MUXH/AXDA source -> per-plane PAN/control ROMB ->
          //   signed MULB -> REGE half-step.
          plane_source = source;
        }

        const u8 control = m_registers[GQ_MIXER_CONTROL_REGISTERS[slot]];
        for (u32 plane = GQ_REGEA_PLANE; plane <= GQ_REGED_PLANE; plane++)
        {
          const u8 coefficient_address =
            GQGetChannelOutputPlaneCoefficientAddress(plane, control, pan);
          const s16 contribution = static_cast<s16>(
            GQMulBHighSigned(GQ_ROMB[coefficient_address], static_cast<u16>(plane_source)));
          gq_output_plane_accumulator[plane] =
            GQAccumulateREGE(gq_output_plane_accumulator[plane], contribution);
        }
      };

      s16 muxh_source_a = 0;
      s16 muxh_source_b = 0;
      const bool muxh_source_a_known =
        ((effects_mux & 0x02u) != 0 && m_gq_axdt_full_feedback_valid);
      const bool muxh_source_b_known =
        ((effects_mux & 0x01u) != 0 && m_gq_axdt_full_feedback_valid);
      if (muxh_source_a_known)
        muxh_source_a = static_cast<s16>(m_gq_axdt_full_feedback[0]);
      else
        gq_axdt_full_exact = false;
      if (muxh_source_b_known)
        muxh_source_b = static_cast<s16>(m_gq_axdt_full_feedback[1]);
      else
        gq_axdt_full_exact = false;

      mix_axdt_slot(GQ_MUXH_SLOT_A, muxh_source_a,
                    m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_A_RVVOL_BYTE] & 0x7fu,
                    m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_A_PAN_BYTE]);
      mix_axdt_slot(GQ_MUXH_SLOT_B, muxh_source_b,
                    m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_B_RVVOL_BYTE] & 0x7fu,
                    m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_B_PAN_BYTE]);
      if (muxh_source_a_known)
      {
        mix_output_plane_slot(
          GQ_MUXH_SLOT_A, muxh_source_a, m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_A_PAN_BYTE]);
      }
      if (muxh_source_b_known)
      {
        mix_output_plane_slot(
          GQ_MUXH_SLOT_B, muxh_source_b, m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_B_PAN_BYTE]);
      }

      if (gq_source_slots)
      {
        (*gq_source_slots)[GQ_MUXH_SLOT_A] = muxh_source_a;
        (*gq_source_slots)[GQ_MUXH_SLOT_B] = muxh_source_b;
      }

      for (u32 channel_index = 0; channel_index < 8; channel_index++)
      {
        const u32 slot = GQ_MUXF_SLOT_BASE + channel_index;
        const u8* const primary = &m_registers[channel_index * 0x20u];

        // AXDT/full-source diagnostics continue to use the independently
        // reconstructed MUXF path and preserve their existing validity rules.
        if (gq_muxf_slot_exact[channel_index])
        {
          const s16 muxf_source = gq_muxf_slot_source[channel_index];
          mix_axdt_slot(slot, muxf_source, primary[0x04], primary[0x05]);
          if (gq_source_slots)
            (*gq_source_slots)[slot] = muxf_source;
        }
        else
        {
          gq_axdt_full_exact = false;
        }

        // REGEA-D uses the separate CH_SAMPLE/MULB/MUXC first-stage candidate.
        // Inactive channels naturally contribute zero because the candidate
        // array is zero-initialized.
        mix_output_plane_slot(slot, gq_output_plane_pcm_source[channel_index], primary[0x05]);
      }

      const s16 axda_source_a = static_cast<s16>(m_aux_input[0]);
      const s16 axda_source_b = static_cast<s16>(m_aux_input[1]);
      mix_axdt_slot(GQ_AXDA_SLOT_A, axda_source_a,
                    m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_A_RVVOL_BYTE] & 0x7fu,
                    m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_A_PAN_BYTE]);
      mix_axdt_slot(GQ_AXDA_SLOT_B, axda_source_b,
                    m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_B_RVVOL_BYTE] & 0x7fu,
                    m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_B_PAN_BYTE]);
      mix_output_plane_slot(
        GQ_AXDA_SLOT_A, axda_source_a, m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_A_PAN_BYTE]);
      mix_output_plane_slot(
        GQ_AXDA_SLOT_B, axda_source_b, m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_B_PAN_BYTE]);

      if (gq_source_slots)
      {
        (*gq_source_slots)[GQ_AXDA_SLOT_A] = axda_source_a;
        (*gq_source_slots)[GQ_AXDA_SLOT_B] = axda_source_b;
      }

      if (gq_axdt_full_mix)
      {
        (*gq_axdt_full_mix)[0] = static_cast<s16>(gq_axdt_full_accumulator[0]);
        (*gq_axdt_full_mix)[1] = static_cast<s16>(gq_axdt_full_accumulator[1]);
      }
      if (gq_axdt_full_mix_valid)
        *gq_axdt_full_mix_valid = gq_axdt_full_exact;

      m_gq_axdt_full_feedback = gq_axdt_full_accumulator;
      m_gq_axdt_full_feedback_valid = gq_axdt_full_exact;

      if (gq_output_planes)
      {
        for (u32 plane = GQ_REGEA_PLANE; plane <= GQ_REGED_PLANE; plane++)
          m_gq_output_latches[plane] = gq_output_plane_accumulator[plane];
      }
    }

    if (gq_axdt_closed_loop_mix || gq_axdt_closed_loop_mix_valid)
    {
      // Independent closed-loop shadow. Slots 2-9 reuse the validated channel
      // MUXF words, while slots 0/1 and 10/11 use this shadow domain's own
      // previous REGEE/F and AXDA return respectively.
      std::array<u16, 2> gq_axdt_closed_loop_accumulator = {};
      bool gq_axdt_closed_loop_exact = m_gq_axdt_closed_loop_feedback_valid;

      const auto mix_closed_loop_slot =
        [this, &gq_axdt_closed_loop_accumulator](u32 slot, s16 source, u8 reverb_volume, u8 pan)
      {
        const u8 control = m_registers[GQ_MIXER_CONTROL_REGISTERS[slot]];
        const u8 regee_address = GQGetChannelREGEECoefficientAddress(reverb_volume);
        const u8 regef_address =
          GQGetChannelREGEFCoefficientAddress(control, pan, m_registers[0x22fu]);
        gq_axdt_closed_loop_accumulator[0] =
          GQMixREGE(gq_axdt_closed_loop_accumulator[0], GQ_ROMB[regee_address], static_cast<u16>(source));
        gq_axdt_closed_loop_accumulator[1] =
          GQMixREGE(gq_axdt_closed_loop_accumulator[1], GQ_ROMB[regef_address], static_cast<u16>(source));
      };

      s16 closed_muxh_source_a = 0;
      s16 closed_muxh_source_b = 0;
      if ((effects_mux & 0x02u) != 0 && m_gq_axdt_closed_loop_feedback_valid)
        closed_muxh_source_a = static_cast<s16>(m_gq_axdt_closed_loop_feedback[0]);
      else
        gq_axdt_closed_loop_exact = false;
      if ((effects_mux & 0x01u) != 0 && m_gq_axdt_closed_loop_feedback_valid)
        closed_muxh_source_b = static_cast<s16>(m_gq_axdt_closed_loop_feedback[1]);
      else
        gq_axdt_closed_loop_exact = false;

      mix_closed_loop_slot(GQ_MUXH_SLOT_A, closed_muxh_source_a,
                           m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_A_RVVOL_BYTE] & 0x7fu,
                           m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_A_PAN_BYTE]);
      mix_closed_loop_slot(GQ_MUXH_SLOT_B, closed_muxh_source_b,
                           m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_B_RVVOL_BYTE] & 0x7fu,
                           m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_B_PAN_BYTE]);

      for (u32 channel_index = 0; channel_index < 8; channel_index++)
      {
        if (!gq_muxf_slot_exact[channel_index])
        {
          gq_axdt_closed_loop_exact = false;
          continue;
        }

        const u32 slot = GQ_MUXF_SLOT_BASE + channel_index;
        const u8* const primary = &m_registers[channel_index * 0x20u];
        mix_closed_loop_slot(slot, gq_muxf_slot_source[channel_index], primary[0x04], primary[0x05]);
      }

      mix_closed_loop_slot(GQ_AXDA_SLOT_A, static_cast<s16>(m_gq_shadow_aux_input[0]),
                           m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_A_RVVOL_BYTE] & 0x7fu,
                           m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_A_PAN_BYTE]);
      mix_closed_loop_slot(GQ_AXDA_SLOT_B, static_cast<s16>(m_gq_shadow_aux_input[1]),
                           m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_B_RVVOL_BYTE] & 0x7fu,
                           m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_B_PAN_BYTE]);

      if (gq_axdt_closed_loop_mix)
      {
        (*gq_axdt_closed_loop_mix)[0] = static_cast<s16>(gq_axdt_closed_loop_accumulator[0]);
        (*gq_axdt_closed_loop_mix)[1] = static_cast<s16>(gq_axdt_closed_loop_accumulator[1]);
      }
      if (gq_axdt_closed_loop_mix_valid)
        *gq_axdt_closed_loop_mix_valid = gq_axdt_closed_loop_exact;

      m_gq_axdt_closed_loop_feedback = gq_axdt_closed_loop_accumulator;
      m_gq_axdt_closed_loop_feedback_valid = gq_axdt_closed_loop_exact;

    }

    if (gq_axdt_rvv_loop_mix || gq_axdt_rvv_loop_mix_valid)
    {
      // Optional RVVOL-pair diagnostic. Silicon reconstruction exposes both
      // RVVOL nibbles as distinct non-pan ROMB lookup cases, while the pan
      // generator supplies later table phases. Keep this isolated from the
      // audible path and evaluate it only when a diagnostic caller requests it.
      std::array<u16, 2> gq_axdt_rvv_loop_accumulator = {};
      bool gq_axdt_rvv_loop_exact = m_gq_axdt_rvv_loop_feedback_valid;

      const auto mix_rvv_loop_slot =
        [&gq_axdt_rvv_loop_accumulator](s16 source, u8 reverb_volume)
      {
        const u8 regee_address = GQGetRVVOLLowCoefficientAddress(reverb_volume);
        const u8 regef_address = GQGetRVVOLHighCoefficientAddress(reverb_volume);
        gq_axdt_rvv_loop_accumulator[0] =
          GQMixREGE(gq_axdt_rvv_loop_accumulator[0], GQ_ROMB[regee_address], static_cast<u16>(source));
        gq_axdt_rvv_loop_accumulator[1] =
          GQMixREGE(gq_axdt_rvv_loop_accumulator[1], GQ_ROMB[regef_address], static_cast<u16>(source));
      };

      s16 rvv_muxh_source_a = 0;
      s16 rvv_muxh_source_b = 0;
      if ((effects_mux & 0x02u) != 0 && m_gq_axdt_rvv_loop_feedback_valid)
        rvv_muxh_source_a = static_cast<s16>(m_gq_axdt_rvv_loop_feedback[0]);
      else
        gq_axdt_rvv_loop_exact = false;
      if ((effects_mux & 0x01u) != 0 && m_gq_axdt_rvv_loop_feedback_valid)
        rvv_muxh_source_b = static_cast<s16>(m_gq_axdt_rvv_loop_feedback[1]);
      else
        gq_axdt_rvv_loop_exact = false;

      mix_rvv_loop_slot(rvv_muxh_source_a,
                        m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_A_RVVOL_BYTE] & 0x7fu);
      mix_rvv_loop_slot(rvv_muxh_source_b,
                        m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_B_RVVOL_BYTE] & 0x7fu);

      for (u32 channel_index = 0; channel_index < 8; channel_index++)
      {
        if (!gq_muxf_slot_exact[channel_index])
        {
          gq_axdt_rvv_loop_exact = false;
          continue;
        }

        const u8* const primary = &m_registers[channel_index * 0x20u];
        mix_rvv_loop_slot(gq_muxf_slot_source[channel_index], primary[0x04]);
      }

      mix_rvv_loop_slot(static_cast<s16>(m_gq_rvv_aux_input[0]),
                        m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_A_RVVOL_BYTE] & 0x7fu);
      mix_rvv_loop_slot(static_cast<s16>(m_gq_rvv_aux_input[1]),
                        m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_B_RVVOL_BYTE] & 0x7fu);

      if (gq_axdt_rvv_loop_mix)
      {
        (*gq_axdt_rvv_loop_mix)[0] = static_cast<s16>(gq_axdt_rvv_loop_accumulator[0]);
        (*gq_axdt_rvv_loop_mix)[1] = static_cast<s16>(gq_axdt_rvv_loop_accumulator[1]);
      }
      if (gq_axdt_rvv_loop_mix_valid)
        *gq_axdt_rvv_loop_mix_valid = gq_axdt_rvv_loop_exact;

      m_gq_axdt_rvv_loop_feedback = gq_axdt_rvv_loop_accumulator;
      m_gq_axdt_rvv_loop_feedback_valid = gq_axdt_rvv_loop_exact;

    }

    if (gq_axdt_signed_mix || gq_axdt_signed_mix_valid)
    {
      // Optional signed-MULB full-shadow diagnostic. The resolved timing model
      // uses the same slot source and ROMB addresses as the twelve-slot shadow;
      // the remaining arithmetic distinction here is multiplier signedness.
      std::array<u16, 2> gq_axdt_signed_accumulator = {};
      bool gq_axdt_signed_exact = m_gq_axdt_signed_feedback_valid;

      const auto mix_signed_slot =
        [this, &gq_axdt_signed_accumulator](u32 slot, s16 source, u8 reverb_volume, u8 pan)
      {
        const u8 control = m_registers[GQ_MIXER_CONTROL_REGISTERS[slot]];
        const u8 regee_address = GQGetChannelREGEECoefficientAddress(reverb_volume);
        const u8 regef_address =
          GQGetChannelREGEFCoefficientAddress(control, pan, m_registers[0x22fu]);
        gq_axdt_signed_accumulator[0] =
          GQAccumulateREGE(gq_axdt_signed_accumulator[0],
                           GQMulBHighSigned(GQ_ROMB[regee_address], static_cast<u16>(source)));
        gq_axdt_signed_accumulator[1] =
          GQAccumulateREGE(gq_axdt_signed_accumulator[1],
                           GQMulBHighSigned(GQ_ROMB[regef_address], static_cast<u16>(source)));
      };

      s16 signed_muxh_source_a = 0;
      s16 signed_muxh_source_b = 0;
      if ((effects_mux & 0x02u) != 0 && m_gq_axdt_signed_feedback_valid)
        signed_muxh_source_a = static_cast<s16>(m_gq_axdt_signed_feedback[0]);
      else
        gq_axdt_signed_exact = false;
      if ((effects_mux & 0x01u) != 0 && m_gq_axdt_signed_feedback_valid)
        signed_muxh_source_b = static_cast<s16>(m_gq_axdt_signed_feedback[1]);
      else
        gq_axdt_signed_exact = false;

      mix_signed_slot(GQ_MUXH_SLOT_A, signed_muxh_source_a,
                      m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_A_RVVOL_BYTE] & 0x7fu,
                      m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_A_PAN_BYTE]);
      mix_signed_slot(GQ_MUXH_SLOT_B, signed_muxh_source_b,
                      m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_B_RVVOL_BYTE] & 0x7fu,
                      m_gq_internal_ram_cpu_shadow[GQ_MUXH_SLOT_B_PAN_BYTE]);

      for (u32 channel_index = 0; channel_index < 8; channel_index++)
      {
        if (!gq_muxf_slot_exact[channel_index])
        {
          gq_axdt_signed_exact = false;
          continue;
        }

        const u32 slot = GQ_MUXF_SLOT_BASE + channel_index;
        const u8* const primary = &m_registers[channel_index * 0x20u];
        mix_signed_slot(slot, gq_muxf_slot_source[channel_index], primary[0x04], primary[0x05]);
      }

      // Keep AXDA identical to the active TMS return so this diagnostic
      // isolates K054539 multiplier arithmetic. No second DSP executes.
      mix_signed_slot(GQ_AXDA_SLOT_A, static_cast<s16>(m_aux_input[0]),
                      m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_A_RVVOL_BYTE] & 0x7fu,
                      m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_A_PAN_BYTE]);
      mix_signed_slot(GQ_AXDA_SLOT_B, static_cast<s16>(m_aux_input[1]),
                      m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_B_RVVOL_BYTE] & 0x7fu,
                      m_gq_internal_ram_cpu_shadow[GQ_AXDA_SLOT_B_PAN_BYTE]);

      if (gq_axdt_signed_mix)
      {
        (*gq_axdt_signed_mix)[0] = static_cast<s16>(gq_axdt_signed_accumulator[0]);
        (*gq_axdt_signed_mix)[1] = static_cast<s16>(gq_axdt_signed_accumulator[1]);
      }
      if (gq_axdt_signed_mix_valid)
        *gq_axdt_signed_mix_valid = gq_axdt_signed_exact;

      m_gq_axdt_signed_feedback = gq_axdt_signed_accumulator;
      m_gq_axdt_signed_feedback_valid = gq_axdt_signed_exact;

    }

    if (gq_axdt_channel_mix)
    {
      (*gq_axdt_channel_mix)[0] = static_cast<s16>(gq_axdt_channel_accumulator[0]);
      (*gq_axdt_channel_mix)[1] = static_cast<s16>(gq_axdt_channel_accumulator[1]);
    }
    if (gq_axdt_channel_mix_valid)
      *gq_axdt_channel_mix_valid = gq_axdt_channels_exact;
  }

  if (use_legacy_software_reverb)
    m_reverb_position = (m_reverb_position + 1u) & 0x1fffu;

  // Controlled GQ follow-up: use the same independently decoded PAN projection
  // for the provisional AXDT/TMS send as for the dry audition. This changes no
  // global gain and keeps AXDA return calibration untouched. It is an audible
  // diagnostic only; the final REGEE/REGEF silicon schedule remains the target.
  const double provisional_axdt_left =
    (m_pan_encoding == PanEncoding::KonamiGQ) ? gq_independent_pan_left_value : left_value;
  const double provisional_axdt_right =
    (m_pan_encoding == PanEncoding::KonamiGQ) ? gq_independent_pan_right_value : right_value;
  const s32 provisional_axdt_a = static_cast<s32>(std::lrint(provisional_axdt_left));
  const s32 provisional_axdt_b = static_cast<s32>(std::lrint(provisional_axdt_right));
  if (aux_send_left)
    *aux_send_left = provisional_axdt_a;
  if (aux_send_right)
    *aux_send_right = provisional_axdt_b;

  if (m_pan_encoding == PanEncoding::KonamiGQ)
  {
    // Change only the audible dry projection for this audition. AXDT/DSP send
    // above intentionally remains the established baseline, so any listening
    // difference is attributable to the dry GQ PAN construction rather than a
    // simultaneous change in TMS57002 drive.
    left_value = gq_independent_pan_left_value;
    right_value = gq_independent_pan_right_value;

    // Keep the established audible output latches separate from the exact
    // twelve-slot REGEE/REGEF shadow above. The latter now owns MUXH feedback
    // diagnostics; these provisional latches remain only until the shadow
    // AXDT stream is runtime-validated and intentionally made audible.
    m_gq_output_latches[GQ_REGEE_PLANE] = static_cast<u16>(provisional_axdt_a);
    m_gq_output_latches[GQ_REGEF_PLANE] = static_cast<u16>(provisional_axdt_b);

    if (gq_output_planes)
    {
      for (u32 plane = 0; plane < m_gq_output_latches.size(); plane++)
        (*gq_output_planes)[plane] = static_cast<s16>(m_gq_output_latches[plane]);
    }

    // AXDA is not a post-mix analog return in the silicon. It is deserialized
    // into source slots 10/11 and processed by the same MULB/six-accumulator
    // output mixer as the other ten sources. Keep the current audible placement
    // unchanged for this foundation step, but derive its attenuation from the
    // exact slot controls so the next phase can replace this provisional add
    // with the real REGEA-F schedule without another routing assumption.
    left_value +=
      static_cast<double>(m_aux_input[0]) * GetGQMixerSlotGain(GQ_AXDA_SLOT_A) * gq_audible_aux_gain;
    right_value +=
      static_cast<double>(m_aux_input[1]) * GetGQMixerSlotGain(GQ_AXDA_SLOT_B) * gq_audible_aux_gain;
  }

  // Preserve the complete chip mix until the final board output clamp.
  *left = static_cast<s32>(std::lrint(left_value));
  *right = static_cast<s32>(std::lrint(right_value));
}

} // namespace K054539