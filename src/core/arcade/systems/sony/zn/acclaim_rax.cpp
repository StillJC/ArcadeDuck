// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/sony/zn/acclaim_rax.h"

#include "core/arcade/third_party/adsp2181/adsp2181_core.h"
#include "core/spu.h"
#include "core/system.h"

#include "common/error.h"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <utility>

Log_SetChannel(SonyZNAcclaimRAX);

namespace SonyZN::AcclaimRAX {
namespace {

constexpr u32 DSP_CLOCK_HZ = 16'670'000;
constexpr u32 ROM_SIZE = 0x800000;
constexpr u32 ROM_BANK_SIZE = 0x400000;
constexpr u32 DATA_BANK_WORDS = 0x2000;
constexpr u32 CONTROL_BASE = 0x3fe0;
constexpr u32 CONTROL_COUNT = 0x20;
constexpr u32 COMMAND_QUANTUM_CYCLES = (DSP_CLOCK_HZ + 199'999) / 200'000; // 5 us
constexpr size_t MAX_AUDIO_QUEUE_FRAMES = 8192;

// ADSP-2181 control-register indexes at DM 0x3fe0-0x3fff.
enum ControlRegister : u32
{
  IDMA_CONTROL_REG = 0,
  BDMA_INT_ADDR_REG = 1,
  BDMA_EXT_ADDR_REG = 2,
  BDMA_CONTROL_REG = 3,
  BDMA_WORD_COUNT_REG = 4,
  PROG_FLAG_DATA_REG = 5,
  PROG_FLAG_CONTROL_REG = 6,
  S1_AUTOBUF_REG = 15,
  S1_RFSDIV_REG = 16,
  S1_SCLKDIV_REG = 17,
  S1_CONTROL_REG = 18,
  S0_AUTOBUF_REG = 19,
  S0_RFSDIV_REG = 20,
  S0_SCLKDIV_REG = 21,
  S0_CONTROL_REG = 22,
  SYSCONTROL_REG = 31,
};

struct State
{
  bool active = false;
  adsp2181_t* cpu = nullptr;
  std::vector<u8> rom;
  std::array<std::array<u16, DATA_BANK_WORDS>, 4> external_ram{};
  std::array<u16, CONTROL_COUNT> control_regs{};

  u16 host_in = 0xffff;
  u16 host_out = 0xffff;
  u8 pf0 = 1;
  u8 ram_bank = 0;
  u8 rom_bank = 0;

  u64 cycle_fraction = 0;
  u64 audio_target_cycles = 0;
  u64 host_target_cycles = 0;
  u64 host_tick_fraction = 0;
  u64 last_host_sync_ticks = 0;
  u64 last_ticks_per_second = 0;
  u64 board_cycles = 0;

  bool in_cpu_run = false;
  u64 run_board_start = 0;
  u64 run_core_start = 0;

  bool bdma_pending = false;
  u64 bdma_due_cycle = 0;
  u32 bdma_final_source = 0;

  bool sport_active = false;
  u8 sport_ireg = 0;
  s32 sport_inc = 0;
  u16 sport_size = 0;
  u16 sport_base = 0;
  u32 sport_sample_rate = 0;
  u64 sport_period_cycles = 0;
  u64 sport_due_cycle = 0;
  u64 output_sample_phase = 0;
  std::deque<std::pair<s16, s16>> audio_queue;
  s32 last_output_left = 0;
  s32 last_output_right = 0;


  bool dmovlay_warning_logged = false;
  bool rom_bank_warning_logged = false;
  bool bdma_oob_logged = false;
  bool unsupported_bdma_write_logged = false;
};

State s_state;

u64 CurrentBoardCycle()
{
  if (!s_state.in_cpu_run || !s_state.cpu)
    return s_state.board_cycles;

  const u64 core_now = adsp2181_cycles(s_state.cpu);
  return s_state.run_board_start + (core_now - s_state.run_core_start);
}

u16 ReadBoardData(u16 address, u16 fallback)
{
  if (address < DATA_BANK_WORDS && s_state.cpu && adsp2181_dmovlay(s_state.cpu) != 0)
    return s_state.external_ram[s_state.ram_bank][address];

  if (address >= CONTROL_BASE)
  {
    const u32 reg = address - CONTROL_BASE;
    return (reg == PROG_FLAG_DATA_REG) ? static_cast<u16>(s_state.pf0) : s_state.control_regs[reg];
  }

  return fallback;
}

u16 ReadBoardDataFromCore(u16 address)
{
  const u16 fallback = s_state.cpu ? adsp2181_dm(s_state.cpu)[address & 0x3fff] : 0;
  return ReadBoardData(address, fallback);
}

void BeginBDMA(u16 value);
void DisableSPORT();

void WriteBoardDataMapped(u16 address, u16 value)
{
  if (!s_state.cpu)
    return;

  address &= 0x3fff;
  if (address < DATA_BANK_WORDS && adsp2181_dmovlay(s_state.cpu) != 0)
  {
    s_state.external_ram[s_state.ram_bank][address] = value;
    return;
  }

  // RAX BDMA targets the same mapped DM address space used by DSP stores.
  adsp2181_dm(s_state.cpu)[address] = value;
  if (address < CONTROL_BASE)
    return;

  const u32 reg = address - CONTROL_BASE;
  s_state.control_regs[reg] = value;
  switch (reg)
  {
    case BDMA_INT_ADDR_REG:
      s_state.control_regs[reg] = value & 0x3fff;
      break;
    case BDMA_EXT_ADDR_REG:
      s_state.control_regs[reg] = value & 0x3fff;
      break;
    case BDMA_CONTROL_REG:
      s_state.control_regs[reg] = value & 0xff0f;
      break;
    case BDMA_WORD_COUNT_REG:
      BeginBDMA(value);
      break;
    case S0_AUTOBUF_REG:
      if ((value & UINT16_C(0x0002)) == 0)
        DisableSPORT();
      break;
    case S1_CONTROL_REG:
    case S0_CONTROL_REG:
      if (((value >> 4) & 3) >= 2)
        DEV_LOG("SonyZN.RAX compressed SPORT mode requested reg={} value=0x{:04X}", reg, value);
      break;
    default:
      break;
  }
}

void PushNativeFrame(s16 left, s16 right)
{
  if (s_state.audio_queue.size() >= MAX_AUDIO_QUEUE_FRAMES)
  {
    s_state.audio_queue.pop_front();
  }
  s_state.audio_queue.emplace_back(left, right);
}

void DisableSPORT()
{
  s_state.sport_active = false;
  s_state.sport_period_cycles = 0;
  s_state.sport_due_cycle = 0;
  s_state.sport_sample_rate = 0;
  s_state.output_sample_phase = 0;
  s_state.audio_queue.clear();
  s_state.last_output_left = 0;
  s_state.last_output_right = 0;
}

void ConfigureSPORT0()
{
  if (!s_state.cpu)
    return;

  if ((s_state.control_regs[SYSCONTROL_REG] & UINT16_C(0x1000)) == 0 ||
      (s_state.control_regs[S0_AUTOBUF_REG] & UINT16_C(0x0002)) == 0)
  {
    DisableSPORT();
    return;
  }

  const u8 ireg = static_cast<u8>((s_state.control_regs[S0_AUTOBUF_REG] >> 9) & 7);
  u8 mreg = static_cast<u8>((s_state.control_regs[S0_AUTOBUF_REG] >> 7) & 3);
  mreg = static_cast<u8>(mreg | (ireg & 4));

  const s32 inc = static_cast<s32>(adsp2181_m(s_state.cpu, mreg));
  const u16 size = adsp2181_l(s_state.cpu, ireg);
  if (inc == 0 || size == 0)
  {
    DisableSPORT();
    return;
  }

  u16 source = adsp2181_i(s_state.cpu, ireg);
  source = static_cast<u16>((static_cast<s32>(source) - inc) & 0x3fff);
  adsp2181_set_i(s_state.cpu, ireg, source);

  const u32 abs_inc = static_cast<u32>(std::abs(inc));
  const u64 scalar_period = static_cast<u64>(s_state.control_regs[S0_SCLKDIV_REG] + 1u) * 16u;
  const u64 event_period = (scalar_period * size) / (8u * abs_inc);
  if (event_period == 0)
  {
    DisableSPORT();
    return;
  }

  s_state.sport_active = true;
  s_state.sport_ireg = ireg;
  s_state.sport_inc = inc;
  s_state.sport_size = size;
  s_state.sport_base = source;
  s_state.sport_sample_rate = static_cast<u32>(DSP_CLOCK_HZ / scalar_period);
  s_state.sport_period_cycles = event_period;
  s_state.sport_due_cycle = CurrentBoardCycle() + event_period;
  s_state.output_sample_phase = 0;
  s_state.audio_queue.clear();

  adsp2181_request_stop(s_state.cpu);
}

bool CopyBDMAFromROM(u32 count)
{
  if (!s_state.cpu)
    return false;

  const u16 control = s_state.control_regs[BDMA_CONTROL_REG];
  const u32 page = (control >> 8) & 0xff;
  const u32 direction = (control >> 2) & 1;
  const u32 type = control & 3;
  u32 source = (page << 14) | s_state.control_regs[BDMA_EXT_ADDR_REG];
  const u32 bytes_per_word = (type == 0) ? 3u : ((type == 1) ? 2u : 1u);
  const u64 bank_base = static_cast<u64>(s_state.rom_bank) * ROM_BANK_SIZE;
  const u64 absolute_source = bank_base + static_cast<u64>(source);
  const u64 required_end = absolute_source + static_cast<u64>(count) * bytes_per_word;

  // A zero word-count still completes BDMA and raises the completion
  // reset/interrupt; it performs no ROM access.
  if (count == 0)
  {
    s_state.bdma_final_source = source;
    return true;
  }

  if (direction != 0)
  {
    if (!s_state.unsupported_bdma_write_logged)
    {
      s_state.unsupported_bdma_write_logged = true;
      DEV_LOG("SonyZN.RAX unsupported BDMA byte-memory write control=0x{:04X} ext=0x{:04X} count={}",
                  control, s_state.control_regs[BDMA_EXT_ADDR_REG], count);
    }
    return false;
  }

  // The RAX board selects a 4 MiB base with rom_bank, but a BDMA transfer
  // itself is not clipped at that 4 MiB boundary. The reference hardware
  // model takes a pointer to the selected bank and lets src_addr advance
  // across the contiguous ROM region. NBA Jam Extreme relies on this for a
  // short type-0 transfer beginning at 0x3FFFF2.
  if (required_end > s_state.rom.size())
  {
    if (!s_state.bdma_oob_logged)
    {
      s_state.bdma_oob_logged = true;
      DEV_LOG("SonyZN.RAX BDMA source out of range bank={} source=0x{:06X} count={} type={}",
                  s_state.rom_bank, source, count, type);
    }
    return false;
  }

  u16 internal = s_state.control_regs[BDMA_INT_ADDR_REG];
  for (u32 remaining = count; remaining != 0; remaining--)
  {
    const u8* const src = &s_state.rom[static_cast<size_t>(bank_base + source)];
    if (type == 0)
    {
      adsp2181_pm(s_state.cpu)[internal & 0x3fff] =
        (static_cast<u32>(src[0]) << 16) | (static_cast<u32>(src[1]) << 8) | src[2];
      source += 3;
    }
    else if (type == 1)
    {
      WriteBoardDataMapped(internal, static_cast<u16>((static_cast<u16>(src[0]) << 8) | src[1]));
      source += 2;
    }
    else
    {
      const u16 value = static_cast<u16>(src[0]) << ((type == 2) ? 8 : 0);
      WriteBoardDataMapped(internal, value);
      source++;
    }
    internal = static_cast<u16>(internal + 1);
  }

  s_state.control_regs[BDMA_INT_ADDR_REG] = internal;
  s_state.bdma_final_source = source;
  return true;
}

void BeginBDMA(u16 value)
{
  const u32 count = value & 0x3fff;
  s_state.control_regs[BDMA_WORD_COUNT_REG] = static_cast<u16>(count);

  if (!CopyBDMAFromROM(count))
  {
    s_state.control_regs[BDMA_WORD_COUNT_REG] = 0;
    return;
  }

  s_state.bdma_pending = true;
  s_state.bdma_due_cycle = CurrentBoardCycle() + count;
  if (s_state.cpu)
    adsp2181_request_stop(s_state.cpu);


}

void CompleteBDMA()
{
  if (!s_state.bdma_pending || !s_state.cpu)
    return;

  s_state.bdma_pending = false;
  s_state.control_regs[BDMA_WORD_COUNT_REG] = 0;
  s_state.control_regs[BDMA_EXT_ADDR_REG] = static_cast<u16>(s_state.bdma_final_source & 0x3fff);
  s_state.control_regs[BDMA_CONTROL_REG] = static_cast<u16>(
    (s_state.control_regs[BDMA_CONTROL_REG] & ~UINT16_C(0xff00)) |
    (((s_state.bdma_final_source >> 14) & 0xff) << 8));

  if ((s_state.control_regs[BDMA_CONTROL_REG] & UINT16_C(0x0008)) != 0)
  {
    adsp2181_reset(s_state.cpu);
  }
  else
  {
    adsp2181_set_irq(s_state.cpu, ADSP2181_BDMA, 1);
    adsp2181_set_irq(s_state.cpu, ADSP2181_BDMA, 0);
  }
}

void ProcessSPORTEvent()
{
  if (!s_state.sport_active || !s_state.cpu || s_state.sport_inc == 0)
    return;

  const u32 abs_inc = static_cast<u32>(std::abs(s_state.sport_inc));
  const u32 count = static_cast<u32>(s_state.sport_size) / (4u * abs_inc);
  if (count < 2)
  {
    DisableSPORT();
    return;
  }

  s32 reg = static_cast<s32>(adsp2181_i(s_state.cpu, s_state.sport_ireg));
  s16 pending_left = 0;
  bool have_left = false;
  for (u32 i = 0; i < count; i++)
  {
    const s16 sample = static_cast<s16>(ReadBoardDataFromCore(static_cast<u16>(reg & 0x3fff)));
    reg += s_state.sport_inc;
    if (!have_left)
    {
      pending_left = sample;
      have_left = true;
    }
    else
    {
      PushNativeFrame(pending_left, sample);
      have_left = false;
    }
  }

  if (s_state.sport_inc > 0)
  {
    if (reg >= static_cast<s32>(s_state.sport_base) + static_cast<s32>(s_state.sport_size))
      reg = s_state.sport_base;
  }
  else if (reg <= static_cast<s32>(s_state.sport_base) - static_cast<s32>(s_state.sport_size))
  {
    reg = s_state.sport_base;
  }

  adsp2181_set_i(s_state.cpu, s_state.sport_ireg, static_cast<u16>(reg & 0x3fff));
  s_state.sport_due_cycle += s_state.sport_period_cycles;
}

void RunToTarget(u64 target_cycles)
{
  if (!s_state.active || !s_state.cpu || target_cycles <= s_state.board_cycles)
    return;

  while (s_state.board_cycles < target_cycles)
  {
    u64 next_cycle = target_cycles;
    if (s_state.bdma_pending)
      next_cycle = std::min(next_cycle, s_state.bdma_due_cycle);
    if (s_state.sport_active)
      next_cycle = std::min(next_cycle, s_state.sport_due_cycle);

    if (next_cycle > s_state.board_cycles)
    {
      const u64 delta64 = next_cycle - s_state.board_cycles;
      const int delta = static_cast<int>(std::min<u64>(delta64, UINT32_C(0x3fffffff)));
      s_state.in_cpu_run = true;
      s_state.run_board_start = s_state.board_cycles;
      s_state.run_core_start = adsp2181_cycles(s_state.cpu);
      adsp2181_run(s_state.cpu, delta);
      const u64 core_after = adsp2181_cycles(s_state.cpu);
      s_state.in_cpu_run = false;

      const u64 executed = core_after - s_state.run_core_start;
      s_state.board_cycles += executed;

      if (executed < static_cast<u64>(delta))
      {
        if (adsp2181_idle(s_state.cpu))
        {
          s_state.board_cycles = next_cycle;
        }
        else
        {
          // A board callback intentionally stopped the core to install or
          // reconfigure an event. Recompute the next boundary immediately.
          continue;
        }
      }
    }

    if (s_state.bdma_pending && s_state.board_cycles >= s_state.bdma_due_cycle)
      CompleteBDMA();
    if (s_state.sport_active && s_state.board_cycles >= s_state.sport_due_cycle)
      ProcessSPORTEvent();
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
    s_state.host_tick_fraction =
      (s_state.host_tick_fraction * ticks_per_second) / s_state.last_ticks_per_second;
    s_state.last_ticks_per_second = ticks_per_second;
  }

  const u64 elapsed_ticks = now - s_state.last_host_sync_ticks;
  s_state.last_host_sync_ticks = now;
  const u64 numerator = (elapsed_ticks * DSP_CLOCK_HZ) + s_state.host_tick_fraction;
  s_state.host_target_cycles += numerator / ticks_per_second;
  s_state.host_tick_fraction = numerator % ticks_per_second;
  RunToTarget(s_state.host_target_cycles);
}

u16 DataReadCallback(adsp2181_t*, u16 address, u16 fallback)
{
  if (address < DATA_BANK_WORDS && adsp2181_dmovlay(s_state.cpu) > 1 && !s_state.dmovlay_warning_logged)
  {
    s_state.dmovlay_warning_logged = true;
    DEV_LOG("SonyZN.RAX DMOVLAY {} is outside the RAX board's populated mapping",
                adsp2181_dmovlay(s_state.cpu));
  }
  return ReadBoardData(address, fallback);
}

void DataWriteCallback(adsp2181_t*, u16 address, u16 value)
{
  WriteBoardDataMapped(address, value);
}

u16 IOReadCallback(adsp2181_t*, u16 address, u16 fallback)
{
  if (address != 3)
    return fallback;

  adsp2181_set_irq(s_state.cpu, ADSP2181_IRQL0, 0);
  return s_state.host_in;
}

void IOWriteCallback(adsp2181_t*, u16 address, u16 value)
{
  switch (address)
  {
    case 0:
      s_state.ram_bank = static_cast<u8>(value & 3);
      break;
    case 1:
      s_state.rom_bank = static_cast<u8>(value);
      if (s_state.rom_bank > 1 && !s_state.rom_bank_warning_logged)
      {
        s_state.rom_bank_warning_logged = true;
        DEV_LOG("SonyZN.RAX ROM bank {} exceeds populated 8MiB region", s_state.rom_bank);
      }
      break;
    case 3:
      s_state.host_out = value;
      s_state.pf0 = 0;
      break;
    default:
      break;
  }
}

int32_t SPORTReadCallback(adsp2181_t*, int)
{
  return 0;
}

void SPORTWriteCallback(adsp2181_t*, int port, int32_t)
{
  if (port == 0)
    ConfigureSPORT0();
}

void TimerCallback(adsp2181_t*, int)
{
}

void InstallCallbacks()
{
  adsp2181_set_callbacks(s_state.cpu, SPORTReadCallback, SPORTWriteCallback, TimerCallback);
  adsp2181_set_memory_callbacks(s_state.cpu, DataReadCallback, DataWriteCallback, IOReadCallback, IOWriteCallback);
}

void LoadBootWords()
{
  u32* const program = adsp2181_pm(s_state.cpu);
  for (u32 i = 0; i < 32; i++)
  {
    program[i] = (static_cast<u32>(s_state.rom[i * 3 + 0]) << 16) |
                 (static_cast<u32>(s_state.rom[i * 3 + 1]) << 8) |
                 static_cast<u32>(s_state.rom[i * 3 + 2]);
  }
}

} // namespace

bool Initialize(const std::vector<u8>& rom, Error* error)
{
  Shutdown();

  if (rom.size() != ROM_SIZE)
  {
    Error::SetStringFmt(error, "Acclaim RAX ROM has size {}; expected {} bytes.", rom.size(), ROM_SIZE);
    return false;
  }

  s_state.rom = rom;
  s_state.cpu = adsp2181_create();
  if (!s_state.cpu)
  {
    Error::SetStringView(error, "Failed to create Acclaim RAX ADSP-2181 core.");
    s_state = {};
    return false;
  }

  s_state.active = true;
  InstallCallbacks();
  Reset();

  VERBOSE_LOG("SonyZN.RAX initialized adsp2181_clock={} rom={} core='cryan209/modem-dsp-emu@a74f6b7'",
           DSP_CLOCK_HZ, s_state.rom.size());
  return true;
}

void Reset()
{
  if (!s_state.active || !s_state.cpu)
    return;

  adsp2181_reset(s_state.cpu);
  InstallCallbacks();
  LoadBootWords();

  s_state.control_regs.fill(0);
  s_state.host_in = 0xffff;
  s_state.host_out = 0xffff;
  s_state.pf0 = 1;
  s_state.ram_bank = 0;
  s_state.rom_bank = 0;
  s_state.cycle_fraction = 0;
  s_state.audio_target_cycles = 0;
  s_state.host_target_cycles = 0;
  s_state.host_tick_fraction = 0;
  s_state.last_host_sync_ticks = static_cast<u64>(System::GetGlobalTickCounter());
  s_state.last_ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());
  s_state.board_cycles = 0;
  s_state.in_cpu_run = false;
  s_state.bdma_pending = false;
  DisableSPORT();

  s_state.dmovlay_warning_logged = false;
  s_state.rom_bank_warning_logged = false;
  s_state.bdma_oob_logged = false;
  s_state.unsupported_bdma_write_logged = false;


}

void Shutdown()
{
  if (s_state.cpu)
    adsp2181_destroy(s_state.cpu);
  s_state = {};
}

bool IsActive()
{
  return s_state.active;
}

void WriteCommand(u16 value)
{
  if (!s_state.active || !s_state.cpu)
    return;

  SynchronizeHostTime();
  s_state.host_in = value;
  adsp2181_set_irq(s_state.cpu, ADSP2181_IRQL0, 1);

  // MAME gives the RAX board a 5 us perfect-quantum window after each host
  // write. Running that small slice here prevents back-to-back 16-bit writes
  // from overwriting the hardware latch before the DSP can read IO port 3.
  RunToTarget(s_state.board_cycles + COMMAND_QUANTUM_CYCLES);


}

void GenerateAudioFrame(s32* left, s32* right)
{
  if (!left || !right)
    return;

  *left = 0;
  *right = 0;
  if (!s_state.active || !s_state.cpu)
    return;

  s_state.cycle_fraction += DSP_CLOCK_HZ;
  s_state.audio_target_cycles += s_state.cycle_fraction / SPU::SAMPLE_RATE;
  s_state.cycle_fraction %= SPU::SAMPLE_RATE;
  RunToTarget(s_state.audio_target_cycles);

  if (s_state.sport_active && s_state.sport_sample_rate != 0)
  {
    s_state.output_sample_phase += s_state.sport_sample_rate;
    while (s_state.output_sample_phase >= SPU::SAMPLE_RATE)
    {
      s_state.output_sample_phase -= SPU::SAMPLE_RATE;
      if (!s_state.audio_queue.empty())
      {
        const auto sample = s_state.audio_queue.front();
        s_state.audio_queue.pop_front();
        s_state.last_output_left = sample.first;
        s_state.last_output_right = sample.second;
      }
    }
  }

  *left = s_state.last_output_left;
  *right = s_state.last_output_right;
}

} // namespace SonyZN::AcclaimRAX
