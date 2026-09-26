// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <memory>
#include <string_view>

class Error;

namespace Arcade::Storage {

class CHDHardDisk
{
public:
  struct Geometry
  {
    u32 cylinders = 0;
    u32 heads = 0;
    u32 sectors = 0;
    u32 bytes_per_sector = 0;
  };

  CHDHardDisk();
  ~CHDHardDisk();

  CHDHardDisk(const CHDHardDisk&) = delete;
  CHDHardDisk& operator=(const CHDHardDisk&) = delete;
  CHDHardDisk(CHDHardDisk&& other) noexcept;
  CHDHardDisk& operator=(CHDHardDisk&& other) noexcept;

  bool Open(const char* path, Error* error);
  void Close();
  bool IsOpen() const;

  std::string_view GetPath() const;
  const Geometry& GetGeometry() const;
  u32 GetBlockCount() const;

  bool ReadSector(u32 lba, u8* buffer);
  bool WriteSector(u32 lba, const u8* buffer);
  void ClearWriteOverlay();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace Arcade::Storage
