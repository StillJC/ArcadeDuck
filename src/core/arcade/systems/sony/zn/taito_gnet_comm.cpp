// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/sony/zn/taito_gnet_comm.h"

#include "core/interrupt_controller.h"
#include "core/settings.h"
#include "core/system.h"

#include "util/sockets.h"

#include "common/error.h"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

Log_SetChannel(SonyZNTaitoGNetComm);

namespace SonyZN::TaitoGNetComm {
namespace {

constexpr u32 SYNC_OFFSET = UINT32_C(0x5ffffe);
constexpr u32 DPRAM_BASE = UINT32_C(0x600000);
constexpr u32 DPRAM_SIZE = UINT32_C(0x2000);
constexpr u32 DPRAM_END = DPRAM_BASE + DPRAM_SIZE;
constexpr u32 CONTROL_OFFSET = UINT32_C(0x700000);

constexpr u32 MAILBOX_FFE = UINT32_C(0x1ffc);
constexpr u32 MAILBOX_FFF = UINT32_C(0x1ffe);

constexpr u32 BOOT_NODE_ID = UINT32_C(0x0000);
constexpr u32 BOOT_HOST_ACK = UINT32_C(0x0002);
constexpr u32 BOOT_STATE = UINT32_C(0x0006);
constexpr u32 BOOT_REQUEST = UINT32_C(0x0008);
constexpr u32 COMM_STATUS = UINT32_C(0x000e);

constexpr u32 NODE_STATUS_BASE = UINT32_C(0x0010);
constexpr u32 NODE_STATUS_STRIDE = UINT32_C(0x0004);

// Static RE of both official U30 programs gives each cabinet two 0x200-byte
// IDT7024 buffers. The normal 448-byte link path uses the first buffer of each
// pair: 1000, 1400, 1800, 1C00. The companion 1200/1600/1A00/1E00 buffers are
// preserved as ordinary shared RAM and are not assigned invented semantics.
constexpr u32 NODE_FRAME_BASE = UINT32_C(0x1000);
constexpr u32 NODE_FRAME_STRIDE = UINT32_C(0x0400);
constexpr size_t NODE_FRAME_SIZE = 448;

constexpr u16 TRANSPORT_DEFAULT_PORT = 19702;
constexpr u8 GNET_COMM_TRANSPORT_VERSION = 1;
constexpr u8 GNET_COMM_PACKET_FRAME = 1;
constexpr size_t TRANSPORT_HEADER_SIZE = 12;
constexpr size_t TRANSPORT_PACKET_SIZE = TRANSPORT_HEADER_SIZE + NODE_FRAME_SIZE;
constexpr std::array<u8, 4> TRANSPORT_MAGIC = {'G', 'N', 'C', 'L'};

class LinkSocket;

struct PendingFrame
{
  u32 sequence = 0;
  std::array<u8, NODE_FRAME_SIZE> payload{};
};

struct State
{
  bool supported = false;
  bool bootstrapped = false;
  bool irq_pending = false;
  u16 control = 0;
  u32 trace_count = 0;
  std::array<u8, DPRAM_SIZE> ram{};

  bool transport_config_checked = false;
  bool transport_enabled = false;
  bool transport_connect_failure_logged = false;
  u16 transport_port = TRANSPORT_DEFAULT_PORT;
  std::string transport_server_address = "127.0.0.1";
  u64 transport_next_retry_tick = 0;
  u32 transport_tx_sequence = 0;
  u32 transport_tx_count = 0;
  u32 transport_rx_count = 0;
  u32 transport_relay_count = 0;
  SocketMultiplexer* transport_multiplexer = nullptr;
  std::shared_ptr<ListenSocket> transport_listener;
  std::vector<std::shared_ptr<LinkSocket>> transport_peers;
  std::array<std::deque<PendingFrame>, 4> pending_rx{};
};

class LinkSocket final : public BufferedStreamSocket
{
public:
  LinkSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor)
    : BufferedStreamSocket(multiplexer, descriptor, 4096, 4096)
  {
  }

  bool SendFrame(std::span<const u8, NODE_FRAME_SIZE> payload);
  bool SendPacket(std::span<const u8> packet);

protected:
  void OnConnected() override;
  void OnDisconnected(const Error& error) override;
  void OnRead() override;
};

State s_state;

u8 GetCabinetIndex()
{
  const u32 cabinet_id =
    std::clamp<u32>(static_cast<u32>(g_settings.system_link_cabinet_id), 1u, 4u);
  return static_cast<u8>(cabinet_id - 1u);
}

u32 GetNodeStatusAOffset(u8 node)
{
  return NODE_STATUS_BASE + (static_cast<u32>(node) * NODE_STATUS_STRIDE);
}

u32 GetNodeStatusBOffset(u8 node)
{
  return GetNodeStatusAOffset(node) + 2;
}

u32 GetNodeFrameOffset(u8 node)
{
  return NODE_FRAME_BASE + (static_cast<u32>(node) * NODE_FRAME_STRIDE);
}

u16 ReadRAM16(u32 offset)
{
  return static_cast<u16>(static_cast<u16>(s_state.ram[offset]) |
                          (static_cast<u16>(s_state.ram[offset + 1]) << 8));
}

void WriteRAM16(u32 offset, u16 value)
{
  s_state.ram[offset] = static_cast<u8>(value);
  s_state.ram[offset + 1] = static_cast<u8>(value >> 8);
}


u32 ReadRAM(u32 width, u32 offset)
{
  u32 value = 0;
  for (u32 i = 0; i < width; i++)
    value |= static_cast<u32>(s_state.ram[offset + i]) << (i * 8);
  return value;
}

void WriteRAM(u32 width, u32 offset, u32 value)
{
  for (u32 i = 0; i < width; i++)
    s_state.ram[offset + i] = static_cast<u8>(value >> (i * 8));
}

void SetIRQ(bool pending)
{
  if (s_state.irq_pending == pending)
    return;

  s_state.irq_pending = pending;
  InterruptController::SetLineState(InterruptController::IRQ::IRQ10, pending);
}

void ScheduleTransportRetry()
{
  const u64 ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());
  s_state.transport_next_retry_tick =
    static_cast<u64>(System::GetGlobalTickCounter()) + (ticks_per_second != 0 ? ticks_per_second : 1);
}

void StopTransport()
{
  // Close() invokes OnDisconnected(), so move the vector first to avoid
  // callbacks mutating the container being iterated.
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

  s_state.transport_next_retry_tick = 0;
  s_state.transport_connect_failure_logged = false;

  for (std::deque<PendingFrame>& queue : s_state.pending_rx)
    queue.clear();
}

void ConfigureTransport()
{
  if (s_state.transport_config_checked)
    return;

  s_state.transport_config_checked = true;
  s_state.transport_enabled = s_state.supported && g_settings.system_link_enabled;
  if (!s_state.transport_enabled)
    return;

  s_state.transport_port = g_settings.system_link_port;
  s_state.transport_server_address =
    g_settings.system_link_server_address.empty() ? "127.0.0.1" : g_settings.system_link_server_address;

  INFO_LOG("TaitoGNetComm system link enabled cabinet_id={} host={} port={}",
           GetCabinetIndex() + 1, s_state.transport_server_address, s_state.transport_port);
}

void ResetTransportConfiguration()
{
  s_state.transport_enabled = false;
  StopTransport();
  s_state.transport_config_checked = false;
}

void RegisterTransportPeer(const std::shared_ptr<LinkSocket>& peer)
{
  const size_t max_peers = (GetCabinetIndex() == 0) ? 3 : 1;
  if (s_state.transport_peers.size() >= max_peers)
  {
    WARNING_LOG("TaitoGNetComm system link rejecting extra peer {}", peer->GetRemoteAddress().ToString());
    peer->Close();
    return;
  }

  s_state.transport_peers.push_back(peer);
  s_state.transport_connect_failure_logged = false;

  Error error;
  if (!peer->SetNagleBuffering(false, &error))
    WARNING_LOG("TaitoGNetComm virtual cable TCP_NODELAY failed: {}", error.GetDescription());

  INFO_LOG("TaitoGNetComm system link connected cabinet_id={} peer_count={}/{} remote={}",
           GetCabinetIndex() + 1, s_state.transport_peers.size(), max_peers,
           peer->GetRemoteAddress().ToString());
}

void RemoveTransportPeer(LinkSocket* peer, const Error& error)
{
  const auto iter =
    std::find_if(s_state.transport_peers.begin(), s_state.transport_peers.end(),
                 [peer](const std::shared_ptr<LinkSocket>& candidate) { return candidate.get() == peer; });

  if (iter != s_state.transport_peers.end())
    s_state.transport_peers.erase(iter);

  if (!s_state.supported || !s_state.transport_enabled)
    return;

  INFO_LOG("TaitoGNetComm system link disconnected cabinet_id={} reason='{}'",
           GetCabinetIndex() + 1, error.GetDescription());


  if (GetCabinetIndex() != 0)
    ScheduleTransportRetry();
}

bool ValidateTransportPacket(std::span<const u8> packet)
{
  if (packet.size() != TRANSPORT_PACKET_SIZE ||
      !std::equal(TRANSPORT_MAGIC.begin(), TRANSPORT_MAGIC.end(), packet.begin()) ||
      packet[4] != GNET_COMM_TRANSPORT_VERSION ||
      packet[5] != GNET_COMM_PACKET_FRAME)
  {
    return false;
  }

  const u8 source_node = packet[6];
  if (source_node >= 4 || source_node == GetCabinetIndex() || packet[7] != 4)
    return false;

  return true;
}

void QueueReceivedFrame(u8 source_node, u32 sequence, std::span<const u8, NODE_FRAME_SIZE> payload)
{
  PendingFrame pending;
  pending.sequence = sequence;
  std::copy(payload.begin(), payload.end(), pending.payload.begin());
  s_state.pending_rx[source_node].push_back(std::move(pending));
}

bool TryDeliverPendingFrame(u8 source_node)
{
  if (source_node == GetCabinetIndex())
    return false;

  std::deque<PendingFrame>& queue = s_state.pending_rx[source_node];
  if (queue.empty())
    return false;

  const u32 status_a_offset = GetNodeStatusAOffset(source_node);
  const u32 status_b_offset = GetNodeStatusBOffset(source_node);

  // The hardware-visible IDT7024 slot remains one frame deep: expose the next
  // ARCNET frame only after the PSX has acknowledged the previous one by
  // bringing B up to A. The FIFO exists only between the host TCP callback and
  // the emulated H8/ARCNET side so host scheduling cannot discard a frame.
  const u16 status_a = ReadRAM16(status_a_offset);
  const u16 status_b = ReadRAM16(status_b_offset);
  if (status_a != status_b)
    return false;

  const PendingFrame& pending = queue.front();
  const u32 frame_offset = GetNodeFrameOffset(source_node);
  std::copy(pending.payload.begin(), pending.payload.end(), s_state.ram.begin() + frame_offset);

  WriteRAM16(status_a_offset, static_cast<u16>(status_a + 1));
  WriteRAM16(MAILBOX_FFE, UINT16_C(1));

  // IDT7024 right-port write of mailbox FFE asserts the left-port interrupt.
  // The PSX clears it by reading FFE; the existing ReadEXP1 path models that.
  SetIRQ(true);

  s_state.transport_rx_count++;
  if (s_state.transport_rx_count <= 24)
  {
    DEV_LOG(
      "TaitoGNetComm link rx #{} from cabinet={} seq={} -> DPRAM[0x{:04X}] "
      "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
      s_state.transport_rx_count, source_node + 1, pending.sequence, frame_offset,
      s_state.ram[frame_offset + 0], s_state.ram[frame_offset + 1],
      s_state.ram[frame_offset + 2], s_state.ram[frame_offset + 3],
      s_state.ram[frame_offset + 4], s_state.ram[frame_offset + 5],
      s_state.ram[frame_offset + 6], s_state.ram[frame_offset + 7]);
  }

  queue.pop_front();
  return true;
}

void HandleTransportPacket(LinkSocket* source, std::span<const u8> packet)
{
  const u8 source_node = packet[6];
  const u32 sequence =
    static_cast<u32>(packet[8]) |
    (static_cast<u32>(packet[9]) << 8) |
    (static_cast<u32>(packet[10]) << 16) |
    (static_cast<u32>(packet[11]) << 24);

  // Cabinet 1 is the virtual ARCNET hub for the existing ArcadeDuck
  // SystemLink star transport. Relay an opaque peer frame to every other TCP
  // client; the guest protocol remains cabinet-to-cabinet.
  if (GetCabinetIndex() == 0)
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
      DEV_LOG("TaitoGNetComm relayed cabinet={} seq={} to {} peer(s)",
              source_node + 1, sequence, relayed);
    }
  }

  std::array<u8, NODE_FRAME_SIZE> payload{};
  std::copy_n(packet.begin() + TRANSPORT_HEADER_SIZE, NODE_FRAME_SIZE, payload.begin());
  QueueReceivedFrame(source_node, sequence, payload);
  TryDeliverPendingFrame(source_node);
}

bool LinkSocket::SendFrame(std::span<const u8, NODE_FRAME_SIZE> payload)
{
  const std::span<u8> write_buffer = AcquireWriteBuffer(TRANSPORT_PACKET_SIZE, false);
  if (write_buffer.size() < TRANSPORT_PACKET_SIZE)
    return false;

  std::copy(TRANSPORT_MAGIC.begin(), TRANSPORT_MAGIC.end(), write_buffer.begin());
  write_buffer[4] = GNET_COMM_TRANSPORT_VERSION;
  write_buffer[5] = GNET_COMM_PACKET_FRAME;
  write_buffer[6] = GetCabinetIndex();
  write_buffer[7] = 4;

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
      WARNING_LOG("TaitoGNetComm invalid system-link packet from {}; closing connection",
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
    ERROR_LOG("TaitoGNetComm failed to parse system-link listen address: {}", error.GetDescription());
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
    ERROR_LOG("TaitoGNetComm system link failed to listen on {}: {}",
              address->ToString(), error.GetDescription());
    System::ReleaseSocketMultiplexer();
    s_state.transport_multiplexer = nullptr;
    ScheduleTransportRetry();
    return false;
  }

  INFO_LOG("TaitoGNetComm cabinet 1 listening on {}", address->ToString());
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
    SocketAddress::Parse(SocketAddress::Type::IPv4,
                         s_state.transport_server_address.c_str(),
                         s_state.transport_port, &error);
  if (!address.has_value())
  {
    if (!s_state.transport_connect_failure_logged)
    {
      s_state.transport_connect_failure_logged = true;
      WARNING_LOG("TaitoGNetComm invalid system-link host IPv4 address '{}': {}",
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
      INFO_LOG("TaitoGNetComm cabinet {} waiting for cabinet 1 on {}: {}",
               GetCabinetIndex() + 1, address->ToString(), error.GetDescription());
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
      (s_state.transport_enabled != (s_state.supported && g_settings.system_link_enabled) ||
       (g_settings.system_link_enabled &&
        (s_state.transport_port != g_settings.system_link_port ||
         s_state.transport_server_address != server_address))))
  {
    INFO_LOG("TaitoGNetComm system-link settings changed; restarting virtual cable");
    ResetTransportConfiguration();
  }

  ConfigureTransport();
  if (!s_state.transport_enabled || !s_state.bootstrapped)
    return;

  if (GetCabinetIndex() == 0)
    StartTransportListener();
  else
    TryConnectTransportPeer();
}

void BootstrapBoard()
{
  // Recovered directly from both official RC de Go! and Go By RC programs.
  // The physical rotary switch is zero-based in DPRAM and displayed as 1-4.
  WriteRAM16(BOOT_NODE_ID, GetCabinetIndex());
  WriteRAM16(BOOT_HOST_ACK, UINT16_C(0x0000));
  WriteRAM16(BOOT_STATE, UINT16_C(0x0000));
  WriteRAM16(BOOT_REQUEST, UINT16_C(0xffff));
  WriteRAM16(COMM_STATUS, UINT16_C(0x0000));

  for (u32 node = 0; node < 4; node++)
  {
    WriteRAM16(NODE_STATUS_BASE + (node * NODE_STATUS_STRIDE), UINT16_C(0x0000));
    WriteRAM16(NODE_STATUS_BASE + (node * NODE_STATUS_STRIDE) + 2, UINT16_C(0x0000));
  }

  s_state.bootstrapped = true;

  INFO_LOG("TaitoGNetComm HLE bootstrap cabinet_id={} shared='1F600000-1F601FFF'",
           GetCabinetIndex() + 1);
}

void HandleMainToCommDoorbell()
{
  if (!s_state.bootstrapped)
    return;

  const u8 local_node = GetCabinetIndex();
  const u32 status_a_offset = GetNodeStatusAOffset(local_node);
  const u32 status_b_offset = GetNodeStatusBOffset(local_node);

  const u16 consumed = ReadRAM16(status_a_offset);
  const u16 produced = ReadRAM16(status_b_offset);

  // Recovered PSX send routine:
  //   copy 28 unrolled groups of 8 halfwords (224 halfwords / 448 bytes) to this node's first 0x200-byte buffer
  //   B = A + 1
  //   write 1 to mailbox FFF
  //
  // The real H8/ARCNET side consumes that posted frame and acknowledges it by
  // bringing A up to B. Forward the opaque 448-byte payload before acknowledging.
  if (produced != consumed)
  {
    ServiceTransport();

    const u32 frame_offset = GetNodeFrameOffset(local_node);
    std::array<u8, NODE_FRAME_SIZE> payload{};
    std::copy_n(s_state.ram.begin() + frame_offset, NODE_FRAME_SIZE, payload.begin());

    bool sent = false;
    for (const std::shared_ptr<LinkSocket>& peer : s_state.transport_peers)
    {
      if (peer && peer->IsConnected() && peer->SendFrame(payload))
        sent = true;
    }

    // The communication CPU consumes the local DPRAM post independently of
    // whether another cabinet is currently present on ARCNET.
    WriteRAM16(status_a_offset, produced);

    s_state.transport_tx_count++;
    if (s_state.transport_tx_count <= 24)
    {
      DEV_LOG(
        "TaitoGNetComm link tx #{} cabinet={} seqB={} peers={} sent={} "
        "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
        s_state.transport_tx_count, local_node + 1, produced,
        s_state.transport_peers.size(), sent,
        payload[0], payload[1], payload[2], payload[3],
        payload[4], payload[5], payload[6], payload[7]);
    }
  }

  if (s_state.trace_count < 32)
  {
    DEV_LOG("TaitoGNetComm PSX->H8 doorbell cabinet={} statusA={} statusB={}",
            local_node + 1, consumed, produced);
    s_state.trace_count++;
  }
}

} // namespace

void Initialize(bool supported)
{
  Shutdown();
  s_state.supported = supported;
  Reset();

  if (supported)
  {
    INFO_LOG("TaitoGNetComm optional Communication PCB available; SystemLink.Enabled controls attachment");
  }
}

void Reset()
{
  StopTransport();
  SetIRQ(false);

  const bool supported = s_state.supported;
  s_state = {};
  s_state.supported = supported;

  ConfigureTransport();
}

void Shutdown()
{
  StopTransport();
  SetIRQ(false);
  s_state = {};
}

void ProcessFrame()
{
  if (!IsActive())
    return;

  ServiceTransport();

  // A peer may have produced another frame while its previous shared-RAM slot
  // was still awaiting PSX acknowledgement. Deliver it as soon as A == B.
  for (u8 node = 0; node < 4; node++)
  {
    if (node != GetCabinetIndex())
      TryDeliverPendingFrame(node);
  }
}

bool IsActive()
{
  return s_state.supported && g_settings.system_link_enabled;
}

bool HandlesEXP1Access(u32 width, u32 offset)
{
  if (!IsActive() || (width != 1 && width != 2 && width != 4))
    return false;

  if (offset == SYNC_OFFSET && width <= 2)
    return true;

  if (offset >= DPRAM_BASE && offset < DPRAM_END &&
      width <= (DPRAM_END - offset))
  {
    return true;
  }

  return (offset == CONTROL_OFFSET && width <= 4);
}

u32 ReadEXP1(u32 width, u32 offset)
{
  if (offset == SYNC_OFFSET)
  {
    // The recovered game paths use this access for ordering/synchronization;
    // no observed path consumes a documented data bit from it.
    return 0;
  }

  if (offset >= DPRAM_BASE && offset < DPRAM_END)
  {
    const u32 ram_offset = offset - DPRAM_BASE;
    const u32 value = ReadRAM(width, ram_offset);

    // IDT7024 mailbox semantics: PSX/left-port read of FFE clears the
    // interrupt generated by H8/right-port write of FFE.
    if (ram_offset <= MAILBOX_FFE && MAILBOX_FFE < (ram_offset + width))
      SetIRQ(false);

    return value;
  }

  if (offset == CONTROL_OFFSET)
    return s_state.control;

  return UINT32_C(0xffffffff);
}

bool WriteEXP1(u32 width, u32 offset, u32 value)
{
  if (offset >= DPRAM_BASE && offset < DPRAM_END)
  {
    const u32 ram_offset = offset - DPRAM_BASE;
    WriteRAM(width, ram_offset, value);

    // IDT7024 mailbox semantics: PSX/left-port write of FFF signals the
    // communication CPU/right port.
    if (ram_offset <= MAILBOX_FFF && MAILBOX_FFF < (ram_offset + width))
      HandleMainToCommDoorbell();

    return true;
  }

  if (offset == CONTROL_OFFSET)
  {
    const u16 old_control = s_state.control;
    s_state.control = static_cast<u16>(value);

    if ((s_state.control & UINT16_C(1)) != 0 && (old_control & UINT16_C(1)) == 0)
      BootstrapBoard();

    if (s_state.trace_count < 32)
    {
      DEV_LOG("TaitoGNetComm control write value=0x{:04X} active={}",
              s_state.control, IsActive());
      s_state.trace_count++;
    }

    return true;
  }

  return (offset == SYNC_OFFSET);
}

} // namespace SonyZN::TaitoGNetComm