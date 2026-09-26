// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/sony/zn/sony_zn.h"

#include "core/arcade/arcade_database.h"
#include "core/arcade/arcade_input.h"
#include "core/arcade/arcade_output.h"
#include "core/arcade/devices/security/cat702.h"
#include "core/arcade/devices/storage/chd_hard_disk.h"
#include "core/arcade/systems/sony/zn/capcom_qsound.h"
#include "core/arcade/systems/sony/zn/acclaim_ata.h"
#include "core/arcade/systems/sony/zn/acclaim_rax.h"
#include "core/arcade/systems/sony/zn/time_warner_ata.h"
#include "core/arcade/systems/sony/zn/atlus_zn1_sound.h"
#include "core/arcade/systems/sony/zn/eighting_raizing_sound.h"
#include "core/arcade/systems/sony/zn/taito_fx1a_sound.h"
#include "core/arcade/systems/sony/zn/taito_fx1b_zoom.h"
#include "core/arcade/systems/sony/zn/tecmo_cbaj_sound.h"
#include "core/arcade/systems/sony/zn/tecmo_gr2_link.h"
#include "core/bus.h"
#include "core/cpu_code_cache.h"
#include "core/cpu_core.h"
#include "core/gpu.h"
#include "core/settings.h"
#include "core/system.h"
#include "core/interrupt_controller.h"
#include "core/timing_event.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/md5_digest.h"
#include "common/minizip_helpers.h"
#include "common/sha1_digest.h"
#include "common/string_util.h"

#include "libchdr/chd.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <utility>

Log_SetChannel(SonyZN);

namespace SonyZN {
namespace {

enum class BoardType : u8
{
  CapcomZN1,
  VideoSystemZN1,
  AtlusZN1,
  EightingRaizingZN1,
  BustAMove2ZN1,
  AcclaimZN1,
  TimeWarnerZN1,
  TaitoFX1A,
  TaitoFX1B,
  TecmoTPS,
};

static constexpr u32 CAPCOM_FIXED_ROM_SIZE = 0x400000;
static constexpr u32 CAPCOM_BANK_SIZE = 0x400000;
static constexpr u32 CAPCOM_EXP1_SIZE = 0x800000;
static constexpr u32 VIDEO_SYSTEM_FIXED_ROM_SIZE = 0x280000;
static constexpr u32 VIDEO_SYSTEM_BANK_SIZE = 0x100000;
static constexpr u32 VIDEO_SYSTEM_BANK_COUNT = 24;
static constexpr u32 VIDEO_SYSTEM_BANKED_ROM_SIZE = VIDEO_SYSTEM_BANK_COUNT * VIDEO_SYSTEM_BANK_SIZE;
static constexpr u32 VIDEO_SYSTEM_BANK_BASE = 0x100000;
static constexpr u32 VIDEO_SYSTEM_BANK_END = 0x1fffff;
static constexpr u32 ATLUS_BANK_SIZE = 0x800000;
static constexpr u32 ATLUS_EXP1_SIZE = 0x800000;
static constexpr u32 ATLUS_BANK_COUNT = 4;
static constexpr u32 ATLUS_BANKED_ROM_SIZE = ATLUS_BANK_COUNT * ATLUS_BANK_SIZE;
static constexpr u32 ATLUS_AUDIO_ROM_SIZE = 0x40000;
static constexpr u32 ATLUS_YMZ280B_ROM_SIZE = 0x800000;
static constexpr u32 EIGHTING_BANK_SIZE = 0x800000;
static constexpr u32 EIGHTING_EXP1_SIZE = 0x800000;
static constexpr u32 EIGHTING_BANK_COUNT = 3;
static constexpr u32 EIGHTING_BANKED_ROM_SIZE = EIGHTING_BANK_COUNT * EIGHTING_BANK_SIZE;
static constexpr u32 EIGHTING_AUDIO_ROM_SIZE = 0x80000;
static constexpr u32 EIGHTING_AUDIO_ROM_MAX_SIZE = 0x100000;
static constexpr u32 EIGHTING_PS9805_FLASH_SIZE = 0x400000;
static constexpr u32 EIGHTING_YMF271_ROM_SIZE = 0x400000;
static constexpr u32 BAM2_FIXED_ROM_SIZE = 0x400000;
static constexpr u32 BAM2_BANK_SIZE = 0x400000;
static constexpr u32 BAM2_EXP1_SIZE = 0x800000;
static constexpr u32 BAM2_BANKED_ROM_SIZE = 0x2c00000;
static constexpr u32 BAM2_BANK_SELECT_COUNT = 16;
static constexpr u32 BAM2_PCM_FILE_COUNT = 39;
static constexpr u32 BAM2_PCM_MIRROR_COUNT = 4;
static constexpr u32 BAM2_SECTOR_SIZE = 512;
static constexpr u32 BAM2_CD_PCM_FILE_COUNT = 27;
static constexpr u32 BAM2_CD_RAW_FRAME_SIZE = 2448;
static constexpr u32 BAM2_CD_DATA_SECTOR_SIZE = 2048;
static constexpr u32 BAM2_CD_MODE1_DATA_OFFSET = 16;
static constexpr u32 BAM2_PCM_BUFFER_SIZE = 0x20000;
static constexpr u32 BAM2_PCM_PREFETCH_BYTES_PER_FRAME = 0x1000;
static constexpr u32 TAITO_BANK_SIZE = 0x800000;
static constexpr u32 TAITO_EXP1_SIZE = 0x800000;
static constexpr u32 TECMO_BANK_SIZE = 0x800000;
static constexpr u32 TECMO_EXP1_SIZE = 0x800000;
static constexpr u32 TECMO_BANK_COUNT = 9;
static constexpr u32 TECMO_BANKED_ROM_SIZE = TECMO_BANK_COUNT * TECMO_BANK_SIZE;
static constexpr u32 TECMO_CBAJ_AUDIO_ROM_SIZE = 0x40000;
static constexpr u32 TECMO_CBAJ_YMZ_ROM_SIZE = 0x800000;
static constexpr u32 TECMO_GR2_LINK_ROM_SIZE = 0x40000;
static constexpr u32 ACCLAIM_NBA_BANK_SIZE = 0x200000;
static constexpr u32 ACCLAIM_EXP1_SIZE = 0x800000;
static constexpr u32 ACCLAIM_JDREDD_ROM_SIZE = 0x200000;
static constexpr u32 TIME_WARNER_PROGRAM_ROM_SIZE = 0x200000;
static constexpr u32 ACCLAIM_NBA_SRAM_BASE = 0x200000;
static constexpr size_t ACCLAIM_NBA_SRAM_SIZE = 0x8000;
static constexpr u32 ACCLAIM_NBA_SOUND_STATUS = 0x1fff08;
static constexpr u32 ACCLAIM_NBA_SOUND_LATCH = 0x1fff80;

static constexpr u32 P1_BASE = 0x000000;
static constexpr u32 P2_BASE = 0x000100;
static constexpr u32 SERVICE_BASE = 0x000200;
static constexpr u32 SYSTEM_BASE = 0x000300;
static constexpr u32 P3_BASE = 0x010000;
static constexpr u32 P4_BASE = 0x010100;
static constexpr u32 BOARD_CONFIG = 0x010200;
static constexpr u32 SECURITY_SELECT = 0x010300;
static constexpr u32 COIN_REGISTER = 0x020000;
static constexpr u32 COMMON_NOP_RW = 0x030000;
static constexpr u32 COMMON_NOP_R = 0x040000;
static constexpr u32 AT28_BASE = 0x0f0000;
static constexpr u32 AT28_END = 0x0f07ff;
static constexpr size_t AT28_SIZE = 0x800;

static constexpr u32 CAPCOM_BANK_REGISTER = 0x100000;
static constexpr u32 VIDEO_SYSTEM_BANK_REGISTER = 0x100000;
static constexpr u32 ATLUS_SOUND_LATCH = 0x100000;
static constexpr u32 ATLUS_BANK_REGISTER = 0x100002;
static constexpr u32 EIGHTING_SOUND_LATCH = 0x100000;
static constexpr u32 EIGHTING_SOUND_IRQ = 0x100004;
static constexpr u32 BAM2_MCU_BASE = 0x100000;
static constexpr u32 BAM2_MCU_END = 0x100007;
static constexpr u32 TECMO_CBAJ_SOUND_DATA = 0x100000;
static constexpr u32 TECMO_CBAJ_SOUND_STATUS = 0x100003;
static constexpr u32 TECMO_GR2_LINK_IRQ = 0x100004;
static constexpr u32 TECMO_BANK_REGISTER = 0x100006;
static constexpr u32 TAITO_BANK_REGISTER = 0x140000;
static constexpr u32 UNKNOWN_BASE = 0x120000;
static constexpr u32 UNKNOWN_END = 0x120007;
static constexpr u32 CAPCOM_KICK1_BASE = 0x140010;
static constexpr u32 CAPCOM_KICK1_END = 0x140013;
static constexpr u32 CAPCOM_KICK2_BASE = 0x140020;
static constexpr u32 CAPCOM_KICK2_END = 0x140023;
static constexpr u32 CAPCOM_QSOUND_LATCH = 0x160000;
static constexpr u32 ACCLAIM_BANK_REGISTER = 0x1fff00;
static constexpr u32 ACCLAIM_ACPSX10_REGISTER = 0x1fff12;
static constexpr u32 TAITO_SOUND_PORT = 0x180000;
static constexpr u32 TAITO_SOUND_COMM = 0x180002;
static constexpr u32 TAITO_FX1B_FRAM_BASE = 0x100000;
static constexpr u32 TAITO_FX1B_FRAM_END = 0x1003ff;
static constexpr size_t TAITO_FX1B_FRAM_SIZE = 0x200;
static constexpr u32 TAITO_ZOOM_DATA = 0x180000;
static constexpr u32 TAITO_ZOOM_ADDRESS = 0x180002;
static constexpr u32 TAITO_ZOOM_IRQ_WRITE = 0x1a0000;
static constexpr u32 TAITO_ZOOM_IRQ_READ = 0x1c0000;
static constexpr u32 TAITO_ZOOM_SHARED_BASE = 0x1e0000;
static constexpr u32 TAITO_ZOOM_SHARED_END = 0x1e01ff;
static constexpr size_t TAITO_ZOOM_SHARED_SIZE = 0x100;
static constexpr u32 CAPCOM_COUNTRY_BASE = 0x180000;
static constexpr u32 CAPCOM_COUNTRY_END = 0x1fffff;

struct BAM2PCMFile
{
  u32 size = 0;
  std::vector<u32> clusters;
};

struct BAM2CDPCMFile
{
  u32 extent_lba = 0;
  u32 size = 0;
};

struct BAM2CDROMImage
{
  chd_file* chd = nullptr;
  std::string path;
  u32 frame_count = 0;
  u32 frames_per_hunk = 0;
  std::vector<u8> hunk_buffer;
  u32 current_hunk = UINT32_MAX;

  BAM2CDROMImage() = default;
  BAM2CDROMImage(const BAM2CDROMImage&) = delete;
  BAM2CDROMImage& operator=(const BAM2CDROMImage&) = delete;

  BAM2CDROMImage(BAM2CDROMImage&& other) noexcept
    : chd(std::exchange(other.chd, nullptr)), path(std::move(other.path)), frame_count(other.frame_count),
      frames_per_hunk(other.frames_per_hunk), hunk_buffer(std::move(other.hunk_buffer)), current_hunk(other.current_hunk)
  {
    other.frame_count = 0;
    other.frames_per_hunk = 0;
    other.current_hunk = UINT32_MAX;
  }

  BAM2CDROMImage& operator=(BAM2CDROMImage&& other) noexcept
  {
    if (this == &other)
      return *this;

    Close();
    chd = std::exchange(other.chd, nullptr);
    path = std::move(other.path);
    frame_count = other.frame_count;
    frames_per_hunk = other.frames_per_hunk;
    hunk_buffer = std::move(other.hunk_buffer);
    current_hunk = other.current_hunk;
    other.frame_count = 0;
    other.frames_per_hunk = 0;
    other.current_hunk = UINT32_MAX;
    return *this;
  }

  ~BAM2CDROMImage()
  {
    Close();
  }

  void Close()
  {
    if (chd)
      chd_close(chd);
    chd = nullptr;
    path.clear();
    frame_count = 0;
    frames_per_hunk = 0;
    hunk_buffer.clear();
    current_hunk = UINT32_MAX;
  }

  bool Open(const std::string& filename, Error* error)
  {
    Close();

    auto fp = FileSystem::OpenManagedSharedCFile(filename.c_str(), "rb", FileSystem::FileShareMode::DenyWrite, error);
    if (!fp)
      return false;

    chd_file* opened = nullptr;
    const chd_error open_error = chd_open_file(fp.get(), CHD_OPEN_READ | CHD_OPEN_TRANSFER_FILE, nullptr, &opened);
    if (open_error != CHDERR_NONE)
    {
      Error::SetStringFmt(error, "Failed to open Bust a Move 2 CD-ROM CHD '{}': {}",
                          Path::GetFileName(filename), chd_error_string(open_error));
      return false;
    }
    fp.release();

    const chd_header* const header = chd_get_header(opened);
    if (!header || header->hunkbytes == 0 || header->unitbytes != BAM2_CD_RAW_FRAME_SIZE ||
        (header->hunkbytes % BAM2_CD_RAW_FRAME_SIZE) != 0)
    {
      chd_close(opened);
      Error::SetStringFmt(error, "Bust a Move 2 CD-ROM CHD '{}' has an unsupported frame layout.",
                          Path::GetFileName(filename));
      return false;
    }

    std::array<char, 256> metadata{};
    u32 metadata_length = 0;
    const chd_error metadata_error =
      chd_get_metadata(opened, CDROM_TRACK_METADATA2_TAG, 0, metadata.data(),
                       static_cast<u32>(metadata.size() - 1), &metadata_length, nullptr, nullptr);
    if (metadata_error != CHDERR_NONE)
    {
      chd_close(opened);
      Error::SetStringFmt(error, "Bust a Move 2 CD-ROM CHD '{}' is missing track metadata: {}",
                          Path::GetFileName(filename), chd_error_string(metadata_error));
      return false;
    }
    metadata[std::min<u32>(metadata_length, static_cast<u32>(metadata.size() - 1))] = '\0';

    int track = 0;
    int frames = 0;
    int pregap = 0;
    int postgap = 0;
    std::array<char, 32> type{};
    std::array<char, 32> subtype{};
    std::array<char, 32> pregap_type{};
    std::array<char, 32> pregap_subtype{};
    if (std::sscanf(metadata.data(),
                    "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
                    &track, type.data(), subtype.data(), &frames, &pregap, pregap_type.data(), pregap_subtype.data(),
                    &postgap) != 8 ||
        track != 1 || std::strcmp(type.data(), "MODE1_RAW") != 0 || frames <= 0 || pregap != 0 || postgap != 0)
    {
      chd_close(opened);
      Error::SetStringFmt(error, "Bust a Move 2 CD-ROM CHD '{}' does not contain the expected single MODE1_RAW track.",
                          Path::GetFileName(filename));
      return false;
    }

    chd = opened;
    path = filename;
    frame_count = static_cast<u32>(frames);
    frames_per_hunk = header->hunkbytes / BAM2_CD_RAW_FRAME_SIZE;
    hunk_buffer.resize(header->hunkbytes);
    current_hunk = UINT32_MAX;
    return true;
  }

  bool ReadSector2048(u32 lba, u8* destination)
  {
    if (!chd || !destination || lba >= frame_count || frames_per_hunk == 0)
      return false;

    const u32 hunk = lba / frames_per_hunk;
    if (current_hunk != hunk)
    {
      const chd_error read_error = chd_read(chd, hunk, hunk_buffer.data());
      if (read_error != CHDERR_NONE)
      {
        ERROR_LOG("SonyZN.BAM2 CD-ROM hunk read failed path='{}' hunk={} error='{}'", path, hunk,
                  chd_error_string(read_error));
        return false;
      }
      current_hunk = hunk;
    }

    const u32 frame_in_hunk = lba % frames_per_hunk;
    const size_t frame_offset = static_cast<size_t>(frame_in_hunk) * BAM2_CD_RAW_FRAME_SIZE;
    const size_t data_offset = frame_offset + BAM2_CD_MODE1_DATA_OFFSET;
    if ((data_offset + BAM2_CD_DATA_SECTOR_SIZE) > hunk_buffer.size())
      return false;

    std::memcpy(destination, hunk_buffer.data() + data_offset, BAM2_CD_DATA_SECTOR_SIZE);
    return true;
  }
};

struct RuntimeState
{
  BoardType board_type = BoardType::CapcomZN1;
  CapcomZN1Content content;
  std::vector<u8> video_system_fixed_rom;
  std::vector<u8> atlus_audio_cpu_rom;
  std::vector<u8> atlus_ymz280b_rom;
  CAT702::Chip motherboard_cat702;
  CAT702::Chip game_cat702;
  u8 bank = 0;
  AcclaimZN1Game acclaim_game = AcclaimZN1Game::NBAJamExtreme;
  std::array<u16, 2> acclaim_bank_registers{};
  u8 acclaim_gun_mux = 0;
  std::vector<u8> acclaim_rax_rom;
  std::string acclaim_nba_sram_path;
  bool acclaim_nba_sram_dirty = false;
  std::array<u8, ACCLAIM_NBA_SRAM_SIZE> acclaim_nba_sram{};
  u16 acclaim_rax_host_latch = 0xffff;
  u8 security_select = 0;
  u8 coin = 0;
  std::string persistence_directory;
  std::string at28_path;
  bool at28_dirty = false;
  bool at28_busy = false;
  u8 at28_last_write = 0xff;
  u64 at28_busy_until = 0;
  u32 at28_trace_count = 0;
  bool known_unknown_io_read_logged = false;
  bool unknown_read_logged = false;
  bool unknown_write_logged = false;
  bool taito_unpopulated_bank_logged = false;
  bool taito_watchdog_ck = false;
  bool taito_watchdog_seen = false;
  bool taito_main_board_reset_pending = false;
  bool time_warner_main_board_reset_pending = false;
  bool tecmo_cbaj_sound_enabled = false;
  bool eighting_ps9805_flash = false;
  BustAMove2Media bam2_media = BustAMove2Media::HardDisk;
  std::array<u16, 4> bam2_mcu_ports{};
  bool bam2_outputs_initialized = false;
  u16 bam2_h8_status = UINT16_C(0x0004);
  u16 bam2_h8_opcode = 0;
  bool bam2_h8_waiting_for_argument = false;
  bool bam2_test_switch_latched = false;
  bool bam2_test_was_pressed = false;
  u16 bam2_selected_track = 0;
  u8 bam2_tc9293_attenuation = 0;
  bool bam2_pcm_playing = false;
  u64 bam2_pcm_track_start_offset = 0;
  u64 bam2_pcm_track_end_offset = 0;
  u64 bam2_pcm_prefetch_offset = 0;
  u64 bam2_pcm_output_bytes = 0;
  Arcade::Storage::CHDHardDisk bam2_hdd;
  std::array<BAM2PCMFile, BAM2_PCM_FILE_COUNT> bam2_pcm_files{};
  BAM2CDROMImage bam2_cdrom;
  std::array<BAM2CDPCMFile, BAM2_CD_PCM_FILE_COUNT> bam2_cd_pcm_files{};
  u32 bam2_fat_start_lba = 0;
  u32 bam2_first_data_lba = 0;
  u32 bam2_sectors_per_cluster = 0;
  u32 bam2_cluster_count = 0;
  u32 bam2_pcm_cached_lba = UINT32_MAX;
  std::array<u8, BAM2_SECTOR_SIZE> bam2_pcm_sector{};
  u32 bam2_cd_cached_lba = UINT32_MAX;
  std::array<u8, BAM2_CD_DATA_SECTOR_SIZE> bam2_cd_sector{};
  std::array<u8, BAM2_PCM_BUFFER_SIZE> bam2_pcm_buffer{};
  u32 bam2_pcm_buffer_read = 0;
  u32 bam2_pcm_buffer_write = 0;
  u32 bam2_pcm_buffered_bytes = 0;
  bool tecmo_gr2_link_enabled = false;

  std::string taito_fx1b_fram_path;
  bool taito_fx1b_fram_dirty = false;
  std::array<u8, TAITO_FX1B_FRAM_SIZE> taito_fx1b_fram{};
  std::array<u8, TAITO_ZOOM_SHARED_SIZE> taito_zoom_shared_ram{};
  u8 taito_zoom_reg_address = 0;
  u16 taito_zoom_gain_left = 0x3f;
  u16 taito_zoom_gain_right = 0x3f;
  std::vector<u8> taito_fx1b_mn10200_rom;
  std::vector<u8> taito_fx1b_zsg2_rom;

  u16 sio0_mode = 0;
  u16 sio0_control = 0;
  u16 sio0_baud = 0;
  u8 sio0_rx_data = 0xff;
  u8 sio0_tx_data = 0xff;
  u8 sio0_tx_shift = 0xff;
  bool sio0_rx_full = false;
  bool sio0_tx_full = false;
  bool sio0_transfer_active = false;
  bool sio0_interrupt_pending = false;
  u32 sio_exchange_count = 0;
  u32 sio_register_trace_count = 0;
  u32 sio_status_trace_count = 0;

  // Sony ZN motherboard MCU transport. The uPD78081 internal ROM is undumped;
  // this models the externally-observed digital I/O serial contract used by
  // MAME's ZN MCU device: select -> 50 us -> DSR low for 5 us -> data byte.
  bool mcu_selected = false;
  bool mcu_dsr_active = false;
  u8 mcu_byte_index = 0;
};

std::optional<RuntimeState> s_runtime;
std::unique_ptr<TimingEvent> s_mcu_dsr_assert_event;
std::unique_ptr<TimingEvent> s_mcu_dsr_release_event;
std::unique_ptr<TimingEvent> s_sio0_transfer_event;
std::unique_ptr<TimingEvent> s_taito_watchdog_event;
std::unique_ptr<TimingEvent> s_time_warner_watchdog_event;

static std::array<u8, AT28_SIZE> s_at28{};

static u64 GetAT28Now()
{
  return static_cast<u64>(System::GetGlobalTickCounter());
}

static u64 GetAT28WriteCycleTicks()
{
  // Normal AT28C16 byte programming is busy for 200 microseconds.
  const u64 ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());
  return std::max<u64>((ticks_per_second + 4999) / 5000, 1);
}

static void UpdateAT28Busy(RuntimeState& runtime)
{
  if (runtime.at28_busy && GetAT28Now() >= runtime.at28_busy_until)
    runtime.at28_busy = false;
}

static bool EnsurePersistenceDirectories(const RuntimeState& runtime)
{
  const std::string nvram_root(Path::GetDirectory(runtime.persistence_directory));
  return (!nvram_root.empty() && FileSystem::CreateDirectory(nvram_root.c_str(), false) &&
          FileSystem::CreateDirectory(runtime.persistence_directory.c_str(), false));
}

static bool LoadAT28(RuntimeState& runtime, std::string_view persistence_directory, Error* error,
                     std::span<const u8> initial_contents = {})
{
  if (persistence_directory.empty())
  {
    Error::SetStringView(error, "Sony ZN AT28C16 persistence directory is empty.");
    return false;
  }

  runtime.persistence_directory.assign(persistence_directory);
  runtime.at28_path = Path::Combine(runtime.persistence_directory, "at28c16");
  runtime.at28_dirty = false;
  runtime.at28_busy = false;
  runtime.at28_last_write = 0xff;
  runtime.at28_busy_until = 0;
  runtime.at28_trace_count = 0;
  s_at28.fill(0xff);

  const bool persistence_exists = FileSystem::FileExists(runtime.at28_path.c_str());
  if (persistence_exists)
  {
    std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(runtime.at28_path.c_str(), error);
    if (!data || data->size() != AT28_SIZE)
    {
      const size_t actual_size = data ? data->size() : 0;
      Error::SetStringFmt(error, "Sony ZN AT28C16 '{}' has size {}; expected {} bytes.", runtime.at28_path,
                          actual_size, AT28_SIZE);
      return false;
    }

    std::memcpy(s_at28.data(), data->data(), s_at28.size());
  }
  else
  {
    if (!initial_contents.empty())
    {
      if (initial_contents.size() != AT28_SIZE)
      {
        Error::SetStringFmt(error, "Sony ZN AT28C16 factory seed has size {}; expected {} bytes.",
                            initial_contents.size(), AT28_SIZE);
        return false;
      }

      std::memcpy(s_at28.data(), initial_contents.data(), s_at28.size());
    }

    if (!EnsurePersistenceDirectories(runtime) ||
        !FileSystem::WriteBinaryFile(runtime.at28_path.c_str(), s_at28.data(), s_at28.size()))
    {
      Error::SetStringFmt(error, "Failed to create Sony ZN AT28C16 persistence '{}'.", runtime.at28_path);
      return false;
    }
  }

  VERBOSE_LOG("SonyZN.AT28 {} set='{}' size={} factory_seed={} path='{}'",
           persistence_exists ? "loaded" : "initialized", runtime.content.set_name, AT28_SIZE,
           !persistence_exists && !initial_contents.empty(), runtime.at28_path);
  return true;
}

static void SaveAT28(RuntimeState& runtime)
{
  if (!runtime.at28_dirty)
    return;

  if (!EnsurePersistenceDirectories(runtime) ||
      !FileSystem::WriteBinaryFile(runtime.at28_path.c_str(), s_at28.data(), s_at28.size()))
  {
    ERROR_LOG("SonyZN.AT28 save failed set='{}' path='{}'", runtime.content.set_name, runtime.at28_path);
    return;
  }

  runtime.at28_dirty = false;
  VERBOSE_LOG("SonyZN.AT28 saved set='{}' path='{}'", runtime.content.set_name, runtime.at28_path);
}

static bool LoadAcclaimNBASRAM(RuntimeState& runtime, Error* error)
{
  runtime.acclaim_nba_sram_path = Path::Combine(runtime.persistence_directory, "71256");
  runtime.acclaim_nba_sram_dirty = false;
  runtime.acclaim_nba_sram.fill(UINT8_C(0xff));

  const bool persistence_exists = FileSystem::FileExists(runtime.acclaim_nba_sram_path.c_str());
  if (persistence_exists)
  {
    std::optional<DynamicHeapArray<u8>> data =
      FileSystem::ReadBinaryFile(runtime.acclaim_nba_sram_path.c_str(), error);
    if (!data || data->size() != runtime.acclaim_nba_sram.size())
    {
      const size_t actual_size = data ? data->size() : 0;
      Error::SetStringFmt(error, "Sony ZN Acclaim 71256 SRAM '{}' has size {}; expected {} bytes.",
                          runtime.acclaim_nba_sram_path, actual_size, runtime.acclaim_nba_sram.size());
      return false;
    }

    std::memcpy(runtime.acclaim_nba_sram.data(), data->data(), runtime.acclaim_nba_sram.size());
  }
  else
  {
    if (!EnsurePersistenceDirectories(runtime) ||
        !FileSystem::WriteBinaryFile(runtime.acclaim_nba_sram_path.c_str(), runtime.acclaim_nba_sram.data(),
                                     runtime.acclaim_nba_sram.size()))
    {
      Error::SetStringFmt(error, "Failed to create Acclaim 71256 SRAM persistence '{}'.",
                          runtime.acclaim_nba_sram_path);
      return false;
    }
  }

  VERBOSE_LOG("SonyZN.Acclaim.SRAM {} set='{}' size={} path='{}'",
           persistence_exists ? "loaded" : "initialized", runtime.content.set_name,
           runtime.acclaim_nba_sram.size(), runtime.acclaim_nba_sram_path);
  return true;
}

static void SaveAcclaimNBASRAM(RuntimeState& runtime)
{
  if (!runtime.acclaim_nba_sram_dirty)
    return;

  if (!EnsurePersistenceDirectories(runtime) ||
      !FileSystem::WriteBinaryFile(runtime.acclaim_nba_sram_path.c_str(), runtime.acclaim_nba_sram.data(),
                                   runtime.acclaim_nba_sram.size()))
  {
    ERROR_LOG("SonyZN.Acclaim.SRAM save failed set='{}' path='{}'", runtime.content.set_name,
              runtime.acclaim_nba_sram_path);
    return;
  }

  runtime.acclaim_nba_sram_dirty = false;
  VERBOSE_LOG("SonyZN.Acclaim.SRAM saved set='{}' path='{}'", runtime.content.set_name,
           runtime.acclaim_nba_sram_path);
}

static bool LoadTaitoFX1BFRAM(RuntimeState& runtime, Error* error)
{
  runtime.taito_fx1b_fram_path = Path::Combine(runtime.persistence_directory, "fm1208s");
  runtime.taito_fx1b_fram_dirty = false;
  runtime.taito_fx1b_fram.fill(UINT8_C(0xff));

  const bool persistence_exists = FileSystem::FileExists(runtime.taito_fx1b_fram_path.c_str());
  if (persistence_exists)
  {
    std::optional<DynamicHeapArray<u8>> data =
      FileSystem::ReadBinaryFile(runtime.taito_fx1b_fram_path.c_str(), error);
    if (!data || data->size() != runtime.taito_fx1b_fram.size())
    {
      const size_t actual_size = data ? data->size() : 0;
      Error::SetStringFmt(error, "Sony ZN Taito FX-1B FM1208S '{}' has size {}; expected {} bytes.",
                          runtime.taito_fx1b_fram_path, actual_size, runtime.taito_fx1b_fram.size());
      return false;
    }

    std::memcpy(runtime.taito_fx1b_fram.data(), data->data(), runtime.taito_fx1b_fram.size());
  }
  else
  {
    if (!EnsurePersistenceDirectories(runtime) ||
        !FileSystem::WriteBinaryFile(runtime.taito_fx1b_fram_path.c_str(), runtime.taito_fx1b_fram.data(),
                                     runtime.taito_fx1b_fram.size()))
    {
      Error::SetStringFmt(error, "Failed to create Taito FX-1B FM1208S persistence '{}'.",
                          runtime.taito_fx1b_fram_path);
      return false;
    }
  }

  VERBOSE_LOG("SonyZN.FX1B.FRAM {} set='{}' size={} path='{}'",
           persistence_exists ? "loaded" : "initialized", runtime.content.set_name,
           runtime.taito_fx1b_fram.size(), runtime.taito_fx1b_fram_path);
  return true;
}

static void SaveTaitoFX1BFRAM(RuntimeState& runtime)
{
  if (!runtime.taito_fx1b_fram_dirty)
    return;

  if (!EnsurePersistenceDirectories(runtime) ||
      !FileSystem::WriteBinaryFile(runtime.taito_fx1b_fram_path.c_str(), runtime.taito_fx1b_fram.data(),
                                   runtime.taito_fx1b_fram.size()))
  {
    ERROR_LOG("SonyZN.FX1B.FRAM save failed set='{}' path='{}'", runtime.content.set_name,
              runtime.taito_fx1b_fram_path);
    return;
  }

  runtime.taito_fx1b_fram_dirty = false;
  VERBOSE_LOG("SonyZN.FX1B.FRAM saved set='{}' path='{}'", runtime.content.set_name, runtime.taito_fx1b_fram_path);
}

static u8 ReadAT28Byte(RuntimeState& runtime, u32 offset)
{
  UpdateAT28Busy(runtime);

  // While programming, data polling returns the last programmed byte with bit 7 inverted.
  const u8 value = runtime.at28_busy ? static_cast<u8>(runtime.at28_last_write ^ 0x80) : s_at28[offset];
  if (runtime.at28_trace_count < 64)
  {
    DEV_LOG("SonyZN.AT28 read={} offset=0x{:03X} value=0x{:02X} busy={}", runtime.at28_trace_count, offset, value,
             runtime.at28_busy);
  }
  runtime.at28_trace_count++;
  return value;
}

static void WriteAT28Byte(RuntimeState& runtime, u32 offset, u8 value)
{
  UpdateAT28Busy(runtime);

  if (runtime.at28_busy)
  {
    if (runtime.at28_trace_count < 64)
    {
      DEV_LOG("SonyZN.AT28 write={} offset=0x{:03X} value=0x{:02X} ignored=busy", runtime.at28_trace_count,
               offset, value);
    }
    runtime.at28_trace_count++;
    return;
  }

  const u8 old_value = s_at28[offset];
  if (old_value == value)
  {
    if (runtime.at28_trace_count < 64)
    {
      DEV_LOG("SonyZN.AT28 write={} offset=0x{:03X} value=0x{:02X} unchanged", runtime.at28_trace_count, offset,
               value);
    }
    runtime.at28_trace_count++;
    return;
  }

  s_at28[offset] = value;
  runtime.at28_last_write = value;
  runtime.at28_busy = true;
  runtime.at28_busy_until = GetAT28Now() + GetAT28WriteCycleTicks();
  runtime.at28_dirty = true;

  if (runtime.at28_trace_count < 64)
  {
    DEV_LOG("SonyZN.AT28 write={} offset=0x{:03X} old=0x{:02X} new=0x{:02X} busy_ticks={}",
             runtime.at28_trace_count, offset, old_value, value, GetAT28WriteCycleTicks());
  }
  runtime.at28_trace_count++;
}

const Arcade::Database::ROMRegionDefinition* FindRegion(const Arcade::Database::FirmwareDefinition& firmware,
                                                        std::string_view id)
{
  for (const Arcade::Database::ROMRegionDefinition& region : firmware.rom_regions)
  {
    if (region.id == id)
      return &region;
  }

  return nullptr;
}

std::optional<size_t> GetSimpleGameRegionSize(const Arcade::Database::GameDefinition& game, std::string_view id)
{
  size_t region_size = 0;
  bool found = false;

  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region != id)
      continue;

    if (!rom.segments.empty() || rom.group_size != 1 || rom.skip != 0 || rom.word_swap || rom.interleave != 1)
      return std::nullopt;

    const size_t offset = static_cast<size_t>(rom.offset);
    const size_t size = static_cast<size_t>(rom.size);
    if (offset > (std::numeric_limits<size_t>::max() - size))
      return std::nullopt;

    region_size = std::max(region_size, offset + size);
    found = true;
  }

  return found ? std::optional<size_t>(region_size) : std::nullopt;
}

const Arcade::Database::ROMDefinition* FindFirmwareROM(const Arcade::Database::FirmwareDefinition& firmware,
                                                      std::string_view region, std::string_view bios_variant)
{
  for (const Arcade::Database::ROMDefinition& rom : firmware.roms)
  {
    if (rom.region != region)
      continue;
    if (!bios_variant.empty() && rom.bios_variant != bios_variant)
      continue;
    return &rom;
  }

  return nullptr;
}

bool LoadROMMember(const char* archive_path, const Arcade::Database::ROMDefinition& rom, std::vector<u8>* data,
                   Error* error)
{
  unzFile zf = MinizipHelpers::OpenUnzFile(archive_path);
  if (!zf)
  {
    Error::SetStringFmt(error, "Failed to open Sony ZN archive '{}'.", archive_path);
    return false;
  }

  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    Error::SetStringFmt(error, "Sony ZN archive '{}' is empty or unreadable.", archive_path);
    return false;
  }

  for (;;)
  {
    unz_file_info64 file_info = {};
    char member_name[512] = {};
    if (unzGetCurrentFileInfo64(zf, &file_info, member_name, sizeof(member_name), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed to read file information from Sony ZN archive '{}'.", archive_path);
      return false;
    }

    member_name[sizeof(member_name) - 1] = '\0';
    if (StringUtil::EqualNoCase(member_name, rom.name))
    {
      if (file_info.uncompressed_size != rom.size)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Sony ZN ROM '{}' has size {}, expected {} bytes.", rom.name,
                            file_info.uncompressed_size, rom.size);
        return false;
      }
      if (rom.has_crc32 && static_cast<u32>(file_info.crc) != rom.crc32)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Sony ZN ROM '{}' has CRC32 {:08x}, expected {:08x}.", rom.name,
                            static_cast<u32>(file_info.crc), rom.crc32);
        return false;
      }
      if (unzOpenCurrentFile(zf) != UNZ_OK)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Failed to decompress Sony ZN ROM '{}' from '{}'.", rom.name, archive_path);
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
          Error::SetStringFmt(error, "Failed reading Sony ZN ROM '{}' from '{}'.", rom.name, archive_path);
          return false;
        }
        read_offset += static_cast<size_t>(bytes_read);
      }

      const int close_result = unzCloseCurrentFile(zf);
      unzClose(zf);
      if (close_result != UNZ_OK)
      {
        Error::SetStringFmt(error, "CRC validation failed for Sony ZN ROM '{}' in '{}'.", rom.name, archive_path);
        return false;
      }

      if (!rom.sha1.empty())
      {
        auto digest = SHA1Digest::GetDigest(std::span<const u8>(data->data(), data->size()));
        const std::string digest_string = SHA1Digest::DigestToString(digest);
        if (!StringUtil::EqualNoCase(digest_string, rom.sha1))
        {
          Error::SetStringFmt(error, "Sony ZN ROM '{}' has SHA-1 {}, expected {}.", rom.name, digest_string,
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
      Error::SetStringFmt(error, "Failed while reading Sony ZN archive '{}'.", archive_path);
      return false;
    }
  }

  unzClose(zf);
  Error::SetStringFmt(error, "Sony ZN archive '{}' does not contain required ROM '{}'.", archive_path, rom.name);
  return false;
}

bool PlaceSimpleROM(const Arcade::Database::ROMDefinition& rom, std::span<const u8> source,
                    std::span<u8> destination, Error* error)
{
  if (!rom.segments.empty() || rom.group_size != 1 || rom.skip != 0 || rom.word_swap || rom.interleave != 1)
  {
    Error::SetStringFmt(error, "Sony ZN ROM '{}' uses a load layout not enabled for the initial Capcom bring-up.",
                        rom.name);
    return false;
  }

  const size_t offset = static_cast<size_t>(rom.offset);
  if (offset > destination.size() || source.size() > (destination.size() - offset))
  {
    Error::SetStringFmt(error, "Sony ZN ROM '{}' exceeds the '{}' region.", rom.name, rom.region);
    return false;
  }

  std::memcpy(destination.data() + offset, source.data(), source.size());
  return true;
}

bool PlaceTaitoBankedROM(const Arcade::Database::ROMDefinition& rom, std::span<const u8> source,
                         std::span<u8> destination, Error* error)
{
  if (rom.segments.empty() && !rom.word_swap && rom.group_size == 1 && rom.interleave == 1 && rom.skip == 0)
    return PlaceSimpleROM(rom, source, destination, error);

  // Taito FX-1 program ROM pairs use MAME's ROM_LOAD16_BYTE layout.
  if (!rom.segments.empty() || rom.word_swap || rom.group_size != 1 || rom.interleave != 2 || rom.skip != 1)
  {
    Error::SetStringFmt(error, "Sony ZN Taito ROM '{}' uses an unsupported banked-ROM layout.", rom.name);
    return false;
  }

  const size_t offset = static_cast<size_t>(rom.offset);
  if (offset >= destination.size() || source.empty())
  {
    Error::SetStringFmt(error, "Sony ZN Taito ROM '{}' exceeds the '{}' region.", rom.name, rom.region);
    return false;
  }

  const size_t last_offset = offset + ((source.size() - 1) * 2);
  if (last_offset >= destination.size())
  {
    Error::SetStringFmt(error, "Sony ZN Taito ROM '{}' exceeds the '{}' region.", rom.name, rom.region);
    return false;
  }

  for (size_t i = 0; i < source.size(); i++)
    destination[offset + (i * 2)] = source[i];

  return true;
}

bool PlaceAtlusROM(const Arcade::Database::ROMDefinition& rom, std::span<const u8> source,
                   std::span<u8> destination, Error* error)
{
  if (rom.segments.empty() && !rom.word_swap && rom.group_size == 1 && rom.interleave == 1 && rom.skip == 0)
    return PlaceSimpleROM(rom, source, destination, error);

  // Heaven's Gate uses ROM_LOAD16_BYTE pairs for both the first PSX program
  // megabyte and the 68000 sound program.
  if (!rom.segments.empty() || rom.word_swap || rom.group_size != 1 || rom.interleave != 2 || rom.skip != 1)
  {
    Error::SetStringFmt(error, "Sony ZN Atlus ROM '{}' uses an unsupported load layout.", rom.name);
    return false;
  }

  const size_t offset = static_cast<size_t>(rom.offset);
  if (offset >= destination.size() || source.empty())
  {
    Error::SetStringFmt(error, "Sony ZN Atlus ROM '{}' exceeds the '{}' region.", rom.name, rom.region);
    return false;
  }

  const size_t last_offset = offset + ((source.size() - 1) * 2);
  if (last_offset >= destination.size())
  {
    Error::SetStringFmt(error, "Sony ZN Atlus ROM '{}' exceeds the '{}' region.", rom.name, rom.region);
    return false;
  }

  for (size_t i = 0; i < source.size(); i++)
    destination[offset + (i * 2)] = source[i];

  return true;
}

bool PlaceEightingROM(const Arcade::Database::ROMDefinition& rom, std::span<const u8> source,
                      std::span<u8> destination, Error* error)
{
  if (rom.segments.empty() && !rom.word_swap && rom.group_size == 1 && rom.interleave == 1 && rom.skip == 0)
    return PlaceSimpleROM(rom, source, destination, error);

  // RA9701 uses ROM_LOAD16_BYTE-style pairs for the first PSX program area and
  // the 68000 sound program.
  if (!rom.segments.empty() || rom.word_swap || rom.group_size != 1 || rom.interleave != 2 || rom.skip != 1)
  {
    Error::SetStringFmt(error, "Sony ZN Eighting/Raizing ROM '{}' uses an unsupported load layout.", rom.name);
    return false;
  }

  const size_t offset = static_cast<size_t>(rom.offset);
  if (offset >= destination.size() || source.empty())
  {
    Error::SetStringFmt(error, "Sony ZN Eighting/Raizing ROM '{}' exceeds the '{}' region.", rom.name,
                        rom.region);
    return false;
  }

  const size_t last_offset = offset + ((source.size() - 1) * 2);
  if (last_offset >= destination.size())
  {
    Error::SetStringFmt(error, "Sony ZN Eighting/Raizing ROM '{}' exceeds the '{}' region.", rom.name,
                        rom.region);
    return false;
  }

  for (size_t i = 0; i < source.size(); i++)
    destination[offset + (i * 2)] = source[i];

  return true;
}

bool PlaceQSoundROM(const Arcade::Database::ROMDefinition& rom, std::span<const u8> source,
                    std::span<u8> destination, Error* error)
{
  if (!rom.segments.empty() || !rom.word_swap || rom.group_size != 2 || rom.skip != 0 || rom.interleave != 2 ||
      (source.size() & 1u) != 0)
  {
    Error::SetStringFmt(error, "Sony ZN QSound ROM '{}' does not use the expected ROM_LOAD16_WORD_SWAP layout.",
                        rom.name);
    return false;
  }

  const size_t offset = static_cast<size_t>(rom.offset);
  if (offset > destination.size() || source.size() > (destination.size() - offset))
  {
    Error::SetStringFmt(error, "Sony ZN QSound ROM '{}' exceeds the '{}' region.", rom.name, rom.region);
    return false;
  }

  for (size_t i = 0; i < source.size(); i += 2)
  {
    destination[offset + i] = source[i + 1];
    destination[offset + i + 1] = source[i];
  }

  return true;
}
u32 ReadBytes(std::span<const u8> data, u32 width, u32 offset)
{
  const size_t read_offset = static_cast<size_t>(offset);
  if ((width != 1 && width != 2 && width != 4) || read_offset > data.size() ||
      static_cast<size_t>(width) > (data.size() - read_offset))
  {
    return UINT32_C(0xffffffff);
  }

  if (width == 1)
    return data[read_offset];

  if (width == 2)
  {
    u16 value;
    std::memcpy(&value, data.data() + read_offset, sizeof(value));
    return value;
  }

  u32 value;
  std::memcpy(&value, data.data() + read_offset, sizeof(value));
  return value;
}

u32 ReadByteRegister(u8 value, u32 width)
{
  if (width == 1)
    return value;
  if (width == 2)
    return UINT32_C(0xff00) | value;
  return UINT32_C(0xffffff00) | value;
}

u32 ReadBytePortWindow(u8 value, u32 width, u32 byte_offset)
{
  if (byte_offset >= 4)
    return UINT32_C(0xffffffff);

  const u32 register_value = UINT32_C(0xffffff00) | value;
  if (width == 1)
    return (register_value >> (byte_offset * 8)) & UINT32_C(0xff);
  if (width == 2 && byte_offset <= 2)
    return (register_value >> (byte_offset * 8)) & UINT32_C(0xffff);
  if (width == 4 && byte_offset == 0)
    return register_value;
  return UINT32_C(0xffffffff);
}

u32 ReadHalfwordPortWindow(u16 value, u32 width, u32 byte_offset)
{
  if (byte_offset >= 4)
    return UINT32_C(0xffffffff);

  const u32 register_value = UINT32_C(0xffff0000) | value;
  if (width == 1)
    return (register_value >> (byte_offset * 8)) & UINT32_C(0xff);
  if (width == 2 && byte_offset <= 2)
    return (register_value >> (byte_offset * 8)) & UINT32_C(0xffff);
  if (width == 4 && byte_offset == 0)
    return register_value;
  return UINT32_C(0xffffffff);
}

u32 ReadEvenByteWindow(std::span<const u8> data, u32 width, u32 byte_offset)
{
  if (width != 1 && width != 2 && width != 4)
    return UINT32_C(0xffffffff);

  u32 value = 0;
  for (u32 i = 0; i < width; i++)
  {
    const u32 address = byte_offset + i;
    u8 byte = UINT8_C(0xff);
    if ((address & 1u) == 0)
    {
      const size_t index = static_cast<size_t>(address >> 1);
      if (index < data.size())
        byte = data[index];
    }
    value |= static_cast<u32>(byte) << (i * 8);
  }
  return value;
}

[[maybe_unused]] void WriteEvenByteWindow(std::span<u8> data, u32 width, u32 byte_offset, u32 value)
{
  if (width != 1 && width != 2 && width != 4)
    return;

  for (u32 i = 0; i < width; i++)
  {
    const u32 address = byte_offset + i;
    if ((address & 1u) != 0)
      continue;

    const size_t index = static_cast<size_t>(address >> 1);
    if (index < data.size())
      data[index] = static_cast<u8>(value >> (i * 8));
  }
}

bool IsArcadeControlPressed(u32 port, std::string_view key)
{
  return ArcadeInput::IsDigitalPressed(port, key);
}

enum class CapcomInputProfile : u8
{
  Unknown,
  Capcom6Button,
  Capcom4Button,
  GallopRacer,
};

CapcomInputProfile GetCapcomInputProfile()
{
  if (!s_runtime)
    return CapcomInputProfile::Unknown;

  const std::string& set = s_runtime->content.set_name;

  if (set == "ts2" || set == "ts2u" || set == "ts2ua" || set == "ts2j" || set == "ts2ja" ||
      set == "sfex" || set == "sfexa" || set == "sfexj" || set == "sfexu" ||
      set == "sfexp" || set == "sfexpj" || set == "sfexpj1" || set == "sfexpu1")
  {
    return CapcomInputProfile::Capcom6Button;
  }

  if (set == "starglad" || set == "stargladj")
    return CapcomInputProfile::Capcom4Button;

  if (set == "glpracr" || set == "glpracrj")
    return CapcomInputProfile::GallopRacer;

  return CapcomInputProfile::Unknown;
}

u8 ReadCapcomPlayerPort(u32 player)
{
  const CapcomInputProfile profile = GetCapcomInputProfile();
  if (profile == CapcomInputProfile::Unknown)
    return UINT8_C(0xff);

  // Gallop Racer is a one-player cabinet. Its specialized controller is kept
  // separate from the fighter mappings; P2 is physically unused.
  if (profile == CapcomInputProfile::GallopRacer && player != 0)
    return UINT8_C(0xff);

  u8 value = UINT8_C(0xff);

  // Common ZN P1/P2 direction bits. MAME's glpracr profile inherits the
  // standard P1 digital directions while leaving the entire P2 port unused.
  if (IsArcadeControlPressed(player, "Up"))
    value &= ~UINT8_C(0x01);
  if (IsArcadeControlPressed(player, "Down"))
    value &= ~UINT8_C(0x02);
  if (IsArcadeControlPressed(player, "Left"))
    value &= ~UINT8_C(0x04);
  if (IsArcadeControlPressed(player, "Right"))
    value &= ~UINT8_C(0x08);

  // capcom6b: buttons 1-3 are on the normal JAMMA player port.
  // capcom4b: only buttons 1-2 are here; buttons 3-4 move to the auxiliary harness.
  // glpracr: P1 buttons 1-2 remain on the normal player port.
  if (IsArcadeControlPressed(player, "Button1"))
    value &= ~UINT8_C(0x10);
  if (IsArcadeControlPressed(player, "Button2"))
    value &= ~UINT8_C(0x20);
  if (profile == CapcomInputProfile::Capcom6Button && IsArcadeControlPressed(player, "Button3"))
    value &= ~UINT8_C(0x40);

  return value;
}

bool IsBloodyRoar2Set(std::string_view set_name)
{
  return set_name == "bldyror2" || set_name == "bldyror2u" || set_name == "bldyror2a" ||
         set_name == "bldyror2j";
}

bool IsBraveBladeSet(std::string_view set_name)
{
  return set_name == "brvblade" || set_name == "brvbladeu" || set_name == "brvbladea" ||
         set_name == "brvbladej";
}

u8 ReadEightingExtendedPlayerPort(u32 port)
{
  u8 value = UINT8_C(0xff);
  if (!s_runtime || s_runtime->board_type != BoardType::EightingRaizingZN1 ||
      !IsBloodyRoar2Set(s_runtime->content.set_name) || port != 0)
  {
    return value;
  }

  // Bloody Roar 2 routes each player's fourth action button through the shared
  // P3 extension byte: P1 on bit 4, P2 on bit 5.
  if (IsArcadeControlPressed(0, "Button4"))
    value &= ~UINT8_C(0x10);
  if (IsArcadeControlPressed(1, "Button4"))
    value &= ~UINT8_C(0x20);

  return value;
}

u8 ReadTaitoPlayerPort(u32 player)
{
  if (player >= 2)
    return UINT8_C(0xff);

  u8 value = UINT8_C(0xff);
  if (IsArcadeControlPressed(player, "Up"))
    value &= ~UINT8_C(0x01);
  if (IsArcadeControlPressed(player, "Down"))
    value &= ~UINT8_C(0x02);
  if (IsArcadeControlPressed(player, "Left"))
    value &= ~UINT8_C(0x04);
  if (IsArcadeControlPressed(player, "Right"))
    value &= ~UINT8_C(0x08);
  if (IsArcadeControlPressed(player, "Button1"))
    value &= ~UINT8_C(0x10);
  if (IsArcadeControlPressed(player, "Button2"))
    value &= ~UINT8_C(0x20);
  if (IsArcadeControlPressed(player, "Button3"))
    value &= ~UINT8_C(0x40);
  if (s_runtime && s_runtime->board_type == BoardType::TecmoTPS &&
      s_runtime->content.set_name == "1on1gov" && IsArcadeControlPressed(player, "Button4"))
  {
    value &= ~UINT8_C(0x80);
  }

  return value;
}

u8 ReadAcclaimPlayerPort(u32 player)
{
  if (!s_runtime)
    return UINT8_C(0xff);

  u8 value = UINT8_C(0xff);
  if (s_runtime->acclaim_game == AcclaimZN1Game::NBAJamExtreme)
  {
    if (player >= 4)
      return value;

    if (IsArcadeControlPressed(player, "Up"))
      value &= ~UINT8_C(0x01);
    if (IsArcadeControlPressed(player, "Down"))
      value &= ~UINT8_C(0x02);
    if (IsArcadeControlPressed(player, "Left"))
      value &= ~UINT8_C(0x04);
    if (IsArcadeControlPressed(player, "Right"))
      value &= ~UINT8_C(0x08);
    if (IsArcadeControlPressed(player, "Button1"))
      value &= ~UINT8_C(0x10);
    if (IsArcadeControlPressed(player, "Button2"))
      value &= ~UINT8_C(0x20);
    if (IsArcadeControlPressed(player, "Button3"))
      value &= ~UINT8_C(0x40);
    if (IsArcadeControlPressed(player, "Button4"))
      value &= ~UINT8_C(0x80);
    return value;
  }

  if (player >= 2)
    return value;

  // Judge Dredd routes each gun's secondary-weapon button to P1/P2 bit 4.
  if (IsArcadeControlPressed(player, "Button2"))
    value &= ~UINT8_C(0x10);

  return value;
}

u8 ReadTimeWarnerPlayerPort(u32 player)
{
  if (player >= 2)
    return UINT8_C(0xff);

  u8 value = UINT8_C(0xff);
  if (IsArcadeControlPressed(player, "Up"))
    value &= ~UINT8_C(0x01);
  if (IsArcadeControlPressed(player, "Down"))
    value &= ~UINT8_C(0x02);
  if (IsArcadeControlPressed(player, "Left"))
    value &= ~UINT8_C(0x04);
  if (IsArcadeControlPressed(player, "Right"))
    value &= ~UINT8_C(0x08);
  if (IsArcadeControlPressed(player, "Button1"))
    value &= ~UINT8_C(0x10);
  if (IsArcadeControlPressed(player, "Button2"))
    value &= ~UINT8_C(0x20);
  if (IsArcadeControlPressed(player, "Button5"))
    value &= ~UINT8_C(0x40);

  return value;
}
u8 ReadBAM2PlayerPort(u32 player)
{
  if (player >= 2)
    return UINT8_C(0xff);

  u8 value = UINT8_C(0xff);

  if (IsArcadeControlPressed(player, "Up"))
    value &= ~UINT8_C(0x01);
  if (IsArcadeControlPressed(player, "Button2")) // Pedal.
    value &= ~UINT8_C(0x02);
  if (IsArcadeControlPressed(player, "Left"))
    value &= ~UINT8_C(0x04);
  if (IsArcadeControlPressed(player, "Right"))
    value &= ~UINT8_C(0x08);
  if (IsArcadeControlPressed(player, "Button1")) // Rhythm.
    value &= ~UINT8_C(0x10);
  if (IsArcadeControlPressed(player, "Start")) // Start/Jammar.
    value &= ~UINT8_C(0x20);

  // Dedicated cabinet selector panel: P1 bit 6 = Up Select, P2 bit 6 = Down Select.
  // The selector is physically shared, so accept the explicit binding from either
  // BAM2 player profile. Retain Button3 only as compatibility for short-lived v3 bindings.
  if (player == 0 &&
      (IsArcadeControlPressed(0, "SelectUp") || IsArcadeControlPressed(1, "SelectUp") ||
       IsArcadeControlPressed(0, "Button3")))
  {
    value &= ~UINT8_C(0x40);
  }
  else if (player == 1 &&
           (IsArcadeControlPressed(0, "SelectDown") || IsArcadeControlPressed(1, "SelectDown") ||
            IsArcadeControlPressed(1, "Button3")))
  {
    value &= ~UINT8_C(0x40);
  }

  return value;
}

u8 ReadZNPlayerPort(u32 player)
{
  if (s_runtime && s_runtime->board_type == BoardType::TimeWarnerZN1)
    return ReadTimeWarnerPlayerPort(player);

  if (s_runtime && s_runtime->board_type == BoardType::AcclaimZN1)
    return ReadAcclaimPlayerPort(player);

  if (s_runtime && s_runtime->board_type == BoardType::BustAMove2ZN1)
    return ReadBAM2PlayerPort(player);

  if (s_runtime && (s_runtime->board_type == BoardType::VideoSystemZN1 ||
                    s_runtime->board_type == BoardType::AtlusZN1 ||
                    s_runtime->board_type == BoardType::EightingRaizingZN1 ||
                    s_runtime->board_type == BoardType::TaitoFX1A ||
                    s_runtime->board_type == BoardType::TaitoFX1B ||
                    s_runtime->board_type == BoardType::TecmoTPS))
  {
    return ReadTaitoPlayerPort(player);
  }

  return ReadCapcomPlayerPort(player);
}


u8 ReadCapcomExtendedButtonPort(u32 player)
{
  u8 value = UINT8_C(0xff);
  const CapcomInputProfile profile = GetCapcomInputProfile();

  if (profile == CapcomInputProfile::Capcom6Button)
  {
    // capcom6b P3/P4 extension: game buttons 4-6.
    if (IsArcadeControlPressed(player, "Button4"))
      value &= ~UINT8_C(0x10);
    if (IsArcadeControlPressed(player, "Button5"))
      value &= ~UINT8_C(0x20);
    if (IsArcadeControlPressed(player, "Button6"))
      value &= ~UINT8_C(0x40);
  }
  else if (profile == CapcomInputProfile::Capcom4Button)
  {
    // capcom4b P3/P4 extension: game button 3 on bit 4, game button 4 on bit 5.
    if (IsArcadeControlPressed(player, "Button3"))
      value &= ~UINT8_C(0x10);
    if (IsArcadeControlPressed(player, "Button4"))
      value &= ~UINT8_C(0x20);
  }

  // Gallop Racer explicitly leaves these extension inputs unused.
  return value;
}

u8 ReadTimeWarnerExtendedPlayerPort(u32 port)
{
  if (port != 1)
    return UINT8_C(0xff);

  u8 value = UINT8_C(0xff);
  if (IsArcadeControlPressed(0, "Button4"))
    value &= ~UINT8_C(0x01);
  if (IsArcadeControlPressed(0, "Button6"))
    value &= ~UINT8_C(0x02);
  if (IsArcadeControlPressed(0, "Button3"))
    value &= ~UINT8_C(0x04);

  if (IsArcadeControlPressed(1, "Button4"))
    value &= ~UINT8_C(0x10);
  if (IsArcadeControlPressed(1, "Button6"))
    value &= ~UINT8_C(0x20);
  if (IsArcadeControlPressed(1, "Button3"))
    value &= ~UINT8_C(0x40);

  return value;
}
u8 ReadZNExtendedPlayerPort(u32 player)
{
  if (s_runtime && s_runtime->board_type == BoardType::TimeWarnerZN1)
    return ReadTimeWarnerExtendedPlayerPort(player);

  if (s_runtime && s_runtime->board_type == BoardType::AcclaimZN1)
  {
    if (s_runtime->acclaim_game == AcclaimZN1Game::NBAJamExtreme)
      return ReadAcclaimPlayerPort(player + 2);
    return UINT8_C(0xff);
  }

  if (s_runtime && s_runtime->board_type == BoardType::EightingRaizingZN1)
    return ReadEightingExtendedPlayerPort(player);

  return ReadCapcomExtendedButtonPort(player);
}

u8 ReadCapcomServicePort()
{
  u8 value = UINT8_C(0xff);

  // ZN common SERVICE: bit 0 is the test/service-mode switch, bit 1 is service credit.
  if (ArcadeInput::IsOperatorPressed("Test"))
    value &= ~UINT8_C(0x01);
  if (ArcadeInput::IsOperatorPressed("Service"))
    value &= ~UINT8_C(0x02);

  return value;
}

u8 ReadAcclaimServicePort()
{
  u8 value = ReadCapcomServicePort();
  if (!s_runtime || s_runtime->acclaim_game != AcclaimZN1Game::JudgeDredd)
    return value;

  // The gun selector is exposed as an active-low custom input on SERVICE bit 3.
  if (s_runtime->acclaim_gun_mux != 0)
    value &= ~UINT8_C(0x08);

  // Gun triggers are separate active-low inputs on SERVICE bits 6 and 7.
  // ArcadeInput already presents the Reload binding as an off-screen gun position; assert the
  // trigger at the same time so a Reload button behaves exactly like firing while aimed off-screen.
  if (IsArcadeControlPressed(0, "Trigger") || IsArcadeControlPressed(0, "Reload"))
    value &= ~UINT8_C(0x40);
  if (IsArcadeControlPressed(1, "Trigger") || IsArcadeControlPressed(1, "Reload"))
    value &= ~UINT8_C(0x80);

  return value;
}

u8 ReadBAM2ServicePort()
{
  u8 value = UINT8_C(0xff);
  const bool test_pressed = ArcadeInput::IsOperatorPressed("Test");

  // The dedicated cabinet has a maintained Test switch. A normal momentary host
  // binding therefore toggles the emulated switch on each rising edge.
  if (test_pressed && !s_runtime->bam2_test_was_pressed)
    s_runtime->bam2_test_switch_latched = !s_runtime->bam2_test_switch_latched;
  s_runtime->bam2_test_was_pressed = test_pressed;

  if (s_runtime->bam2_test_switch_latched)
    value &= ~UINT8_C(0x01);
  if (ArcadeInput::IsOperatorPressed("Service"))
    value &= ~UINT8_C(0x02);

  return value;
}

u8 ReadZNServicePort()
{
  if (s_runtime && s_runtime->board_type == BoardType::BustAMove2ZN1)
    return ReadBAM2ServicePort();

  if (s_runtime && s_runtime->board_type == BoardType::AcclaimZN1)
    return ReadAcclaimServicePort();

  u8 value = ReadCapcomServicePort();
  if (s_runtime && s_runtime->board_type == BoardType::TecmoTPS &&
      s_runtime->content.set_name == "1on1gov")
  {
    // The base ZN input profile routes each player's fifth action button through SERVICE bits 4 and 5.
    if (IsArcadeControlPressed(0, "Button5"))
      value &= ~UINT8_C(0x10);
    if (IsArcadeControlPressed(1, "Button5"))
      value &= ~UINT8_C(0x20);
  }

  return value;
}

bool IsCoinSlotPressed(u32 slot)
{
  if (slot >= 2)
    return false;

  // Match ArcadeDuck's physical cabinet convention: ports 1/3 share Coin 1,
  // and ports 2/4 share Coin 2.
  return IsArcadeControlPressed(slot, "Coin") || IsArcadeControlPressed(slot + 2, "Coin");
}

u8 ReadCapcomSystemPort()
{
  u8 value = UINT8_C(0xff);
  const CapcomInputProfile profile = GetCapcomInputProfile();

  if (IsArcadeControlPressed(0, "Start"))
    value &= ~UINT8_C(0x01);
  if (IsCoinSlotPressed(0))
    value &= ~UINT8_C(0x10);

  // Gallop Racer is a one-player cabinet; MAME marks START2/COIN2 unused.
  if (profile != CapcomInputProfile::GallopRacer)
  {
    if (IsArcadeControlPressed(1, "Start"))
      value &= ~UINT8_C(0x02);
    if (IsCoinSlotPressed(1))
      value &= ~UINT8_C(0x20);
  }

  return value;
}

u8 ReadBAM2SystemPort()
{
  u8 value = UINT8_C(0xff);

  // Dedicated cabinet central selector panel.
  if (IsArcadeControlPressed(0, "Enter") || IsArcadeControlPressed(1, "Enter"))
    value &= ~UINT8_C(0x01);
  if (IsArcadeControlPressed(0, "Select") || IsArcadeControlPressed(1, "Select"))
    value &= ~UINT8_C(0x02);

  if (IsCoinSlotPressed(0))
    value &= ~UINT8_C(0x10);
  if (IsCoinSlotPressed(1))
    value &= ~UINT8_C(0x20);

  return value;
}

u8 ReadZNSystemPort()
{
  if (s_runtime && s_runtime->board_type == BoardType::BustAMove2ZN1)
    return ReadBAM2SystemPort();

  if (!s_runtime || s_runtime->board_type != BoardType::AcclaimZN1 ||
      s_runtime->acclaim_game != AcclaimZN1Game::NBAJamExtreme)
  {
    return ReadCapcomSystemPort();
  }

  u8 value = UINT8_C(0xff);
  for (u32 player = 0; player < 4; player++)
  {
    if (IsArcadeControlPressed(player, "Start"))
      value &= ~static_cast<u8>(UINT8_C(0x01) << player);
    if (IsArcadeControlPressed(player, "Coin"))
      value &= ~static_cast<u8>(UINT8_C(0x10) << player);
  }
  return value;
}

u8 ReadCapcomKickPort(u32 player)
{
  u8 value = UINT8_C(0xff);
  const CapcomInputProfile profile = GetCapcomInputProfile();

  if (profile == CapcomInputProfile::Capcom6Button)
  {
    if (IsArcadeControlPressed(player, "Button4"))
      value &= ~UINT8_C(0x01);
    if (IsArcadeControlPressed(player, "Button5"))
      value &= ~UINT8_C(0x02);
    if (IsArcadeControlPressed(player, "Button6"))
      value &= ~UINT8_C(0x04);
  }
  else if (profile == CapcomInputProfile::Capcom4Button)
  {
    if (IsArcadeControlPressed(player, "Button3"))
      value &= ~UINT8_C(0x01);
    if (IsArcadeControlPressed(player, "Button4"))
      value &= ~UINT8_C(0x02);
  }

  // Gallop Racer explicitly leaves the auxiliary kick-harness inputs unused.
  return value;
}

bool IsZNMCUSelected(u8 value)
{
  // Hardware decode used by MAME: the motherboard MCU is selected when
  // security-select bits 7, 3 and 2 are all asserted.
  return (value & UINT8_C(0x8c)) == UINT8_C(0x8c);
}

TickCount ZNMCUMicrosecondsToTicks(u64 microseconds)
{
  const u64 ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());
  const u64 ticks = ((ticks_per_second * microseconds) + UINT64_C(999999)) / UINT64_C(1000000);
  return std::max<TickCount>(static_cast<TickCount>(ticks), 1);
}

void SetSIO0Interrupt(RuntimeState& runtime, bool pending)
{
  if (runtime.sio0_interrupt_pending == pending)
    return;

  runtime.sio0_interrupt_pending = pending;
  InterruptController::SetLineState(InterruptController::IRQ::PAD, pending);
}

void SetZNMCUDSRActive(RuntimeState& runtime, bool active)
{
  const bool was_active = runtime.mcu_dsr_active;
  runtime.mcu_dsr_active = active;

  // PSX SIO0 raises IRQ7 on a DSR status 0->1 edge when JOY_CTRL bit 12 is enabled.
  if (!was_active && active && (runtime.sio0_control & UINT16_C(0x1000)) != 0)
    SetSIO0Interrupt(runtime, true);
}

void CancelZNMCUDSR(RuntimeState& runtime)
{
  if (s_mcu_dsr_assert_event)
    s_mcu_dsr_assert_event->Deactivate();
  if (s_mcu_dsr_release_event)
    s_mcu_dsr_release_event->Deactivate();

  SetZNMCUDSRActive(runtime, false);
}

void ZNMCUDSRReleaseEventCallback(void*, TickCount, TickCount)
{
  if (s_mcu_dsr_release_event)
    s_mcu_dsr_release_event->Deactivate();

  if (!s_runtime || !s_runtime->mcu_selected)
    return;

  SetZNMCUDSRActive(*s_runtime, false);
}

void ZNMCUDSRAssertEventCallback(void*, TickCount, TickCount)
{
  if (s_mcu_dsr_assert_event)
    s_mcu_dsr_assert_event->Deactivate();

  if (!s_runtime || !s_runtime->mcu_selected)
    return;

  SetZNMCUDSRActive(*s_runtime, true);

  // MAME's ZN MCU holds DSR low for 5 microseconds before releasing it.
  if (s_mcu_dsr_release_event)
    s_mcu_dsr_release_event->Schedule(ZNMCUMicrosecondsToTicks(5));
}

void ScheduleZNMCUDSR(RuntimeState& runtime)
{
  if (s_mcu_dsr_assert_event)
    s_mcu_dsr_assert_event->Deactivate();
  if (s_mcu_dsr_release_event)
    s_mcu_dsr_release_event->Deactivate();

  runtime.mcu_dsr_active = false;

  // This must be a scheduler event rather than a timestamp checked from
  // JOY_STAT reads. The BIOS busy-loop can execute many MMIO polls in one CPU
  // dispatcher slice; an event forces execution to stop exactly at the 50us
  // hardware boundary so the 5us DSR-low pulse cannot be skipped.
  if (s_mcu_dsr_assert_event)
    s_mcu_dsr_assert_event->Schedule(ZNMCUMicrosecondsToTicks(50));
}

void BeginZNMCUTransaction(RuntimeState& runtime)
{
  runtime.mcu_selected = true;
  runtime.mcu_byte_index = 0;
  ScheduleZNMCUDSR(runtime);
}

void EndZNMCUTransaction(RuntimeState& runtime)
{
  runtime.mcu_selected = false;
  runtime.mcu_byte_index = 0;
  CancelZNMCUDSR(runtime);
}

u8 GetZNMCUDSWValue(const RuntimeState& runtime)
{
  // S551 is active-low. Unknown/unexposed switches retain their inactive state.
  u8 value = UINT8_C(0x0f);

  if (runtime.board_type == BoardType::VideoSystemZN1)
  {
    if (g_settings.arcade_video_system_zn1_bios_service_mode)
      value &= ~UINT8_C(0x02); // S551:2
    if (g_settings.arcade_video_system_zn1_test_mode)
      value &= ~UINT8_C(0x04); // S551:3
    if (!g_settings.arcade_video_system_zn1_save)
      value &= ~UINT8_C(0x08); // S551:4: high=Yes, low=No
  }
  else
  {
    if (g_settings.arcade_sony_zn_bios_service_mode)
      value &= ~UINT8_C(0x02); // S551:2: common ZN BIOS service mode.
  }

  if (runtime.board_type == BoardType::BustAMove2ZN1)
  {
    value &= ~UINT8_C(0x01); // S551:1: Generic Cab exists in hardware, but ArcadeDuck currently forces Dedicated Cab.

    value = static_cast<u8>((value & ~UINT8_C(0x0c)) |
                            ((g_settings.arcade_bust_a_move_2_region & UINT8_C(0x03)) << 2));
  }
  else if ((runtime.board_type == BoardType::EightingRaizingZN1 ||
            runtime.board_type == BoardType::TaitoFX1A ||
            runtime.board_type == BoardType::TaitoFX1B) &&
           g_settings.arcade_sony_zn_game_test_mode)
  {
    value &= ~UINT8_C(0x08); // S551:4: game test mode on the znt2p hardware profile.
  }

  return value;
}

u8 ExchangeZNMCUByte(RuntimeState& runtime, u8 tx)
{
  // Digital ZN MCU transactions report one data byte. The first response byte
  // carries the one-byte payload length in the high nibble and S551 in the low
  // nibble; the second byte is zero.
  const std::array<u8, 2> response = {
    static_cast<u8>(UINT8_C(0x10) | GetZNMCUDSWValue(runtime)),
    UINT8_C(0x00),
  };

  const u8 index = runtime.mcu_byte_index;
  const u8 rx = (index < response.size()) ? response[index] : UINT8_C(0x00);

  runtime.mcu_byte_index++;

  // The real MCU schedules another 50us DSR pulse after each completed byte
  // while additional response data remains.
  if (runtime.mcu_byte_index < response.size())
    ScheduleZNMCUDSR(runtime);
  else
    CancelZNMCUDSR(runtime);

  return rx;
}

const char* GetBAM2MediaName(BustAMove2Media media)
{
  return media == BustAMove2Media::CDROM ? "CD-ROM" : "HDD";
}

u16 ReadBAM2LE16(const u8* data)
{
  return static_cast<u16>(data[0]) | (static_cast<u16>(data[1]) << 8);
}

u32 ReadBAM2LE32(const u8* data)
{
  return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) |
         (static_cast<u32>(data[2]) << 16) | (static_cast<u32>(data[3]) << 24);
}

bool IsBAM2FAT32BootSector(const std::array<u8, BAM2_SECTOR_SIZE>& sector)
{
  return ReadBAM2LE16(sector.data() + 11) == BAM2_SECTOR_SIZE && sector[13] != 0 &&
         ReadBAM2LE16(sector.data() + 17) == 0 && ReadBAM2LE16(sector.data() + 22) == 0 &&
         ReadBAM2LE32(sector.data() + 36) != 0 && ReadBAM2LE32(sector.data() + 44) >= 2;
}

u32 GetBAM2ClusterLBA(const RuntimeState& runtime, u32 cluster)
{
  return runtime.bam2_first_data_lba + ((cluster - 2) * runtime.bam2_sectors_per_cluster);
}

bool ReadBAM2FATEntry(RuntimeState& runtime, u32 cluster, u32* next_cluster,
                      u32* cached_lba, std::array<u8, BAM2_SECTOR_SIZE>* cache)
{
  if (!next_cluster || !cached_lba || !cache)
    return false;

  const u32 fat_offset = cluster * 4;
  const u32 lba = runtime.bam2_fat_start_lba + (fat_offset / BAM2_SECTOR_SIZE);
  const u32 offset = fat_offset % BAM2_SECTOR_SIZE;
  if (*cached_lba != lba)
  {
    if (!runtime.bam2_hdd.ReadSector(lba, cache->data()))
      return false;
    *cached_lba = lba;
  }

  *next_cluster = ReadBAM2LE32(cache->data() + offset) & UINT32_C(0x0fffffff);
  return true;
}

bool BuildBAM2ClusterChain(RuntimeState& runtime, u32 first_cluster, u32 size, std::vector<u32>* clusters)
{
  if (!clusters || first_cluster < 2 || size == 0)
    return false;

  const u64 cluster_bytes = static_cast<u64>(runtime.bam2_sectors_per_cluster) * BAM2_SECTOR_SIZE;
  const u32 required_clusters = static_cast<u32>((static_cast<u64>(size) + cluster_bytes - 1) / cluster_bytes);
  if (required_clusters == 0 || required_clusters > runtime.bam2_cluster_count)
    return false;

  clusters->clear();
  clusters->reserve(required_clusters);

  u32 cluster = first_cluster;
  u32 cached_lba = UINT32_MAX;
  std::array<u8, BAM2_SECTOR_SIZE> fat_sector{};
  for (u32 i = 0; i < required_clusters; i++)
  {
    if (cluster < 2 || cluster >= (runtime.bam2_cluster_count + 2))
      return false;

    clusters->push_back(cluster);
    if ((i + 1) == required_clusters)
      return true;

    u32 next_cluster = 0;
    if (!ReadBAM2FATEntry(runtime, cluster, &next_cluster, &cached_lba, &fat_sector) ||
        next_cluster < 2 || next_cluster >= UINT32_C(0x0ffffff8) || next_cluster == cluster)
    {
      return false;
    }
    cluster = next_cluster;
  }

  return false;
}

int GetBAM2PCMFileIndex(const u8* entry)
{
  if (!entry || entry[3] != ' ' || entry[4] != ' ' || entry[5] != ' ' || entry[6] != ' ' || entry[7] != ' ' ||
      entry[8] != '4' || entry[9] != ' ' || entry[10] != ' ')
  {
    return -1;
  }

  if (entry[0] < '0' || entry[0] > '9' || entry[1] < '0' || entry[1] > '9' || entry[2] < '0' || entry[2] > '9')
    return -1;

  const int index = ((entry[0] - '0') * 100) + ((entry[1] - '0') * 10) + (entry[2] - '0');
  return index < static_cast<int>(BAM2_PCM_FILE_COUNT) ? index : -1;
}

bool LoadBAM2HDDPCM(RuntimeState& runtime, const std::string& path, Error* error)
{
  if (!runtime.bam2_hdd.Open(path.c_str(), error))
    return false;

  std::array<u8, BAM2_SECTOR_SIZE> sector{};
  if (!runtime.bam2_hdd.ReadSector(0, sector.data()))
  {
    Error::SetStringView(error, "Bust a Move 2 HDD could not read sector 0.");
    return false;
  }

  u32 partition_lba = 0;
  if (!IsBAM2FAT32BootSector(sector))
  {
    bool found_partition = false;
    for (u32 i = 0; i < 4; i++)
    {
      const u8* const partition = sector.data() + 446 + (i * 16);
      const u8 type = partition[4];
      const u32 first_lba = ReadBAM2LE32(partition + 8);
      const u32 sector_count = ReadBAM2LE32(partition + 12);
      if ((type == 0x0b || type == 0x0c) && first_lba != 0 && sector_count != 0)
      {
        partition_lba = first_lba;
        found_partition = true;
        break;
      }
    }

    if (!found_partition || !runtime.bam2_hdd.ReadSector(partition_lba, sector.data()) ||
        !IsBAM2FAT32BootSector(sector))
    {
      Error::SetStringView(error, "Bust a Move 2 HDD does not contain the expected FAT32 volume.");
      return false;
    }
  }

  const u32 sectors_per_cluster = sector[13];
  const u32 reserved_sectors = ReadBAM2LE16(sector.data() + 14);
  const u32 fat_count = sector[16];
  const u32 sectors_per_fat = ReadBAM2LE32(sector.data() + 36);
  const u32 root_cluster = ReadBAM2LE32(sector.data() + 44) & UINT32_C(0x0fffffff);
  const u32 total_sectors16 = ReadBAM2LE16(sector.data() + 19);
  const u32 total_sectors32 = ReadBAM2LE32(sector.data() + 32);
  const u32 total_sectors = total_sectors16 != 0 ? total_sectors16 : total_sectors32;
  if (sectors_per_cluster == 0 || reserved_sectors == 0 || fat_count == 0 || sectors_per_fat == 0 ||
      root_cluster < 2 || total_sectors == 0)
  {
    Error::SetStringView(error, "Bust a Move 2 HDD has invalid FAT32 geometry.");
    return false;
  }

  const u64 volume_end_lba = static_cast<u64>(partition_lba) + total_sectors;
  if (volume_end_lba > runtime.bam2_hdd.GetBlockCount())
  {
    Error::SetStringView(error, "Bust a Move 2 HDD FAT32 volume extends past the end of the CHD.");
    return false;
  }

  const u64 fat_area = static_cast<u64>(fat_count) * sectors_per_fat;
  const u64 first_data_relative = static_cast<u64>(reserved_sectors) + fat_area;
  if (first_data_relative >= total_sectors)
  {
    Error::SetStringView(error, "Bust a Move 2 HDD FAT32 data area is invalid.");
    return false;
  }

  const u32 data_sectors = total_sectors - static_cast<u32>(first_data_relative);
  runtime.bam2_fat_start_lba = partition_lba + reserved_sectors;
  runtime.bam2_first_data_lba = partition_lba + static_cast<u32>(first_data_relative);
  runtime.bam2_sectors_per_cluster = sectors_per_cluster;
  runtime.bam2_cluster_count = data_sectors / sectors_per_cluster;
  if (runtime.bam2_cluster_count == 0)
  {
    Error::SetStringView(error, "Bust a Move 2 HDD FAT32 data area contains no clusters.");
    return false;
  }

  std::array<u32, BAM2_PCM_FILE_COUNT> first_clusters{};
  std::array<u32, BAM2_PCM_FILE_COUNT> file_sizes{};
  std::array<bool, BAM2_PCM_FILE_COUNT> found{};
  u32 found_count = 0;

  u32 cluster = root_cluster;
  u32 cached_fat_lba = UINT32_MAX;
  std::array<u8, BAM2_SECTOR_SIZE> fat_sector{};
  for (u32 chain_count = 0; chain_count < runtime.bam2_cluster_count; chain_count++)
  {
    if (cluster < 2 || cluster >= (runtime.bam2_cluster_count + 2))
      break;

    const u32 cluster_lba = GetBAM2ClusterLBA(runtime, cluster);
    bool end_of_directory = false;
    for (u32 sector_index = 0; sector_index < sectors_per_cluster && !end_of_directory; sector_index++)
    {
      if (!runtime.bam2_hdd.ReadSector(cluster_lba + sector_index, sector.data()))
      {
        Error::SetStringView(error, "Bust a Move 2 HDD root-directory read failed.");
        return false;
      }

      for (u32 offset = 0; offset < BAM2_SECTOR_SIZE; offset += 32)
      {
        const u8* const entry = sector.data() + offset;
        if (entry[0] == 0x00)
        {
          end_of_directory = true;
          break;
        }
        if (entry[0] == 0xe5 || entry[11] == 0x0f || (entry[11] & 0x18) != 0)
          continue;

        const int index = GetBAM2PCMFileIndex(entry);
        if (index < 0 || found[static_cast<size_t>(index)])
          continue;

        const u32 first_cluster = (static_cast<u32>(ReadBAM2LE16(entry + 20)) << 16) | ReadBAM2LE16(entry + 26);
        const u32 file_size = ReadBAM2LE32(entry + 28);
        if (first_cluster < 2 || file_size == 0 || (file_size % (BAM2_PCM_MIRROR_COUNT * 4)) != 0)
        {
          Error::SetStringFmt(error, "Bust a Move 2 HDD PCM file {:03}.4 has an invalid size or cluster.", index);
          return false;
        }

        first_clusters[static_cast<size_t>(index)] = first_cluster;
        file_sizes[static_cast<size_t>(index)] = file_size;
        found[static_cast<size_t>(index)] = true;
        found_count++;
      }
    }

    if (end_of_directory)
      break;

    u32 next_cluster = 0;
    if (!ReadBAM2FATEntry(runtime, cluster, &next_cluster, &cached_fat_lba, &fat_sector) ||
        next_cluster >= UINT32_C(0x0ffffff8))
    {
      break;
    }
    cluster = next_cluster;
  }

  if (found_count != BAM2_PCM_FILE_COUNT)
  {
    Error::SetStringFmt(error, "Bust a Move 2 HDD contains {} of {} expected PCM files.", found_count,
                        BAM2_PCM_FILE_COUNT);
    return false;
  }

  u64 total_pcm_bytes = 0;
  for (u32 i = 0; i < BAM2_PCM_FILE_COUNT; i++)
  {
    BAM2PCMFile& file = runtime.bam2_pcm_files[i];
    file.size = file_sizes[i];
    if (!BuildBAM2ClusterChain(runtime, first_clusters[i], file.size, &file.clusters))
    {
      Error::SetStringFmt(error, "Bust a Move 2 HDD PCM file {:03}.4 has an invalid FAT chain.", i);
      return false;
    }
    total_pcm_bytes += file.size;
  }

  VERBOSE_LOG(
    "SonyZN.BAM2 HDD mounted path='{}' fat32_files={} virtual_tracks={} mirrors={} pcm='44.1kHz s16le stereo' "
    "pcm_bytes={}",
    path, BAM2_PCM_FILE_COUNT, BAM2_PCM_FILE_COUNT * BAM2_PCM_MIRROR_COUNT, BAM2_PCM_MIRROR_COUNT,
    total_pcm_bytes);
  return true;
}

int GetBAM2CDPCMFileIndex(std::string_view name)
{
  if (name.size() != 6 || name[3] != '.' || name[4] != ';' || name[5] != '1' ||
      name[0] < '0' || name[0] > '9' || name[1] < '0' || name[1] > '9' || name[2] < '0' || name[2] > '9')
  {
    return -1;
  }

  const u32 index = static_cast<u32>(name[0] - '0') * 100 + static_cast<u32>(name[1] - '0') * 10 +
                    static_cast<u32>(name[2] - '0');
  return index < BAM2_CD_PCM_FILE_COUNT ? static_cast<int>(index) : -1;
}

bool LoadBAM2CDROMPCM(RuntimeState& runtime, const std::string& path, Error* error)
{
  if (!runtime.bam2_cdrom.Open(path, error))
    return false;

  std::array<u8, BAM2_CD_DATA_SECTOR_SIZE> sector{};
  if (!runtime.bam2_cdrom.ReadSector2048(16, sector.data()) || sector[0] != 1 ||
      std::memcmp(sector.data() + 1, "CD001", 5) != 0 || sector[6] != 1)
  {
    Error::SetStringView(error, "Bust a Move 2 CD-ROM is missing a valid ISO9660 primary volume descriptor.");
    return false;
  }

  std::string volume_label(reinterpret_cast<const char*>(sector.data() + 40), 32);
  while (!volume_label.empty() && (volume_label.back() == ' ' || volume_label.back() == '\0'))
    volume_label.pop_back();
  if (volume_label != "BUST_A_MOVE_2_ARCADE95_JAPAN")
  {
    Error::SetStringFmt(error, "Bust a Move 2 CD-ROM has unexpected ISO9660 volume label '{}'.", volume_label);
    return false;
  }

  const u32 volume_sectors = ReadBAM2LE32(sector.data() + 80);
  const u8 root_record_length = sector[156];
  if (volume_sectors == 0 || volume_sectors > runtime.bam2_cdrom.frame_count || root_record_length < 34 ||
      (156u + root_record_length) > sector.size())
  {
    Error::SetStringView(error, "Bust a Move 2 CD-ROM has an invalid ISO9660 root directory record.");
    return false;
  }

  const u8* const root_record = sector.data() + 156;
  const u32 root_extent = ReadBAM2LE32(root_record + 2);
  const u32 root_size = ReadBAM2LE32(root_record + 10);
  const u32 root_sector_count = (root_size + BAM2_CD_DATA_SECTOR_SIZE - 1) / BAM2_CD_DATA_SECTOR_SIZE;
  if (root_size == 0 || root_sector_count == 0 || root_extent >= volume_sectors ||
      root_sector_count > (volume_sectors - root_extent))
  {
    Error::SetStringView(error, "Bust a Move 2 CD-ROM root directory lies outside the ISO9660 volume.");
    return false;
  }

  std::array<bool, BAM2_CD_PCM_FILE_COUNT> found{};
  u32 found_count = 0;
  u64 total_pcm_bytes = 0;

  for (u32 root_sector = 0; root_sector < root_sector_count; root_sector++)
  {
    if (!runtime.bam2_cdrom.ReadSector2048(root_extent + root_sector, sector.data()))
    {
      Error::SetStringFmt(error, "Bust a Move 2 CD-ROM failed reading root directory sector {}.", root_sector);
      return false;
    }

    const u32 valid_bytes = static_cast<u32>(std::min<u64>(BAM2_CD_DATA_SECTOR_SIZE,
      static_cast<u64>(root_size) - static_cast<u64>(root_sector) * BAM2_CD_DATA_SECTOR_SIZE));
    for (u32 offset = 0; offset < valid_bytes;)
    {
      const u8 record_length = sector[offset];
      if (record_length == 0)
        break;
      if (record_length < 34 || (offset + record_length) > valid_bytes)
      {
        Error::SetStringView(error, "Bust a Move 2 CD-ROM contains an invalid ISO9660 directory record.");
        return false;
      }

      const u8* const entry = sector.data() + offset;
      const u8 name_length = entry[32];
      if ((33u + name_length) <= record_length && (entry[25] & UINT8_C(0x02)) == 0)
      {
        const std::string_view name(reinterpret_cast<const char*>(entry + 33), name_length);
        const int index = GetBAM2CDPCMFileIndex(name);
        if (index >= 0)
        {
          if (found[static_cast<u32>(index)])
          {
            Error::SetStringFmt(error, "Bust a Move 2 CD-ROM contains duplicate PCM file '{}'.", name);
            return false;
          }

          const u32 extent = ReadBAM2LE32(entry + 2);
          const u32 file_size = ReadBAM2LE32(entry + 10);
          const u32 file_sectors = (file_size + BAM2_CD_DATA_SECTOR_SIZE - 1) / BAM2_CD_DATA_SECTOR_SIZE;
          if (file_size == 0 || (file_size & 3u) != 0 || extent >= volume_sectors ||
              file_sectors > (volume_sectors - extent))
          {
            Error::SetStringFmt(error, "Bust a Move 2 CD-ROM PCM file '{}' has an invalid extent or size.", name);
            return false;
          }

          BAM2CDPCMFile& file = runtime.bam2_cd_pcm_files[static_cast<u32>(index)];
          file.extent_lba = extent;
          file.size = file_size;
          found[static_cast<u32>(index)] = true;
          found_count++;
          total_pcm_bytes += file_size;
        }
      }

      offset += record_length;
    }
  }

  if (found_count != BAM2_CD_PCM_FILE_COUNT)
  {
    Error::SetStringFmt(error, "Bust a Move 2 CD-ROM contains {} PCM files; expected {} (000.;1 through 026.;1).",
                        found_count, BAM2_CD_PCM_FILE_COUNT);
    return false;
  }

  VERBOSE_LOG(
    "SonyZN.BAM2 CD-ROM mounted path='{}' iso9660_volume='{}' pcm_files={} virtual_tracks={} "
    "pcm='44.1kHz s16le stereo' pcm_bytes={}",
    path, volume_label, BAM2_CD_PCM_FILE_COUNT, BAM2_CD_PCM_FILE_COUNT, total_pcm_bytes);
  return true;
}

bool IsBAM2CDTrackValid(const RuntimeState& runtime, u16 track)
{
  return runtime.bam2_media == BustAMove2Media::CDROM && track != 0 && track <= BAM2_CD_PCM_FILE_COUNT &&
         runtime.bam2_cd_pcm_files[track - 1].size != 0;
}

bool IsBAM2HDDTrackValid(const RuntimeState& runtime, u16 track)
{
  if (runtime.bam2_media != BustAMove2Media::HardDisk || track == 0 ||
      track > (BAM2_PCM_FILE_COUNT * BAM2_PCM_MIRROR_COUNT))
  {
    return false;
  }

  const u32 file_index = (track - 1) % BAM2_PCM_FILE_COUNT;
  return runtime.bam2_pcm_files[file_index].size != 0;
}

bool IsBAM2TrackValid(const RuntimeState& runtime, u16 track)
{
  return runtime.bam2_media == BustAMove2Media::HardDisk ? IsBAM2HDDTrackValid(runtime, track) :
                                                           IsBAM2CDTrackValid(runtime, track);
}

void ResetBAM2PCMBuffer(RuntimeState& runtime)
{
  runtime.bam2_pcm_prefetch_offset = runtime.bam2_pcm_track_start_offset;
  runtime.bam2_pcm_output_bytes = 0;
  runtime.bam2_pcm_cached_lba = UINT32_MAX;
  runtime.bam2_cd_cached_lba = UINT32_MAX;
  runtime.bam2_pcm_buffer_read = 0;
  runtime.bam2_pcm_buffer_write = 0;
  runtime.bam2_pcm_buffered_bytes = 0;
}

bool PrepareBAM2PCMTrack(RuntimeState& runtime, u16 track)
{
  if (!IsBAM2TrackValid(runtime, track))
    return false;

  if (runtime.bam2_media == BustAMove2Media::HardDisk)
  {
    const u32 virtual_track = track - 1;
    const u32 file_index = virtual_track % BAM2_PCM_FILE_COUNT;
    const u32 mirror_index = virtual_track / BAM2_PCM_FILE_COUNT;
    const BAM2PCMFile& file = runtime.bam2_pcm_files[file_index];
    const u32 mirror_bytes = file.size / BAM2_PCM_MIRROR_COUNT;

    runtime.bam2_pcm_track_start_offset = static_cast<u64>(mirror_index) * mirror_bytes;
    runtime.bam2_pcm_track_end_offset = runtime.bam2_pcm_track_start_offset + mirror_bytes;
  }
  else
  {
    const BAM2CDPCMFile& file = runtime.bam2_cd_pcm_files[track - 1];
    runtime.bam2_pcm_track_start_offset = 0;
    runtime.bam2_pcm_track_end_offset = file.size;
  }

  ResetBAM2PCMBuffer(runtime);
  return true;
}

bool PrefetchBAM2HDDPCM(RuntimeState& runtime, u32 max_bytes)
{
  const u32 virtual_track = runtime.bam2_selected_track - 1;
  const u32 file_index = virtual_track % BAM2_PCM_FILE_COUNT;
  const BAM2PCMFile& file = runtime.bam2_pcm_files[file_index];
  const u64 cluster_bytes = static_cast<u64>(runtime.bam2_sectors_per_cluster) * BAM2_SECTOR_SIZE;

  u32 bytes_to_fill = std::min<u32>(max_bytes, BAM2_PCM_BUFFER_SIZE - runtime.bam2_pcm_buffered_bytes);
  bytes_to_fill = static_cast<u32>(
    std::min<u64>(bytes_to_fill, runtime.bam2_pcm_track_end_offset - runtime.bam2_pcm_prefetch_offset));

  while (bytes_to_fill > 0)
  {
    const u64 file_offset = runtime.bam2_pcm_prefetch_offset;
    const u32 cluster_index = static_cast<u32>(file_offset / cluster_bytes);
    if (cluster_index >= file.clusters.size())
      return false;

    const u32 offset_in_cluster = static_cast<u32>(file_offset % cluster_bytes);
    const u32 sector_in_cluster = offset_in_cluster / BAM2_SECTOR_SIZE;
    const u32 offset_in_sector = offset_in_cluster % BAM2_SECTOR_SIZE;
    const u32 lba = GetBAM2ClusterLBA(runtime, file.clusters[cluster_index]) + sector_in_cluster;
    if (runtime.bam2_pcm_cached_lba != lba)
    {
      if (!runtime.bam2_hdd.ReadSector(lba, runtime.bam2_pcm_sector.data()))
        return false;
      runtime.bam2_pcm_cached_lba = lba;
    }

    const u32 sector_bytes = BAM2_SECTOR_SIZE - offset_in_sector;
    const u32 ring_bytes = BAM2_PCM_BUFFER_SIZE - runtime.bam2_pcm_buffer_write;
    const u32 copy_bytes = std::min({bytes_to_fill, sector_bytes, ring_bytes});
    std::memcpy(runtime.bam2_pcm_buffer.data() + runtime.bam2_pcm_buffer_write,
                runtime.bam2_pcm_sector.data() + offset_in_sector, copy_bytes);

    runtime.bam2_pcm_buffer_write = (runtime.bam2_pcm_buffer_write + copy_bytes) % BAM2_PCM_BUFFER_SIZE;
    runtime.bam2_pcm_buffered_bytes += copy_bytes;
    runtime.bam2_pcm_prefetch_offset += copy_bytes;
    bytes_to_fill -= copy_bytes;
  }

  return true;
}

bool PrefetchBAM2CDPCM(RuntimeState& runtime, u32 max_bytes)
{
  const BAM2CDPCMFile& file = runtime.bam2_cd_pcm_files[runtime.bam2_selected_track - 1];

  u32 bytes_to_fill = std::min<u32>(max_bytes, BAM2_PCM_BUFFER_SIZE - runtime.bam2_pcm_buffered_bytes);
  bytes_to_fill = static_cast<u32>(
    std::min<u64>(bytes_to_fill, runtime.bam2_pcm_track_end_offset - runtime.bam2_pcm_prefetch_offset));

  while (bytes_to_fill > 0)
  {
    const u64 file_offset = runtime.bam2_pcm_prefetch_offset;
    const u32 sector_in_file = static_cast<u32>(file_offset / BAM2_CD_DATA_SECTOR_SIZE);
    const u32 offset_in_sector = static_cast<u32>(file_offset % BAM2_CD_DATA_SECTOR_SIZE);
    const u32 lba = file.extent_lba + sector_in_file;
    if (runtime.bam2_cd_cached_lba != lba)
    {
      if (!runtime.bam2_cdrom.ReadSector2048(lba, runtime.bam2_cd_sector.data()))
        return false;
      runtime.bam2_cd_cached_lba = lba;
    }

    const u32 sector_bytes = BAM2_CD_DATA_SECTOR_SIZE - offset_in_sector;
    const u32 ring_bytes = BAM2_PCM_BUFFER_SIZE - runtime.bam2_pcm_buffer_write;
    const u32 copy_bytes = std::min({bytes_to_fill, sector_bytes, ring_bytes});
    std::memcpy(runtime.bam2_pcm_buffer.data() + runtime.bam2_pcm_buffer_write,
                runtime.bam2_cd_sector.data() + offset_in_sector, copy_bytes);

    runtime.bam2_pcm_buffer_write = (runtime.bam2_pcm_buffer_write + copy_bytes) % BAM2_PCM_BUFFER_SIZE;
    runtime.bam2_pcm_buffered_bytes += copy_bytes;
    runtime.bam2_pcm_prefetch_offset += copy_bytes;
    bytes_to_fill -= copy_bytes;
  }

  return true;
}

bool PrefetchBAM2PCM(RuntimeState& runtime, u32 max_bytes)
{
  if (runtime.bam2_selected_track == 0 || runtime.bam2_pcm_prefetch_offset >= runtime.bam2_pcm_track_end_offset ||
      runtime.bam2_pcm_buffered_bytes >= BAM2_PCM_BUFFER_SIZE || max_bytes == 0)
  {
    return true;
  }

  return runtime.bam2_media == BustAMove2Media::HardDisk ? PrefetchBAM2HDDPCM(runtime, max_bytes) :
                                                           PrefetchBAM2CDPCM(runtime, max_bytes);
}

void ExecuteBAM2H8Command(RuntimeState& runtime, u16 opcode, u16 argument)
{
  runtime.bam2_h8_status = UINT16_C(0x0004);

  switch (opcode)
  {
    case 0x0081:
    case 0x0087:
    {
      const u16 track = static_cast<u16>((opcode == 0x0081 ? 1 : 129) + (argument & UINT16_C(0x00ff)));
      if (!IsBAM2TrackValid(runtime, track))
      {
        runtime.bam2_selected_track = 0;
        runtime.bam2_h8_status = UINT16_C(0x0001);
        return;
      }

      // The game can re-issue SELECT for the track which is already playing.
      // Re-preparing that same active track discards the live PCM ring and
      // creates an artificial underrun before the next PLAY command.
      if (runtime.bam2_pcm_playing && runtime.bam2_selected_track == track)
        return;

      runtime.bam2_selected_track = track;
      if (!PrepareBAM2PCMTrack(runtime, track))
      {
        runtime.bam2_selected_track = 0;
        runtime.bam2_h8_status = UINT16_C(0x0001);
      }
      return;
    }

    case 0x0082:
      if (!IsBAM2TrackValid(runtime, runtime.bam2_selected_track))
      {
        runtime.bam2_h8_status = UINT16_C(0x0001);
        return;
      }

      // Track selection normally precedes PLAY by about a second, allowing
      // ProcessFrame() to stage media data. A repeated PLAY restarts the track.
      if (runtime.bam2_pcm_output_bytes != 0)
        ResetBAM2PCMBuffer(runtime);
      if (runtime.bam2_pcm_buffered_bytes < 4 &&
          !PrefetchBAM2PCM(runtime, BAM2_PCM_PREFETCH_BYTES_PER_FRAME))
      {
        runtime.bam2_h8_status = UINT16_C(0x0001);
        return;
      }

      runtime.bam2_pcm_playing = true;
      return;

    case 0x0083:
      runtime.bam2_pcm_playing = false;
      return;

    case 0x0084:
    case 0x0088:
      return;

    case 0x0086:
      runtime.bam2_tc9293_attenuation = static_cast<u8>(argument & UINT16_C(0x007f));
      return;

    default:
      runtime.bam2_h8_status = UINT16_C(0x0001);
      return;
  }
}

void WriteBAM2H8CommandWord(RuntimeState& runtime, u16 value)
{
  if (!runtime.bam2_h8_waiting_for_argument)
  {
    runtime.bam2_h8_opcode = value;
    runtime.bam2_h8_waiting_for_argument = true;
    runtime.bam2_h8_status = UINT16_C(0x0004);
    return;
  }

  // The game can retransmit an opcode while the H8 command port is still
  // waiting for that command's argument. Treating the repeated opcode as the
  // argument shifts the HLE parser by one word and can consume a following
  // STOP/PLAY as data. Keep the existing command pending when a known opcode
  // is repeated verbatim.
  if (value == runtime.bam2_h8_opcode)
  {
    switch (value)
    {
      case 0x0081:
      case 0x0082:
      case 0x0083:
      case 0x0084:
      case 0x0086:
      case 0x0087:
      case 0x0088:
        runtime.bam2_h8_status = UINT16_C(0x0004);
        return;

      default:
        break;
    }
  }

  const u16 opcode = runtime.bam2_h8_opcode;
  runtime.bam2_h8_waiting_for_argument = false;
  ExecuteBAM2H8Command(runtime, opcode, value);
}

u16 ReadBAM2MCUPort(RuntimeState& runtime, u32 port)
{
  if ((port & 3u) == 2)
    return runtime.bam2_h8_status;
  return runtime.bam2_mcu_ports[port & 3u];
}

u32 ReadBAM2MCU(RuntimeState& runtime, u32 width, u32 offset)
{
  if (offset < BAM2_MCU_BASE || offset > BAM2_MCU_END || (width != 1 && width != 2 && width != 4) ||
      (offset + width - 1) > BAM2_MCU_END)
  {
    return UINT32_C(0xffffffff);
  }

  u32 value = 0;
  for (u32 i = 0; i < width; i++)
  {
    const u32 byte_offset = (offset - BAM2_MCU_BASE) + i;
    const u32 port = byte_offset >> 1;
    const u32 shift = (byte_offset & 1u) * 8;
    value |= static_cast<u32>((ReadBAM2MCUPort(runtime, port) >> shift) & UINT16_C(0x00ff)) << (i * 8);
  }
  return value;
}

void ApplyBAM2MCUPortWrite(RuntimeState& runtime, u32 port)
{
  switch (port & 3u)
  {
    case 0:
    {
      const u8 new_bank = static_cast<u8>(runtime.bam2_mcu_ports[0] & UINT16_C(0x000f));
      if (runtime.bank != new_bank)
      {
        runtime.bank = new_bank;
        CPU::CodeCache::InvalidateBlocksInPhysicalRange(Bus::EXP1_BASE + BAM2_FIXED_ROM_SIZE, BAM2_BANK_SIZE);
      }
      break;
    }

    case 1:
      WriteBAM2H8CommandWord(runtime, runtime.bam2_mcu_ports[1]);
      break;

    case 2:
      // The game writes 0x0007 before a command sequence and 0x0001 when
      // acknowledging a stream error. Both clear the externally-visible
      // command/error result before the next H8 transaction.
      runtime.bam2_h8_status = UINT16_C(0x0004);
      break;

    case 3:
    {
      // Active-low two-phase cabinet output/lamp latch. Bit 0 selects which
      // seven-line group is being presented; bits 1..7 are that group's
      // active-low output payload. The mapping below was validated directly
      // against BAM2's fourteen-entry OUTPUT TEST menu.
      //
      // Phase 1:
      //   line 0 = 1P_BUTTONS_LAMP
      //   line 1 = 1P_JAMMAR
      //   line 2 = 1P_RHYTHM
      //   line 3 = 2P_JAMMAR
      //   line 4 = 2P_RHYTHM
      //   line 5 = LAY_LAMP_AND_MUSIC
      //   line 6 = LAY_LAMP
      //
      // Phase 0:
      //   line 0 = HALOGEN_LIGHT_1
      //   line 1 = HALOGEN_LIGHT_2
      //   line 2 = HALOGEN_LIGHT_3
      //   line 3 = HALOGEN_LIGHT_4
      //   line 4 = CD_MOTOR
      //   line 5 = 2P_BUTTONS_LAMP
      //   line 6 = CD_LAMP
      static constexpr const char* PHASE0_NAMES[7] = {
        "halogen_light_1", "halogen_light_2", "halogen_light_3", "halogen_light_4",
        "cd_motor", "2p_buttons_lamp", "cd_lamp",
      };
      static constexpr const char* PHASE1_NAMES[7] = {
        "1p_buttons_lamp", "1p_jammar", "1p_rhythm", "2p_jammar",
        "2p_rhythm", "lay_lamp_and_music", "lay_lamp",
      };
      static constexpr const char* MENU_ORDER_NAMES[14] = {
        "1p_buttons_lamp",
        "2p_buttons_lamp",
        "1p_rhythm",
        "1p_jammar",
        "2p_rhythm",
        "2p_jammar",
        "lay_lamp_and_music",
        "lay_lamp",
        "halogen_light_1",
        "halogen_light_2",
        "halogen_light_3",
        "halogen_light_4",
        "cd_motor",
        "cd_lamp",
      };

      if (!runtime.bam2_outputs_initialized)
      {
        for (const char* const name : MENU_ORDER_NAMES)
          Arcade::Output::SetValue(name, 0, "BAM2 H8 MCU port 3");

        runtime.bam2_outputs_initialized = true;
      }

      const u16 current = runtime.bam2_mcu_ports[3];
      const u32 phase = static_cast<u32>(current & UINT16_C(0x0001));
      const char* const* names = (phase != 0) ? PHASE1_NAMES : PHASE0_NAMES;

      for (u32 line = 0; line < 7; line++)
      {
        const u32 raw_bit = line + 1;
        const s32 active = ((current & static_cast<u16>(UINT16_C(1) << raw_bit)) != 0) ? 0 : 1;
        Arcade::Output::SetValue(names[line], active, "BAM2 H8 MCU port 3");
      }
      break;
    }
  }
}

void WriteBAM2MCU(RuntimeState& runtime, u32 width, u32 offset, u32 value)
{
  if (offset < BAM2_MCU_BASE || offset > BAM2_MCU_END || (width != 1 && width != 2 && width != 4) ||
      (offset + width - 1) > BAM2_MCU_END)
  {
    return;
  }

  std::array<bool, 4> touched{};
  for (u32 i = 0; i < width; i++)
  {
    const u32 byte_offset = (offset - BAM2_MCU_BASE) + i;
    const u32 port = byte_offset >> 1;
    const u32 shift = (byte_offset & 1u) * 8;
    const u16 mask = static_cast<u16>(UINT16_C(0x00ff) << shift);
    const u16 byte_value = static_cast<u16>(((value >> (i * 8)) & UINT32_C(0xff)) << shift);
    runtime.bam2_mcu_ports[port] = static_cast<u16>((runtime.bam2_mcu_ports[port] & ~mask) | byte_value);
    touched[port] = true;
  }

  for (u32 port = 0; port < touched.size(); port++)
  {
    if (touched[port])
      ApplyBAM2MCUPortWrite(runtime, port);
  }
}

bool ReadBAM2PCMFrame(RuntimeState& runtime, s16* left, s16* right)
{
  if (!left || !right || !runtime.bam2_pcm_playing || !IsBAM2TrackValid(runtime, runtime.bam2_selected_track))
    return false;

  if (runtime.bam2_pcm_buffered_bytes < 4)
  {
    if (runtime.bam2_pcm_prefetch_offset >= runtime.bam2_pcm_track_end_offset)
      runtime.bam2_pcm_playing = false;
    return false;
  }

  std::array<u8, 4> frame{};
  for (u32 i = 0; i < frame.size(); i++)
  {
    frame[i] = runtime.bam2_pcm_buffer[runtime.bam2_pcm_buffer_read];
    runtime.bam2_pcm_buffer_read = (runtime.bam2_pcm_buffer_read + 1) % BAM2_PCM_BUFFER_SIZE;
  }
  runtime.bam2_pcm_buffered_bytes -= static_cast<u32>(frame.size());
  runtime.bam2_pcm_output_bytes += frame.size();

  *left = static_cast<s16>(ReadBAM2LE16(frame.data()));
  *right = static_cast<s16>(ReadBAM2LE16(frame.data() + 2));
  return true;
}

void ApplySecuritySelect(RuntimeState& runtime, u8 value)
{
  const bool was_mcu_selected = runtime.mcu_selected;
  const bool mcu_selected = IsZNMCUSelected(value);

  runtime.security_select = value;
  runtime.motherboard_cat702.SetSelectLine((value & 0x04) != 0);
  runtime.game_cat702.SetSelectLine((value & 0x08) != 0);

  // Eighting/Raizing folds the 8 MiB game-ROM bank selector into the ordinary
  // ZN security-select register. Bits 1:0 select one of four decode states;
  // the validated RA9701 content populates banks 0-2.
  if (runtime.board_type == BoardType::EightingRaizingZN1)
  {
    const u8 new_bank = value & UINT8_C(0x03);
    if (runtime.bank != new_bank)
    {
      runtime.bank = new_bank;
      CPU::CodeCache::InvalidateBlocksInPhysicalRange(Bus::EXP1_BASE, EIGHTING_EXP1_SIZE);

    }
  }

  if (!was_mcu_selected && mcu_selected)
    BeginZNMCUTransaction(runtime);
  else if (was_mcu_selected && !mcu_selected)
    EndZNMCUTransaction(runtime);

}

TickCount TimeWarnerWatchdogTimeoutTicks(const RuntimeState&)
{
  // PSXTRA uses a DS1232S with TD floating: retain the hardware 600 ms watchdog
  // period now that the COH-1000W high-speed CAT702 transfer path is no longer
  // stretched by byte-granular scheduler latency.
  return std::max<TickCount>((System::GetTicksPerSecond() * 3) / 5, 1);
}

void TimeWarnerWatchdogEventCallback(void*, TickCount, TickCount)
{
  if (s_time_warner_watchdog_event)
    s_time_warner_watchdog_event->Deactivate();

  if (!s_runtime || s_runtime->board_type != BoardType::TimeWarnerZN1)
    return;

  s_runtime->time_warner_main_board_reset_pending = true;
  WARNING_LOG("SonyZN Time Warner DS1232 watchdog timeout; requesting full main-board reset.");
}

void ClockTimeWarnerWatchdog(RuntimeState& runtime)
{
  runtime.time_warner_main_board_reset_pending = false;
  if (s_time_warner_watchdog_event)
    s_time_warner_watchdog_event->Schedule(TimeWarnerWatchdogTimeoutTicks(runtime));
}
TickCount TaitoWatchdogTimeoutTicks()
{
  // MB3773 tWD is set by the external CT capacitor: tWD(ms) ~= 100 * CT(uF).
  // The exact Taito SROM PCB-A CT value is not yet known. Retain the proven
  // five-second functional timeout temporarily rather than inventing a board value.
  // This constant is provisional and must not be treated as hardware-final timing.
  return std::max<TickCount>(System::GetTicksPerSecond() * 5, 1);
}

void TaitoWatchdogEventCallback(void*, TickCount, TickCount)
{
  if (s_taito_watchdog_event)
    s_taito_watchdog_event->Deactivate();

  if (!s_runtime || (s_runtime->board_type != BoardType::TaitoFX1A &&
                       s_runtime->board_type != BoardType::TaitoFX1B))
  {
    return;
  }

  s_runtime->taito_main_board_reset_pending = true;
  WARNING_LOG("SonyZN Taito FX-1 MB3773 watchdog timeout; requesting full main-board reset.");
}
void ClockTaitoWatchdog(RuntimeState& runtime, bool ck)
{
  // MB3773 CK is negative-edge triggered. Only a High->Low transition
  // restarts the watchdog; rising edges merely update the sampled CK state.
  const bool falling_edge = runtime.taito_watchdog_seen && runtime.taito_watchdog_ck && !ck;
  runtime.taito_watchdog_seen = true;
  runtime.taito_watchdog_ck = ck;

  if (!falling_edge)
    return;

  runtime.taito_main_board_reset_pending = false;
  if (s_taito_watchdog_event)
    s_taito_watchdog_event->Schedule(TaitoWatchdogTimeoutTicks());
}

} // namespace

std::optional<BIOS::Image> LoadFirmwareBIOS(const char* firmware_archive_path,
                                            const Arcade::Database::FirmwareDefinition& firmware,
                                            std::string_view bios_variant, Error* error)
{
  const Arcade::Database::ROMDefinition* bios_rom = FindFirmwareROM(firmware, "maincpu:rom", bios_variant);
  if (!bios_rom || bios_rom->size != BIOS::BIOS_SIZE)
  {
    Error::SetStringFmt(error, "Sony ZN firmware '{}' has no valid '{}' 512 KiB BIOS definition.", firmware.id,
                        bios_variant);
    return std::nullopt;
  }

  std::vector<u8> data;
  if (!LoadROMMember(firmware_archive_path, *bios_rom, &data, error))
    return std::nullopt;

  BIOS::Image image{};
  image.info = nullptr;
  image.data.resize(BIOS::BIOS_SIZE);
  std::memcpy(image.data.data(), data.data(), BIOS::BIOS_SIZE);
  image.hash = MD5Digest::HashData(image.data);

  VERBOSE_LOG("SonyZN BIOS validated archive='{}' member='{}' variant='{}' size={}", firmware_archive_path,
           bios_rom->name, bios_variant, static_cast<u32>(BIOS::BIOS_SIZE));
  return image;
}

std::optional<CapcomZN1Content> LoadCapcomZN1Content(const char* archive_path,
                                                    const Arcade::Database::GameDefinition& game,
                                                    const char* firmware_archive_path,
                                                    const Arcade::Database::FirmwareDefinition& firmware,
                                                    Error* error)
{
  const Arcade::Database::ROMRegionDefinition* country_region = FindRegion(firmware, "countryrom");
  const Arcade::Database::ROMRegionDefinition* banked_region = FindRegion(firmware, "bankedroms");
  const Arcade::Database::ROMRegionDefinition* audio_cpu_region = FindRegion(firmware, "audiocpu");
  const Arcade::Database::ROMRegionDefinition* qsound_region = FindRegion(firmware, "qsound");
  if (!country_region || !banked_region || !audio_cpu_region || !qsound_region || country_region->size != 0x80000 ||
      banked_region->size != 0x2400000 || audio_cpu_region->size != 0x40000 || qsound_region->size != 0x400000)
  {
    Error::SetStringView(error, "Capcom ZN-1 firmware definition has unexpected ROM region sizes.");
    return std::nullopt;
  }

  CapcomZN1Content content;
  content.set_name = game.id;
  if (game.hardware_profile == "coh1000c")
  {
    content.use_2mb_vram = false;
  }
  else if (game.hardware_profile == "coh1002c")
  {
    content.use_2mb_vram = true;
  }
  else
  {
    Error::SetStringFmt(error, "Unsupported Capcom ZN-1 hardware profile '{}'.", game.hardware_profile);
    return std::nullopt;
  }

  content.country_rom.assign(country_region->size, country_region->has_erase_value ? country_region->erase_value : 0);
  content.banked_rom.assign(banked_region->size, banked_region->has_erase_value ? banked_region->erase_value : 0);
  content.audio_cpu_rom.assign(audio_cpu_region->size,
                               audio_cpu_region->has_erase_value ? audio_cpu_region->erase_value : 0);
  content.qsound_rom.assign(qsound_region->size, qsound_region->has_erase_value ? qsound_region->erase_value : 0);

  const Arcade::Database::ROMDefinition* motherboard_key_rom = FindFirmwareROM(firmware, "cat702_1", {});
  if (!motherboard_key_rom || motherboard_key_rom->size != content.motherboard_cat702_key.size())
  {
    Error::SetStringView(error, "Capcom ZN-1 firmware definition is missing the motherboard CAT702 key.");
    return std::nullopt;
  }

  std::vector<u8> key_data;
  if (!LoadROMMember(firmware_archive_path, *motherboard_key_rom, &key_data, error))
    return std::nullopt;
  std::copy(key_data.begin(), key_data.end(), content.motherboard_cat702_key.begin());

  u32 country_count = 0;
  u32 banked_count = 0;
  u32 audio_cpu_count = 0;
  u32 qsound_count = 0;
  u32 game_key_count = 0;

  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "countryrom" || rom.region == "bankedroms" || rom.region == "audiocpu" ||
        rom.region == "qsound")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;

      if (rom.region == "countryrom")
      {
        if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.country_rom), error))
          return std::nullopt;
        country_count++;
      }
      else if (rom.region == "bankedroms")
      {
        if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.banked_rom), error))
          return std::nullopt;
        banked_count++;
      }
      else if (rom.region == "audiocpu")
      {
        if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.audio_cpu_rom), error))
          return std::nullopt;
        audio_cpu_count++;
      }
      else
      {
        if (!PlaceQSoundROM(rom, member_data, std::span<u8>(content.qsound_rom), error))
          return std::nullopt;
        qsound_count++;
      }
    }
    else if (rom.region == "cat702_2")
    {
      if (rom.size != content.game_cat702_key.size())
      {
        Error::SetStringFmt(error, "Capcom ZN-1 game CAT702 key '{}' has unexpected size {}.", rom.name, rom.size);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;

      std::copy(member_data.begin(), member_data.end(), content.game_cat702_key.begin());
      game_key_count++;
    }
  }

  const bool qsound_program_present = (audio_cpu_count != 0);
  const bool qsound_samples_present = (qsound_count != 0);
  if (qsound_program_present != qsound_samples_present)
  {
    Error::SetStringView(error, "Capcom ZN-1 game definition has an incomplete QSound ROM population.");
    return std::nullopt;
  }

  content.qsound_enabled = qsound_program_present;
  if (!content.qsound_enabled &&
      (!audio_cpu_region->has_erase_value || audio_cpu_region->erase_value != UINT8_C(0xff) ||
       !qsound_region->has_erase_value || qsound_region->erase_value != UINT8_C(0xff)))
  {
    Error::SetStringView(
      error, "Capcom ZN-1 game definition omits QSound ROMs without declaring the sockets erased/unpopulated.");
    return std::nullopt;
  }

  if (country_count != 1 || banked_count == 0 || game_key_count != 1)
  {
    Error::SetStringView(error, "Capcom ZN-1 game definition is missing required country, banked, or CAT702 ROMs.");
    return std::nullopt;
  }

  VERBOSE_LOG(
    "SonyZN Capcom content validated set='{}' country={} bytes banked={} bytes banked_roms={} "
    "audio_cpu={} bytes audio_roms={} qsound={} bytes qsound_roms={} qsound_enabled={} cat702_keys=2",
    content.set_name, content.country_rom.size(), content.banked_rom.size(), banked_count, content.audio_cpu_rom.size(),
    audio_cpu_count, content.qsound_rom.size(), qsound_count, content.qsound_enabled);
  return content;
}

std::optional<VideoSystemZN1Content> LoadVideoSystemZN1Content(
  const char* archive_path, const Arcade::Database::GameDefinition& game, const char* firmware_archive_path,
  const Arcade::Database::FirmwareDefinition& firmware, Error* error)
{
  if (game.hardware_profile != "coh1002v")
  {
    Error::SetStringFmt(error, "Unsupported Video System ZN-1 hardware profile '{}'.", game.hardware_profile);
    return std::nullopt;
  }

  const Arcade::Database::ROMRegionDefinition* fixed_region = FindRegion(firmware, "fixedroms");
  const Arcade::Database::ROMRegionDefinition* banked_region = FindRegion(firmware, "bankedroms");
  if (!fixed_region || fixed_region->size != VIDEO_SYSTEM_FIXED_ROM_SIZE || !banked_region ||
      banked_region->size != VIDEO_SYSTEM_BANKED_ROM_SIZE)
  {
    Error::SetStringView(error, "Video System ZN-1 firmware definition has unexpected ROM region sizes.");
    return std::nullopt;
  }

  VideoSystemZN1Content content;
  content.set_name = game.id;
  content.fixed_rom.assign(fixed_region->size, fixed_region->has_erase_value ? fixed_region->erase_value : 0);
  content.banked_rom.assign(banked_region->size, banked_region->has_erase_value ? banked_region->erase_value : 0);

  const Arcade::Database::ROMDefinition* motherboard_key_rom = FindFirmwareROM(firmware, "cat702_1", {});
  if (!motherboard_key_rom || motherboard_key_rom->size != content.motherboard_cat702_key.size())
  {
    Error::SetStringView(error, "Video System ZN-1 firmware definition is missing the motherboard CAT702 key.");
    return std::nullopt;
  }

  std::vector<u8> key_data;
  if (!LoadROMMember(firmware_archive_path, *motherboard_key_rom, &key_data, error))
    return std::nullopt;
  std::copy(key_data.begin(), key_data.end(), content.motherboard_cat702_key.begin());

  u32 fixed_count = 0;
  u32 banked_count = 0;
  u32 game_key_count = 0;
  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "fixedroms" || rom.region == "bankedroms")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;

      if (rom.region == "fixedroms")
      {
        if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.fixed_rom), error))
          return std::nullopt;
        fixed_count++;
      }
      else
      {
        if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.banked_rom), error))
          return std::nullopt;
        banked_count++;
      }
    }
    else if (rom.region == "cat702_2")
    {
      if (rom.size != content.game_cat702_key.size())
      {
        Error::SetStringFmt(error, "Video System ZN-1 game CAT702 key '{}' has unexpected size {}.", rom.name,
                            rom.size);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      std::copy(member_data.begin(), member_data.end(), content.game_cat702_key.begin());
      game_key_count++;
    }
  }

  if (fixed_count == 0 || banked_count == 0 || game_key_count != 1)
  {
    Error::SetStringView(error, "Video System ZN-1 game definition is missing required fixed, banked, or CAT702 ROMs.");
    return std::nullopt;
  }

  VERBOSE_LOG(
    "SonyZN Video System content validated set='{}' fixed={} bytes fixed_roms={} banked={} bytes "
    "banked_roms={} cat702_keys=2",
    content.set_name, content.fixed_rom.size(), fixed_count, content.banked_rom.size(), banked_count);
  return content;
}

std::optional<AtlusZN1Content> LoadAtlusZN1Content(const char* archive_path,
                                                    const Arcade::Database::GameDefinition& game,
                                                    const char* firmware_archive_path,
                                                    const Arcade::Database::FirmwareDefinition& firmware,
                                                    Error* error)
{
  if (game.hardware_profile != "coh1001l" || game.id != "hvnsgate")
  {
    Error::SetStringFmt(error, "Unsupported Atlus ZN-1 set '{}' with hardware profile '{}'.", game.id,
                        game.hardware_profile);
    return std::nullopt;
  }

  const Arcade::Database::ROMRegionDefinition* banked_region = FindRegion(firmware, "bankedroms");
  const Arcade::Database::ROMRegionDefinition* audio_region = FindRegion(firmware, "audiocpu");
  if (!banked_region || banked_region->size != ATLUS_BANKED_ROM_SIZE || !audio_region ||
      audio_region->size != ATLUS_AUDIO_ROM_SIZE)
  {
    Error::SetStringView(error, "Atlus ZN-1 firmware definition has unexpected ROM region sizes.");
    return std::nullopt;
  }

  AtlusZN1Content content;
  content.set_name = game.id;
  content.banked_rom.assign(banked_region->size, banked_region->has_erase_value ? banked_region->erase_value : 0);
  content.audio_cpu_rom.assign(ATLUS_AUDIO_ROM_SIZE, UINT8_C(0xff));
  content.ymz280b_rom.assign(ATLUS_YMZ280B_ROM_SIZE, UINT8_C(0xff));

  const Arcade::Database::ROMDefinition* motherboard_key_rom = FindFirmwareROM(firmware, "cat702_1", {});
  if (!motherboard_key_rom || motherboard_key_rom->size != content.motherboard_cat702_key.size())
  {
    Error::SetStringView(error, "Atlus ZN-1 firmware definition is missing the motherboard CAT702 key.");
    return std::nullopt;
  }

  std::vector<u8> key_data;
  if (!LoadROMMember(firmware_archive_path, *motherboard_key_rom, &key_data, error))
    return std::nullopt;
  std::copy(key_data.begin(), key_data.end(), content.motherboard_cat702_key.begin());

  u32 banked_count = 0;
  u32 audio_cpu_count = 0;
  u32 ymz280b_count = 0;
  u32 game_key_count = 0;
  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "bankedroms")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error) ||
          !PlaceAtlusROM(rom, member_data, std::span<u8>(content.banked_rom), error))
      {
        return std::nullopt;
      }
      banked_count++;
    }
    else if (rom.region == "audiocpu")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error) ||
          !PlaceAtlusROM(rom, member_data, std::span<u8>(content.audio_cpu_rom), error))
      {
        return std::nullopt;
      }
      audio_cpu_count++;
    }
    else if (rom.region == "ymz280b")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error) ||
          !PlaceSimpleROM(rom, member_data, std::span<u8>(content.ymz280b_rom), error))
      {
        return std::nullopt;
      }
      ymz280b_count++;
    }
    else if (rom.region == "cat702_2")
    {
      if (rom.size != content.game_cat702_key.size())
      {
        Error::SetStringFmt(error, "Atlus ZN-1 game CAT702 key '{}' has unexpected size {}.", rom.name, rom.size);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      std::copy(member_data.begin(), member_data.end(), content.game_cat702_key.begin());
      game_key_count++;
    }
  }

  if (banked_count != 7 || audio_cpu_count != 2 || ymz280b_count != 2 || game_key_count != 1)
  {
    Error::SetStringView(error,
                         "Atlus ZN-1 game definition is missing required banked, 68000, YMZ280B, or CAT702 ROMs.");
    return std::nullopt;
  }

  VERBOSE_LOG(
    "SonyZN Atlus content validated set='{}' banked={} bytes banked_roms={} audiocpu={} bytes audio_roms={} "
    "ymz280b={} bytes ymz_roms={} cat702_keys=2",
    content.set_name, content.banked_rom.size(), banked_count, content.audio_cpu_rom.size(), audio_cpu_count,
    content.ymz280b_rom.size(), ymz280b_count);
  return content;
}

std::optional<EightingRaizingZN1Content> LoadEightingRaizingZN1Content(
  const char* archive_path, const Arcade::Database::GameDefinition& game, const char* firmware_archive_path,
  const Arcade::Database::FirmwareDefinition& firmware, Error* error)
{
  const bool brave_blade = IsBraveBladeSet(game.id);
  const bool supported_eighting =
    game.id == "beastrzr" || game.id == "beastrzra" || game.id == "bldyroar" || IsBloodyRoar2Set(game.id) ||
    brave_blade;
  if (!supported_eighting || game.hardware_profile != "coh1002e")
  {
    Error::SetStringFmt(error, "Unsupported Eighting/Raizing ZN-1 set '{}' with hardware profile '{}'.",
                        game.id, game.hardware_profile);
    return std::nullopt;
  }

  const Arcade::Database::ROMRegionDefinition* banked_region = FindRegion(firmware, "bankedroms");
  const Arcade::Database::ROMRegionDefinition* audio_region = FindRegion(firmware, "audiocpu");
  const Arcade::Database::ROMRegionDefinition* ymf_region = FindRegion(firmware, "ymf");

  // Brave Blade is the hybrid: COH-1002M/MG01 motherboard BIOS and security,
  // with the Eighting/Raizing PS9805 game/sound board. The Tecmo firmware
  // definition therefore has a larger generic banked region and no Eighting
  // audio regions. The daughterboard sizes are fixed by the Brave Blade ROM
  // layout rather than inherited from the Tecmo firmware schema.
  if (!brave_blade &&
      (!banked_region || banked_region->size != EIGHTING_BANKED_ROM_SIZE || !audio_region ||
       audio_region->size != EIGHTING_AUDIO_ROM_SIZE || !ymf_region || ymf_region->size != EIGHTING_YMF271_ROM_SIZE))
  {
    Error::SetStringView(error, "Eighting/Raizing ZN-1 firmware definition has unexpected ROM region sizes.");
    return std::nullopt;
  }

  const size_t banked_rom_size = brave_blade ? EIGHTING_BANKED_ROM_SIZE : banked_region->size;

  // RA9701 carries a 512 KiB 68000 image. PS9805 populates two 27C040s as an
  // interleaved 1 MiB region while retaining the same 0x000000-0x07FFFF CPU
  // decode. Size the retained image from the ROM placement rather than from a
  // game-name exception. Brave Blade starts from the 1 MiB PS9805 layout
  // because its Tecmo motherboard firmware does not describe the sound board.
  size_t audio_cpu_size = brave_blade ? EIGHTING_AUDIO_ROM_MAX_SIZE : audio_region->size;
  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region != "audiocpu" || rom.size == 0)
      continue;

    size_t extent = 0;
    if (rom.segments.empty() && !rom.word_swap && rom.group_size == 1 && rom.interleave == 1 && rom.skip == 0)
    {
      extent = static_cast<size_t>(rom.offset) + static_cast<size_t>(rom.size);
    }
    else if (rom.segments.empty() && !rom.word_swap && rom.group_size == 1 && rom.interleave == 2 && rom.skip == 1)
    {
      extent = static_cast<size_t>(rom.offset) + ((static_cast<size_t>(rom.size) - 1) * 2) + 1;
    }

    audio_cpu_size = std::max(audio_cpu_size, extent);
  }

  if (audio_cpu_size < EIGHTING_AUDIO_ROM_SIZE || audio_cpu_size > EIGHTING_AUDIO_ROM_MAX_SIZE)
  {
    Error::SetStringFmt(error, "Eighting/Raizing ZN-1 68000 ROM region has unsupported size {}.", audio_cpu_size);
    return std::nullopt;
  }

  EightingRaizingZN1Content content;
  content.set_name = game.id;
  content.uses_tecmo_motherboard = brave_blade;
  content.banked_rom.assign(banked_rom_size,
                            brave_blade ? UINT8_C(0xff) :
                                          (banked_region->has_erase_value ? banked_region->erase_value : 0));
  content.audio_cpu_rom.assign(audio_cpu_size,
                               brave_blade ? UINT8_C(0xff) :
                                             (audio_region->has_erase_value ? audio_region->erase_value : 0));
  content.ymf271_rom.assign(EIGHTING_YMF271_ROM_SIZE,
                            brave_blade ? UINT8_C(0xff) :
                                          (ymf_region->has_erase_value ? ymf_region->erase_value : 0));

  const Arcade::Database::ROMDefinition* motherboard_key_rom = FindFirmwareROM(firmware, "cat702_1", {});
  if (!motherboard_key_rom || motherboard_key_rom->size != content.motherboard_cat702_key.size())
  {
    Error::SetStringView(error, "Eighting/Raizing ZN-1 firmware definition is missing the motherboard CAT702 key.");
    return std::nullopt;
  }

  std::vector<u8> key_data;
  if (!LoadROMMember(firmware_archive_path, *motherboard_key_rom, &key_data, error))
    return std::nullopt;
  std::copy(key_data.begin(), key_data.end(), content.motherboard_cat702_key.begin());

  u32 banked_count = 0;
  u32 audio_cpu_count = 0;
  u32 ymf271_count = 0;
  u32 at28_count = 0;
  u32 game_key_count = 0;
  u32 ps9805_flash_count = 0;
  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "bankedroms")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error) ||
          !PlaceEightingROM(rom, member_data, std::span<u8>(content.banked_rom), error))
      {
        return std::nullopt;
      }
      if (rom.name == "flash0.021" || rom.name == "flash1.024")
        ps9805_flash_count++;
      banked_count++;
    }
    else if (rom.region == "audiocpu")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error) ||
          !PlaceEightingROM(rom, member_data, std::span<u8>(content.audio_cpu_rom), error))
      {
        return std::nullopt;
      }
      audio_cpu_count++;
    }
    else if (rom.region == "ymf")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error) ||
          !PlaceSimpleROM(rom, member_data, std::span<u8>(content.ymf271_rom), error))
      {
        return std::nullopt;
      }
      ymf271_count++;
    }
    else if (rom.region == "at28c16")
    {
      if (rom.size != AT28_SIZE || at28_count != 0)
      {
        Error::SetStringFmt(error, "Eighting/Raizing ZN-1 AT28C16 image '{}' has unexpected size or duplicate definition.",
                            rom.name);
        return std::nullopt;
      }

      if (!LoadROMMember(archive_path, rom, &content.at28_initial, error))
        return std::nullopt;
      at28_count++;
    }
    else if (rom.region == "cat702_2")
    {
      if (rom.size != content.game_cat702_key.size())
      {
        Error::SetStringFmt(error, "Eighting/Raizing ZN-1 game CAT702 key '{}' has unexpected size {}.",
                            rom.name, rom.size);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      std::copy(member_data.begin(), member_data.end(), content.game_cat702_key.begin());
      game_key_count++;
    }
  }

  const u32 expected_banked_count = brave_blade ? 3 : 6;
  if (banked_count != expected_banked_count || audio_cpu_count != 2 || ymf271_count != 1 || game_key_count != 1)
  {
    Error::SetStringView(
      error, "Eighting/Raizing ZN-1 definition is missing required game, 68000, YMF271, or CAT702 ROMs.");
    return std::nullopt;
  }

  if ((IsBloodyRoar2Set(game.id) || brave_blade) && at28_count != 1)
  {
    Error::SetStringFmt(error, "{} PS9805 definition is missing its AT28C16 factory image.", game.id);
    return std::nullopt;
  }

  content.has_ps9805_flash = (ps9805_flash_count == 2);

  VERBOSE_LOG(
    "SonyZN Eighting/Raizing content validated set='{}' banked={} bytes banked_roms={} audiocpu={} bytes "
    "audio_roms={} ymf271={} bytes ymf_roms={} at28_seed={} ps9805_flash={} cat702_keys=2",
    content.set_name, content.banked_rom.size(), banked_count, content.audio_cpu_rom.size(), audio_cpu_count,
    content.ymf271_rom.size(), ymf271_count, at28_count == 1, content.has_ps9805_flash);
  return content;
}


std::optional<BustAMove2ZN1Content> LoadBustAMove2ZN1Content(
  const char* archive_path, const Arcade::Database::GameDefinition& game, const char* firmware_archive_path,
  const Arcade::Database::FirmwareDefinition& firmware, Error* error)
{
  const bool is_hdd = game.id == "bam2" && game.hardware_profile == "bam2hdd";
  const bool is_cdrom = game.id == "bam2a" && game.hardware_profile == "bam2cdrom";
  if (!is_hdd && !is_cdrom)
  {
    Error::SetStringFmt(error, "Unsupported Bust a Move 2 set '{}' with hardware profile '{}'.", game.id,
                        game.hardware_profile);
    return std::nullopt;
  }

  const std::optional<size_t> banked_size = GetSimpleGameRegionSize(game, "bankedroms");
  if (!banked_size || *banked_size != BAM2_BANKED_ROM_SIZE)
  {
    Error::SetStringFmt(error, "Bust a Move 2 banked ROM layout has unexpected size {}; expected {}.",
                        banked_size.value_or(0), BAM2_BANKED_ROM_SIZE);
    return std::nullopt;
  }

  BustAMove2ZN1Content content;
  content.set_name = game.id;
  content.media = is_cdrom ? BustAMove2Media::CDROM : BustAMove2Media::HardDisk;
  content.banked_rom.assign(BAM2_BANKED_ROM_SIZE, UINT8_C(0xff));

  const Arcade::Database::ROMDefinition* motherboard_key_rom = FindFirmwareROM(firmware, "cat702_1", {});
  if (!motherboard_key_rom || motherboard_key_rom->size != content.motherboard_cat702_key.size())
  {
    Error::SetStringView(error, "Bust a Move 2 firmware definition is missing the motherboard CAT702 key.");
    return std::nullopt;
  }

  std::vector<u8> key_data;
  if (!LoadROMMember(firmware_archive_path, *motherboard_key_rom, &key_data, error))
    return std::nullopt;
  std::copy(key_data.begin(), key_data.end(), content.motherboard_cat702_key.begin());

  u32 banked_count = 0;
  u32 game_key_count = 0;
  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "bankedroms")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error) ||
          !PlaceSimpleROM(rom, member_data, std::span<u8>(content.banked_rom), error))
      {
        return std::nullopt;
      }
      banked_count++;
    }
    else if (rom.region == "cat702_2")
    {
      if (rom.size != content.game_cat702_key.size())
      {
        Error::SetStringFmt(error, "Bust a Move 2 game CAT702 key '{}' has unexpected size {}.", rom.name, rom.size);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      std::copy(member_data.begin(), member_data.end(), content.game_cat702_key.begin());
      game_key_count++;
    }
  }

  if (banked_count != 12 || game_key_count != 1)
  {
    Error::SetStringFmt(error, "Bust a Move 2 definition is incomplete: banked_roms={} cat702_2={}.",
                        banked_count, game_key_count);
    return std::nullopt;
  }

  const char* const media_role = is_cdrom ? "ata:0:cdrom_001" : "ata:0:hdd_001";
  const Arcade::Database::MediaDefinition* media = Arcade::Database::GetMediaByRole(game, media_role);
  const Arcade::Database::MediaType expected_type =
    is_cdrom ? Arcade::Database::MediaType::CHDCDROM : Arcade::Database::MediaType::CHDHardDisk;
  if (!media || media->type != expected_type)
  {
    Error::SetStringFmt(error, "Bust a Move 2 definition is missing required {} media role '{}'.",
                        is_cdrom ? "CD-ROM" : "hard-disk", media_role);
    return std::nullopt;
  }
  content.media_path = Arcade::Database::GetCompanionMediaPath(archive_path, game, *media);

  VERBOSE_LOG(
    "SonyZN BAM2 content validated set='{}' banked={} bytes banked_roms={} fixed=4MiB bank_window=4MiB "
    "bank_select=4bit media='{}' cat702_keys=2 h8='undumped, MTR990601 command HLE'",
    content.set_name, content.banked_rom.size(), banked_count, GetBAM2MediaName(content.media));
  return content;
}

std::optional<TimeWarnerZN1Content> LoadTimeWarnerZN1Content(
  const char* archive_path, const Arcade::Database::GameDefinition& game, const char* firmware_archive_path,
  const Arcade::Database::FirmwareDefinition& firmware, Error* error)
{
  if (game.id != "primrag2" || game.hardware_profile != "coh1000w")
  {
    Error::SetStringFmt(error, "Unsupported Time Warner ZN-1 set '{}' with hardware profile '{}'.", game.id,
                        game.hardware_profile);
    return std::nullopt;
  }

  TimeWarnerZN1Content content;
  content.set_name = game.id;
  content.program_rom.assign(TIME_WARNER_PROGRAM_ROM_SIZE, UINT8_C(0xff));

  const Arcade::Database::ROMDefinition* motherboard_key_rom = FindFirmwareROM(firmware, "cat702_1", {});
  if (!motherboard_key_rom || motherboard_key_rom->size != content.motherboard_cat702_key.size())
  {
    Error::SetStringView(error, "Time Warner ZN-1 firmware definition is missing the motherboard CAT702 key.");
    return std::nullopt;
  }

  std::vector<u8> key_data;
  if (!LoadROMMember(firmware_archive_path, *motherboard_key_rom, &key_data, error))
    return std::nullopt;
  std::copy(key_data.begin(), key_data.end(), content.motherboard_cat702_key.begin());

  u32 program_count = 0;
  u32 at28_count = 0;
  u32 game_key_count = 0;
  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "roms")
    {
      if (!rom.segments.empty() || rom.word_swap || rom.group_size != 1 || rom.interleave != 2 || rom.skip != 1)
      {
        Error::SetStringFmt(error, "Time Warner ZN-1 program ROM '{}' uses an unsupported load layout.", rom.name);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;

      const size_t offset = static_cast<size_t>(rom.offset);
      if (offset >= content.program_rom.size() || member_data.empty())
      {
        Error::SetStringFmt(error, "Time Warner ZN-1 program ROM '{}' exceeds the program region.", rom.name);
        return std::nullopt;
      }
      const size_t last_offset = offset + ((member_data.size() - 1) * 2);
      if (last_offset >= content.program_rom.size())
      {
        Error::SetStringFmt(error, "Time Warner ZN-1 program ROM '{}' exceeds the program region.", rom.name);
        return std::nullopt;
      }

      for (size_t i = 0; i < member_data.size(); i++)
        content.program_rom[offset + (i * 2)] = member_data[i];
      program_count++;
    }
    else if (rom.region == "at28c16")
    {
      if (rom.size != AT28_SIZE || at28_count != 0)
      {
        Error::SetStringFmt(error, "Time Warner ZN-1 AT28C16 image '{}' has unexpected size or duplicate definition.",
                            rom.name);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      content.at28_initial.assign(AT28_SIZE, UINT8_C(0xff));
      if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.at28_initial), error))
        return std::nullopt;
      at28_count++;
    }
    else if (rom.region == "cat702_2")
    {
      if (rom.size != content.game_cat702_key.size() || game_key_count != 0)
      {
        Error::SetStringFmt(error, "Time Warner ZN-1 game CAT702 key '{}' has unexpected size or duplicate definition.",
                            rom.name);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      std::copy(member_data.begin(), member_data.end(), content.game_cat702_key.begin());
      game_key_count++;
    }
  }

  if (program_count != 4 || at28_count != 1 || game_key_count != 1)
  {
    Error::SetStringFmt(error, "Time Warner ZN-1 definition is incomplete: program_roms={} at28={} cat702_2={}.",
                        program_count, at28_count, game_key_count);
    return std::nullopt;
  }

  const Arcade::Database::MediaDefinition* media = Arcade::Database::GetMediaByRole(game, "ata:0:hdd_001");
  if (!media || media->type != Arcade::Database::MediaType::CHDHardDisk)
  {
    Error::SetStringView(error, "Time Warner ZN-1 definition is missing required ATA hard-disk media.");
    return std::nullopt;
  }
  content.harddisk_path = Arcade::Database::GetCompanionMediaPath(archive_path, game, *media);

  VERBOSE_LOG("SonyZN Time Warner content validated set='{}' program={} bytes program_roms={} at28_seed={} "
           "harddisk='{}' cat702_keys=2",
           content.set_name, content.program_rom.size(), program_count, content.at28_initial.size(),
           content.harddisk_path);
  return content;
}

std::optional<AcclaimZN1Content> LoadAcclaimZN1Content(const char* archive_path,
                                                      const Arcade::Database::GameDefinition& game,
                                                      const char* firmware_archive_path,
                                                      const Arcade::Database::FirmwareDefinition& firmware,
                                                      Error* error)
{
  AcclaimZN1Content content;
  content.set_name = game.id;

  if (game.hardware_profile == "nbajamex")
  {
    content.game = AcclaimZN1Game::NBAJamExtreme;
  }
  else if (game.hardware_profile == "jdredd")
  {
    content.game = AcclaimZN1Game::JudgeDredd;
  }
  else
  {
    Error::SetStringFmt(error, "Unsupported Acclaim ZN-1 hardware profile '{}'.", game.hardware_profile);
    return std::nullopt;
  }

  const size_t banked_rom_size =
    (content.game == AcclaimZN1Game::JudgeDredd) ? ACCLAIM_JDREDD_ROM_SIZE : UINT32_C(0x2000000);
  content.banked_rom.assign(banked_rom_size, 0);
  if (content.game == AcclaimZN1Game::NBAJamExtreme)
  {
    const std::optional<size_t> rax_size = GetSimpleGameRegionSize(game, "rax");
    if (!rax_size || *rax_size != 0x800000)
    {
      Error::SetStringView(error, "NBA Jam Extreme RAX ROM region is missing or has an unexpected layout.");
      return std::nullopt;
    }
    content.rax_rom.assign(*rax_size, 0);
  }

  const Arcade::Database::ROMDefinition* motherboard_key_rom = FindFirmwareROM(firmware, "cat702_1", {});
  if (!motherboard_key_rom || motherboard_key_rom->size != content.motherboard_cat702_key.size())
  {
    Error::SetStringView(error, "Acclaim ZN-1 firmware definition is missing the motherboard CAT702 key.");
    return std::nullopt;
  }

  std::vector<u8> key_data;
  if (!LoadROMMember(firmware_archive_path, *motherboard_key_rom, &key_data, error))
    return std::nullopt;
  std::copy(key_data.begin(), key_data.end(), content.motherboard_cat702_key.begin());

  u32 banked_count = 0;
  u32 rax_count = 0;
  u32 game_key_count = 0;
  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "bankedroms")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      if (!PlaceTaitoBankedROM(rom, member_data, std::span<u8>(content.banked_rom), error))
        return std::nullopt;
      banked_count++;
    }
    else if (rom.region == "rax")
    {
      if (content.game != AcclaimZN1Game::NBAJamExtreme || content.rax_rom.empty())
      {
        Error::SetStringFmt(error, "Unexpected Acclaim RAX ROM '{}' for this hardware profile.", rom.name);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.rax_rom), error))
        return std::nullopt;
      rax_count++;
    }
    else if (rom.region == "cat702_2")
    {
      if (rom.size != content.game_cat702_key.size())
      {
        Error::SetStringFmt(error, "Acclaim ZN-1 game CAT702 key '{}' has unexpected size {}.", rom.name, rom.size);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      std::copy(member_data.begin(), member_data.end(), content.game_cat702_key.begin());
      game_key_count++;
    }
  }

  if (banked_count == 0 || game_key_count != 1 ||
      (content.game == AcclaimZN1Game::NBAJamExtreme && rax_count == 0))
  {
    Error::SetStringView(error, "Acclaim ZN-1 game definition is missing required banked, RAX, or CAT702 ROMs.");
    return std::nullopt;
  }

  if (content.game == AcclaimZN1Game::JudgeDredd)
  {
    const Arcade::Database::MediaDefinition* const harddisk = Arcade::Database::GetMediaByRole(game, "ata:0:hdd_001");
    if (!harddisk || harddisk->type != Arcade::Database::MediaType::CHDHardDisk)
    {
      Error::SetStringView(error, "Judge Dredd database definition is missing its ATA hard-disk CHD.");
      return std::nullopt;
    }
    content.harddisk_path = Arcade::Database::GetCompanionMediaPath(archive_path, game, *harddisk);
  }

  VERBOSE_LOG("SonyZN Acclaim content validated set='{}' profile='{}' banked={} bytes banked_roms={} "
           "rax={} bytes rax_roms={} cat702_keys=2 hdd='{}'",
           content.set_name, game.hardware_profile, content.banked_rom.size(), banked_count,
           content.rax_rom.size(), rax_count, content.harddisk_path);
  return content;
}

std::optional<TaitoFX1AContent> LoadTaitoFX1AContent(const char* archive_path,
                                                    const Arcade::Database::GameDefinition& game,
                                                    const char* firmware_archive_path,
                                                    const Arcade::Database::FirmwareDefinition& firmware,
                                                    Error* error)
{
  if (game.hardware_profile != "coh1000ta")
  {
    Error::SetStringFmt(error, "Unsupported Taito FX-1A hardware profile '{}'.", game.hardware_profile);
    return std::nullopt;
  }

  const Arcade::Database::ROMRegionDefinition* banked_region = FindRegion(firmware, "bankedroms");
  const std::optional<size_t> audio_cpu_size = GetSimpleGameRegionSize(game, "audiocpu");
  const std::optional<size_t> adpcma_size = GetSimpleGameRegionSize(game, "ym2610b:adpcma");
  const bool valid_adpcma_size = adpcma_size && (*adpcma_size == 0x200000 || *adpcma_size == 0x400000);
  if (!banked_region || banked_region->size != 0x1000000 || !audio_cpu_size || *audio_cpu_size != 0x20000 ||
      !valid_adpcma_size)
  {
    Error::SetStringView(
      error, "Taito FX-1A game definition has an unsupported sound ROM layout or size; expected 128 KiB Z80 "
             "program and 2 MiB or 4 MiB YM2610B sample ROM.");
    return std::nullopt;
  }

  TaitoFX1AContent content;
  content.set_name = game.id;
  content.banked_rom.assign(banked_region->size, banked_region->has_erase_value ? banked_region->erase_value : 0);
  content.audio_cpu_rom.assign(*audio_cpu_size, UINT8_C(0xff));
  content.ym2610_adpcma_rom.assign(*adpcma_size, UINT8_C(0xff));

  const Arcade::Database::ROMDefinition* motherboard_key_rom = FindFirmwareROM(firmware, "cat702_1", {});
  if (!motherboard_key_rom || motherboard_key_rom->size != content.motherboard_cat702_key.size())
  {
    Error::SetStringView(error, "Taito FX-1 firmware definition is missing the motherboard CAT702 key.");
    return std::nullopt;
  }

  std::vector<u8> key_data;
  if (!LoadROMMember(firmware_archive_path, *motherboard_key_rom, &key_data, error))
    return std::nullopt;
  std::copy(key_data.begin(), key_data.end(), content.motherboard_cat702_key.begin());

  u32 banked_count = 0;
  u32 audio_cpu_count = 0;
  u32 adpcma_count = 0;
  u32 game_key_count = 0;
  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "bankedroms")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      if (!PlaceTaitoBankedROM(rom, member_data, std::span<u8>(content.banked_rom), error))
        return std::nullopt;
      banked_count++;
    }
    else if (rom.region == "audiocpu")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.audio_cpu_rom), error))
        return std::nullopt;
      audio_cpu_count++;
    }
    else if (rom.region == "ym2610b:adpcma")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.ym2610_adpcma_rom), error))
        return std::nullopt;
      adpcma_count++;
    }
    else if (rom.region == "cat702_2")
    {
      if (rom.size != content.game_cat702_key.size())
      {
        Error::SetStringFmt(error, "Taito FX-1 game CAT702 key '{}' has unexpected size {}.", rom.name, rom.size);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      std::copy(member_data.begin(), member_data.end(), content.game_cat702_key.begin());
      game_key_count++;
    }
  }

  if (banked_count == 0 || audio_cpu_count != 1 || adpcma_count != 1 || game_key_count != 1)
  {
    Error::SetStringView(
      error, "Taito FX-1A game definition is missing required banked, Z80, YM2610B ADPCM-A, or CAT702 ROMs.");
    return std::nullopt;
  }

  VERBOSE_LOG(
    "SonyZN Taito FX-1A content validated set='{}' banked={} bytes banked_roms={} audio_cpu={} bytes "
    "adpcma={} bytes cat702_keys=2",
    content.set_name, content.banked_rom.size(), banked_count, content.audio_cpu_rom.size(),
    content.ym2610_adpcma_rom.size());
  return content;
}

std::optional<TaitoFX1BContent> LoadTaitoFX1BContent(const char* archive_path,
                                                    const Arcade::Database::GameDefinition& game,
                                                    const char* firmware_archive_path,
                                                    const Arcade::Database::FirmwareDefinition& firmware,
                                                    Error* error)
{
  if (game.hardware_profile != "coh1000tb" && game.hardware_profile != "coh1002tb")
  {
    Error::SetStringFmt(error, "Unsupported Taito FX-1B hardware profile '{}'.", game.hardware_profile);
    return std::nullopt;
  }

  const Arcade::Database::ROMRegionDefinition* banked_region = FindRegion(firmware, "bankedroms");
  const std::optional<size_t> mn10200_size = GetSimpleGameRegionSize(game, "taito_zoom:mn10200");
  const std::optional<size_t> zsg2_size = GetSimpleGameRegionSize(game, "taito_zoom:zsg2");
  const bool valid_zsg2_size = zsg2_size && (*zsg2_size == 0x400000 || *zsg2_size == 0x600000);
  if (!banked_region || banked_region->size != 0x1000000 || !mn10200_size || *mn10200_size != 0x80000 ||
      !valid_zsg2_size)
  {
    Error::SetStringView(
      error, "Taito FX-1B game definition has an unsupported ROM layout or size; expected 16 MiB banked ROM, "
             "512 KiB MN10200 program, and 4 MiB or 6 MiB ZSG-2 sample ROM.");
    return std::nullopt;
  }

  TaitoFX1BContent content;
  content.set_name = game.id;
  content.use_2mb_vram = (game.hardware_profile == "coh1002tb");
  content.banked_rom.assign(banked_region->size, UINT8_C(0x00));
  content.mn10200_rom.assign(*mn10200_size, UINT8_C(0xff));
  content.zsg2_rom.assign(*zsg2_size, UINT8_C(0xff));

  const Arcade::Database::ROMDefinition* motherboard_key_rom = FindFirmwareROM(firmware, "cat702_1", {});
  if (!motherboard_key_rom || motherboard_key_rom->size != content.motherboard_cat702_key.size())
  {
    Error::SetStringView(error, "Taito FX-1 firmware definition is missing the motherboard CAT702 key.");
    return std::nullopt;
  }

  std::vector<u8> key_data;
  if (!LoadROMMember(firmware_archive_path, *motherboard_key_rom, &key_data, error))
    return std::nullopt;
  std::copy(key_data.begin(), key_data.end(), content.motherboard_cat702_key.begin());

  u32 banked_count = 0;
  u32 mn10200_count = 0;
  u32 zsg2_count = 0;
  u32 game_key_count = 0;
  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "bankedroms")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      if (!PlaceTaitoBankedROM(rom, member_data, std::span<u8>(content.banked_rom), error))
        return std::nullopt;
      banked_count++;
    }
    else if (rom.region == "taito_zoom:mn10200")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.mn10200_rom), error))
        return std::nullopt;
      mn10200_count++;
    }
    else if (rom.region == "taito_zoom:zsg2")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.zsg2_rom), error))
        return std::nullopt;
      zsg2_count++;
    }
    else if (rom.region == "cat702_2")
    {
      if (rom.size != content.game_cat702_key.size())
      {
        Error::SetStringFmt(error, "Taito FX-1 game CAT702 key '{}' has unexpected size {}.", rom.name, rom.size);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      std::copy(member_data.begin(), member_data.end(), content.game_cat702_key.begin());
      game_key_count++;
    }
  }

  if (banked_count == 0 || mn10200_count != 1 || zsg2_count == 0 || game_key_count != 1)
  {
    Error::SetStringView(
      error, "Taito FX-1B game definition is missing required banked, MN10200, ZSG-2, or CAT702 ROMs.");
    return std::nullopt;
  }

  VERBOSE_LOG(
    "SonyZN Taito FX-1B content validated set='{}' banked={} bytes banked_roms={} mn10200={} bytes "
    "zsg2={} bytes zsg2_roms={} vram={}MiB cat702_keys=2",
    content.set_name, content.banked_rom.size(), banked_count, content.mn10200_rom.size(), content.zsg2_rom.size(),
    zsg2_count, content.use_2mb_vram ? 2 : 1);
  return content;
}

std::optional<TecmoTPSContent> LoadTecmoTPSContent(const char* archive_path,
                                                  const Arcade::Database::GameDefinition& game,
                                                  const char* firmware_archive_path,
                                                  const Arcade::Database::FirmwareDefinition& firmware,
                                                  Error* error)
{
  const bool cbaj_sound_enabled = game.hardware_profile == "cbaj";
  const bool gr2_link_enabled = game.hardware_profile == "coh1002ml";
  if (game.hardware_profile != "coh1002m" && !cbaj_sound_enabled && !gr2_link_enabled)
  {
    Error::SetStringFmt(error, "Unsupported Tecmo TPS hardware profile '{}'.", game.hardware_profile);
    return std::nullopt;
  }

  const Arcade::Database::ROMRegionDefinition* banked_region = FindRegion(firmware, "bankedroms");
  if (!banked_region || banked_region->size != TECMO_BANKED_ROM_SIZE)
  {
    Error::SetStringView(error, "Tecmo TPS firmware definition has an unexpected banked ROM region size.");
    return std::nullopt;
  }

  TecmoTPSContent content;
  content.set_name = game.id;
  content.cbaj_sound_enabled = cbaj_sound_enabled;
  content.gr2_link_enabled = gr2_link_enabled;
  content.banked_rom.assign(banked_region->size, banked_region->has_erase_value ? banked_region->erase_value : 0);
  if (cbaj_sound_enabled)
  {
    content.audio_cpu_rom.assign(TECMO_CBAJ_AUDIO_ROM_SIZE, UINT8_C(0xff));
    content.ymz280b_rom.assign(TECMO_CBAJ_YMZ_ROM_SIZE, UINT8_C(0xff));
  }
  if (gr2_link_enabled)
    content.link_cpu_rom.assign(TECMO_GR2_LINK_ROM_SIZE, UINT8_C(0xff));

  const Arcade::Database::ROMDefinition* motherboard_key_rom = FindFirmwareROM(firmware, "cat702_1", {});
  if (!motherboard_key_rom || motherboard_key_rom->size != content.motherboard_cat702_key.size())
  {
    Error::SetStringView(error, "Tecmo TPS firmware definition is missing the motherboard CAT702 key.");
    return std::nullopt;
  }

  std::vector<u8> key_data;
  if (!LoadROMMember(firmware_archive_path, *motherboard_key_rom, &key_data, error))
    return std::nullopt;
  std::copy(key_data.begin(), key_data.end(), content.motherboard_cat702_key.begin());

  u32 banked_count = 0;
  u32 at28_count = 0;
  u32 audio_cpu_count = 0;
  u32 ymz280b_count = 0;
  u32 link_cpu_count = 0;
  u32 bootleg_prot_count = 0;
  u32 game_key_count = 0;
  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "bankedroms")
    {
      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      if (!PlaceTaitoBankedROM(rom, member_data, std::span<u8>(content.banked_rom), error))
        return std::nullopt;
      banked_count++;
    }
    else if (rom.region == "audiocpu")
    {
      if (!cbaj_sound_enabled || audio_cpu_count != 0)
      {
        Error::SetStringFmt(error, "Unexpected or duplicate Tecmo TPS audio CPU ROM '{}'.", rom.name);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.audio_cpu_rom), error))
        return std::nullopt;
      audio_cpu_count++;
    }
    else if (rom.region == "ymz280b")
    {
      if (!cbaj_sound_enabled)
      {
        Error::SetStringFmt(error, "Unexpected Tecmo TPS YMZ280B ROM '{}'.", rom.name);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.ymz280b_rom), error))
        return std::nullopt;
      ymz280b_count++;
    }
    else if (rom.region == "linkcpu")
    {
      if (!gr2_link_enabled || link_cpu_count != 0)
      {
        Error::SetStringFmt(error, "Unexpected or duplicate Tecmo TPS link CPU ROM '{}'.", rom.name);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.link_cpu_rom), error))
        return std::nullopt;
      link_cpu_count++;
    }
    else if (rom.region == "blprot")
    {
      // The cbajbl dump includes a 128 KiB ROM associated with the bootleg
      // protection/logic device. Current reference drivers do not map it into
      // the PSX/Z80 address spaces. Validate that the dumped device image is
      // present, but do not invent an unverified hardware mapping.
      if (game.id != "cbajbl" || bootleg_prot_count != 0 || rom.size != 0x20000)
      {
        Error::SetStringFmt(error, "Unexpected Tecmo TPS bootleg protection ROM '{}'.", rom.name);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      bootleg_prot_count++;
    }
    else if (rom.region == "at28c16")
    {
      if (rom.size != AT28_SIZE || at28_count != 0)
      {
        Error::SetStringFmt(
          error, "Tecmo TPS AT28C16 factory image '{}' has unexpected size or duplicate definition.", rom.name);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;

      content.at28_initial.assign(AT28_SIZE, UINT8_C(0xff));
      if (!PlaceSimpleROM(rom, member_data, std::span<u8>(content.at28_initial), error))
        return std::nullopt;
      at28_count++;
    }
    else if (rom.region == "cat702_2")
    {
      if (rom.size != content.game_cat702_key.size())
      {
        Error::SetStringFmt(error, "Tecmo TPS game CAT702 key '{}' has unexpected size {}.", rom.name, rom.size);
        return std::nullopt;
      }

      std::vector<u8> member_data;
      if (!LoadROMMember(archive_path, rom, &member_data, error))
        return std::nullopt;
      std::copy(member_data.begin(), member_data.end(), content.game_cat702_key.begin());
      game_key_count++;
    }
  }

  const u32 expected_ymz280b_count = (game.id == "cbajbl") ? 3u : 2u;
  if (banked_count == 0 || game_key_count != 1 ||
      (cbaj_sound_enabled && (audio_cpu_count != 1 || ymz280b_count != expected_ymz280b_count)) ||
      (gr2_link_enabled && link_cpu_count != 1) ||
      (game.id == "cbajbl" && bootleg_prot_count != 1))
  {
    Error::SetStringView(
      error,
      "Tecmo TPS game definition is missing required banked, CBAJ sound-board, link-board, bootleg-device, or CAT702 ROMs.");
    return std::nullopt;
  }

  VERBOSE_LOG(
    "SonyZN Tecmo TPS content validated set='{}' banked={} bytes banked_roms={} at28_seed={} bytes "
    "cbaj_sound={} audiocpu={} bytes ymz280b={} bytes ymz_roms={} gr2_link={} linkcpu={} bytes "
    "bootleg_prot={} cat702_keys=2",
    content.set_name, content.banked_rom.size(), banked_count, content.at28_initial.size(), cbaj_sound_enabled,
    content.audio_cpu_rom.size(), content.ymz280b_rom.size(), ymz280b_count, gr2_link_enabled,
    content.link_cpu_rom.size(), bootleg_prot_count);
  return content;
}

bool InitializeCapcomZN1(const BIOS::Image& bios, CapcomZN1Content content, std::string_view persistence_directory,
                         Error* error)
{
  if (bios.data.size() != BIOS::BIOS_SIZE || content.country_rom.size() != 0x80000 ||
      content.banked_rom.size() != 0x2400000 || content.audio_cpu_rom.size() != 0x40000 ||
      content.qsound_rom.size() != 0x400000)
  {
    Error::SetStringView(error, "Invalid Capcom ZN-1 validated content.");
    return false;
  }

  if (!s_mcu_dsr_assert_event)
  {
    s_mcu_dsr_assert_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Assert", 1, 1, ZNMCUDSRAssertEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_assert_event->Deactivate();
  }

  if (!s_mcu_dsr_release_event)
  {
    s_mcu_dsr_release_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Release", 1, 1, ZNMCUDSRReleaseEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_release_event->Deactivate();
  }


  RuntimeState runtime;
  runtime.board_type = BoardType::CapcomZN1;
  runtime.content = std::move(content);
  runtime.motherboard_cat702.Initialize(runtime.content.motherboard_cat702_key);
  runtime.game_cat702.Initialize(runtime.content.game_cat702_key);
  ApplySecuritySelect(runtime, 0x0c);

  if (!LoadAT28(runtime, persistence_directory, error))
    return false;

  if (runtime.content.qsound_enabled)
  {
    if (!CapcomQSound::Initialize(runtime.content.audio_cpu_rom, runtime.content.qsound_rom, error))
      return false;
  }
  else
  {
    // Gallop Racer's Z80/QSound ROM sockets are physically unpopulated.
    // MAME holds the audio CPU in reset; an inactive QSound backend is equivalent here.
    CapcomQSound::Shutdown();
  }

  std::memcpy(Bus::g_bios, bios.data.data(), BIOS::BIOS_SIZE);

  s_runtime = std::move(runtime);

  VERBOSE_LOG(
    "SonyZN initialized set='{}' board='{}' ram=4MiB vram={}MiB "
    "exp1='0x1F000000-0x1F7FFFFF' bank='0x1FB00000' country='0x1FB80000-0x1FBFFFFF' "
    "security_select='0x1FA10300' sio0='0x1F801040 baud-timed CAT702/ZNMCU transport' "
    "qsound='{}' at28='2KiB persistent, 200us data-poll'",
    s_runtime->content.set_name, s_runtime->content.use_2mb_vram ? "COH-1002C" : "COH-1000C",
    s_runtime->content.use_2mb_vram ? 2 : 1,
    s_runtime->content.qsound_enabled ? "Z80 8MHz + DL-1425 HLE" : "disabled (ROM sockets unpopulated)");
  return true;
}

bool InitializeVideoSystemZN1(const BIOS::Image& bios, VideoSystemZN1Content content,
                              std::string_view persistence_directory, Error* error)
{
  if (bios.data.size() != BIOS::BIOS_SIZE || content.fixed_rom.size() != VIDEO_SYSTEM_FIXED_ROM_SIZE ||
      content.banked_rom.size() != VIDEO_SYSTEM_BANKED_ROM_SIZE)
  {
    Error::SetStringView(error, "Invalid Video System ZN-1 validated content.");
    return false;
  }

  if (!s_mcu_dsr_assert_event)
  {
    s_mcu_dsr_assert_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Assert", 1, 1, ZNMCUDSRAssertEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_assert_event->Deactivate();
  }

  if (!s_mcu_dsr_release_event)
  {
    s_mcu_dsr_release_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Release", 1, 1, ZNMCUDSRReleaseEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_release_event->Deactivate();
  }

  CapcomQSound::Shutdown();
  TaitoFX1ASound::Shutdown();
  TaitoFX1BZoom::Shutdown();
  AcclaimATA::Shutdown();
  AcclaimRAX::Shutdown();

  RuntimeState runtime;
  runtime.board_type = BoardType::VideoSystemZN1;
  runtime.content.set_name = std::move(content.set_name);
  runtime.content.use_2mb_vram = true;
  runtime.content.qsound_enabled = false;
  runtime.video_system_fixed_rom = std::move(content.fixed_rom);
  runtime.content.banked_rom = std::move(content.banked_rom);
  runtime.content.motherboard_cat702_key = content.motherboard_cat702_key;
  runtime.content.game_cat702_key = content.game_cat702_key;
  runtime.motherboard_cat702.Initialize(runtime.content.motherboard_cat702_key);
  runtime.game_cat702.Initialize(runtime.content.game_cat702_key);
  ApplySecuritySelect(runtime, 0x0c);

  if (!LoadAT28(runtime, persistence_directory, error))
    return false;

  std::memcpy(Bus::g_bios, bios.data.data(), BIOS::BIOS_SIZE);
  s_runtime = std::move(runtime);

  VERBOSE_LOG(
    "SonyZN initialized set='{}' board='COH-1002V' ram=4MiB vram=2MiB "
    "fixed='0x1F000000-0x1F27FFFF' banked='0x1FB00000-0x1FBFFFFF 1MiB window' "
    "bank='0x1FB00000 low byte, 24 entries' "
    "security_select='0x1FA10300' sio0='0x1F801040 baud-timed CAT702/ZNMCU transport' "
    "at28='2KiB persistent, 200us data-poll' "
    "dsw='S551:2 bios_service={} S551:3 test={} S551:4 save={}'",
    s_runtime->content.set_name,
    g_settings.arcade_video_system_zn1_bios_service_mode ? "on" : "off",
    g_settings.arcade_video_system_zn1_test_mode ? "on" : "off",
    g_settings.arcade_video_system_zn1_save ? "yes" : "no");
  return true;
}

bool InitializeAtlusZN1(const BIOS::Image& bios, AtlusZN1Content content,
                         std::string_view persistence_directory, Error* error)
{
  if (bios.data.size() != BIOS::BIOS_SIZE || content.banked_rom.size() != ATLUS_BANKED_ROM_SIZE ||
      content.audio_cpu_rom.size() != ATLUS_AUDIO_ROM_SIZE || content.ymz280b_rom.size() != ATLUS_YMZ280B_ROM_SIZE)
  {
    Error::SetStringView(error, "Invalid Atlus ZN-1 validated content.");
    return false;
  }

  if (!s_mcu_dsr_assert_event)
  {
    s_mcu_dsr_assert_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Assert", 1, 1, ZNMCUDSRAssertEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_assert_event->Deactivate();
  }

  if (!s_mcu_dsr_release_event)
  {
    s_mcu_dsr_release_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Release", 1, 1, ZNMCUDSRReleaseEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_release_event->Deactivate();
  }

  AtlusZN1Sound::Shutdown();
  EightingRaizingSound::Shutdown();
  CapcomQSound::Shutdown();
  TaitoFX1ASound::Shutdown();
  TaitoFX1BZoom::Shutdown();
  TecmoCBAJSound::Shutdown();
  TecmoGR2Link::Shutdown();
  AcclaimATA::Shutdown();
  AcclaimRAX::Shutdown();

  RuntimeState runtime;
  runtime.board_type = BoardType::AtlusZN1;
  runtime.content.set_name = std::move(content.set_name);
  runtime.content.use_2mb_vram = true;
  runtime.content.qsound_enabled = false;
  runtime.content.banked_rom = std::move(content.banked_rom);
  runtime.atlus_audio_cpu_rom = std::move(content.audio_cpu_rom);
  runtime.atlus_ymz280b_rom = std::move(content.ymz280b_rom);
  runtime.content.motherboard_cat702_key = content.motherboard_cat702_key;
  runtime.content.game_cat702_key = content.game_cat702_key;
  runtime.motherboard_cat702.Initialize(runtime.content.motherboard_cat702_key);
  runtime.game_cat702.Initialize(runtime.content.game_cat702_key);
  ApplySecuritySelect(runtime, 0x0c);

  if (!LoadAT28(runtime, persistence_directory, error))
    return false;

  if (!AtlusZN1Sound::Initialize(std::move(runtime.atlus_audio_cpu_rom), std::move(runtime.atlus_ymz280b_rom), error))
    return false;

  std::memcpy(Bus::g_bios, bios.data.data(), BIOS::BIOS_SIZE);
  s_runtime = std::move(runtime);

  VERBOSE_LOG(
    "SonyZN initialized set='{}' board='COH-1001L' ram=4MiB vram=2MiB "
    "exp1='0x1F000000-0x1F7FFFFF 8MiB bank window' bank='0x1FB00002 low byte bits1:0' "
    "sound='ATHG-01 MC68000 10MHz + YMZ280B 16.9344MHz' "
    "security_select='0x1FA10300' sio0='0x1F801040 baud-timed CAT702/ZNMCU transport' "
    "at28='2KiB persistent, 200us data-poll'",
    s_runtime->content.set_name);
  return true;
}

bool InitializeEightingRaizingZN1(const BIOS::Image& bios, EightingRaizingZN1Content content,
                                  std::string_view persistence_directory, Error* error)
{
  if (bios.data.size() != BIOS::BIOS_SIZE || content.banked_rom.size() != EIGHTING_BANKED_ROM_SIZE ||
      content.audio_cpu_rom.size() < EIGHTING_AUDIO_ROM_SIZE ||
      content.audio_cpu_rom.size() > EIGHTING_AUDIO_ROM_MAX_SIZE ||
      content.ymf271_rom.size() != EIGHTING_YMF271_ROM_SIZE)
  {
    Error::SetStringView(error, "Invalid Eighting/Raizing ZN-1 validated content.");
    return false;
  }

  if (!s_mcu_dsr_assert_event)
  {
    s_mcu_dsr_assert_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Assert", 1, 1, ZNMCUDSRAssertEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_assert_event->Deactivate();
  }

  if (!s_mcu_dsr_release_event)
  {
    s_mcu_dsr_release_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Release", 1, 1, ZNMCUDSRReleaseEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_release_event->Deactivate();
  }

  AtlusZN1Sound::Shutdown();
  EightingRaizingSound::Shutdown();
  CapcomQSound::Shutdown();
  TaitoFX1ASound::Shutdown();
  TaitoFX1BZoom::Shutdown();
  TecmoCBAJSound::Shutdown();
  AcclaimATA::Shutdown();
  AcclaimRAX::Shutdown();

  const bool uses_tecmo_motherboard = content.uses_tecmo_motherboard;

  RuntimeState runtime;
  runtime.board_type = BoardType::EightingRaizingZN1;
  runtime.content.set_name = std::move(content.set_name);
  runtime.content.use_2mb_vram = true;
  runtime.content.qsound_enabled = false;
  runtime.content.banked_rom = std::move(content.banked_rom);
  runtime.content.motherboard_cat702_key = content.motherboard_cat702_key;
  runtime.content.game_cat702_key = content.game_cat702_key;
  runtime.eighting_ps9805_flash = content.has_ps9805_flash;
  runtime.motherboard_cat702.Initialize(runtime.content.motherboard_cat702_key);
  runtime.game_cat702.Initialize(runtime.content.game_cat702_key);
  ApplySecuritySelect(runtime, 0x0c);

  if (!LoadAT28(runtime, persistence_directory, error, std::span<const u8>(content.at28_initial)))
    return false;

  if (!EightingRaizingSound::Initialize(std::move(content.audio_cpu_rom), std::move(content.ymf271_rom), error))
    return false;

  std::memcpy(Bus::g_bios, bios.data.data(), BIOS::BIOS_SIZE);
  s_runtime = std::move(runtime);

  VERBOSE_LOG(
    "SonyZN initialized set='{}' board='{}' ram=4MiB vram=2MiB "
    "exp1='0x1F000000-0x1F7FFFFF 8MiB bank window' bank='0x1FA10300 bits1:0' "
    "sound='MC68000 12MHz + YMF271 16.9344MHz PCM + polled timers' "
    "security_select='0x1FA10300 shared bank/CAT702 select' "
    "sio0='0x1F801040 baud-timed CAT702/ZNMCU transport' at28='2KiB persistent, 200us data-poll'",
    s_runtime->content.set_name,
    uses_tecmo_motherboard ? "COH-1002M/MG01 + Eighting-Raizing PS9805" : "COH-1002E / Eighting-Raizing");
  return true;
}


bool InitializeBustAMove2ZN1(const BIOS::Image& bios, BustAMove2ZN1Content content,
                            std::string_view persistence_directory, Error* error)
{
  if (bios.data.size() != BIOS::BIOS_SIZE || content.banked_rom.size() != BAM2_BANKED_ROM_SIZE)
  {
    Error::SetStringView(error, "Invalid Bust a Move 2 validated content.");
    return false;
  }

  if (!s_mcu_dsr_assert_event)
  {
    s_mcu_dsr_assert_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Assert", 1, 1, ZNMCUDSRAssertEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_assert_event->Deactivate();
  }

  if (!s_mcu_dsr_release_event)
  {
    s_mcu_dsr_release_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Release", 1, 1, ZNMCUDSRReleaseEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_release_event->Deactivate();
  }

  AtlusZN1Sound::Shutdown();
  EightingRaizingSound::Shutdown();
  CapcomQSound::Shutdown();
  TaitoFX1ASound::Shutdown();
  TaitoFX1BZoom::Shutdown();
  TecmoCBAJSound::Shutdown();
  AcclaimATA::Shutdown();
  AcclaimRAX::Shutdown();

  RuntimeState runtime;
  runtime.board_type = BoardType::BustAMove2ZN1;
  runtime.content.set_name = std::move(content.set_name);
  runtime.content.use_2mb_vram = true;
  runtime.content.qsound_enabled = false;
  runtime.content.banked_rom = std::move(content.banked_rom);
  runtime.content.motherboard_cat702_key = content.motherboard_cat702_key;
  runtime.content.game_cat702_key = content.game_cat702_key;
  runtime.bam2_media = content.media;
  runtime.bank = 1;
  runtime.bam2_mcu_ports.fill(0);
  runtime.bam2_mcu_ports[0] = 1;
  runtime.motherboard_cat702.Initialize(runtime.content.motherboard_cat702_key);
  runtime.game_cat702.Initialize(runtime.content.game_cat702_key);
  ApplySecuritySelect(runtime, 0x0c);

  // The common COH-1002E motherboard still carries its AT28C16 even though
  // BAM2 has no dumped factory seed in the current database.
  if (!LoadAT28(runtime, persistence_directory, error))
    return false;

  if (runtime.bam2_media == BustAMove2Media::HardDisk)
  {
    if (!LoadBAM2HDDPCM(runtime, content.media_path, error))
      return false;
  }
  else if (!LoadBAM2CDROMPCM(runtime, content.media_path, error))
  {
    return false;
  }

  std::memcpy(Bus::g_bios, bios.data.data(), BIOS::BIOS_SIZE);
  s_runtime = std::move(runtime);

  VERBOSE_LOG(
    "SonyZN initialized set='{}' board='COH-1002E + MTR990601-(A)' ram=4MiB vram=2MiB "
    "exp1='0x1F000000-0x1F3FFFFF fixed + 0x1F400000-0x1F7FFFFF 4MiB bank window' "
    "bank='H8 @ 0x1FB00000 bits3:0, reset=1' "
    "h8='0x1FB00002 command/argument + 0x1FB00004 status/control, firmware undumped' "
    "media='{} mounted' sound='H8-buffered media PCM -> TC9293 attenuation' security_select='0x1FA10300 CAT702 only' "
    "sio0='0x1F801040 baud-timed CAT702/ZNMCU transport'",
    s_runtime->content.set_name, GetBAM2MediaName(s_runtime->bam2_media));
  return true;
}

bool InitializeTimeWarnerZN1(const BIOS::Image& bios, TimeWarnerZN1Content content,
                             std::string_view persistence_directory, Error* error)
{
  if (bios.data.size() != BIOS::BIOS_SIZE || content.program_rom.size() != TIME_WARNER_PROGRAM_ROM_SIZE ||
      content.at28_initial.size() != AT28_SIZE || content.harddisk_path.empty())
  {
    Error::SetStringView(error, "Invalid Time Warner ZN-1 validated content.");
    return false;
  }

  if (!s_mcu_dsr_assert_event)
  {
    s_mcu_dsr_assert_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Assert", 1, 1, ZNMCUDSRAssertEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_assert_event->Deactivate();
  }

  if (!s_mcu_dsr_release_event)
  {
    s_mcu_dsr_release_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Release", 1, 1, ZNMCUDSRReleaseEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_release_event->Deactivate();
  }

  if (!s_time_warner_watchdog_event)
  {
    s_time_warner_watchdog_event = std::make_unique<TimingEvent>(
      "Sony ZN Time Warner DS1232 Watchdog", 1, 1, TimeWarnerWatchdogEventCallback, nullptr);
  }
  else
  {
    s_time_warner_watchdog_event->Deactivate();
  }

  TaitoFX1ASound::Shutdown();
  TaitoFX1BZoom::Shutdown();
  CapcomQSound::Shutdown();
  AcclaimRAX::Shutdown();
  AcclaimATA::Shutdown();
  TimeWarnerATA::Shutdown();

  RuntimeState runtime;
  runtime.board_type = BoardType::TimeWarnerZN1;
  runtime.content.set_name = std::move(content.set_name);
  runtime.content.use_2mb_vram = true;
  runtime.content.qsound_enabled = false;
  runtime.content.banked_rom = std::move(content.program_rom);
  runtime.content.motherboard_cat702_key = content.motherboard_cat702_key;
  runtime.content.game_cat702_key = content.game_cat702_key;
  runtime.motherboard_cat702.Initialize(runtime.content.motherboard_cat702_key);
  runtime.game_cat702.Initialize(runtime.content.game_cat702_key);
  ApplySecuritySelect(runtime, 0x0c);

  if (!LoadAT28(runtime, persistence_directory, error, std::span<const u8>(content.at28_initial)))
    return false;
  if (!TimeWarnerATA::Initialize(content.harddisk_path, error))
    return false;

  std::memcpy(Bus::g_bios, bios.data.data(), BIOS::BIOS_SIZE);
  s_runtime = std::move(runtime);
  if (s_time_warner_watchdog_event)
    s_time_warner_watchdog_event->Deactivate();

  VERBOSE_LOG(
    "SonyZN initialized set='{}' board='COH-1000W + Atari PSXTRA' ram=8MiB vram=2MiB "
    "exp1_rom='0x1F000000-0x1F1FFFFF' via='0x1F7E4000' irqctrl='0x1F7E8000' "
    "data32='0x1F7F4000' dma='PIO/channel5' irq='IRQ10' at28='2KiB persistent, factory seed'",
    s_runtime->content.set_name);
  return true;
}

bool InitializeAcclaimZN1(const BIOS::Image& bios, AcclaimZN1Content content,
                          std::string_view persistence_directory, Error* error)
{
  const size_t expected_banked_rom_size =
    (content.game == AcclaimZN1Game::JudgeDredd) ? ACCLAIM_JDREDD_ROM_SIZE : UINT32_C(0x2000000);
  if (bios.data.size() != BIOS::BIOS_SIZE || content.banked_rom.size() != expected_banked_rom_size ||
      (content.game == AcclaimZN1Game::NBAJamExtreme && content.rax_rom.size() != 0x800000) ||
      (content.game == AcclaimZN1Game::JudgeDredd && !content.rax_rom.empty()))
  {
    Error::SetStringView(error, "Invalid Acclaim ZN-1 validated content.");
    return false;
  }

  if (!s_mcu_dsr_assert_event)
  {
    s_mcu_dsr_assert_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Assert", 1, 1, ZNMCUDSRAssertEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_assert_event->Deactivate();
  }

  if (!s_mcu_dsr_release_event)
  {
    s_mcu_dsr_release_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Release", 1, 1, ZNMCUDSRReleaseEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_release_event->Deactivate();
  }

  TaitoFX1ASound::Shutdown();
  TaitoFX1BZoom::Shutdown();
  CapcomQSound::Shutdown();
  AcclaimRAX::Shutdown();
  TimeWarnerATA::Shutdown();

  RuntimeState runtime;
  runtime.board_type = BoardType::AcclaimZN1;
  runtime.acclaim_game = content.game;
  runtime.content.set_name = std::move(content.set_name);
  runtime.content.use_2mb_vram = true;
  runtime.content.qsound_enabled = false;
  runtime.content.banked_rom = std::move(content.banked_rom);
  runtime.acclaim_rax_rom = std::move(content.rax_rom);
  runtime.content.motherboard_cat702_key = content.motherboard_cat702_key;
  runtime.content.game_cat702_key = content.game_cat702_key;
  runtime.motherboard_cat702.Initialize(runtime.content.motherboard_cat702_key);
  runtime.game_cat702.Initialize(runtime.content.game_cat702_key);
  ApplySecuritySelect(runtime, 0x0c);

  if (!LoadAT28(runtime, persistence_directory, error))
    return false;
  if (content.game == AcclaimZN1Game::NBAJamExtreme && !LoadAcclaimNBASRAM(runtime, error))
    return false;

  if (content.game == AcclaimZN1Game::JudgeDredd)
  {
    if (!AcclaimATA::Initialize(content.harddisk_path, error))
      return false;
  }
  else
  {
    AcclaimATA::Shutdown();
  }

  std::memcpy(Bus::g_bios, bios.data.data(), BIOS::BIOS_SIZE);
  s_runtime = std::move(runtime);

  if (s_runtime->acclaim_game == AcclaimZN1Game::NBAJamExtreme &&
      !AcclaimRAX::Initialize(s_runtime->acclaim_rax_rom, error))
  {
    s_runtime.reset();
    return false;
  }

  VERBOSE_LOG(
    "SonyZN initialized set='{}' board='COH-1000A' ram=4MiB vram=2MiB "
    "security_select='0x1FA10300' sio0='0x1F801040 baud-timed CAT702/ZNMCU transport' "
    "at28='2KiB persistent, 200us data-poll' acclaim_profile='{}' nba_sram={} rax_rom={} bytes",
    s_runtime->content.set_name,
    s_runtime->acclaim_game == AcclaimZN1Game::JudgeDredd ? "jdredd" : "nbajamex",
    s_runtime->acclaim_game == AcclaimZN1Game::NBAJamExtreme ? "71256 32KiB persistent" : "n/a",
    s_runtime->acclaim_rax_rom.size());
  return true;
}

bool InitializeTaitoFX1A(const BIOS::Image& bios, TaitoFX1AContent content, std::string_view persistence_directory,
                         Error* error)
{
  const bool valid_adpcma_size =
    content.ym2610_adpcma_rom.size() == 0x200000 || content.ym2610_adpcma_rom.size() == 0x400000;
  if (bios.data.size() != BIOS::BIOS_SIZE || content.banked_rom.size() != 0x1000000 ||
      content.audio_cpu_rom.size() != 0x20000 || !valid_adpcma_size)
  {
    Error::SetStringView(error, "Invalid Taito FX-1A validated content.");
    return false;
  }

  if (!s_mcu_dsr_assert_event)
  {
    s_mcu_dsr_assert_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Assert", 1, 1, ZNMCUDSRAssertEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_assert_event->Deactivate();
  }

  if (!s_mcu_dsr_release_event)
  {
    s_mcu_dsr_release_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Release", 1, 1, ZNMCUDSRReleaseEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_release_event->Deactivate();
  }

  if (!s_taito_watchdog_event)
  {
    s_taito_watchdog_event =
      std::make_unique<TimingEvent>("Sony ZN Taito MB3773 Watchdog", 1, 1, TaitoWatchdogEventCallback, nullptr);
  }
  else
  {
    s_taito_watchdog_event->Deactivate();
  }

  if (!TaitoFX1ASound::Initialize(content.audio_cpu_rom, content.ym2610_adpcma_rom, error))
    return false;

  RuntimeState runtime;
  runtime.board_type = BoardType::TaitoFX1A;
  runtime.content.set_name = std::move(content.set_name);
  runtime.content.use_2mb_vram = false;
  runtime.content.qsound_enabled = false;
  runtime.content.banked_rom = std::move(content.banked_rom);
  runtime.content.motherboard_cat702_key = content.motherboard_cat702_key;
  runtime.content.game_cat702_key = content.game_cat702_key;
  runtime.motherboard_cat702.Initialize(runtime.content.motherboard_cat702_key);
  runtime.game_cat702.Initialize(runtime.content.game_cat702_key);
  ApplySecuritySelect(runtime, 0x0c);

  if (!LoadAT28(runtime, persistence_directory, error))
  {
    TaitoFX1ASound::Shutdown();
    return false;
  }

  CapcomQSound::Shutdown();
  std::memcpy(Bus::g_bios, bios.data.data(), BIOS::BIOS_SIZE);
  s_runtime = std::move(runtime);

  VERBOSE_LOG(
    "SonyZN initialized set='{}' board='COH-1000TA' ram=4MiB vram=1MiB "
    "exp1='0x1F000000-0x1F7FFFFF 8MiB bank window' bank='0x1FB40000 bits1:0' "
    "security_select='0x1FA10300' sio0='0x1F801040 baud-timed CAT702/ZNMCU transport' "
    "fx1a_sound='TC0140SYT + Z80 4MHz + YM2610B 8MHz (YMFM, level IRQ)' "
    "at28='2KiB persistent, 200us data-poll'",
    s_runtime->content.set_name);
  return true;
}

bool InitializeTaitoFX1B(const BIOS::Image& bios, TaitoFX1BContent content, std::string_view persistence_directory,
                         Error* error)
{
  const bool valid_zsg2_size = content.zsg2_rom.size() == 0x400000 || content.zsg2_rom.size() == 0x600000;
  if (bios.data.size() != BIOS::BIOS_SIZE || content.banked_rom.size() != 0x1000000 ||
      content.mn10200_rom.size() != 0x80000 || !valid_zsg2_size)
  {
    Error::SetStringView(error, "Invalid Taito FX-1B validated content.");
    return false;
  }

  if (!s_mcu_dsr_assert_event)
  {
    s_mcu_dsr_assert_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Assert", 1, 1, ZNMCUDSRAssertEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_assert_event->Deactivate();
  }

  if (!s_mcu_dsr_release_event)
  {
    s_mcu_dsr_release_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Release", 1, 1, ZNMCUDSRReleaseEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_release_event->Deactivate();
  }

  if (!s_taito_watchdog_event)
  {
    s_taito_watchdog_event =
      std::make_unique<TimingEvent>("Sony ZN Taito MB3773 Watchdog", 1, 1, TaitoWatchdogEventCallback, nullptr);
  }
  else
  {
    s_taito_watchdog_event->Deactivate();
  }

  TaitoFX1ASound::Shutdown();
  CapcomQSound::Shutdown();
  TaitoFX1BZoom::Shutdown();

  RuntimeState runtime;
  runtime.board_type = BoardType::TaitoFX1B;
  runtime.content.set_name = std::move(content.set_name);
  runtime.content.use_2mb_vram = content.use_2mb_vram;
  runtime.content.qsound_enabled = false;
  runtime.content.banked_rom = std::move(content.banked_rom);
  runtime.content.motherboard_cat702_key = content.motherboard_cat702_key;
  runtime.content.game_cat702_key = content.game_cat702_key;
  runtime.taito_fx1b_mn10200_rom = std::move(content.mn10200_rom);
  runtime.taito_fx1b_zsg2_rom = std::move(content.zsg2_rom);
  runtime.taito_zoom_shared_ram.fill(0);
  runtime.taito_zoom_reg_address = 0;
  runtime.taito_zoom_gain_left = 0x3f;
  runtime.taito_zoom_gain_right = 0x3f;
  runtime.motherboard_cat702.Initialize(runtime.content.motherboard_cat702_key);
  runtime.game_cat702.Initialize(runtime.content.game_cat702_key);
  ApplySecuritySelect(runtime, 0x0c);

  if (!LoadAT28(runtime, persistence_directory, error))
    return false;
  if (!LoadTaitoFX1BFRAM(runtime, error))
    return false;

  std::memcpy(Bus::g_bios, bios.data.data(), BIOS::BIOS_SIZE);
  s_runtime = std::move(runtime);

  if (!TaitoFX1BZoom::Initialize(
        std::span<const u8>(s_runtime->taito_fx1b_mn10200_rom.data(), s_runtime->taito_fx1b_mn10200_rom.size()),
        std::span<const u8>(s_runtime->taito_fx1b_zsg2_rom.data(), s_runtime->taito_fx1b_zsg2_rom.size()),
        std::span<u8>(s_runtime->taito_zoom_shared_ram.data(), s_runtime->taito_zoom_shared_ram.size())))
  {
    Error::SetStringView(error, "Failed to initialize Taito FX-1B ZOOM sound board.");
    s_runtime.reset();
    return false;
  }

  VERBOSE_LOG(
    "SonyZN initialized set='{}' board='{}' ram=4MiB vram={}MiB "
    "exp1='0x1F000000-0x1F7FFFFF 8MiB bank window' bank='0x1FB40000 bits1:0' "
    "fram='FM1208S 512B persistent' zoom_host='M66220 shared RAM + IRQ/status + gain registers' "
    "zoom_sound='MN1020012A 12.5MHz + ZSG-2 25MHz + TMS57002 12.5MHz' "
    "security_select='0x1FA10300' sio0='0x1F801040 baud-timed CAT702/ZNMCU transport'",
    s_runtime->content.set_name, s_runtime->content.use_2mb_vram ? "COH-1002TB" : "COH-1000TB",
    s_runtime->content.use_2mb_vram ? 2 : 1);
  return true;
}


bool InitializeTecmoTPS(const BIOS::Image& bios, TecmoTPSContent content, std::string_view persistence_directory,
                        Error* error)
{
  if (bios.data.size() != BIOS::BIOS_SIZE || content.banked_rom.size() != TECMO_BANKED_ROM_SIZE ||
      (content.cbaj_sound_enabled &&
       (content.audio_cpu_rom.size() != TECMO_CBAJ_AUDIO_ROM_SIZE ||
        content.ymz280b_rom.size() != TECMO_CBAJ_YMZ_ROM_SIZE)) ||
      (content.gr2_link_enabled && content.link_cpu_rom.size() != TECMO_GR2_LINK_ROM_SIZE))
  {
    Error::SetStringView(error, "Invalid Tecmo TPS validated content.");
    return false;
  }

  if (!s_mcu_dsr_assert_event)
  {
    s_mcu_dsr_assert_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Assert", 1, 1, ZNMCUDSRAssertEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_assert_event->Deactivate();
  }

  if (!s_mcu_dsr_release_event)
  {
    s_mcu_dsr_release_event =
      std::make_unique<TimingEvent>("Sony ZN MCU DSR Release", 1, 1, ZNMCUDSRReleaseEventCallback, nullptr);
  }
  else
  {
    s_mcu_dsr_release_event->Deactivate();
  }

  CapcomQSound::Shutdown();
  TaitoFX1ASound::Shutdown();
  TaitoFX1BZoom::Shutdown();
  TecmoCBAJSound::Shutdown();
  TecmoGR2Link::Shutdown();
  AcclaimATA::Shutdown();
  AcclaimRAX::Shutdown();

  RuntimeState runtime;
  runtime.board_type = BoardType::TecmoTPS;
  runtime.tecmo_cbaj_sound_enabled = content.cbaj_sound_enabled;
  runtime.tecmo_gr2_link_enabled = content.gr2_link_enabled;
  runtime.content.set_name = std::move(content.set_name);
  runtime.content.use_2mb_vram = true;
  runtime.content.qsound_enabled = false;
  runtime.content.banked_rom = std::move(content.banked_rom);
  runtime.content.motherboard_cat702_key = content.motherboard_cat702_key;
  runtime.content.game_cat702_key = content.game_cat702_key;
  runtime.motherboard_cat702.Initialize(runtime.content.motherboard_cat702_key);
  runtime.game_cat702.Initialize(runtime.content.game_cat702_key);
  ApplySecuritySelect(runtime, 0x0c);

  if (!LoadAT28(runtime, persistence_directory, error,
                std::span<const u8>(content.at28_initial.data(), content.at28_initial.size())))
  {
    return false;
  }

  if (runtime.tecmo_cbaj_sound_enabled &&
      !TecmoCBAJSound::Initialize(content.audio_cpu_rom, content.ymz280b_rom, error))
  {
    return false;
  }
  if (runtime.tecmo_gr2_link_enabled && !TecmoGR2Link::Initialize(content.link_cpu_rom, error))
    return false;

  std::memcpy(Bus::g_bios, bios.data.data(), BIOS::BIOS_SIZE);
  s_runtime = std::move(runtime);

  VERBOSE_LOG(
    "SonyZN initialized set='{}' board='{}' ram=4MiB vram=2MiB "
    "exp1='0x1F000000-0x1F7FFFFF 8MiB bank window' bank='0x1FB00006 low byte' "
    "security_select='0x1FA10300' sio0='0x1F801040 baud-timed CAT702/ZNMCU transport' "
    "at28='2KiB persistent, 200us data-poll' cbaj_sound='{}' gr2_link='{}'",
    s_runtime->content.set_name, s_runtime->tecmo_gr2_link_enabled ? "COH-1002ML" : "COH-1002M",
    s_runtime->tecmo_cbaj_sound_enabled ? "Z80 4MHz + dual FIFO + YMZ280B 16.9344MHz" : "not fitted",
    s_runtime->tecmo_gr2_link_enabled ? "Z80 4MHz + host FIFO + uPD72103A" : "not fitted");
  return true;
}

void Reset()
{
  if (!s_runtime)
    return;

  s_runtime->bank = (s_runtime->board_type == BoardType::BustAMove2ZN1) ? UINT8_C(1) : UINT8_C(0);

  s_runtime->acclaim_bank_registers.fill(0);
  s_runtime->acclaim_gun_mux = 0;
  s_runtime->acclaim_rax_host_latch = UINT16_C(0xffff);
  if (s_runtime->board_type == BoardType::AcclaimZN1)
  {
    if (s_runtime->acclaim_game == AcclaimZN1Game::JudgeDredd)
      AcclaimATA::Reset();
    else if (s_runtime->acclaim_game == AcclaimZN1Game::NBAJamExtreme)
      AcclaimRAX::Reset();
  }
  else if (s_runtime->board_type == BoardType::TimeWarnerZN1)
  {
    TimeWarnerATA::Reset();
    if (s_time_warner_watchdog_event)
      s_time_warner_watchdog_event->Deactivate();
    s_runtime->time_warner_main_board_reset_pending = false;
  }
  s_runtime->coin = 0;
  s_runtime->motherboard_cat702.Reset();
  s_runtime->game_cat702.Reset();
  ApplySecuritySelect(*s_runtime, 0x0c);
  if (s_runtime->board_type == BoardType::BustAMove2ZN1)
  {
    s_runtime->bank = 1;
    s_runtime->bam2_mcu_ports.fill(0);
    s_runtime->bam2_mcu_ports[0] = 1;
    s_runtime->bam2_outputs_initialized = false;
    s_runtime->bam2_h8_status = UINT16_C(0x0004);
    s_runtime->bam2_h8_opcode = 0;
    s_runtime->bam2_h8_waiting_for_argument = false;
    s_runtime->bam2_selected_track = 0;
    s_runtime->bam2_tc9293_attenuation = 0;
    s_runtime->bam2_pcm_playing = false;
    s_runtime->bam2_pcm_track_start_offset = 0;
    s_runtime->bam2_pcm_track_end_offset = 0;
    ResetBAM2PCMBuffer(*s_runtime);
  }
  // AT28C16 contents and an in-flight write cycle are independent of the PSX reset line.
  s_runtime->at28_trace_count = 0;
  AtlusZN1Sound::Reset();
  EightingRaizingSound::Reset();
  CapcomQSound::Reset();
  TaitoFX1ASound::Reset();
  TecmoCBAJSound::Reset();
  TecmoGR2Link::Reset();
  if (s_runtime->board_type == BoardType::TaitoFX1B)
  {
    s_runtime->taito_zoom_reg_address = 0;
    TaitoFX1BZoom::Reset();
  }
  s_runtime->unknown_read_logged = false;
  s_runtime->unknown_write_logged = false;
  s_runtime->taito_unpopulated_bank_logged = false;
  s_runtime->taito_watchdog_ck = false;
  s_runtime->taito_watchdog_seen = false;
  s_runtime->taito_main_board_reset_pending = false;
  if (s_taito_watchdog_event)
    s_taito_watchdog_event->Deactivate();
  if (s_sio0_transfer_event)
    s_sio0_transfer_event->Deactivate();
  s_runtime->sio0_mode = 0;
  s_runtime->sio0_control = 0;
  s_runtime->sio0_baud = 0;
  s_runtime->sio0_rx_data = 0xff;
  s_runtime->sio0_tx_data = 0xff;
  s_runtime->sio0_tx_shift = 0xff;
  s_runtime->sio0_rx_full = false;
  s_runtime->sio0_tx_full = false;
  s_runtime->sio0_transfer_active = false;
  SetSIO0Interrupt(*s_runtime, false);
  s_runtime->sio_exchange_count = 0;
  s_runtime->sio_register_trace_count = 0;
  s_runtime->sio_status_trace_count = 0;
  s_runtime->mcu_selected = false;
  s_runtime->mcu_byte_index = 0;
  CancelZNMCUDSR(*s_runtime);
}

void Shutdown()
{
  if (!s_runtime)
    return;

  CancelZNMCUDSR(*s_runtime);
  if (s_sio0_transfer_event)
    s_sio0_transfer_event->Deactivate();
  if (s_taito_watchdog_event)
    s_taito_watchdog_event->Deactivate();
  if (s_time_warner_watchdog_event)
    s_time_warner_watchdog_event->Deactivate();

  AtlusZN1Sound::Shutdown();
  EightingRaizingSound::Shutdown();
  CapcomQSound::Shutdown();
  TaitoFX1ASound::Shutdown();
  TaitoFX1BZoom::Shutdown();
  TecmoCBAJSound::Shutdown();
  TecmoGR2Link::Shutdown();
  AcclaimATA::Shutdown();
  TimeWarnerATA::Shutdown();
  AcclaimRAX::Shutdown();
  SetSIO0Interrupt(*s_runtime, false);
  SaveAT28(*s_runtime);
  if (s_runtime->board_type == BoardType::AcclaimZN1 &&
      s_runtime->acclaim_game == AcclaimZN1Game::NBAJamExtreme)
  {
    SaveAcclaimNBASRAM(*s_runtime);
  }
  if (s_runtime->board_type == BoardType::TaitoFX1B)
    SaveTaitoFX1BFRAM(*s_runtime);
  s_runtime.reset();
}

bool IsActive()
{
  return s_runtime.has_value();
}

void ApplySPUOutputGain(s32* left, s32* right)
{
  if (!s_runtime)
    return;

  // Base ZN routes the onboard SPU at 0.35. Atlus ZN-1 and Taito FX-1B
  // override that board-level route to 0.175 and 0.30 respectively.
  u32 gain_per_thousand = 350;
  if (s_runtime->board_type == BoardType::AtlusZN1)
    gain_per_thousand = 1000;
  else if (s_runtime->board_type == BoardType::TaitoFX1B)
    gain_per_thousand = 1000;

  *left = (*left * static_cast<s32>(gain_per_thousand)) / 1000;
  *right = (*right * static_cast<s32>(gain_per_thousand)) / 1000;
}

void ProcessFrame()
{
  if (!s_runtime)
    return;

  if (s_runtime->board_type == BoardType::TecmoTPS && s_runtime->tecmo_gr2_link_enabled)
  {
    TecmoGR2Link::ProcessFrame();
    return;
  }

  if (s_runtime->board_type == BoardType::BustAMove2ZN1 && s_runtime->bam2_selected_track != 0)
  {
    // The physical H8/media path runs independently of the PSX SPU. Stage media
    // data per VBlank so libchdr decompression never occurs in the per-sample
    // mixer path. 4 KiB/60 Hz comfortably exceeds 44.1 kHz stereo s16
    // consumption (about 2.94 KiB/VBlank).
    if (!PrefetchBAM2PCM(*s_runtime, BAM2_PCM_PREFETCH_BYTES_PER_FRAME))
    {
      s_runtime->bam2_pcm_playing = false;
      s_runtime->bam2_h8_status = UINT16_C(0x0001);
    }
  }

  if (s_runtime->board_type != BoardType::AcclaimZN1 ||
      s_runtime->acclaim_game != AcclaimZN1Game::JudgeDredd)
  {
    return;
  }

  // The Judge Dredd board alternates which gun is sampled on each VBlank.
  s_runtime->acclaim_gun_mux ^= UINT8_C(1);

  const ArcadeInput::LightgunPresentationState state = ArcadeInput::GetLightgunPresentationState();
  const u32 player = s_runtime->acclaim_gun_mux;
  if (player >= state.ports.size())
    return;

  const ArcadeInput::LightgunPresentationPort& gun = state.ports[player];
  if (!gun.active || gun.offscreen)
    return;

  static constexpr u16 GUN_X_MIN = 0x0392;
  static constexpr u16 GUN_X_MAX = 0x0cb3;
  static constexpr u16 GUN_Y_MIN = 0x002c;
  static constexpr u16 GUN_Y_MAX = 0x0218;

  const auto scale_axis = [](float normalized, u16 min_value, u16 max_value) -> u16 {
    normalized = std::clamp(normalized, 0.0f, 1.0f);
    return static_cast<u16>(std::lround(static_cast<float>(min_value) +
                                        (normalized * static_cast<float>(max_value - min_value))));
  };

  const u16 x = scale_axis(gun.x, GUN_X_MIN, GUN_X_MAX);
  const u16 y = scale_axis(gun.y, GUN_Y_MIN, GUN_Y_MAX);

  // Match the board's calibrated valid window. Invalid/off-screen samples leave the GPU latch untouched.
  if (x > UINT16_C(0x0393) && x < UINT16_C(0x0cb2) &&
      y > UINT16_C(0x002d) && y < UINT16_C(0x0217))
  {
    g_gpu->SetLightgunCoordinates(x, y);
  }
}

bool BeginMainBoardReset()
{
  if (!s_runtime)
    return false;

  if (s_runtime->board_type == BoardType::TimeWarnerZN1 &&
      s_runtime->time_warner_main_board_reset_pending)
  {
    s_runtime->time_warner_main_board_reset_pending = false;
    if (s_time_warner_watchdog_event)
      s_time_warner_watchdog_event->Deactivate();
    return true;
  }

  if ((s_runtime->board_type != BoardType::TaitoFX1A && s_runtime->board_type != BoardType::TaitoFX1B) ||
      !s_runtime->taito_main_board_reset_pending)
  {
    return false;
  }

  s_runtime->taito_main_board_reset_pending = false;
  if (s_taito_watchdog_event)
    s_taito_watchdog_event->Deactivate();
  return true;
}

void EndMainBoardReset()
{
  Reset();
}

static u8 ExchangeCAT702Byte(RuntimeState& runtime, u8 tx)
{
  u8 rx = 0;
  for (u32 bit = 0; bit < 8; bit++)
  {
    const bool data_in = ((tx >> bit) & 1u) != 0;
    runtime.motherboard_cat702.SetDataInLine(data_in);
    runtime.game_cat702.SetDataInLine(data_in);

    // Both CAT702s share SIO0 clock/data. A deselected CAT702 releases RXD high,
    // so the CPU sees the wired-AND of the two physical data-out lines.
    runtime.motherboard_cat702.SetClockLine(false);
    runtime.game_cat702.SetClockLine(false);

    const bool data_out = runtime.motherboard_cat702.GetDataOutLine() && runtime.game_cat702.GetDataOutLine();
    if (data_out)
      rx |= static_cast<u8>(1u << bit);

    runtime.motherboard_cat702.SetClockLine(true);
    runtime.game_cat702.SetClockLine(true);
  }

  if (runtime.sio_exchange_count < 64)
  {
    DEV_LOG("SonyZN.SIO0 CAT702 byte={} sec=0x{:02X} tx=0x{:02X} rx=0x{:02X}", runtime.sio_exchange_count,
            runtime.security_select, tx, rx);
  }
  runtime.sio_exchange_count++;
  return rx;
}

static bool CanStartSIO0Transfer(const RuntimeState& runtime)
{
  // PSX SIO0 shifts only when TX is enabled and DTR/SELECT is asserted.
  return runtime.sio0_tx_full && (runtime.sio0_control & UINT16_C(0x0003)) == UINT16_C(0x0003);
}

static TickCount GetSIO0TransferTicks(const RuntimeState& runtime)
{
  u32 baud_prescaler;
  switch (runtime.sio0_mode & UINT16_C(0x0003))
  {
    case 1:
      baud_prescaler = 1;
      break;
    case 2:
      baud_prescaler = 16;
      break;
    case 3:
      baud_prescaler = 64;
      break;
    default:
      return 0;
  }

  if (runtime.sio0_baud == 0)
    return 0;

  const TickCount byte_ticks =
    static_cast<TickCount>(runtime.sio0_baud) * static_cast<TickCount>(baud_prescaler) * 8;
  return System::ScaleTicksToOverclock(byte_ticks);
}

static void BeginSIO0Transfer(RuntimeState& runtime);

static void SIO0TransferEventCallback(void*, TickCount, TickCount)
{
  if (s_sio0_transfer_event)
    s_sio0_transfer_event->Deactivate();

  if (!s_runtime || !s_runtime->sio0_transfer_active)
    return;

  RuntimeState& runtime = *s_runtime;
  runtime.sio0_transfer_active = false;

  runtime.sio0_rx_data = runtime.mcu_selected ? ExchangeZNMCUByte(runtime, runtime.sio0_tx_shift)
                                              : ExchangeCAT702Byte(runtime, runtime.sio0_tx_shift);
  runtime.sio0_rx_full = true;

  if ((runtime.sio0_control & UINT16_C(0x0800)) != 0) // RX interrupt enable
    SetSIO0Interrupt(runtime, true);

  // Like the PSX SIO holding/shift registers, a byte written while one was
  // shifting begins only after the current byte reaches the receive FIFO.
  if (CanStartSIO0Transfer(runtime))
    BeginSIO0Transfer(runtime);
}

static void BeginSIO0Transfer(RuntimeState& runtime)
{
  if (runtime.sio0_transfer_active || !CanStartSIO0Transfer(runtime))
    return;

  const TickCount transfer_ticks = GetSIO0TransferTicks(runtime);
  if (transfer_ticks <= 0)
    return;

  runtime.sio0_tx_shift = runtime.sio0_tx_data;
  runtime.sio0_tx_full = false;

  // COH-1000W performs its CAT702 security burst at mode 0x0E / baud 2. The
  // nominal byte time is only 256 master-clock ticks (~7.6 us), while the BIOS
  // immediately polls JOY_STAT in a tight loop. Complete this high-speed CAT702
  // byte atomically so recompiler/event scheduling granularity cannot stretch a
  // few milliseconds of serial traffic beyond the physical 600 ms watchdog.
  // Keep the timed path for the ZN MCU, whose DSR pulse timing is guest-visible.
  if (runtime.board_type == BoardType::TimeWarnerZN1 && !runtime.mcu_selected &&
      runtime.sio0_mode == UINT16_C(0x000e) && runtime.sio0_baud == UINT16_C(0x0002))
  {

    runtime.sio0_rx_data = ExchangeCAT702Byte(runtime, runtime.sio0_tx_shift);
    runtime.sio0_rx_full = true;
    if ((runtime.sio0_control & UINT16_C(0x0800)) != 0)
      SetSIO0Interrupt(runtime, true);
    return;
  }

  if (!s_sio0_transfer_event)
  {
    s_sio0_transfer_event =
      std::make_unique<TimingEvent>("Sony ZN SIO0 Serial Transfer", 1, 1, SIO0TransferEventCallback, nullptr);
  }

  runtime.sio0_transfer_active = true;
  s_sio0_transfer_event->SetPeriodAndSchedule(transfer_ticks);
}

u32 ReadSIO0Register(u32 offset)
{
  if (!s_runtime)
    return UINT32_C(0xffffffff);

  switch (offset)
  {
    case 0x00: // JOY_DATA
    {
      const u8 byte = s_runtime->sio0_rx_full ? s_runtime->sio0_rx_data : 0xff;
      s_runtime->sio0_rx_full = false;
      const u32 value = static_cast<u32>(byte);
      return value | (value << 8) | (value << 16) | (value << 24);
    }

    case 0x04: // JOY_STAT
    {
      // PSX SIO0 status is derived from the holding register, shift engine,
      // receive FIFO, physical /ACK line, and latched interrupt request.
      u32 status = 0;
      if (!s_runtime->sio0_tx_full)
        status |= UINT32_C(0x00000001); // TX ready
      if (s_runtime->sio0_rx_full)
        status |= UINT32_C(0x00000002); // RX FIFO not empty
      if (!s_runtime->sio0_tx_full && !s_runtime->sio0_transfer_active)
        status |= UINT32_C(0x00000004); // TX finished

      if (s_runtime->mcu_selected && s_runtime->mcu_dsr_active)
        status |= UINT32_C(0x00000080); // SIO0 /ACK input is active-low; STAT bit 7 reads 1 while low.
      if (s_runtime->sio0_interrupt_pending)
        status |= UINT32_C(0x00000200); // SIO0 interrupt request pending.

      if (s_runtime->sio_status_trace_count < 32)
      {
        DEV_LOG("SonyZN.SIO0 STAT read={} value=0x{:08X} rx_full={} tx_full={} active={} ctrl=0x{:04X} mode=0x{:04X} baud=0x{:04X}",
                 s_runtime->sio_status_trace_count, status, s_runtime->sio0_rx_full, s_runtime->sio0_tx_full,
                 s_runtime->sio0_transfer_active, s_runtime->sio0_control, s_runtime->sio0_mode,
                 s_runtime->sio0_baud);
      }
      s_runtime->sio_status_trace_count++;

      return status;
    }

    case 0x08: // JOY_MODE
      return static_cast<u32>(s_runtime->sio0_mode);

    case 0x0a: // JOY_CTRL
      return static_cast<u32>(s_runtime->sio0_control);

    case 0x0e: // JOY_BAUD
      return static_cast<u32>(s_runtime->sio0_baud);

    default:
      return UINT32_C(0xffffffff);
  }
}

void WriteSIO0Register(u32 offset, u32 value)
{
  if (!s_runtime)
    return;

  switch (offset)
  {
    case 0x00: // JOY_DATA
    {
      s_runtime->sio0_tx_data = static_cast<u8>(value);
      s_runtime->sio0_tx_full = true;

      if ((s_runtime->sio0_control & UINT16_C(0x0400)) != 0) // TX interrupt enable
        SetSIO0Interrupt(*s_runtime, true);

      if (!s_runtime->sio0_transfer_active && CanStartSIO0Transfer(*s_runtime))
        BeginSIO0Transfer(*s_runtime);
      return;
    }

    case 0x08: // JOY_MODE

      s_runtime->sio0_mode = static_cast<u16>(value);
      if (s_runtime->sio_register_trace_count < 32)
        DEV_LOG("SonyZN.SIO0 MODE <- 0x{:04X}", s_runtime->sio0_mode);
      s_runtime->sio_register_trace_count++;
      return;

    case 0x0a: // JOY_CTRL
    {
      const u16 control = static_cast<u16>(value);

      if (s_runtime->sio_register_trace_count < 32)
        DEV_LOG("SonyZN.SIO0 CTRL <- 0x{:04X}", control);
      s_runtime->sio_register_trace_count++;

      if ((control & UINT16_C(0x0010)) != 0) // SIO0 interrupt acknowledge
        SetSIO0Interrupt(*s_runtime, false);

      if ((control & UINT16_C(0x0040)) != 0) // SIO0 reset
      {
        if (s_sio0_transfer_event)
          s_sio0_transfer_event->Deactivate();
        s_runtime->sio0_mode = 0;
        s_runtime->sio0_control = 0;
        s_runtime->sio0_rx_data = 0xff;
        s_runtime->sio0_tx_data = 0xff;
        s_runtime->sio0_tx_shift = 0xff;
        s_runtime->sio0_rx_full = false;
        s_runtime->sio0_tx_full = false;
        s_runtime->sio0_transfer_active = false;
        SetSIO0Interrupt(*s_runtime, false);
        return;
      }

      s_runtime->sio0_control = control;

      // Disabling TX or deasserting SELECT aborts the shift engine, matching
      // the normal PSX controller-port serial state machine.
      if ((control & UINT16_C(0x0003)) != UINT16_C(0x0003) && s_runtime->sio0_transfer_active)
      {
        if (s_sio0_transfer_event)
          s_sio0_transfer_event->Deactivate();
        s_runtime->sio0_transfer_active = false;
      }
      else if (!s_runtime->sio0_transfer_active && CanStartSIO0Transfer(*s_runtime))
      {
        BeginSIO0Transfer(*s_runtime);
      }
      return;
    }

    case 0x0e: // JOY_BAUD

      s_runtime->sio0_baud = static_cast<u16>(value);
      if (s_runtime->sio_register_trace_count < 32)
        DEV_LOG("SonyZN.SIO0 BAUD <- 0x{:04X}", s_runtime->sio0_baud);
      s_runtime->sio_register_trace_count++;
      return;

    default:
      if (s_runtime->sio_register_trace_count < 32)
        DEV_LOG("SonyZN.SIO0 unhandled write offset=0x{:02X} value=0x{:08X}", offset, value);
      s_runtime->sio_register_trace_count++;
      return;
  }
}
u32 ReadEXP1(u32 width, u32 offset)
{
  if (!s_runtime)
    return UINT32_C(0xffffffff);

  if (s_runtime->board_type == BoardType::TimeWarnerZN1)
  {
    if (TimeWarnerATA::HandlesOffset(offset))
      return TimeWarnerATA::Read(width, offset);
    if (offset >= TIME_WARNER_PROGRAM_ROM_SIZE)
      return UINT32_C(0xffffffff);


    return ReadBytes(s_runtime->content.banked_rom, width, offset);
  }

  if (s_runtime->board_type == BoardType::AcclaimZN1)
  {
    if (s_runtime->acclaim_game == AcclaimZN1Game::JudgeDredd)
    {
      if (offset >= ACCLAIM_JDREDD_ROM_SIZE)
        return UINT32_C(0xffffffff);
      return ReadBytes(s_runtime->content.banked_rom, width, offset);
    }

    if (offset >= ACCLAIM_EXP1_SIZE)
      return UINT32_C(0xffffffff);

    const u16 bank0_reg = s_runtime->acclaim_bank_registers[0];
    const u16 bank1_reg = s_runtime->acclaim_bank_registers[1];
    const u32 bank0 = ((static_cast<u32>(bank0_reg) >> 4) & 1u) |
                      ((static_cast<u32>(bank0_reg) & 7u) << 1);
    if (offset < ACCLAIM_NBA_BANK_SIZE)
    {
      return ReadBytes(s_runtime->content.banked_rom, width,
                       (bank0 * ACCLAIM_NBA_BANK_SIZE) + offset);
    }

    // With bank register 0 clear, the game board exposes its 32 KiB battery-backed
    // 71256 SRAM at 0x1F200000 and leaves the remainder of this window unmapped.
    if (bank0_reg == 0)
    {
      if (offset >= ACCLAIM_NBA_SRAM_BASE &&
          offset < (ACCLAIM_NBA_SRAM_BASE + ACCLAIM_NBA_SRAM_SIZE))
      {
        return ReadBytes(std::span<const u8>(s_runtime->acclaim_nba_sram), width,
                         offset - ACCLAIM_NBA_SRAM_BASE);
      }
      return UINT32_C(0xffffffff);
    }

    const u32 bank1 = ((bank1_reg & UINT16_C(0x0010)) == 0 ? 1u : 0u) |
                      ((static_cast<u32>(bank1_reg) & 7u) << 1);
    return ReadBytes(s_runtime->content.banked_rom, width,
                     (bank1 * ACCLAIM_NBA_BANK_SIZE) + (offset - ACCLAIM_NBA_BANK_SIZE));
  }

  if (s_runtime->board_type == BoardType::VideoSystemZN1)
  {
    if (offset >= VIDEO_SYSTEM_FIXED_ROM_SIZE)
      return UINT32_C(0xffffffff);

    return ReadBytes(s_runtime->video_system_fixed_rom, width, offset);
  }

  if (s_runtime->board_type == BoardType::AtlusZN1)
  {
    if (offset >= ATLUS_EXP1_SIZE)
      return UINT32_C(0xffffffff);

    const u32 source_offset = (static_cast<u32>(s_runtime->bank) * ATLUS_BANK_SIZE) + offset;
    return ReadBytes(s_runtime->content.banked_rom, width, source_offset);
  }

  if (s_runtime->board_type == BoardType::BustAMove2ZN1)
  {
    if (offset >= BAM2_EXP1_SIZE)
      return UINT32_C(0xffffffff);

    if (offset < BAM2_FIXED_ROM_SIZE)
      return ReadBytes(s_runtime->content.banked_rom, width, offset);

    if (s_runtime->bank >= BAM2_BANK_SELECT_COUNT)
      return UINT32_C(0xffffffff);

    const u32 source_offset = (static_cast<u32>(s_runtime->bank) * BAM2_BANK_SIZE) +
                              (offset - BAM2_FIXED_ROM_SIZE);
    return ReadBytes(s_runtime->content.banked_rom, width, source_offset);
  }

  if (s_runtime->board_type == BoardType::EightingRaizingZN1)
  {
    if (offset >= EIGHTING_EXP1_SIZE || s_runtime->bank >= EIGHTING_BANK_COUNT)
      return UINT32_C(0xffffffff);

    const u32 source_offset = (static_cast<u32>(s_runtime->bank) * EIGHTING_BANK_SIZE) + offset;
    return ReadBytes(s_runtime->content.banked_rom, width, source_offset);
  }

  if (s_runtime->board_type == BoardType::TecmoTPS)
  {
    if (offset >= TECMO_EXP1_SIZE)
      return UINT32_C(0xffffffff);

    const u32 source_offset = (static_cast<u32>(s_runtime->bank) * TECMO_BANK_SIZE) + offset;
    return ReadBytes(s_runtime->content.banked_rom, width, source_offset);
  }

  if (s_runtime->board_type == BoardType::TaitoFX1A || s_runtime->board_type == BoardType::TaitoFX1B)
  {
    if (offset >= TAITO_EXP1_SIZE)
      return UINT32_C(0xffffffff);

    const u32 source_offset = (static_cast<u32>(s_runtime->bank) * TAITO_BANK_SIZE) + offset;
    return ReadBytes(s_runtime->content.banked_rom, width, source_offset);
  }

  if (offset >= CAPCOM_EXP1_SIZE)
    return UINT32_C(0xffffffff);

  if (offset < CAPCOM_FIXED_ROM_SIZE)
    return ReadBytes(s_runtime->content.banked_rom, width, offset);

  const u32 window_offset = offset - CAPCOM_FIXED_ROM_SIZE;
  const u32 source_offset = CAPCOM_FIXED_ROM_SIZE + (static_cast<u32>(s_runtime->bank) * CAPCOM_BANK_SIZE) +
                            window_offset;
  return ReadBytes(s_runtime->content.banked_rom, width, source_offset);
}

bool WriteEXP1(u32 width, u32 offset, u32 value)
{
  if (s_runtime && s_runtime->board_type == BoardType::TimeWarnerZN1)
  {
    if (offset < 4 && (width == 1 || width == 2 || width == 4))
    {
      ClockTimeWarnerWatchdog(*s_runtime);
      return true;
    }

    if (TimeWarnerATA::HandlesOffset(offset))
    {
      TimeWarnerATA::Write(width, offset, value);
      return true;
    }
    return false;
  }

  if (s_runtime && s_runtime->board_type == BoardType::EightingRaizingZN1 &&
      s_runtime->eighting_ps9805_flash && s_runtime->bank == 0 &&
      offset < EIGHTING_PS9805_FLASH_SIZE && (width == 1 || width == 2 || width == 4))
  {

    // PS9805 uses two Sharp LH28F160 devices in the first 4 MiB. Keep the
    // current ROM contents unchanged until guest writes demonstrate that
    // command-state emulation is actually required.
    return true;
  }

  if (!s_runtime || s_runtime->board_type != BoardType::AcclaimZN1 ||
      s_runtime->acclaim_game != AcclaimZN1Game::NBAJamExtreme ||
      s_runtime->acclaim_bank_registers[0] != 0 ||
      offset < ACCLAIM_NBA_SRAM_BASE ||
      offset >= (ACCLAIM_NBA_SRAM_BASE + ACCLAIM_NBA_SRAM_SIZE) ||
      (width != 1 && width != 2 && width != 4))
  {
    return false;
  }

  const u32 sram_offset = offset - ACCLAIM_NBA_SRAM_BASE;
  for (u32 i = 0; i < width && (sram_offset + i) < s_runtime->acclaim_nba_sram.size(); i++)
  {
    const u8 byte = static_cast<u8>(value >> (i * 8));
    if (s_runtime->acclaim_nba_sram[sram_offset + i] != byte)
    {
      s_runtime->acclaim_nba_sram[sram_offset + i] = byte;
      s_runtime->acclaim_nba_sram_dirty = true;
    }
  }
  return true;
}

bool ReadEXP3InstructionWord(u32 offset, u32* value)
{
  if (!s_runtime || !value)
    return false;

  if (s_runtime->board_type == BoardType::CapcomZN1 && offset >= CAPCOM_COUNTRY_BASE &&
      offset <= (CAPCOM_COUNTRY_END - (sizeof(u32) - 1)))
  {
    *value = ReadBytes(s_runtime->content.country_rom, sizeof(u32), offset - CAPCOM_COUNTRY_BASE);
    return true;
  }

  if (s_runtime->board_type == BoardType::VideoSystemZN1 && offset >= VIDEO_SYSTEM_BANK_BASE &&
      offset <= (VIDEO_SYSTEM_BANK_END - (sizeof(u32) - 1)))
  {
    const u32 source_offset = (static_cast<u32>(s_runtime->bank) * VIDEO_SYSTEM_BANK_SIZE) +
                              (offset - VIDEO_SYSTEM_BANK_BASE);
    *value = ReadBytes(s_runtime->content.banked_rom, sizeof(u32), source_offset);
    return true;
  }

  return false;
}

u32 ReadEXP3(u32 width, u32 offset)
{
  if (!s_runtime)
    return UINT32_C(0xffffffff);


  if (offset >= P1_BASE && offset <= (P1_BASE + 3))
    return ReadBytePortWindow(ReadZNPlayerPort(0), width, offset - P1_BASE);
  if (offset >= P2_BASE && offset <= (P2_BASE + 3))
    return ReadBytePortWindow(ReadZNPlayerPort(1), width, offset - P2_BASE);
  if (offset >= SERVICE_BASE && offset <= (SERVICE_BASE + 3))
    return ReadBytePortWindow(ReadZNServicePort(), width, offset - SERVICE_BASE);
  if (offset >= SYSTEM_BASE && offset <= (SYSTEM_BASE + 3))
    return ReadBytePortWindow(ReadZNSystemPort(), width, offset - SYSTEM_BASE);
  if (offset >= P3_BASE && offset <= (P3_BASE + 3))
    return ReadBytePortWindow(ReadZNExtendedPlayerPort(0), width, offset - P3_BASE);
  if (offset >= P4_BASE && offset <= (P4_BASE + 3))
    return ReadBytePortWindow(ReadZNExtendedPlayerPort(1), width, offset - P4_BASE);
  if (offset >= CAPCOM_KICK1_BASE && offset <= CAPCOM_KICK1_END)
    return ReadBytePortWindow(ReadCapcomKickPort(0), width, offset - CAPCOM_KICK1_BASE);
  if (offset >= CAPCOM_KICK2_BASE && offset <= CAPCOM_KICK2_END)
    return ReadBytePortWindow(ReadCapcomKickPort(1), width, offset - CAPCOM_KICK2_BASE);

  if (offset == BOARD_CONFIG)
  {
    // ZN board-config bits encode main RAM and VRAM population. COH-1000W
    // is the Time Warner 8 MiB RAM / 2 MiB VRAM configuration (0x6A).
    const u8 board_config = s_runtime->board_type == BoardType::TimeWarnerZN1 ? UINT8_C(0x6a) :
                            s_runtime->content.use_2mb_vram ? UINT8_C(0x69) : UINT8_C(0x61);
    return ReadByteRegister(board_config, width);
  }
  if (offset == SECURITY_SELECT)
    return ReadByteRegister(s_runtime->security_select, width);
  if (offset == COIN_REGISTER)
    return ReadByteRegister(s_runtime->coin, width);

  if (s_runtime->board_type == BoardType::BustAMove2ZN1 &&
      offset >= BAM2_MCU_BASE && offset <= BAM2_MCU_END)
  {
    return ReadBAM2MCU(*s_runtime, width, offset);
  }

  if (s_runtime->board_type == BoardType::TecmoTPS && s_runtime->tecmo_gr2_link_enabled &&
      offset >= TECMO_CBAJ_SOUND_DATA && offset <= TECMO_CBAJ_SOUND_STATUS &&
      (width == 1 || width == 2 || width == 4) &&
      (offset + width - 1) <= TECMO_CBAJ_SOUND_STATUS)
  {
    u32 result = 0;
    for (u32 i = 0; i < width; i++)
    {
      const u32 address = offset + i;
      u8 byte = UINT8_C(0xff);
      if (address == TECMO_CBAJ_SOUND_DATA)
        byte = TecmoGR2Link::MainDataRead();
      else if (address == TECMO_CBAJ_SOUND_STATUS)
        byte = TecmoGR2Link::MainStatusRead();
      result |= static_cast<u32>(byte) << (i * 8);
    }
    return result;
  }

  if (s_runtime->board_type == BoardType::TecmoTPS && s_runtime->tecmo_cbaj_sound_enabled &&
      offset >= TECMO_CBAJ_SOUND_DATA && offset <= TECMO_CBAJ_SOUND_STATUS &&
      (width == 1 || width == 2 || width == 4) &&
      (offset + width - 1) <= TECMO_CBAJ_SOUND_STATUS)
  {
    // CBAJ decodes byte 0 as FIFO data and byte 3 as the active-low /EF status.
    // Compose wider PSX accesses byte-by-byte; the game reads a halfword at
    // 0x1FB00002 and expects the status byte in the high lane.
    u32 result = 0;
    for (u32 i = 0; i < width; i++)
    {
      const u32 address = offset + i;
      u8 byte = UINT8_C(0xff);
      if (address == TECMO_CBAJ_SOUND_DATA)
        byte = TecmoCBAJSound::MainDataRead();
      else if (address == TECMO_CBAJ_SOUND_STATUS)
        byte = TecmoCBAJSound::MainStatusRead();
      result |= static_cast<u32>(byte) << (i * 8);
    }
    return result;
  }

  if ((offset >= COMMON_NOP_RW && offset <= (COMMON_NOP_RW + 3)) ||
      (offset >= COMMON_NOP_R && offset <= (COMMON_NOP_R + 3)))
  {
    return UINT32_C(0xffffffff);
  }

  if (offset >= UNKNOWN_BASE && offset <= UNKNOWN_END)
  {
    const u32 value =
      (width == 1) ? UINT32_C(0x000000ff) :
      (width == 2) ? UINT32_C(0x0000ffff) :
      (width == 4) ? UINT32_C(0x0000ffff) : UINT32_C(0xffffffff);

    if (!s_runtime->known_unknown_io_read_logged)
    {
      DEV_LOG("SonyZN known unknown-I/O read physical=0x{:08X} offset=0x{:06X} width={} value=0x{:08X}",
               UINT32_C(0x1fa00000) + offset, offset, width, value);
      s_runtime->known_unknown_io_read_logged = true;
    }

    return value;
  }

  if (offset >= AT28_BASE && offset <= AT28_END)
  {
    if (width != 1 && width != 2 && width != 4)
      return UINT32_C(0xffffffff);

    u32 value = 0;
    for (u32 i = 0; i < width; i++)
    {
      const u32 address = offset + i;
      const u8 byte = (address <= AT28_END) ? ReadAT28Byte(*s_runtime, address - AT28_BASE) : UINT8_C(0xff);
      value |= static_cast<u32>(byte) << (i * 8);
    }
    return value;
  }

  if (s_runtime->board_type == BoardType::AcclaimZN1 &&
      s_runtime->acclaim_game == AcclaimZN1Game::NBAJamExtreme)
  {
    if (offset >= ACCLAIM_NBA_SOUND_STATUS && offset <= (ACCLAIM_NBA_SOUND_STATUS + 1))
      return ReadHalfwordPortWindow(UINT16_C(0x0400), width, offset - ACCLAIM_NBA_SOUND_STATUS);
    if (offset >= ACCLAIM_NBA_SOUND_LATCH && offset <= (ACCLAIM_NBA_SOUND_LATCH + 1))
      return ReadHalfwordPortWindow(UINT16_C(0xffff), width, offset - ACCLAIM_NBA_SOUND_LATCH);
  }

  if (s_runtime->board_type == BoardType::AcclaimZN1 && s_runtime->acclaim_game == AcclaimZN1Game::JudgeDredd &&
      AcclaimATA::HandlesOffset(offset))
  {
    return AcclaimATA::Read(width, offset);
  }

  if (s_runtime->board_type == BoardType::TaitoFX1B)
  {
    if (offset >= TAITO_FX1B_FRAM_BASE && offset <= TAITO_FX1B_FRAM_END)
    {
      return ReadEvenByteWindow(std::span<const u8>(s_runtime->taito_fx1b_fram), width,
                                offset - TAITO_FX1B_FRAM_BASE);
    }

    if (offset >= TAITO_ZOOM_IRQ_READ && offset <= (TAITO_ZOOM_IRQ_READ + 1))
      return ReadHalfwordPortWindow(TaitoFX1BZoom::StatusRead(), width, offset - TAITO_ZOOM_IRQ_READ);

    if (offset >= TAITO_ZOOM_SHARED_BASE && offset <= TAITO_ZOOM_SHARED_END)
    {
      if (width != 1 && width != 2 && width != 4)
        return UINT32_C(0xffffffff);

      u32 value = 0;
      for (u32 i = 0; i < width; i++)
      {
        const u32 address = (offset - TAITO_ZOOM_SHARED_BASE) + i;
        const u8 byte = ((address & 1u) == 0) ? TaitoFX1BZoom::SharedRead(address >> 1) : UINT8_C(0xff);
        value |= static_cast<u32>(byte) << (i * 8);
      }
      return value;
    }
  }

  if (s_runtime->board_type == BoardType::TaitoFX1A && offset == TAITO_SOUND_COMM)
    return ReadByteRegister(TaitoFX1ASound::MasterCommRead(), width);

  if (s_runtime->board_type == BoardType::VideoSystemZN1 && offset >= VIDEO_SYSTEM_BANK_BASE &&
      offset <= VIDEO_SYSTEM_BANK_END)
  {
    const u32 source_offset = (static_cast<u32>(s_runtime->bank) * VIDEO_SYSTEM_BANK_SIZE) +
                              (offset - VIDEO_SYSTEM_BANK_BASE);
    return ReadBytes(s_runtime->content.banked_rom, width, source_offset);
  }

  if (s_runtime->board_type == BoardType::CapcomZN1 && offset >= CAPCOM_COUNTRY_BASE &&
      offset <= CAPCOM_COUNTRY_END)
  {
    return ReadBytes(s_runtime->content.country_rom, width, offset - CAPCOM_COUNTRY_BASE);
  }

  if (!s_runtime->unknown_read_logged)
  {
    DEV_LOG("SonyZN first unhandled EXP3 read offset=0x{:06X} width={}.", offset, width);
    s_runtime->unknown_read_logged = true;
  }
  return UINT32_C(0xffffffff);
}

void WriteEXP3(u32 width, u32 offset, u32 value)
{
  if (!s_runtime)
    return;


  if (offset == SECURITY_SELECT)
  {
    ApplySecuritySelect(*s_runtime, static_cast<u8>(value));
    return;
  }

  if (offset == COIN_REGISTER)
  {
    s_runtime->coin = static_cast<u8>(value);
    return;
  }

  if (offset >= COMMON_NOP_RW && offset <= (COMMON_NOP_RW + 3))
    return;

  if (s_runtime->board_type == BoardType::BustAMove2ZN1 &&
      offset >= BAM2_MCU_BASE && offset <= BAM2_MCU_END)
  {
    WriteBAM2MCU(*s_runtime, width, offset, value);
    return;
  }

  if (s_runtime->board_type == BoardType::EightingRaizingZN1 && offset == EIGHTING_SOUND_LATCH)
  {

    EightingRaizingSound::MainCommandWrite(static_cast<u8>(value));
    return;
  }

  if (s_runtime->board_type == BoardType::EightingRaizingZN1 && offset == EIGHTING_SOUND_IRQ)
  {

    EightingRaizingSound::MainIRQWrite();
    return;
  }

  if (s_runtime->board_type == BoardType::TecmoTPS && s_runtime->tecmo_gr2_link_enabled &&
      offset == TECMO_GR2_LINK_IRQ && width == 2)
  {
    TecmoGR2Link::MainIRQWrite(static_cast<u8>(value));
    return;
  }

  if (s_runtime->board_type == BoardType::TecmoTPS && s_runtime->tecmo_gr2_link_enabled &&
      offset >= TECMO_CBAJ_SOUND_DATA && offset <= TECMO_CBAJ_SOUND_STATUS &&
      (width == 1 || width == 2 || width == 4) &&
      (offset + width - 1) <= TECMO_CBAJ_SOUND_STATUS)
  {
    bool data_written = false;
    for (u32 i = 0; i < width; i++)
    {
      if ((offset + i) == TECMO_CBAJ_SOUND_DATA)
      {
        TecmoGR2Link::MainDataWrite(static_cast<u8>(value >> (i * 8)));
        data_written = true;
      }
    }

    if (data_written)
      return;
  }

  if (s_runtime->board_type == BoardType::TecmoTPS && s_runtime->tecmo_cbaj_sound_enabled &&
      offset >= TECMO_CBAJ_SOUND_DATA && offset <= TECMO_CBAJ_SOUND_STATUS &&
      (width == 1 || width == 2 || width == 4) &&
      (offset + width - 1) <= TECMO_CBAJ_SOUND_STATUS)
  {
    bool data_written = false;
    for (u32 i = 0; i < width; i++)
    {
      if ((offset + i) == TECMO_CBAJ_SOUND_DATA)
      {
        TecmoCBAJSound::MainDataWrite(static_cast<u8>(value >> (i * 8)));
        data_written = true;
      }
    }

    if (data_written)
      return;
  }

  if (offset >= AT28_BASE && offset <= AT28_END)
  {
    if (width != 1 && width != 2 && width != 4)
      return;

    for (u32 i = 0; i < width && (offset + i) <= AT28_END; i++)
      WriteAT28Byte(*s_runtime, (offset + i) - AT28_BASE, static_cast<u8>(value >> (i * 8)));
    return;
  }

  if (s_runtime->board_type == BoardType::AcclaimZN1 &&
      s_runtime->acclaim_game == AcclaimZN1Game::NBAJamExtreme &&
      offset >= ACCLAIM_NBA_SOUND_LATCH && offset <= (ACCLAIM_NBA_SOUND_LATCH + 1))
  {
    if (width != 1 && width != 2 && width != 4)
      return;

    for (u32 i = 0; i < width && (offset + i) <= (ACCLAIM_NBA_SOUND_LATCH + 1); i++)
    {
      const u32 byte_offset = (offset + i) - ACCLAIM_NBA_SOUND_LATCH;
      const u32 shift = byte_offset * 8;
      const u16 mask = static_cast<u16>(UINT16_C(0x00ff) << shift);
      const u16 byte_value = static_cast<u16>(((value >> (i * 8)) & 0xffu) << shift);
      s_runtime->acclaim_rax_host_latch =
        static_cast<u16>((s_runtime->acclaim_rax_host_latch & ~mask) | byte_value);
    }


    AcclaimRAX::WriteCommand(s_runtime->acclaim_rax_host_latch);
    return;
  }

  if (s_runtime->board_type == BoardType::AcclaimZN1 && s_runtime->acclaim_game == AcclaimZN1Game::JudgeDredd &&
      AcclaimATA::HandlesOffset(offset))
  {
    AcclaimATA::Write(width, offset, value);
    return;
  }

  if (s_runtime->board_type == BoardType::AcclaimZN1 &&
      offset >= ACCLAIM_BANK_REGISTER && offset <= (ACCLAIM_BANK_REGISTER + 3))
  {
    if (width != 1 && width != 2 && width != 4)
      return;

    bool changed = false;
    for (u32 i = 0; i < width && (offset + i) <= (ACCLAIM_BANK_REGISTER + 3); i++)
    {
      const u32 byte_offset = (offset + i) - ACCLAIM_BANK_REGISTER;
      const u32 reg_index = byte_offset >> 1;
      const u32 shift = (byte_offset & 1u) * 8u;
      const u16 mask = static_cast<u16>(UINT16_C(0x00ff) << shift);
      const u16 byte_value = static_cast<u16>(((value >> (i * 8u)) & 0xffu) << shift);
      const u16 old_value = s_runtime->acclaim_bank_registers[reg_index];
      const u16 new_value = static_cast<u16>((old_value & ~mask) | byte_value);
      if (old_value != new_value)
      {
        s_runtime->acclaim_bank_registers[reg_index] = new_value;
        changed = true;
      }
    }

    if (changed && s_runtime->acclaim_game == AcclaimZN1Game::NBAJamExtreme)
      CPU::CodeCache::InvalidateBlocksInPhysicalRange(Bus::EXP1_BASE, ACCLAIM_EXP1_SIZE);

    DEV_LOG("SonyZN Acclaim bank regs <- [0x{:04X}, 0x{:04X}]",
            s_runtime->acclaim_bank_registers[0], s_runtime->acclaim_bank_registers[1]);
    return;
  }

  if (s_runtime->board_type == BoardType::AcclaimZN1 &&
      offset >= ACCLAIM_ACPSX10_REGISTER && offset <= (ACCLAIM_ACPSX10_REGISTER + 1))
  {
    // The physical function of this Acclaim board output remains unknown. Judge
    // Dredd toggles bit 1 periodically, but boot/storage behavior does not consume it.
    return;
  }

  if (s_runtime->board_type == BoardType::TaitoFX1B)
  {
    if (offset >= TAITO_FX1B_FRAM_BASE && offset <= TAITO_FX1B_FRAM_END)
    {
      if (width != 1 && width != 2 && width != 4)
        return;

      for (u32 i = 0; i < width && (offset + i) <= TAITO_FX1B_FRAM_END; i++)
      {
        const u32 address = (offset + i) - TAITO_FX1B_FRAM_BASE;
        if ((address & 1u) != 0)
          continue;

        const size_t index = static_cast<size_t>(address >> 1);
        const u8 byte = static_cast<u8>(value >> (i * 8));
        if (index < s_runtime->taito_fx1b_fram.size() && s_runtime->taito_fx1b_fram[index] != byte)
        {
          s_runtime->taito_fx1b_fram[index] = byte;
          s_runtime->taito_fx1b_fram_dirty = true;
        }
      }
      return;
    }

    if (offset >= TAITO_ZOOM_DATA && offset <= (TAITO_ZOOM_DATA + 1))
    {
      const u16 data =
        (width == 1 && offset == (TAITO_ZOOM_DATA + 1)) ? static_cast<u16>((value & 0xff) << 8) :
                                                         static_cast<u16>(value);
      if (s_runtime->taito_zoom_reg_address == 0x04)
        s_runtime->taito_zoom_gain_left = data & UINT16_C(0x003f);
      else if (s_runtime->taito_zoom_reg_address == 0x05)
        s_runtime->taito_zoom_gain_right = data & UINT16_C(0x003f);
      TaitoFX1BZoom::RegDataWrite(data);
      return;
    }

    if (offset >= TAITO_ZOOM_ADDRESS && offset <= (TAITO_ZOOM_ADDRESS + 1))
    {
      const u16 data =
        (width == 1 && offset == (TAITO_ZOOM_ADDRESS + 1)) ? static_cast<u16>((value & 0xff) << 8) :
                                                            static_cast<u16>(value);
      s_runtime->taito_zoom_reg_address = static_cast<u8>(data);
      TaitoFX1BZoom::RegAddressWrite(data);
      return;
    }

    if (offset >= TAITO_ZOOM_IRQ_WRITE && offset <= (TAITO_ZOOM_IRQ_WRITE + 1))
    {
      TaitoFX1BZoom::PulseMainIRQ();
      return;
    }

    if (offset >= TAITO_ZOOM_SHARED_BASE && offset <= TAITO_ZOOM_SHARED_END)
    {
      if (width != 1 && width != 2 && width != 4)
        return;

      for (u32 i = 0; i < width; i++)
      {
        const u32 address = (offset - TAITO_ZOOM_SHARED_BASE) + i;
        if ((address & 1u) == 0)
          TaitoFX1BZoom::SharedWrite(address >> 1, static_cast<u8>(value >> (i * 8)));
      }
      return;
    }
  }

  if (s_runtime->board_type == BoardType::AtlusZN1 && offset == ATLUS_SOUND_LATCH && width == 2)
  {
    AtlusZN1Sound::MainCommandWrite(static_cast<u16>(value));
    return;
  }

  if (s_runtime->board_type == BoardType::AtlusZN1 && offset == ATLUS_BANK_REGISTER)
  {
    // Heaven's Gate performs SH to 0x1FB00002. The physical bank register is
    // byte-wide on the low lane, so a 16-bit PSX write still updates it from
    // the low byte.
    if (width != 1 && width != 2)
      return;

    const u8 new_bank = static_cast<u8>(value & 0x03);
    if (s_runtime->bank != new_bank)
    {
      s_runtime->bank = new_bank;
      CPU::CodeCache::InvalidateBlocksInPhysicalRange(Bus::EXP1_BASE, ATLUS_EXP1_SIZE);
      DEV_LOG("SonyZN Atlus ROM bank <- 0x{:02X} (write width={})", s_runtime->bank, width);
    }
    return;
  }

  if (s_runtime->board_type == BoardType::VideoSystemZN1 && offset == VIDEO_SYSTEM_BANK_REGISTER)
  {
    if (width != 1 && width != 2 && width != 4)
      return;

    const u8 new_bank = static_cast<u8>(value);
    if (s_runtime->bank != new_bank)
    {
      s_runtime->bank = new_bank;
      CPU::CodeCache::InvalidateBlocksInPhysicalRange(Bus::EXP3_BASE + VIDEO_SYSTEM_BANK_BASE,
                                                       VIDEO_SYSTEM_BANK_SIZE);
    }

    DEV_LOG("SonyZN Video System ROM bank <- 0x{:02X}", s_runtime->bank);
    return;
  }

  if (s_runtime->board_type == BoardType::TecmoTPS && offset == TECMO_BANK_REGISTER)
  {
    if (width != 1 && width != 2 && width != 4)
      return;

    const u8 new_bank = static_cast<u8>(value);
    if (s_runtime->bank != new_bank)
    {
      s_runtime->bank = new_bank;
      CPU::CodeCache::InvalidateBlocksInPhysicalRange(Bus::EXP1_BASE, TECMO_EXP1_SIZE);
      DEV_LOG("SonyZN Tecmo TPS ROM bank <- 0x{:02X} (write width={})", s_runtime->bank, width);
    }
    return;
  }

  if ((s_runtime->board_type == BoardType::TaitoFX1A || s_runtime->board_type == BoardType::TaitoFX1B) &&
      offset == TAITO_BANK_REGISTER)
  {
    ClockTaitoWatchdog(*s_runtime, ((value >> 5) & 1) != 0);

    const u8 new_bank = static_cast<u8>(value & 0x03);
    if (s_runtime->bank != new_bank)
    {
      s_runtime->bank = new_bank;
      CPU::CodeCache::InvalidateBlocksInPhysicalRange(Bus::EXP1_BASE, TAITO_EXP1_SIZE);
      DEV_LOG("SonyZN Taito ROM bank <- 0x{:02X}", s_runtime->bank);
    }

    if (s_runtime->bank >= 2 && !s_runtime->taito_unpopulated_bank_logged)
    {
      DEV_LOG(
        "SonyZN Taito FX-1 selected ROM bank {} but the validated set contains only 16MiB (banks 0-1); "
        "reads from this bank will remain open-bus until the decode is resolved.",
        s_runtime->bank);
      s_runtime->taito_unpopulated_bank_logged = true;
    }

    return;
  }

  if (s_runtime->board_type == BoardType::TaitoFX1A && offset == TAITO_SOUND_PORT)
  {
    TaitoFX1ASound::MasterPortWrite(static_cast<u8>(value));
    return;
  }

  if (s_runtime->board_type == BoardType::TaitoFX1A && offset == TAITO_SOUND_COMM)
  {
    TaitoFX1ASound::MasterCommWrite(static_cast<u8>(value));
    return;
  }

  if (s_runtime->board_type == BoardType::CapcomZN1 && offset == CAPCOM_BANK_REGISTER)
  {
    const u8 new_bank = static_cast<u8>(value & 0x0f);
    if (s_runtime->bank != new_bank)
    {
      s_runtime->bank = new_bank;
      CPU::CodeCache::InvalidateBlocksInPhysicalRange(Bus::EXP1_BASE + CAPCOM_FIXED_ROM_SIZE, CAPCOM_BANK_SIZE);
    }

    DEV_LOG("SonyZN Capcom ROM bank <- 0x{:02X}", s_runtime->bank);
    return;
  }

  if (s_runtime->board_type == BoardType::CapcomZN1 && offset == CAPCOM_QSOUND_LATCH)
  {
    CapcomQSound::WriteCommand(static_cast<u8>(value));
    return;
  }

  if (!s_runtime->unknown_write_logged)
  {
    DEV_LOG("SonyZN first unhandled EXP3 write offset=0x{:06X} width={} value=0x{:08X}.", offset, width, value);
    s_runtime->unknown_write_logged = true;
  }
}

void GenerateAudioFrame(s32* left, s32* right)
{
  if (s_runtime && s_runtime->board_type == BoardType::AtlusZN1)
  {
    AtlusZN1Sound::GenerateAudioFrame(left, right);
  }
  else if (s_runtime && s_runtime->board_type == BoardType::EightingRaizingZN1)
  {
    EightingRaizingSound::GenerateAudioFrame(left, right);
  }
  else if (s_runtime && s_runtime->board_type == BoardType::BustAMove2ZN1)
  {
    *left = 0;
    *right = 0;

    // The H8/IDE PCM path runs independently of the PSX. The dedicated cabinet's
    // maintained Test switch stops the attract sequence, so freeze PCM consumption
    // while Test is active as well. Muting without freezing would let the music run
    // ahead and leave it out of sync when the game returns to attract mode.
    if (s_runtime->bam2_pcm_playing && !s_runtime->bam2_test_switch_latched)
    {
      s16 pcm_left = 0;
      s16 pcm_right = 0;
      if (ReadBAM2PCMFrame(*s_runtime, &pcm_left, &pcm_right))
      {
        // TC9293 serial attenuator: output amplitude = ATT/127.
        // 0 is mute and 0x7f is 0 dB (Toshiba TC9293F/FN/N Table 7).
        *left = (static_cast<s32>(pcm_left) * s_runtime->bam2_tc9293_attenuation) / 127;
        *right = (static_cast<s32>(pcm_right) * s_runtime->bam2_tc9293_attenuation) / 127;
      }
    }
  }
  else if (s_runtime && s_runtime->board_type == BoardType::TecmoTPS && s_runtime->tecmo_cbaj_sound_enabled)
  {
    TecmoCBAJSound::GenerateAudioFrame(left, right);
  }
  else if (s_runtime && s_runtime->board_type == BoardType::TaitoFX1A)
  {
    TaitoFX1ASound::GenerateAudioFrame(left, right);
  }
  else if (s_runtime && s_runtime->board_type == BoardType::TaitoFX1B)
  {
    TaitoFX1BZoom::GenerateAudioFrame(left, right);
  }
  else if (s_runtime && s_runtime->board_type == BoardType::AcclaimZN1 &&
           s_runtime->acclaim_game == AcclaimZN1Game::NBAJamExtreme)
  {
    AcclaimRAX::GenerateAudioFrame(left, right);
  }
  else
  {
    CapcomQSound::GenerateAudioFrame(left, right);
  }
}

} // namespace SonyZN
