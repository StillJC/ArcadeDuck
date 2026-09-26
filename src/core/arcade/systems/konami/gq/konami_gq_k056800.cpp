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
static constexpr u32 ACCESS_LOG_LIMIT = 32;

struct State
{
  std::array<u8, 4> host_to_sound = {};
  std::array<u8, 2> sound_to_host = {};
  u8 front_volume = 0;
  u8 rear_volume = 0;
  u8 common_volume = 0;
  u8 output_control = 0;
  bool output_control_seen = false;
  std::array<u8, 8> sound_control = {};
  std::array<bool, 8> sound_control_seen = {};
  std::array<u32, 8> sound_control_writes = {};
  u32 output_control_writes = 0;
  bool interrupt_pending = false;
  bool interrupt_enabled = false;
  bool active = false;
  bool first_access_logged = false;
  u32 host_reads = 0;
  u32 host_writes = 0;
  u32 sound_reads = 0;
  u32 sound_writes = 0;
  u32 host_commands = 0;
  u32 access_logs = 0;
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

static void LogFirstAccess(bool write, u32 width, u32 offset)
{
  if (s_state.first_access_logged)
    return;

  s_state.first_access_logged = true;
  DEV_LOG("KonamiGQ.K056800 first_access operation='{}' width={} offset=0x{:06X}", write ? "write" : "read",
           width, offset);
}

static bool ShouldLogAccess()
{
  if (s_state.access_logs >= ACCESS_LOG_LIMIT)
    return false;

  s_state.access_logs++;
  return true;
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

static void TraceSoundControlWrite(u32 reg, u8 value)
{
  if (reg >= s_state.sound_control.size())
    return;

  const u8 old_value = s_state.sound_control[reg];
  const bool first_write = !s_state.sound_control_seen[reg];
  s_state.sound_control_seen[reg] = true;
  s_state.sound_control[reg] = value;
  s_state.sound_control_writes[reg]++;

  if (!first_write && old_value == value)
    return;

  if (reg == 5)
  {
    DEV_LOG(
      "KonamiGQ.K056800 sound_control_transition register=5 write_index={} first={} old=0x{:02X} new=0x{:02X} "
      "field_high={} field_low={} reg2=0x{:02X} reg3=0x{:02X}",
      s_state.sound_control_writes[reg], first_write, old_value, value, (value >> 4) & 0x07u, value & 0x07u,
      s_state.sound_control[2], s_state.sound_control[3]);
  }
  else
  {
    DEV_LOG(
      "KonamiGQ.K056800 sound_control_transition register={} write_index={} first={} old=0x{:02X} new=0x{:02X} "
      "reg2=0x{:02X} reg3=0x{:02X} reg5=0x{:02X}",
      reg, s_state.sound_control_writes[reg], first_write, old_value, value, s_state.sound_control[2],
      s_state.sound_control[3], s_state.sound_control[5]);
  }
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
      if (s_state.host_commands <= 16 || (s_state.host_commands & (s_state.host_commands - 1)) == 0)
      {
        DEV_LOG(
          "KonamiGQ.K056800 host_command index={} command=0x{:02X} args='{:02X} {:02X} {:02X}' "
          "interrupt_enabled={} interrupt_pending={} pc=0x{:08X}",
          s_state.host_commands, s_state.host_to_sound[0], s_state.host_to_sound[1], s_state.host_to_sound[2],
          s_state.host_to_sound[3], s_state.interrupt_enabled, s_state.interrupt_pending, pc);
      }
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

  if (ShouldLogAccess())
  {
    DEV_LOG(
      "KonamiGQ.K056800 host_read index={} offset=0x{:06X} register={} value=0x{:02X} "
      "sound_to_host='{:02X} {:02X}'",
      s_state.host_reads, offset, reg, value, s_state.sound_to_host[0], s_state.sound_to_host[1]);
  }

  return value;
}

static void WriteActiveByte(u32 offset, u8 value, u32 pc)
{
  if (!IsActiveByteOffset(offset))
    return;

  const u32 reg = GetRegister(offset);
  s_state.host_writes++;
  WriteHostRegister(reg, value, pc);

  if (ShouldLogAccess())
  {
    DEV_LOG(
      "KonamiGQ.K056800 host_write index={} offset=0x{:06X} register={} value=0x{:02X} "
      "host_to_sound='{:02X} {:02X} {:02X} {:02X}' front_volume={} rear_volume={} output_control=0x{:02X} "
      "pc=0x{:08X}",
      s_state.host_writes, offset, reg, value, s_state.host_to_sound[0], s_state.host_to_sound[1],
      s_state.host_to_sound[2], s_state.host_to_sound[3], s_state.front_volume, s_state.rear_volume,
      s_state.output_control, pc);
  }
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
  s_state.sound_control.fill(0);
  s_state.sound_control_seen.fill(false);
  s_state.sound_control_writes.fill(0);
  s_state.output_control_writes = 0;
  s_state.interrupt_pending = false;
  s_state.interrupt_enabled = false;
  s_state.first_access_logged = false;
  s_state.host_reads = 0;
  s_state.host_writes = 0;
  s_state.sound_reads = 0;
  s_state.sound_writes = 0;
  s_state.host_commands = 0;
  s_state.access_logs = 0;
  VERBOSE_LOG("KonamiGQ.K056800 reset");
}

void Shutdown()
{
  if (s_state.active)
  {
    VERBOSE_LOG(
      "KonamiGQ.K056800 shutdown host_reads={} host_writes={} sound_reads={} sound_writes={} host_commands={} "
      "interrupt_enabled={} interrupt_pending={} front_volume={} rear_volume={} output_control=0x{:02X} "
      "output_control_writes={} sound_reg2=0x{:02X} sound_reg2_writes={} sound_reg3=0x{:02X} "
      "sound_reg3_writes={} sound_reg5=0x{:02X} sound_reg5_high={} sound_reg5_low={} sound_reg5_writes={}",
      s_state.host_reads, s_state.host_writes, s_state.sound_reads, s_state.sound_writes, s_state.host_commands,
      s_state.interrupt_enabled, s_state.interrupt_pending, s_state.front_volume, s_state.rear_volume,
      s_state.output_control, s_state.output_control_writes, s_state.sound_control[2], s_state.sound_control_writes[2],
      s_state.sound_control[3], s_state.sound_control_writes[3], s_state.sound_control[5],
      (s_state.sound_control[5] >> 4) & 0x07u, s_state.sound_control[5] & 0x07u,
      s_state.sound_control_writes[5]);
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

  LogFirstAccess(false, width, offset);

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

  LogFirstAccess(true, width, offset);

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
  if (ShouldLogAccess())
  {
    DEV_LOG("KonamiGQ.K056800 sound_read index={} register={} value=0x{:02X}", s_state.sound_reads, reg, value);
  }
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
      // Unknown sound-side MIRAC controls. Trace transitions independently of
      // the generic 256-access log cap so later gameplay changes are visible.
      TraceSoundControlWrite(reg, value);
      break;

    case 4:
      s_state.interrupt_enabled = (value & 1u) != 0;
      if (!s_state.interrupt_enabled)
        s_state.interrupt_pending = false;
      break;

    case 5:
      // Crypt Killer firmware writes 0x44/0x77 through a helper that encodes
      // two independent three-bit fields. Keep semantics unknown for now and
      // capture every transition for correlation with speech/music/effects.
      TraceSoundControlWrite(reg, value);
      break;

    default:
      break;
  }

  if (ShouldLogAccess())
  {
    DEV_LOG(
      "KonamiGQ.K056800 sound_write index={} register={} value=0x{:02X} sound_to_host='{:02X} {:02X}' "
      "interrupt_enabled={} interrupt_pending={}",
      s_state.sound_writes, reg, value, s_state.sound_to_host[0], s_state.sound_to_host[1],
      s_state.interrupt_enabled, s_state.interrupt_pending);
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
