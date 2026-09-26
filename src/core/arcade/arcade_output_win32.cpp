// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/arcade_output_win32.h"

#if defined(_WIN32)

#include "common/log.h"
#include "common/windows_headers.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

Log_SetChannel(ArcadeOutputWin32);

namespace Arcade::Output::Win32Transport {
namespace {

// MAME Win32 output protocol names. These are public IPC identifiers used by
// MAMEHooker and other output clients.
constexpr wchar_t WINDOW_CLASS_NAME[] = L"MAMEOutput";
constexpr wchar_t WINDOW_NAME[] = L"MAMEOutput";
constexpr wchar_t MESSAGE_START[] = L"MAMEOutputStart";
constexpr wchar_t MESSAGE_STOP[] = L"MAMEOutputStop";
constexpr wchar_t MESSAGE_UPDATE_STATE[] = L"MAMEOutputUpdateState";
constexpr wchar_t MESSAGE_REGISTER_CLIENT[] = L"MAMEOutputRegister";
constexpr wchar_t MESSAGE_UNREGISTER_CLIENT[] = L"MAMEOutputUnregister";
constexpr wchar_t MESSAGE_GET_ID_STRING[] = L"MAMEOutputGetIDString";

constexpr ULONG_PTR COPYDATA_MESSAGE_ID_STRING = 1;
constexpr UINT WM_ARCADE_OUTPUT_UPDATE = WM_APP + 0x4d0;
constexpr UINT WM_ARCADE_OUTPUT_STOP = WM_APP + 0x4d1;
constexpr u32 FIRST_OUTPUT_ID = 12345;

struct CopyDataIdString
{
  u32 id;
  char string[1];
};

struct RegisteredClient
{
  LPARAM id;
  HWND hwnd;
};

struct OutputItem
{
  u32 id;
  s32 value;
  std::string name;
};

struct PendingUpdate
{
  std::string name;
  s32 value;
};

class Transport
{
public:
  ~Transport() { Stop(); }

  void Start(std::string_view game_id)
  {
    Stop();

    std::unique_lock<std::mutex> lock(m_control_mutex);
    m_game_id.assign(game_id.data(), game_id.size());
    m_start_complete = false;
    m_start_success = false;
    m_hwnd = nullptr;
    m_thread_id = 0;

    {
      std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
      m_pending_updates.clear();
    }

    m_clients.clear();
    m_outputs.clear();
    m_output_indices.clear();
    m_next_output_id = FIRST_OUTPUT_ID;

    m_thread = std::thread(&Transport::ThreadMain, this);
    m_start_cv.wait(lock, [this]() { return m_start_complete; });
    const bool success = m_start_success;
    lock.unlock();

    if (!success)
    {
      if (m_thread.joinable())
        m_thread.join();
      ERROR_LOG("Failed to initialize MAME-compatible Win32 output transport.");
    }
    else
    {
      INFO_LOG("MAME-compatible Win32 output transport started for '{}'.", m_game_id);
    }
  }

  void Stop()
  {
    HWND hwnd = nullptr;
    DWORD thread_id = 0;
    {
      std::lock_guard<std::mutex> lock(m_control_mutex);
      if (!m_thread.joinable())
        return;

      hwnd = m_hwnd;
      thread_id = m_thread_id;
    }

    if (hwnd)
    {
      if (!PostMessageW(hwnd, WM_ARCADE_OUTPUT_STOP, 0, 0) && thread_id != 0)
        PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
    }
    else if (thread_id != 0)
    {
      PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
    }

    if (m_thread.joinable())
      m_thread.join();

    {
      std::lock_guard<std::mutex> lock(m_control_mutex);
      m_hwnd = nullptr;
      m_thread_id = 0;
      m_start_complete = false;
      m_start_success = false;
    }

    {
      std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
      m_pending_updates.clear();
    }

    m_clients.clear();
    m_outputs.clear();
    m_output_indices.clear();
    m_game_id.clear();
  }

  void Notify(std::string_view name, s32 value)
  {
    if (name.empty())
      return;

    HWND hwnd = nullptr;
    {
      std::lock_guard<std::mutex> lock(m_control_mutex);
      hwnd = m_hwnd;
    }

    if (!hwnd)
      return;

    {
      std::lock_guard<std::mutex> lock(m_queue_mutex);
      PendingUpdate update;
      update.name.assign(name.data(), name.size());
      update.value = value;
      m_pending_updates.push_back(std::move(update));
    }

    PostMessageW(hwnd, WM_ARCADE_OUTPUT_UPDATE, 0, 0);
  }

private:
  static LRESULT CALLBACK StaticWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
  {
    Transport* transport = reinterpret_cast<Transport*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
      const CREATESTRUCTW* const create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
      transport = static_cast<Transport*>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(transport));
    }

    if (!transport)
      return DefWindowProcW(hwnd, message, wparam, lparam);

    return transport->WindowProc(hwnd, message, wparam, lparam);
  }

  LRESULT WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
  {
    if (message == m_message_register_client)
      return RegisterClient(reinterpret_cast<HWND>(wparam), lparam);

    if (message == m_message_unregister_client)
      return UnregisterClient(reinterpret_cast<HWND>(wparam), lparam);

    if (message == m_message_get_id_string)
      return SendIdString(reinterpret_cast<HWND>(wparam), static_cast<u32>(lparam));

    if (message == WM_ARCADE_OUTPUT_UPDATE)
    {
      DrainPendingUpdates();
      return 0;
    }

    if (message == WM_ARCADE_OUTPUT_STOP)
    {
      // Clear any changes queued immediately before shutdown before advertising
      // that the output provider has stopped.
      DrainPendingUpdates();
      PostMessageW(HWND_BROADCAST, m_message_stop, reinterpret_cast<WPARAM>(hwnd), 0);
      DestroyWindow(hwnd);
      PostQuitMessage(0);
      return 0;
    }

    if (message == WM_DESTROY)
    {
      PostQuitMessage(0);
      return 0;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

  bool RegisterMessages()
  {
    m_message_start = RegisterWindowMessageW(MESSAGE_START);
    m_message_stop = RegisterWindowMessageW(MESSAGE_STOP);
    m_message_update_state = RegisterWindowMessageW(MESSAGE_UPDATE_STATE);
    m_message_register_client = RegisterWindowMessageW(MESSAGE_REGISTER_CLIENT);
    m_message_unregister_client = RegisterWindowMessageW(MESSAGE_UNREGISTER_CLIENT);
    m_message_get_id_string = RegisterWindowMessageW(MESSAGE_GET_ID_STRING);

    return (m_message_start != 0 && m_message_stop != 0 && m_message_update_state != 0 &&
            m_message_register_client != 0 && m_message_unregister_client != 0 && m_message_get_id_string != 0);
  }

  bool RegisterWindowClass(HINSTANCE instance)
  {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = StaticWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = WINDOW_CLASS_NAME;

    if (RegisterClassW(&wc))
      return true;

    return (GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
  }

  void SignalStart(bool success, HWND hwnd)
  {
    {
      std::lock_guard<std::mutex> lock(m_control_mutex);
      m_start_success = success;
      m_start_complete = true;
      m_hwnd = hwnd;
    }
    m_start_cv.notify_all();
  }

  void ThreadMain()
  {
    {
      std::lock_guard<std::mutex> lock(m_control_mutex);
      m_thread_id = GetCurrentThreadId();
    }

    if (!RegisterMessages())
    {
      ERROR_LOG("RegisterWindowMessageW() failed for the MAME output protocol.");
      SignalStart(false, nullptr);
      return;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!RegisterWindowClass(instance))
    {
      ERROR_LOG("RegisterClassW('{}') failed: {}.", "MAMEOutput", GetLastError());
      SignalStart(false, nullptr);
      return;
    }

    HWND const hwnd =
      CreateWindowExW(0, WINDOW_CLASS_NAME, WINDOW_NAME, WS_OVERLAPPED, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!hwnd)
    {
      ERROR_LOG("CreateWindowExW('{}') failed: {}.", "MAMEOutput", GetLastError());
      SignalStart(false, nullptr);
      return;
    }

    SignalStart(true, hwnd);

    // Existing MAMEHooker-style clients discover the provider from this
    // broadcast and then register their own listener HWND.
    PostMessageW(HWND_BROADCAST, m_message_start, reinterpret_cast<WPARAM>(hwnd), 0);

    MSG message = {};
    while (true)
    {
      const BOOL result = GetMessageW(&message, nullptr, 0, 0);
      if (result <= 0)
        break;

      TranslateMessage(&message);
      DispatchMessageW(&message);
    }

    if (IsWindow(hwnd))
      DestroyWindow(hwnd);

    std::lock_guard<std::mutex> lock(m_control_mutex);
    if (m_hwnd == hwnd)
      m_hwnd = nullptr;
  }

  LRESULT RegisterClient(HWND hwnd, LPARAM id)
  {
    if (!hwnd || !IsWindow(hwnd))
      return 1;

    auto existing = std::find_if(m_clients.begin(), m_clients.end(),
                                 [id](const RegisteredClient& client) { return client.id == id; });
    const bool was_registered = (existing != m_clients.end());

    if (was_registered)
    {
      existing->hwnd = hwnd;
    }
    else
    {
      m_clients.push_back({id, hwnd});
    }

    // MAME sends every currently known output when a client registers (or
    // re-registers), allowing clients to synchronize without waiting for the
    // next cabinet state change.
    for (const OutputItem& item : m_outputs)
      PostMessageW(hwnd, m_message_update_state, static_cast<WPARAM>(item.id), static_cast<LPARAM>(item.value));

    return was_registered ? 1 : 0;
  }

  LRESULT UnregisterClient(HWND hwnd, LPARAM id)
  {
    (void)hwnd;

    const auto found =
      std::find_if(m_clients.begin(), m_clients.end(), [id](const RegisteredClient& client) { return client.id == id; });
    if (found == m_clients.end())
      return 1;

    m_clients.erase(found);
    return 0;
  }

  LRESULT SendIdString(HWND hwnd, u32 id)
  {
    if (!hwnd || !IsWindow(hwnd))
      return 1;

    std::string_view name;
    if (id == 0)
    {
      name = m_game_id;
    }
    else if (id == 1)
    {
      // Current MAME reserves ID 1 for its synthetic pause output. ArcadeDuck
      // does not publish pause state yet, but returning the canonical name
      // preserves ID lookup compatibility.
      name = "pause";
    }
    else
    {
      const auto found =
        std::find_if(m_outputs.begin(), m_outputs.end(), [id](const OutputItem& item) { return item.id == id; });
      if (found != m_outputs.end())
        name = found->name;
    }

    const size_t data_length = sizeof(CopyDataIdString) + name.size() + 1;
    std::vector<u8> buffer(data_length);
    CopyDataIdString* const data = reinterpret_cast<CopyDataIdString*>(buffer.data());
    data->id = id;
    std::memcpy(data->string, name.data(), name.size());
    data->string[name.size()] = '\0';

    COPYDATASTRUCT copydata = {};
    copydata.dwData = COPYDATA_MESSAGE_ID_STRING;
    copydata.cbData = static_cast<DWORD>(data_length);
    copydata.lpData = data;

    SendMessageW(hwnd, WM_COPYDATA, reinterpret_cast<WPARAM>(m_hwnd), reinterpret_cast<LPARAM>(&copydata));
    return 0;
  }

  void DrainPendingUpdates()
  {
    std::deque<PendingUpdate> updates;
    {
      std::lock_guard<std::mutex> lock(m_queue_mutex);
      updates.swap(m_pending_updates);
    }

    for (PendingUpdate& update : updates)
    {
      OutputItem* item = nullptr;

      const auto existing = m_output_indices.find(update.name);
      if (existing == m_output_indices.end())
      {
        const size_t index = m_outputs.size();
        OutputItem new_item;
        new_item.id = m_next_output_id++;
        new_item.value = update.value;
        new_item.name = std::move(update.name);
        m_outputs.push_back(std::move(new_item));
        m_output_indices.emplace(m_outputs.back().name, index);
        item = &m_outputs.back();
      }
      else
      {
        item = &m_outputs[existing->second];
        item->value = update.value;
      }

      for (auto client = m_clients.begin(); client != m_clients.end();)
      {
        if (!client->hwnd || !IsWindow(client->hwnd))
        {
          client = m_clients.erase(client);
          continue;
        }

        PostMessageW(client->hwnd, m_message_update_state, static_cast<WPARAM>(item->id),
                     static_cast<LPARAM>(item->value));
        ++client;
      }
    }
  }

  std::mutex m_control_mutex;
  std::condition_variable m_start_cv;
  std::thread m_thread;
  HWND m_hwnd = nullptr;
  DWORD m_thread_id = 0;
  bool m_start_complete = false;
  bool m_start_success = false;
  std::string m_game_id;

  std::mutex m_queue_mutex;
  std::deque<PendingUpdate> m_pending_updates;

  std::vector<RegisteredClient> m_clients;
  std::vector<OutputItem> m_outputs;
  std::unordered_map<std::string, size_t> m_output_indices;
  u32 m_next_output_id = FIRST_OUTPUT_ID;

  UINT m_message_start = 0;
  UINT m_message_stop = 0;
  UINT m_message_update_state = 0;
  UINT m_message_register_client = 0;
  UINT m_message_unregister_client = 0;
  UINT m_message_get_id_string = 0;
};

Transport s_transport;

} // namespace

void Start(std::string_view game_id)
{
  s_transport.Start(game_id);
}

void Stop()
{
  s_transport.Stop();
}

void Notify(std::string_view name, s32 value)
{
  s_transport.Notify(name, value);
}

} // namespace Arcade::Output::Win32Transport

#else

namespace Arcade::Output::Win32Transport {

void Start(std::string_view game_id)
{
  (void)game_id;
}

void Stop()
{
}

void Notify(std::string_view name, s32 value)
{
  (void)name;
  (void)value;
}

} // namespace Arcade::Output::Win32Transport

#endif