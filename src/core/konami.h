// SPDX-FileCopyrightText: 2026 ArcadeDuck Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "types.h"

#include <string_view>

namespace Konami {

enum class GVBIOSProfile : u8
{
  KonamiGV,
};

struct GVGameDefinition
{
  std::string_view set_name;
  std::string_view title;
  GVBIOSProfile bios_profile;
  std::string_view hardware_profile;
};

/// Returns the immutable definition for a case-insensitive exact MAME set-name match.
const GVGameDefinition* GetGVGameDefinition(std::string_view set_name);

/// Returns true when path names a ZIP archive. The extension comparison is case-insensitive.
bool IsGVArchivePath(std::string_view path);

/// Returns the archive basename without its ZIP extension, or an empty view for non-ZIP paths.
std::string_view GetGVSetNameFromArchivePath(std::string_view path);

/// Identifies an exact Konami GV MAME set from a ZIP archive path.
const GVGameDefinition* IdentifyGVArchive(std::string_view path);

bool IsGVSet(std::string_view set_name);
const char* GetGVBIOSProfileName(GVBIOSProfile profile);

} // namespace Konami
