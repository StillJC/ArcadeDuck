#pragma once
#include "common/windows_headers.h"
#include "nogui_host_interface.h"
#include <memory>
#include <vector>

class Win32HostInterface final : public NoGUIHostInterface
{
public:
  Win32HostInterface();
  ~Win32HostInterface();

  bool Initialize();
  void Shutdown();

  static std::unique_ptr<NoGUIHostInterface> Create();

protected:
  bool CreatePlatformWindow() override;
  void DestroyPlatformWindow() override;
  std::optional<WindowInfo> GetPlatformWindowInfo() override;

  std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const override;

  void PollAndUpdate() override;

  // The Win32 frontend was never built by the fork author; CommonHostInterface::UpdateInputMap() is pure
  // virtual, so without this override Win32HostInterface stays abstract and won't instantiate (MSVC C2259).
  // Matches the empty stub already present in sdl_host_interface.h.
  void UpdateInputMap() override {}

private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  bool RegisterWindowClass();
  void ProcessWin32Events();

  HWND m_hwnd{};
};
