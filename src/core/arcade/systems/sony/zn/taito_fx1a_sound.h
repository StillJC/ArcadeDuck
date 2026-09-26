// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <vector>

class Error;

namespace SonyZN::TaitoFX1ASound {

bool Initialize(const std::vector<u8>& sound_program, const std::vector<u8>& adpcma_rom, Error* error);
void Reset();
void Shutdown();
bool IsActive();

void MasterPortWrite(u8 value);
u8 MasterCommRead();
void MasterCommWrite(u8 value);

void GenerateAudioFrame(s32* left, s32* right);

} // namespace SonyZN::TaitoFX1ASound
