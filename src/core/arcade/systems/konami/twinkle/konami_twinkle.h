// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Konami Twinkle experimental skeleton.
//
// Hardware direction:
// - PlayStation-derived main CPU/GPU/SPU board.
// - Separate video/overlay hardware supports external video playback.
// - Separate audio board uses its own CPU and PCM hardware.
// - Program media uses SCSI CD-ROM; known configurations also use IDE HDD
//   storage for audio data.
// - Security, external video control and cabinet I/O should remain separate
//   from the shared PlayStation-derived core.
//
// This branch intentionally starts with a compile-only namespace. Runtime
// integration, media loading, video mixing, audio, security, I/O and game
// gating should be added as individually verified and reviewable milestones.

namespace KonamiTwinkle {

} // namespace KonamiTwinkle
