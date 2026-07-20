// SPDX-FileCopyrightText: 2026 ArcadeDuck Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "konami.h"

#include "common/path.h"
#include "common/string_util.h"

#include <array>

namespace Konami {

namespace {

// Source authority: simpsons-bowling-duckstation dc6720ae7:data/database/gamedb.json.
// Keep this table limited to identity and boot-plumbing data. Device behavior belongs to later commits.
constexpr std::array<GVGameDefinition, 14> s_gv_game_definitions = {{
  {"powyak96", "Jikkyou Powerful Pro Yakyuu '96", GVBIOSProfile::KonamiGV, "konamigv_standard"},
  {"hyperath", "Hyper Athlete", GVBIOSProfile::KonamiGV, "konamigv_standard"},
  {"lacrazyc", "Let's Attack Crazy Cross", GVBIOSProfile::KonamiGV, "konamigv_standard"},
  {"susume", "Susume! Taisen Puzzle-Dama", GVBIOSProfile::KonamiGV, "konamigv_standard"},
  {"btchamp", "Beat the Champ", GVBIOSProfile::KonamiGV, "konamigv_btchamp"},
  {"kdeadeye", "Dead Eye", GVBIOSProfile::KonamiGV, "konamigv_kdeadeye"},
  {"weddingr", "Wedding Rhapsody", GVBIOSProfile::KonamiGV, "konamigv_standard"},
  {"tmosh", "Tokimeki Memorial Oshiete Your Heart", GVBIOSProfile::KonamiGV, "konamigv_tokimeki"},
  {"tmoshs", "Tokimeki Memorial Oshiete Your Heart Seal Version", GVBIOSProfile::KonamiGV, "konamigv_tokimeki"},
  {"tmoshsp", "Tokimeki Memorial Oshiete Your Heart Seal Version Plus", GVBIOSProfile::KonamiGV, "konamigv_tokimeki"},
  {"tmoshspa", "Tokimeki Memorial Oshiete Your Heart Seal Version Plus", GVBIOSProfile::KonamiGV, "konamigv_tokimeki"},
  {"nagano98", "Nagano Winter Olympics '98", GVBIOSProfile::KonamiGV, "konamigv_standard"},
  {"naganoj", "Hyper Olympics in Nagano", GVBIOSProfile::KonamiGV, "konamigv_standard"},
  {"simpbowl", "The Simpsons Bowling", GVBIOSProfile::KonamiGV, "konamigv_simpbowl"},
}};

} // namespace

const GVGameDefinition* GetGVGameDefinition(std::string_view set_name)
{
  for (const GVGameDefinition& definition : s_gv_game_definitions)
  {
    if (StringUtil::EqualNoCase(definition.set_name, set_name))
      return &definition;
  }

  return nullptr;
}

bool IsGVArchivePath(std::string_view path)
{
  return StringUtil::EqualNoCase(Path::GetExtension(path), "zip");
}

std::string_view GetGVSetNameFromArchivePath(std::string_view path)
{
  return IsGVArchivePath(path) ? Path::GetFileTitle(path) : std::string_view{};
}

const GVGameDefinition* IdentifyGVArchive(std::string_view path)
{
  return GetGVGameDefinition(GetGVSetNameFromArchivePath(path));
}

bool IsGVSet(std::string_view set_name)
{
  return (GetGVGameDefinition(set_name) != nullptr);
}

const char* GetGVBIOSProfileName(GVBIOSProfile profile)
{
  switch (profile)
  {
    case GVBIOSProfile::KonamiGV:
      return "konamigv";

    default:
      return "unknown";
  }
}

} // namespace Konami
