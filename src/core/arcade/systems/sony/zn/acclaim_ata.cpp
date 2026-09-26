// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/sony/zn/acclaim_ata.h"

#include "core/arcade/devices/storage/chd_hard_disk.h"
#include "core/dma.h"
#include "core/interrupt_controller.h"

#include "common/error.h"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

Log_SetChannel(AcclaimATA);

namespace SonyZN::AcclaimATA {
namespace {

static constexpr u32 DMA_BRIDGE_ADDRESS_BASE = 0x1fff18;
static constexpr u32 DMA_BRIDGE_CONTROL_BASE = 0x1fff1c;
static constexpr u32 DMA_BRIDGE_END = 0x1fff1f;

static constexpr u32 CS1_BASE = 0x1fff80;
static constexpr u32 CS1_END = 0x1fff8f;
static constexpr u32 CS0_BASE = 0x1fff90;
static constexpr u32 CS0_END = 0x1fff9f;
static constexpr u32 ALT_STATUS_DEVICE_CONTROL = CS1_BASE + 0x0c;

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

static constexpr u32 JDREDD_EXPECTED_BLOCKS = 0x002040f0;

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

  u32 dma_bridge_address = 0;
  u32 dma_bridge_control = 0;
};

RuntimeState s_state;
bool s_initialized = false;

static void UpdateDMARequest()
{
  const bool data_transfer =
    s_state.transfer_mode == TransferMode::Read || s_state.transfer_mode == TransferMode::Write;
  DMA::SetRequest(DMA::Channel::PIO, s_initialized && data_transfer && (s_state.status & ATA_STATUS_DRQ) != 0);
}

static void SetIRQLine(bool state)
{
  InterruptController::SetLineState(InterruptController::IRQ::IRQ10, state);
}

static void UpdateIRQLine()
{
  const bool enabled = (s_state.device_control & ATA_DEVICE_CONTROL_NIEN) == 0;
  SetIRQLine(s_state.irq_pending && enabled);
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
  s_state.dma_bridge_address = 0;
  s_state.dma_bridge_control = 0;
  UpdateDMARequest();
}

static u32 GetTaskLBA()
{
  return (static_cast<u32>(s_state.device_head & 0x0f) << 24) |
         (static_cast<u32>(s_state.lba_high) << 16) |
         (static_cast<u32>(s_state.lba_mid) << 8) |
         static_cast<u32>(s_state.lba_low);
}

static void SetTaskLBA(u32 lba)
{
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
  PutIdentifyString(10, 10, "JDREDD-QUANTUM-0001");
  PutIdentifyString(23, 4, "1.0");
  PutIdentifyString(27, 20, "QUANTUM 2.1GB ATA HARD DISK");
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
  LoadCurrentReadSector();
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
      if (clear_irq)
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
      DEV_LOG("AcclaimATA unsupported command 0x{:02X} lba=0x{:08X} count={}", command, GetTaskLBA(),
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

static u32 ReadCS0(u32 width, u32 offset)
{
  const u32 relative = offset - CS0_BASE;
  if (relative == 0)
  {
    if (width != 1 && width != 2 && width != 4)
      return UINT32_C(0xffffffff);

    u32 value = 0;
    for (u32 i = 0; i < width; i++)
      value |= static_cast<u32>(ReadDataByte()) << (i * 8);
    return value;
  }

  if ((relative & 1u) != 0 || relative > 0x0e)
    return width == 1 ? UINT32_C(0xff) : width == 2 ? UINT32_C(0xffff) : UINT32_C(0xffffffff);

  const u8 value = ReadTaskRegister(relative >> 1, true);
  if (width == 1)
    return value;
  if (width == 2)
    return UINT32_C(0xff00) | value;
  if (width == 4)
    return UINT32_C(0xffffff00) | value;
  return UINT32_C(0xffffffff);
}

static void WriteCS0(u32 width, u32 offset, u32 value)
{
  const u32 relative = offset - CS0_BASE;
  if (relative == 0)
  {
    if (width != 1 && width != 2 && width != 4)
      return;
    for (u32 i = 0; i < width; i++)
      WriteDataByte(static_cast<u8>(value >> (i * 8)));
    return;
  }

  if ((relative & 1u) != 0 || relative > 0x0e)
    return;

  WriteTaskRegister(relative >> 1, static_cast<u8>(value));
}

static u32 ReadCS1(u32 width, u32 offset)
{
  if (offset != ALT_STATUS_DEVICE_CONTROL)
    return width == 1 ? UINT32_C(0xff) : width == 2 ? UINT32_C(0xffff) : UINT32_C(0xffffffff);

  const u8 value = s_state.status;
  if (width == 1)
    return value;
  if (width == 2)
    return UINT32_C(0xff00) | value;
  if (width == 4)
    return UINT32_C(0xffffff00) | value;
  return UINT32_C(0xffffffff);
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

static void WriteCS1(u32 width, u32 offset, u32 value)
{
  if (offset == ALT_STATUS_DEVICE_CONTROL && (width == 1 || width == 2 || width == 4))
  {
    WriteDeviceControl(static_cast<u8>(value));
    return;
  }
}

static u32 ReadBridgeRegister(u32 width, u32 offset)
{
  const u32 base = (offset < DMA_BRIDGE_CONTROL_BASE) ? DMA_BRIDGE_ADDRESS_BASE : DMA_BRIDGE_CONTROL_BASE;
  const u32 value = (base == DMA_BRIDGE_ADDRESS_BASE) ? s_state.dma_bridge_address : s_state.dma_bridge_control;
  const u32 shift = (offset - base) * 8u;
  if (width == 1)
    return (value >> shift) & 0xffu;
  if (width == 2)
    return (value >> shift) & 0xffffu;
  if (width == 4 && offset == base)
    return value;
  return UINT32_C(0xffffffff);
}

static void WriteBridgeRegister(u32 width, u32 offset, u32 value)
{
  const u32 base = (offset < DMA_BRIDGE_CONTROL_BASE) ? DMA_BRIDGE_ADDRESS_BASE : DMA_BRIDGE_CONTROL_BASE;
  u32& reg = (base == DMA_BRIDGE_ADDRESS_BASE) ? s_state.dma_bridge_address : s_state.dma_bridge_control;
  const u32 byte_offset = offset - base;
  if ((width != 1 && width != 2 && width != 4) || byte_offset + width > 4)
    return;

  for (u32 i = 0; i < width; i++)
  {
    const u32 shift = (byte_offset + i) * 8u;
    reg = (reg & ~(UINT32_C(0xff) << shift)) | (((value >> (i * 8u)) & 0xffu) << shift);
  }

  UpdateDMARequest();
}

} // namespace

bool Initialize(std::string_view chd_path, Error* error)
{
  Shutdown();
  if (chd_path.empty())
  {
    Error::SetStringView(error, "Judge Dredd is missing its ATA hard-disk CHD path.");
    return false;
  }

  s_state.path.assign(chd_path);
  if (!s_state.disk.Open(s_state.path.c_str(), error))
  {
    s_state.path.clear();
    return false;
  }

  if (s_state.disk.GetBlockCount() != JDREDD_EXPECTED_BLOCKS)
  {
    WARNING_LOG("AcclaimATA Judge Dredd CHD block count is {}, expected {} (0x{:08X}).",
                s_state.disk.GetBlockCount(), JDREDD_EXPECTED_BLOCKS, JDREDD_EXPECTED_BLOCKS);
  }

  s_initialized = true;
  ResetTaskFile();

  const auto& geometry = s_state.disk.GetGeometry();
  VERBOSE_LOG("AcclaimATA initialized path='{}' geometry={}/{}/{} blocks={} cs1='0x1FBFFF80-8F' "
           "cs0='0x1FBFFF90-9F' irq='IRQ10'",
           s_state.path, geometry.cylinders, geometry.heads, geometry.sectors, s_state.disk.GetBlockCount());
  return true;
}

void Shutdown()
{
  if (!s_initialized && !s_state.disk.IsOpen())
    return;

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

  s_state.device_control = 0;
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
  return s_initialized && ((offset >= DMA_BRIDGE_ADDRESS_BASE && offset <= DMA_BRIDGE_END) ||
                           (offset >= CS1_BASE && offset <= CS1_END) ||
                           (offset >= CS0_BASE && offset <= CS0_END));
}

u32 Read(u32 width, u32 offset)
{
  if (!HandlesOffset(offset))
    return UINT32_C(0xffffffff);

  if (offset >= DMA_BRIDGE_ADDRESS_BASE && offset <= DMA_BRIDGE_END)
    return ReadBridgeRegister(width, offset);
  if (offset >= CS0_BASE)
    return ReadCS0(width, offset);
  return ReadCS1(width, offset);
}

void Write(u32 width, u32 offset, u32 value)
{
  if (!HandlesOffset(offset))
    return;

  if (offset >= DMA_BRIDGE_ADDRESS_BASE && offset <= DMA_BRIDGE_END)
    WriteBridgeRegister(width, offset, value);
  else if (offset >= CS0_BASE)
    WriteCS0(width, offset, value);
  else
    WriteCS1(width, offset, value);
}

} // namespace SonyZN::AcclaimATA
