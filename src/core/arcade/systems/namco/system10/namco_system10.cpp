// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/namco/system10/namco_system10.h"

#include "core/arcade/systems/namco/system10/namco_system10_memn.h"

#include "core/arcade/arcade_database.h"
#include "core/interrupt_controller.h"
#include "core/system.h"
#include "core/timing_event.h"

#include "common/error.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
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

static constexpr u32 PROGRAM_ROM_SIZE = 0x400000;
static constexpr u32 PROGRAM_FIRST_PAGE = 0x040;
static constexpr u32 PROGRAM_FIRST_END = 0x01c000;
static constexpr u32 PROGRAM_FILL_END = 0x020000;
static constexpr u32 PROGRAM_SECOND_PAGE = 0x120;

static_assert(PROGRAM_FIRST_END == UINT32_C(0x0e0) * MemNRawNAND::DATA_BYTES_PER_PAGE);
static_assert((PROGRAM_ROM_SIZE - PROGRAM_FILL_END) ==
              UINT32_C(0x1f00) * MemNRawNAND::DATA_BYTES_PER_PAGE);

struct Type2Equation
{
  std::array<u16, 3> cipher_taps;
  std::array<u16, 3> plain_taps;
  bool constant;
  bool nonlinear_mask_12_and_14;
};

struct Type2State
{
  // History index 0 is the most recently consumed protected word.
  std::array<u16, 3> cipher_history = {};
  std::array<u16, 3> plain_history = {};
  u16 mask = 0;
};

// CORROBORATED, not locally hardware-proven: this reduced Star Trigon model
// was fitted against protected-stream input/output behavior. It is expressed
// as profile-local three-word history equations and remains separate from the
// ordinary static MEM(N) permutation.
//
// Each row computes one bit of the next XOR mask. Tap words are ordered newest
// to oldest. The final flag selects the recovered nonlinear term for that bit.
static constexpr std::array<Type2Equation, 16> STAR_TRIGON_TYPE2_EQUATIONS = {
  Type2Equation{{UINT16_C(0x75a4), UINT16_C(0x2cf8), UINT16_C(0x0036)},
                {UINT16_C(0x75a5), UINT16_C(0x0230), UINT16_C(0x0012)},
                false, false},
  Type2Equation{{UINT16_C(0x010c), UINT16_C(0x0000), UINT16_C(0x0000)},
                {UINT16_C(0x0008), UINT16_C(0x0000), UINT16_C(0x0000)},
                true, false},
  Type2Equation{{UINT16_C(0x0781), UINT16_C(0x2290), UINT16_C(0x0036)},
                {UINT16_C(0x8581), UINT16_C(0x0200), UINT16_C(0x0012)},
                true, false},
  Type2Equation{{UINT16_C(0x49b5), UINT16_C(0xd6d2), UINT16_C(0x8fd4)},
                {UINT16_C(0x4da5), UINT16_C(0x0832), UINT16_C(0x0010)},
                false, false},
  Type2Equation{{UINT16_C(0x0080), UINT16_C(0x0000), UINT16_C(0x0000)},
                {UINT16_C(0x2000), UINT16_C(0x0000), UINT16_C(0x0000)},
                false, true},
  Type2Equation{{UINT16_C(0x8c00), UINT16_C(0x0000), UINT16_C(0x0000)},
                {UINT16_C(0x8800), UINT16_C(0x0000), UINT16_C(0x0000)},
                false, false},
  Type2Equation{{UINT16_C(0xc7e4), UINT16_C(0xf79e), UINT16_C(0x8fd4)},
                {UINT16_C(0xc7e4), UINT16_C(0x0277), UINT16_C(0x0010)},
                true, false},
  Type2Equation{{UINT16_C(0x102a), UINT16_C(0x097c), UINT16_C(0x0000)},
                {UINT16_C(0x10a8), UINT16_C(0x0860), UINT16_C(0x0000)},
                false, false},
  Type2Equation{{UINT16_C(0x103a), UINT16_C(0x097c), UINT16_C(0x0000)},
                {UINT16_C(0x1038), UINT16_C(0x0860), UINT16_C(0x0000)},
                false, false},
  Type2Equation{{UINT16_C(0xc4e4), UINT16_C(0xf7ba), UINT16_C(0x8fd4)},
                {UINT16_C(0x85e4), UINT16_C(0x0273), UINT16_C(0x0010)},
                false, false},
  Type2Equation{{UINT16_C(0x2200), UINT16_C(0x0024), UINT16_C(0x0000)},
                {UINT16_C(0x0200), UINT16_C(0x0004), UINT16_C(0x0000)},
                true, false},
  Type2Equation{{UINT16_C(0x6040), UINT16_C(0x0000), UINT16_C(0x0000)},
                {UINT16_C(0x2040), UINT16_C(0x0000), UINT16_C(0x0000)},
                true, false},
  Type2Equation{{UINT16_C(0x0420), UINT16_C(0x0000), UINT16_C(0x0000)},
                {UINT16_C(0x0428), UINT16_C(0x0000), UINT16_C(0x0000)},
                false, false},
  Type2Equation{{UINT16_C(0x1801), UINT16_C(0x0000), UINT16_C(0x0000)},
                {UINT16_C(0x1001), UINT16_C(0x0000), UINT16_C(0x0000)},
                false, false},
  Type2Equation{{UINT16_C(0x1d80), UINT16_C(0x2290), UINT16_C(0x0036)},
                {UINT16_C(0x0d80), UINT16_C(0x0200), UINT16_C(0x0012)},
                false, false},
  Type2Equation{{UINT16_C(0x0105), UINT16_C(0x0000), UINT16_C(0x0000)},
                {UINT16_C(0x0144), UINT16_C(0x0000), UINT16_C(0x0000)},
                true, false},
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

struct RuntimeState
{
  std::string set_name;
  MemNBoardProfile board_profile = MemNBoardProfile::Unknown;
  MemNRawNAND nand0;
  MemNRawNAND nand1;
  std::vector<u8> lookup_ram;
  u16 control_latch = 0;
  u16 nand_device = 0;

  bool type2_enabled = false;
  bool type2_transform_supported = false;
  u8 type2_iv = 0;
  u32 type2_generation = 0;
  u32 type2_trace_words = 0;
  Type2State type2_state;

  I2CState i2c;

  u16 baseboard_interrupt_status = 0;
  u32 baseboard_irq_trace_events = 0;
};

std::optional<RuntimeState> s_runtime;
std::unique_ptr<TimingEvent> s_baseboard_io_event;

// PROVEN/CORROBORATED: Star Trigon ordinary MEM(N) data is stored with a
// pre-permute XOR of 0xAAAA and this recovered 16-bit bit permutation.
// The array lists the source bit for output bits 15 down to 0.
static constexpr std::array<u8, 16> STAR_TRIGON_STATIC_BIT_ORDER = {
  14, 13, 12, 15, 9, 11, 8, 10, 4, 5, 6, 7, 0, 3, 2, 1,
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

constexpr u16 DecodeStaticWord(MemNBoardProfile profile, u16 raw_word)
{
  switch (profile)
  {
    case MemNBoardProfile::StarTrigon:
      return Permute16(static_cast<u16>(raw_word ^ UINT16_C(0xaaaa)), STAR_TRIGON_STATIC_BIT_ORDER);

    case MemNBoardProfile::Unknown:
    default:
      return raw_word;
  }
}

static_assert(DecodeStaticWord(MemNBoardProfile::StarTrigon, UINT16_C(0xaaaa)) == UINT16_C(0x0000));
static_assert(DecodeStaticWord(MemNBoardProfile::StarTrigon, UINT16_C(0x5555)) == UINT16_C(0xffff));
static_assert(DecodeStaticWord(MemNBoardProfile::StarTrigon, UINT16_C(0x1234)) == UINT16_C(0x7497));

constexpr void PushType2History(std::array<u16, 3>& history, u16 word)
{
  history[2] = history[1];
  history[1] = history[0];
  history[0] = word;
}

constexpr u16 ComputeStarTrigonType2NextMask(const Type2State& state, u16 used_mask)
{
  u16 next_mask = 0;

  for (u32 bit = 0; bit < STAR_TRIGON_TYPE2_EQUATIONS.size(); bit++)
  {
    const Type2Equation& equation = STAR_TRIGON_TYPE2_EQUATIONS[bit];
    u16 parity_input = 0;

    for (u32 history_index = 0; history_index < 3; history_index++)
    {
      parity_input ^= static_cast<u16>(state.cipher_history[history_index] &
                                       equation.cipher_taps[history_index]);
      parity_input ^= static_cast<u16>(state.plain_history[history_index] &
                                       equation.plain_taps[history_index]);
    }

    u32 bit_value = (std::popcount(static_cast<u32>(parity_input)) & 1u) ^
                    (equation.constant ? 1u : 0u);

    if (equation.nonlinear_mask_12_and_14 &&
        (used_mask & UINT16_C(0x5000)) == UINT16_C(0x5000))
    {
      bit_value ^= 1u;
    }

    next_mask |= static_cast<u16>(bit_value << bit);
  }

  return next_mask;
}

constexpr u16 DecodeStarTrigonType2Word(Type2State& state, u16 cipher_word)
{
  const u16 used_mask = state.mask;
  const u16 plain_word = static_cast<u16>(cipher_word ^ used_mask);

  PushType2History(state.cipher_history, cipher_word);
  PushType2History(state.plain_history, plain_word);
  state.mask = ComputeStarTrigonType2NextMask(state, used_mask);
  return plain_word;
}

constexpr bool ValidateStarTrigonType2KnownAnswer()
{
  // First 32 bytes of the protected stream captured by S10-02A. The plaintext
  // forms a coherent MIPS entry sequence: J 0x80012220, NOP, then an address
  // ladder beginning at 0x800126BC.
  constexpr std::array<u16, 16> cipher = {
    UINT16_C(0x4888), UINT16_C(0xbed6), UINT16_C(0x1c94), UINT16_C(0x1b5c),
    UINT16_C(0xa21c), UINT16_C(0x1d38), UINT16_C(0x082b), UINT16_C(0xdf32),
    UINT16_C(0x4600), UINT16_C(0x5275), UINT16_C(0x985d), UINT16_C(0x73d4),
    UINT16_C(0xa392), UINT16_C(0x9563), UINT16_C(0x86e1), UINT16_C(0xebc8),
  };
  constexpr std::array<u16, 16> expected_plain = {
    UINT16_C(0x4888), UINT16_C(0x0800), UINT16_C(0x0000), UINT16_C(0x0000),
    UINT16_C(0x26bc), UINT16_C(0x8001), UINT16_C(0x26cc), UINT16_C(0x8001),
    UINT16_C(0x26dc), UINT16_C(0x8001), UINT16_C(0x26ec), UINT16_C(0x8001),
    UINT16_C(0x26fc), UINT16_C(0x8001), UINT16_C(0x270c), UINT16_C(0x8001),
  };

  Type2State state;
  for (u32 i = 0; i < cipher.size(); i++)
  {
    if (DecodeStarTrigonType2Word(state, cipher[i]) != expected_plain[i])
      return false;
  }

  return true;
}

static_assert(ValidateStarTrigonType2KnownAnswer());

void ResetType2State(RuntimeState& runtime)
{
  runtime.type2_state = {};
  runtime.type2_trace_words = 0;
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
    runtime.type2_transform_supported =
      (runtime.board_profile == MemNBoardProfile::StarTrigon && runtime.type2_iv == 0);
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

  return runtime.board_profile != MemNBoardProfile::Unknown;
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
  if (runtime.board_profile != MemNBoardProfile::StarTrigon || offset >= PROGRAM_ROM_SIZE)
    return UINT8_C(0xff);

  if (offset >= PROGRAM_FIRST_END && offset < PROGRAM_FILL_END)
    return UINT8_C(0x55);

  const u32 word_offset = offset & ~UINT32_C(1);
  u32 page;
  u32 column;
  if (!MapProgramWordToNAND(word_offset, &page, &column))
    return UINT8_C(0xff);

  const std::span<const u8> data = runtime.nand0.GetPageData(page);
  if (data.empty() || (column + 1) >= data.size())
    return UINT8_C(0xff);

  // Raw MEM(N) words arrive high byte first at the board. Decode the complete
  // 16-bit word, then expose the resulting program aperture in PSX little-endian
  // byte order. The immutable NAND image itself is never rewritten.
  const u16 raw_word = static_cast<u16>((static_cast<u16>(data[column]) << 8) | data[column + 1]);
  const u16 decoded_word = DecodeStaticWord(runtime.board_profile, raw_word);
  return static_cast<u8>((decoded_word >> ((offset & 1u) * 8)) & UINT16_C(0x00ff));
}

MemNRawNAND* GetSelectedNAND(RuntimeState& runtime)
{
  if (runtime.nand_device == 0)
    return &runtime.nand0;
  if (runtime.nand_device == 1)
    return &runtime.nand1;

  return nullptr;
}

const MemNRawNAND* GetSelectedNAND(const RuntimeState& runtime)
{
  if (runtime.nand_device == 0)
    return &runtime.nand0;
  if (runtime.nand_device == 1)
    return &runtime.nand1;

  return nullptr;
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

bool LoadRawNANDMember(const char* archive_path, const Arcade::Database::ROMDefinition& rom,
                       std::vector<u8>* data, Error* error)
{
  if (rom.size != MemNRawNAND::RAW_IMAGE_SIZE || rom.offset != 0 || rom.interleave != 1 || rom.group_size != 1 ||
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

} // namespace

std::optional<MemNLoadedContent> LoadStarTrigonContent(const char* archive_path,
                                                      const Arcade::Database::GameDefinition& game, Error* error)
{
  if (game.hardware_profile != "ns10_startrgn")
  {
    Error::SetStringFmt(error, "System 10 S10-01C only supports the Star Trigon MEM(N) profile; requested '{}'.",
                        game.hardware_profile);
    return std::nullopt;
  }

  const Arcade::Database::ROMDefinition* nand0_rom = nullptr;
  const Arcade::Database::ROMDefinition* nand1_rom = nullptr;
  u32 nand_region_count = 0;

  for (const Arcade::Database::ROMDefinition& rom : game.roms)
  {
    if (rom.region == "nand0")
    {
      nand0_rom = &rom;
      nand_region_count++;
    }
    else if (rom.region == "nand1")
    {
      nand1_rom = &rom;
      nand_region_count++;
    }
    else if (rom.region.size() >= 4 && rom.region.compare(0, 4, "nand") == 0)
    {
      nand_region_count++;
    }
  }

  if (!nand0_rom || !nand1_rom || nand_region_count != 2)
  {
    Error::SetStringView(error, "Star Trigon database entry must contain exactly raw NAND regions nand0 and nand1.");
    return std::nullopt;
  }

  MemNLoadedContent content;
  content.set_name = game.id;
  content.board_profile = MemNBoardProfile::StarTrigon;

  if (!LoadRawNANDMember(archive_path, *nand0_rom, &content.nand0, error) ||
      !LoadRawNANDMember(archive_path, *nand1_rom, &content.nand1, error))
  {
    return std::nullopt;
  }

  VERBOSE_LOG("Loaded Namco System 10 Star Trigon raw NANDs from '{}': nand0={} bytes nand1={} bytes.",
              archive_path, content.nand0.size(), content.nand1.size());
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

  if (content.board_profile == MemNBoardProfile::Unknown)
  {
    Error::SetStringView(error, "Namco System 10 MEM(N) content has no board profile.");
    return false;
  }

  RuntimeState runtime;
  runtime.set_name = std::move(content.set_name);
  runtime.board_profile = content.board_profile;

  if (!runtime.nand0.Load(std::move(content.nand0), error) ||
      !runtime.nand1.Load(std::move(content.nand1), error))
  {
    return false;
  }

  runtime.lookup_ram.resize(LOOKUP_RAM_SIZE, 0);

  ResetI2CState(runtime);

  VERBOSE_LOG("Namco System 10 MEM(N) runtime initialized set='{}' nand0={} nand1={}.", runtime.set_name,
              runtime.nand0.GetRawImage().size(), runtime.nand1.GetRawImage().size());

  s_runtime = std::move(runtime);
  ResetBaseboardInterruptState(*s_runtime, true);
  return true;
}

void Reset()
{
  if (!s_runtime.has_value())
    return;

  s_runtime->nand0.ResetInterface();
  s_runtime->nand1.ResetInterface();
  s_runtime->control_latch = 0;
  s_runtime->nand_device = 0;
  s_runtime->type2_enabled = false;
  s_runtime->type2_transform_supported = false;
  s_runtime->type2_iv = 0;
  s_runtime->type2_generation = 0;
  ResetType2State(*s_runtime);
  ResetI2CState(*s_runtime);
  ResetBaseboardInterruptState(*s_runtime, true);
  std::fill(s_runtime->lookup_ram.begin(), s_runtime->lookup_ram.end(), 0);
}

void Shutdown()
{
  if (s_runtime.has_value())
    ResetBaseboardInterruptState(*s_runtime, false);

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

bool ReadEXP1(u32 width, u32 offset, u32* value)
{
  if (!s_runtime.has_value() || !value)
    return false;

  RuntimeState& runtime = s_runtime.value();

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
        const u16 plain_word = runtime.type2_transform_supported ?
                                 DecodeStarTrigonType2Word(runtime.type2_state, raw_word) :
                                 raw_word;

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
      *value = apply_static_transform ? DecodeStaticWord(runtime.board_profile, raw_word) : raw_word;
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

  if (offset == MEMN_DEVICE_BASE)
  {
    if (width != 1 && width != 2 && width != 4)
      return false;

    // Preserve the software-written selector instead of clamping it. The first
    // MEM(N) target (Star Trigon) physically populates NAND 0 and NAND 1.
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

  // NAND data program/erase is intentionally deferred.
  return false;
}


bool ReadEXP3(u32 width, u32 offset, u32* value)
{
  if (!s_runtime.has_value() || !value)
    return false;

  RuntimeState& runtime = s_runtime.value();

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

  if (HandleBaseboardInterruptAck(runtime, width, offset))
    return true;

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
