// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Namco System 10 experimental skeleton.
//
// Hardware direction:
// - PlayStation-derived main CPU/GPU/SPU platform.
// - Game program/data lives on interchangeable MEM boards.
// - Known MEM-board families use mask/flash or NAND storage; later variants
//   add auxiliary audio/media hardware.
// - Protection/decryption belongs to the MEM-board implementation rather than
//   the shared PlayStation core.
// - Optional EXIO hardware should be modeled separately from the base board.
//
// This branch intentionally starts with a compile-only namespace. Runtime
// integration, media loading, memory mapping, protection and game gating are
// added as individually reviewable milestones.

namespace NamcoSystem10 {

} // namespace NamcoSystem10