// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

class StateWrapper;
struct z80;

namespace Arcade::Z80State {

/// Serializes a superzazu Z80 core's registers, flags and interrupt state. Callbacks and userdata are kept.
bool DoState(StateWrapper& sw, z80& cpu);

} // namespace Arcade::Z80State
