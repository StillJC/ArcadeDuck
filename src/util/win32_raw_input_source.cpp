// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 Tovarichtch
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

// Raw Input multi-mouse handling is adapted from pcsx2x6 PR #130 (GPL-3.0+).

#include "win32_raw_input_source.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/host.h"
#include "core/system.h"
#include "input_manager.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <map>
#include <string>
#include <utility>
#include <hidusage.h>
#include <hidsdi.h>
#include <malloc.h>

#pragma comment(lib, "hid.lib")

Log_SetChannel(Win32RawInputSource);

static const wchar_t* WINDOW_CLASS_NAME = L"Win32RawInputSource";
static bool s_window_class_registered = false;
static constexpr UINT WM_RAW_INPUT_RELOAD_DEVICES = WM_APP + 1;

static constexpr const u32 ALL_BUTTON_MASKS = RI_MOUSE_BUTTON_1_DOWN | RI_MOUSE_BUTTON_1_UP | RI_MOUSE_BUTTON_2_DOWN |
                                              RI_MOUSE_BUTTON_2_UP | RI_MOUSE_BUTTON_3_DOWN | RI_MOUSE_BUTTON_3_UP |
                                              RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_DOWN |
                                              RI_MOUSE_BUTTON_5_UP;

static InputBindingKey MakeRawMouseButtonKey(u32 slot, u32 button)
{
  InputBindingKey key = {};
  key.source_type = InputSourceType::RawInput;
  key.source_subtype = InputSubclass::ControllerButton;
  key.source_index = slot;
  key.data = button;
  return key;
}

Win32RawInputSource::Win32RawInputSource() = default;

Win32RawInputSource::~Win32RawInputSource() = default;

bool Win32RawInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  if (!RegisterDummyClass())
  {
    ERROR_LOG("Failed to register dummy window class");
    return false;
  }

  if (!CreateDummyWindow())
  {
    ERROR_LOG("Failed to create dummy window");
    return false;
  }

  if (!OpenDevices())
  {
    ERROR_LOG("Failed to open devices");
    CloseDevices();
    DestroyDummyWindow();
    return false;
  }

  if (!SetRawInputCaptureEnabled(true))
  {
    CloseDevices();
    DestroyDummyWindow();
    return false;
  }

  return true;
}

void Win32RawInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
}

bool Win32RawInputSource::ReloadDevices()
{
  return OpenDevices();
}

void Win32RawInputSource::Shutdown()
{
  SetRawInputCaptureEnabled(false);
  CloseDevices();
  DestroyDummyWindow();
}

void Win32RawInputSource::PollEvents()
{
  // noop, handled by message pump
}

std::vector<std::pair<std::string, std::string>> Win32RawInputSource::EnumerateDevices()
{
  std::vector<std::pair<std::string, std::string>> ret;
  for (const MouseState& mouse : m_mice)
    ret.emplace_back(GetRawMouseDeviceName(mouse.raw_mouse_slot), mouse.display_name);

  return ret;
}

std::vector<PointerDeviceInfo> Win32RawInputSource::EnumeratePointerDevices()
{
  const std::vector<MouseState> mice = EnumerateRawInputMice();
  std::vector<PointerDeviceInfo> ret;
  ret.reserve(mice.size());
  for (const MouseState& mouse : mice)
    ret.push_back({mouse.identifier, mouse.display_name, "Windows Raw Input pointer", true});
  return ret;
}

bool Win32RawInputSource::SetPointerAbsolutePosition(std::string_view device, float x, float y)
{
  const auto it = std::find_if(m_mice.begin(), m_mice.end(),
                               [device](const MouseState& mouse) { return mouse.identifier == device; });
  if (it == m_mice.end())
    return false;

  InputManager::SetRawPointerVirtualPosition(it->raw_mouse_slot, x, y);
  InputManager::InvokePointerAbsoluteCallbacksLocal(it->identifier, x, y);
  return true;
}
void Win32RawInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
}

void Win32RawInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                                           float small_intensity)
{
}

std::optional<InputBindingKey> Win32RawInputSource::ParseKeyString(std::string_view device, std::string_view binding)
{
  if (!device.starts_with("RawMouse-") || !binding.starts_with("Button"))
    return std::nullopt;

  const std::optional<u32> slot = StringUtil::FromChars<u32>(device.substr(9));
  const std::optional<u32> button = StringUtil::FromChars<u32>(binding.substr(6));
  if (!slot.has_value() || !button.has_value())
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::RawInput;
  key.source_subtype = InputSubclass::ControllerButton;
  key.source_index = slot.value();
  key.data = button.value();
  return key;
}

TinyString Win32RawInputSource::ConvertKeyToString(InputBindingKey key)
{
  TinyString ret;
  if (key.source_type == InputSourceType::RawInput && key.source_subtype == InputSubclass::ControllerButton)
    ret.format("RawMouse-{}/Button{}", u32{key.source_index}, key.data);
  return ret;
}

TinyString Win32RawInputSource::ConvertKeyToIcon(InputBindingKey key)
{
  return {};
}

std::vector<InputBindingKey> Win32RawInputSource::EnumerateMotors()
{
  return {};
}

bool Win32RawInputSource::GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping)
{
  return {};
}

std::string Win32RawInputSource::GetRawMouseDeviceName(u32 slot)
{
  return fmt::format("RawMouse-{}", slot);
}

bool Win32RawInputSource::RegisterDummyClass()
{
  if (s_window_class_registered)
    return true;

  WNDCLASSW wc = {};
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpfnWndProc = DummyWindowProc;
  wc.lpszClassName = WINDOW_CLASS_NAME;
  s_window_class_registered = (RegisterClassW(&wc) != 0);
  return s_window_class_registered;
}

bool Win32RawInputSource::CreateDummyWindow()
{
  m_dummy_window = CreateWindowExW(0, WINDOW_CLASS_NAME, WINDOW_CLASS_NAME, WS_OVERLAPPED, CW_USEDEFAULT, CW_USEDEFAULT,
                                   CW_USEDEFAULT, CW_USEDEFAULT, HWND_MESSAGE, NULL, GetModuleHandleW(nullptr), NULL);
  if (!m_dummy_window)
    return false;

  SetWindowLongPtrW(m_dummy_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void Win32RawInputSource::DestroyDummyWindow()
{
  m_device_reload_pending = false;

  if (!m_dummy_window)
    return;

  DestroyWindow(m_dummy_window);
  m_dummy_window = {};
}

bool Win32RawInputSource::SetRawInputCaptureEnabled(bool enabled)
{
  if (m_raw_input_capture_enabled == enabled)
    return true;

  RAWINPUTDEVICE request = {};
  request.usUsagePage = HID_USAGE_PAGE_GENERIC;
  request.usUsage = HID_USAGE_GENERIC_MOUSE;

  if (enabled)
  {
    request.dwFlags = RIDEV_DEVNOTIFY | RIDEV_INPUTSINK;
    request.hwndTarget = m_dummy_window;
  }
  else
  {
    request.dwFlags = RIDEV_REMOVE;
    request.hwndTarget = nullptr;
  }

  if (!RegisterRawInputDevices(&request, 1, sizeof(request)))
  {
    const DWORD error = GetLastError();
    if (enabled)
      ERROR_LOG("Failed to enable Raw Input mouse capture: {}", error);
    else
      WARNING_LOG("Failed to disable Raw Input mouse capture: {}", error);

    return false;
  }

  m_raw_input_capture_enabled = enabled;
  return true;
}

LRESULT CALLBACK Win32RawInputSource::DummyWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  Win32RawInputSource* ris = reinterpret_cast<Win32RawInputSource*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_INPUT_DEVICE_CHANGE)
  {
    if (ris && !ris->m_device_reload_pending)
    {
      ris->m_device_reload_pending = true;
      if (!PostMessageW(hwnd, WM_RAW_INPUT_RELOAD_DEVICES, 0, 0))
        ris->m_device_reload_pending = false;
    }

    return 0;
  }

  if (msg == WM_RAW_INPUT_RELOAD_DEVICES)
  {
    if (ris)
    {
      ris->m_device_reload_pending = false;
      ris->ReloadDevices();
    }

    return 0;
  }
  if (msg != WM_INPUT)
    return DefWindowProcW(hwnd, msg, wParam, lParam);

  UINT size = 0;
  GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));

  PRAWINPUT data = static_cast<PRAWINPUT>(_alloca(size));
  GetRawInputData((HRAWINPUT)lParam, RID_INPUT, data, &size, sizeof(RAWINPUTHEADER));

  // we shouldn't get any WM_INPUT messages prior to SetWindowLongPtr(), so this'll be fine
  if (ris && ris->ProcessRawInputEvent(data))
    return 0;

  // forward through to normal message processing
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::string Win32RawInputSource::GetMouseDeviceName(std::string_view device_path)
{
  constexpr std::string_view prefix = "RawInput:";
  const auto read_hid_id = [device_path](std::string_view key) -> std::string {
    const size_t start = device_path.find(key);
    if (start == std::string_view::npos || (start + key.size() + 4) > device_path.size())
      return {};

    const std::string_view value = device_path.substr(start + key.size(), 4);
    return std::all_of(value.begin(), value.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    }) ? std::string(value) : std::string{};
  };
  const std::string vid = read_hid_id("VID_");
  const std::string pid = read_hid_id("PID_");
  const std::string fallback = (!vid.empty() && !pid.empty()) ? fmt::format("Windows Raw Mouse [{}:{}]", vid, pid) :
                                                                 "Windows Raw Mouse";

  const auto trim_name = [](std::wstring name) {
    const auto first = std::find_if_not(name.begin(), name.end(), [](wchar_t c) { return std::iswspace(c) != 0; });
    const auto last = std::find_if_not(name.rbegin(), name.rend(), [](wchar_t c) { return std::iswspace(c) != 0; }).base();
    return (first < last) ? std::wstring(first, last) : std::wstring{};
  };
  const auto normalize_name = [&trim_name](std::wstring name) {
    name = trim_name(std::move(name));
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return name;
  };
  const auto is_useful_name = [&normalize_name](const std::wstring& name) {
    const std::wstring normalized = normalize_name(name);
    return !normalized.empty() && normalized != L"hid-compliant mouse" && normalized != L"hid-compliant device" &&
           normalized != L"usb input device" && normalized != L"usb composite device" &&
           normalized != L"bluetooth hid device" && normalized != L"mouse" &&
           !normalized.starts_with(L"hid-compliant");
  };

  const std::wstring interface_path = StringUtil::UTF8StringToWideString(
    device_path.starts_with(prefix) ? device_path.substr(prefix.size()) : device_path);
  if (interface_path.empty())
    return fallback;

  HANDLE handle = CreateFileW(interface_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE)
  {
    handle = CreateFileW(interface_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                         nullptr);
  }
  if (handle == INVALID_HANDLE_VALUE)
    return fallback;

  std::array<wchar_t, 256> product = {};
  std::array<wchar_t, 256> manufacturer = {};
  const bool have_product = HidD_GetProductString(handle, product.data(), sizeof(product));
  const bool have_manufacturer = HidD_GetManufacturerString(handle, manufacturer.data(), sizeof(manufacturer));
  CloseHandle(handle);

  const std::wstring product_name = have_product ? trim_name(product.data()) : std::wstring{};
  const std::wstring manufacturer_name = have_manufacturer ? trim_name(manufacturer.data()) : std::wstring{};
  const bool useful_product = is_useful_name(product_name);
  const bool useful_manufacturer = is_useful_name(manufacturer_name);
  if (!useful_product && !useful_manufacturer)
    return fallback;

  std::wstring name;
  if (useful_product && useful_manufacturer)
  {
    const std::wstring normalized_product = normalize_name(product_name);
    const std::wstring normalized_manufacturer = normalize_name(manufacturer_name);
    name = (normalized_product.find(normalized_manufacturer) != std::wstring::npos) ? product_name :
                                                                                        manufacturer_name + L" " + product_name;
  }
  else
  {
    name = useful_product ? product_name : manufacturer_name;
  }

  const std::string utf8_name = StringUtil::WideStringToUTF8String(name);
  return (!vid.empty() && !pid.empty()) ? fmt::format("{} [{}:{}]", utf8_name, vid, pid) : utf8_name;
}

std::vector<Win32RawInputSource::MouseState> Win32RawInputSource::EnumerateRawInputMice()
{
  UINT num_devices = 0;
  if (GetRawInputDeviceList(nullptr, &num_devices, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1) ||
      num_devices == 0)
  {
    return {};
  }

  std::vector<RAWINPUTDEVICELIST> devices(num_devices);
  if (GetRawInputDeviceList(devices.data(), &num_devices, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
    return {};
  devices.resize(num_devices);

  std::vector<MouseState> mice;
  for (const RAWINPUTDEVICELIST& rid : devices)
  {
    if (rid.dwType != RIM_TYPEMOUSE)
      continue;

    RID_DEVICE_INFO device_info = {};
    device_info.cbSize = sizeof(device_info);
    device_info.dwType = RIM_TYPEMOUSE;
    UINT device_info_size = sizeof(device_info);
    GetRawInputDeviceInfoW(rid.hDevice, RIDI_DEVICEINFO, &device_info, &device_info_size);

    UINT name_size = 0;
    if (GetRawInputDeviceInfoW(rid.hDevice, RIDI_DEVICENAME, nullptr, &name_size) == static_cast<UINT>(-1) ||
        name_size == 0)
    {
      continue;
    }
    std::wstring interface_path(name_size, L'\0');
    const UINT written_size = GetRawInputDeviceInfoW(rid.hDevice, RIDI_DEVICENAME, interface_path.data(), &name_size);
    if (written_size == static_cast<UINT>(-1))
      continue;
    interface_path.resize(written_size);

    // RIDI_DEVICENAME can leave the terminating NUL included in the reported length.
    // Never include it in the persistent RawInput device identifier.
    while (!interface_path.empty() && interface_path.back() == L'\0')
      interface_path.pop_back();

    const std::string identifier = "RawInput:" + StringUtil::WideStringToUTF8String(interface_path);
    mice.push_back({.device = rid.hDevice, .identifier = identifier, .display_name = GetMouseDeviceName(identifier),
                    .raw_mouse_slot = 0, .button_state = 0, .last_x = 0, .last_y = 0});
  }

  std::sort(mice.begin(), mice.end(),
            [](const MouseState& lhs, const MouseState& rhs) { return lhs.identifier < rhs.identifier; });
  std::map<std::string, u32> name_counts;
  for (const MouseState& mouse : mice)
    ++name_counts[mouse.display_name];

  std::map<std::string, u32> name_ordinals;
  for (MouseState& mouse : mice)
  {
    const std::string base_name = mouse.display_name;
    if (name_counts[base_name] > 1)
      mouse.display_name = fmt::format("{} ({})", base_name, ++name_ordinals[base_name]);
  }

  return mice;
}

bool Win32RawInputSource::OpenDevices()
{
  std::vector<MouseState> mice = EnumerateRawInputMice();
  std::vector<bool> old_matched(m_mice.size(), false);
  std::vector<bool> slot_assigned(mice.size(), false);
  std::vector<bool> slot_used;
  for (u32 mouse_index = 0; mouse_index < static_cast<u32>(mice.size()); mouse_index++)
  {
    MouseState& mouse = mice[mouse_index];
    for (u32 i = 0; i < static_cast<u32>(m_mice.size()); i++)
    {
      const MouseState& old_mouse = m_mice[i];
      if (old_mouse.identifier != mouse.identifier)
        continue;

      mouse.raw_mouse_slot = old_mouse.raw_mouse_slot;
      mouse.button_state = old_mouse.button_state;
      mouse.last_x = old_mouse.last_x;
      mouse.last_y = old_mouse.last_y;
      old_matched[i] = true;
      if (slot_used.size() <= mouse.raw_mouse_slot)
        slot_used.resize(mouse.raw_mouse_slot + 1, false);
      slot_used[mouse.raw_mouse_slot] = true;
      slot_assigned[mouse_index] = true;
      break;
    }
  }
  for (u32 mouse_index = 0; mouse_index < static_cast<u32>(mice.size()); mouse_index++)
  {
    if (slot_assigned[mouse_index])
      continue;
    MouseState& mouse = mice[mouse_index];
    u32 slot = 0;
    while (slot < slot_used.size() && slot_used[slot])
      slot++;
    if (slot == slot_used.size())
      slot_used.push_back(true);
    else
      slot_used[slot] = true;
    mouse.raw_mouse_slot = slot;
  }

  for (u32 i = 0; i < static_cast<u32>(m_mice.size()); i++)
  {
    if (!old_matched[i])
      InputManager::OnInputDeviceDisconnected(MakeRawMouseButtonKey(m_mice[i].raw_mouse_slot, 0),
                                              GetRawMouseDeviceName(m_mice[i].raw_mouse_slot));
  }
  for (const MouseState& mouse : mice)
  {
    const bool existing = std::any_of(m_mice.begin(), m_mice.end(), [&mouse](const MouseState& old_mouse) {
      return old_mouse.identifier == mouse.identifier;
    });
    if (!existing)
      InputManager::OnInputDeviceConnected(GetRawMouseDeviceName(mouse.raw_mouse_slot), mouse.display_name);
  }

  m_mice = std::move(mice);
  m_handle_to_mouse_index.clear();
  for (u32 i = 0; i < static_cast<u32>(m_mice.size()); i++)
  {
    m_handle_to_mouse_index.emplace(m_mice[i].device, i);
    InputManager::ResetRawPointerVirtualPosition(m_mice[i].raw_mouse_slot);
  }

  DEV_LOG("Found {} mice", m_mice.size());
  return true;
}

void Win32RawInputSource::CloseDevices()
{
  for (const MouseState& mouse : m_mice)
    InputManager::OnInputDeviceDisconnected(MakeRawMouseButtonKey(mouse.raw_mouse_slot, 0),
                                            GetRawMouseDeviceName(mouse.raw_mouse_slot));

  m_mice.clear();
  m_handle_to_mouse_index.clear();
}

bool Win32RawInputSource::ProcessRawInputEvent(const RAWINPUT* event)
{
  if (event->header.dwType != RIM_TYPEMOUSE)
    return false;

  const auto mouse_it = m_handle_to_mouse_index.find(event->header.hDevice);
  if (mouse_it == m_handle_to_mouse_index.end())
    return false;

  MouseState& state = m_mice[mouse_it->second];
  const RAWMOUSE& rm = event->data.mouse;
  const bool absolute = (rm.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;

  if (absolute)
  {
    state.last_x = rm.lLastX;
    state.last_y = rm.lLastY;
    const bool virtual_desktop = (rm.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
    const int left = virtual_desktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
    const int top = virtual_desktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
    const int width = GetSystemMetrics(virtual_desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
    const int height = GetSystemMetrics(virtual_desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);

    InputManager::InvokePointerAbsoluteCallbacks(
      state.identifier, (static_cast<float>(left) + (static_cast<float>(rm.lLastX) / 65535.0f) * width - left) / width,
      (static_cast<float>(top) + (static_cast<float>(rm.lLastY) / 65535.0f) * height - top) / height);
  }
  else
  {
    const s32 dx = rm.lLastX;
    const s32 dy = rm.lLastY;
    if (dx != 0)
      InputManager::InvokePointerMoveCallbacks(state.identifier, InputManager::MakePointerAxisKey(state.raw_mouse_slot, InputPointerAxis::X), static_cast<float>(dx));
    if (dy != 0)
      InputManager::InvokePointerMoveCallbacks(state.identifier, InputManager::MakePointerAxisKey(state.raw_mouse_slot, InputPointerAxis::Y), static_cast<float>(dy));

    if (dx != 0 || dy != 0)
    {
      const auto [virtual_x, virtual_y] = InputManager::UpdateRawPointerVirtualPosition(
        state.raw_mouse_slot, static_cast<float>(dx), static_cast<float>(dy));
      InputManager::InvokePointerAbsoluteCallbacksLocal(state.identifier, virtual_x, virtual_y);
    }
  }

  unsigned long button_mask =
    (rm.usButtonFlags & (rm.usButtonFlags ^ std::exchange(state.button_state, rm.usButtonFlags))) & ALL_BUTTON_MASKS;

  while (button_mask != 0)
  {
    unsigned long bit_index;
    _BitScanForward(&bit_index, button_mask);

    const u32 button_number = bit_index >> 1;
    const bool button_pressed = (bit_index & 1u) == 0;
    InputManager::InvokeEvents(MakeRawMouseButtonKey(state.raw_mouse_slot, button_number),
                               static_cast<float>(button_pressed), GenericInputBinding::Unknown);

    button_mask &= ~(1u << bit_index);
  }

  return true;
}

std::unique_ptr<InputSource> InputSource::CreateWin32RawInputSource()
{
  return std::make_unique<Win32RawInputSource>();
}
