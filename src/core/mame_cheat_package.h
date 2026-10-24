// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "mame_cheats.h"

#include <string>
#include <string_view>

class Error;

namespace MAMECheats {

enum class PackageLoadStatus
{
  NotFound,
  Loaded,
  Error,
};

enum class PackageSource
{
  None,
  LooseXML,
  SevenZip,
};

struct PackageLoadInfo
{
  PackageSource source = PackageSource::None;
  std::string source_path;
  std::string matched_shortname;
  bool used_parent = false;
};

// Lookup order is exact loose XML, exact cheat.7z member, then the same two sources for the parent set.
PackageLoadStatus LoadForGame(std::string_view cheats_directory, std::string_view shortname,
                              std::string_view parent_shortname, List* list, PackageLoadInfo* info, Error* error);

} // namespace MAMECheats
