// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/sony/zn/atlus_zn1_sound.h"

#include "core/arcade/systems/sony/zn/ymz280b.h"
#include "core/musashi/m68k.h"
#include "core/musashi/m68k_bus.h"
#include "core/system.h"

#include "common/error.h"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <utility>

Log_SetChannel(AtlusZN1Sound);

namespace SonyZN::AtlusZN1Sound {
namespace {

static constexpr u32 SOUND_CLOCK_HZ = 10'000'000;
static constexpr u32 YMZ280B_CLOCK_HZ = 16'934'400;
static constexpr u32 OUTPUT_SAMPLE_RATE = 44'100;
static constexpr u32 SOUND_ROM_SIZE = 0x40000;
static constexpr u32 RAM_BASE = 0x700000;
static constexpr u32 RAM_SIZE = 0x10000;
static constexpr u32 COMMAND_BASE = 0x100000;
static constexpr u32 YMZ_BASE = 0x200000;
static constexpr u32 ADDRESS_MASK = 0x00ffffff;
static constexpr int MAX_EXECUTE_CYCLES = 0x10000;

struct State
{
  bool active = false;
  std::vector<u8> sound_program;
  std::array<u8, RAM_SIZE> ram{};
  YMZ280B ymz280b;

  u16 command_latch = 0;
  u16 sound_control = 0;
  bool command_pending = false;
  bool ymz_irq = false;

  s64 cycle_balance = 0;
  u64 audio_cycle_fraction = 0;
  u64 audio_target_cycles = 0;
  u64 host_target_cycles = 0;
  u64 scheduled_target_cycles = 0;
  u64 host_tick_fraction = 0;
  u64 last_host_sync_ticks = 0;
  u64 last_ticks_per_second = 0;


  bool unmapped_read_logged = false;
  bool unmapped_write_logged = false;
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

  return sp >= RAM_BASE && sp < (RAM_BASE + RAM_SIZE) && (sp & 1u) == 0 && pc < SOUND_ROM_SIZE && (pc & 1u) == 0;
}

u32 GetIRQLevel()
{
  if (s_state.command_pending)
    return 3;
  return s_state.ymz_irq ? 2u : 0u;
}

void UpdateIRQLine()
{
  if (s_state.active)
    m68k_set_irq(GetIRQLevel());
}

void YMZIRQCallback(void*, bool state)
{
  if (!s_state.active)
  {
    s_state.ymz_irq = state;
    return;
  }

  if (s_state.ymz_irq == state)
    return;

  s_state.ymz_irq = state;
  UpdateIRQLine();
}

u16 ReadCommand()
{
  const u16 value = s_state.command_latch;

  // The ATHG-01 reference path uses a generic 16-bit latch whose pending line
  // drives IRQ3. Reading the latch consumes the pending command and releases
  // IRQ3. Sound-side writes to the same decoded address remain separate state.
  s_state.command_pending = false;
  UpdateIRQLine();
  return value;
}

void WriteSoundControl(u16 value)
{
  s_state.sound_control = value;
}

u8 ReadByte(u32 address)
{
  address &= ADDRESS_MASK;

  if (address < SOUND_ROM_SIZE)
    return s_state.sound_program[address];

  if (address >= RAM_BASE && address < (RAM_BASE + RAM_SIZE))
    return s_state.ram[address - RAM_BASE];

  if (address == COMMAND_BASE || address == (COMMAND_BASE + 1))
  {
    const u16 value = ReadCommand();
    return (address == COMMAND_BASE) ? static_cast<u8>(value >> 8) : static_cast<u8>(value);
  }

  // YMZ280B is connected to the low byte of the 68000's 16-bit bus.
  if (address == (YMZ_BASE + 1))
    return s_state.ymz280b.ReadData();
  if (address == (YMZ_BASE + 3))
    return s_state.ymz280b.ReadStatus();
  if (address >= YMZ_BASE && address <= (YMZ_BASE + 3))
    return UINT8_C(0xff);

  if (!s_state.unmapped_read_logged)
  {
    s_state.unmapped_read_logged = true;
    DEV_LOG("AtlusZN1.Sound first unmapped 68000 read address=0x{:06X} pc=0x{:06X}", address,
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

  if (address == COMMAND_BASE || address == (COMMAND_BASE + 1))
  {
    const u32 shift = (address == COMMAND_BASE) ? 8u : 0u;
    const u16 mask = static_cast<u16>(UINT16_C(0x00ff) << shift);
    const u16 byte_value = static_cast<u16>(static_cast<u16>(value) << shift);
    WriteSoundControl(static_cast<u16>((s_state.sound_control & ~mask) | byte_value));
    return;
  }

  if (address == (YMZ_BASE + 1))
  {
    s_state.ymz280b.WriteAddress(value);
    return;
  }
  if (address == (YMZ_BASE + 3))
  {
    s_state.ymz280b.WriteData(value);
    return;
  }
  if (address >= YMZ_BASE && address <= (YMZ_BASE + 3))
    return;

  if (!s_state.unmapped_write_logged)
  {
    s_state.unmapped_write_logged = true;
    DEV_LOG("AtlusZN1.Sound first unmapped 68000 write address=0x{:06X} value=0x{:02X} pc=0x{:06X}",
                address, value, m68k_get_reg(nullptr, M68K_REG_PC));
  }
}

u32 ReadMemory8ForCore(u32 address)
{
  return ReadByte(address);
}

u32 ReadMemory16ForCore(u32 address)
{
  address &= ADDRESS_MASK;
  if (address == COMMAND_BASE)
    return ReadCommand();

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
  address &= ADDRESS_MASK;
  if (address == COMMAND_BASE)
  {
    WriteSoundControl(static_cast<u16>(value));
    return;
  }

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
    UpdateIRQLine();
    const int executed = m68k_execute(requested_cycles);
    if (executed <= 0)
      break;

    s_state.ymz280b.AdvanceCycles(static_cast<u32>(executed), SOUND_CLOCK_HZ);
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

bool Initialize(std::vector<u8> sound_program, std::vector<u8> ymz280b_rom, Error* error)
{
  Shutdown();

  if (sound_program.size() != SOUND_ROM_SIZE)
  {
    Error::SetStringFmt(error, "Atlus ZN-1 68000 ROM has size {}; expected {} bytes.", sound_program.size(),
                        SOUND_ROM_SIZE);
    return false;
  }
  if (ymz280b_rom.size() != 0x800000)
  {
    Error::SetStringFmt(error, "Atlus ZN-1 YMZ280B ROM has size {}; expected {} bytes.", ymz280b_rom.size(),
                        UINT32_C(0x800000));
    return false;
  }

  u32 initial_sp = 0;
  u32 initial_pc = 0;
  if (!HasPlausibleResetVectors(sound_program, &initial_sp, &initial_pc))
  {
    Error::SetStringFmt(error, "Atlus ZN-1 68000 reset vectors are invalid (SP=0x{:08X}, PC=0x{:08X}).", initial_sp,
                        initial_pc);
    return false;
  }

  s_state.sound_program = std::move(sound_program);
  s_state.ymz280b.Initialize(ymz280b_rom, YMZ280B_CLOCK_HZ, YMZIRQCallback, nullptr);
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
    "AtlusZN1.Sound initialized cpu='MC68000' clock_hz={} rom_size={} ram_size={} initial_sp=0x{:06X} "
    "initial_pc=0x{:06X} ymz280b_clock={} ymz280b_rate={} ymz280b_rom={} irq2='YMZ280B' irq3='PSX command latch'",
    SOUND_CLOCK_HZ, s_state.sound_program.size(), RAM_SIZE, initial_sp, initial_pc, s_state.ymz280b.GetClockHz(),
    s_state.ymz280b.GetNativeSampleRate(), s_state.ymz280b.GetROMSize());
  return true;
}

void Reset()
{
  if (!s_state.active)
    return;

  s_state.ram.fill(0);
  s_state.command_latch = 0;
  s_state.sound_control = 0;
  s_state.command_pending = false;
  s_state.ymz_irq = false;
  s_state.ymz280b.Reset();

  s_state.cycle_balance = 0;
  s_state.audio_cycle_fraction = 0;
  s_state.audio_target_cycles = 0;
  s_state.host_target_cycles = 0;
  s_state.scheduled_target_cycles = 0;
  s_state.host_tick_fraction = 0;
  s_state.last_host_sync_ticks = static_cast<u64>(System::GetGlobalTickCounter());
  s_state.last_ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());


  s_state.unmapped_read_logged = false;
  s_state.unmapped_write_logged = false;

  m68k_set_irq(0);
  m68k_pulse_reset();

}

void Shutdown()
{
  const bool was_active = s_state.active;

  // Disable callbacks before shutting down YMZ so its IRQ-release callback cannot
  // reassert a stale 68000 level while the device is being torn down.
  s_state.active = false;
  s_state.ymz280b.Shutdown();
  if (was_active)
  {
    m68k_set_irq(0);
    MusashiBus::ClearCallbacks();
  }
  s_state = {};
}

bool IsActive()
{
  return s_state.active;
}

void MainCommandWrite(u16 value)
{
  if (!s_state.active)
    return;

  SynchronizeHostTime();
  s_state.command_latch = value;
  s_state.command_pending = true;
  UpdateIRQLine();
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
  s_state.ymz280b.GetOutput(left, right);
}

} // namespace SonyZN::AtlusZN1Sound