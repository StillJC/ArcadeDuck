// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

class StateWrapper;
struct h8_system_t;

namespace Arcade::H8 {

/// Serializes a libh8300h core: registers, CCR, PC, the fetched instruction, the 64 KiB internal address space,
/// the fault/cycle state, the IR buffers and SLEEP. Bus/I/O callbacks, pin and device tables and the instruction-
/// fetch mapping are host configuration and are kept.
bool DoCoreState(StateWrapper& sw, h8_system_t& cpu);

} // namespace Arcade::H8
