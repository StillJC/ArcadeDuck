// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <string_view>

class Error;

namespace SonyZN::AcclaimATA {

bool Initialize(std::string_view chd_path, Error* error);
void Shutdown();
void Reset();
bool IsActive();

/// PSX DMA channel 5 payload exchange for the active Judge Dredd ATA bridge.
void DMARead(u32* data, u32 word_count);
void DMAWrite(const u32* data, u32 word_count);

bool HandlesOffset(u32 offset);
u32 Read(u32 width, u32 offset);
void Write(u32 width, u32 offset, u32 value);

} // namespace SonyZN::AcclaimATA
