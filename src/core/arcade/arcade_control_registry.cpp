// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/arcade_control_registry.h"
#include "core/arcade/arcade_database.h"

#include <array>
#include <utility>
#include <vector>

namespace Arcade {
namespace {

constexpr std::array<ArcadeControllerTypeInfo, 7> s_controller_type_infos = {{
  {ArcadeControllerType::None, "none", "None", "none"},
  {ArcadeControllerType::Arcade, "arcade", "Arcade Controls", "arcade"},
  {ArcadeControllerType::Trackball, "trackball", "Trackball", "trackball"},
  {ArcadeControllerType::Lightgun, "lightgun", "Lightgun", "lightgun"},
  {ArcadeControllerType::Driving, "driving", "Driving / Racing", "racing"},
  {ArcadeControllerType::Tokimeki, "tokimeki", "Tokimeki Controls", "tokimeki"},
  {ArcadeControllerType::Mahjong, "mahjong", "Mahjong Panel", "mahjong"},
}};

constexpr std::array<std::pair<std::string_view, std::string_view>, 7> s_layout_aliases = {{
  {"glpracr2", "glpracr"},
  {"tekken3", "tekken"},
  {"tektagt", "tekken"},
  {"sfex2", "sfex"},
  {"fgtlayer", "sfex"},
  {"soulclbr", "souledge"},
  {"hypbbc2p", "hyperbbc"},
}};

std::string_view ResolveLayoutAlias(std::string_view layout_key)
{
  for (const auto& [alias, canonical] : s_layout_aliases)
  {
    if (layout_key == alias)
      return canonical;
  }

  return layout_key;
}

bool s_registry_built = false;
std::vector<std::vector<ArcadeControlLayoutBindingInfo>> s_layout_binding_storage;
std::vector<ArcadeControlLayoutInfo> s_control_layout_infos;
std::vector<ArcadeGameControlProfile> s_game_control_profiles;

void EnsureRegistryBuilt()
{
  if (s_registry_built)
    return;

  s_registry_built = true;
  if (!Database::EnsureLoaded())
    return;

  const std::span<const Database::ControlLayoutDefinition> layouts = Database::GetControlLayouts();
  s_layout_binding_storage.reserve(layouts.size());
  s_control_layout_infos.reserve(layouts.size());
  for (const Database::ControlLayoutDefinition& source : layouts)
  {
    std::vector<ArcadeControlLayoutBindingInfo>& bindings = s_layout_binding_storage.emplace_back();
    bindings.reserve(source.bindings.size());
    for (const Database::ControlLayoutBindingDefinition& binding : source.bindings)
      bindings.push_back({binding.key, binding.display_name, binding.kind});

    s_control_layout_infos.push_back(
      {source.id, source.display_name, source.controller_type, source.joystick_mode, bindings});
  }

  const std::span<const Database::GameDefinition> games = Database::GetGames();
  s_game_control_profiles.reserve(games.size());
  for (const Database::GameDefinition& game : games)
  {
    ArcadeGameControlProfile profile;
    profile.game_id = game.id;
    for (u32 i = 0; i < NUM_ARCADE_CONTROLLER_PORTS; i++)
    {
      const Database::PortDefinition& port = game.ports[i];
      profile.ports[i] = {port.controller_type, port.joystick_mode, port.layout};
    }
    s_game_control_profiles.push_back(profile);
  }
}

} // namespace

std::span<const ArcadeControllerTypeInfo> GetArcadeControllerTypeInfos()
{
  return s_controller_type_infos;
}

const ArcadeControllerTypeInfo* GetArcadeControllerTypeInfo(ArcadeControllerType type)
{
  for (const ArcadeControllerTypeInfo& info : s_controller_type_infos)
  {
    if (info.type == type)
      return &info;
  }
  return nullptr;
}

std::span<const ArcadeControlLayoutInfo> GetArcadeControlLayoutInfos()
{
  EnsureRegistryBuilt();
  return s_control_layout_infos;
}

const ArcadeControlLayoutInfo* GetArcadeControlLayoutInfo(std::string_view layout_key)
{
  EnsureRegistryBuilt();
  layout_key = ResolveLayoutAlias(layout_key);
  for (const ArcadeControlLayoutInfo& info : s_control_layout_infos)
  {
    if (info.layout_key == layout_key)
      return &info;
  }
  return nullptr;
}

const ArcadeGameControlProfile* GetArcadeGameControlProfile(std::string_view game_id)
{
  EnsureRegistryBuilt();
  for (const ArcadeGameControlProfile& profile : s_game_control_profiles)
  {
    if (profile.game_id == game_id)
      return &profile;
  }
  return nullptr;
}

} // namespace Arcade
