// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <vector>

class Error;

namespace SonyZN::TecmoGR2Link {

bool Initialize(const std::vector<u8>& link_program, Error* error);
void Reset();
void Shutdown();
bool IsActive();

void ProcessFrame();

u8 MainDataRead();
void MainDataWrite(u8 value);
void MainIRQWrite(u8 value);
u8 MainStatusRead();

} // namespace SonyZN::TecmoGR2Link
