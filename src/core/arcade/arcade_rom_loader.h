// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/arcade_database.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

class Error;

namespace Arcade {

void ClearROMArchiveCache();

bool LoadROMFromArchive(std::string_view archive_path, const Database::ROMDefinition& rom, std::vector<u8>* data,
                        Error* error);
bool LoadROMFromArchives(std::span<const std::string> archive_paths, const Database::ROMDefinition& rom,
                         std::vector<u8>* data, Error* error);
bool LoadGameROM(std::string_view archive_path, const Database::GameDefinition& game,
                 const Database::ROMDefinition& rom, std::vector<u8>* data, Error* error);
bool CanResolveGameROMs(std::string_view archive_path, const Database::GameDefinition& game);

} // namespace Arcade
