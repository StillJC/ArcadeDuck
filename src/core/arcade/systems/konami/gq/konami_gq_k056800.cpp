// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/konami/gq/konami_gq_k056800.h"

#include "common/log.h"

#include <array>

Log_SetChannel(KonamiGQK056800);

namespace KonamiGQK056800 {

namespace {

static constexpr u32 WINDOW_BASE_OFFSET = 0x100000;
static constexpr u32 WINDOW_END_OFFSET = 0x10001f;
static constexpr u32 REGISTER_COUNT = 8;

struct State
{
  std::array<u8, 4> host_to_sound = {};
  std::array<u8, 2> sound_to_host = {};
  u8 front_volume = 0;
  u8 rear_volume = 0;
  u8 common_volume = 0;
  u8 output_control = 0;
  bool output_control_seen = false;
  u32 output_control_writes = 0;
  bool interrupt_pending = false;
  bool interrupt_enabled = false;
  bool active = false;
  u32 host_reads = 0;
  u32 host_writes = 0;
  u32 sound_reads = 0;
  u32 sound_writes = 0;
  u32 host_commands = 0;
};

static State s_state;

static bool IsWindowOffset(u32 offset)
{
  return offset >= WINDOW_BASE_OFFSET && offset <= WINDOW_END_OFFSET;
}

static bool IsActiveByteOffset(u32 offset)
{
  return IsWindowOffset(offset) && ((offset - WINDOW_BASE_OFFSET) & 1u) == 0;
}

static u32 GetRegister(u32 offset)
{
  return ((offset - WINDOW_BASE_OFFSET) >> 1) & (REGISTER_COUNT - 1);
}

static u8 ReadHostRegister(u32 reg)
{
  switch (reg & 7u)
  {
    case 0:
    case 1:
      return s_state.sound_to_host[reg & 1u];

    case 2:
      // Bit 0: front-volume busy. Bit 1: rear-volume busy.
      return 0;

    default:
      return 0;
  }
}

static void StepVolume(u8& volume, u8 command)
{
  if (command == 0xcau)
  {
    if (volume != 0xffu)
      volume++;
  }
  else if (command == 0x35u)
  {
    if (volume != 0)
      volume--;
  }
}

static void UpdateCommonVolume()
{
  if (s_state.front_volume == s_state.rear_volume)
    s_state.common_volume = s_state.front_volume;
}

static void WriteHostRegister(u32 reg, u8 value, u32 pc)
{
  reg &= 7u;
  switch (reg)
  {
    case 0:
    case 1:
    case 2:
    case 3:
      s_state.host_to_sound[reg] = value;
      break;

    case 4:
      // Front volume: 0xCA increments, 0x35 decrements.
      StepVolume(s_state.front_volume, value);
      UpdateCommonVolume();
      break;

    case 5:
      // Rear volume: same command encoding as front.
      StepVolume(s_state.rear_volume, value);
      UpdateCommonVolume();
      break;

    case 6:
    {
      // Preserve the raw front/rear control bits. Their exact polarity is not
      // applied yet. Log this control independently of the generic access cap
      // so any runtime transition cannot be hidden by early initialization.
      const u8 old_value = s_state.output_control;
      const bool first_write = !s_state.output_control_seen;
      s_state.output_control_seen = true;
      s_state.output_control = value;
      s_state.output_control_writes++;

      if (first_write || old_value != value)
      {
        DEV_LOG(
          "KonamiGQ.K056800 output_control_transition write_index={} first={} old=0x{:02X} new=0x{:02X} "
          "front_volume={} rear_volume={} pc=0x{:08X}",
          s_state.output_control_writes, first_write, old_value, value, s_state.front_volume, s_state.rear_volume,
          pc);
      }
      break;
    }

    case 7:
    {
      s_state.host_commands++;
      if (s_state.interrupt_enabled)
        s_state.interrupt_pending = true;
      break;
    }

    default:
      break;
  }
}

static u8 ReadActiveByte(u32 offset)
{
  if (!IsActiveByteOffset(offset))
    return 0;

  const u32 reg = GetRegister(offset);
  const u8 value = ReadHostRegister(reg);
  s_state.host_reads++;

  return value;
}

static void WriteActiveByte(u32 offset, u8 value, u32 pc)
{
  if (!IsActiveByteOffset(offset))
    return;

  const u32 reg = GetRegister(offset);
  s_state.host_writes++;
  WriteHostRegister(reg, value, pc);

}

} // namespace

void Initialize()
{
  Shutdown();
  s_state.active = true;
  VERBOSE_LOG(
    "KonamiGQ.K056800 initialized host_window='0x1F100000-0x1F10001F' lane_mask='0x00FF00FF' "
    "sound_window='0x400000-0x40001F'");
}

void Reset()
{
  if (!s_state.active)
    return;

  s_state.host_to_sound.fill(0);
  s_state.sound_to_host.fill(0);
  s_state.front_volume = 0;
  s_state.rear_volume = 0;
  s_state.common_volume = 0;
  s_state.output_control = 0;
  s_state.output_control_seen = false;
  s_state.output_control_writes = 0;
  s_state.interrupt_pending = false;
  s_state.interrupt_enabled = false;
  s_state.host_reads = 0;
  s_state.host_writes = 0;
  s_state.sound_reads = 0;
  s_state.sound_writes = 0;
  s_state.host_commands = 0;
  VERBOSE_LOG("KonamiGQ.K056800 reset");
}

void Shutdown()
{
  if (s_state.active)
  {
    VERBOSE_LOG(
      "KonamiGQ.K056800 shutdown host_reads={} host_writes={} sound_reads={} sound_writes={} host_commands={} "
      "interrupt_enabled={} interrupt_pending={} front_volume={} rear_volume={} output_control=0x{:02X} "
      "output_control_writes={}",
      s_state.host_reads, s_state.host_writes, s_state.sound_reads, s_state.sound_writes, s_state.host_commands,
      s_state.interrupt_enabled, s_state.interrupt_pending, s_state.front_volume, s_state.rear_volume,
      s_state.output_control, s_state.output_control_writes);
  }

  s_state = {};
}

bool IsActive()
{
  return s_state.active;
}

u32 ReadHost(u32 width, u32 offset)
{
  if (!s_state.active || !IsWindowOffset(offset))
    return UINT32_C(0xffffffff);

  switch (width)
  {
    case 1:
      return ReadActiveByte(offset);

    case 2:
      return ReadActiveByte(offset);

    case 4:
      return static_cast<u32>(ReadActiveByte(offset)) | (static_cast<u32>(ReadActiveByte(offset + 2)) << 16);

    default:
      ERROR_LOG("KonamiGQ.K056800 unsupported_host_read_width width={} offset=0x{:06X}", width, offset);
      return UINT32_C(0xffffffff);
  }
}

void WriteHost(u32 width, u32 offset, u32 value, u32 pc)
{
  if (!s_state.active || !IsWindowOffset(offset))
    return;

  switch (width)
  {
    case 1:
      WriteActiveByte(offset, static_cast<u8>(value), pc);
      break;

    case 2:
      WriteActiveByte(offset, static_cast<u8>(value), pc);
      break;

    case 4:
      WriteActiveByte(offset, static_cast<u8>(value), pc);
      WriteActiveByte(offset + 2, static_cast<u8>(value >> 16), pc);
      break;

    default:
      ERROR_LOG("KonamiGQ.K056800 unsupported_host_write_width width={} offset=0x{:06X} value=0x{:08X}",
                width, offset, value);
      break;
  }
}

u8 ReadSound(u32 offset)
{
  if (!s_state.active)
    return 0;

  const u32 reg = offset & 7u;
  u8 value = 0;
  if (reg < s_state.host_to_sound.size())
    value = s_state.host_to_sound[reg];

  s_state.sound_reads++;
  return value;
}

void WriteSound(u32 offset, u8 value)
{
  if (!s_state.active)
    return;

  const u32 reg = offset & 7u;
  s_state.sound_writes++;

  switch (reg)
  {
    case 0:
    case 1:
      s_state.sound_to_host[reg] = value;
      break;

    case 2:
    case 3:
      // Sound-side MIRAC controls are not currently modeled.
      break;

    case 4:
      s_state.interrupt_enabled = (value & 1u) != 0;
      if (!s_state.interrupt_enabled)
        s_state.interrupt_pending = false;
      break;

    case 5:
      // Crypt Killer writes 0x44/0x77 here; the two three-bit fields are not
      // currently modeled.
      break;

    default:
      break;
  }

}

bool IsSoundInterruptPending()
{
  return s_state.active && s_state.interrupt_pending;
}

u8 GetFrontVolume()
{
  return s_state.front_volume;
}

u8 GetRearVolume()
{
  return s_state.rear_volume;
}

u8 GetCommonVolume()
{
  return s_state.common_volume;
}

u8 GetOutputControl()
{
  return s_state.output_control;
}

} // namespace KonamiGQK056800
