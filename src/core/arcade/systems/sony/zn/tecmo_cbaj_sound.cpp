// SPDX-FileCopyrightText: 2026 StillJC

// SPDX-License-Identifier: GPL-3.0-only



#include "core/arcade/systems/sony/zn/tecmo_cbaj_sound.h"



#include "core/arcade/systems/sony/zn/ymz280b.h"

#include "core/arcade/third_party/z80_superzazu/z80.h"

#include "core/system.h"



#include "common/error.h"

#include "common/log.h"



#include <array>

#include <vector>



Log_SetChannel(SonyZNTecmoCBAJSound);



namespace SonyZN::TecmoCBAJSound {

namespace {



constexpr u32 Z80_CLOCK_HZ = 4'000'000;

constexpr u32 YMZ280B_CLOCK_HZ = 16'934'400;

constexpr u32 OUTPUT_SAMPLE_RATE = 44'100;

constexpr u32 SOUND_ROM_SIZE = 0x40000;

constexpr u32 YMZ280B_ROM_SIZE = 0x800000;

constexpr u32 Z80_ROM_END = 0x7fff;

constexpr u32 Z80_RAM_BASE = 0x8000;

constexpr u32 Z80_RAM_SIZE = 0x8000;

constexpr u32 FIFO_CAPACITY = 1024;



struct ByteFIFO

{

  std::array<u8, FIFO_CAPACITY> data{};

  u32 read = 0;

  u32 write = 0;

  u32 count = 0;



  void Clear()

  {

    read = 0;

    write = 0;

    count = 0;

  }



  bool Empty() const { return count == 0; }

  bool Full() const { return count == FIFO_CAPACITY; }



  bool Push(u8 value)

  {

    if (Full())

      return false;



    data[write] = value;

    write = (write + 1) % FIFO_CAPACITY;

    count++;

    return true;

  }



  u8 Pop()

  {

    if (Empty())

      return UINT8_C(0xff);



    const u8 value = data[read];

    read = (read + 1) % FIFO_CAPACITY;

    count--;

    return value;

  }

};



struct State

{

  bool active = false;

  z80 cpu = {};

  std::vector<u8> sound_program;

  std::array<u8, Z80_RAM_SIZE> ram{};



  ByteFIFO main_to_sound;

  ByteFIFO sound_to_main;

  YMZ280B ymz280b;



  s64 cycle_balance = 0;

  u64 audio_cycle_fraction = 0;

  u64 audio_target_cycles = 0;

  u64 host_target_cycles = 0;

  u64 scheduled_target_cycles = 0;

  u64 host_tick_fraction = 0;

  u64 last_host_sync_ticks = 0;

  u64 last_ticks_per_second = 0;



  bool fifo_overflow_logged = false;

  bool unmapped_port_logged = false;

  bool unmapped_memory_read_logged = false;

  bool unmapped_memory_write_logged = false;

};



State s_state;



u8 ReadMemory(void*, u16 address)

{

  if (address <= Z80_ROM_END)

    return s_state.sound_program[address];



  if (address >= Z80_RAM_BASE)

    return s_state.ram[address - Z80_RAM_BASE];



  if (!s_state.unmapped_memory_read_logged)

  {

    s_state.unmapped_memory_read_logged = true;

    DEV_LOG("TecmoCBAJ.Sound first unmapped Z80 read address=0x{:04X} pc=0x{:04X}", address, s_state.cpu.pc);

  }

  return UINT8_C(0xff);

}



void WriteMemory(void*, u16 address, u8 value)

{

  if (address >= Z80_RAM_BASE)

  {

    s_state.ram[address - Z80_RAM_BASE] = value;

    return;

  }



  if (!s_state.unmapped_memory_write_logged)

  {

    s_state.unmapped_memory_write_logged = true;

    DEV_LOG("TecmoCBAJ.Sound first unmapped Z80 write address=0x{:04X} value=0x{:02X} pc=0x{:04X}", address,

                value, s_state.cpu.pc);

  }

}



u8 PortIn(z80*, u8 port)

{

  switch (port)

  {

    case 0x84:

      return s_state.ymz280b.ReadData();



    case 0x85:

      return s_state.ymz280b.ReadStatus();



    case 0x90:

      return s_state.main_to_sound.Pop();



    case 0x91:

      // IDT7202 /EF is active-low: 0 when the receive FIFO is empty, 1 when data is available.

      return s_state.main_to_sound.Empty() ? UINT8_C(0x00) : UINT8_C(0x02);



    default:

      if (!s_state.unmapped_port_logged)

      {

        s_state.unmapped_port_logged = true;

        DEV_LOG("TecmoCBAJ.Sound first unmapped Z80 port read port=0x{:02X} pc=0x{:04X}", port,

                    s_state.cpu.pc);

      }

      return UINT8_C(0xff);

  }

}



void PortOut(z80*, u8 port, u8 value)

{

  switch (port)

  {

    case 0x84:

      s_state.ymz280b.WriteAddress(value);

      return;



    case 0x85:

      s_state.ymz280b.WriteData(value);

      return;



    case 0x90:

      if (!s_state.sound_to_main.Push(value))

      {

        if (!s_state.fifo_overflow_logged)

        {

          s_state.fifo_overflow_logged = true;

          WARNING_LOG("TecmoCBAJ.Sound Z80-to-main FIFO overflow pc=0x{:04X}", s_state.cpu.pc);

        }

      }

      return;



    default:

      if (!s_state.unmapped_port_logged)

      {

        s_state.unmapped_port_logged = true;

        DEV_LOG("TecmoCBAJ.Sound first unmapped Z80 port write port=0x{:02X} value=0x{:02X} pc=0x{:04X}",

                    port, value, s_state.cpu.pc);

      }

      return;

  }

}



void InstallCPUCallbacks()

{

  s_state.cpu.read_byte = ReadMemory;

  s_state.cpu.write_byte = WriteMemory;

  s_state.cpu.port_in = PortIn;

  s_state.cpu.port_out = PortOut;

  s_state.cpu.userdata = &s_state;

}



void ResetZ80Core()

{

  z80_init(&s_state.cpu);

  InstallCPUCallbacks();

  s_state.cycle_balance = 0;

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



    s_state.ymz280b.AdvanceCycles(executed, Z80_CLOCK_HZ);

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



  const u64 numerator = (elapsed_ticks * Z80_CLOCK_HZ) + s_state.host_tick_fraction;

  s_state.host_target_cycles += numerator / ticks_per_second;

  s_state.host_tick_fraction = numerator % ticks_per_second;

  RunToTarget(s_state.host_target_cycles);

}



} // namespace



bool Initialize(const std::vector<u8>& sound_program, const std::vector<u8>& ymz280b_rom, Error* error)

{

  Shutdown();



  if (sound_program.size() != SOUND_ROM_SIZE)

  {

    Error::SetStringFmt(error, "Tecmo CBAJ Z80 ROM has size {}; expected {} bytes.", sound_program.size(),

                        SOUND_ROM_SIZE);

    return false;

  }

  if (ymz280b_rom.size() != YMZ280B_ROM_SIZE)

  {

    Error::SetStringFmt(error, "Tecmo CBAJ YMZ280B sample ROM has size {}; expected {} bytes.", ymz280b_rom.size(),

                        YMZ280B_ROM_SIZE);

    return false;

  }



  s_state.sound_program = sound_program;



  // The CBAJ PCB does not wire the YMZ280B IRQ output to the Z80, so the

  // reusable device intentionally has no IRQ callback for this board.

  s_state.ymz280b.Initialize(ymz280b_rom, YMZ280B_CLOCK_HZ);

  s_state.active = true;

  Reset();



  VERBOSE_LOG(

    "TecmoCBAJ.Sound initialized z80_clock={} ymz280b_clock={} ymz280b_rate={} program_rom={} ymz280b_rom={} "

    "fifo='2x IDT7202 1024-byte' ymz280b='shared 8-voice core; IRQ unconnected on CBAJ'",

    Z80_CLOCK_HZ, s_state.ymz280b.GetClockHz(), s_state.ymz280b.GetNativeSampleRate(), s_state.sound_program.size(),

    s_state.ymz280b.GetROMSize());

  return true;

}



void Reset()

{

  if (!s_state.active)

    return;



  s_state.ram.fill(0);

  s_state.main_to_sound.Clear();

  s_state.sound_to_main.Clear();

  s_state.ymz280b.Reset();



  s_state.cycle_balance = 0;

  s_state.audio_cycle_fraction = 0;

  s_state.audio_target_cycles = 0;

  s_state.host_target_cycles = 0;

  s_state.scheduled_target_cycles = 0;

  s_state.host_tick_fraction = 0;

  s_state.last_host_sync_ticks = static_cast<u64>(System::GetGlobalTickCounter());

  s_state.last_ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());



  s_state.fifo_overflow_logged = false;

  s_state.unmapped_port_logged = false;

  s_state.unmapped_memory_read_logged = false;

  s_state.unmapped_memory_write_logged = false;



  ResetZ80Core();

}



void Shutdown()

{

  s_state = {};

}



bool IsActive()

{

  return s_state.active;

}



u8 MainDataRead()

{

  if (!s_state.active)

    return UINT8_C(0xff);



  SynchronizeHostTime();

  return s_state.sound_to_main.Pop();

}



void MainDataWrite(u8 value)

{

  if (!s_state.active)

    return;



  SynchronizeHostTime();

  if (!s_state.main_to_sound.Push(value))

  {

    if (!s_state.fifo_overflow_logged)

    {

      s_state.fifo_overflow_logged = true;

      WARNING_LOG("TecmoCBAJ.Sound main-to-Z80 FIFO overflow");

    }

  }

}



u8 MainStatusRead()

{

  if (!s_state.active)

    return UINT8_C(0x00);



  SynchronizeHostTime();

  // Match the physical IDT7202 /EF output used on status bit 1.

  return s_state.sound_to_main.Empty() ? UINT8_C(0x00) : UINT8_C(0x02);

}



void GenerateAudioFrame(s32* left, s32* right)

{

  if (!left || !right)

    return;



  *left = 0;

  *right = 0;

  if (!s_state.active)

    return;



  s_state.audio_cycle_fraction += Z80_CLOCK_HZ;

  s_state.audio_target_cycles += s_state.audio_cycle_fraction / OUTPUT_SAMPLE_RATE;

  s_state.audio_cycle_fraction %= OUTPUT_SAMPLE_RATE;

  RunToTarget(s_state.audio_target_cycles);



  s_state.ymz280b.GetOutput(left, right);

}



} // namespace SonyZN::TecmoCBAJSound