// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <vector>

class Error;

namespace SonyZN::AtlusZN1Sound {

bool Initialize(std::vector<u8> sound_program, std::vector<u8> ymz280b_rom, Error* error);
void Reset();
void Shutdown();
bool IsActive();

void MainCommandWrite(u16 value);
void GenerateAudioFrame(s32* left, s32* right);

} // namespace SonyZN::AtlusZN1Sound