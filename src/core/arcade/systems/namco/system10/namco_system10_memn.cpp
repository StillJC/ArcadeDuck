// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system10/namco_system10_memn.h"

#include "common/error.h"

#include <iterator>

#include <utility>

namespace NamcoSystem10 {

bool MemNRawNAND::Load(std::vector<u8> raw_image, Error* error)
{
  if (raw_image.size() != RAW_IMAGE_SIZE)
  {
    Error::SetStringFmt(error, "Namco System 10 MEM(N) NAND image has invalid size {} (expected {}).",
                        raw_image.size(), RAW_IMAGE_SIZE);
    return false;
  }

  m_raw_image = std::move(raw_image);
  ResetInterface();
  return true;
}

void MemNRawNAND::ResetInterface()
{
  m_read_mode = ReadMode::None;
  m_pointer_command = 0x00;
  m_column_address = 0;
  m_page_address = 0;
  m_serial_index = 0;
  m_id_address_valid = false;
}

void MemNRawNAND::CommandWrite(u8 command)
{
  m_serial_index = 0;

  switch (command)
  {
    case 0x00:
    case 0x01:
    case 0x50:
      m_pointer_command = command;
      m_read_mode = ReadMode::Array;
      break;

    case 0x70:
      m_read_mode = ReadMode::Status;
      break;

    case 0x90:
      m_read_mode = ReadMode::Id;
      m_id_address_valid = false;
      break;

    case 0xff:
      ResetInterface();
      break;

    default:
      // S10-01B is intentionally read-side only. Program/erase commands are
      // left unimplemented until a title demonstrates that they are required.
      m_read_mode = ReadMode::None;
      break;
  }
}

void MemNRawNAND::AddressColumnWrite(u8 value)
{
  m_serial_index = 0;

  if (m_read_mode == ReadMode::Id)
  {
    // K9F2808U0B READ ID requires a single 00h address cycle.
    m_id_address_valid = (value == 0x00);
    return;
  }

  if (m_read_mode != ReadMode::Array)
    return;

  if (m_pointer_command == 0x01)
    m_column_address = static_cast<u16>(0x100u + value);
  else if (m_pointer_command == 0x50)
    m_column_address = static_cast<u16>(DATA_BYTES_PER_PAGE + (value & 0x0fu));
  else
    m_column_address = value;
}

void MemNRawNAND::AddressRowLowWrite(u8 value)
{
  if (m_read_mode != ReadMode::Array)
    return;

  m_page_address = (m_page_address & UINT32_C(0x7f00)) | value;
}

void MemNRawNAND::AddressRowHighWrite(u8 value)
{
  if (m_read_mode != ReadMode::Array)
    return;

  // The K9F2808U0B has 32K pages. The third address cycle supplies A17-A23.
  m_page_address = (m_page_address & UINT32_C(0x00ff)) | (static_cast<u32>(value & 0x7fu) << 8);
}

u8 MemNRawNAND::DataRead()
{
  switch (m_read_mode)
  {
    case ReadMode::Array:
      return ReadArrayByte();

    case ReadMode::Id:
    {
      if (!m_id_address_valid)
        return UINT8_C(0xff);

      // Samsung manufacturer code, K9F2808U0B device code.
      static constexpr u8 id_bytes[] = {UINT8_C(0xec), UINT8_C(0x73)};
      const u8 value = (m_serial_index < std::size(id_bytes)) ? id_bytes[m_serial_index] : UINT8_C(0xff);
      m_serial_index++;
      return value;
    }

    case ReadMode::Status:
      // Read-side implementation is always ready, successful, and not write-protected.
      return UINT8_C(0xc0);

    case ReadMode::None:
    default:
      return UINT8_C(0xff);
  }
}

u8 MemNRawNAND::ReadArrayByte()
{
  if (!IsLoaded() || m_page_address >= (PAGES_PER_BLOCK * BLOCK_COUNT) || m_column_address >= RAW_PAGE_SIZE)
    return UINT8_C(0xff);

  const std::span<const u8> page = GetRawPage(m_page_address);
  const u8 value = page[m_column_address];
  AdvanceArrayPointer();
  return value;
}

void MemNRawNAND::AdvanceArrayPointer()
{
  m_column_address++;
  if (m_column_address < RAW_PAGE_SIZE)
    return;

  // K9F2808U0B supports sequential row read within one block. Crossing a block
  // requires a fresh command/address sequence.
  if ((m_page_address % PAGES_PER_BLOCK) == (PAGES_PER_BLOCK - 1))
  {
    m_read_mode = ReadMode::None;
    return;
  }

  m_page_address++;

  if (m_pointer_command == 0x50)
  {
    // READ2 remains on the spare area for each sequential page.
    m_column_address = DATA_BYTES_PER_PAGE;
  }
  else
  {
    // After a 01h (second-half) read the next sequential page starts in the
    // first half, matching the documented pointer behavior.
    m_pointer_command = 0x00;
    m_column_address = 0;
  }
}

std::span<const u8> MemNRawNAND::GetRawPage(u32 page) const
{
  if (!IsLoaded() || page >= (PAGES_PER_BLOCK * BLOCK_COUNT))
    return {};

  const size_t offset = static_cast<size_t>(page) * RAW_PAGE_SIZE;
  return std::span<const u8>(m_raw_image.data(), m_raw_image.size()).subspan(offset, RAW_PAGE_SIZE);
}

std::span<const u8> MemNRawNAND::GetPageData(u32 page) const
{
  const std::span<const u8> raw_page = GetRawPage(page);
  return raw_page.empty() ? std::span<const u8>() : raw_page.first(DATA_BYTES_PER_PAGE);
}

std::span<const u8> MemNRawNAND::GetPageSpare(u32 page) const
{
  const std::span<const u8> raw_page = GetRawPage(page);
  return raw_page.empty() ? std::span<const u8>() : raw_page.subspan(DATA_BYTES_PER_PAGE, SPARE_BYTES_PER_PAGE);
}

} // namespace NamcoSystem10