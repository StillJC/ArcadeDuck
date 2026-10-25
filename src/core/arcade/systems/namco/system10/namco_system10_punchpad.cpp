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
// Full fall time for the hardest and lightest punch. Ippo 2 reads the MID-to-DOWN interval as
// about 600 and 100 kgw; it treats 1000 kgw or more as a sensor fault.
constexpr float HARD_FALL_MS = 72.0f;
constexpr float LIGHT_FALL_MS = 177.0f;

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

  // Strength scales the pad speed.
  strength = std::clamp(strength, 0.0f, 1.0f);
  const float speed = (1.0f / LIGHT_FALL_MS) + ((1.0f / HARD_FALL_MS) - (1.0f / LIGHT_FALL_MS)) * strength;
  StartMotion(Motion::Falling, 0.0f, MillisecondsToTicks(1.0f / speed), now);
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

} // namespace NamcoSystem10
