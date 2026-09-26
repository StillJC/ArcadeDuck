// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/konami/gq/konami_gq_pcm.h"

#include "common/log.h"

#include <algorithm>
#include <array>

Log_SetChannel(KonamiGQPCM);

namespace KonamiGQPCM {

namespace {

static constexpr u32 WINDOW_BASE_OFFSET = 0x300000;
static constexpr u32 WINDOW_END_OFFSET = 0x5fffff;
static constexpr u32 LAST_ACTIVE_OFFSET = WINDOW_END_OFFSET - 1;

struct State
{
  std::array<u8, RAM_SIZE> ram = {};
  bool active = false;
  bool first_access_logged = false;
  bool first_read_logged = false;
  bool write_sweep_started = false;
  bool write_sweep_completed = false;
  u32 active_byte_writes = 0;
  u32 active_byte_reads = 0;
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

static u32 GetRAMIndex(u32 offset)
{
  return (offset - WINDOW_BASE_OFFSET) >> 1;
}

static void LogFirstAccess(u32 width, u32 offset, bool write)
{
  if (s_state.first_access_logged)
    return;

  s_state.first_access_logged = true;
  DEV_LOG("KonamiGQ.PCMRAM first_access operation='{}' width={} offset=0x{:06X}", write ? "write" : "read",
           width, offset);
}

static u8 ReadWindowByte(u32 offset)
{
  if (!IsActiveByteOffset(offset))
    return 0;

  return ReadRAMByte(GetRAMIndex(offset));
}

static void WriteByte(u32 offset, u8 value)
{
  if (!IsActiveByteOffset(offset))
    return;

  const u32 index = GetRAMIndex(offset);
  if (index >= s_state.ram.size())
    return;

  if (!s_state.write_sweep_started)
  {
    s_state.write_sweep_started = true;
    DEV_LOG("KonamiGQ.PCMRAM write_sweep_started offset=0x{:06X} ram_index=0x{:06X}", offset, index);
  }

  s_state.ram[index] = value;
  s_state.active_byte_writes++;

  if (!s_state.write_sweep_completed && offset == LAST_ACTIVE_OFFSET)
  {
    s_state.write_sweep_completed = true;
    DEV_LOG("KonamiGQ.PCMRAM write_sweep_completed active_byte_writes={} size={}", s_state.active_byte_writes,
             static_cast<u32>(s_state.ram.size()));
  }
}

} // namespace

void Initialize()
{
  Shutdown();
  s_state.active = true;
  VERBOSE_LOG("KonamiGQ.PCMRAM initialized size={} psx_window='0x1F300000-0x1F5FFFFF' lane_mask='0x00FF00FF'",
           static_cast<u32>(s_state.ram.size()));
}

void Reset()
{
  if (!s_state.active)
    return;

  std::fill(s_state.ram.begin(), s_state.ram.end(), u8{0});
  s_state.first_access_logged = false;
  s_state.first_read_logged = false;
  s_state.write_sweep_started = false;
  s_state.write_sweep_completed = false;
  s_state.active_byte_writes = 0;
  s_state.active_byte_reads = 0;
  VERBOSE_LOG("KonamiGQ.PCMRAM reset size={}", static_cast<u32>(s_state.ram.size()));
}

void Shutdown()
{
  if (s_state.active)
  {
    VERBOSE_LOG("KonamiGQ.PCMRAM shutdown active_byte_writes={} active_byte_reads={}", s_state.active_byte_writes,
             s_state.active_byte_reads);
  }

  std::fill(s_state.ram.begin(), s_state.ram.end(), u8{0});
  s_state.active = false;
  s_state.first_access_logged = false;
  s_state.first_read_logged = false;
  s_state.write_sweep_started = false;
  s_state.write_sweep_completed = false;
  s_state.active_byte_writes = 0;
  s_state.active_byte_reads = 0;
}

bool IsActive()
{
  return s_state.active;
}

u32 Read(u32 width, u32 offset)
{
  if (!s_state.active || !IsWindowOffset(offset))
    return UINT32_C(0xffffffff);

  LogFirstAccess(width, offset, false);
  if (!s_state.first_read_logged)
  {
    s_state.first_read_logged = true;
    DEV_LOG("KonamiGQ.PCMRAM first_read width={} offset=0x{:06X} ram_index=0x{:06X}", width, offset,
             IsActiveByteOffset(offset) ? GetRAMIndex(offset) : UINT32_C(0xffffffff));
  }

  switch (width)
  {
    case 1:
      return ReadWindowByte(offset);

    case 2:
      return ReadWindowByte(offset);

    case 4:
      return static_cast<u32>(ReadWindowByte(offset)) | (static_cast<u32>(ReadWindowByte(offset + 2)) << 16);

    default:
      ERROR_LOG("KonamiGQ.PCMRAM unsupported_read_width width={} offset=0x{:06X}", width, offset);
      return UINT32_C(0xffffffff);
  }
}

void Write(u32 width, u32 offset, u32 value)
{
  if (!s_state.active || !IsWindowOffset(offset))
    return;

  LogFirstAccess(width, offset, true);

  switch (width)
  {
    case 1:
      WriteByte(offset, static_cast<u8>(value));
      break;

    case 2:
      WriteByte(offset, static_cast<u8>(value));
      break;

    case 4:
      WriteByte(offset, static_cast<u8>(value));
      WriteByte(offset + 2, static_cast<u8>(value >> 16));
      break;

    default:
      ERROR_LOG("KonamiGQ.PCMRAM unsupported_write_width width={} offset=0x{:06X} value=0x{:08X}", width, offset,
                value);
      break;
  }
}

u8 ReadRAMByte(u32 index)
{
  if (!s_state.active || index >= s_state.ram.size())
    return 0;

  s_state.active_byte_reads++;
  return s_state.ram[index];
}

} // namespace KonamiGQPCM
