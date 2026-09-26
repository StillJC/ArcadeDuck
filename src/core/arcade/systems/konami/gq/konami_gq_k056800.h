// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

namespace KonamiGQK056800 {

void Initialize();
void Reset();
void Shutdown();
bool IsActive();

u32 ReadHost(u32 width, u32 offset);
void WriteHost(u32 width, u32 offset, u32 value, u32 pc);

u8 ReadSound(u32 offset);
void WriteSound(u32 offset, u8 value);

bool IsSoundInterruptPending();

// Raw 058800 front/rear control state.
u8 GetFrontVolume();
u8 GetRearVolume();

// Crypt Killer programs the front and rear counters identically. This returns
// the last position at which both counters agree, allowing their common-mode
// attenuation to be applied without guessing the unresolved front/rear fold.
u8 GetCommonVolume();

u8 GetOutputControl();

} // namespace KonamiGQK056800
