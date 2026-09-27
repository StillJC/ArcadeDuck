// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Konami System 573 experimental skeleton.
//
// Hardware direction:
// - PlayStation-derived main CPU/GPU/SPU platform.
// - Software/media commonly uses CD-ROM and onboard flash storage.
// - Security, I/O and peripheral configurations vary significantly by title.
// - Base-board behavior, media/storage, security and optional I/O should remain
//   separated as implementation is added.
//
// This branch intentionally starts with a compile-only namespace. Runtime
// integration, media loading, memory mapping, security, I/O and game gating
// should be added as individually verified and reviewable milestones.

namespace KonamiSystem573 {

} // namespace KonamiSystem573
