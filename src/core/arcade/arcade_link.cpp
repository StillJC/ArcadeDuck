// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/arcade_link.h"
#include "core/settings.h"

#include "common/error.h"
#include "common/log.h"

#include "util/sockets.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

Log_SetChannel(ArcadeLink);

namespace Arcade::Link {
namespace {

// Message: u16 little-endian length of the bytes that follow, u8 type, payload.
//   HELLO   magic "ADLK", u8 version, u8 cabinet id, session string
//   MEMBERS u8 connected-cabinet mask (sent by cabinet 1)
//   DATA    u8 source cabinet, u8 destination cabinet, message bytes
constexpr std::array<u8, 4> HELLO_MAGIC = {'A', 'D', 'L', 'K'};
constexpr u8 PROTOCOL_VERSION = 1;
constexpr u8 MSG_HELLO = 1;
constexpr u8 MSG_MEMBERS = 2;
constexpr u8 MSG_DATA = 3;
constexpr size_t MAX_WIRE_SIZE = Transport::MAX_MESSAGE_SIZE + 16;

constexpr u32 POLL_INTERVAL_MS = 1;
constexpr auto RETRY_INTERVAL = std::chrono::seconds(1);
constexpr size_t MAX_QUEUED_MESSAGES = 4096;
constexpr size_t SOCKET_BUFFER_SIZE = 256 * 1024;

class NetworkTransport;
NetworkTransport* s_owner = nullptr;

class PeerSocket final : public BufferedStreamSocket
{
public:
  PeerSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor)
    : BufferedStreamSocket(multiplexer, descriptor, SOCKET_BUFFER_SIZE, SOCKET_BUFFER_SIZE)
  {
  }

  bool SendMessage(u8 type, std::span<const u8> header, std::span<const u8> payload);

  u8 cabinet = 0;

protected:
  void OnConnected() override;
  void OnDisconnected(const Error& error) override;
  void OnRead() override;
};

class NetworkTransport final : public Transport
{
public:
  NetworkTransport(u8 cabinet, std::string session, std::string host, u16 port)
    : m_cabinet(cabinet), m_session(std::move(session)), m_host(std::move(host)), m_port(port)
  {
    m_connected_mask.store(CabinetBit(m_cabinet), std::memory_order_relaxed);
    m_thread = std::thread(&NetworkTransport::ThreadMain, this);
  }

  ~NetworkTransport() override
  {
    m_stop_requested.store(true, std::memory_order_release);
    if (m_thread.joinable())
      m_thread.join();
  }

  u8 GetCabinetID() const override { return m_cabinet; }

  u32 GetConnectedMask() const override { return m_connected_mask.load(std::memory_order_acquire); }

  void Send(u8 cabinet, std::span<const u8> message) override
  {
    if (message.empty() || message.size() > MAX_MESSAGE_SIZE || cabinet == m_cabinet ||
        (GetConnectedMask() & CabinetBit(cabinet)) == 0)
    {
      return;
    }

    std::lock_guard<std::mutex> lock(m_queue_mutex);
    if (m_tx_queue.size() >= MAX_QUEUED_MESSAGES)
    {
      if (!m_overflow_logged)
        WARNING_LOG("System link send queue full; dropping messages");
      m_overflow_logged = true;
      return;
    }
    m_tx_queue.push_back(Message{cabinet, std::vector<u8>(message.begin(), message.end())});
  }

  bool Receive(u8* cabinet, std::vector<u8>* message) override
  {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    if (m_rx_queue.empty())
      return false;

    *cabinet = m_rx_queue.front().cabinet;
    *message = std::move(m_rx_queue.front().data);
    m_rx_queue.pop_front();
    return true;
  }

  void PeerConnected(const std::shared_ptr<PeerSocket>& peer);
  void PeerDisconnected(PeerSocket* peer, const Error& error);
  void PeerMessage(PeerSocket* peer, u8 type, std::span<const u8> payload);

private:
  struct Message
  {
    u8 cabinet;
    std::vector<u8> data;
  };

  static u32 CabinetBit(u8 cabinet) { return 1u << (cabinet - 1); }

  bool IsHub() const { return m_cabinet == 1; }

  void ThreadMain();
  void UpdateConnection(SocketMultiplexer& multiplexer);
  void DrainOutgoing();
  void Deliver(u8 source, u8 destination, std::span<const u8> data, PeerSocket* from);
  void SendHello(PeerSocket& peer);
  void PublishMembers();
  PeerSocket* FindPeer(u8 cabinet);

  const u8 m_cabinet;
  const std::string m_session;
  const std::string m_host;
  const u16 m_port;

  std::thread m_thread;
  std::atomic_bool m_stop_requested{false};
  std::atomic<u32> m_connected_mask{0};

  std::mutex m_queue_mutex;
  std::deque<Message> m_tx_queue;
  std::deque<Message> m_rx_queue;
  bool m_overflow_logged = false;

  // Worker thread only.
  std::deque<Message> m_unsent;
  std::shared_ptr<ListenSocket> m_listener;
  std::vector<std::shared_ptr<PeerSocket>> m_peers;
  std::chrono::steady_clock::time_point m_next_retry{};
  bool m_failure_logged = false;
  bool m_refusal_logged = false;
  bool m_joined = false;
};

bool PeerSocket::SendMessage(u8 type, std::span<const u8> header, std::span<const u8> payload)
{
  const size_t body = 1 + header.size() + payload.size();
  if (body > MAX_WIRE_SIZE)
    return false;

  std::unique_lock lock = GetLock();
  const std::span<u8> buffer = AcquireWriteBuffer(body + 2, false);
  if (buffer.size() < body + 2)
    return false;

  buffer[0] = static_cast<u8>(body);
  buffer[1] = static_cast<u8>(body >> 8);
  buffer[2] = type;
  std::copy(header.begin(), header.end(), buffer.begin() + 3);
  std::copy(payload.begin(), payload.end(), buffer.begin() + 3 + header.size());
  ReleaseWriteBuffer(body + 2);
  return true;
}

void PeerSocket::OnConnected()
{
  SetNagleBuffering(false);
  if (s_owner)
    s_owner->PeerConnected(std::static_pointer_cast<PeerSocket>(shared_from_this()));
}

void PeerSocket::OnDisconnected(const Error& error)
{
  if (s_owner)
    s_owner->PeerDisconnected(this, error);
}

void PeerSocket::OnRead()
{
  const std::span<const u8> buffer = AcquireReadBuffer();
  size_t consumed = 0;
  while (buffer.size() - consumed >= 2)
  {
    const size_t body = static_cast<size_t>(buffer[consumed]) | (static_cast<size_t>(buffer[consumed + 1]) << 8);
    if (body == 0 || body > MAX_WIRE_SIZE)
    {
      WARNING_LOG("Invalid link message from {}; closing connection", GetRemoteAddress().ToString());
      ReleaseReadBuffer(buffer.size());
      Close();
      return;
    }
    if (buffer.size() - consumed - 2 < body)
      break;

    const std::span<const u8> message = buffer.subspan(consumed + 2, body);
    consumed += 2 + body;
    if (s_owner)
      s_owner->PeerMessage(this, message[0], message.subspan(1));
    if (!IsConnected())
      return;
  }
  ReleaseReadBuffer(consumed);
}

void NetworkTransport::ThreadMain()
{
  Error error;
  std::unique_ptr<SocketMultiplexer> multiplexer = SocketMultiplexer::Create(&error);
  if (!multiplexer)
  {
    ERROR_LOG("System link socket multiplexer failed: {}", error.GetDescription());
    return;
  }

  s_owner = this;
  INFO_LOG("System link cabinet {} session '{}' {} {}:{}", m_cabinet, m_session,
           IsHub() ? "hosting on port" : "connecting to", IsHub() ? "0.0.0.0" : m_host, m_port);

  while (!m_stop_requested.load(std::memory_order_acquire))
  {
    UpdateConnection(*multiplexer);
    DrainOutgoing();
    if (!multiplexer->PollEventsWithTimeout(POLL_INTERVAL_MS))
      std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
  }

  multiplexer->CloseAll();
  m_peers.clear();
  m_listener.reset();
  s_owner = nullptr;
}

void NetworkTransport::UpdateConnection(SocketMultiplexer& multiplexer)
{
  const auto now = std::chrono::steady_clock::now();
  if (now < m_next_retry)
    return;

  Error error;
  if (IsHub())
  {
    if (m_listener)
      return;

    const std::optional<SocketAddress> address =
      SocketAddress::Parse(SocketAddress::Type::IPv4, "0.0.0.0", m_port, &error);
    if (address.has_value())
      m_listener = multiplexer.CreateListenSocket<PeerSocket>(*address, &error);
    if (!m_listener)
    {
      if (!m_failure_logged)
        ERROR_LOG("System link failed to listen on port {}: {}", m_port, error.GetDescription());
      m_failure_logged = true;
      m_next_retry = now + RETRY_INTERVAL;
    }
    return;
  }

  if (!m_peers.empty())
    return;

  // connect() blocks on this worker thread only.
  const std::optional<SocketAddress> address =
    SocketAddress::Parse(SocketAddress::Type::IPv4, m_host.c_str(), m_port, &error);
  std::shared_ptr<PeerSocket> peer;
  if (address.has_value())
    peer = multiplexer.ConnectStreamSocket<PeerSocket>(*address, &error);
  if (!peer)
  {
    if (!m_failure_logged)
      WARNING_LOG("System link cannot reach cabinet 1 at {}:{}: {}", m_host, m_port, error.GetDescription());
    m_failure_logged = true;
    m_next_retry = std::chrono::steady_clock::now() + RETRY_INTERVAL;
  }
}

void NetworkTransport::PeerConnected(const std::shared_ptr<PeerSocket>& peer)
{
  m_peers.push_back(peer);
  if (!IsHub())
  {
    // The hub is cabinet 1 by definition.
    peer->cabinet = 1;
    SendHello(*peer);
  }
}

void NetworkTransport::PeerDisconnected(PeerSocket* peer, const Error& error)
{
  const auto it = std::find_if(m_peers.begin(), m_peers.end(),
                               [peer](const std::shared_ptr<PeerSocket>& p) { return p.get() == peer; });
  if (it == m_peers.end())
    return;

  const u8 cabinet = peer->cabinet;
  m_peers.erase(it);

  if (IsHub())
  {
    if (cabinet != 0)
      INFO_LOG("System link cabinet {} disconnected: {}", cabinet, error.GetDescription());
    PublishMembers();
    return;
  }

  if (m_joined)
  {
    INFO_LOG("System link cabinet 1 disconnected: {}", error.GetDescription());
  }
  else if (!m_failure_logged)
  {
    WARNING_LOG("System link cabinet 1 refused this cabinet; check the cabinet ID and that both run the same game");
    m_failure_logged = true;
  }

  m_joined = false;
  m_unsent.clear();
  m_connected_mask.store(CabinetBit(m_cabinet), std::memory_order_release);
  m_next_retry = std::chrono::steady_clock::now() + RETRY_INTERVAL;
}

void NetworkTransport::PeerMessage(PeerSocket* peer, u8 type, std::span<const u8> payload)
{
  switch (type)
  {
    case MSG_HELLO:
    {
      if (!IsHub() || peer->cabinet != 0 || payload.size() < HELLO_MAGIC.size() + 2 ||
          !std::equal(HELLO_MAGIC.begin(), HELLO_MAGIC.end(), payload.begin()))
      {
        peer->Close();
        return;
      }

      const u8 version = payload[4];
      const u8 cabinet = payload[5];
      const std::string_view session(reinterpret_cast<const char*>(payload.data() + 6), payload.size() - 6);
      if (version != PROTOCOL_VERSION || session != m_session || cabinet < 2 || cabinet > MAX_CABINETS ||
          FindPeer(cabinet))
      {
        if (!m_refusal_logged)
        {
          WARNING_LOG("System link refused cabinet {} session '{}' version {} from {}", cabinet, session, version,
                      peer->GetRemoteAddress().ToString());
        }
        m_refusal_logged = true;
        peer->Close();
        return;
      }

      m_refusal_logged = false;
      peer->cabinet = cabinet;
      INFO_LOG("System link cabinet {} joined from {}", cabinet, peer->GetRemoteAddress().ToString());
      PublishMembers();
      return;
    }

    case MSG_MEMBERS:
    {
      if (IsHub() || payload.size() != 1)
        return;

      const u32 mask = payload[0] | CabinetBit(m_cabinet);
      if (mask != GetConnectedMask())
        INFO_LOG("System link cabinets connected: mask {:02X}", mask);
      m_connected_mask.store(mask, std::memory_order_release);
      m_joined = true;
      m_failure_logged = false;
      return;
    }

    case MSG_DATA:
    {
      if (peer->cabinet == 0 || payload.size() < 3)
        return;
      Deliver(payload[0], payload[1], payload.subspan(2), peer);
      return;
    }

    default:
      return;
  }
}

void NetworkTransport::Deliver(u8 source, u8 destination, std::span<const u8> data, PeerSocket* from)
{
  if (destination == m_cabinet)
  {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    if (m_rx_queue.size() < MAX_QUEUED_MESSAGES)
      m_rx_queue.push_back(Message{source, std::vector<u8>(data.begin(), data.end())});
    return;
  }

  // Only the hub relays between other cabinets.
  if (!IsHub())
    return;

  PeerSocket* target = FindPeer(destination);
  if (target && target != from)
  {
    const std::array<u8, 2> header = {source, destination};
    target->SendMessage(MSG_DATA, header, data);
  }
}

void NetworkTransport::DrainOutgoing()
{
  {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    while (!m_tx_queue.empty())
    {
      m_unsent.push_back(std::move(m_tx_queue.front()));
      m_tx_queue.pop_front();
    }
  }

  // Messages stay queued, in order, while the socket send buffer is full.
  while (!m_unsent.empty())
  {
    const Message& out = m_unsent.front();
    PeerSocket* target = IsHub() ? FindPeer(out.cabinet) : (m_peers.empty() ? nullptr : m_peers.front().get());
    if (target)
    {
      const std::array<u8, 2> header = {m_cabinet, out.cabinet};
      if (!target->SendMessage(MSG_DATA, header, out.data))
        break;
    }
    m_unsent.pop_front();
  }
}

void NetworkTransport::SendHello(PeerSocket& peer)
{
  std::array<u8, 6> header = {HELLO_MAGIC[0], HELLO_MAGIC[1], HELLO_MAGIC[2], HELLO_MAGIC[3], PROTOCOL_VERSION,
                              m_cabinet};
  peer.SendMessage(MSG_HELLO, header,
                   std::span<const u8>(reinterpret_cast<const u8*>(m_session.data()), m_session.size()));
}

void NetworkTransport::PublishMembers()
{
  u32 mask = CabinetBit(m_cabinet);
  for (const std::shared_ptr<PeerSocket>& peer : m_peers)
  {
    if (peer->cabinet != 0)
      mask |= CabinetBit(peer->cabinet);
  }

  if (mask != GetConnectedMask())
    INFO_LOG("System link cabinets connected: mask {:02X}", mask);
  m_connected_mask.store(mask, std::memory_order_release);

  const std::array<u8, 1> payload = {static_cast<u8>(mask)};
  for (const std::shared_ptr<PeerSocket>& peer : m_peers)
  {
    if (peer->cabinet != 0)
      peer->SendMessage(MSG_MEMBERS, {}, payload);
  }
}

PeerSocket* NetworkTransport::FindPeer(u8 cabinet)
{
  for (const std::shared_ptr<PeerSocket>& peer : m_peers)
  {
    if (peer->cabinet == cabinet && peer->IsConnected())
      return peer.get();
  }
  return nullptr;
}

} // namespace

std::unique_ptr<Transport> CreateTransport(std::string_view session)
{
  if (!g_settings.system_link_enabled)
    return {};

  const u8 cabinet = std::clamp<u8>(g_settings.system_link_cabinet_id, 1, Transport::MAX_CABINETS);
  const std::string host =
    g_settings.system_link_server_address.empty() ? std::string("127.0.0.1") : g_settings.system_link_server_address;
  return std::make_unique<NetworkTransport>(cabinet, std::string(session), host, g_settings.system_link_port);
}

} // namespace Arcade::Link
