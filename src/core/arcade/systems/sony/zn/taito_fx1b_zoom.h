// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <span>

namespace TaitoFX1BZoom {

bool Initialize(std::span<const u8> mn10200_rom, std::span<const u8> zsg2_rom, std::span<u8> shared_ram);
void Reset();
void Shutdown();
bool IsActive();

void RegAddressWrite(u16 data);
void RegDataWrite(u16 data);
u16 StatusRead();
void PulseMainIRQ();

u8 SharedRead(u32 index);
void SharedWrite(u32 index, u8 value);

void GenerateAudioFrame(s32* left, s32* right);

} // namespace TaitoFX1BZoom
