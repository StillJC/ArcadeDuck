// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

namespace KonamiGQPCM {

inline constexpr u32 RAM_SIZE = 0x180000;

void Initialize();
void Reset();
void Shutdown();
bool IsActive();

u32 Read(u32 width, u32 offset);
void Write(u32 width, u32 offset, u32 value);

/// Reads one byte from the contiguous 1.5 MiB shared PCM RAM backing store.
u8 ReadRAMByte(u32 index);

} // namespace KonamiGQPCM
