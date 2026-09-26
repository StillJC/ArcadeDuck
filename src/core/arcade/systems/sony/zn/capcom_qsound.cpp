// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/sony/zn/capcom_qsound.h"

#include "core/arcade/third_party/qsound_hle/qsound.h"
#include "core/arcade/third_party/z80_superzazu/z80.h"
#include "core/spu.h"
#include "core/system.h"

#include "common/error.h"
#include "common/log.h"

#include <array>
#include <vector>

Log_SetChannel(SonyZNQSound);

namespace SonyZN::CapcomQSound {
namespace {

constexpr u32 Z80_CLOCK_HZ = 8'000'000;
constexpr u32 PERIODIC_IRQ_HZ = 250;
constexpr u32 PERIODIC_IRQ_CYCLES = Z80_CLOCK_HZ / PERIODIC_IRQ_HZ;
constexpr u32 SOUND_ROM_SIZE = 0x40000;
constexpr u32 SAMPLE_ROM_SIZE = 0x400000;
constexpr u32 Z80_FIXED_ROM_END = 0x7fff;
constexpr u32 Z80_BANKED_ROM_BASE = 0x8000;
constexpr u32 Z80_BANKED_ROM_END = 0xbfff;
constexpr u32 Z80_BANK_SIZE = 0x4000;
constexpr u32 QSOUND_WRITE_BASE = 0xd000;
constexpr u32 QSOUND_WRITE_END = 0xd002;
constexpr u32 QSOUND_BANK_REGISTER = 0xd003;
constexpr u32 QSOUND_STATUS_REGISTER = 0xd007;
constexpr u32 Z80_RAM_BASE = 0xf000;
constexpr u32 Z80_RAM_SIZE = 0x1000;
constexpr u32 COMMAND_LOG_LIMIT = 32;
constexpr u32 REGISTER_LOG_LIMIT = 64;

struct State
{
  bool active = false;
  z80 cpu = {};
  qsound_chip qsound = {};
  std::vector<u8> sound_program;
  std::vector<u8> sample_rom;
  std::array<u8, Z80_RAM_SIZE> ram = {};

  u8 sound_bank = 0;
  u8 sound_latch = 0xff;
  bool sound_latch_pending = false;

  s64 cycle_balance = 0;

  // Two independent clocks describe the same 8 MHz sound CPU timeline:
  // audio_target_cycles follows generated 44.1 kHz output frames, while
  // host_target_cycles follows the main CPU's current emulated timestamp.
  // scheduled_target_cycles is the furthest ideal point already submitted
  // to RunCycles(), preventing either source from double-running the Z80.
  u64 cycle_fraction = 0;
  u64 audio_target_cycles = 0;
  u64 host_target_cycles = 0;
  u64 scheduled_target_cycles = 0;
  u64 host_tick_fraction = 0;
  u64 last_host_sync_ticks = 0;
  u64 last_ticks_per_second = 0;

  u64 total_cycles = 0;
  u64 irq_cycle_phase = 0;
  u64 native_sample_phase = 0;
  u32 native_sample_rate = 0;

  s32 last_output_left = 0;
  s32 last_output_right = 0;

  u64 command_writes = 0;
  u64 latch_reads = 0;
  u64 nmi_edges = 0;
  u64 periodic_irqs = 0;
  u64 qsound_writes = 0;
  u64 qsound_status_reads = 0;
  u64 native_samples = 0;
  u64 overwritten_commands = 0;

  u32 register_logs = 0;
  bool unmapped_read_logged = false;
  bool unmapped_write_logged = false;
};

State s_state;

u8 ReadMemory(void*, u16 address)
{
  if (address <= Z80_FIXED_ROM_END)
    return s_state.sound_program[address];

  if (address >= Z80_BANKED_ROM_BASE && address <= Z80_BANKED_ROM_END)
  {
    const u32 offset = Z80_BANKED_ROM_BASE + (static_cast<u32>(s_state.sound_bank) * Z80_BANK_SIZE) +
                       (static_cast<u32>(address) - Z80_BANKED_ROM_BASE);
    return (offset < s_state.sound_program.size()) ? s_state.sound_program[offset] : UINT8_C(0xff);
  }

  if (address == QSOUND_STATUS_REGISTER)
  {
    s_state.qsound_status_reads++;
    return qsound_r(&s_state.qsound);
  }

  if (address >= Z80_RAM_BASE)
    return s_state.ram[address - Z80_RAM_BASE];

  if (!s_state.unmapped_read_logged)
  {
    s_state.unmapped_read_logged = true;
    DEV_LOG("SonyZN.QSound first unmapped Z80 read address=0x{:04X} pc=0x{:04X}", address, s_state.cpu.pc);
  }
  return UINT8_C(0xff);
}

void WriteMemory(void*, u16 address, u8 value)
{
  if (address >= QSOUND_WRITE_BASE && address <= QSOUND_WRITE_END)
  {
    qsound_w(&s_state.qsound, static_cast<u8>(address - QSOUND_WRITE_BASE), value);
    s_state.qsound_writes++;
    if (s_state.register_logs < REGISTER_LOG_LIMIT)
    {
      s_state.register_logs++;
      DEV_LOG("SonyZN.QSound DSP write={} offset={} value=0x{:02X} pc=0x{:04X}", s_state.qsound_writes,
              address - QSOUND_WRITE_BASE, value, s_state.cpu.pc);
    }
    return;
  }

  if (address == QSOUND_BANK_REGISTER)
  {
    s_state.sound_bank = value & 0x0f;
    if (s_state.register_logs < REGISTER_LOG_LIMIT)
    {
      s_state.register_logs++;
      DEV_LOG("SonyZN.QSound ROM bank <- 0x{:02X} pc=0x{:04X}", s_state.sound_bank, s_state.cpu.pc);
    }
    return;
  }

  if (address >= Z80_RAM_BASE)
  {
    s_state.ram[address - Z80_RAM_BASE] = value;
    return;
  }

  if (!s_state.unmapped_write_logged)
  {
    s_state.unmapped_write_logged = true;
    DEV_LOG("SonyZN.QSound first unmapped Z80 write address=0x{:04X} value=0x{:02X} pc=0x{:04X}", address, value,
             s_state.cpu.pc);
  }
}

u8 PortIn(z80* cpu, u8 port)
{
  if (port != 0x00)
    return UINT8_C(0xff);

  State* const state = static_cast<State*>(cpu->userdata);
  state->sound_latch_pending = false;
  state->latch_reads++;
  return state->sound_latch;
}

void PortOut(z80*, u8, u8)
{
}

void InstallCPUCallbacks()
{
  s_state.cpu.read_byte = ReadMemory;
  s_state.cpu.write_byte = WriteMemory;
  s_state.cpu.port_in = PortIn;
  s_state.cpu.port_out = PortOut;
  s_state.cpu.userdata = &s_state;
}

void GenerateNativeSample()
{
  int16_t left = 0;
  int16_t right = 0;
  int16_t* outputs[2] = {&left, &right};
  qsound_stream_update(&s_state.qsound, outputs, 1);
  s_state.last_output_left = static_cast<s32>(left);
  s_state.last_output_right = static_cast<s32>(right);
  s_state.native_samples++;
}

void AdvanceQSound(u32 executed_cycles)
{
  if (s_state.native_sample_rate == 0)
    return;

  s_state.native_sample_phase += static_cast<u64>(executed_cycles) * s_state.native_sample_rate;
  while (s_state.native_sample_phase >= Z80_CLOCK_HZ)
  {
    s_state.native_sample_phase -= Z80_CLOCK_HZ;
    GenerateNativeSample();
  }
}

void AdvancePeriodicIRQ(u32 executed_cycles)
{
  s_state.irq_cycle_phase += executed_cycles;
  while (s_state.irq_cycle_phase >= PERIODIC_IRQ_CYCLES)
  {
    s_state.irq_cycle_phase -= PERIODIC_IRQ_CYCLES;
    z80_gen_int(&s_state.cpu, 0xff);
    s_state.periodic_irqs++;
  }
}

void RunCycles(u32 cycles)
{
  s_state.cycle_balance += cycles;

  while (s_state.cycle_balance > 0)
  {
    s_state.cpu.cyc = 0;
    z80_step(&s_state.cpu);
    const u32 executed = static_cast<u32>(s_state.cpu.cyc);
    if (executed == 0)
      break;

    s_state.cycle_balance -= static_cast<s64>(executed);
    s_state.total_cycles += executed;
    AdvanceQSound(executed);
    AdvancePeriodicIRQ(executed);
  }
}

void RunToTarget(u64 target_cycles)
{
  if (target_cycles <= s_state.scheduled_target_cycles)
    return;

  u64 cycles = target_cycles - s_state.scheduled_target_cycles;
  s_state.scheduled_target_cycles = target_cycles;

  // Normal deltas are only a fraction of a millisecond. Keep this robust if
  // execution resumes after an unusually large timing jump.
  while (cycles != 0)
  {
    const u32 chunk = static_cast<u32>((cycles > UINT64_C(0x01000000)) ? UINT64_C(0x01000000) : cycles);
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

  // Defensive only: resets establish a new epoch, but do not manufacture
  // sound CPU time if an event ever presents an earlier global timestamp.
  if (now < s_state.last_host_sync_ticks)
  {
    s_state.last_host_sync_ticks = now;
    s_state.host_tick_fraction = 0;
    s_state.last_ticks_per_second = ticks_per_second;
    return;
  }

  if (s_state.last_ticks_per_second != ticks_per_second)
  {
    s_state.host_tick_fraction =
      (s_state.host_tick_fraction * ticks_per_second) / s_state.last_ticks_per_second;
    s_state.last_ticks_per_second = ticks_per_second;
  }

  const u64 elapsed_ticks = now - s_state.last_host_sync_ticks;
  s_state.last_host_sync_ticks = now;

  const u64 numerator = (elapsed_ticks * Z80_CLOCK_HZ) + s_state.host_tick_fraction;
  s_state.host_target_cycles += numerator / ticks_per_second;
  s_state.host_tick_fraction = numerator % ticks_per_second;

  RunToTarget(s_state.host_target_cycles);
}

} // namespace

bool Initialize(const std::vector<u8>& sound_program, const std::vector<u8>& sample_rom, Error* error)
{
  Shutdown();

  if (sound_program.size() != SOUND_ROM_SIZE)
  {
    Error::SetStringFmt(error, "Capcom ZN QSound CPU ROM has size {}; expected {} bytes.", sound_program.size(),
                        SOUND_ROM_SIZE);
    return false;
  }
  if (sample_rom.size() != SAMPLE_ROM_SIZE)
  {
    Error::SetStringFmt(error, "Capcom ZN QSound sample ROM has size {}; expected {} bytes.", sample_rom.size(),
                        SAMPLE_ROM_SIZE);
    return false;
  }

  s_state.sound_program = sound_program;
  s_state.sample_rom = sample_rom;
  s_state.active = true;
  Reset();

  VERBOSE_LOG(
    "SonyZN.QSound initialized z80_core='superzazu/z80@d64fe10' z80_clock={} program_rom={} "
    "sample_rom={} qsound_hle='ValleyBell/qsound-hle@68e63be' dsp_clock=60000000 native_rate={} irq_hz={}",
    Z80_CLOCK_HZ, s_state.sound_program.size(), s_state.sample_rom.size(), s_state.native_sample_rate,
    PERIODIC_IRQ_HZ);
  return true;
}

void Reset()
{
  if (!s_state.active)
    return;

  s_state.ram.fill(0);
  s_state.sound_bank = 0;
  s_state.sound_latch = 0xff;
  s_state.sound_latch_pending = false;
  s_state.cycle_balance = 0;
  s_state.cycle_fraction = 0;
  s_state.audio_target_cycles = 0;
  s_state.host_target_cycles = 0;
  s_state.scheduled_target_cycles = 0;
  s_state.host_tick_fraction = 0;
  s_state.last_host_sync_ticks = static_cast<u64>(System::GetGlobalTickCounter());
  s_state.last_ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());
  s_state.total_cycles = 0;
  s_state.irq_cycle_phase = 0;
  s_state.native_sample_phase = 0;
  s_state.last_output_left = 0;
  s_state.last_output_right = 0;
  s_state.command_writes = 0;
  s_state.latch_reads = 0;
  s_state.nmi_edges = 0;
  s_state.periodic_irqs = 0;
  s_state.qsound_writes = 0;
  s_state.qsound_status_reads = 0;
  s_state.native_samples = 0;
  s_state.overwritten_commands = 0;

  s_state.register_logs = 0;
  s_state.unmapped_read_logged = false;
  s_state.unmapped_write_logged = false;

  z80_init(&s_state.cpu);
  InstallCPUCallbacks();

  s_state.native_sample_rate = static_cast<u32>(qsound_start(&s_state.qsound, 60'000'000));
  s_state.qsound.rom_data = s_state.sample_rom.data();
  s_state.qsound.rom_mask = static_cast<unsigned long>(s_state.sample_rom.size() - 1);
  qsound_reset(&s_state.qsound);

  DEV_LOG("SonyZN.QSound reset z80_pc=0x{:04X} bank=0x00", s_state.cpu.pc);
}

void Shutdown()
{
  if (s_state.active)
  {
    DEV_LOG(
      "SonyZN.QSound shutdown cycles={} pc=0x{:04X} commands={} latch_reads={} nmi_edges={} overwrites={} "
      "periodic_irqs={} dsp_writes={} status_reads={} native_samples={}",
      s_state.total_cycles, s_state.cpu.pc, s_state.command_writes, s_state.latch_reads, s_state.nmi_edges,
      s_state.overwritten_commands, s_state.periodic_irqs, s_state.qsound_writes, s_state.qsound_status_reads,
      s_state.native_samples);

    DEV_LOG("SonyZN.QSound command_sync commands={} latch_reads={} nmi_edges={} overwrites={}",
             s_state.command_writes, s_state.latch_reads, s_state.nmi_edges, s_state.overwritten_commands);

  }

  s_state = {};
}

bool IsActive()
{
  return s_state.active;
}

void WriteCommand(u8 value)
{
  if (!s_state.active)
    return;

  // The hardware Z80 runs concurrently with the main CPU. Bring it to the
  // current host timestamp before changing the single-byte latch so a prior
  // command gets the same opportunity to be consumed that it has on hardware.
  SynchronizeHostTime();

  const u8 previous_value = s_state.sound_latch;
  if (s_state.sound_latch_pending)
  {
    s_state.overwritten_commands++;
    if (s_state.overwritten_commands <= 8)
    {
      DEV_LOG("SonyZN.QSound latch overwrite={} old=0x{:02X} new=0x{:02X} z80_pc=0x{:04X}",
              s_state.overwritten_commands, previous_value, value, s_state.cpu.pc);
    }
  }

  s_state.sound_latch = value;
  s_state.command_writes++;

  if (!s_state.sound_latch_pending)
  {
    s_state.sound_latch_pending = true;
    z80_gen_nmi(&s_state.cpu);
    s_state.nmi_edges++;
  }

  if (s_state.command_writes <= COMMAND_LOG_LIMIT)
  {
    DEV_LOG("SonyZN.QSound command={} value=0x{:02X} nmi_pending={} overwrites={} z80_pc=0x{:04X}",
            s_state.command_writes, value, s_state.sound_latch_pending, s_state.overwritten_commands,
            s_state.cpu.pc);
  }
}

void GenerateAudioFrame(s32* left, s32* right)
{
  if (!left || !right)
    return;

  *left = 0;
  *right = 0;
  if (!s_state.active)
    return;

  s_state.cycle_fraction += Z80_CLOCK_HZ;
  s_state.audio_target_cycles += s_state.cycle_fraction / SPU::SAMPLE_RATE;
  s_state.cycle_fraction %= SPU::SAMPLE_RATE;
  RunToTarget(s_state.audio_target_cycles);

  *left = s_state.last_output_left;
  *right = s_state.last_output_right;
}

} // namespace SonyZN::CapcomQSound