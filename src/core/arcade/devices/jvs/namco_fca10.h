// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "common/types.h"

#include <array>
#include <span>

class StateWrapper;

namespace Arcade::JVS {

// High-level model of the Namco FCA-1 (FCA10) JVS I/O board used by
// Quest for Fame. MAME identifies FCA10 as an ASCA-3-derived board; the
// undumped FCA10 MCU/PIC firmware is not required for the JVS protocol surface
// implemented here.
class NamcoFCA10
{
public:
  bool DoState(StateWrapper& sw);

  struct PacketResult
  {
    std::array<u8, 256> response{};
    u32 response_length = 0;
    bool reset_requested = false;
    bool initialize_sense_after_response = false;
  };

  void Reset();
  PacketResult ProcessPacket(std::span<const u8> packet);
  void SetMaintainedTestSwitch(bool maintained) { m_test_maintained = maintained; }

private:
  u32 ReadSwitchInputs() const;
  bool ReadTestSwitch();
  void UpdateCoinCounters();

  bool m_test_maintained = false;
  bool m_test_latched = false;
  bool m_test_previous = false;
  u8 m_address = UINT8_C(0xFF);
  std::array<u16, 2> m_coin_counter{};
  std::array<bool, 2> m_coin_previous{};
  u32 m_unsupported_command_logs = 0;
};

} // namespace Arcade::JVS
