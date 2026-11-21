// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <string_view>
#include <vector>

class Error;
class StateWrapper;

namespace Arcade::SaveState {
struct NVRAMComponent;
}

namespace KonamiGQEEPROM {

static constexpr u32 EEPROM_SIZE = 0x80;

bool Initialize(std::string_view set_name, std::string_view persistence_directory, Error* error);
void Reset();
void Shutdown();

bool IsActive();
bool IsSoundCPUReleased();

void WriteControl(u32 value);
u32 ReadDSW(u32 width, u32 offset);

bool DoState(StateWrapper& sw);
void GetNVRAMComponents(std::vector<Arcade::SaveState::NVRAMComponent>* components);

} // namespace KonamiGQEEPROM
