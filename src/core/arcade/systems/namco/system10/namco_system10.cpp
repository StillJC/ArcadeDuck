// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system10/namco_system10.h"

#include "core/arcade/systems/namco/system10/namco_system10_memn.h"

#include "core/arcade/arcade_database.h"

#include "common/error.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/sha1_digest.h"
#include "common/string_util.h"

#include <algorithm>
#include <array>
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
  MemNBoardProfile board_profile = MemNBoardProfile::Unknown;
  MemNRawNAND nand0;
  MemNRawNAND nand1;
  std::vector<u8> lookup_ram;
  u16 control_latch = 0;
  u16 nand_device = 0;
};

std::optional<RuntimeState> s_runtime;

// PROVEN/CORROBORATED: Star Trigon ordinary MEM(N) data is stored with a
// pre-permute XOR of 0xAAAA and this recovered 16-bit bit permutation.
// The array lists the source bit for output bits 15 down to 0.
static constexpr std::array<u8, 16> STAR_TRIGON_STATIC_BIT_ORDER = {
  14, 13, 12, 15, 9, 11, 8, 10, 4, 5, 6, 7, 0, 3, 2, 1,
};

constexpr u16 Permute16(u16 value, const std::array<u8, 16>& source_bits)
{
  u16 result = 0;
  for (u32 i = 0; i < source_bits.size(); i++)
  {
    const u32 output_bit = 15 - i;
    result |= static_cast<u16>(((value >> source_bits[i]) & 1u) << output_bit);
  }
  return result;
}

constexpr u16 DecodeStaticWord(MemNBoardProfile profile, u16 raw_word)
{
  switch (profile)
  {
    case MemNBoardProfile::StarTrigon:
      return Permute16(static_cast<u16>(raw_word ^ UINT16_C(0xaaaa)), STAR_TRIGON_STATIC_BIT_ORDER);

    case MemNBoardProfile::Unknown:
    default:
      return raw_word;
  }
}

static_assert(DecodeStaticWord(MemNBoardProfile::StarTrigon, UINT16_C(0xaaaa)) == UINT16_C(0x0000));
static_assert(DecodeStaticWord(MemNBoardProfile::StarTrigon, UINT16_C(0x5555)) == UINT16_C(0xffff));
static_assert(DecodeStaticWord(MemNBoardProfile::StarTrigon, UINT16_C(0x1234)) == UINT16_C(0x7497));

bool ShouldApplyStaticTransform(const RuntimeState& runtime, const MemNRawNAND& nand)
{
  if (!nand.IsArrayReadActive())
    return false;

  // STRONG INFERENCE, preserved explicitly: NAND0 block 0 is the platform
  // remap table and block 1 is the writable/settings area. Reference behavior
  // treats these two platform blocks as plaintext while ordinary storage uses
  // the board's static transform.
  if (runtime.nand_device == 0 && nand.GetCurrentBlock() < 2)
    return false;

  return runtime.board_profile != MemNBoardProfile::Unknown;
}

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

bool LoadRawNANDMember(const char* archive_path, const Arcade::Database::ROMDefinition& rom,
                       std::vector<u8>* data, Error* error)
{
  if (rom.size != MemNRawNAND::RAW_IMAGE_SIZE || rom.offset != 0 || rom.interleave != 1 || rom.group_size != 1 ||
      rom.skip != 0 || rom.word_swap || !rom.segments.empty())
  {
    Error::SetStringFmt(error,
                        "System 10 raw NAND '{}' uses a database layout that would alter the dumped 0x210-byte "
                        "page image.",
                        rom.name);
    return false;
  }

  unzFile zf = MinizipHelpers::OpenUnzFile(archive_path);
  if (!zf)
  {
    Error::SetStringFmt(error, "Failed to open Namco System 10 set archive '{}'.", archive_path);
    return false;
  }

  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    Error::SetStringFmt(error, "Namco System 10 set archive '{}' is empty or unreadable.", archive_path);
    return false;
  }

  for (;;)
  {
    unz_file_info64 file_info = {};
    char member_name[512] = {};
    if (unzGetCurrentFileInfo64(zf, &file_info, member_name, sizeof(member_name), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed to read file information from Namco System 10 set archive '{}'.",
                          archive_path);
      return false;
    }

    member_name[sizeof(member_name) - 1] = '\0';
    if (StringUtil::EqualNoCase(member_name, rom.name))
    {
      if (file_info.uncompressed_size != rom.size)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "System 10 NAND '{}' has size {}, expected {} bytes.", rom.name,
                            file_info.uncompressed_size, rom.size);
        return false;
      }

      if (rom.has_crc32 && static_cast<u32>(file_info.crc) != rom.crc32)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "System 10 NAND '{}' has CRC32 {:08x}, expected {:08x}.", rom.name,
                            static_cast<u32>(file_info.crc), rom.crc32);
        return false;
      }

      if (unzOpenCurrentFile(zf) != UNZ_OK)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Failed to decompress System 10 NAND '{}' from '{}'.", rom.name, archive_path);
        return false;
      }

      data->resize(rom.size);
      size_t read_offset = 0;
      while (read_offset < data->size())
      {
        const int bytes_read =
          unzReadCurrentFile(zf, data->data() + read_offset, static_cast<unsigned>(data->size() - read_offset));
        if (bytes_read <= 0)
        {
          unzCloseCurrentFile(zf);
          unzClose(zf);
          Error::SetStringFmt(error, "Failed reading System 10 NAND '{}' from '{}'.", rom.name, archive_path);
          return false;
        }

        read_offset += static_cast<size_t>(bytes_read);
      }

      const int close_result = unzCloseCurrentFile(zf);
      unzClose(zf);
      if (close_result != UNZ_OK)
      {
        Error::SetStringFmt(error, "CRC validation failed for System 10 NAND '{}' in '{}'.", rom.name, archive_path);
        return false;
      }

      if (!rom.sha1.empty())
      {
        auto digest = SHA1Digest::GetDigest(std::span<const u8>(data->data(), data->size()));
        const std::string digest_string = SHA1Digest::DigestToString(digest);
        if (!StringUtil::EqualNoCase(digest_string, rom.sha1))
        {
          Error::SetStringFmt(error, "System 10 NAND '{}' has SHA-1 {}, expected {}.", rom.name, digest_string,
                              rom.sha1);
          return false;
        }
      }

      return true;
    }

    const int next_result = unzGoToNextFile(zf);
    if (next_result == UNZ_END_OF_LIST_OF_FILE)
      break;
    if (next_result != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed while reading Namco System 10 set archive '{}'.", archive_path);
      return false;
    }
  }

  unzClose(zf);
  Error::SetStringFmt(error, "Namco System 10 set archive '{}' does not contain required NAND '{}'.", archive_path,
                      rom.name);
  return false;
}

} // namespace

std::optional<MemNLoadedContent> LoadStarTrigonContent(const char* archive_path,
                                                      const Arcade::Database::GameDefinition& game, Error* error)
{
  if (game.hardware_profile != "ns10_startrgn")
  {
    Error::SetStringFmt(error, "System 10 S10-01C only supports the Star Trigon MEM(N) profile; requested '{}'.",
                        game.hardware_profile);
    return std::nullopt;
  }

  const Arcade::Database::ROMDefinition* nand0_rom = nullptr;
  const Arcade::Database::ROMDefinition* nand1_rom = nullptr;
  u32 nand_region_count = 0;

  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "nand0")
    {
      nand0_rom = &rom;
      nand_region_count++;
    }
    else if (rom.region == "nand1")
    {
      nand1_rom = &rom;
      nand_region_count++;
    }
    else if (rom.region.size() >= 4 && rom.region.compare(0, 4, "nand") == 0)
    {
      nand_region_count++;
    }
  }

  if (!nand0_rom || !nand1_rom || nand_region_count != 2)
  {
    Error::SetStringView(error, "Star Trigon database entry must contain exactly raw NAND regions nand0 and nand1.");
    return std::nullopt;
  }

  MemNLoadedContent content;
  content.set_name = game.id;
  content.board_profile = MemNBoardProfile::StarTrigon;

  if (!LoadRawNANDMember(archive_path, *nand0_rom, &content.nand0, error) ||
      !LoadRawNANDMember(archive_path, *nand1_rom, &content.nand1, error))
  {
    return std::nullopt;
  }

  VERBOSE_LOG("Loaded Namco System 10 Star Trigon raw NANDs from '{}': nand0={} bytes nand1={} bytes.",
              archive_path, content.nand0.size(), content.nand1.size());
  return content;
}

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

  if (content.board_profile == MemNBoardProfile::Unknown)
  {
    Error::SetStringView(error, "Namco System 10 MEM(N) content has no board profile.");
    return false;
  }

  RuntimeState runtime;
  runtime.set_name = std::move(content.set_name);
  runtime.board_profile = content.board_profile;

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
      // OPEN: exact byte-lane behavior through the 16-bit MEM(N) host data
      // register has not been required by the bootstrap path. Preserve the
      // existing raw x8 diagnostic behavior until software proves otherwise.
      *value = nand->DataRead();
      return true;
    }

    if (width == 2)
    {
      // MEM(N) presents two consecutive 8-bit NAND reads as one 16-bit host word,
      // first NAND byte in the high byte. Static scrambling is a board-layer
      // property, so the raw NAND object itself remains untouched.
      const bool apply_static_transform = ShouldApplyStaticTransform(runtime, *nand);
      const u16 first = nand->DataRead();
      const u16 second = nand->DataRead();
      const u16 raw_word = static_cast<u16>((first << 8) | second);
      *value = apply_static_transform ? DecodeStaticWord(runtime.board_profile, raw_word) : raw_word;
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