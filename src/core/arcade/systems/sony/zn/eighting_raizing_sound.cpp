// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/sony/zn/eighting_raizing_sound.h"

#include "core/arcade/devices/audio/ymf271.h"
#include "core/musashi/m68k.h"
#include "core/musashi/m68k_bus.h"
#include "core/system.h"

#include "common/error.h"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <utility>

Log_SetChannel(EightingRaizingSound);

namespace SonyZN::EightingRaizingSound {
namespace {

static constexpr u32 SOUND_CLOCK_HZ = 12'000'000;
static constexpr u32 YMF271_CLOCK_HZ = 16'934'400;
static constexpr u32 OUTPUT_SAMPLE_RATE = 44'100;
static constexpr u32 SOUND_ROM_DECODE_SIZE = 0x80000;
static constexpr u32 SOUND_ROM_MAX_SIZE = 0x100000;
static constexpr u32 YMF271_ROM_SIZE = 0x400000;
static constexpr u32 RAM_BASE = 0x080000;
static constexpr u32 RAM_SIZE = 0x80000;
static constexpr u32 YMF271_BASE = 0x100000;
static constexpr u32 YMF271_END = 0x10001f;
static constexpr u32 COMMAND_LATCH_HIGH = 0x180008;
static constexpr u32 COMMAND_LATCH_LOW = 0x180009;
static constexpr u32 ADDRESS_MASK = 0x00ffffff;
static constexpr int MAX_EXECUTE_CYCLES = 0x10000;
static constexpr u32 TRACE_LIMIT = 128;

struct State
{
  bool active = false;
  std::vector<u8> sound_program;
  std::array<u8, RAM_SIZE> ram{};
  YMF271::Chip ymf271;

  u8 command_latch = 0;

  s64 cycle_balance = 0;
  int ymf_execute_accounted_cycles = 0;
  bool in_m68k_execute = false;
  u64 audio_cycle_fraction = 0;
  u64 audio_target_cycles = 0;
  u64 host_target_cycles = 0;
  u64 scheduled_target_cycles = 0;
  u64 host_tick_fraction = 0;
  u64 last_host_sync_ticks = 0;
  u64 last_ticks_per_second = 0;

  u32 command_write_count = 0;
  u32 command_read_count = 0;
  u32 irq_write_count = 0;
  bool unmapped_read_logged = false;
};

State s_state;

u32 ReadBigEndian32(const std::vector<u8>& data, u32 offset)
{
  return (static_cast<u32>(data[offset]) << 24) | (static_cast<u32>(data[offset + 1]) << 16) |
         (static_cast<u32>(data[offset + 2]) << 8) | static_cast<u32>(data[offset + 3]);
}

bool HasPlausibleResetVectors(const std::vector<u8>& rom, u32* initial_sp, u32* initial_pc)
{
  if (rom.size() < 8)
    return false;

  const u32 sp = ReadBigEndian32(rom, 0);
  const u32 pc = ReadBigEndian32(rom, 4);
  if (initial_sp)
    *initial_sp = sp;
  if (initial_pc)
    *initial_pc = pc;

  return sp >= RAM_BASE && sp < (RAM_BASE + RAM_SIZE) && (sp & 1u) == 0 && pc < SOUND_ROM_DECODE_SIZE && (pc & 1u) == 0;
}

void SynchronizeYMFToCurrentCPU()
{
  if (!s_state.in_m68k_execute)
    return;

  const int current_cycles = m68k_cycles_run();
  if (current_cycles <= s_state.ymf_execute_accounted_cycles)
    return;

  s_state.ymf271.AdvanceCycles(static_cast<u32>(current_cycles - s_state.ymf_execute_accounted_cycles),
                               SOUND_CLOCK_HZ);
  s_state.ymf_execute_accounted_cycles = current_cycles;
}

u8 ReadYMF271(u32 address)
{
  // The OPX is connected to the low byte of the 68000 data bus.
  if ((address & 1u) == 0)
    return UINT8_C(0xff);

  SynchronizeYMFToCurrentCPU();
  const u32 selection = (address - YMF271_BASE) >> 1;
  return s_state.ymf271.Read(selection);
}

void WriteYMF271(u32 address, u8 value)
{
  if ((address & 1u) == 0)
    return;

  SynchronizeYMFToCurrentCPU();
  const u32 selection = (address - YMF271_BASE) >> 1;
  s_state.ymf271.Write(selection, value);
}

u8 ReadByte(u32 address)
{
  address &= ADDRESS_MASK;

  if (address < SOUND_ROM_DECODE_SIZE)
    return s_state.sound_program[address];

  if (address >= RAM_BASE && address < (RAM_BASE + RAM_SIZE))
    return s_state.ram[address - RAM_BASE];

  if (address >= YMF271_BASE && address <= YMF271_END)
    return ReadYMF271(address);

  // The game performs MOVE.W $180008,Dn and masks to the low byte. The
  // high byte is not connected to the 8-bit command latch, so expose it as
  // open bus rather than reporting it as an unmapped access.
  if (address == COMMAND_LATCH_HIGH)
    return UINT8_C(0xff);

  if (address == COMMAND_LATCH_LOW)
  {
    if (s_state.command_read_count < TRACE_LIMIT)
    {
      DEV_LOG("Eighting.Sound command_read={} value=0x{:02X} pc=0x{:06X}", s_state.command_read_count,
               s_state.command_latch, m68k_get_reg(nullptr, M68K_REG_PC));
    }
    s_state.command_read_count++;
    return s_state.command_latch;
  }

  if (!s_state.unmapped_read_logged)
  {
    s_state.unmapped_read_logged = true;
    DEV_LOG("Eighting.Sound first unmapped 68000 read address=0x{:06X} pc=0x{:06X}", address,
                m68k_get_reg(nullptr, M68K_REG_PC));
  }
  return UINT8_C(0xff);
}

void WriteByte(u32 address, u8 value)
{
  address &= ADDRESS_MASK;

  if (address >= RAM_BASE && address < (RAM_BASE + RAM_SIZE))
  {
    s_state.ram[address - RAM_BASE] = value;
    return;
  }

  if (address >= YMF271_BASE && address <= YMF271_END)
  {
    WriteYMF271(address, value);
    return;
  }

  // Reference decode treats all other writes from 0x100020 upward as no-ops;
  // ROM writes below 0x080000 are also ignored.
}

u32 ReadMemory8ForCore(u32 address)
{
  return ReadByte(address);
}

u32 ReadMemory16ForCore(u32 address)
{
  return (static_cast<u32>(ReadByte(address)) << 8) | static_cast<u32>(ReadByte(address + 1));
}

u32 ReadMemory32ForCore(u32 address)
{
  return (ReadMemory16ForCore(address) << 16) | ReadMemory16ForCore(address + 2);
}

void WriteMemory8ForCore(u32 address, u32 value)
{
  WriteByte(address, static_cast<u8>(value));
}

void WriteMemory16ForCore(u32 address, u32 value)
{
  WriteByte(address, static_cast<u8>(value >> 8));
  WriteByte(address + 1, static_cast<u8>(value));
}

void WriteMemory32ForCore(u32 address, u32 value)
{
  WriteMemory16ForCore(address, value >> 16);
  WriteMemory16ForCore(address + 2, value);
}

void RunCycles(u32 cycles)
{
  s_state.cycle_balance += cycles;
  while (s_state.cycle_balance > 0)
  {
    const int requested_cycles = static_cast<int>(std::min<s64>(s_state.cycle_balance, MAX_EXECUTE_CYCLES));
    s_state.ymf_execute_accounted_cycles = 0;
    s_state.in_m68k_execute = true;
    const int executed = m68k_execute(requested_cycles);
    s_state.in_m68k_execute = false;
    if (executed <= 0)
      break;

    if (executed > s_state.ymf_execute_accounted_cycles)
    {
      s_state.ymf271.AdvanceCycles(static_cast<u32>(executed - s_state.ymf_execute_accounted_cycles), SOUND_CLOCK_HZ);
    }
    s_state.ymf_execute_accounted_cycles = 0;
    s_state.cycle_balance -= static_cast<s64>(executed);
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
    const u32 chunk = static_cast<u32>(std::min<u64>(cycles, UINT64_C(0x01000000)));
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
    s_state.host_tick_fraction = (s_state.host_tick_fraction * ticks_per_second) / s_state.last_ticks_per_second;
    s_state.last_ticks_per_second = ticks_per_second;
  }

  const u64 elapsed_ticks = now - s_state.last_host_sync_ticks;
  s_state.last_host_sync_ticks = now;
  const u64 numerator = (elapsed_ticks * SOUND_CLOCK_HZ) + s_state.host_tick_fraction;
  s_state.host_target_cycles += numerator / ticks_per_second;
  s_state.host_tick_fraction = numerator % ticks_per_second;
  RunToTarget(s_state.host_target_cycles);
}

} // namespace

bool Initialize(std::vector<u8> sound_program, std::vector<u8> ymf271_rom, Error* error)
{
  Shutdown();

  if (sound_program.size() < SOUND_ROM_DECODE_SIZE || sound_program.size() > SOUND_ROM_MAX_SIZE)
  {
    Error::SetStringFmt(error, "Eighting/Raizing ZN-1 68000 ROM has unsupported size {}; expected {} to {} bytes.",
                        sound_program.size(), SOUND_ROM_DECODE_SIZE, SOUND_ROM_MAX_SIZE);
    return false;
  }
  if (ymf271_rom.size() != YMF271_ROM_SIZE)
  {
    Error::SetStringFmt(error, "Eighting/Raizing ZN-1 YMF271 ROM has size {}; expected {} bytes.", ymf271_rom.size(),
                        YMF271_ROM_SIZE);
    return false;
  }

  u32 initial_sp = 0;
  u32 initial_pc = 0;
  if (!HasPlausibleResetVectors(sound_program, &initial_sp, &initial_pc))
  {
    Error::SetStringFmt(error, "Eighting/Raizing ZN-1 68000 reset vectors are invalid (SP=0x{:08X}, PC=0x{:08X}).",
                        initial_sp, initial_pc);
    return false;
  }

  s_state.sound_program = std::move(sound_program);
  s_state.ymf271.Initialize(std::move(ymf271_rom), YMF271_CLOCK_HZ);
  s_state.active = true;

  MusashiBus::Callbacks callbacks;
  callbacks.read8 = ReadMemory8ForCore;
  callbacks.read16 = ReadMemory16ForCore;
  callbacks.read32 = ReadMemory32ForCore;
  callbacks.write8 = WriteMemory8ForCore;
  callbacks.write16 = WriteMemory16ForCore;
  callbacks.write32 = WriteMemory32ForCore;
  MusashiBus::SetCallbacks(callbacks);

  m68k_init();
  m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  Reset();

  VERBOSE_LOG(
    "Eighting.Sound initialized cpu='MC68000' clock_hz={} rom_size={} rom_decode={} ram_decode={} initial_sp=0x{:06X} "
    "initial_pc=0x{:06X} ymf271='PCM + polled timers' ymf271_clock={} ymf271_rom={} irq2='PSX 0x1FB00004 HOLD/autovector' "
    "command='PSX 0x1FB00000 -> 68000 0x180009 low byte'",
    SOUND_CLOCK_HZ, s_state.sound_program.size(), SOUND_ROM_DECODE_SIZE, RAM_SIZE, initial_sp, initial_pc,
    YMF271_CLOCK_HZ,
    s_state.ymf271.GetROMSize());
  return true;
}

void Reset()
{
  if (!s_state.active)
    return;

  s_state.ram.fill(0);
  s_state.ymf271.Reset();
  s_state.command_latch = 0;
  s_state.cycle_balance = 0;
  s_state.ymf_execute_accounted_cycles = 0;
  s_state.in_m68k_execute = false;
  s_state.audio_cycle_fraction = 0;
  s_state.audio_target_cycles = 0;
  s_state.host_target_cycles = 0;
  s_state.scheduled_target_cycles = 0;
  s_state.host_tick_fraction = 0;
  s_state.last_host_sync_ticks = static_cast<u64>(System::GetGlobalTickCounter());
  s_state.last_ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());
  s_state.command_write_count = 0;
  s_state.command_read_count = 0;
  s_state.irq_write_count = 0;
  s_state.unmapped_read_logged = false;

  m68k_set_irq(0);
  m68k_pulse_reset();

  VERBOSE_LOG("Eighting.Sound reset sp=0x{:06X} pc=0x{:06X}", m68k_get_reg(nullptr, M68K_REG_SP),
           m68k_get_reg(nullptr, M68K_REG_PC));
}

void Shutdown()
{
  const bool was_active = s_state.active;
  s_state.active = false;
  if (was_active)
  {
    m68k_set_irq(0);
    MusashiBus::ClearCallbacks();
    s_state.ymf271.Shutdown();
  }
  s_state = {};
}

bool IsActive()
{
  return s_state.active;
}

void MainCommandWrite(u8 value)
{
  if (!s_state.active)
    return;

  SynchronizeHostTime();
  s_state.command_latch = value;
  if (s_state.command_write_count < TRACE_LIMIT)
  {
    DEV_LOG("Eighting.Sound command_write={} value=0x{:02X} sound_pc=0x{:06X}", s_state.command_write_count,
             value, m68k_get_reg(nullptr, M68K_REG_PC));
  }
  s_state.command_write_count++;
}

void MainIRQWrite()
{
  if (!s_state.active)
    return;

  SynchronizeHostTime();
  // Musashi is built with interrupt-ack callbacks disabled; its default
  // autovector path clears the asserted level when the interrupt is taken,
  // which matches HOLD_LINE semantics for this IRQ2 strobe.
  m68k_set_irq(2);
  if (s_state.irq_write_count < TRACE_LIMIT)
  {
    DEV_LOG("Eighting.Sound irq2_write={} sound_pc=0x{:06X}", s_state.irq_write_count,
             m68k_get_reg(nullptr, M68K_REG_PC));
  }
  s_state.irq_write_count++;
}

void GenerateAudioFrame(s32* left, s32* right)
{
  if (!left || !right)
    return;

  *left = 0;
  *right = 0;
  if (!s_state.active)
    return;

  s_state.audio_cycle_fraction += SOUND_CLOCK_HZ;
  s_state.audio_target_cycles += s_state.audio_cycle_fraction / OUTPUT_SAMPLE_RATE;
  s_state.audio_cycle_fraction %= OUTPUT_SAMPLE_RATE;
  RunToTarget(s_state.audio_target_cycles);
  s_state.ymf271.GetOutput(left, right);
}

} // namespace SonyZN::EightingRaizingSound
