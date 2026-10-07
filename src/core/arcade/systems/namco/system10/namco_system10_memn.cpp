// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system10/namco_system10_memn.h"

#include "common/error.h"

#include <algorithm>
#include <iterator>

#include <utility>

namespace NamcoSystem10 {

bool MemNRawNAND::Load(std::vector<u8> raw_image, Error* error)
{
  if (raw_image.size() == K9F2808_RAW_IMAGE_SIZE)
  {
    m_block_count = K9F2808_BLOCK_COUNT;
    m_device_code = UINT8_C(0x73);
    m_row_high_mask = UINT8_C(0x7f);
  }
  else if (raw_image.size() == K9F5608_RAW_IMAGE_SIZE)
  {
    m_block_count = K9F5608_BLOCK_COUNT;
    m_device_code = UINT8_C(0x75);
    m_row_high_mask = UINT8_C(0xff);
  }
  else
  {
    Error::SetStringFmt(error,
                        "Namco System 10 MEM(N) NAND image has invalid size {} (expected {} or {}).",
                        raw_image.size(), K9F2808_RAW_IMAGE_SIZE, K9F5608_RAW_IMAGE_SIZE);
    return false;
  }

  m_raw_image = std::move(raw_image);
  m_dirty = false;
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
  m_write_mode = WriteMode::None;
  m_program_buffer.fill(UINT8_C(0xff));
}

void MemNRawNAND::CommandWrite(u8 command)
{
  m_serial_index = 0;

  switch (command)
  {
    case 0x00:
    case 0x01:
    case 0x50:
      m_write_mode = WriteMode::None;
      m_pointer_command = command;
      m_read_mode = ReadMode::Array;
      break;

    case 0x70:
      m_write_mode = WriteMode::None;
      m_read_mode = ReadMode::Status;
      break;

    case 0x80:
      m_read_mode = ReadMode::None;
      m_write_mode = WriteMode::Program;
      m_column_address = 0;
      m_page_address = 0;
      m_program_buffer.fill(UINT8_C(0xff));
      break;

    case 0x10:
      if (m_write_mode == WriteMode::Program)
        CommitProgram();
      m_write_mode = WriteMode::None;
      m_read_mode = ReadMode::None;
      break;

    case 0x60:
      m_read_mode = ReadMode::None;
      m_write_mode = WriteMode::Erase;
      m_page_address = 0;
      break;

    case 0xd0:
      if (m_write_mode == WriteMode::Erase)
        CommitErase();
      m_write_mode = WriteMode::None;
      m_read_mode = ReadMode::None;
      break;

    case 0x90:
      m_write_mode = WriteMode::None;
      m_read_mode = ReadMode::Id;
      m_id_address_valid = false;
      break;

    case 0xff:
      ResetInterface();
      break;

    default:
      m_write_mode = WriteMode::None;
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

  if (m_write_mode == WriteMode::Program)
  {
    if (m_pointer_command == 0x01)
      m_column_address = static_cast<u16>(0x100u + value);
    else if (m_pointer_command == 0x50)
      m_column_address = static_cast<u16>(DATA_BYTES_PER_PAGE + (value & 0x0fu));
    else
      m_column_address = value;
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
  if (m_read_mode != ReadMode::Array && m_write_mode == WriteMode::None)
    return;

  m_page_address = (m_page_address & UINT32_C(0x7f00)) | value;
}

void MemNRawNAND::AddressRowHighWrite(u8 value)
{
  if (m_read_mode != ReadMode::Array && m_write_mode == WriteMode::None)
    return;

  // K9F2808U0B uses A17-A23 here; K9F5608U0D extends the same cycle
  // through A24. The device geometry selected at Load() supplies the mask.
  m_page_address = (m_page_address & UINT32_C(0x00ff)) |
                   (static_cast<u32>(value & m_row_high_mask) << 8);
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

      // Samsung manufacturer code followed by the geometry-specific device code.
      const u8 id_bytes[] = {UINT8_C(0xec), m_device_code};
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


void MemNRawNAND::DataWrite(u8 value)
{
  if (m_write_mode != WriteMode::Program || m_column_address >= RAW_PAGE_SIZE)
    return;

  // NAND page programming can only clear bits. Buffer the complete transaction
  // and apply it only after the 10h program-confirm command.
  m_program_buffer[m_column_address] &= value;
  m_column_address++;
}

void MemNRawNAND::CommitProgram()
{
  if (!IsLoaded() || m_page_address >= GetPageCount())
    return;

  const size_t page_offset = static_cast<size_t>(m_page_address) * RAW_PAGE_SIZE;
  for (size_t index = 0; index < RAW_PAGE_SIZE; index++)
  {
    const u8 current = m_raw_image[page_offset + index];
    const u8 programmed = static_cast<u8>(current & m_program_buffer[index]);
    if (programmed != current)
    {
      m_raw_image[page_offset + index] = programmed;
      m_dirty = true;
    }
  }

  // The small-page NAND pointer for the second 256-byte area resets to the
  // first area after every program operation. The spare-area pointer remains.
  if (m_pointer_command == 0x01)
    m_pointer_command = 0x00;
}

void MemNRawNAND::CommitErase()
{
  if (!IsLoaded() || m_page_address >= GetPageCount())
    return;

  const u32 block = m_page_address / PAGES_PER_BLOCK;
  const size_t block_offset = static_cast<size_t>(block) * PAGES_PER_BLOCK * RAW_PAGE_SIZE;
  const size_t block_size = static_cast<size_t>(PAGES_PER_BLOCK) * RAW_PAGE_SIZE;
  const auto first = m_raw_image.begin() + static_cast<std::ptrdiff_t>(block_offset);
  const auto last = first + static_cast<std::ptrdiff_t>(block_size);
  if (std::any_of(first, last, [](u8 value) { return value != UINT8_C(0xff); }))
  {
    std::fill(first, last, UINT8_C(0xff));
    m_dirty = true;
  }
}

u8 MemNRawNAND::ReadArrayByte()
{
  if (!IsLoaded() || m_page_address >= GetPageCount() || m_column_address >= RAW_PAGE_SIZE)
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
  if (!IsLoaded() || page >= GetPageCount())
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