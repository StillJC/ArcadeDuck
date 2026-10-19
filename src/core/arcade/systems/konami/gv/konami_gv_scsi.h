// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/arcade/devices/storage/ncr53cf96.h"

class StateWrapper;

namespace KonamiGVScsi {

using DeferredStopReason = NCR53CF96::DeferredStopReason;

void Initialize();
void Reset();
void Shutdown();
bool IsActive();
bool DoState(StateWrapper& sw);

u32 ReadRegister(u32 width, u32 offset);
void WriteRegister(u32 width, u32 offset, u32 value, u32 pc);

/// Returns and clears the pending deferred-stop reason.
DeferredStopReason ConsumeDeferredStopRequest();
u8 GetActiveCommand();
u8 GetTargetCommandOpcode();

/// DMA channel 5 payload exchange for the active Konami GV NCR53CF96 Data In path.
void DMARead(u32* data, u32 word_count);
void DMAWrite(const u32* data, u32 word_count);

} // namespace KonamiGVScsi
