// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-FileCopyrightText: windyfairy
// SPDX-License-Identifier: GPL-3.0-only
//
// Port wiring follows MAME's BSD-3-Clause namcos10.cpp (namcos10_mgexio).
// See THIRD-PARTY-LICENSES.txt.

#include "core/arcade/systems/namco/system10/namco_system10_medal.h"

#include "core/arcade/arcade_input.h"
#include "core/arcade/arcade_output.h"
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

// MAME's MGEXIO_OUTPUT0-15 numbering. Outputs 0, 1 and 4 are the hopper motors, which MAME
// drives as hopper devices rather than as outputs.
constexpr u32 OUTPUT_LOCKOUT_L = 2;
constexpr u32 OUTPUT_LOCKOUT_R = 3;
constexpr u32 OUTPUT_PAYOUT = 5;
constexpr u32 OUTPUT_DIVIDER_L = 6;
constexpr u32 OUTPUT_DIVIDER_R = 7;
constexpr u32 OUTPUT_WIN_LAMP = 8;

// Host output latch n drives these outputs (LED 2, 1, 4, 3, 6, 5, 7).
constexpr std::array<u32, 7> LATCH_OUTPUTS = {10, 9, 12, 11, 14, 13, 15};

void SetOutput(u32 index, bool value)
{
  static constexpr std::array<const char*, 16> NAMES = {
    "MGEXIO_OUTPUT0",  "MGEXIO_OUTPUT1",  "MGEXIO_OUTPUT2",  "MGEXIO_OUTPUT3",
    "MGEXIO_OUTPUT4",  "MGEXIO_OUTPUT5",  "MGEXIO_OUTPUT6",  "MGEXIO_OUTPUT7",
    "MGEXIO_OUTPUT8",  "MGEXIO_OUTPUT9",  "MGEXIO_OUTPUT10", "MGEXIO_OUTPUT11",
    "MGEXIO_OUTPUT12", "MGEXIO_OUTPUT13", "MGEXIO_OUTPUT14", "MGEXIO_OUTPUT15"};
  Arcade::Output::SetValue(NAMES[index], value ? 1 : 0, "Namco System 10 MGEXIO");
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

  for (const u32 index : {OUTPUT_LOCKOUT_L, OUTPUT_LOCKOUT_R, OUTPUT_PAYOUT, OUTPUT_DIVIDER_L, OUTPUT_DIVIDER_R,
                          OUTPUT_WIN_LAMP})
  {
    SetOutput(index, false);
  }
  for (const u32 index : LATCH_OUTPUTS)
    SetOutput(index, false);
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
      SetOutput(OUTPUT_WIN_LAMP, m_win_lamp);
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
      SetOutput(OUTPUT_LOCKOUT_L, m_lockout[0]);
      SetOutput(OUTPUT_LOCKOUT_R, m_lockout[1]);
      SetOutput(OUTPUT_PAYOUT, m_payout);
      SetOutput(OUTPUT_DIVIDER_L, m_divider[0]);
      SetOutput(OUTPUT_DIVIDER_R, m_divider[1]);
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
    SetOutput(LATCH_OUTPUTS[index], value != 0);
  }
}

} // namespace NamcoSystem10
