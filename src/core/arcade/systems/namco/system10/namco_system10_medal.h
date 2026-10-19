// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/systems/namco/system10/namco_system10_mgexio.h"
#include "core/types.h"

#include <array>

namespace NamcoSystem10 {

// Medal pusher cabinet mechanics wired to the MGEXIO ports: medal entry
// sensors, three hoppers, lockout/divider/payout solenoids, lamps and the
// check sensors used by the test menus.
class MedalCabinet
{
public:
  void Reset();

  u8 ReadPort(MGEXIOBoard::Port port);
  void WritePort(MGEXIOBoard::Port port, u8 value);
  void WriteOutput(u32 index, u16 value);

private:
  struct Hopper
  {
    bool power = false;
    bool status = false;
    u64 next_toggle = 0;
  };

  static u64 Now();
  void UpdateHoppers(u64 now);
  void SetHopperMotor(Hopper& hopper, bool on, u64 now);
  void UpdateMedalEntry(u64 now);

  std::array<Hopper, 3> m_hoppers{};
  std::array<u64, 2> m_medal_start{};
  std::array<bool, 2> m_medal_pressed{};
  std::array<bool, 2> m_divider{};
  std::array<bool, 2> m_lockout{};
  bool m_payout = false;
  bool m_win_lamp = false;
  u8 m_leds = 0;
};

} // namespace NamcoSystem10
