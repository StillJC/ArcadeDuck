// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system10/namco_system10_kd2001.h"
#include "core/arcade/systems/namco/system10/namco_system10_pdrive.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace NamcoSystem10 {
namespace {
constexpr double CLOCK_HZ = static_cast<double>(PDriveBoard::CPU_CLOCK_HZ);

constexpr double TRAVEL = 6000.0;
constexpr double LIMIT_WIDTH = 20.0;

struct FlagSensor
{
  u8 bit; // in the PA4-PA7 sensor byte
  double from;
  double to;
};

// Positions over which the carriage flag blocks the timed sensors. Just behind the front limit the
// flag crosses SENSOR3 (PA7) and then the wider SENSOR2 (PA6) vane.
constexpr std::array<FlagSensor, 2> FLAG_SENSORS = {{
  {3, TRAVEL - 100.0, TRAVEL - 40.0},
  {2, TRAVEL - 215.0, TRAVEL - 140.0},
}};

// A punch drives the carriage back at constant speed through the sensors to the end of its stroke.
constexpr double PUNCH_STROKE = 300.0;

// Stroke speeds for strength 0 and 1 (steps/s). The game scores 1.5e6 / t, with t the SENSOR3
// pulse in 256/7.3728 MHz units, and takes a result over 4000 (t < 375) as a bad reading. The
// light punch clears SENSOR3 within the 0.9 s scoring window that follows the gong.
constexpr double LIGHT_SPEED = 220.0;
constexpr double HARD_SPEED = 4571.0;
} // namespace

void KnockDownCarriage::Reset()
{
  m_position = TRAVEL / 2.0;
  m_rate = 0.0;
  m_origin = 0;
  m_punch_speed = 0.0;
}

double KnockDownCarriage::Position(u64 clock) const
{
  const double elapsed = static_cast<double>(clock - std::min(clock, m_origin)) / CLOCK_HZ;
  double position;
  if (m_punch_speed > 0.0)
    position = m_position - std::min(m_punch_speed * elapsed, PUNCH_STROKE);
  else
    position = m_position + m_rate * elapsed;
  return std::clamp(position, 0.0, TRAVEL);
}

void KnockDownCarriage::SetMotor(double steps_per_second, u64 clock)
{
  m_position = Position(clock);
  m_origin = clock;
  m_rate = steps_per_second;
  m_punch_speed = 0.0;
}

bool KnockDownCarriage::IsReady(u64 clock) const
{
  return m_rate == 0.0 && m_punch_speed == 0.0 && Position(clock) >= TRAVEL - LIMIT_WIDTH;
}

void KnockDownCarriage::Punch(float strength, u64 clock)
{
  m_position = Position(clock);
  m_origin = clock;
  m_punch_speed = LIGHT_SPEED + static_cast<double>(std::max(strength, 0.0f)) * (HARD_SPEED - LIGHT_SPEED);
}

u8 KnockDownCarriage::Sensors(u64 clock) const
{
  const double x = Position(clock);
  u8 levels = 0x0F;
  for (const FlagSensor& sensor : FLAG_SENSORS)
  {
    if (x >= sensor.from && x <= sensor.to)
      levels &= static_cast<u8>(~(1u << sensor.bit));
  }
  return levels;
}

u8 KnockDownCarriage::Switches(u64 clock) const
{
  const double x = Position(clock);
  u8 levels = 0xFF;
  if (x >= TRAVEL - LIMIT_WIDTH)
    levels &= ~0x01;
  if (x <= LIMIT_WIDTH)
    levels &= ~0x02;
  return levels;
}

u64 KnockDownCarriage::NextChange(u64 clock) const
{
  const double elapsed = static_cast<double>(clock - std::min(clock, m_origin)) / CLOCK_HZ;
  double next = std::numeric_limits<double>::infinity();
  for (const FlagSensor& sensor : FLAG_SENSORS)
  {
    for (const double edge : {sensor.from, sensor.to})
    {
      // Time from m_origin at which the carriage reaches `edge`.
      double t;
      if (m_punch_speed > 0.0)
      {
        const double distance = m_position - edge;
        if (distance < 0.0 || distance > PUNCH_STROKE)
          continue;
        t = distance / m_punch_speed;
      }
      else
      {
        if (m_rate == 0.0)
          continue;
        t = (edge - m_position) / m_rate;
      }
      if (t > elapsed)
        next = std::min(next, t);
    }
  }
  if (!std::isfinite(next))
    return std::numeric_limits<u64>::max();
  return m_origin + static_cast<u64>(std::ceil(next * CLOCK_HZ)) + 1;
}

} // namespace NamcoSystem10
