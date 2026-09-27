// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/devices/jvs/jvs_chain.h"

#include <array>

namespace Arcade::JVS {

class NamcoEMIO102 final : public Node
{
public:
  void Reset() override;
  Node::PacketResult ProcessAddressedPacket(std::span<const u8> packet) override;

  const std::array<u8, 3>& GetOutputs() const { return m_outputs; }

private:
  void UpdateCoinCounters();
  u32 ReadPlayerInputs() const;
  u16 ReadAnalog(u32 channel) const;

  std::array<u16, 2> m_coin_counter{};
  std::array<bool, 2> m_coin_previous{};
  std::array<u8, 3> m_outputs{};

  bool m_test_latched = false;
  bool m_test_previous = false;
};

} // namespace Arcade::JVS