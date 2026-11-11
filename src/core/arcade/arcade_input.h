// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "common/types.h"

#include <array>
#include <string>
#include <string_view>

class SettingsInterface;

namespace Arcade {
struct ArcadeGameControlProfile;
}

namespace ArcadeInput {

void CopyControllerConfiguration(SettingsInterface* dest_si, const SettingsInterface& src_si,
                                 const Arcade::ArcadeGameControlProfile* target_profile = nullptr);

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

void RegisterBindings(SettingsInterface& si, SettingsInterface& operator_si, std::string_view game_id = {},
                      SettingsInterface* game_si = nullptr);
bool IsDigitalPressed(u32 port, std::string_view key);
bool HasDigitalControl(u32 port, std::string_view key);
float GetAnalogValue(u32 port, std::string_view key);
bool HasAnalogControl(u32 port, std::string_view key);
bool IsOperatorPressed(std::string_view key);

// Keeps Service asserted for at least this long in emulated time after a press (0 disables).
void SetOperatorServiceMinimumHold(u32 milliseconds);
LightgunPresentationState GetLightgunPresentationState();
void SetLightgunViewport(float left, float top, float right, float bottom, float border_x, float border_y);
void ResyncWindowedPointer(float x, float y);
void Update();

} // namespace ArcadeInput
