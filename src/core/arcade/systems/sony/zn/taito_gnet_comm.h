// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

namespace SonyZN::TaitoGNetComm {

void Initialize(bool supported);
void Reset();
void Shutdown();
void ProcessFrame();

bool IsActive();
bool HandlesEXP1Access(u32 width, u32 offset);
u32 ReadEXP1(u32 width, u32 offset);
bool WriteEXP1(u32 width, u32 offset, u32 value);

} // namespace SonyZN::TaitoGNetComm