// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/arcade_input.h"

#include "core/arcade/arcade_control_registry.h"
#include "core/arcade/systems/konami/konami.h"
#include "core/arcade/systems/namco/system11/namco_system11.h"
#include "core/arcade/systems/sony/zn/sony_zn.h"
#include "core/settings.h"
#include "core/system.h"

#include "common/path.h"
#include "common/settings_interface.h"
#include "util/input_manager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ArcadeInput {

namespace {

enum class Control : u8
{
  Left, Right, Up, Down, Button1, Button2, Button3, Button4, Button5, Button6, Coin, Start, Trigger, Reload,
  GSRIncrease, GSRDecrease, GearUp, GearDown, Handbrake, View, Horn, MusicNext, MusicPrevious, Select,
  SelectUp, SelectDown, SelectLeft, SelectRight, Enter, Sensor, Count
};

enum class DrivingAxis : u8
{
  Steering,
  Accelerator,
  Brake,
  Clutch,
};

constexpr u32 NUM_CONTROLS = static_cast<u32>(Control::Count);
constexpr float DIGITAL_AXIS_PRESS_THRESHOLD = 0.50f;
constexpr float DIGITAL_AXIS_RELEASE_THRESHOLD = 0.40f;

enum class OperatorControl : u8
{
  Service,
  Test,
  Count
};

struct TrackballButtonAxis
{
  std::array<u32, 2> pressed_counts{};
  float held_seconds = 0.0f;
  float remainder = 0.0f;
};

struct PortState
{
  Arcade::ArcadeControllerType type = Arcade::ArcadeControllerType::None;
  Arcade::ArcadeJoystickMode joystick_mode = Arcade::ArcadeJoystickMode::None;
  std::string layout;
  std::string input_mode;
  std::string physical_device;
  float x_sensitivity = 1.0f;
  float y_sensitivity = 1.0f;
  bool invert_x = false;
  bool invert_y = false;
  bool offscreen_reload = true;
  bool crosshair_enabled = false;
  u32 crosshair_scale = 100;
  std::string crosshair_image_path;
  float lightgun_x = 0.5f;
  float lightgun_y = 0.5f;
  bool lightgun_offscreen = false;
  std::array<bool, 2> controller_bound_axes{};
  float axis_x = 0.0f;
  float axis_y = 0.0f;
  bool trackball_buttons_as_axis = false;
  std::array<TrackballButtonAxis, 2> trackball_button_axes{};
  float steering = 0.5f;
  float steering_axis = 0.5f;
  std::array<u32, 2> steering_button_counts = {};
  float throttle_brake = 0.5f;
  float accelerator = 0.0f;
  float brake = 0.0f;
  float clutch = 0.0f;
  float steering_deadzone = 0.0f;
  float steering_sensitivity = 1.0f;
  float accelerator_deadzone = 0.0f;
  float accelerator_sensitivity = 1.0f;
  float brake_deadzone = 0.0f;
  float brake_sensitivity = 1.0f;
  float clutch_deadzone = 0.0f;
  float clutch_sensitivity = 1.0f;
  u64 direction_sequence = 0;
  std::array<u64, 4> direction_order = {};
  std::array<u32, NUM_CONTROLS> pressed_counts = {};
};

struct OperatorState
{
  std::array<u32, static_cast<u32>(OperatorControl::Count)> pressed_counts = {};
};

std::mutex s_mutex;
std::array<PortState, Arcade::NUM_ARCADE_CONTROLLER_PORTS> s_ports;
OperatorState s_operator;
bool s_sinden_border_enabled = false;
u32 s_sinden_border_width = 4;
float s_lightgun_viewport_left = 0.0f;
float s_lightgun_viewport_top = 0.0f;
float s_lightgun_viewport_right = 1.0f;
float s_lightgun_viewport_bottom = 1.0f;
float s_lightgun_viewport_border_x = 0.0f;
float s_lightgun_viewport_border_y = 0.0f;

std::optional<Control> GetControl(std::string_view key)
{
  static constexpr std::array<std::pair<std::string_view, Control>, 30> controls = {{
    {"Left", Control::Left}, {"Right", Control::Right}, {"Up", Control::Up}, {"Down", Control::Down},
    {"Button1", Control::Button1}, {"Button2", Control::Button2}, {"Button3", Control::Button3},
    {"Button4", Control::Button4}, {"Button5", Control::Button5}, {"Button6", Control::Button6},
    {"Coin", Control::Coin}, {"Start", Control::Start}, {"Trigger", Control::Trigger}, {"Reload", Control::Reload},
    {"GSRIncrease", Control::GSRIncrease}, {"GSRDecrease", Control::GSRDecrease},
    {"GearUp", Control::GearUp}, {"GearDown", Control::GearDown}, {"Handbrake", Control::Handbrake},
    {"View", Control::View}, {"Horn", Control::Horn}, {"MusicNext", Control::MusicNext},
    {"MusicPrevious", Control::MusicPrevious}, {"Select", Control::Select}, {"SelectUp", Control::SelectUp},
    {"SelectDown", Control::SelectDown}, {"SelectLeft", Control::SelectLeft},
    {"SelectRight", Control::SelectRight}, {"Enter", Control::Enter}, {"Sensor", Control::Sensor},
  }};
  for (const auto& [name, control] : controls)
  {
    if (name == key)
      return control;
  }
  return std::nullopt;
}

bool IsSingleControllerAxisBinding(std::string_view binding)
{
  const std::optional<InputBindingKey> key = InputManager::ParseInputBindingKey(binding);
  return key.has_value() && key->source_subtype == InputSubclass::ControllerAxis &&
         key->source_type != InputSourceType::Keyboard && key->source_type != InputSourceType::Pointer &&
         key->source_type != InputSourceType::Sensor;
}

bool IsPressed(const PortState& port, Control control)
{
  return port.pressed_counts[static_cast<u32>(control)] != 0;
}

u32 GetGVOperatorMask(OperatorControl control)
{
  switch (control)
  {
    case OperatorControl::Service: return 1U << 11;
    case OperatorControl::Test: return 1U << 12;
    default: return 0;
  }
}

u32 GetGQMask(Control control)
{
  switch (control)
  {
    case Control::Coin: return 1U << 0;
    case Control::Start: return 1U << 1;
    case Control::Trigger: return 1U << 3;
    case Control::Reload: return 1U << 4;
    default: return 0;
  }
}

bool IsCoinSlotPressed(u32 slot)
{
  if (slot >= 2)
    return false;

  // Arcade cabinets expose at most two physical coin slots. Ports 1/3 share Coin 1 and ports 2/4 share Coin 2.
  return IsPressed(s_ports[slot], Control::Coin) || IsPressed(s_ports[slot + 2], Control::Coin);
}

u32 GetGVMask(u32 port, Control control, const PortState& state)
{
  if (state.layout == "weddingr")
  {
    if (control == Control::Button1) return 1U << 2;
    if (control == Control::Button2) return 1U << 3;
    if (control == Control::Button3) return 1U << 0;
    if (control == Control::Button4) return 1U << 1;
    return 0;
  }

  switch (control)
  {
    case Control::Left: return 1U << 0;
    case Control::Right: return 1U << 1;
    case Control::Up: return 1U << 2;
    case Control::Down: return 1U << 3;
    case Control::Button1: return 1U << 4;
    case Control::Button2: return 1U << 5;
    case Control::Button3: return 1U << 6;
    case Control::Start: return 1U << (port < 2 ? 9 : 7);
    case Control::Coin: return 0; // Routed globally through the two physical cabinet coin slots.
    default: return 0;
  }
}

bool GetFourWayDirection(const PortState& port, Control control)
{
  if (!IsPressed(port, control))
    return false;
  if (port.joystick_mode != Arcade::ArcadeJoystickMode::FourWay)
    return true;

  u64 newest = 0;
  Control selected = Control::Left;
  for (u32 i = 0; i < 4; i++)
  {
    const Control candidate = static_cast<Control>(i);
    if (IsPressed(port, candidate) && port.direction_order[i] >= newest)
    {
      newest = port.direction_order[i];
      selected = candidate;
    }
  }
  return selected == control;
}

void ApplyPort(u32 index)
{
  PortState& port = s_ports[index];
  if (port.type == Arcade::ArcadeControllerType::None)
    return;

  if (Konami::IsGQActive())
  {
    Konami::SetGQArcadeButton(0, GetGQMask(Control::Coin), IsCoinSlotPressed(0));
    Konami::SetGQArcadeButton(1, GetGQMask(Control::Coin), IsCoinSlotPressed(1));
    Konami::SetGQArcadeButton(2, GetGQMask(Control::Coin), false);

    if (port.type != Arcade::ArcadeControllerType::Lightgun || index >= 3)
      return;

    Konami::SetGQArcadeButton(index, GetGQMask(Control::Start), IsPressed(port, Control::Start));

    const bool trigger = IsPressed(port, Control::Trigger);
    const bool explicit_reload = IsPressed(port, Control::Reload);
    const bool offscreen_reload = port.offscreen_reload && trigger && port.lightgun_offscreen;
    Konami::SetGQArcadeButton(index, GetGQMask(Control::Trigger), trigger && !offscreen_reload);
    Konami::SetGQArcadeButton(index, GetGQMask(Control::Reload), explicit_reload || offscreen_reload);
    return;
  }

  if (!Konami::IsGVActive())
    return;

  const std::array<Control, 9> controls = {{Control::Left, Control::Right, Control::Up, Control::Down, Control::Button1,
                                            Control::Button2, Control::Button3, Control::Button4, Control::Start}};
  for (const Control control : controls)
  {
    const u32 mask = GetGVMask(index, control, port);
    if (mask == 0)
      continue;
    const bool pressed = (control <= Control::Down) ? GetFourWayDirection(port, control) : IsPressed(port, control);
    Konami::SetGVArcadeButton(index, mask, pressed);
  }

  Konami::SetGVArcadeButton(0, 1U << 10, IsCoinSlotPressed(0));
  Konami::SetGVArcadeButton(1, 1U << 10, IsCoinSlotPressed(1));

  if (port.type == Arcade::ArcadeControllerType::Lightgun && index < 2)
  {
    const bool trigger = IsPressed(port, Control::Trigger);
    const bool reload_button = port.offscreen_reload && IsPressed(port, Control::Reload);
    const bool aimed_offscreen = port.offscreen_reload && port.lightgun_offscreen;
    const bool offscreen_shot = reload_button || (trigger && aimed_offscreen);
    Konami::SetGVLightgunTrigger(index, trigger || reload_button);
    Konami::SetGVLightgunShootOffscreen(index, offscreen_shot);
  }
}

void OnDigital(u32 port_index, Control control, s32 value)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  PortState& port = s_ports[port_index];
  const u32 control_index = static_cast<u32>(control);
  const bool was_pressed = port.pressed_counts[control_index] != 0;
  if (value > 0)
    port.pressed_counts[control_index]++;
  else if (value <= 0 && port.pressed_counts[control_index] > 0)
    port.pressed_counts[control_index]--;
  const bool pressed = port.pressed_counts[control_index] != 0;
  if (pressed && !was_pressed)
  {
    if (control <= Control::Down)
      port.direction_order[static_cast<u32>(control)] = ++port.direction_sequence;
    if (port.type == Arcade::ArcadeControllerType::Tokimeki)
    {
      // Legacy GSR binding names control the combined excitement level for profile compatibility.
      if (control == Control::GSRIncrease)
        Konami::AdjustGVTokimekiExcitement(+1);
      if (control == Control::GSRDecrease)
        Konami::AdjustGVTokimekiExcitement(-1);
    }
  }
  ApplyPort(port_index);
}

void OnOperatorDigital(OperatorControl control, s32 value)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  const u32 control_index = static_cast<u32>(control);
  if (value > 0)
    s_operator.pressed_counts[control_index]++;
  else if (s_operator.pressed_counts[control_index] > 0)
    s_operator.pressed_counts[control_index]--;

  const bool pressed = s_operator.pressed_counts[control_index] != 0;
  if (Konami::IsGQActive())
  {
    if (control == OperatorControl::Service)
    {
      for (u32 player = 0; player < 3; player++)
        Konami::SetGQArcadeButton(player, 1U << 5, pressed);
    }
    else if (control == OperatorControl::Test)
    {
      Konami::SetGQTest(pressed);
    }
  }
  else
  {
    Konami::SetGVArcadeButton(0, GetGVOperatorMask(control), pressed);
  }
}

s32 GetTrackballButtonDirection(const TrackballButtonAxis& axis)
{
  return static_cast<s32>(axis.pressed_counts[1] != 0) - static_cast<s32>(axis.pressed_counts[0] != 0);
}

void OnTrackballButton(u32 port_index, bool x_axis, bool positive, s32 value)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  TrackballButtonAxis& axis = s_ports[port_index].trackball_button_axes[x_axis ? 0 : 1];
  const s32 previous_direction = GetTrackballButtonDirection(axis);
  u32& count = axis.pressed_counts[positive ? 1 : 0];
  if (value > 0)
    count++;
  else if (count > 0)
    count--;

  if (GetTrackballButtonDirection(axis) != previous_direction)
  {
    axis.held_seconds = 0.0f;
    axis.remainder = 0.0f;
  }
}

s32 GetTrackballButtonDelta(TrackballButtonAxis& axis, float sensitivity, bool invert, float frame_seconds)
{
  const s32 direction = GetTrackballButtonDirection(axis);
  if (direction == 0)
  {
    axis.held_seconds = 0.0f;
    axis.remainder = 0.0f;
    return 0;
  }

  // Reach full trackball speed after holding a direction for 1 second.
  const float speed = 2.0f + 126.0f * std::min(axis.held_seconds / 1.0f, 1.0f);
  axis.held_seconds = std::min(axis.held_seconds + frame_seconds, 1.0f);
  axis.remainder += static_cast<float>(direction) * speed * sensitivity * (invert ? -1.0f : 1.0f);
  const s32 delta = static_cast<s32>(axis.remainder);
  axis.remainder -= static_cast<float>(delta);
  return delta;
}

void OnAxis(u32 port_index, bool x_axis, float value, bool controller_axis)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  PortState& port = s_ports[port_index];
  port.controller_bound_axes[x_axis ? 0 : 1] = controller_axis;
  (x_axis ? port.axis_x : port.axis_y) = std::clamp(value, -1.0f, 1.0f);
  const u32 lightgun_port_count = Konami::IsGQActive() ? 3U : 2U;
  if (port.type == Arcade::ArcadeControllerType::Lightgun && port.input_mode == "BoundAxis" &&
      port_index < lightgun_port_count)
  {
    float aim_x = port.axis_x;
    float aim_y = port.axis_y;
    if (port.controller_bound_axes[0] && port.controller_bound_axes[1])
    {
      // Expand a circular stick gate to the rectangular aiming area.
      const float extent = std::max(std::abs(aim_x), std::abs(aim_y));
      if (extent > 0.0f)
      {
        const float scale = std::min(std::sqrt(aim_x * aim_x + aim_y * aim_y), 1.0f) / extent;
        aim_x *= scale;
        aim_y *= scale;
      }
    }
    const float x = 0.5f + aim_x * 0.5f * (port.invert_x ? -1.0f : 1.0f);
    const float y = 0.5f + aim_y * 0.5f * (port.invert_y ? -1.0f : 1.0f);
    port.lightgun_x = std::clamp(x, 0.0f, 1.0f);
    port.lightgun_y = std::clamp(y, 0.0f, 1.0f);
    port.lightgun_offscreen = false;
    if (Konami::IsGQActive())
      Konami::SetGQLightgunPosition(port_index, port.lightgun_x, port.lightgun_y);
    else
      Konami::SetGVLightgunPosition(port_index, port.lightgun_x, port.lightgun_y);
  }
}

float ApplyDrivingAxisAdjustment(float value, bool centered, float deadzone, float sensitivity)
{
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  if (deadzone <= 0.0f && sensitivity == 1.0f)
    return clamped;

  if (centered)
  {
    const float signed_value = (clamped - 0.5f) * 2.0f;
    const float magnitude = std::abs(signed_value);
    if (magnitude <= deadzone)
      return 0.5f;

    const float normalized = std::clamp((magnitude - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);
    const float adjusted = (sensitivity == 1.0f) ? normalized : std::pow(normalized, 1.0f / sensitivity);
    return 0.5f + (signed_value < 0.0f ? -adjusted : adjusted) * 0.5f;
  }

  if (clamped <= deadzone)
    return 0.0f;

  const float normalized = std::clamp((clamped - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);
  return (sensitivity == 1.0f) ? normalized : std::pow(normalized, 1.0f / sensitivity);
}

void UpdateDrivingSteering(PortState& port)
{
  const bool left = port.steering_button_counts[0] != 0;
  const bool right = port.steering_button_counts[1] != 0;
  if (left && !right)
    port.steering = 0.0f;
  else if (right && !left)
    port.steering = 1.0f;
  else if (left && right)
    port.steering = 0.5f;
  else
    port.steering = port.steering_axis;
}

void OnDrivingAxis(u32 port_index, DrivingAxis axis, float value)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  PortState& port = s_ports[port_index];
  switch (axis)
  {
    case DrivingAxis::Steering:
      port.steering_axis =
        ApplyDrivingAxisAdjustment(value, true, port.steering_deadzone, port.steering_sensitivity);
      UpdateDrivingSteering(port);
      break;
    case DrivingAxis::Accelerator:
      port.accelerator =
        ApplyDrivingAxisAdjustment(value, false, port.accelerator_deadzone, port.accelerator_sensitivity);
      break;
    case DrivingAxis::Brake:
      port.brake = ApplyDrivingAxisAdjustment(value, false, port.brake_deadzone, port.brake_sensitivity);
      break;
    case DrivingAxis::Clutch:
      port.clutch = ApplyDrivingAxisAdjustment(value, false, port.clutch_deadzone, port.clutch_sensitivity);
      break;
  }
}

void OnDrivingSteeringButton(u32 port_index, bool right, s32 value)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  PortState& port = s_ports[port_index];
  u32& pressed_count = port.steering_button_counts[right ? 1 : 0];
  if (value > 0)
    pressed_count++;
  else if (pressed_count > 0)
    pressed_count--;

  UpdateDrivingSteering(port);
}

void OnPointerMove(u32 port_index, InputBindingKey key, float delta)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  PortState& port = s_ports[port_index];
  if (key.data != static_cast<u32>(InputPointerAxis::X) && key.data != static_cast<u32>(InputPointerAxis::Y))
    return;
  const bool x_axis = key.data == static_cast<u32>(InputPointerAxis::X);
  const float scale = (x_axis ? port.x_sensitivity : port.y_sensitivity) * (x_axis ? (port.invert_x ? -1.0f : 1.0f) : (port.invert_y ? -1.0f : 1.0f));
  if (port.type == Arcade::ArcadeControllerType::Trackball && port_index < 2)
  {
    const s32 delta_x = x_axis ? static_cast<s32>(delta * scale) : 0;
    const s32 delta_y = x_axis ? 0 : static_cast<s32>(delta * scale);
    if (SonyZN::IsMawasundaActive())
      SonyZN::AddMawasundaHandleDelta(port_index, delta_x);
    else if (NamcoSystem11::IsFamilyBowlActive())
      NamcoSystem11::AddFamilyBowlTrackballDelta(port_index, delta_x, delta_y);
    else
      Konami::AddGVTrackballDelta(port_index, delta_x, delta_y);
  }
  else if (port.type == Arcade::ArcadeControllerType::Lightgun &&
           port_index < (Konami::IsGQActive() ? 3U : 2U))
  {
    if (x_axis)
      port.axis_x = std::clamp(port.axis_x + delta * scale / 1000.0f, 0.0f, 1.0f);
    else
      port.axis_y = std::clamp(port.axis_y + delta * scale / 1000.0f, 0.0f, 1.0f);
    port.lightgun_x = port.axis_x;
    port.lightgun_y = port.axis_y;
    port.lightgun_offscreen = false;
    if (Konami::IsGQActive())
      Konami::SetGQLightgunPosition(port_index, port.lightgun_x, port.lightgun_y);
    else
      Konami::SetGVLightgunPosition(port_index, port.lightgun_x, port.lightgun_y);
  }
}

void OnPointerAbsolute(u32 port_index, float x, float y)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  PortState& port = s_ports[port_index];
  if (port.type != Arcade::ArcadeControllerType::Lightgun ||
      port_index >= (Konami::IsGQActive() ? 3U : 2U))
    return;

  float playable_left = s_lightgun_viewport_left + s_lightgun_viewport_border_x;
  float playable_top = s_lightgun_viewport_top + s_lightgun_viewport_border_y;
  float playable_right = s_lightgun_viewport_right - s_lightgun_viewport_border_x;
  float playable_bottom = s_lightgun_viewport_bottom - s_lightgun_viewport_border_y;
  if (playable_right <= playable_left || playable_bottom <= playable_top)
  {
    playable_left = s_lightgun_viewport_left;
    playable_top = s_lightgun_viewport_top;
    playable_right = s_lightgun_viewport_right;
    playable_bottom = s_lightgun_viewport_bottom;
  }

  port.lightgun_offscreen = (x < playable_left || x > playable_right || y < playable_top || y > playable_bottom);
  const float game_x = (x - playable_left) / (playable_right - playable_left);
  const float game_y = (y - playable_top) / (playable_bottom - playable_top);
  const float adjusted_x = port.invert_x ? (1.0f - game_x) : game_x;
  const float adjusted_y = port.invert_y ? (1.0f - game_y) : game_y;
  port.axis_x = std::clamp(adjusted_x, 0.0f, 1.0f);
  port.axis_y = std::clamp(adjusted_y, 0.0f, 1.0f);
  port.lightgun_x = port.axis_x;
  port.lightgun_y = port.axis_y;
  if (Konami::IsGQActive())
    Konami::SetGQLightgunPosition(port_index, port.lightgun_x, port.lightgun_y);
  else
    Konami::SetGVLightgunPosition(port_index, port.lightgun_x, port.lightgun_y);
  ApplyPort(port_index);
}

void AddDigitalBindings(SettingsInterface& si, const std::string& section, u32 port, std::string_view key)
{
  const std::optional<Control> control = GetControl(key);
  if (!control.has_value())
    return;
  for (const std::string& binding : si.GetStringList(section.c_str(), key.data()))
  {
    if (IsSingleControllerAxisBinding(binding))
    {
      InputManager::AddBinding(binding, InputAxisEventHandler{[port, control = control.value(), active = false](float value) mutable {
        const bool next_active = active ? (value > DIGITAL_AXIS_RELEASE_THRESHOLD) :
                                          (value >= DIGITAL_AXIS_PRESS_THRESHOLD);
        if (next_active == active)
          return;

        active = next_active;
        OnDigital(port, control, active ? 1 : 0);
      }});
    }
    else
    {
      InputManager::AddBinding(binding,
                               InputButtonEventHandler{[port, control = control.value()](s32 value) { OnDigital(port, control, value); }});
    }
  }
}

void AddOperatorBinding(SettingsInterface& si, std::string_view key, OperatorControl control)
{
  for (const std::string& binding : si.GetStringList("ArcadeOperator", key.data()))
  {
    InputManager::AddBinding(
      binding, InputButtonEventHandler{[control](s32 value) { OnOperatorDigital(control, value); }});
  }
}

std::string NormalizeCenteredControllerAxisBinding(std::string_view binding)
{
  std::optional<InputBindingKey> parsed = InputManager::ParseInputBindingKey(binding);
  if (!parsed.has_value() || parsed->source_subtype != InputSubclass::ControllerAxis ||
      parsed->source_type == InputSourceType::Keyboard || parsed->source_type == InputSourceType::Pointer ||
      parsed->source_type == InputSourceType::Sensor)
  {
    return std::string(binding);
  }

  parsed->modifier = InputModifier::FullAxis;
  return InputManager::ConvertInputBindingKeyToString(InputBindingInfo::Type::Axis, parsed.value());
}

void AddAxisBindings(SettingsInterface& si, const std::string& section, u32 port, std::string_view key, bool x_axis)
{
  for (const std::string& binding : si.GetStringList(section.c_str(), key.data()))
  {
    const bool controller_axis = IsSingleControllerAxisBinding(binding);
    const std::string normalized_binding = controller_axis ? NormalizeCenteredControllerAxisBinding(binding) : binding;
    InputManager::AddBinding(normalized_binding, InputAxisEventHandler{[port, x_axis, controller_axis](float value) {
      // Controller full axes arrive as 0..1; trackballs and lightguns expect -1..1.
      OnAxis(port, x_axis, controller_axis ? (value * 2.0f - 1.0f) : value, controller_axis);
    }});
  }
}

void AddTrackballButtonBindings(SettingsInterface& si, const std::string& section, u32 port,
                               const char* key, bool x_axis, bool positive)
{
  for (const std::string& binding : si.GetStringList(section.c_str(), key))
  {
    InputManager::AddBinding(binding, InputButtonEventHandler{
      [port, x_axis, positive](s32 value) { OnTrackballButton(port, x_axis, positive, value); }});
  }
}

void AddDrivingAxisBindings(SettingsInterface& si, const std::string& section, u32 port, std::string_view key,
                            DrivingAxis axis)
{
  for (const std::string& binding : si.GetStringList(section.c_str(), key.data()))
  {
    const std::string normalized_binding =
      (axis == DrivingAxis::Steering) ? NormalizeCenteredControllerAxisBinding(binding) : binding;
    InputManager::AddBinding(
      normalized_binding, InputAxisEventHandler{[port, axis](float value) { OnDrivingAxis(port, axis, value); }});
  }
}

void AddDrivingSteeringButtonBindings(SettingsInterface& si, const std::string& section, u32 port,
                                      std::string_view key, bool right)
{
  for (const std::string& binding : si.GetStringList(section.c_str(), key.data()))
  {
    InputManager::AddBinding(
      binding, InputButtonEventHandler{[port, right](s32 value) { OnDrivingSteeringButton(port, right, value); }});
  }
}

} // namespace

void SetLightgunViewport(float left, float top, float right, float bottom, float border_x, float border_y)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  s_lightgun_viewport_left = left;
  s_lightgun_viewport_top = top;
  s_lightgun_viewport_right = right;
  s_lightgun_viewport_bottom = bottom;
  s_lightgun_viewport_border_x = border_x;
  s_lightgun_viewport_border_y = border_y;
}

void ResyncWindowedPointer(float x, float y)
{
  std::string physical_device;
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    const PortState& state = s_ports[0];
    if (state.type != Arcade::ArcadeControllerType::Lightgun ||
        state.input_mode != "NativeRawInputDevice" ||
        state.physical_device.empty())
    {
      return;
    }

    physical_device = state.physical_device;
  }

  InputManager::SetPointerAbsolutePosition(physical_device, x, y);
}
void RegisterBindings(SettingsInterface& si, SettingsInterface& operator_si, std::string_view game_id)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  Konami::ResetGQArcadeInputs();
  Konami::ResetGVArcadeInputs();
  s_ports = {};
  s_operator = {};
  s_sinden_border_enabled = si.GetBoolValue("ArcadeControllerPort1", "SindenBorder", false);
  s_sinden_border_width = std::clamp<u32>(si.GetUIntValue("ArcadeControllerPort1", "SindenBorderWidth", 4), 1, 64);
  AddOperatorBinding(operator_si, "Test", OperatorControl::Test);
  AddOperatorBinding(operator_si, "Service", OperatorControl::Service);
  const Arcade::ArcadeGameControlProfile* const game_profile =
    game_id.empty() ? nullptr : Arcade::GetArcadeGameControlProfile(game_id);
  for (u32 port = 0; port < Arcade::NUM_ARCADE_CONTROLLER_PORTS; port++)
  {
    const std::string section = "ArcadeControllerPort" + std::to_string(port + 1);
    PortState& state = s_ports[port];
    if (game_profile)
    {
      const Arcade::ArcadePortProfile& port_profile = game_profile->ports[port];
      state.type = port_profile.controller_type;
      state.layout = port_profile.game_layout_key;
      state.joystick_mode = port_profile.joystick_mode;
    }
    else
    {
      const std::string type_name = si.GetStringValue(section.c_str(), "Type", "none");
      for (const Arcade::ArcadeControllerTypeInfo& info : Arcade::GetArcadeControllerTypeInfos())
      {
        if (info.name == type_name)
          state.type = info.type;
      }
      state.layout = si.GetStringValue(section.c_str(), "Layout", "");
      if (const Arcade::ArcadeControlLayoutInfo* layout = Arcade::GetArcadeControlLayoutInfo(state.layout))
        state.joystick_mode = layout->joystick_mode;
    }
    if (state.type == Arcade::ArcadeControllerType::None)
      continue;
    state.input_mode = si.GetStringValue(section.c_str(), "InputMode", "BoundAxis");
    if (state.input_mode != "NativeRawInputDevice")
      state.input_mode = "BoundAxis";
    state.physical_device = si.GetStringValue(section.c_str(), "PhysicalDevice", "");
    state.x_sensitivity = si.GetIntValue(section.c_str(), "XSensitivity", 100) / 100.0f;
    state.y_sensitivity = si.GetIntValue(section.c_str(), "YSensitivity", 100) / 100.0f;
    state.invert_x = si.GetBoolValue(section.c_str(), "InvertX", false);
    state.invert_y = si.GetBoolValue(section.c_str(), "InvertY", false);
    state.offscreen_reload = si.GetBoolValue(section.c_str(), "OffscreenReload", true);
    state.crosshair_enabled = si.GetBoolValue(section.c_str(), "CrosshairEnabled", false);
    state.crosshair_scale =
      static_cast<u32>(std::clamp(si.GetIntValue(section.c_str(), "CrosshairScale", 100), 0, 200));
    state.crosshair_image_path = si.GetStringValue(section.c_str(), "CrosshairImagePath", "");
    if (!state.crosshair_image_path.empty() && !Path::IsAbsolute(state.crosshair_image_path))
      state.crosshair_image_path = Path::Combine(EmuFolders::Crosshairs, state.crosshair_image_path);
    state.steering_deadzone =
      std::clamp(si.GetIntValue(section.c_str(), "SteeringDeadzone", 0) / 100.0f, 0.0f, 0.5f);
    state.steering_sensitivity =
      std::clamp(si.GetIntValue(section.c_str(), "SteeringSensitivity", 100) / 100.0f, 0.01f, 5.0f);
    state.accelerator_deadzone =
      std::clamp(si.GetIntValue(section.c_str(), "AcceleratorDeadzone", 0) / 100.0f, 0.0f, 0.5f);
    state.accelerator_sensitivity =
      std::clamp(si.GetIntValue(section.c_str(), "AcceleratorSensitivity", 100) / 100.0f, 0.01f, 5.0f);
    state.brake_deadzone =
      std::clamp(si.GetIntValue(section.c_str(), "BrakeDeadzone", 0) / 100.0f, 0.0f, 0.5f);
    state.brake_sensitivity =
      std::clamp(si.GetIntValue(section.c_str(), "BrakeSensitivity", 100) / 100.0f, 0.01f, 5.0f);
    state.clutch_deadzone =
      std::clamp(si.GetIntValue(section.c_str(), "ClutchDeadzone", 0) / 100.0f, 0.0f, 0.5f);
    state.clutch_sensitivity =
      std::clamp(si.GetIntValue(section.c_str(), "ClutchSensitivity", 100) / 100.0f, 0.01f, 5.0f);
    for (const char* key : {"Up", "Down", "Left", "Right", "Button1", "Button2", "Button3", "Button4",
                            "Button5", "Button6", "Coin", "Start", "Trigger", "Reload", "GSRIncrease",
                            "GSRDecrease", "GearUp", "GearDown", "Handbrake", "View", "Horn", "MusicNext",
                            "MusicPrevious", "Select", "SelectUp", "SelectDown", "SelectLeft", "SelectRight",
                            "Enter", "Sensor"})
    {
      AddDigitalBindings(si, section, port, key);
    }
    if (state.type == Arcade::ArcadeControllerType::Trackball)
    {
      state.trackball_buttons_as_axis = state.input_mode != "NativeRawInputDevice" &&
                                        si.GetBoolValue(section.c_str(), "UseButtonsAsAxis", false);
      if (state.trackball_buttons_as_axis)
      {
        AddTrackballButtonBindings(si, section, port, "TrackballLeft", true, false);
        AddTrackballButtonBindings(si, section, port, "TrackballRight", true, true);
        AddTrackballButtonBindings(si, section, port, "TrackballUp", false, false);
        AddTrackballButtonBindings(si, section, port, "TrackballDown", false, true);
      }
      else
      {
        AddAxisBindings(si, section, port, "TrackballX", true);
        AddAxisBindings(si, section, port, "TrackballY", false);
      }
    }
    else if (state.type == Arcade::ArcadeControllerType::Lightgun)
    {
      AddAxisBindings(si, section, port, "GunX", true);
      AddAxisBindings(si, section, port, "GunY", false);
    }
    else if (state.type == Arcade::ArcadeControllerType::Driving)
    {
      AddDrivingAxisBindings(si, section, port, "Steering", DrivingAxis::Steering);
      if (si.GetBoolValue(section.c_str(), "UseButtonsAsAxis", false))
      {
        AddDrivingSteeringButtonBindings(si, section, port, "SteeringLeft", false);
        AddDrivingSteeringButtonBindings(si, section, port, "SteeringRight", true);
      }
      AddDrivingAxisBindings(si, section, port, "Accelerator", DrivingAxis::Accelerator);
      AddDrivingAxisBindings(si, section, port, "Brake", DrivingAxis::Brake);
      AddDrivingAxisBindings(si, section, port, "Clutch", DrivingAxis::Clutch);
    }
    if (state.input_mode == "NativeRawInputDevice" && state.type == Arcade::ArcadeControllerType::Trackball)
    {
      InputManager::AddPointerMoveCallback(si.GetStringValue(section.c_str(), "PhysicalDevice", ""),
                                           [port](InputBindingKey key, float delta) { OnPointerMove(port, key, delta); });
    }
    else if (state.input_mode == "NativeRawInputDevice" && state.type == Arcade::ArcadeControllerType::Lightgun)
    {
      InputManager::AddPointerAbsoluteCallback(si.GetStringValue(section.c_str(), "PhysicalDevice", ""),
                                               [port](float x, float y) { OnPointerAbsolute(port, x, y); });
    }
  }
}

bool IsDigitalPressed(u32 port, std::string_view key)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  if (port >= s_ports.size())
    return false;

  const std::optional<Control> control = GetControl(key);
  if (!control.has_value())
    return false;

  // Direction consumers should observe the configured joystick topology.
  // Four-way layouts use the existing last-pressed arbitration; eight-way
  // and non-directional controls retain their previous raw pressed behavior.
  if (control.value() <= Control::Down)
    return GetFourWayDirection(s_ports[port], control.value());

  return IsPressed(s_ports[port], control.value());
}

bool HasDigitalControl(u32 port, std::string_view key)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  if (port >= s_ports.size())
    return false;

  const PortState& state = s_ports[port];
  if (state.layout.empty())
    return false;

  const Arcade::ArcadeControlLayoutInfo* layout = Arcade::GetArcadeControlLayoutInfo(state.layout);
  if (!layout)
    return false;

  return std::any_of(layout->bindings.begin(), layout->bindings.end(),
                     [key](const Arcade::ArcadeControlLayoutBindingInfo& binding) {
                       return binding.kind == Arcade::ArcadeControlBindingKind::Button &&
                              binding.binding_key == key;
                     });
}

float GetAnalogValue(u32 port, std::string_view key)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  if (port >= s_ports.size())
    return 0.0f;

  const PortState& state = s_ports[port];
  if (key == "Steering")
    return state.steering;
  if (key == "Accelerator")
    return state.accelerator;
  if (key == "Brake")
    return state.brake;
  if (key == "Clutch")
    return state.clutch;

  return 0.0f;
}

bool HasAnalogControl(u32 port, std::string_view key)
{
  std::lock_guard<std::mutex> lock(s_mutex);
  if (port >= s_ports.size())
    return false;

  const PortState& state = s_ports[port];
  if (state.type != Arcade::ArcadeControllerType::Driving || state.layout.empty())
    return false;

  const Arcade::ArcadeControlLayoutInfo* const layout = Arcade::GetArcadeControlLayoutInfo(state.layout);
  if (!layout)
    return false;

  for (const Arcade::ArcadeControlLayoutBindingInfo& binding : layout->bindings)
  {
    if (binding.kind == Arcade::ArcadeControlBindingKind::Axis && binding.binding_key == key)
      return true;
  }

  return false;
}

bool IsOperatorPressed(std::string_view key)
{
  std::lock_guard<std::mutex> lock(s_mutex);

  if (key == "Service")
    return s_operator.pressed_counts[static_cast<u32>(OperatorControl::Service)] != 0;
  if (key == "Test")
    return s_operator.pressed_counts[static_cast<u32>(OperatorControl::Test)] != 0;

  return false;
}

void Update()
{
  std::lock_guard<std::mutex> lock(s_mutex);
  for (u32 port = 0; port < Arcade::NUM_ARCADE_CONTROLLER_PORTS; port++)
  {
    PortState& state = s_ports[port];
    if (state.type == Arcade::ArcadeControllerType::Trackball && state.input_mode != "NativeRawInputDevice" && port < 2)
    {
      const float frame_seconds = 1.0f / std::max(System::GetThrottleFrequency(), 1.0f);
      const s32 delta_x = state.trackball_buttons_as_axis ?
        GetTrackballButtonDelta(state.trackball_button_axes[0], state.x_sensitivity, state.invert_x, frame_seconds) :
        static_cast<s32>(state.axis_x * state.x_sensitivity * (state.invert_x ? -128.0f : 128.0f));
      const s32 delta_y = state.trackball_buttons_as_axis ?
        GetTrackballButtonDelta(state.trackball_button_axes[1], state.y_sensitivity, state.invert_y, frame_seconds) :
        static_cast<s32>(state.axis_y * state.y_sensitivity * (state.invert_y ? -128.0f : 128.0f));
      if (SonyZN::IsMawasundaActive())
        SonyZN::AddMawasundaHandleDelta(port, delta_x);
      else if (NamcoSystem11::IsFamilyBowlActive())
        NamcoSystem11::AddFamilyBowlTrackballDelta(port, delta_x, delta_y);
      else
        Konami::AddGVTrackballDelta(port, delta_x, delta_y);
    }
  }
}

LightgunPresentationState GetLightgunPresentationState()
{
  std::lock_guard<std::mutex> lock(s_mutex);
  LightgunPresentationState result;
  result.sinden_border_enabled = s_sinden_border_enabled;
  result.sinden_border_width = s_sinden_border_width;
  for (u32 i = 0; i < NUM_LIGHTGUN_PRESENTATION_PORTS; i++)
  {
    const PortState& port = s_ports[i];
    LightgunPresentationPort& out = result.ports[i];
    out.active = (port.type == Arcade::ArcadeControllerType::Lightgun);
    out.crosshair_enabled = port.crosshair_enabled;
    out.offscreen = port.lightgun_offscreen || (port.offscreen_reload && IsPressed(port, Control::Reload));
    out.x = port.lightgun_x;
    out.y = port.lightgun_y;
    out.crosshair_scale = port.crosshair_scale;
    out.crosshair_image_path = port.crosshair_image_path;
  }
  return result;
}

} // namespace ArcadeInput
