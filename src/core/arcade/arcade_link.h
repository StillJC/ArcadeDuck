// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Arcade::Link {

// Message transport between linked cabinets. Each cabinet has an id from 1 to
// MAX_CABINETS. Messages are delivered reliably and in order per sender.
class Transport
{
public:
  static constexpr u8 MAX_CABINETS = 4;
  static constexpr size_t MAX_MESSAGE_SIZE = 16 * 1024;

  virtual ~Transport() = default;

  virtual u8 GetCabinetID() const = 0;

  // Returns a bit per connected cabinet, bit 0 for cabinet 1.
  virtual u32 GetConnectedMask() const = 0;

  virtual void Send(u8 cabinet, std::span<const u8> message) = 0;
  virtual bool Receive(u8* cabinet, std::vector<u8>* message) = 0;
};

// Creates the TCP transport configured by the [SystemLink] settings, or nullptr
// when linking is disabled. Cabinet 1 listens and relays for the others, which
// connect to it. Cabinets with a different session string are refused.
std::unique_ptr<Transport> CreateTransport(std::string_view session);

} // namespace Arcade::Link
