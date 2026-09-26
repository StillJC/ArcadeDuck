// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/devices/storage/chd_hard_disk.h"

#include <string_view>

class Error;

namespace KonamiGQHDD {

using Geometry = Arcade::Storage::CHDHardDisk::Geometry;

bool Initialize(const char* path, Error* error);
void Shutdown();
bool IsOpen();

std::string_view GetPath();
const Geometry& GetGeometry();
u32 GetBlockCount();

bool ReadSector(u32 lba, u8* buffer);
bool WriteSector(u32 lba, const u8* buffer);
void ClearWriteOverlay();

} // namespace KonamiGQHDD
