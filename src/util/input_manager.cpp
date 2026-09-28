// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "input_manager.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "core/host.h"
#include "core/settings.h"
#include "core/system.h"
#include "imgui_manager.h"
#include "input_source.h"

#include "IconsPromptFont.h"

#include "fmt/core.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <variant>
#include <vector>

Log_SetChannel(InputManager);

namespace {

// ------------------------------------------------------------------------
// Constants
// ------------------------------------------------------------------------

enum : u32
{
  MAX_KEYS_PER_BINDING = 4,
  FIRST_EXTERNAL_INPUT_SOURCE = static_cast<u32>(InputSourceType::Pointer) + 1u,
  LAST_EXTERNAL_INPUT_SOURCE = static_cast<u32>(InputSourceType::Count),
};

// ------------------------------------------------------------------------
// Binding Type
// ------------------------------------------------------------------------
// This class tracks both the keys which make it up (for chords), as well
// as the state of all buttons. For button callbacks, it's fired when
// all keys go active, and for axis callbacks, when all are active and
// the value changes.

struct InputBinding
{
  InputBindingKey keys[MAX_KEYS_PER_BINDING] = {};
  InputEventHandler handler;
  u8 num_keys = 0;
  u8 full_mask = 0;
  u8 current_mask = 0;
};

} // namespace

// ------------------------------------------------------------------------
// Forward Declarations (for static qualifier)
// ------------------------------------------------------------------------
namespace InputManager {
static std::optional<InputBindingKey> ParseHostKeyboardKey(std::string_view source, std::string_view sub_binding);
static std::optional<InputBindingKey> ParsePointerKey(std::string_view source, std::string_view sub_binding);
static std::optional<InputBindingKey> ParseSensorKey(std::string_view source, std::string_view sub_binding);

static std::vector<std::string_view> SplitChord(std::string_view binding);
static bool SplitBinding(std::string_view binding, std::string_view* source, std::string_view* sub_binding);
static void PrettifyInputBindingPart(std::string_view binding, SmallString& ret, bool& changed);
static void AddBindings(const std::vector<std::string>& bindings, const InputEventHandler& handler);
static void UpdatePointerCount();

static bool IsAxisHandler(const InputEventHandler& handler);

static void AddHotkeyBindings(SettingsInterface& si);
static void GenerateRelativeMouseEvents();

static bool DoEventHook(InputBindingKey key, float value);
static bool PreprocessEvent(InputBindingKey key, float value, GenericInputBinding generic_key);
static bool ProcessEvent(InputBindingKey key, float value, bool skip_button_handlers);

static void UpdateInputSourceState(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock,
                                   InputSourceType type, std::unique_ptr<InputSource> (*factory_function)());
} // namespace InputManager

// ------------------------------------------------------------------------
// Local Variables
// ------------------------------------------------------------------------

// This is a multimap containing any binds related to the specified key.
using BindingMap = std::unordered_multimap<InputBindingKey, std::shared_ptr<InputBinding>, InputBindingKeyHash>;
static BindingMap s_binding_map;
static std::mutex s_binding_map_write_lock;

// Hooks/intercepting (for setting bindings)
static std::mutex m_event_intercept_mutex;
static InputInterceptHook::Callback m_event_intercept_callback;

// Input sources. Keyboard/mouse don't exist here.
static std::array<std::unique_ptr<InputSource>, static_cast<u32>(InputSourceType::Count)> s_input_sources;

// ------------------------------------------------------------------------
// Hotkeys
// ------------------------------------------------------------------------

static const HotkeyInfo* const s_hotkey_list[] = {g_common_hotkeys, g_host_hotkeys};

// ------------------------------------------------------------------------
// Tracking host mouse movement and turning into relative events
// 4 axes: pointer left/right, wheel vertical/horizontal. Last/Next/Normalized.
// ------------------------------------------------------------------------
static constexpr const std::array<const char*, static_cast<u8>(InputPointerAxis::Count)> s_pointer_axis_names = {
  {"X", "Y", "WheelX", "WheelY"}};
static constexpr const std::array<const char*, 3> s_pointer_button_names = {
  {"LeftButton", "RightButton", "MiddleButton"}};
static constexpr const std::array<const char*, 3> s_sensor_accelerometer_names = {{"Turn", "Tilt", "Rotate"}};

struct PointerAxisState
{
  std::atomic<s32> delta;
  float last_value;
};
static std::array<std::array<float, static_cast<u8>(InputPointerAxis::Count)>, InputManager::MAX_POINTER_DEVICES>
  s_host_pointer_positions;
static std::array<std::array<PointerAxisState, static_cast<u8>(InputPointerAxis::Count)>,
                  InputManager::MAX_POINTER_DEVICES>
  s_pointer_state;
static u32 s_pointer_count = 0;
static std::array<float, static_cast<u8>(InputPointerAxis::Count)> s_pointer_axis_scale;

using PointerMoveCallback = std::function<void(InputBindingKey key, float value)>;
static std::vector<std::pair<u32, PointerMoveCallback>> s_pointer_move_callbacks;
static std::vector<std::pair<std::string, PointerMoveCallback>> s_pointer_identifier_move_callbacks;
static std::vector<std::pair<std::string, std::function<void(float, float)>>> s_pointer_identifier_absolute_callbacks;
static std::mutex s_pointer_absolute_rect_mutex;
static std::array<float, 4> s_pointer_absolute_display_rect = {};
static std::array<std::array<float, 2>, InputManager::MAX_POINTER_DEVICES> s_raw_pointer_virtual_positions = {};
static std::array<bool, InputManager::MAX_POINTER_DEVICES> s_raw_pointer_virtual_position_initialized = {};
static std::mutex s_raw_pointer_virtual_position_mutex;

// Window size, used for clamping the mouse position in raw input modes.
static std::array<float, 2> s_window_size = {};
static bool s_relative_mouse_mode = false;
static bool s_relative_mouse_mode_active = false;
static bool s_hide_host_mouse_cursor = false;
static bool s_hide_host_mouse_cusor_active = false;

// ------------------------------------------------------------------------
// Binding Parsing
// ------------------------------------------------------------------------

std::vector<std::string_view> InputManager::SplitChord(std::string_view binding)
{
  std::vector<std::string_view> parts;

  // under an if for RVO
  if (!binding.empty())
  {
    std::string_view::size_type last = 0;
    std::string_view::size_type next;
    while ((next = binding.find('&', last)) != std::string_view::npos)
    {
      if (last != next)
      {
        std::string_view part(StringUtil::StripWhitespace(binding.substr(last, next - last)));
        if (!part.empty())
          parts.push_back(std::move(part));
      }
      last = next + 1;
    }
    if (last < (binding.size() - 1))
    {
      std::string_view part(StringUtil::StripWhitespace(binding.substr(last)));
      if (!part.empty())
        parts.push_back(std::move(part));
    }
  }

  return parts;
}

bool InputManager::SplitBinding(std::string_view binding, std::string_view* source, std::string_view* sub_binding)
{
  const std::string_view::size_type slash_pos = binding.find('/');
  if (slash_pos == std::string_view::npos)
  {
    WARNING_LOG("Malformed binding: '{}'", binding);
    return false;
  }

  *source = std::string_view(binding).substr(0, slash_pos);
  *sub_binding = std::string_view(binding).substr(slash_pos + 1);
  return true;
}

std::optional<InputBindingKey> InputManager::ParseInputBindingKey(std::string_view binding)
{
  std::string_view source, sub_binding;
  if (!SplitBinding(binding, &source, &sub_binding))
    return std::nullopt;

  // lameee, string matching
  if (source.starts_with("Keyboard"))
  {
    return ParseHostKeyboardKey(source, sub_binding);
  }
  else if (source.starts_with("Pointer"))
  {
    return ParsePointerKey(source, sub_binding);
  }
  else if (source.starts_with("Sensor"))
  {
    return ParseSensorKey(source, sub_binding);
  }
  else
  {
    for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
    {
      if (s_input_sources[i])
      {
        std::optional<InputBindingKey> key = s_input_sources[i]->ParseKeyString(source, sub_binding);
        if (key.has_value())
          return key;
      }
    }
  }

  return std::nullopt;
}

std::string InputManager::ConvertInputBindingKeyToString(InputBindingInfo::Type binding_type, InputBindingKey key)
{
  if (binding_type == InputBindingInfo::Type::Pointer)
  {
    // pointer and device bindings don't have a data part
    if (key.source_type == InputSourceType::Pointer)
    {
      return GetPointerDeviceName(key.source_index);
    }
    else if (key.source_type < InputSourceType::Count && s_input_sources[static_cast<u32>(key.source_type)])
    {
      // This assumes that it always follows the Type/Binding form.
      std::string keystr(s_input_sources[static_cast<u32>(key.source_type)]->ConvertKeyToString(key));
      std::string::size_type pos = keystr.find('/');
      if (pos != std::string::npos)
        keystr.erase(pos);
      return keystr;
    }
  }
  else
  {
    if (key.source_type == InputSourceType::Keyboard)
    {
      const std::optional<std::string> str(ConvertHostKeyboardCodeToString(key.data));
      if (str.has_value() && !str->empty())
        return fmt::format("Keyboard/{}", str->c_str());
    }
    else if (key.source_type == InputSourceType::Pointer)
    {
      if (key.source_subtype == InputSubclass::PointerButton)
      {
        if (key.data < s_pointer_button_names.size())
          return fmt::format("Pointer-{}/{}", u32{key.source_index}, s_pointer_button_names[key.data]);
        else
          return fmt::format("Pointer-{}/Button{}", u32{key.source_index}, key.data);
      }
      else if (key.source_subtype == InputSubclass::PointerAxis)
      {
        return fmt::format("Pointer-{}/{}{:c}", u32{key.source_index}, s_pointer_axis_names[key.data],
                           key.modifier == InputModifier::Negate ? '-' : '+');
      }
    }
    else if (key.source_type < InputSourceType::Count && s_input_sources[static_cast<u32>(key.source_type)])
    {
      return std::string(s_input_sources[static_cast<u32>(key.source_type)]->ConvertKeyToString(key));
    }
  }

  return {};
}

std::string InputManager::ConvertInputBindingKeysToString(InputBindingInfo::Type binding_type,
                                                          const InputBindingKey* keys, size_t num_keys)
{
  // can't have a chord of devices/pointers
  if (binding_type == InputBindingInfo::Type::Pointer)
  {
    // so only take the first
    if (num_keys > 0)
      return ConvertInputBindingKeyToString(binding_type, keys[0]);
  }

  std::stringstream ss;
  for (size_t i = 0; i < num_keys; i++)
  {
    const std::string keystr(ConvertInputBindingKeyToString(binding_type, keys[i]));
    if (keystr.empty())
      return std::string();

    if (i > 0)
      ss << " & ";

    ss << keystr;
  }

  return ss.str();
}

bool InputManager::PrettifyInputBinding(SmallStringBase& binding)
{
  if (binding.empty())
    return false;

  const std::string_view binding_view = binding.view();

  SmallString ret;
  bool changed = false;

  std::string_view::size_type last = 0;
  std::string_view::size_type next;
  while ((next = binding_view.find('&', last)) != std::string_view::npos)
  {
    if (last != next)
    {
      const std::string_view part = StringUtil::StripWhitespace(binding_view.substr(last, next - last));
      if (!part.empty())
      {
        if (!ret.empty())
          ret.append(" + ");
        PrettifyInputBindingPart(part, ret, changed);
      }
    }
    last = next + 1;
  }
  if (last < (binding_view.size() - 1))
  {
    const std::string_view part = StringUtil::StripWhitespace(binding_view.substr(last));
    if (!part.empty())
    {
      if (!ret.empty())
        ret.append(" + ");
      PrettifyInputBindingPart(part, ret, changed);
    }
  }

  if (changed)
    binding = ret;

  return changed;
}

void InputManager::PrettifyInputBindingPart(const std::string_view binding, SmallString& ret, bool& changed)
{
  std::string_view source, sub_binding;
  if (!SplitBinding(binding, &source, &sub_binding))
    return;

  // lameee, string matching
  if (source.starts_with("Keyboard"))
  {
    std::optional<InputBindingKey> key = ParseHostKeyboardKey(source, sub_binding);
    const char* icon = key.has_value() ? ConvertHostKeyboardCodeToIcon(key->data) : nullptr;
    if (icon)
    {
      ret.append(icon);
      changed = true;
      return;
    }
  }
  else if (source.starts_with("Pointer"))
  {
    const std::optional<InputBindingKey> key = ParsePointerKey(source, sub_binding);
    if (key.has_value())
    {
      if (key->source_subtype == InputSubclass::PointerButton)
      {
        static constexpr const char* button_icons[] = {
          ICON_PF_MOUSE_BUTTON_1, ICON_PF_MOUSE_BUTTON_2, ICON_PF_MOUSE_BUTTON_3,
          ICON_PF_MOUSE_BUTTON_4, ICON_PF_MOUSE_BUTTON_5,
        };
        if (key->data < std::size(button_icons))
        {
          ret.append(button_icons[key->data]);
          changed = true;
          return;
        }
      }
    }
  }
  else if (source.starts_with("Sensor"))
  {
  }
  else
  {
    for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
    {
      if (s_input_sources[i])
      {
        std::optional<InputBindingKey> key = s_input_sources[i]->ParseKeyString(source, sub_binding);
        if (key.has_value())
        {
          const TinyString icon = s_input_sources[i]->ConvertKeyToIcon(key.value());
          if (!icon.empty())
          {
            ret.append(icon);
            changed = true;
            return;
          }

          break;
        }
      }
    }
  }

  ret.append(binding);
}

void InputManager::AddBindings(const std::vector<std::string>& bindings, const InputEventHandler& handler)
{
  for (const std::string& binding : bindings)
    AddBinding(binding, handler);
}

void InputManager::AddBinding(std::string_view binding, const InputEventHandler& handler)
{
  std::shared_ptr<InputBinding> ibinding;
  const std::vector<std::string_view> chord_bindings(SplitChord(binding));

  for (const std::string_view& chord_binding : chord_bindings)
  {
    std::optional<InputBindingKey> key = ParseInputBindingKey(chord_binding);
    if (!key.has_value())
    {
      ERROR_LOG("Invalid binding: '{}'", binding);
      ibinding.reset();
      break;
    }

    if (!ibinding)
    {
      ibinding = std::make_shared<InputBinding>();
      ibinding->handler = handler;
    }

    if (ibinding->num_keys == MAX_KEYS_PER_BINDING)
    {
      ERROR_LOG("Too many chord parts, max is {} ({})", static_cast<unsigned>(MAX_KEYS_PER_BINDING), binding.size());
      ibinding.reset();
      break;
    }

    ibinding->keys[ibinding->num_keys] = key.value();
    ibinding->full_mask |= (static_cast<u8>(1) << ibinding->num_keys);
    ibinding->num_keys++;
  }

  if (!ibinding)
    return;

  // plop it in the input map for all the keys
  for (u32 i = 0; i < ibinding->num_keys; i++)
    s_binding_map.emplace(ibinding->keys[i].MaskDirection(), ibinding);
}

void InputManager::AddPointerMoveCallback(std::string_view device,
                                          std::function<void(InputBindingKey key, float value)> callback)
{
  const std::optional<u32> index(GetIndexFromPointerBinding(device));
  if (index.has_value())
    s_pointer_move_callbacks.emplace_back(index.value(), std::move(callback));
  else if (!device.empty())
    s_pointer_identifier_move_callbacks.emplace_back(std::string(device), std::move(callback));
}

void InputManager::InvokePointerMoveCallbacks(std::string_view device, InputBindingKey key, float value)
{
  for (const auto& [identifier, callback] : s_pointer_identifier_move_callbacks)
  {
    if (identifier == device)
      callback(key, value);
  }
}

void InputManager::AddPointerAbsoluteCallback(std::string_view device, std::function<void(float x, float y)> callback)
{
  if (!device.empty())
    s_pointer_identifier_absolute_callbacks.emplace_back(std::string(device), std::move(callback));
}

void InputManager::InvokePointerAbsoluteCallbacks(std::string_view device, float x, float y)
{
  {
    std::lock_guard<std::mutex> lock(s_pointer_absolute_rect_mutex);
    const float left = s_pointer_absolute_display_rect[0], top = s_pointer_absolute_display_rect[1];
    const float right = s_pointer_absolute_display_rect[2], bottom = s_pointer_absolute_display_rect[3];
    if (right > left && bottom > top)
    {
      x = (x - left) / (right - left);
      y = (y - top) / (bottom - top);
    }
  }
  for (const auto& [identifier, callback] : s_pointer_identifier_absolute_callbacks)
  {
    if (identifier == device)
      callback(x, y);
  }
}

void InputManager::InvokePointerAbsoluteCallbacksLocal(std::string_view device, float x, float y)
{
  for (const auto& [identifier, callback] : s_pointer_identifier_absolute_callbacks)
  {
    if (identifier == device)
      callback(x, y);
  }
}

void InputManager::SetPointerAbsoluteDisplayRect(float left, float top, float right, float bottom)
{
  std::lock_guard<std::mutex> lock(s_pointer_absolute_rect_mutex);
  s_pointer_absolute_display_rect = {{left, top, right, bottom}};
}

bool InputManager::SetPointerAbsolutePosition(std::string_view device, float x, float y)
{
  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i] && s_input_sources[i]->SetPointerAbsolutePosition(device, x, y))
      return true;
  }

  return false;
}
void InputManager::ResetRawPointerVirtualPosition(u32 index)
{
  if (index >= MAX_POINTER_DEVICES)
    return;

  std::lock_guard<std::mutex> lock(s_raw_pointer_virtual_position_mutex);
  s_raw_pointer_virtual_positions[index] = {{0.5f, 0.5f}};
  s_raw_pointer_virtual_position_initialized[index] = true;
}

void InputManager::SetRawPointerVirtualPosition(u32 index, float x, float y)
{
  if (index >= MAX_POINTER_DEVICES)
    return;

  std::lock_guard<std::mutex> lock(s_raw_pointer_virtual_position_mutex);
  s_raw_pointer_virtual_positions[index] = {{
    std::clamp(x, -0.25f, 1.25f),
    std::clamp(y, -0.25f, 1.25f),
  }};
  s_raw_pointer_virtual_position_initialized[index] = true;
}
std::pair<float, float> InputManager::UpdateRawPointerVirtualPosition(u32 index, float delta_x, float delta_y)
{
  if (index >= MAX_POINTER_DEVICES)
    return {0.5f, 0.5f};

  std::lock_guard<std::mutex> lock(s_raw_pointer_virtual_position_mutex);
  auto& position = s_raw_pointer_virtual_positions[index];
  if (!s_raw_pointer_virtual_position_initialized[index])
  {
    position = {{0.5f, 0.5f}};
    s_raw_pointer_virtual_position_initialized[index] = true;
  }

  if (s_window_size[0] <= 0.0f || s_window_size[1] <= 0.0f)
    return {position[0], position[1]};

  position[0] = std::clamp(position[0] + (delta_x / s_window_size[0]), -0.25f, 1.25f);
  position[1] = std::clamp(position[1] + (delta_y / s_window_size[1]), -0.25f, 1.25f);
  return {position[0], position[1]};
}

// ------------------------------------------------------------------------
// Key Decoders
// ------------------------------------------------------------------------

InputBindingKey InputManager::MakeHostKeyboardKey(u32 key_code)
{
  InputBindingKey key = {};
  key.source_type = InputSourceType::Keyboard;
  key.data = key_code;
  return key;
}

InputBindingKey InputManager::MakePointerButtonKey(u32 index, u32 button_index)
{
  InputBindingKey key = {};
  key.source_index = index;
  key.source_type = InputSourceType::Pointer;
  key.source_subtype = InputSubclass::PointerButton;
  key.data = button_index;
  return key;
}

InputBindingKey InputManager::MakePointerAxisKey(u32 index, InputPointerAxis axis)
{
  InputBindingKey key = {};
  key.data = static_cast<u32>(axis);
  key.source_index = index;
  key.source_type = InputSourceType::Pointer;
  key.source_subtype = InputSubclass::PointerAxis;
  return key;
}

InputBindingKey InputManager::MakeSensorAxisKey(InputSubclass sensor, u32 axis)
{
  InputBindingKey key = {};
  key.data = static_cast<u32>(axis);
  key.source_index = 0;
  key.source_type = InputSourceType::Sensor;
  key.source_subtype = sensor;
  return key;
}

// ------------------------------------------------------------------------
// Bind Encoders
// ------------------------------------------------------------------------

static std::array<const char*, static_cast<u32>(InputSourceType::Count)> s_input_class_names = {{
  "Keyboard",
  "Pointer",
  "Sensor",
#ifdef _WIN32
  "DInput",
  "XInput",
#endif
  "SDL",
  "RawInput",
}};

InputSource* InputManager::GetInputSourceInterface(InputSourceType type)
{
  return s_input_sources[static_cast<u32>(type)].get();
}

const char* InputManager::InputSourceToString(InputSourceType clazz)
{
  return s_input_class_names[static_cast<u32>(clazz)];
}

bool InputManager::GetInputSourceDefaultEnabled(InputSourceType type)
{
  switch (type)
  {
    case InputSourceType::Keyboard:
    case InputSourceType::Pointer:
      return true;

#ifdef _WIN32
    case InputSourceType::DInput:
      return false;

    case InputSourceType::XInput:
      return false;
#endif

    case InputSourceType::SDL:
      return true;
    case InputSourceType::RawInput:
      return false;

    default:
      return false;
  }
}

std::optional<InputSourceType> InputManager::ParseInputSourceString(std::string_view str)
{
  for (u32 i = 0; i < static_cast<u32>(InputSourceType::Count); i++)
  {
    if (str == s_input_class_names[i])
      return static_cast<InputSourceType>(i);
  }

  return std::nullopt;
}

std::optional<InputBindingKey> InputManager::ParseHostKeyboardKey(std::string_view source, std::string_view sub_binding)
{
  if (source != "Keyboard")
    return std::nullopt;

  const std::optional<s32> code = ConvertHostKeyboardStringToCode(sub_binding);
  if (!code.has_value())
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::Keyboard;
  key.data = static_cast<u32>(code.value());
  return key;
}

std::optional<InputBindingKey> InputManager::ParsePointerKey(std::string_view source, std::string_view sub_binding)
{
  const std::optional<s32> pointer_index = StringUtil::FromChars<s32>(source.substr(8));
  if (!pointer_index.has_value() || pointer_index.value() < 0)
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::Pointer;
  key.source_index = static_cast<u32>(pointer_index.value());

  if (sub_binding.starts_with("Button"))
  {
    const std::optional<s32> button_number = StringUtil::FromChars<s32>(sub_binding.substr(6));
    if (!button_number.has_value() || button_number.value() < 0)
      return std::nullopt;

    key.source_subtype = InputSubclass::PointerButton;
    key.data = static_cast<u32>(button_number.value());
    return key;
  }

  for (u32 i = 0; i < s_pointer_axis_names.size(); i++)
  {
    if (sub_binding.starts_with(s_pointer_axis_names[i]))
    {
      key.source_subtype = InputSubclass::PointerAxis;
      key.data = i;

      const std::string_view dir_part(sub_binding.substr(std::strlen(s_pointer_axis_names[i])));
      if (dir_part == "+")
        key.modifier = InputModifier::None;
      else if (dir_part == "-")
        key.modifier = InputModifier::Negate;
      else
        return std::nullopt;

      return key;
    }
  }

  for (u32 i = 0; i < s_pointer_button_names.size(); i++)
  {
    if (sub_binding == s_pointer_button_names[i])
    {
      key.source_subtype = InputSubclass::PointerButton;
      key.data = i;
      return key;
    }
  }

  return std::nullopt;
}

std::optional<u32> InputManager::GetIndexFromPointerBinding(std::string_view source)
{
  if (!source.starts_with("Pointer-"))
    return std::nullopt;

  const std::optional<s32> pointer_index = StringUtil::FromChars<s32>(source.substr(8));
  if (!pointer_index.has_value() || pointer_index.value() < 0)
    return std::nullopt;

  return static_cast<u32>(pointer_index.value());
}

std::string InputManager::GetPointerDeviceName(u32 pointer_index)
{
  return fmt::format("Pointer-{}", pointer_index);
}

std::optional<InputBindingKey> InputManager::ParseSensorKey(std::string_view source, std::string_view sub_binding)
{
  if (source != "Sensor")
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::Sensor;
  key.source_index = 0;

  for (u32 i = 0; i < s_sensor_accelerometer_names.size(); i++)
  {
    if (sub_binding.starts_with(s_sensor_accelerometer_names[i]))
    {
      key.source_subtype = InputSubclass::SensorAccelerometer;
      key.data = i;

      const std::string_view dir_part(sub_binding.substr(std::strlen(s_sensor_accelerometer_names[i])));
      if (dir_part == "+")
        key.modifier = InputModifier::None;
      else if (dir_part == "-")
        key.modifier = InputModifier::Negate;
      else
        return std::nullopt;

      return key;
    }
  }

  return std::nullopt;
}

// ------------------------------------------------------------------------
// Binding Enumeration
// ------------------------------------------------------------------------

std::vector<const HotkeyInfo*> InputManager::GetHotkeyList()
{
  std::vector<const HotkeyInfo*> ret;
  for (const HotkeyInfo* hotkey_list : s_hotkey_list)
  {
    for (const HotkeyInfo* hotkey = hotkey_list; hotkey->name != nullptr; hotkey++)
      ret.push_back(hotkey);
  }
  return ret;
}

void InputManager::AddHotkeyBindings(SettingsInterface& si)
{
  for (const HotkeyInfo* hotkey_list : s_hotkey_list)
  {
    for (const HotkeyInfo* hotkey = hotkey_list; hotkey->name != nullptr; hotkey++)
    {
      const std::vector<std::string> bindings(si.GetStringList("Hotkeys", hotkey->name));
      if (bindings.empty())
        continue;

      AddBindings(bindings, InputButtonEventHandler{hotkey->handler});
    }
  }
}

// ------------------------------------------------------------------------
// Event Handling
// ------------------------------------------------------------------------

bool InputManager::HasAnyBindingsForKey(InputBindingKey key)
{
  std::unique_lock lock(s_binding_map_write_lock);
  return (s_binding_map.find(key.MaskDirection()) != s_binding_map.end());
}

bool InputManager::HasAnyBindingsForSource(InputBindingKey key)
{
  std::unique_lock lock(s_binding_map_write_lock);
  for (const auto& it : s_binding_map)
  {
    const InputBindingKey& okey = it.first;
    if (okey.source_type == key.source_type && okey.source_index == key.source_index &&
        okey.source_subtype == key.source_subtype)
    {
      return true;
    }
  }

  return false;
}

bool InputManager::IsAxisHandler(const InputEventHandler& handler)
{
  return std::holds_alternative<InputAxisEventHandler>(handler);
}

bool InputManager::InvokeEvents(InputBindingKey key, float value, GenericInputBinding generic_key)
{
  // Raw Input emits per-device mouse buttons. Suppress the matching merged Pointer event.
  if (key.source_type == InputSourceType::Pointer && key.source_subtype == InputSubclass::PointerButton && IsUsingRawInput())
    return false;

  if (DoEventHook(key, value))
    return true;

  // If imgui ate the event, don't fire our handlers.
  const bool skip_button_handlers = PreprocessEvent(key, value, generic_key);
  return ProcessEvent(key, value, skip_button_handlers);
}

bool InputManager::ProcessEvent(InputBindingKey key, float value, bool skip_button_handlers)
{
  // find all the bindings associated with this key
  const InputBindingKey masked_key = key.MaskDirection();
  const auto range = s_binding_map.equal_range(masked_key);
  if (range.first == s_binding_map.end())
    return false;

  // Now we can actually fire/activate bindings.
  u32 min_num_keys = 0;
  for (auto it = range.first; it != range.second; ++it)
  {
    InputBinding* binding = it->second.get();

    // find the key which matches us
    for (u32 i = 0; i < binding->num_keys; i++)
    {
      if (binding->keys[i].MaskDirection() != masked_key)
        continue;

      const u8 bit = static_cast<u8>(1) << i;
      const bool negative = binding->keys[i].modifier == InputModifier::Negate;
      const bool new_state = (negative ? (value < 0.0f) : (value > 0.0f));

      float value_to_pass = 0.0f;
      switch (binding->keys[i].modifier)
      {
        case InputModifier::None:
          if (value > 0.0f)
            value_to_pass = value;
          break;
        case InputModifier::Negate:
          if (value < 0.0f)
            value_to_pass = -value;
          break;
        case InputModifier::FullAxis:
          value_to_pass = value * 0.5f + 0.5f;
          break;
      }

      // handle inverting, needed for some wheels.
      value_to_pass = binding->keys[i].invert ? (1.0f - value_to_pass) : value_to_pass;

      // axes are fired regardless of a state change, unless they're zero
      // (but going from not-zero to zero will still fire, because of the full state)
      // for buttons, we can use the state of the last chord key, because it'll be 1 on press,
      // and 0 on release (when the full state changes).
      if (IsAxisHandler(binding->handler))
      {
        if (value_to_pass >= 0.0f && (!skip_button_handlers || value_to_pass == 0.0f))
          std::get<InputAxisEventHandler>(binding->handler)(value_to_pass);
      }
      else if (binding->num_keys >= min_num_keys)
      {
        // update state based on whether the whole chord was activated
        const u8 new_mask =
          ((new_state && !skip_button_handlers) ? (binding->current_mask | bit) : (binding->current_mask & ~bit));
        const bool prev_full_state = (binding->current_mask == binding->full_mask);
        const bool new_full_state = (new_mask == binding->full_mask);
        binding->current_mask = new_mask;

        // Workaround for multi-key bindings that share the same keys.
        if (binding->num_keys > 1 && new_full_state && prev_full_state != new_full_state && range.first != range.second)
        {
          // Because the binding map isn't ordered, we could iterate in the order of Shift+F1 and then
          // F1, which would mean that F1 wouldn't get cancelled and still activate. So, to handle this
          // case, we skip activating any future bindings with a fewer number of keys.
          min_num_keys = std::max<u32>(min_num_keys, binding->num_keys);

          // Basically, if we bind say, F1 and Shift+F1, and press shift and then F1, we'll fire bindings
          // for both F1 and Shift+F1, when we really only want to fire the binding for Shift+F1. So,
          // when we activate a multi-key chord (key press), we go through the binding map for all the
          // other keys in the chord, and cancel them if they have a shorter chord. If they're longer,
          // they could still activate and take precedence over us, so we leave them alone.
          for (u32 j = 0; j < binding->num_keys; j++)
          {
            const auto range2 = s_binding_map.equal_range(binding->keys[j].MaskDirection());
            for (auto it2 = range2.first; it2 != range2.second; ++it2)
            {
              InputBinding* other_binding = it2->second.get();
              if (other_binding == binding || IsAxisHandler(other_binding->handler) ||
                  other_binding->num_keys >= binding->num_keys)
              {
                continue;
              }

              // We only need to cancel the binding if it was fully active before. Which in the above
              // case of Shift+F1 / F1, it will be.
              if (other_binding->current_mask == other_binding->full_mask)
                std::get<InputButtonEventHandler>(other_binding->handler)(-1);

              // Zero out the current bits so that we don't release this binding, if the other part
              // of the chord releases first.
              other_binding->current_mask = 0;
            }
          }
        }

        if (prev_full_state != new_full_state && binding->num_keys >= min_num_keys)
        {
          const s32 pressed = skip_button_handlers ? -1 : static_cast<s32>(value_to_pass > 0.0f);
          std::get<InputButtonEventHandler>(binding->handler)(pressed);
        }
      }

      // bail out, since we shouldn't have the same key twice in the chord
      break;
    }
  }

  return true;
}

void InputManager::ClearBindStateFromSource(InputBindingKey key)
{
  // Why are we doing it this way? Because any of the bindings could cause a reload and invalidate our iterators :(.
  // Axis handlers should be fine, so we'll do those as a first pass.
  for (const auto& [match_key, binding] : s_binding_map)
  {
    if (key.source_type != match_key.source_type || key.source_subtype != match_key.source_subtype ||
        key.source_index != match_key.source_index || !IsAxisHandler(binding->handler))
    {
      continue;
    }

    for (u32 i = 0; i < binding->num_keys; i++)
    {
      if (binding->keys[i].MaskDirection() != match_key)
        continue;

      std::get<InputAxisEventHandler>(binding->handler)(0.0f);
      break;
    }
  }

  // Now go through the button handlers, and pick them off.
  bool matched;
  do
  {
    matched = false;

    for (const auto& [match_key, binding] : s_binding_map)
    {
      if (key.source_type != match_key.source_type || key.source_subtype != match_key.source_subtype ||
          key.source_index != match_key.source_index || IsAxisHandler(binding->handler))
      {
        continue;
      }

      for (u32 i = 0; i < binding->num_keys; i++)
      {
        if (binding->keys[i].MaskDirection() != match_key)
          continue;

        // Skip if we weren't pressed.
        const u8 bit = static_cast<u8>(1) << i;
        if ((binding->current_mask & bit) == 0)
          continue;

        // Only fire handler if we're changing from active state.
        const u8 current_mask = binding->current_mask;
        binding->current_mask &= ~bit;

        if (current_mask == binding->full_mask)
        {
          std::get<InputButtonEventHandler>(binding->handler)(0);
          matched = true;
          break;
        }
      }

      // Need to start again, might've reloaded.
      if (matched)
        break;
    }
  } while (matched);
}

bool InputManager::PreprocessEvent(InputBindingKey key, float value, GenericInputBinding generic_key)
{
  // does imgui want the event?
  if (key.source_type == InputSourceType::Keyboard)
  {
    if (ImGuiManager::ProcessHostKeyEvent(key, value))
      return true;
  }
  else if (key.source_type == InputSourceType::Pointer && key.source_subtype == InputSubclass::PointerButton)
  {
    if (ImGuiManager::ProcessPointerButtonEvent(key, value))
      return true;
  }
  else if (generic_key != GenericInputBinding::Unknown)
  {
    if (ImGuiManager::ProcessGenericInputEvent(generic_key, value) && value != 0.0f)
      return true;
  }

  return false;
}

void InputManager::GenerateRelativeMouseEvents()
{
  const bool system_running = System::IsRunning();

  for (u32 device = 0; device < s_pointer_count; device++)
  {
    for (u32 axis = 0; axis < static_cast<u32>(static_cast<u8>(InputPointerAxis::Count)); axis++)
    {
      PointerAxisState& state = s_pointer_state[device][axis];
      const float delta = static_cast<float>(state.delta.exchange(0, std::memory_order_acquire)) / 65536.0f;
      if (delta == 0.0f)
        continue;

      const float unclamped_value = delta * s_pointer_axis_scale[axis];

      const InputBindingKey key(MakePointerAxisKey(device, static_cast<InputPointerAxis>(axis)));
      if (axis >= static_cast<u32>(InputPointerAxis::WheelX) &&
          ImGuiManager::ProcessPointerAxisEvent(key, unclamped_value))
      {
        continue;
      }

      if (!system_running)
        continue;

      const float value = std::clamp(unclamped_value, -1.0f, 1.0f);
      if (value != state.last_value)
      {
        state.last_value = value;
        InvokeEvents(key, value, GenericInputBinding::Unknown);
      }

      if (delta != 0.0f)
      {
        for (const std::pair<u32, PointerMoveCallback>& pmc : s_pointer_move_callbacks)
        {
          if (pmc.first == device)
            pmc.second(key, delta);
        }
      }
    }
  }
}

void InputManager::UpdatePointerCount()
{
  if (!IsUsingRawInput())
  {
    s_pointer_count = 1;
    return;
  }

  InputSource* ris = GetInputSourceInterface(InputSourceType::RawInput);
  DebugAssert(ris);

  s_pointer_count = 0;
  s_pointer_count = std::min<u32>(static_cast<u32>(ris->EnumeratePointerDevices().size()), MAX_POINTER_DEVICES);
}

u32 InputManager::GetPointerCount()
{
  return s_pointer_count;
}

std::pair<float, float> InputManager::GetPointerAbsolutePosition(u32 index)
{
  DebugAssert(index < s_host_pointer_positions.size());
  return std::make_pair(s_host_pointer_positions[index][static_cast<u8>(InputPointerAxis::X)],
                        s_host_pointer_positions[index][static_cast<u8>(InputPointerAxis::Y)]);
}

void InputManager::UpdatePointerAbsolutePosition(u32 index, float x, float y)
{
  if (index >= MAX_POINTER_DEVICES || s_relative_mouse_mode_active) [[unlikely]]
    return;

  const float dx = x - std::exchange(s_host_pointer_positions[index][static_cast<u8>(InputPointerAxis::X)], x);
  const float dy = y - std::exchange(s_host_pointer_positions[index][static_cast<u8>(InputPointerAxis::Y)], y);

  if (dx != 0.0f)
  {
    s_pointer_state[index][static_cast<u8>(InputPointerAxis::X)].delta.fetch_add(static_cast<s32>(dx * 65536.0f),
                                                                                 std::memory_order_release);
  }
  if (dy != 0.0f)
  {
    s_pointer_state[index][static_cast<u8>(InputPointerAxis::Y)].delta.fetch_add(static_cast<s32>(dy * 65536.0f),
                                                                                 std::memory_order_release);
  }

  if (index == 0)
    ImGuiManager::UpdateMousePosition(x, y);
}

void InputManager::UpdatePointerRelativeDelta(u32 index, InputPointerAxis axis, float d, bool raw_input)
{
  if (index >= MAX_POINTER_DEVICES || (axis < InputPointerAxis::WheelX && !s_relative_mouse_mode_active))
    return;

  s_host_pointer_positions[index][static_cast<u8>(axis)] += d;
  s_pointer_state[index][static_cast<u8>(axis)].delta.fetch_add(static_cast<s32>(d * 65536.0f),
                                                                std::memory_order_release);

  // We need to clamp the position ourselves in relative mode.
  if (axis <= InputPointerAxis::Y)
  {
    s_host_pointer_positions[index][static_cast<u8>(axis)] =
      std::clamp(s_host_pointer_positions[index][static_cast<u8>(axis)], 0.0f, s_window_size[static_cast<u8>(axis)]);

    // Imgui also needs to be updated, since the absolute position won't be set above.
    if (index == 0)
      ImGuiManager::UpdateMousePosition(s_host_pointer_positions[0][0], s_host_pointer_positions[0][1]);
  }
}

void InputManager::UpdateRelativeMouseMode()
{
  // Device-identity callbacks (for example native Win32 Raw Input trackballs)
  // receive movement directly and must not capture or hide the host cursor.
  bool has_relative_mode_bindings = !s_pointer_move_callbacks.empty();
  if (!has_relative_mode_bindings)
  {
    for (const auto& it : s_binding_map)
    {
      const InputBindingKey& key = it.first;
      if (key.source_type == InputSourceType::Pointer && key.source_subtype == InputSubclass::PointerAxis &&
          key.data >= static_cast<u32>(InputPointerAxis::X) && key.data <= static_cast<u32>(InputPointerAxis::Y))
      {
        has_relative_mode_bindings = true;
        break;
      }
    }
  }

  const bool hide_mouse_cursor = has_relative_mode_bindings || ImGuiManager::HasSoftwareCursor(0);
  if (s_relative_mouse_mode == has_relative_mode_bindings && s_hide_host_mouse_cursor == hide_mouse_cursor)
    return;

  s_relative_mouse_mode = has_relative_mode_bindings;
  s_hide_host_mouse_cursor = hide_mouse_cursor;
  UpdateHostMouseMode();
}

void InputManager::UpdateHostMouseMode()
{
  const bool can_change = System::IsRunning();
  const bool wanted_relative_mouse_mode = (s_relative_mouse_mode && can_change);
  const bool wanted_hide_host_mouse_cursor = (s_hide_host_mouse_cursor && can_change);
  if (wanted_relative_mouse_mode == s_relative_mouse_mode_active &&
      wanted_hide_host_mouse_cursor == s_hide_host_mouse_cusor_active)
  {
    return;
  }

  s_relative_mouse_mode_active = wanted_relative_mouse_mode;
  s_hide_host_mouse_cusor_active = wanted_hide_host_mouse_cursor;
  Host::SetMouseMode(wanted_relative_mouse_mode, wanted_hide_host_mouse_cursor);
}

bool InputManager::IsUsingRawInput()
{
#if defined(_WIN32)
  return static_cast<bool>(s_input_sources[static_cast<u32>(InputSourceType::RawInput)]);
#else
  return false;
#endif
}

void InputManager::SetDisplayWindowSize(float width, float height)
{
  s_window_size[0] = width;
  s_window_size[1] = height;
}

void InputManager::SetDefaultSourceConfig(SettingsInterface& si)
{
  si.ClearSection("InputSources");
  si.SetBoolValue("InputSources", "SDL", true);
  si.SetBoolValue("InputSources", "SDLControllerEnhancedMode", false);
  si.SetBoolValue("InputSources", "SDLPS5PlayerLED", false);
  si.SetBoolValue("InputSources", "XInput", false);
  si.SetBoolValue("InputSources", "RawInput", false);
}

void InputManager::CopyConfiguration(SettingsInterface* dest_si, const SettingsInterface& src_si,
                                     bool copy_pad_config /*= true*/, bool copy_pad_bindings /*= true*/,
                                     bool copy_hotkey_bindings /*= true*/)
{
  (void)copy_pad_config;
  (void)copy_pad_bindings;
  Settings::RemoveStandardControllerConfig(*dest_si);

  if (copy_hotkey_bindings)
  {
    std::vector<const HotkeyInfo*> hotkeys(InputManager::GetHotkeyList());
    for (const HotkeyInfo* hki : hotkeys)
      dest_si->CopyStringListValue(src_si, "Hotkeys", hki->name);
  }
}

bool InputManager::MapController(SettingsInterface& si, u32 controller,
                                 const std::vector<std::pair<GenericInputBinding, std::string>>& mapping)
{
  return false;
}

std::vector<std::string> InputManager::GetInputProfileNames()
{
  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(EmuFolders::InputProfiles.c_str(), "*.ini",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RELATIVE_PATHS |
                          FILESYSTEM_FIND_SORT_BY_NAME,
                        &results);

  std::vector<std::string> ret;
  ret.reserve(results.size());
  for (FILESYSTEM_FIND_DATA& fd : results)
    ret.emplace_back(Path::GetFileTitle(fd.FileName));

  return ret;
}

void InputManager::OnInputDeviceConnected(std::string_view identifier, std::string_view device_name)
{
  INFO_LOG("Device '{}' connected: '{}'", identifier, device_name);
  Host::OnInputDeviceConnected(identifier, device_name);
}

void InputManager::OnInputDeviceDisconnected(InputBindingKey key, std::string_view identifier)
{
  INFO_LOG("Device '{}' disconnected", identifier);
  Host::OnInputDeviceDisconnected(key, identifier);
}

// ------------------------------------------------------------------------
// Hooks/Event Intercepting
// ------------------------------------------------------------------------

void InputManager::SetHook(InputInterceptHook::Callback callback)
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  DebugAssert(!m_event_intercept_callback);
  m_event_intercept_callback = std::move(callback);
}

void InputManager::RemoveHook()
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  if (m_event_intercept_callback)
    m_event_intercept_callback = {};
}

bool InputManager::HasHook()
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  return (bool)m_event_intercept_callback;
}

bool InputManager::DoEventHook(InputBindingKey key, float value)
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  if (!m_event_intercept_callback)
    return false;

  const InputInterceptHook::CallbackResult action = m_event_intercept_callback(key, value);
  if (action >= InputInterceptHook::CallbackResult::RemoveHookAndStopProcessingEvent)
    m_event_intercept_callback = {};

  return (action == InputInterceptHook::CallbackResult::RemoveHookAndStopProcessingEvent ||
          action == InputInterceptHook::CallbackResult::StopProcessingEvent);
}

// ------------------------------------------------------------------------
// Binding Updater
// ------------------------------------------------------------------------

void InputManager::ReloadBindings(SettingsInterface& binding_si, SettingsInterface& hotkey_binding_si)
{
  std::unique_lock lock(s_binding_map_write_lock);

  s_binding_map.clear();
  s_pointer_move_callbacks.clear();
  s_pointer_identifier_move_callbacks.clear();
  s_pointer_identifier_absolute_callbacks.clear();

  Host::AddFixedInputBindings(binding_si);

  // Hotkeys use the base configuration, except if the custom hotkeys option is enabled.
  AddHotkeyBindings(hotkey_binding_si);

  for (u32 axis = 0; axis < static_cast<u32>(InputPointerAxis::Count); axis++)
  {
    // From lilypad: 1 mouse pixel = 1/8th way down.
    const float default_scale = (axis <= static_cast<u32>(InputPointerAxis::Y)) ? 8.0f : 1.0f;
    s_pointer_axis_scale[axis] =
      1.0f / std::max(binding_si.GetFloatValue("Pointer",
                                               fmt::format("Pointer{}Scale", s_pointer_axis_names[axis]).c_str(),
                                               default_scale),
                      1.0f);
  }

  UpdateRelativeMouseMode();
}

// ------------------------------------------------------------------------
// Source Management
// ------------------------------------------------------------------------

bool InputManager::ReloadDevices()
{
  bool changed = false;

  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
      changed |= s_input_sources[i]->ReloadDevices();
  }

  UpdatePointerCount();

  return changed;
}

void InputManager::CloseSources()
{
  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
    {
      s_input_sources[i]->Shutdown();
      s_input_sources[i].reset();
    }
  }
}

void InputManager::PollSources()
{
  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
      s_input_sources[i]->PollEvents();
  }

  GenerateRelativeMouseEvents();
}

std::vector<std::pair<std::string, std::string>> InputManager::EnumerateDevices()
{
  std::vector<std::pair<std::string, std::string>> ret;

  ret.emplace_back("Keyboard", "Keyboard");
  ret.emplace_back("Mouse", "Mouse");

  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
    {
      std::vector<std::pair<std::string, std::string>> devs(s_input_sources[i]->EnumerateDevices());
      if (ret.empty())
        ret = std::move(devs);
      else
        std::move(devs.begin(), devs.end(), std::back_inserter(ret));
    }
  }

  return ret;
}

std::vector<PointerDeviceInfo> InputManager::EnumeratePointerDevices()
{
  InputSource* source = GetInputSourceInterface(InputSourceType::RawInput);
  if (source)
    return source->EnumeratePointerDevices();

#ifdef _WIN32
  // Raw Input device discovery does not require the event source to be initialized.
  return InputSource::CreateWin32RawInputSource()->EnumeratePointerDevices();
#else
  return {};
#endif
}

std::vector<InputBindingKey> InputManager::EnumerateMotors()
{
  std::vector<InputBindingKey> ret;

  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
    {
      std::vector<InputBindingKey> devs(s_input_sources[i]->EnumerateMotors());
      if (ret.empty())
        ret = std::move(devs);
      else
        std::move(devs.begin(), devs.end(), std::back_inserter(ret));
    }
  }

  return ret;
}

static void GetKeyboardGenericBindingMapping(std::vector<std::pair<GenericInputBinding, std::string>>* mapping)
{
  mapping->emplace_back(GenericInputBinding::DPadUp, "Keyboard/Up");
  mapping->emplace_back(GenericInputBinding::DPadRight, "Keyboard/Right");
  mapping->emplace_back(GenericInputBinding::DPadDown, "Keyboard/Down");
  mapping->emplace_back(GenericInputBinding::DPadLeft, "Keyboard/Left");
  mapping->emplace_back(GenericInputBinding::LeftStickUp, "Keyboard/W");
  mapping->emplace_back(GenericInputBinding::LeftStickRight, "Keyboard/D");
  mapping->emplace_back(GenericInputBinding::LeftStickDown, "Keyboard/S");
  mapping->emplace_back(GenericInputBinding::LeftStickLeft, "Keyboard/A");
  mapping->emplace_back(GenericInputBinding::RightStickUp, "Keyboard/T");
  mapping->emplace_back(GenericInputBinding::RightStickRight, "Keyboard/H");
  mapping->emplace_back(GenericInputBinding::RightStickDown, "Keyboard/G");
  mapping->emplace_back(GenericInputBinding::RightStickLeft, "Keyboard/F");
  mapping->emplace_back(GenericInputBinding::Start, "Keyboard/Return");
  mapping->emplace_back(GenericInputBinding::Select, "Keyboard/Backspace");
  mapping->emplace_back(GenericInputBinding::Triangle, "Keyboard/I");
  mapping->emplace_back(GenericInputBinding::Circle, "Keyboard/L");
  mapping->emplace_back(GenericInputBinding::Cross, "Keyboard/K");
  mapping->emplace_back(GenericInputBinding::Square, "Keyboard/J");
  mapping->emplace_back(GenericInputBinding::L1, "Keyboard/Q");
  mapping->emplace_back(GenericInputBinding::L2, "Keyboard/1");
  mapping->emplace_back(GenericInputBinding::L3, "Keyboard/2");
  mapping->emplace_back(GenericInputBinding::R1, "Keyboard/E");
  mapping->emplace_back(GenericInputBinding::R2, "Keyboard/3");
  mapping->emplace_back(GenericInputBinding::R3, "Keyboard/4");
}

static bool GetInternalGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping)
{
  if (device == "Keyboard")
  {
    GetKeyboardGenericBindingMapping(mapping);
    return true;
  }

  return false;
}

GenericInputBindingMapping InputManager::GetGenericBindingMapping(std::string_view device)
{
  GenericInputBindingMapping mapping;

  if (!GetInternalGenericBindingMapping(device, &mapping))
  {
    for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
    {
      if (s_input_sources[i] && s_input_sources[i]->GetGenericBindingMapping(device, &mapping))
        break;
    }
  }

  return mapping;
}

bool InputManager::IsInputSourceEnabled(SettingsInterface& si, InputSourceType type)
{
  return si.GetBoolValue("InputSources", InputManager::InputSourceToString(type), GetInputSourceDefaultEnabled(type));
}

void InputManager::UpdateInputSourceState(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock,
                                          InputSourceType type, std::unique_ptr<InputSource> (*factory_function)())
{
  const bool enabled = IsInputSourceEnabled(si, type);
  if (enabled)
  {
    if (s_input_sources[static_cast<u32>(type)])
    {
      s_input_sources[static_cast<u32>(type)]->UpdateSettings(si, settings_lock);
    }
    else
    {
      std::unique_ptr<InputSource> source(factory_function());
      if (!source->Initialize(si, settings_lock))
      {
        ERROR_LOG("Source '{}' failed to initialize.", InputManager::InputSourceToString(type));
        return;
      }

      s_input_sources[static_cast<u32>(type)] = std::move(source);
    }
  }
  else
  {
    if (s_input_sources[static_cast<u32>(type)])
    {
      s_input_sources[static_cast<u32>(type)]->Shutdown();
      s_input_sources[static_cast<u32>(type)].reset();
    }
  }
}

void InputManager::ReloadSources(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
#ifdef _WIN32
  UpdateInputSourceState(si, settings_lock, InputSourceType::DInput, &InputSource::CreateDInputSource);
  UpdateInputSourceState(si, settings_lock, InputSourceType::XInput, &InputSource::CreateXInputSource);
  UpdateInputSourceState(si, settings_lock, InputSourceType::RawInput, &InputSource::CreateWin32RawInputSource);
#endif
  UpdateInputSourceState(si, settings_lock, InputSourceType::SDL, &InputSource::CreateSDLSource);

  UpdatePointerCount();
}
