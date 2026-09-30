// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <optional>
#include <string>
#include <vector>

class Error;

namespace Arcade::Database {
struct GameDefinition;
}

namespace NamcoSystem10 {

enum class MemNBoardProfile : u8
{
  Unknown = 0,
  StarTrigon,
};

struct MemNLoadedContent
{
  std::string set_name;
  MemNBoardProfile board_profile = MemNBoardProfile::Unknown;
  std::vector<u8> nand0;
  std::vector<u8> nand1;
};

/// Loads the first supported MEM(N) bring-up target without transforming the
/// NAND bytes. The raw 0x210-byte page image, including spare/OOB, is retained.
std::optional<MemNLoadedContent> LoadStarTrigonContent(const char* archive_path,
                                                      const Arcade::Database::GameDefinition& game, Error* error);

bool InitializeMemN(MemNLoadedContent content, Error* error);
void Reset();
void Shutdown();
bool IsActive();

/// Handles System 10 devices mapped into the PlayStation EXP1 window.
/// Returns false when the offset is not owned by the active System 10 profile.
bool ReadEXP1(u32 width, u32 offset, u32* value);
bool WriteEXP1(u32 width, u32 offset, u32 value);

} // namespace NamcoSystem10