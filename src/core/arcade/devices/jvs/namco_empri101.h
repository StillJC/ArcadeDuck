// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/devices/jvs/jvs_chain.h"

#include <functional>
#include <utility>
#include <vector>

namespace Arcade::JVS {

class NamcoEMPri101 final : public Node
{
public:
  using JobCallback = std::function<void(std::span<const u8>)>;

  void Reset() override;
  Node::PacketResult ProcessAddressedPacket(std::span<const u8> packet) override;
  void SetJobCallback(JobCallback callback) { m_job_callback = std::move(callback); }

private:
  JobCallback m_job_callback;
  std::vector<u8> m_job_data;
};

} // namespace Arcade::JVS