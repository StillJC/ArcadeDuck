// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/jvs/namco_fca10.h"

#include "core/arcade/arcade_input.h"
#include "core/arcade/devices/jvs/jvs_bus.h"
#include "common/log.h"

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

bool AppendResponseString(std::array<u8, 256>& response, u32& length, const char* string)
{
  while (*string)
  {
    if (!AppendResponseByte(response, length, static_cast<u8>(*string++)))
      return false;
  }

  return AppendResponseByte(response, length, UINT8_C(0x00));
}

} // namespace

void NamcoFCA10::Reset()
{
  m_address = Bus::BroadcastAddress;
  m_coin_counter.fill(0);
  m_coin_previous.fill(false);
  m_unsupported_command_logs = 0;
}

u32 NamcoFCA10::ReadSwitchInputs() const
{
  // FCA10 is ASCA-3-derived. Quest for Fame uses the board as a single JVS
  // player with its cabinet controls distributed across the switch bank.
  // Keep the standard JVS low-byte layout used by the other Namco System 12
  // responders, then expose the additional QF action buttons in byte 2.
  u32 value = 0;
  const auto set_if_pressed = [&value](u32 mask, const char* key) {
    if (ArcadeInput::IsDigitalPressed(0, key))
      value |= mask;
  };

  set_if_pressed(UINT32_C(0x00000080), "Start");
  if (ArcadeInput::IsOperatorPressed("Service"))
    value |= UINT32_C(0x00000040);

  // Quest for Fame's own program confirms that Up/Down/Enter are the same
  // switch signals used for 1P Left/Right/Determine. The 2P controls occupy
  // the otherwise-unused low-byte switch positions.
  set_if_pressed(UINT32_C(0x00000020), "SelectUp");   // Up Select / 1P Left Choose
  set_if_pressed(UINT32_C(0x00000010), "SelectDown"); // Down Select / 1P Right Choose
  set_if_pressed(UINT32_C(0x00000002), "Enter");      // Enter / 1P Determine
  set_if_pressed(UINT32_C(0x00000004), "Button3");    // 2P Right Choose
  set_if_pressed(UINT32_C(0x00000008), "Button4");    // 2P Left Choose
  set_if_pressed(UINT32_C(0x00000001), "Button5");    // 2P Determine

  set_if_pressed(UINT32_C(0x00000100), "Button1"); // Drum
  set_if_pressed(UINT32_C(0x00001000), "Button2"); // Guitar

  return value;
}

bool NamcoFCA10::ReadTestSwitch()
{
  const bool pressed = ArcadeInput::IsOperatorPressed("Test");
  if (!m_test_maintained)
    return pressed;

  if (pressed && !m_test_previous)
    m_test_latched = !m_test_latched;
  m_test_previous = pressed;
  return m_test_latched;
}

void NamcoFCA10::UpdateCoinCounters()
{
  for (u32 slot = 0; slot < m_coin_counter.size(); slot++)
  {
    const bool pressed = ArcadeInput::IsDigitalPressed(slot, "Coin");
    if (pressed && !m_coin_previous[slot])
      m_coin_counter[slot] = static_cast<u16>((m_coin_counter[slot] + 1) & 0x3FFF);
    m_coin_previous[slot] = pressed;
  }
}

NamcoFCA10::PacketResult NamcoFCA10::ProcessPacket(std::span<const u8> packet)
{
  PacketResult result;

  if (packet.size() < 4)
    return result;

  const u8 destination = packet[0];
  const u32 packet_length = static_cast<u32>(packet.size());
  if ((static_cast<u32>(packet[1]) + 2U) != packet_length ||
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
  bool initialize_sense_after_response = false;
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
        initialize_sense_after_response = true;
        break;

      case UINT8_C(0x10): // I/O identification
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        if (!AppendResponseString(response, response_length,
                                  "namco ltd.;FCA-1;Ver1.00;JPN,Multipurpose + Rotary Encoder"))
        {
          return result;
        }
        produce_response = true;
        break;

      case UINT8_C(0x11): // command format revision 1.2
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        AppendResponseByte(response, response_length, UINT8_C(0x12));
        produce_response = true;
        break;

      case UINT8_C(0x12): // JVS revision 1.0
      case UINT8_C(0x13): // communications revision 1.0
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        AppendResponseByte(response, response_length, UINT8_C(0x10));
        produce_response = true;
        break;

      case UINT8_C(0x14): // FCA10/ASCA-3 capability list
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        // Switch input: one physical player, sixteen switch positions.
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        AppendResponseByte(response, response_length, UINT8_C(0x10));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        // Coin input: two slots.
        AppendResponseByte(response, response_length, UINT8_C(0x02));
        AppendResponseByte(response, response_length, UINT8_C(0x02));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        // Analog input: eight 10-bit channels on the ASCA-3 base hardware.
        AppendResponseByte(response, response_length, UINT8_C(0x03));
        AppendResponseByte(response, response_length, UINT8_C(0x08));
        AppendResponseByte(response, response_length, UINT8_C(0x0A));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        // Rotary input: two channels.
        AppendResponseByte(response, response_length, UINT8_C(0x04));
        AppendResponseByte(response, response_length, UINT8_C(0x02));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        // General-purpose output: sixteen bits.
        AppendResponseByte(response, response_length, UINT8_C(0x10));
        AppendResponseByte(response, response_length, UINT8_C(0x10));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        AppendResponseByte(response, response_length, UINT8_C(0x00));
        produce_response = true;
        break;

      case UINT8_C(0x15): // main-board identification
        while (index < commands_end && packet[index] != UINT8_C(0x00))
          index++;
        if (index >= commands_end)
          return result;
        index++;
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        produce_response = true;
        break;

      case UINT8_C(0x20): // switch inputs
      {
        if ((index + 2) > commands_end)
          return result;

        const u8 players = packet[index++];
        const u8 bytes = packet[index++];
        if (players > 1 || bytes > 3)
          return result;

        AppendResponseByte(response, response_length, UINT8_C(0x01));
        AppendResponseByte(response, response_length, ReadTestSwitch() ? UINT8_C(0x80) : UINT8_C(0x00));

        const u32 switches = ReadSwitchInputs();
        for (u32 player = 0; player < players; player++)
        {
          // ReadSwitchInputs() stores JVS byte 0 in bits 7:0, byte 1 in
          // bits 15:8, etc. Emit those bytes in that same wire order.
          for (u32 byte = 0; byte < bytes; byte++)
            AppendResponseByte(response, response_length,
                               static_cast<u8>(switches >> (byte * 8U)));
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

      case UINT8_C(0x22): // analog inputs
      {
        if (index >= commands_end)
          return result;
        const u8 count = packet[index++];
        if (count > 8)
          return result;

        AppendResponseByte(response, response_length, UINT8_C(0x01));
        for (u32 channel = 0; channel < count; channel++)
        {
          // QF does not use the ASCA-3 analog channels. Return their neutral
          // mid-scale state while preserving the board's 10-bit resolution.
          AppendResponseByte(response, response_length, UINT8_C(0x80));
          AppendResponseByte(response, response_length, UINT8_C(0x00));
        }
        produce_response = true;
        break;
      }

      case UINT8_C(0x23): // rotary inputs
      {
        if (index >= commands_end)
          return result;
        const u8 count = packet[index++];
        if (count > 2)
          return result;

        AppendResponseByte(response, response_length, UINT8_C(0x01));
        for (u32 channel = 0; channel < count; channel++)
        {
          AppendResponseByte(response, response_length, UINT8_C(0x00));
          AppendResponseByte(response, response_length, UINT8_C(0x00));
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
        const u16 amount = static_cast<u16>((static_cast<u16>(packet[index]) << 8) |
                                            static_cast<u16>(packet[index + 1]));
        index += 2;
        AppendResponseByte(response, response_length, UINT8_C(0x01));

        if (slot < 1 || slot > m_coin_counter.size())
        {
          response[response_length - 1] = UINT8_C(0x02);
          produce_response = true;
          break;
        }

        u16& counter = m_coin_counter[slot - 1];
        if (command == UINT8_C(0x30))
          counter = (counter < amount) ? UINT16_C(0) : static_cast<u16>(counter - amount);
        else
          counter = static_cast<u16>((static_cast<u32>(counter) + amount) & UINT32_C(0x3FFF));

        produce_response = true;
        break;
      }

      case UINT8_C(0x32): // general-purpose outputs
      {
        if (index >= commands_end)
          return result;
        const u8 bytes = packet[index++];
        if ((index + bytes) > commands_end)
          return result;
        index += bytes;
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        produce_response = true;
        break;
      }


      case UINT8_C(0x70): // Namco FCA vendor-specific command
      {
        // FCA vendor commands are subcommand-based and may be concatenated
        // inside one JVS packet. QF sends:
        //   70 04 70 02
        // as two commands, not one command with four parameters.
        if (index >= commands_end)
          return result;

        const u8 subcommand = packet[index++];
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        switch (subcommand)
        {
          case UINT8_C(0x02): // FCA program/date information
          {
            static constexpr std::array<u8, 8> program_date = {
              UINT8_C(0x19), UINT8_C(0x98), UINT8_C(0x10), UINT8_C(0x26),
              UINT8_C(0x12), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00)};
            for (const u8 value : program_date)
              AppendResponseByte(response, response_length, value);
            break;
          }

          case UINT8_C(0x04): // FCA board self-test/status result
            AppendResponseByte(response, response_length, UINT8_C(0xFF));
            AppendResponseByte(response, response_length, UINT8_C(0xFF));
            break;

          case UINT8_C(0x18): // FCA unique-PL transaction
          {
            // This subcommand carries four payload bytes after 0x18.
            // QF currently sends a six-byte vendor command beginning
            // 70 18 50 4C ... . Consume the entire transaction so the
            // payload is not misparsed as additional JVS commands.
            if ((index + 4) > commands_end)
              return result;

            const u8 parameter0 = packet[index++];
            const u8 parameter1 = packet[index++];
            const u8 parameter2 = packet[index++];
            const u8 parameter3 = packet[index++];
            AppendResponseByte(response, response_length, UINT8_C(0x01));
            break;
          }

          default:
            response[response_length - 1] = UINT8_C(0x02);
            break;
        }

        produce_response = true;
        break;
      }

      default:
        if (m_unsupported_command_logs < 32)
        {
          WARNING_LOG("System12 FCA10 unsupported JVS command 0x{:02X}", command);
          m_unsupported_command_logs++;
        }
        response[0] = UINT8_C(0x02); // packet status: unknown command
        response_length = 1;
        produce_response = true;
        index = commands_end;
        break;
    }
  }

  if (produce_response && response_length > 0)
  {
    result.response_length = response_length;
    result.initialize_sense_after_response = initialize_sense_after_response;
  }

  return result;
}

} // namespace Arcade::JVS
