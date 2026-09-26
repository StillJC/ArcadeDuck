// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/pch.h"
#include "core/arcade/systems/sony/zn/taito_fx1b_zoom.h"

#include "core/arcade/systems/konami/gq/konami_gq_tms57002.h"
#include "core/arcade/systems/sony/zn/taito_fx1b_mn10200.h"
#include "core/arcade/systems/sony/zn/taito_fx1b_zsg2.h"

#include "core/spu.h"
#include "core/system.h"

#include "common/log.h"

#include <algorithm>
#include <array>
#include <deque>
#include <span>
#include <vector>

Log_SetChannel(TaitoFX1BZoom);

namespace TaitoFX1BZoom {

namespace {

constexpr u32 MN_ROM_BASE = 0x080000;
constexpr u32 MN_ROM_SIZE = 0x080000;
constexpr u32 MN_RAM_BASE = 0x400000;
constexpr u32 MN_RAM_SIZE = 0x020000;
constexpr u32 ZSG2_BASE = 0x800000;
constexpr u32 ZSG2_END = 0x8007ff;
constexpr u32 TMS_DATA = 0xc00000;
constexpr u32 SHARED_BASE = 0xe00000;
constexpr u32 SHARED_SIZE = 0x100;

constexpr u32 MN_PHYSICAL_CLOCK_HZ = 12'500'000;
constexpr u32 MN_EXECUTE_CLOCK_HZ = MN_PHYSICAL_CLOCK_HZ;
constexpr u32 ZSG2_CLOCK_HZ = 25'000'000;
constexpr u32 ZSG2_CLOCKS_PER_SAMPLE = 768;
constexpr u32 MN_CYCLES_PER_NATIVE_FRAME = 384;
[[maybe_unused]] constexpr u32 TMS_CLOCK_HZ = 12'500'000;
constexpr u32 TMS_CYCLES_PER_NATIVE_FRAME = 384;
constexpr u64 RESAMPLE_DENOMINATOR =
  static_cast<u64>(ZSG2_CLOCKS_PER_SAMPLE) * static_cast<u64>(SPU::SAMPLE_RATE);
constexpr size_t MAX_NATIVE_QUEUE_FRAMES = 8192;

struct StereoFrame
{
  s32 left = 0;
  s32 right = 0;
};

struct State
{
  bool active = false;
  std::vector<u8> mn_rom;
  std::array<u8, MN_RAM_SIZE> mn_ram{};
  u8* shared_ram = nullptr;
  size_t shared_ram_size = 0;

  MN10200::Core mn10200;
  TaitoFX1BZSG2::Chip zsg2;
  KonamiGQTMS57002::Core tms57002;

  u8 tms_ctrl = 0;
  u8 reg_address = 0;
  u16 gain_left = 0x3f;
  u16 gain_right = 0x3f;

  u64 last_global_ticks = 0;
  u64 cycle_fraction = 0;
  s64 cycle_balance = 0;
  u32 native_cycle_accumulator = 0;

  std::deque<StereoFrame> native_queue;
  StereoFrame resample_a{};
  StereoFrame resample_b{};
  bool have_resample_a = false;
  bool have_resample_b = false;
  u64 resample_phase = 0;

  u64 cpu_cycles = 0;
  u64 native_frames = 0;
};

State s_state;

u8 ReadMemory8(u32 address);
u16 ReadMemory16(u32 address);
void WriteMemory8(u32 address, u8 value);
void WriteMemory16(u32 address, u16 value);
u8 ReadPort(u32 port);
void WritePort(u32 port, u8 value);

void UpdateTMSIRQ1()
{
  if (!s_state.active)
    return;

  // Taito ZOOM wires the TMS57002 EMPTY output to MN10200 IRQ1 through
  // an inverter. EMPTY high therefore releases the active-low IRQ pin.
  s_state.mn10200.SetInput(1, !s_state.tms57002.UpdateFIFOEmpty());
}

u8 ReadROMByte(u32 address)
{
  const u32 offset = address - MN_ROM_BASE;
  return (offset < s_state.mn_rom.size()) ? s_state.mn_rom[offset] : UINT8_C(0xff);
}

u8 ReadMemory8(u32 address)
{
  address &= 0x00ffffffu;

  if (address >= MN_ROM_BASE && address < (MN_ROM_BASE + MN_ROM_SIZE))
    return ReadROMByte(address);

  if (address >= MN_RAM_BASE && address < (MN_RAM_BASE + MN_RAM_SIZE))
    return s_state.mn_ram[address - MN_RAM_BASE];

  if (address >= ZSG2_BASE && address <= ZSG2_END)
  {
    // MAME's ZSG-2 device only accepts full 16-bit accesses.
    return 0;
  }

  if (address == TMS_DATA)
    return s_state.tms57002.DataRead();

  if (address >= SHARED_BASE && address < (SHARED_BASE + SHARED_SIZE))
  {
    const size_t index = static_cast<size_t>(address - SHARED_BASE);
    return (s_state.shared_ram && index < s_state.shared_ram_size) ? s_state.shared_ram[index] : UINT8_C(0xff);
  }

  return UINT8_C(0xff);
}

u16 ReadMemory16(u32 address)
{
  address &= 0x00fffffeu;

  if (address >= ZSG2_BASE && address <= (ZSG2_END - 1))
    return s_state.zsg2.Read((address - ZSG2_BASE) >> 1);

  return static_cast<u16>(ReadMemory8(address) | (static_cast<u16>(ReadMemory8(address + 1)) << 8));
}

void WriteMemory8(u32 address, u8 value)
{
  address &= 0x00ffffffu;

  if (address >= MN_RAM_BASE && address < (MN_RAM_BASE + MN_RAM_SIZE))
  {
    s_state.mn_ram[address - MN_RAM_BASE] = value;
    return;
  }

  if (address >= ZSG2_BASE && address <= ZSG2_END)
  {
    // The physical ZSG-2 window is a 16-bit register bus. MAME rejects
    // partial-byte accesses as well, so do not synthesize an RMW here.
    return;
  }

  if (address == TMS_DATA)
  {
    s_state.tms57002.DataWrite(value);
    UpdateTMSIRQ1();
    return;
  }

  if (address >= SHARED_BASE && address < (SHARED_BASE + SHARED_SIZE))
  {
    const size_t index = static_cast<size_t>(address - SHARED_BASE);
    if (s_state.shared_ram && index < s_state.shared_ram_size)
      s_state.shared_ram[index] = value;
  }
}

void WriteMemory16(u32 address, u16 value)
{
  address &= 0x00fffffeu;

  if (address >= ZSG2_BASE && address <= (ZSG2_END - 1))
  {
    s_state.zsg2.Write((address - ZSG2_BASE) >> 1, value);
    return;
  }

  WriteMemory8(address, static_cast<u8>(value));
  WriteMemory8(address + 1, static_cast<u8>(value >> 8));
}

u8 ReadPort(u32 port)
{
  return (port == 1) ? s_state.tms_ctrl : UINT8_C(0xff);
}

void WritePort(u32 port, u8 value)
{
  if (port != 1)
    return;

  // MAME's Taito ZOOM device leaves the TMS reset line released during
  // transfers and drives only CLOAD (bit 1) and PLOAD (bit 0).
  s_state.tms_ctrl = value;
  s_state.tms57002.CLoadWrite((value & 0x02) != 0);
  s_state.tms57002.PLoadWrite((value & 0x01) != 0);
  UpdateTMSIRQ1();
}

void QueueNativeFrame(const StereoFrame& frame)
{
  if (s_state.native_queue.size() >= MAX_NATIVE_QUEUE_FRAMES)
    s_state.native_queue.pop_front();
  s_state.native_queue.push_back(frame);
}

void GenerateNativeFrame()
{
  std::array<s32, 4> zsg{};
  s_state.zsg2.GenerateFrame(&zsg);

  // Physical ZROM routing:
  //   ZSG2 0/1 -> TMS inputs 0/1 at 0.5 (effect sends)
  //   ZSG2 2/3 -> TMS inputs 2/3 at 1.0 (direct L/R)
  s_state.tms57002.SetSerialInputs(zsg[0] / 2, zsg[1] / 2, zsg[2], zsg[3]);
  s_state.tms57002.Sync();
  s_state.tms57002.Execute(static_cast<int>(TMS_CYCLES_PER_NATIVE_FRAME));
  UpdateTMSIRQ1();

  StereoFrame frame;
  frame.left =
    static_cast<s32>((static_cast<s64>(s_state.tms57002.GetSerialOutput(2)) * s_state.gain_left) / 63);
  frame.right =
    static_cast<s32>((static_cast<s64>(s_state.tms57002.GetSerialOutput(3)) * s_state.gain_right) / 63);

  QueueNativeFrame(frame);
  s_state.native_frames++;
}

s64 RunSoundCPUCycles(s64 cycles)
{
  s64 total_executed = 0;
  while (cycles > 0)
  {
    const u32 until_native = MN_CYCLES_PER_NATIVE_FRAME - s_state.native_cycle_accumulator;
    const int request = static_cast<int>(std::min<s64>(cycles, std::max<u32>(until_native, 1)));
    const int executed = s_state.mn10200.Execute(request);
    if (executed <= 0)
      break;

    s_state.cpu_cycles += static_cast<u64>(executed);
    total_executed += static_cast<s64>(executed);
    cycles -= static_cast<s64>(executed);
    s_state.native_cycle_accumulator += static_cast<u32>(executed);

    while (s_state.native_cycle_accumulator >= MN_CYCLES_PER_NATIVE_FRAME)
    {
      s_state.native_cycle_accumulator -= MN_CYCLES_PER_NATIVE_FRAME;
      GenerateNativeFrame();
    }
  }

  return total_executed;
}

void SynchronizeToNow()
{
  if (!s_state.active)
    return;

  const u64 now = static_cast<u64>(System::GetGlobalTickCounter());
  if (now <= s_state.last_global_ticks)
    return;

  const u64 elapsed = now - s_state.last_global_ticks;
  s_state.last_global_ticks = now;

  const u64 ticks_per_second = static_cast<u64>(std::max<TickCount>(System::GetTicksPerSecond(), 1));
  const u64 numerator = (elapsed * static_cast<u64>(MN_EXECUTE_CLOCK_HZ)) + s_state.cycle_fraction;
  const u64 due_cycles = numerator / ticks_per_second;
  s_state.cycle_fraction = numerator % ticks_per_second;

  // Instruction execution may pass the requested boundary by one instruction.
  // Carry that signed overrun forward rather than accumulating timing error at
  // every host access or audio synchronization point.
  s_state.cycle_balance += static_cast<s64>(due_cycles);
  if (s_state.cycle_balance > 0)
  {
    const s64 executed = RunSoundCPUCycles(s_state.cycle_balance);
    s_state.cycle_balance -= executed;
  }
}

bool PopNativeFrame(StereoFrame* frame)
{
  if (!frame || s_state.native_queue.empty())
    return false;

  *frame = s_state.native_queue.front();
  s_state.native_queue.pop_front();
  return true;
}

void PrimeResampler()
{
  if (!s_state.have_resample_a)
    s_state.have_resample_a = PopNativeFrame(&s_state.resample_a);

  if (!s_state.have_resample_b)
  {
    s_state.have_resample_b = PopNativeFrame(&s_state.resample_b);
    if (!s_state.have_resample_b && s_state.have_resample_a)
    {
      s_state.resample_b = s_state.resample_a;
      s_state.have_resample_b = true;
    }
  }
}

} // namespace

bool Initialize(std::span<const u8> mn10200_rom, std::span<const u8> zsg2_rom, std::span<u8> shared_ram)
{
  if (mn10200_rom.size() != MN_ROM_SIZE || zsg2_rom.empty() || shared_ram.size() < SHARED_SIZE)
    return false;

  Shutdown();

  s_state.mn_rom.assign(mn10200_rom.begin(), mn10200_rom.end());
  s_state.mn_ram.fill(0);
  s_state.shared_ram = shared_ram.data();
  s_state.shared_ram_size = shared_ram.size();
  s_state.tms_ctrl = 0;
  s_state.reg_address = 0;
  s_state.gain_left = 0x3f;
  s_state.gain_right = 0x3f;
  s_state.cycle_fraction = 0;
  s_state.cycle_balance = 0;
  s_state.native_cycle_accumulator = 0;
  s_state.native_queue.clear();
  s_state.have_resample_a = false;
  s_state.have_resample_b = false;
  s_state.resample_phase = 0;
  s_state.cpu_cycles = 0;
  s_state.native_frames = 0;

  if (!s_state.zsg2.Initialize(zsg2_rom))
  {
    Shutdown();
    return false;
  }

  s_state.active = true;

  s_state.mn10200.Initialize(ReadMemory8, ReadMemory16, WriteMemory8, WriteMemory16, ReadPort, WritePort);

  s_state.tms57002.Reset();
  s_state.tms57002.SetResetReleased(true);
  s_state.tms57002.CLoadWrite(false);
  s_state.tms57002.PLoadWrite(false);
  UpdateTMSIRQ1();

  s_state.last_global_ticks = static_cast<u64>(System::GetGlobalTickCounter());

  VERBOSE_LOG(
    "TaitoFX1B.ZOOM initialized mn10200='12.5MHz execute / 6.25MHz timer clock' "
    "zsg2='25MHz /768 = 32.552kHz' tms57002='12.5MHz' program_rom={} sample_rom={} shared_ram={}B",
    s_state.mn_rom.size(), zsg2_rom.size(), SHARED_SIZE);
  return true;
}

void Reset()
{
  if (!s_state.active)
    return;

  SynchronizeToNow();

  s_state.mn10200.Reset();
  s_state.zsg2.Reset();
  s_state.tms57002.Reset();
  s_state.tms57002.SetResetReleased(true);
  s_state.tms57002.CLoadWrite(false);
  s_state.tms57002.PLoadWrite(false);

  s_state.tms_ctrl = 0;
  s_state.reg_address = 0;
  s_state.cycle_fraction = 0;
  s_state.cycle_balance = 0;
  s_state.native_cycle_accumulator = 0;
  s_state.native_queue.clear();
  s_state.have_resample_a = false;
  s_state.have_resample_b = false;
  s_state.resample_phase = 0;
  s_state.last_global_ticks = static_cast<u64>(System::GetGlobalTickCounter());
  UpdateTMSIRQ1();
}

void Shutdown()
{
  if (s_state.active)
  {
    VERBOSE_LOG("TaitoFX1B.ZOOM shutdown cpu_cycles={} native_frames={} tms_cycles={} tms_syncs={}",
             s_state.cpu_cycles, s_state.native_frames, s_state.tms57002.GetExecutedCycles(),
             s_state.tms57002.GetSyncCount());
  }

  s_state = {};
}

bool IsActive()
{
  return s_state.active;
}

void RegAddressWrite(u16 data)
{
  if (!s_state.active)
    return;

  SynchronizeToNow();
  s_state.reg_address = static_cast<u8>(data);
}

void RegDataWrite(u16 data)
{
  if (!s_state.active)
    return;

  SynchronizeToNow();

  switch (s_state.reg_address)
  {
    case 0x04:
      s_state.gain_left = data & UINT16_C(0x003f);
      break;
    case 0x05:
      s_state.gain_right = data & UINT16_C(0x003f);
      break;
    default:
      break;
  }
}

u16 StatusRead()
{
  if (!s_state.active)
    return 0;

  SynchronizeToNow();
  // Current MAME hardware model returns zero; bit 0 is only noted as a
  // possible busy flag and is not implemented as asserted.
  return 0;
}

void PulseMainIRQ()
{
  if (!s_state.active)
    return;

  SynchronizeToNow();
  s_state.mn10200.PulseIRQ(0);
}

u8 SharedRead(u32 index)
{
  if (!s_state.active || !s_state.shared_ram || index >= s_state.shared_ram_size)
    return UINT8_C(0xff);

  SynchronizeToNow();
  return s_state.shared_ram[index];
}

void SharedWrite(u32 index, u8 value)
{
  if (!s_state.active || !s_state.shared_ram || index >= s_state.shared_ram_size)
    return;

  SynchronizeToNow();
  s_state.shared_ram[index] = value;
}

void GenerateAudioFrame(s32* left, s32* right)
{
  if (!left || !right)
    return;

  *left = 0;
  *right = 0;

  if (!s_state.active)
    return;

  SynchronizeToNow();
  PrimeResampler();

  if (!s_state.have_resample_a || !s_state.have_resample_b)
    return;

  const s64 phase = static_cast<s64>(s_state.resample_phase);
  const s64 denom = static_cast<s64>(RESAMPLE_DENOMINATOR);

  *left = static_cast<s32>(
    static_cast<s64>(s_state.resample_a.left) +
    ((static_cast<s64>(s_state.resample_b.left) - s_state.resample_a.left) * phase) / denom);
  *right = static_cast<s32>(
    static_cast<s64>(s_state.resample_a.right) +
    ((static_cast<s64>(s_state.resample_b.right) - s_state.resample_a.right) * phase) / denom);

  s_state.resample_phase += ZSG2_CLOCK_HZ;
  while (s_state.resample_phase >= RESAMPLE_DENOMINATOR)
  {
    s_state.resample_phase -= RESAMPLE_DENOMINATOR;
    s_state.resample_a = s_state.resample_b;

    StereoFrame next;
    if (PopNativeFrame(&next))
      s_state.resample_b = next;
    else
      s_state.resample_b = s_state.resample_a;
  }
}

} // namespace TaitoFX1BZoom
