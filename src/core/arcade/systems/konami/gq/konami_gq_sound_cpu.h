// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <vector>

namespace KonamiGQSoundCPU {

bool Initialize(const std::vector<u8>& sound_program, const std::vector<u8>& pcm_samples);
void Reset();
void Shutdown();
bool IsActive();
void GenerateAudioFrame(s32* left, s32* right);

void SetResetReleased(bool released);
void SynchronizeAfterHostWrite();
void SynchronizeBeforeHostRead();

} // namespace KonamiGQSoundCPU
