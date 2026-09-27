// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/devices/jvs/namco_cyberlead_led.h"
#include "common/types.h"

#include <array>
#include <span>

namespace Arcade::JVS {

class NamcoCyberLead
{
public:
  enum class InputProfile : u8
  {
    Standard,
    AplaRail
  };

  struct PacketResult
  {
    std::array<u8, 256> response{};
    u32 response_length = 0;
    bool reset_requested = false;
    bool initialize_sense_after_response = false;
  };

  struct LEDTransportState
  {
    // JVS 0x71/0x01: eight bytes supplied by the game. The physical I/O PCB
    // augments these with cabinet status before forwarding 0x71/0x01 to the
    // separate Cyber Lead LED PCB.
    std::array<u8, 8> control_data{};

    // JVS 0x71/0x02: one-byte selector followed by the eight-byte game/profile
    // identifier (for Aplarail this is selector 01 + "AP_A    ").
    u8 profile_selector = 0;
    std::array<u8, 8> profile_id{};

    // JVS 0x71/0x05: firmware-defined 70-byte custom data transfer.
    std::array<u8, 70> custom_data{};

    // JVS 0x71/0x06: one-byte status/query argument.
    u8 status_argument = 0;

    u8 last_subcommand = 0;
    u64 generation = 0;
    bool active = false;
  };

  void Reset();
  void SetInputProfile(InputProfile profile) { m_input_profile = profile; }
  PacketResult ProcessPacket(std::span<const u8> packet);

  bool SetLEDFirmware(std::span<const u8> firmware);
  void ResetLED();
  void AdvanceLED(u32 source_clocks, u32 source_clock_hz);
  bool CopyLEDFrame(std::span<u8> pixels, std::array<u32, 4>* intensity_counts = nullptr,
                    u16* start_address = nullptr, u32* scroll_x = nullptr,
                    u32* scroll_y = nullptr, u64* generation = nullptr) const;

  const LEDTransportState& GetLEDTransportState() const { return m_led_transport; }
  const NamcoCyberLeadLEDLink& GetLEDLink() const { return m_led_link; }
  const NamcoCyberLeadLEDDevice& GetLEDDevice() const { return m_led_device; }

private:
  u32 ReadPlayerInputs(u32 player) const;
  void UpdateCoinCounters();
  void QueueLEDCommand(std::span<const u8> command);
  void QueueLEDControlCommand();

  InputProfile m_input_profile = InputProfile::Standard;
  u8 m_address = UINT8_C(0xFF);
  std::array<u16, 2> m_coin_counter{};
  std::array<bool, 2> m_coin_previous{};
  LEDTransportState m_led_transport{};
  NamcoCyberLeadLEDLink m_led_link{};
  NamcoCyberLeadLEDDevice m_led_device{};
  u32 m_led_link_diag_count = 0;
};

} // namespace Arcade::JVS