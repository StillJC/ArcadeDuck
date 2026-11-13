// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system12/namco_system12.h"
#include "core/arcade/devices/cpu/h8_3002_timer16.h"
#include "core/arcade/devices/cpu/h8_3002_intc.h"
#include "core/arcade/devices/audio/namco_c352.h"
#include "core/arcade/devices/cpu/h8_3002_peripherals.h"
#include "core/arcade/devices/jvs/jvs_bus.h"
#include "core/arcade/devices/jvs/namco_cyberlead.h"
#include "core/arcade/devices/jvs/namco_fca10.h"
#include "core/arcade/devices/jvs/jvs_chain.h"
#include "core/arcade/devices/jvs/namco_emio102.h"
#include "core/arcade/devices/jvs/namco_empri101.h"
#include "core/arcade/devices/namco/namco_system12_cdxa.h"
#include "core/arcade/devices/namco/namco_system12_hsc.h"

#include "core/arcade/arcade_database.h"
#include "core/arcade/arcade_input.h"
#include "core/arcade/arcade_output.h"
#include "core/arcade/arcade_rom_loader.h"
#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/dma.h"
#include "core/host.h"
#include "core/system.h"
#include "core/timing_event.h"
#include "core/settings.h"
#include "util/cd_image.h"
#include "util/image.h"
#include "util/imgui_manager.h"

#include "fmt/format.h"
#include "imgui.h"

#include "core/arcade/arcade_save_state.h"
#include "core/arcade/devices/cpu/h8_300h_state.h"

extern "C" {
#include "core/arcade/third_party/h8_300h/system.h"
}

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"

#include "util/state_wrapper.h"
#include "common/path.h"
#include "common/sha1_digest.h"
#include "common/string_util.h"
#include "common/timer.h"

#ifdef _WIN32
#include "common/windows_headers.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

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

static constexpr u32 GUN_INTERFACE_READ_BASE = 0x780000;
static constexpr u32 GUN_INTERFACE_READ_END = 0x78000f;
static constexpr u32 GUN_INTERFACE_WRITE_BASE = 0x788000;
static constexpr u32 GUN_INTERFACE_WRITE_END = 0x788003;
static constexpr u32 GUN_INTERFACE_MIRROR = 0x800000;

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
static constexpr size_t EEPROM_ID_SIZE = 0x20;
static constexpr size_t EEPROM_PERSIST_SIZE = EEPROM_SIZE + EEPROM_ID_SIZE;

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

// The H8 I/O firmware debounces Service over about 14 polls, and several titles poll
// at 30 Hz (about 0.47 s). A brief host press is stretched past that window.
static constexpr u32 SERVICE_MINIMUM_HOLD_MS = 600;

static constexpr u32 H8_CLOCK_HZ = 16934400;
static constexpr u32 CDXA_SH2_CLOCK_HZ = 14745600;
static constexpr u32 CDXA_SH2_SCHEDULER_HZ = 1000;

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


enum class JVSProfile : u8
{
  None,
  CyberLead,
  NamcoFCA10,
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
  std::unique_ptr<Arcade::Namco::System12HSC> hsc;
  bool has_gun_interface = false;
  u8 gun_output_latch = 0;
  bool gun_output_latch_valid = false;
  u32 bank = 0;
  u32 dma_source = 0;
  u64 dma_transfer_count = 0;

  std::unique_ptr<Arcade::Namco::System12CDXA> cdxa;
  std::unique_ptr<TimingEvent> cdxa_timing_event;
  TickCount cdxa_scheduler_interval_ticks = 1;
  u32 cdxa_cycle_fraction = 0;
  u64 cdxa_scheduler_count = 0;

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

  u8 h8_jvs_sense = H8_JVS_SENSE_NONE;
  bool h8_jvs_rts = false;
  Arcade::JVS::Bus h8_jvs_bus;
  Arcade::JVS::NamcoCyberLead h8_cyberlead;
  bool cyberlead_led_enabled = false;
  u64 h8_cyberlead_deferred_clocks = 0;
  Arcade::JVS::NamcoFCA10 h8_fca10;
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

  bool buttons_on_direction_lines = false;
  bool test_switch_maintained = false;
  bool test_switch_latched = false;
  bool test_switch_was_pressed = false;

  bool h8_fault_logged = false;
  std::string persistence_directory;
  std::string eeprom_path;
  Arcade::SaveState::PersistentFile eeprom_file;
  bool eeprom_dirty = false;

  Arcade::H8::H83002Timer16 h8_timer16;

  Arcade::H8::H83002Peripherals h8_peripherals;

  Arcade::H8::H83002Intc h8_intc;
  Arcade::Audio::NamcoC352 c352;
  u32 h8_c352_instruction_clocks = 0;
  u64 h8_c352_deferred_clocks = 0;
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

bool UsesCDXA(const LoadedContent& content)
{
  return content.machine_config == "cdxa_pcb" ||
         content.machine_config == "truckk" ||
         content.state_class == "namcos12_cdxa_state" ||
         content.state_class == "truckk_state";
}

bool UsesCDXA(const RuntimeState& runtime)
{
  return UsesCDXA(runtime.content);
}

bool IsM8F4(const RuntimeState& runtime)
{
  return runtime.content.rom_board_profile == ROMBoardProfile::M8F4Protected;
}

bool HasMaintainedTestSwitch(const LoadedContent& content)
{
  return content.machine_config == "truckk" || content.machine_config == "cdxa_pcb" ||
         content.machine_config == "golgo13" || content.machine_config == "kartduel";
}

bool HasGunInterface(const LoadedContent& content)
{
  return content.state_class == "ptblank2_state";
}

bool HasLinkBoard(const LoadedContent& content)
{
  return content.machine_config == "kartduel";
}

bool ReadH8TestSwitch(RuntimeState& runtime)
{
  const bool pressed = ArcadeInput::IsOperatorPressed("Test");
  if (!runtime.test_switch_maintained)
    return pressed;

  if (pressed && !runtime.test_switch_was_pressed)
    runtime.test_switch_latched = !runtime.test_switch_latched;
  runtime.test_switch_was_pressed = pressed;
  return runtime.test_switch_latched;
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

static std::array<u8, EEPROM_PERSIST_SIZE> s_eeprom{};

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

    if (!data || (data->size() != EEPROM_SIZE && data->size() != EEPROM_PERSIST_SIZE))
    {
      const size_t actual_size = data ? data->size() : 0;
      Error::SetStringFmt(error,
                          "Namco System 12 AT28C16 '{}' has invalid size {}; expected {} or {} bytes.",
                          runtime.eeprom_path, actual_size, EEPROM_SIZE, EEPROM_PERSIST_SIZE);
      ERROR_LOG("System12 AT28C16 malformed persistence path='{}' actual_size={} expected_size={} or {}",
                runtime.eeprom_path, actual_size, EEPROM_SIZE, EEPROM_PERSIST_SIZE);
      return false;
    }

    std::memcpy(s_eeprom.data(), data->data(), data->size());
    if (data->size() == EEPROM_SIZE)
      runtime.eeprom_dirty = true;
    runtime.eeprom_file.SetPath(runtime.eeprom_path);
    runtime.eeprom_file.RecordDiskContents(data->cspan());
  }
  else
  {
    if (runtime.content.set_name == "kartduel")
    {
      // Preserve the initialized settings block together with the steering calibration.
      static constexpr size_t KART_DUEL_DEFAULTS_OFFSET = 0x0140;
      static constexpr std::array<u8, 0xBC> KART_DUEL_DEFAULTS = {{
        0x02, 0x01, 0x00, 0xFB, 0x00, 0x14, 0x05, 0x03, 0x03, 0x02, 0x06, 0x04,
        0x04, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
        0x02, 0x02, 0x98, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0xFF, 0x20,
        0x26, 0x11, 0x11, 0x03, 0x23, 0x03, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xC8, 0x6D, 0x01, 0xFD, 0x02, 0x02, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x5F, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x4B, 0x44, 0x32, 0x01, 0x01, 0x01, 0x06, 0x04, 0x04, 0x02, 0x02,
        0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      }};
      static_assert(KART_DUEL_DEFAULTS_OFFSET + KART_DUEL_DEFAULTS.size() <= EEPROM_SIZE);
      std::memcpy(s_eeprom.data() + KART_DUEL_DEFAULTS_OFFSET, KART_DUEL_DEFAULTS.data(), KART_DUEL_DEFAULTS.size());
    }
    else if (runtime.content.set_name == "technodr")
    {
      // Default settings and calibration captured before starting a course.
      static constexpr std::array<u8, 0x250> TECHNO_DRIVE_DEFAULTS = {{
        0x4D, 0x31, 0x33, 0x38, 0x4A, 0x50, 0x31, 0x00, 0x00, 0x00, 0x05, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5E, 0x40, 0x12,
        0xC0, 0x81, 0x00, 0x00, 0x00, 0x00, 0xB0, 0x03, 0x5E, 0x2A, 0x2A, 0x00,
        0x01, 0x02, 0x4A, 0x24, 0x47, 0x4E, 0x54, 0x03, 0x04, 0x05, 0xEC, 0x22,
        0x45, 0x44, 0x40, 0x06, 0x07, 0x08, 0x8E, 0x21, 0x43, 0x53, 0x5D, 0x09,
        0x0A, 0x0B, 0x30, 0x20, 0x45, 0x4D, 0x5D, 0x0C, 0x0D, 0x0E, 0xD2, 0x1E,
        0x37, 0x36, 0x35, 0x00, 0x01, 0x02, 0x74, 0x1D, 0x48, 0x4F, 0x49, 0x03,
        0x04, 0x05, 0x16, 0x1C, 0x5A, 0x49, 0x4E, 0x06, 0x07, 0x08, 0xB8, 0x1A,
        0x47, 0x45, 0x52, 0x09, 0x0A, 0x0B, 0x5A, 0x19, 0x54, 0x44, 0x23, 0x0C,
        0x0D, 0x0E, 0x5A, 0x19, 0x5E, 0x2A, 0x2A, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x43, 0x53, 0x5D, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x45, 0x4D, 0x5D, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x5E, 0x2A, 0x2A, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x43, 0x53, 0x5D, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x45, 0x4D, 0x5D, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x5E, 0x2A, 0x2A, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x43, 0x53, 0x5D, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x45, 0x4D, 0x5D, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x45, 0x44, 0x40, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x4B, 0x2E, 0x49, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x54, 0x59, 0x5A, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x45, 0x44, 0x40, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x4B, 0x2E, 0x49, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x54, 0x59, 0x5A, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x45, 0x44, 0x40, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x4B, 0x2E, 0x49, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x54, 0x59, 0x5A, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x54, 0x44, 0x23, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x4E, 0x41, 0x49, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x4E, 0x45, 0x4C, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x54, 0x44, 0x23, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x4E, 0x41, 0x49, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x4E, 0x45, 0x4C, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x54, 0x44, 0x23, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x4E, 0x41, 0x49, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x4E, 0x45, 0x4C, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x37, 0x36, 0x35, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x4D, 0x2E, 0x4E, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x55, 0x4D, 0x41, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x37, 0x36, 0x35, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x4D, 0x2E, 0x4E, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x55, 0x4D, 0x41, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x37, 0x36, 0x35, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x4D, 0x2E, 0x4E, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x55, 0x4D, 0x41, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x48, 0x4F, 0x49, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x5A, 0x49, 0x4E, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x47, 0x45, 0x52, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x48, 0x4F, 0x49, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x5A, 0x49, 0x4E, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x47, 0x45, 0x52, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0x48, 0x4F, 0x49, 0x00, 0x4A, 0x24, 0x00, 0x00,
        0x5A, 0x49, 0x4E, 0x00, 0x8E, 0x21, 0x00, 0x00, 0x47, 0x45, 0x52, 0x00,
        0xE2, 0x1D, 0x00, 0x00, 0xEE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
      }};
      static_assert(TECHNO_DRIVE_DEFAULTS.size() <= EEPROM_SIZE);
      std::memcpy(s_eeprom.data(), TECHNO_DRIVE_DEFAULTS.data(), TECHNO_DRIVE_DEFAULTS.size());
    }

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
    runtime.eeprom_file.SetPath(runtime.eeprom_path);
    runtime.eeprom_file.RecordDiskContents(s_eeprom);
  }

  INFO_LOG("System12 AT28C16 {} set='{}' size={} path='{}'",
           persistence_exists ? "loaded" : "initialized",
           runtime.content.set_name, EEPROM_PERSIST_SIZE, runtime.eeprom_path);
  return true;
}

void SaveEEPROM(RuntimeState& runtime)
{
  if (!runtime.eeprom_dirty || Arcade::SaveState::IsRestoring())
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

  runtime.eeprom_file.RecordDiskContents(s_eeprom);
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
      if (eeprom_offset < EEPROM_SIZE)
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
    if (eeprom_offset >= EEPROM_SIZE)
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

static constexpr u32 TECHNODR_PRINT_WIDTH = 576;
static constexpr u32 TECHNODR_PRINT_CELL_WIDTH = 12;
static constexpr u32 TECHNODR_PRINT_LINE_HEIGHT = 24;
static constexpr u32 TECHNODR_PRINT_MARGIN = 12;

bool DecodeTechnoDrivePrintCharacter(std::span<const u8> data, size_t* offset, ImWchar* codepoint, u32* cells)
{
  if (*offset >= data.size())
    return false;

  const u8 first = data[*offset];
  size_t length = 1;
  if ((first >= UINT8_C(0x81) && first <= UINT8_C(0x9F)) ||
      (first >= UINT8_C(0xE0) && first <= UINT8_C(0xFC)))
  {
    if ((*offset + 1) >= data.size())
      return false;
    length = 2;
  }

#ifdef _WIN32
  wchar_t wide = L'?';
  const int converted = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
                                             reinterpret_cast<const char*>(data.data() + *offset),
                                             static_cast<int>(length), &wide, 1);
  *codepoint = static_cast<ImWchar>(converted == 1 ? wide : L'?');
#else
  *codepoint = static_cast<ImWchar>((length == 1 && first < UINT8_C(0x80)) ? first : UINT8_C('?'));
#endif

  *cells = (length == 2) ? 2 : 1;
  *offset += length;
  return true;
}

void DrawTechnoDrivePrintGlyph(std::vector<u32>& page, u32 page_height, const unsigned char* atlas_pixels,
                               int atlas_width, int atlas_height, const ImFont* font, ImWchar codepoint,
                               u32 x, u32 y, u32 scale = 1, bool white = false)
{
  const ImFontGlyph* const glyph = font->FindGlyph(codepoint);
  if (!glyph)
    return;

  const int sx0 = static_cast<int>(glyph->U0 * atlas_width + 0.5f);
  const int sy0 = static_cast<int>(glyph->V0 * atlas_height + 0.5f);
  const int sx1 = static_cast<int>(glyph->U1 * atlas_width + 0.5f);
  const int sy1 = static_cast<int>(glyph->V1 * atlas_height + 0.5f);
  const int dx0 = static_cast<int>(x) + static_cast<int>(glyph->X0 * scale);
  const int dy0 = static_cast<int>(y) + static_cast<int>(glyph->Y0 * scale);
  const int dx1 = static_cast<int>(x) + static_cast<int>(glyph->X1 * scale);
  const int dy1 = static_cast<int>(y) + static_cast<int>(glyph->Y1 * scale);
  if (sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0)
    return;

  for (int dy = std::max(0, dy0); dy < std::min<int>(dy1, static_cast<int>(page_height)); dy++)
  {
    const int sy = sy0 + (dy - dy0) * (sy1 - sy0) / (dy1 - dy0);
    if (sy < 0 || sy >= atlas_height)
      continue;
    for (int dx = std::max(0, dx0); dx < std::min<int>(dx1, TECHNODR_PRINT_WIDTH); dx++)
    {
      const int sx = sx0 + (dx - dx0) * (sx1 - sx0) / (dx1 - dx0);
      if (sx < 0 || sx >= atlas_width)
        continue;
      const u32 alpha = std::max<u32>(atlas_pixels[sy * atlas_width + sx],
                                     atlas_pixels[sy * atlas_width + std::max(sx0, sx - 1)]);
      u32& pixel = page[static_cast<size_t>(dy) * TECHNODR_PRINT_WIDTH + dx];
      const u32 shade = ((pixel & 0xFF) * (255 - alpha) + (white ? 255 : 0) * alpha) / 255;
      pixel = UINT32_C(0xFF000000) | shade | (shade << 8) | (shade << 16);
    }
  }
}

bool ReadTechnoDrivePrintCharacter(std::span<const u8> job, size_t* offset, ImWchar* codepoint, u32* cells,
                                  std::array<u8, 3>* control = nullptr)
{
  while (*offset < job.size())
  {
    const u8 value = job[*offset];
    if (value == UINT8_C(0x0D))
    {
      (*offset)++;
      continue;
    }
    if (value == UINT8_C(0x0A))
    {
      (*offset)++;
      *codepoint = static_cast<ImWchar>('\n');
      *cells = 0;
      return true;
    }
    if (value != UINT8_C(0x1B))
      return DecodeTechnoDrivePrintCharacter(job, offset, codepoint, cells);

    if ((job.size() - *offset) < 2)
      return false;
    const u8 command = job[*offset + 1];
    *offset += 2;
    if (command == UINT8_C(0x10) || command == UINT8_C(0x11) || command == UINT8_C(0x12))
    {
      if ((job.size() - *offset) < 2)
        return false;
      const std::array<u8, 3> parameters = {command, job[*offset], job[*offset + 1]};
      *offset += 2;
      if (control)
      {
        *control = parameters;
        *codepoint = 0;
        *cells = 0;
        return true;
      }
    }
    else if (command == UINT8_C(0x15))
    {
      if (*offset >= job.size())
        return false;
      const u8 parameter = job[(*offset)++];
      if (control)
      {
        *control = {command, parameter, 0};
        *codepoint = 0;
        *cells = 0;
        return true;
      }
    }
    else if (command == UINT8_C(0x13))
    {
      if (*offset >= job.size())
        return false;
      const u8 glyph = job[(*offset)++];
      if (control)
        *control = {command, glyph, 0};
      *cells = 1;
      if (glyph >= UINT8_C(0x08) && glyph <= UINT8_C(0x11))
        *codepoint = static_cast<ImWchar>('0' + glyph - UINT8_C(0x08));
      else if (glyph >= UINT8_C(0x18) && glyph <= UINT8_C(0x1C))
        *codepoint = static_cast<ImWchar>('A' + glyph - UINT8_C(0x18));
      else
      {
        // Custom printer glyphs use host-font substitutes.
        switch (glyph)
        {
          case UINT8_C(0x01): *codepoint = static_cast<ImWchar>(0x25A0); break;
          case UINT8_C(0x12): *codepoint = static_cast<ImWchar>('.'); break;
          case UINT8_C(0x13): *codepoint = static_cast<ImWchar>('['); break;
          case UINT8_C(0x14): *codepoint = static_cast<ImWchar>(']'); break;
          case UINT8_C(0x15): *codepoint = static_cast<ImWchar>('%'); break;
          case UINT8_C(0x16): *codepoint = static_cast<ImWchar>('+'); break;
          case UINT8_C(0x17): *codepoint = static_cast<ImWchar>('-'); break;
          default: *codepoint = static_cast<ImWchar>('?'); break;
        }
      }
      return true;
    }
  }
  return false;
}

struct TechnoDrivePrintImage
{
  u32 width = 0;
  u32 height = 0;
  std::vector<u16> pixels;
};

bool LoadTechnoDrivePrintTIM(std::span<const u8> rom, size_t offset, TechnoDrivePrintImage* image)
{
  const auto read16 = [&rom](size_t p) { return static_cast<u16>(rom[p] | (static_cast<u16>(rom[p + 1]) << 8)); };
  const auto read32 = [&read16](size_t p) { return static_cast<u32>(read16(p)) | (static_cast<u32>(read16(p + 2)) << 16); };
  if (offset > rom.size() || (rom.size() - offset) < 8 || read32(offset) != 0x10)
    return false;
  const u32 flags = read32(offset + 4);
  const u32 mode = flags & 3;
  if ((flags & ~UINT32_C(0x0B)) != 0 || mode == 3 || (mode < 2 && (flags & 8) == 0))
    return false;
  offset += 8;
  size_t palette_offset = 0;
  if ((flags & 8) != 0)
  {
    if ((rom.size() - offset) < 12)
      return false;
    const u32 size = read32(offset);
    const u32 colors = (mode == 0) ? 16 : 256;
    if (size < (12 + colors * 2) || size > (rom.size() - offset))
      return false;
    palette_offset = offset + 12;
    offset += size;
  }
  if ((rom.size() - offset) < 12)
    return false;
  const u32 size = read32(offset);
  const u32 words = read16(offset + 8);
  const u32 height = read16(offset + 10);
  if (words == 0 || words > 1024 || height == 0 || height > 1024 ||
      size != (12 + words * height * 2) || size > (rom.size() - offset))
    return false;
  const u32 width = words * ((mode == 0) ? 4 : (mode == 1) ? 2 : 1);
  image->width = width;
  image->height = height;
  image->pixels.resize(static_cast<size_t>(width) * height);
  for (u32 y = 0; y < height; y++)
  {
    for (u32 x = 0; x < width; x++)
    {
      const size_t row = offset + 12 + static_cast<size_t>(y) * words * 2;
      u16 color;
      if (mode == 0)
      {
        const u8 index = (rom[row + x / 2] >> ((x & 1) * 4)) & 15;
        color = read16(palette_offset + index * 2);
      }
      else if (mode == 1)
      {
        color = read16(palette_offset + rom[row + x] * 2);
      }
      else
      {
        color = read16(row + x * 2);
      }
      image->pixels[static_cast<size_t>(y) * width + x] = color;
    }
  }
  return true;
}

static constexpr std::array<std::u16string_view, 16> TECHNODR_EXERCISES = {
  u"", u"高速適応力", u"度胸", u"スピン走行", u"車幅感覚", u"ジャンプ技術", u"スムーズ走行",
  u"動体視力", u"パワー制御能力", u"連続集中走行", u"決断力", u"状況判断", u"思考リンク走行",
  u"誘導反射", u"反応速度", u"バック走行"};

static constexpr std::array<std::u16string_view, 3> TECHNODR_CATEGORIES = {u"手技", u"足技", u"実技"};

// Advice indices select HLE substitutes; the original printer text bank is unavailable.
static constexpr std::array<std::array<std::u16string_view, 3>, 16> TECHNODR_PRINT_ADVICE = {{
  {u"", u"", u""},
  {u"遠くの進路を見て、次の障害物を早めに見つけよう。",
   u"大きく切り返さず、小さなハンドル操作で進路を修正しよう。",
   u"接触せずに走れる速さから始め、少しずつ速度を上げよう。"},
  {u"芸能界に入り、アッコを敵に回せ。紅白のとき生放送で宣戦布告せよ",
   u"今すぐ銀行に行き、お子様銀行発行紙幣を貯金せよ",
   u"頑固な店主がいるラーメン屋へ行き、大盛りを注文して残せ"},
  {u"車の向きが変わり始めたら、早めにハンドルを戻そう。",
   u"アクセルを踏み続けず、姿勢が乱れたら速度を落とそう。",
   u"同じコーナーを繰り返し走り、切り返すタイミングを覚えよう。"},
  {u"車の左右の端を意識し、通り抜ける場所の中央を狙おう。",
   u"狭い場所では速度を落とし、車幅とすき間を見比べよう。",
   u"入口の手前で向きを整え、通過中の急な操作を減らそう。"},
  {u"踏み切る位置を確認し、毎回同じ場所から跳んでみよう。",
   u"助走の速度をそろえ、着地までの距離を比べよう。",
   u"着地点を先に見て、空中で慌てず姿勢を整えよう。"},
  {u"アクセルもブレーキも少しずつ動かし、急な加減速を減らそう。",
   u"カーブの手前で速度を整え、一定の操作で曲がろう。",
   u"ハンドルを戻すときもゆっくり動かし、車の揺れを抑えよう。"},
  {u"動く目標を目で追い、進む方向を見失わないようにしよう。",
   u"目の前だけでなく、画面全体の動きを確認しよう。",
   u"目標が動いた後の位置を予想して、少し早く操作しよう。"},
  {u"ペダルを踏む量と車の反応を確かめ、必要な力を覚えよう。",
   u"強く踏むことより、狙った量を一定に保つことを意識しよう。",
   u"弱い操作から練習し、目標に近づくまで少しずつ力を加えよう。"},
  {u"次の旗を早めに見つけ、順番に目標を切り替えよう。",
   u"一度のミスを引きずらず、すぐ次の目標に意識を戻そう。",
   u"最後まで同じリズムを保ち、操作が雑にならないようにしよう。"},
  {u"選べる進路を早めに確認し、迷う時間を短くしよう。",
   u"状況が変わったら、前の判断にこだわらず進路を選び直そう。",
   u"判断と操作を分けて練習し、決めてから素早く動かそう。"},
  {u"目標の位置と周囲の動きを確認してから操作を始めよう。",
   u"成功したときの状況を覚え、次の判断に役立てよう。",
   u"一つの場所を見続けず、周囲の変化にも注意を向けよう。"},
  {u"問題を落ち着いて読み、答えを決めてから進路を選ぼう。",
   u"計算中も車の位置を確認し、操作がおろそかにならないようにしよう。",
   u"簡単な問題で操作の流れを覚え、判断と運転をつなげよう。"},
  {u"目標の動きを予想し、追いかけるだけの操作を減らそう。",
   u"狙いが外れたら、小さく修正して中心に戻そう。",
   u"目標を見ることとハンドル操作のタイミングをそろえよう。"},
  {u"合図が出る場所に注意を向け、操作の準備をしておこう。",
   u"合図を確かめてから素早く動き、早すぎる操作を減らそう。",
   u"毎回同じ姿勢で構え、合図から操作までの流れを覚えよう。"},
  {u"後ろ向きの移動とハンドルの関係を、低い速度で確かめよう。",
   u"次に通る場所を先に見て、余裕を持って向きを変えよう。",
   u"大きく切り返さず、少しずつ修正して接触を減らそう。"}
}};

// Transcribed from the supplied courage receipt; its individual variant ID is unknown.
static constexpr std::u16string_view TECHNODR_COURAGE_ANALYSIS =
  u"【度胸に問題あり】\n\n"
  u"他人と比べて、度胸が劣ってますね。やーい、怖がり。"
  u"まあ、車に乗ること自体が最初は怖いものですが、恐怖心があっては何においても上達は望めません。"
  u"まずは、度胸をつけるために“恥ずかしいめ”に遭いましょう。\n"
  u"手始めに、べん髪（ラーメンマンの髪形）にして、珍味の訪問販売でもしてみますか。"
  u"恥ずかしがってはいけません。度胸のある人たち、例えばスタントマンやボクサーは皆こうやって自分に勝ち、"
  u"悟りを開き度胸を身につけていったのです。";

static constexpr std::array<std::u16string_view, 4> TECHNODR_ANALYSIS_TEXT = {
  u"今回の測定では、次の技術を重点的に練習しましょう。\n［",
  u"］\n無理に速さを求めず、正確な操作を安定して続けることを目指しましょう。\n",
  u"今回の測定で力を発揮した技術は［",
  u"］です。得意な操作の感覚を覚え、ほかの課題でも落ち着いて操作しましょう。"};

static constexpr std::u16string_view TECHNODR_GENERIC_ANALYSIS =
  u"測定結果を振り返り、各課題の記録と操作の安定性を確認しましょう。"
  u"得意な技術を生かしながら、苦手な課題を繰り返し練習しましょう。";

std::u16string TechnoDrivePrintAnalysis(u8 group, u8 variant)
{
  u32 exercise = 0;
  if (group == 2 || group == 3 || group == 5 || group == 7)
    exercise = variant;
  else if (group == 6 && variant > 0 && variant <= 45)
    exercise = (variant - 1) / 3 + 1;

  if (exercise == 2 && group == 6)
    return std::u16string(TECHNODR_COURAGE_ANALYSIS);
  if (exercise > 0 && exercise < TECHNODR_EXERCISES.size())
  {
    std::u16string result(TECHNODR_ANALYSIS_TEXT[(group == 5) ? 2 : 0]);
    result += TECHNODR_EXERCISES[exercise];
    result += TECHNODR_ANALYSIS_TEXT[(group == 5) ? 3 : 1];
    if (group != 5)
      result += TECHNODR_PRINT_ADVICE[exercise][(variant - 1) % 3];
    return result;
  }
  return std::u16string(TECHNODR_GENERIC_ANALYSIS);
}

bool RenderTechnoDrivePrintPage(std::span<const u8> job, std::vector<u32>* pixels, u32* page_height)
{
  if (!s_runtime)
    return false;

  ImFontGlyphRangesBuilder range_builder;
  range_builder.AddChar(static_cast<ImWchar>('?'));
  range_builder.AddChar(static_cast<ImWchar>(' '));
  const auto add_text = [&range_builder](std::u16string_view text) {
    for (const char16_t c : text)
      range_builder.AddChar(static_cast<ImWchar>(c));
  };
  for (const auto text : TECHNODR_EXERCISES)
    add_text(text);
  for (const auto text : TECHNODR_CATEGORIES)
    add_text(text);
  for (const auto& exercise : TECHNODR_PRINT_ADVICE)
    for (const auto advice : exercise)
      add_text(advice);
  for (const auto text : TECHNODR_ANALYSIS_TEXT)
    add_text(text);
  add_text(TECHNODR_COURAGE_ANALYSIS);
  add_text(TECHNODR_GENERIC_ANALYSIS);
  add_text(u"テクノドライブ運転技術革命指導書現在の運転技術レベル総合評価解析結果トレーニングメニューnamco");
  size_t offset = 0;
  ImWchar codepoint;
  u32 cells;
  while (ReadTechnoDrivePrintCharacter(job, &offset, &codepoint, &cells))
  {
    if (codepoint != static_cast<ImWchar>('\n'))
      range_builder.AddChar(codepoint);
  }
  ImVector<ImWchar> glyph_ranges;
  range_builder.BuildRanges(&glyph_ranges);
  ImFontAtlas atlas;
  const std::string font_path =
    EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "NotoSansJP-Regular.ttf");
  ImFont* font = atlas.AddFontFromFileTTF(font_path.c_str(), 24.0f, nullptr, glyph_ranges.Data);
  if (!font)
    return false;
  unsigned char* atlas_pixels = nullptr;
  int atlas_width = 0;
  int atlas_height = 0;
  atlas.GetTexDataAsAlpha8(&atlas_pixels, &atlas_width, &atlas_height);
  if (!atlas_pixels || atlas_width <= 0 || atlas_height <= 0)
    return false;

  // TH1/VER.B TIMs: title halves, event-score digits and the split namco wordmark.
  TechnoDrivePrintImage title_left, title_right, digits, zero, wordmark;
  const auto& content = s_runtime->content;
  const auto load_image = [](std::span<const u8> rom, size_t address, u32 width, u32 height,
                             TechnoDrivePrintImage* image) {
    if (!LoadTechnoDrivePrintTIM(rom, address, image) || image->width != width || image->height != height)
    {
      image->pixels.clear();
      return false;
    }
    return true;
  };
  const bool title_left_loaded = load_image(content.banked_rom, 0xC27B2C, 256, 136, &title_left);
  const bool title_right_loaded = load_image(content.banked_rom, 0xC30550, 204, 121, &title_right);
  const bool digits_loaded = load_image(content.program_rom, 0x39510, 204, 210, &digits);
  const bool zero_loaded = load_image(content.program_rom, 0x3E8FC, 68, 70, &zero);
  const bool wordmark_loaded = load_image(content.banked_rom, 0xC0FFA8, 168, 194, &wordmark);
  if (!title_left_loaded || !title_right_loaded || !digits_loaded || !zero_loaded || !wordmark_loaded)
    WARNING_LOG("Techno Drive printer ROM artwork could not be decoded; using text fallbacks");

  static constexpr u32 MAX_HEIGHT = 16384;
  static constexpr u32 LEFT = 24;
  static constexpr u32 RIGHT = TECHNODR_PRINT_WIDTH - LEFT;
  u32 required_height = 0;
  bool valid = true;
  const auto layout = [&](bool draw) {
    u32 x = LEFT;
    u32 y = 12;
    bool score = false;
    u32 score_row = 0;
    u32 score_top = 0;
    bool analysis_section = false;
    bool training_section = false;
    std::array<u8, 16> advice_seen{};
    size_t cursor = 0;
    const auto reserve = [&](u32 bottom) {
      if (bottom > MAX_HEIGHT)
      {
        valid = false;
        return false;
      }
      required_height = std::max(required_height, bottom);
      return true;
    };
    const auto rect = [&](u32 px, u32 py, u32 width, u32 height, u32 color) {
      if (!reserve(py + height) || !draw)
        return;
      for (u32 dy = py; dy < py + height; dy++)
        for (u32 dx = px; dx < std::min<u32>(px + width, TECHNODR_PRINT_WIDTH); dx++)
          (*pixels)[static_cast<size_t>(dy) * TECHNODR_PRINT_WIDTH + dx] = color;
    };
    const auto text = [&](std::u16string_view value, u32 px, u32 py, u32 scale, bool white) {
      if (!reserve(py + TECHNODR_PRINT_LINE_HEIGHT * scale))
        return;
      for (const char16_t c : value)
      {
        if (draw)
          DrawTechnoDrivePrintGlyph(*pixels, *page_height, atlas_pixels, atlas_width, atlas_height,
                                   font, static_cast<ImWchar>(c), px, py, scale, white);
        px += TECHNODR_PRINT_CELL_WIDTH * ((c < 0x100) ? 1 : 2) * scale;
      }
    };
    const auto paragraph = [&](std::u16string_view value) {
      u32 px = LEFT + 16;
      for (const char16_t c : value)
      {
        const u32 width = TECHNODR_PRINT_CELL_WIDTH * ((c < 0x100) ? 1 : 2);
        if (c == u'\n' || px + width > RIGHT - 16)
        {
          y += TECHNODR_PRINT_LINE_HEIGHT;
          px = LEFT + 16;
          if (c == u'\n')
            continue;
        }
        text(std::u16string_view(&c, 1), px, y, 1, false);
        px += width;
      }
      y += TECHNODR_PRINT_LINE_HEIGHT;
      x = LEFT;
    };
    const auto centered = [&](std::u16string_view value, u32 py, bool white) {
      u32 width = 0;
      for (const char16_t c : value)
        width += TECHNODR_PRINT_CELL_WIDTH * ((c < 0x100) ? 1 : 2);
      text(value, (width < TECHNODR_PRINT_WIDTH) ? (TECHNODR_PRINT_WIDTH - width) / 2 : LEFT, py, 1, white);
    };
    const auto bar = [&](std::u16string_view value) {
      rect(LEFT, y, RIGHT - LEFT, 30, UINT32_C(0xFF000000));
      centered(value, y + 2, true);
      y += 36;
      x = LEFT;
    };
    const auto image = [&](const TechnoDrivePrintImage& source, u32 sx, u32 sy, u32 sw, u32 sh,
                           u32 dx, u32 dy, u32 dw, u32 dh, bool mask) {
      if (source.pixels.empty() || sw == 0 || sh == 0 || dw == 0 || dh == 0 ||
          sx > source.width || sw > source.width - sx || sy > source.height || sh > source.height - sy)
        return false;
      if (!reserve(dy + dh) || !draw)
        return true;
      for (u32 py = 0; py < dh; py++)
      {
        for (u32 px = 0; px < dw && dx + px < TECHNODR_PRINT_WIDTH; px++)
        {
          const u16 color = source.pixels[static_cast<size_t>(sy + py * sh / dh) * source.width + sx + px * sw / dw];
          if (color == 0)
            continue;
          const u32 r = color & 31;
          const u32 g = (color >> 5) & 31;
          const u32 b = (color >> 10) & 31;
          const u32 ink = mask ? (std::max({r, g, b}) * 255 / 31) :
                                (255 - (r * 77 + g * 150 + b * 29) * 255 / (31 * 256));
          const u32 shade = 255 - ink;
          (*pixels)[static_cast<size_t>(dy + py) * TECHNODR_PRINT_WIDTH + dx + px] =
            UINT32_C(0xFF000000) | shade | (shade << 8) | (shade << 16);
        }
      }
      return true;
    };

    while (cursor < job.size() && valid)
    {
      std::array<u8, 3> control{};
      ImWchar c;
      u32 character_cells;
      if (!ReadTechnoDrivePrintCharacter(job, &cursor, &c, &character_cells, &control))
        break;
      if (control[0] == UINT8_C(0x12))
      {
        if (control[1] == 1)
        {
          switch (control[2])
          {
            case 1:
              if (!title_left.pixels.empty() && !title_right.pixels.empty())
              {
                image(title_left, 0, 0, 256, 136, 58, y, 256, 136, false);
                image(title_right, 0, 0, 204, 121, 314, y + 15, 204, 121, false);
                y += 144;
              }
              else
              {
                centered(u"テクノドライブ", y, false);
                y += 32;
              }
              centered(u"運転技術革命指導書", y, false);
              y += 30;
              x = LEFT;
              break;
            case 2: bar(u"現在の運転技術レベル"); break;
            case 3:
              score_top = y;
              bar(u"総合評価");
              score = true;
              score_row = 0;
              x = 54;
              break;
            case 4:
              bar(u"解析結果");
              analysis_section = true;
              training_section = false;
              break;
            case 5:
              bar(u"トレーニングメニュー");
              analysis_section = false;
              training_section = true;
              break;
            case 6:
              y += 12;
              if (!wordmark.pixels.empty())
              {
                image(wordmark, 0, 3, 168, 55, 36, y, 252, 83, true);
                image(wordmark, 0, 99, 168, 55, 288, y, 252, 83, true);
              }
              else
              {
                text(u"namco", 198, y, 3, false);
              }
              y += 100;
              x = LEFT;
              break;
            case 8:
              if (score)
              {
                rect(LEFT, score_top + 30, 1, y - score_top - 30, UINT32_C(0xFF000000));
                rect(RIGHT - 1, score_top + 30, 1, y - score_top - 30, UINT32_C(0xFF000000));
                rect(LEFT, y, RIGHT - LEFT, 1, UINT32_C(0xFF000000));
                y += 12;
                score = false;
                x = LEFT;
              }
              break;
            default: break;
          }
        }
        else if (control[1] == 2 && control[2] > 0 && control[2] < TECHNODR_EXERCISES.size())
        {
          const u32 category = (control[2] - 1) % 3;
          rect(LEFT, y, RIGHT - LEFT, 28, UINT32_C(0xFF000000));
          rect(LEFT + 96, y + 2, RIGHT - LEFT - 98, 24, UINT32_C(0xFFFFFFFF));
          text(TECHNODR_CATEGORIES[category], LEFT + 22, y, 1, true);
          text(TECHNODR_EXERCISES[control[2]], LEFT + 110, y, 1, false);
          y += 32;
          x = LEFT;
        }
        continue;
      }
      if (control[0] == UINT8_C(0x11))
      {
        if (analysis_section && control[1] > 0 && control[1] < UINT8_C(0x10))
        {
          paragraph(TechnoDrivePrintAnalysis(control[1], control[2]));
        }
        else if (training_section && control[1] > UINT8_C(0x10) && control[1] <= UINT8_C(0x1F) &&
                 control[2] != 0)
        {
          const u32 exercise = control[1] - UINT8_C(0x10);
          u32 choice = (control[2] - 1) % 3;
          u8& seen = advice_seen[exercise];
          while (seen != 7 && (seen & (1 << choice)) != 0)
            choice = (choice + 1) % 3;
          seen |= static_cast<u8>(1 << choice);
          paragraph(TECHNODR_PRINT_ADVICE[exercise][choice]);
        }
        continue;
      }
      if (control[0] != 0 && control[0] != UINT8_C(0x13))
        continue;
      if (c == static_cast<ImWchar>('\n'))
      {
        y += (score && score_row++ == 0) ? 54 : TECHNODR_PRINT_LINE_HEIGHT;
        x = score ? 42 : LEFT;
        continue;
      }
      if (score && c == static_cast<ImWchar>(0x2503))
        continue;
      if (c == static_cast<ImWchar>(0x2501))
      {
        rect(x, y + 12, character_cells * TECHNODR_PRINT_CELL_WIDTH, 1, UINT32_C(0xFF000000));
        x += character_cells * TECHNODR_PRINT_CELL_WIDTH;
        continue;
      }
      if (score && score_row > 0 && control[0] == UINT8_C(0x13) &&
          control[1] >= 1 && control[1] <= 4)
      {
        // The ROM selects quarter, half and three-quarter symbols before the full ones.
        const u32 fraction = (control[1] == 1) ? 4 : 5 - control[1];
        if (!reserve(y + 20))
          break;
        if (draw)
        {
          for (s32 dy = -8; dy < 8; dy++)
          {
            for (s32 dx = -8; dx < 8; dx++)
            {
              const s32 radius = dx * dx + dy * dy;
              if (radius > 49 || dx + 8 >= static_cast<s32>(fraction * 4))
                continue;
              bool ink = dx >= 0;
              if (dx * dx + (dy + 4) * (dy + 4) <= 16)
                ink = false;
              if (dx * dx + (dy - 4) * (dy - 4) <= 16)
                ink = true;
              if (dx * dx + (dy + 4) * (dy + 4) <= 2)
                ink = true;
              if (dx * dx + (dy - 4) * (dy - 4) <= 2)
                ink = false;
              if (radius >= 36)
                ink = true;
              if (ink && (x + dx + 8) < RIGHT)
                (*pixels)[static_cast<size_t>(y + dy + 12) * TECHNODR_PRINT_WIDTH + x + dx + 8] =
                  UINT32_C(0xFF000000);
            }
          }
        }
        x += 24;
        continue;
      }
      if (score && score_row == 0 && control[0] == UINT8_C(0x13) &&
          control[1] >= UINT8_C(0x08) && control[1] <= UINT8_C(0x11))
      {
        const u32 digit = control[1] - UINT8_C(0x08);
        const TechnoDrivePrintImage& source = (digit == 0) ? zero : digits;
        const u32 sx = (digit == 0) ? 0 : ((digit - 1) % 3) * 68;
        const u32 sy = (digit == 0) ? 0 : ((digit - 1) / 3) * 70;
        if (!image(source, sx, sy, 68, 70, x, y, 37, 40, true))
        {
          const char16_t character = static_cast<char16_t>(c);
          text(std::u16string_view(&character, 1), x, y, 2, false);
        }
        x += 38;
        continue;
      }
      const u32 scale = (score && score_row == 0 && (c == '.' || c == '%')) ? 2 : 1;
      u32 width = character_cells * TECHNODR_PRINT_CELL_WIDTH * scale;
      if (scale > 1)
      {
        const ImFontGlyph* const glyph = font->FindGlyph(c);
        if (glyph)
          width = std::max<u32>(width, static_cast<u32>(glyph->AdvanceX * scale + 0.5f));
      }
      if ((x + width) > RIGHT)
      {
        x = LEFT;
        y += TECHNODR_PRINT_LINE_HEIGHT;
      }
      if (!reserve(y + TECHNODR_PRINT_LINE_HEIGHT * scale))
        break;
      if (draw)
      {
        const u32 text_y = y + ((score && score_row == 0 && scale == 1) ? 14 : 0);
        DrawTechnoDrivePrintGlyph(*pixels, *page_height, atlas_pixels, atlas_width, atlas_height, font, c, x, text_y,
                                 scale, false);
      }
      x += width;
    }
    reserve(y + 32);
  };

  layout(false);
  if (!valid)
    return false;
  *page_height = required_height;
  pixels->assign(static_cast<size_t>(TECHNODR_PRINT_WIDTH) * *page_height, UINT32_C(0xFFFFFFFF));
  layout(true);
  return valid;
}

void SaveTechnoDrivePrintJob(std::span<const u8> job)
{
  std::vector<u32> pixels;
  u32 page_height = 0;
  if (!RenderTechnoDrivePrintPage(job, &pixels, &page_height))
  {
    WARNING_LOG("System12 Techno Drive printer failed to render {} bytes", job.size());
    Host::AddOSDMessage("Techno Drive printer failed to render page.", Host::OSD_ERROR_DURATION);
    return;
  }

  const std::string prints_root = Path::Combine(EmuFolders::DataRoot, "prints");
  const std::string prints_directory = Path::Combine(prints_root, "technodr");
  Error error;
  if (!FileSystem::CreateDirectory(prints_root.c_str(), false, &error) ||
      !FileSystem::CreateDirectory(prints_directory.c_str(), false, &error))
  {
    WARNING_LOG("System12 Techno Drive printer failed to create '{}' ({})", prints_directory,
                error.GetDescription());
    Host::AddOSDMessage("Techno Drive printer failed to create output directory.", Host::OSD_ERROR_DURATION);
    return;
  }

  const s64 timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const std::string filename_base = fmt::format("technodr_{}", timestamp);
  std::string filename = Path::Combine(prints_directory, fmt::format("{}.png", filename_base));
  for (u32 suffix = 1; FileSystem::FileExists(filename.c_str()); suffix++)
    filename = Path::Combine(prints_directory, fmt::format("{}_{}.png", filename_base, suffix));

  RGBA8Image image(TECHNODR_PRINT_WIDTH, page_height, std::move(pixels));
  if (!image.SaveToFile(filename.c_str()))
  {
    WARNING_LOG("System12 Techno Drive printer failed to save '{}'", filename);
    Host::AddOSDMessage("Techno Drive printer failed to save page.", Host::OSD_ERROR_DURATION);
    return;
  }

  INFO_LOG("System12 Techno Drive print saved: {}", filename);
  Host::AddOSDMessage(fmt::format("Print saved: {}", filename), Host::OSD_INFO_DURATION);
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
  runtime.h8_fca10.Reset();
  runtime.h8_jvs_chain.Reset();
  runtime.h8_sci0_ssr_read = 0;
  runtime.h8_sci0_tx_busy = false;
  runtime.h8_sci0_tx_shift_byte = UINT8_C(0xFF);
  runtime.h8_sci0_tx_clocks_remaining = 0;
  runtime.h8_sci0_rx_busy = false;
  runtime.h8_sci0_rx_clocks_remaining = 0;
  jvs.initialize_sense_after_response = false;

  runtime.h8_jvs_sense = HasH8JVSDevice(runtime) ? H8_JVS_SENSE_UNINITIALIZED : H8_JVS_SENSE_NONE;
}






void H8JVSTransmitByte(RuntimeState& runtime, u8 value);

// The LED C77 and the I/O C77 link state machine run in their own clock
// domain; the main H8 only reaches them through JVS packets. Bring them up to
// date before a packet or reset, and at the end of each H8 slice.
void FlushCyberLeadLED(RuntimeState& runtime)
{
  if (runtime.h8_cyberlead_deferred_clocks == 0)
    return;

  runtime.h8_cyberlead.AdvanceLED(static_cast<u32>(runtime.h8_cyberlead_deferred_clocks), H8_CLOCK_HZ);
  runtime.h8_cyberlead_deferred_clocks = 0;
}

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
  FlushCyberLeadLED(runtime);
  JVSState& jvs = runtime.h8_jvs;
  const std::span<const u8> packet = runtime.h8_jvs_bus.GetPacket();

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
  else if (jvs.profile == JVSProfile::NamcoFCA10)
  {
    const Arcade::JVS::NamcoFCA10::PacketResult result =
      runtime.h8_fca10.ProcessPacket(packet);
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

u8 ReadTruckH8JAMMAInput(u32 offset)
{
  // Truck Kyosokyoku uses the motherboard's JAMMA compatibility conversion,
  // not a JVS I/O device. MAME's truckk input profile places the cabinet's
  // music buttons on the logical P2 left/right lines and keeps P2 Button 1
  // asserted as the V233 valve-presence signal.
  u8 value = UINT8_C(0xFF);
  const auto clear_if_pressed = [&value](u8 mask, bool pressed) {
    if (pressed)
      value = static_cast<u8>(value & static_cast<u8>(~mask));
  };

  switch (offset & UINT32_C(3))
  {
    case 0:
      // P2 compatibility byte: B1 is the active-low valve-presence signal.
      value = static_cast<u8>(value & static_cast<u8>(~UINT8_C(0x10)));
      clear_if_pressed(UINT8_C(0x02), ArcadeInput::IsDigitalPressed(0, "MusicPrevious"));
      clear_if_pressed(UINT8_C(0x01), ArcadeInput::IsDigitalPressed(0, "MusicNext"));
      return value;

    case 1:
      // P1 compatibility byte: START/B3/B2/B1/UP/DOWN/LEFT/RIGHT.
      clear_if_pressed(UINT8_C(0x80), ArcadeInput::IsDigitalPressed(0, "View"));
      clear_if_pressed(UINT8_C(0x40), ArcadeInput::IsDigitalPressed(0, "Horn"));
      clear_if_pressed(UINT8_C(0x10), ArcadeInput::IsDigitalPressed(0, "Enter"));
      clear_if_pressed(UINT8_C(0x08), ArcadeInput::IsDigitalPressed(0, "SelectUp"));
      clear_if_pressed(UINT8_C(0x04), ArcadeInput::IsDigitalPressed(0, "SelectDown"));
      return value;

    case 2:
      clear_if_pressed(UINT8_C(0x80), ArcadeInput::IsOperatorPressed("Service"));
      clear_if_pressed(UINT8_C(0x40), ReadH8TestSwitch(*s_runtime));
      clear_if_pressed(UINT8_C(0x20), ArcadeInput::IsDigitalPressed(0, "Coin"));
      return value;

    case 3:
    default:
      return value;
  }
}

u8 ReadH8JAMMAInput(u32 offset)
{
  if (s_runtime && s_runtime->content.set_name == "truckk")
    return ReadTruckH8JAMMAInput(offset);
  // System 12 exposes four active-low JAMMA compatibility bytes to the H8.
  // This matches the motherboard input conversion used when no external JVS
  // I/O board is present, including the extended Button7/Button8 lanes used
  // by titles such as Um Jammer Lammy NOW!.
  const auto player_byte = [](u32 port) {
    u8 result = UINT8_C(0xFF);

    // Um Jammer Lammy NOW! expects the otherwise-unused P2 UP/DOWN lines
    // asserted on the JAMMA compatibility input. MAME models those two
    // inputs as active-low unused lines specifically to clear the game's
    // I/O hardware errors.
    if (s_runtime && s_runtime->content.set_name == "ujlnow" && port == 1)
      result = static_cast<u8>(result & static_cast<u8>(~UINT8_C(0x0C)));

    const auto clear_if_pressed = [&result, port](u8 mask, const char* key) {
      if (ArcadeInput::IsDigitalPressed(port, key))
        result = static_cast<u8>(result & static_cast<u8>(~mask));
    };

    clear_if_pressed(UINT8_C(0x80), "Start");

    if (s_runtime)
    {
      const bool menu_aliases = s_runtime->content.machine_config == "golgo13" ||
                                s_runtime->content.machine_config == "kartduel";
      if (menu_aliases && port == 0)
      {
        if (ArcadeInput::IsDigitalPressed(port, "SelectUp"))
          result = static_cast<u8>(result & static_cast<u8>(~UINT8_C(0x08)));
        if (ArcadeInput::IsDigitalPressed(port, "SelectDown"))
          result = static_cast<u8>(result & static_cast<u8>(~UINT8_C(0x04)));
        if (ArcadeInput::IsDigitalPressed(port, "Enter"))
          result = static_cast<u8>(result & static_cast<u8>(~UINT8_C(0x10)));
      }

      if (s_runtime->content.machine_config == "golgo13" && port == 0 &&
          ArcadeInput::IsDigitalPressed(port, "Trigger"))
      {
        result = static_cast<u8>(result & static_cast<u8>(~UINT8_C(0x20)));
      }

      if (s_runtime->has_gun_interface && ArcadeInput::IsDigitalPressed(port, "Trigger"))
        result = static_cast<u8>(result & static_cast<u8>(~UINT8_C(0x10)));
    }

    // Quiz and Paca Paca panels wire their four buttons to the direction lines.
    if (s_runtime && s_runtime->buttons_on_direction_lines)
    {
      clear_if_pressed(UINT8_C(0x08), "Button1");
      clear_if_pressed(UINT8_C(0x04), "Button2");
      clear_if_pressed(UINT8_C(0x02), "Button3");
      clear_if_pressed(UINT8_C(0x01), "Button4");
      return result;
    }

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
      clear_if_pressed(UINT8_C(0x40), s_runtime && ReadH8TestSwitch(*s_runtime));
      clear_if_pressed(UINT8_C(0x20), ArcadeInput::IsDigitalPressed(0, "Coin"));
      clear_if_pressed(UINT8_C(0x10), ArcadeInput::IsDigitalPressed(1, "Coin"));
      clear_if_pressed(UINT8_C(0x08), ArcadeInput::IsDigitalPressed(0, "Button7") ||
                                           (s_runtime && s_runtime->content.machine_config == "golgo13" &&
                                            ArcadeInput::IsDigitalPressed(0, "Sensor")));
      clear_if_pressed(UINT8_C(0x04), ArcadeInput::IsDigitalPressed(1, "Button7"));
      clear_if_pressed(UINT8_C(0x02), ArcadeInput::IsDigitalPressed(0, "Button8"));
      clear_if_pressed(UINT8_C(0x01), ArcadeInput::IsDigitalPressed(1, "Button8"));
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

void H8AdvanceC352ToCurrentInstructionCycle(RuntimeState& runtime)
{
  if (!runtime.h8_cpu)
    return;

  const u32 current_clocks =
    (runtime.h8_cpu->cycles > 0) ? static_cast<u32>(runtime.h8_cpu->cycles) : 0;
  const u32 instruction_delta =
    (current_clocks > runtime.h8_c352_instruction_clocks) ?
      (current_clocks - runtime.h8_c352_instruction_clocks) : 0;

  const u64 clocks = runtime.h8_c352_deferred_clocks + instruction_delta;
  if (clocks == 0)
    return;

  runtime.c352.AdvanceH8Clocks(
    clocks, std::span<const u8>(runtime.content.c352_samples));
  runtime.h8_c352_deferred_clocks = 0;
  runtime.h8_c352_instruction_clocks = current_clocks;
}

u16 GetTruckADCValue(u32 channel)
{
  // Truck uses the full 10-bit ADC range: steering on ADC0, brake on ADC1,
  // and accelerator on ADC2. The two pedal channels are electrically reversed.
  float normalized = 0.0f;
  bool reverse = false;

  switch (channel)
  {
    case 0:
      normalized = ArcadeInput::GetAnalogValue(0, "Steering");
      break;

    case 1:
      normalized = ArcadeInput::GetAnalogValue(0, "Brake");
      reverse = true;
      break;

    case 2:
      normalized = ArcadeInput::GetAnalogValue(0, "Accelerator");
      reverse = true;
      break;

    default:
      return UINT16_C(0x03FF);
  }

  normalized = std::clamp(normalized, 0.0f, 1.0f);
  if (reverse)
    normalized = 1.0f - normalized;

  return static_cast<u16>((normalized * static_cast<float>(UINT16_C(0x03FF))) + 0.5f);
}

u16 GetAplaRailLeverValue()
{
  // ADC0 lever range from the cabinet input definition, reversed around the host accelerator axis.
  constexpr u16 LEVER_MIN = UINT16_C(0x08B);
  constexpr u16 LEVER_MAX = UINT16_C(0x397);

  const float position = 1.0f - std::clamp(ArcadeInput::GetAnalogValue(0, "Accelerator"), 0.0f, 1.0f);
  return static_cast<u16>(LEVER_MIN + (position * static_cast<float>(LEVER_MAX - LEVER_MIN)) + 0.5f);
}

u16 GetKartDuelADCValue(u32 channel)
{
  float normalized = 0.0f;
  u16 minimum = 0;
  u16 maximum = 0;

  switch (channel)
  {
    case 0:
      normalized = ArcadeInput::GetAnalogValue(0, "Brake");
      minimum = UINT16_C(0x000);
      maximum = UINT16_C(0x15A);
      break;

    case 1:
      normalized = ArcadeInput::GetAnalogValue(0, "Accelerator");
      minimum = UINT16_C(0x000);
      maximum = UINT16_C(0x15A);
      break;

    case 2:
      normalized = ArcadeInput::GetAnalogValue(0, "Steering");
      minimum = UINT16_C(0x0E0);
      maximum = UINT16_C(0x320);
      break;

    default:
      return UINT16_C(0x03FF);
  }

  normalized = std::clamp(normalized, 0.0f, 1.0f);
  return static_cast<u16>(minimum +
                          (normalized * static_cast<float>(maximum - minimum)) + 0.5f);
}

u16 GetGolgoADCValue(u32 channel)
{
  const ArcadeInput::LightgunPresentationState state = ArcadeInput::GetLightgunPresentationState();

  switch (channel)
  {
    case 1:
    {
      const float x = std::clamp(state.ports[0].x, 0.0f, 1.0f);
      return static_cast<u16>(UINT16_C(0x07A) + (x * static_cast<float>(UINT16_C(0x240))) + 0.5f);
    }

    case 2:
    {
      const float y = 1.0f - std::clamp(state.ports[0].y, 0.0f, 1.0f);
      return static_cast<u16>(UINT16_C(0x01F) + (y * static_cast<float>(UINT16_C(0x1BF))) + 0.5f);
    }

    default:
      return UINT16_C(0x03FF);
  }
}

void UpdateADCInputs(RuntimeState& runtime)
{
  if (runtime.content.set_name == "truckk")
  {
    for (u32 channel = 0; channel < 3; channel++)
      runtime.h8_peripherals.SetADCInput(channel, GetTruckADCValue(channel));
  }
  else if (runtime.content.machine_config == "aplarail")
  {
    runtime.h8_peripherals.SetADCInput(0, GetAplaRailLeverValue());
  }
  else if (runtime.content.machine_config == "kartduel")
  {
    for (u32 channel = 0; channel < 3; channel++)
      runtime.h8_peripherals.SetADCInput(channel, GetKartDuelADCValue(channel));
  }
  else if (runtime.content.machine_config == "golgo13")
  {
    runtime.h8_peripherals.SetADCInput(1, GetGolgoADCValue(1));
    runtime.h8_peripherals.SetADCInput(2, GetGolgoADCValue(2));
  }
}

u8 ReadTruckADCRegister(u16 reg)
{
  const u32 channel = static_cast<u32>((reg - UINT16_C(0xFFE0)) >> 1);
  const u16 adc = GetTruckADCValue(channel);

  // H8 ADC 8-bit register reads expose the 10-bit conversion result as
  // ADDRxH = result >> 2 and ADDRxL = result << 6.
  return ((reg & UINT16_C(1)) != 0) ?
    static_cast<u8>(adc << 6) :
    static_cast<u8>(adc >> 2);
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


    return TRUE;
  }

  if (address >= H8_C352_BASE && address <= H8_C352_END)
  {
    // MAME updates the C352 stream at the H8 register-access boundary.
    // Bring only the C352 forward through the cycles already consumed by
    // this instruction before performing the register read.
    H8AdvanceC352ToCurrentInstructionCycle(*runtime);
    value->u = runtime->c352.ReadH8Byte(static_cast<u32>(address - H8_C352_BASE));
    return TRUE;
  }

  if (address >= H8_JAMMA_BASE && address <= H8_JAMMA_END)
  {
    const u32 jamma_offset = static_cast<u32>(address - H8_JAMMA_BASE);
    value->u = (runtime->h8_jvs.profile == JVSProfile::None) ? ReadH8JAMMAInput(jamma_offset) : UINT8_C(0xFF);

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
        value->u =
          (runtime->content.set_name == "truckk") ? ReadTruckADCRegister(reg) : UINT8_C(0xFF);
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
    const u32 shared_storage_index = offset ^ UINT32_C(1);


    runtime->shared_ram[shared_storage_index] = value.u;

    return TRUE;
  }

  if (address >= H8_C352_BASE && address <= H8_C352_END)
  {
    // MAME updates the C352 stream at the H8 register-access boundary.
    // Bring only the C352 forward through the cycles already consumed by
    // this instruction before performing the register write.
    H8AdvanceC352ToCurrentInstructionCycle(*runtime);
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
    if (reg == UINT16_C(0xFFE8))
      UpdateADCInputs(*runtime);

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

// The two helpers below run after every H8 instruction.
ALWAYS_INLINE void H8CollectInterruptSources(RuntimeState& runtime)
{
  // External IRQ0-7 are already represented directly inside h8_intc.
  // Drain peripheral-local event latches into the single H8H INTC pending set.
  if (runtime.h8_timer16.HasPendingInterrupt())
  {
    while (const std::optional<u32> vector = runtime.h8_timer16.TakePendingInterrupt())
      runtime.h8_intc.InternalInterrupt(*vector);
  }

  if (runtime.h8_peripherals.HasPendingInterrupt())
  {
    while (const std::optional<u32> vector = runtime.h8_peripherals.TakePendingInterrupt())
      runtime.h8_intc.InternalInterrupt(*vector);
  }
}

ALWAYS_INLINE void H8AdvanceClockedDevices(RuntimeState& runtime, u32 h8_clocks, bool advance_c352)
{
  if (h8_clocks == 0)
    return;

  runtime.h8_timer16.Advance(h8_clocks);
  runtime.h8_peripherals.Advance(h8_clocks);
  if (runtime.h8_sci0_tx_busy || runtime.h8_sci0_rx_busy)
    H8SCI0CyberLeadAdvance(runtime, h8_clocks);
  if (runtime.h8_sci1_rx_busy)
    H8SCI1Advance(runtime, h8_clocks);
  if (runtime.h8_jvs.profile == JVSProfile::CyberLead)
    runtime.h8_cyberlead_deferred_clocks += h8_clocks;

  if (advance_c352)
    runtime.c352.AdvanceH8Clocks(h8_clocks, std::span<const u8>(runtime.content.c352_samples));
}

u32 H8ServiceInterrupts(RuntimeState& runtime)
{
  if (!runtime.h8_cpu)
    return 0;

  if (runtime.h8_intc.GetPendingMask() == 0)
    return 0;

  h8_system_t& h8 = *runtime.h8_cpu;
  const std::optional<u32> vector = runtime.h8_intc.PeekNext(
    h8.cpu.ccr.flags.i != 0, h8.cpu.ccr.flags.ui != 0, runtime.h8_peripherals.SYSCR());
  if (!vector)
    return 0;


  h8.cycles = 0;
  if (!h8_interrupt(&h8, *vector))
    return 0;

  // H83002 irq_setup(): SYSCR bit3=0 sets I+UI; bit3=1 sets I only.
  if ((runtime.h8_peripherals.SYSCR() & UINT8_C(0x08)) == 0)
    h8.cpu.ccr.flags.ui = 1;

  runtime.h8_intc.Acknowledge(*vector);
  runtime.h8_internal[UINT16_C(0xFFF6)] = runtime.h8_intc.GetISR();

  return (h8.cycles > 0) ? static_cast<u32>(h8.cycles) : 0;
}

void CDXASH2SchedulerCallback(void*, TickCount ticks, TickCount /*ticks_late*/)
{
  if (!s_runtime || !s_runtime->cdxa || !s_runtime->cdxa->IsSH2Running())
    return;

  RuntimeState& runtime = *s_runtime;
  runtime.cdxa_scheduler_count++;

  const u32 elapsed_events = std::max<u32>(1, static_cast<u32>(
    (static_cast<u64>(ticks) + static_cast<u64>(runtime.cdxa_scheduler_interval_ticks) - 1) /
    static_cast<u64>(runtime.cdxa_scheduler_interval_ticks)));
  const u32 bounded_events = std::min<u32>(elapsed_events, 8);

  const u64 cycle_numerator =
    (static_cast<u64>(CDXA_SH2_CLOCK_HZ) * bounded_events) + runtime.cdxa_cycle_fraction;
  const u64 cycles = cycle_numerator / CDXA_SH2_SCHEDULER_HZ;
  runtime.cdxa_cycle_fraction = static_cast<u32>(cycle_numerator % CDXA_SH2_SCHEDULER_HZ);

  if (cycles != 0)
    runtime.cdxa->Run(cycles);
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

  const u32 max_catchup_events =
    (runtime.h8_scheduler_rate_hz == 1000) ?
      8 : std::max<u32>(1, (runtime.h8_scheduler_rate_hz * 8) / 1000);
  const u64 ticks_per_second =
    static_cast<u64>(std::max<TickCount>(System::GetTicksPerSecond(), 1));

  // RTC follows elapsed emulated time, including time beyond the bounded H8
  // execution catch-up window.
  const u64 rtc_numerator =
    (static_cast<u64>(ticks) * 1000) + runtime.h8_rtc_scheduler_fraction;
  const u32 elapsed_milliseconds =
    static_cast<u32>(rtc_numerator / ticks_per_second);
  runtime.h8_rtc_scheduler_fraction =
    static_cast<u32>(rtc_numerator % ticks_per_second);
  if (elapsed_milliseconds != 0)
    AdvanceH8RTC(runtime, elapsed_milliseconds);

  // InvokeEarly() can split one scheduler interval into multiple callbacks.
  // Convert the actual elapsed ticks to H8 clocks instead of rounding every
  // callback up to a full scheduler quantum.
  const u64 max_catchup_ticks =
    static_cast<u64>(runtime.h8_scheduler_interval_ticks) * max_catchup_events;
  const u64 bounded_ticks = std::min<u64>(static_cast<u64>(ticks), max_catchup_ticks);
  const u64 cycle_numerator =
    (static_cast<u64>(H8_CLOCK_HZ) * bounded_ticks) + runtime.h8_cycle_fraction;
  runtime.h8_cycle_balance += static_cast<s64>(cycle_numerator / ticks_per_second);
  runtime.h8_cycle_fraction = static_cast<u32>(cycle_numerator % ticks_per_second);

  u32 h8_last_step_pc = static_cast<u32>(h8.cpu.pc);
  while (runtime.h8_cycle_balance > 0 && h8.error_code == H8_DEBUG_NO_ERROR)
  {
    h8_last_step_pc = static_cast<u32>(h8.cpu.pc);


    runtime.h8_c352_instruction_clocks = 0;
    h8_step(&h8);

    if (h8.error_code != H8_DEBUG_NO_ERROR)
      break;

    // h8_step reports H8/300H execution states for the instruction. A valid
    // instruction always includes at least the two-state opcode fetch.
    const u32 instruction_clocks =
      (h8.cycles > 0) ? static_cast<u32>(h8.cycles) : UINT32_C(2);
    const u32 c352_instruction_clocks =
      std::min(runtime.h8_c352_instruction_clocks, instruction_clocks);

    // All non-C352 clocked devices still advance at instruction completion.
    // C352 has already been advanced to any in-instruction register-access
    // points; only the remaining instruction clocks are advanced here.
    H8AdvanceClockedDevices(runtime, instruction_clocks, false);
    if (instruction_clocks > c352_instruction_clocks)
      runtime.h8_c352_deferred_clocks += instruction_clocks - c352_instruction_clocks;
    runtime.h8_c352_instruction_clocks = 0;

    runtime.h8_cycle_balance -= static_cast<s64>(instruction_clocks);

    H8CollectInterruptSources(runtime);
    const u32 interrupt_clocks = H8ServiceInterrupts(runtime);
    if (interrupt_clocks != 0)
    {
      H8AdvanceClockedDevices(runtime, interrupt_clocks, false);
      runtime.h8_c352_deferred_clocks += interrupt_clocks;
      runtime.h8_cycle_balance -= static_cast<s64>(interrupt_clocks);
      H8CollectInterruptSources(runtime);
    }
  }

  if (runtime.h8_c352_deferred_clocks != 0)
  {
    runtime.c352.AdvanceH8Clocks(
      runtime.h8_c352_deferred_clocks,
      std::span<const u8>(runtime.content.c352_samples));
    runtime.h8_c352_deferred_clocks = 0;
  }

  FlushCyberLeadLED(runtime);

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

  FlushCyberLeadLED(runtime);

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
  runtime.h8_c352_instruction_clocks = 0;
  runtime.h8_c352_deferred_clocks = 0;
  runtime.h8_peripherals.Reset();


  runtime.h8_scheduler_count = 0;
  runtime.h8_cycle_balance = 0;
  runtime.h8_cycle_fraction = 0;
  runtime.h8_rtc_scheduler_fraction = 0;
  runtime.h8_jvs_chain.Clear();

  // Cyber Lead cabinets: the titles that send LED data to the panel (MAME cyberlead.cpp).
  if (runtime.content.io_board == "cyberlead")
  {
    runtime.h8_jvs.profile = JVSProfile::CyberLead;
  }
  else if (runtime.content.set_name == "questff")
  {
    runtime.h8_jvs.profile = JVSProfile::NamcoFCA10;
    runtime.h8_fca10.SetMaintainedTestSwitch(true);
  }
  else if (runtime.content.machine_config == "technodr")
  {
    runtime.h8_jvs.profile = JVSProfile::NamcoEMIO102Printer;
    runtime.h8_empri101.SetJobCallback(SaveTechnoDrivePrintJob);

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

  runtime.h8_cpu->instruction_fetch_data = runtime.content.sub_program.data();
  runtime.h8_cpu->instruction_fetch_base = H8_ROM_BASE;
  runtime.h8_cpu->instruction_fetch_size = static_cast<unsigned>(runtime.content.sub_program.size());

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
  const std::string* io_board = FindGameProperty(game, "ioBoard");

  content.machine_config = machine_config ? *machine_config : game.hardware_profile;
  content.state_class = state_class ? *state_class : std::string();
  content.input_profile = input_profile ? *input_profile : std::string();
  content.io_board = io_board ? *io_board : std::string();

  if (content.state_class == "tektagt_state")
    content.rom_board_profile = ROMBoardProfile::M8F4Protected;
  else if (IsAlternateBankStateClass(content.state_class))
    content.rom_board_profile = ROMBoardProfile::AlternateBank;
  else
    content.rom_board_profile = ROMBoardProfile::Ordinary;

  content.alternate_bank = (content.rom_board_profile == ROMBoardProfile::AlternateBank);
  content.requires_ram_preserving_boot_reset =
    RequiresRamPreservingBootReset(content.machine_config) ||
    RequiresRamPreservingBootReset(game.hardware_profile);

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
    if (!Arcade::LoadGameROM(archive_path, game, rom, &member_data, error) ||
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

  if (UsesCDXA(content))
  {
    const Arcade::Database::MediaDefinition* const cdrom =
      Arcade::Database::GetMediaByRole(game, "cdxa_pcb:ata:0:cdrom_001");
    if (!cdrom || cdrom->type != Arcade::Database::MediaType::CHDCDROM)
    {
      Error::SetStringView(error, "Namco System 12 CDXA definition is missing its CD-ROM CHD media.");
      return std::nullopt;
    }

    content.cdxa_cdrom_path = Arcade::Database::GetCompanionMediaPath(archive_path, game, *cdrom);
    if (!FileSystem::FileExists(content.cdxa_cdrom_path.c_str()))
    {
      Error::SetStringFmt(error, "Namco System 12 CDXA CHD for '{}' was not found:\n{}", game.id,
                          content.cdxa_cdrom_path);
      return std::nullopt;
    }

    std::array<u8, SHA1Digest::DIGEST_SIZE> chd_sha1;
    if (!CDImage::GetCHDImageSHA1(content.cdxa_cdrom_path.c_str(), &chd_sha1, error))
      return std::nullopt;

    const std::string chd_sha1_string = SHA1Digest::DigestToString(chd_sha1);
    if (!StringUtil::EqualNoCase(chd_sha1_string, cdrom->sha1))
    {
      Error::SetStringFmt(error, "Namco System 12 CDXA CHD '{}' SHA-1 mismatch.", content.cdxa_cdrom_path);
      return std::nullopt;
    }

    INFO_LOG("System12 CDXA media validated set='{}' path='{}' sha1={}", game.id, content.cdxa_cdrom_path,
             chd_sha1_string);
  }

  if (content.io_board == "cyberlead" && g_settings.arcade_namco_system12_cyber_lead_led_hardware_enabled)
    content.cyberlead_led_firmware = Arcade::JVS::NamcoCyberLead::LoadFactoryLEDFirmware();

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

  RuntimeState& runtime = *s_runtime;
  runtime.c352.PopHostFrame(left, right);

  if (runtime.cdxa)
  {
    s32 cdxa_left = 0;
    s32 cdxa_right = 0;
    runtime.cdxa->PopAudioFrame(&cdxa_left, &cdxa_right);

    *left = std::clamp<s32>(*left + cdxa_left, -32768, 32767);
    *right = std::clamp<s32>(*right + cdxa_right, -32768, 32767);
  }
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

  // Point Blank 2 / Gunbarl wait for a DMA5 completion already consumed during startup.
  // Guard the ROM branch opcode so this workaround cannot patch another revision.
  if (content.set_name == "ptblank2" || content.set_name == "gunbarl")
  {
    static constexpr size_t PB2_DMA_WAIT_BRANCH_OFFSET = 0x331C4;
    static constexpr u32 PB2_DMA_WAIT_BRANCH = UINT32_C(0x1040FFFB);

    if (content.program_rom.size() < (PB2_DMA_WAIT_BRANCH_OFFSET + sizeof(u32)))
    {
      Error::SetStringView(error, "Point Blank 2 compatibility patch is outside the loaded program ROM.");
      return false;
    }

    u32 instruction = 0;
    std::memcpy(&instruction, content.program_rom.data() + PB2_DMA_WAIT_BRANCH_OFFSET, sizeof(instruction));
    if (instruction != PB2_DMA_WAIT_BRANCH)
    {
      Error::SetStringFmt(error,
                          "Point Blank 2 compatibility patch validation failed at 0x{:X}: expected {:08X}, got {:08X}.",
                          PB2_DMA_WAIT_BRANCH_OFFSET, PB2_DMA_WAIT_BRANCH, instruction);
      return false;
    }

    static constexpr u32 MIPS_NOP = UINT32_C(0x00000000);
    std::memcpy(content.program_rom.data() + PB2_DMA_WAIT_BRANCH_OFFSET, &MIPS_NOP, sizeof(MIPS_NOP));
    INFO_LOG("Applied Point Blank 2/Gunbarl DMA completion-wait compatibility patch set='{}' offset={:08X}",
             content.set_name, static_cast<u32>(PB2_DMA_WAIT_BRANCH_OFFSET));
  }
  // Truck Kyosokyoku starts DMA5 before its status words are initialized.
  // Match the instruction order used by MAME's truckk_state::machine_start().
  if (content.set_name == "truckk")
  {
    static constexpr size_t TRUCK_DMA_START_OFFSET = 0x376E4;
    static constexpr size_t TRUCK_STATUS_INIT_OFFSET = 0x376EC;
    static constexpr size_t TRUCK_STATUS_INIT_SIZE = 5 * sizeof(u32);
    static constexpr size_t TRUCK_DMA_START_SIZE = 2 * sizeof(u32);
    static constexpr size_t TRUCK_PATCH_END =
      TRUCK_DMA_START_OFFSET + TRUCK_STATUS_INIT_SIZE + TRUCK_DMA_START_SIZE;

    if (content.program_rom.size() < TRUCK_PATCH_END)
    {
      Error::SetStringView(error, "Truck Kyosokyoku DMA5 startup-order workaround is outside the loaded program ROM.");
      return false;
    }

    std::array<u8, TRUCK_STATUS_INIT_SIZE> status_init{};
    std::memcpy(status_init.data(),
                content.program_rom.data() + TRUCK_STATUS_INIT_OFFSET,
                status_init.size());
    std::memcpy(content.program_rom.data() + TRUCK_DMA_START_OFFSET + status_init.size(),
                content.program_rom.data() + TRUCK_DMA_START_OFFSET,
                TRUCK_DMA_START_SIZE);
    std::memcpy(content.program_rom.data() + TRUCK_DMA_START_OFFSET,
                status_init.data(),
                status_init.size());

    INFO_LOG("Applied Truck Kyosokyoku DMA5 startup-order compatibility workaround.");
  }

  std::memcpy(Bus::g_bios, content.program_rom.data(), Bus::BIOS_SIZE);

  RuntimeState runtime;
  runtime.content = std::move(content);
  runtime.cyberlead_led_enabled = runtime.content.io_board == "cyberlead" &&
                                  g_settings.arcade_namco_system12_cyber_lead_led_hardware_enabled;
  if (HasLinkBoard(runtime.content))
  {
    runtime.hsc = std::make_unique<Arcade::Namco::System12HSC>();
    runtime.hsc->Initialize(fmt::format("namco_system12_hsc/{}", runtime.content.set_name));
  }
  runtime.has_gun_interface = HasGunInterface(runtime.content);
  runtime.test_switch_maintained = HasMaintainedTestSwitch(runtime.content);
  runtime.buttons_on_direction_lines = runtime.content.input_profile == "mdhorse" ||
                                       runtime.content.input_profile == "pacapp" ||
                                       runtime.content.input_profile == "pacapp2";
  runtime.h8_cyberlead.SetButtonsOnDirectionLines(runtime.buttons_on_direction_lines);

  if (UsesCDXA(runtime))
  {
    runtime.cdxa = std::make_unique<Arcade::Namco::System12CDXA>();
    if (!runtime.content.cdxa_cdrom_path.empty() &&
        !runtime.cdxa->SetMedia(runtime.content.cdxa_cdrom_path.c_str(), error))
    {
      return false;
    }

    runtime.cdxa_scheduler_interval_ticks =
      std::max<TickCount>(1, System::GetTicksPerSecond() / CDXA_SH2_SCHEDULER_HZ);
    runtime.cdxa_timing_event =
      std::make_unique<TimingEvent>("Namco System 12 CDXA SH7014", runtime.cdxa_scheduler_interval_ticks,
                                    runtime.cdxa_scheduler_interval_ticks, CDXASH2SchedulerCallback, nullptr);
    runtime.cdxa_timing_event->SetPeriodAndSchedule(runtime.cdxa_scheduler_interval_ticks);
  }

  if (!LoadEEPROM(runtime, error))
    return false;

  runtime.h8_cpu = std::make_unique<h8_system_t>();
  if (runtime.cyberlead_led_enabled && !runtime.content.cyberlead_led_firmware.empty() &&
      !runtime.h8_cyberlead.InitializeLEDFlash(std::span<const u8>(runtime.content.cyberlead_led_firmware),
                                              Path::Combine(runtime.persistence_directory, "cl1-leda.ic5")))
  {
    runtime.content.cyberlead_led_firmware.clear();
  }

  s_runtime = std::move(runtime);
  if (s_runtime->cyberlead_led_enabled)
    Arcade::JVS::NamcoCyberLead::SetActive(&s_runtime->h8_cyberlead);
  ArcadeInput::SetOperatorServiceMinimumHold(SERVICE_MINIMUM_HOLD_MS);
  Reset();

  INFO_LOG("Namco System 12 initialized for '{}'", s_runtime->content.set_name);
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

  if (s_runtime->hsc)
    s_runtime->hsc->Reset();

  if (s_runtime->cdxa)
  {
    s_runtime->cdxa->Reset();
    s_runtime->cdxa_cycle_fraction = 0;
    s_runtime->cdxa_scheduler_count = 0;
  }

  // System 12 board reset leaves the PIO/DMA5 source idle.
  DMA::SetRequest(DMA::Channel::PIO, false);

  ResetH8(*s_runtime);
}

void Shutdown()
{
  if (s_runtime && s_runtime->h8_timing_event && s_runtime->h8_timing_event->IsActive())
    s_runtime->h8_timing_event->Deactivate();

  if (s_runtime && s_runtime->cdxa_timing_event && s_runtime->cdxa_timing_event->IsActive())
    s_runtime->cdxa_timing_event->Deactivate();

  if (s_runtime && s_runtime->hsc)
    s_runtime->hsc->Shutdown();

  // Clear the shared DMA5 request before removing the System 12 provider.
  DMA::SetRequest(DMA::Channel::PIO, false);

  if (s_runtime)
  {
    SaveEEPROM(*s_runtime);
    FlushCyberLeadLED(*s_runtime);
    s_runtime->h8_cyberlead.SaveLEDFlash();
  }

  Arcade::JVS::NamcoCyberLead::SetActive(nullptr);

  ArcadeInput::SetOperatorServiceMinimumHold(0);
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
  UpdateADCInputs(runtime);
  runtime.h8_peripherals.SetADCTrigger(state);
  H8CollectInterruptSources(runtime);

  // System 12 H8 VBlank interleave.
  //
  // The PSX CRTC event can cross active-display and VBlank boundaries in one
  // callback. On real hardware the H8 continues executing between those
  // signal changes; MAME obtains that naturally from its multi-CPU scheduler.
  // Service the H8 immediately on the falling edge so PB7/IRQ/ADC state is
  // observable before a same-callback rising edge replaces it.
  if (!state && runtime.h8_timing_event && runtime.h8_timing_event->IsActive())
    runtime.h8_timing_event->InvokeEarly(true);


  const u32 interrupt_clocks = H8ServiceInterrupts(runtime);
  if (interrupt_clocks != 0)
  {
    H8AdvanceClockedDevices(runtime, interrupt_clocks, true);
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

bool AccessFitsRange(u32 offset, u32 width, u32 base, u32 end)
{
  return (width == 1 || width == 2 || width == 4) && offset >= base && offset <= end &&
         width <= (end - offset + 1);
}

bool ReadLinkBoard(const RuntimeState& runtime, u32 width, u32 offset, u32* value)
{
  return runtime.hsc && value && runtime.hsc->ReadHost(width, offset, value);
}

bool WriteLinkBoard(RuntimeState& runtime, u32 width, u32 offset, u32 value)
{
  return runtime.hsc && runtime.hsc->WriteHost(width, offset, value);
}

u16 ScaleGunAxis(float normalized, u16 minimum, u16 maximum)
{
  normalized = std::clamp(normalized, 0.0f, 1.0f);
  u16 value = static_cast<u16>(minimum +
                               (normalized * static_cast<float>(maximum - minimum)) + 0.5f);
  if ((value & UINT16_C(3)) == UINT16_C(3))
    value++;
  return value;
}

bool ReadGunInterface(const RuntimeState& runtime, u32 width, u32 offset, u32* value)
{
  if (!runtime.has_gun_interface || !value)
    return false;

  u32 canonical_offset = offset;
  if (canonical_offset >= (GUN_INTERFACE_READ_BASE + GUN_INTERFACE_MIRROR) &&
      canonical_offset <= (GUN_INTERFACE_READ_END + GUN_INTERFACE_MIRROR))
  {
    canonical_offset -= GUN_INTERFACE_MIRROR;
  }

  if (!AccessFitsRange(canonical_offset, width, GUN_INTERFACE_READ_BASE, GUN_INTERFACE_READ_END))
    return false;

  const bool ghoul_panic = runtime.content.input_profile == "ghlpanic";
  const u16 x_min = ghoul_panic ? UINT16_C(0x0BE) : UINT16_C(0x0DC);
  const u16 x_max = ghoul_panic ? UINT16_C(0x35F) : UINT16_C(0x387);
  const u16 y_min = ghoul_panic ? UINT16_C(0x01A) : UINT16_C(0x028);
  const u16 y_max = ghoul_panic ? UINT16_C(0x109) : UINT16_C(0x11B);

  const ArcadeInput::LightgunPresentationState state = ArcadeInput::GetLightgunPresentationState();
  const std::array<u16, 4> axes = {
    ScaleGunAxis(state.ports[0].x, x_min, x_max),
    ScaleGunAxis(state.ports[0].y, y_min, y_max),
    ScaleGunAxis(state.ports[1].x, x_min, x_max),
    ScaleGunAxis(state.ports[1].y, y_min, y_max),
  };

  std::array<u8, 16> registers{};
  for (u32 axis = 0; axis < axes.size(); axis++)
  {
    std::memcpy(registers.data() + axis * 4, &axes[axis], sizeof(u16));
    std::memcpy(registers.data() + axis * 4 + 2, &axes[axis], sizeof(u16));
  }

  *value = ReadBytes(std::span<const u8>(registers), width, canonical_offset - GUN_INTERFACE_READ_BASE);
  return true;
}

bool WriteGunInterface(RuntimeState& runtime, u32 width, u32 offset, u32 value)
{
  if (!runtime.has_gun_interface)
    return false;

  u32 canonical_offset = offset;
  if (canonical_offset >= (GUN_INTERFACE_WRITE_BASE + GUN_INTERFACE_MIRROR) &&
      canonical_offset <= (GUN_INTERFACE_WRITE_END + GUN_INTERFACE_MIRROR))
  {
    canonical_offset -= GUN_INTERFACE_MIRROR;
  }

  if (!AccessFitsRange(canonical_offset, width, GUN_INTERFACE_WRITE_BASE, GUN_INTERFACE_WRITE_END))
    return false;

  // The second halfword is the gun read strobe. Only the low halfword drives cabinet outputs.
  if (canonical_offset == GUN_INTERFACE_WRITE_BASE)
  {
    const u8 data = static_cast<u8>(value);
    const u8 changed = runtime.gun_output_latch_valid ?
      static_cast<u8>((runtime.gun_output_latch ^ data) & UINT8_C(0x0F)) : UINT8_C(0x0F);

    if (changed & UINT8_C(0x01))
      Arcade::Output::SetValue("Player2_Gun_Recoil", (data & UINT8_C(0x01)) ? 0 : 1, "Namco System 12 GUN I/F");
    if (changed & UINT8_C(0x02))
      Arcade::Output::SetValue("Player1_Gun_Recoil", (data & UINT8_C(0x02)) ? 0 : 1, "Namco System 12 GUN I/F");
    if (changed & UINT8_C(0x04))
      Arcade::Output::SetValue("P2_Start_lamp", (data & UINT8_C(0x04)) ? 0 : 1, "Namco System 12 GUN I/F");
    if (changed & UINT8_C(0x08))
      Arcade::Output::SetValue("P1_Start_lamp", (data & UINT8_C(0x08)) ? 0 : 1, "Namco System 12 GUN I/F");

    runtime.gun_output_latch = data;
    runtime.gun_output_latch_valid = true;
  }

  return true;
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

  if (s_runtime->cdxa)
  {
    u32 value = UINT32_C(0xFFFFFFFF);
    if (s_runtime->cdxa->ReadPSX(width, offset, value))
    {
      return value;
    }
  }

  u32 device_value = UINT32_C(0xFFFFFFFF);
  if (ReadLinkBoard(*s_runtime, width, offset, &device_value) ||
      ReadGunInterface(*s_runtime, width, offset, &device_value))
  {
    return device_value;
  }

  if (offset >= SHARED_RAM_BASE && offset <= SHARED_RAM_END)
  {
    const u32 shared_offset = offset - SHARED_RAM_BASE;
    const u32 value =
      ReadBytes(std::span<const u8>(s_runtime->shared_ram), width, shared_offset);


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
  if (runtime.cdxa && runtime.cdxa->WritePSX(width, offset, value))
  {
    return true;
  }

  if (WriteLinkBoard(runtime, width, offset, value) ||
      WriteGunInterface(runtime, width, offset, value))
  {
    return true;
  }

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

  // Golgo 13 continuously writes zero to the legacy gun-output window at
  // 0x1F788002 once normal rendering begins. The base System 12 map has no
  // write-side state for this location, so consume this known no-op here
  // instead of falling through to the generic EXP1 warning path.
  if (offset == UINT32_C(0x788002) && value == UINT32_C(0) &&
      runtime.content.machine_config == "golgo13")
  {
    return true;
  }

  if (offset >= SHARED_RAM_BASE && offset <= SHARED_RAM_END)
  {
    const u32 shared_offset = offset - SHARED_RAM_BASE;


    WriteBytes(std::span<u8>(runtime.shared_ram), width, shared_offset, value);

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
            // The H8 leaves reset at this write. Retire the scheduler interval
            // that elapsed while it was held so its first quantum starts here
            // instead of at the previous scheduler boundary.
            if (runtime.h8_timing_event && runtime.h8_timing_event->IsActive())
              runtime.h8_timing_event->InvokeEarly(true);

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

bool SupportsSaveStates(const Arcade::Database::GameDefinition& game)
{
  LoadedContent content;
  const std::string* machine_config = FindGameProperty(game, "machineConfig");
  const std::string* state_class = FindGameProperty(game, "stateClass");
  content.machine_config = machine_config ? *machine_config : game.hardware_profile;
  content.state_class = state_class ? *state_class : std::string();
  return !HasLinkBoard(content);
}

bool CanSaveState(Error* error)
{
  if (s_runtime.has_value() && !s_runtime->h8_cyberlead.IsLEDSaveReady())
  {
    Error::SetStringView(error, TRANSLATE_SV("System", "The Cyber Lead LED board is still initializing or programming "
                                                       "its flash. Try saving again after it finishes."));
    return false;
  }

  return true;
}

bool IsSaveReadinessPending(std::string* message)
{
  if (!s_runtime.has_value() || s_runtime->h8_cyberlead.IsLEDSaveReady())
    return false;

  *message = TRANSLATE_STR("System", "The Cyber Lead LED board is busy. The state will be saved as soon as it "
                                     "finishes (up to 30 seconds).");
  return true;
}

void AppendStateIdentity(std::string* record)
{
  if (s_runtime.has_value() && s_runtime->content.io_board == "cyberlead")
  {
    *record += fmt::format("cyberlead_led={}\n", s_runtime->h8_cyberlead.IsLEDPresent() ? "present" : "absent");
    if (!s_runtime->cyberlead_led_enabled)
      *record += "cyberlead_led_hardware=disabled\n";
  }
}

void GetNVRAMComponents(std::vector<Arcade::SaveState::NVRAMComponent>* components)
{
  if (!s_runtime.has_value())
    return;

  components->push_back({"at28c16", s_eeprom, &s_runtime->eeprom_file});
  s_runtime->h8_cyberlead.GetNVRAMComponents(components);
}

// AT28C16 and Cyber Lead LED flash writes complete immediately; their contents are matched by fingerprint, not
// stored. Host configuration (content, callbacks, JVS chain wiring) and diagnostic counters are excluded.
bool DoState(StateWrapper& sw)
{
  if (!s_runtime.has_value() || !s_runtime->h8_cpu || !s_runtime->h8_timing_event || s_runtime->hsc ||
      !sw.DoMarker("S12Board"))
  {
    return false;
  }

  RuntimeState& r = *s_runtime;
  sw.DoArray(r.shared_ram.data(), r.shared_ram.size());
  sw.DoArray(r.board_control.data(), r.board_control.size());
  sw.Do(&r.gun_output_latch);
  sw.Do(&r.gun_output_latch_valid);
  sw.Do(&r.bank);
  sw.Do(&r.dma_source);
  sw.Do(&r.dma_transfer_count);
  sw.DoArray(r.m8f4_challenge.data(), r.m8f4_challenge.size());
  sw.Do(&r.m8f4_challenge_count);
  sw.Do(&r.m8f4_protected_source);
  sw.Do(&r.m8f4_response);
  sw.Do(&r.m8f4_nonce);
  sw.Do(&r.m8f4_response_class);
  sw.Do(&r.m8f4_challenge_active);
  sw.Do(&r.m8f4_response_valid);
  sw.Do(&r.m8f4_protected_dma_armed);
  sw.Do(&r.m8f4_waiting_for_release);
  sw.Do(&r.m8f4_ready);
  sw.Do(&r.m8f4_transaction_count);
  sw.Do(&r.m8f4_dma_count);
  sw.Do(&r.mainboard_reset_pending);
  sw.Do(&r.boot_warm_reset_completed);
  sw.Do(&r.cold_boot_h8_released);
  sw.DoArray(r.cold_boot_config_header.data(), r.cold_boot_config_header.size());
  sw.Do(&r.cold_boot_config_feed_words);
  sw.Do(&r.cold_boot_config_finalize_words);
  sw.Do(&r.cold_boot_config_bulk_complete);
  sw.Do(&r.ram_preserving_reset_count);
  sw.Do(&r.test_switch_latched);
  sw.Do(&r.test_switch_was_pressed);

  if (!sw.DoMarker("S12H8") || !Arcade::H8::DoCoreState(sw, *r.h8_cpu))
    return false;

  sw.Do(&r.h8_scheduler_interval_ticks);
  sw.Do(&r.h8_scheduler_rate_hz);
  sw.Do(&r.h8_cycle_balance);
  sw.Do(&r.h8_cycle_fraction);
  sw.Do(&r.h8_rtc_scheduler_fraction);
  sw.DoArray(r.h8_internal.data(), r.h8_internal.size());
  // h8_scheduler_count only counts scheduler callbacks for the fault log; it is a diagnostic counter, not stored.
  sw.Do(&r.h8_jvs_sense);
  sw.Do(&r.h8_jvs_rts);
  sw.Do(&r.h8_cyberlead_deferred_clocks);
  sw.Do(&r.h8_jvs.initialize_sense_after_response);
  sw.Do(&r.h8_sci0_ssr_read);
  sw.Do(&r.h8_sci0_tx_busy);
  sw.Do(&r.h8_sci0_tx_shift_byte);
  sw.Do(&r.h8_sci0_tx_clocks_remaining);
  sw.Do(&r.h8_sci0_rx_busy);
  sw.Do(&r.h8_sci0_rx_clocks_remaining);
  sw.Do(&r.h8_sci1_ssr_read);
  sw.Do(&r.h8_sci1_rx_busy);
  sw.Do(&r.h8_sci1_rx_clocks_remaining);
  sw.Do(&r.h8_sub_porta);
  sw.Do(&r.h8_sub_portb);
  sw.Do(&r.h8_settings_ce);
  sw.Do(&r.h8_settings_clk);
  sw.Do(&r.h8_settings_data);
  sw.Do(&r.h8_settings_address);
  sw.Do(&r.h8_settings_value);
  sw.Do(&r.h8_settings_bit);
  sw.Do(&r.h8_rtc_ce);
  sw.Do(&r.h8_rtc_clk);
  sw.Do(&r.h8_rtc_data);
  sw.Do(&r.h8_rtc_bit);
  sw.DoArray(r.h8_rtc_regs.data(), r.h8_rtc_regs.size());
  // The RTC runs from emulated time once seeded from the host clock at boot; the restored machine keeps its time.
  s64 rtc_time = static_cast<s64>(r.h8_rtc_time);
  sw.Do(&rtc_time);
  r.h8_rtc_time = static_cast<std::time_t>(rtc_time);
  sw.Do(&r.h8_rtc_millisecond_accumulator);
  sw.Do(&r.h8_vblank);
  sw.Do(&r.h8_c352_instruction_clocks);
  sw.Do(&r.h8_c352_deferred_clocks);

  if (!r.h8_timer16.DoState(sw) || !r.h8_peripherals.DoState(sw) || !r.h8_intc.DoState(sw) || !r.c352.DoState(sw) ||
      !sw.DoMarker("S12JVS") || !r.h8_jvs_bus.DoState(sw) || !r.h8_fca10.DoState(sw) || !r.h8_emio102.DoState(sw) ||
      !r.h8_empri101.DoState(sw) || !r.h8_cyberlead.DoState(sw))
  {
    return false;
  }

  bool has_cdxa = static_cast<bool>(r.cdxa);
  sw.Do(&has_cdxa);
  if (has_cdxa != static_cast<bool>(r.cdxa))
    return false;
  if (has_cdxa)
  {
    // The scheduler event's deadline and period are restored by TimingEvents; it is always active with the board.
    sw.Do(&r.cdxa_scheduler_interval_ticks);
    sw.Do(&r.cdxa_cycle_fraction);
    sw.Do(&r.cdxa_scheduler_count);
    if (!r.cdxa_timing_event || !r.cdxa->DoState(sw))
      return false;
  }

  return !sw.HasError();
}

} // namespace NamcoSystem12
