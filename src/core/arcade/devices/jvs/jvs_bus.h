// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "common/types.h"

#include <array>
#include <span>

namespace Arcade::JVS {

class Bus
{
public:
  static constexpr u8 Sync = UINT8_C(0xE0);
  static constexpr u8 Escape = UINT8_C(0xD0);
  static constexpr u8 BroadcastAddress = UINT8_C(0xFF);
  static constexpr u8 MasterAddress = UINT8_C(0x00);

  void Reset();

  // Feed one raw byte from the JVS master. Returns true when one complete,
  // de-escaped packet is ready for the attached device layer to process.
  bool FeedByte(u8 value);
  std::span<const u8> GetPacket() const;
  void ClearPacket();

  // Queue a device response payload. Framing, escaping, master destination
  // and checksum generation are handled by the bus.
  void QueueResponse(std::span<const u8> payload);
  bool HasResponseByte() const;
  u8 ReadResponseByte();
  bool ResponseComplete() const;

private:
  bool m_in_frame = false;
  bool m_escape = false;
  std::array<u8, 256> m_packet{};
  u32 m_packet_length = 0;

  std::array<u8, 512> m_response{};
  u32 m_response_read = 0;
  u32 m_response_length = 0;
};

} // namespace Arcade::JVS