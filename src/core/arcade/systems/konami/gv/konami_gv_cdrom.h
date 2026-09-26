// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "core/types.h"

namespace KonamiGVCDROM {
void Initialize();
void Reset();
void Shutdown();
u32 GetLBACount();
bool ReadDataSector(u32 lba, u8* sector);
u32 ReadTOC(const u8* cdb, u8* response, u32 response_size);
u32 ReadSubChannel(const u8* cdb, u8* response, u32 response_size);
bool PlayAudioTrackIndex(u8 start_track, u8 start_index, u8 end_track, u8 end_index);
void PauseAudio(bool resume);
void SetAudioOutput(u8 output, u8 channel, u8 volume);
bool GetAudioOutput(u8 output, u8* channel, u8* volume);
void MixAudioFrame(s16 input_left, s16 input_right, s32* output_left, s32* output_right);
}
