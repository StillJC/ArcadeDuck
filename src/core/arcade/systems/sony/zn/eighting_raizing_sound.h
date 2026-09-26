// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <vector>

class Error;

namespace SonyZN::EightingRaizingSound {

bool Initialize(std::vector<u8> sound_program, std::vector<u8> ymf271_rom, Error* error);
void Reset();
void Shutdown();
bool IsActive();

void MainCommandWrite(u8 value);
void MainIRQWrite();
void GenerateAudioFrame(s32* left, s32* right);

} // namespace SonyZN::EightingRaizingSound
