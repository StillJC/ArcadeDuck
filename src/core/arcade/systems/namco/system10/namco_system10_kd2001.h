// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

class StateWrapper;

namespace NamcoSystem10 {

// Knock Down 2001 punch-pad carriage on the P-DRIVE PCB. A stepper slides the pad between a back
// and a front limit switch; the game waits with the pad at the front limit, and a punch drives the
// carriage back, sweeping a flag through two timed sensors. The P-DRIVE times each blocked pulse and
// the game scores the punch from the shorter one. Positions are motor steps from the back limit;
// time is in P-DRIVE board clocks.
//
// Test menu names: SENSOR1 = front limit (P7.0), SENSOR4 = back limit (P7.1), SENSOR2 = PA6 and
// SENSOR3 = PA7. PA4/PA5 are timed by the firmware but unused by the game.
class KnockDownCarriage
{
public:
  bool DoState(StateWrapper& sw);

  void Reset();

  // Step rate from `clock` (positive = towards the front). The motor takes over from a punch.
  void SetMotor(double steps_per_second, u64 clock);

  // True while the pad rests at the front limit with the motor stopped.
  bool IsReady(u64 clock) const;

  // Drives the resting pad back. Strength 0 is the lightest punch and 1 the hardest the game scores
  // as valid; higher strengths sweep the sensors faster than a valid reading.
  void Punch(float strength, u64 clock);

  // PA4-PA7 sensor levels in bits 0-3, 1 = clear.
  u8 Sensors(u64 clock) const;
  // First clock after `clock` at which a sensor level may change, or UINT64_MAX.
  u64 NextChange(u64 clock) const;
  // Front (bit 0) and back (bit 1) limit switch levels, active low.
  u8 Switches(u64 clock) const;

private:
  double Position(u64 clock) const;

  // Motor motion from m_origin.
  double m_position = 0.0;
  double m_rate = 0.0;
  u64 m_origin = 0;

  // Punch stroke speed from m_position at m_origin; zero when the carriage is not being punched.
  double m_punch_speed = 0.0;
};

} // namespace NamcoSystem10
