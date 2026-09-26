// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "legacy_pad_state.h"
#include "types.h"

#include "util/state_wrapper.h"

#include <array>

namespace LegacyPadState {
namespace {
constexpr size_t MEMORY_CARD_STATE_SIZE = 1 + 1 + 2 + 1 + 1 + 1 + (128 * 1024) + 1;
constexpr size_t MULTITAP_STATE_SIZE = 1 + 1 + 4 + 1 + 1 + 1 + 32;

size_t GetControllerStateSize(ControllerType type, u32 version)
{
  switch (type)
  {
    case ControllerType::DigitalController:
    case ControllerType::NeGcon:
      return 3;

    case ControllerType::AnalogController:
      if (version >= 55)
        return 26;
      if (version >= 45)
        return 25;
      return (version >= 44) ? 10 : 7;

    case ControllerType::AnalogJoystick:
      return 8;

    case ControllerType::GunCon:
      return 7;

    case ControllerType::PlayStationMouse:
      return (version >= 60) ? 11 : 5;

    case ControllerType::NeGconRumble:
      if (version >= 55)
        return 24;
      if (version >= 45)
        return 23;
      return (version >= 44) ? 8 : 6;

    case ControllerType::Justifier:
      return 11;

    case ControllerType::None:
    default:
      return 0;
  }
}

bool ReadLegacyState(StateWrapper& sw)
{
  const u32 port_count = (sw.GetVersion() >= 50) ? NUM_CONTROLLER_AND_CARD_PORTS : 2;
  for (u32 i = 0; i < port_count; i++)
  {
    u32 type_value = 0;
    sw.Do(&type_value);
    if (type_value >= static_cast<u32>(ControllerType::Count))
      return false;

    const ControllerType type = static_cast<ControllerType>(type_value);
    if (type != ControllerType::None)
    {
      if (!sw.DoMarker("Controller"))
        return false;
      sw.SkipBytes(GetControllerStateSize(type, sw.GetVersion()));
    }

    bool card_present = false;
    sw.Do(&card_present);
    if (card_present)
    {
      if (!sw.DoMarker("MemoryCard"))
        return false;
      sw.SkipBytes(MEMORY_CARD_STATE_SIZE);
    }
  }

  if (sw.GetVersion() >= 50)
    sw.SkipBytes(MULTITAP_STATE_SIZE * NUM_MULTITAPS);

  // Pad transfer state, JOY registers, buffers, and buffer-full flags.
  sw.SkipBytes(4 + 2 + 4 + 2 + 2 + 1 + 1 + 1 + 1);
  return !sw.HasError();
}

bool WriteEmptyState(StateWrapper& sw)
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    u32 type = static_cast<u32>(ControllerType::None);
    bool card_present = false;
    sw.Do(&type);
    sw.Do(&card_present);
  }

  std::array<u8, MULTITAP_STATE_SIZE * NUM_MULTITAPS> multitaps{};
  sw.DoBytes(multitaps.data(), multitaps.size());

  u32 state = 0;
  u16 joy_ctrl = 0;
  u32 joy_stat = 5;
  u16 joy_mode = 0;
  u16 joy_baud = 0;
  u8 receive_buffer = 0;
  u8 transmit_buffer = 0;
  bool receive_buffer_full = false;
  bool transmit_buffer_full = false;
  sw.Do(&state);
  sw.Do(&joy_ctrl);
  sw.Do(&joy_stat);
  sw.Do(&joy_mode);
  sw.Do(&joy_baud);
  sw.Do(&receive_buffer);
  sw.Do(&transmit_buffer);
  sw.Do(&receive_buffer_full);
  sw.Do(&transmit_buffer_full);
  return !sw.HasError();
}
} // namespace

bool DoState(StateWrapper& sw)
{
  return sw.IsReading() ? ReadLegacyState(sw) : WriteEmptyState(sw);
}

} // namespace LegacyPadState
