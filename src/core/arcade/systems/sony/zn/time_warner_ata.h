// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <string_view>

class Error;

namespace SonyZN::TimeWarnerATA {

bool Initialize(std::string_view chd_path, Error* error);
void Shutdown();
void Reset();
bool IsActive();


/// PSX DMA channel 5 payload exchange for the Atari PSXTRA/VIA IDE path.
void DMARead(u32* data, u32 word_count);
void DMAWrite(const u32* data, u32 word_count);

/// Offsets are relative to the ZN EXP1 base (physical 0x1F000000).
bool HandlesOffset(u32 offset);
u32 Read(u32 width, u32 offset);
void Write(u32 width, u32 offset, u32 value);

} // namespace SonyZN::TimeWarnerATA
