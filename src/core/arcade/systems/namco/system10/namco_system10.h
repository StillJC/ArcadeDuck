// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <string>
#include <vector>

class Error;

namespace NamcoSystem10 {

struct MemNLoadedContent
{
  std::string set_name;
  std::vector<u8> nand0;
  std::vector<u8> nand1;
};

bool InitializeMemN(MemNLoadedContent content, Error* error);
void Reset();
void Shutdown();
bool IsActive();

/// Handles System 10 devices mapped into the PlayStation EXP1 window.
/// Returns false when the offset is not owned by the active System 10 profile.
bool ReadEXP1(u32 width, u32 offset, u32* value);
bool WriteEXP1(u32 width, u32 offset, u32 value);

} // namespace NamcoSystem10