// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/systems/sony/zn/taito_fx1a_sound.h"

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
#include "core/arcade/third_party/ymfm/ymfm_opn.h"
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
#include "core/arcade/third_party/z80_superzazu/z80.h"
#include "core/system.h"

#include "common/error.h"
#include "common/log.h"

#include <array>
#include <memory>
#include <vector>

Log_SetChannel(SonyZNTaitoFX1ASound);

namespace SonyZN::TaitoFX1ASound {
namespace {

constexpr u32 Z80_CLOCK_HZ = 4'000'000;
constexpr u32 YM2610_CLOCK_HZ = 8'000'000;
constexpr u32 OUTPUT_SAMPLE_RATE = 44'100;
constexpr u32 SOUND_ROM_SIZE = 0x20000;
constexpr u32 ADPCMA_ROM_SIZE_2MB = 0x200000;
constexpr u32 ADPCMA_ROM_SIZE_4MB = 0x400000;
constexpr u32 Z80_FIXED_ROM_END = 0x3fff;
constexpr u32 Z80_BANKED_ROM_BASE = 0x4000;
constexpr u32 Z80_BANKED_ROM_END = 0x7fff;
constexpr u32 Z80_BANK_SIZE = 0x4000;
constexpr u32 Z80_RAM_BASE = 0xc000;
constexpr u32 Z80_RAM_END = 0xdfff;
constexpr u32 Z80_RAM_SIZE = Z80_RAM_END - Z80_RAM_BASE + 1;
constexpr u32 YM2610_BASE = 0xe000;
constexpr u32 YM2610_END = 0xe003;
constexpr u32 TC0140_SLAVE_PORT = 0xe200;
constexpr u32 TC0140_SLAVE_COMM = 0xe201;
constexpr u32 PAN_BASE = 0xe400;
constexpr u32 PAN_END = 0xe403;
constexpr u32 UNKNOWN_EE00 = 0xee00;
constexpr u32 UNKNOWN_F000 = 0xf000;
constexpr u32 SOUND_BANK_REGISTER = 0xf200;

constexpr u8 PORT01_FULL = 0x01;
constexpr u8 PORT23_FULL = 0x02;
constexpr u8 PORT01_FULL_MASTER = 0x04;
constexpr u8 PORT23_FULL_MASTER = 0x08;

struct State
{
  bool active = false;
  z80 cpu = {};
  std::vector<u8> sound_program;
  std::vector<u8> adpcma_rom;
  std::array<u8, Z80_RAM_SIZE> ram = {};

  u8 sound_bank = 1;
  bool cpu_reset_asserted = false;

  std::array<u8, 4> slave_data = {};
  std::array<u8, 4> master_data = {};
  u8 main_mode = 0;
  u8 sub_mode = 0;
  u8 status = 0;
  bool nmi_enabled = false;
  bool nmi_asserted = false;

  std::array<s64, 2> ym_timer_clocks = {-1, -1};
  s64 ym_busy_clocks = 0;
  bool ym_irq_asserted = false;
  u32 ym_native_sample_rate = 0;
  u64 ym_native_phase = 0;
  s32 ym_last_left = 0;
  s32 ym_last_right = 0;

  s64 cycle_balance = 0;
  u64 audio_cycle_fraction = 0;
  u64 audio_target_cycles = 0;
  u64 host_target_cycles = 0;
  u64 scheduled_target_cycles = 0;
  u64 host_tick_fraction = 0;
  u64 last_host_sync_ticks = 0;
  u64 last_ticks_per_second = 0;
  u64 total_cycles = 0;

  bool unmapped_read_logged = false;
  bool unmapped_write_logged = false;
};

State s_state;

class YM2610Interface final : public ymfm::ymfm_interface
{
public:
  void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override
  {
    if (tnum < s_state.ym_timer_clocks.size())
      s_state.ym_timer_clocks[tnum] = (duration_in_clocks < 0) ? -1 : static_cast<s64>(duration_in_clocks);
  }

  void ymfm_set_busy_end(uint32_t clocks) override
  {
    s_state.ym_busy_clocks = static_cast<s64>(clocks);
  }

  bool ymfm_is_busy() override
  {
    return s_state.ym_busy_clocks > 0;
  }

  void ymfm_update_irq(bool asserted) override
  {
    const bool changed = (s_state.ym_irq_asserted != asserted);
    s_state.ym_irq_asserted = asserted;

    if (!s_state.active || !changed)
      return;

    if (!asserted)
    {
      // Superzazu exposes a latched interrupt request rather than a level input.
      // If the YM2610B drops IRQ before the Z80 accepts it, the hardware request
      // is gone and the pending latch must be cancelled as well. FX-1A has no
      // other maskable Z80 interrupt source.
      s_state.cpu.int_pending = 0;
      return;
    }

    if (!s_state.cpu_reset_asserted)
      z80_gen_int(&s_state.cpu, UINT8_C(0xff));
  }

  uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override
  {
    // FX-1A carries a single YM2610B sample ROM. The reference YM2610 device
    // exposes that same ROM to ADPCM-B when no separate ADPCM-B region is
    // populated, so both engines must see the loaded sample data.
    if (type == ymfm::ACCESS_ADPCM_A || type == ymfm::ACCESS_ADPCM_B)
      return (address < s_state.adpcma_rom.size()) ? s_state.adpcma_rom[address] : UINT8_C(0xff);

    return UINT8_C(0x00);
  }

  void TimerExpired(uint32_t tnum)
  {
    m_engine->engine_timer_expired(tnum);
  }
};

std::unique_ptr<YM2610Interface> s_ym_interface;
std::unique_ptr<ymfm::ym2610b> s_ym2610;

void InstallCPUCallbacks();

void UpdateNMI()
{
  const bool pending = (s_state.status & (PORT01_FULL | PORT23_FULL)) != 0;
  const bool asserted = pending && s_state.nmi_enabled && !s_state.cpu_reset_asserted;

  if (asserted && !s_state.nmi_asserted)
    z80_gen_nmi(&s_state.cpu);

  s_state.nmi_asserted = asserted;
}

void ResetZ80Core()
{
  z80_init(&s_state.cpu);
  InstallCPUCallbacks();
  s_state.cycle_balance = 0;
  s_state.nmi_asserted = false;
  UpdateNMI();

  // A level which was already asserted while RESET was active is visible when
  // the Z80 is released. RunCycles() then continues mirroring that physical
  // level until YM2610B deasserts it.
  if (s_state.ym_irq_asserted && !s_state.cpu_reset_asserted)
    z80_gen_int(&s_state.cpu, UINT8_C(0xff));
}

void SetZ80Reset(bool asserted)
{
  if (s_state.cpu_reset_asserted == asserted)
    return;

  s_state.cpu_reset_asserted = asserted;
  s_state.nmi_asserted = false;

  if (!asserted)
    ResetZ80Core();
}

u8 SlaveCommRead()
{
  u8 result = 0;

  switch (s_state.sub_mode)
  {
    case 0x00:
      result = s_state.slave_data[s_state.sub_mode++];
      break;

    case 0x01:
      result = s_state.slave_data[s_state.sub_mode++];
      s_state.status &= ~PORT01_FULL;
      UpdateNMI();
      break;

    case 0x02:
      result = s_state.slave_data[s_state.sub_mode++];
      break;

    case 0x03:
      result = s_state.slave_data[s_state.sub_mode++];
      s_state.status &= ~PORT23_FULL;
      UpdateNMI();
      break;

    case 0x04:
      result = s_state.status;
      break;

    default:
      break;
  }

  return result;
}

void SlavePortWrite(u8 value)
{
  s_state.sub_mode = value & 0x0f;
}

void SlaveCommWrite(u8 value)
{
  value &= 0x0f;

  switch (s_state.sub_mode)
  {
    case 0x00:
      s_state.master_data[s_state.sub_mode++] = value;
      break;

    case 0x01:
      s_state.master_data[s_state.sub_mode++] = value;
      s_state.status |= PORT01_FULL_MASTER;
      break;

    case 0x02:
      s_state.master_data[s_state.sub_mode++] = value;
      break;

    case 0x03:
      s_state.master_data[s_state.sub_mode++] = value;
      s_state.status |= PORT23_FULL_MASTER;
      break;

    case 0x04:
      break;

    case 0x05:
      s_state.nmi_enabled = false;
      UpdateNMI();
      break;

    case 0x06:
      s_state.nmi_enabled = true;
      UpdateNMI();
      break;

    default:
      break;
  }
}

u8 ReadMemory(void*, u16 address)
{
  if (address <= Z80_FIXED_ROM_END)
    return s_state.sound_program[address];

  if (address >= Z80_BANKED_ROM_BASE && address <= Z80_BANKED_ROM_END)
  {
    const u32 offset = (static_cast<u32>(s_state.sound_bank) * Z80_BANK_SIZE) +
                       (static_cast<u32>(address) - Z80_BANKED_ROM_BASE);
    return (offset < s_state.sound_program.size()) ? s_state.sound_program[offset] : UINT8_C(0xff);
  }

  if (address >= Z80_RAM_BASE && address <= Z80_RAM_END)
    return s_state.ram[address - Z80_RAM_BASE];

  if (address >= YM2610_BASE && address <= YM2610_END)
    return s_ym2610 ? s_ym2610->read(address - YM2610_BASE) : UINT8_C(0xff);

  if (address == TC0140_SLAVE_COMM)
    return SlaveCommRead();

  if (address == TC0140_SLAVE_PORT || address == UNKNOWN_EE00 || address == UNKNOWN_F000)
    return UINT8_C(0xff);

  if (!s_state.unmapped_read_logged)
  {
    s_state.unmapped_read_logged = true;
    DEV_LOG("TaitoFX1A.Sound first unmapped Z80 read address=0x{:04X} pc=0x{:04X}", address, s_state.cpu.pc);
  }
  return UINT8_C(0xff);
}

void WriteMemory(void*, u16 address, u8 value)
{
  if (address >= Z80_RAM_BASE && address <= Z80_RAM_END)
  {
    s_state.ram[address - Z80_RAM_BASE] = value;
    return;
  }

  if (address >= YM2610_BASE && address <= YM2610_END)
  {
    if (s_ym2610)
      s_ym2610->write(address - YM2610_BASE, value);
    return;
  }

  if (address == TC0140_SLAVE_PORT)
  {
    SlavePortWrite(value);
    return;
  }

  if (address == TC0140_SLAVE_COMM)
  {
    SlaveCommWrite(value);
    return;
  }

  if ((address >= PAN_BASE && address <= PAN_END) || address == UNKNOWN_EE00 || address == UNKNOWN_F000)
    return;

  if (address == SOUND_BANK_REGISTER)
  {
    s_state.sound_bank = value & 0x07;
    return;
  }

  if (!s_state.unmapped_write_logged)
  {
    s_state.unmapped_write_logged = true;
    DEV_LOG("TaitoFX1A.Sound first unmapped Z80 write address=0x{:04X} value=0x{:02X} pc=0x{:04X}", address, value,
            s_state.cpu.pc);
  }
}

u8 PortIn(z80*, u8)
{
  return UINT8_C(0xff);
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

void GenerateYMNativeSample();

void AdvanceYMSynthesis(u32 z80_cycles)
{
  if (!s_ym2610 || s_state.ym_native_sample_rate == 0 || z80_cycles == 0)
    return;

  // Keep the YM2610B synthesis timeline tied to the same 4 MHz Z80
  // timeline used for command execution and Yamaha timer scheduling.
  // This is the same architecture used by the working Capcom QSound path:
  // hardware sound generation advances whenever the sound CPU advances,
  // including host-side synchronization before command writes.
  s_state.ym_native_phase += static_cast<u64>(z80_cycles) * s_state.ym_native_sample_rate;
  while (s_state.ym_native_phase >= Z80_CLOCK_HZ)
  {
    s_state.ym_native_phase -= Z80_CLOCK_HZ;
    GenerateYMNativeSample();
  }
}

void AdvanceYMTiming(u32 z80_cycles)
{
  if (!s_ym_interface || z80_cycles == 0)
    return;

  const s64 ym_clocks = static_cast<s64>(z80_cycles) * (YM2610_CLOCK_HZ / Z80_CLOCK_HZ);

  if (s_state.ym_busy_clocks > 0)
    s_state.ym_busy_clocks = (s_state.ym_busy_clocks > ym_clocks) ? (s_state.ym_busy_clocks - ym_clocks) : 0;

  for (u32 tnum = 0; tnum < s_state.ym_timer_clocks.size(); tnum++)
  {
    s64 clocks_left = ym_clocks;
    while (s_state.ym_timer_clocks[tnum] >= 0 && clocks_left > 0)
    {
      const s64 remaining = (s_state.ym_timer_clocks[tnum] > 0) ? s_state.ym_timer_clocks[tnum] : 1;
      if (clocks_left < remaining)
      {
        s_state.ym_timer_clocks[tnum] -= clocks_left;
        clocks_left = 0;
        break;
      }

      clocks_left -= remaining;
      s_state.ym_timer_clocks[tnum] = -1;
      s_ym_interface->TimerExpired(tnum);
    }
  }

}

void RunCycles(u32 cycles)
{
  s_state.cycle_balance += cycles;
  while (s_state.cycle_balance > 0)
  {
    // The YM2610B drives a level-sensitive Z80 INT line. Superzazu exposes
    // a latched request instead, so mirror the physical line at each
    // instruction boundary: re-latch after an accepted interrupt while the
    // Yamaha line is still high, and cancel the latch when the line drops.
    // This lets RETI/EI immediately service a still-asserted timer source
    // without manufacturing extra interrupts after YM2610B deassertion.
    if (s_state.ym_irq_asserted)
    {
      if (!s_state.cpu.int_pending)
        z80_gen_int(&s_state.cpu, UINT8_C(0xff));
    }
    else
    {
      s_state.cpu.int_pending = 0;
    }

    s_state.cpu.cyc = 0;
    z80_step(&s_state.cpu);
    const u32 executed = static_cast<u32>(s_state.cpu.cyc);
    if (executed == 0)
      break;

    AdvanceYMTiming(executed);
    AdvanceYMSynthesis(executed);
    s_state.cycle_balance -= static_cast<s64>(executed);
    s_state.total_cycles += executed;
  }
}

void RunToTarget(u64 target_cycles)
{
  if (target_cycles <= s_state.scheduled_target_cycles)
    return;

  u64 cycles = target_cycles - s_state.scheduled_target_cycles;
  s_state.scheduled_target_cycles = target_cycles;

  if (s_state.cpu_reset_asserted)
  {
    while (cycles != 0)
    {
      const u32 chunk = static_cast<u32>((cycles > UINT64_C(0x01000000)) ? UINT64_C(0x01000000) : cycles);
      AdvanceYMTiming(chunk);
      AdvanceYMSynthesis(chunk);
      cycles -= chunk;
    }
    s_state.cycle_balance = 0;
    return;
  }

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

void GenerateYMNativeSample()
{
  if (!s_ym2610)
    return;

  ymfm::ym2610b::output_data sample = {};
  s_ym2610->generate(&sample, 1);

  // YMFM exposes the OPNB stereo mix on outputs 0/1 and the SSG mono mix on
  // output 2. Match the FX-1A board routing used by the reference driver:
  // SSG 0.75 to both channels, FM/ADPCM at unity to their stereo channels.
  const s32 ssg = (sample.data[2] * 3) / 4;
  s_state.ym_last_left = sample.data[0] + ssg;
  s_state.ym_last_right = sample.data[1] + ssg;
}

} // namespace

bool Initialize(const std::vector<u8>& sound_program, const std::vector<u8>& adpcma_rom, Error* error)
{
  Shutdown();

  if (sound_program.size() != SOUND_ROM_SIZE)
  {
    Error::SetStringFmt(error, "Taito FX-1A Z80 ROM has size {}; expected {} bytes.", sound_program.size(),
                        SOUND_ROM_SIZE);
    return false;
  }
  if (adpcma_rom.size() != ADPCMA_ROM_SIZE_2MB && adpcma_rom.size() != ADPCMA_ROM_SIZE_4MB)
  {
    Error::SetStringFmt(
      error, "Taito FX-1A YM2610B sample ROM has size {}; expected 2 MiB or 4 MiB.", adpcma_rom.size());
    return false;
  }

  s_state.sound_program = sound_program;
  s_state.adpcma_rom = adpcma_rom;
  s_state.active = true;

  s_ym_interface = std::make_unique<YM2610Interface>();
  s_ym2610 = std::make_unique<ymfm::ym2610b>(*s_ym_interface);
  s_ym2610->set_fidelity(ymfm::OPN_FIDELITY_MIN);
  s_state.ym_native_sample_rate = s_ym2610->sample_rate(YM2610_CLOCK_HZ);

  Reset();

  VERBOSE_LOG(
    "TaitoFX1A.Sound initialized z80_clock={} ym2610b_clock={} ym2610b_rate={} program_rom={} adpcma_rom={} "
    "tc0140syt='4-nibble mailbox + NMI/reset' ym2610b='YMFM + Z80-timeline synthesis + level-faithful timer IRQ + shared ADPCM-A/B ROM'",
    Z80_CLOCK_HZ, YM2610_CLOCK_HZ, s_state.ym_native_sample_rate, s_state.sound_program.size(),
    s_state.adpcma_rom.size());
  return true;
}

void Reset()
{
  if (!s_state.active)
    return;

  s_state.ram.fill(0);
  s_state.sound_bank = 1;
  s_state.cpu_reset_asserted = false;

  s_state.slave_data.fill(0);
  s_state.master_data.fill(0);
  s_state.main_mode = 0;
  s_state.sub_mode = 0;
  s_state.status = 0;
  s_state.nmi_enabled = false;
  s_state.nmi_asserted = false;

  s_state.ym_timer_clocks = {-1, -1};
  s_state.ym_busy_clocks = 0;
  s_state.ym_irq_asserted = false;
  s_state.ym_native_phase = 0;
  s_state.ym_last_left = 0;
  s_state.ym_last_right = 0;
  if (s_ym2610)
    s_ym2610->reset();

  s_state.cycle_balance = 0;
  s_state.audio_cycle_fraction = 0;
  s_state.audio_target_cycles = 0;
  s_state.host_target_cycles = 0;
  s_state.scheduled_target_cycles = 0;
  s_state.host_tick_fraction = 0;
  s_state.last_host_sync_ticks = static_cast<u64>(System::GetGlobalTickCounter());
  s_state.last_ticks_per_second = static_cast<u64>(System::GetTicksPerSecond());
  s_state.total_cycles = 0;

  s_state.unmapped_read_logged = false;
  s_state.unmapped_write_logged = false;

  ResetZ80Core();
}

void Shutdown()
{
  if (s_state.active)
    VERBOSE_LOG("TaitoFX1A.Sound shutdown cycles={} pc=0x{:04X}", s_state.total_cycles, s_state.cpu.pc);

  s_ym2610.reset();
  s_ym_interface.reset();
  s_state = {};
}

bool IsActive()
{
  return s_state.active;
}

void MasterPortWrite(u8 value)
{
  if (!s_state.active)
    return;

  SynchronizeHostTime();
  s_state.main_mode = value & 0x0f;
}

u8 MasterCommRead()
{
  if (!s_state.active)
    return UINT8_C(0xff);

  SynchronizeHostTime();

  u8 result = 0;
  switch (s_state.main_mode)
  {
    case 0x00:
      result = s_state.master_data[s_state.main_mode++];
      break;

    case 0x01:
      result = s_state.master_data[s_state.main_mode++];
      s_state.status &= ~PORT01_FULL_MASTER;
      break;

    case 0x02:
      result = s_state.master_data[s_state.main_mode++];
      break;

    case 0x03:
      result = s_state.master_data[s_state.main_mode++];
      s_state.status &= ~PORT23_FULL_MASTER;
      break;

    case 0x04:
      result = s_state.status;
      break;

    default:
      break;
  }

  return result;
}

void MasterCommWrite(u8 value)
{
  if (!s_state.active)
    return;

  SynchronizeHostTime();

  value &= 0x0f;
  switch (s_state.main_mode)
  {
    case 0x00:
      s_state.slave_data[s_state.main_mode++] = value;
      break;

    case 0x01:
      s_state.slave_data[s_state.main_mode++] = value;
      s_state.status |= PORT01_FULL;
      UpdateNMI();
      break;

    case 0x02:
      s_state.slave_data[s_state.main_mode++] = value;
      break;

    case 0x03:
      s_state.slave_data[s_state.main_mode++] = value;
      s_state.status |= PORT23_FULL;
      UpdateNMI();
      break;

    case 0x04:
      SetZ80Reset(value != 0);
      break;

    default:
      break;
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

  s_state.audio_cycle_fraction += Z80_CLOCK_HZ;
  s_state.audio_target_cycles += s_state.audio_cycle_fraction / OUTPUT_SAMPLE_RATE;
  s_state.audio_cycle_fraction %= OUTPUT_SAMPLE_RATE;
  RunToTarget(s_state.audio_target_cycles);

  // YM2610B generation is advanced by RunCycles()/RunToTarget(), not by
  // the host output callback. The callback only samples the current board
  // output after bringing the sound timeline to the requested point.
  *left = s_state.ym_last_left;
  *right = s_state.ym_last_right;
}

} // namespace SonyZN::TaitoFX1ASound
