// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/jvs/namco_cyberlead.h"

#include "core/arcade/arcade_input.h"
#include "common/log.h"
#include "core/arcade/devices/jvs/jvs_bus.h"

#include <algorithm>
#include <array>

Log_SetChannel(NamcoSystem12);

namespace Arcade::JVS {
namespace {

bool AppendResponseByte(std::array<u8, 256>& response, u32& length, u8 value)
{
  if (length >= response.size())
    return false;

  response[length++] = value;
  return true;
}

bool AppendResponseData(std::array<u8, 256>& response, u32& length, const char* data)
{
  while (*data)
  {
    if (!AppendResponseByte(response, length, static_cast<u8>(*data++)))
      return false;
  }

  return AppendResponseByte(response, length, UINT8_C(0x00));
}

} // namespace

void NamcoCyberLead::Reset()
{
  m_address = Bus::BroadcastAddress;
  m_coin_counter.fill(0);
  m_coin_previous.fill(false);
  m_led_transport = {};
  m_led_link.Reset();
  m_led_link_diag_count = 0;
}

bool NamcoCyberLead::SetLEDFirmware(std::span<const u8> firmware)
{
  return m_led_device.SetFirmware(firmware);
}

void NamcoCyberLead::ResetLED()
{
  m_led_device.Reset();
}

void NamcoCyberLead::AdvanceLED(u32 source_clocks, u32 source_clock_hz)
{
  m_led_device.Advance(source_clocks, source_clock_hz);
}

bool NamcoCyberLead::CopyLEDFrame(std::span<u8> pixels, std::array<u32, 4>* intensity_counts,
                                  u16* start_address, u32* scroll_x,
                                  u32* scroll_y, u64* generation) const
{
  const NamcoCyberLeadLEDDevice::FrameState& frame = m_led_device.GetFrameState();
  if (!frame.valid || pixels.size() < frame.pixels.size())
    return false;

  std::copy(frame.pixels.begin(), frame.pixels.end(), pixels.begin());

  if (intensity_counts)
    *intensity_counts = frame.intensity_counts;
  if (start_address)
    *start_address = frame.start_address;
  if (scroll_x)
    *scroll_x = frame.scroll_x;
  if (scroll_y)
    *scroll_y = frame.scroll_y;
  if (generation)
    *generation = frame.generation;

  return true;
}

u32 NamcoCyberLead::ReadPlayerInputs(u32 player) const
{
  if (m_input_profile == InputProfile::AplaRail)
  {
    if (player != 0)
      return 0;

    u32 value = 0;
    if (ArcadeInput::IsDigitalPressed(0, "Start"))
      value |= UINT32_C(0x00000001); // OK/Horn: JVS Button 2
    if (ArcadeInput::IsDigitalPressed(0, "Button1"))
      value |= UINT32_C(0x00000002); // Left: JVS Button 1
    if (ArcadeInput::IsOperatorPressed("Service"))
      value |= UINT32_C(0x00000040);
    if (ArcadeInput::IsDigitalPressed(0, "Button2"))
      value |= UINT32_C(0x00008000); // Right: JVS Button 3
    return value;
  }

  if (player >= 2)
    return 0;

  u32 value = 0;
  const auto set_if_pressed = [&value, player](u32 mask, const char* key) {
    if (ArcadeInput::IsDigitalPressed(player, key))
      value |= mask;
  };

  set_if_pressed(UINT32_C(0x00000080), "Start");
  if (player == 0 && ArcadeInput::IsOperatorPressed("Service"))
    value |= UINT32_C(0x00000040);
  set_if_pressed(UINT32_C(0x00000020), "Up");
  set_if_pressed(UINT32_C(0x00000010), "Down");
  set_if_pressed(UINT32_C(0x00000008), "Left");
  set_if_pressed(UINT32_C(0x00000004), "Right");
  set_if_pressed(UINT32_C(0x00000002), "Button1");
  set_if_pressed(UINT32_C(0x00000001), "Button2");
  set_if_pressed(UINT32_C(0x00008000), "Button3");
  set_if_pressed(UINT32_C(0x00004000), "Button4");
  set_if_pressed(UINT32_C(0x00002000), "Button5");
  set_if_pressed(UINT32_C(0x00001000), "Button6");
  return value;
}

void NamcoCyberLead::UpdateCoinCounters()
{
  for (u32 slot = 0; slot < m_coin_counter.size(); slot++)
  {
    const bool pressed = ArcadeInput::IsDigitalPressed(slot, "Coin");
    if (pressed && !m_coin_previous[slot])
      m_coin_counter[slot] = static_cast<u16>((m_coin_counter[slot] + 1) & 0x3FFF);
    m_coin_previous[slot] = pressed;
  }
}

void NamcoCyberLead::QueueLEDCommand(std::span<const u8> command)
{
  if (!m_led_link.QueueCommand(command))
  {
    WARNING_LOG("CyberLead LED link rejected command size={}", command.size());
    return;
  }

  const NamcoCyberLeadLEDLink::Frame& frame = m_led_link.GetLastFrame();
  if (!m_led_device.QueueWireBytes(std::span<const u8>(frame.wire.data(), frame.wire_size)))
  {
    WARNING_LOG("CyberLead LED C77 RX queue overflow frame={} wire_bytes={}",
                frame.generation, frame.wire_size);
  }

  if (m_led_link_diag_count < 32)
  {
    INFO_LOG(
      "CyberLead LED link TX #{} address={} command={:02X}/{:02X} command_bytes={} logical_bytes={} "
      "wire_bytes={} checksum={:02X}",
      frame.generation, m_led_link.GetAddress(), command[0],
      (command.size() > 1) ? command[1] : UINT8_C(0x00), command.size(),
      frame.logical_size, frame.wire_size, frame.checksum);
    m_led_link_diag_count++;
  }
}

void NamcoCyberLead::QueueLEDControlCommand()
{
  // CL1-I/OB Ver1.03 periodic LED-board 0x71/0x01 forwarding at 0x1494:
  //   system/test + DIP, P1 low/high, P2 low/high, then the eight game bytes.
  // C00F (the DIP/status byte) is 00 with the currently validated default
  // Cyber Lead configuration.
  std::array<u8, 15> command{};
  command[0] = UINT8_C(0x71);
  command[1] = UINT8_C(0x01);
  command[2] = ArcadeInput::IsOperatorPressed("Test") ? UINT8_C(0x80) : UINT8_C(0x00);

  const u32 p1 = ReadPlayerInputs(0);
  const u32 p2 = ReadPlayerInputs(1);
  command[3] = static_cast<u8>(p1);
  command[4] = static_cast<u8>(p1 >> 8);
  command[5] = static_cast<u8>(p2);
  command[6] = static_cast<u8>(p2 >> 8);
  std::copy(m_led_transport.control_data.begin(), m_led_transport.control_data.end(), command.begin() + 7);

  QueueLEDCommand(command);
}

NamcoCyberLead::PacketResult NamcoCyberLead::ProcessPacket(std::span<const u8> packet)
{
  PacketResult result;

  static bool s_diag_initialized = false;
  static bool s_diag_service_previous = false;
  static std::array<bool, 2> s_diag_coin_previous{false, false};

  const bool diag_service = ArcadeInput::IsOperatorPressed("Service");
  const std::array<bool, 2> diag_coin = {
    ArcadeInput::IsDigitalPressed(0, "Coin"),
    ArcadeInput::IsDigitalPressed(1, "Coin")
  };

  const bool diag_service_changed =
    !s_diag_initialized || diag_service != s_diag_service_previous;
  const bool diag_coin0_changed =
    !s_diag_initialized || diag_coin[0] != s_diag_coin_previous[0];
  const bool diag_coin1_changed =
    !s_diag_initialized || diag_coin[1] != s_diag_coin_previous[1];

  if (diag_service_changed || diag_coin0_changed || diag_coin1_changed)
  {
    WARNING_LOG("CyberLead DIAG host inputs: Service={} Coin1={} Coin2={}",
                diag_service, diag_coin[0], diag_coin[1]);
  }

  s_diag_initialized = true;
  s_diag_service_previous = diag_service;
  s_diag_coin_previous = diag_coin;

  if (packet.size() < 4)
    return result;

  const u8 destination = packet[0];
  const u32 packet_length = static_cast<u32>(packet.size());
  const u32 expected_length = static_cast<u32>(packet[1]) + 2;
  if (expected_length != packet_length ||
      (destination != Bus::BroadcastAddress && destination != m_address))
  {
    return result;
  }

  u8 checksum = 0;
  for (u32 i = 0; i + 1 < packet_length; i++)
    checksum = static_cast<u8>(checksum + packet[i]);
  if (checksum != packet[packet_length - 1])
    return result;

  std::array<u8, 256>& response = result.response;
  u32 response_length = 0;
  AppendResponseByte(response, response_length, UINT8_C(0x01)); // packet status: normal

  bool produce_response = false;
  bool set_initialized_after_response = false;
  u32 index = 2;
  const u32 commands_end = packet_length - 1;

  while (index < commands_end)
  {
    const u8 command = packet[index++];
    switch (command)
    {
      case UINT8_C(0xF0): // reset
        if (index >= commands_end || packet[index++] != UINT8_C(0xD9))
          return result;

        result.reset_requested = true;
        return result;

      case UINT8_C(0xF1): // set address
        if (index >= commands_end || m_address != Bus::BroadcastAddress)
          return result;

        m_address = packet[index++];
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        produce_response = true;
        set_initialized_after_response = true;
        break;

      case UINT8_C(0x10): // I/O identification
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        if (!AppendResponseData(response, response_length,
                                "namco ltd.;I/O CYBER LEAD;Ver1.03;JPN,LED-0100"))
        {
          return result;
        }
        produce_response = true;
        break;

      case UINT8_C(0x11): // command format revision
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        AppendResponseByte(response, response_length, UINT8_C(0x12));
        produce_response = true;
        break;

      case UINT8_C(0x12): // JVS revision
      case UINT8_C(0x13): // communications revision
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        AppendResponseByte(response, response_length, UINT8_C(0x10));
        produce_response = true;
        break;

      case UINT8_C(0x14): // feature list: 2 players/12 switches, 2 coin slots
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        AppendResponseByte(response, response_length, UINT8_C(0x02));
        AppendResponseByte(response, response_length, UINT8_C(0x0C));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        AppendResponseByte(response, response_length, UINT8_C(0x02));
        AppendResponseByte(response, response_length, UINT8_C(0x02));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        produce_response = true;
        break;

      case UINT8_C(0x15): // main-board identification
      {
        while (index < commands_end && packet[index] != UINT8_C(0x00))
          index++;

        if (index >= commands_end)
          return result;

        index++;
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        produce_response = true;
        break;
      }

      case UINT8_C(0x20): // switch inputs
      {
        if ((index + 2) > commands_end)
          return result;

        const u8 players = packet[index++];
        const u8 bytes = packet[index++];
        if (players > 2 || bytes > 2)
          return result;

        AppendResponseByte(response, response_length, UINT8_C(0x01));
        AppendResponseByte(response, response_length,
                           ArcadeInput::IsOperatorPressed("Test") ? UINT8_C(0x80) : UINT8_C(0x00));

        if (diag_service_changed)
        {
          WARNING_LOG("CyberLead DIAG 0x20: players={} bytes={} Test={} P1={:08X} P2={:08X}",
                      players, bytes, ArcadeInput::IsOperatorPressed("Test"),
                      ReadPlayerInputs(0), ReadPlayerInputs(1));
        }

        for (u32 player = 0; player < players; player++)
        {
          const u32 value = ReadPlayerInputs(player);
          for (u32 byte = 0; byte < bytes; byte++)
            AppendResponseByte(response, response_length, static_cast<u8>(value >> (byte * 8U)));
        }

        produce_response = true;
        break;
      }

      case UINT8_C(0x21): // coin inputs
      {
        if (index >= commands_end)
          return result;

        const u8 count = packet[index++];
        if (count > 2)
          return result;

        UpdateCoinCounters();

        if (diag_coin0_changed || diag_coin1_changed)
        {
          WARNING_LOG("CyberLead DIAG 0x21: count={} Coin1Raw={} Coin2Raw={} Counter1={} Counter2={}",
                      count, diag_coin[0], diag_coin[1],
                      m_coin_counter[0], m_coin_counter[1]);
        }

        AppendResponseByte(response, response_length, UINT8_C(0x01));
        for (u32 slot = 0; slot < count; slot++)
        {
          const u16 coin = m_coin_counter[slot];
          AppendResponseByte(response, response_length, static_cast<u8>((coin >> 8) & 0x3F));
          AppendResponseByte(response, response_length, static_cast<u8>(coin));
        }

        produce_response = true;
        break;
      }

      case UINT8_C(0x30): // subtract from coin counter
      case UINT8_C(0x31): // add to coin counter
      {
        if ((index + 3) > commands_end)
          return result;

        const u8 slot = packet[index++];
        const u16 amount = static_cast<u16>(
          (static_cast<u16>(packet[index]) << 8) |
          static_cast<u16>(packet[index + 1]));
        index += 2;

        WARNING_LOG("CyberLead DIAG 0x{:02X}: slot={} amount={} before1={} before2={}",
                    command, slot, amount, m_coin_counter[0], m_coin_counter[1]);

        AppendResponseByte(response, response_length, UINT8_C(0x01));

        if (slot < 1 || slot > m_coin_counter.size())
        {
          // JVS report: incorrect parameter.
          response[response_length - 1] = UINT8_C(0x02);
          produce_response = true;
          break;
        }

        u16& counter = m_coin_counter[slot - 1];
        if (command == UINT8_C(0x30))
        {
          counter = (counter < amount) ? UINT16_C(0) :
                    static_cast<u16>(counter - amount);
        }
        else
        {
          counter = static_cast<u16>(
            (static_cast<u32>(counter) + static_cast<u32>(amount)) &
            UINT32_C(0x3FFF));
        }

        produce_response = true;
        break;
      }

      case UINT8_C(0x70): // Namco manufacturer-specific
      {
        if (index >= commands_end)
          return result;

        const u8 subcommand = packet[index++];
        AppendResponseByte(response, response_length, UINT8_C(0x01)); // report: normal

        switch (subcommand)
        {
          case UINT8_C(0x01): // read 8 bytes of board memory
            for (u32 i = 0; i < 8; i++)
              AppendResponseByte(response, response_length, UINT8_C(0xFF));
            break;

          case UINT8_C(0x02): // program date
          {
            static constexpr std::array<u8, 8> PROGRAM_DATE = {
              UINT8_C(0x19), UINT8_C(0x98), UINT8_C(0x10), UINT8_C(0x26),
              UINT8_C(0x12), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00)
            };
            for (const u8 value : PROGRAM_DATE)
              AppendResponseByte(response, response_length, value);
            break;
          }

          case UINT8_C(0x03): // DIP-switch status
            // CL1-I/OB Ver1.03 returns the byte at C00F here. The firmware
            // initializes C00F to 00, and no runtime writer updates it.
            AppendResponseByte(response, response_length, UINT8_C(0x00));
            break;

          case UINT8_C(0x04): // Cyber Lead board-specific status query
            AppendResponseByte(response, response_length, UINT8_C(0xFF));
            AppendResponseByte(response, response_length, UINT8_C(0xFF));
            break;

          case UINT8_C(0x18): // ID check, followed by four request bytes
            if ((index + 4) > commands_end)
              return result;
            index += 4;
            AppendResponseByte(response, response_length, UINT8_C(0xFF));
            break;

          default:
            response[0] = UINT8_C(0x02);
            response_length = 1;
            index = commands_end;
            break;
        }

        produce_response = true;
        break;
      }

      case UINT8_C(0x71): // Namco Cyber Lead LED/control transport
      {
        if (index >= commands_end)
          return result;

        const u8 subcommand = packet[index++];

        // CL1-I/OB Ver1.03 does not treat 0x71 as "consume the rest of the
        // JVS packet". Each subcommand has a fixed payload length and the
        // outer JVS parser then continues with any following command.
        //
        // This matters for Aplarail, which sends:
        //   71 01 <8 bytes> 70 03
        // in one compound JVS request. The old high-level implementation
        // swallowed the trailing 70 03.
        switch (subcommand)
        {
          case UINT8_C(0x01): // game control data: 8 bytes
          {
            static constexpr u32 PAYLOAD_SIZE = 8;
            if ((index + PAYLOAD_SIZE) > commands_end)
              return result;

            std::copy_n(packet.begin() + index, PAYLOAD_SIZE,
                        m_led_transport.control_data.begin());
            index += PAYLOAD_SIZE;

            m_led_transport.last_subcommand = subcommand;
            m_led_transport.generation++;
            m_led_transport.active = true;
            QueueLEDControlCommand();

            AppendResponseByte(response, response_length, UINT8_C(0x01));
            produce_response = true;
            break;
          }

          case UINT8_C(0x02): // profile selector + 8-byte game/profile ID
          {
            static constexpr u32 PROFILE_SIZE = 8;
            if ((index + 1 + PROFILE_SIZE) > commands_end)
              return result;

            m_led_transport.profile_selector = packet[index++];
            std::copy_n(packet.begin() + index, PROFILE_SIZE,
                        m_led_transport.profile_id.begin());
            index += PROFILE_SIZE;

            m_led_transport.last_subcommand = subcommand;
            m_led_transport.generation++;
            m_led_transport.active = true;

            std::array<u8, 11> led_command{};
            led_command[0] = UINT8_C(0x71);
            led_command[1] = UINT8_C(0x02);
            led_command[2] = m_led_transport.profile_selector;
            std::copy(m_led_transport.profile_id.begin(), m_led_transport.profile_id.end(),
                      led_command.begin() + 3);
            QueueLEDCommand(led_command);

            AppendResponseByte(response, response_length, UINT8_C(0x01));
            produce_response = true;
            break;
          }

          case UINT8_C(0x04): // LED-board state/control
          {
            m_led_transport.last_subcommand = subcommand;
            m_led_transport.generation++;
            m_led_transport.active = true;

            static constexpr std::array<u8, 2> LED_COMMAND = {UINT8_C(0x71), UINT8_C(0x04)};
            QueueLEDCommand(LED_COMMAND);

            AppendResponseByte(response, response_length, UINT8_C(0x01));
            produce_response = true;
            break;
          }

          case UINT8_C(0x05): // 70-byte custom display-data block
          {
            static constexpr u32 PAYLOAD_SIZE = 70;
            if ((index + PAYLOAD_SIZE) > commands_end)
              return result;

            std::copy_n(packet.begin() + index, PAYLOAD_SIZE,
                        m_led_transport.custom_data.begin());
            index += PAYLOAD_SIZE;

            m_led_transport.last_subcommand = subcommand;
            m_led_transport.generation++;
            m_led_transport.active = true;

            std::array<u8, 72> led_command{};
            led_command[0] = UINT8_C(0x71);
            led_command[1] = UINT8_C(0x05);
            std::copy(m_led_transport.custom_data.begin(), m_led_transport.custom_data.end(),
                      led_command.begin() + 2);
            QueueLEDCommand(led_command);

            AppendResponseByte(response, response_length, UINT8_C(0x01));
            produce_response = true;
            break;
          }

          case UINT8_C(0x06): // one-byte status/query argument
          {
            if (index >= commands_end)
              return result;

            m_led_transport.status_argument = packet[index++];
            m_led_transport.last_subcommand = subcommand;
            m_led_transport.generation++;
            m_led_transport.active = true;

            const std::array<u8, 3> led_command = {
              UINT8_C(0x71), UINT8_C(0x06), m_led_transport.status_argument
            };
            QueueLEDCommand(led_command);

            AppendResponseByte(response, response_length, UINT8_C(0x01));
            produce_response = true;
            break;
          }

          case UINT8_C(0x07): // host-facing success/no-op in CL1-I/OB Ver1.03
            m_led_transport.last_subcommand = subcommand;
            m_led_transport.generation++;
            m_led_transport.active = true;
            AppendResponseByte(response, response_length, UINT8_C(0x01));
            produce_response = true;
            break;

          case UINT8_C(0x08): // LED-board mode/control
          {
            m_led_transport.last_subcommand = subcommand;
            m_led_transport.generation++;
            m_led_transport.active = true;

            static constexpr std::array<u8, 2> LED_COMMAND = {UINT8_C(0x71), UINT8_C(0x08)};
            QueueLEDCommand(LED_COMMAND);

            AppendResponseByte(response, response_length, UINT8_C(0x01));
            produce_response = true;
            break;
          }

          default:
            response[0] = UINT8_C(0x02);
            response_length = 1;
            produce_response = true;
            index = commands_end;
            break;
        }
        break;
      }

      default:
        response[0] = UINT8_C(0x02);
        response_length = 1;
        produce_response = true;
        index = commands_end;
        break;
    }
  }

  if (produce_response && response_length > 0)
  {
    result.response_length = response_length;
    result.initialize_sense_after_response = set_initialized_after_response;
  }

  return result;
}

} // namespace Arcade::JVS