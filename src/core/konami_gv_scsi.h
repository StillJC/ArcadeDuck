// SPDX-FileCopyrightText: 2026 ArcadeDuck Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "types.h"

class StateWrapper;

namespace KonamiGVScsi {

enum class MigrationStopReason : u8
{
  None,
  FirstCDB,
  UnsupportedControllerCommand,
  IncompleteCDB,
  UnsupportedTargetCommand,
};

void Initialize();
void Reset();
void Shutdown();
bool IsActive();
bool DoState(StateWrapper& sw);

u32 ReadRegister(u32 width, u32 offset);
void WriteRegister(u32 width, u32 offset, u32 value, u32 pc);

/// Returns the Stage 3A boundary reason once after it is requested.
MigrationStopReason ConsumeMigrationStopRequest();
u8 GetActiveCommand();
u8 GetTargetCommandOpcode();

} // namespace KonamiGVScsi
