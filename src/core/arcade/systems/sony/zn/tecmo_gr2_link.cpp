// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/sony/zn/tecmo_gr2_link.h"

#include "core/arcade/third_party/z80_superzazu/z80.h"
#include "core/settings.h"
#include "core/system.h"

#include "util/sockets.h"

#include "common/error.h"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

Log_SetChannel(SonyZNTecmoGR2Link);

namespace SonyZN::TecmoGR2Link {
namespace {

constexpr u32 Z80_CLOCK_HZ = 4'000'000;
constexpr u32 LINK_ROM_SIZE = 0x40000;
constexpr u32 Z80_ROM_END = 0x7fff;
constexpr u32 Z80_RAM_BASE = 0x8000;
constexpr u32 Z80_RAM_SIZE = 0x8000;
constexpr u32 FIFO_CAPACITY = 1024;
constexpr u32 TRACE_LIMIT = 96;

constexpr u8 HOST_IRQ_VECTOR = UINT8_C(0xe7);       // IM0 RST 20h.
constexpr u8 CONTROLLER_IRQ_VECTOR = UINT8_C(0xef); // IM0 RST 28h.

constexpr u16 TRANSPORT_DEFAULT_PORT = 19702;
constexpr u8 TRANSPORT_MIN_NODES = 2;
constexpr u8 TRANSPORT_MAX_NODES = 4;
constexpr size_t TRANSPORT_PAYLOAD_SIZE = 0x48;
constexpr size_t TRANSPORT_HEADER_SIZE = 12;
constexpr size_t TRANSPORT_PACKET_SIZE = TRANSPORT_HEADER_SIZE + TRANSPORT_PAYLOAD_SIZE;
constexpr u8 TRANSPORT_VERSION = 1;
constexpr u8 TRANSPORT_PACKET_CANDIDATE_FRAME = 1;
constexpr std::array<u8, 4> TRANSPORT_MAGIC = {'G', 'R', '2', 'L'};
constexpr std::array<u8, 16> DYNAMIC_31_SIGNATURE = {
  0x31, 0x00, 0x00, 0x48, 0x00, 0x00, 0xb4, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

class LinkSocket;

struct ByteFIFO
{
  std::array<u8, FIFO_CAPACITY> data{};
  u32 read = 0;
  u32 write = 0;
  u32 count = 0;

  void Clear()
  {
    read = 0;
    write = 0;
    count = 0;
  }

  bool Empty() const { return count == 0; }
  bool Full() const { return count == FIFO_CAPACITY; }

  bool Push(u8 value)
  {
    if (Full())
      return false;

    data[write] = value;
    write = (write + 1) % FIFO_CAPACITY;
    count++;
    return true;
  }

  u8 Pop()
  {
    if (Empty())
      return UINT8_C(0xff);

    const u8 value = data[read];
    read = (read + 1) % FIFO_CAPACITY;
    count--;
    return value;
  }
};

struct ControllerState
{
  bool mset_fifo_active = false;
  bool mset_complete = false;
  bool irq_latched = false;
  std::array<u8, 64> fifo{};
  u32 fifo_count = 0;
  u32 status_reads = 0;
  u32 control_writes = 0;
  u32 command_requests = 0;
};

struct State
{
  bool active = false;
  z80 cpu = {};
  std::vector<u8> link_program;
  std::array<u8, Z80_RAM_SIZE> ram{};

  ByteFIFO main_to_link;
  ByteFIFO link_to_main;
  ControllerState controller;

  u8 ymz_register = 0;

  s64 cycle_balance = 0;
  u64 host_target_cycles = 0;
  u64 scheduled_target_cycles = 0;
  u64 host_tick_fraction = 0;
  u64 last_host_sync_ticks = 0;
  u64 last_ticks_per_second = 0;
  u64 total_cycles = 0;

  bool host_irq_latched = false;
  bool fifo_overflow_logged = false;
  bool unmapped_port_read_logged = false;
  bool unmapped_port_write_logged = false;
  bool unmapped_memory_read_logged = false;
  bool unmapped_memory_write_logged = false;
  bool controller_control_unknown_logged = false;
  bool controller_fifo_overflow_logged = false;

  u32 trace_count = 0;
  u32 ymz_write_count = 0;
  u32 main_data_read_count = 0;
  u32 main_data_write_count = 0;
  u32 main_status_read_count = 0;
  u32 main_irq_write_count = 0;
  u32 controller_irq_count = 0;

  bool transport_config_checked = false;
  bool transport_enabled = false;
  bool transport_identity_valid = false;
  bool transport_identity_invalid_logged = false;
  bool transport_connect_failure_logged = false;
  u16 transport_port = TRANSPORT_DEFAULT_PORT;
  std::string transport_server_address = "127.0.0.1";
  u8 transport_node_id = 0;
  u8 transport_total_nodes = 0;
  u32 transport_tx_sequence = 0;
  u32 transport_tx_count = 0;
  u32 transport_rx_count = 0;
  u32 transport_relay_count = 0;
  u64 transport_next_retry_tick = 0;
  SocketMultiplexer* transport_multiplexer = nullptr;
  std::shared_ptr<ListenSocket> transport_listener;
  std::vector<std::shared_ptr<LinkSocket>> transport_peers;
  std::array<u8, TRANSPORT_PAYLOAD_SIZE> transport_last_tx{};
  bool transport_have_last_tx = false;
  std::array<u8, TRANSPORT_PAYLOAD_SIZE> transport_last_rx{};
  bool transport_have_last_rx = false;
};

class LinkSocket final : public BufferedStreamSocket
{
public:
  LinkSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor)
    : BufferedStreamSocket(multiplexer, descriptor, 4096, 4096)
  {
  }

  bool SendCandidateFrame(const std::array<u8, TRANSPORT_PAYLOAD_SIZE>& payload);
  bool SendPacket(std::span<const u8> packet);

protected:
  void OnConnected() override;
  void OnDisconnected(const Error& error) override;
  void OnRead() override;
};

State s_state;

u8 ReadTransportNodeID()
{
  return s_state.ram[UINT16_C(0x8025) - Z80_RAM_BASE];
}

u8 ReadTransportTotalNodes()
{
  return s_state.ram[UINT16_C(0x8026) - Z80_RAM_BASE];
}

bool IsHostBootstrapComplete()
{
  return s_state.ram[UINT16_C(0x8028) - Z80_RAM_BASE] != 0;
}

void ScheduleTransportRetry()
{
  const u64 ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());
  s_state.transport_next_retry_tick =
    static_cast<u64>(System::GetGlobalTickCounter()) + (ticks_per_second != 0 ? ticks_per_second : 1);
}

void StopTransport()
{
  // Move the peer references out first. Close() invokes OnDisconnected(), which
  // may otherwise mutate the vector while we are iterating it.
  std::vector<std::shared_ptr<LinkSocket>> peers = std::move(s_state.transport_peers);
  s_state.transport_peers.clear();
  for (const std::shared_ptr<LinkSocket>& peer : peers)
  {
    if (peer && peer->IsConnected())
      peer->Close();
  }

  if (s_state.transport_listener)
  {
    s_state.transport_listener->Close();
    s_state.transport_listener.reset();
  }

  if (s_state.transport_multiplexer)
  {
    System::ReleaseSocketMultiplexer();
    s_state.transport_multiplexer = nullptr;
  }

  s_state.transport_identity_valid = false;
  s_state.transport_have_last_tx = false;
  s_state.transport_have_last_rx = false;
  s_state.transport_next_retry_tick = 0;
}

void ConfigureTransport()
{
  if (s_state.transport_config_checked)
    return;

  s_state.transport_config_checked = true;
  s_state.transport_enabled = g_settings.system_link_enabled;
  if (!s_state.transport_enabled)
    return;

  s_state.transport_port = g_settings.system_link_port;
  s_state.transport_server_address =
    g_settings.system_link_server_address.empty() ? "127.0.0.1" : g_settings.system_link_server_address;

  INFO_LOG("TecmoGR2Link system link enabled host={} port={}",
           s_state.transport_server_address, s_state.transport_port);
}

void ResetTransportConfiguration()
{
  s_state.transport_enabled = false;
  StopTransport();
  s_state.transport_config_checked = false;
  s_state.transport_identity_invalid_logged = false;
  s_state.transport_connect_failure_logged = false;
}

void RegisterTransportPeer(const std::shared_ptr<LinkSocket>& peer)
{
  const size_t max_peers =
    (s_state.transport_identity_valid && s_state.transport_node_id == 0) ?
      static_cast<size_t>(s_state.transport_total_nodes - 1) :
      1;
  if (s_state.transport_peers.size() >= max_peers)
  {
    WARNING_LOG("TecmoGR2Link system link rejecting extra peer {}", peer->GetRemoteAddress().ToString());
    peer->Close();
    return;
  }

  s_state.transport_peers.push_back(peer);
  s_state.transport_have_last_tx = false;
  s_state.transport_connect_failure_logged = false;

  Error error;
  if (!peer->SetNagleBuffering(false, &error))
    WARNING_LOG("TecmoGR2Link virtual cable TCP_NODELAY failed: {}", error.GetDescription());

  INFO_LOG("TecmoGR2Link system link connected local_node={} peer_count={}/{} remote={}",
           s_state.transport_node_id + 1, s_state.transport_peers.size(), max_peers,
           peer->GetRemoteAddress().ToString());
}

void RemoveTransportPeer(LinkSocket* peer, const Error& error)
{
  const auto iter =
    std::find_if(s_state.transport_peers.begin(), s_state.transport_peers.end(),
                 [peer](const std::shared_ptr<LinkSocket>& candidate) { return candidate.get() == peer; });

  if (iter != s_state.transport_peers.end())
    s_state.transport_peers.erase(iter);

  if (!s_state.active || !s_state.transport_enabled)
    return;

  INFO_LOG("TecmoGR2Link system link disconnected local_node={} reason='{}'",
           s_state.transport_node_id + 1, error.GetDescription());

  s_state.transport_have_last_tx = false;
  if (s_state.transport_identity_valid && s_state.transport_node_id != 0)
    ScheduleTransportRetry();
}

bool ValidateTransportPacket(std::span<const u8> packet)
{
  if (packet.size() != TRANSPORT_PACKET_SIZE ||
      !std::equal(TRANSPORT_MAGIC.begin(), TRANSPORT_MAGIC.end(), packet.begin()) ||
      packet[4] != TRANSPORT_VERSION || packet[5] != TRANSPORT_PACKET_CANDIDATE_FRAME)
  {
    return false;
  }

  if (!s_state.transport_identity_valid || packet[6] == s_state.transport_node_id ||
      packet[7] != s_state.transport_total_nodes || packet[6] >= packet[7])
  {
    return false;
  }

  return true;
}

void HandleTransportPacket(LinkSocket* source, std::span<const u8> packet)
{
  const u32 sequence =
    static_cast<u32>(packet[8]) |
    (static_cast<u32>(packet[9]) << 8) |
    (static_cast<u32>(packet[10]) << 16) |
    (static_cast<u32>(packet[11]) << 24);

  if (s_state.transport_node_id == 0)
  {
    u32 relayed = 0;
    for (const std::shared_ptr<LinkSocket>& peer : s_state.transport_peers)
    {
      if (peer && peer.get() != source && peer->IsConnected() && peer->SendPacket(packet))
        relayed++;
    }

    s_state.transport_relay_count += relayed;
    if (relayed != 0 && s_state.transport_relay_count <= 24)
    {
      DEV_LOG("TecmoGR2Link system link relayed peer_node={} seq={} to {} peer(s)",
               packet[6] + 1, sequence, relayed);
    }
  }

  std::copy_n(packet.begin() + TRANSPORT_HEADER_SIZE, TRANSPORT_PAYLOAD_SIZE,
              s_state.transport_last_rx.begin());
  s_state.transport_have_last_rx = true;
  s_state.transport_rx_count++;

  if (s_state.transport_rx_count <= 16)
  {
    DEV_LOG(
      "TecmoGR2Link system link rx candidate #{} peer_node={} seq={} "
      "payload[0..15]={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
      "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
      s_state.transport_rx_count, packet[6] + 1, sequence,
      s_state.transport_last_rx[0], s_state.transport_last_rx[1],
      s_state.transport_last_rx[2], s_state.transport_last_rx[3],
      s_state.transport_last_rx[4], s_state.transport_last_rx[5],
      s_state.transport_last_rx[6], s_state.transport_last_rx[7],
      s_state.transport_last_rx[8], s_state.transport_last_rx[9],
      s_state.transport_last_rx[10], s_state.transport_last_rx[11],
      s_state.transport_last_rx[12], s_state.transport_last_rx[13],
      s_state.transport_last_rx[14], s_state.transport_last_rx[15]);
  }

  // link3118.bin routine 0x084F builds every outbound 0x48-byte B400 block by
  // first copying B200 -> B400, then overwriting only this cabinet's 0x10-byte
  // slot. That makes B200 the firmware's receive aggregate and B400 the
  // forwarded/transmit aggregate. Deliver the peer's opaque aggregate to the
  // same B200 buffer that the real uPD72103A external-memory receive path uses.
  std::copy(s_state.transport_last_rx.begin(), s_state.transport_last_rx.end(),
            s_state.ram.begin() + (UINT16_C(0xb200) - Z80_RAM_BASE));

  // The controller interrupt handler at RST 28h calls firmware routine 0x083A,
  // which advances the A1xx status ring/flags, then acknowledges the controller
  // with OUT (0x80),0x08. An interrupt without received data is
  // insufficient; received data must reach B200 before driving
  // the real firmware interrupt path. No synthetic LSW bytes are fabricated.
  if (!s_state.controller.irq_latched)
  {
    s_state.controller.irq_latched = true;
    s_state.controller_irq_count++;
    z80_gen_int(&s_state.cpu, CONTROLLER_IRQ_VECTOR);

    if (s_state.controller_irq_count <= 16)
    {
      const size_t peer_slot = static_cast<size_t>(packet[6]) * 0x10;
      DEV_LOG(
        "TecmoGR2Link system link receive DMA #{} peer_node={} seq={} -> B200, RST28 "
        "peer_slot={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
        "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
        "meta={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
        s_state.controller_irq_count, packet[6] + 1, sequence,
        s_state.transport_last_rx[peer_slot + 0], s_state.transport_last_rx[peer_slot + 1],
        s_state.transport_last_rx[peer_slot + 2], s_state.transport_last_rx[peer_slot + 3],
        s_state.transport_last_rx[peer_slot + 4], s_state.transport_last_rx[peer_slot + 5],
        s_state.transport_last_rx[peer_slot + 6], s_state.transport_last_rx[peer_slot + 7],
        s_state.transport_last_rx[peer_slot + 8], s_state.transport_last_rx[peer_slot + 9],
        s_state.transport_last_rx[peer_slot + 10], s_state.transport_last_rx[peer_slot + 11],
        s_state.transport_last_rx[peer_slot + 12], s_state.transport_last_rx[peer_slot + 13],
        s_state.transport_last_rx[peer_slot + 14], s_state.transport_last_rx[peer_slot + 15],
        s_state.transport_last_rx[0x40], s_state.transport_last_rx[0x41],
        s_state.transport_last_rx[0x42], s_state.transport_last_rx[0x43],
        s_state.transport_last_rx[0x44], s_state.transport_last_rx[0x45],
        s_state.transport_last_rx[0x46], s_state.transport_last_rx[0x47]);
    }
  }
}

bool LinkSocket::SendCandidateFrame(const std::array<u8, TRANSPORT_PAYLOAD_SIZE>& payload)
{
  const std::span<u8> write_buffer = AcquireWriteBuffer(TRANSPORT_PACKET_SIZE, false);
  if (write_buffer.size() < TRANSPORT_PACKET_SIZE)
    return false;

  std::copy(TRANSPORT_MAGIC.begin(), TRANSPORT_MAGIC.end(), write_buffer.begin());
  write_buffer[4] = TRANSPORT_VERSION;
  write_buffer[5] = TRANSPORT_PACKET_CANDIDATE_FRAME;
  write_buffer[6] = s_state.transport_node_id;
  write_buffer[7] = s_state.transport_total_nodes;

  const u32 sequence = ++s_state.transport_tx_sequence;
  write_buffer[8] = static_cast<u8>(sequence);
  write_buffer[9] = static_cast<u8>(sequence >> 8);
  write_buffer[10] = static_cast<u8>(sequence >> 16);
  write_buffer[11] = static_cast<u8>(sequence >> 24);
  std::copy(payload.begin(), payload.end(), write_buffer.begin() + TRANSPORT_HEADER_SIZE);

  ReleaseWriteBuffer(TRANSPORT_PACKET_SIZE);
  return true;
}

bool LinkSocket::SendPacket(std::span<const u8> packet)
{
  if (packet.size() != TRANSPORT_PACKET_SIZE)
    return false;

  const std::span<u8> write_buffer = AcquireWriteBuffer(TRANSPORT_PACKET_SIZE, false);
  if (write_buffer.size() < TRANSPORT_PACKET_SIZE)
    return false;

  std::copy(packet.begin(), packet.end(), write_buffer.begin());
  ReleaseWriteBuffer(TRANSPORT_PACKET_SIZE);
  return true;
}

void LinkSocket::OnConnected()
{
  RegisterTransportPeer(std::static_pointer_cast<LinkSocket>(shared_from_this()));
}

void LinkSocket::OnDisconnected(const Error& error)
{
  RemoveTransportPeer(this, error);
}

void LinkSocket::OnRead()
{
  const std::span<const u8> buffer = AcquireReadBuffer();
  size_t consumed = 0;

  while ((buffer.size() - consumed) >= TRANSPORT_PACKET_SIZE)
  {
    const std::span<const u8> packet = buffer.subspan(consumed, TRANSPORT_PACKET_SIZE);
    if (!ValidateTransportPacket(packet))
    {
      WARNING_LOG("TecmoGR2Link invalid system-link packet from {}; closing connection",
                  GetRemoteAddress().ToString());
      Close();
      return;
    }

    HandleTransportPacket(this, packet);
    consumed += TRANSPORT_PACKET_SIZE;
  }

  ReleaseReadBuffer(consumed);
}

bool StartTransportListener()
{
  if (s_state.transport_listener)
    return true;

  const u64 now = static_cast<u64>(System::GetGlobalTickCounter());
  if (now < s_state.transport_next_retry_tick)
    return false;

  Error error;
  const std::optional<SocketAddress> address =
    SocketAddress::Parse(SocketAddress::Type::IPv4, "0.0.0.0", s_state.transport_port, &error);
  if (!address.has_value())
  {
    ERROR_LOG("TecmoGR2Link failed to parse system-link listen address: {}", error.GetDescription());
    ScheduleTransportRetry();
    return false;
  }

  if (!s_state.transport_multiplexer)
  {
    s_state.transport_multiplexer = System::GetSocketMultiplexer();
    if (!s_state.transport_multiplexer)
    {
      ScheduleTransportRetry();
      return false;
    }
  }

  s_state.transport_listener =
    s_state.transport_multiplexer->CreateListenSocket<LinkSocket>(address.value(), &error);
  if (!s_state.transport_listener)
  {
    ERROR_LOG("TecmoGR2Link system link failed to listen on {}: {}", address->ToString(), error.GetDescription());
    System::ReleaseSocketMultiplexer();
    s_state.transport_multiplexer = nullptr;
    ScheduleTransportRetry();
    return false;
  }

  INFO_LOG("TecmoGR2Link system link node 1 listening on {}", address->ToString());
  return true;
}

void TryConnectTransportPeer()
{
  if (!s_state.transport_peers.empty())
    return;

  const u64 now = static_cast<u64>(System::GetGlobalTickCounter());
  if (now < s_state.transport_next_retry_tick)
    return;

  Error error;
  const std::optional<SocketAddress> address =
    SocketAddress::Parse(SocketAddress::Type::IPv4, s_state.transport_server_address.c_str(),
                         s_state.transport_port, &error);
  if (!address.has_value())
  {
    if (!s_state.transport_connect_failure_logged)
    {
      s_state.transport_connect_failure_logged = true;
      WARNING_LOG("TecmoGR2Link invalid system-link host IPv4 address '{}': {}",
                  s_state.transport_server_address, error.GetDescription());
    }
    ScheduleTransportRetry();
    return;
  }

  if (!s_state.transport_multiplexer)
  {
    s_state.transport_multiplexer = System::GetSocketMultiplexer();
    if (!s_state.transport_multiplexer)
      return;
  }

  const std::shared_ptr<LinkSocket> peer =
    s_state.transport_multiplexer->ConnectStreamSocket<LinkSocket>(address.value(), &error);
  if (!peer)
  {
    if (!s_state.transport_connect_failure_logged)
    {
      s_state.transport_connect_failure_logged = true;
      INFO_LOG("TecmoGR2Link system link node {} waiting for node 1 on {}: {}",
               s_state.transport_node_id + 1, address->ToString(), error.GetDescription());
    }
    ScheduleTransportRetry();
    return;
  }

  s_state.transport_connect_failure_logged = false;
}

void ServiceTransport()
{
  const std::string server_address =
    g_settings.system_link_server_address.empty() ? "127.0.0.1" : g_settings.system_link_server_address;

  if (s_state.transport_config_checked &&
      (s_state.transport_enabled != g_settings.system_link_enabled ||
       (g_settings.system_link_enabled &&
        (s_state.transport_port != g_settings.system_link_port ||
         s_state.transport_server_address != server_address))))
  {
    INFO_LOG("TecmoGR2Link system-link settings changed; restarting transport");
    ResetTransportConfiguration();
  }

  ConfigureTransport();
  if (!s_state.transport_enabled || !IsHostBootstrapComplete())
    return;

  const u8 node_id = ReadTransportNodeID();
  const u8 total_nodes = ReadTransportTotalNodes();

  // link3118.bin directly implements four 0x10-byte cabinet slots in its
  // 0x48-byte B200/B400 working block. Keep the transport bounded by that
  // firmware contract instead of inventing additional HLE-only nodes.
  if (total_nodes < TRANSPORT_MIN_NODES || total_nodes > TRANSPORT_MAX_NODES || node_id >= total_nodes)
  {
    if (!s_state.transport_identity_invalid_logged)
    {
      s_state.transport_identity_invalid_logged = true;
      VERBOSE_LOG("TecmoGR2Link system link inactive for firmware node_id={} total_nodes={}; "
               "supported firmware topology is {}-{} cabinets",
               node_id, total_nodes, TRANSPORT_MIN_NODES, TRANSPORT_MAX_NODES);
    }
    return;
  }

  if (!s_state.transport_identity_valid)
  {
    s_state.transport_identity_valid = true;
    s_state.transport_node_id = node_id;
    s_state.transport_total_nodes = total_nodes;
    DEV_LOG("TecmoGR2Link system-link firmware identity local_node={} total_nodes={}",
             node_id + 1, total_nodes);
  }
  else if (s_state.transport_node_id != node_id || s_state.transport_total_nodes != total_nodes)
  {
    VERBOSE_LOG("TecmoGR2Link system-link firmware identity changed; restarting virtual cable");
    StopTransport();
    s_state.transport_config_checked = false;
    s_state.transport_identity_invalid_logged = false;
    return;
  }

  if (node_id == 0)
    StartTransportListener();
  else
    TryConnectTransportPeer();
}

void MaybeTransmitCandidateFrame()
{
  if (!s_state.transport_identity_valid || s_state.transport_peers.empty())
    return;

  // link3118.bin allocates LCW candidates in 0x10-byte slots at A000. RAM[8020]
  // holds the next slot offset, so the preceding slot is the command whose
  // request edge was just asserted.
  const u8 next_slot = s_state.ram[UINT16_C(0x8020) - Z80_RAM_BASE];
  const u8 current_slot = static_cast<u8>(next_slot - UINT8_C(0x10));
  const size_t lcw_offset = (UINT16_C(0xa000) - Z80_RAM_BASE) + current_slot;
  if ((lcw_offset + DYNAMIC_31_SIGNATURE.size()) > s_state.ram.size() ||
      !std::equal(DYNAMIC_31_SIGNATURE.begin(), DYNAMIC_31_SIGNATURE.end(),
                  s_state.ram.begin() + lcw_offset))
  {
    return;
  }

  // The exact firmware path that builds this observed 0x31 candidate first
  // constructs the 0x48-byte working block at B400. The transport forwards that block
  // as an opaque transport block; it deliberately does not assign official
  // uPD72103A command semantics to opcode 0x31.
  std::array<u8, TRANSPORT_PAYLOAD_SIZE> payload{};
  std::copy_n(s_state.ram.begin() + (UINT16_C(0xb400) - Z80_RAM_BASE),
              TRANSPORT_PAYLOAD_SIZE, payload.begin());

  if (s_state.transport_have_last_tx && payload == s_state.transport_last_tx)
    return;

  bool sent = false;
  for (const std::shared_ptr<LinkSocket>& peer : s_state.transport_peers)
  {
    if (peer && peer->IsConnected() && peer->SendCandidateFrame(payload))
      sent = true;
  }

  if (!sent)
    return;

  s_state.transport_last_tx = payload;
  s_state.transport_have_last_tx = true;
  s_state.transport_tx_count++;

  if (s_state.transport_tx_count <= 16)
  {
    DEV_LOG(
      "TecmoGR2Link system link tx candidate #{} local_node={} "
      "payload[0..15]={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
      "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
      s_state.transport_tx_count, s_state.transport_node_id + 1,
      payload[0], payload[1], payload[2], payload[3],
      payload[4], payload[5], payload[6], payload[7],
      payload[8], payload[9], payload[10], payload[11],
      payload[12], payload[13], payload[14], payload[15]);
  }
}

void Trace(const char* operation, u32 value, u32 extra = 0)
{
  if (s_state.trace_count >= TRACE_LIMIT)
    return;

  DEV_LOG("TecmoGR2Link {} value=0x{:02X} extra={} pc=0x{:04X}", operation, value, extra, s_state.cpu.pc);
  s_state.trace_count++;
}

void TraceRAMBlock(const char* label, u16 address)
{
  if (s_state.trace_count >= TRACE_LIMIT || address < Z80_RAM_BASE ||
      static_cast<u32>(address - Z80_RAM_BASE) + 16 > s_state.ram.size())
  {
    return;
  }

  const u32 offset = address - Z80_RAM_BASE;
  DEV_LOG(
    "TecmoGR2Link controller {} addr=0x{:04X} "
    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
    label, address,
    s_state.ram[offset + 0], s_state.ram[offset + 1], s_state.ram[offset + 2], s_state.ram[offset + 3],
    s_state.ram[offset + 4], s_state.ram[offset + 5], s_state.ram[offset + 6], s_state.ram[offset + 7],
    s_state.ram[offset + 8], s_state.ram[offset + 9], s_state.ram[offset + 10], s_state.ram[offset + 11],
    s_state.ram[offset + 12], s_state.ram[offset + 13], s_state.ram[offset + 14], s_state.ram[offset + 15]);
  s_state.trace_count++;
}

void TraceControllerCommandState()
{
  u32 matches = 0;
  for (u32 address = 0xa000; address < 0xa200 && matches < 4; address += 0x10)
  {
    const u8 command = s_state.ram[address - Z80_RAM_BASE];
    if (command == UINT8_C(0x31) || command == UINT8_C(0x35) ||
        command == UINT8_C(0x37) || command == UINT8_C(0x38))
    {
      TraceRAMBlock("LCW candidate", static_cast<u16>(address));
      matches++;
    }
  }

  if (matches == 0)
  {
    TraceRAMBlock("A000", UINT16_C(0xa000));
    TraceRAMBlock("A100", UINT16_C(0xa100));
  }

  TraceRAMBlock("B400[00]", UINT16_C(0xb400));
  TraceRAMBlock("B400[10]", UINT16_C(0xb410));
  TraceRAMBlock("B400[20]", UINT16_C(0xb420));
  TraceRAMBlock("B400[30]", UINT16_C(0xb430));
  TraceRAMBlock("B400[40]", UINT16_C(0xb440));
}

void ResetController()
{
  s_state.controller = {};
}

u8 ReadControllerStatus()
{
  s_state.controller.status_reads++;

  // Implement only the readiness bits proven by link3118.bin.
  // Returning zero means CRST completion (bit 1 clear) and command-request
  // acceptance (bit 5 clear). Unknown link/DMA status is intentionally not
  // fabricated until the first real firmware trace tells us what is required.
  if (s_state.trace_count < 12)
    Trace("uPD72103 status read", UINT8_C(0x00), s_state.controller.status_reads);

  return UINT8_C(0x00);
}

void ControllerControlWrite(u8 value)
{
  s_state.controller.control_writes++;
  Trace("uPD72103 control write", value, s_state.controller.control_writes);

  if (value & UINT8_C(0x02))
  {
    ResetController();
    return;
  }

  if (value & UINT8_C(0x08))
    s_state.controller.irq_latched = false;

  if (value & UINT8_C(0x01))
  {
    s_state.controller.command_requests++;

    if (s_state.controller.mset_fifo_active && !s_state.controller.mset_complete)
    {
      const auto& fifo = s_state.controller.fifo;
      DEV_LOG(
        "TecmoGR2Link uPD72103 MSET request bytes={} "
        "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
        s_state.controller.fifo_count, fifo[0], fifo[1], fifo[2], fifo[3],
        fifo[4], fifo[5], fifo[6], fifo[7]);
      s_state.controller.mset_complete = true;
      s_state.controller.mset_fifo_active = false;
      s_state.controller.fifo_count = 0;
    }
    else
    {
      if (s_state.controller.command_requests <= 32)
      {
        DEV_LOG("TecmoGR2Link uPD72103 command request #{} pc=0x{:04X}",
                 s_state.controller.command_requests, s_state.cpu.pc);
        TraceControllerCommandState();
      }

      MaybeTransmitCandidateFrame();
      s_state.controller.fifo_count = 0;
    }
  }

  if ((value & UINT8_C(0xf4)) != 0 && !s_state.controller_control_unknown_logged)
  {
    s_state.controller_control_unknown_logged = true;
    DEV_LOG("TecmoGR2Link first unsupported uPD72103 control bits value=0x{:02X} pc=0x{:04X}",
                value, s_state.cpu.pc);
  }
}

void ControllerSelectWrite(u8 value)
{
  Trace("uPD72103 selector write", value);

  if (value == UINT8_C(0x05))
  {
    s_state.controller.mset_fifo_active = true;
    s_state.controller.fifo_count = 0;
  }
}

void ControllerFIFODataWrite(u8 value)
{
  if (s_state.controller.fifo_count < s_state.controller.fifo.size())
  {
    s_state.controller.fifo[s_state.controller.fifo_count++] = value;
    Trace("uPD72103 FIFO write", value, s_state.controller.fifo_count);
    return;
  }

  if (!s_state.controller_fifo_overflow_logged)
  {
    s_state.controller_fifo_overflow_logged = true;
    DEV_LOG("TecmoGR2Link uPD72103 internal FIFO trace buffer overflow pc=0x{:04X}", s_state.cpu.pc);
  }
}

void RaiseHostIRQ()
{
  if (s_state.host_irq_latched)
    return;

  s_state.host_irq_latched = true;
  z80_gen_int(&s_state.cpu, HOST_IRQ_VECTOR);
  Trace("host IRQ IM0 RST20", HOST_IRQ_VECTOR);
}

u8 ReadMemory(void*, u16 address)
{
  if (address <= Z80_ROM_END)
    return s_state.link_program[address];

  if (address >= Z80_RAM_BASE)
    return s_state.ram[address - Z80_RAM_BASE];

  if (!s_state.unmapped_memory_read_logged)
  {
    s_state.unmapped_memory_read_logged = true;
    DEV_LOG("TecmoGR2Link first unmapped Z80 read address=0x{:04X} pc=0x{:04X}", address, s_state.cpu.pc);
  }

  return UINT8_C(0xff);
}

void WriteMemory(void*, u16 address, u8 value)
{
  if (address >= Z80_RAM_BASE)
  {
    s_state.ram[address - Z80_RAM_BASE] = value;
    return;
  }

  if (!s_state.unmapped_memory_write_logged)
  {
    s_state.unmapped_memory_write_logged = true;
    DEV_LOG("TecmoGR2Link first unmapped Z80 write address=0x{:04X} value=0x{:02X} pc=0x{:04X}",
                address, value, s_state.cpu.pc);
  }
}

u8 PortIn(z80*, u8 port)
{
  switch (port)
  {
    case 0x80:
      return ReadControllerStatus();

    case 0x81:
      Trace("uPD72103 selector read", UINT8_C(0x00));
      return UINT8_C(0x00);

    case 0x83:
      Trace("uPD72103 FIFO read", UINT8_C(0xff));
      return UINT8_C(0xff);

    case 0x84:
      Trace("YMZ address-side read", UINT8_C(0x00));
      return UINT8_C(0x00);

    case 0x85:
      return UINT8_C(0x00);

    case 0x90:
    {
      const u8 value = s_state.main_to_link.Pop();
      Trace("Z80 host FIFO read", value, s_state.main_to_link.count);
      return value;
    }

    case 0x91:
    {
      // link3118.bin polls bit 0 before writing to the PSX and bit 1 before
      // reading from the PSX.
      const u8 status =
        (s_state.link_to_main.Full() ? UINT8_C(0x00) : UINT8_C(0x01)) |
        (s_state.main_to_link.Empty() ? UINT8_C(0x00) : UINT8_C(0x02));
      return status;
    }

    default:
      if (!s_state.unmapped_port_read_logged)
      {
        s_state.unmapped_port_read_logged = true;
        DEV_LOG("TecmoGR2Link first unmapped Z80 port read port=0x{:02X} pc=0x{:04X}", port, s_state.cpu.pc);
      }
      return UINT8_C(0xff);
  }
}

void PortOut(z80*, u8 port, u8 value)
{
  switch (port)
  {
    case 0x80:
      ControllerControlWrite(value);
      return;

    case 0x81:
      ControllerSelectWrite(value);
      return;

    case 0x83:
      ControllerFIFODataWrite(value);
      return;

    case 0x84:
      s_state.ymz_register = value;
      return;

    case 0x85:
      if (s_state.ymz_write_count < 16)
      {
        DEV_LOG("TecmoGR2Link YMZ-facing write reg=0x{:02X} value=0x{:02X} pc=0x{:04X}",
                s_state.ymz_register, value, s_state.cpu.pc);
      }
      s_state.ymz_write_count++;
      return;

    case 0x90:
      if (!s_state.link_to_main.Push(value))
      {
        if (!s_state.fifo_overflow_logged)
        {
          s_state.fifo_overflow_logged = true;
          WARNING_LOG("TecmoGR2Link Z80-to-main FIFO overflow pc=0x{:04X}", s_state.cpu.pc);
        }
      }
      else
      {
        Trace("Z80 host FIFO write", value, s_state.link_to_main.count);
      }
      return;

    case 0x92:
      s_state.host_irq_latched = false;
      Trace("host IRQ acknowledge", value);
      return;

    default:
      if (!s_state.unmapped_port_write_logged)
      {
        s_state.unmapped_port_write_logged = true;
        DEV_LOG("TecmoGR2Link first unmapped Z80 port write port=0x{:02X} value=0x{:02X} pc=0x{:04X}",
                    port, value, s_state.cpu.pc);
      }
      return;
  }
}

void InstallCPUCallbacks()
{
  s_state.cpu.read_byte = ReadMemory;
  s_state.cpu.write_byte = WriteMemory;
  s_state.cpu.port_in = PortIn;
  s_state.cpu.port_out = PortOut;
  s_state.cpu.userdata = &s_state;
}

void ResetZ80Core()
{
  z80_init(&s_state.cpu);
  InstallCPUCallbacks();
  s_state.cycle_balance = 0;
}

void RunCycles(u32 cycles)
{
  s_state.cycle_balance += cycles;
  while (s_state.cycle_balance > 0)
  {
    s_state.cpu.cyc = 0;
    z80_step(&s_state.cpu);

    const u32 executed = static_cast<u32>(s_state.cpu.cyc);
    if (executed == 0)
      break;

    s_state.cycle_balance -= static_cast<s64>(executed);
    s_state.total_cycles += executed;
  }
}

void RunToTarget(u64 target_cycles)
{
  if (target_cycles <= s_state.scheduled_target_cycles)
    return;

  u64 cycles = target_cycles - s_state.scheduled_target_cycles;
  s_state.scheduled_target_cycles = target_cycles;

  while (cycles != 0)
  {
    const u32 chunk =
      static_cast<u32>((cycles > UINT64_C(0x01000000)) ? UINT64_C(0x01000000) : cycles);
    RunCycles(chunk);
    cycles -= chunk;
  }
}

void SynchronizeHostTime()
{
  if (!s_state.active)
    return;

  const u64 now = static_cast<u64>(System::GetGlobalTickCounter());
  const u64 ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());
  if (ticks_per_second == 0)
    return;

  if (s_state.last_ticks_per_second == 0)
  {
    s_state.last_host_sync_ticks = now;
    s_state.last_ticks_per_second = ticks_per_second;
    return;
  }

  if (now < s_state.last_host_sync_ticks)
  {
    s_state.last_host_sync_ticks = now;
    s_state.host_tick_fraction = 0;
    s_state.last_ticks_per_second = ticks_per_second;
    return;
  }

  if (s_state.last_ticks_per_second != ticks_per_second)
  {
    s_state.host_tick_fraction =
      (s_state.host_tick_fraction * ticks_per_second) / s_state.last_ticks_per_second;
    s_state.last_ticks_per_second = ticks_per_second;
  }

  const u64 elapsed_ticks = now - s_state.last_host_sync_ticks;
  s_state.last_host_sync_ticks = now;

  const u64 numerator = (elapsed_ticks * Z80_CLOCK_HZ) + s_state.host_tick_fraction;
  s_state.host_target_cycles += numerator / ticks_per_second;
  s_state.host_tick_fraction = numerator % ticks_per_second;
  RunToTarget(s_state.host_target_cycles);
}

} // namespace

bool Initialize(const std::vector<u8>& link_program, Error* error)
{
  Shutdown();

  if (link_program.size() != LINK_ROM_SIZE)
  {
    Error::SetStringFmt(error, "Tecmo Gallop Racer 2 Link Z80 ROM has size {}; expected {} bytes.",
                        link_program.size(), LINK_ROM_SIZE);
    return false;
  }

  s_state.link_program = link_program;
  s_state.active = true;
  Reset();

  VERBOSE_LOG(
    "TecmoGR2Link initialized z80_clock={} program_rom={} ram={} host_fifo='2x 1024-byte' "
    "upd72103='reset/MSET/CCRQ frontend + receive completion' "
    "transport='system-link 0x48-byte aggregate virtual cable'",
    Z80_CLOCK_HZ, s_state.link_program.size(), s_state.ram.size());
  return true;
}

void Reset()
{
  if (!s_state.active)
    return;

  StopTransport();
  s_state.transport_config_checked = false;
  s_state.transport_enabled = false;
  s_state.transport_identity_invalid_logged = false;
  s_state.transport_connect_failure_logged = false;
  s_state.transport_port = TRANSPORT_DEFAULT_PORT;
  s_state.transport_tx_sequence = 0;
  s_state.transport_tx_count = 0;
  s_state.transport_rx_count = 0;
  s_state.transport_relay_count = 0;

  s_state.ram.fill(0);
  s_state.main_to_link.Clear();
  s_state.link_to_main.Clear();
  ResetController();
  s_state.ymz_register = 0;

  s_state.cycle_balance = 0;
  s_state.host_target_cycles = 0;
  s_state.scheduled_target_cycles = 0;
  s_state.host_tick_fraction = 0;
  s_state.last_host_sync_ticks = static_cast<u64>(System::GetGlobalTickCounter());
  s_state.last_ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());
  s_state.total_cycles = 0;

  s_state.host_irq_latched = false;
  s_state.fifo_overflow_logged = false;
  s_state.unmapped_port_read_logged = false;
  s_state.unmapped_port_write_logged = false;
  s_state.unmapped_memory_read_logged = false;
  s_state.unmapped_memory_write_logged = false;
  s_state.controller_control_unknown_logged = false;
  s_state.controller_fifo_overflow_logged = false;
  s_state.trace_count = 0;
  s_state.ymz_write_count = 0;
  s_state.main_data_read_count = 0;
  s_state.main_data_write_count = 0;
  s_state.main_status_read_count = 0;
  s_state.main_irq_write_count = 0;
  s_state.controller_irq_count = 0;

  ResetZ80Core();
}

void Shutdown()
{
  if (s_state.active)
  {
    DEV_LOG(
      "TecmoGR2Link shutdown cycles={} pc=0x{:04X} main_to_link={} link_to_main={} "
      "mset_complete={} ccrq={} controller_status_reads={} ymz_writes={} "
      "main_data_reads={} main_data_writes={} main_status_reads={} main_irq_writes={} "
      "controller_irqs={} transport_tx={} transport_rx={} transport_relay={}",
      s_state.total_cycles, s_state.cpu.pc, s_state.main_to_link.count, s_state.link_to_main.count,
      s_state.controller.mset_complete, s_state.controller.command_requests,
      s_state.controller.status_reads, s_state.ymz_write_count,
      s_state.main_data_read_count, s_state.main_data_write_count, s_state.main_status_read_count,
      s_state.main_irq_write_count, s_state.controller_irq_count,
      s_state.transport_tx_count, s_state.transport_rx_count, s_state.transport_relay_count);
  }

  StopTransport();
  s_state = {};
}

bool IsActive()
{
  return s_state.active;
}

void ProcessFrame()
{
  SynchronizeHostTime();
  ServiceTransport();
}

u8 MainDataRead()
{
  if (!s_state.active)
    return UINT8_C(0xff);

  SynchronizeHostTime();
  s_state.main_data_read_count++;
  const u8 value = s_state.link_to_main.Pop();
  Trace("main host FIFO read", value, s_state.link_to_main.count);
  return value;
}

void MainDataWrite(u8 value)
{
  if (!s_state.active)
    return;

  SynchronizeHostTime();
  s_state.main_data_write_count++;

  if (!s_state.main_to_link.Push(value))
  {
    if (!s_state.fifo_overflow_logged)
    {
      s_state.fifo_overflow_logged = true;
      WARNING_LOG("TecmoGR2Link main-to-Z80 FIFO overflow");
    }
    return;
  }

  Trace("main host FIFO write", value, s_state.main_to_link.count);
}

void MainIRQWrite(u8 value)
{
  if (!s_state.active)
    return;

  SynchronizeHostTime();
  s_state.main_irq_write_count++;
  Trace("main host IRQ strobe", value, s_state.main_irq_write_count);

  // Two host-control values are now observed from the real PSX program.
  // 0x0001 requests the initial bootstrap exchange; after that succeeds,
  // 0x0002 is written before the PSX waits for the steady-state 64-byte
  // response. The Z80 has only one host interrupt vector (IM0 RST 20h), and
  // its handler selects bootstrap vs steady-state behavior from RAM[0x8028].
  if (value == UINT8_C(0x01) || value == UINT8_C(0x02))
    RaiseHostIRQ();
}

u8 MainStatusRead()
{
  if (!s_state.active)
    return UINT8_C(0x00);

  SynchronizeHostTime();
  s_state.main_status_read_count++;

  // The LINK-ON bootstrap proves both host FIFO directions are represented in
  // this status byte. Bit 0 tells the PSX that the main-to-Z80 FIFO can accept
  // data; bit 1 tells it that the Z80-to-main FIFO contains data. This mirrors
  // the two ready indications observed by link3118.bin at Z80 port 0x91.
  const u8 status =
    (s_state.main_to_link.Full() ? UINT8_C(0x00) : UINT8_C(0x01)) |
    (s_state.link_to_main.Empty() ? UINT8_C(0x00) : UINT8_C(0x02));

  if (s_state.main_status_read_count <= 24)
  {
    DEV_LOG("TecmoGR2Link main status read #{} value=0x{:02X} main_to_link={} link_to_main={}",
            s_state.main_status_read_count, status, s_state.main_to_link.count, s_state.link_to_main.count);
  }

  return status;
}

} // namespace SonyZN::TecmoGR2Link
