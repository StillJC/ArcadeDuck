// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system10/namco_system10_memn.h"

#include "common/error.h"

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
  return true;
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