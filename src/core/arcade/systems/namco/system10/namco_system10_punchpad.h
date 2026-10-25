// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

namespace NamcoSystem10 {

// Hajime no Ippo punch pad. The pad rests on the DOWN speed sensor; the solenoid stands it up
// past MID to UP, where it stays latched until a punch knocks it back down. The game times the
// fall between the speed sensors to measure the punch, so the sensors follow the pad position
// at the moment IN1 is read.
class PunchPad
{
public:
  // IN1 bits.
  static constexpr u32 SENSOR_DOWN = 0x0002;
  static constexpr u32 SENSOR_MID = 0x0004;
  static constexpr u32 SENSOR_UP = 0x0008;
  static constexpr u32 SENSOR_SAFETY = 0x0010;
  static constexpr u32 SENSOR_HUMAN = 0x7000;
  static constexpr u32 SENSOR_MASK = SENSOR_DOWN | SENSOR_MID | SENSOR_UP | SENSOR_SAFETY | SENSOR_HUMAN;

  void Reset();

  void SetSolenoid(bool on);

  // Knocks the raised pad down. Strength runs from 0 (lightest) to 1 (hardest).
  void Punch(float strength);

  // Speed, safety and human sensor bits of IN1. The player is always in front of the cabinet.
  u32 ReadSensors();

private:
  enum class Motion : u8
  {
    Down,
    Rising,
    Up,
    Falling,
  };

  static u64 Now();
  void Update(u64 now);
  float Position(u64 now) const;
  void StartMotion(Motion motion, float from, u64 duration, u64 now);

  Motion m_motion = Motion::Down;
  // 0 = upright on UP, 1 = down on DOWN.
  float m_from = 1.0f;
  u64 m_start = 0;
  u64 m_duration = 1;
  bool m_solenoid = false;
};

} // namespace NamcoSystem10
