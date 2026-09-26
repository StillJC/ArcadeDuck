// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once
#include "types.h"
#include <memory>
#include <string>
#include <tuple>

class CDImage;
class StateWrapper;

namespace CDROM {

void Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);

bool HasMedia();
const std::string& GetMediaFileName();
const CDImage* GetMedia();
DiscRegion GetDiscRegion();
bool IsMediaPS1Disc();
bool IsMediaAudioCD();
bool DoesMediaRegionMatchConsole();

void InsertMedia(std::unique_ptr<CDImage> media, DiscRegion region);
std::unique_ptr<CDImage> RemoveMedia(bool for_disc_swap);
bool PrecacheMedia();

void CPUClockChanged();

// I/O
u8 ReadRegister(u32 offset);
void WriteRegister(u32 offset, u8 value);
void DMARead(u32* words, u32 word_count);

// Render statistics debug window.
void DrawDebugWindow();

void SetReadaheadSectors(u32 readahead_sectors);

/// Reads a frame from the audio FIFO, used by the SPU.
std::tuple<s16, s16> GetAudioFrame();

/// Supplies arcade hardware CDDA frames to the independent external CDDA FIFO.
void PushExternalCDAudioFrames(const u32* frames, u32 frame_count);

/// Clears all externally supplied CDDA frames.
void ClearExternalCDAudioFrames();

/// Reads one externally supplied stereo CDDA frame.
std::tuple<s16, s16> GetExternalCDAudioFrame();

} // namespace CDROM
