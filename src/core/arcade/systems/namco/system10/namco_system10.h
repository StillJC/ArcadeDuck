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

} // namespace NamcoSystem10