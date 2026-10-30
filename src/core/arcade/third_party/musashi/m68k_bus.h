// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

class StateWrapper;

namespace MusashiBus {

struct Callbacks
{
  u32 (*read8)(u32 address) = nullptr;
  u32 (*read16)(u32 address) = nullptr;
  u32 (*read32)(u32 address) = nullptr;
  void (*write8)(u32 address, u32 value) = nullptr;
  void (*write16)(u32 address, u32 value) = nullptr;
  void (*write32)(u32 address, u32 value) = nullptr;
};

void SetCallbacks(const Callbacks& callbacks);
void ClearCallbacks();

/// Serializes the global Musashi CPU context and its execution counters. Callbacks are not saved.
bool DoCPUState(StateWrapper& sw);

} // namespace MusashiBus