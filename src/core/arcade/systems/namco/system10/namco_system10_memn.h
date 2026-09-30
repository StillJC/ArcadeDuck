// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <span>
#include <vector>

class Error;

namespace NamcoSystem10 {

// Raw Samsung K9F2808U0B image used by MEM(N).
//
// The dumped image includes the complete 16-byte spare/OOB area for every
// 512-byte data page. Keep the image raw here; firmware-visible logical block
// mapping and MEM(N) protection are separate layers.
class MemNRawNAND
{
public:
  static constexpr u32 DATA_BYTES_PER_PAGE = 0x200;
  static constexpr u32 SPARE_BYTES_PER_PAGE = 0x10;
  static constexpr u32 RAW_PAGE_SIZE = DATA_BYTES_PER_PAGE + SPARE_BYTES_PER_PAGE;
  static constexpr u32 PAGES_PER_BLOCK = 32;
  static constexpr u32 BLOCK_COUNT = 1024;
  static constexpr u32 RAW_IMAGE_SIZE = RAW_PAGE_SIZE * PAGES_PER_BLOCK * BLOCK_COUNT;

  bool Load(std::vector<u8> raw_image, Error* error);

  bool IsLoaded() const { return m_raw_image.size() == RAW_IMAGE_SIZE; }
  std::span<const u8> GetRawImage() const
  {
    return std::span<const u8>(m_raw_image.data(), m_raw_image.size());
  }
  std::span<const u8> GetRawPage(u32 page) const;
  std::span<const u8> GetPageData(u32 page) const;
  std::span<const u8> GetPageSpare(u32 page) const;

private:
  std::vector<u8> m_raw_image;
};

static_assert(MemNRawNAND::RAW_PAGE_SIZE == 0x210);
static_assert(MemNRawNAND::RAW_IMAGE_SIZE == 0x1080000);

} // namespace NamcoSystem10