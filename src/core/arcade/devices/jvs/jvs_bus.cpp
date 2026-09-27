// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/jvs/jvs_bus.h"

#include <algorithm>

namespace Arcade::JVS {

void Bus::Reset()
{
  m_in_frame = false;
  m_escape = false;
  m_packet.fill(0);
  m_packet_length = 0;

  m_response.fill(0);
  m_response_read = 0;
  m_response_length = 0;
}

bool Bus::FeedByte(u8 value)
{
  if (value == Sync)
  {
    m_in_frame = true;
    m_escape = false;
    m_packet_length = 0;
    return false;
  }

  if (!m_in_frame)
    return false;

  if (m_escape)
  {
    value = static_cast<u8>(value + 1);
    m_escape = false;
  }
  else if (value == Escape)
  {
    m_escape = true;
    return false;
  }

  if (m_packet_length >= m_packet.size())
  {
    ClearPacket();
    return false;
  }

  m_packet[m_packet_length++] = value;
  if (m_packet_length < 2)
    return false;

  const u32 expected_length = static_cast<u32>(m_packet[1]) + 2;
  if (expected_length > m_packet.size())
  {
    ClearPacket();
    return false;
  }

  return m_packet_length == expected_length;
}

std::span<const u8> Bus::GetPacket() const
{
  return std::span<const u8>(m_packet.data(), m_packet_length);
}

void Bus::ClearPacket()
{
  m_in_frame = false;
  m_escape = false;
  m_packet_length = 0;
}

void Bus::QueueResponse(std::span<const u8> payload)
{
  m_response.fill(0);
  m_response_read = 0;
  m_response_length = 0;

  std::array<u8, 260> packet{};
  u32 packet_length = 0;
  packet[packet_length++] = MasterAddress;
  packet[packet_length++] = static_cast<u8>(payload.size() + 1);

  for (const u8 value : payload)
    packet[packet_length++] = value;

  u8 checksum = 0;
  for (u32 i = 0; i < packet_length; i++)
    checksum = static_cast<u8>(checksum + packet[i]);
  packet[packet_length++] = checksum;

  m_response[m_response_length++] = Sync;
  for (u32 i = 0; i < packet_length; i++)
  {
    const u8 value = packet[i];
    if (value == Sync || value == Escape)
    {
      if ((m_response_length + 2) > m_response.size())
        break;

      m_response[m_response_length++] = Escape;
      m_response[m_response_length++] = static_cast<u8>(value - 1);
    }
    else
    {
      if (m_response_length >= m_response.size())
        break;

      m_response[m_response_length++] = value;
    }
  }
}

bool Bus::HasResponseByte() const
{
  return m_response_read < m_response_length;
}

u8 Bus::ReadResponseByte()
{
  if (!HasResponseByte())
    return 0;

  return m_response[m_response_read++];
}

bool Bus::ResponseComplete() const
{
  return m_response_read >= m_response_length;
}

} // namespace Arcade::JVS