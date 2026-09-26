// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/security/cat702.h"

namespace CAT702 {

namespace {

constexpr Chip::Key INITIAL_TRANSFORM = {
  0xff, 0xfe, 0xfc, 0xf8, 0xf0, 0xe0, 0xc0, 0x7f,
};

} // namespace

void Chip::Initialize(const Key& key)
{
  m_key = key;
  Reset();
}

void Chip::Reset()
{
  m_state = 0;
  m_bit = 0;

  m_select = true;
  m_clock = true;
  m_data_in = true;
  m_data_out = true;
}

void Chip::SetSelectLine(bool high)
{
  if (m_select == high)
    return;

  m_select = high;

  if (!m_select)
  {
    // A new selected transaction starts from the CAT702's fixed state.
    m_state = 0xfc;
    m_bit = 0;
  }
  else
  {
    // The device releases the shared serial output while deselected.
    m_data_out = true;
  }
}

void Chip::SetClockLine(bool high)
{
  if (m_clock == high)
    return;

  const bool falling_edge = (m_clock && !high);
  const bool rising_edge = (!m_clock && high);
  m_clock = high;

  if (m_select)
    return;

  if (falling_edge)
  {
    // The fixed transform is applied at the start of each 8-bit group.
    if (m_bit == 0)
      m_state = ApplyTransform(m_state, INITIAL_TRANSFORM);

    m_data_out = ((m_state >> m_bit) & 1u) != 0;
  }
  else if (rising_edge)
  {
    // Input zero transforms the internal state; input one leaves it unchanged.
    if (!m_data_in)
      ApplyKeyTransform(m_bit);

    m_bit = static_cast<u8>((m_bit + 1u) & 7u);
  }
}

void Chip::SetDataInLine(bool high)
{
  m_data_in = high;
}

u8 Chip::ShiftCoefficient(u8 value)
{
  const u8 feedback = static_cast<u8>(((value >> 7) ^ (value >> 6)) & 1u);
  return static_cast<u8>((value << 1) | feedback);
}

u8 Chip::ApplyTransform(u8 value, const Key& coefficients)
{
  u8 result = 0;

  for (u32 bit = 0; bit < KEY_SIZE; bit++)
  {
    if ((value & (1u << bit)) != 0)
      result ^= coefficients[bit];
  }

  return result;
}

u8 Chip::ComputeCoefficient(u32 selector, u32 bit) const
{
  if (selector == 0)
    return m_key[bit & 7u];

  u8 result = ComputeCoefficient(selector - 1u, (bit - 1u) & 7u);
  result = ShiftCoefficient(result);

  if (bit != 7)
    return result;

  return static_cast<u8>(result ^ ComputeCoefficient(selector, 0));
}

void Chip::ApplyKeyTransform(u32 selector)
{
  Key coefficients = {};

  for (u32 bit = 0; bit < KEY_SIZE; bit++)
    coefficients[bit] = ComputeCoefficient(selector, bit);

  m_state = ApplyTransform(m_state, coefficients);
}

} // namespace CAT702