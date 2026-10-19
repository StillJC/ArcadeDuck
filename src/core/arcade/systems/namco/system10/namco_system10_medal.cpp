// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-FileCopyrightText: windyfairy
// SPDX-License-Identifier: GPL-3.0-only
//
// Port wiring follows MAME's BSD-3-Clause namcos10.cpp (namcos10_mgexio).
// See THIRD-PARTY-LICENSES.txt.

#include "core/arcade/systems/namco/system10/namco_system10_medal.h"

#include "core/arcade/arcade_input.h"
#include "core/system.h"

namespace NamcoSystem10 {
namespace {
// A hopper's medal sensor changes state this often while its motor runs.
constexpr u64 HOPPER_PERIOD_MS = 100;

// A medal passes the two entry sensors (L1/L2, R1/R2) one after the other.
constexpr u64 MEDAL_SENSOR_MS = 25;

u64 MillisecondsToTicks(u64 ms)
{
  return static_cast<u64>(System::GetTicksPerSecond()) * ms / 1000;
}
} // namespace

void MedalCabinet::Reset()
{
  m_hoppers = {};
  m_medal_start = {};
  m_medal_pressed = {};
  m_divider = {};
  m_lockout = {};
  m_payout = false;
  m_win_lamp = false;
  m_leds = 0;
}

u64 MedalCabinet::Now()
{
  return System::GetGlobalTickCounter();
}

void MedalCabinet::SetHopperMotor(Hopper& hopper, bool on, u64 now)
{
  if (on && !hopper.power)
  {
    hopper.power = true;
    hopper.status = false;
    hopper.next_toggle = now + MillisecondsToTicks(HOPPER_PERIOD_MS);
  }
  else if (!on && hopper.power)
  {
    // A medal already under the sensor still leaves the hopper.
    hopper.power = false;
  }
}

void MedalCabinet::UpdateHoppers(u64 now)
{
  const u64 period = MillisecondsToTicks(HOPPER_PERIOD_MS);
  for (Hopper& hopper : m_hoppers)
  {
    while ((hopper.power || hopper.status) && now >= hopper.next_toggle)
    {
      hopper.status = !hopper.status;
      hopper.next_toggle += period;
    }
  }
}

void MedalCabinet::UpdateMedalEntry(u64 now)
{
  // Each press of a coin input inserts one medal on that side.
  for (u32 side = 0; side < 2; side++)
  {
    const bool pressed = ArcadeInput::IsDigitalPressed(side, "Coin");
    if (pressed && !m_medal_pressed[side] && m_medal_start[side] == 0)
      m_medal_start[side] = now + 1;
    m_medal_pressed[side] = pressed;
  }
}

u8 MedalCabinet::ReadPort(MGEXIOBoard::Port port)
{
  const u64 now = Now();
  switch (port)
  {
    case MGEXIOBoard::Port::P4:
      // Divider solenoid position sensors.
      return static_cast<u8>((m_divider[0] ? 0x01 : 0x00) | (m_divider[1] ? 0x02 : 0x00));

    case MGEXIOBoard::Port::P7:
    {
      // Check sensors 1-7 (active low) used by the I/O test menus.
      static constexpr std::array<const char*, 7> SENSORS = {"Button1", "Button2", "Button3", "Button4",
                                                            "Button5", "Button6", "Button7"};
      u8 value = 0xFF;
      for (u32 i = 0; i < SENSORS.size(); i++)
      {
        if (ArcadeInput::IsDigitalPressed(0, SENSORS[i]))
          value &= static_cast<u8>(~(1u << i));
      }
      return value;
    }

    case MGEXIOBoard::Port::PA:
    {
      UpdateMedalEntry(now);
      UpdateHoppers(now);

      // Bits 0-3: medal entry sensors L1, L2, R1, R2 (active low).
      u8 value = 0x0F;
      const u64 sensor = MillisecondsToTicks(MEDAL_SENSOR_MS);
      for (u32 side = 0; side < 2; side++)
      {
        if (m_medal_start[side] == 0)
          continue;

        const u64 elapsed = now - (m_medal_start[side] - 1);
        if (elapsed < sensor)
          value ^= static_cast<u8>(1u << (side * 2));
        else if (elapsed < sensor * 2)
          value ^= static_cast<u8>(1u << (side * 2 + 1));
        else
          m_medal_start[side] = 0;
      }

      // Bits 4-6: hopper medal sensors (left, right, centre). Bit 7: pusher
      // motor position, not modelled.
      value |= static_cast<u8>((m_hoppers[0].status ? 0x10 : 0) | (m_hoppers[1].status ? 0x20 : 0) |
                               (m_hoppers[2].status ? 0x40 : 0) | 0x80);
      return value;
    }

    default:
      return 0xFF;
  }
}

void MedalCabinet::WritePort(MGEXIOBoard::Port port, u8 value)
{
  const u64 now = Now();
  switch (port)
  {
    case MGEXIOBoard::Port::P4:
      m_win_lamp = (value & 0x40) != 0;
      break;

    case MGEXIOBoard::Port::PB:
      UpdateHoppers(now);
      SetHopperMotor(m_hoppers[0], (value & 0x01) != 0, now);
      SetHopperMotor(m_hoppers[1], (value & 0x02) != 0, now);
      SetHopperMotor(m_hoppers[2], (value & 0x10) != 0, now);
      m_lockout[0] = (value & 0x04) != 0;
      m_lockout[1] = (value & 0x08) != 0;
      m_payout = (value & 0x20) != 0;
      m_divider[0] = (value & 0x40) == 0;
      m_divider[1] = (value & 0x80) == 0;
      break;

    default:
      break;
  }
}

void MedalCabinet::WriteOutput(u32 index, u16 value)
{
  // Cabinet LEDs 1-7.
  if (index < 7)
  {
    const u8 bit = static_cast<u8>(1u << index);
    m_leds = (value != 0) ? static_cast<u8>(m_leds | bit) : static_cast<u8>(m_leds & ~bit);
  }
}

} // namespace NamcoSystem10
