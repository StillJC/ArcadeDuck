// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <vector>

class Error;

namespace SonyZN::TecmoCBAJSound {

bool Initialize(const std::vector<u8>& sound_program, const std::vector<u8>& ymz280b_rom, Error* error);
void Reset();
void Shutdown();
bool IsActive();

u8 MainDataRead();
void MainDataWrite(u8 value);
u8 MainStatusRead();

void GenerateAudioFrame(s32* left, s32* right);

} // namespace SonyZN::TecmoCBAJSound