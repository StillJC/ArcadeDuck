// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>
#include <span>
#include <vector>

class Error;

class StateWrapper;

namespace NamcoSystem10 {

// Raw Samsung K9F2808U0B image used by MEM(N).
//
// The dumped image includes the complete 16-byte spare/OOB area for every
// 512-byte data page. Keep the image raw here; firmware-visible logical block
// mapping and MEM(N) protection are separate layers.
class MemNRawNAND
{
public:
  // Command state only; the array contents are NVRAM matched by fingerprint. Program and erase complete
  // immediately, so a restored command mode only affects later writes.
  bool DoState(StateWrapper& sw);

  static constexpr u32 DATA_BYTES_PER_PAGE = 0x200;
  static constexpr u32 SPARE_BYTES_PER_PAGE = 0x10;
  static constexpr u32 RAW_PAGE_SIZE = DATA_BYTES_PER_PAGE + SPARE_BYTES_PER_PAGE;
  static constexpr u32 PAGES_PER_BLOCK = 32;
  static constexpr u32 K9F2808_BLOCK_COUNT = 1024;
  static constexpr u32 K9F5608_BLOCK_COUNT = 2048;
  static constexpr u32 K9F2808_RAW_IMAGE_SIZE = RAW_PAGE_SIZE * PAGES_PER_BLOCK * K9F2808_BLOCK_COUNT;
  static constexpr u32 K9F5608_RAW_IMAGE_SIZE = RAW_PAGE_SIZE * PAGES_PER_BLOCK * K9F5608_BLOCK_COUNT;

  bool Load(std::vector<u8> raw_image, Error* error);

  void ResetInterface();
  void CommandWrite(u8 command);
  void AddressColumnWrite(u8 value);
  void AddressRowLowWrite(u8 value);
  void AddressRowHighWrite(u8 value);
  u8 DataRead();
  void DataWrite(u8 value);

  bool IsReady() const { return true; }
  bool IsDirty() const { return m_dirty; }
  void ClearDirty() { m_dirty = false; }
  bool IsArrayReadActive() const { return m_read_mode == ReadMode::Array; }
  u32 GetCurrentPageAddress() const { return m_page_address; }
  u32 GetCurrentColumnAddress() const { return m_column_address; }
  u32 GetCurrentBlock() const { return m_page_address / PAGES_PER_BLOCK; }
  u32 GetBlockCount() const { return m_block_count; }
  bool IsLoaded() const
  {
    return m_block_count != 0 &&
           m_raw_image.size() == (static_cast<size_t>(RAW_PAGE_SIZE) * PAGES_PER_BLOCK * m_block_count);
  }
  std::span<const u8> GetRawImage() const
  {
    return std::span<const u8>(m_raw_image.data(), m_raw_image.size());
  }
  std::span<const u8> GetRawPage(u32 page) const;
  std::span<const u8> GetPageData(u32 page) const;
  std::span<const u8> GetPageSpare(u32 page) const;

private:
  enum class ReadMode : u8
  {
    None,
    Array,
    Id,
    Status,
  };

  enum class WriteMode : u8
  {
    None,
    Program,
    Erase,
  };

  u8 ReadArrayByte();
  void AdvanceArrayPointer();
  void CommitProgram();
  void CommitErase();
  u32 GetPageCount() const { return PAGES_PER_BLOCK * m_block_count; }

  std::vector<u8> m_raw_image;
  ReadMode m_read_mode = ReadMode::None;
  u8 m_pointer_command = 0x00;
  u16 m_column_address = 0;
  u32 m_page_address = 0;
  u32 m_serial_index = 0;
  bool m_id_address_valid = false;
  WriteMode m_write_mode = WriteMode::None;
  std::array<u8, RAW_PAGE_SIZE> m_program_buffer = {};
  u32 m_block_count = 0;
  u8 m_device_code = 0xff;
  u8 m_row_high_mask = 0;
  bool m_dirty = false;
};

static_assert(MemNRawNAND::RAW_PAGE_SIZE == 0x210);
static_assert(MemNRawNAND::K9F2808_RAW_IMAGE_SIZE == 0x1080000);
static_assert(MemNRawNAND::K9F5608_RAW_IMAGE_SIZE == 0x2100000);

} // namespace NamcoSystem10