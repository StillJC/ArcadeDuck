// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "common/types.h"

#include <array>
#include <span>

namespace Arcade::JVS {

class Node
{
public:
  struct PacketResult
  {
    std::array<u8, 256> response{};
    u32 response_length = 0;
  };

  virtual ~Node() = default;
  virtual void Reset();

  u8 GetAddress() const { return m_address; }
  bool IsAssigned() const;
  void AssignAddress(u8 address) { m_address = address; }

  virtual PacketResult ProcessAddressedPacket(std::span<const u8> packet) = 0;

private:
  u8 m_address = UINT8_C(0xFF);
};

class Chain
{
public:
  struct PacketResult
  {
    std::array<u8, 256> response{};
    u32 response_length = 0;
    bool reset_requested = false;
    bool initialize_sense_after_response = false;
  };

  void Clear();
  bool AddNode(Node& node);
  void Reset();
  PacketResult ProcessPacket(std::span<const u8> packet);

private:
  bool AllAssigned() const;
  Node* FindNode(u8 address) const;

  std::array<Node*, 8> m_nodes{};
  u32 m_node_count = 0;
};

} // namespace Arcade::JVS