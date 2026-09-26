// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <string_view>

namespace Arcade::Output {

void Reset();
void SetContext(std::string_view game_id, std::string_view system_id);
void SetValue(std::string_view name, s32 value, std::string_view source = {});
void ClearValues();
void Shutdown();
void ResetStatistics();
void DrawDebugWindow();

} // namespace Arcade::Output