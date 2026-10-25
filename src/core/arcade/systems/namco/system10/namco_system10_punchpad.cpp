// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system10/namco_system10_punchpad.h"

#include "core/system.h"

#include <algorithm>

namespace NamcoSystem10 {
namespace {
// Pad travel between upright (0) and down (1) covered by each sensor. MID clears before DOWN
// closes; the games treat two speed sensors on at once as a sensor fault.
constexpr float UP_END = 0.05f;
constexpr float MID_BEGIN = 0.40f;
constexpr float MID_END = 0.60f;
constexpr float DOWN_BEGIN = 0.95f;

// The test and the game allow the solenoid 4 seconds to reach UP.
constexpr u64 RISE_MS = 400;
// A released pad that never latched drops back on its own weight.
constexpr u64 DROP_MS = 300;
// Full fall time for strength 1 and 0. Ippo 2 reads the MID-to-DOWN interval as about 990 and
// 56.5 kgw. It treats 999.999 kgw or more as a sensor fault, which its jitter of up to 2 kgw
// reaches below 416 sampler ticks, so strength 1 lands at 417. A reading must exceed 56 kgw to
// score.
constexpr float HARD_FALL_MS = 58.0f;
constexpr float LIGHT_FALL_MS = 243.0f;

// Tap rates (taps per second) mapped to strength 0 and 1. Faster tapping keeps raising the
// strength and knocks the pad down faster than the games accept: Ippo 2 reports a sensor fault,
// the original Ippo shows its 999.999 kgw maximum.
// Host input arrives once per frame, so an eight-tap burst at 16 taps/s spans 26 frames and
// measures 16.13; the next shorter burst (25 frames, 16.8 taps/s) is the first that faults.
constexpr float SLOW_TAPS_PER_SECOND = 3.0f;
constexpr float FAST_TAPS_PER_SECOND = 16.13f;
constexpr u32 MAX_TAPS = 8;
// Presses closer together than this are switch bounce.
constexpr float TAP_DEBOUNCE_MS = 30.0f;

u64 MillisecondsToTicks(float ms)
{
  return std::max<u64>(static_cast<u64>(static_cast<float>(System::GetTicksPerSecond()) * ms / 1000.0f), 1);
}
} // namespace

void PunchPad::Reset()
{
  m_motion = Motion::Down;
  m_from = 1.0f;
  m_start = 0;
  m_duration = 1;
  m_solenoid = false;
}

u64 PunchPad::Now()
{
  return System::GetGlobalTickCounter();
}

float PunchPad::Position(u64 now) const
{
  const float progress =
    std::clamp(static_cast<float>(now - m_start) / static_cast<float>(m_duration), 0.0f, 1.0f);
  switch (m_motion)
  {
    case Motion::Down:
      return 1.0f;
    case Motion::Up:
      return 0.0f;
    case Motion::Rising:
      return m_from - m_from * progress;
    case Motion::Falling:
      return m_from + (1.0f - m_from) * progress;
  }
  return 1.0f;
}

void PunchPad::StartMotion(Motion motion, float from, u64 duration, u64 now)
{
  m_motion = motion;
  m_from = from;
  m_start = now;
  m_duration = std::max<u64>(duration, 1);
}

void PunchPad::Update(u64 now)
{
  if ((m_motion == Motion::Rising || m_motion == Motion::Falling) && (now - m_start) >= m_duration)
    m_motion = (m_motion == Motion::Rising) ? Motion::Up : Motion::Down;

  if (m_motion == Motion::Down && m_solenoid)
    StartMotion(Motion::Rising, 1.0f, MillisecondsToTicks(static_cast<float>(RISE_MS)), now);
}

void PunchPad::SetSolenoid(bool on)
{
  const u64 now = Now();
  Update(now);
  m_solenoid = on;
  if (!on && m_motion == Motion::Rising)
  {
    const float position = Position(now);
    StartMotion(Motion::Falling, position, MillisecondsToTicks(static_cast<float>(DROP_MS) * (1.0f - position)), now);
  }
  Update(now);
}

void PunchPad::Punch(float strength)
{
  const u64 now = Now();
  Update(now);
  if (m_motion != Motion::Up)
    return;

  // Strength scales the pad speed; above 1 the pad falls faster than the games accept.
  strength = std::max(strength, 0.0f);
  const float speed = (1.0f / LIGHT_FALL_MS) + ((1.0f / HARD_FALL_MS) - (1.0f / LIGHT_FALL_MS)) * strength;
  StartMotion(Motion::Falling, 0.0f, MillisecondsToTicks(1.0f / speed), now);
}

bool PunchPad::IsUp()
{
  Update(Now());
  return m_motion == Motion::Up;
}

u32 PunchPad::ReadSensors()
{
  const u64 now = Now();
  Update(now);
  const float position = Position(now);

  u32 bits = 0;
  if (position < UP_END)
    bits |= SENSOR_UP;
  if (position >= MID_BEGIN && position < MID_END)
    bits |= SENSOR_MID;
  if (position >= DOWN_BEGIN)
    bits |= SENSOR_DOWN;
  else
    bits |= SENSOR_SAFETY; // the lowered pad blocks the safety beam
  return bits;
}

void PunchButton::Reset()
{
  m_first_tap = 0;
  m_last_tap = 0;
  m_taps = 0;
}

bool PunchButton::Tap(u64 now)
{
  if (m_taps == 0)
    m_first_tap = now;
  else if ((now - m_last_tap) < MillisecondsToTicks(TAP_DEBOUNCE_MS))
    return false;

  m_last_tap = now;
  m_taps++;
  return m_taps >= MAX_TAPS;
}

bool PunchButton::Paused(u64 now) const
{
  return (now - m_last_tap) >= MillisecondsToTicks(1000.0f / SLOW_TAPS_PER_SECOND);
}

float PunchButton::Strength() const
{
  if (m_taps < 2)
    return 0.0f;

  // Mean interval over the burst. Host input arrives once per frame, so a steady rate between
  // two frame multiples alternates between them; the mean recovers it.
  const float rate = static_cast<float>(System::GetTicksPerSecond()) * static_cast<float>(m_taps - 1) /
                     static_cast<float>(m_last_tap - m_first_tap);
  return std::max((rate - SLOW_TAPS_PER_SECOND) / (FAST_TAPS_PER_SECOND - SLOW_TAPS_PER_SECOND), 0.0f);
}

} // namespace NamcoSystem10
