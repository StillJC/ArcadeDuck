// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>

namespace CAT702 {

class Chip
{
public:
  static constexpr u32 KEY_SIZE = 8;
  using Key = std::array<u8, KEY_SIZE>;

  void Initialize(const Key& key);
  void Reset();

  // CAT702 select is active low. These methods expose the physical line levels
  // so ZN SIO timing can be modeled separately from the security transform.
  void SetSelectLine(bool high);
  void SetClockLine(bool high);
  void SetDataInLine(bool high);

  bool GetDataOutLine() const { return m_data_out; }
  bool IsSelected() const { return !m_select; }

private:
  static u8 ShiftCoefficient(u8 value);
  static u8 ApplyTransform(u8 value, const Key& coefficients);

  u8 ComputeCoefficient(u32 selector, u32 bit) const;
  void ApplyKeyTransform(u32 selector);

  Key m_key = {};
  u8 m_state = 0;
  u8 m_bit = 0;

  bool m_select = true;
  bool m_clock = true;
  bool m_data_in = true;
  bool m_data_out = true;
};

} // namespace CAT702