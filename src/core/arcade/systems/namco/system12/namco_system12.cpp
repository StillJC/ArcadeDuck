// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system12/namco_system12.h"
#include "core/arcade/devices/cpu/h8_3002_timer16.h"
#include "core/arcade/devices/cpu/h8_3002_intc.h"
#include "core/arcade/devices/audio/namco_c352.h"
#include "core/arcade/devices/cpu/h8_3002_peripherals.h"
#include "core/arcade/devices/jvs/jvs_bus.h"
#include "core/arcade/devices/jvs/namco_cyberlead.h"
#include "core/arcade/devices/jvs/jvs_chain.h"
#include "core/arcade/devices/jvs/namco_emio102.h"
#include "core/arcade/devices/jvs/namco_empri101.h"

#include "core/arcade/arcade_database.h"
#include "core/arcade/arcade_input.h"
#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/dma.h"
#include "core/system.h"
#include "core/timing_event.h"
#include "core/settings.h"

extern "C" {
#include "core/arcade/third_party/h8_300h/system.h"
}

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/minizip_helpers.h"
#include "common/sha1_digest.h"
#include "common/string_util.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <memory>
#include <span>
#include <utility>

Log_SetChannel(NamcoSystem12);

namespace NamcoSystem12 {
namespace {

static constexpr u32 PROGRAM_ROM_SIZE = 0x400000;
static constexpr u32 SUB_PROGRAM_SIZE = 0x80000;
static constexpr u32 C352_SAMPLE_ROM_SIZE = 0x1000000;
static constexpr u32 SHARED_RAM_SIZE = 0x10000;
static constexpr u32 PSX_SHARED_WINDOW_SIZE = 0x4000;
static constexpr u32 BANK_WINDOW_SIZE = 0x200000;

static constexpr u32 BANK_REGISTER_BASE = 0x000000;
static constexpr u32 BANK_REGISTER_END = 0x000003;
static constexpr u32 DMA_COMPLETE_STROBE_BASE = 0x008000;
static constexpr u32 DMA_COMPLETE_STROBE_END = 0x008003;
static constexpr u32 UNKNOWN_STROBE_BASE = 0x010000;
static constexpr u32 UNKNOWN_STROBE_END = 0x010003;
static constexpr u32 WAIT_PACING_STROBE_BASE = 0x018000;
static constexpr u32 WAIT_PACING_STROBE_END = 0x018003;
static constexpr u32 SHARED_RAM_BASE = 0x080000;
static constexpr u32 SHARED_RAM_END = SHARED_RAM_BASE + PSX_SHARED_WINDOW_SIZE - 1;
static constexpr u32 BOARD_CONTROL_BASE = 0x1bff08;
static constexpr u32 BOARD_CONTROL_END = 0x1bff0f;

// Later MOTHER(B)-class boot code serializes program-ROM halfwords to FF0E.
// The bulk length is derived from the physical 4 MiB program-ROM geometry:
// source 0x20200..end, one halfword every four bytes. After the bulk pass,
// the loader repeats the first four feed words as a completion/finalization
// signature. Keep these names neutral until the electrical destination of
// FF0E is known.
static constexpr u32 BOARD_CONFIG_FEED_ROM_OFFSET = 0x20200;
static constexpr u32 BOARD_CONFIG_FEED_STRIDE = 4;
static constexpr u32 BOARD_CONFIG_FINALIZE_WORDS = 4;
static constexpr u32 DMA_SOURCE_BASE = 0x700000;
static constexpr u32 DMA_SOURCE_END = 0x70ffff;
static constexpr u32 EEPROM_BASE = 0x140000;
static constexpr u32 EEPROM_END = 0x140fff;
static constexpr size_t EEPROM_SIZE = 0x800;

// M8F4 ROM-board protection complex. These are EXP3-relative offsets for
// physical 0x1FB00000 and 0x1FB80000.
static constexpr u32 M8F4_PROTECTION1_BASE = 0x100000;
static constexpr u32 M8F4_PROTECTION1_END = M8F4_PROTECTION1_BASE + 3;
static constexpr u32 M8F4_PROTECTION2_BASE = 0x180000;
static constexpr u32 M8F4_PROTECTION2_END = M8F4_PROTECTION2_BASE + 3;

// Protected-DMA logical namespace recovered from the original software.
static constexpr u32 M8F4_GAME_ROM_LIMIT = 0x03800000;
static constexpr u32 M8F4_PROGRAM_ROM_BASE = 0x04000000;
static constexpr u32 M8F4_PROGRAM_ROM_LIMIT = 0x04400000;

static constexpr u32 H8_CLOCK_HZ = 16934400;

static constexpr u32 H8_ROM_BASE = 0x000000;
static constexpr u32 H8_ROM_END = 0x07ffff;
static constexpr u32 H8_SHARED_BASE = 0x080000;
static constexpr u32 H8_SHARED_END = 0x08ffff;
static constexpr u32 H8_C352_BASE = 0x280000;
static constexpr u32 H8_C352_END = 0x287fff;
static constexpr u32 H8_JAMMA_BASE = 0x300000;
static constexpr u32 H8_JAMMA_END = 0x300003;
static constexpr u32 H8_WAIT0_BASE = 0x300010;
static constexpr u32 H8_WAIT0_END = 0x300011;
static constexpr u32 H8_WAIT1_BASE = 0x300030;
static constexpr u32 H8_WAIT1_END = 0x300031;
static constexpr u32 H8_INTERNAL_BASE = 0xff0000;

static constexpr u8 H8_JVS_SENSE_INITIALIZED = 0;
static constexpr u8 H8_JVS_SENSE_UNINITIALIZED = 1;
static constexpr u8 H8_JVS_SENSE_NONE = 2;

static constexpr u8 H8_SCI_SMR_CA = 0x80;
static constexpr u8 H8_SCI_SMR_CKS = 0x03;
static constexpr u8 H8_SCI_SCR_TIE = 0x80;
static constexpr u8 H8_SCI_SCR_RIE = 0x40;
static constexpr u8 H8_SCI_SCR_TE = 0x20;
static constexpr u8 H8_SCI_SCR_RE = 0x10;
static constexpr u8 H8_SCI_SCR_TEIE = 0x04;
static constexpr u8 H8_SCI_SSR_TDRE = 0x80;
static constexpr u8 H8_SCI_SSR_RDRF = 0x40;
static constexpr u8 H8_SCI_SSR_ORER = 0x20;
static constexpr u8 H8_SCI_SSR_FER = 0x10;
static constexpr u8 H8_SCI_SSR_PER = 0x08;
static constexpr u8 H8_SCI_SSR_TEND = 0x04;
static constexpr u8 H8_SCI_SSR_MPB = 0x02;
static constexpr u8 H8_SCI_SSR_MPBT = 0x01;

static constexpr u32 CYBERLEAD_JVS_BAUD = 115200;
static constexpr u32 CYBERLEAD_SCHEDULER_HZ = CYBERLEAD_JVS_BAUD * 2;
static constexpr size_t CYBERLEAD_LED_FIRMWARE_SIZE = 0x40000;
static constexpr std::array<u8, SHA1Digest::DIGEST_SIZE> CYBERLEAD_LED_FIRMWARE_SHA1 = {
  UINT8_C(0x61), UINT8_C(0xD6), UINT8_C(0x13), UINT8_C(0x38), UINT8_C(0xE1),
  UINT8_C(0xBB), UINT8_C(0x41), UINT8_C(0x4A), UINT8_C(0xF3), UINT8_C(0x2D),
  UINT8_C(0xBD), UINT8_C(0xC1), UINT8_C(0xE2), UINT8_C(0x4D), UINT8_C(0x54),
  UINT8_C(0xC0), UINT8_C(0x2F), UINT8_C(0xB9), UINT8_C(0x19), UINT8_C(0x6E)
};


enum class JVSProfile : u8
{
  None,
  CyberLead,
  NamcoEMIO102Printer
};

struct JVSState
{
  JVSProfile profile = JVSProfile::None;


  bool initialize_sense_after_response = false;

};

struct RuntimeState
{
  LoadedContent content;
  std::array<u8, SHARED_RAM_SIZE> shared_ram{};
  std::array<u8, 8> board_control{};
  u32 bank = 0;
  u32 dma_source = 0;
  u64 dma_transfer_count = 0;

  // M8F4 protected ROM-board state. Protection is a ROM-board capability,
  // not generic System 12 KEYCUS behavior.
  std::array<u32, 2> m8f4_challenge{};
  u32 m8f4_challenge_count = 0;
  u32 m8f4_protected_source = 0;
  u32 m8f4_response = 0;
  u16 m8f4_nonce = 0;
  u16 m8f4_response_class = 0;
  bool m8f4_challenge_active = false;
  bool m8f4_response_valid = false;
  bool m8f4_protected_dma_armed = false;
  bool m8f4_waiting_for_release = false;
  bool m8f4_ready = true;
  u64 m8f4_transaction_count = 0;
  u64 m8f4_dma_count = 0;

  std::unique_ptr<h8_system_t> h8_cpu;
  std::unique_ptr<TimingEvent> h8_timing_event;
  TickCount h8_scheduler_interval_ticks = System::MASTER_CLOCK / 1000;
  u32 h8_scheduler_rate_hz = 1000;
  s64 h8_cycle_balance = 0;
  u32 h8_cycle_fraction = 0;
  u32 h8_rtc_scheduler_fraction = 0;
  std::array<u8, 0x10000> h8_internal{};
  u64 h8_scheduler_count = 0;

  // Attack Pla Rail mailbox/task diagnostic state.
  // Diagnostic only; no emulation behavior depends on these fields.
  u32 h8_aplarail_last_irq_vector = UINT32_C(0xFFFFFFFF);
  u32 h8_aplarail_last_irq_from_pc = 0;
  u32 h8_aplarail_last_irq_to_pc = 0;
  u64 h8_aplarail_last_irq_scheduler = 0;
  u64 h8_aplarail_irq13_count = 0;
  u64 h8_aplarail_irq26_count = 0;
  u64 h8_aplarail_irq30_count = 0;
  u32 h8_aplarail_prev_step_pc = UINT32_C(0xFFFFFFFF);
  u32 h8_aplarail_task_trace_count = 0;
  u32 h8_aplarail_irq_trace_count = 0;
  u8 h8_irq_input = 0;
  u8 h8_jvs_sense = H8_JVS_SENSE_NONE;
  bool h8_jvs_rts = false;
  Arcade::JVS::Bus h8_jvs_bus;
  Arcade::JVS::NamcoCyberLead h8_cyberlead;
  Arcade::JVS::Chain h8_jvs_chain;
  Arcade::JVS::NamcoEMIO102 h8_emio102;
  Arcade::JVS::NamcoEMPri101 h8_empri101;
  JVSState h8_jvs{};
  u8 h8_sci0_ssr_read = 0;

  // Cyber Lead JVS transport pacing. The JVS protocol responder remains
  // high-level; these fields model the time bytes spend in H8 SCI0.
  bool h8_sci0_tx_busy = false;
  u8 h8_sci0_tx_shift_byte = UINT8_C(0xFF);
  u32 h8_sci0_tx_clocks_remaining = 0;
  bool h8_sci0_rx_busy = false;
  u32 h8_sci0_rx_clocks_remaining = 0;
  bool h8_sci0_pacing_logged = false;

  u8 h8_sci1_ssr_read = 0;
  bool h8_sci1_rx_busy = false;
  u32 h8_sci1_rx_clocks_remaining = 0;

  u8 h8_sub_porta = 0;
  u8 h8_sub_portb = 0x50;

  bool h8_settings_ce = false;
  bool h8_settings_clk = true;
  bool h8_settings_data = false;
  u8 h8_settings_address = 0;
  u8 h8_settings_value = 0;
  u8 h8_settings_bit = 0;

  bool h8_rtc_ce = false;
  bool h8_rtc_clk = false;
  bool h8_rtc_data = false;
  u8 h8_rtc_bit = 0;
  std::array<u8, 7> h8_rtc_regs{};
  std::time_t h8_rtc_time = 0;
  u32 h8_rtc_millisecond_accumulator = 0;

  bool h8_vblank = false;
  bool h8_fault_logged = false;
  std::string persistence_directory;
  std::string eeprom_path;
  bool eeprom_dirty = false;

  Arcade::H8::H83002Timer16 h8_timer16;

  Arcade::H8::H83002Peripherals h8_peripherals;

  Arcade::H8::H83002Intc h8_intc;
  Arcade::Audio::NamcoC352 c352;
  bool mainboard_reset_pending = false;
  bool boot_warm_reset_completed = false;
  bool cold_boot_h8_released = true;
  std::array<u16, BOARD_CONFIG_FINALIZE_WORDS> cold_boot_config_header{};
  u32 cold_boot_config_feed_words = 0;
  u32 cold_boot_config_finalize_words = 0;
  bool cold_boot_config_bulk_complete = false;
  u64 ram_preserving_reset_count = 0;
};

std::optional<RuntimeState> s_runtime;

bool IsM8F4(const RuntimeState& runtime)
{
  return runtime.content.rom_board_profile == ROMBoardProfile::M8F4Protected;
}

u32 ReadM8F4RegisterValue(u32 width, u32 byte_offset, u32 value)
{
  if (width == 4 && byte_offset == 0)
    return value;

  if (width == 2 && (byte_offset & 1u) == 0 && byte_offset <= 2)
    return (value >> (byte_offset * 8u)) & UINT32_C(0x0000FFFF);

  if (width == 1 && byte_offset <= 3)
    return (value >> (byte_offset * 8u)) & UINT32_C(0x000000FF);

  return UINT32_C(0xFFFFFFFF);
}

u32 DecodeM8F4Response(RuntimeState& runtime)
{
  const u32 challenge0 = runtime.m8f4_challenge[0];
  const u32 challenge1 = runtime.m8f4_challenge[1];

  const u8 class0 = static_cast<u8>((challenge0 >> 16) & UINT32_C(0xFF));
  const u8 class1 = static_cast<u8>((challenge1 >> 16) & UINT32_C(0xFF));
  const u16 class1_word = static_cast<u16>(challenge1 >> 16);

  runtime.m8f4_response_class = 0;

  // The challenge words themselves move with executable layout. Decode only
  // the recovered invariant class fields and return the required response lane.
  if (class0 == UINT8_C(0x6D))
  {
    runtime.m8f4_response_class = UINT16_C(0x006D);
    return UINT32_C(0x000036E2);
  }
  if (class1 == UINT8_C(0x82))
  {
    runtime.m8f4_response_class = UINT16_C(0x0082);
    return UINT32_C(0x41860000);
  }
  if (class0 == UINT8_C(0x4C))
  {
    runtime.m8f4_response_class = UINT16_C(0x004C);
    return UINT32_C(0x00002651);
  }
  if (class1_word == UINT16_C(0x0178))
  {
    runtime.m8f4_response_class = UINT16_C(0x0178);
    return UINT32_C(0x3C7D0000);
  }
  if (class1 == UINT8_C(0xA9))
  {
    runtime.m8f4_response_class = UINT16_C(0x00A9);
    return UINT32_C(0x552E0000);
  }

  return 0;
}

static std::array<u8, EEPROM_SIZE> s_eeprom{};

bool LoadEEPROM(RuntimeState& runtime, Error* error)
{
  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  runtime.persistence_directory = Path::Combine(nvram_root, runtime.content.set_name);
  runtime.eeprom_path = Path::Combine(runtime.persistence_directory, "at28c16");
  runtime.eeprom_dirty = false;
  s_eeprom.fill(UINT8_C(0xff));

  const bool persistence_exists = FileSystem::FileExists(runtime.eeprom_path.c_str());
  if (persistence_exists)
  {
    std::optional<DynamicHeapArray<u8>> data =
      FileSystem::ReadBinaryFile(runtime.eeprom_path.c_str(), error);

    if (!data || data->size() != EEPROM_SIZE)
    {
      const size_t actual_size = data ? data->size() : 0;
      Error::SetStringFmt(error,
                          "Namco System 12 AT28C16 '{}' has invalid size {}; expected {} bytes.",
                          runtime.eeprom_path, actual_size, EEPROM_SIZE);
      ERROR_LOG("System12 AT28C16 malformed persistence path='{}' actual_size={} expected_size={}",
                runtime.eeprom_path, actual_size, EEPROM_SIZE);
      return false;
    }

    std::memcpy(s_eeprom.data(), data->data(), s_eeprom.size());
  }
  else
  {
    if (!FileSystem::CreateDirectory(nvram_root.c_str(), false) ||
        !FileSystem::CreateDirectory(runtime.persistence_directory.c_str(), false) ||
        !FileSystem::WriteBinaryFile(runtime.eeprom_path.c_str(), s_eeprom.data(), s_eeprom.size()))
    {
      Error::SetStringFmt(error, "Failed to create Namco System 12 AT28C16 persistence '{}'.",
                          runtime.eeprom_path);
      ERROR_LOG("System12 AT28C16 persistence create failed set='{}' path='{}'",
                runtime.content.set_name, runtime.eeprom_path);
      return false;
    }
  }

  INFO_LOG("System12 AT28C16 {} set='{}' size={} path='{}'",
           persistence_exists ? "loaded" : "initialized",
           runtime.content.set_name, EEPROM_SIZE, runtime.eeprom_path);
  return true;
}

void SaveEEPROM(RuntimeState& runtime)
{
  if (!runtime.eeprom_dirty)
    return;

  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  if (!FileSystem::CreateDirectory(nvram_root.c_str(), false) ||
      !FileSystem::CreateDirectory(runtime.persistence_directory.c_str(), false) ||
      !FileSystem::WriteBinaryFile(runtime.eeprom_path.c_str(), s_eeprom.data(), s_eeprom.size()))
  {
    ERROR_LOG("System12 AT28C16 save failed set='{}' path='{}'",
              runtime.content.set_name, runtime.eeprom_path);
    return;
  }

  runtime.eeprom_dirty = false;
  INFO_LOG("System12 AT28C16 saved set='{}' path='{}'",
           runtime.content.set_name, runtime.eeprom_path);
}

u32 ReadEEPROM(RuntimeState& runtime, u32 width, u32 offset)
{
  if (offset < EEPROM_BASE || offset > EEPROM_END ||
      (width != 1 && width != 2 && width != 4))
  {
    return UINT32_C(0xffffffff);
  }

  const u32 relative_offset = offset - EEPROM_BASE;
  u32 value = UINT32_C(0xffffffff);

  // MAME maps the 8-bit AT28C16 on byte lanes 0 and 2 of the 32-bit PSX bus.
  for (u32 i = 0; i < width && (relative_offset + i) <= (EEPROM_END - EEPROM_BASE); i++)
  {
    const u32 bus_offset = relative_offset + i;
    if ((bus_offset & 1u) == 0)
    {
      const size_t eeprom_offset = static_cast<size_t>(bus_offset >> 1);
      if (eeprom_offset < s_eeprom.size())
      {
        value = (value & ~(UINT32_C(0xff) << (i * 8))) |
                (static_cast<u32>(s_eeprom[eeprom_offset]) << (i * 8));
      }
    }
  }

  return value;
}

void WriteEEPROM(RuntimeState& runtime, u32 width, u32 offset, u32 value)
{
  if (offset < EEPROM_BASE || offset > EEPROM_END ||
      (width != 1 && width != 2 && width != 4))
  {
    return;
  }

  const u32 relative_offset = offset - EEPROM_BASE;
  bool changed = false;

  for (u32 i = 0; i < width && (relative_offset + i) <= (EEPROM_END - EEPROM_BASE); i++)
  {
    const u32 bus_offset = relative_offset + i;
    if ((bus_offset & 1u) != 0)
      continue;

    const size_t eeprom_offset = static_cast<size_t>(bus_offset >> 1);
    if (eeprom_offset >= s_eeprom.size())
      continue;

    const u8 new_value = static_cast<u8>(value >> (i * 8));
    if (s_eeprom[eeprom_offset] != new_value)
    {
      s_eeprom[eeprom_offset] = new_value;
      changed = true;
    }
  }

  if (changed)
    runtime.eeprom_dirty = true;

}

// System12 AT28C16
bool LoadROMMember(const char* archive_path, const Arcade::Database::ROMDefinition& rom, std::vector<u8>* data,
                   Error* error)
{
  unzFile zf = MinizipHelpers::OpenUnzFile(archive_path);
  if (!zf)
  {
    Error::SetStringFmt(error, "Failed to open Namco System 12 set archive '{}'.", archive_path);
    return false;
  }

  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    Error::SetStringFmt(error, "Namco System 12 set archive '{}' is empty or unreadable.", archive_path);
    return false;
  }

  for (;;)
  {
    unz_file_info64 file_info = {};
    char member_name[512] = {};
    if (unzGetCurrentFileInfo64(zf, &file_info, member_name, sizeof(member_name), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed to read file information from Namco System 12 set archive '{}'.",
                          archive_path);
      return false;
    }

    member_name[sizeof(member_name) - 1] = '\0';
    if (StringUtil::EqualNoCase(member_name, rom.name))
    {
      if (file_info.uncompressed_size != rom.size)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "System 12 ROM '{}' has size {}, expected {} bytes.", rom.name,
                            file_info.uncompressed_size, rom.size);
        return false;
      }
      if (rom.has_crc32 && static_cast<u32>(file_info.crc) != rom.crc32)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "System 12 ROM '{}' has CRC32 {:08x}, expected {:08x}.", rom.name,
                            static_cast<u32>(file_info.crc), rom.crc32);
        return false;
      }
      if (unzOpenCurrentFile(zf) != UNZ_OK)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Failed to decompress System 12 ROM '{}' from '{}'.", rom.name, archive_path);
        return false;
      }

      data->resize(rom.size);
      size_t read_offset = 0;
      while (read_offset < data->size())
      {
        const int bytes_read = unzReadCurrentFile(zf, data->data() + read_offset,
                                                  static_cast<unsigned>(data->size() - read_offset));
        if (bytes_read <= 0)
        {
          unzCloseCurrentFile(zf);
          unzClose(zf);
          Error::SetStringFmt(error, "Failed reading System 12 ROM '{}' from '{}'.", rom.name, archive_path);
          return false;
        }
        read_offset += static_cast<size_t>(bytes_read);
      }

      const int close_result = unzCloseCurrentFile(zf);
      unzClose(zf);
      if (close_result != UNZ_OK)
      {
        Error::SetStringFmt(error, "CRC validation failed for System 12 ROM '{}' in '{}'.", rom.name, archive_path);
        return false;
      }

      if (!rom.sha1.empty())
      {
        auto digest = SHA1Digest::GetDigest(std::span<const u8>(data->data(), data->size()));
        const std::string digest_string = SHA1Digest::DigestToString(digest);
        if (!StringUtil::EqualNoCase(digest_string, rom.sha1))
        {
          Error::SetStringFmt(error, "System 12 ROM '{}' has SHA-1 {}, expected {}.", rom.name, digest_string,
                              rom.sha1);
          return false;
        }
      }

      return true;
    }

    const int next_result = unzGoToNextFile(zf);
    if (next_result == UNZ_END_OF_LIST_OF_FILE)
      break;
    if (next_result != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed while reading Namco System 12 set archive '{}'.", archive_path);
      return false;
    }
  }

  unzClose(zf);
  Error::SetStringFmt(error, "Namco System 12 set archive '{}' does not contain required ROM '{}'.", archive_path,
                      rom.name);
  return false;
}

size_t ComputeRangeExtent(u32 offset, u32 length, u32 group_size, u32 skip)
{
  if (length == 0 || group_size == 0)
    return static_cast<size_t>(offset);

  const size_t groups = (static_cast<size_t>(length) + group_size - 1) / group_size;
  const size_t last_group_size =
    static_cast<size_t>(length) - ((groups - 1) * static_cast<size_t>(group_size));
  return static_cast<size_t>(offset) + ((groups - 1) * (static_cast<size_t>(group_size) + skip)) + last_group_size;
}

size_t ComputeROMExtent(const Arcade::Database::ROMDefinition& rom)
{
  size_t extent = 0;
  if (!rom.segments.empty())
  {
    for (const Arcade::Database::ROMSegmentDefinition& segment : rom.segments)
      extent = std::max(extent, ComputeRangeExtent(segment.offset, segment.length, segment.group_size, segment.skip));
  }
  else
  {
    extent = ComputeRangeExtent(rom.offset, rom.size, rom.group_size, rom.skip);
  }

  return extent;
}

bool PlaceROM(const Arcade::Database::ROMDefinition& rom, std::span<const u8> source, std::span<u8> destination,
              Error* error)
{
  if (source.empty())
    return true;

  const auto place_range = [&](u32 source_offset, u32 length, u32 destination_offset, u32 group_size, u32 skip,
                               bool reverse) -> bool {
    const size_t source_start = static_cast<size_t>(source_offset);
    const size_t source_length = static_cast<size_t>(length);
    if (source_start > source.size() || source_length > (source.size() - source_start))
    {
      Error::SetStringFmt(error, "System 12 ROM '{}' segment exceeds its source data.", rom.name);
      return false;
    }
    if (group_size == 0)
    {
      Error::SetStringFmt(error, "System 12 ROM '{}' has an invalid group size of zero.", rom.name);
      return false;
    }

    const size_t group_length = static_cast<size_t>(group_size);
    const size_t stride = group_length + static_cast<size_t>(skip);
    size_t source_position = source_start;
    size_t destination_position = static_cast<size_t>(destination_offset);
    const size_t source_end = source_start + source_length;

    while (source_position < source_end)
    {
      const size_t bytes_this_group = std::min(group_length, source_end - source_position);
      if (destination_position > destination.size() || bytes_this_group > (destination.size() - destination_position))
      {
        Error::SetStringFmt(error, "System 12 ROM '{}' exceeds the '{}' region.", rom.name, rom.region);
        return false;
      }

      if (reverse)
      {
        for (size_t i = 0; i < bytes_this_group; i++)
          destination[destination_position + i] = source[source_position + (bytes_this_group - 1 - i)];
      }
      else
      {
        std::memcpy(destination.data() + destination_position, source.data() + source_position, bytes_this_group);
      }

      source_position += bytes_this_group;
      destination_position += stride;
    }

    return true;
  };

  if (!rom.segments.empty())
  {
    for (const Arcade::Database::ROMSegmentDefinition& segment : rom.segments)
    {
      if (segment.operation != "load" && segment.operation != "continue" && segment.operation != "reload")
      {
        Error::SetStringFmt(error, "System 12 ROM '{}' uses unsupported segment operation '{}'.", rom.name,
                            segment.operation);
        return false;
      }

      if (!place_range(segment.source_offset, segment.length, segment.offset, segment.group_size, segment.skip,
                       segment.reverse ^ rom.word_swap))
      {
        return false;
      }
    }
    return true;
  }

  return place_range(0, static_cast<u32>(source.size()), rom.offset, rom.group_size, rom.skip, rom.word_swap);
}

u32 ReadBytes(std::span<const u8> data, u32 width, u32 offset)
{
  const size_t read_offset = static_cast<size_t>(offset);
  if ((width != 1 && width != 2 && width != 4) || read_offset > data.size() ||
      static_cast<size_t>(width) > (data.size() - read_offset))
  {
    return UINT32_C(0xFFFFFFFF);
  }

  if (width == 1)
    return static_cast<u32>(data[read_offset]);

  if (width == 2)
  {
    u16 value;
    std::memcpy(&value, data.data() + read_offset, sizeof(value));
    return static_cast<u32>(value);
  }

  u32 value;
  std::memcpy(&value, data.data() + read_offset, sizeof(value));
  return value;
}

void WriteBytes(std::span<u8> data, u32 width, u32 offset, u32 value)
{
  const size_t write_offset = static_cast<size_t>(offset);
  if ((width != 1 && width != 2 && width != 4) || write_offset > data.size() ||
      static_cast<size_t>(width) > (data.size() - write_offset))
  {
    return;
  }

  if (width == 1)
  {
    data[write_offset] = static_cast<u8>(value);
  }
  else if (width == 2)
  {
    const u16 halfword = static_cast<u16>(value);
    std::memcpy(data.data() + write_offset, &halfword, sizeof(halfword));
  }
  else
  {
    std::memcpy(data.data() + write_offset, &value, sizeof(value));
  }
}

u32 ReadH8BE32(std::span<const u8> data, u32 offset)
{
  const size_t read_offset = static_cast<size_t>(offset);
  if (read_offset > data.size() || 4 > (data.size() - read_offset))
    return UINT32_C(0xFFFFFFFF);

  return (static_cast<u32>(data[read_offset + 0]) << 24) |
         (static_cast<u32>(data[read_offset + 1]) << 16) |
         (static_cast<u32>(data[read_offset + 2]) << 8) |
         static_cast<u32>(data[read_offset + 3]);
}

u8 ReadH8Port(const RuntimeState& runtime, u16 ddr_reg, u16 dr_reg, u8 external_input, u8 fixed_high_mask)
{
  const u8 ddr = runtime.h8_internal[ddr_reg];
  const u8 dr = runtime.h8_internal[dr_reg];
  return static_cast<u8>(fixed_high_mask | (dr & ddr) | (external_input & static_cast<u8>(~ddr)));
}

u8 ToBCD(u32 value)
{
  return static_cast<u8>(((value / 10) << 4) | (value % 10));
}

void UpdateH8RTCRegisters(RuntimeState& runtime)
{
  runtime.h8_rtc_regs.fill(0);

  const std::tm* const local = std::localtime(&runtime.h8_rtc_time);
  if (!local)
    return;

  static constexpr std::array<u8, 7> WEEKDAY = {7, 1, 2, 3, 4, 5, 6};

  runtime.h8_rtc_regs[0] = ToBCD(static_cast<u32>(local->tm_sec));
  runtime.h8_rtc_regs[1] = ToBCD(static_cast<u32>(local->tm_min));
  runtime.h8_rtc_regs[2] = ToBCD(static_cast<u32>(local->tm_hour));
  runtime.h8_rtc_regs[3] = WEEKDAY[static_cast<size_t>(local->tm_wday) % WEEKDAY.size()];
  runtime.h8_rtc_regs[4] = ToBCD(static_cast<u32>(local->tm_mday));
  runtime.h8_rtc_regs[5] = ToBCD(static_cast<u32>(local->tm_mon + 1));
  runtime.h8_rtc_regs[6] = ToBCD(static_cast<u32>((local->tm_year + 1900) % 100));
}

void ResetH8RTC(RuntimeState& runtime)
{
  runtime.h8_rtc_ce = false;
  runtime.h8_rtc_clk = false;
  runtime.h8_rtc_data = false;
  runtime.h8_rtc_bit = 0;
  runtime.h8_rtc_time = std::time(nullptr);
  runtime.h8_rtc_millisecond_accumulator = 0;
  UpdateH8RTCRegisters(runtime);
}

void AdvanceH8RTC(RuntimeState& runtime, u32 elapsed_milliseconds)
{
  runtime.h8_rtc_millisecond_accumulator += elapsed_milliseconds;
  while (runtime.h8_rtc_millisecond_accumulator >= 1000)
  {
    runtime.h8_rtc_millisecond_accumulator -= 1000;
    runtime.h8_rtc_time++;
    UpdateH8RTCRegisters(runtime);
  }
}

void H8RTCSetCE(RuntimeState& runtime, bool state)
{
  if (state && !runtime.h8_rtc_ce)
    runtime.h8_rtc_bit = 0;

  runtime.h8_rtc_ce = state;
}

void H8RTCSetClock(RuntimeState& runtime, bool state)
{
  if (runtime.h8_rtc_ce && !runtime.h8_rtc_clk && state)
  {
    // RTC4543 is read-only on System 12. Data changes on the RTC rising edge.
    if (runtime.h8_rtc_bit < 56)
    {
      const u32 reg = runtime.h8_rtc_bit / 8;
      const u32 bit = runtime.h8_rtc_bit & 7;
      runtime.h8_rtc_data = ((runtime.h8_rtc_regs[reg] >> bit) & 1) != 0;

      runtime.h8_rtc_bit++;
      if (runtime.h8_rtc_bit == 28)
        runtime.h8_rtc_bit = 32;
    }
  }

  runtime.h8_rtc_clk = state;
}

void H8SettingsSetCE(RuntimeState& runtime, bool state)
{
  if (state != runtime.h8_settings_ce)
  {
    runtime.h8_settings_ce = state;
    if (state)
      runtime.h8_settings_bit = 0;
  }
}

void H8SettingsSetClock(RuntimeState& runtime, bool state)
{
  if (runtime.h8_settings_ce && state != runtime.h8_settings_clk && state)
  {
    if (runtime.h8_settings_bit < 8)
      runtime.h8_settings_address =
        static_cast<u8>((runtime.h8_settings_address >> 1) |
                        (runtime.h8_settings_data ? UINT8_C(0x80) : UINT8_C(0x00)));
    else
      runtime.h8_settings_value =
        static_cast<u8>((runtime.h8_settings_value << 1) |
                        (runtime.h8_settings_data ? UINT8_C(1) : UINT8_C(0)));

    runtime.h8_settings_bit++;

    if (runtime.h8_settings_bit == 16)
      runtime.h8_settings_bit = 0;
  }

  runtime.h8_settings_clk = state;
}

u8 H8PortOutput(const RuntimeState& runtime, u16 ddr_reg, u16 dr_reg)
{
  // Matches H8 port output behavior for unmasked Port A/B pins:
  // input/high-Z pins are presented high to the board callback.
  return static_cast<u8>(runtime.h8_internal[dr_reg] |
                         static_cast<u8>(~runtime.h8_internal[ddr_reg]));
}

bool HasH8JVSDevice(const RuntimeState& runtime)
{
  return runtime.h8_jvs.profile != JVSProfile::None;
}

void ResetH8JVSProtocol(RuntimeState& runtime)
{
  JVSState& jvs = runtime.h8_jvs;
  runtime.h8_jvs_bus.Reset();
  runtime.h8_cyberlead.Reset();
  runtime.h8_jvs_chain.Reset();
  runtime.h8_sci0_ssr_read = 0;
  runtime.h8_sci0_tx_busy = false;
  runtime.h8_sci0_tx_shift_byte = UINT8_C(0xFF);
  runtime.h8_sci0_tx_clocks_remaining = 0;
  runtime.h8_sci0_rx_busy = false;
  runtime.h8_sci0_rx_clocks_remaining = 0;
  runtime.h8_sci0_pacing_logged = false;
  jvs.initialize_sense_after_response = false;

  runtime.h8_jvs_sense = HasH8JVSDevice(runtime) ? H8_JVS_SENSE_UNINITIALIZED : H8_JVS_SENSE_NONE;
}






void H8JVSTransmitByte(RuntimeState& runtime, u8 value);

bool H8SCI0CyberLeadPacing(const RuntimeState& runtime)
{
  return runtime.h8_jvs.profile == JVSProfile::CyberLead;
}

u32 H8SCI0CyberLeadFrameClocks(const RuntimeState& runtime)
{
  const u8 smr = runtime.h8_internal[UINT16_C(0xFFB0)];
  const u8 scr = runtime.h8_internal[UINT16_C(0xFFB2)];

  // System 12 SCI0 is externally clocked for JVS. MAME configures the
  // external SCI clock as 14.7456 MHz / 8; asynchronous H8 SCI divides
  // that by 16, yielding the JVS-standard 115200 bps.
  //
  // AplaRail programs SCR=0x72, so CKE1 is set and this is the active path.
  if ((smr & H8_SCI_SMR_CA) == 0 && (scr & UINT8_C(0x02)) != 0)
  {
    constexpr u32 FRAME_BITS = 10; // 1 start + 8 data + 1 stop
    return static_cast<u32>(
      (static_cast<u64>(H8_CLOCK_HZ) * FRAME_BITS + CYBERLEAD_JVS_BAUD - 1) /
      CYBERLEAD_JVS_BAUD);
  }

  // Fallback for any unexpected firmware mode: retain the prior
  // register-derived timing model rather than changing unrelated behavior.
  const u8 brr = runtime.h8_internal[UINT16_C(0xFFB1)];
  const u32 divider =
    (UINT32_C(2) << (2 * (smr & H8_SCI_SMR_CKS))) *
    (static_cast<u32>(brr) + 1);

  if ((smr & H8_SCI_SMR_CA) == 0)
    return std::max<u32>(UINT32_C(1), divider * UINT32_C(16) * UINT32_C(10));

  return std::max<u32>(UINT32_C(1), divider * UINT32_C(2) * UINT32_C(8));
}
void H8SCI0LogCyberLeadPacing(RuntimeState& runtime)
{
  if (runtime.h8_sci0_pacing_logged)
    return;

  runtime.h8_sci0_pacing_logged = true;

  const u8 smr = runtime.h8_internal[UINT16_C(0xFFB0)];
  const u8 brr = runtime.h8_internal[UINT16_C(0xFFB1)];
  const u8 scr = runtime.h8_internal[UINT16_C(0xFFB2)];
  const bool external_async =
    (smr & H8_SCI_SMR_CA) == 0 && (scr & UINT8_C(0x02)) != 0;

  u32 baud = CYBERLEAD_JVS_BAUD;
  if (!external_async)
  {
    const u32 divider =
      (UINT32_C(2) << (2 * (smr & H8_SCI_SMR_CKS))) *
      (static_cast<u32>(brr) + 1);

    baud =
      ((smr & H8_SCI_SMR_CA) == 0) ?
        (H8_CLOCK_HZ / std::max<u32>(UINT32_C(1), divider * UINT32_C(16))) :
        (H8_CLOCK_HZ / std::max<u32>(UINT32_C(1), divider * UINT32_C(2)));
  }

  INFO_LOG(
    "System12 CyberLead SCI0 paced transport active: SMR={:02X} BRR={:02X} "
    "SCR={:02X} external_async={} frame_clocks={} baud={} scheduler_hz={} "
    "tx_double_buffer=true",
    smr, brr, scr, external_async, H8SCI0CyberLeadFrameClocks(runtime), baud,
    runtime.h8_scheduler_rate_hz);
}
void H8SCI0CyberLeadStartReceive(RuntimeState& runtime)
{
  if (!H8SCI0CyberLeadPacing(runtime) ||
      runtime.h8_sci0_rx_busy ||
      runtime.h8_jvs_rts ||
      !runtime.h8_jvs_bus.HasResponseByte())
  {
    return;
  }

  const u8 scr = runtime.h8_internal[UINT16_C(0xFFB2)];
  const u8 ssr = runtime.h8_internal[UINT16_C(0xFFB4)];

  if ((scr & H8_SCI_SCR_RE) == 0 ||
      (ssr & H8_SCI_SSR_RDRF) != 0 ||
      (ssr & static_cast<u8>(H8_SCI_SSR_ORER |
                              H8_SCI_SSR_FER |
                              H8_SCI_SSR_PER)) != 0)
  {
    return;
  }

  H8SCI0LogCyberLeadPacing(runtime);
  runtime.h8_sci0_rx_busy = true;
  runtime.h8_sci0_rx_clocks_remaining = H8SCI0CyberLeadFrameClocks(runtime);
}

void H8SCI0CyberLeadCompleteReceive(RuntimeState& runtime)
{
  runtime.h8_sci0_rx_busy = false;
  runtime.h8_sci0_rx_clocks_remaining = 0;

  if (runtime.h8_jvs_rts || !runtime.h8_jvs_bus.HasResponseByte())
    return;

  const u8 scr = runtime.h8_internal[UINT16_C(0xFFB2)];
  u8& ssr = runtime.h8_internal[UINT16_C(0xFFB4)];

  if ((scr & H8_SCI_SCR_RE) == 0)
    return;

  if ((ssr & H8_SCI_SSR_RDRF) != 0)
  {
    ssr = static_cast<u8>(ssr | H8_SCI_SSR_ORER);
    if ((scr & H8_SCI_SCR_RIE) != 0)
      runtime.h8_intc.InternalInterrupt(UINT32_C(52)); // ERI0
    return;
  }

  JVSState& jvs = runtime.h8_jvs;
  runtime.h8_internal[UINT16_C(0xFFB5)] = runtime.h8_jvs_bus.ReadResponseByte();
  ssr = static_cast<u8>(ssr | H8_SCI_SSR_RDRF);

  if ((scr & H8_SCI_SCR_RIE) != 0)
    runtime.h8_intc.InternalInterrupt(UINT32_C(53)); // RXI0

  if (runtime.h8_jvs_bus.ResponseComplete() && jvs.initialize_sense_after_response)
  {
    jvs.initialize_sense_after_response = false;
    runtime.h8_jvs_sense = H8_JVS_SENSE_INITIALIZED;
  }
}

void H8SCI0CyberLeadStartTransmit(RuntimeState& runtime)
{
  if (!H8SCI0CyberLeadPacing(runtime))
    return;

  u8& ssr = runtime.h8_internal[UINT16_C(0xFFB4)];
  const u8 scr = runtime.h8_internal[UINT16_C(0xFFB2)];

  // If TSR is already active, the byte just written by the firmware remains
  // in TDR as the one-byte holding register. TDRE must stay clear until TSR
  // becomes available.
  if (runtime.h8_sci0_tx_busy)
  {
    ssr = static_cast<u8>(
      ssr & static_cast<u8>(~(H8_SCI_SSR_TDRE | H8_SCI_SSR_TEND)));
    return;
  }

  H8SCI0LogCyberLeadPacing(runtime);

  // TSR is empty: hardware transfers TDR -> TSR and immediately makes TDR
  // available again. This is the SCI's double-buffered transmit behavior.
  runtime.h8_sci0_tx_shift_byte =
    runtime.h8_internal[UINT16_C(0xFFB3)];
  runtime.h8_sci0_tx_busy = true;
  runtime.h8_sci0_tx_clocks_remaining =
    H8SCI0CyberLeadFrameClocks(runtime);

  ssr = static_cast<u8>(
    (ssr | H8_SCI_SSR_TDRE) &
    static_cast<u8>(~H8_SCI_SSR_TEND));

  // TXI0 is generated when TDRE becomes set.
  if ((scr & H8_SCI_SCR_TIE) != 0)
    runtime.h8_intc.InternalInterrupt(UINT32_C(54));
}
void H8SCI0CyberLeadCompleteTransmit(RuntimeState& runtime)
{
  u8& ssr = runtime.h8_internal[UINT16_C(0xFFB4)];
  const u8 scr = runtime.h8_internal[UINT16_C(0xFFB2)];

  const u8 completed_byte = runtime.h8_sci0_tx_shift_byte;

  // The frame that occupied TSR has now physically left SCI0.
  if ((scr & H8_SCI_SCR_TE) != 0)
    H8JVSTransmitByte(runtime, completed_byte);

  // TDRE clear means firmware filled TDR while the old TSR byte was still
  // shifting. Transfer that queued byte into TSR now and continue.
  if ((scr & H8_SCI_SCR_TE) != 0 &&
      (ssr & H8_SCI_SSR_TDRE) == 0)
  {
    runtime.h8_sci0_tx_shift_byte =
      runtime.h8_internal[UINT16_C(0xFFB3)];
    runtime.h8_sci0_tx_busy = true;
    runtime.h8_sci0_tx_clocks_remaining =
      H8SCI0CyberLeadFrameClocks(runtime);

    ssr = static_cast<u8>(
      (ssr | H8_SCI_SSR_TDRE) &
      static_cast<u8>(~H8_SCI_SSR_TEND));

    if ((scr & H8_SCI_SCR_TIE) != 0)
      runtime.h8_intc.InternalInterrupt(UINT32_C(54)); // TXI0

    return;
  }

  runtime.h8_sci0_tx_busy = false;
  runtime.h8_sci0_tx_shift_byte = UINT8_C(0xFF);
  runtime.h8_sci0_tx_clocks_remaining = 0;

  ssr = static_cast<u8>(ssr | H8_SCI_SSR_TDRE | H8_SCI_SSR_TEND);

  if ((scr & H8_SCI_SCR_TEIE) != 0)
    runtime.h8_intc.InternalInterrupt(UINT32_C(55)); // TEI0
}
void H8SCI0CyberLeadAdvance(RuntimeState& runtime, u32 h8_clocks)
{
  if (!H8SCI0CyberLeadPacing(runtime) || h8_clocks == 0)
    return;

  if (runtime.h8_sci0_tx_busy)
  {
    u32 tx_clocks_left = h8_clocks;

    while (runtime.h8_sci0_tx_busy && tx_clocks_left != 0)
    {
      if (tx_clocks_left < runtime.h8_sci0_tx_clocks_remaining)
      {
        runtime.h8_sci0_tx_clocks_remaining -= tx_clocks_left;
        tx_clocks_left = 0;
        break;
      }

      tx_clocks_left -= runtime.h8_sci0_tx_clocks_remaining;
      H8SCI0CyberLeadCompleteTransmit(runtime);
    }
  }

  // RX timing is wall-clock parallel to TX timing. Cyber Lead normally
  // operates half-duplex through RTS, but keep the accounting correct if
  // firmware transitions directions around a scheduler boundary.
  if (runtime.h8_sci0_rx_busy)
  {
    if (h8_clocks < runtime.h8_sci0_rx_clocks_remaining)
    {
      runtime.h8_sci0_rx_clocks_remaining -= h8_clocks;
    }
    else
    {
      H8SCI0CyberLeadCompleteReceive(runtime);
    }
  }
}
void ServiceH8JVSSCI0Receive(RuntimeState& runtime)
{
  if (!HasH8JVSDevice(runtime) || runtime.h8_jvs_rts)
    return;

  if (H8SCI0CyberLeadPacing(runtime))
  {
    H8SCI0CyberLeadStartReceive(runtime);
    return;
  }

  const u8 scr = runtime.h8_internal[UINT16_C(0xFFB2)];
  u8& ssr = runtime.h8_internal[UINT16_C(0xFFB4)];
  if ((scr & H8_SCI_SCR_RE) == 0 || (ssr & H8_SCI_SSR_RDRF) != 0)
    return;

  JVSState& jvs = runtime.h8_jvs;
  if (!runtime.h8_jvs_bus.HasResponseByte())
    return;

  runtime.h8_internal[UINT16_C(0xFFB5)] = runtime.h8_jvs_bus.ReadResponseByte();
  ssr = static_cast<u8>(ssr | H8_SCI_SSR_RDRF);

  // H8/3002 SCI0 RXI0 is vector 53. The real SCI asserts RXI when RDRF
  // becomes set while RIE is enabled; System 12 JVS firmware uses this
  // interrupt-driven receive path.
  if ((scr & H8_SCI_SCR_RIE) != 0)
    runtime.h8_intc.InternalInterrupt(UINT32_C(53));

  if (runtime.h8_jvs_bus.ResponseComplete() && jvs.initialize_sense_after_response)
  {
    jvs.initialize_sense_after_response = false;
    runtime.h8_jvs_sense = H8_JVS_SENSE_INITIALIZED;
  }
}








void ProcessH8JVSPacket(RuntimeState& runtime)
{
  JVSState& jvs = runtime.h8_jvs;
  const std::span<const u8> packet = runtime.h8_jvs_bus.GetPacket();

  // Temporary Cyber Lead frame-level diagnostic.  Open a short trace window
  // whenever Service or Coin1 changes so we can see the exact JVS request and
  // response bytes that the H8 firmware receives around the problem.
  static bool s_cyberlead_diag_initialized = false;
  static bool s_cyberlead_diag_service_previous = false;
  static bool s_cyberlead_diag_coin_previous = false;
  static bool s_cyberlead_diag_start_previous = false;
  static bool s_cyberlead_diag_button1_previous = false;
  static u32 s_cyberlead_diag_packets_remaining = 0;
  static u64 s_cyberlead_diag_sequence = 0;

  const bool cyberlead_diag_service = ArcadeInput::IsOperatorPressed("Service");
  const bool cyberlead_diag_coin = ArcadeInput::IsDigitalPressed(0, "Coin");
  const bool cyberlead_diag_start = ArcadeInput::IsDigitalPressed(0, "Start");
  const bool cyberlead_diag_button1 = ArcadeInput::IsDigitalPressed(0, "Button1");
  if (jvs.profile == JVSProfile::CyberLead)
  {
    const bool changed =
      !s_cyberlead_diag_initialized ||
      cyberlead_diag_service != s_cyberlead_diag_service_previous ||
      cyberlead_diag_coin != s_cyberlead_diag_coin_previous ||
      cyberlead_diag_start != s_cyberlead_diag_start_previous ||
      cyberlead_diag_button1 != s_cyberlead_diag_button1_previous;

    if (changed)
    {
      s_cyberlead_diag_packets_remaining = 16;
      WARNING_LOG(
        "System12 CyberLead FRAME-DIAG trigger seq={} Service={} Coin1={} Start={} Button1={}",
        s_cyberlead_diag_sequence, cyberlead_diag_service, cyberlead_diag_coin,
        cyberlead_diag_start, cyberlead_diag_button1);
    }

    s_cyberlead_diag_initialized = true;
    s_cyberlead_diag_service_previous = cyberlead_diag_service;
    s_cyberlead_diag_coin_previous = cyberlead_diag_coin;
    s_cyberlead_diag_start_previous = cyberlead_diag_start;
    s_cyberlead_diag_button1_previous = cyberlead_diag_button1;
  }

  std::array<u8, 256> response{};
  u32 response_length = 0;
  bool reset_requested = false;
  bool initialize_sense_after_response = false;

  if (jvs.profile == JVSProfile::NamcoEMIO102Printer)
  {
    const Arcade::JVS::Chain::PacketResult result =
      runtime.h8_jvs_chain.ProcessPacket(packet);
    response = result.response;
    response_length = result.response_length;
    reset_requested = result.reset_requested;
    initialize_sense_after_response = result.initialize_sense_after_response;
  }
  else if (jvs.profile == JVSProfile::CyberLead)
  {
    const Arcade::JVS::NamcoCyberLead::PacketResult result =
      runtime.h8_cyberlead.ProcessPacket(packet);
    response = result.response;
    response_length = result.response_length;
    reset_requested = result.reset_requested;
    initialize_sense_after_response = result.initialize_sense_after_response;

    if (s_cyberlead_diag_packets_remaining > 0)
    {
      const u64 seq = ++s_cyberlead_diag_sequence;
      WARNING_LOG(
        "System12 CyberLead FRAME-DIAG seq={} Service={} Coin1={} Start={} Button1={} RXlen={} TXlen={} reset={} sense_after={}",
        seq, cyberlead_diag_service, cyberlead_diag_coin, cyberlead_diag_start,
        cyberlead_diag_button1, packet.size(), response_length,
        reset_requested, initialize_sense_after_response);

      for (u32 i = 0; i < packet.size(); i++)
        WARNING_LOG("System12 CyberLead FRAME-DIAG seq={} RX[{}]={:02X}", seq, i, packet[i]);

      for (u32 i = 0; i < response_length; i++)
        WARNING_LOG("System12 CyberLead FRAME-DIAG seq={} TX[{}]={:02X}", seq, i, response[i]);

      s_cyberlead_diag_packets_remaining--;
    }
  }
  else
  {
    return;
  }

  if (reset_requested)
  {
    ResetH8JVSProtocol(runtime);
    return;
  }

  if (response_length == 0)
    return;

  jvs.initialize_sense_after_response = initialize_sense_after_response;
  runtime.h8_jvs_bus.QueueResponse(
    std::span<const u8>(response.data(), response_length));
  ServiceH8JVSSCI0Receive(runtime);
}

void H8JVSTransmitByte(RuntimeState& runtime, u8 value)
{
  if (!HasH8JVSDevice(runtime) || !runtime.h8_jvs_rts)
    return;

  if (runtime.h8_jvs_bus.FeedByte(value))
  {
    ProcessH8JVSPacket(runtime);
    runtime.h8_jvs_bus.ClearPacket();
  }
}

u8 H8SCI0SSRRead(RuntimeState& runtime)
{
  const u8 ssr = runtime.h8_internal[UINT16_C(0xFFB4)];
  runtime.h8_sci0_ssr_read = ssr;
  return ssr;
}

void H8SCI0SSRWrite(RuntimeState& runtime, u8 data)
{
  u8& ssr = runtime.h8_internal[UINT16_C(0xFFB4)];
  const u8 scr = runtime.h8_internal[UINT16_C(0xFFB2)];
  const u8 ssr_read = runtime.h8_sci0_ssr_read;

  if ((scr & H8_SCI_SCR_TE) != 0 &&
      (ssr & ssr_read & H8_SCI_SSR_TDRE) != 0 &&
      (data & H8_SCI_SSR_TDRE) == 0)
  {
    ssr = static_cast<u8>(ssr & static_cast<u8>(~(H8_SCI_SSR_TDRE | H8_SCI_SSR_TEND)));

    if (H8SCI0CyberLeadPacing(runtime))
    {
      H8SCI0CyberLeadStartTransmit(runtime);
    }
    else
    {
      H8JVSTransmitByte(runtime, runtime.h8_internal[UINT16_C(0xFFB3)]);
      ssr = static_cast<u8>(ssr | H8_SCI_SSR_TDRE | H8_SCI_SSR_TEND);

      // H8/3002 SCI0 TXI0 is vector 54. The firmware can switch from polling
      // TDRE during early JVS reset traffic to interrupt-driven transmission
      // for subsequent packets. Raise TXI when TDRE becomes set and TIE is on.
      if ((scr & H8_SCI_SCR_TIE) != 0)
        runtime.h8_intc.InternalInterrupt(UINT32_C(54));
    }
  }

  const u8 clearable = static_cast<u8>(H8_SCI_SSR_RDRF | H8_SCI_SSR_ORER |
                                       H8_SCI_SSR_FER | H8_SCI_SSR_PER);
  const u8 clear_mask = static_cast<u8>(ssr_read & clearable & static_cast<u8>(~data));
  ssr = static_cast<u8>(ssr & static_cast<u8>(~clear_mask));
  runtime.h8_sci0_ssr_read = static_cast<u8>(runtime.h8_sci0_ssr_read & ssr);

  ServiceH8JVSSCI0Receive(runtime);
}

void UpdateH8System12SerialSelect(RuntimeState& runtime)
{
  runtime.h8_sub_porta =
    H8PortOutput(runtime, UINT16_C(0xFFD1), UINT16_C(0xFFD3));

  const u8 portb_output =
    H8PortOutput(runtime, UINT16_C(0xFFD4), UINT16_C(0xFFD6));

  runtime.h8_sub_portb =
    static_cast<u8>((runtime.h8_sub_portb & UINT8_C(0x80)) |
                    (portb_output & UINT8_C(0x7F)));

  const bool old_jvs_rts = runtime.h8_jvs_rts;
  runtime.h8_jvs_rts = (runtime.h8_sub_portb & UINT8_C(0x01)) != 0;
  if (old_jvs_rts && !runtime.h8_jvs_rts)
    ServiceH8JVSSCI0Receive(runtime);

  const bool serial_enable = (runtime.h8_sub_portb & UINT8_C(0x20)) != 0;
  H8RTCSetCE(runtime, serial_enable && ((runtime.h8_sub_porta & UINT8_C(0x01)) != 0));
  H8SettingsSetCE(runtime, serial_enable && ((runtime.h8_sub_porta & UINT8_C(0x01)) == 0));
}

u8 H8SCI1SSRRead(RuntimeState& runtime)
{
  const u8 ssr = runtime.h8_internal[UINT16_C(0xFFBC)];
  runtime.h8_sci1_ssr_read = ssr;
  return ssr;
}

bool H8SCI1ReceiveOnlyEligible(const RuntimeState& runtime)
{
  const u8 smr = runtime.h8_internal[UINT16_C(0xFFB8)];
  const u8 scr = runtime.h8_internal[UINT16_C(0xFFBA)];
  const u8 ssr = runtime.h8_internal[UINT16_C(0xFFBC)];

  return (smr & H8_SCI_SMR_CA) != 0 &&
         (scr & H8_SCI_SCR_RE) != 0 &&
         (scr & H8_SCI_SCR_TE) == 0 &&
         (ssr & static_cast<u8>(H8_SCI_SSR_ORER | H8_SCI_SSR_FER | H8_SCI_SSR_PER)) == 0;
}

u32 H8SCI1SynchronousByteClocks(const RuntimeState& runtime)
{
  const u8 smr = runtime.h8_internal[UINT16_C(0xFFB8)];
  const u8 brr = runtime.h8_internal[UINT16_C(0xFFB9)];
  const u32 divider = (UINT32_C(2) << (2 * (smr & H8_SCI_SMR_CKS))) *
                      (static_cast<u32>(brr) + 1);

  // MAME H8 SCI uses two synchronous clock events per data bit.
  return divider * 16;
}

void H8SCI1TryStartReceiveOnly(RuntimeState& runtime)
{
  if (runtime.h8_sci1_rx_busy || !H8SCI1ReceiveOnlyEligible(runtime))
    return;

  runtime.h8_sci1_rx_busy = true;
  runtime.h8_sci1_rx_clocks_remaining = H8SCI1SynchronousByteClocks(runtime);
}

void H8SCI1CompleteReceiveOnly(RuntimeState& runtime)
{
  runtime.h8_sci1_rx_busy = false;
  runtime.h8_sci1_rx_clocks_remaining = 0;

  u8& ssr = runtime.h8_internal[UINT16_C(0xFFBC)];
  u8 rx = 0;

  for (u32 bit = 0; bit < 8; bit++)
  {
    // System 12 inverts SCI1 CLK on the RTC4543 wire. The RTC updates
    // data on its rising edge; the H8 samples on the following SCI edge.
    H8SettingsSetClock(runtime, false);
    H8RTCSetClock(runtime, true);
    H8SettingsSetClock(runtime, true);
    H8RTCSetClock(runtime, false);

    rx >>= 1;
    if (runtime.h8_rtc_data)
      rx = static_cast<u8>(rx | UINT8_C(0x80));
  }

  if ((ssr & H8_SCI_SSR_RDRF) != 0)
  {
    ssr = static_cast<u8>(ssr | H8_SCI_SSR_ORER);
  }
  else
  {
    runtime.h8_internal[UINT16_C(0xFFBD)] = rx;
    ssr = static_cast<u8>(ssr | H8_SCI_SSR_RDRF);
  }

  // MAME immediately starts another receive while synchronous RE-only
  // remains enabled. If software fails to service RDRF before the next
  // byte completes, the following completion raises ORER.
  H8SCI1TryStartReceiveOnly(runtime);
}

void H8SCI1Advance(RuntimeState& runtime, u32 h8_clocks)
{
  if (!runtime.h8_sci1_rx_busy || h8_clocks == 0)
    return;

  while (runtime.h8_sci1_rx_busy && h8_clocks != 0)
  {
    if (h8_clocks < runtime.h8_sci1_rx_clocks_remaining)
    {
      runtime.h8_sci1_rx_clocks_remaining -= h8_clocks;
      return;
    }

    h8_clocks -= runtime.h8_sci1_rx_clocks_remaining;
    H8SCI1CompleteReceiveOnly(runtime);
  }
}

void H8SCI1SynchronousTransfer(RuntimeState& runtime)
{
  const u8 smr = runtime.h8_internal[UINT16_C(0xFFB8)];
  const u8 scr = runtime.h8_internal[UINT16_C(0xFFBA)];

  if ((smr & H8_SCI_SMR_CA) == 0 || (scr & H8_SCI_SCR_TE) == 0)
    return;

  u8& ssr = runtime.h8_internal[UINT16_C(0xFFBC)];
  u8 tx = runtime.h8_internal[UINT16_C(0xFFBB)];
  u8 rx = 0;

  // MAME's H8 SCI marks TDR empty when TDR is copied into the transmit shift
  // register. System 12 SCI1 uses synchronous, LSB-first transfers.
  ssr = static_cast<u8>(ssr | H8_SCI_SSR_TDRE);

  const bool receive =
    ((scr & H8_SCI_SCR_RE) != 0) &&
    ((ssr & static_cast<u8>(H8_SCI_SSR_ORER | H8_SCI_SSR_FER | H8_SCI_SSR_PER)) == 0);

  for (u32 bit = 0; bit < 8; bit++)
  {
    const bool tx_bit = (tx & UINT8_C(1)) != 0;
    tx >>= 1;

    runtime.h8_settings_data = tx_bit;

    // SCI1 CLK low:
    //   settings sees falling edge;
    //   RTC4543 sees rising edge because System 12 inverts this clock wire.
    H8SettingsSetClock(runtime, false);
    H8RTCSetClock(runtime, true);

    // SCI1 CLK high:
    //   settings captures TX on rising edge;
    //   RTC sees falling edge;
    //   H8 samples RX after the rising SCI clock edge.
    H8SettingsSetClock(runtime, true);
    H8RTCSetClock(runtime, false);

    if (receive)
    {
      rx >>= 1;
      if (runtime.h8_rtc_data)
        rx = static_cast<u8>(rx | UINT8_C(0x80));
    }
  }

  if (receive)
  {
    if ((ssr & H8_SCI_SSR_RDRF) != 0)
    {
      ssr = static_cast<u8>(ssr | H8_SCI_SSR_ORER);
    }
    else
    {
      runtime.h8_internal[UINT16_C(0xFFBD)] = rx;
      ssr = static_cast<u8>(ssr | H8_SCI_SSR_RDRF);
    }
  }

  ssr = static_cast<u8>(ssr | H8_SCI_SSR_TEND);
  runtime.h8_sci1_ssr_read = static_cast<u8>(runtime.h8_sci1_ssr_read & ssr);
}

void H8SCI1SSRWrite(RuntimeState& runtime, u8 data)
{
  u8& ssr = runtime.h8_internal[UINT16_C(0xFFBC)];
  const u8 scr = runtime.h8_internal[UINT16_C(0xFFBA)];
  const u8 ssr_read = runtime.h8_sci1_ssr_read;

  // Match MAME H8 SCI SSR write semantics. Status flags can only be cleared
  // after software has observed them. Clearing TDRE while TE is enabled
  // hands the current TDR byte to the transmitter.
  if ((scr & H8_SCI_SCR_TE) != 0 &&
      (ssr & ssr_read & H8_SCI_SSR_TDRE) != 0 &&
      (data & H8_SCI_SSR_TDRE) == 0)
  {
    ssr = static_cast<u8>(ssr & static_cast<u8>(~(H8_SCI_SSR_TDRE | H8_SCI_SSR_TEND)));
  }

  const u8 preserve_mask =
    static_cast<u8>(static_cast<u8>(~ssr_read) |
                    data |
                    H8_SCI_SSR_TDRE |
                    H8_SCI_SSR_TEND |
                    H8_SCI_SSR_MPB);

  ssr = static_cast<u8>(
    (ssr & preserve_mask & static_cast<u8>(~H8_SCI_SSR_MPBT)) |
    (data & H8_SCI_SSR_MPBT));

  runtime.h8_sci1_ssr_read =
    static_cast<u8>(runtime.h8_sci1_ssr_read & ssr);

  if ((scr & H8_SCI_SCR_TE) != 0 && (ssr & H8_SCI_SSR_TDRE) == 0)
    H8SCI1SynchronousTransfer(runtime);

  H8SCI1TryStartReceiveOnly(runtime);
}

// System12 H8 SCI1 synchronous transfer
u8 H83002ReadPortExact(const RuntimeState& runtime, u16 ddr_reg, u16 dr_reg, u8 external, u8 mask)
{
  const u8 ddr = runtime.h8_internal[ddr_reg];
  const u8 dr = runtime.h8_internal[dr_reg];

  // MAME h8_port_device::port_r(): masked pins read high; output pins return
  // DR; input pins return the external board value.
  return static_cast<u8>(
    mask |
    (dr & ddr) |
    (external & static_cast<u8>(~ddr)));
}

u8 ReadH8JAMMAInput(u32 offset)
{
  // System 12 exposes four active-low JAMMA compatibility bytes to the H8.
  // This matches the motherboard input conversion used when no external JVS
  // I/O board is present. Buttons 7/8 are not part of ArcadeDuck's current
  // standard arcade layout and therefore remain inactive/high.
  const auto player_byte = [](u32 port) {
    u8 result = UINT8_C(0xFF);
    const auto clear_if_pressed = [&result, port](u8 mask, const char* key) {
      if (ArcadeInput::IsDigitalPressed(port, key))
        result = static_cast<u8>(result & static_cast<u8>(~mask));
    };

    clear_if_pressed(UINT8_C(0x80), "Start");
    clear_if_pressed(UINT8_C(0x40), "Button3");
    clear_if_pressed(UINT8_C(0x20), "Button2");
    clear_if_pressed(UINT8_C(0x10), "Button1");
    clear_if_pressed(UINT8_C(0x08), "Up");
    clear_if_pressed(UINT8_C(0x04), "Down");
    clear_if_pressed(UINT8_C(0x02), "Left");
    clear_if_pressed(UINT8_C(0x01), "Right");
    return result;
  };

  u8 value = UINT8_C(0xFF);
  const auto clear_if_pressed = [&value](u8 mask, bool pressed) {
    if (pressed)
      value = static_cast<u8>(value & static_cast<u8>(~mask));
  };

  switch (offset & UINT32_C(3))
  {
    case 0:
      // P2 START/B3/B2/B1/UP/DOWN/LEFT/RIGHT.
      return player_byte(1);

    case 1:
      // P1 START/B3/B2/B1/UP/DOWN/LEFT/RIGHT.
      return player_byte(0);

    case 2:
      // P1 SERVICE/TEST/COIN1/COIN2/P1 B7/P2 B7/P1 B8/P2 B8.
      clear_if_pressed(UINT8_C(0x80), ArcadeInput::IsOperatorPressed("Service"));
      clear_if_pressed(UINT8_C(0x40), ArcadeInput::IsOperatorPressed("Test"));
      clear_if_pressed(UINT8_C(0x20), ArcadeInput::IsDigitalPressed(0, "Coin"));
      clear_if_pressed(UINT8_C(0x10), ArcadeInput::IsDigitalPressed(1, "Coin"));
      return value;

    case 3:
      // P2 B6/B5/B4/unused/P1 B6/B5/B4/unused.
      clear_if_pressed(UINT8_C(0x80), ArcadeInput::IsDigitalPressed(1, "Button6"));
      clear_if_pressed(UINT8_C(0x40), ArcadeInput::IsDigitalPressed(1, "Button5"));
      clear_if_pressed(UINT8_C(0x20), ArcadeInput::IsDigitalPressed(1, "Button4"));
      clear_if_pressed(UINT8_C(0x08), ArcadeInput::IsDigitalPressed(0, "Button6"));
      clear_if_pressed(UINT8_C(0x04), ArcadeInput::IsDigitalPressed(0, "Button5"));
      clear_if_pressed(UINT8_C(0x02), ArcadeInput::IsDigitalPressed(0, "Button4"));
      return value;

    default:
      return UINT8_C(0xFF);
  }
}

h8_bool H8BusRead(void* opaque, unsigned address, h8_byte_t* value)
{
  RuntimeState* const runtime = static_cast<RuntimeState*>(opaque);
  if (!runtime || !value)
    return FALSE;

  address &= UINT32_C(0x00FFFFFF);

  if (address >= H8_ROM_BASE && address <= H8_ROM_END)
  {
    value->u = runtime->content.sub_program[address - H8_ROM_BASE];
    return TRUE;
  }

  if (address >= H8_SHARED_BASE && address <= H8_SHARED_END)
  {
    const u32 offset = static_cast<u32>(address - H8_SHARED_BASE);
    const u32 psx_index = offset ^ UINT32_C(1);
    value->u = runtime->shared_ram[psx_index];

    // Attack Pla Rail coin-mailbox diagnostic only. The PSX writes the
    // decrement request at shared offset 0x3300 and the H8 publishes the
    // current JVS coin count at 0x32C0. Log H8 firmware reads while a coin
    // transaction is active so we can measure where the observed ~56 ms
    // latency actually begins.
    if (runtime->content.set_name == "aplarail")
    {
      const bool mailbox_nonzero =
        runtime->shared_ram[UINT32_C(0x3300)] != 0 ||
        runtime->shared_ram[UINT32_C(0x3301)] != 0;
      const bool coin_count_nonzero =
        runtime->shared_ram[UINT32_C(0x32C0)] != 0 ||
        runtime->shared_ram[UINT32_C(0x32C1)] != 0;
      const bool coin_pressed = ArcadeInput::IsDigitalPressed(0, "Coin");

      const bool interesting_offset =
        (psx_index >= UINT32_C(0x32C0) && psx_index <= UINT32_C(0x32C3)) ||
        (psx_index >= UINT32_C(0x3300) && psx_index <= UINT32_C(0x3303));

      if (interesting_offset &&
          (mailbox_nonzero || coin_count_nonzero || coin_pressed))
      {
        static u32 s_aplarail_h8_shared_read_count = 0;
        if (s_aplarail_h8_shared_read_count < UINT32_C(1024))
        {
          WARNING_LOG(
            "System12 AplaRail H8-SHARED-R #{} scheduler={} h8pc={:06X} "
            "h8off={:04X} psxindex={:04X} value={:02X} "
            "coin_pressed={} coin_count={:02X}{:02X} mailbox={:02X}{:02X}",
            ++s_aplarail_h8_shared_read_count,
            runtime->h8_scheduler_count,
            runtime->h8_cpu ? static_cast<u32>(runtime->h8_cpu->cpu.pc) : 0u,
            offset, psx_index, value->u,
            coin_pressed,
            runtime->shared_ram[UINT32_C(0x32C1)],
            runtime->shared_ram[UINT32_C(0x32C0)],
            runtime->shared_ram[UINT32_C(0x3301)],
            runtime->shared_ram[UINT32_C(0x3300)]);
        }
      }
    }

    return TRUE;
  }

  if (address >= H8_C352_BASE && address <= H8_C352_END)
  {

    value->u = runtime->c352.ReadH8Byte(static_cast<u32>(address - H8_C352_BASE));
    return TRUE;
  }

  if (address >= H8_JAMMA_BASE && address <= H8_JAMMA_END)
  {
    value->u = ReadH8JAMMAInput(static_cast<u32>(address - H8_JAMMA_BASE));
    return TRUE;
  }

  if ((address >= H8_WAIT0_BASE && address <= H8_WAIT0_END) ||
      (address >= H8_WAIT1_BASE && address <= H8_WAIT1_END))
  {
    value->u = UINT8_C(0xFF);
    return TRUE;
  }

  if (address >= H8_INTERNAL_BASE)
  {
    const u16 reg = static_cast<u16>(address);
    if (runtime->h8_intc.Handles(reg))
    {
      value->u = runtime->h8_intc.Read(reg);
      return TRUE;
    }
    u8 peripheral_value = 0;
    if (runtime->h8_peripherals.Read(reg, peripheral_value))
    {
      value->u = peripheral_value;
      return TRUE;
    }

    // H83002 DDR registers are write-only in the memory map and read as FF.
    switch (reg)
    {
      case UINT16_C(0xFFC5):
      case UINT16_C(0xFFC9):
      case UINT16_C(0xFFCD):
      case UINT16_C(0xFFD0):
      case UINT16_C(0xFFD1):
      case UINT16_C(0xFFD4):
        value->u = UINT8_C(0xFF);
        return TRUE;

      case UINT16_C(0xFFC7): // Port 4
        value->u = H83002ReadPortExact(*runtime, UINT16_C(0xFFC5), UINT16_C(0xFFC7),
                                       UINT8_C(0xFF), UINT8_C(0x00));
        return TRUE;

      case UINT16_C(0xFFD2): // Port 9
        value->u = H83002ReadPortExact(*runtime, UINT16_C(0xFFD0), UINT16_C(0xFFD2),
                                       UINT8_C(0xFF), UINT8_C(0xC0));
        return TRUE;

      case UINT16_C(0xFFD3): // Port A
        value->u = H83002ReadPortExact(*runtime, UINT16_C(0xFFD1), UINT16_C(0xFFD3),
                                       UINT8_C(0xFF), UINT8_C(0x00));
        return TRUE;

      case UINT16_C(0xFFD6): // Port B, bit 7 carries CRTC VBlank
      {
        const u8 external = runtime->h8_vblank ? UINT8_C(0xFF) : UINT8_C(0x7F);
        value->u = H83002ReadPortExact(
          *runtime, UINT16_C(0xFFD4), UINT16_C(0xFFD6), external, UINT8_C(0x00));
        return TRUE;
      }

      case UINT16_C(0xFFDA): // Port 4 PCR
        value->u = runtime->h8_internal[UINT16_C(0xFFDA)];
        return TRUE;

      default:
        break;
    }

    if (runtime->h8_timer16.Handles(reg))
    {
      value->u = runtime->h8_timer16.Read(reg);
      return TRUE;
    }

    switch (reg)
    {
      case UINT16_C(0xFFB4): // SCI0 SSR / JVS
        value->u = H8SCI0SSRRead(*runtime);
        return TRUE;

      case UINT16_C(0xFFBC): // SCI1 SSR
        value->u = H8SCI1SSRRead(*runtime);
        return TRUE;

      case UINT16_C(0xFFB5): // SCI0 RDR / JVS
        value->u = runtime->h8_internal[UINT16_C(0xFFB5)];
        return TRUE;

      case UINT16_C(0xFFBD): // SCI1 RDR
        value->u = runtime->h8_internal[UINT16_C(0xFFBD)];
        return TRUE;

      case UINT16_C(0xFFC7): // Port 4
        value->u = ReadH8Port(*runtime, UINT16_C(0xFFC5), UINT16_C(0xFFC7), UINT8_C(0xFF), UINT8_C(0x00));
        return TRUE;

      case UINT16_C(0xFFCB): // Port 6 / JVS initialized sense
      {
        const u8 external =
          static_cast<u8>(((runtime->h8_jvs_sense != H8_JVS_SENSE_INITIALIZED) ? UINT8_C(0x02) : UINT8_C(0x00)) |
                          UINT8_C(0xFD));
        value->u = H83002ReadPortExact(
          *runtime, UINT16_C(0xFFC9), UINT16_C(0xFFCB), external, UINT8_C(0x80));
        return TRUE;
      }

      case UINT16_C(0xFFCE): // Port 7 / DIP switches
        value->u = UINT8_C(0xFF);
        return TRUE;

      case UINT16_C(0xFFCF): // Port 8 / JVS physical-presence sense
      {
        const u8 external =
          static_cast<u8>(((runtime->h8_jvs_sense != H8_JVS_SENSE_NONE) ? UINT8_C(0x10) : UINT8_C(0x00)) |
                          UINT8_C(0xEF));
        value->u = H83002ReadPortExact(
          *runtime, UINT16_C(0xFFCD), UINT16_C(0xFFCF), external, UINT8_C(0xE0));
        return TRUE;
      }

      case UINT16_C(0xFFD2): // Port 9
        value->u = ReadH8Port(*runtime, UINT16_C(0xFFD0), UINT16_C(0xFFD2), UINT8_C(0xFF), UINT8_C(0xC0));
        return TRUE;

      case UINT16_C(0xFFD3): // Port A
        value->u = ReadH8Port(*runtime, UINT16_C(0xFFD1), UINT16_C(0xFFD3), UINT8_C(0xFF), UINT8_C(0x00));
        return TRUE;

      case UINT16_C(0xFFD6): // Port B, bit 7 carries vblank
      {
        const u8 external = runtime->h8_vblank ? UINT8_C(0xFF) : UINT8_C(0x7F);
        value->u = ReadH8Port(*runtime, UINT16_C(0xFFD4), UINT16_C(0xFFD6), external, UINT8_C(0x00));
        return TRUE;
      }

      case UINT16_C(0xFFE0):
      case UINT16_C(0xFFE1):
      case UINT16_C(0xFFE2):
      case UINT16_C(0xFFE3):
      case UINT16_C(0xFFE4):
      case UINT16_C(0xFFE5):
      case UINT16_C(0xFFE6):
      case UINT16_C(0xFFE7):
        value->u = UINT8_C(0xFF);
        return TRUE;

      case UINT16_C(0xFFE8): // ADC status
        value->u = UINT8_C(0x80);
        return TRUE;

      default:
        value->u = runtime->h8_internal[reg];
        return TRUE;
    }
  }

  return FALSE;
}

h8_bool H8BusWrite(void* opaque, unsigned address, h8_byte_t value)
{
  RuntimeState* const runtime = static_cast<RuntimeState*>(opaque);
  if (!runtime)
    return FALSE;

  address &= UINT32_C(0x00FFFFFF);

  if (address >= H8_SHARED_BASE && address <= H8_SHARED_END)
  {
    const u32 offset = static_cast<u32>(address - H8_SHARED_BASE);
    static bool s_cyberlead_shared_diag_initialized = false;
    static bool s_cyberlead_shared_diag_service_previous = false;
    static bool s_cyberlead_shared_diag_coin_previous = false;
    static bool s_cyberlead_shared_diag_start_previous = false;
    static bool s_cyberlead_shared_diag_button1_previous = false;
    static u32 s_cyberlead_shared_diag_changes_remaining = 0;
    static u64 s_cyberlead_shared_diag_window = 0;

    const bool cyberlead_shared_diag_active =
      runtime->h8_jvs.profile == JVSProfile::CyberLead;
    const bool cyberlead_shared_diag_service =
      cyberlead_shared_diag_active && ArcadeInput::IsOperatorPressed("Service");
    const bool cyberlead_shared_diag_coin =
      cyberlead_shared_diag_active && ArcadeInput::IsDigitalPressed(0, "Coin");
    const bool cyberlead_shared_diag_start =
      cyberlead_shared_diag_active && ArcadeInput::IsDigitalPressed(0, "Start");
    const bool cyberlead_shared_diag_button1 =
      cyberlead_shared_diag_active && ArcadeInput::IsDigitalPressed(0, "Button1");

    if (cyberlead_shared_diag_active)
    {
      if (!s_cyberlead_shared_diag_initialized)
      {
        s_cyberlead_shared_diag_initialized = true;
        s_cyberlead_shared_diag_service_previous = cyberlead_shared_diag_service;
        s_cyberlead_shared_diag_coin_previous = cyberlead_shared_diag_coin;
        s_cyberlead_shared_diag_start_previous = cyberlead_shared_diag_start;
        s_cyberlead_shared_diag_button1_previous = cyberlead_shared_diag_button1;
      }
      else if (cyberlead_shared_diag_service != s_cyberlead_shared_diag_service_previous ||
               cyberlead_shared_diag_coin != s_cyberlead_shared_diag_coin_previous ||
               cyberlead_shared_diag_start != s_cyberlead_shared_diag_start_previous ||
               cyberlead_shared_diag_button1 != s_cyberlead_shared_diag_button1_previous)
      {
        s_cyberlead_shared_diag_window++;
        s_cyberlead_shared_diag_changes_remaining = 128;
        WARNING_LOG(
          "System12 CyberLead SHARED-DIAG trigger window={} Service={} Coin1={} Start={} Button1={} h8pc={:06X}",
          s_cyberlead_shared_diag_window,
          cyberlead_shared_diag_service,
          cyberlead_shared_diag_coin,
          cyberlead_shared_diag_start,
          cyberlead_shared_diag_button1,
          runtime->h8_cpu ? static_cast<u32>(runtime->h8_cpu->cpu.pc) : 0u);

        s_cyberlead_shared_diag_service_previous = cyberlead_shared_diag_service;
        s_cyberlead_shared_diag_coin_previous = cyberlead_shared_diag_coin;
        s_cyberlead_shared_diag_start_previous = cyberlead_shared_diag_start;
        s_cyberlead_shared_diag_button1_previous = cyberlead_shared_diag_button1;
      }
    }

    const u32 shared_storage_index = offset ^ UINT32_C(1);
    const u8 shared_previous = runtime->shared_ram[shared_storage_index];
    runtime->shared_ram[shared_storage_index] = value.u;

    if (cyberlead_shared_diag_active &&
        s_cyberlead_shared_diag_changes_remaining > 0 &&
        offset < PSX_SHARED_WINDOW_SIZE &&
        shared_previous != value.u)
    {
      WARNING_LOG(
        "System12 CyberLead SHARED-DIAG window={} h8pc={:06X} h8off={:04X} psxindex={:04X} {:02X}->{:02X}",
        s_cyberlead_shared_diag_window,
        runtime->h8_cpu ? static_cast<u32>(runtime->h8_cpu->cpu.pc) : 0u,
        offset, shared_storage_index, shared_previous, value.u);
      s_cyberlead_shared_diag_changes_remaining--;
    }

    return TRUE;
  }

  if (address >= H8_C352_BASE && address <= H8_C352_END)
  {

    runtime->c352.WriteH8Byte(static_cast<u32>(address - H8_C352_BASE), value.u);

    return TRUE;
  }

  if ((address >= H8_WAIT0_BASE && address <= H8_WAIT0_END) ||
      (address >= H8_WAIT1_BASE && address <= H8_WAIT1_END))
  {
    return TRUE;
  }

  if (address >= H8_INTERNAL_BASE)
  {
    const u16 reg = static_cast<u16>(address);
    if (runtime->h8_intc.Handles(reg))
    {
      runtime->h8_intc.Write(reg, value.u);
      runtime->h8_internal[UINT16_C(0xFFF4)] = runtime->h8_intc.GetISCR();
      runtime->h8_internal[UINT16_C(0xFFF5)] = runtime->h8_intc.GetIER();
      runtime->h8_internal[UINT16_C(0xFFF6)] = runtime->h8_intc.GetISR();
      runtime->h8_internal[UINT16_C(0xFFF8)] = static_cast<u8>(runtime->h8_intc.GetICR());
      runtime->h8_internal[UINT16_C(0xFFF9)] = static_cast<u8>(runtime->h8_intc.GetICR() >> 8);
      return TRUE;
    }
    if (runtime->h8_peripherals.Write(reg, value.u))
    {
      // Mirror SYSCR/ICR into the legacy internal byte array because exception-entry
      // code and firmware-visible register state inspect it.
      if (reg == UINT16_C(0xFFF2) || reg == UINT16_C(0xFFF8) || reg == UINT16_C(0xFFF9))
        runtime->h8_internal[reg] = value.u;
      return TRUE;
    }

    if (runtime->h8_timer16.Handles(reg))
    {
      runtime->h8_timer16.Write(reg, value.u);
      return TRUE;
    }

    switch (reg)
    {
      case UINT16_C(0xFFB0): // SCI0 SMR / JVS
      case UINT16_C(0xFFB1): // SCI0 BRR / JVS
      case UINT16_C(0xFFB3): // SCI0 TDR / JVS
        runtime->h8_internal[reg] = value.u;
        return TRUE;

      case UINT16_C(0xFFB2): // SCI0 SCR / JVS
      {
        const u8 old_scr = runtime->h8_internal[reg];
        const u8 new_scr = value.u;
        const u8 delta = static_cast<u8>(old_scr ^ new_scr);
        runtime->h8_internal[reg] = new_scr;

        if (H8SCI0CyberLeadPacing(*runtime))
        {
          if ((new_scr & H8_SCI_SCR_TE) == 0)
          {
            runtime->h8_sci0_tx_busy = false;
            runtime->h8_sci0_tx_clocks_remaining = 0;
          }

          if ((new_scr & H8_SCI_SCR_RE) == 0)
          {
            runtime->h8_sci0_rx_busy = false;
            runtime->h8_sci0_rx_clocks_remaining = 0;
          }
        }

        const u8 ssr = runtime->h8_internal[UINT16_C(0xFFB4)];

        // Match H8 SCI interrupt-on-enable behavior. Firmware may enable an
        // interrupt after its status condition is already true; the real SCI
        // immediately presents the corresponding interrupt in that case.
        if ((delta & H8_SCI_SCR_TIE) != 0 &&
            (new_scr & H8_SCI_SCR_TIE) != 0 &&
            (ssr & H8_SCI_SSR_TDRE) != 0)
        {
          runtime->h8_intc.InternalInterrupt(UINT32_C(54)); // TXI0
        }

        if ((delta & H8_SCI_SCR_RIE) != 0 &&
            (new_scr & H8_SCI_SCR_RIE) != 0)
        {
          if ((ssr & H8_SCI_SSR_RDRF) != 0)
            runtime->h8_intc.InternalInterrupt(UINT32_C(53)); // RXI0

          if ((ssr & static_cast<u8>(H8_SCI_SSR_ORER | H8_SCI_SSR_FER | H8_SCI_SSR_PER)) != 0)
            runtime->h8_intc.InternalInterrupt(UINT32_C(52)); // ERI0
        }

        if ((delta & H8_SCI_SCR_TEIE) != 0 &&
            (new_scr & H8_SCI_SCR_TEIE) != 0 &&
            (ssr & H8_SCI_SSR_TEND) != 0)
        {
          runtime->h8_intc.InternalInterrupt(UINT32_C(55)); // TEI0
        }

        ServiceH8JVSSCI0Receive(*runtime);
        return TRUE;
      }

      case UINT16_C(0xFFB4): // SCI0 SSR / JVS
        H8SCI0SSRWrite(*runtime, value.u);
        return TRUE;

      case UINT16_C(0xFFB5): // SCI0 RDR is read-only
        return TRUE;

      case UINT16_C(0xFFB8): // SCI1 SMR
        runtime->h8_internal[reg] = value.u;
        if ((value.u & H8_SCI_SMR_CA) == 0)
        {
          runtime->h8_sci1_rx_busy = false;
          runtime->h8_sci1_rx_clocks_remaining = 0;
        }
        H8SCI1TryStartReceiveOnly(*runtime);
        return TRUE;

      case UINT16_C(0xFFB9): // SCI1 BRR
        runtime->h8_internal[reg] = value.u;
        return TRUE;

      case UINT16_C(0xFFBA): // SCI1 SCR
        runtime->h8_internal[reg] = value.u;
        if ((value.u & H8_SCI_SCR_RE) == 0 || (value.u & H8_SCI_SCR_TE) != 0)
        {
          runtime->h8_sci1_rx_busy = false;
          runtime->h8_sci1_rx_clocks_remaining = 0;
        }
        H8SCI1TryStartReceiveOnly(*runtime);
        return TRUE;

      case UINT16_C(0xFFBB): // SCI1 TDR
        runtime->h8_internal[reg] = value.u;
        return TRUE;

      case UINT16_C(0xFFBC): // SCI1 SSR
        H8SCI1SSRWrite(*runtime, value.u);
        return TRUE;

      case UINT16_C(0xFFBD): // SCI1 RDR is read-only
        return TRUE;

      default:
        break;
    }

    runtime->h8_internal[reg] = value.u;

    if (reg == UINT16_C(0xFFD1) || reg == UINT16_C(0xFFD3) ||
        reg == UINT16_C(0xFFD4) || reg == UINT16_C(0xFFD6))
    {
      UpdateH8System12SerialSelect(*runtime);
    }

    return TRUE;
  }

  return FALSE;
}

void H8SetExternalIRQLine(RuntimeState& runtime, u32 irq, bool asserted)
{
  runtime.h8_intc.SetInput(irq, asserted);

  // Keep the legacy mirrors coherent for firmware register snapshots and
  // exception-entry state. Reads/writes are routed through h8_intc below.
  runtime.h8_internal[UINT16_C(0xFFF4)] = runtime.h8_intc.GetISCR();
  runtime.h8_internal[UINT16_C(0xFFF5)] = runtime.h8_intc.GetIER();
  runtime.h8_internal[UINT16_C(0xFFF6)] = runtime.h8_intc.GetISR();
  runtime.h8_internal[UINT16_C(0xFFF8)] = static_cast<u8>(runtime.h8_intc.GetICR());
  runtime.h8_internal[UINT16_C(0xFFF9)] = static_cast<u8>(runtime.h8_intc.GetICR() >> 8);
}

void H8CollectInterruptSources(RuntimeState& runtime)
{
  // External IRQ0-7 are already represented directly inside h8_intc.
  // Drain peripheral-local event latches into the single H8H INTC pending set.
  while (const std::optional<u32> vector = runtime.h8_timer16.TakePendingInterrupt())
    runtime.h8_intc.InternalInterrupt(*vector);

  while (const std::optional<u32> vector = runtime.h8_peripherals.TakePendingInterrupt())
    runtime.h8_intc.InternalInterrupt(*vector);
}

void H8AdvanceClockedDevices(RuntimeState& runtime, u32 h8_clocks)
{
  if (h8_clocks == 0)
    return;

  runtime.h8_timer16.Advance(h8_clocks);
  runtime.h8_peripherals.Advance(h8_clocks);
  H8SCI0CyberLeadAdvance(runtime, h8_clocks);
  H8SCI1Advance(runtime, h8_clocks);
  runtime.h8_cyberlead.AdvanceLED(h8_clocks, H8_CLOCK_HZ);
  runtime.c352.AdvanceH8Clocks(h8_clocks, std::span<const u8>(runtime.content.c352_samples));
}

u32 H8ServiceInterrupts(RuntimeState& runtime)
{
  if (!runtime.h8_cpu)
    return 0;

  h8_system_t& h8 = *runtime.h8_cpu;
  const std::optional<u32> vector = runtime.h8_intc.PeekNext(
    h8.cpu.ccr.flags.i != 0, h8.cpu.ccr.flags.ui != 0, runtime.h8_peripherals.SYSCR());
  if (!vector)
    return 0;


  const u32 interrupted_pc = static_cast<u32>(h8.cpu.pc);

  h8.cycles = 0;
  if (!h8_interrupt(&h8, *vector))
    return 0;

  if (runtime.content.set_name == "aplarail")
  {
    runtime.h8_aplarail_last_irq_vector = *vector;
    runtime.h8_aplarail_last_irq_from_pc = interrupted_pc;
    runtime.h8_aplarail_last_irq_to_pc = static_cast<u32>(h8.cpu.pc);
    runtime.h8_aplarail_last_irq_scheduler = runtime.h8_scheduler_count;

    if (*vector == UINT32_C(13))
      runtime.h8_aplarail_irq13_count++;
    else if (*vector == UINT32_C(26))
      runtime.h8_aplarail_irq26_count++;
    else if (*vector == UINT32_C(30))
      runtime.h8_aplarail_irq30_count++;

    const u16 mailbox =
      static_cast<u16>(ReadBytes(std::span<const u8>(runtime.shared_ram), 2, UINT32_C(0x3300)));

    if (mailbox != 0 &&
        (*vector == UINT32_C(13) || *vector == UINT32_C(26) || *vector == UINT32_C(30)) &&
        runtime.h8_aplarail_irq_trace_count < UINT32_C(1024))
    {
      runtime.h8_aplarail_irq_trace_count++;

      const auto timer_word = [&runtime](u16 reg) -> u16 {
        return static_cast<u16>(
          (static_cast<u16>(runtime.h8_timer16.Read(reg)) << 8) |
          static_cast<u16>(runtime.h8_timer16.Read(static_cast<u16>(reg + 1))));
      };

      WARNING_LOG(
        "System12 AplaRail TASK-IRQ #{} scheduler={} vector={} from={:06X} to={:06X} "
        "mailbox={:04X} irq13={} irq26={} irq30={} vblank={} "
        "TSTR={:02X} T0={:02X}/{:02X}/{:02X}/{:04X} T1={:02X}/{:02X}/{:02X}/{:04X}",
        runtime.h8_aplarail_irq_trace_count,
        runtime.h8_scheduler_count, *vector,
        interrupted_pc, static_cast<u32>(h8.cpu.pc),
        mailbox,
        runtime.h8_aplarail_irq13_count,
        runtime.h8_aplarail_irq26_count,
        runtime.h8_aplarail_irq30_count,
        runtime.h8_vblank,
        runtime.h8_timer16.Read(UINT16_C(0xFF60)),
        runtime.h8_timer16.Read(UINT16_C(0xFF64)),
        runtime.h8_timer16.Read(UINT16_C(0xFF65)),
        runtime.h8_timer16.Read(UINT16_C(0xFF66)),
        timer_word(UINT16_C(0xFF68)),
        runtime.h8_timer16.Read(UINT16_C(0xFF6E)),
        runtime.h8_timer16.Read(UINT16_C(0xFF6F)),
        runtime.h8_timer16.Read(UINT16_C(0xFF70)),
        timer_word(UINT16_C(0xFF72)));
    }
  }

  // H83002 irq_setup(): SYSCR bit3=0 sets I+UI; bit3=1 sets I only.
  if ((runtime.h8_peripherals.SYSCR() & UINT8_C(0x08)) == 0)
    h8.cpu.ccr.flags.ui = 1;

  runtime.h8_intc.Acknowledge(*vector);
  runtime.h8_internal[UINT16_C(0xFFF6)] = runtime.h8_intc.GetISR();

  return (h8.cycles > 0) ? static_cast<u32>(h8.cycles) : 0;
}

void H8SchedulerCallback(void*, TickCount ticks, TickCount ticks_late)
{
  if (s_runtime && s_runtime->content.requires_ram_preserving_boot_reset &&
      !s_runtime->boot_warm_reset_completed && !s_runtime->cold_boot_h8_released)
  {
    return;
  }
  if (!s_runtime || !s_runtime->h8_cpu)
    return;

  RuntimeState& runtime = *s_runtime;
  h8_system_t& h8 = *runtime.h8_cpu;

  runtime.h8_scheduler_count++;

  const u32 elapsed_events = std::max<u32>(1, static_cast<u32>(
    (static_cast<u64>(ticks) + static_cast<u64>(runtime.h8_scheduler_interval_ticks) - 1) /
    static_cast<u64>(runtime.h8_scheduler_interval_ticks)));

  if (runtime.h8_scheduler_rate_hz == 1000)
  {
    // Preserve the established 1 kHz System 12 path exactly for all
    // non-CyberLead profiles.
    const u32 bounded_events = std::min<u32>(elapsed_events, 8);

    AdvanceH8RTC(runtime, elapsed_events);

    const u64 cycle_numerator =
      (static_cast<u64>(H8_CLOCK_HZ) * bounded_events) + runtime.h8_cycle_fraction;
    runtime.h8_cycle_balance += static_cast<s64>(cycle_numerator / 1000);
    runtime.h8_cycle_fraction = static_cast<u32>(cycle_numerator % 1000);
  }
  else
  {
    // Match the fine interleave MAME requests for the Cyber Lead device.
    // Keep the same maximum catch-up horizon as the old path: 8 ms.
    const u32 max_catchup_events =
      std::max<u32>(1, (runtime.h8_scheduler_rate_hz * 8) / 1000);
    const u32 bounded_events =
      std::min<u32>(elapsed_events, max_catchup_events);

    // RTC is wall-time based, so account all elapsed scheduler events.
    const u64 rtc_numerator =
      (static_cast<u64>(elapsed_events) * 1000) +
      runtime.h8_rtc_scheduler_fraction;
    const u32 elapsed_milliseconds =
      static_cast<u32>(rtc_numerator / runtime.h8_scheduler_rate_hz);
    runtime.h8_rtc_scheduler_fraction =
      static_cast<u32>(rtc_numerator % runtime.h8_scheduler_rate_hz);

    if (elapsed_milliseconds != 0)
      AdvanceH8RTC(runtime, elapsed_milliseconds);

    // Preserve the exact 16.9344 MHz H8 rate at the finer scheduler quantum.
    const u64 cycle_numerator =
      (static_cast<u64>(H8_CLOCK_HZ) * bounded_events) +
      runtime.h8_cycle_fraction;
    runtime.h8_cycle_balance +=
      static_cast<s64>(cycle_numerator / runtime.h8_scheduler_rate_hz);
    runtime.h8_cycle_fraction =
      static_cast<u32>(cycle_numerator % runtime.h8_scheduler_rate_hz);
  }

  u32 h8_last_step_pc = static_cast<u32>(h8.cpu.pc);
  while (runtime.h8_cycle_balance > 0 && h8.error_code == H8_DEBUG_NO_ERROR)
  {
    h8_last_step_pc = static_cast<u32>(h8.cpu.pc);

    if (runtime.content.set_name == "aplarail")
    {
      const u32 step_pc = h8_last_step_pc;
      const u32 prev_pc = runtime.h8_aplarail_prev_step_pc;
      const bool in_task =
        step_pc >= UINT32_C(0x001540) && step_pc < UINT32_C(0x001680);
      const bool prev_in_task =
        prev_pc >= UINT32_C(0x001540) && prev_pc < UINT32_C(0x001680);
      const bool task_entry = in_task && !prev_in_task;
      const bool key_pc =
        step_pc == UINT32_C(0x00157C) ||
        step_pc == UINT32_C(0x0015A0) ||
        step_pc == UINT32_C(0x001630) ||
        step_pc == UINT32_C(0x001644);

      const u16 mailbox =
        static_cast<u16>(ReadBytes(std::span<const u8>(runtime.shared_ram), 2, UINT32_C(0x3300)));

      if (mailbox != 0 &&
          (task_entry || key_pc) &&
          runtime.h8_aplarail_task_trace_count < UINT32_C(1024))
      {
        runtime.h8_aplarail_task_trace_count++;

        const char* marker =
          (step_pc == UINT32_C(0x00157C)) ? "POLL3300" :
          (step_pc == UINT32_C(0x0015A0)) ? "ACK3300" :
          (step_pc == UINT32_C(0x001630)) ? "READ32C0" :
          (step_pc == UINT32_C(0x001644)) ? "CLEAR3300" :
          "ENTRY";

        const auto timer_word = [&runtime](u16 reg) -> u16 {
          return static_cast<u16>(
            (static_cast<u16>(runtime.h8_timer16.Read(reg)) << 8) |
            static_cast<u16>(runtime.h8_timer16.Read(static_cast<u16>(reg + 1))));
        };

        const u64 irq_age =
          (runtime.h8_aplarail_last_irq_vector != UINT32_C(0xFFFFFFFF) &&
           runtime.h8_scheduler_count >= runtime.h8_aplarail_last_irq_scheduler) ?
            (runtime.h8_scheduler_count - runtime.h8_aplarail_last_irq_scheduler) :
            UINT64_C(0xFFFFFFFFFFFFFFFF);

        WARNING_LOG(
          "System12 AplaRail TASK-PC #{} mark={} scheduler={} pc={:06X} prev={:06X} "
          "mailbox={:04X} coin={:04X} last_irq={} irq_age={} irq_from={:06X} irq_to={:06X} "
          "irq13={} irq26={} irq30={} vblank={} CCR={:02X} SYSCR={:02X} IER={:02X} ISR={:02X} "
          "TSTR={:02X} "
          "T0[tcr={:02X} tier={:02X} tsr={:02X} tcnt={:04X} a={:04X} b={:04X}] "
          "T1[tcr={:02X} tier={:02X} tsr={:02X} tcnt={:04X} a={:04X} b={:04X}] "
          "ER0={:08X} ER1={:08X} ER2={:08X} ER3={:08X} "
          "ER4={:08X} ER5={:08X} ER6={:08X} ER7={:08X}",
          runtime.h8_aplarail_task_trace_count,
          marker,
          runtime.h8_scheduler_count,
          step_pc, prev_pc,
          mailbox,
          static_cast<u16>(ReadBytes(std::span<const u8>(runtime.shared_ram), 2, UINT32_C(0x32C0))),
          runtime.h8_aplarail_last_irq_vector,
          irq_age,
          runtime.h8_aplarail_last_irq_from_pc,
          runtime.h8_aplarail_last_irq_to_pc,
          runtime.h8_aplarail_irq13_count,
          runtime.h8_aplarail_irq26_count,
          runtime.h8_aplarail_irq30_count,
          runtime.h8_vblank,
          h8.cpu.ccr.raw.u,
          runtime.h8_peripherals.SYSCR(),
          runtime.h8_intc.GetIER(),
          runtime.h8_intc.GetISR(),
          runtime.h8_timer16.Read(UINT16_C(0xFF60)),
          runtime.h8_timer16.Read(UINT16_C(0xFF64)),
          runtime.h8_timer16.Read(UINT16_C(0xFF65)),
          runtime.h8_timer16.Read(UINT16_C(0xFF66)),
          timer_word(UINT16_C(0xFF68)),
          timer_word(UINT16_C(0xFF6A)),
          timer_word(UINT16_C(0xFF6C)),
          runtime.h8_timer16.Read(UINT16_C(0xFF6E)),
          runtime.h8_timer16.Read(UINT16_C(0xFF6F)),
          runtime.h8_timer16.Read(UINT16_C(0xFF70)),
          timer_word(UINT16_C(0xFF72)),
          timer_word(UINT16_C(0xFF74)),
          timer_word(UINT16_C(0xFF76)),
          h8.cpu.regs[0].er.u,
          h8.cpu.regs[1].er.u,
          h8.cpu.regs[2].er.u,
          h8.cpu.regs[3].er.u,
          h8.cpu.regs[4].er.u,
          h8.cpu.regs[5].er.u,
          h8.cpu.regs[6].er.u,
          h8.cpu.regs[7].er.u);
      }

      runtime.h8_aplarail_prev_step_pc = step_pc;
    }

    h8_step(&h8);

    if (h8.error_code != H8_DEBUG_NO_ERROR)
      break;

    // h8_step reports H8/300H execution states for the instruction. A valid
    // instruction always includes at least the two-state opcode fetch.
    const u32 instruction_clocks =
      (h8.cycles > 0) ? static_cast<u32>(h8.cycles) : UINT32_C(2);
    H8AdvanceClockedDevices(runtime, instruction_clocks);
    runtime.h8_cycle_balance -= static_cast<s64>(instruction_clocks);

    H8CollectInterruptSources(runtime);
    const u32 interrupt_clocks = H8ServiceInterrupts(runtime);
    if (interrupt_clocks != 0)
    {
      H8AdvanceClockedDevices(runtime, interrupt_clocks);
      runtime.h8_cycle_balance -= static_cast<s64>(interrupt_clocks);
      H8CollectInterruptSources(runtime);
    }
  }

  if (h8.error_code != H8_DEBUG_NO_ERROR && !runtime.h8_fault_logged)
  {
    runtime.h8_fault_logged = true;
    const u32 fault_pc = h8_last_step_pc;
    const u32 shared_3002 = ReadBytes(std::span<const u8>(runtime.shared_ram), 2, UINT32_C(0x3002));
    ERROR_LOG(
      "System12 H8 bootstrap fault error={} core_line={} pc={:06X} opcode={:04X} "
      "er7={:08X} shared3002={:04X} scheduler={} late_ticks={}",
      static_cast<u32>(h8.error_code), h8.error_line, fault_pc, h8.dbus.bits.u,
      h8.cpu.regs[7].er.u, static_cast<u16>(shared_3002),
      runtime.h8_scheduler_count, static_cast<s64>(ticks_late));

  }
}

void ResetH8(RuntimeState& runtime)
{
  if (!runtime.h8_cpu)
    return;

  if (runtime.h8_timing_event)
  {
    if (runtime.h8_timing_event->IsActive())
      runtime.h8_timing_event->Deactivate();
    runtime.h8_timing_event.reset();
  }

  std::memset(runtime.h8_cpu.get(), 0, sizeof(h8_system_t));
  runtime.h8_internal.fill(0);
  // H83002 reset values from MAME.
  runtime.h8_internal[UINT16_C(0xFFF2)] = UINT8_C(0x09); // SYSCR
  runtime.h8_internal[UINT16_C(0xFFF4)] = UINT8_C(0x00); // ISCR
  runtime.h8_internal[UINT16_C(0xFFF5)] = UINT8_C(0x00); // IER
  runtime.h8_internal[UINT16_C(0xFFF6)] = UINT8_C(0x00); // ISR
  runtime.h8_internal[UINT16_C(0xFFF8)] = UINT8_C(0x00); // ICR low
  runtime.h8_internal[UINT16_C(0xFFF9)] = UINT8_C(0x00); // ICR high
  runtime.h8_internal[UINT16_C(0xFFE8)] = UINT8_C(0x00); // ADCSR mirror
  runtime.h8_internal[UINT16_C(0xFFE9)] = UINT8_C(0x00); // ADCR mirror
  runtime.h8_internal[UINT16_C(0xFFC5)] = UINT8_C(0x00); // Port 4 DDR
  runtime.h8_internal[UINT16_C(0xFFC9)] = UINT8_C(0x80); // Port 6 DDR
  runtime.h8_internal[UINT16_C(0xFFCD)] = UINT8_C(0xF0); // Port 8 DDR
  runtime.h8_internal[UINT16_C(0xFFD0)] = UINT8_C(0x00); // Port 9 DDR
  runtime.h8_internal[UINT16_C(0xFFD1)] = UINT8_C(0x00); // Port A DDR
  runtime.h8_internal[UINT16_C(0xFFD4)] = UINT8_C(0x00); // Port B DDR
  runtime.h8_timer16.Reset();
  runtime.h8_intc.Reset();
  runtime.c352.Reset();

  runtime.h8_peripherals.Reset();
  runtime.h8_scheduler_count = 0;
  runtime.h8_cycle_balance = 0;
  runtime.h8_cycle_fraction = 0;
  runtime.h8_rtc_scheduler_fraction = 0;
  runtime.h8_irq_input = 0;
  runtime.h8_jvs_chain.Clear();

  runtime.h8_cyberlead.SetInputProfile(
    runtime.content.machine_config == "aplarail" ?
      Arcade::JVS::NamcoCyberLead::InputProfile::AplaRail :
      Arcade::JVS::NamcoCyberLead::InputProfile::Standard);

  // Standard Cyber Lead JVS for the conventional digital-input titles that
  // are ready for cabinet validation. Aqua Rush is intentionally held back
  // until its independent boot failure is resolved.
  if (runtime.content.machine_config == "aplarail" ||
      runtime.content.set_name.rfind("ehrgeiz", 0) == 0 ||
      runtime.content.set_name.rfind("fgtlayer", 0) == 0 ||
      runtime.content.set_name.rfind("lbgrande", 0) == 0 ||
      runtime.content.set_name.rfind("mrdrillr", 0) == 0 ||
      runtime.content.set_name.rfind("soulclbr", 0) == 0 ||
      runtime.content.set_name.rfind("sws98", 0) == 0 ||
      runtime.content.set_name.rfind("sws99", 0) == 0 ||
      runtime.content.set_name.rfind("tekken3", 0) == 0 ||
      runtime.content.set_name.rfind("tektagt", 0) == 0)
  {
    runtime.h8_jvs.profile = JVSProfile::CyberLead;
  }
  else if (runtime.content.machine_config == "technodr")
  {
    runtime.h8_jvs.profile = JVSProfile::NamcoEMIO102Printer;

    // Physical topology: host -> EM I/O1-02 -> EM Pri1-01.
    // Generic Chain SET_ADDRESS semantics assign the far end first.
    if (!runtime.h8_jvs_chain.AddNode(runtime.h8_emio102) ||
        !runtime.h8_jvs_chain.AddNode(runtime.h8_empri101))
    {
      ERROR_LOG("System12 failed to configure Namco JVS device chain");
      runtime.h8_jvs.profile = JVSProfile::None;
    }
  }
  else
  {
    runtime.h8_jvs.profile = JVSProfile::None;
  }

  runtime.h8_jvs_rts = false;
  ResetH8JVSProtocol(runtime);
  runtime.h8_cyberlead.ResetLED();
  runtime.h8_sci1_ssr_read = 0;
  runtime.h8_sci1_rx_busy = false;
  runtime.h8_sci1_rx_clocks_remaining = 0;
  runtime.h8_vblank = false;
  runtime.h8_fault_logged = false;

  runtime.h8_sub_porta = UINT8_C(0x00);
  runtime.h8_sub_portb = UINT8_C(0x50);

  runtime.h8_settings_ce = false;
  runtime.h8_settings_clk = true;
  runtime.h8_settings_data = false;
  runtime.h8_settings_address = 0;
  runtime.h8_settings_value = 0;
  runtime.h8_settings_bit = 0;

  ResetH8RTC(runtime);
  // H8/3002 reset defaults used by the System 12 firmware before it programs
  // the corresponding data-direction and serial control registers itself.
  runtime.h8_internal[UINT16_C(0xFFB4)] = UINT8_C(0x84);
  runtime.h8_internal[UINT16_C(0xFFBC)] = UINT8_C(0x84);

  // H8/3002 SCI reset values, matching MAME's H8 SCI device:
  // SMR=00, BRR=FF, SCR=00, TDR=FF, SSR=84, RDR=00.
  runtime.h8_internal[UINT16_C(0xFFB0)] = UINT8_C(0x00);
  runtime.h8_internal[UINT16_C(0xFFB1)] = UINT8_C(0xFF);
  runtime.h8_internal[UINT16_C(0xFFB2)] = UINT8_C(0x00);
  runtime.h8_internal[UINT16_C(0xFFB3)] = UINT8_C(0xFF);
  runtime.h8_internal[UINT16_C(0xFFB4)] = UINT8_C(0x84);
  runtime.h8_internal[UINT16_C(0xFFB5)] = UINT8_C(0x00);

  runtime.h8_internal[UINT16_C(0xFFB8)] = UINT8_C(0x00);
  runtime.h8_internal[UINT16_C(0xFFB9)] = UINT8_C(0xFF);
  runtime.h8_internal[UINT16_C(0xFFBA)] = UINT8_C(0x00);
  runtime.h8_internal[UINT16_C(0xFFBB)] = UINT8_C(0xFF);
  runtime.h8_internal[UINT16_C(0xFFBC)] = UINT8_C(0x84);
  runtime.h8_internal[UINT16_C(0xFFBD)] = UINT8_C(0x00);
  runtime.h8_cpu->bus_opaque = &runtime;
  runtime.h8_cpu->bus_read = H8BusRead;
  runtime.h8_cpu->bus_write = H8BusWrite;
  h8_init(runtime.h8_cpu.get());

  runtime.h8_scheduler_rate_hz =
    (runtime.h8_jvs.profile == JVSProfile::CyberLead) ?
      CYBERLEAD_SCHEDULER_HZ : 1000;
  runtime.h8_rtc_scheduler_fraction = 0;

  runtime.h8_scheduler_interval_ticks =
    std::max<TickCount>(
      1, System::GetTicksPerSecond() /
           static_cast<TickCount>(runtime.h8_scheduler_rate_hz));
  runtime.h8_timing_event =
    std::make_unique<TimingEvent>("Namco System 12 H8/3002", runtime.h8_scheduler_interval_ticks,
                                  runtime.h8_scheduler_interval_ticks, H8SchedulerCallback, nullptr);
  runtime.h8_timing_event->SetPeriodAndSchedule(runtime.h8_scheduler_interval_ticks);

  VERBOSE_LOG(
    "System12 H8 bootstrap reset core='libh8300h-adapted' clock_hz={} reset_pc={:06X} "
    "scheduler_ticks={} scheduler_hz={} timing_model='instruction-states' reset_vector={:08X}",
    H8_CLOCK_HZ, static_cast<u32>(runtime.h8_cpu->cpu.pc), runtime.h8_scheduler_interval_ticks,
    runtime.h8_scheduler_rate_hz,
    ReadH8BE32(std::span<const u8>(runtime.content.sub_program), 0));
}
bool HasSoftwareGeneratedHotMarker(const RuntimeState& runtime)
{
  static constexpr u32 RAM_MARKER_OFFSET = UINT32_C(0x00010000);
  static constexpr u32 PROGRAM_MARKER_OFFSET = UINT32_C(0x00020280);
  static constexpr size_t MARKER_SIZE = 12;

  if (!Bus::g_ram ||
      Bus::g_ram_size < (RAM_MARKER_OFFSET + MARKER_SIZE) ||
      runtime.content.program_rom.size() < (PROGRAM_MARKER_OFFSET + MARKER_SIZE))
  {
    return false;
  }

  return std::memcmp(Bus::g_ram + RAM_MARKER_OFFSET,
                     runtime.content.program_rom.data() + PROGRAM_MARKER_OFFSET,
                     MARKER_SIZE) == 0;
}

void CheckRamPreservingBootReset(RuntimeState& runtime)
{
  if (!runtime.content.requires_ram_preserving_boot_reset ||
      runtime.boot_warm_reset_completed ||
      runtime.mainboard_reset_pending ||
      !HasSoftwareGeneratedHotMarker(runtime))
  {
    return;
  }

  const u16 ff0a = static_cast<u16>(
    ReadBytes(std::span<const u8>(runtime.board_control), 2, UINT32_C(0x02)));
  const u16 ff0c = static_cast<u16>(
    ReadBytes(std::span<const u8>(runtime.board_control), 2, UINT32_C(0x04)));

  if (ff0a != UINT16_C(0x0001) || ff0c != UINT16_C(0x0003))
    return;

  runtime.mainboard_reset_pending = true;
  runtime.ram_preserving_reset_count++;

  INFO_LOG(
    "System12 RAM-preserving boot reset requested #{} machine='{}' "
    "marker_source=guest ff0a={:04X} ff0c={:04X}",
    runtime.ram_preserving_reset_count, runtime.content.machine_config, ff0a, ff0c);
}
void SetBank(RuntimeState& runtime, u16 data)
{
  if (runtime.content.alternate_bank)
  {
    if ((data & UINT16_C(0x0008)) != 0)
      runtime.bank = (runtime.bank & UINT32_C(0x0007)) |
                     (static_cast<u32>(data - UINT16_C(0x0008)) << 2);
    else
      runtime.bank = (runtime.bank & ~UINT32_C(0x0007)) | static_cast<u32>(data & UINT16_C(0x0007));
  }
  else
  {
    runtime.bank = data;
  }

}

} // namespace

const std::string* FindGameProperty(const Arcade::Database::GameDefinition& game, const char* name)
{
  for (const Arcade::Database::Property& property : game.properties)
  {
    if (property.name == name)
      return &property.value;
  }

  return nullptr;
}

bool IsAlternateBankStateClass(const std::string& state_class)
{
  return state_class == "namcos12_altbank_state" ||
         state_class == "namcos12_cdxa_state" ||
         state_class == "golgo13_state" ||
         state_class == "truckk_state";
}

bool RequiresRamPreservingBootReset(const std::string& machine_config)
{
  // These configurations inherit the MOTHER(B)/later two-stage bootstrap.
  // Capability is selected from database machineConfig, never from a set name.
  return machine_config == "coh700b" ||
         machine_config == "coh716" ||
         machine_config == "cdxa_pcb" ||
         machine_config == "golgo13" ||
         machine_config == "kartduel" ||
         machine_config == "truckk";
}
std::optional<LoadedContent> LoadSystem12Content(const char* archive_path,
                                                const Arcade::Database::GameDefinition& game, Error* error)
{
  LoadedContent content;
  content.set_name = game.id;
  const std::string* machine_config = FindGameProperty(game, "machineConfig");
  const std::string* state_class = FindGameProperty(game, "stateClass");
  const std::string* input_profile = FindGameProperty(game, "inputProfile");

  content.machine_config = machine_config ? *machine_config : game.hardware_profile;
  content.state_class = state_class ? *state_class : std::string();
  content.input_profile = input_profile ? *input_profile : std::string();

  if (content.state_class == "tektagt_state")
    content.rom_board_profile = ROMBoardProfile::M8F4Protected;
  else if (IsAlternateBankStateClass(content.state_class))
    content.rom_board_profile = ROMBoardProfile::AlternateBank;
  else
    content.rom_board_profile = ROMBoardProfile::Ordinary;

  content.alternate_bank = (content.rom_board_profile == ROMBoardProfile::AlternateBank);
  content.requires_ram_preserving_boot_reset = RequiresRamPreservingBootReset(content.machine_config);

  size_t banked_size = 0;
  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "bankedroms")
      banked_size = std::max(banked_size, ComputeROMExtent(rom));
  }

  if (banked_size == 0)
  {
    Error::SetStringView(error, "Namco System 12 database entry is missing the banked ROM region.");
    return std::nullopt;
  }

  banked_size = (banked_size + BANK_WINDOW_SIZE - 1) & ~(static_cast<size_t>(BANK_WINDOW_SIZE) - 1);

  content.program_rom.resize(PROGRAM_ROM_SIZE);
  content.banked_rom.resize(banked_size);
  content.sub_program.resize(SUB_PROGRAM_SIZE);
  content.c352_samples.resize(C352_SAMPLE_ROM_SIZE);

  u32 program_count = 0;
  u32 banked_count = 0;
  u32 sub_count = 0;
  u32 c352_count = 0;

  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    std::span<u8> destination;
    if (rom.region == "maincpu:rom")
    {
      destination = std::span<u8>(content.program_rom);
      program_count++;
    }
    else if (rom.region == "bankedroms")
    {
      destination = std::span<u8>(content.banked_rom);
      banked_count++;
    }
    else if (rom.region == "sub")
    {
      destination = std::span<u8>(content.sub_program);
      sub_count++;
    }
    else if (rom.region == "c352")
    {
      destination = std::span<u8>(content.c352_samples);
      c352_count++;
    }
    else
    {
      continue;
    }

    std::vector<u8> member_data;
    if (!LoadROMMember(archive_path, rom, &member_data, error) ||
        !PlaceROM(rom, member_data, destination, error))
    {
      return std::nullopt;
    }
  }

  if (program_count == 0 || banked_count == 0 || sub_count == 0 || c352_count == 0)
  {
    Error::SetStringView(error, "Namco System 12 database entry is missing required ROM definitions.");
    return std::nullopt;
  }

  if (content.machine_config == "aplarail" ||
      content.set_name.rfind("soulclbr", 0) == 0)
  {
    const std::string led_firmware_path(Path::Combine(EmuFolders::Bios, "cl1-leda.ic5"));
    const std::optional<DynamicHeapArray<u8>> led_firmware =
      FileSystem::ReadBinaryFile(led_firmware_path.c_str());
    if (!led_firmware)
    {
      INFO_LOG(
        "CyberLead LED firmware not found path='{}'. Cabinet LED emulation will remain disabled; "
        "The game itself will continue normally.",
        led_firmware_path);
    }
    else if (led_firmware->size() != CYBERLEAD_LED_FIRMWARE_SIZE)
    {
      WARNING_LOG("CyberLead LED firmware wrong size path='{}' expected={} actual={}; LED disabled",
                  led_firmware_path, CYBERLEAD_LED_FIRMWARE_SIZE, led_firmware->size());
    }
    else
    {
      const std::span<const u8> firmware_span = led_firmware->cspan();
      auto sha1 = SHA1Digest::GetDigest(firmware_span);
      const std::string sha1_string = SHA1Digest::DigestToString(sha1);
      if (sha1 != CYBERLEAD_LED_FIRMWARE_SHA1)
      {
        WARNING_LOG("CyberLead LED firmware SHA1 mismatch path='{}' expected={} actual={}; LED disabled",
                    led_firmware_path, "61d61338e1bb414af32dbdc1e24d54c02fb9196e", sha1_string);
      }
      else
      {
        content.cyberlead_led_firmware.assign(firmware_span.begin(), firmware_span.end());
        INFO_LOG("CyberLead LED firmware loaded path='{}' size={} sha1={}", led_firmware_path,
                 content.cyberlead_led_firmware.size(), sha1_string);
      }
    }
  }

  INFO_LOG("Loaded System 12 set '{}' profile='{}': program={} banked={} sub={} c352={} alternate_bank={}",
           game.id, game.hardware_profile, content.program_rom.size(), content.banked_rom.size(),
           content.sub_program.size(), content.c352_samples.size(), content.alternate_bank);
  return content;
}

void GenerateAudioFrame(s32* left, s32* right)
{
  if (!left || !right)
    return;

  *left = 0;
  *right = 0;
  if (!s_runtime)
    return;

  s_runtime->c352.PopHostFrame(left, right);
}
bool Initialize(LoadedContent content, Error* error)
{
  if (content.set_name.empty() || content.program_rom.size() != PROGRAM_ROM_SIZE ||
      content.banked_rom.empty() || content.sub_program.size() != SUB_PROGRAM_SIZE ||
      content.c352_samples.size() != C352_SAMPLE_ROM_SIZE)
  {
    Error::SetStringView(error, "Invalid Namco System 12 ROM content.");
    return false;
  }

  if (!Bus::g_bios)
  {
    Error::SetStringView(error, "System 12 cannot install its program ROM before Bus initialization.");
    return false;
  }

  std::memcpy(Bus::g_bios, content.program_rom.data(), Bus::BIOS_SIZE);

  RuntimeState runtime;
  runtime.content = std::move(content);

  if (!LoadEEPROM(runtime, error))
    return false;

  runtime.h8_cpu = std::make_unique<h8_system_t>();
  if (!runtime.content.cyberlead_led_firmware.empty() &&
      !runtime.h8_cyberlead.SetLEDFirmware(std::span<const u8>(runtime.content.cyberlead_led_firmware)))
  {
    WARNING_LOG("CyberLead LED firmware was validated but C77 initialization failed; LED disabled");
    runtime.content.cyberlead_led_firmware.clear();
  }

  s_runtime = std::move(runtime);
  Reset();

  INFO_LOG(
    "Namco System 12 base board initialized for '{}'. H8/3002 instruction-state timing is active; "
    "external H8 wait-state refinement, JVS edge cases and protected ROM-board capabilities remain staged follow-up work.",
    s_runtime->content.set_name);
  return true;
}

void Reset()
{
  if (!s_runtime)
    return;

  s_runtime->shared_ram.fill(0);
  s_runtime->board_control.fill(0);
  s_runtime->mainboard_reset_pending = false;
  s_runtime->boot_warm_reset_completed = false;
  s_runtime->cold_boot_h8_released = !s_runtime->content.requires_ram_preserving_boot_reset;
  s_runtime->cold_boot_config_header.fill(0);
  s_runtime->cold_boot_config_feed_words = 0;
  s_runtime->cold_boot_config_finalize_words = 0;
  s_runtime->cold_boot_config_bulk_complete = false;
  s_runtime->bank = 0;
  s_runtime->dma_source = 0;
  s_runtime->dma_transfer_count = 0;

  s_runtime->m8f4_challenge.fill(0);
  s_runtime->m8f4_challenge_count = 0;
  s_runtime->m8f4_protected_source = 0;
  s_runtime->m8f4_response = 0;
  s_runtime->m8f4_nonce = 0;
  s_runtime->m8f4_response_class = 0;
  s_runtime->m8f4_challenge_active = false;
  s_runtime->m8f4_response_valid = false;
  s_runtime->m8f4_protected_dma_armed = false;
  s_runtime->m8f4_waiting_for_release = false;
  s_runtime->m8f4_ready = true;
  s_runtime->m8f4_transaction_count = 0;
  s_runtime->m8f4_dma_count = 0;

  // System 12 board reset leaves the PIO/DMA5 source idle.
  DMA::SetRequest(DMA::Channel::PIO, false);

  ResetH8(*s_runtime);
}

void Shutdown()
{
  if (s_runtime && s_runtime->h8_timing_event && s_runtime->h8_timing_event->IsActive())
    s_runtime->h8_timing_event->Deactivate();

  // Clear the shared DMA5 request before removing the System 12 provider.
  DMA::SetRequest(DMA::Channel::PIO, false);

  if (s_runtime)
    SaveEEPROM(*s_runtime);

  s_runtime.reset();
}

bool IsActive()
{
  return s_runtime.has_value();
}

void SetVBlank(bool state)
{
  if (!s_runtime)
    return;

  RuntimeState& runtime = *s_runtime;
  if (runtime.h8_vblank == state)
    return;

  runtime.h8_vblank = state;
  runtime.h8_sub_portb =
    static_cast<u8>((runtime.h8_sub_portb & UINT8_C(0x7F)) |
                    (state ? UINT8_C(0x80) : UINT8_C(0x00)));

  H8SetExternalIRQLine(runtime, 1, state);
  runtime.h8_peripherals.SetADCTrigger(state);
  H8CollectInterruptSources(runtime);


  const u32 interrupt_clocks = H8ServiceInterrupts(runtime);
  if (interrupt_clocks != 0)
  {
    H8AdvanceClockedDevices(runtime, interrupt_clocks);
    runtime.h8_cycle_balance -= static_cast<s64>(interrupt_clocks);
    H8CollectInterruptSources(runtime);
  }
}

bool BeginMainBoardReset()
{
  if (!s_runtime || !s_runtime->mainboard_reset_pending)
    return false;

  s_runtime->mainboard_reset_pending = false;
  return true;
}

void EndMainBoardReset()
{
  if (!s_runtime)
    return;

  // InternalReset() has already reset H8/board/timing state. The PSX main RAM
  // is restored by System::FrameDone after that reset. Inhibit another cold
  // transition until a normal user/power reset starts a fresh lifecycle.
  s_runtime->mainboard_reset_pending = false;
  s_runtime->boot_warm_reset_completed = true;

  INFO_LOG("System12 RAM-preserving warm boot resumed machine='{}'",
           s_runtime->content.machine_config);
}

u32 ReadProgramROM(u32 width, u32 offset)
{
  if (!s_runtime)
    return UINT32_C(0xFFFFFFFF);

  return ReadBytes(std::span<const u8>(s_runtime->content.program_rom), width, offset);
}

u32 ReadEXP1(u32 width, u32 offset)
{
  if (!s_runtime)
    return UINT32_C(0xFFFFFFFF);

  if (offset >= SHARED_RAM_BASE && offset <= SHARED_RAM_END)
  {
    const u32 shared_offset = offset - SHARED_RAM_BASE;
    const u32 value =
      ReadBytes(std::span<const u8>(s_runtime->shared_ram), width, shared_offset);

    // Diagnostic only: identify the exact PSX-side reads which consume the
    // Cyber Lead input/coin state decoded by the original H8 firmware.
    const bool trace_aplarail_input_shared =
      s_runtime->content.set_name == "aplarail" &&
      ((shared_offset >= UINT32_C(0x313C) && shared_offset <= UINT32_C(0x3187)) ||
       (shared_offset >= UINT32_C(0x32BC) && shared_offset <= UINT32_C(0x3307)));

    if (trace_aplarail_input_shared)
    {
      static std::array<u32, PSX_SHARED_WINDOW_SIZE> s_last_values{};
      static std::array<u8, PSX_SHARED_WINDOW_SIZE> s_last_widths{};
      static bool s_initialized = false;
      static u32 s_log_count = 0;

      if (!s_initialized)
      {
        s_last_values.fill(UINT32_C(0xFFFFFFFF));
        s_last_widths.fill(UINT8_C(0));
        s_initialized = true;
      }

      if (shared_offset < PSX_SHARED_WINDOW_SIZE &&
          s_log_count < UINT32_C(4096) &&
          (s_last_widths[shared_offset] != width ||
           s_last_values[shared_offset] != value))
      {
        s_last_widths[shared_offset] = static_cast<u8>(width);
        s_last_values[shared_offset] = value;
        s_log_count++;

        WARNING_LOG(
          "System12 AplaRail PSX-SHARED read #{} offset={:04X} width={} value={:08X} "
          "pc={:08X} current_pc={:08X} ra={:08X} gp={:08X}",
          s_log_count, shared_offset, width, value,
          CPU::g_state.pc, CPU::g_state.current_instruction_pc,
          CPU::g_state.regs.ra, CPU::g_state.regs.gp);
      }
    }

    // AplaRail diagnostic: trace PSX cached inputs and consumer sites.
    //
    // This is diagnostic-only. It does not alter shared RAM, cached input
    // values, JVS state, H8 timing, or PSX execution.
    if (s_runtime->content.set_name == "aplarail")
    {
      const bool service_pressed = ArcadeInput::IsOperatorPressed("Service");
      const bool coin_pressed = ArcadeInput::IsDigitalPressed(0, "Coin");

      // Scan the loaded PSX executable once, on the first operator input, for
      // MIPS memory operations which can touch the cached input words. The
      // GP-relative ranges cover the input cache around gp+0474..0486 and the
      // baseline/reference words around gp+05F0..0608. Absolute-address
      // candidates cover the same RAM using a 0x8007xxxx base register.
      static bool s_aplarail_consumer_scan_done = false;
      if (!s_aplarail_consumer_scan_done && (service_pressed || coin_pressed))
      {
        s_aplarail_consumer_scan_done = true;
        u32 match_count = 0;

        WARNING_LOG(
          "System12 AplaRail PSX-CONSUMER-SCAN BEGIN start=80010000 end=80080000 gp={:08X}",
          CPU::g_state.regs.gp);

        for (u32 address = UINT32_C(0x80010000);
             address < UINT32_C(0x80080000); address += 4)
        {
          u32 instruction = 0;
          if (!CPU::SafeReadMemoryWord(address, &instruction))
            continue;

          const u32 opcode = (instruction >> 26) & UINT32_C(0x3F);
          const u32 rs = (instruction >> 21) & UINT32_C(0x1F);
          const u32 rt = (instruction >> 16) & UINT32_C(0x1F);
          const u16 imm = static_cast<u16>(instruction);

          const char* op_name = nullptr;
          bool is_load = false;
          switch (opcode)
          {
            case 0x20: op_name = "LB";  is_load = true; break;
            case 0x21: op_name = "LH";  is_load = true; break;
            case 0x22: op_name = "LWL"; is_load = true; break;
            case 0x23: op_name = "LW";  is_load = true; break;
            case 0x24: op_name = "LBU"; is_load = true; break;
            case 0x25: op_name = "LHU"; is_load = true; break;
            case 0x26: op_name = "LWR"; is_load = true; break;
            case 0x28: op_name = "SB"; break;
            case 0x29: op_name = "SH"; break;
            case 0x2A: op_name = "SWL"; break;
            case 0x2B: op_name = "SW"; break;
            case 0x2E: op_name = "SWR"; break;
            default: break;
          }

          if (!op_name)
            continue;

          const bool gp_cache_candidate =
            rs == UINT32_C(28) &&
            ((imm >= UINT16_C(0x0470) && imm <= UINT16_C(0x0490)) ||
             (imm >= UINT16_C(0x05F0) && imm <= UINT16_C(0x0608)));

          const bool absolute_cache_candidate =
            (imm >= UINT16_C(0x1BD0) && imm <= UINT16_C(0x1BDC)) ||
            (imm >= UINT16_C(0x1D50) && imm <= UINT16_C(0x1D64));

          if (!gp_cache_candidate && !absolute_cache_candidate)
            continue;

          match_count++;
          WARNING_LOG(
            "System12 AplaRail PSX-CONSUMER-SCAN match={} addr={:08X} word={:08X} "
            "op={} rs={} rt={} imm={:04X} mode={}",
            match_count, address, instruction, op_name, rs, rt, imm,
            gp_cache_candidate ? "gp-relative" : "absolute-candidate");

          // Context around load sites should expose the subsequent mask/test/
          // branch without requiring another full-memory dump.
          if (is_load)
          {
            const u32 context_start = address - UINT32_C(0x18);
            const u32 context_end = address + UINT32_C(0x30);
            for (u32 context = context_start; context <= context_end; context += 4)
            {
              u32 context_word = 0;
              const bool valid = CPU::SafeReadMemoryWord(context, &context_word);
              WARNING_LOG(
                "System12 AplaRail PSX-CONSUMER-CONTEXT match={} addr={:08X} "
                "focus={} valid={} word={:08X}",
                match_count, context, context == address, valid, context_word);
            }
          }
        }

        WARNING_LOG(
          "System12 AplaRail PSX-CONSUMER-SCAN END matches={}", match_count);
      }

      // Sample the PSX-side cache while the firmware/shared-RAM refresh path
      // is executing. Reading 3142/32C2/3302 is especially useful because it
      // occurs after the preceding cached halfword has had a chance to store.
      const bool cache_trace_point =
        shared_offset == UINT32_C(0x3140) || shared_offset == UINT32_C(0x3142) ||
        shared_offset == UINT32_C(0x32C0) || shared_offset == UINT32_C(0x32C2) ||
        shared_offset == UINT32_C(0x3300) || shared_offset == UINT32_C(0x3302);

      if (cache_trace_point)
      {
        u16 cached_service = 0;
        u16 cached_coin = 0;
        u16 baseline_coin = 0;
        const bool cached_service_valid =
          CPU::SafeReadMemoryHalfWord(CPU::g_state.regs.gp + UINT32_C(0x476),
                                      &cached_service);
        const bool cached_coin_valid =
          CPU::SafeReadMemoryHalfWord(CPU::g_state.regs.gp + UINT32_C(0x47A),
                                      &cached_coin);
        const bool baseline_coin_valid =
          CPU::SafeReadMemoryHalfWord(UINT32_C(0x80071D5C), &baseline_coin);

        static bool s_cache_initialized = false;
        static u16 s_last_cached_service = UINT16_C(0xFFFF);
        static u16 s_last_cached_coin = UINT16_C(0xFFFF);
        static u16 s_last_baseline_coin = UINT16_C(0xFFFF);
        static u32 s_cache_log_count = 0;

        const bool cache_changed =
          !s_cache_initialized || cached_service != s_last_cached_service ||
          cached_coin != s_last_cached_coin || baseline_coin != s_last_baseline_coin;
        const bool shared_nonzero = value != 0;
        const bool input_active = service_pressed || coin_pressed;
        const bool coin_latched =
          cached_coin_valid && baseline_coin_valid && cached_coin != baseline_coin;

        if (s_cache_log_count < UINT32_C(512) &&
            (cache_changed || shared_nonzero || input_active || coin_latched))
        {
          s_cache_initialized = true;
          s_last_cached_service = cached_service;
          s_last_cached_coin = cached_coin;
          s_last_baseline_coin = baseline_coin;
          s_cache_log_count++;

          WARNING_LOG(
            "System12 AplaRail PSX-CACHE #{} shared={:04X} width={} value={:08X} "
            "service_raw={} coin_raw={} service_valid={} service={:04X} "
            "coin_valid={} coin={:04X} baseline_valid={} baseline={:04X} "
            "pc={:08X} current_pc={:08X} ra={:08X} gp={:08X}",
            s_cache_log_count, shared_offset, width, value,
            service_pressed, coin_pressed, cached_service_valid, cached_service,
            cached_coin_valid, cached_coin, baseline_coin_valid, baseline_coin,
            CPU::g_state.pc, CPU::g_state.current_instruction_pc,
            CPU::g_state.regs.ra, CPU::g_state.regs.gp);
        }
      }
    }
    return value;
  }

  // The System 12 EEPROM decode ignores A12. Mirror the second 4 KiB CPU
  // aperture onto the same AT28C16 storage.
  if (offset >= UINT32_C(0x141000) && offset <= UINT32_C(0x141FFF))
    return ReadEEPROM(*s_runtime, width, offset - UINT32_C(0x1000));

  if (offset >= EEPROM_BASE && offset <= EEPROM_END)
    return ReadEEPROM(*s_runtime, width, offset);

  // M8F4 commits its alternate protected source by reading the base of the
  // ordinary System 12 DMA-source aperture. The arm is one-shot and applies
  // only to the next DMA5 transfer.
  if (IsM8F4(*s_runtime) &&
      offset >= DMA_SOURCE_BASE && offset <= (DMA_SOURCE_BASE + 3))
  {
    RuntimeState& runtime = *s_runtime;
    runtime.m8f4_protected_dma_armed = true;
    runtime.m8f4_waiting_for_release = false;
    runtime.m8f4_ready = false;
    DMA::SetRequest(DMA::Channel::PIO, true);

    return 0;
  }

  return UINT32_C(0xFFFFFFFF);
}

bool WriteEXP1(u32 width, u32 offset, u32 value)
{
  if (!s_runtime)
    return false;

  RuntimeState& runtime = *s_runtime;

  if (offset >= BANK_REGISTER_BASE && offset <= BANK_REGISTER_END)
  {
    SetBank(runtime, static_cast<u16>(value));
    return true;
  }

  if (offset >= DMA_COMPLETE_STROBE_BASE && offset <= DMA_COMPLETE_STROBE_END)
  {
    if (IsM8F4(runtime) && runtime.m8f4_waiting_for_release)
    {
      runtime.m8f4_waiting_for_release = false;
      runtime.m8f4_ready = true;

    }

    return true;
  }

  if (offset >= UNKNOWN_STROBE_BASE && offset <= UNKNOWN_STROBE_END)
    return true;

  if (offset >= WAIT_PACING_STROBE_BASE && offset <= WAIT_PACING_STROBE_END)
    return true;

  if (offset >= SHARED_RAM_BASE && offset <= SHARED_RAM_END)
  {
    const u32 shared_offset = offset - SHARED_RAM_BASE;

    // Diagnostic only: log PSX writes into the two narrow mailbox regions
    // implicated by the Cyber Lead H8 trace. This does not alter the write.
    const bool trace_aplarail_input_shared =
      runtime.content.set_name == "aplarail" &&
      ((shared_offset >= UINT32_C(0x313C) && shared_offset <= UINT32_C(0x3187)) ||
       (shared_offset >= UINT32_C(0x32BC) && shared_offset <= UINT32_C(0x3307)));

    u32 before = 0;
    if (trace_aplarail_input_shared)
      before = ReadBytes(std::span<const u8>(runtime.shared_ram), width, shared_offset);

    WriteBytes(std::span<u8>(runtime.shared_ram), width, shared_offset, value);

    if (trace_aplarail_input_shared)
    {
      static u32 s_log_count = 0;
      if (s_log_count < UINT32_C(24))
      {
        const u32 after =
          ReadBytes(std::span<const u8>(runtime.shared_ram), width, shared_offset);
        s_log_count++;

        u16 cached_coin = 0;
        u16 baseline_coin = 0;
        const bool cached_coin_valid =
          CPU::SafeReadMemoryHalfWord(CPU::g_state.regs.gp + UINT32_C(0x47A), &cached_coin);
        const bool baseline_coin_valid =
          CPU::SafeReadMemoryHalfWord(UINT32_C(0x80071D5C), &baseline_coin);

        WARNING_LOG(
          "System12 AplaRail COIN-MAILBOX PSX write #{} offset={:04X} width={} "
          "value={:08X} before={:08X} after={:08X} "
          "pc={:08X} current_pc={:08X} instr={:08X} ra={:08X} gp={:08X} "
          "cached_valid={} cached={:04X} baseline_valid={} baseline={:04X} "
          "v0={:08X} v1={:08X} a0={:08X} a1={:08X} a2={:08X} a3={:08X} "
          "t0={:08X} t1={:08X} t2={:08X} t3={:08X} s0={:08X} s1={:08X} sp={:08X}",
          s_log_count, shared_offset, width, value, before, after,
          CPU::g_state.pc, CPU::g_state.current_instruction_pc,
          CPU::g_state.current_instruction.bits, CPU::g_state.regs.ra,
          CPU::g_state.regs.gp,
          cached_coin_valid, cached_coin, baseline_coin_valid, baseline_coin,
          CPU::g_state.regs.v0, CPU::g_state.regs.v1,
          CPU::g_state.regs.a0, CPU::g_state.regs.a1,
          CPU::g_state.regs.a2, CPU::g_state.regs.a3,
          CPU::g_state.regs.t0, CPU::g_state.regs.t1,
          CPU::g_state.regs.t2, CPU::g_state.regs.t3,
          CPU::g_state.regs.s0, CPU::g_state.regs.s1,
          CPU::g_state.regs.sp);
        static bool s_coin_flow_dumped = false;
        if (!s_coin_flow_dumped)
        {
          s_coin_flow_dumped = true;

          struct CoinFlowRegion
          {
            u32 start;
            u32 end;
            const char* label;
          };

          static constexpr CoinFlowRegion regions[] = {
            {UINT32_C(0x80026A40), UINT32_C(0x80026B20), "COIN_HANDLER"},
            {UINT32_C(0x80026E40), UINT32_C(0x80026FC0), "INPUT_REFRESH"},
            {UINT32_C(0x800504D0), UINT32_C(0x80050680), "CALLER"},
            {UINT32_C(0x8004FA80), UINT32_C(0x8004FBC0), "BASELINE"},
          };

          for (const CoinFlowRegion& region : regions)
          {
            WARNING_LOG(
              "System12 AplaRail COIN-FLOW-DUMP BEGIN label={} start={:08X} end={:08X}",
              region.label, region.start, region.end);

            for (u32 address = region.start; address <= region.end; address += 4)
            {
              u32 instruction = 0;
              const bool valid = CPU::SafeReadMemoryWord(address, &instruction);
              WARNING_LOG(
                "System12 AplaRail COIN-FLOW-DUMP label={} addr={:08X} valid={} word={:08X}",
                region.label, address, valid, instruction);
            }

            WARNING_LOG(
              "System12 AplaRail COIN-FLOW-DUMP END label={}",
              region.label);
          }
        }

        static bool s_coin_code_dumped = false;
        if (!s_coin_code_dumped)
        {
          s_coin_code_dumped = true;

          const u32 centers[2] = {
            CPU::g_state.pc,
            CPU::g_state.regs.ra,
          };
          static constexpr const char* labels[2] = {
            "PC",
            "RA",
          };

          for (u32 region = 0; region < 2; region++)
          {
            const u32 center = centers[region];
            const u32 start = (center - UINT32_C(0x60)) & ~UINT32_C(3);
            const u32 end = (center + UINT32_C(0x60)) & ~UINT32_C(3);

            WARNING_LOG(
              "System12 AplaRail COIN-CODE-DUMP BEGIN label={} center={:08X} start={:08X} end={:08X}",
              labels[region], center, start, end);

            for (u32 address = start; address <= end; address += 4)
            {
              u32 instruction = 0;
              const bool valid = CPU::SafeReadMemoryWord(address, &instruction);

              WARNING_LOG(
                "System12 AplaRail COIN-CODE-DUMP label={} addr={:08X} valid={} word={:08X}",
                labels[region], address, valid, instruction);
            }

            WARNING_LOG(
              "System12 AplaRail COIN-CODE-DUMP END label={} center={:08X}",
              labels[region], center);
          }
        }
      }
    }

    return true;
  }

  // The System 12 EEPROM decode ignores A12. Mirror the second 4 KiB CPU
  // aperture onto the same AT28C16 storage.
  if (offset >= UINT32_C(0x141000) && offset <= UINT32_C(0x141FFF))
  {
    WriteEEPROM(runtime, width, offset - UINT32_C(0x1000), value);
    return true;
  }

  if (offset >= EEPROM_BASE && offset <= EEPROM_END)
  {
    WriteEEPROM(runtime, width, offset, value);
    return true;
  }
  if (offset >= BOARD_CONTROL_BASE && offset <= BOARD_CONTROL_END)
  {
    const u32 control_offset = offset - BOARD_CONTROL_BASE;
    WriteBytes(std::span<u8>(runtime.board_control), width, control_offset, value);

    if (runtime.content.requires_ram_preserving_boot_reset &&
        !runtime.boot_warm_reset_completed &&
        !runtime.cold_boot_h8_released &&
        control_offset == UINT32_C(0x06) && width == 2)
    {
      const u16 feed_word = static_cast<u16>(value);

      if (!runtime.cold_boot_config_bulk_complete)
      {
        const u32 feed_index = runtime.cold_boot_config_feed_words;
        if (feed_index < runtime.cold_boot_config_header.size())
          runtime.cold_boot_config_header[feed_index] = feed_word;

        runtime.cold_boot_config_feed_words++;

        if (runtime.content.program_rom.size() > BOARD_CONFIG_FEED_ROM_OFFSET)
        {
          const u32 expected_bulk_words = static_cast<u32>(
            (runtime.content.program_rom.size() - BOARD_CONFIG_FEED_ROM_OFFSET) /
            BOARD_CONFIG_FEED_STRIDE);

          if (runtime.cold_boot_config_feed_words == expected_bulk_words)
          {
            runtime.cold_boot_config_bulk_complete = true;
            runtime.cold_boot_config_finalize_words = 0;

            INFO_LOG(
              "System12 board-config bulk feed complete machine='{}' words={} "
              "header={:04X},{:04X},{:04X},{:04X}",
              runtime.content.machine_config, runtime.cold_boot_config_feed_words,
              runtime.cold_boot_config_header[0], runtime.cold_boot_config_header[1],
              runtime.cold_boot_config_header[2], runtime.cold_boot_config_header[3]);
          }
        }
      }
      else
      {
        const u32 finalize_index = runtime.cold_boot_config_finalize_words;
        if (finalize_index < runtime.cold_boot_config_header.size() &&
            feed_word == runtime.cold_boot_config_header[finalize_index])
        {
          runtime.cold_boot_config_finalize_words++;

          if (runtime.cold_boot_config_finalize_words == BOARD_CONFIG_FINALIZE_WORDS)
          {
            runtime.cold_boot_h8_released = true;
            INFO_LOG(
              "System12 cold-boot H8 release machine='{}' "
              "reason='board-config-feed-complete' bulk_words={} finalize_words={}",
              runtime.content.machine_config, runtime.cold_boot_config_feed_words,
              runtime.cold_boot_config_finalize_words);
          }
        }
        else
        {
          // Stay in the finalization phase and resynchronize on the first
          // header word. This avoids tying the board model to a guest PC or
          // literal per-revision data values.
          runtime.cold_boot_config_finalize_words =
            (feed_word == runtime.cold_boot_config_header[0]) ? 1u : 0u;
        }
      }
    }

    CheckRamPreservingBootReset(runtime);
    return true;
  }

  if (offset >= DMA_SOURCE_BASE && offset <= DMA_SOURCE_END)
  {
    const u32 low_source = offset - DMA_SOURCE_BASE;
    runtime.dma_source = low_source | ((value & UINT32_C(0xFFFF)) << 16);

    // Arm the PSX DMA5/PIO request line for this ROM transfer.
    DMA::SetRequest(DMA::Channel::PIO, true);
    return true;
  }

  return false;
}

u32 ReadEXP3(u32 width, u32 offset)
{
  if (!s_runtime)
    return UINT32_C(0xFFFFFFFF);

  RuntimeState& runtime = *s_runtime;
  if (IsM8F4(runtime))
  {
    if (offset >= M8F4_PROTECTION1_BASE && offset <= M8F4_PROTECTION1_END)
    {
      const u32 byte_offset = offset - M8F4_PROTECTION1_BASE;
      const u32 status = runtime.m8f4_ready ? UINT32_C(0x00008000) : 0;
      return ReadM8F4RegisterValue(width, byte_offset, status);
    }

    if (offset >= M8F4_PROTECTION2_BASE && offset <= M8F4_PROTECTION2_END)
    {
      const u32 byte_offset = offset - M8F4_PROTECTION2_BASE;
      const u32 response = runtime.m8f4_response_valid ? runtime.m8f4_response : 0;
      return ReadM8F4RegisterValue(width, byte_offset, response);
    }
  }

  return ReadBankedROM(width, offset);
}

bool WriteEXP3(u32 width, u32 offset, u32 value)
{
  if (!s_runtime)
    return false;

  RuntimeState& runtime = *s_runtime;
  if (!IsM8F4(runtime))
    return false;

  if (offset >= M8F4_PROTECTION2_BASE && offset <= M8F4_PROTECTION2_END)
  {
    const u32 byte_offset = offset - M8F4_PROTECTION2_BASE;

    // The original software writes a variable 15-bit LCG value to the low
    // halfword. It is a transaction-start nonce, not part of challenge matching.
    if (byte_offset == 0 && (width == 2 || width == 4))
    {
      runtime.m8f4_nonce = static_cast<u16>(value);
      runtime.m8f4_challenge.fill(0);
      runtime.m8f4_challenge_count = 0;
      runtime.m8f4_challenge_active = true;
      runtime.m8f4_response_valid = false;
      runtime.m8f4_response = 0;
      runtime.m8f4_response_class = 0;
      runtime.m8f4_transaction_count++;

    }

    return true;
  }

  if (offset >= M8F4_PROTECTION1_BASE && offset <= M8F4_PROTECTION1_END)
  {
    const u32 byte_offset = offset - M8F4_PROTECTION1_BASE;

    if (byte_offset == 0 && width == 4)
    {
      if (runtime.m8f4_challenge_active && runtime.m8f4_challenge_count < 2)
      {
        runtime.m8f4_challenge[runtime.m8f4_challenge_count++] = value;

        if (runtime.m8f4_challenge_count == 2)
        {
          runtime.m8f4_response = DecodeM8F4Response(runtime);
          runtime.m8f4_response_valid = true;
          runtime.m8f4_challenge_active = false;

          if (runtime.m8f4_response_class == 0 && runtime.m8f4_transaction_count <= 16)
          {
            WARNING_LOG(
              "System12 M8F4 unknown protection class challenge0={:08X} challenge1={:08X}",
              runtime.m8f4_challenge[0], runtime.m8f4_challenge[1]);
          }
        }
      }
      else
      {
        // Outside an active two-word challenge transaction, the same register
        // carries the logical source for the protected one-shot DMA path.
        runtime.m8f4_protected_source = value;

      }

      return true;
    }

    // The recovered protocol uses aligned 32-bit writes here. Consume other
    // accesses without inventing partial-write semantics.
    return true;
  }

  return false;
}

u32 ReadBankedROM(u32 width, u32 offset)
{
  if (!s_runtime)
    return UINT32_C(0xFFFFFFFF);

  const u64 source = (static_cast<u64>(s_runtime->bank) * BANK_WINDOW_SIZE) + offset;
  if (source > UINT32_MAX)
    return UINT32_C(0xFFFFFFFF);

  return ReadBytes(std::span<const u8>(s_runtime->content.banked_rom), width, static_cast<u32>(source));
}

bool DebugCopyCyberLeadLEDFrame(std::span<u8> pixels, std::array<u32, 4>* intensity_counts,
                                u16* start_address, u32* scroll_x,
                                u32* scroll_y, u64* generation)
{
  if (!s_runtime)
    return false;

  return s_runtime->h8_cyberlead.CopyLEDFrame(pixels, intensity_counts, start_address,
                                              scroll_x, scroll_y, generation);
}

void DMARead(u32* destination, u32 word_count)
{
  if (!s_runtime || !destination || word_count == 0)
    return;

  RuntimeState& runtime = *s_runtime;

  if (IsM8F4(runtime) && runtime.m8f4_protected_dma_armed)
  {
    const u32 logical_source = runtime.m8f4_protected_source;
    u64 source = 0;
    std::span<const u8> region;
    const char* provider = "invalid";

    if (logical_source < M8F4_GAME_ROM_LIMIT)
    {
      source = logical_source;
      region = std::span<const u8>(runtime.content.banked_rom);
      provider = "m8f4-data";
    }
    else if (logical_source >= M8F4_PROGRAM_ROM_BASE &&
             logical_source < M8F4_PROGRAM_ROM_LIMIT)
    {
      source = static_cast<u64>(logical_source - M8F4_PROGRAM_ROM_BASE);
      region = std::span<const u8>(runtime.content.program_rom);
      provider = "m8f4-program";
    }

    u32 copied_words = 0;
    if (!region.empty() && source < region.size())
    {
      const u64 available_bytes = static_cast<u64>(region.size()) - source;
      copied_words = static_cast<u32>(
        std::min<u64>(word_count, available_bytes / sizeof(u32)));

      if (copied_words != 0)
      {
        std::memcpy(destination, region.data() + static_cast<size_t>(source),
                    static_cast<size_t>(copied_words) * sizeof(u32));
      }
    }

    runtime.m8f4_protected_dma_armed = false;
    runtime.m8f4_waiting_for_release = true;
    runtime.m8f4_dma_count++;
    runtime.dma_transfer_count++;

    if (copied_words < word_count && runtime.m8f4_dma_count <= 64)
    {
      WARNING_LOG(
        "System12 M8F4 protected DMA5 truncated logical={:08X} requested={} copied={} provider={}",
        logical_source, word_count, copied_words, provider);
    }

    // Consume the protected one-shot DREQ. 0x1F008000 will subsequently
    // release the transaction and expose ready bit 15 again.
    DMA::SetRequest(DMA::Channel::PIO, false);
    return;
  }

  const bool main_program_rom =
    ((runtime.dma_source & UINT32_C(0x80000000)) != 0) || (Bus::GetEXP1Base() == UINT32_C(0x1F300000));

  u64 source = 0;
  std::span<const u8> region;
  if (main_program_rom)
  {
    source = runtime.dma_source & UINT32_C(0x00FFFFFF);
    region = std::span<const u8>(runtime.content.program_rom);
  }
  else
  {
    source = runtime.dma_source & UINT32_C(0x7FFFFFFF);
    if (runtime.content.alternate_bank)
      source += static_cast<u64>(runtime.bank) * BANK_WINDOW_SIZE;
    region = std::span<const u8>(runtime.content.banked_rom);
  }

  u32 copied_words = 0;
  if (source < region.size())
  {
    const u64 available_bytes = static_cast<u64>(region.size()) - source;
    copied_words = static_cast<u32>(std::min<u64>(word_count, available_bytes / sizeof(u32)));
    if (copied_words != 0)
    {
      std::memcpy(destination, region.data() + static_cast<size_t>(source),
                  static_cast<size_t>(copied_words) * sizeof(u32));
    }
  }

  runtime.dma_transfer_count++;

  if (copied_words < word_count && runtime.dma_transfer_count <= 64)
  {
    WARNING_LOG("System12 DMA5 transfer truncated: requested={} copied={} source={:08X} region_size={:08X}",
                word_count, copied_words, static_cast<u32>(source), static_cast<u32>(region.size()));
  }

  // Consume this ROM-board DREQ; the next source-latch write rearms it.
  DMA::SetRequest(DMA::Channel::PIO, false);
}
} // namespace NamcoSystem12
