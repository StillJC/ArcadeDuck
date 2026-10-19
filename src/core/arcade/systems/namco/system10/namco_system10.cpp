// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-FileCopyrightText: Andreas Naive
// SPDX-FileCopyrightText: smf
// SPDX-FileCopyrightText: windyfairy
// SPDX-License-Identifier: GPL-3.0-only
//
// Namco System 10 protection datasets, static transforms, and documented board
// behavior are adapted from/corroborated against MAME's BSD-3-Clause
// namcos10.cpp and ns10crypt.cpp implementations. See THIRD-PARTY-LICENSES.txt.

#include "core/arcade/systems/namco/system10/namco_system10.h"

#include "core/arcade/systems/namco/system10/namco_system10_exio.h"
#include "core/arcade/systems/namco/system10/namco_system10_memn.h"
#include "core/arcade/systems/namco/system10/namco_system10_protection.h"

#include "core/arcade/arcade_database.h"
#include "core/arcade/devices/jvs/jvs_bus.h"
#include "core/arcade/devices/jvs/namco_cyberlead.h"
#include "core/arcade/arcade_input.h"
#include "core/cpu_core.h"
#include "core/interrupt_controller.h"
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
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <utility>

Log_SetChannel(NamcoSystem10);

namespace NamcoSystem10 {
namespace {

static constexpr u32 MEMN_READY_BASE = 0x400000;
static constexpr u32 MEMN_COMMAND_BASE = 0x410000;
static constexpr u32 MEMN_COLUMN_BASE = 0x420000;
static constexpr u32 MEMN_ROW_LOW_BASE = 0x430000;
static constexpr u32 MEMN_ROW_HIGH_BASE = 0x440000;
static constexpr u32 MEMN_DATA_BASE = 0x450000;
static constexpr u32 MEMN_DEVICE_BASE = 0x460000;
static constexpr u32 MEMN_CONTROL_BASE = 0x470000;
static constexpr u32 LOOKUP_RAM_BASE = 0x500000;
static constexpr u32 LOOKUP_RAM_SIZE = 0x100000;

// MEM(M) Type-1 board. Physical 0x1F300000/0x1F400000 live in EXP1,
// while physical 0x1FB40000 is EXP3 offset 0x140000.
static constexpr u32 MEMM_CRYPTO_SWITCH_BASE = 0x300000;
static constexpr u32 MEMM_RANGE_BASE = 0x400000;
static constexpr u32 MEMM_RANGE_SIZE = 0x200000;
static constexpr u32 MEMM_BANK_EXP3_BASE = 0x140000;
static constexpr u32 MEMM_BANK_EXP3_SIZE = 0x20000;
static constexpr u32 MEMM_PROGRAM_FLASH_SIZE = 0x800000;
static constexpr u32 MEMM_DATA_ROM_SIZE = 0x2000000;
static constexpr u32 MEMM_BOOT_DECODE_SIZE = 0x62000;
static constexpr u32 MEMM_TRACE_EVENT_LIMIT = 128;

// Intel 28F640J5 geometry, x16 mode.
// 64 Mbit / 8 MiB total, 64 symmetric 128 KiB erase blocks,
// and one 32-byte (16-word) write buffer.
static constexpr u32 MEMM_FLASH_WORD_COUNT = MEMM_PROGRAM_FLASH_SIZE / 2;
static constexpr u32 MEMM_FLASH_BLOCK_SIZE = 0x20000;
static constexpr u32 MEMM_FLASH_BLOCK_WORDS = MEMM_FLASH_BLOCK_SIZE / 2;
static constexpr u32 MEMM_FLASH_BLOCK_COUNT = MEMM_PROGRAM_FLASH_SIZE / MEMM_FLASH_BLOCK_SIZE;
static constexpr u32 MEMM_FLASH_WRITE_BUFFER_WORDS = 16;

static constexpr u16 MEMM_FLASH_STATUS_READY = UINT16_C(0x0080);
static constexpr u16 MEMM_FLASH_STATUS_ERASE_ERROR = UINT16_C(0x0020);
static constexpr u16 MEMM_FLASH_STATUS_PROGRAM_ERROR = UINT16_C(0x0010);
static constexpr u16 MEMM_FLASH_STATUS_PROTECT_ERROR = UINT16_C(0x0002);
static constexpr u16 MEMM_FLASH_MAKER_ID = UINT16_C(0x0089);
static constexpr u16 MEMM_FLASH_DEVICE_ID = UINT16_C(0x0015);

enum class MediaType : u8
{
  None = 0,
  MemN,
  MemM,
};

enum class Intel28F640J5Mode : u8
{
  Array = 0,
  Identifier,
  Query,
  Status,
  ProgramData,
  EraseConfirm,
  LockConfirm,
  ConfigurationConfirm,
  WriteBufferCount,
  WriteBufferData,
  WriteBufferConfirm,
};

struct Intel28F640J5State
{
  Intel28F640J5Mode mode = Intel28F640J5Mode::Array;
  u16 status = MEMM_FLASH_STATUS_READY;
  bool master_lock = false;
  std::array<bool, MEMM_FLASH_BLOCK_COUNT> block_locked = {};
  u8 sts_configuration = 0;

  u32 setup_word_address = 0;
  u32 write_buffer_block_word = 0;
  u32 write_buffer_start_word = 0;
  u32 write_buffer_count = 0;
  u32 write_buffer_received = 0;
  std::array<u16, MEMM_FLASH_WRITE_BUFFER_WORDS> write_buffer = {};
};

// PROVEN at runtime on Star Trigon: the firmware writes 0x8000 here immediately
// before copying the protected program beginning at logical NAND block 10.
// The 0x380000 alias is retained from the independent System 10 RE dossier.
static constexpr u32 TYPE2_SWITCH_BASE = 0x300000;
static constexpr u32 TYPE2_SWITCH_ALIAS_BASE = 0x380000;
static constexpr u32 TYPE2_TRACE_WORD_LIMIT = 128;

// S10-03A base-board serial lines. Runtime execution independently reaches
// 0x1FBA000A after the protected Star Trigon program enters ResetGraph.
// The clock/data interpretation is CORROBORATED by the System 10 behavioral
// reference; the identity/content of the attached I2C device remains OPEN.
static constexpr u32 EXP3_SYSTEM_SWITCH_BASE = 0x1a0000;
static constexpr u32 EXP3_EXIO_IDENT_BASE = 0x1a0002;
static constexpr u32 EXP3_PLAYER_INPUT_BASE = 0x1a0004;
static constexpr u32 EXP3_PLAYER_INPUT_SIZE = 4;
static constexpr u32 EXP3_I2C_CLOCK_BASE = 0x1a0008;
static constexpr u32 EXP3_I2C_DATA_BASE = 0x1a000a;
static constexpr u32 EXP3_INTERRUPT_ACK_BASE = 0x1a0012;
static constexpr u32 EXP3_INTERRUPT_ACK_END = 0x1a001a;
static constexpr u32 EXP3_INTERRUPT_STATUS_BASE = 0x1a001a;
static constexpr u32 I2C_TRACE_EVENT_LIMIT = 64;

// S10-04A base-board interrupt block.
// CORROBORATED: the System 10 base board raises the PSX external/PIO IRQ at
// 100 Hz, latches I/O cause bit 3 in 0x1FBA001A, and uses 0x1FBA0018 to
// acknowledge that cause. Star Trigon now runs with the periodic IRQ present,
// but has not yet been observed reading or acknowledging these registers.
static constexpr u16 BASEBOARD_IO_INTERRUPT_BIT = UINT16_C(0x0008);
static constexpr u32 BASEBOARD_IO_INTERRUPT_HZ = 100;
static constexpr u32 BASEBOARD_IRQ_TRACE_LIMIT = 16;
static constexpr u32 BASEBOARD_INPUT_TRACE_LIMIT = 32;
static constexpr u32 EXIO_IDENT_TRACE_LIMIT = 16;

static constexpr u32 PROGRAM_ROM_SIZE = 0x400000;
static constexpr u32 EXIO_WINDOW_OFFSET = 0x200000;
static constexpr u32 PROGRAM_FIRST_PAGE = 0x040;
static constexpr u32 PROGRAM_FIRST_END = 0x01c000;
static constexpr u32 PROGRAM_FILL_END = 0x020000;
static constexpr u32 PROGRAM_SECOND_PAGE = 0x120;

static_assert(PROGRAM_FIRST_END == UINT32_C(0x0e0) * MemNRawNAND::DATA_BYTES_PER_PAGE);
static_assert((PROGRAM_ROM_SIZE - PROGRAM_FILL_END) ==
              UINT32_C(0x1f00) * MemNRawNAND::DATA_BYTES_PER_PAGE);

struct Type1State
{
  u16 mask = 0;
  u32 counter = 0;
};

enum class I2CPhase : u8
{
  Idle = 0,
  ReceiveByte,
  AckRise,
  AckFall,
};

struct I2CState
{
  u8 host_clock = 1;
  u8 host_data = 1;
  u8 device_clock = 1;
  u8 device_data = 1;
  u8 previous_clock = 1;
  u8 previous_data = 1;
  I2CPhase phase = I2CPhase::Idle;
  u8 byte = 0;
  s32 bit = 7;
  u32 trace_events = 0;
};

struct CabinetInputs;

struct RuntimeState
{
  std::string set_name;
  MediaType media_type = MediaType::None;

  const MemNProfile* memn_profile = nullptr;
  u16 data_xor = 0;
  std::vector<MemNRawNAND> nands;
  std::vector<u8> lookup_ram;
  u16 control_latch = 0;
  u16 nand_device = 0;

  MemMBoardProfile memm_board_profile = MemMBoardProfile::Unknown;
  std::vector<u8> memm_program_flash;
  std::vector<u8> memm_data_rom;
  std::string memm_persistence_directory;
  std::string memm_flash_path;
  bool memm_flash_dirty = false;
  u32 memm_bank = 0;
  bool memm_direct_access = false;
  Intel28F640J5State memm_flash;
  bool type1_enabled = false;
  u8 type1_iv = 0;
  u32 type1_generation = 0;
  u32 type1_trace_words = 0;
  u32 memm_trace_events = 0;
  Type1State type1_state;

  bool type2_enabled = false;
  bool type2_transform_supported = false;
  u8 type2_iv = 0;
  u32 type2_generation = 0;
  u32 type2_trace_words = 0;
  Type2State type2_state;
  std::vector<u8> type2_nonlinear_table;

  I2CState i2c;

  // Main-board JAMMA/system input state. The System 10 test switch is a
  // maintained cabinet switch, so ArcadeDuck latches the momentary UI binding
  // on its rising edge.
  bool test_switch_was_pressed = false;
  bool test_switch_latched = false;

  const CabinetInputs* cabinet_inputs = nullptr;
  std::unique_ptr<EXIOBoard> exio;

  // Cabinet JVS I/O on the EXIO serial channel 1 (RS-485) link.
  bool cyberlead_cabinet = false;
  bool jvs_addressed = false;
  Arcade::JVS::Bus jvs_bus;
  Arcade::JVS::NamcoCyberLead cyberlead;

  // Expansion-board identifier serializer.
  s8 exio_ident_bit = 0;
  u32 exio_ident_byte = 0;
  u32 exio_ident_trace_events = 0;
  u32 baseboard_input_trace_events = 0;

  u16 baseboard_interrupt_status = 0;
  u32 baseboard_irq_trace_events = 0;
};

std::optional<RuntimeState> s_runtime;
std::unique_ptr<TimingEvent> s_baseboard_io_event;
std::unique_ptr<TimingEvent> s_cabinet_io_event;

// CORROBORATED behavior for the KC001A MEM(M) board used by Mr. Driller 2.
// These bit orders describe observable transforms; they are kept profile-local
// instead of treating them as universal System 10 behavior.
static constexpr std::array<u8, 16> MRDRILLER2_STATIC_BIT_ORDER = {
  12, 13, 15, 14, 11, 10, 9, 8, 7, 6, 4, 1, 2, 5, 0, 3,
};
static constexpr std::array<u8, 16> MRDRILLER2_TYPE1_OUTPUT_BIT_ORDER = {
  9, 13, 15, 7, 14, 8, 6, 10, 11, 12, 3, 5, 0, 1, 4, 2,
};
static constexpr std::array<u8, 16> MRDRILLER2_TYPE1_CIPHER_FEEDBACK_BIT_ORDER = {
  6, 11, 3, 1, 13, 5, 15, 10, 2, 9, 8, 4, 0, 12, 7, 14,
};
static constexpr std::array<u8, 16> MRDRILLER2_TYPE1_PLAIN_FEEDBACK_BIT_ORDER = {
  9, 7, 5, 2, 14, 4, 13, 8, 0, 15, 10, 1, 3, 6, 12, 11,
};

// The reverse-engineered equivalent-state initialization currently available
// for Type-1. Value 16 is the published placeholder for IVs whose independent
// hardware value remains unknown; preserve it explicitly rather than inventing
// a different seed.
static constexpr std::array<u16, 16> MRDRILLER2_TYPE1_INITIAL_MASK = {
  16, 16, 16, 0, 4, 9, 16, 16, 16, 8, 16, 1, 16, 9, 16, 5,
};

constexpr u16 Permute16(u16 value, const std::array<u8, 16>& source_bits)
{
  u16 result = 0;
  for (u32 i = 0; i < source_bits.size(); i++)
  {
    const u32 output_bit = 15 - i;
    result |= static_cast<u16>(((value >> source_bits[i]) & 1u) << output_bit);
  }
  return result;
}

constexpr u16 DecodeMemMStaticWord(MemMBoardProfile profile, u16 raw_word)
{
  switch (profile)
  {
    case MemMBoardProfile::MrDriller2:
      return Permute16(static_cast<u16>(raw_word ^ UINT16_C(0xaaaa)), MRDRILLER2_STATIC_BIT_ORDER);

    case MemMBoardProfile::Unknown:
    default:
      return raw_word;
  }
}

constexpr u32 BitValue(u32 value, u32 bit)
{
  return (value >> bit) & 1u;
}

constexpr u16 DecodeMrDriller2Type1Word(Type1State& state, u16 cipher_word)
{
  const u16 permuted_cipher = Permute16(cipher_word, MRDRILLER2_TYPE1_OUTPUT_BIT_ORDER);
  const u16 plain_word = static_cast<u16>(permuted_cipher ^ state.mask);
  const u32 counter = state.counter;

  u16 nonlinear = 0;
  nonlinear |= static_cast<u16>(BitValue(counter, 4) << 15);
  nonlinear |= static_cast<u16>((BitValue(cipher_word, 2) ^ BitValue(cipher_word, 5)) << 14);
  nonlinear |= static_cast<u16>(BitValue(cipher_word, 0) << 13);
  nonlinear |= static_cast<u16>((BitValue(cipher_word, 4) | BitValue(cipher_word, 5)) << 12);
  nonlinear |= static_cast<u16>(BitValue(counter, 0) << 11);
  nonlinear |= static_cast<u16>(BitValue(cipher_word, 6) << 10);
  nonlinear |= static_cast<u16>((BitValue(cipher_word, 4) & BitValue(counter, 1)) << 8);
  nonlinear |= static_cast<u16>(BitValue(counter, 3) << 6);
  nonlinear |= static_cast<u16>((BitValue(cipher_word, 3) | BitValue(counter, 7)) << 5);
  nonlinear |= static_cast<u16>((BitValue(cipher_word, 2) ^ BitValue(counter, 3)) << 4);
  nonlinear |= static_cast<u16>(BitValue(counter, 2) << 3);
  nonlinear |= static_cast<u16>((BitValue(cipher_word, 7) & BitValue(counter, 7)) << 2);
  nonlinear |= static_cast<u16>(BitValue(counter, 5) << 1);
  nonlinear |= static_cast<u16>(BitValue(cipher_word, 7) | BitValue(counter, 1));

  state.mask =
    static_cast<u16>(nonlinear ^
                     Permute16(cipher_word, MRDRILLER2_TYPE1_CIPHER_FEEDBACK_BIT_ORDER) ^
                     Permute16(plain_word, MRDRILLER2_TYPE1_PLAIN_FEEDBACK_BIT_ORDER) ^
                     UINT16_C(0xecbe));
  state.counter++;
  return plain_word;
}


void ResetType1State(RuntimeState& runtime, u8 iv)
{
  runtime.type1_state = {};
  runtime.type1_state.mask = MRDRILLER2_TYPE1_INITIAL_MASK[iv & 0x0f];
  runtime.type1_trace_words = 0;
}

void ResetType2State(RuntimeState& runtime)
{
  // The nonlinear table address counter is not reset by the Type-2 enable.
  const u32 nonlinear_count = runtime.type2_state.nonlinear_count;
  runtime.type2_state = {};
  runtime.type2_state.nonlinear_count = nonlinear_count;
  runtime.type2_trace_words = 0;
}

void InitializeType2State(RuntimeState& runtime, u8 iv)
{
  ResetType2State(runtime);
  if (runtime.memn_profile && runtime.memn_profile->type2)
    InitializeType2State(*runtime.memn_profile->type2, runtime.type2_state, iv);
}

void ResetI2CState(RuntimeState& runtime)
{
  runtime.i2c = {};
}

bool TakeI2CTraceSlot(RuntimeState& runtime)
{
  if (runtime.i2c.trace_events >= I2C_TRACE_EVENT_LIMIT)
    return false;

  runtime.i2c.trace_events++;
  return true;
}

void UpdateI2CState(RuntimeState& runtime)
{
  I2CState& i2c = runtime.i2c;
  u8 clock = static_cast<u8>(i2c.device_clock & i2c.host_clock & 1);
  u8 data = static_cast<u8>(i2c.device_data & i2c.host_data & 1);

  if (i2c.previous_clock == clock && i2c.previous_data == data)
    return;

  switch (i2c.phase)
  {
    case I2CPhase::Idle:
    {
      if (clock && !data)
      {
        i2c.byte = 0;
        i2c.bit = 7;
        i2c.phase = I2CPhase::ReceiveByte;
        if (TakeI2CTraceSlot(runtime))
          INFO_LOG("System10 I2C START");
      }
      break;
    }

    case I2CPhase::ReceiveByte:
    {
      if (clock && data && !i2c.previous_data)
      {
        i2c.phase = I2CPhase::Idle;
        if (TakeI2CTraceSlot(runtime))
          INFO_LOG("System10 I2C STOP");
      }
      else if (clock && !i2c.previous_clock)
      {
        i2c.byte = static_cast<u8>(i2c.byte | (data << i2c.bit));
        i2c.bit--;
        if (i2c.bit < 0)
        {
          // CORROBORATED base-board behavior: acknowledge each transmitted
          // host byte by pulling SDA low for the following ACK clock. This is
          // line-level behavior only; no EEPROM payload is synthesized.
          i2c.phase = I2CPhase::AckRise;
          i2c.device_data = 0;
          data = 0;
          if (TakeI2CTraceSlot(runtime))
            INFO_LOG("System10 I2C RX byte=0x{:02X} ACK", i2c.byte);
        }
      }
      break;
    }

    case I2CPhase::AckRise:
    {
      if (clock && !i2c.previous_clock)
        i2c.phase = I2CPhase::AckFall;
      break;
    }

    case I2CPhase::AckFall:
    {
      if (!clock && i2c.previous_clock)
      {
        i2c.device_data = 1;
        data = static_cast<u8>(i2c.host_data & 1);
        i2c.byte = 0;
        i2c.bit = 7;
        i2c.phase = I2CPhase::ReceiveByte;
      }
      break;
    }
  }

  i2c.previous_clock = clock;
  i2c.previous_data = data;
}

u16 ReadI2CClockLine(const RuntimeState& runtime)
{
  return static_cast<u16>(runtime.i2c.device_clock & runtime.i2c.host_clock & 1);
}

u16 ReadI2CDataLine(const RuntimeState& runtime)
{
  return static_cast<u16>(runtime.i2c.device_data & runtime.i2c.host_data & 1);
}

void WriteI2CClockLine(RuntimeState& runtime, u16 value)
{
  runtime.i2c.host_clock = static_cast<u8>(value & 1);
  UpdateI2CState(runtime);
}

void WriteI2CDataLine(RuntimeState& runtime, u16 value)
{
  runtime.i2c.host_data = static_cast<u8>(value & 1);
  UpdateI2CState(runtime);
}

void ResetBaseboardInputState(RuntimeState& runtime)
{
  runtime.test_switch_was_pressed = false;
  runtime.test_switch_latched = false;
  runtime.exio_ident_bit = 0;
  runtime.exio_ident_byte = 0;
  runtime.exio_ident_trace_events = 0;
  runtime.baseboard_input_trace_events = 0;
}

u16 ReadBaseboardSystemSwitches(const RuntimeState& runtime)
{
  // Main-board DSW1 and the JVS sense input (bit 10) are active-low. Sense is
  // pulled low while a JVS I/O board is connected.
  u16 value = UINT16_C(0xffff);
  if (runtime.cyberlead_cabinet)
    value &= static_cast<u16>(~UINT16_C(0x0400));
  return value;
}

u16 ReadEXIOIdentifier(RuntimeState& runtime)
{
  // The base board shifts one expansion-board ID bit onto both low return bits.
  // A board-less cabinet reports 0xff, so every serialized bit is high. After
  // 0x20 response bytes the line remains in the high/high idle state.
  const u8 response = runtime.exio ? runtime.exio->GetIdentifier() : UINT8_C(0xff);

  u16 result;
  if (runtime.exio_ident_byte >= 0x20)
  {
    result = UINT16_C(0x0003);
  }
  else
  {
    const bool bit = ((response >> runtime.exio_ident_bit) & 1u) != 0;
    result = bit ? UINT16_C(0x0003) : UINT16_C(0x0000);

    runtime.exio_ident_bit--;
    if (runtime.exio_ident_bit < 0)
    {
      runtime.exio_ident_bit = 7;
      runtime.exio_ident_byte++;
    }
  }

  if (runtime.exio_ident_trace_events < EXIO_IDENT_TRACE_LIMIT)
  {
    INFO_LOG("System10 base-board EXIO ID read byte={} bit={} value=0x{:04X}",
             runtime.exio_ident_byte, runtime.exio_ident_bit, result);
    runtime.exio_ident_trace_events++;
  }

  return result;
}

void ResetEXIOIdentifier(RuntimeState& runtime)
{
  runtime.exio_ident_bit = 7;
  runtime.exio_ident_byte = 0;
  runtime.exio_ident_trace_events = 0;
}

bool ReadU32Register(u32 reg, u32 byte_offset, u32 width, u32* value);

// Cabinet input wiring per MAME input profile (database "inputProfile"). IN1 and EXIO_IN1 are
// active low unless a bit is marked active high.
struct InputBit
{
  u8 bit;
  u8 port;
  std::string_view key;
  bool active_high = false;
};

enum class CabinetAnalog : u8
{
  None,
  Sticks, // two players, two analog sticks each
  Drums,  // piezo drum sensors, full scale while struck
};

struct CabinetInputs
{
  std::string_view input_profile;
  std::span<const InputBit> in1;
  std::span<const InputBit> exio_in1;
  CabinetAnalog analog;
};

constexpr std::array<InputBit, 22> DEFAULT_IN1 = {{
  {0, 0, "Right"}, {1, 0, "Left"}, {2, 0, "Down"}, {3, 0, "Up"}, {4, 0, "Button1"}, {5, 0, "Button2"},
  {6, 0, "Button3"}, {7, 0, "Start"}, {17, 0, "Button4"}, {18, 0, "Button5"}, {19, 0, "Button6"},
  {8, 1, "Right"}, {9, 1, "Left"}, {10, 1, "Down"}, {11, 1, "Up"}, {12, 1, "Button1"}, {13, 1, "Button2"},
  {14, 1, "Button3"}, {15, 1, "Start"}, {21, 1, "Button4"}, {22, 1, "Button5"}, {23, 1, "Button6"},
}};

constexpr std::array<InputBit, 16> THREE_BUTTON_IN1 = {{
  {0, 0, "Right"}, {1, 0, "Left"}, {2, 0, "Down"}, {3, 0, "Up"}, {4, 0, "Button1"}, {5, 0, "Button2"},
  {6, 0, "Button3"}, {7, 0, "Start"},
  {8, 1, "Right"}, {9, 1, "Left"}, {10, 1, "Down"}, {11, 1, "Up"}, {12, 1, "Button1"}, {13, 1, "Button2"},
  {14, 1, "Button3"}, {15, 1, "Start"},
}};

constexpr std::array<InputBit, 2> STARTRGN_IN1 = {{{4, 0, "Button1"}, {7, 0, "Start"}}};

constexpr std::array<InputBit, 14> KONOTAKO_IN1 = {{
  {0, 0, "Right"}, {1, 0, "Left"}, {2, 0, "Down"}, {3, 0, "Up"}, {4, 0, "Button1"}, {5, 0, "Button2"},
  {7, 0, "Start"},
  {8, 1, "Right"}, {9, 1, "Left"}, {10, 1, "Down"}, {11, 1, "Up"}, {12, 1, "Button1"}, {13, 1, "Button2"},
  {15, 1, "Start"},
}};

// Four-player panel: shared left (1P/2P) and right (3P/4P) side selection keys.
constexpr std::array<InputBit, 13> NFLCLSFB_IN1 = {{
  {0, 2, "SelectLeft"}, {1, 0, "SelectLeft"}, {2, 0, "SelectDown"}, {3, 0, "SelectUp"}, {4, 0, "Enter"},
  {5, 0, "Start"}, {6, 1, "Start"}, {7, 0, "Button3"}, {8, 2, "SelectRight"}, {9, 0, "SelectRight"},
  {13, 2, "Start"}, {14, 3, "Start"}, {15, 2, "Button3"},
}};

constexpr std::array<InputBit, 8> NFLCLSFB_EXIO_IN1 = {{
  {0, 0, "Button1"}, {2, 1, "Button1"}, {1, 2, "Button1"}, {3, 3, "Button1"},
  {4, 0, "Button2"}, {6, 1, "Button2"}, {5, 2, "Button2"}, {7, 3, "Button2"},
}};

constexpr std::array<InputBit, 9> GAHAHA_IN1 = {{
  {0, 0, "Button2"}, {1, 0, "Button1"}, {8, 1, "Button2"}, {9, 1, "Button1"}, {2, 0, "SelectDown"},
  {3, 0, "SelectUp"}, {4, 0, "Enter"}, {7, 0, "Start"}, {15, 1, "Start"},
}};

// The scope sensor is on while the player looks through the scope.
constexpr std::array<InputBit, 14> G13JNR_IN1 = {{
  {2, 0, "SelectDown"}, {3, 0, "SelectUp"}, {4, 0, "Enter"}, {5, 0, "Trigger"}, {7, 0, "Start"},
  {27, 0, "Sensor", true},
  {8, 1, "Right"}, {9, 1, "Left"}, {10, 1, "Down"}, {11, 1, "Up"}, {12, 1, "Button1"}, {13, 1, "Button2"},
  {14, 1, "Button3"}, {15, 1, "Start"},
}};

constexpr std::array<InputBit, 3> OPERATOR_PANEL_IN1 = {{{2, 0, "SelectDown"}, {3, 0, "SelectUp"}, {4, 0, "Enter"}}};

constexpr std::array<InputBit, 10> NICETSUK_IN1 = {{
  {0, 0, "Up"}, {1, 0, "Down"}, {2, 0, "SelectDown"}, {3, 0, "SelectUp"}, {4, 0, "Enter"}, {5, 0, "Button4"},
  {7, 0, "Start"}, {17, 0, "Button1"}, {18, 0, "Button3"}, {12, 0, "Button2"},
}};

constexpr std::array<InputBit, 10> SQUIZCHS_IN1 = {{
  {0, 0, "Button4"}, {1, 0, "Button3"}, {2, 0, "Button2"}, {3, 0, "Button1"}, {7, 0, "Start"},
  {8, 1, "Button4"}, {9, 1, "Button3"}, {10, 1, "Button2"}, {11, 1, "Button1"}, {15, 1, "Start"},
}};

constexpr std::array<InputBit, 4> PTBLANK3_IN1 = {{{4, 0, "Trigger"}, {7, 0, "Start"}, {12, 1, "Trigger"}, {15, 1, "Start"}}};

constexpr std::array<InputBit, 9> IPPO2_IN1 = {{
  {1, 0, "Button1"}, {2, 0, "Button2", true}, {3, 0, "Button3", true}, {4, 0, "Button7", true},
  {12, 0, "Button4", true}, {13, 0, "Button5", true}, {14, 0, "Button6", true}, {15, 0, "Select"},
  {27, 0, "Sensor"},
}};

constexpr std::array<CabinetInputs, 15> CABINET_INPUTS = {{
  {"namcos10", DEFAULT_IN1, {}, CabinetAnalog::None},
  {"mrdrilr2", DEFAULT_IN1, {}, CabinetAnalog::None},
  {"gamshara", THREE_BUTTON_IN1, {}, CabinetAnalog::None},
  {"gjspace", THREE_BUTTON_IN1, {}, CabinetAnalog::None},
  {"startrgn", STARTRGN_IN1, {}, CabinetAnalog::None},
  {"konotako", KONOTAKO_IN1, {}, CabinetAnalog::None},
  {"nflclsfb", NFLCLSFB_IN1, NFLCLSFB_EXIO_IN1, CabinetAnalog::None},
  {"gahaha", GAHAHA_IN1, {}, CabinetAnalog::Sticks},
  {"g13jnr", G13JNR_IN1, {}, CabinetAnalog::None},
  {"mgexio_medal", OPERATOR_PANEL_IN1, {}, CabinetAnalog::None},
  {"taiko", OPERATOR_PANEL_IN1, {}, CabinetAnalog::Drums},
  {"nicetsuk", NICETSUK_IN1, {}, CabinetAnalog::None},
  {"squizchs", SQUIZCHS_IN1, {}, CabinetAnalog::None},
  {"ptblank3", PTBLANK3_IN1, {}, CabinetAnalog::None},
  {"ippo2", IPPO2_IN1, {}, CabinetAnalog::None},
}};

const CabinetInputs& FindCabinetInputs(std::string_view set_name)
{
  std::string_view input_profile = "namcos10";
  if (const Arcade::Database::GameDefinition* game = Arcade::Database::GetGame(set_name))
  {
    for (const Arcade::Database::Property& property : game->properties)
    {
      if (property.name == "inputProfile")
        input_profile = property.value;
    }
  }

  const auto it = std::find_if(CABINET_INPUTS.begin(), CABINET_INPUTS.end(),
                               [input_profile](const CabinetInputs& c) { return c.input_profile == input_profile; });
  return (it != CABINET_INPUTS.end()) ? *it : CABINET_INPUTS[0];
}

u32 ReadInputBits(std::span<const InputBit> bits)
{
  u32 value = UINT32_MAX;
  for (const InputBit& input : bits)
  {
    if (ArcadeInput::IsDigitalPressed(input.port, input.key) != input.active_high)
      value &= ~(UINT32_C(1) << input.bit);
  }
  return value;
}

u32 ReadBaseboardPlayerInputs(RuntimeState& runtime)
{
  u32 value = ReadInputBits(runtime.cabinet_inputs->in1);

  const auto clear_if_pressed = [&value](bool pressed, u32 bit) {
    if (pressed)
      value &= ~(UINT32_C(1) << bit);
  };

  // Two physical coin inputs occupy bits 29/28 respectively.
  clear_if_pressed(ArcadeInput::IsDigitalPressed(0, "Coin"), 29);
  clear_if_pressed(ArcadeInput::IsDigitalPressed(1, "Coin"), 28);

  // The System 10 TEST input is a maintained slide switch. Convert the normal
  // momentary ArcadeDuck operator binding to a latched state on each press.
  const bool test_pressed = ArcadeInput::IsOperatorPressed("Test");
  if (test_pressed && !runtime.test_switch_was_pressed)
    runtime.test_switch_latched = !runtime.test_switch_latched;
  runtime.test_switch_was_pressed = test_pressed;

  clear_if_pressed(runtime.test_switch_latched, 30);
  clear_if_pressed(ArcadeInput::IsOperatorPressed("Service"), 31);

  return value;
}

bool ReadBaseboardPlayerRegister(RuntimeState& runtime, u32 width, u32 offset, u32* value)
{
  if (offset < EXP3_PLAYER_INPUT_BASE ||
      offset >= (EXP3_PLAYER_INPUT_BASE + EXP3_PLAYER_INPUT_SIZE))
  {
    return false;
  }

  const u32 input_value = ReadBaseboardPlayerInputs(runtime);
  const bool handled =
    ReadU32Register(input_value, offset - EXP3_PLAYER_INPUT_BASE, width, value);

  if (handled && runtime.baseboard_input_trace_events < BASEBOARD_INPUT_TRACE_LIMIT)
  {
    INFO_LOG("System10 base-board input read offset=0x{:06X} width={} register=0x{:08X} value=0x{:08X}",
             offset, width, input_value, *value);
    runtime.baseboard_input_trace_events++;
  }

  return handled;
}

TickCount GetBaseboardIOIntervalTicks()
{
  return std::max<TickCount>(
    System::ScaleTicksToOverclock(System::MASTER_CLOCK / BASEBOARD_IO_INTERRUPT_HZ), 1);
}

void BaseboardIOEventCallback(void*, TickCount, TickCount)
{
  if (!s_runtime.has_value())
    return;

  RuntimeState& runtime = s_runtime.value();
  runtime.baseboard_interrupt_status =
    static_cast<u16>(runtime.baseboard_interrupt_status | BASEBOARD_IO_INTERRUPT_BIT);

  // ArcadeDuck's PSX interrupt controller is edge-triggered. Pulse IRQ10 for
  // each 100 Hz board event while keeping the System 10 cause bit latched until
  // the guest acknowledges the corresponding board register.
  InterruptController::SetLineState(InterruptController::IRQ::IRQ10, true);
  InterruptController::SetLineState(InterruptController::IRQ::IRQ10, false);

  if (runtime.baseboard_irq_trace_events < BASEBOARD_IRQ_TRACE_LIMIT)
  {
    INFO_LOG("System10 base-board I/O IRQ event={} status=0x{:04X}",
             runtime.baseboard_irq_trace_events, runtime.baseboard_interrupt_status);
    runtime.baseboard_irq_trace_events++;
  }
}

u16 ReadEXIOAnalog(void*, u32 index)
{
  if (!s_runtime.has_value())
    return 0;

  const u32 player = index / 4;
  switch (s_runtime->cabinet_inputs->analog)
  {
    case CabinetAnalog::Sticks:
    {
      // Per player: left stick X/Y, right stick X/Y. Left Y and right X are wired reversed.
      static constexpr std::array<std::string_view, 4> AXES = {"LeftStickX", "LeftStickY", "RightStickX",
                                                               "RightStickY"};
      float value = std::clamp(ArcadeInput::GetAnalogValue(player, AXES[index % 4]), 0.0f, 1.0f);
      if ((index % 4) == 1 || (index % 4) == 2)
        value = 1.0f - value;
      return static_cast<u16>(value * 1023.0f + 0.5f);
    }

    case CabinetAnalog::Drums:
    {
      // Per player: right rim, right surface, left surface, left rim.
      static constexpr std::array<std::string_view, 4> PADS = {"Button1", "Button2", "Button3", "Button4"};
      return ArcadeInput::IsDigitalPressed(player, PADS[index % 4]) ? 0x3ff : 0x000;
    }

    default:
      return 0x200;
  }
}

u32 ReadEXIOLightgun(void*, u32 index)
{
  // Gun counters span 0-0x13F horizontally and 0-0xEF vertically. Off-screen
  // reports X=0, Y=0xF0.
  const u32 player = index / 2;
  const bool x_axis = (index & 1) == 0;
  const ArcadeInput::LightgunPresentationState state = ArcadeInput::GetLightgunPresentationState();
  if (player >= state.ports.size() || state.ports[player].offscreen)
    return x_axis ? 0x000 : 0x0f0;

  const float position = std::clamp(x_axis ? state.ports[player].x : state.ports[player].y, 0.0f, 1.0f);
  const float maximum = x_axis ? 319.0f : 239.0f;
  return static_cast<u32>(position * maximum + 0.5f);
}

u32 ReadEXIOInput(void*, u32 index)
{
  if (!s_runtime.has_value() || index != 0)
    return UINT32_C(0xffffffff);
  return ReadInputBits(s_runtime->cabinet_inputs->exio_in1);
}

void EXIOSerialTransmit(void*, u32 channel, u8 value)
{
  if (!s_runtime.has_value() || channel != 1 || !s_runtime->cyberlead_cabinet)
    return;

  RuntimeState& runtime = *s_runtime;
  if (!runtime.jvs_bus.FeedByte(value))
    return;

  const Arcade::JVS::NamcoCyberLead::PacketResult result = runtime.cyberlead.ProcessPacket(runtime.jvs_bus.GetPacket());
  runtime.jvs_bus.ClearPacket();
  if (result.reset_requested)
  {
    runtime.cyberlead.Reset();
    runtime.jvs_addressed = false;
  }
  if (result.initialize_sense_after_response)
    runtime.jvs_addressed = true;
  if (result.response_length == 0)
    return;

  runtime.jvs_bus.QueueResponse(std::span<const u8>(result.response.data(), result.response_length));
  while (runtime.jvs_bus.HasResponseByte())
    runtime.exio->ReceiveSerial(1, runtime.jvs_bus.ReadResponseByte());
}

u16 ReadJVSSense(void*)
{
  // A JVS device holds sense at 2.5 V until it has an address, then pulls it to 0 V.
  if (!s_runtime.has_value() || !s_runtime->cyberlead_cabinet)
    return 0x3ff;
  return s_runtime->jvs_addressed ? 0x000 : 0x200;
}

void CabinetIOEventCallback(void*, TickCount ticks, TickCount)
{
  if (s_runtime.has_value() && s_runtime->cyberlead_cabinet && ticks > 0)
    s_runtime->cyberlead.AdvanceLED(static_cast<u32>(ticks), static_cast<u32>(System::GetTicksPerSecond()));
}

bool HasCyberLeadCabinet(std::string_view set_name)
{
  const Arcade::Database::GameDefinition* game = Arcade::Database::GetGame(set_name);
  if (!game)
    return false;

  return std::any_of(game->properties.begin(), game->properties.end(), [](const Arcade::Database::Property& p) {
    return p.name == "ioBoard" && p.value == "cyberlead";
  });
}

void CreateExpansionBoard(RuntimeState& runtime)
{
  runtime.exio.reset();
  runtime.cabinet_inputs = &FindCabinetInputs(runtime.set_name);
  runtime.cyberlead_cabinet = HasCyberLeadCabinet(runtime.set_name);

  // A Cyber Lead cabinet connects its JVS I/O through a full EXIO board.
  ExpansionBoard type = runtime.memn_profile ? runtime.memn_profile->expansion : ExpansionBoard::None;
  if (runtime.cyberlead_cabinet && type == ExpansionBoard::None)
    type = ExpansionBoard::EXIO;
  if (type != ExpansionBoard::EXIO && type != ExpansionBoard::EXIOG)
  {
    runtime.cyberlead_cabinet = false;
    return;
  }

  runtime.jvs_bus.Reset();
  runtime.cyberlead.Reset();
  runtime.jvs_addressed = false;
  if (runtime.cyberlead_cabinet)
  {
    // The LED PCB keeps the data games program into it with the set's NVRAM.
    const std::string flash_path =
      Path::Combine(Path::Combine(Path::Combine(EmuFolders::DataRoot, "nvram"), runtime.set_name), "cl1-leda.ic5");
    const std::vector<u8> factory_firmware = Arcade::JVS::NamcoCyberLead::LoadFactoryLEDFirmware();
    runtime.cyberlead.InitializeLEDFlash(std::span<const u8>(factory_firmware), flash_path);
    Arcade::JVS::NamcoCyberLead::SetActive(&runtime.cyberlead);

    const TickCount interval = std::max<TickCount>(1, System::GetTicksPerSecond() / 1000);
    s_cabinet_io_event =
      std::make_unique<TimingEvent>("Namco System 10 cabinet I/O", interval, interval, CabinetIOEventCallback, nullptr);
    s_cabinet_io_event->SetPeriodAndSchedule(interval);
  }

  EXIOBoard::Cabinet cabinet;
  cabinet.analog = &ReadEXIOAnalog;
  cabinet.lightgun = &ReadEXIOLightgun;
  cabinet.input = &ReadEXIOInput;
  cabinet.serial_transmit = &EXIOSerialTransmit;
  cabinet.jvs_sense = &ReadJVSSense;
  runtime.exio = std::make_unique<EXIOBoard>(type, cabinet);
  runtime.exio->Reset();
}

void ResetBaseboardInterruptState(RuntimeState& runtime, bool schedule_event)
{
  runtime.baseboard_interrupt_status = 0;
  runtime.baseboard_irq_trace_events = 0;
  InterruptController::SetLineState(InterruptController::IRQ::IRQ10, false);

  if (s_baseboard_io_event)
    s_baseboard_io_event->Deactivate();

  if (!schedule_event)
    return;

  const TickCount interval = GetBaseboardIOIntervalTicks();
  if (!s_baseboard_io_event)
  {
    s_baseboard_io_event =
      std::make_unique<TimingEvent>("Namco System 10 Baseboard I/O", interval, interval,
                                    BaseboardIOEventCallback, nullptr);
  }
  else
  {
    s_baseboard_io_event->SetPeriod(interval);
    s_baseboard_io_event->SetInterval(interval);
  }

  s_baseboard_io_event->Schedule(interval);
}

bool HandleBaseboardInterruptAck(RuntimeState& runtime, u32 width, u32 offset)
{
  if (width != 2 || offset < EXP3_INTERRUPT_ACK_BASE || offset >= EXP3_INTERRUPT_ACK_END ||
      ((offset - EXP3_INTERRUPT_ACK_BASE) & 1u) != 0)
  {
    return false;
  }

  const u32 bit_index = (offset - EXP3_INTERRUPT_ACK_BASE) / 2;
  runtime.baseboard_interrupt_status =
    static_cast<u16>(runtime.baseboard_interrupt_status & ~(UINT16_C(1) << bit_index));

  if (runtime.baseboard_irq_trace_events < BASEBOARD_IRQ_TRACE_LIMIT)
  {
    INFO_LOG("System10 base-board IRQ ACK offset=0x{:06X} bit={} status=0x{:04X}",
             offset, bit_index, runtime.baseboard_interrupt_status);
    runtime.baseboard_irq_trace_events++;
  }

  return true;
}

bool HandleType2SwitchWrite(RuntimeState& runtime, u32 width, u32 offset, u32 value)
{
  if (offset != TYPE2_SWITCH_BASE && offset != TYPE2_SWITCH_ALIAS_BASE)
    return false;

  if (width != 2 && width != 4)
    return false;

  const u16 reg = static_cast<u16>(value);
  if ((reg & UINT16_C(0x8000)) != 0)
  {
    runtime.type2_enabled = true;
    runtime.type2_iv = static_cast<u8>(reg & UINT16_C(0x000f));
    runtime.type2_generation++;
    runtime.type2_transform_supported = runtime.memn_profile && runtime.memn_profile->type2;
    if (runtime.type2_transform_supported)
      InitializeType2State(runtime, runtime.type2_iv);
    else
      ResetType2State(runtime);

    INFO_LOG("System10 Type2 activate generation={} alias=0x{:06X} value=0x{:04X} iv={} supported={}",
             runtime.type2_generation, offset, reg, runtime.type2_iv,
             runtime.type2_transform_supported);
  }
  else
  {
    INFO_LOG("System10 Type2 disable generation={} alias=0x{:06X} value=0x{:04X} prior_iv={}",
             runtime.type2_generation, offset, reg, runtime.type2_iv);
    runtime.type2_enabled = false;
    runtime.type2_transform_supported = false;
    ResetType2State(runtime);
  }

  return true;
}

bool ShouldApplyStaticTransform(const RuntimeState& runtime, const MemNRawNAND& nand)
{
  if (!nand.IsArrayReadActive())
    return false;

  // STRONG INFERENCE, preserved explicitly: NAND0 block 0 is the platform
  // remap table and block 1 is the writable/settings area. Reference behavior
  // treats these two platform blocks as plaintext while ordinary storage uses
  // the board's static transform.
  if (runtime.nand_device == 0 && nand.GetCurrentBlock() < 2)
    return false;

  return runtime.memn_profile != nullptr;
}

bool ReadVectorWordLE(std::span<const u8> bytes, u64 word_index, u16* value)
{
  if (!value)
    return false;

  const u64 byte_offset = word_index * 2;
  if (byte_offset + 1 >= bytes.size())
    return false;

  *value = static_cast<u16>(static_cast<u16>(bytes[static_cast<size_t>(byte_offset)]) |
                            (static_cast<u16>(bytes[static_cast<size_t>(byte_offset + 1)]) << 8));
  return true;
}

u32 GetMemMFlashAbsoluteWordAddress(const RuntimeState& runtime, u32 range_word_offset)
{
  return (runtime.memm_bank * UINT32_C(0x100000)) + range_word_offset;
}

u32 GetMemMFlashBlockIndex(u32 word_address)
{
  return word_address / MEMM_FLASH_BLOCK_WORDS;
}

bool ReadMemMFlashArrayWord(const RuntimeState& runtime, u32 word_address, u16* value)
{
  if (!value || word_address >= MEMM_FLASH_WORD_COUNT)
    return false;

  return ReadVectorWordLE(std::span<const u8>(runtime.memm_program_flash.data(),
                                               runtime.memm_program_flash.size()),
                          word_address, value);
}

bool WriteMemMFlashArrayWord(RuntimeState& runtime, u32 word_address, u16 value)
{
  if (word_address >= MEMM_FLASH_WORD_COUNT)
    return false;

  const size_t byte_offset = static_cast<size_t>(word_address) * 2;
  const u16 current = static_cast<u16>(static_cast<u16>(runtime.memm_program_flash[byte_offset]) |
                                       (static_cast<u16>(runtime.memm_program_flash[byte_offset + 1]) << 8));

  // NOR programming can only change erased 1 bits to 0 bits. A 0->1 request
  // requires an erase and is reported as a program error.
  if ((value & static_cast<u16>(~current)) != 0)
    return false;

  const u16 programmed = static_cast<u16>(current & value);
  if (programmed != current)
  {
    runtime.memm_program_flash[byte_offset] = static_cast<u8>(programmed);
    runtime.memm_program_flash[byte_offset + 1] = static_cast<u8>(programmed >> 8);
    runtime.memm_flash_dirty = true;
  }
  return true;
}

bool LoadMemMProgramFlashPersistence(RuntimeState& runtime, Error* error)
{
  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  runtime.memm_persistence_directory = Path::Combine(nvram_root, runtime.set_name);
  runtime.memm_flash_path = Path::Combine(runtime.memm_persistence_directory, "28f640j5");
  runtime.memm_flash_dirty = false;

  if (!FileSystem::FileExists(runtime.memm_flash_path.c_str()))
    return true;

  std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(runtime.memm_flash_path.c_str(), error);
  if (!data || data->size() != MEMM_PROGRAM_FLASH_SIZE)
  {
    const size_t actual_size = data ? data->size() : 0;
    Error::SetStringFmt(error, "Namco System 10 MEM(M) flash '{}' has invalid size {}; expected {} bytes.",
                        runtime.memm_flash_path, actual_size, MEMM_PROGRAM_FLASH_SIZE);
    ERROR_LOG("System 10 MEM(M) flash malformed persistence canonical_set='{}' path='{}' actual_size={} "
              "expected_size={}.",
              runtime.set_name, runtime.memm_flash_path, actual_size, MEMM_PROGRAM_FLASH_SIZE);
    return false;
  }

  std::copy(data->begin(), data->end(), runtime.memm_program_flash.begin());
  VERBOSE_LOG("System 10 MEM(M) flash loaded canonical_set='{}' path='{}'.",
              runtime.set_name, runtime.memm_flash_path);
  return true;
}

void SaveMemMProgramFlashPersistence(RuntimeState& runtime)
{
  if (!runtime.memm_flash_dirty)
    return;

  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  if (!FileSystem::CreateDirectory(nvram_root.c_str(), false) ||
      !FileSystem::CreateDirectory(runtime.memm_persistence_directory.c_str(), false) ||
      !FileSystem::WriteBinaryFile(runtime.memm_flash_path.c_str(), runtime.memm_program_flash.data(),
                                   runtime.memm_program_flash.size()))
  {
    ERROR_LOG("System 10 MEM(M) flash save failed canonical_set='{}' path='{}'.",
              runtime.set_name, runtime.memm_flash_path);
    return;
  }

  runtime.memm_flash_dirty = false;
  VERBOSE_LOG("System 10 MEM(M) flash saved canonical_set='{}' path='{}'.",
              runtime.set_name, runtime.memm_flash_path);
}

bool LoadMemNPersistence(RuntimeState& runtime, Error* error)
{
  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  const std::string persistence_directory = Path::Combine(nvram_root, runtime.set_name);

  for (size_t index = 0; index < runtime.nands.size(); index++)
  {
    const std::string path = Path::Combine(persistence_directory, std::string("nand") + std::to_string(index));
    if (!FileSystem::FileExists(path.c_str()))
      continue;

    std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(path.c_str(), error);
    const size_t expected_size = runtime.nands[index].GetRawImage().size();
    if (!data || data->size() != expected_size)
    {
      const size_t actual_size = data ? data->size() : 0;
      Error::SetStringFmt(error, "Namco System 10 MEM(N) NAND '{}' has invalid size {}; expected {} bytes.",
                          path, actual_size, expected_size);
      ERROR_LOG("System 10 MEM(N) malformed persistence canonical_set='{}' nand={} path='{}' actual_size={} "
                "expected_size={}.",
                runtime.set_name, index, path, actual_size, expected_size);
      return false;
    }

    std::vector<u8> raw_image(data->begin(), data->end());
    if (!runtime.nands[index].Load(std::move(raw_image), error))
      return false;

    VERBOSE_LOG("System 10 MEM(N) NAND loaded canonical_set='{}' nand={} path='{}'.",
                runtime.set_name, index, path);
  }

  return true;
}

void SaveMemNPersistence(RuntimeState& runtime)
{
  bool has_dirty_nand = false;
  for (const MemNRawNAND& nand : runtime.nands)
    has_dirty_nand |= nand.IsDirty();

  if (!has_dirty_nand)
    return;

  const std::string nvram_root(Path::Combine(EmuFolders::DataRoot, "nvram"));
  const std::string persistence_directory = Path::Combine(nvram_root, runtime.set_name);
  if (!FileSystem::CreateDirectory(nvram_root.c_str(), false) ||
      !FileSystem::CreateDirectory(persistence_directory.c_str(), false))
  {
    ERROR_LOG("System 10 MEM(N) persistence directory creation failed canonical_set='{}' path='{}'.",
              runtime.set_name, persistence_directory);
    return;
  }

  for (size_t index = 0; index < runtime.nands.size(); index++)
  {
    MemNRawNAND& nand = runtime.nands[index];
    if (!nand.IsDirty())
      continue;

    const std::string path = Path::Combine(persistence_directory, std::string("nand") + std::to_string(index));
    const std::span<const u8> raw_image = nand.GetRawImage();
    if (!FileSystem::WriteBinaryFile(path.c_str(), raw_image.data(), raw_image.size()))
    {
      ERROR_LOG("System 10 MEM(N) NAND save failed canonical_set='{}' nand={} path='{}'.",
                runtime.set_name, index, path);
      continue;
    }

    nand.ClearDirty();
    VERBOSE_LOG("System 10 MEM(N) NAND saved canonical_set='{}' nand={} path='{}'.",
                runtime.set_name, index, path);
  }
}

void ResetIntel28F640J5Interface(RuntimeState& runtime)
{
  runtime.memm_flash.mode = Intel28F640J5Mode::Array;
  runtime.memm_flash.status = MEMM_FLASH_STATUS_READY;
  runtime.memm_flash.setup_word_address = 0;
  runtime.memm_flash.write_buffer_block_word = 0;
  runtime.memm_flash.write_buffer_start_word = 0;
  runtime.memm_flash.write_buffer_count = 0;
  runtime.memm_flash.write_buffer_received = 0;
  runtime.memm_flash.write_buffer.fill(UINT16_C(0xffff));

  // J5 lock bits power up clear. They are volatile device state; the flash
  // array contents themselves are deliberately not reset here.
  runtime.memm_flash.master_lock = false;
  runtime.memm_flash.block_locked.fill(false);
  runtime.memm_flash.sts_configuration = 0;
}

u16 ReadIntel28F640J5Identifier(const RuntimeState& runtime, u32 word_address)
{
  switch (word_address & UINT32_C(0xff))
  {
    case 0:
      return MEMM_FLASH_MAKER_ID;

    case 1:
      return MEMM_FLASH_DEVICE_ID;

    case 2:
    {
      const u32 block = GetMemMFlashBlockIndex(word_address);
      return (block < MEMM_FLASH_BLOCK_COUNT && runtime.memm_flash.block_locked[block]) ?
               UINT16_C(0x0001) :
               UINT16_C(0x0000);
    }

    case 3:
      return runtime.memm_flash.master_lock ? UINT16_C(0x0001) : UINT16_C(0x0000);

    default:
      return UINT16_C(0x0000);
  }
}

u16 ReadIntel28F640J5Query(u32 word_address)
{
  // Intel 28F640J5 CFI data in x16 mode. Query values occupy the low byte;
  // the upper byte is zero.
  switch (word_address & UINT32_C(0xff))
  {
    case 0x10: return UINT16_C(0x0051); // Q
    case 0x11: return UINT16_C(0x0052); // R
    case 0x12: return UINT16_C(0x0059); // Y
    case 0x13: return UINT16_C(0x0001); // Intel command set
    case 0x14: return UINT16_C(0x0000);
    case 0x15: return UINT16_C(0x0031); // primary extended query table
    case 0x16: return UINT16_C(0x0000);
    case 0x17: return UINT16_C(0x0000); // no alternate command set
    case 0x18: return UINT16_C(0x0000);
    case 0x19: return UINT16_C(0x0000);
    case 0x1a: return UINT16_C(0x0000);

    case 0x1b: return UINT16_C(0x0045); // VCC min 4.5 V
    case 0x1c: return UINT16_C(0x0055); // VCC max 5.5 V
    case 0x1d: return UINT16_C(0x0000);
    case 0x1e: return UINT16_C(0x0000);
    case 0x1f: return UINT16_C(0x0007);
    case 0x20: return UINT16_C(0x0007);
    case 0x21: return UINT16_C(0x000a);
    case 0x22: return UINT16_C(0x0000);
    case 0x23: return UINT16_C(0x0004);
    case 0x24: return UINT16_C(0x0004);
    case 0x25: return UINT16_C(0x0004);
    case 0x26: return UINT16_C(0x0000);

    case 0x27: return UINT16_C(0x0017); // 2^23 bytes = 8 MiB
    case 0x28: return UINT16_C(0x0002); // x8/x16 async
    case 0x29: return UINT16_C(0x0000);
    case 0x2a: return UINT16_C(0x0005); // 2^5-byte write buffer
    case 0x2b: return UINT16_C(0x0000);
    case 0x2c: return UINT16_C(0x0001); // one symmetric erase region
    case 0x2d: return UINT16_C(0x003f); // 63 + 1 = 64 blocks
    case 0x2e: return UINT16_C(0x0000);
    case 0x2f: return UINT16_C(0x0000);
    case 0x30: return UINT16_C(0x0002); // 0x0200 * 256 = 128 KiB

    default:
      return UINT16_C(0x0000);
  }
}

u16 ReadIntel28F640J5Word(const RuntimeState& runtime, u32 range_word_offset)
{
  const u32 word_address = GetMemMFlashAbsoluteWordAddress(runtime, range_word_offset);

  switch (runtime.memm_flash.mode)
  {
    case Intel28F640J5Mode::Identifier:
      return ReadIntel28F640J5Identifier(runtime, word_address);

    case Intel28F640J5Mode::Query:
      return ReadIntel28F640J5Query(word_address);

    case Intel28F640J5Mode::WriteBufferCount:
      // Extended status register: write buffer available.
      return UINT16_C(0x0080);

    case Intel28F640J5Mode::Status:
    case Intel28F640J5Mode::ProgramData:
    case Intel28F640J5Mode::EraseConfirm:
    case Intel28F640J5Mode::LockConfirm:
    case Intel28F640J5Mode::ConfigurationConfirm:
    case Intel28F640J5Mode::WriteBufferData:
    case Intel28F640J5Mode::WriteBufferConfirm:
      return runtime.memm_flash.status;

    case Intel28F640J5Mode::Array:
    default:
    {
      u16 word = UINT16_C(0xffff);
      ReadMemMFlashArrayWord(runtime, word_address, &word);
      return word;
    }
  }
}

void FinishIntel28F640J5Operation(RuntimeState& runtime)
{
  runtime.memm_flash.status |= MEMM_FLASH_STATUS_READY;
  runtime.memm_flash.mode = Intel28F640J5Mode::Status;
}

void SetIntel28F640J5SequenceError(RuntimeState& runtime)
{
  runtime.memm_flash.status |=
    MEMM_FLASH_STATUS_READY | MEMM_FLASH_STATUS_ERASE_ERROR | MEMM_FLASH_STATUS_PROGRAM_ERROR;
  runtime.memm_flash.mode = Intel28F640J5Mode::Status;
}

void HandleIntel28F640J5Program(RuntimeState& runtime, u32 word_address, u16 value)
{
  const u32 block = GetMemMFlashBlockIndex(word_address);
  if (block >= MEMM_FLASH_BLOCK_COUNT)
  {
    runtime.memm_flash.status |= MEMM_FLASH_STATUS_PROGRAM_ERROR;
    FinishIntel28F640J5Operation(runtime);
    return;
  }

  if (runtime.memm_flash.block_locked[block])
  {
    runtime.memm_flash.status |= MEMM_FLASH_STATUS_PROGRAM_ERROR | MEMM_FLASH_STATUS_PROTECT_ERROR;
    FinishIntel28F640J5Operation(runtime);
    return;
  }

  if (!WriteMemMFlashArrayWord(runtime, word_address, value))
    runtime.memm_flash.status |= MEMM_FLASH_STATUS_PROGRAM_ERROR;

  FinishIntel28F640J5Operation(runtime);
}

void HandleIntel28F640J5Erase(RuntimeState& runtime, u32 word_address, u16 value)
{
  if ((value & UINT16_C(0x00ff)) != UINT16_C(0x00d0))
  {
    SetIntel28F640J5SequenceError(runtime);
    return;
  }

  const u32 block = GetMemMFlashBlockIndex(word_address);
  if (block >= MEMM_FLASH_BLOCK_COUNT)
  {
    runtime.memm_flash.status |= MEMM_FLASH_STATUS_ERASE_ERROR;
    FinishIntel28F640J5Operation(runtime);
    return;
  }

  if (runtime.memm_flash.block_locked[block])
  {
    runtime.memm_flash.status |= MEMM_FLASH_STATUS_ERASE_ERROR | MEMM_FLASH_STATUS_PROTECT_ERROR;
    FinishIntel28F640J5Operation(runtime);
    return;
  }

  const size_t byte_base = static_cast<size_t>(block) * MEMM_FLASH_BLOCK_SIZE;
  const auto block_begin = runtime.memm_program_flash.begin() + byte_base;
  const bool changed = std::any_of(block_begin, block_begin + MEMM_FLASH_BLOCK_SIZE,
                                   [](u8 value) { return value != UINT8_C(0xff); });
  std::fill_n(block_begin, MEMM_FLASH_BLOCK_SIZE, UINT8_C(0xff));
  runtime.memm_flash_dirty |= changed;
  FinishIntel28F640J5Operation(runtime);
}

void HandleIntel28F640J5LockConfirm(RuntimeState& runtime, u32 word_address, u16 value)
{
  switch (value & UINT16_C(0x00ff))
  {
    case 0x01:
    {
      const u32 block = GetMemMFlashBlockIndex(word_address);
      if (block >= MEMM_FLASH_BLOCK_COUNT)
      {
        SetIntel28F640J5SequenceError(runtime);
        return;
      }

      if (runtime.memm_flash.master_lock)
      {
        runtime.memm_flash.status |= MEMM_FLASH_STATUS_PROGRAM_ERROR | MEMM_FLASH_STATUS_PROTECT_ERROR;
      }
      else
      {
        runtime.memm_flash.block_locked[block] = true;
      }
      FinishIntel28F640J5Operation(runtime);
      return;
    }

    case 0xd0:
      if (runtime.memm_flash.master_lock)
      {
        runtime.memm_flash.status |= MEMM_FLASH_STATUS_ERASE_ERROR | MEMM_FLASH_STATUS_PROTECT_ERROR;
      }
      else
      {
        runtime.memm_flash.block_locked.fill(false);
      }
      FinishIntel28F640J5Operation(runtime);
      return;

    case 0xf1:
      // The J5 requires RP# at VHH to set the master lock bit. System 10's
      // board-level RP# drive has not been independently traced; current MAME
      // exposes this operation as available. Keep that unknown isolated here.
      runtime.memm_flash.master_lock = true;
      FinishIntel28F640J5Operation(runtime);
      return;

    default:
      SetIntel28F640J5SequenceError(runtime);
      return;
  }
}

void HandleIntel28F640J5Configuration(RuntimeState& runtime, u16 value)
{
  if ((value & UINT16_C(0xfffc)) != 0)
  {
    SetIntel28F640J5SequenceError(runtime);
    return;
  }

  runtime.memm_flash.sts_configuration = static_cast<u8>(value & UINT16_C(0x0003));
  FinishIntel28F640J5Operation(runtime);
}

void HandleIntel28F640J5WriteBufferData(RuntimeState& runtime, u32 word_address, u16 value)
{
  if (runtime.memm_flash.write_buffer_received == 0)
    runtime.memm_flash.write_buffer_start_word = word_address;

  const u32 expected_word =
    runtime.memm_flash.write_buffer_start_word + runtime.memm_flash.write_buffer_received;
  const u32 block_base = word_address & ~(MEMM_FLASH_BLOCK_WORDS - 1);

  if (word_address != expected_word || block_base != runtime.memm_flash.write_buffer_block_word ||
      runtime.memm_flash.write_buffer_received >= runtime.memm_flash.write_buffer_count)
  {
    SetIntel28F640J5SequenceError(runtime);
    return;
  }

  runtime.memm_flash.write_buffer[runtime.memm_flash.write_buffer_received] = value;
  runtime.memm_flash.write_buffer_received++;

  if (runtime.memm_flash.write_buffer_received == runtime.memm_flash.write_buffer_count)
    runtime.memm_flash.mode = Intel28F640J5Mode::WriteBufferConfirm;
}

void HandleIntel28F640J5WriteBufferConfirm(RuntimeState& runtime, u32 word_address, u16 value)
{
  if ((value & UINT16_C(0x00ff)) != UINT16_C(0x00d0) ||
      (word_address & ~(MEMM_FLASH_BLOCK_WORDS - 1)) != runtime.memm_flash.write_buffer_block_word)
  {
    SetIntel28F640J5SequenceError(runtime);
    return;
  }

  const u32 block = GetMemMFlashBlockIndex(runtime.memm_flash.write_buffer_start_word);
  if (block >= MEMM_FLASH_BLOCK_COUNT || runtime.memm_flash.block_locked[block])
  {
    runtime.memm_flash.status |= MEMM_FLASH_STATUS_PROGRAM_ERROR;
    if (block < MEMM_FLASH_BLOCK_COUNT && runtime.memm_flash.block_locked[block])
      runtime.memm_flash.status |= MEMM_FLASH_STATUS_PROTECT_ERROR;
    FinishIntel28F640J5Operation(runtime);
    return;
  }

  // Validate the complete buffer before changing the array.
  for (u32 i = 0; i < runtime.memm_flash.write_buffer_count; i++)
  {
    const u32 target_word = runtime.memm_flash.write_buffer_start_word + i;
    u16 current = UINT16_C(0xffff);
    if (!ReadMemMFlashArrayWord(runtime, target_word, &current) ||
        (runtime.memm_flash.write_buffer[i] & static_cast<u16>(~current)) != 0)
    {
      runtime.memm_flash.status |= MEMM_FLASH_STATUS_PROGRAM_ERROR;
      FinishIntel28F640J5Operation(runtime);
      return;
    }
  }

  for (u32 i = 0; i < runtime.memm_flash.write_buffer_count; i++)
  {
    const u32 target_word = runtime.memm_flash.write_buffer_start_word + i;
    WriteMemMFlashArrayWord(runtime, target_word, runtime.memm_flash.write_buffer[i]);
  }

  FinishIntel28F640J5Operation(runtime);
}

void WriteIntel28F640J5Word(RuntimeState& runtime, u32 range_word_offset, u16 value)
{
  // Current MAME forwards MEM(M) flash writes using the 2 MiB range offset
  // directly. The exact effect of the external System 10 bank selector on
  // flash write address lines is not documented, so keep that unknown isolated
  // rather than asserting an unverified high-address mapping.
  const u32 word_address = range_word_offset;
  const u8 command = static_cast<u8>(value);

  switch (runtime.memm_flash.mode)
  {
    case Intel28F640J5Mode::ProgramData:
      HandleIntel28F640J5Program(runtime, word_address, value);
      return;

    case Intel28F640J5Mode::EraseConfirm:
      HandleIntel28F640J5Erase(runtime, word_address, value);
      return;

    case Intel28F640J5Mode::LockConfirm:
      HandleIntel28F640J5LockConfirm(runtime, word_address, value);
      return;

    case Intel28F640J5Mode::ConfigurationConfirm:
      HandleIntel28F640J5Configuration(runtime, value);
      return;

    case Intel28F640J5Mode::WriteBufferCount:
    {
      const u32 count_minus_one = value & UINT16_C(0x00ff);
      if (count_minus_one >= MEMM_FLASH_WRITE_BUFFER_WORDS)
      {
        SetIntel28F640J5SequenceError(runtime);
        return;
      }

      runtime.memm_flash.write_buffer_count = count_minus_one + 1;
      runtime.memm_flash.write_buffer_received = 0;
      runtime.memm_flash.write_buffer_start_word = 0;
      runtime.memm_flash.write_buffer.fill(UINT16_C(0xffff));
      runtime.memm_flash.mode = Intel28F640J5Mode::WriteBufferData;
      return;
    }

    case Intel28F640J5Mode::WriteBufferData:
      HandleIntel28F640J5WriteBufferData(runtime, word_address, value);
      return;

    case Intel28F640J5Mode::WriteBufferConfirm:
      HandleIntel28F640J5WriteBufferConfirm(runtime, word_address, value);
      return;

    case Intel28F640J5Mode::Array:
    case Intel28F640J5Mode::Identifier:
    case Intel28F640J5Mode::Query:
    case Intel28F640J5Mode::Status:
    default:
      break;
  }

  switch (command)
  {
    case 0xff:
    case 0xf0:
      runtime.memm_flash.mode = Intel28F640J5Mode::Array;
      return;

    case 0x90:
      runtime.memm_flash.mode = Intel28F640J5Mode::Identifier;
      return;

    case 0x98:
      runtime.memm_flash.mode = Intel28F640J5Mode::Query;
      return;

    case 0x70:
      runtime.memm_flash.mode = Intel28F640J5Mode::Status;
      return;

    case 0x50:
      runtime.memm_flash.status = MEMM_FLASH_STATUS_READY;
      runtime.memm_flash.mode = Intel28F640J5Mode::Status;
      return;

    case 0x40:
    case 0x10:
      runtime.memm_flash.mode = Intel28F640J5Mode::ProgramData;
      runtime.memm_flash.setup_word_address = word_address;
      return;

    case 0x20:
      runtime.memm_flash.mode = Intel28F640J5Mode::EraseConfirm;
      runtime.memm_flash.setup_word_address = word_address;
      return;

    case 0x60:
      runtime.memm_flash.mode = Intel28F640J5Mode::LockConfirm;
      runtime.memm_flash.setup_word_address = word_address;
      return;

    case 0xb8:
      runtime.memm_flash.mode = Intel28F640J5Mode::ConfigurationConfirm;
      runtime.memm_flash.setup_word_address = word_address;
      return;

    case 0xe8:
      runtime.memm_flash.write_buffer_block_word = word_address & ~(MEMM_FLASH_BLOCK_WORDS - 1);
      runtime.memm_flash.write_buffer_count = 0;
      runtime.memm_flash.write_buffer_received = 0;
      runtime.memm_flash.mode = Intel28F640J5Mode::WriteBufferCount;
      return;

    case 0xb0:
    case 0xd0:
      // Suspend/resume only matter while the WSM is busy. Program/erase
      // operations are presently functionally completed at command completion.
      runtime.memm_flash.mode = Intel28F640J5Mode::Status;
      return;

    default:
      SetIntel28F640J5SequenceError(runtime);
      return;
  }
}

bool ReadMemMRange(RuntimeState& runtime, u32 width, u32 offset, u32* value)
{
  if (!value || offset < MEMM_RANGE_BASE || offset >= (MEMM_RANGE_BASE + MEMM_RANGE_SIZE) ||
      width > ((MEMM_RANGE_BASE + MEMM_RANGE_SIZE) - offset))
  {
    return false;
  }

  // The MEM(M) range is a 16-bit device, but the PSX CPU can issue byte reads.
  // Mr. Driller 2's software decoder does exactly that with LBU while consuming
  // compressed streams, starting at arbitrary (including odd) offsets, from both
  // the mask-ROM banks (0x10-0x1F) and the direct-read program-flash banks
  // (0x00-0x0F, statically unscrambled). A direct byte read exposes the addressed
  // byte lane of the same 16-bit word a halfword read would return and has no
  // side effects.
  //
  // Keep flash-command and active Type1 byte-read behavior out of this change:
  // both have stateful 16-bit semantics that need independent evidence before
  // defining how byte accesses affect them.
  if (width == 1)
  {
    if (!runtime.memm_direct_access || runtime.type1_enabled || runtime.memm_bank >= 0x20)
      return false;

    const u32 range_byte = offset - MEMM_RANGE_BASE;
    const u64 bank_word = static_cast<u64>(range_byte / 2);
    u16 word = UINT16_C(0xffff);
    if (runtime.memm_bank < 0x10)
    {
      if (ReadVectorWordLE(std::span<const u8>(runtime.memm_program_flash.data(),
                                               runtime.memm_program_flash.size()),
                           static_cast<u64>(runtime.memm_bank) * UINT64_C(0x100000) + bank_word, &word))
      {
        word = DecodeMemMStaticWord(runtime.memm_board_profile, word);
      }
      else
      {
        word = UINT16_C(0xffff);
      }
    }
    else if (!ReadVectorWordLE(std::span<const u8>(runtime.memm_data_rom.data(), runtime.memm_data_rom.size()),
                               static_cast<u64>(runtime.memm_bank - 0x10) * UINT64_C(0x100000) + bank_word,
                               &word))
    {
      word = UINT16_C(0xffff);
    }

    *value = static_cast<u32>((word >> ((range_byte & 1u) * 8)) & UINT16_C(0x00ff));
    return true;
  }

  if ((width != 2 && width != 4) || (offset & 1u) != 0)
    return false;

  const u32 first_word = (offset - MEMM_RANGE_BASE) / 2;
  const u32 word_count = width / 2;
  u32 result = 0;

  for (u32 i = 0; i < word_count; i++)
  {
    const u32 range_word = first_word + i;
    u16 word = UINT16_C(0xffff);

    if (!runtime.memm_direct_access)
    {
      word = ReadIntel28F640J5Word(runtime, range_word);
    }
    else
    {
      const bool program_flash = runtime.memm_bank < 0x10;
      const bool data_rom = runtime.memm_bank >= 0x10 && runtime.memm_bank < 0x20;
      if (program_flash)
      {
        const u64 bank_word = static_cast<u64>(runtime.memm_bank) * UINT64_C(0x100000) + range_word;
        if (!ReadVectorWordLE(std::span<const u8>(runtime.memm_program_flash.data(),
                                                  runtime.memm_program_flash.size()),
                              bank_word, &word))
        {
          word = UINT16_C(0xffff);
        }
      }
      else if (data_rom)
      {
        const u64 bank_word =
          static_cast<u64>(runtime.memm_bank - 0x10) * UINT64_C(0x100000) + range_word;
        if (!ReadVectorWordLE(std::span<const u8>(runtime.memm_data_rom.data(),
                                                  runtime.memm_data_rom.size()),
                              bank_word, &word))
        {
          word = UINT16_C(0xffff);
        }
      }
      else
      {
        word = UINT16_C(0xffff);
      }

      const u16 source_word = word;

      if (runtime.type1_enabled)
      {
        const u16 cipher = word;
        word = DecodeMrDriller2Type1Word(runtime.type1_state, cipher);

        if (runtime.type1_trace_words < MEMM_TRACE_EVENT_LIMIT)
        {
          INFO_LOG("System10 MEM(M) Type1 gen={} word={} iv={} bank=0x{:02X} range=0x{:06X} "
                   "cipher=0x{:04X} plain=0x{:04X} next_mask=0x{:04X} counter={}",
                   runtime.type1_generation, runtime.type1_trace_words, runtime.type1_iv,
                   runtime.memm_bank, range_word, cipher, word, runtime.type1_state.mask,
                   runtime.type1_state.counter);
          runtime.type1_trace_words++;
        }
      }
      else if (program_flash)
      {
        word = DecodeMemMStaticWord(runtime.memm_board_profile, word);
      }

    }

    result |= static_cast<u32>(word) << (i * 16);
  }

  *value = result;
  return true;
}

bool WriteMemMRange(RuntimeState& runtime, u32 width, u32 offset, u32 value)
{
  if (offset < MEMM_RANGE_BASE || offset >= (MEMM_RANGE_BASE + MEMM_RANGE_SIZE) ||
      (width != 2 && width != 4) || (offset & 1u) != 0 ||
      width > ((MEMM_RANGE_BASE + MEMM_RANGE_SIZE) - offset))
  {
    return false;
  }

  if (runtime.memm_direct_access)
  {
    // Raw/decrypted window accesses are ROM-like. Flash command writes are
    // accepted only through the direct-flash path selected by the board.
    return true;
  }

  const u32 first_word = (offset - MEMM_RANGE_BASE) / 2;
  const u32 word_count = width / 2;
  for (u32 i = 0; i < word_count; i++)
    WriteIntel28F640J5Word(runtime, first_word + i, static_cast<u16>(value >> (i * 16)));

  return true;
}

bool HandleMemMCryptoSwitchWrite(RuntimeState& runtime, u32 width, u32 offset, u32 value)
{
  if (offset != MEMM_CRYPTO_SWITCH_BASE || (width != 2 && width != 4))
    return false;

  const u16 reg = static_cast<u16>(value);
  if ((reg & UINT16_C(0x8000)) != 0)
  {
    runtime.type1_enabled = true;
    runtime.type1_iv = static_cast<u8>(reg & UINT16_C(0x000f));
    runtime.type1_generation++;
    ResetType1State(runtime, runtime.type1_iv);

    INFO_LOG("System10 MEM(M) Type1 activate generation={} value=0x{:04X} iv={} seed=0x{:04X}",
             runtime.type1_generation, reg, runtime.type1_iv, runtime.type1_state.mask);
  }
  else
  {
    INFO_LOG("System10 MEM(M) Type1 disable generation={} value=0x{:04X} prior_iv={}",
             runtime.type1_generation, reg, runtime.type1_iv);
    runtime.type1_enabled = false;
    runtime.type1_state = {};
    runtime.type1_trace_words = 0;
  }

  return true;
}

bool ReadMemMBank(RuntimeState& runtime, u32 width, u32 offset, u32* value)
{
  if (!value || offset < MEMM_BANK_EXP3_BASE || offset >= (MEMM_BANK_EXP3_BASE + MEMM_BANK_EXP3_SIZE) ||
      (width != 1 && width != 2 && width != 4))
  {
    return false;
  }

  runtime.memm_direct_access = true;
  *value = 0;

  if (runtime.memm_trace_events < MEMM_TRACE_EVENT_LIMIT)
  {
    INFO_LOG("System10 MEM(M) bank read offset=0x{:06X} width={} bank=0x{:02X} -> direct/raw",
             offset, width, runtime.memm_bank);
    runtime.memm_trace_events++;
  }
  return true;
}

bool WriteMemMBank(RuntimeState& runtime, u32 width, u32 offset, u32 value)
{
  if (offset < MEMM_BANK_EXP3_BASE || offset >= (MEMM_BANK_EXP3_BASE + MEMM_BANK_EXP3_SIZE) ||
      (width != 2 && width != 4) || (offset & 1u) != 0)
  {
    return false;
  }

  runtime.memm_bank = (offset - MEMM_BANK_EXP3_BASE) / 2;
  runtime.memm_direct_access = runtime.memm_bank >= 0x10;

  if (runtime.memm_trace_events < MEMM_TRACE_EVENT_LIMIT)
  {
    INFO_LOG("System10 MEM(M) bank write bank=0x{:02X} direct={} width={} value=0x{:08X}",
             runtime.memm_bank, runtime.memm_direct_access, width, value);
    runtime.memm_trace_events++;
  }
  return true;
}

bool MapProgramWordToNAND(u32 word_offset, u32* page, u32* column)
{
  if (!page || !column || word_offset >= PROGRAM_ROM_SIZE || (word_offset & 1u) != 0)
    return false;

  u32 source_offset;
  u32 first_page;

  if (word_offset < PROGRAM_FIRST_END)
  {
    source_offset = word_offset;
    first_page = PROGRAM_FIRST_PAGE;
  }
  else if (word_offset < PROGRAM_FILL_END)
  {
    return false;
  }
  else
  {
    source_offset = word_offset - PROGRAM_FILL_END;
    first_page = PROGRAM_SECOND_PAGE;
  }

  *page = first_page + (source_offset / MemNRawNAND::DATA_BYTES_PER_PAGE);
  *column = source_offset % MemNRawNAND::DATA_BYTES_PER_PAGE;
  return true;
}

u8 ReadProgramByte(const RuntimeState& runtime, u32 offset)
{
  if (offset >= PROGRAM_ROM_SIZE)
    return UINT8_C(0xff);

  if (runtime.media_type == MediaType::MemM)
  {
    // MEM(M) exposes the statically decoded first 0x62000 bytes of the 1A
    // program flash as the boot aperture. The rest of the 4 MiB BIOS window is
    // the board's 0x55 fill until software switches to the banked interface.
    if (runtime.memm_board_profile != MemMBoardProfile::MrDriller2 || offset >= MEMM_BOOT_DECODE_SIZE)
      return UINT8_C(0x55);

    const u32 word_offset = offset & ~UINT32_C(1);
    u16 raw_word = UINT16_C(0xffff);
    if (!ReadVectorWordLE(std::span<const u8>(runtime.memm_program_flash.data(),
                                              runtime.memm_program_flash.size()),
                          word_offset / 2, &raw_word))
    {
      return UINT8_C(0xff);
    }

    const u16 decoded_word = DecodeMemMStaticWord(runtime.memm_board_profile, raw_word);
    return static_cast<u8>((decoded_word >> ((offset & 1u) * 8)) & UINT16_C(0x00ff));
  }

  if (runtime.media_type != MediaType::MemN)
    return UINT8_C(0xff);

  if (offset >= PROGRAM_FIRST_END && offset < PROGRAM_FILL_END)
    return UINT8_C(0x55);

  const u32 word_offset = offset & ~UINT32_C(1);
  u32 page;
  u32 column;
  if (!MapProgramWordToNAND(word_offset, &page, &column))
    return UINT8_C(0xff);

  if (runtime.nands.empty())
    return UINT8_C(0xff);

  const std::span<const u8> data = runtime.nands[0].GetPageData(page);
  if (data.empty() || (column + 1) >= data.size())
    return UINT8_C(0xff);

  // Raw MEM(N) words arrive high byte first at the board. Decode the complete
  // 16-bit word, then expose the resulting program aperture in PSX little-endian
  // byte order. The immutable NAND image itself is never rewritten.
  const u16 raw_word = static_cast<u16>((static_cast<u16>(data[column]) << 8) | data[column + 1]);
  const u16 decoded_word =
    runtime.memn_profile ? DecodeMemNStaticWord(*runtime.memn_profile, raw_word, runtime.data_xor) : raw_word;
  return static_cast<u8>((decoded_word >> ((offset & 1u) * 8)) & UINT16_C(0x00ff));
}

MemNRawNAND* GetSelectedNAND(RuntimeState& runtime)
{
  if (runtime.nand_device >= runtime.nands.size())
    return nullptr;

  return &runtime.nands[runtime.nand_device];
}

const MemNRawNAND* GetSelectedNAND(const RuntimeState& runtime)
{
  if (runtime.nand_device >= runtime.nands.size())
    return nullptr;

  return &runtime.nands[runtime.nand_device];
}

bool ReadLittleEndian(std::span<const u8> bytes, u32 offset, u32 width, u32* value)
{
  if (!value || (width != 1 && width != 2 && width != 4) || offset > bytes.size() ||
      width > (bytes.size() - offset))
  {
    return false;
  }

  u32 result = 0;
  for (u32 i = 0; i < width; i++)
    result |= static_cast<u32>(bytes[offset + i]) << (i * 8);

  *value = result;
  return true;
}

bool WriteLittleEndian(std::span<u8> bytes, u32 offset, u32 width, u32 value)
{
  if ((width != 1 && width != 2 && width != 4) || offset > bytes.size() || width > (bytes.size() - offset))
    return false;

  for (u32 i = 0; i < width; i++)
    bytes[offset + i] = static_cast<u8>(value >> (i * 8));

  return true;
}

bool ReadU16Register(u16 reg, u32 byte_offset, u32 width, u32* value)
{
  if (!value || byte_offset >= sizeof(reg) || (width != 1 && width != 2) || width > (sizeof(reg) - byte_offset))
    return false;

  *value = (static_cast<u32>(reg) >> (byte_offset * 8)) & ((width == 1) ? UINT32_C(0xff) : UINT32_C(0xffff));
  return true;
}

bool ReadU32Register(u32 reg, u32 byte_offset, u32 width, u32* value)
{
  if (!value || byte_offset >= sizeof(reg) || (width != 1 && width != 2 && width != 4) ||
      width > (sizeof(reg) - byte_offset))
  {
    return false;
  }

  const u32 mask = (width == 1) ? UINT32_C(0xff) :
                   (width == 2) ? UINT32_C(0xffff) :
                                  UINT32_MAX;
  *value = (reg >> (byte_offset * 8)) & mask;
  return true;
}

bool LoadRawNANDMember(const char* archive_path, const Arcade::Database::ROMDefinition& rom,
                       std::vector<u8>* data, Error* error)
{
  const bool supported_raw_size =
    (rom.size == MemNRawNAND::K9F2808_RAW_IMAGE_SIZE || rom.size == MemNRawNAND::K9F5608_RAW_IMAGE_SIZE);
  if (!supported_raw_size || rom.offset != 0 || rom.interleave != 1 || rom.group_size != 1 ||
      rom.skip != 0 || rom.word_swap || !rom.segments.empty())
  {
    Error::SetStringFmt(error,
                        "System 10 raw NAND '{}' uses a database layout that would alter the dumped 0x210-byte "
                        "page image.",
                        rom.name);
    return false;
  }

  unzFile zf = MinizipHelpers::OpenUnzFile(archive_path);
  if (!zf)
  {
    Error::SetStringFmt(error, "Failed to open Namco System 10 set archive '{}'.", archive_path);
    return false;
  }

  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    Error::SetStringFmt(error, "Namco System 10 set archive '{}' is empty or unreadable.", archive_path);
    return false;
  }

  for (;;)
  {
    unz_file_info64 file_info = {};
    char member_name[512] = {};
    if (unzGetCurrentFileInfo64(zf, &file_info, member_name, sizeof(member_name), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed to read file information from Namco System 10 set archive '{}'.",
                          archive_path);
      return false;
    }

    member_name[sizeof(member_name) - 1] = '\0';
    if (StringUtil::EqualNoCase(member_name, rom.name))
    {
      if (file_info.uncompressed_size != rom.size)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "System 10 NAND '{}' has size {}, expected {} bytes.", rom.name,
                            file_info.uncompressed_size, rom.size);
        return false;
      }

      if (rom.has_crc32 && static_cast<u32>(file_info.crc) != rom.crc32)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "System 10 NAND '{}' has CRC32 {:08x}, expected {:08x}.", rom.name,
                            static_cast<u32>(file_info.crc), rom.crc32);
        return false;
      }

      if (unzOpenCurrentFile(zf) != UNZ_OK)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Failed to decompress System 10 NAND '{}' from '{}'.", rom.name, archive_path);
        return false;
      }

      data->resize(rom.size);
      size_t read_offset = 0;
      while (read_offset < data->size())
      {
        const int bytes_read =
          unzReadCurrentFile(zf, data->data() + read_offset, static_cast<unsigned>(data->size() - read_offset));
        if (bytes_read <= 0)
        {
          unzCloseCurrentFile(zf);
          unzClose(zf);
          Error::SetStringFmt(error, "Failed reading System 10 NAND '{}' from '{}'.", rom.name, archive_path);
          return false;
        }

        read_offset += static_cast<size_t>(bytes_read);
      }

      const int close_result = unzCloseCurrentFile(zf);
      unzClose(zf);
      if (close_result != UNZ_OK)
      {
        Error::SetStringFmt(error, "CRC validation failed for System 10 NAND '{}' in '{}'.", rom.name, archive_path);
        return false;
      }

      if (!rom.sha1.empty())
      {
        auto digest = SHA1Digest::GetDigest(std::span<const u8>(data->data(), data->size()));
        const std::string digest_string = SHA1Digest::DigestToString(digest);
        if (!StringUtil::EqualNoCase(digest_string, rom.sha1))
        {
          Error::SetStringFmt(error, "System 10 NAND '{}' has SHA-1 {}, expected {}.", rom.name, digest_string,
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
      Error::SetStringFmt(error, "Failed while reading Namco System 10 set archive '{}'.", archive_path);
      return false;
    }
  }

  unzClose(zf);
  Error::SetStringFmt(error, "Namco System 10 set archive '{}' does not contain required NAND '{}'.", archive_path,
                      rom.name);
  return false;
}

bool LoadSimpleROMMember(const char* archive_path, const Arcade::Database::ROMDefinition& rom,
                         std::span<u8> destination, Error* error)
{
  if (rom.interleave != 1 || rom.group_size != 1 || rom.skip != 0 || rom.word_swap || !rom.segments.empty() ||
      rom.offset > destination.size() || rom.size > (destination.size() - rom.offset))
  {
    Error::SetStringFmt(error, "System 10 ROM '{}' uses an unsupported layout for direct MEM(M) loading.", rom.name);
    return false;
  }

  unzFile zf = MinizipHelpers::OpenUnzFile(archive_path);
  if (!zf)
  {
    Error::SetStringFmt(error, "Failed to open Namco System 10 set archive '{}'.", archive_path);
    return false;
  }

  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    Error::SetStringFmt(error, "Namco System 10 set archive '{}' is empty or unreadable.", archive_path);
    return false;
  }

  for (;;)
  {
    unz_file_info64 file_info = {};
    char member_name[512] = {};
    if (unzGetCurrentFileInfo64(zf, &file_info, member_name, sizeof(member_name), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      unzClose(zf);
      Error::SetStringFmt(error, "Failed to read file information from Namco System 10 set archive '{}'.",
                          archive_path);
      return false;
    }

    member_name[sizeof(member_name) - 1] = '\0';
    if (StringUtil::EqualNoCase(member_name, rom.name))
    {
      if (file_info.uncompressed_size != rom.size)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "System 10 ROM '{}' has size {}, expected {} bytes.", rom.name,
                            file_info.uncompressed_size, rom.size);
        return false;
      }

      if (rom.has_crc32 && static_cast<u32>(file_info.crc) != rom.crc32)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "System 10 ROM '{}' has CRC32 {:08x}, expected {:08x}.", rom.name,
                            static_cast<u32>(file_info.crc), rom.crc32);
        return false;
      }

      if (unzOpenCurrentFile(zf) != UNZ_OK)
      {
        unzClose(zf);
        Error::SetStringFmt(error, "Failed to decompress System 10 ROM '{}' from '{}'.", rom.name, archive_path);
        return false;
      }

      std::span<u8> output = destination.subspan(rom.offset, rom.size);
      size_t read_offset = 0;
      while (read_offset < output.size())
      {
        const int bytes_read =
          unzReadCurrentFile(zf, output.data() + read_offset, static_cast<unsigned>(output.size() - read_offset));
        if (bytes_read <= 0)
        {
          unzCloseCurrentFile(zf);
          unzClose(zf);
          Error::SetStringFmt(error, "Failed reading System 10 ROM '{}' from '{}'.", rom.name, archive_path);
          return false;
        }
        read_offset += static_cast<size_t>(bytes_read);
      }

      const int close_result = unzCloseCurrentFile(zf);
      unzClose(zf);
      if (close_result != UNZ_OK)
      {
        Error::SetStringFmt(error, "CRC validation failed for System 10 ROM '{}' in '{}'.", rom.name, archive_path);
        return false;
      }

      if (!rom.sha1.empty())
      {
        auto digest = SHA1Digest::GetDigest(std::span<const u8>(output.data(), output.size()));
        const std::string digest_string = SHA1Digest::DigestToString(digest);
        if (!StringUtil::EqualNoCase(digest_string, rom.sha1))
        {
          Error::SetStringFmt(error, "System 10 ROM '{}' has SHA-1 {}, expected {}.", rom.name, digest_string,
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
      Error::SetStringFmt(error, "Failed while reading Namco System 10 set archive '{}'.", archive_path);
      return false;
    }
  }

  unzClose(zf);
  Error::SetStringFmt(error, "Namco System 10 set archive '{}' does not contain required ROM '{}'.", archive_path,
                      rom.name);
  return false;
}

} // namespace

std::optional<BoardProfile> FindBoardProfile(std::string_view hardware_profile)
{
  if (hardware_profile == "ns10_mrdrilr2")
    return BoardProfile{hardware_profile, BoardKind::MemM, nullptr, MemMBoardProfile::MrDriller2};

  if (const MemNProfile* memn_profile = FindMemNProfile(hardware_profile))
    return BoardProfile{hardware_profile, BoardKind::MemN, memn_profile, MemMBoardProfile::Unknown};

  return std::nullopt;
}

std::optional<MemMLoadedContent> LoadMrDriller2Content(const char* archive_path,
                                                       const Arcade::Database::GameDefinition& game, Error* error)
{
  if (game.hardware_profile != "ns10_mrdrilr2")
  {
    Error::SetStringFmt(error, "System 10 MEM(M) loader requires profile 'ns10_mrdrilr2'; requested '{}'.",
                        game.hardware_profile);
    return std::nullopt;
  }

  if (game.id != "mrdrilr2" && game.id != "mrdrilr2j" && game.id != "mrdrilr2u")
  {
    Error::SetStringFmt(error, "System 10 MEM(M) bring-up does not recognize set '{}'.", game.id);
    return std::nullopt;
  }

  const Arcade::Database::ROMDefinition* flash_rom = nullptr;
  std::vector<const Arcade::Database::ROMDefinition*> data_roms;

  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "nand")
    {
      if (flash_rom)
      {
        Error::SetStringView(error, "Mr. Driller 2 database entry contains more than one MEM(M) program flash ROM.");
        return std::nullopt;
      }
      flash_rom = &rom;
    }
    else if (rom.region == "data")
    {
      data_roms.push_back(&rom);
    }
  }

  if (!flash_rom || flash_rom->offset != 0 || flash_rom->size != MEMM_PROGRAM_FLASH_SIZE)
  {
    Error::SetStringView(error, "Mr. Driller 2 requires one 8 MiB MEM(M) program flash ROM in region 'nand'.");
    return std::nullopt;
  }

  if (data_roms.empty())
  {
    Error::SetStringView(error, "Mr. Driller 2 requires MEM(M) mask-ROM data in region 'data'.");
    return std::nullopt;
  }

  MemMLoadedContent content;
  content.set_name = game.id;
  content.board_profile = MemMBoardProfile::MrDriller2;
  content.program_flash.resize(MEMM_PROGRAM_FLASH_SIZE, UINT8_C(0xff));
  content.data_rom.resize(MEMM_DATA_ROM_SIZE, UINT8_C(0xff));

  if (!LoadSimpleROMMember(archive_path, *flash_rom,
                           std::span<u8>(content.program_flash.data(), content.program_flash.size()), error))
  {
    return std::nullopt;
  }

  for (const Arcade::Database::ROMDefinition* rom : data_roms)
  {
    if (!LoadSimpleROMMember(archive_path, *rom,
                             std::span<u8>(content.data_rom.data(), content.data_rom.size()), error))
    {
      return std::nullopt;
    }
  }

  INFO_LOG("Loaded Namco System 10 Mr. Driller 2 MEM(M) set='{}' flash={} bytes data={} bytes.",
           content.set_name, content.program_flash.size(), content.data_rom.size());
  return content;
}

std::optional<MemNLoadedContent> LoadMemNContent(const char* archive_path, const Arcade::Database::GameDefinition& game,
                                                Error* error)
{
  const std::optional<BoardProfile> profile = FindBoardProfile(game.hardware_profile);
  if (!profile || profile->kind != BoardKind::MemN || !profile->memn_profile)
  {
    Error::SetStringFmt(error, "System 10 MEM(N) loader has no supported profile '{}'.",
                        game.hardware_profile);
    return std::nullopt;
  }

  static constexpr size_t MAX_MEMN_NANDS = 16;
  std::array<const Arcade::Database::ROMDefinition*, MAX_MEMN_NANDS> nand_roms = {};
  size_t nand_region_count = 0;
  const Arcade::Database::ROMDefinition* nonlinear_rom = nullptr;

  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "decrypter:nonlinear_table")
    {
      nonlinear_rom = &rom;
      continue;
    }

    if (rom.region.size() < 5 || rom.region.size() > 6 || rom.region.compare(0, 4, "nand") != 0)
      continue;

    const std::optional<u32> region_index = StringUtil::FromChars<u32>(std::string_view(rom.region).substr(4));
    if (!region_index.has_value() || region_index.value() >= MAX_MEMN_NANDS)
      continue;

    const size_t index = region_index.value();
    if (nand_roms[index])
    {
      Error::SetStringFmt(error, "System 10 MEM(N) profile '{}' defines duplicate region '{}'.",
                          game.hardware_profile, rom.region);
      return std::nullopt;
    }

    nand_roms[index] = &rom;
    nand_region_count++;
  }

  if (nand_region_count < 2 || !nand_roms[0] || !nand_roms[1])
  {
    Error::SetStringFmt(error,
                        "System 10 MEM(N) profile '{}' requires sequential raw NAND regions beginning at "
                        "nand0/nand1.",
                        game.hardware_profile);
    return std::nullopt;
  }

  for (size_t index = 0; index < nand_region_count; index++)
  {
    if (!nand_roms[index])
    {
      Error::SetStringFmt(error, "System 10 MEM(N) profile '{}' is missing nand{} before its final NAND region.",
                          game.hardware_profile, index);
      return std::nullopt;
    }
  }

  MemNLoadedContent content;
  content.set_name = game.id;
  content.profile = profile->memn_profile;
  content.nands.resize(nand_region_count);

  for (size_t index = 0; index < nand_region_count; index++)
  {
    if (!LoadRawNANDMember(archive_path, *nand_roms[index], &content.nands[index], error))
      return std::nullopt;
  }

  const Type2Logic* type2 = content.profile->type2;
  if (type2 && type2->nonlinear_table_shift != 0)
  {
    if (!nonlinear_rom || nonlinear_rom->offset != 0 || nonlinear_rom->size == 0)
    {
      Error::SetStringFmt(error, "System 10 MEM(N) profile '{}' requires its Type-2 nonlinear table ROM.",
                          game.hardware_profile);
      return std::nullopt;
    }

    content.nonlinear_table.resize(nonlinear_rom->size);
    if (!LoadSimpleROMMember(archive_path, *nonlinear_rom, std::span<u8>(content.nonlinear_table), error))
      return std::nullopt;
  }

  VERBOSE_LOG("Loaded Namco System 10 MEM(N) profile='{}' raw NANDs from '{}': count={}.",
              game.hardware_profile, archive_path, content.nands.size());
  return content;
}

std::optional<MemNLoadedContent> LoadExperimentalMemNProbeContent(
  const char* archive_path, const Arcade::Database::GameDefinition& game, bool* eligible, Error* error)
{
  *eligible = false;

  static constexpr size_t MAX_MEMN_NANDS = 8;
  std::array<const Arcade::Database::ROMDefinition*, MAX_MEMN_NANDS> nand_roms = {};
  size_t nand_region_count = 0;

  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region.size() != 5 || rom.region.compare(0, 4, "nand") != 0 ||
        rom.region[4] < '0' || rom.region[4] > '7')
    {
      continue;
    }

    const size_t index = static_cast<size_t>(rom.region[4] - '0');
    if (nand_roms[index])
      return std::nullopt;

    nand_roms[index] = &rom;
    nand_region_count++;
  }

  if (nand_region_count < 2 || !nand_roms[0] || !nand_roms[1])
    return std::nullopt;

  for (size_t index = 0; index < nand_region_count; index++)
  {
    if (!nand_roms[index] ||
        (nand_roms[index]->size != MemNRawNAND::K9F2808_RAW_IMAGE_SIZE &&
         nand_roms[index]->size != MemNRawNAND::K9F5608_RAW_IMAGE_SIZE))
    {
      return std::nullopt;
    }
  }

  *eligible = true;
  MemNLoadedContent content;
  content.set_name = game.id;
  content.profile = nullptr;
  content.nands.resize(nand_region_count);

  for (size_t index = 0; index < nand_region_count; index++)
  {
    if (!LoadRawNANDMember(archive_path, *nand_roms[index], &content.nands[index], error))
      return std::nullopt;
  }

  WARNING_LOG("Namco System 10 experimental MEM(N) probe set='{}' profile='{}': loading {} raw NAND regions with "
              "raw word semantics only; profile crypto and board-specific devices remain unmodeled.",
              game.id, game.hardware_profile, nand_region_count);
  return content;
}

bool InitializeMemN(MemNLoadedContent content, Error* error)
{
  if (s_runtime.has_value())
  {
    Error::SetStringView(error, "Namco System 10 runtime is already active.");
    return false;
  }

  if (content.set_name.empty())
  {
    Error::SetStringView(error, "Namco System 10 MEM(N) content has no set identity.");
    return false;
  }

  RuntimeState runtime;
  runtime.set_name = std::move(content.set_name);
  runtime.media_type = MediaType::MemN;
  runtime.memn_profile = content.profile;
  runtime.data_xor = content.profile ? content.profile->data_xor : UINT16_C(0xaaaa);
  runtime.type2_nonlinear_table = std::move(content.nonlinear_table);

  if (content.nands.size() < 2)
  {
    Error::SetStringView(error, "Namco System 10 MEM(N) content must contain at least two NAND devices.");
    return false;
  }

  runtime.nands.resize(content.nands.size());
  for (size_t index = 0; index < content.nands.size(); index++)
  {
    if (!runtime.nands[index].Load(std::move(content.nands[index]), error))
      return false;
  }

  if (!LoadMemNPersistence(runtime, error))
    return false;

  runtime.lookup_ram.resize(LOOKUP_RAM_SIZE, 0);

  ResetI2CState(runtime);
  ResetBaseboardInputState(runtime);

  VERBOSE_LOG("Namco System 10 MEM(N) runtime initialized set='{}' NAND devices={}.", runtime.set_name,
              runtime.nands.size());

  s_runtime = std::move(runtime);
  CreateExpansionBoard(*s_runtime);
  ResetBaseboardInterruptState(*s_runtime, true);
  return true;
}

bool InitializeMemM(MemMLoadedContent content, Error* error)
{
  if (s_runtime.has_value())
  {
    Error::SetStringView(error, "Namco System 10 runtime is already active.");
    return false;
  }

  if (content.set_name.empty())
  {
    Error::SetStringView(error, "Namco System 10 MEM(M) content has no set identity.");
    return false;
  }

  if (content.board_profile == MemMBoardProfile::Unknown ||
      content.program_flash.size() != MEMM_PROGRAM_FLASH_SIZE ||
      content.data_rom.size() != MEMM_DATA_ROM_SIZE)
  {
    Error::SetStringView(error, "Namco System 10 MEM(M) content has an invalid board profile or ROM layout.");
    return false;
  }

  RuntimeState runtime;
  runtime.set_name = std::move(content.set_name);
  runtime.media_type = MediaType::MemM;
  runtime.memm_board_profile = content.board_profile;
  runtime.memm_program_flash = std::move(content.program_flash);
  runtime.memm_data_rom = std::move(content.data_rom);
  if (!LoadMemMProgramFlashPersistence(runtime, error))
    return false;

  runtime.memm_bank = 0;
  runtime.memm_direct_access = false;
  ResetIntel28F640J5Interface(runtime);
  ResetType1State(runtime, 0);
  runtime.type1_enabled = false;
  ResetI2CState(runtime);
  ResetBaseboardInputState(runtime);

  INFO_LOG("Namco System 10 MEM(M) runtime initialized set='{}' flash={} data={}.",
           runtime.set_name, runtime.memm_program_flash.size(), runtime.memm_data_rom.size());

  s_runtime = std::move(runtime);
  CreateExpansionBoard(*s_runtime);
  ResetBaseboardInterruptState(*s_runtime, true);
  return true;
}

void Reset()
{
  if (!s_runtime.has_value())
    return;

  RuntimeState& runtime = *s_runtime;
  if (runtime.media_type == MediaType::MemN)
  {
    for (MemNRawNAND& nand : runtime.nands)
      nand.ResetInterface();
    runtime.control_latch = 0;
    runtime.nand_device = 0;
    runtime.type2_enabled = false;
    runtime.type2_transform_supported = false;
    runtime.type2_iv = 0;
    runtime.type2_generation = 0;
    runtime.type2_state.nonlinear_count = 0;
    ResetType2State(runtime);
    std::fill(runtime.lookup_ram.begin(), runtime.lookup_ram.end(), 0);
  }
  else if (runtime.media_type == MediaType::MemM)
  {
    runtime.memm_bank = 0;
    runtime.memm_direct_access = false;
    ResetIntel28F640J5Interface(runtime);
    runtime.type1_enabled = false;
    runtime.type1_iv = 0;
    runtime.type1_generation = 0;
    runtime.type1_state = {};
    runtime.type1_trace_words = 0;
    runtime.memm_trace_events = 0;
  }

  if (runtime.exio)
    runtime.exio->Reset();
  runtime.jvs_bus.Reset();
  runtime.cyberlead.Reset();
  runtime.cyberlead.ResetLED();
  runtime.jvs_addressed = false;

  ResetI2CState(runtime);
  ResetBaseboardInputState(runtime);
  ResetBaseboardInterruptState(runtime, true);
}

void Shutdown()
{
  if (s_runtime.has_value())
  {
    if (s_runtime->media_type == MediaType::MemM)
      SaveMemMProgramFlashPersistence(*s_runtime);
    else if (s_runtime->media_type == MediaType::MemN)
      SaveMemNPersistence(*s_runtime);
    ResetBaseboardInterruptState(*s_runtime, false);
    s_runtime->exio.reset();
    if (s_runtime->cyberlead_cabinet)
    {
      s_runtime->cyberlead.SaveLEDFlash();
      Arcade::JVS::NamcoCyberLead::SetActive(nullptr);
    }
  }

  if (s_cabinet_io_event)
    s_cabinet_io_event->Deactivate();
  s_cabinet_io_event.reset();

  s_baseboard_io_event.reset();
  s_runtime.reset();
}

bool IsActive()
{
  return s_runtime.has_value();
}

u32 ReadProgramROM(u32 width, u32 offset)
{
  if (!s_runtime.has_value() || (width != 1 && width != 2 && width != 4) ||
      offset >= PROGRAM_ROM_SIZE || width > (PROGRAM_ROM_SIZE - offset))
  {
    return UINT32_MAX;
  }

  const RuntimeState& runtime = s_runtime.value();
  u32 value = 0;
  for (u32 i = 0; i < width; i++)
    value |= static_cast<u32>(ReadProgramByte(runtime, offset + i)) << (i * 8);

  return value;
}

u32 ReadBIOSWindow(u32 width, u32 offset)
{
  // An expansion board decodes the upper 2 MiB of the 4 MiB BIOS window.
  if (s_runtime.has_value() && s_runtime->exio && offset >= EXIO_WINDOW_OFFSET)
  {
    u32 value = 0;
    if (s_runtime->exio->Read(width, offset - EXIO_WINDOW_OFFSET, &value))
      return value;
    return (width == 4) ? UINT32_MAX : ((1u << (width * 8)) - 1);
  }

  return ReadProgramROM(width, offset);
}

void WriteBIOSWindow(u32 width, u32 offset, u32 value)
{
  if (s_runtime.has_value() && s_runtime->exio && offset >= EXIO_WINDOW_OFFSET)
    s_runtime->exio->Write(width, offset - EXIO_WINDOW_OFFSET, value);
}

bool ReadEXP1(u32 width, u32 offset, u32* value)
{
  if (!s_runtime.has_value() || !value)
    return false;

  RuntimeState& runtime = s_runtime.value();

  if (runtime.media_type == MediaType::MemM)
  {
    if (offset >= MEMM_RANGE_BASE && offset < (MEMM_RANGE_BASE + MEMM_RANGE_SIZE))
      return ReadMemMRange(runtime, width, offset, value);
    return false;
  }

  if (runtime.media_type != MediaType::MemN)
    return false;

  if (offset >= LOOKUP_RAM_BASE && offset < (LOOKUP_RAM_BASE + LOOKUP_RAM_SIZE))
  {
    return ReadLittleEndian(std::span<const u8>(runtime.lookup_ram.data(), runtime.lookup_ram.size()),
                            offset - LOOKUP_RAM_BASE, width, value);
  }

  if (offset >= MEMN_READY_BASE && offset < (MEMN_READY_BASE + 2))
  {
    const MemNRawNAND* nand = GetSelectedNAND(runtime);
    if (!nand)
      return false;

    // The System 10 register is software-visible as zero while the selected
    // K9F2808 is ready. Busy timing is not modeled in this read-side milestone.
    return ReadU16Register(nand->IsReady() ? UINT16_C(0x0000) : UINT16_C(0x0001),
                           offset - MEMN_READY_BASE, width, value);
  }

  if (offset >= MEMN_DATA_BASE && offset < (MEMN_DATA_BASE + 2))
  {
    MemNRawNAND* nand = GetSelectedNAND(runtime);
    if (!nand)
      return false;

    if (offset != MEMN_DATA_BASE)
      return false;

    if (width == 1)
    {
      // OPEN: exact byte-lane behavior through the 16-bit MEM(N) host data
      // register has not been required by the bootstrap path. Preserve the
      // existing raw x8 diagnostic behavior until software proves otherwise.
      *value = nand->DataRead();
      return true;
    }

    if (width == 2)
    {
      // MEM(N) presents two consecutive 8-bit NAND reads as one 16-bit host word,
      // first NAND byte in the high byte. The immutable raw NAND image remains
      // untouched; board-layer transforms are applied only to the returned word.
      const u32 page_before = nand->GetCurrentPageAddress();
      const u32 column_before = nand->GetCurrentColumnAddress();
      const u16 first = nand->DataRead();
      const u16 second = nand->DataRead();
      const u16 raw_word = static_cast<u16>((first << 8) | second);

      if (runtime.type2_enabled)
      {
        // PROVEN at runtime for Star Trigon: Type-2 is enabled immediately
        // before the protected copy from logical block 10. CORROBORATED by the
        // captured stream: dynamic decode consumes the raw word directly; the
        // ordinary static transform is not part of this active path.
        u16 plain_word = raw_word;
        if (runtime.type2_transform_supported)
        {
          plain_word = DecodeType2Word(*runtime.memn_profile->type2, runtime.type2_state, raw_word,
                                       std::span<const u8>(runtime.type2_nonlinear_table));
        }

        if (runtime.type2_trace_words < TYPE2_TRACE_WORD_LIMIT)
        {
          INFO_LOG("System10 Type2 decode gen={} word={} iv={} dev={} page=0x{:04X} block=0x{:03X} "
                   "column=0x{:03X} raw=0x{:04X} plain=0x{:04X} next_mask=0x{:04X}",
                   runtime.type2_generation, runtime.type2_trace_words, runtime.type2_iv,
                   runtime.nand_device, page_before,
                   page_before / MemNRawNAND::PAGES_PER_BLOCK, column_before, raw_word, plain_word,
                   runtime.type2_state.mask);
          runtime.type2_trace_words++;
        }

        *value = plain_word;
        return true;
      }

      const bool apply_static_transform = ShouldApplyStaticTransform(runtime, *nand);
      *value =
        apply_static_transform ? DecodeMemNStaticWord(*runtime.memn_profile, raw_word, runtime.data_xor) : raw_word;
      return true;
    }

    return false;
  }

  if (offset >= MEMN_CONTROL_BASE && offset < (MEMN_CONTROL_BASE + 2))
    return ReadU16Register(runtime.control_latch, offset - MEMN_CONTROL_BASE, width, value);

  return false;
}

bool WriteEXP1(u32 width, u32 offset, u32 value)
{
  if (!s_runtime.has_value())
    return false;

  RuntimeState& runtime = s_runtime.value();

  if (runtime.media_type == MediaType::MemM)
  {
    if (HandleMemMCryptoSwitchWrite(runtime, width, offset, value))
      return true;
    if (offset >= MEMM_RANGE_BASE && offset < (MEMM_RANGE_BASE + MEMM_RANGE_SIZE))
      return WriteMemMRange(runtime, width, offset, value);
    return false;
  }

  if (runtime.media_type != MediaType::MemN)
    return false;

  if (offset >= LOOKUP_RAM_BASE && offset < (LOOKUP_RAM_BASE + LOOKUP_RAM_SIZE))
  {
    return WriteLittleEndian(std::span<u8>(runtime.lookup_ram.data(), runtime.lookup_ram.size()),
                             offset - LOOKUP_RAM_BASE, width, value);
  }

  if (HandleType2SwitchWrite(runtime, width, offset, value))
    return true;

  MemNRawNAND* nand = GetSelectedNAND(runtime);

  if (offset == MEMN_COMMAND_BASE)
  {
    if (!nand || (width != 1 && width != 2 && width != 4))
      return false;

    nand->CommandWrite(static_cast<u8>(value));
    return true;
  }

  if (offset == MEMN_COLUMN_BASE)
  {
    if (!nand || (width != 1 && width != 2 && width != 4))
      return false;

    nand->AddressColumnWrite(static_cast<u8>(value));
    return true;
  }

  if (offset == MEMN_ROW_LOW_BASE)
  {
    if (!nand || (width != 1 && width != 2 && width != 4))
      return false;

    nand->AddressRowLowWrite(static_cast<u8>(value));
    return true;
  }

  if (offset == MEMN_ROW_HIGH_BASE)
  {
    if (!nand || (width != 1 && width != 2 && width != 4))
      return false;

    nand->AddressRowHighWrite(static_cast<u8>(value));
    return true;
  }

  if (offset == MEMN_DATA_BASE)
  {
    if (!nand || (width != 1 && width != 2))
      return false;

    if (width == 1)
    {
      nand->DataWrite(static_cast<u8>(value));
    }
    else
    {
      // The 16-bit host register mirrors the read path: the first x8 NAND byte
      // occupies the high byte of the host word.
      nand->DataWrite(static_cast<u8>(value >> 8));
      nand->DataWrite(static_cast<u8>(value));
    }
    return true;
  }

  if (offset == MEMN_DEVICE_BASE)
  {
    if (width != 1 && width != 2 && width != 4)
      return false;

    // The MEM(N) CPLD accepts a new NAND selector only while control bit 2 is
    // clear. Software sets bit 2 while accessing the command/address/data
    // registers, which keeps the selected device stable for that transaction.
    if ((runtime.control_latch & UINT16_C(0x0004)) == 0)
      runtime.nand_device = static_cast<u16>(value);
    return true;
  }

  if (offset >= MEMN_CONTROL_BASE && offset < (MEMN_CONTROL_BASE + 2))
  {
    if (width == 1)
    {
      const u32 shift = (offset - MEMN_CONTROL_BASE) * 8;
      runtime.control_latch =
        static_cast<u16>((runtime.control_latch & ~(UINT16_C(0xff) << shift)) |
                         ((static_cast<u16>(value) & UINT16_C(0xff)) << shift));
      return true;
    }

    if (offset == MEMN_CONTROL_BASE && width == 2)
    {
      // Keep all control bits exactly as written. Only bit 2 has a proven
      // transaction-related role; the meanings of the other bits remain open.
      runtime.control_latch = static_cast<u16>(value);
      return true;
    }

    return false;
  }

  return false;
}



bool DMARead(u32* words, u32 word_count)
{
  if (!words || !s_runtime.has_value() || s_runtime->media_type != MediaType::MemN)
    return false;

  for (u32 index = 0; index < word_count; index++)
  {
    u32 low = 0;
    u32 high = 0;
    if (!ReadEXP1(2, MEMN_DATA_BASE, &low) || !ReadEXP1(2, MEMN_DATA_BASE, &high))
      return false;

    // PIO DMA exposes the MEM(N) x16 data register as consecutive little-endian
    // halfwords in PSX RAM: first NAND word in bits 0-15, second in bits 16-31.
    words[index] = (low & UINT32_C(0xffff)) | ((high & UINT32_C(0xffff)) << 16);
  }

  return true;
}

bool ReadEXP3(u32 width, u32 offset, u32* value)
{
  if (!s_runtime.has_value() || !value)
    return false;

  RuntimeState& runtime = s_runtime.value();

  if (runtime.media_type == MediaType::MemM &&
      offset >= MEMM_BANK_EXP3_BASE && offset < (MEMM_BANK_EXP3_BASE + MEMM_BANK_EXP3_SIZE))
  {
    return ReadMemMBank(runtime, width, offset, value);
  }

  if (offset >= EXP3_SYSTEM_SWITCH_BASE && offset < (EXP3_SYSTEM_SWITCH_BASE + 2))
  {
    return ReadU16Register(ReadBaseboardSystemSwitches(runtime), offset - EXP3_SYSTEM_SWITCH_BASE, width, value);
  }

  if (offset == EXP3_EXIO_IDENT_BASE && width == 2)
  {
    *value = ReadEXIOIdentifier(runtime);
    return true;
  }

  if (offset >= EXP3_PLAYER_INPUT_BASE &&
      offset < (EXP3_PLAYER_INPUT_BASE + EXP3_PLAYER_INPUT_SIZE))
  {
    return ReadBaseboardPlayerRegister(runtime, width, offset, value);
  }

  if (offset == EXP3_I2C_CLOCK_BASE && width == 4)
  {
    *value = static_cast<u32>(ReadI2CClockLine(runtime)) |
             (static_cast<u32>(ReadI2CDataLine(runtime)) << 16);
    return true;
  }

  if (offset >= EXP3_I2C_CLOCK_BASE && offset < (EXP3_I2C_CLOCK_BASE + 2))
    return ReadU16Register(ReadI2CClockLine(runtime), offset - EXP3_I2C_CLOCK_BASE, width, value);

  if (offset >= EXP3_I2C_DATA_BASE && offset < (EXP3_I2C_DATA_BASE + 2))
    return ReadU16Register(ReadI2CDataLine(runtime), offset - EXP3_I2C_DATA_BASE, width, value);

  if (offset >= EXP3_INTERRUPT_STATUS_BASE && offset < (EXP3_INTERRUPT_STATUS_BASE + 2))
  {
    const bool handled =
      ReadU16Register(runtime.baseboard_interrupt_status, offset - EXP3_INTERRUPT_STATUS_BASE, width, value);
    if (handled && runtime.baseboard_irq_trace_events < BASEBOARD_IRQ_TRACE_LIMIT)
    {
      INFO_LOG("System10 base-board IRQ status read=0x{:04X}", runtime.baseboard_interrupt_status);
      runtime.baseboard_irq_trace_events++;
    }
    return handled;
  }

  return false;
}

bool WriteEXP3(u32 width, u32 offset, u32 value)
{
  if (!s_runtime.has_value())
    return false;

  RuntimeState& runtime = s_runtime.value();

  if (runtime.media_type == MediaType::MemM &&
      offset >= MEMM_BANK_EXP3_BASE && offset < (MEMM_BANK_EXP3_BASE + MEMM_BANK_EXP3_SIZE))
  {
    return WriteMemMBank(runtime, width, offset, value);
  }

  if (HandleBaseboardInterruptAck(runtime, width, offset))
    return true;

  if (offset == EXP3_EXIO_IDENT_BASE && width == 2)
  {
    ResetEXIOIdentifier(runtime);
    return true;
  }

  if (offset == EXP3_I2C_CLOCK_BASE && (width == 1 || width == 2))
  {
    WriteI2CClockLine(runtime, static_cast<u16>(value));
    return true;
  }

  if (offset == EXP3_I2C_DATA_BASE && (width == 1 || width == 2))
  {
    WriteI2CDataLine(runtime, static_cast<u16>(value));
    return true;
  }

  if (offset == EXP3_I2C_CLOCK_BASE && width == 4)
  {
    // Preserve little-endian register ordering for a word access spanning the
    // adjacent 16-bit clock and data registers.
    WriteI2CClockLine(runtime, static_cast<u16>(value));
    WriteI2CDataLine(runtime, static_cast<u16>(value >> 16));
    return true;
  }

  return false;
}

} // namespace NamcoSystem10
