// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/jvs/namco_emio102.h"

#include "core/arcade/arcade_input.h"

#include <algorithm>
#include <array>

namespace Arcade::JVS {
namespace {

bool AppendByte(std::array<u8, 256>& response, u32& length, u8 value)
{
  if (length >= response.size())
    return false;
  response[length++] = value;
  return true;
}

bool AppendData(std::array<u8, 256>& response, u32& length, const char* data)
{
  while (*data)
  {
    if (!AppendByte(response, length, static_cast<u8>(*data++)))
      return false;
  }
  return AppendByte(response, length, UINT8_C(0x00));
}

u16 ScaleAnalog(float value, u16 minimum, u16 maximum, bool reverse)
{
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  const float normalized = reverse ? (1.0f - clamped) : clamped;
  return static_cast<u16>(static_cast<float>(minimum) +
                          normalized * static_cast<float>(maximum - minimum) + 0.5f);
}

u16 ScalePedal(float value, u16 released, u16 pressed)
{
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  return static_cast<u16>(
    static_cast<float>(released) +
    clamped * static_cast<float>(static_cast<s32>(pressed) - static_cast<s32>(released)) +
    0.5f);
}

} // namespace

void NamcoEMIO102::Reset()
{
  Node::Reset();
  m_coin_counter.fill(0);
  m_coin_previous.fill(false);
  m_outputs.fill(0);
  m_test_latched = false;
  m_test_previous = false;
}

void NamcoEMIO102::UpdateCoinCounters()
{
  for (u32 slot = 0; slot < m_coin_counter.size(); slot++)
  {
    const bool pressed = ArcadeInput::IsDigitalPressed(slot, "Coin");
    if (pressed && !m_coin_previous[slot])
      m_coin_counter[slot] = static_cast<u16>((m_coin_counter[slot] + 1) & 0x3FFF);
    m_coin_previous[slot] = pressed;
  }
}

u32 NamcoEMIO102::ReadPlayerInputs() const
{
  u32 value = 0;
  if (ArcadeInput::IsOperatorPressed("Service"))
    value |= UINT32_C(0x00000040);
  if (ArcadeInput::IsDigitalPressed(0, "Select") ||
      ArcadeInput::IsDigitalPressed(0, "Button3"))
  {
    value |= UINT32_C(0x00002000);
  }
  return value;
}

u16 NamcoEMIO102::ReadAnalog(u32 channel) const
{
  switch (channel)
  {
    case 0:
      if (!ArcadeInput::HasAnalogControl(0, "Steering"))
        return UINT16_C(0x5E00);
      return ScaleAnalog(ArcadeInput::GetAnalogValue(0, "Steering"),
                         UINT16_C(0x1600), UINT16_C(0xA600), true);

    case 1:
      if (!ArcadeInput::HasAnalogControl(0, "Brake"))
        return UINT16_C(0x5E00);
      return ScalePedal(ArcadeInput::GetAnalogValue(0, "Brake"),
                        UINT16_C(0x5E00), UINT16_C(0x3E00));

    case 2:
      if (!ArcadeInput::HasAnalogControl(0, "Accelerator"))
        return UINT16_C(0x9A00);
      return ScalePedal(ArcadeInput::GetAnalogValue(0, "Accelerator"),
                        UINT16_C(0x9A00), UINT16_C(0x4680));

    default:
      return UINT16_C(0x8000);
  }
}

Node::PacketResult NamcoEMIO102::ProcessAddressedPacket(std::span<const u8> packet)
{
  Node::PacketResult result;
  if (packet.size() < 4)
    return result;

  auto& response = result.response;
  u32 response_length = 0;
  AppendByte(response, response_length, UINT8_C(0x01));

  bool produce_response = false;
  u32 index = 2;
  const u32 commands_end = static_cast<u32>(packet.size()) - 1;

  while (index < commands_end)
  {
    const u8 command = packet[index++];

    switch (command)
    {
      case UINT8_C(0x10):
        AppendByte(response, response_length, UINT8_C(0x01));
        if (!AppendData(response, response_length,
                        "namco ltd.;EM I/O1-02;Ver2.00;JPN&EXP,Techno-Drive I/O"))
          return result;
        produce_response = true;
        break;

      case UINT8_C(0x11):
        AppendByte(response, response_length, UINT8_C(0x01));
        AppendByte(response, response_length, UINT8_C(0x11));
        produce_response = true;
        break;

      case UINT8_C(0x12):
        AppendByte(response, response_length, UINT8_C(0x01));
        AppendByte(response, response_length, UINT8_C(0x20));
        produce_response = true;
        break;

      case UINT8_C(0x13):
        AppendByte(response, response_length, UINT8_C(0x01));
        AppendByte(response, response_length, UINT8_C(0x10));
        produce_response = true;
        break;

      case UINT8_C(0x14):
        AppendByte(response, response_length, UINT8_C(0x01));

        AppendByte(response, response_length, UINT8_C(0x01));
        AppendByte(response, response_length, UINT8_C(0x01));
        AppendByte(response, response_length, UINT8_C(0x13));
        AppendByte(response, response_length, UINT8_C(0x00));

        AppendByte(response, response_length, UINT8_C(0x02));
        AppendByte(response, response_length, UINT8_C(0x02));
        AppendByte(response, response_length, UINT8_C(0x00));
        AppendByte(response, response_length, UINT8_C(0x00));

        // 8 x 10-bit analog inputs.
        AppendByte(response, response_length, UINT8_C(0x03));
        AppendByte(response, response_length, UINT8_C(0x08));
        AppendByte(response, response_length, UINT8_C(0x0A));
        AppendByte(response, response_length, UINT8_C(0x00));

        // 24 general-purpose output bits.
        AppendByte(response, response_length, UINT8_C(0x12));
        AppendByte(response, response_length, UINT8_C(0x18));
        AppendByte(response, response_length, UINT8_C(0x00));
        AppendByte(response, response_length, UINT8_C(0x00));

        AppendByte(response, response_length, UINT8_C(0x00));
        produce_response = true;
        break;

      case UINT8_C(0x15):
        while (index < commands_end && packet[index] != UINT8_C(0x00))
          index++;
        if (index >= commands_end)
          return result;
        index++;
        AppendByte(response, response_length, UINT8_C(0x01));
        produce_response = true;
        break;

      case UINT8_C(0x20):
      {
        if ((index + 2) > commands_end)
          return result;

        const u8 players = packet[index++];
        const u8 bytes = packet[index++];
        if (players > 1 || bytes > 3)
        {
          AppendByte(response, response_length, UINT8_C(0x03));
          produce_response = true;
          break;
        }

        const bool test_pressed = ArcadeInput::IsOperatorPressed("Test");
        if (test_pressed && !m_test_previous)
          m_test_latched = !m_test_latched;
        m_test_previous = test_pressed;

        AppendByte(response, response_length, UINT8_C(0x01));
        AppendByte(response, response_length,
                   m_test_latched ? UINT8_C(0x80) : UINT8_C(0x00));

        const u32 value = ReadPlayerInputs();
        for (u32 player = 0; player < players; player++)
        {
          for (u32 byte = 0; byte < bytes; byte++)
            AppendByte(response, response_length, static_cast<u8>(value >> (byte * 8U)));
        }
        produce_response = true;
        break;
      }

      case UINT8_C(0x21):
      {
        if (index >= commands_end)
          return result;

        const u8 count = packet[index++];
        if (count > 2)
        {
          AppendByte(response, response_length, UINT8_C(0x03));
          produce_response = true;
          break;
        }

        UpdateCoinCounters();
        AppendByte(response, response_length, UINT8_C(0x01));
        for (u32 slot = 0; slot < count; slot++)
        {
          const u16 coin = m_coin_counter[slot];
          AppendByte(response, response_length, static_cast<u8>((coin >> 8) & 0x3F));
          AppendByte(response, response_length, static_cast<u8>(coin));
        }
        produce_response = true;
        break;
      }

      case UINT8_C(0x22):
      {
        if (index >= commands_end)
          return result;

        const u8 count = packet[index++];
        if (count > 8)
        {
          AppendByte(response, response_length, UINT8_C(0x03));
          produce_response = true;
          break;
        }

        AppendByte(response, response_length, UINT8_C(0x01));
        for (u32 channel = 0; channel < count; channel++)
        {
          const u16 value = ReadAnalog(channel);
          AppendByte(response, response_length, static_cast<u8>(value >> 8));
          AppendByte(response, response_length, static_cast<u8>(value));
        }
        produce_response = true;
        break;
      }

      case UINT8_C(0x30):
      {
        if ((index + 3) > commands_end)
          return result;

        const u8 slot = packet[index++];
        const u16 amount = static_cast<u16>(
          (static_cast<u16>(packet[index]) << 8) | static_cast<u16>(packet[index + 1]));
        index += 2;

        AppendByte(response, response_length, UINT8_C(0x01));
        if (slot >= 1 && slot <= m_coin_counter.size())
        {
          u16& counter = m_coin_counter[slot - 1];
          counter = (counter < amount) ? 0 : static_cast<u16>(counter - amount);
        }
        produce_response = true;
        break;
      }

      case UINT8_C(0x32):
      {
        if (index >= commands_end)
          return result;

        const u8 count = packet[index++];
        if ((index + count) > commands_end || count > m_outputs.size())
          return result;

        for (u32 i = 0; i < count; i++)
          m_outputs[i] = packet[index++];

        AppendByte(response, response_length, UINT8_C(0x01));
        produce_response = true;
        break;
      }

      case UINT8_C(0x70):
      {
        if (index >= commands_end)
          return result;

        // th1io-a.4f dispatches one-byte subcommands. "70 04 70 02"
        // is therefore two commands in one JVS packet.
        const u8 subcommand = packet[index++];
        AppendByte(response, response_length, UINT8_C(0x01));

        switch (subcommand)
        {
          case UINT8_C(0x00):
            break;

          case UINT8_C(0x01):
            for (u32 i = 0; i < 8; i++)
              AppendByte(response, response_length, UINT8_C(0xFF));
            break;

          case UINT8_C(0x02):
          {
            static constexpr std::array<u8, 8> PROGRAM_DATE = {
              UINT8_C(0x19), UINT8_C(0x98), UINT8_C(0x04), UINT8_C(0x09),
              UINT8_C(0x04), UINT8_C(0x15), UINT8_C(0x11), UINT8_C(0x14)
            };
            for (const u8 value : PROGRAM_DATE)
              AppendByte(response, response_length, value);
            break;
          }

          case UINT8_C(0x03):
            AppendByte(response, response_length, UINT8_C(0xFF));
            break;

          case UINT8_C(0x04):
            AppendByte(response, response_length, UINT8_C(0x00));
            AppendByte(response, response_length, UINT8_C(0x00));
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

      default:
        response[0] = UINT8_C(0x02);
        response_length = 1;
        produce_response = true;
        index = commands_end;
        break;
    }
  }

  if (produce_response && response_length > 0)
    result.response_length = response_length;

  return result;
}

} // namespace Arcade::JVS