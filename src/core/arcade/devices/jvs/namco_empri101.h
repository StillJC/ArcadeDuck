// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/devices/jvs/jvs_chain.h"

namespace Arcade::JVS {

class NamcoEMPri101 final : public Node
{
public:
  void Reset() override;
  Node::PacketResult ProcessAddressedPacket(std::span<const u8> packet) override;
};

} // namespace Arcade::JVS