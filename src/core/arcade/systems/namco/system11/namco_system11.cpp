// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system11/namco_system11.h"

#include "core/arcade/arcade_database.h"
#include "core/arcade/arcade_input.h"
#include "core/arcade/arcade_output.h"
#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/m37710/m37710.h"
#include "core/settings.h"
#include "core/system.h"
#include "core/timing_event.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/path.h"
#include "common/sha1_digest.h"
#include "common/string_util.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

Log_SetChannel(NamcoSystem11);

namespace NamcoSystem11 {
namespace {

static constexpr u32 PROGRAM_ROM_SIZE = 0x400000;
static constexpr u32 BANKED_ROM_SIZE = 0x1000000;
static constexpr u32 BANKED_ROM64_SIZE = 0x2000000;
static constexpr u32 C76_INTERNAL_ROM_SIZE = 0x4000;
static constexpr u32 C76_PROGRAM_ROM_SIZE = 0x80000;
static constexpr u32 C352_SAMPLE_ROM_SIZE = 0x1000000;
static constexpr u32 IOMCU_PROGRAM_ROM_SIZE = 0x80000;
static constexpr u32 BANK_WINDOW_SIZE = 0x100000;
static constexpr u32 BANK_REGISTER_BASE = 0x10020;
static constexpr u32 BANK_REGISTER_END = 0x1002f;

static constexpr u32 C76_CLOCK_HZ = 16'934'400;
static constexpr TickCount C76_SCHEDULER_INTERVAL_TICKS = System::MASTER_CLOCK / 1000;
static constexpr int C76_MAX_EXECUTE_CYCLES = static_cast<int>((C76_CLOCK_HZ + 999) / 1000);

static constexpr u16 C352_FLAG_BUSY = 0x8000;
static constexpr u16 C352_FLAG_KEYON = 0x4000;
static constexpr u16 C352_FLAG_KEYOFF = 0x2000;
static constexpr u16 C352_FLAG_LOOPHIST = 0x0800;
static constexpr u16 C352_FLAG_PHASEFL = 0x0100;
static constexpr u16 C352_FLAG_PHASEFR = 0x0080;
static constexpr u16 C352_FLAG_LDIR = 0x0040;
static constexpr u16 C352_FLAG_LINK = 0x0020;
static constexpr u16 C352_FLAG_NOISE = 0x0010;
static constexpr u16 C352_FLAG_MULAW = 0x0008;
static constexpr u16 C352_FLAG_FILTER = 0x0004;
static constexpr u16 C352_FLAG_LOOP = 0x0002;
static constexpr u16 C352_FLAG_REVERSE = 0x0001;

static const std::array<s16, 256> s_c352_mulaw_table = []() {
  std::array<s16, 256> table{};
  s32 value = 0;

  for (u32 i = 0; i < 128; i++)
  {
    table[i] = static_cast<s16>(value << 5);
    if (i < 16)
      value += 1;
    else if (i < 24)
      value += 2;
    else if (i < 48)
      value += 4;
    else if (i < 100)
      value += 8;
    else
      value += 16;
  }

  for (u32 i = 0; i < 128; i++)
    table[i + 128] = static_cast<s16>((~static_cast<u16>(table[i])) & 0xffe0);

  return table;
}();

struct C352Voice
{
  u32 pos = 0;
  u32 counter = 0;
  s16 sample = 0;
  s16 last_sample = 0;
  u16 vol_f = 0;
  u16 vol_r = 0;
  std::array<u8, 4> curr_vol{};
  u16 freq = 0;
  u16 flags = 0;
  u16 wave_bank = 0;
  u16 wave_start = 0;
  u16 wave_end = 0;
  u16 wave_loop = 0;
};

struct RuntimeState
{
  LoadedContent content;
  std::array<u8, 8> bank_entries{};
  u8 rom8_64_bank_offset = 0;
  u16 keycus_p1 = 0;
  u16 keycus_p2 = 0;
  u16 keycus_p3 = 0;
  std::array<C352Voice, 32> c352_voices{};
  u16 c352_control = 0;
  u16 c352_random = 0x1234;
  bool c352_first_keyon_logged = false;
  std::unique_ptr<m37710_cpu_device> c76_cpu;
  std::unique_ptr<TimingEvent> c76_timing_event;
  u64 c76_cycle_fraction = 0;
  u64 c76_last_ticks_per_second = 0;
  s64 c76_cycle_balance = 0;
  u64 c76_total_cycles = 0;
  u64 c76_scheduler_callbacks = 0;
  u32 c76_slice_logs = 0;
  bool c76_first_c352_access_logged = false;
  bool c76_first_unmapped_access_logged = false;

  // Cabinet outputs are written repeatedly by the game. Keep the physical
  // output latch here so the generic output manager only sees actual changes.
  u8 gun_output_latch = 0;
  bool gun_output_latch_valid = false;

  u8 family_bowl_m139_pending_command = 0;
  u8 family_bowl_m139_param_bytes_remaining = 0;
  std::array<u8, 2> family_bowl_m139_params{};

  // A successful M139 SENS 7/Ball Detect event reports C9 C9. Startup uses
  // one pair after configuration; gameplay uses another pair whenever the
  // live ball-ready state returns to its unprimed signature.
  u32 family_bowl_startup_c9_stage = 0;
  u32 family_bowl_startup_c9_delay_ms = 0;
  u32 family_bowl_ready_c9_stage = 0;

  // End-of-game cabinet ball-return state. The real cabinet asks the player
  // to return the physical ball after a completed game. The PSX waits at
  // gate=3/game=0102 for the M139 SENS7/Ball Detect completion.
  u32 family_bowl_end_game_c9_stage = 0;
  u32 family_bowl_end_game_c9_delay_ms = 0;

  // A0 enables one M139 measurement cycle. Trackball motion is accumulated
  // until the roll settles, converted to virtual SENS1..SENS4 timestamps, and
  // encoded through the recovered M139 C3 equations.
  s64 family_bowl_c3_pending_x = 0;
  s64 family_bowl_c3_pending_y = 0;
  u32 family_bowl_c3_generation = 0;
  u32 family_bowl_c3_seen_generation = 0;
  u32 family_bowl_c3_idle_ms = 0;
  bool family_bowl_c3_measurement_enabled = false;
  bool family_bowl_c3_packet_armed = false;
  u32 family_bowl_c3_packet_index = 0;
  std::array<u8, 11> family_bowl_c3_packet{};

  s64 family_bowl_trackball_pending_x = 0;
  s64 family_bowl_trackball_pending_y = 0;
  u32 family_bowl_trackball_generation = 0;
  u32 family_bowl_trackball_seen_generation = 0;
  u32 family_bowl_trackball_settle_polls = 0;

  // B1 exposes the seven active-low M139 sensors in the game's input test.
  // Stage 1 = near sensor, 2 = travel gap, 3 = far sensor.
  u32 family_bowl_sensor_sequence_stage = 0;
  u32 family_bowl_sensor_sequence_polls_remaining = 0;
  u8 family_bowl_sensor_sequence_first = UINT8_C(0x7f);
  u8 family_bowl_sensor_sequence_second = UINT8_C(0x7f);

  bool test_switch_latched = false;
  bool test_switch_was_pressed = false;


  // C76 P8.1 is connected to the System 11 main-board reset path.
  // The request is consumed at the frame boundary as a complete board reset.
  bool mainboard_reset_pending = false;

  std::string persistence_directory;
  std::string eeprom_path;
  bool eeprom_dirty = false;
  bool eeprom_busy = false;
  u8 eeprom_last_write = UINT8_C(0xff);
  u64 eeprom_busy_until = 0;
};

std::optional<RuntimeState> s_runtime;
std::mutex s_family_bowl_trackball_mutex;

bool LoadROMMember(const char* archive_path, const Arcade::Database::ROMDefinition& rom, std::vector<u8>* data,
                   Error* error)
{
  unzFile zf = MinizipHelpers::OpenUnzFile(archive_path);
  if (!zf)
  {
    Error::SetStringFmt(error, "Failed to open Namco System 11 set archive '{}'.", archive_path);
    return false;
  }

  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    Error::SetStringFmt(error, "Namco System 11 set archive '{}' is empty or unreadable.", archive_path);
    return false;
  }

  for (;;)
  {
    unz_file_info64 file_info = {};
    char member_name[512] = {};
    if (unzGetCurrentFileInfo64(zf, &file_info, member_name, sizeof(member_name), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed to read file information from Namco System 11 set archive '{}'.",
                          archive_path);
      return false;
    }

    member_name[sizeof(member_name) - 1] = '\0';
    if (StringUtil::EqualNoCase(member_name, rom.name))
    {
      if (file_info.uncompressed_size != rom.size)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "System 11 ROM '{}' has size {}, expected {} bytes.", rom.name,
                            file_info.uncompressed_size, rom.size);
        return false;
      }
      if (rom.has_crc32 && static_cast<u32>(file_info.crc) != rom.crc32)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "System 11 ROM '{}' has CRC32 {:08x}, expected {:08x}.", rom.name,
                            static_cast<u32>(file_info.crc), rom.crc32);
        return false;
      }
      if (unzOpenCurrentFile(zf) != UNZ_OK)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Failed to decompress System 11 ROM '{}' from '{}'.", rom.name, archive_path);
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
          Error::SetStringFmt(error, "Failed reading System 11 ROM '{}' from '{}'.", rom.name, archive_path);
          return false;
        }
        read_offset += static_cast<size_t>(bytes_read);
      }

      const int close_result = unzCloseCurrentFile(zf);
      unzClose(zf);
      if (close_result != UNZ_OK)
      {
        Error::SetStringFmt(error, "CRC validation failed for System 11 ROM '{}' in '{}'.", rom.name, archive_path);
        return false;
      }

      if (!rom.sha1.empty())
      {
        auto digest = SHA1Digest::GetDigest(std::span<const u8>(data->data(), data->size()));
        const std::string digest_string = SHA1Digest::DigestToString(digest);
        if (!StringUtil::EqualNoCase(digest_string, rom.sha1))
        {
          Error::SetStringFmt(error, "System 11 ROM '{}' has SHA-1 {}, expected {}.", rom.name, digest_string,
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
      Error::SetStringFmt(error, "Failed while reading Namco System 11 set archive '{}'.", archive_path);
      return false;
    }
  }

  unzClose(zf);
  Error::SetStringFmt(error, "Namco System 11 set archive '{}' does not contain required ROM '{}'.", archive_path,
                      rom.name);
  return false;
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
      Error::SetStringFmt(error, "System 11 ROM '{}' segment exceeds its source data.", rom.name);
      return false;
    }
    if (group_size == 0)
    {
      Error::SetStringFmt(error, "System 11 ROM '{}' has an invalid group size of zero.", rom.name);
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
        Error::SetStringFmt(error, "System 11 ROM '{}' exceeds the '{}' region.", rom.name, rom.region);
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
        Error::SetStringFmt(error, "System 11 ROM '{}' uses unsupported segment operation '{}'.", rom.name,
                            segment.operation);
        return false;
      }

      if (!place_range(segment.source_offset, segment.length, segment.offset, segment.group_size, segment.skip,
                       segment.reverse))
      {
        return false;
      }
    }

    return true;
  }

  if (rom.group_size != 1 || rom.word_swap || rom.interleave == 0 || rom.interleave != (rom.group_size + rom.skip))
  {
    Error::SetStringFmt(error, "System 11 ROM '{}' uses an unsupported database load layout.", rom.name);
    return false;
  }

  return place_range(0, static_cast<u32>(source.size()), rom.offset, rom.group_size, rom.skip, false);
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
    std::memcpy(&value, &data[read_offset], sizeof(value));
    return static_cast<u32>(value);
  }

  u32 value;
  std::memcpy(&value, &data[read_offset], sizeof(value));
  return value;
}

} // namespace

std::optional<LoadedContent> LoadSystem11Content(const char* archive_path,
                                               const Arcade::Database::GameDefinition& game,
                                               const char* firmware_archive_path,
                                               const Arcade::Database::FirmwareDefinition& firmware, Error* error)
{
  LoadedContent content;
  content.set_name = game.id;

  // C76 input routing follows each game's MAME input profile rather than inheriting
  // the wiring of whichever System 11 title was implemented first.
  if (game.hardware_profile == "tekken" || game.hardware_profile == "tekken2" ||
      game.hardware_profile == "tekken2o" || game.hardware_profile == "primglex")
  {
    content.input_profile = C76InputProfile::Tekken;
  }
  else if (game.hardware_profile == "souledge")
  {
    content.input_profile = C76InputProfile::SoulEdge;
  }
  else if (game.hardware_profile == "myangel3")
  {
    content.input_profile = C76InputProfile::MyAngel3;
  }
  else if (game.hardware_profile == "pocketrc")
  {
    content.input_profile = C76InputProfile::PocketRacer;
  }
  else if (game.hardware_profile == "ptblank2ua")
  {
    content.input_profile = C76InputProfile::PointBlank2;
  }

  // System 11 KEYCUS selection follows MAME's device assignments and the independently
  // implemented SYSTEM11_MiSTer keycus_id table. Tekken 1 and Family Bowl have no physical KEYCUS.
  if (game.hardware_profile == "tekken2" || game.hardware_profile == "tekken2o")
    content.keycus_type = KeycusType::C406;
  else if (game.hardware_profile == "souledge")
    content.keycus_type = KeycusType::C409;
  else if (game.hardware_profile == "dunkmnia")
    content.keycus_type = KeycusType::C410;
  else if (game.hardware_profile == "primglex")
    content.keycus_type = KeycusType::C411;
  else if (game.hardware_profile == "xevi3dg")
    content.keycus_type = KeycusType::C430;
  else if (game.hardware_profile == "danceyes")
    content.keycus_type = KeycusType::C431;
  else if (game.hardware_profile == "pocketrc")
    content.keycus_type = KeycusType::C432;
  else if (game.hardware_profile == "starswep")
    content.keycus_type = KeycusType::C442;
  else if (game.hardware_profile == "myangel3" || game.hardware_profile == "ptblank2ua")
    content.keycus_type = KeycusType::C443;

  // Point Blank 2 / Gunbarl add the System 11 GUN I/F board over the final ROM8 window.
  content.has_gun_interface = (game.hardware_profile == "ptblank2ua");
  content.has_family_bowl_io = (game.hardware_profile == "fambowl");

  content.program_rom.resize(PROGRAM_ROM_SIZE);
  content.banked_rom.resize((content.keycus_type == KeycusType::C443) ? BANKED_ROM64_SIZE : BANKED_ROM_SIZE);
  content.c76_program.resize(C76_PROGRAM_ROM_SIZE);
  content.c352_samples.resize(C352_SAMPLE_ROM_SIZE);
  if (content.has_family_bowl_io)
    content.iomcu_program.resize(IOMCU_PROGRAM_ROM_SIZE);

  u32 program_rom_count = 0;
  u32 banked_rom_count = 0;
  u32 c76_program_rom_count = 0;
  u32 c352_sample_rom_count = 0;
  u32 iomcu_program_rom_count = 0;

  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    std::span<u8> destination;
    if (rom.region == "maincpu:rom")
    {
      destination = std::span<u8>(content.program_rom);
      program_rom_count++;
    }
    else if (rom.region == "bankedroms")
    {
      destination = std::span<u8>(content.banked_rom);
      banked_rom_count++;
    }
    else if (rom.region == "c76")
    {
      destination = std::span<u8>(content.c76_program);
      c76_program_rom_count++;
    }
    else if (rom.region == "c352")
    {
      destination = std::span<u8>(content.c352_samples);
      c352_sample_rom_count++;
    }
    else if (rom.region == "iomcu" && content.has_family_bowl_io)
    {
      destination = std::span<u8>(content.iomcu_program);
      iomcu_program_rom_count++;
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

  // Star Sweep runs directly from the System 11 MOTHER(B) PCB and has no ROM8
  // daughterboard/banked ROM region. All other currently supported System 11
  // profiles require banked ROM definitions.
  const bool banked_rom_required = (game.hardware_profile != "starswep");

  if (program_rom_count == 0 || (banked_rom_required && banked_rom_count == 0) ||
      c76_program_rom_count == 0 || c352_sample_rom_count == 0 ||
      (content.has_family_bowl_io && iomcu_program_rom_count == 0))
  {
    Error::SetStringView(error, "Namco System 11 database entry is missing required ROM definitions.");
    return std::nullopt;
  }

  const Arcade::Database::ROMDefinition* c76_internal_rom = nullptr;
  for (const Arcade::Database::ROMDefinition& rom : firmware.roms)
  {
    if (rom.role == "c76_internal")
    {
      c76_internal_rom = &rom;
      break;
    }
  }

  if (!c76_internal_rom || c76_internal_rom->size != C76_INTERNAL_ROM_SIZE)
  {
    Error::SetStringView(error, "Namco System 11 firmware definition is missing the C76 internal ROM.");
    return std::nullopt;
  }

  if (!LoadROMMember(firmware_archive_path, *c76_internal_rom, &content.c76_internal, error))
    return std::nullopt;

  VERBOSE_LOG(
    "Loaded Namco System 11 ROMs from '{}': program={} bytes banked={} bytes c76_internal={} bytes c76={} bytes "
    "c352={} bytes iomcu={} bytes.",
    archive_path, content.program_rom.size(), content.banked_rom.size(), content.c76_internal.size(),
    content.c76_program.size(), content.c352_samples.size(), content.iomcu_program.size());
  return content;
}

static constexpr u32 KEYCUS_BASE = 0x20000;
static constexpr u32 KEYCUS_END = 0x2001f;

static constexpr u32 C76_SHARED_RAM_BASE = 0x04000;
static constexpr u32 C76_SHARED_RAM_END = 0x0ffff;
static std::array<u8, (C76_SHARED_RAM_END - C76_SHARED_RAM_BASE) + 1> s_c76_shared_ram{};

// SYSTEM11_MiSTer uses a 2^18-C76-cycle period, with IRQ2 half a period after IRQ0.
// MAME still labels its 60 Hz IRQ0/IRQ2 generation as TODO, so keep the independently verified phase relationship here.
static constexpr u32 C76_IRQ_PERIOD_CYCLES = UINT32_C(1) << 18;
static constexpr u32 C76_IRQ_HALF_PERIOD_CYCLES = C76_IRQ_PERIOD_CYCLES / 2;
static u32 s_c76_irq_cycles_until_next = 0;
static bool s_c76_next_irq_is_irq0 = true;
static u32 s_c76_irq0_count = 0;
static u32 s_c76_irq2_count = 0;
static u32 s_c76_irq_logs = 0;

static constexpr u32 C76_MCU_SHARED_RAM_BASE = 0x004000;
static constexpr u32 C76_MCU_SHARED_RAM_END = 0x00bfff;
static constexpr u32 C76_MCU_INPUT_BASE = 0x001000;
static constexpr u32 C76_MCU_INPUT_END = 0x001007;
static constexpr u32 C76_MCU_C352_BASE = 0x002000;
static constexpr u32 C76_MCU_C352_END = 0x002fff;
static constexpr u32 C76_MCU_PROGRAM_BASE = 0x080000;
static constexpr u32 C76_MCU_PROGRAM_END = 0x0fffff;
static constexpr u32 C76_MCU_PROGRAM_MIRROR_BASE = 0x200000;
static constexpr u32 C76_MCU_PROGRAM_MIRROR_END = 0x2fffff;
static constexpr u32 C76_MCU_FAMILY_BOWL_IO_BASE = 0x510000;
static constexpr u32 C76_MCU_FAMILY_BOWL_IO_END = 0x51ffff;

static void C352FetchSample(C352Voice& voice)
{
  voice.last_sample = voice.sample;

  if (voice.flags & C352_FLAG_NOISE)
  {
    const u16 feedback = (s_runtime->c352_random & 1) ? 0xfff6 : 0;
    s_runtime->c352_random = static_cast<u16>((s_runtime->c352_random >> 1) ^ feedback);
    voice.sample = static_cast<s16>(s_runtime->c352_random);
    return;
  }

  const u8 raw_sample = s_runtime->content.c352_samples[voice.pos & 0x00ffffff];
  if (voice.flags & C352_FLAG_MULAW)
  {
    voice.sample = s_c352_mulaw_table[raw_sample];
  }
  else
  {
    const s32 signed_sample = (raw_sample & 0x80) ? (static_cast<s32>(raw_sample) - 0x100) : raw_sample;
    voice.sample = static_cast<s16>(signed_sample * 0x100);
  }

  const u16 pos = static_cast<u16>(voice.pos);
  if ((voice.flags & C352_FLAG_LOOP) && (voice.flags & C352_FLAG_REVERSE))
  {
    if ((voice.flags & C352_FLAG_LDIR) && pos == voice.wave_loop)
      voice.flags &= static_cast<u16>(~C352_FLAG_LDIR);
    else if (!(voice.flags & C352_FLAG_LDIR) && pos == voice.wave_end)
      voice.flags |= C352_FLAG_LDIR;

    voice.pos = (voice.pos + ((voice.flags & C352_FLAG_LDIR) ? 0x00ffffff : 1)) & 0x00ffffff;
  }
  else if (pos == voice.wave_end)
  {
    if ((voice.flags & C352_FLAG_LINK) && (voice.flags & C352_FLAG_LOOP))
    {
      voice.pos = ((static_cast<u32>(voice.wave_start) & 0xff) << 16) | voice.wave_loop;
      voice.flags |= C352_FLAG_LOOPHIST;
    }
    else if (voice.flags & C352_FLAG_LOOP)
    {
      voice.pos = (voice.pos & 0xff0000) | voice.wave_loop;
      voice.flags |= C352_FLAG_LOOPHIST;
    }
    else
    {
      voice.flags |= C352_FLAG_KEYOFF;
      voice.flags &= static_cast<u16>(~C352_FLAG_BUSY);
      voice.sample = 0;
    }
  }
  else
  {
    voice.pos = (voice.pos + ((voice.flags & C352_FLAG_REVERSE) ? 0x00ffffff : 1)) & 0x00ffffff;
  }
}

static void C352RampVolume(C352Voice& voice, u32 channel, u8 target)
{
  if (voice.curr_vol[channel] < target)
    voice.curr_vol[channel]++;
  else if (voice.curr_vol[channel] > target)
    voice.curr_vol[channel]--;
}

static void C352GenerateNativeFrame(s32* left, s32* right)
{
  s32 output_left = 0;
  s32 output_right = 0;

  for (C352Voice& voice : s_runtime->c352_voices)
  {
    s32 sample = 0;

    if (voice.flags & C352_FLAG_BUSY)
    {
      const u32 next_counter = voice.counter + voice.freq;
      if (next_counter & 0x10000)
        C352FetchSample(voice);

      if ((next_counter ^ voice.counter) & 0x18000)
      {
        C352RampVolume(voice, 0, static_cast<u8>(voice.vol_f >> 8));
        C352RampVolume(voice, 1, static_cast<u8>(voice.vol_f));
        C352RampVolume(voice, 2, static_cast<u8>(voice.vol_r >> 8));
        C352RampVolume(voice, 3, static_cast<u8>(voice.vol_r));
      }

      voice.counter = next_counter & 0xffff;

      // MiSTer's System 11 implementation gates a voice which became idle while
      // fetching its final sample. MAME otherwise uses the same interpolation.
      if (voice.flags & C352_FLAG_BUSY)
      {
        sample = voice.sample;
        if (!(voice.flags & C352_FLAG_FILTER))
        {
          const s32 difference = static_cast<s32>(voice.sample) - voice.last_sample;
          sample = voice.last_sample +
                   static_cast<s32>((static_cast<s64>(voice.counter) * difference) >> 16);
        }
      }
    }

    const s32 left_sample = (voice.flags & C352_FLAG_PHASEFL) ? -sample : sample;
    const s32 right_sample = (voice.flags & C352_FLAG_PHASEFR) ? -sample : sample;
    output_left += (left_sample * voice.curr_vol[0]) >> 8;
    output_right += (right_sample * voice.curr_vol[1]) >> 8;
  }

  // System 11 only wires the C352 front DAC pair. MAME and MiSTer both scale
  // the final C352 mix by eight before presenting the signed 16-bit output.
  *left = static_cast<s16>(output_left >> 3);
  *right = static_cast<s16>(output_right >> 3);
}

static u16 C352ReadRegister(u32 offset)
{
  if (!s_runtime.has_value())
    return 0;

  if (offset < 0x100)
  {
    const C352Voice& voice = s_runtime->c352_voices[offset / 8];
    switch (offset & 7)
    {
      case 0: return voice.vol_f;
      case 1: return voice.vol_r;
      case 2: return voice.freq;
      case 3: return voice.flags;
      case 4: return voice.wave_bank;
      case 5: return voice.wave_start;
      case 6: return voice.wave_end;
      case 7: return voice.wave_loop;
      default: return 0;
    }
  }

  if (offset == 0x200)
    return s_runtime->c352_control;

  return 0;
}

static void C352ExecuteKeyOns()
{
  for (u32 i = 0; i < s_runtime->c352_voices.size(); i++)
  {
    C352Voice& voice = s_runtime->c352_voices[i];

    if (voice.flags & C352_FLAG_KEYON)
    {
      voice.pos = (static_cast<u32>(voice.wave_bank) << 16) | voice.wave_start;
      voice.sample = 0;
      voice.last_sample = 0;
      voice.counter = 0xffff;
      voice.flags |= C352_FLAG_BUSY;
      voice.flags &= static_cast<u16>(~(C352_FLAG_KEYON | C352_FLAG_LOOPHIST));
      voice.curr_vol.fill(0);

      if (!s_runtime->c352_first_keyon_logged)
      {
        s_runtime->c352_first_keyon_logged = true;
        DEV_LOG("System 11 C352 first key-on voice={} pos=0x{:06X} freq=0x{:04X} flags=0x{:04X}.",
                 i, voice.pos, voice.freq, voice.flags);
      }
    }

    if (voice.flags & C352_FLAG_KEYOFF)
    {
      voice.flags &= static_cast<u16>(~(C352_FLAG_BUSY | C352_FLAG_KEYOFF));
      voice.counter = 0xffff;
    }
  }
}

static void C352WriteRegister(u32 offset, u16 value)
{
  if (!s_runtime.has_value())
    return;

  if (offset < 0x100)
  {
    C352Voice& voice = s_runtime->c352_voices[offset / 8];
    switch (offset & 7)
    {
      case 0: voice.vol_f = value; break;
      case 1: voice.vol_r = value; break;
      case 2: voice.freq = value; break;
      case 3: voice.flags = value; break;
      case 4: voice.wave_bank = value; break;
      case 5: voice.wave_start = value; break;
      case 6: voice.wave_end = value; break;
      case 7: voice.wave_loop = value; break;
    }
    return;
  }

  if (offset == 0x200)
  {
    s_runtime->c352_control = value;
    return;
  }

  // MAME's C352 implementation treats this as the 16-bit command which commits
  // pending key-on/key-off flags for all 32 voices.
  if (offset == 0x202)
    C352ExecuteKeyOns();
}

static u8 C352ReadByte(u32 address)
{
  const u32 byte_offset = address - C76_MCU_C352_BASE;
  const u16 value = C352ReadRegister(byte_offset >> 1);
  return (byte_offset & 1) ? static_cast<u8>(value >> 8) : static_cast<u8>(value);
}

static void C352WriteByte(u32 address, u8 value)
{
  const u32 byte_offset = address - C76_MCU_C352_BASE;
  const u32 offset = byte_offset >> 1;

  // The C76 reaches the C352 over an 8-bit bus. MiSTer commits pending
  // key-on/key-off state on the high byte at C352 offset 0x405.
  if (byte_offset == 0x405)
  {
    C352ExecuteKeyOns();
    return;
  }
  if (byte_offset == 0x404)
    return;

  u16 reg = C352ReadRegister(offset);
  if (byte_offset & 1)
    reg = static_cast<u16>((reg & 0x00ff) | (static_cast<u16>(value) << 8));
  else
    reg = static_cast<u16>((reg & 0xff00) | value);

  C352WriteRegister(offset, reg);
}

static C76InputProfile GetC76InputProfile()
{
  return s_runtime.has_value() ? s_runtime->content.input_profile : C76InputProfile::Standard;
}

static u8 C76ReadPlayer(u32 port)
{
  u8 value = 0xff;
  const C76InputProfile profile = GetC76InputProfile();

  if (profile == C76InputProfile::PocketRacer)
    return value;

  if (profile == C76InputProfile::MyAngel3)
  {
    if (ArcadeInput::IsDigitalPressed(port, "Button4"))
      value &= ~UINT8_C(0x01);
    if (ArcadeInput::IsDigitalPressed(port, "Button3"))
      value &= ~UINT8_C(0x02);
    if (ArcadeInput::IsDigitalPressed(port, "Button2"))
      value &= ~UINT8_C(0x04);
    if (ArcadeInput::IsDigitalPressed(port, "Button1"))
      value &= ~UINT8_C(0x08);
    if (ArcadeInput::IsDigitalPressed(port, "Start"))
      value &= ~UINT8_C(0x80);
    return value;
  }

  if (profile == C76InputProfile::PointBlank2)
  {
    if (ArcadeInput::IsDigitalPressed(port, "Trigger"))
      value &= ~UINT8_C(0x10);
    if (ArcadeInput::IsDigitalPressed(port, "Start"))
      value &= ~UINT8_C(0x80);
    return value;
  }

  if (ArcadeInput::IsDigitalPressed(port, "Right"))
    value &= ~UINT8_C(0x01);
  if (ArcadeInput::IsDigitalPressed(port, "Left"))
    value &= ~UINT8_C(0x02);
  if (ArcadeInput::IsDigitalPressed(port, "Down"))
    value &= ~UINT8_C(0x04);
  if (ArcadeInput::IsDigitalPressed(port, "Up"))
    value &= ~UINT8_C(0x08);
  if (ArcadeInput::IsDigitalPressed(port, "Button1"))
    value &= ~UINT8_C(0x10);
  if (ArcadeInput::IsDigitalPressed(port, "Button2"))
    value &= ~UINT8_C(0x20);
  if (profile != C76InputProfile::Tekken && ArcadeInput::IsDigitalPressed(port, "Button3"))
    value &= ~UINT8_C(0x40);
  if (ArcadeInput::IsDigitalPressed(port, "Start"))
    value &= ~UINT8_C(0x80);

  return value;
}

static u8 C76ReadSwitches()
{
  u8 value = 0xff;
  const C76InputProfile profile = GetC76InputProfile();

  if (ArcadeInput::IsOperatorPressed("Service"))
    value &= ~UINT8_C(0x80);

  // Test is a maintained cabinet switch, not a momentary pushbutton.
  const bool test_pressed = ArcadeInput::IsOperatorPressed("Test");
  if (test_pressed && !s_runtime->test_switch_was_pressed)
    s_runtime->test_switch_latched = !s_runtime->test_switch_latched;
  s_runtime->test_switch_was_pressed = test_pressed;

  if (s_runtime->test_switch_latched)
    value &= ~UINT8_C(0x40);

  if (ArcadeInput::IsDigitalPressed(0, "Coin"))
    value &= ~UINT8_C(0x20);

  if (profile != C76InputProfile::PocketRacer && ArcadeInput::IsDigitalPressed(1, "Coin"))
    value &= ~UINT8_C(0x10);

  if ((profile == C76InputProfile::Standard || profile == C76InputProfile::PointBlank2) &&
      ArcadeInput::IsDigitalPressed(2, "Coin"))
  {
    value &= ~UINT8_C(0x08);
  }
  if ((profile == C76InputProfile::Standard || profile == C76InputProfile::PointBlank2) &&
      ArcadeInput::IsDigitalPressed(3, "Coin"))
  {
    value &= ~UINT8_C(0x04);
  }

  // Physical DIP SW2 on the System 11 I/O board. Both switches are active-low.
  if (g_settings.arcade_namco_system11_dip_test)
    value &= ~UINT8_C(0x02);
  if (g_settings.arcade_namco_system11_dip_freeze)
    value &= ~UINT8_C(0x01);

  return value;
}

static u8 C76ReadPlayer4()
{
  u8 value = 0xff;

  switch (GetC76InputProfile())
  {
    case C76InputProfile::Standard:
      return C76ReadPlayer(3);

    case C76InputProfile::Tekken:
      if (ArcadeInput::IsDigitalPressed(1, "Button3"))
        value &= ~UINT8_C(0x10);
      if (ArcadeInput::IsDigitalPressed(1, "Button4"))
        value &= ~UINT8_C(0x20);
      break;

    case C76InputProfile::SoulEdge:
      if (ArcadeInput::IsDigitalPressed(1, "Button4"))
        value &= ~UINT8_C(0x10);
      break;

    case C76InputProfile::PocketRacer:
      if (ArcadeInput::IsDigitalPressed(0, "View"))
        value &= ~UINT8_C(0x08);
      break;

    case C76InputProfile::MyAngel3:
    case C76InputProfile::PointBlank2:
      break;
  }

  return value;
}

static u16 C76ReadDigitalADC(u32 port, const char* key)
{
  return ArcadeInput::IsDigitalPressed(port, key) ? UINT16_C(0x0000) : UINT16_C(0x00ff);
}

static u16 C76ScaleAnalog(float normalized, u16 min_value, u16 max_value, bool reverse)
{
  normalized = std::clamp(normalized, 0.0f, 1.0f);
  const u16 scaled = static_cast<u16>(static_cast<float>(min_value) +
                                      normalized * static_cast<float>(max_value - min_value) + 0.5f);
  return reverse ? static_cast<u16>(max_value - (scaled - min_value)) : scaled;
}

static u8 C76ReadByte(u32 address);
static void C76WriteByte(u32 address, u8 value);

static u16 C76ReadWord(u32 address)
{
  address &= 0x00ffffff;

  if (!(address & 1) && address >= C76_MCU_C352_BASE && (address + 1) <= C76_MCU_C352_END)
    return C352ReadRegister((address - C76_MCU_C352_BASE) >> 1);

  return static_cast<u16>(C76ReadByte(address) | (static_cast<u16>(C76ReadByte(address + 1)) << 8));
}

static void C76WriteWord(u32 address, u16 value)
{
  address &= 0x00ffffff;

  if (!(address & 1) && address >= C76_MCU_C352_BASE && (address + 1) <= C76_MCU_C352_END)
  {
    C352WriteRegister((address - C76_MCU_C352_BASE) >> 1, value);
    return;
  }

  C76WriteByte(address, static_cast<u8>(value));
  C76WriteByte(address + 1, static_cast<u8>(value >> 8));
}

static u8 C76ReadByte(u32 address)
{
  if (!s_runtime.has_value())
    return 0xff;

  address &= 0x00ffffff;

  // The standalone M37702 core handles implemented SFRs internally and falls through for reserved holes.
  // Treat those reserved internal addresses as open/no-op space instead of reporting them as board accesses.
  if (address < 0x80)
    return 0x00;

  if (address >= C76_MCU_SHARED_RAM_BASE && address <= C76_MCU_SHARED_RAM_END)
    return s_c76_shared_ram[address - C76_MCU_SHARED_RAM_BASE];

  if (address >= C76_MCU_PROGRAM_BASE && address <= C76_MCU_PROGRAM_END)
    return s_runtime->content.c76_program[address - C76_MCU_PROGRAM_BASE];

  if (address >= C76_MCU_PROGRAM_MIRROR_BASE && address <= C76_MCU_PROGRAM_MIRROR_END)
    return s_runtime->content.c76_program[(address - C76_MCU_PROGRAM_MIRROR_BASE) & (C76_PROGRAM_ROM_SIZE - 1)];

  if (address >= C76_MCU_INPUT_BASE && address <= C76_MCU_INPUT_END)
  {
    // The C76 exposes four active-low 8-bit ports across paired byte addresses.
    // Their meaning is selected from the current game's System 11 input profile.
    switch ((address - C76_MCU_INPUT_BASE) >> 1)
    {
      case 0: return C76ReadPlayer4();
      case 1: return C76ReadSwitches();
      case 2: return C76ReadPlayer(0);
      case 3: return C76ReadPlayer(1);
      default: return 0xff;
    }
  }

  if (address >= C76_MCU_C352_BASE && address <= C76_MCU_C352_END)
  {
    if (!s_runtime->c76_first_c352_access_logged)
    {
      s_runtime->c76_first_c352_access_logged = true;
      DEV_LOG("System 11 C76 reached C352 register space at 0x{:06X}.", address);
    }
    return C352ReadByte(address);
  }

  if (s_runtime->content.has_family_bowl_io &&
      address >= C76_MCU_FAMILY_BOWL_IO_BASE && address <= C76_MCU_FAMILY_BOWL_IO_END)
  {
    // Family Bowl expects bit 7 asserted on this board-status window. The M139
    // command/response path itself is handled through C76 UART1 below.
    return 0x80;
  }

  if (!s_runtime->c76_first_unmapped_access_logged)
  {
    s_runtime->c76_first_unmapped_access_logged = true;
    DEV_LOG("System 11 C76 first unmapped read at 0x{:06X}.", address);
  }
  return 0xff;
}

static void C76WriteByte(u32 address, u8 value)
{
  if (!s_runtime.has_value())
    return;

  address &= 0x00ffffff;

  // Reserved addresses below 0x80 belong to the MCU's internal register area.
  if (address < 0x80)
    return;

  // These windows are the C76 sound-program ROM and its mirrors. Writes are ignored by the hardware map.
  if ((address >= C76_MCU_PROGRAM_BASE && address <= C76_MCU_PROGRAM_END) ||
      (address >= C76_MCU_PROGRAM_MIRROR_BASE && address <= C76_MCU_PROGRAM_MIRROR_END))
  {
    return;
  }

  if (address >= C76_MCU_SHARED_RAM_BASE && address <= C76_MCU_SHARED_RAM_END)
  {
    s_c76_shared_ram[address - C76_MCU_SHARED_RAM_BASE] = value;
    return;
  }

  if (address >= C76_MCU_C352_BASE && address <= C76_MCU_C352_END)
  {
    if (!s_runtime->c76_first_c352_access_logged)
    {
      s_runtime->c76_first_c352_access_logged = true;
      DEV_LOG("System 11 C76 reached C352 register space at 0x{:06X}.", address);
    }
    C352WriteByte(address, value);
    return;
  }

  // These board registers are write-only no-ops in the current System 11 map.
  if ((address >= 0x300000 && address <= 0x300001) ||
      (address >= 0x301000 && address <= 0x301001))
  {
    return;
  }

  if (s_runtime->content.has_family_bowl_io &&
      address >= C76_MCU_FAMILY_BOWL_IO_BASE && address <= C76_MCU_FAMILY_BOWL_IO_END)
  {
    return;
  }

  if (!s_runtime->c76_first_unmapped_access_logged)
  {
    s_runtime->c76_first_unmapped_access_logged = true;
    DEV_LOG("System 11 C76 first unmapped write at 0x{:06X} <- 0x{:02X}.", address, value);
  }
}

static void AccumulateC76Cycles(TickCount ticks)
{
  if (!s_runtime.has_value() || ticks <= 0)
    return;

  const u64 ticks_per_second = static_cast<u64>(std::max<TickCount>(System::GetTicksPerSecond(), 1));
  if (s_runtime->c76_last_ticks_per_second != 0 &&
      s_runtime->c76_last_ticks_per_second != ticks_per_second)
  {
    s_runtime->c76_cycle_fraction =
      (s_runtime->c76_cycle_fraction * ticks_per_second) / s_runtime->c76_last_ticks_per_second;
  }
  s_runtime->c76_last_ticks_per_second = ticks_per_second;

  const u64 numerator =
    (static_cast<u64>(static_cast<u32>(ticks)) * C76_CLOCK_HZ) + s_runtime->c76_cycle_fraction;
  const u64 cycles = numerator / ticks_per_second;
  s_runtime->c76_cycle_fraction = numerator % ticks_per_second;
  s_runtime->c76_cycle_balance += static_cast<s64>(cycles);
}

static void DeliverC76IRQ()
{
  if (!s_runtime.has_value() || !s_runtime->c76_cpu)
    return;

  const bool irq0 = s_c76_next_irq_is_irq0;
  const int line = irq0 ? M37710_LINE_IRQ0 : M37710_LINE_IRQ2;
  const u32 pc = s_runtime->c76_cpu->program_counter();

  // The imported core latches ASSERT_LINE and clears the pending line when the interrupt is serviced.
  s_runtime->c76_cpu->set_input(line, true);

  if (irq0)
    s_c76_irq0_count++;
  else
    s_c76_irq2_count++;

  if (s_c76_irq_logs < 8)
  {
    s_c76_irq_logs++;
    DEV_LOG("System 11 C76 {} asserted count={} pc=0x{:06X}.", irq0 ? "IRQ0" : "IRQ2",
             irq0 ? s_c76_irq0_count : s_c76_irq2_count, pc);
  }

  s_c76_next_irq_is_irq0 = !s_c76_next_irq_is_irq0;
  s_c76_irq_cycles_until_next = C76_IRQ_HALF_PERIOD_CYCLES;
}

static void RunPendingC76Cycles()
{
  if (!s_runtime.has_value() || !s_runtime->c76_cpu)
    return;

  while (s_runtime->c76_cycle_balance > 0)
  {
    if (s_c76_irq_cycles_until_next == 0)
      DeliverC76IRQ();

    const s64 cycles_to_irq = static_cast<s64>(s_c76_irq_cycles_until_next);
    const s64 requested =
      std::min<s64>(std::min<s64>(s_runtime->c76_cycle_balance, C76_MAX_EXECUTE_CYCLES), cycles_to_irq);
    const int requested_cycles = static_cast<int>(requested);
    const u32 pc_before = s_runtime->c76_cpu->program_counter();
    const int executed_cycles = s_runtime->c76_cpu->execute(requested_cycles);

    if (s_runtime->c76_slice_logs < 16)
    {
      s_runtime->c76_slice_logs++;
      DEV_LOG("System 11 C76 slice {} requested={} executed={} pc=0x{:06X}->0x{:06X}.",
               s_runtime->c76_slice_logs, requested_cycles, executed_cycles, pc_before,
               s_runtime->c76_cpu->program_counter());
    }

    if (executed_cycles <= 0)
      break;

    s_runtime->c76_total_cycles += static_cast<u64>(executed_cycles);
    s_runtime->c76_cycle_balance -= static_cast<s64>(executed_cycles);

    if (static_cast<u32>(executed_cycles) >= s_c76_irq_cycles_until_next)
      s_c76_irq_cycles_until_next = 0;
    else
      s_c76_irq_cycles_until_next -= static_cast<u32>(executed_cycles);

    if (executed_cycles < requested_cycles)
      break;
  }
}

struct FamilyBowlM139C3Words
{
  u16 w1 = 0;
  u16 w2 = 0;
  u16 w3 = 0;
  u16 w4 = 0;
  bool branch_b = false;
};

static s32 FamilyBowlM139SignedWord(u16 value)
{
  return (value & UINT16_C(0x8000)) ? (static_cast<s32>(value) - 0x10000) : static_cast<s32>(value);
}

static s32 FamilyBowlM139SignedByte(u8 value)
{
  return (value & UINT8_C(0x80)) ? (static_cast<s32>(value) - 0x100) : static_cast<s32>(value);
}

static s32 FamilyBowlM139ArithmeticShiftRightOne(s32 value)
{
  return (value >= 0) ? (value / 2) : -(((-value) + 1) / 2);
}

static u16 FamilyBowlM139MulShift10(u16 left, u16 right)
{
  return static_cast<u16>((static_cast<u32>(left) * static_cast<u32>(right)) >> 10);
}

static bool CalculateFamilyBowlM139C3(u32 sens1_time, u32 sens2_time, u32 sens3_time, u32 sens4_time,
                                     u8 calibration_left, u8 calibration_right,
                                     FamilyBowlM139C3Words* result)
{
  if (!result || sens1_time == 0 || sens2_time == 0 || sens3_time == 0 || sens4_time == 0 ||
      sens1_time < sens3_time || sens1_time < sens4_time ||
      sens2_time < sens3_time || sens2_time < sens4_time)
  {
    return false;
  }

  // fb1_spr0.ic5 0x0021DC. The H8/3002 firmware keeps these intermediates as
  // 16-bit words, so each assignment intentionally preserves word wrapping.
  const u16 d2 = static_cast<u16>(sens1_time - sens4_time);
  const u16 d4 = static_cast<u16>(sens2_time - sens4_time);
  const u16 d6 = static_cast<u16>(sens1_time - sens3_time);
  const u16 d8 = static_cast<u16>(sens2_time - sens3_time);
  if (d4 == 0 || d6 == 0)
    return false;

  u16 ratio = static_cast<u16>(((sens1_time - sens4_time) << 10) / d4);
  u16 x = static_cast<u16>(UINT16_C(0x0725) - ratio);
  u16 da = FamilyBowlM139MulShift10(x, UINT16_C(0x01A2));
  u16 dc = static_cast<u16>(UINT16_C(0x00FA) - FamilyBowlM139MulShift10(da, UINT16_C(0x00AC)));

  ratio = static_cast<u16>(((sens2_time - sens3_time) << 10) / d6);
  x = static_cast<u16>(ratio - UINT16_C(0x00DB));
  u16 de = FamilyBowlM139MulShift10(x, UINT16_C(0x01A2));
  u16 e0 = static_cast<u16>(FamilyBowlM139MulShift10(de, UINT16_C(0x00AC)) + UINT16_C(0x008C));

  ratio = static_cast<u16>(((sens2_time - sens3_time) << 10) / d4);
  x = static_cast<u16>(ratio - UINT16_C(0x00DB));
  u16 e2 = FamilyBowlM139MulShift10(x, UINT16_C(0x01A2));
  u16 e4 = static_cast<u16>(UINT16_C(0x006E) - FamilyBowlM139MulShift10(e2, UINT16_C(0x00AC)));

  ratio = static_cast<u16>(((sens1_time - sens4_time) << 10) / d6);
  x = static_cast<u16>(UINT16_C(0x0725) - ratio);
  u16 e6 = FamilyBowlM139MulShift10(x, UINT16_C(0x01A2));
  u16 e8 = FamilyBowlM139MulShift10(e6, UINT16_C(0x00AC));

  // fb1_spr0.ic5 0x00241A. B3 parameter 0 calibrates the SENS1/3 side and
  // parameter 1 calibrates the SENS2/4 side. The firmware accepts +/-30.
  const s32 cal_left = FamilyBowlM139SignedByte(calibration_left);
  const s32 cal_right = FamilyBowlM139SignedByte(calibration_right);
  if (cal_left >= -30 && cal_left <= 30 && cal_right >= -30 && cal_right <= 30)
  {
    da = static_cast<u16>(static_cast<s32>(da) - cal_left);
    de = static_cast<u16>(static_cast<s32>(de) - cal_left);
    e2 = static_cast<u16>(static_cast<s32>(e2) - cal_right);
    e6 = static_cast<u16>(static_cast<s32>(e6) - cal_right);
  }

  // fb1_spr0.ic5 0x0024A8. Select one of the two measured sides, then derive
  // the four big-endian words transmitted after C3.
  u16 left_x;
  u16 left_y;
  u16 right_x;
  u16 right_y;
  u16 divisor_word;
  if (FamilyBowlM139SignedWord(e0) >= FamilyBowlM139SignedWord(dc))
  {
    result->branch_b = true;
    left_x = de;
    left_y = e0;
    right_x = e6;
    right_y = e8;
    divisor_word = d4;
  }
  else
  {
    result->branch_b = false;
    left_x = da;
    left_y = dc;
    right_x = e2;
    right_y = e4;
    divisor_word = d6;
  }

  const s32 divisor = FamilyBowlM139SignedWord(divisor_word);
  if (divisor == 0)
    return false;

  const u16 dx_word = static_cast<u16>(left_x - right_x);
  const u16 dy_word = static_cast<u16>(left_y - right_y);
  result->w3 =
    static_cast<u16>((FamilyBowlM139SignedWord(dx_word) * 0x100) / divisor);
  result->w4 =
    static_cast<u16>((FamilyBowlM139SignedWord(dy_word) * 0x100) / divisor);

  const u16 sum_x = static_cast<u16>(left_x + right_x);
  const s32 average_x = FamilyBowlM139ArithmeticShiftRightOne(FamilyBowlM139SignedWord(sum_x));
  result->w1 =
    static_cast<u16>(average_x - 0x0148 + static_cast<u32>(dx_word));

  const u16 sum_y = static_cast<u16>(left_y + right_y);
  const s32 average_y = FamilyBowlM139ArithmeticShiftRightOne(FamilyBowlM139SignedWord(sum_y));
  const u16 correction = static_cast<u16>(right_y + UINT16_C(0x008C) - left_y);
  result->w2 = static_cast<u16>(average_y - static_cast<s32>(correction));

  (void)d2;
  (void)d8;
  return true;
}
static bool ReadFamilyBowlPSXState(std::array<u32, 8>* values)
{
  u16 half = 0;

  // These fields are read by the game's C0/C1/C2/C3/C5/C9 state machine.
  // They synchronize the HLE M139 event stream with the game's existing
  // ready/consume states; no PSX RAM is modified here.
  static constexpr std::array<u32, 8> addresses = {
    UINT32_C(0x802A1720), // C0/C1/C2 gate
    UINT32_C(0x802914F0), // C0/C1/C2 result
    UINT32_C(0x802EE14C), // completed C3 ready
    UINT32_C(0x802917C4), // C5 flag
    UINT32_C(0x802A16CC), // game state
    UINT32_C(0x8029C59C), // C9 state 1
    UINT32_C(0x8029C5E4), // C9 state 2
    UINT32_C(0x8029CABC), // C5 branch selector
  };

  for (u32 i = 0; i < addresses.size(); i++)
  {
    if (!CPU::SafeReadMemoryHalfWord(addresses[i], &half))
      return false;
    (*values)[i] = half;
  }

  return true;
}

static void C76SchedulerCallback(void*, TickCount ticks, TickCount)
{
  if (!s_runtime.has_value() || !s_runtime->c76_cpu)
    return;

  s_runtime->c76_scheduler_callbacks++;
  AccumulateC76Cycles(ticks);
  RunPendingC76Cycles();

  // The successful M139 SENS 7/Ball Detect path queues C9 C9. Deliver the two
  // bytes through the real C76 UART1 receive path, retrying automatically if
  // the UART still has an unread byte. Do not append E0 E0: that is the
  // separate M139 motor/mechanism timeout path.
  if (s_runtime->content.has_family_bowl_io && s_runtime->family_bowl_startup_c9_stage != 0 &&
      s_runtime->family_bowl_startup_c9_stage != 3)
  {
    if (s_runtime->family_bowl_startup_c9_delay_ms != 0)
    {
      s_runtime->family_bowl_startup_c9_delay_ms--;
    }
    else if (s_runtime->family_bowl_startup_c9_stage == 1)
    {
      if (s_runtime->c76_cpu->uart_receive_byte(1, UINT8_C(0xc9)))
      {
        s_runtime->family_bowl_startup_c9_stage = 2;
      }
    }
    else if (s_runtime->family_bowl_startup_c9_stage == 2)
    {
      if (s_runtime->c76_cpu->uart_receive_byte(1, UINT8_C(0xc9)))
      {
        s_runtime->family_bowl_startup_c9_stage = 3;
      }
    }
  }

  // The startup C9/C9 pair is consumed before live gameplay state exists.
  // Re-arm the firmware-derived Ball Detect event whenever the live ready state
  // returns to its unprimed signature, then wait for the game to acknowledge it.
  if (s_runtime->content.has_family_bowl_io &&
      s_runtime->family_bowl_startup_c9_stage == 3)
  {
    std::array<u32, 8> psx_state{};
    const bool state_valid = ReadFamilyBowlPSXState(&psx_state);


    // End-of-game physical-ball storage state captured from a complete game:
    //   gate=3 result=0 c3=0 c5=0 game=0102 c9_1=0 c9_2=1 c5_branch=0
    //
    // The screen simultaneously displays "ボールを中に戻してください"
    // ("Please return the ball inside."). This is distinct from the normal
    // gate=2/game=0080 ready cycle below.
    const bool end_game_ball_return_wait =
      state_valid &&
      psx_state[0] == UINT32_C(0x0003) &&
      psx_state[1] == UINT32_C(0x0000) &&
      psx_state[2] == UINT32_C(0x0000) &&
      psx_state[3] == UINT32_C(0x0000) &&
      psx_state[4] == UINT32_C(0x0102) &&
      psx_state[5] == UINT32_C(0x0000) &&
      psx_state[6] == UINT32_C(0x0001) &&
      psx_state[7] == UINT32_C(0x0000);

    const bool ready_unprimed =
      state_valid &&
      psx_state[0] == UINT32_C(0x0002) &&
      psx_state[1] == UINT32_C(0x0000) &&
      psx_state[2] == UINT32_C(0x0000) &&
      psx_state[3] == UINT32_C(0x0000) &&
      psx_state[4] == UINT32_C(0x0080) &&
      psx_state[5] == UINT32_C(0x0000) &&
      psx_state[6] == UINT32_C(0x0000) &&
      psx_state[7] == UINT32_C(0x0000);

    const bool ready_primed =
      state_valid &&
      psx_state[0] == UINT32_C(0x0002) &&
      psx_state[1] == UINT32_C(0x0000) &&
      psx_state[2] == UINT32_C(0x0000) &&
      psx_state[3] == UINT32_C(0x0000) &&
      psx_state[4] == UINT32_C(0x0080) &&
      psx_state[5] == UINT32_C(0x0000) &&
      psx_state[6] == UINT32_C(0x0001) &&
      psx_state[7] == UINT32_C(0x0001);

    if (s_runtime->family_bowl_ready_c9_stage == 4 && ready_unprimed)
    {
      s_runtime->family_bowl_ready_c9_stage = 1;
    }
    else if (s_runtime->family_bowl_ready_c9_stage == 0 && ready_unprimed)
    {
      s_runtime->family_bowl_ready_c9_stage = 1;
    }

    if (s_runtime->family_bowl_ready_c9_stage == 1)
    {
      if (s_runtime->c76_cpu->uart_receive_byte(1, UINT8_C(0xc9)))
      {
        s_runtime->family_bowl_ready_c9_stage = 2;
      }
    }
    else if (s_runtime->family_bowl_ready_c9_stage == 2)
    {
      if (s_runtime->c76_cpu->uart_receive_byte(1, UINT8_C(0xc9)))
      {
        s_runtime->family_bowl_ready_c9_stage = 3;
      }
    }
    else if (s_runtime->family_bowl_ready_c9_stage == 3 && ready_primed)
    {
      s_runtime->family_bowl_ready_c9_stage = 4;
    }

    // The physical cabinet holds the ball after a completed game and asks the
    // player to return it inside the machine. Once that terminal state is
    // reached, wait briefly to model the mechanical return and report the
    // firmware-derived SENS7/Ball Detect completion (C9 C9) through UART1.
    if (s_runtime->family_bowl_end_game_c9_stage == 0 && end_game_ball_return_wait)
    {
      s_runtime->family_bowl_end_game_c9_stage = 1;
      s_runtime->family_bowl_end_game_c9_delay_ms = 500;
    }

    if (s_runtime->family_bowl_end_game_c9_stage == 1)
    {
      if (s_runtime->family_bowl_end_game_c9_delay_ms != 0)
      {
        s_runtime->family_bowl_end_game_c9_delay_ms--;
      }
      else if (s_runtime->c76_cpu->uart_receive_byte(1, UINT8_C(0xc9)))
      {
        s_runtime->family_bowl_end_game_c9_stage = 2;
      }
    }
    else if (s_runtime->family_bowl_end_game_c9_stage == 2)
    {
      if (s_runtime->c76_cpu->uart_receive_byte(1, UINT8_C(0xc9)))
      {
        s_runtime->family_bowl_end_game_c9_stage = 3;
      }
    }
    else if (s_runtime->family_bowl_end_game_c9_stage == 3 && !end_game_ball_return_wait)
    {
      // Re-arm only after the PSX has consumed this occurrence and left the
      // terminal wait state, allowing subsequent complete games in one boot.
      s_runtime->family_bowl_end_game_c9_stage = 0;
      s_runtime->family_bowl_end_game_c9_delay_ms = 0;
    }
  }

  // After a trackball roll settles, synthesize a coherent four-sensor traversal
  // and run it through the recovered fb1_spr0.ic5 M139 trajectory equations.
  // Normal throws report C0/C1/C2 + C3 + four data words + 00. C5 is a
  // separately gated M139 event; emitting it unconditionally selects the game's
  // fixed gutter fallback instead of the native C3 trajectory.
  if (s_runtime->content.has_family_bowl_io)
  {
    constexpr s64 MIN_C3_THROW_Y = 80;
    constexpr s64 C3_IDLE_MS = 50;

    if (s_runtime->family_bowl_startup_c9_stage != 3)
    {
      std::lock_guard<std::mutex> lock(s_family_bowl_trackball_mutex);
      s_runtime->family_bowl_c3_measurement_enabled = false;
      s_runtime->family_bowl_c3_pending_x = 0;
      s_runtime->family_bowl_c3_pending_y = 0;
      s_runtime->family_bowl_c3_seen_generation = s_runtime->family_bowl_c3_generation;
      s_runtime->family_bowl_c3_idle_ms = 0;
    }
    else if (s_runtime->family_bowl_c3_measurement_enabled &&
             !s_runtime->family_bowl_c3_packet_armed)
    {
      s64 throw_dx = 0;
      s64 throw_dy = 0;
      bool commit_throw = false;

      // Keep the completed physical roll buffered until the PSX is in a
      // mechanism-primed ready state. For the first ball, the live game=0080
      // state must first consume the C9/C9 pair above:
      //
      // First ball after C9/C9 priming:
      //   gate=2 result=0 c3=0 c5=0 game=0080 c9_1=0 c9_2=1 c5_branch=1
      //
      // Later ball:
      //   gate=2 result=0 c3=0 c5=0 game=0105 c9_1=0 c9_2=1 c5_branch=1
      //
      // This gate only decides when to release an already-completed trackball
      // roll; it does not write or force any PSX state.
      std::array<u32, 8> psx_state{};
      const bool psx_state_valid = ReadFamilyBowlPSXState(&psx_state);

      const bool common_ready =
        psx_state_valid &&
        psx_state[0] == UINT32_C(0x0002) &&
        psx_state[1] == UINT32_C(0x0000) &&
        psx_state[2] == UINT32_C(0x0000) &&
        psx_state[3] == UINT32_C(0x0000) &&
        psx_state[5] == UINT32_C(0x0000);

      const bool first_ball_ready =
        common_ready &&
        s_runtime->family_bowl_ready_c9_stage == 4 &&
        psx_state[4] == UINT32_C(0x0080) &&
        psx_state[6] == UINT32_C(0x0001) &&
        psx_state[7] == UINT32_C(0x0001);

      const bool second_ball_ready =
        common_ready &&
        psx_state[4] == UINT32_C(0x0105) &&
        psx_state[6] == UINT32_C(0x0001) &&
        psx_state[7] == UINT32_C(0x0001);

      const bool psx_ready = first_ball_ready || second_ball_ready;

      {
        std::lock_guard<std::mutex> lock(s_family_bowl_trackball_mutex);

        if (s_runtime->family_bowl_c3_generation != s_runtime->family_bowl_c3_seen_generation)
        {
          s_runtime->family_bowl_c3_seen_generation = s_runtime->family_bowl_c3_generation;
          s_runtime->family_bowl_c3_idle_ms = static_cast<u32>(C3_IDLE_MS);
        }
        else if (s_runtime->family_bowl_c3_idle_ms != 0)
        {
          s_runtime->family_bowl_c3_idle_ms--;

          if (s_runtime->family_bowl_c3_idle_ms == 0)
          {
            throw_dx = s_runtime->family_bowl_c3_pending_x;
            throw_dy = s_runtime->family_bowl_c3_pending_y;

            // Forward motion is negative Y for the current Family Bowl
            // trackball presentation. Preserve a completed forward roll while
            // waiting for the exact PSX ready signature instead of consuming
            // it early.
            commit_throw = (throw_dy <= -MIN_C3_THROW_Y);
            if (commit_throw && psx_ready)
            {
              s_runtime->family_bowl_c3_pending_x = 0;
              s_runtime->family_bowl_c3_pending_y = 0;
              s_runtime->family_bowl_c3_measurement_enabled = false;

            }
            else if (commit_throw)
            {
              commit_throw = false;
              // Recheck the ready signature on the next 1 ms scheduler pass
              // without discarding the completed physical roll.
              s_runtime->family_bowl_c3_idle_ms = 1;

            }
            else
            {
              s_runtime->family_bowl_c3_pending_x = 0;
              s_runtime->family_bowl_c3_pending_y = 0;
            }
          }
        }
      }
      if (commit_throw)
      {
        // Convert the trackball roll into one coherent virtual SENS1..SENS4
        // traversal, then let the recovered M139 firmware arithmetic derive
        // the C3 words. This replaces the earlier approximation where horizontal
        // motion selected one of three unrelated speed profiles.
        //
        // The H8 validity checks require SENS1/SENS2 to occur no earlier than
        // SENS3/SENS4. A straight roll therefore uses two symmetric sensor
        // pairs separated by transit_ticks. Forward trackball magnitude sets
        // that pair-to-pair transit time using the physical inverse
        // relationship between travel time and speed.
        //
        // For steering, the two sensor pairs are skewed in opposite left/right
        // directions. The recovered M139 equations show that a 1:3 skew ratio
        // keeps W1 (initial lateral position) essentially centered while W3
        // becomes signed lateral velocity. This lets the cabinet's existing
        // left/right controls remain responsible for pre-shot positioning while
        // the trackball roll controls throw angle and speed.
        static constexpr u32 SENSOR_BASE_TIME = 1000;
        static constexpr u32 TRANSIT_NUMERATOR = 120000;
        static constexpr u32 MIN_TRANSIT_TICKS = 150;
        static constexpr u32 MAX_TRANSIT_TICKS = 600;
        static constexpr s64 MIN_SPEED_UNITS = 200;
        static constexpr s64 MAX_SPEED_UNITS = 1600;

        const s64 forward_magnitude = -throw_dy;
        const s64 speed_units =
          std::clamp<s64>(forward_magnitude, MIN_SPEED_UNITS, MAX_SPEED_UNITS);
        const u32 transit_ticks =
          std::clamp<u32>(static_cast<u32>(TRANSIT_NUMERATOR / speed_units),
                          MIN_TRANSIT_TICKS, MAX_TRANSIT_TICKS);

        // Normalize lateral motion against forward motion so the same physical
        // throw angle behaves consistently at different speeds. Keep enough
        // timing margin that every generated traversal satisfies the M139's
        // four timestamp-order checks.
        const s64 skew_denominator = std::max<s64>(forward_magnitude * 12, 1);
        const s64 unclamped_skew =
          (throw_dx * static_cast<s64>(transit_ticks)) / skew_denominator;
        const s32 max_pair_skew =
          static_cast<s32>(std::max<u32>((transit_ticks - 4) / 4, 1));
        const s32 pair_skew = static_cast<s32>(
          std::clamp<s64>(unclamped_skew, -static_cast<s64>(max_pair_skew),
                          static_cast<s64>(max_pair_skew)));

        const s32 sensor_1_time_s =
          static_cast<s32>(SENSOR_BASE_TIME + transit_ticks) + pair_skew;
        const s32 sensor_2_time_s =
          static_cast<s32>(SENSOR_BASE_TIME + transit_ticks) - pair_skew;
        const s32 sensor_3_time_s =
          static_cast<s32>(SENSOR_BASE_TIME) + (pair_skew * 3);
        const s32 sensor_4_time_s =
          static_cast<s32>(SENSOR_BASE_TIME) - (pair_skew * 3);

        const u32 sensor_1_time = static_cast<u32>(sensor_1_time_s);
        const u32 sensor_2_time = static_cast<u32>(sensor_2_time_s);
        const u32 sensor_3_time = static_cast<u32>(sensor_3_time_s);
        const u32 sensor_4_time = static_cast<u32>(sensor_4_time_s);

        // Feed the recovered M139 SENS1-SENS4 calculation into the C3 payload
        // so trackball direction and speed reach the native PSX trajectory path.
        //
        // SENS5 classification remains coupled to the same throw vector:
        //   strong left component  -> C0
        //   mostly straight        -> C1
        //   strong right component -> C2
        u8 classification_event = UINT8_C(0xc1);
        if ((throw_dx * 4) < -forward_magnitude)
        {
          classification_event = UINT8_C(0xc0);
        }
        else if ((throw_dx * 4) > forward_magnitude)
        {
          classification_event = UINT8_C(0xc2);
        }

        FamilyBowlM139C3Words words{};
        if (!CalculateFamilyBowlM139C3(sensor_1_time, sensor_2_time,
                                       sensor_3_time, sensor_4_time,
                                       s_runtime->family_bowl_m139_params[0],
                                       s_runtime->family_bowl_m139_params[1], &words))
        {
          ERROR_LOG("Family Bowl trackball sensor model: recovered trajectory calculation rejected "
                    "dx={} dy={} transit_ticks={} skew={} timestamps=[{}, {}, {}, {}].",
                    throw_dx, throw_dy, transit_ticks, pair_skew,
                    sensor_1_time, sensor_2_time, sensor_3_time, sensor_4_time);
          s_runtime->family_bowl_c3_measurement_enabled = true;
        }
        else
        {
          s_runtime->family_bowl_c3_packet = {
            classification_event,
            UINT8_C(0xc3),
            static_cast<u8>(words.w1 >> 8), static_cast<u8>(words.w1),
            static_cast<u8>(words.w2 >> 8), static_cast<u8>(words.w2),
            static_cast<u8>(words.w3 >> 8), static_cast<u8>(words.w3),
            static_cast<u8>(words.w4 >> 8), static_cast<u8>(words.w4),
            UINT8_C(0x00),
          };
          s_runtime->family_bowl_c3_packet_index = 0;
          s_runtime->family_bowl_c3_packet_armed = true;
        }
      }
    }

    if (s_runtime->family_bowl_c3_packet_armed &&
        s_runtime->family_bowl_c3_packet_index < s_runtime->family_bowl_c3_packet.size())
    {
      const u8 event_byte = s_runtime->family_bowl_c3_packet[s_runtime->family_bowl_c3_packet_index];
      if (s_runtime->c76_cpu->uart_receive_byte(1, event_byte))
      {
        s_runtime->family_bowl_c3_packet_index++;
        if (s_runtime->family_bowl_c3_packet_index >= s_runtime->family_bowl_c3_packet.size())
        {
          s_runtime->family_bowl_c3_packet_armed = false;
          s_runtime->family_bowl_c3_packet_index = 0;
        }
      }
    }
  }
}

static constexpr u32 EEPROM_BASE = 0x30000;
static constexpr u32 EEPROM_END = 0x30fff;
static constexpr size_t EEPROM_SIZE = 0x800;
static std::array<u8, EEPROM_SIZE> s_eeprom{};

static u64 GetEEPROMNow()
{
  return static_cast<u64>(System::GetGlobalTickCounter());
}

static u64 GetEEPROMWriteCycleTicks()
{
  // MAME's AT28C16 model and the fast-write device specification use a
  // 200 microsecond self-timed byte-programming cycle.
  const u64 ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());
  return std::max<u64>((ticks_per_second + 4999) / 5000, 1);
}

static void UpdateEEPROMBusy(RuntimeState& runtime)
{
  if (runtime.eeprom_busy && GetEEPROMNow() >= runtime.eeprom_busy_until)
    runtime.eeprom_busy = false;
}

static u8 ReadEEPROMByte(RuntimeState& runtime, u32 offset)
{
  UpdateEEPROMBusy(runtime);

  // During programming, AT28C16 data polling returns the last programmed
  // value with I/O7 inverted until the write cycle completes.
  return runtime.eeprom_busy ? static_cast<u8>(runtime.eeprom_last_write ^ UINT8_C(0x80)) : s_eeprom[offset];
}

static void WriteEEPROMByte(RuntimeState& runtime, u32 offset, u8 value)
{
  UpdateEEPROMBusy(runtime);

  // The device does not accept another byte program while its self-timed
  // write cycle is active.
  if (runtime.eeprom_busy)
    return;

  u8& byte = s_eeprom[offset];
  if (byte == value)
    return;

  byte = value;
  runtime.eeprom_last_write = value;
  runtime.eeprom_busy = true;
  runtime.eeprom_busy_until = GetEEPROMNow() + GetEEPROMWriteCycleTicks();
  runtime.eeprom_dirty = true;
}

static bool LoadEEPROM(RuntimeState& runtime, Error* error)
{
  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  runtime.persistence_directory = Path::Combine(nvram_root, runtime.content.set_name);
  runtime.eeprom_path = Path::Combine(runtime.persistence_directory, "at28c16");
  runtime.eeprom_dirty = false;
  runtime.eeprom_busy = false;
  runtime.eeprom_last_write = UINT8_C(0xff);
  runtime.eeprom_busy_until = 0;
  s_eeprom.fill(0xff);

  const bool persistence_exists = FileSystem::FileExists(runtime.eeprom_path.c_str());
  if (persistence_exists)
  {
    std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(runtime.eeprom_path.c_str(), error);
    if (!data || data->size() != EEPROM_SIZE)
    {
      const size_t actual_size = data ? data->size() : 0;
      Error::SetStringFmt(error, "Namco System 11 AT28C16 '{}' has invalid size {}; expected {} bytes.",
                          runtime.eeprom_path, actual_size, EEPROM_SIZE);
      ERROR_LOG("System 11 AT28C16 malformed persistence path='{}' actual_size={} expected_size={}.",
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
      Error::SetStringFmt(error, "Failed to create Namco System 11 AT28C16 persistence '{}'.", runtime.eeprom_path);
      ERROR_LOG("System 11 AT28C16 persistence create failed canonical_set='{}' path='{}'.",
                runtime.content.set_name, runtime.eeprom_path);
      return false;
    }
  }

  VERBOSE_LOG("System 11 AT28C16 {} canonical_set='{}' path='{}'.",
           persistence_exists ? "loaded" : "initialized", runtime.content.set_name, runtime.eeprom_path);
  return true;
}

static void SaveEEPROM(RuntimeState& runtime)
{
  if (!runtime.eeprom_dirty)
    return;

  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  if (!FileSystem::CreateDirectory(nvram_root.c_str(), false) ||
      !FileSystem::CreateDirectory(runtime.persistence_directory.c_str(), false) ||
      !FileSystem::WriteBinaryFile(runtime.eeprom_path.c_str(), s_eeprom.data(), s_eeprom.size()))
  {
    ERROR_LOG("System 11 AT28C16 save failed canonical_set='{}' path='{}'.",
              runtime.content.set_name, runtime.eeprom_path);
    return;
  }

  runtime.eeprom_dirty = false;
  VERBOSE_LOG("System 11 AT28C16 saved canonical_set='{}' path='{}'.",
           runtime.content.set_name, runtime.eeprom_path);
}

void GenerateAudioFrame(s32* left, s32* right)
{
  if (!left || !right)
    return;

  *left = 0;
  *right = 0;
  if (!s_runtime.has_value())
    return;

  // The System 11 C352 produces 88.2 kHz while the PlayStation SPU host stream
  // is fixed at 44.1 kHz. Advance two native C352 ticks for each host frame and
  // average the pair as a minimal 2:1 low-pass/downsample. This conversion is
  // host integration rather than C352 hardware behavior.
  s32 left0 = 0;
  s32 right0 = 0;
  s32 left1 = 0;
  s32 right1 = 0;
  C352GenerateNativeFrame(&left0, &right0);
  C352GenerateNativeFrame(&left1, &right1);

  *left = (left0 + left1) / 2;
  *right = (right0 + right1) / 2;
}

bool Initialize(LoadedContent content, Error* error)
{
  if (content.set_name.empty() || content.program_rom.size() != PROGRAM_ROM_SIZE ||
      content.banked_rom.size() != ((content.keycus_type == KeycusType::C443) ? BANKED_ROM64_SIZE : BANKED_ROM_SIZE) ||
      content.c76_internal.size() != C76_INTERNAL_ROM_SIZE || content.c76_program.size() != C76_PROGRAM_ROM_SIZE ||
      content.c352_samples.size() != C352_SAMPLE_ROM_SIZE ||
      (content.has_family_bowl_io && content.iomcu_program.size() != IOMCU_PROGRAM_ROM_SIZE))
  {
    Error::SetStringView(error, "Invalid Namco System 11 ROM content.");
    return false;
  }
  if (!Bus::g_bios)
  {
    Error::SetStringView(error, "System 11 cannot install its program ROM before Bus initialization.");
    return false;
  }

  // The PSX core keeps the reset-vector BIOS window as a direct 512 KiB allocation.
  // Keep its first window synchronized while the rest of the 4 MiB program ROM remains handler-backed.
  std::memcpy(Bus::g_bios, content.program_rom.data(), Bus::BIOS_SIZE);

  RuntimeState runtime;
  runtime.content = std::move(content);
  runtime.bank_entries.fill(0);
  s_runtime = std::move(runtime);
  s_c76_shared_ram.fill(0);
  if (!LoadEEPROM(*s_runtime, error))
  {
    s_runtime.reset();
    return false;
  }
  s_c76_irq_cycles_until_next = 0;
  s_c76_next_irq_is_irq0 = true;
  s_c76_irq0_count = 0;
  s_c76_irq2_count = 0;
  s_c76_irq_logs = 0;

  s_runtime->c76_cpu = std::make_unique<m37710_cpu_device>();
  if (!s_runtime->c76_cpu->load_internal_rom(s_runtime->content.c76_internal.data(),
                                              s_runtime->content.c76_internal.size()))
  {
    Error::SetStringView(error, "Failed to load the Namco C76 internal ROM into the M37702 core.");
    s_runtime.reset();
    return false;
  }

  s_runtime->c76_cpu->set_memory_callbacks(C76ReadByte, C76WriteByte, C76ReadWord, C76WriteWord);

  if (s_runtime->content.has_family_bowl_io)
  {
    s_runtime->c76_cpu->set_uart_trace_callback(
      [](unsigned uart, bool write, u8 reg, u16 value) {
        if (!s_runtime.has_value())
          return;

        if (write && uart == 1 && reg == 0x3a && s_runtime->c76_cpu)
        {
          const u8 tx = static_cast<u8>(value);

          // fb1_spr0.ic5 uses B3 as a two-parameter command. Keep the parameters
          // visible so the real startup configuration can be reproduced later.
          if (s_runtime->family_bowl_m139_param_bytes_remaining != 0)
          {
            const u32 param_index =
              static_cast<u32>(s_runtime->family_bowl_m139_params.size()) -
              s_runtime->family_bowl_m139_param_bytes_remaining;
            if (param_index < s_runtime->family_bowl_m139_params.size())
              s_runtime->family_bowl_m139_params[param_index] = tx;

            s_runtime->family_bowl_m139_param_bytes_remaining--;

            if (s_runtime->family_bowl_m139_param_bytes_remaining == 0)
            {
              // The startup stream sends B3 00 00 once after the initial A9
              // commands. The firmware-derived successful Ball Detect event is
              // C9 C9, so arm the startup event after setup completes.
              if (s_runtime->family_bowl_m139_pending_command == UINT8_C(0xb3) &&
                  s_runtime->family_bowl_startup_c9_stage == 0)
              {
                s_runtime->family_bowl_startup_c9_stage = 1;
                s_runtime->family_bowl_startup_c9_delay_ms = 250;
              }

              s_runtime->family_bowl_m139_pending_command = 0;
            }
          }
          else
          {

            switch (tx)
            {
              case 0xa7:
              {
                // The M139 H8 firmware queues 0xD0 in direct response to A7.
                s_runtime->c76_cpu->uart_receive_byte(1, UINT8_C(0xd0));
                break;
              }

              case 0xb1:
              {
                // INPUT TEST polls B1 at about 30 Hz. Confirmed mapping is active-low:
                // bit 0..6 = SENS 1..7, and 0x7F means all sensors OFF.
                //
                // For the first trackball-driven test, emulate a ball crossing the
                // center near sensor (SENS 2), then one of SENS 4/5/6 at the far end.
                // Horizontal roll component selects the far sensor; vertical magnitude
                // controls the delay between the two crossings.
                constexpr u8 SENSOR_IDLE = UINT8_C(0x7f);
                constexpr u8 SENSOR_2 = UINT8_C(0x7d);
                constexpr u8 SENSOR_4 = UINT8_C(0x77);
                constexpr u8 SENSOR_5 = UINT8_C(0x6f);
                constexpr u8 SENSOR_6 = UINT8_C(0x5f);
                constexpr u32 ACTIVE_POLLS = 2;
                constexpr u32 SETTLE_POLLS = 2;
                constexpr s64 MIN_THROW_Y = 20;

                u8 sensor_state = SENSOR_IDLE;

                {
                  std::lock_guard<std::mutex> lock(s_family_bowl_trackball_mutex);

                  if (s_runtime->family_bowl_sensor_sequence_stage != 0)
                  {
                    switch (s_runtime->family_bowl_sensor_sequence_stage)
                    {
                      case 1:
                        sensor_state = s_runtime->family_bowl_sensor_sequence_first;
                        break;

                      case 2:
                        sensor_state = SENSOR_IDLE;
                        break;

                      case 3:
                        sensor_state = s_runtime->family_bowl_sensor_sequence_second;
                        break;

                      default:
                        s_runtime->family_bowl_sensor_sequence_stage = 0;
                        sensor_state = SENSOR_IDLE;
                        break;
                    }

                    if (s_runtime->family_bowl_sensor_sequence_polls_remaining > 0)
                      s_runtime->family_bowl_sensor_sequence_polls_remaining--;

                    if (s_runtime->family_bowl_sensor_sequence_polls_remaining == 0)
                    {
                      if (s_runtime->family_bowl_sensor_sequence_stage == 1)
                      {
                        s_runtime->family_bowl_sensor_sequence_stage = 2;
                        // The precomputed travel gap is transferred below.
                      }
                      else if (s_runtime->family_bowl_sensor_sequence_stage == 2)
                      {
                        s_runtime->family_bowl_sensor_sequence_stage = 3;
                        s_runtime->family_bowl_sensor_sequence_polls_remaining = ACTIVE_POLLS;
                      }
                      else
                      {
                        // Keep this final poll active, then return to idle on the
                        // following B1 request.
                        s_runtime->family_bowl_sensor_sequence_stage = 0;
                      }
                    }
                  }
                  else
                  {
                    if (s_runtime->family_bowl_trackball_generation !=
                        s_runtime->family_bowl_trackball_seen_generation)
                    {
                      s_runtime->family_bowl_trackball_seen_generation =
                        s_runtime->family_bowl_trackball_generation;
                      s_runtime->family_bowl_trackball_settle_polls = SETTLE_POLLS;
                    }
                    else if (s_runtime->family_bowl_trackball_settle_polls != 0)
                    {
                      s_runtime->family_bowl_trackball_settle_polls--;

                      if (s_runtime->family_bowl_trackball_settle_polls == 0)
                      {
                        const s64 dx = s_runtime->family_bowl_trackball_pending_x;
                        const s64 dy = s_runtime->family_bowl_trackball_pending_y;
                        const s64 abs_y = (dy < 0) ? -dy : dy;

                        if (abs_y >= MIN_THROW_Y)
                        {
                          // A noticeable horizontal component selects left/right.
                          // The existing per-game invert-X setting can reverse this
                          // without changing the Family Bowl hardware model.
                          u8 far_sensor = SENSOR_5;
                          if ((dx * 4) < -abs_y)
                          {
                            far_sensor = SENSOR_4;
                          }
                          else if ((dx * 4) > abs_y)
                          {
                            far_sensor = SENSOR_6;
                          }

                          // B1 is ~30 Hz. Stronger/faster forward rolls shorten the
                          // travel time between near and far sensors.
                          const u32 speed = static_cast<u32>(std::min<s64>(abs_y, 300));
                          const u32 travel_polls = std::clamp<u32>(30 - (speed / 10), 4, 28);

                          s_runtime->family_bowl_sensor_sequence_first = SENSOR_2;
                          s_runtime->family_bowl_sensor_sequence_second = far_sensor;
                          s_runtime->family_bowl_sensor_sequence_stage = 1;
                          s_runtime->family_bowl_sensor_sequence_polls_remaining = ACTIVE_POLLS;

                          // Store the travel gap in settle_polls while the sequence
                          // is active; stage 1 transfers it into polls_remaining.
                          s_runtime->family_bowl_trackball_settle_polls = travel_polls;
                        }

                        s_runtime->family_bowl_trackball_pending_x = 0;
                        s_runtime->family_bowl_trackball_pending_y = 0;
                      }
                    }
                  }

                  // Stage transition from the first sensor into the travel gap.
                  if (s_runtime->family_bowl_sensor_sequence_stage == 2 &&
                      s_runtime->family_bowl_sensor_sequence_polls_remaining == 0)
                  {
                    s_runtime->family_bowl_sensor_sequence_polls_remaining =
                      std::max<u32>(s_runtime->family_bowl_trackball_settle_polls, 1);
                    s_runtime->family_bowl_trackball_settle_polls = 0;
                  }
                }

                s_runtime->c76_cpu->uart_receive_byte(1, sensor_state);
                break;
              }

              case 0xb3:
                s_runtime->family_bowl_m139_pending_command = UINT8_C(0xb3);
                s_runtime->family_bowl_m139_param_bytes_remaining = 2;
                break;

              case 0xa0:
              {
                std::lock_guard<std::mutex> lock(s_family_bowl_trackball_mutex);
                if (!s_runtime->family_bowl_c3_packet_armed)
                {
                  s_runtime->family_bowl_c3_measurement_enabled = true;
                  s_runtime->family_bowl_c3_pending_x = 0;
                  s_runtime->family_bowl_c3_pending_y = 0;
                  s_runtime->family_bowl_c3_seen_generation = s_runtime->family_bowl_c3_generation;
                  s_runtime->family_bowl_c3_idle_ms = 0;
                }
                break;
              }
              case 0xa9:
              case 0xb4:
              case 0xb5:
              case 0xb6:
                break;

              default:
                DEV_LOG("Family Bowl M139 HLE: unhandled command 0x{:02X}.", tx);
                break;
            }
          }
        }

      });

  }

  // The C76 drives the System 11 main-board reset through P8.1. The real
  // firmware parks the PSX side, then generates a driven high -> low edge.
  // Observe the physical GPIO edge for every System 11 title rather than
  // special-casing Pocket Racer or a shared-RAM address.
  s_runtime->c76_cpu->set_port_callbacks(
    8, {},
    [](u8 value) {
      if (!s_runtime.has_value() || !s_runtime->c76_cpu)
        return;

      constexpr u8 MAINBOARD_RESET_BIT = UINT8_C(0x02);
      const u8 direction = s_runtime->c76_cpu->port_direction(8);
      const u8 old_driven = s_runtime->c76_cpu->port_data_latch(8) & direction;
      const u8 new_driven = value & direction;

      if ((old_driven & MAINBOARD_RESET_BIT) != 0 &&
          (new_driven & MAINBOARD_RESET_BIT) == 0 &&
          !s_runtime->mainboard_reset_pending)
      {
        s_runtime->mainboard_reset_pending = true;
        DEV_LOG("System 11 C76 P8.1 main-board reset edge: P8={:02X}->{:02X} dir={:02X} "
                 "c76_pc={:06X} psx_pc={:08X}.",
                 old_driven, new_driven, direction, s_runtime->c76_cpu->program_counter(), CPU::g_state.pc);
      }
    });
  // All eight C76 A/D inputs are connected on System 11. Unused active-low inputs idle at 0xFF;
  // leaving a callback unbound makes the standalone M37702 core convert it as 0x0000 instead.
  for (u32 channel = 0; channel < 8; channel++)
    s_runtime->c76_cpu->set_analog_callback(channel, []() -> u16 { return UINT16_C(0x00ff); });

  switch (GetC76InputProfile())
  {
    case C76InputProfile::Standard:
      // The standard System 11 profile carries Player 3 through the eight C76 A/D channels.
      s_runtime->c76_cpu->set_analog_callback(0, []() -> u16 { return C76ReadDigitalADC(2, "Button3"); });
      s_runtime->c76_cpu->set_analog_callback(1, []() -> u16 { return C76ReadDigitalADC(2, "Button2"); });
      s_runtime->c76_cpu->set_analog_callback(2, []() -> u16 { return C76ReadDigitalADC(2, "Button1"); });
      s_runtime->c76_cpu->set_analog_callback(3, []() -> u16 { return C76ReadDigitalADC(2, "Right"); });
      s_runtime->c76_cpu->set_analog_callback(4, []() -> u16 { return C76ReadDigitalADC(2, "Left"); });
      s_runtime->c76_cpu->set_analog_callback(5, []() -> u16 { return C76ReadDigitalADC(2, "Down"); });
      s_runtime->c76_cpu->set_analog_callback(6, []() -> u16 { return C76ReadDigitalADC(2, "Up"); });
      s_runtime->c76_cpu->set_analog_callback(7, []() -> u16 { return C76ReadDigitalADC(2, "Start"); });
      break;

    case C76InputProfile::Tekken:
      s_runtime->c76_cpu->set_analog_callback(1, []() -> u16 { return C76ReadDigitalADC(0, "Button4"); });
      s_runtime->c76_cpu->set_analog_callback(2, []() -> u16 { return C76ReadDigitalADC(0, "Button3"); });
      break;

    case C76InputProfile::SoulEdge:
      s_runtime->c76_cpu->set_analog_callback(2, []() -> u16 { return C76ReadDigitalADC(0, "Button4"); });
      break;

    case C76InputProfile::PocketRacer:
      s_runtime->c76_cpu->set_analog_callback(
        0, []() -> u16 { return C76ScaleAnalog(ArcadeInput::GetAnalogValue(0, "Steering"), 0x38, 0xc8, true); });
      s_runtime->c76_cpu->set_analog_callback(
        1, []() -> u16 { return C76ScaleAnalog(ArcadeInput::GetAnalogValue(0, "Accelerator"), 0x00, 0x7f, true); });
      break;

    case C76InputProfile::MyAngel3:
    case C76InputProfile::PointBlank2:
      break;
  }

  s_runtime->c76_cpu->reset();
  s_runtime->c76_timing_event =
    std::make_unique<TimingEvent>("Namco System 11 C76", C76_SCHEDULER_INTERVAL_TICKS,
                                  C76_SCHEDULER_INTERVAL_TICKS, C76SchedulerCallback, nullptr);
  s_runtime->c76_timing_event->SetPeriodAndSchedule(C76_SCHEDULER_INTERVAL_TICKS);

  VERBOSE_LOG("Namco System 11 memory initialized. C76 core='M37702' clock_hz={} reset_pc=0x{:06X}.",
           C76_CLOCK_HZ, s_runtime->c76_cpu->program_counter());
  return true;
}

void Reset()
{
  if (!s_runtime.has_value())
    return;

  s_runtime->bank_entries.fill(0);
  s_runtime->rom8_64_bank_offset = 0;
  s_runtime->keycus_p1 = 0;
  s_runtime->keycus_p2 = 0;
  s_runtime->keycus_p3 = 0;
  s_runtime->c352_voices = {};
  s_runtime->c352_control = 0;
  s_runtime->c352_random = 0x1234;
  s_runtime->c352_first_keyon_logged = false;
  s_runtime->family_bowl_m139_pending_command = 0;
  s_runtime->family_bowl_m139_param_bytes_remaining = 0;
  s_runtime->family_bowl_m139_params.fill(0);
  s_runtime->family_bowl_startup_c9_stage = 0;
  s_runtime->family_bowl_startup_c9_delay_ms = 0;
  s_runtime->family_bowl_ready_c9_stage = 0;

  {
    std::lock_guard<std::mutex> lock(s_family_bowl_trackball_mutex);
    s_runtime->family_bowl_c3_pending_x = 0;
    s_runtime->family_bowl_c3_pending_y = 0;
    s_runtime->family_bowl_c3_generation = 0;
    s_runtime->family_bowl_c3_seen_generation = 0;
    s_runtime->family_bowl_c3_idle_ms = 0;
    s_runtime->family_bowl_c3_measurement_enabled = false;
    s_runtime->family_bowl_c3_packet_armed = false;
    s_runtime->family_bowl_c3_packet_index = 0;
    s_runtime->family_bowl_c3_packet.fill(0);
    s_runtime->family_bowl_trackball_pending_x = 0;
    s_runtime->family_bowl_trackball_pending_y = 0;
    s_runtime->family_bowl_trackball_generation = 0;
    s_runtime->family_bowl_trackball_seen_generation = 0;
    s_runtime->family_bowl_trackball_settle_polls = 0;
    s_runtime->family_bowl_sensor_sequence_stage = 0;
    s_runtime->family_bowl_sensor_sequence_polls_remaining = 0;
    s_runtime->family_bowl_sensor_sequence_first = UINT8_C(0x7f);
    s_runtime->family_bowl_sensor_sequence_second = UINT8_C(0x7f);
  }

  if (s_runtime->c76_timing_event && s_runtime->c76_timing_event->IsActive())
    s_runtime->c76_timing_event->Deactivate();

  s_runtime->c76_cycle_fraction = 0;
  s_runtime->c76_last_ticks_per_second = 0;
  s_runtime->c76_cycle_balance = 0;
  s_runtime->c76_total_cycles = 0;
  s_runtime->c76_scheduler_callbacks = 0;
  s_runtime->c76_slice_logs = 0;
  s_runtime->c76_first_c352_access_logged = false;
  s_runtime->c76_first_unmapped_access_logged = false;
  s_runtime->mainboard_reset_pending = false;
  s_c76_irq_cycles_until_next = 0;
  s_c76_next_irq_is_irq0 = true;
  s_c76_irq0_count = 0;
  s_c76_irq2_count = 0;
  s_c76_irq_logs = 0;

  if (s_runtime->c76_cpu)
  {
    s_runtime->c76_cpu->reset();
    if (s_runtime->c76_timing_event)
      s_runtime->c76_timing_event->SetPeriodAndSchedule(C76_SCHEDULER_INTERVAL_TICKS);
  }
}

void Shutdown()
{
  if (!s_runtime.has_value())
    return;

  if (s_runtime->c76_timing_event && s_runtime->c76_timing_event->IsActive())
    s_runtime->c76_timing_event->Deactivate();

  if (s_runtime->c76_cpu)
  {
    DEV_LOG("System 11 C76 shutdown callbacks={} total_cycles={} cycle_balance={} irq0={} irq2={} pc=0x{:06X}.",
             s_runtime->c76_scheduler_callbacks, s_runtime->c76_total_cycles,
             s_runtime->c76_cycle_balance, s_c76_irq0_count, s_c76_irq2_count,
             s_runtime->c76_cpu->program_counter());
  }

  SaveEEPROM(*s_runtime);
  s_runtime.reset();
}

bool IsActive()
{
  return s_runtime.has_value();
}

bool BeginMainBoardReset()
{
  if (!s_runtime.has_value() || !s_runtime->mainboard_reset_pending)
    return false;

  s_runtime->mainboard_reset_pending = false;

  if (s_runtime->c76_timing_event && s_runtime->c76_timing_event->IsActive())
    s_runtime->c76_timing_event->Deactivate();

  // System::InternalReset() resets global PSX timing immediately afterward.
  // Drop the host scheduler remainder while the C76 event is suspended.
  s_runtime->c76_cycle_fraction = 0;
  s_runtime->c76_last_ticks_per_second = 0;
  s_runtime->c76_cycle_balance = 0;
  return true;
}

void EndMainBoardReset()
{
  if (!s_runtime.has_value())
    return;

  // The P8.1 edge is a board reset, not a PSX-only reset. Reuse the normal
  // System 11 reset path after System::InternalReset() has reset global timing.
  // Reset() restarts the C76 from its reset vector, resets C352/KEYCUS/banking
  // state and C76 IRQ phase, and schedules a fresh C76 timing event. Shared SRAM
  // and persistent EEPROM contents remain intact, matching the existing reset path.
  Reset();

  DEV_LOG("System 11 C76 full-board reset completed; C76 restarted at pc=0x{:06X}.",
           s_runtime->c76_cpu ? s_runtime->c76_cpu->program_counter() : 0);
}


bool IsFamilyBowlActive()
{
  return s_runtime.has_value() && s_runtime->content.has_family_bowl_io;
}

void AddFamilyBowlTrackballDelta(u32 port, s32 delta_x, s32 delta_y)
{
  if (!IsFamilyBowlActive() || port != 0 || (delta_x == 0 && delta_y == 0))
    return;

  std::lock_guard<std::mutex> lock(s_family_bowl_trackball_mutex);

  static constexpr s64 PENDING_LIMIT = 0x10000000;
  s_runtime->family_bowl_trackball_pending_x =
    std::clamp(s_runtime->family_bowl_trackball_pending_x + static_cast<s64>(delta_x), -PENDING_LIMIT, PENDING_LIMIT);
  s_runtime->family_bowl_trackball_pending_y =
    std::clamp(s_runtime->family_bowl_trackball_pending_y + static_cast<s64>(delta_y), -PENDING_LIMIT, PENDING_LIMIT);

  // Gameplay C3 data is accepted only while A0 has enabled measurement.
  // Negative Y is forward motion. Positive/backward Y is ignored completely.
  if (s_runtime->family_bowl_c3_measurement_enabled)
  {
    bool c3_motion_changed = false;

    if (delta_y < 0)
    {
      s_runtime->family_bowl_c3_pending_y =
        std::clamp(s_runtime->family_bowl_c3_pending_y + static_cast<s64>(delta_y),
                   -PENDING_LIMIT, PENDING_LIMIT);
      c3_motion_changed = true;
    }

    if (delta_x != 0 && s_runtime->family_bowl_c3_pending_y < 0)
    {
      s_runtime->family_bowl_c3_pending_x =
        std::clamp(s_runtime->family_bowl_c3_pending_x + static_cast<s64>(delta_x),
                   -PENDING_LIMIT, PENDING_LIMIT);
      c3_motion_changed = true;
    }

    if (c3_motion_changed)
      s_runtime->family_bowl_c3_generation++;
  }
  s_runtime->family_bowl_trackball_generation++;

}

u32 ReadC76SharedRAM(u32 width, u32 offset)
{
  if (!s_runtime.has_value() || offset < C76_SHARED_RAM_BASE || offset > C76_SHARED_RAM_END)
    return UINT32_C(0xFFFFFFFF);

  return ReadBytes(std::span<const u8>(s_c76_shared_ram), width, offset - C76_SHARED_RAM_BASE);
}

void WriteC76SharedRAM(u32 width, u32 offset, u32 value)
{
  if (!s_runtime.has_value() || offset < C76_SHARED_RAM_BASE || offset > C76_SHARED_RAM_END)
    return;

  const size_t write_offset = static_cast<size_t>(offset - C76_SHARED_RAM_BASE);
  if ((width != 1 && width != 2 && width != 4) || write_offset > s_c76_shared_ram.size() ||
      static_cast<size_t>(width) > (s_c76_shared_ram.size() - write_offset))
  {
    return;
  }

  // The PSX and C76 share this SRAM. Preserve byte lanes by writing only the width requested by the CPU.
  std::memcpy(&s_c76_shared_ram[write_offset], &value, width);
}

u32 ReadEEPROM(u32 width, u32 offset)
{
  if (!s_runtime.has_value() || offset < EEPROM_BASE || offset > EEPROM_END ||
      (width != 1 && width != 2 && width != 4))
  {
    return UINT32_C(0xFFFFFFFF);
  }

  const u32 relative_offset = offset - EEPROM_BASE;
  u32 value = UINT32_C(0xFFFFFFFF);

  // MAME maps the 8-bit AT28C16 on byte lanes 0 and 2 of the 32-bit PSX bus.
  for (u32 i = 0; i < width && (relative_offset + i) <= (EEPROM_END - EEPROM_BASE); i++)
  {
    const u32 bus_offset = relative_offset + i;
    if ((bus_offset & 1) == 0)
    {
      const u32 eeprom_offset = bus_offset >> 1;
      value = (value & ~(UINT32_C(0xFF) << (i * 8))) |
              (static_cast<u32>(ReadEEPROMByte(*s_runtime, eeprom_offset)) << (i * 8));
    }
  }


  return value;
}

void WriteEEPROM(u32 width, u32 offset, u32 value)
{
  if (!s_runtime.has_value() || offset < EEPROM_BASE || offset > EEPROM_END ||
      (width != 1 && width != 2 && width != 4))
  {
    return;
  }

  const u32 relative_offset = offset - EEPROM_BASE;


  for (u32 i = 0; i < width && (relative_offset + i) <= (EEPROM_END - EEPROM_BASE); i++)
  {
    const u32 bus_offset = relative_offset + i;
    if ((bus_offset & 1) == 0)
      WriteEEPROMByte(*s_runtime, bus_offset >> 1, static_cast<u8>(value >> (i * 8)));
  }
}
static u16 DecimalDigit(u16 value, u32 divisor)
{
  return static_cast<u16>((value / divisor) % 10);
}

static u16 ReadKEYCUSRegister(u32 reg)
{
  if (!s_runtime.has_value())
    return 0;

  const u16 p1 = s_runtime->keycus_p1;
  const u16 p2 = s_runtime->keycus_p2;
  const u16 p3 = s_runtime->keycus_p3;

  // MAME documents the KEYCUS challenge/response algorithms. SYSTEM11_MiSTer independently
  // implements the same logic and returns zero for unexpected reads instead of random data.
  switch (s_runtime->content.keycus_type)
  {
    case KeycusType::C406:
      if (reg == 0 && p1 == 0x1234 && p2 == 0x5678 && p3 == 0x000f)
        return 0x3256;
      break;

    case KeycusType::C409:
      if (reg == 7)
      {
        const int a2 = (static_cast<int>(p1) - 0x01) & 0x1f;
        const int a3 = (0x20 - static_cast<int>(p1)) & 0x1f;
        const int r = (((p2 & 0x1f) * a2) + ((p3 & 0x1f) * a3)) / 0x1f;
        const int g = ((((p2 >> 5) & 0x1f) * a2) + (((p3 >> 5) & 0x1f) * a3)) / 0x1f;
        const int b = ((((p2 >> 10) & 0x1f) * a2) + (((p3 >> 10) & 0x1f) * a3)) / 0x1f;
        return static_cast<u16>(r | (g << 5) | (b << 10));
      }
      break;

    case KeycusType::C410:
      if (p2 == 0)
      {
        const u16 value = (p1 == 0xfffe) ? 410 : p1;
        if (reg == 1)
          return DecimalDigit(value, 1);
        if (reg == 2)
          return static_cast<u16>(DecimalDigit(value, 100) | (DecimalDigit(value, 1000) << 8));
        if (reg == 3)
          return static_cast<u16>(DecimalDigit(value, 10000) | (DecimalDigit(value, 10) << 8));
      }
      break;

    case KeycusType::C411:
      if (p2 == 0 && (((p1 == 0 || p1 == 0x0100) && p3 == 0xff7f) || p1 == 0x7256))
      {
        const u16 value = (p1 == 0x7256) ? p3 : 411;
        if (reg == 0)
          return static_cast<u16>((DecimalDigit(value, 10) << 8) | DecimalDigit(value, 1));
        if (reg == 2)
          return static_cast<u16>((DecimalDigit(value, 1000) << 8) | DecimalDigit(value, 100));
        if (reg == 8)
          return DecimalDigit(value, 10000);
      }
      break;

    case KeycusType::C430:
      if (p2 == 0 && ((p1 == 0xbfff && p3 == 0) || p3 == 0xe296))
      {
        const u16 value = (p3 == 0xe296) ? p1 : 430;
        if (reg == 1)
          return DecimalDigit(value, 10000);
        if (reg == 4)
          return static_cast<u16>(DecimalDigit(value, 100) | (DecimalDigit(value, 1000) << 8));
        if (reg == 5)
          return static_cast<u16>(DecimalDigit(value, 1) | (DecimalDigit(value, 10) << 8));
      }
      break;

    case KeycusType::C431:
      if (p2 == 0 && (((p1 == 0 || p1 == 0xab50) && p3 == 0x7fff) || p1 == 0x9e61))
      {
        const u16 value = (p1 == 0x9e61) ? p3 : 431;
        if (reg == 0)
          return static_cast<u16>(DecimalDigit(value, 1) | (DecimalDigit(value, 10) << 8));
        if (reg == 4)
          return static_cast<u16>(DecimalDigit(value, 100) | (DecimalDigit(value, 1000) << 8));
        if (reg == 8)
          return DecimalDigit(value, 10000);
      }
      break;

    case KeycusType::C432:
      if (p1 == 0 && (((p3 == 0 || p3 == 0x00dc) && p2 == 0xefff) || p3 == 0x2f15))
      {
        const u16 value = (p3 == 0x2f15) ? p2 : 432;
        if (reg == 2)
          return static_cast<u16>(DecimalDigit(value, 1) | (DecimalDigit(value, 10) << 8));
        if (reg == 4)
          return static_cast<u16>(DecimalDigit(value, 100) | (DecimalDigit(value, 1000) << 8));
        if (reg == 6)
          return static_cast<u16>(DecimalDigit(value, 10000) | (DecimalDigit(value, 100000) << 8));
      }
      break;

    case KeycusType::C442:
      if (reg == 1 && p1 == 0x0020 && p2 == 0x0021)
        return 0xc442;
      break;

    case KeycusType::C443:
      if (reg == 0 && p1 == 0x0020 && (p2 == 0 || p2 == 0xffff || p2 == 0xffe0))
        return 0x0020;
      if (reg == 1 && p1 == 0x0020 && (p2 == 0xffff || p2 == 0xffe0))
        return 0xc443;
      break;

    case KeycusType::None:
      break;
  }

  return 0;
}

static void WriteKEYCUSRegister(u32 reg, u16 data)
{
  if (!s_runtime.has_value())
    return;

  switch (s_runtime->content.keycus_type)
  {
    case KeycusType::C406:
      if (reg == 1)
        s_runtime->keycus_p1 = data;
      else if (reg == 2)
        s_runtime->keycus_p2 = data;
      else if (reg == 3)
        s_runtime->keycus_p3 = data;
      break;

    case KeycusType::C409:
      if (reg == 1)
        s_runtime->keycus_p1 = data;
      else if (reg == 3)
        s_runtime->keycus_p2 = data;
      else if (reg == 7)
        s_runtime->keycus_p3 = data;
      break;

    case KeycusType::C410:
      if (reg == 0)
        s_runtime->keycus_p1 = data;
      else if (reg == 2)
        s_runtime->keycus_p2 = data;
      break;

    case KeycusType::C411:
      if (reg == 2)
        s_runtime->keycus_p1 = data;
      else if (reg == 8)
        s_runtime->keycus_p2 = data;
      else if (reg == 10)
        s_runtime->keycus_p3 = data;
      break;

    case KeycusType::C430:
      if (reg == 0)
        s_runtime->keycus_p1 = data;
      else if (reg == 1)
        s_runtime->keycus_p2 = data;
      else if (reg == 4)
        s_runtime->keycus_p3 = data;
      break;

    case KeycusType::C431:
      if (reg == 0)
        s_runtime->keycus_p1 = data;
      else if (reg == 4)
        s_runtime->keycus_p2 = data;
      else if (reg == 12)
        s_runtime->keycus_p3 = data;
      break;

    case KeycusType::C432:
      if (reg == 0)
        s_runtime->keycus_p1 = data;
      else if (reg == 2)
        s_runtime->keycus_p2 = data;
      else if (reg == 6)
        s_runtime->keycus_p3 = data;
      break;

    case KeycusType::C442:
    case KeycusType::C443:
      if (reg == 0)
        s_runtime->keycus_p1 = data;
      else if (reg == 1)
        s_runtime->keycus_p2 = data;
      break;

    case KeycusType::None:
      break;
  }
}

u32 ReadKEYCUS(u32 width, u32 offset)
{
  if (!s_runtime.has_value() || offset < KEYCUS_BASE || offset > KEYCUS_END ||
      (width != 1 && width != 2 && width != 4))
  {
    return UINT32_C(0xFFFFFFFF);
  }

  const u32 relative_offset = offset - KEYCUS_BASE;
  u32 value = 0;
  for (u32 i = 0; i < width && (relative_offset + i) <= (KEYCUS_END - KEYCUS_BASE); i++)
  {
    const u32 byte_offset = relative_offset + i;
    const u16 register_value = ReadKEYCUSRegister(byte_offset >> 1);
    const u32 byte_value = (register_value >> ((byte_offset & 1) * 8)) & 0xff;
    value |= byte_value << (i * 8);
  }

  return value;
}

void WriteKEYCUS(u32 width, u32 offset, u32 value)
{
  if (!s_runtime.has_value() || offset < KEYCUS_BASE || offset > KEYCUS_END)
    return;

  // The KEYCUS is a 16-bit device. A 32-bit PSX store addresses two adjacent registers.
  if ((offset & 1) != 0 || (width != 2 && width != 4))
  {
    DEV_LOG("System 11 KEYCUS write with unsupported width/address: width={} offset=0x{:X} value=0x{:08X}.",
                width, offset, value);
    return;
  }

  const u32 relative_offset = offset - KEYCUS_BASE;
  for (u32 i = 0; i < width && (relative_offset + i + 1) <= (KEYCUS_END - KEYCUS_BASE); i += 2)
    WriteKEYCUSRegister((relative_offset + i) >> 1, static_cast<u16>(value >> (i * 8)));
}
u32 ReadProgramROM(u32 width, u32 offset)
{
  if (!s_runtime.has_value())
    return UINT32_C(0xFFFFFFFF);

  return ReadBytes(s_runtime->content.program_rom, width, offset);
}

static u16 ScaleGunAxis(float normalized, u16 min_value, u16 max_value)
{
  normalized = std::clamp(normalized, 0.0f, 1.0f);
  return static_cast<u16>(static_cast<float>(min_value) +
                          (normalized * static_cast<float>(max_value - min_value)) + 0.5f);
}

static u32 ReadGunInterface(u32 width, u32 offset)
{
  static constexpr u32 GUN_BASE = 0x780000;
  static constexpr u16 GUN_X_MIN = 0x00d8;
  static constexpr u16 GUN_X_MAX = 0x0387;
  static constexpr u16 GUN_Y_MIN = 0x002c;
  static constexpr u16 GUN_Y_MAX = 0x011b;

  if (width != 1 && width != 2 && width != 4)
    return UINT32_C(0xFFFFFFFF);

  const ArcadeInput::LightgunPresentationState state = ArcadeInput::GetLightgunPresentationState();
  const u16 p1_x = ScaleGunAxis(state.ports[0].x, GUN_X_MIN, GUN_X_MAX);
  const u16 p1_y = ScaleGunAxis(state.ports[0].y, GUN_Y_MIN, GUN_Y_MAX);
  const u16 p2_x = ScaleGunAxis(state.ports[1].x, GUN_X_MIN, GUN_X_MAX);
  const u16 p2_y = ScaleGunAxis(state.ports[1].y, GUN_Y_MIN, GUN_Y_MAX);

  // MAME's GUN I/F register order is P1 X, unused, P1 Y, P1 Y+1,
  // P2 X, unused, P2 Y, P2 Y+1. MiSTer independently implements the same layout.
  const std::array<u16, 8> registers = {p1_x, 0, p1_y, static_cast<u16>(p1_y + 1),
                                         p2_x, 0, p2_y, static_cast<u16>(p2_y + 1)};

  const u32 relative_offset = offset - GUN_BASE;
  u32 value = 0;
  for (u32 i = 0; i < width; i++)
  {
    const u32 byte_offset = relative_offset + i;
    if (byte_offset >= 0x10)
      break;

    const u16 reg = registers[byte_offset >> 1];
    const u8 byte = static_cast<u8>(reg >> ((byte_offset & 1) * 8));
    value |= static_cast<u32>(byte) << (i * 8);
  }

  return value;
}

u32 ReadBankedROM(u32 width, u32 offset)
{
  if (!s_runtime.has_value() || offset >= Bus::EXP1_SIZE)
    return UINT32_C(0xFFFFFFFF);

  // The Point Blank 2 GUN I/F board overlays 0x1F780000-0x1F78000F on top of ROM8 bank 8.
  if (s_runtime->content.has_gun_interface && offset >= 0x780000 && offset <= 0x78000f)
    return ReadGunInterface(width, offset);

  const u32 bank = offset / BANK_WINDOW_SIZE;
  const u32 bank_offset = offset & (BANK_WINDOW_SIZE - 1);
  const u32 rom_offset =
    (static_cast<u32>(s_runtime->bank_entries[bank]) * BANK_WINDOW_SIZE) + bank_offset;

  return ReadBytes(s_runtime->content.banked_rom, width, rom_offset);
}

void WriteBankRegister(u32 width, u32 offset, u32 value)
{

  if (!s_runtime.has_value() || offset < BANK_REGISTER_BASE || offset > BANK_REGISTER_END)
    return;

  if (width != 2 || (offset & 1) != 0)
  {
    DEV_LOG("System 11 ROM8 bank write with unsupported width/address: width={} offset=0x{:X} value=0x{:08X}.",
                width, offset, value);
    return;
  }

  const u32 bank = (offset - BANK_REGISTER_BASE) / 2;
  const u16 data = static_cast<u16>(value);

  if (s_runtime->content.keycus_type == KeycusType::C443)
  {
    // ROM8(64) extends the page number to five bits. MAME marks the exact decode
    // as needing verification; SYSTEM11_MiSTer independently uses the same write-time XOR.
    s_runtime->bank_entries[bank] =
      static_cast<u8>((((data & 0xc0) >> 3) + (data & 0x07)) ^ s_runtime->rom8_64_bank_offset);
  }
  else
  {
    // Standard ROM8: bits 6-7 choose a group of four 1 MiB entries, bits 0-1 choose within that group.
    s_runtime->bank_entries[bank] = static_cast<u8>(((data & 0xc0) >> 4) + (data & 0x03));
  }
}

void WriteGunOutput(u32 width, u32 offset, u32 value)
{
  if (!s_runtime.has_value() || !s_runtime->content.has_gun_interface ||
      offset < 0x788000 || offset > 0x788003)
  {
    return;
  }

  // The output register is the first 16-bit word in the GUN I/F window.
  // The second word begins the light-gun read sequence and is not a cabinet output.
  if (offset != 0x788000)
    return;

  // MAME's System 11 driver identifies the low nibble as active-low:
  // bit 3 = P1 Start lamp (led0), bit 2 = P2 Start lamp (led1),
  // bit 1 = P1 recoil (recoil0), bit 0 = P2 recoil (recoil1).
  // Preserve MAME's output names so later external transports can be drop-in compatible.
  const u8 data = static_cast<u8>(value);
  const u8 changed =
    s_runtime->gun_output_latch_valid ? static_cast<u8>((s_runtime->gun_output_latch ^ data) & 0x0f) : UINT8_C(0x0f);

  if (changed != 0)
  {
    if (changed & 0x08)
      Arcade::Output::SetValue("led0", (data & 0x08) ? 0 : 1, "Namco System 11 GUN I/F");
    if (changed & 0x04)
      Arcade::Output::SetValue("led1", (data & 0x04) ? 0 : 1, "Namco System 11 GUN I/F");
    if (changed & 0x02)
      Arcade::Output::SetValue("recoil0", (data & 0x02) ? 0 : 1, "Namco System 11 GUN I/F");
    if (changed & 0x01)
      Arcade::Output::SetValue("recoil1", (data & 0x01) ? 0 : 1, "Namco System 11 GUN I/F");

    s_runtime->gun_output_latch = data;
    s_runtime->gun_output_latch_valid = true;
  }

  (void)width;
}
void WriteBankUpperRegister(u32 width, u32 offset, u32 value)
{

  static constexpr u32 ROM8_64_UPPER_BASE = 0x080000;
  static constexpr u32 ROM8_64_UPPER_END = 0x080003;

  if (!s_runtime.has_value() || offset < ROM8_64_UPPER_BASE || offset > ROM8_64_UPPER_END)
    return;

  if (s_runtime->content.keycus_type != KeycusType::C443)
  {
    DEV_LOG("System 11 ROM8(64) upper-bank write on non-C443 game: offset=0x{:X} value=0x{:08X}.",
                offset, value);
    return;
  }

  if (width == 4)
  {
    if (offset != ROM8_64_UPPER_BASE)
    {
      DEV_LOG("System 11 ROM8(64) upper-bank word write at unsupported address: offset=0x{:X}.", offset);
      return;
    }

    // A full-word store covers both halfwords. MAME invokes offset 0 then 1, so the upper half wins.
    s_runtime->rom8_64_bank_offset = 16;
    return;
  }

  if ((width != 1 && width != 2) || (width == 2 && (offset & 1) != 0))
  {
    DEV_LOG("System 11 ROM8(64) upper-bank write with unsupported width/address: width={} offset=0x{:X}.",
                width, offset);
    return;
  }

  // The register ignores data. Address 0x1F080000/1 selects the lower 16 MiB;
  // 0x1F080002/3 selects the upper 16 MiB.
  s_runtime->rom8_64_bank_offset = ((offset - ROM8_64_UPPER_BASE) >= 2) ? 16 : 0;
}

} // namespace NamcoSystem11
