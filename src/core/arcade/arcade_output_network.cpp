// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/arcade_output_network.h"

#include "common/error.h"
#include "common/log.h"

#include "util/sockets.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

Log_SetChannel(ArcadeOutputNetwork);

namespace Arcade::Output::NetworkTransport {
namespace {

constexpr u32 MAME_NETWORK_OUTPUT_PORT = 8000;
constexpr u32 POLL_INTERVAL_MS = 10;
constexpr u32 SHUTDOWN_FLUSH_POLLS = 25;

class Transport;
Transport* s_client_owner = nullptr;

class NetworkClient final : public BufferedStreamSocket
{
public:
  NetworkClient(SocketMultiplexer& multiplexer, SocketDescriptor descriptor)
    : BufferedStreamSocket(multiplexer, descriptor, 256, 65536)
  {
  }

  bool SendLine(std::string_view line)
  {
    const size_t total_size = line.size() + 1;
    std::unique_lock lock = GetLock();
    const std::span<u8> buffer = AcquireWriteBuffer(total_size, false);
    if (buffer.size() < total_size)
      return false;

    if (!line.empty())
      std::memcpy(buffer.data(), line.data(), line.size());
    buffer[line.size()] = '\r';
    ReleaseWriteBuffer(total_size);
    return true;
  }

protected:
  void OnConnected() override;
  void OnDisconnected(const Error& error) override;
  void OnRead() override;
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
    m_stop_requested.store(false, std::memory_order_release);

    {
      std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
      m_pending_messages.clear();
    }

    m_thread = std::thread(&Transport::ThreadMain, this);
    m_start_cv.wait(lock, [this]() { return m_start_complete; });
    const bool success = m_start_success;
    lock.unlock();

    if (!success)
    {
      if (m_thread.joinable())
        m_thread.join();

      m_game_id.clear();
      WARNING_LOG("MAME TCP output transport is unavailable. ArcadeDuck will continue without TCP output.");
      return;
    }

    m_running.store(true, std::memory_order_release);
    INFO_LOG("MAME-compatible TCP output transport listening on port {} for '{}'.", MAME_NETWORK_OUTPUT_PORT, m_game_id);
  }

  void Stop()
  {
    if (!m_thread.joinable())
      return;

    if (m_running.exchange(false, std::memory_order_acq_rel))
      QueueRawMessage("mame_stop = 1");

    m_stop_requested.store(true, std::memory_order_release);
    m_thread.join();

    {
      std::lock_guard<std::mutex> lock(m_control_mutex);
      m_start_complete = false;
      m_start_success = false;
    }

    {
      std::lock_guard<std::mutex> lock(m_queue_mutex);
      m_pending_messages.clear();
    }

    m_clients.clear();
    m_game_id.clear();
  }

  void Notify(std::string_view name, s32 value)
  {
    if (!m_running.load(std::memory_order_acquire) || name.empty())
      return;

    std::string message;
    message.reserve(name.size() + 24);
    message.append(name.data(), name.size());
    message.append(" = ");
    message.append(std::to_string(value));
    QueueRawMessage(std::move(message));
  }

  void ClientConnected(const std::shared_ptr<NetworkClient>& client)
  {
    if (!client)
      return;

    m_clients.emplace_back(client);

    std::string message("mame_start = ");
    message.append(m_game_id);

    if (!client->SendLine(message))
      WARNING_LOG("Failed to queue MAME TCP startup message for client {}.", client->GetRemoteAddress().ToString());
  }

  void ClientDisconnected(NetworkClient* client, const Error& error)
  {
    m_clients.erase(
      std::remove_if(m_clients.begin(), m_clients.end(),
                     [client](const std::weak_ptr<NetworkClient>& weak) {
                       const std::shared_ptr<NetworkClient> existing = weak.lock();
                       return !existing || existing.get() == client;
                     }),
      m_clients.end());

    DEV_LOG("MAME TCP output client disconnected: {}", error.GetDescription());
  }

private:
  void SignalStart(bool success)
  {
    {
      std::lock_guard<std::mutex> lock(m_control_mutex);
      m_start_success = success;
      m_start_complete = true;
    }
    m_start_cv.notify_all();
  }

  void QueueRawMessage(std::string message)
  {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    m_pending_messages.push_back(std::move(message));
  }

  void DrainPendingMessages()
  {
    std::deque<std::string> messages;
    {
      std::lock_guard<std::mutex> lock(m_queue_mutex);
      messages.swap(m_pending_messages);
    }

    if (messages.empty())
      return;

    for (const std::string& message : messages)
    {
      for (auto it = m_clients.begin(); it != m_clients.end();)
      {
        const std::shared_ptr<NetworkClient> client = it->lock();
        if (!client || !client->IsConnected())
        {
          it = m_clients.erase(it);
          continue;
        }

        if (!client->SendLine(message))
        {
          WARNING_LOG("MAME TCP output client {} could not keep up; dropping queued output line.",
                      client->GetRemoteAddress().ToString());
        }

        ++it;
      }
    }
  }

  void ThreadMain()
  {
    Error error;
    std::unique_ptr<SocketMultiplexer> multiplexer = SocketMultiplexer::Create(&error);
    if (!multiplexer)
    {
      ERROR_LOG("Failed to initialize MAME TCP output socket multiplexer: {}", error.GetDescription());
      SignalStart(false);
      return;
    }

    const std::optional<SocketAddress> address =
      SocketAddress::Parse(SocketAddress::Type::IPv4, "0.0.0.0", MAME_NETWORK_OUTPUT_PORT, &error);
    if (!address.has_value())
    {
      ERROR_LOG("Failed to create MAME TCP output listen address: {}", error.GetDescription());
      SignalStart(false);
      return;
    }

    s_client_owner = this;
    std::shared_ptr<ListenSocket> listen_socket = multiplexer->CreateListenSocket<NetworkClient>(*address, &error);
    if (!listen_socket)
    {
      s_client_owner = nullptr;
      ERROR_LOG("Failed to listen for MAME TCP output clients on port {}: {}", MAME_NETWORK_OUTPUT_PORT,
                error.GetDescription());
      SignalStart(false);
      return;
    }

    SignalStart(true);

    while (!m_stop_requested.load(std::memory_order_acquire))
    {
      multiplexer->PollEventsWithTimeout(POLL_INTERVAL_MS);
      DrainPendingMessages();
    }

    // Preserve ordering from ClearValues() -> mame_stop = 1 before closing
    // connected clients. Local output clients normally flush immediately, but
    // give pending non-blocking sends a short grace period as well.
    DrainPendingMessages();
    for (u32 i = 0; i < SHUTDOWN_FLUSH_POLLS; i++)
      multiplexer->PollEventsWithTimeout(POLL_INTERVAL_MS);

    multiplexer->CloseAll();
    m_clients.clear();
    s_client_owner = nullptr;
  }

  std::mutex m_control_mutex;
  std::condition_variable m_start_cv;
  std::thread m_thread;
  bool m_start_complete = false;
  bool m_start_success = false;

  std::atomic_bool m_running{false};
  std::atomic_bool m_stop_requested{false};
  std::string m_game_id;

  std::mutex m_queue_mutex;
  std::deque<std::string> m_pending_messages;

  // Accessed only from the network worker thread.
  std::vector<std::weak_ptr<NetworkClient>> m_clients;
};

void NetworkClient::OnConnected()
{
  Error error;
  if (!SetNagleBuffering(false, &error))
    WARNING_LOG("Failed to disable Nagle buffering for MAME TCP output client: {}", error.GetDescription());

  if (s_client_owner)
    s_client_owner->ClientConnected(std::static_pointer_cast<NetworkClient>(shared_from_this()));
}

void NetworkClient::OnDisconnected(const Error& error)
{
  if (s_client_owner)
    s_client_owner->ClientDisconnected(this, error);
}

void NetworkClient::OnRead()
{
  // MAME's network provider can accept mame_message control commands. This
  // phase is output-only, so consume any input without acting on it. In
  // particular, do not re-enable remote save-state behavior.
  const std::span<const u8> buffer = AcquireReadBuffer();
  if (!buffer.empty())
    ReleaseReadBuffer(buffer.size());
}

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

} // namespace Arcade::Output::NetworkTransport