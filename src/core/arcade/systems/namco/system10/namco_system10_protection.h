// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>
#include <span>
#include <string_view>

namespace NamcoSystem10 {

enum class MemNBoard : u8
{
  MemN = 0,
  MemP3,
  MemIO,
};

enum class ExpansionBoard : u8
{
  None = 0,
  EXIO,      // TMP95C061-based EXIO, identifier 0x30
  EXIOG,     // CPU-less EXIO(G), identifier 0x32
  MGEXIO,    // H8/3007-based medal EXIO, identifier 0x33
  EXFINALIO, // medal I/O board, not identified through the EXIO serializer
};

// Type-2 protection CPLD. Each output mask bit is the parity of the previous
// four cipher/plain words selected by one cipher and one plain mask, then the
// fixed XOR and one board-specific nonlinear term are applied.
struct Type2Logic
{
  std::array<u64, 16> cipher_masks;
  std::array<u64, 16> plain_masks;
  u16 xor_mask;
  u16 (*nonlinear)(u64 previous_cipherwords, u64 previous_plainwords);
  const std::array<u64, 16>* initial_history;
  // Nonzero when the nonlinear term is one bit read sequentially from the
  // board's nonlinear table ROM, shifted into this mask bit position.
  u8 nonlinear_table_shift;
};

struct Type2State
{
  u64 previous_cipherwords = 0;
  u64 previous_plainwords = 0;
  u16 mask = 0;
  u32 nonlinear_count = 0;
};

struct MemNProfile
{
  std::string_view hardware_profile;
  MemNBoard board;
  ExpansionBoard expansion;
  u16 data_xor;
  // Source bit for output bits 15 down to 0 of the static NAND unscramble.
  std::array<u8, 16> static_bit_order;
  const Type2Logic* type2;
};

const MemNProfile* FindMemNProfile(std::string_view hardware_profile);

u16 DecodeMemNStaticWord(const MemNProfile& profile, u16 raw_word, u16 data_xor);

void InitializeType2State(const Type2Logic& logic, Type2State& state, u8 iv);
u16 DecodeType2Word(const Type2Logic& logic, Type2State& state, u16 cipher_word, std::span<const u8> nonlinear_table);

} // namespace NamcoSystem10
