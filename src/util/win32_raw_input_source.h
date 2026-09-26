// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once
#include "common/windows_headers.h"
#include "input_source.h"
#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class SettingsInterface;

class Win32RawInputSource final : public InputSource
{
public:
  Win32RawInputSource();
  ~Win32RawInputSource();

  bool Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  bool ReloadDevices() override;
  void Shutdown() override;

  void PollEvents() override;
  std::vector<std::pair<std::string, std::string>> EnumerateDevices() override;
  std::vector<PointerDeviceInfo> EnumeratePointerDevices() override;
  bool SetPointerAbsolutePosition(std::string_view device, float x, float y) override;
  std::vector<InputBindingKey> EnumerateMotors() override;
  bool GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping) override;
  void UpdateMotorState(InputBindingKey key, float intensity) override;
  void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                        float small_intensity) override;

  std::optional<InputBindingKey> ParseKeyString(std::string_view device, std::string_view binding) override;
  TinyString ConvertKeyToString(InputBindingKey key) override;
  TinyString ConvertKeyToIcon(InputBindingKey key) override;

private:
  struct MouseState
  {
    HANDLE device;
    std::string identifier;
    std::string display_name;
    u32 raw_mouse_slot;
    u32 button_state;
    s32 last_x;
    s32 last_y;
  };

  static bool RegisterDummyClass();
  static LRESULT CALLBACK DummyWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  static std::vector<MouseState> EnumerateRawInputMice();
  static std::string GetMouseDeviceName(std::string_view device_path);
  static std::string GetRawMouseDeviceName(u32 slot);

  bool CreateDummyWindow();
  void DestroyDummyWindow();
  bool SetRawInputCaptureEnabled(bool enabled);
  bool OpenDevices();
  void CloseDevices();

  bool ProcessRawInputEvent(const RAWINPUT* event);

  HWND m_dummy_window = {};
  bool m_raw_input_capture_enabled = false;
  bool m_device_reload_pending = false;

  std::vector<MouseState> m_mice;
  std::unordered_map<HANDLE, u32> m_handle_to_mouse_index;
};
