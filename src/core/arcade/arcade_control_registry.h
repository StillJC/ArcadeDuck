// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/types.h"

#include <array>
#include <span>
#include <string_view>

namespace Arcade {

constexpr u32 NUM_ARCADE_CONTROLLER_PORTS = 4;

enum class ArcadeControllerType : u8
{
  None,
  Arcade,
  Trackball,
  Lightgun,
  Driving,
  Tokimeki,
};

enum class ArcadeJoystickMode : u8
{
  None,
  FourWay,
  EightWay,
};

struct ArcadeControllerTypeInfo
{
  ArcadeControllerType type;
  std::string_view name;
  std::string_view display_name;
  std::string_view artwork_key;
};

enum class ArcadeControlBindingKind : u8
{
  Button,
  Axis,
};

struct ArcadeControlLayoutBindingInfo
{
  std::string_view binding_key;
  std::string_view display_name;
  ArcadeControlBindingKind kind;
};

struct ArcadeControlLayoutInfo
{
  std::string_view layout_key;
  std::string_view display_name;
  ArcadeControllerType controller_type;
  ArcadeJoystickMode joystick_mode;
  std::span<const ArcadeControlLayoutBindingInfo> bindings;
};

struct ArcadePortProfile
{
  ArcadeControllerType controller_type;
  ArcadeJoystickMode joystick_mode;
  std::string_view game_layout_key;
};

struct ArcadeGameControlProfile
{
  std::string_view game_id;
  std::array<ArcadePortProfile, NUM_ARCADE_CONTROLLER_PORTS> ports;
};

std::span<const ArcadeControllerTypeInfo> GetArcadeControllerTypeInfos();
const ArcadeControllerTypeInfo* GetArcadeControllerTypeInfo(ArcadeControllerType type);

std::span<const ArcadeControlLayoutInfo> GetArcadeControlLayoutInfos();
const ArcadeControlLayoutInfo* GetArcadeControlLayoutInfo(std::string_view layout_key);

const ArcadeGameControlProfile* GetArcadeGameControlProfile(std::string_view game_id);

} // namespace Arcade
