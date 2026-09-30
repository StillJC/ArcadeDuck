// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system10/namco_system10.h"

#include "core/arcade/systems/namco/system10/namco_system10_memn.h"

#include "common/error.h"
#include "common/log.h"

#include <optional>
#include <algorithm>
#include <optional>
#include <span>
#include <utility>

Log_SetChannel(NamcoSystem10);

namespace NamcoSystem10 {
namespace {

static constexpr u32 MEMN_READY_BASE = 0x400000;
static constexpr u32 MEMN_COMMAND_BASE = 0x410000;
static constexpr u32 MEMN_COLUMN_BASE = 0x420000;
static constexpr u32 MEMN_ROW_LOW_BASE = 0x430000;
static constexpr u32 MEMN_ROW_HIGH_BASE = 0x440000;
static constexpr u32 MEMN_DATA_BASE = 0x450000;
static constexpr u32 MEMN_DEVICE_BASE = 0x460000;
static constexpr u32 MEMN_CONTROL_BASE = 0x470000;
static constexpr u32 LOOKUP_RAM_BASE = 0x500000;
static constexpr u32 LOOKUP_RAM_SIZE = 0x100000;

struct RuntimeState
{
  std::string set_name;
  MemNRawNAND nand0;
  MemNRawNAND nand1;
  std::vector<u8> lookup_ram;
  u16 control_latch = 0;
  u16 nand_device = 0;
};

std::optional<RuntimeState> s_runtime;

MemNRawNAND* GetSelectedNAND(RuntimeState& runtime)
{
  if (runtime.nand_device == 0)
    return &runtime.nand0;
  if (runtime.nand_device == 1)
    return &runtime.nand1;

  return nullptr;
}

const MemNRawNAND* GetSelectedNAND(const RuntimeState& runtime)
{
  if (runtime.nand_device == 0)
    return &runtime.nand0;
  if (runtime.nand_device == 1)
    return &runtime.nand1;

  return nullptr;
}

bool ReadLittleEndian(std::span<const u8> bytes, u32 offset, u32 width, u32* value)
{
  if (!value || (width != 1 && width != 2 && width != 4) || offset > bytes.size() ||
      width > (bytes.size() - offset))
  {
    return false;
  }

  u32 result = 0;
  for (u32 i = 0; i < width; i++)
    result |= static_cast<u32>(bytes[offset + i]) << (i * 8);

  *value = result;
  return true;
}

bool WriteLittleEndian(std::span<u8> bytes, u32 offset, u32 width, u32 value)
{
  if ((width != 1 && width != 2 && width != 4) || offset > bytes.size() || width > (bytes.size() - offset))
    return false;

  for (u32 i = 0; i < width; i++)
    bytes[offset + i] = static_cast<u8>(value >> (i * 8));

  return true;
}

bool ReadU16Register(u16 reg, u32 byte_offset, u32 width, u32* value)
{
  if (!value || byte_offset >= sizeof(reg) || (width != 1 && width != 2) || width > (sizeof(reg) - byte_offset))
    return false;

  *value = (static_cast<u32>(reg) >> (byte_offset * 8)) & ((width == 1) ? UINT32_C(0xff) : UINT32_C(0xffff));
  return true;
}

} // namespace

bool InitializeMemN(MemNLoadedContent content, Error* error)
{
  if (s_runtime.has_value())
  {
    Error::SetStringView(error, "Namco System 10 runtime is already active.");
    return false;
  }

  if (content.set_name.empty())
  {
    Error::SetStringView(error, "Namco System 10 MEM(N) content has no set identity.");
    return false;
  }

  RuntimeState runtime;
  runtime.set_name = std::move(content.set_name);

  if (!runtime.nand0.Load(std::move(content.nand0), error) ||
      !runtime.nand1.Load(std::move(content.nand1), error))
  {
    return false;
  }

  runtime.lookup_ram.resize(LOOKUP_RAM_SIZE, 0);

  VERBOSE_LOG("Namco System 10 MEM(N) runtime initialized set='{}' nand0={} nand1={}.", runtime.set_name,
              runtime.nand0.GetRawImage().size(), runtime.nand1.GetRawImage().size());

  s_runtime = std::move(runtime);
  return true;
}

void Reset()
{
  if (!s_runtime.has_value())
    return;

  s_runtime->nand0.ResetInterface();
  s_runtime->nand1.ResetInterface();
  s_runtime->control_latch = 0;
  s_runtime->nand_device = 0;
  std::fill(s_runtime->lookup_ram.begin(), s_runtime->lookup_ram.end(), 0);
}

void Shutdown()
{
  s_runtime.reset();
}

bool IsActive()
{
  return s_runtime.has_value();
}

bool ReadEXP1(u32 width, u32 offset, u32* value)
{
  if (!s_runtime.has_value() || !value)
    return false;

  RuntimeState& runtime = s_runtime.value();

  if (offset >= LOOKUP_RAM_BASE && offset < (LOOKUP_RAM_BASE + LOOKUP_RAM_SIZE))
  {
    return ReadLittleEndian(std::span<const u8>(runtime.lookup_ram.data(), runtime.lookup_ram.size()),
                            offset - LOOKUP_RAM_BASE, width, value);
  }

  if (offset >= MEMN_READY_BASE && offset < (MEMN_READY_BASE + 2))
  {
    const MemNRawNAND* nand = GetSelectedNAND(runtime);
    if (!nand)
      return false;

    // The System 10 register is software-visible as zero while the selected
    // K9F2808 is ready. Busy timing is not modeled in this read-side milestone.
    return ReadU16Register(nand->IsReady() ? UINT16_C(0x0000) : UINT16_C(0x0001),
                           offset - MEMN_READY_BASE, width, value);
  }

  if (offset >= MEMN_DATA_BASE && offset < (MEMN_DATA_BASE + 2))
  {
    MemNRawNAND* nand = GetSelectedNAND(runtime);
    if (!nand)
      return false;

    if (offset != MEMN_DATA_BASE)
      return false;

    if (width == 1)
    {
      *value = nand->DataRead();
      return true;
    }

    if (width == 2)
    {
      // MEM(N) presents two consecutive 8-bit NAND reads as one 16-bit host word,
      // first NAND byte in the high byte.
      const u16 first = nand->DataRead();
      const u16 second = nand->DataRead();
      *value = static_cast<u16>((first << 8) | second);
      return true;
    }

    return false;
  }

  if (offset >= MEMN_CONTROL_BASE && offset < (MEMN_CONTROL_BASE + 2))
    return ReadU16Register(runtime.control_latch, offset - MEMN_CONTROL_BASE, width, value);

  return false;
}

bool WriteEXP1(u32 width, u32 offset, u32 value)
{
  if (!s_runtime.has_value())
    return false;

  RuntimeState& runtime = s_runtime.value();

  if (offset >= LOOKUP_RAM_BASE && offset < (LOOKUP_RAM_BASE + LOOKUP_RAM_SIZE))
  {
    return WriteLittleEndian(std::span<u8>(runtime.lookup_ram.data(), runtime.lookup_ram.size()),
                             offset - LOOKUP_RAM_BASE, width, value);
  }

  MemNRawNAND* nand = GetSelectedNAND(runtime);

  if (offset == MEMN_COMMAND_BASE)
  {
    if (!nand || (width != 1 && width != 2 && width != 4))
      return false;

    nand->CommandWrite(static_cast<u8>(value));
    return true;
  }

  if (offset == MEMN_COLUMN_BASE)
  {
    if (!nand || (width != 1 && width != 2 && width != 4))
      return false;

    nand->AddressColumnWrite(static_cast<u8>(value));
    return true;
  }

  if (offset == MEMN_ROW_LOW_BASE)
  {
    if (!nand || (width != 1 && width != 2 && width != 4))
      return false;

    nand->AddressRowLowWrite(static_cast<u8>(value));
    return true;
  }

  if (offset == MEMN_ROW_HIGH_BASE)
  {
    if (!nand || (width != 1 && width != 2 && width != 4))
      return false;

    nand->AddressRowHighWrite(static_cast<u8>(value));
    return true;
  }

  if (offset == MEMN_DEVICE_BASE)
  {
    if (width != 1 && width != 2 && width != 4)
      return false;

    // Preserve the software-written selector instead of clamping it. The first
    // MEM(N) target (Star Trigon) physically populates NAND 0 and NAND 1.
    runtime.nand_device = static_cast<u16>(value);
    return true;
  }

  if (offset >= MEMN_CONTROL_BASE && offset < (MEMN_CONTROL_BASE + 2))
  {
    if (width == 1)
    {
      const u32 shift = (offset - MEMN_CONTROL_BASE) * 8;
      runtime.control_latch =
        static_cast<u16>((runtime.control_latch & ~(UINT16_C(0xff) << shift)) |
                         ((static_cast<u16>(value) & UINT16_C(0xff)) << shift));
      return true;
    }

    if (offset == MEMN_CONTROL_BASE && width == 2)
    {
      // Keep all control bits exactly as written. Only bit 2 has a proven
      // transaction-related role; the meanings of the other bits remain open.
      runtime.control_latch = static_cast<u16>(value);
      return true;
    }

    return false;
  }

  // NAND data program/erase is intentionally deferred.
  return false;
}

} // namespace NamcoSystem10