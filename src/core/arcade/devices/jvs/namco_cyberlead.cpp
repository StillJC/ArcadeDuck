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

// The I/O C77 runs from the same 14.7456 MHz clock as the LED C77. Its main
// loop waits for every second 8-bit timer compare (TCORA 0x78, phi/1024).
static constexpr u64 LINK_LOOP_CLOCKS = 2 * 121 * 1024;

// SCI1 to the LED PCB: 9600 bps 8N1.
static constexpr u64 LINK_BYTE_CLOCKS = 15360;

// C39C reload values: after 0xF1 and after a 71/02 or 71/06 transaction.
static constexpr u16 LINK_ADDRESS_COUNTDOWN = UINT16_C(0x0708);
static constexpr u16 LINK_TRANSACTION_COUNTDOWN = UINT16_C(0xFFFF);

// JVS 70/02 program date, from ROM offset 186A.
static constexpr std::array<u8, 8> PROGRAM_DATE = {
  UINT8_C(0x19), UINT8_C(0x97), UINT8_C(0x03), UINT8_C(0x05),
  UINT8_C(0x03), UINT8_C(0x19), UINT8_C(0x35), UINT8_C(0x29)};

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
  // JVS reset (F0 D9): the firmware clears its coin state and returns the LED
  // link to the idle state with no node address. Transaction results are kept.
  m_address = Bus::BroadcastAddress;
  m_coin_counter.fill(0);
  m_coin_used.fill(0);
  m_coin_previous.fill(false);
  m_led_link.SetAddress(m_address);
  SetLinkState(LinkState::Idle);
}

bool NamcoCyberLead::SetLEDFirmware(std::span<const u8> firmware)
{
  return m_led_device.SetFirmware(firmware);
}

void NamcoCyberLead::ResetLED()
{
  m_led_device.Reset();
  m_led_link.Reset();
  m_led_link.SetAddress(m_address);
  m_link = {};
}

void NamcoCyberLead::AdvanceLED(u32 source_clocks, u32 source_clock_hz)
{
  if (source_clocks == 0 || source_clock_hz == 0)
    return;

  const u64 scaled = m_link.clock_fraction +
                     static_cast<u64>(source_clocks) * NamcoCyberLeadLEDDevice::CLOCK_HZ;
  u64 remaining = scaled / source_clock_hz;
  m_link.clock_fraction = scaled % source_clock_hz;

  while (remaining != 0)
  {
    const u64 step = std::min<u64>({remaining, m_link.wake_clock - m_link.clock, UINT32_MAX});
    if (step != 0)
    {
      m_led_device.Advance(static_cast<u32>(step), NamcoCyberLeadLEDDevice::CLOCK_HZ);
      m_link.clock += step;
      remaining -= step;
    }

    u8 value;
    while (m_led_device.PopTransmittedByte(&value))
      ReceiveLEDByte(value);

    if (m_link.clock == m_link.wake_clock)
      RunLinkStep();
  }
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

u8 NamcoCyberLead::ReadBoardMemory(u16 address) const
{
  // JVS 70/01 reads eight bytes of I/O C77 memory. Games read C0D0-C0D7, which
  // the main loop refreshes with its pass counter and the port 4-7 pins.
  switch (address)
  {
    case UINT16_C(0xC0D0):
      return m_link.loop_counter;
    case UINT16_C(0xC0D4):
      // P4: bit 2 is the JVS sense output, driven once an address is assigned.
      return (m_address != Bus::BroadcastAddress) ? UINT8_C(0x8C) : UINT8_C(0x88);
    case UINT16_C(0xC0D5):
      return UINT8_C(0xFA);
    case UINT16_C(0xC0D6):
      return UINT8_C(0x1F);
    case UINT16_C(0xC0D7):
    {
      // SW1 (Mode A) and SW2 (Mode B) are active-low bits 1 and 0 of port 7.
      u8 value = UINT8_C(0x8F);
      if (ArcadeInput::IsOperatorPressed("CyberLeadModeA"))
        value &= static_cast<u8>(~UINT8_C(0x02));
      if (ArcadeInput::IsOperatorPressed("CyberLeadModeB"))
        value &= static_cast<u8>(~UINT8_C(0x01));
      return value;
    }
    case UINT16_C(0xC0D1):
    case UINT16_C(0xC0D2):
    case UINT16_C(0xC0D3):
      return UINT8_C(0x00);
    default:
      return UINT8_C(0xFF);
  }
}

u64 NamcoCyberLead::SendLEDCommand(std::span<const u8> command)
{
  if (!m_led_link.QueueCommand(command))
  {
    WARNING_LOG("CyberLead LED link rejected command size={}", command.size());
    return m_link.clock;
  }

  const NamcoCyberLeadLEDLink::Frame& frame = m_led_link.GetLastFrame();
  if (!m_led_device.QueueWireBytes(std::span<const u8>(frame.wire.data(), frame.wire_size)))
  {
    WARNING_LOG("CyberLead LED C77 RX queue overflow frame={} wire_bytes={}",
                frame.generation, frame.wire_size);
  }

  // The firmware polls TDRE for each byte, so it returns once the last byte
  // enters the transmit data register while the one before it is shifting.
  return m_link.clock + LINK_BYTE_CLOCKS * (std::max<u32>(frame.wire_size, 2) - 2);
}

void NamcoCyberLead::SetLinkState(LinkState state)
{
  // A JVS command that changes the state runs after any transaction in
  // progress has been evaluated, so that evaluation must not overwrite it.
  m_link.state = state;
  m_link.generation++;
}

void NamcoCyberLead::RunLinkStep()
{
  switch (m_link.step)
  {
    case LinkStep::Run:
      m_link.loop_counter++;
      StartLinkState();
      break;

    case LinkStep::SecondWait:
      m_link.wake_clock = m_link.clock + LINK_LOOP_CLOCKS;
      m_link.step = LinkStep::Evaluate;
      break;

    case LinkStep::Evaluate:
      if (m_link.started_generation == m_link.generation)
        EvaluateLinkState();
      m_link.wake_clock = m_link.clock + LINK_LOOP_CLOCKS;
      m_link.step = LinkStep::Run;
      break;
  }
}

void NamcoCyberLead::StartLinkState()
{
  const auto next_loop = [](u64 clock) { return ((clock / LINK_LOOP_CLOCKS) + 1) * LINK_LOOP_CLOCKS; };

  u64 sent = m_link.clock;
  switch (m_link.state)
  {
    case LinkState::Idle:
    {
      // 1494: Test is bit 7. CL1-I/OB packs port 7 SW1/SW2 into C00F as
      // active-high Mode A bit 3 and Mode B bit 2.
      std::array<u8, 15> command{};
      command[0] = UINT8_C(0x71);
      command[1] = UINT8_C(0x01);
      command[2] = ArcadeInput::IsOperatorPressed("Test") ? UINT8_C(0x80) : UINT8_C(0x00);
      if (ArcadeInput::IsOperatorPressed("CyberLeadModeA"))
        command[2] |= UINT8_C(0x08);
      if (ArcadeInput::IsOperatorPressed("CyberLeadModeB"))
        command[2] |= UINT8_C(0x04);
      const u32 p1 = ReadPlayerInputs(0);
      const u32 p2 = ReadPlayerInputs(1);
      command[3] = static_cast<u8>(p1);
      command[4] = static_cast<u8>(p1 >> 8);
      command[5] = static_cast<u8>(p2);
      command[6] = static_cast<u8>(p2 >> 8);
      std::copy(m_link.control_data.begin(), m_link.control_data.end(), command.begin() + 7);
      sent = SendLEDCommand(command);

      if (m_link.idle_countdown != 0 && --m_link.idle_countdown == 0)
      {
        m_link.attempts = 0;
        m_link.state = LinkState::Status;
      }

      m_link.wake_clock = next_loop(sent);
      return;
    }

    case LinkState::BlockDone:
      m_link.wake_clock = next_loop(sent);
      return;

    case LinkState::Profile:
    {
      std::array<u8, 11> command{};
      command[0] = UINT8_C(0x71);
      command[1] = UINT8_C(0x02);
      command[2] = m_link.transaction_id;
      std::copy(m_link.profile_id.begin(), m_link.profile_id.end(), command.begin() + 3);
      m_link.reply_received = false;
      sent = SendLEDCommand(command);
      break;
    }

    case LinkState::Block:
    {
      std::array<u8, 72> command{};
      command[0] = UINT8_C(0x71);
      command[1] = UINT8_C(0x05);
      std::copy(m_link.block.begin(), m_link.block.end(), command.begin() + 2);
      m_link.reply_received = false;
      sent = SendLEDCommand(command);
      break;
    }

    case LinkState::Commit:
    {
      const std::array<u8, 3> command = {UINT8_C(0x71), UINT8_C(0x06), m_link.transaction_id};
      m_link.reply_received = false;
      sent = SendLEDCommand(command);
      break;
    }

    case LinkState::Status:
    case LinkState::CommitFinish:
    case LinkState::Mode:
    {
      const u8 subcommand = (m_link.state == LinkState::Status)       ? UINT8_C(0x04) :
                            (m_link.state == LinkState::CommitFinish) ? UINT8_C(0x07) :
                                                                         UINT8_C(0x08);
      const std::array<u8, 2> command = {UINT8_C(0x71), subcommand};
      m_link.reply_received = false;
      sent = SendLEDCommand(command);
      break;
    }
  }

  // Transaction states wait two timer passes for the LED reply.
  m_link.started_generation = m_link.generation;
  m_link.wake_clock = next_loop(sent);
  m_link.step = LinkStep::SecondWait;
}

bool NamcoCyberLead::LEDReplyAccepted() const
{
  return m_link.reply_received && m_link.reply[3] == UINT8_C(0x01) && m_link.reply[4] == UINT8_C(0x01);
}

void NamcoCyberLead::EvaluateLinkState()
{
  const u8 result = m_link.reply[5];
  switch (m_link.state)
  {
    case LinkState::Profile:
      if (++m_link.attempts >= 0x80)
        m_link.state = LinkState::Idle;
      else if (LEDReplyAccepted() && result != 0)
      {
        m_link.profile_result = result;
        m_link.state = LinkState::Idle;
      }
      break;

    case LinkState::Status:
      if (++m_link.attempts >= 0x60 || LEDReplyAccepted())
        m_link.state = LinkState::Idle;
      break;

    case LinkState::Block:
      if (LEDReplyAccepted() && result == 0x01)
      {
        m_link.transfer_result = result;
        m_link.state = LinkState::BlockDone;
      }
      break;

    case LinkState::Commit:
    case LinkState::CommitFinish:
      // Ver1.03 tests the receive flag where it means to test the first reply
      // byte (1360/13CC load C403 into R0H but compare R0L).
      if (m_link.reply_received && m_link.reply[4] == 0x01 && result == 0x01)
      {
        if (m_link.state == LinkState::Commit)
        {
          m_link.state = LinkState::CommitFinish;
        }
        else
        {
          m_link.transfer_result = result;
          m_link.state = LinkState::Idle;
        }
      }
      break;

    case LinkState::Mode:
      if (LEDReplyAccepted())
        m_link.state = LinkState::Idle;
      break;

    case LinkState::Idle:
    case LinkState::BlockDone:
      break;
  }
}

void NamcoCyberLead::ReceiveLEDByte(u8 value)
{
  // SCI1 receive interrupt (15F2): E0 starts a frame, D0 escapes the next
  // byte (sent as value - 1), the destination must be 00 (the I/O PCB) and the
  // checksum is the sum of destination, length and payload.
  Link& link = m_link;
  if (value == UINT8_C(0xE0))
  {
    link.reply_previous = value;
    link.reply[0] = value;
    link.reply_size = 1;
    link.reply_state = 1;
    return;
  }

  if (link.reply_state == 0)
    return;

  const u8 previous = link.reply_previous;
  link.reply_previous = value;
  if (value == UINT8_C(0xD0))
    return;
  if (previous == UINT8_C(0xD0))
    value++;

  if (link.reply_size >= link.reply.size())
  {
    link.reply_state = 0;
    return;
  }
  link.reply[link.reply_size++] = value;

  switch (link.reply_state)
  {
    case 1:
      link.reply_state = (value == 0) ? 2 : 0;
      break;

    case 2:
      link.reply_remaining = value;
      link.reply_state = (value != 0 && value < 0x20) ? 3 : 0;
      break;

    default:
    {
      if (--link.reply_remaining != 0)
        break;

      u8 checksum = 0;
      for (u32 i = 1; i + 1 < link.reply_size; i++)
        checksum = static_cast<u8>(checksum + link.reply[i]);
      if (checksum == link.reply[link.reply_size - 1])
        link.reply_received = true;
      link.reply_state = 0;
      break;
    }
  }
}

NamcoCyberLead::PacketResult NamcoCyberLead::ProcessPacket(std::span<const u8> packet)
{
  PacketResult result;

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
        m_led_link.SetAddress(m_address);
        m_link.idle_countdown = LINK_ADDRESS_COUNTDOWN;
        SetLinkState(LinkState::Idle);
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

        const bool test = ArcadeInput::IsOperatorPressed("Test");
        const std::array<u32, 2> player_inputs = {ReadPlayerInputs(0), ReadPlayerInputs(1)};

        AppendResponseByte(response, response_length, UINT8_C(0x01));
        AppendResponseByte(response, response_length, test ? UINT8_C(0x80) : UINT8_C(0x00));


        for (u32 player = 0; player < players; player++)
        {
          const u32 value = player_inputs[player];
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


        // Ver1.03 reports coins inserted minus coins used (0x30).
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        for (u32 slot = 0; slot < count; slot++)
        {
          if (m_coin_used[slot] > m_coin_counter[slot])
            m_coin_used[slot] = m_coin_counter[slot];
          const u16 coin = static_cast<u16>(m_coin_counter[slot] - m_coin_used[slot]);
          AppendResponseByte(response, response_length, static_cast<u8>((coin >> 8) & 0x3F));
          AppendResponseByte(response, response_length, static_cast<u8>(coin));
        }

        produce_response = true;
        break;
      }

      case UINT8_C(0x30): // subtract from coin counter
      {
        // Ver1.03 (0B9C) numbers the slots from 0, and on a bad slot reports
        // 02 without consuming the amount bytes.
        if (index >= commands_end)
          return result;

        const u8 slot = packet[index++];
        produce_response = true;
        if (slot >= m_coin_counter.size())
        {
          AppendResponseByte(response, response_length, UINT8_C(0x02));
          break;
        }

        if ((index + 2) > commands_end)
          return result;

        const u16 amount = static_cast<u16>((static_cast<u16>(packet[index]) << 8) | packet[index + 1]);
        index += 2;
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        if ((amount & UINT16_C(0x8000)) == 0)
        {
          m_coin_used[slot] =
            static_cast<u16>(std::min<u32>(static_cast<u32>(m_coin_used[slot]) + amount, m_coin_counter[slot]));
        }
        break;
      }

      case UINT8_C(0x32): // general-purpose output: byte count, then data
      {
        // Ver1.03 stores up to eight bytes at C080; their use is not modelled.
        if (index >= commands_end)
          return result;

        const u8 count = packet[index++];
        if ((index + count) > commands_end)
          return result;

        index += count;

        AppendResponseByte(response, response_length, UINT8_C(0x01));
        produce_response = true;
        break;
      }

      case UINT8_C(0x70): // Namco manufacturer-specific (table 0C0C, index & 3F)
      {
        if (index >= commands_end)
          return result;

        const u8 subcommand = packet[index++] & UINT8_C(0x3F);
        AppendResponseByte(response, response_length, UINT8_C(0x01));

        switch (subcommand)
        {
          case UINT8_C(0x01): // read 8 bytes of I/O C77 memory
          {
            if ((index + 2) > commands_end)
              return result;

            const u16 address = static_cast<u16>((static_cast<u16>(packet[index]) << 8) | packet[index + 1]);
            index += 2;
            for (u32 i = 0; i < 8; i++)
              AppendResponseByte(response, response_length, ReadBoardMemory(static_cast<u16>(address + i)));
            break;
          }

          case UINT8_C(0x02): // program date
            for (const u8 value : PROGRAM_DATE)
              AppendResponseByte(response, response_length, value);
            break;

          case UINT8_C(0x03): // DIP switches (C00F)
            AppendResponseByte(response, response_length, UINT8_C(0x00));
            break;

          case UINT8_C(0x04):
            AppendResponseByte(response, response_length, UINT8_C(0xFF));
            AppendResponseByte(response, response_length, UINT8_C(0xFF));
            break;

          case UINT8_C(0x22): // five parameter bytes, no data
            if ((index + 5) > commands_end)
              return result;
            index += 5;
            break;

          default: // every other subcommand only acknowledges
            break;
        }

        produce_response = true;
        break;
      }

      case UINT8_C(0x71): // Cyber Lead LED link (table 0D0A, index & 0F)
      {
        if (index >= commands_end)
          return result;

        const u8 subcommand = packet[index++] & UINT8_C(0x0F);
        AppendResponseByte(response, response_length, UINT8_C(0x01));
        produce_response = true;

        // 71/05 and 71/06 merge ~FF80 bits 4-7 into the high nibble of the
        // status; those inputs read high on the PCB, so the nibble is 0.
        switch (subcommand)
        {
          case UINT8_C(0x01): // eight bytes for the periodic 71/01 frame
          {
            if ((index + 8) > commands_end)
              return result;

            std::copy_n(packet.begin() + index, 8, m_link.control_data.begin());
            index += 8;
            break;
          }

          case UINT8_C(0x02): // profile query: transaction ID + 8-byte profile name
          {
            if ((index + 9) > commands_end)
              return result;

            const u8 id = packet[index];
            u8 status = 0;
            if (id == m_link.transaction_id && m_link.profile_result != 0)
            {
              status = m_link.profile_result;
            }
            else if (id != m_link.transaction_id || ++m_link.poll_count >= 0x10)
            {
              m_link.transaction_id = id;
              std::copy_n(packet.begin() + index + 1, 8, m_link.profile_id.begin());
              m_link.poll_count = 0;
              m_link.attempts = 0;
              m_link.profile_result = 0;
              m_link.idle_countdown = LINK_TRANSACTION_COUNTDOWN;
              SetLinkState(LinkState::Profile);
            }

            AppendResponseByte(response, response_length, status);
            index += 9;
            break;
          }

          case UINT8_C(0x04):
            m_link.attempts = 0;
            m_link.idle_countdown = 0;
            SetLinkState(LinkState::Status);
            break;

          case UINT8_C(0x05): // block ID, 67 data bytes, sum/XOR check word
          {
            if ((index + 70) > commands_end)
              return result;

            const u8 id = packet[index];
            u8 status = m_link.transfer_result;
            if (id != m_link.transaction_id || (status == 0 && ++m_link.poll_count >= 0x10))
            {
              u8 sum = id;
              u8 xor_value = id;
              for (u32 i = 1; i <= 67; i++)
              {
                sum = static_cast<u8>(sum + packet[index + i]);
                xor_value = static_cast<u8>(xor_value ^ packet[index + i]);
              }

              status = UINT8_C(0x02);
              if (packet[index + 68] == sum && packet[index + 69] == xor_value)
              {
                m_link.transaction_id = id;
                m_link.poll_count = 0;
                m_link.transfer_result = 0;
                std::copy_n(packet.begin() + index, 70, m_link.block.begin());
                SetLinkState(LinkState::Block);
                status = 0;
              }
            }

            AppendResponseByte(response, response_length, status);
            index += 70;
            break;
          }

          case UINT8_C(0x06): // commit: transaction ID
          {
            if (index >= commands_end)
              return result;

            const u8 id = packet[index++];
            u8 status = m_link.transfer_result;
            if (id != m_link.transaction_id || (status == 0 && ++m_link.poll_count >= 0x20))
            {
              m_link.transaction_id = id;
              m_link.poll_count = 0;
              m_link.transfer_result = 0;
              m_link.idle_countdown = LINK_TRANSACTION_COUNTDOWN;
              SetLinkState(LinkState::Commit);
              status = 0;
            }

            AppendResponseByte(response, response_length, status);
            break;
          }

          case UINT8_C(0x08):
            m_link.idle_countdown = 0;
            SetLinkState(LinkState::Mode);
            break;

          default: // every other subcommand only acknowledges
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
