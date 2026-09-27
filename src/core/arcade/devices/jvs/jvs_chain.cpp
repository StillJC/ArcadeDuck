// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/jvs/jvs_chain.h"

#include "core/arcade/devices/jvs/jvs_bus.h"

namespace Arcade::JVS {

void Node::Reset()
{
  m_address = Bus::BroadcastAddress;
}

bool Node::IsAssigned() const
{
  return m_address != Bus::BroadcastAddress;
}

void Chain::Clear()
{
  m_nodes.fill(nullptr);
  m_node_count = 0;
}

bool Chain::AddNode(Node& node)
{
  if (m_node_count >= m_nodes.size())
    return false;

  m_nodes[m_node_count++] = &node;
  return true;
}

void Chain::Reset()
{
  for (u32 i = 0; i < m_node_count; i++)
    m_nodes[i]->Reset();
}

bool Chain::AllAssigned() const
{
  if (m_node_count == 0)
    return false;

  for (u32 i = 0; i < m_node_count; i++)
  {
    if (!m_nodes[i]->IsAssigned())
      return false;
  }
  return true;
}

Node* Chain::FindNode(u8 address) const
{
  for (u32 i = 0; i < m_node_count; i++)
  {
    if (m_nodes[i]->GetAddress() == address)
      return m_nodes[i];
  }
  return nullptr;
}

Chain::PacketResult Chain::ProcessPacket(std::span<const u8> packet)
{
  PacketResult result;

  if (packet.size() < 4)
    return result;

  const u32 packet_length = static_cast<u32>(packet.size());
  if ((static_cast<u32>(packet[1]) + 2) != packet_length)
    return result;

  u8 checksum = 0;
  for (u32 i = 0; i + 1 < packet_length; i++)
    checksum = static_cast<u8>(checksum + packet[i]);
  if (checksum != packet[packet_length - 1])
    return result;

  const u8 destination = packet[0];
  if (destination != Bus::BroadcastAddress)
  {
    Node* const node = FindNode(destination);
    if (!node)
      return result;

    const Node::PacketResult node_result = node->ProcessAddressedPacket(packet);
    result.response = node_result.response;
    result.response_length = node_result.response_length;
    return result;
  }

  const u32 commands_end = packet_length - 1;
  if (commands_end <= 2)
    return result;

  u32 index = 2;
  const u8 command = packet[index++];

  if (command == UINT8_C(0xF0))
  {
    if (index >= commands_end || packet[index] != UINT8_C(0xD9))
      return result;

    Reset();
    result.reset_requested = true;
    return result;
  }

  if (command == UINT8_C(0xF1))
  {
    if (index >= commands_end)
      return result;

    Node* claimant = nullptr;

    // Nodes are registered host -> downstream. JVS SET_ADDRESS is claimed
    // from the far end of the daisy chain toward the host.
    for (u32 i = m_node_count; i > 0; i--)
    {
      Node* const candidate = m_nodes[i - 1];
      if (!candidate->IsAssigned())
      {
        claimant = candidate;
        break;
      }
    }

    if (!claimant)
      return result;

    claimant->AssignAddress(packet[index]);

    result.response[0] = UINT8_C(0x01); // packet status: normal
    result.response[1] = UINT8_C(0x01); // report: normal
    result.response_length = 2;
    result.initialize_sense_after_response = AllAssigned();
    return result;
  }

  result.response[0] = UINT8_C(0x02);
  result.response_length = 1;
  return result;
}

} // namespace Arcade::JVS