// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/jvs/namco_empri101.h"

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

} // namespace

void NamcoEMPri101::Reset()
{
  Node::Reset();
}

Node::PacketResult NamcoEMPri101::ProcessAddressedPacket(std::span<const u8> packet)
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
                        "namco ltd.;EM Pri1-01;Ver2.00;JPN&EXP,Techno-Drive PRN"))
          return result;
        produce_response = true;
        break;

      case UINT8_C(0x11):
        AppendByte(response, response_length, UINT8_C(0x01));
        AppendByte(response, response_length, UINT8_C(0x13));
        produce_response = true;
        break;

      case UINT8_C(0x12):
        AppendByte(response, response_length, UINT8_C(0x01));
        AppendByte(response, response_length, UINT8_C(0x30));
        produce_response = true;
        break;

      case UINT8_C(0x13):
        AppendByte(response, response_length, UINT8_C(0x01));
        AppendByte(response, response_length, UINT8_C(0x10));
        produce_response = true;
        break;

      case UINT8_C(0x14):
        AppendByte(response, response_length, UINT8_C(0x01));
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

      case UINT8_C(0x72):
      {
        if (index >= commands_end)
          return result;

        const u8 subcommand = packet[index++];
        switch (subcommand)
        {
          case UINT8_C(0x01):
          case UINT8_C(0x02):
          case UINT8_C(0x04):
            AppendByte(response, response_length, UINT8_C(0x01));
            produce_response = true;
            break;

          case UINT8_C(0x03):
            AppendByte(response, response_length, UINT8_C(0x01));
            AppendByte(response, response_length, UINT8_C(0x01));
            produce_response = true;
            break;

          case UINT8_C(0x05):
            if (index >= commands_end)
              return result;
            AppendByte(response, response_length,
                       packet[index++] == UINT8_C(0x01) ? UINT8_C(0x01) : UINT8_C(0x03));
            produce_response = true;
            break;

          case UINT8_C(0x07):
          {
            if (index >= commands_end)
              return result;

            const u8 count = packet[index++];
            if ((index + count) > commands_end)
              return result;

            index += count;
            AppendByte(response, response_length, UINT8_C(0x01));
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
    result.response_length = response_length;

  return result;
}

} // namespace Arcade::JVS