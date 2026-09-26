// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once

// Compatibility shim for DMA's unchanged timing query. The consumer Pad runtime is removed.
namespace Pad {
constexpr bool IsTransmitting()
{
  return false;
}
} // namespace Pad
