// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/sony/zn/time_warner_ata.h"

#include "core/arcade/devices/storage/chd_hard_disk.h"
#include "core/dma.h"
#include "core/interrupt_controller.h"
#include "core/system.h"
#include "core/timing_event.h"

#include "common/error.h"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>

Log_SetChannel(TimeWarnerATA);

namespace SonyZN::TimeWarnerATA {
namespace {

// Atari PSXTRA windows, expressed relative to PSX EXP1 (0x1F000000).
static constexpr u32 VIA_BRIDGE_BASE = 0x007e4000;
static constexpr u32 VIA_BRIDGE_END = 0x007e4fff;
static constexpr u32 IRQ_CONTROL_BASE = 0x007e8000;
static constexpr u32 IRQ_CONTROL_END = 0x007e8003;
static constexpr u32 DATA32_BASE = 0x007f4000;
static constexpr u32 DATA32_END = 0x007f4fff;

// VT83C461 host-visible offsets inside the PSXTRA bridge.
// The AV150 manual documents both 34/38/3C and B4/B8/BC config-port groups;
// Primal Rage II probes both banks and then uses the standard ATA task file.
static constexpr u32 CONFIG_LOW_BASE = 0x0030;
static constexpr u32 CONFIG_LOW_END = 0x003f;
static constexpr u32 CONFIG_HIGH_BASE = 0x00b0;
static constexpr u32 CONFIG_HIGH_END = 0x00bf;
static constexpr u32 CS0_BASE = 0x01f0;
static constexpr u32 CS0_END = 0x01f7;
[[maybe_unused]] static constexpr u32 CS1_BASE = 0x03f0;
[[maybe_unused]] static constexpr u32 CS1_END = 0x03f7;
static constexpr u32 ALT_STATUS_DEVICE_CONTROL = 0x03f6;
static constexpr u32 DATA32_PORT = 0x01f0;

static constexpr u8 ATA_STATUS_ERR = 0x01;
static constexpr u8 ATA_STATUS_DRQ = 0x08;
static constexpr u8 ATA_STATUS_DSC = 0x10;
static constexpr u8 ATA_STATUS_DRDY = 0x40;
static constexpr u8 ATA_STATUS_BSY = 0x80;

static constexpr u8 ATA_ERROR_ABRT = 0x04;
static constexpr u8 ATA_ERROR_IDNF = 0x10;

static constexpr u8 ATA_DEVICE_CONTROL_NIEN = 0x02;
static constexpr u8 ATA_DEVICE_CONTROL_SRST = 0x04;

static constexpr u8 ATA_CMD_RECALIBRATE = 0x10;
static constexpr u8 ATA_CMD_READ_SECTORS = 0x20;
static constexpr u8 ATA_CMD_WRITE_SECTORS = 0x30;
static constexpr u8 ATA_CMD_SEEK = 0x70;
static constexpr u8 ATA_CMD_READ_MULTIPLE = 0xc4;
static constexpr u8 ATA_CMD_WRITE_MULTIPLE = 0xc5;
static constexpr u8 ATA_CMD_SET_MULTIPLE_MODE = 0xc6;
static constexpr u8 ATA_CMD_IDENTIFY_DEVICE = 0xec;
static constexpr u8 ATA_CMD_SET_FEATURES = 0xef;

static constexpr size_t SECTOR_SIZE = 512;
static constexpr size_t IDENTIFY_WORDS = 256;
static constexpr u32 PRIMAL_RAGE_II_EXPECTED_BLOCKS = 0x00203d00;
static constexpr size_t VIA_CONFIG_REGISTER_COUNT = 0x10;

enum class TransferMode : u8
{
  None,
  Identify,
  Read,
  Write,
};

struct RuntimeState
{
  Arcade::Storage::CHDHardDisk disk;
  std::string path;

  u8 error = 0x01;
  u8 features = 0;
  u8 sector_count = 0x01;
  u8 lba_low = 0x01;
  u8 lba_mid = 0x00;
  u8 lba_high = 0x00;
  u8 device_head = 0x00;
  u8 status = ATA_STATUS_DRDY | ATA_STATUS_DSC;
  u8 device_control = 0;
  u8 multiple_count = 0;

  TransferMode transfer_mode = TransferMode::None;
  std::array<u8, SECTOR_SIZE> data_buffer{};
  u32 data_position = 0;
  u32 current_lba = 0;
  u32 sectors_remaining = 0;

  bool irq_pending = false;
  u16 irq_control = 0;

  u8 config_selector = 0;
  u8 config_register_num = 0;
  std::array<u8, VIA_CONFIG_REGISTER_COUNT> config_registers{};

  bool read_sector_refill_pending = false;
};

RuntimeState s_state;
bool s_initialized = false;
std::unique_ptr<TimingEvent> s_read_sector_ready_event;

static void CancelReadSectorReadyEvent()
{
  if (s_read_sector_ready_event)
    s_read_sector_ready_event->Deactivate();
  s_state.read_sector_refill_pending = false;
}

static void UpdateDMARequest()
{
  const bool data_transfer =
    s_state.transfer_mode == TransferMode::Read || s_state.transfer_mode == TransferMode::Write;
  const bool request = s_initialized && data_transfer && (s_state.status & ATA_STATUS_DRQ) != 0;
  DMA::SetRequest(DMA::Channel::PIO, request);
}
static void SetIRQLine(bool state)
{
  InterruptController::SetLineState(InterruptController::IRQ::IRQ10, state);
}
static void UpdateIRQLine()
{
  const bool ata_irq_enabled = (s_state.device_control & ATA_DEVICE_CONTROL_NIEN) == 0;
  const bool psxtra_irq_enabled = (s_state.irq_control & UINT16_C(0x0001)) != 0;
  SetIRQLine(s_state.irq_pending && ata_irq_enabled && psxtra_irq_enabled);
}

static void RaiseIRQ()
{
  s_state.irq_pending = true;
  UpdateIRQLine();
}

static void ClearIRQ()
{
  s_state.irq_pending = false;
  SetIRQLine(false);
}

static void ResetTaskFile()
{
  CancelReadSectorReadyEvent();
  ClearIRQ();
  s_state.error = 0x01;
  s_state.features = 0;
  s_state.sector_count = 0x01;
  s_state.lba_low = 0x01;
  s_state.lba_mid = 0x00;
  s_state.lba_high = 0x00;
  s_state.device_head = 0x00;
  s_state.status = ATA_STATUS_DRDY | ATA_STATUS_DSC;
  s_state.multiple_count = 0;
  s_state.transfer_mode = TransferMode::None;
  s_state.data_position = 0;
  s_state.current_lba = 0;
  s_state.sectors_remaining = 0;
  UpdateDMARequest();
}

static void ResetVIAConfiguration()
{
  s_state.config_selector = 0;
  s_state.config_register_num = 0;
  s_state.config_registers.fill(0);
}

static u32 GetTaskLBA()
{
  // ATA task-file CHS addressing is selected when Device/Head bit 6 is clear.
  // Sector numbers are 1-based in CHS mode.
  if ((s_state.device_head & 0x40) == 0)
  {
    const auto& geometry = s_state.disk.GetGeometry();
    const u32 cylinder = (static_cast<u32>(s_state.lba_high) << 8) | static_cast<u32>(s_state.lba_mid);
    const u32 head = static_cast<u32>(s_state.device_head & 0x0f);
    const u32 sector = static_cast<u32>(s_state.lba_low);

    if (geometry.heads == 0 || geometry.sectors == 0 || cylinder >= geometry.cylinders ||
        head >= geometry.heads || sector == 0 || sector > geometry.sectors)
    {
      return UINT32_MAX;
    }

    return ((cylinder * geometry.heads + head) * geometry.sectors) + (sector - 1);
  }

  return (static_cast<u32>(s_state.device_head & 0x0f) << 24) |
         (static_cast<u32>(s_state.lba_high) << 16) |
         (static_cast<u32>(s_state.lba_mid) << 8) |
         static_cast<u32>(s_state.lba_low);
}

static void SetTaskLBA(u32 lba)
{
  if ((s_state.device_head & 0x40) == 0)
  {
    const auto& geometry = s_state.disk.GetGeometry();
    if (geometry.heads == 0 || geometry.sectors == 0)
      return;

    const u32 sectors_per_cylinder = geometry.heads * geometry.sectors;
    const u32 cylinder = lba / sectors_per_cylinder;
    const u32 cylinder_remainder = lba % sectors_per_cylinder;
    const u32 head = cylinder_remainder / geometry.sectors;
    const u32 sector = (cylinder_remainder % geometry.sectors) + 1;

    s_state.lba_low = static_cast<u8>(sector);
    s_state.lba_mid = static_cast<u8>(cylinder);
    s_state.lba_high = static_cast<u8>(cylinder >> 8);
    s_state.device_head = static_cast<u8>((s_state.device_head & 0xf0) | (head & 0x0f));
    return;
  }

  s_state.lba_low = static_cast<u8>(lba);
  s_state.lba_mid = static_cast<u8>(lba >> 8);
  s_state.lba_high = static_cast<u8>(lba >> 16);
  s_state.device_head = static_cast<u8>((s_state.device_head & 0xf0) | ((lba >> 24) & 0x0f));
}
static u32 GetTaskSectorCount()
{
  return s_state.sector_count != 0 ? s_state.sector_count : 256;
}

static void SetIdleStatus()
{
  s_state.status = ATA_STATUS_DRDY | ATA_STATUS_DSC;
  UpdateDMARequest();
}

static void SetError(u8 error)
{
  s_state.error = error;
  s_state.status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_ERR;
  s_state.transfer_mode = TransferMode::None;
  s_state.data_position = 0;
  s_state.sectors_remaining = 0;
  UpdateDMARequest();
  RaiseIRQ();
}

static void PutIdentifyWord(u32 index, u16 value)
{
  if (index >= IDENTIFY_WORDS)
    return;

  s_state.data_buffer[index * 2] = static_cast<u8>(value);
  s_state.data_buffer[index * 2 + 1] = static_cast<u8>(value >> 8);
}

static void PutIdentifyString(u32 first_word, u32 word_count, std::string_view text)
{
  for (u32 i = 0; i < word_count; i++)
  {
    const size_t char_index = static_cast<size_t>(i) * 2;
    const u8 a = char_index < text.size() ? static_cast<u8>(text[char_index]) : static_cast<u8>(' ');
    const u8 b = (char_index + 1) < text.size() ? static_cast<u8>(text[char_index + 1]) : static_cast<u8>(' ');
    PutIdentifyWord(first_word + i, static_cast<u16>((static_cast<u16>(a) << 8) | b));
  }
}

static void BuildIdentifyData()
{
  s_state.data_buffer.fill(0);

  const Arcade::Storage::CHDHardDisk::Geometry& geometry = s_state.disk.GetGeometry();
  const u32 blocks = s_state.disk.GetBlockCount();
  const u32 current_capacity = std::min<u32>(blocks, UINT32_C(0xffffffff));

  PutIdentifyWord(0, 0x0040); // fixed disk
  PutIdentifyWord(1, static_cast<u16>(std::min<u32>(geometry.cylinders, 0xffff)));
  PutIdentifyWord(3, static_cast<u16>(std::min<u32>(geometry.heads, 0xffff)));
  PutIdentifyWord(6, static_cast<u16>(std::min<u32>(geometry.sectors, 0xffff)));
  // The preserved CHD supplies geometry, not ATA IDENTIFY metadata. Keep the
  // serial/firmware fields neutral; the physical PSXTRA board documentation
  // identifies the original drive family as a Quantum Fireball 1080AT.
  PutIdentifyString(10, 10, "");
  PutIdentifyString(23, 4, "");
  PutIdentifyString(27, 20, "QUANTUM FIREBALL1080A");
  PutIdentifyWord(47, 0x8008); // maximum multiple-sector block size: 8
  PutIdentifyWord(49, 0x0200); // LBA supported
  PutIdentifyWord(53, 0x0001); // words 54-58 valid
  PutIdentifyWord(54, static_cast<u16>(std::min<u32>(geometry.cylinders, 0xffff)));
  PutIdentifyWord(55, static_cast<u16>(std::min<u32>(geometry.heads, 0xffff)));
  PutIdentifyWord(56, static_cast<u16>(std::min<u32>(geometry.sectors, 0xffff)));
  PutIdentifyWord(57, static_cast<u16>(current_capacity));
  PutIdentifyWord(58, static_cast<u16>(current_capacity >> 16));
  PutIdentifyWord(59, static_cast<u16>(0x0100 | (s_state.multiple_count & 0xff)));
  PutIdentifyWord(60, static_cast<u16>(blocks));
  PutIdentifyWord(61, static_cast<u16>(blocks >> 16));
  PutIdentifyWord(64, 0x0003); // PIO modes 3 and 4 supported
  PutIdentifyWord(67, 120);
  PutIdentifyWord(68, 120);
}

static TickCount GetIntersectorDelayTicks()
{
  // ATA READ SECTORS drops DRQ/DMARQ between sector data phases. MAME's
  // ATA mass-storage model uses a 400 ns sequential-sector gap; at the PSX
  // clock this is only a handful of cycles, but the scheduler boundary is
  // essential because it lets the just-completed DMA5 transfer retire before
  // the next DMARQ assertion.
  constexpr u64 INTERSECTOR_NANOSECONDS = 400;
  const u64 ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());
  const u64 ticks = ((ticks_per_second * INTERSECTOR_NANOSECONDS) + UINT64_C(999999999)) / UINT64_C(1000000000);
  return std::max<TickCount>(static_cast<TickCount>(ticks), 1);
}

static bool LoadCurrentReadSector();

static void ReadSectorReadyEventCallback(void*, TickCount, TickCount)
{
  if (s_read_sector_ready_event)
    s_read_sector_ready_event->Deactivate();

  if (!s_initialized || !s_state.read_sector_refill_pending || s_state.transfer_mode != TransferMode::Read ||
      s_state.sectors_remaining == 0)
  {
    s_state.read_sector_refill_pending = false;
    return;
  }

  s_state.read_sector_refill_pending = false;
  LoadCurrentReadSector();
}

static void ScheduleNextReadSector()
{
  s_state.data_position = 0;
  s_state.status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_BSY;
  s_state.read_sector_refill_pending = true;

  // End the current ATA data phase before the next one is exposed. In
  // particular, DMARQ must fall so a one-sector DMA5 operation is not left
  // looking continuously requested, and the prior ATA IRQ must be released
  // before the next sector raises it again.
  UpdateDMARequest();
  ClearIRQ();

  if (!s_read_sector_ready_event)
  {
    s_read_sector_ready_event =
      std::make_unique<TimingEvent>("Time Warner ATA Next Sector", 1, 1, ReadSectorReadyEventCallback, nullptr);
  }
  else
  {
    s_read_sector_ready_event->Deactivate();
  }

  const TickCount delay_ticks = GetIntersectorDelayTicks();
  s_read_sector_ready_event->Schedule(delay_ticks);
}

static bool LoadCurrentReadSector()
{
  if (s_state.current_lba >= s_state.disk.GetBlockCount())
  {
    SetError(ATA_ERROR_IDNF);
    return false;
  }

  if (!s_state.disk.ReadSector(s_state.current_lba, s_state.data_buffer.data()))
  {
    SetError(ATA_ERROR_ABRT);
    return false;
  }

  s_state.data_position = 0;
  s_state.status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
  UpdateDMARequest();
  RaiseIRQ();
  return true;
}

static void AdvanceReadTransfer()
{
  if (s_state.transfer_mode == TransferMode::Identify)
  {
    s_state.transfer_mode = TransferMode::None;
    s_state.data_position = 0;
    SetIdleStatus();
    return;
  }

  if (s_state.transfer_mode != TransferMode::Read || s_state.sectors_remaining == 0)
    return;

  s_state.sectors_remaining--;
  s_state.sector_count = static_cast<u8>(s_state.sectors_remaining & 0xff);
  if (s_state.sectors_remaining == 0)
  {
    s_state.transfer_mode = TransferMode::None;
    s_state.data_position = 0;
    SetIdleStatus();
    return;
  }

  s_state.current_lba++;
  SetTaskLBA(s_state.current_lba);
  ScheduleNextReadSector();
}

static void CompleteWriteSector()
{
  if (s_state.transfer_mode != TransferMode::Write || s_state.sectors_remaining == 0)
    return;

  if (s_state.current_lba >= s_state.disk.GetBlockCount() ||
      !s_state.disk.WriteSector(s_state.current_lba, s_state.data_buffer.data()))
  {
    SetError(s_state.current_lba >= s_state.disk.GetBlockCount() ? ATA_ERROR_IDNF : ATA_ERROR_ABRT);
    return;
  }

  s_state.sectors_remaining--;
  s_state.sector_count = static_cast<u8>(s_state.sectors_remaining & 0xff);
  if (s_state.sectors_remaining == 0)
  {
    s_state.transfer_mode = TransferMode::None;
    s_state.data_position = 0;
    SetIdleStatus();
    RaiseIRQ();
    return;
  }

  s_state.current_lba++;
  SetTaskLBA(s_state.current_lba);
  s_state.data_buffer.fill(0);
  s_state.data_position = 0;
  s_state.status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
  UpdateDMARequest();
  RaiseIRQ();
}

static u8 ReadDataByte()
{
  if ((s_state.status & ATA_STATUS_DRQ) == 0 ||
      (s_state.transfer_mode != TransferMode::Identify && s_state.transfer_mode != TransferMode::Read))
  {
    return 0xff;
  }

  const u8 value = s_state.data_buffer[s_state.data_position++];
  if (s_state.data_position >= s_state.data_buffer.size())
    AdvanceReadTransfer();
  return value;
}

static void WriteDataByte(u8 value)
{
  if ((s_state.status & ATA_STATUS_DRQ) == 0 || s_state.transfer_mode != TransferMode::Write)
    return;

  s_state.data_buffer[s_state.data_position++] = value;
  if (s_state.data_position >= s_state.data_buffer.size())
    CompleteWriteSector();
}

static u8 ReadTaskRegister(u32 reg, bool clear_irq)
{
  switch (reg)
  {
    case 1:
      return s_state.error;
    case 2:
      return s_state.sector_count;
    case 3:
      return s_state.lba_low;
    case 4:
      return s_state.lba_mid;
    case 5:
      return s_state.lba_high;
    case 6:
      return s_state.device_head;
    case 7:
      // ATA status-register reads acknowledge INTRQ only after BSY has deasserted.
      if (clear_irq && (s_state.status & ATA_STATUS_BSY) == 0)
        ClearIRQ();
      return s_state.status;
    default:
      return 0xff;
  }
}

static void BeginReadCommand(TransferMode mode)
{
  s_state.error = 0;
  s_state.transfer_mode = mode;
  s_state.current_lba = GetTaskLBA();
  s_state.sectors_remaining = GetTaskSectorCount();
  if (s_state.current_lba >= s_state.disk.GetBlockCount() ||
      s_state.sectors_remaining > (s_state.disk.GetBlockCount() - s_state.current_lba))
  {
    SetError(ATA_ERROR_IDNF);
    return;
  }

  LoadCurrentReadSector();
}

static void BeginWriteCommand()
{
  s_state.error = 0;
  s_state.transfer_mode = TransferMode::Write;
  s_state.current_lba = GetTaskLBA();
  s_state.sectors_remaining = GetTaskSectorCount();
  if (s_state.current_lba >= s_state.disk.GetBlockCount() ||
      s_state.sectors_remaining > (s_state.disk.GetBlockCount() - s_state.current_lba))
  {
    SetError(ATA_ERROR_IDNF);
    return;
  }

  s_state.data_buffer.fill(0);
  s_state.data_position = 0;
  s_state.status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
  ClearIRQ();
  UpdateDMARequest();
}

static void ExecuteCommand(u8 command)
{
  CancelReadSectorReadyEvent();
  ClearIRQ();
  switch (command)
  {
    case ATA_CMD_IDENTIFY_DEVICE:
      s_state.error = 0;
      BuildIdentifyData();
      s_state.transfer_mode = TransferMode::Identify;
      s_state.data_position = 0;
      s_state.sectors_remaining = 1;
      s_state.status = ATA_STATUS_DRDY | ATA_STATUS_DSC | ATA_STATUS_DRQ;
      UpdateDMARequest();
      RaiseIRQ();
      break;

    case ATA_CMD_SET_MULTIPLE_MODE:
      if (s_state.sector_count == 0 || s_state.sector_count > 8)
      {
        SetError(ATA_ERROR_ABRT);
        break;
      }
      s_state.multiple_count = s_state.sector_count;
      s_state.error = 0;
      SetIdleStatus();
      RaiseIRQ();
      break;

    case ATA_CMD_SET_FEATURES:
      if (s_state.features != 0x03)
      {
        SetError(ATA_ERROR_ABRT);
        break;
      }
      s_state.error = 0;
      SetIdleStatus();
      RaiseIRQ();
      break;

    case ATA_CMD_READ_MULTIPLE:
    case ATA_CMD_READ_SECTORS:
      BeginReadCommand(TransferMode::Read);
      break;

    case ATA_CMD_WRITE_MULTIPLE:
    case ATA_CMD_WRITE_SECTORS:
      BeginWriteCommand();
      break;

    case ATA_CMD_SEEK:
    case ATA_CMD_RECALIBRATE:
      s_state.error = 0;
      SetIdleStatus();
      RaiseIRQ();
      break;

    default:
      DEV_LOG("TimeWarnerATA unsupported command 0x{:02X} lba=0x{:08X} count={}", command, GetTaskLBA(),
                  GetTaskSectorCount());
      SetError(ATA_ERROR_ABRT);
      break;
  }
}

static void WriteTaskRegister(u32 reg, u8 value)
{
  switch (reg)
  {
    case 1:
      s_state.features = value;
      break;
    case 2:
      s_state.sector_count = value;
      break;
    case 3:
      s_state.lba_low = value;
      break;
    case 4:
      s_state.lba_mid = value;
      break;
    case 5:
      s_state.lba_high = value;
      break;
    case 6:
      s_state.device_head = value;
      break;
    case 7:
      ExecuteCommand(value);
      break;
    default:
      break;
  }
}

static void WriteDeviceControl(u8 value)
{
  const u8 old_control = s_state.device_control;
  const bool old_srst = (old_control & ATA_DEVICE_CONTROL_SRST) != 0;
  const bool new_srst = (value & ATA_DEVICE_CONTROL_SRST) != 0;
  s_state.device_control = value;

  if (!old_srst && new_srst)
  {
    ClearIRQ();
    s_state.transfer_mode = TransferMode::None;
    s_state.status = ATA_STATUS_BSY;
    UpdateDMARequest();
  }
  else if (old_srst && !new_srst)
  {
    ResetTaskFile();
    s_state.device_control = value;
  }
  else
  {
    UpdateIRQLine();
  }
}

static bool GetConfigPort(u32 relative, u32* port, u32* lane)
{
  u32 normalized = relative;
  if (relative >= CONFIG_HIGH_BASE && relative <= CONFIG_HIGH_END)
    normalized -= 0x80;

  if (normalized < CONFIG_LOW_BASE || normalized > CONFIG_LOW_END)
    return false;

  const u32 byte_offset = normalized - CONFIG_LOW_BASE;
  *port = byte_offset >> 2;
  *lane = byte_offset & 3;
  return true;
}

static u8 ReadConfigPort(u32 port)
{
  switch (port)
  {
    case 1:
      return s_state.config_selector;
    case 2:
      return s_state.config_register_num;
    case 3:
      return s_state.config_register_num < s_state.config_registers.size() ?
               s_state.config_registers[s_state.config_register_num] : UINT8_C(0x00);
    default:
      return 0;
  }
}

static void WriteConfigPort(u32 port, u8 value)
{
  switch (port)
  {
    case 1:
      s_state.config_selector = value;
      break;
    case 2:
      s_state.config_register_num = value;
      break;
    case 3:
      if (s_state.config_register_num < s_state.config_registers.size())
        s_state.config_registers[s_state.config_register_num] = value;
      break;
    default:
      break;
  }
}

static u8 ReadBridgeByte(u32 relative)
{
  u32 config_port = 0;
  u32 config_lane = 0;
  if (GetConfigPort(relative, &config_port, &config_lane))
    return config_lane == 0 ? ReadConfigPort(config_port) : UINT8_C(0xff);

  if (relative >= CS0_BASE && relative <= CS0_END)
  {
    const u32 reg = relative - CS0_BASE;
    if (reg == 0)
      return ReadDataByte();
    return ReadTaskRegister(reg, reg == 7);
  }

  if (relative == ALT_STATUS_DEVICE_CONTROL)
    return s_state.status;

  return UINT8_C(0xff);
}

static void WriteBridgeByte(u32 relative, u8 value)
{
  u32 config_port = 0;
  u32 config_lane = 0;
  if (GetConfigPort(relative, &config_port, &config_lane))
  {
    if (config_lane == 0)
      WriteConfigPort(config_port, value);
    return;
  }

  if (relative >= CS0_BASE && relative <= CS0_END)
  {
    const u32 reg = relative - CS0_BASE;
    if (reg == 0)
      WriteDataByte(value);
    else
      WriteTaskRegister(reg, value);
    return;
  }

  if (relative == ALT_STATUS_DEVICE_CONTROL)
    WriteDeviceControl(value);
}

static u32 ReadBridge(u32 width, u32 relative)
{
  if (width != 1 && width != 2 && width != 4)
    return UINT32_C(0xffffffff);

  // The task-file data register is 16-bit. Wider PSX accesses intentionally
  // consume consecutive data bytes rather than aliasing error/count registers.
  if (relative == CS0_BASE)
  {
    u32 value = 0;
    for (u32 i = 0; i < width; i++)
      value |= static_cast<u32>(ReadDataByte()) << (i * 8);
    return value;
  }

  u32 value = 0;
  for (u32 i = 0; i < width; i++)
    value |= static_cast<u32>(ReadBridgeByte(relative + i)) << (i * 8);
  return value;
}
static void WriteBridge(u32 width, u32 relative, u32 value)
{
  if (width != 1 && width != 2 && width != 4)
    return;

  if (relative == CS0_BASE)
  {
    for (u32 i = 0; i < width; i++)
      WriteDataByte(static_cast<u8>(value >> (i * 8)));
    return;
  }

  for (u32 i = 0; i < width; i++)
    WriteBridgeByte(relative + i, static_cast<u8>(value >> (i * 8)));
}

static u32 ReadData32(u32 width, u32 relative)
{
  if (relative != DATA32_PORT || (width != 1 && width != 2 && width != 4))
    return width == 1 ? UINT32_C(0xff) : width == 2 ? UINT32_C(0xffff) : UINT32_C(0xffffffff);

  // ArcadeDuck delivers the PSX access width directly. Primal Rage II uses lw
  // at 0x1F7F41F0, so consume exactly four FIFO bytes for that 32-bit access.
  u32 value = 0;
  for (u32 i = 0; i < width; i++)
    value |= static_cast<u32>(ReadDataByte()) << (i * 8);
  return value;
}
static void WriteData32(u32 width, u32 relative, u32 value)
{
  if (relative != DATA32_PORT || (width != 1 && width != 2 && width != 4))
    return;

  // The game contains an ATA write path. Keep the wide FIFO aperture symmetric
  // without assigning semantics to the otherwise-unused 0x1F2 halfword latch.
  for (u32 i = 0; i < width; i++)
    WriteDataByte(static_cast<u8>(value >> (i * 8)));
}

static u32 ReadIRQControl(u32 width, u32 relative)
{
  if (width != 1 && width != 2 && width != 4)
    return UINT32_C(0xffffffff);

  u32 value = 0;
  for (u32 i = 0; i < width; i++)
  {
    const u32 byte = relative + i;
    const u8 data = byte < 2 ? static_cast<u8>(s_state.irq_control >> (byte * 8)) : UINT8_C(0xff);
    value |= static_cast<u32>(data) << (i * 8);
  }
  return value;
}

static void WriteIRQControl(u32 width, u32 relative, u32 value)
{
  if (width != 1 && width != 2 && width != 4)
    return;

  const u16 old_control = s_state.irq_control;
  u16 new_control = old_control;
  for (u32 i = 0; i < width; i++)
  {
    const u32 byte = relative + i;
    if (byte >= 2)
      continue;

    const u32 shift = byte * 8;
    new_control = static_cast<u16>((new_control & ~(UINT16_C(0xff) << shift)) |
                                   ((static_cast<u16>(value >> (i * 8)) & UINT16_C(0xff)) << shift));
  }

  s_state.irq_control = new_control;
  UpdateIRQLine();
}

} // namespace

bool Initialize(std::string_view chd_path, Error* error)
{
  Shutdown();
  if (chd_path.empty())
  {
    Error::SetStringView(error, "Primal Rage II is missing its ATA hard-disk CHD path.");
    return false;
  }

  s_state.path.assign(chd_path);
  if (!s_state.disk.Open(s_state.path.c_str(), error))
  {
    s_state.path.clear();
    return false;
  }

  if (s_state.disk.GetBlockCount() != PRIMAL_RAGE_II_EXPECTED_BLOCKS)
  {
    WARNING_LOG("TimeWarnerATA Primal Rage II CHD block count is {}, expected {} (0x{:08X}).",
                s_state.disk.GetBlockCount(), PRIMAL_RAGE_II_EXPECTED_BLOCKS, PRIMAL_RAGE_II_EXPECTED_BLOCKS);
  }

  s_initialized = true;
  s_state.irq_control = 0;
  ResetVIAConfiguration();
  ResetTaskFile();

  const auto& geometry = s_state.disk.GetGeometry();
  VERBOSE_LOG("TimeWarnerATA initialized path='{}' geometry={}/{}/{} blocks={} "
           "bridge='0x1F7E4000' irqctrl='0x1F7E8000' data32='0x1F7F4000' irq='IRQ10 gate bit0'",
           s_state.path, geometry.cylinders, geometry.heads, geometry.sectors, s_state.disk.GetBlockCount());
  return true;
}

void Shutdown()
{
  if (!s_initialized && !s_state.disk.IsOpen())
    return;

  CancelReadSectorReadyEvent();
  DMA::SetRequest(DMA::Channel::PIO, false);
  ClearIRQ();
  s_state.disk.Close();
  s_state = {};
  s_initialized = false;
}

void Reset()
{
  if (!s_initialized)
    return;

  s_state.irq_control = 0;
  s_state.device_control = 0;
  ResetVIAConfiguration();
  ResetTaskFile();
}

bool IsActive()
{
  return s_initialized;
}

void DMARead(u32* data, u32 word_count)
{
  if (!s_initialized || !data)
    return;

  for (u32 i = 0; i < word_count; i++)
  {
    u32 value = 0;
    for (u32 byte = 0; byte < 4; byte++)
      value |= static_cast<u32>(ReadDataByte()) << (byte * 8u);
    data[i] = value;
  }

  UpdateDMARequest();
}
void DMAWrite(const u32* data, u32 word_count)
{
  if (!s_initialized || !data)
    return;

  for (u32 i = 0; i < word_count; i++)
  {
    const u32 value = data[i];
    for (u32 byte = 0; byte < 4; byte++)
      WriteDataByte(static_cast<u8>(value >> (byte * 8u)));
  }

  UpdateDMARequest();
}

bool HandlesOffset(u32 offset)
{
  return s_initialized && ((offset >= VIA_BRIDGE_BASE && offset <= VIA_BRIDGE_END) ||
                           (offset >= IRQ_CONTROL_BASE && offset <= IRQ_CONTROL_END) ||
                           (offset >= DATA32_BASE && offset <= DATA32_END));
}

u32 Read(u32 width, u32 offset)
{
  if (!HandlesOffset(offset))
    return UINT32_C(0xffffffff);

  if (offset >= VIA_BRIDGE_BASE && offset <= VIA_BRIDGE_END)
    return ReadBridge(width, offset - VIA_BRIDGE_BASE);
  if (offset >= IRQ_CONTROL_BASE && offset <= IRQ_CONTROL_END)
    return ReadIRQControl(width, offset - IRQ_CONTROL_BASE);
  return ReadData32(width, offset - DATA32_BASE);
}

void Write(u32 width, u32 offset, u32 value)
{
  if (!HandlesOffset(offset))
    return;

  if (offset >= VIA_BRIDGE_BASE && offset <= VIA_BRIDGE_END)
    WriteBridge(width, offset - VIA_BRIDGE_BASE, value);
  else if (offset >= IRQ_CONTROL_BASE && offset <= IRQ_CONTROL_END)
    WriteIRQControl(width, offset - IRQ_CONTROL_BASE, value);
  else
    WriteData32(width, offset - DATA32_BASE, value);
}

} // namespace SonyZN::TimeWarnerATA
