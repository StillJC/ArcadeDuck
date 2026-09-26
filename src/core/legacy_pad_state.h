// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

class StateWrapper;

namespace LegacyPadState {

// Consumes legacy controller, multitap, memory-card, and Pad register payloads.
// New states retain an empty legacy-shaped section so state stream ordering remains stable.
bool DoState(StateWrapper& sw);

} // namespace LegacyPadState
