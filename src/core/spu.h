// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "types.h"

#include <array>

class StateWrapper;

class AudioStream;

namespace SPU {

enum : u32
{
  RAM_SIZE = 512 * 1024,
  RAM_MASK = RAM_SIZE - 1,
  SAMPLE_RATE = 44100,
};

/// Selects the board SPU base-frequency ratio relative to the ordinary
/// 44.1 kHz PlayStation SPU clock domain. Host output remains 44.1 kHz.
void SetHardwareClockRatio(u32 numerator, u32 denominator);
void Initialize();
void CPUClockChanged();
void RestoreLegacyClockDomainFromState(bool saved_overclock_active, u32 saved_overclock_numerator,
                                       u32 saved_overclock_denominator);
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);

u16 ReadRegister(u32 offset);
void WriteRegister(u32 offset, u16 value);

void DMARead(u32* words, u32 word_count);
void DMAWrite(const u32* words, u32 word_count);

// Render statistics debug window.
void DrawDebugStateWindow();

// Executes the SPU, generating any pending samples.
void GeneratePendingSamples();

/// Access to SPU RAM.
const std::array<u8, RAM_SIZE>& GetRAM();
std::array<u8, RAM_SIZE>& GetWritableRAM();

/// Change output stream - used for runahead.
// TODO: Make it use system "running ahead" flag
bool IsAudioOutputMuted();
void SetAudioOutputMuted(bool muted);

AudioStream* GetOutputStream();
void RecreateOutputStream();

}; // namespace SPU
