// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/musashi/m68k_bus.h"

namespace MusashiBus {
namespace {

Callbacks s_callbacks;

} // namespace

u32 Read8(u32 address)
{
  return s_callbacks.read8 ? s_callbacks.read8(address) : UINT32_C(0xff);
}

u32 Read16(u32 address)
{
  return s_callbacks.read16 ? s_callbacks.read16(address) : UINT32_C(0xffff);
}

u32 Read32(u32 address)
{
  return s_callbacks.read32 ? s_callbacks.read32(address) : UINT32_C(0xffffffff);
}

void Write8(u32 address, u32 value)
{
  if (s_callbacks.write8)
    s_callbacks.write8(address, value);
}

void Write16(u32 address, u32 value)
{
  if (s_callbacks.write16)
    s_callbacks.write16(address, value);
}

void Write32(u32 address, u32 value)
{
  if (s_callbacks.write32)
    s_callbacks.write32(address, value);
}

void SetCallbacks(const Callbacks& callbacks)
{
  s_callbacks = callbacks;
}

void ClearCallbacks()
{
  s_callbacks = {};
}

} // namespace MusashiBus

extern "C" {

unsigned int m68k_read_memory_8(unsigned int address)
{
  return MusashiBus::Read8(address);
}

unsigned int m68k_read_memory_16(unsigned int address)
{
  return MusashiBus::Read16(address);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
  return MusashiBus::Read32(address);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
  MusashiBus::Write8(address, value);
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
  MusashiBus::Write16(address, value);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
  MusashiBus::Write32(address, value);
}

} // extern "C"