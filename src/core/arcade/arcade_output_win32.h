// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <string_view>

namespace Arcade::Output::Win32Transport {

void Start(std::string_view game_id);
void Stop();
void Notify(std::string_view name, s32 value);

} // namespace Arcade::Output::Win32Transport