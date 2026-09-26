// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "common/types.h"

#include <array>
#include <string>
#include <string_view>

class SettingsInterface;

namespace ArcadeInput {

static constexpr u32 NUM_LIGHTGUN_PRESENTATION_PORTS = 3;

struct LightgunPresentationPort
{
  bool active = false;
  bool crosshair_enabled = false;
  bool offscreen = false;
  float x = 0.5f;
  float y = 0.5f;
  u32 crosshair_scale = 100;
  std::string crosshair_image_path;
};

struct LightgunPresentationState
{
  std::array<LightgunPresentationPort, NUM_LIGHTGUN_PRESENTATION_PORTS> ports = {};
  bool sinden_border_enabled = false;
  u32 sinden_border_width = 4;
};

void RegisterBindings(SettingsInterface& si, SettingsInterface& operator_si);
bool IsDigitalPressed(u32 port, std::string_view key);
float GetAnalogValue(u32 port, std::string_view key);
bool IsOperatorPressed(std::string_view key);
LightgunPresentationState GetLightgunPresentationState();
void SetLightgunViewport(float left, float top, float right, float bottom, float border_x, float border_y);
void ResyncWindowedPointer(float x, float y);
void Update();

} // namespace ArcadeInput
