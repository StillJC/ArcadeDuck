// SPDX-License-Identifier: ISC
// Adapted from the ares SH-2 interpreter (ares team, Near et al.).
#include "sh2.h"

#include "common/log.h"

#include <algorithm>
#include <limits>

Log_SetChannel(SH2Interpreter);

namespace ArcadeDuck::SH2
{
CPU::CPU(Bus bus) : m_bus(std::move(bus)) {}

const CPU::InstructionFetchRegion* CPU::FindDirectRegion(uint32_t a, uint32_t width) const
{
  const InstructionFetchRegion& r0 = m_instruction_fetch_regions[0];
  if (r0.data && a >= r0.base)
  {
    const uint32_t offset = a - r0.base;
    if (width <= r0.size && offset <= r0.size - width)
      return &r0;
  }

  const InstructionFetchRegion& r1 = m_instruction_fetch_regions[1];
  if (r1.data && a >= r1.base)
  {
    const uint32_t offset = a - r1.base;
    if (width <= r1.size && offset <= r1.size - width)
      return &r1;
  }

  const InstructionFetchRegion& r2 = m_instruction_fetch_regions[2];
  if (r2.data && a >= r2.base)
  {
    const uint32_t offset = a - r2.base;
    if (width <= r2.size && offset <= r2.size - width)
      return &r2;
  }

  return nullptr;
}

CPU::InstructionFetchRegion* CPU::FindDirectRegion(uint32_t a, uint32_t width)
{
  return const_cast<InstructionFetchRegion*>(
    static_cast<const CPU*>(this)->FindDirectRegion(a, width));
}

uint8_t CPU::ReadByte(uint32_t a) const
{
  if (const InstructionFetchRegion* const region = FindDirectRegion(a, 1))
    return region->data[a - region->base];

  return m_bus.read8 ? m_bus.read8(a) : 0xFF;
}

uint16_t CPU::ReadWord(uint32_t a) const
{
  if (const InstructionFetchRegion* const region = FindDirectRegion(a, 2))
  {
    const uint32_t offset = a - region->base;
    return static_cast<uint16_t>(
      (static_cast<uint16_t>(region->data[offset]) << 8) |
      static_cast<uint16_t>(region->data[offset + 1u]));
  }

  return m_bus.read16 ? m_bus.read16(a) : 0xFFFF;
}

uint32_t CPU::ReadLong(uint32_t a) const
{
  if (const InstructionFetchRegion* const region = FindDirectRegion(a, 4))
  {
    const uint32_t offset = a - region->base;
    return (static_cast<uint32_t>(region->data[offset]) << 24) |
           (static_cast<uint32_t>(region->data[offset + 1u]) << 16) |
           (static_cast<uint32_t>(region->data[offset + 2u]) << 8) |
           static_cast<uint32_t>(region->data[offset + 3u]);
  }

  return m_bus.read32 ? m_bus.read32(a) : 0xFFFFFFFFu;
}

void CPU::WriteByte(uint32_t a, uint8_t v)
{
  if (InstructionFetchRegion* const region = FindDirectRegion(a, 1))
  {
    region->data[a - region->base] = v;
    return;
  }

  if (m_bus.write8)
    m_bus.write8(a, v);
}

void CPU::WriteWord(uint32_t a, uint16_t v)
{
  if (InstructionFetchRegion* const region = FindDirectRegion(a, 2))
  {
    const uint32_t offset = a - region->base;
    region->data[offset] = static_cast<uint8_t>(v >> 8);
    region->data[offset + 1u] = static_cast<uint8_t>(v);
    return;
  }

  if (m_bus.write16)
    m_bus.write16(a, v);
}

void CPU::WriteLong(uint32_t a, uint32_t v)
{
  if (InstructionFetchRegion* const region = FindDirectRegion(a, 4))
  {
    const uint32_t offset = a - region->base;
    region->data[offset] = static_cast<uint8_t>(v >> 24);
    region->data[offset + 1u] = static_cast<uint8_t>(v >> 16);
    region->data[offset + 2u] = static_cast<uint8_t>(v >> 8);
    region->data[offset + 3u] = static_cast<uint8_t>(v);
    return;
  }

  if (m_bus.write32)
    m_bus.write32(a, v);
}

void CPU::SetInstructionFetchRegion(uint32_t index, uint32_t base, uint32_t size, uint8_t* data)
{
  if (index >= m_instruction_fetch_regions.size())
    return;

  m_instruction_fetch_regions[index] = {base, size, data};
}

uint16_t CPU::FetchWord(uint32_t a) const
{
  if (const InstructionFetchRegion* const region = FindDirectRegion(a, 2))
  {
    const uint32_t offset = a - region->base;
    return static_cast<uint16_t>(
      (static_cast<uint16_t>(region->data[offset]) << 8) |
      static_cast<uint16_t>(region->data[offset + 1u]));
  }

  return ReadWord(a);
}

void CPU::Reset(bool power_on)
{
  m_state = {};
  m_state.vbr = 0;
  m_state.sr = 0xF0;
  m_delay_pending = false;
  m_executing_delay_slot = false;
  m_sleeping = false;
  m_illegal_instruction = false;
  const uint32_t vector = power_on ? 0u : 2u;
  m_state.pc = ReadLong(vector * 4u);
  m_state.r[15] = ReadLong((vector + 1u) * 4u);
}

void CPU::SetInterrupt(uint8_t level, uint8_t vector, bool asserted)
{
  m_irq_level = level & 0x0F;
  m_irq_vector = vector;
  m_irq_asserted = asserted;
  if (asserted)
    m_sleeping = false;
}

void CPU::ScheduleDelayedBranch(uint32_t target)
{
  if (InDelaySlot())
  {
    RaiseException(6, m_last_pc);
    return;
  }
  m_delay_pending = true;
  m_delay_target = target;
}

void CPU::RaiseException(uint8_t vector, uint32_t saved_pc)
{
  m_state.r[15] -= 4;
  WriteLong(m_state.r[15], m_state.sr & SR_MASK);
  m_state.r[15] -= 4;
  WriteLong(m_state.r[15], saved_pc);
  m_state.pc = ReadLong(m_state.vbr + static_cast<uint32_t>(vector) * 4u);
  m_delay_pending = false;
  m_executing_delay_slot = false;
  m_sleeping = false;
}

bool CPU::ServiceInterrupt()
{
  // SH-2 interrupts are not recognized between a delayed-control-transfer
  // instruction and its delay-slot instruction. m_delay_pending becomes set
  // as soon as the branch/return schedules that slot.
  if (!m_irq_asserted || m_delay_pending || m_executing_delay_slot)
    return false;
  const uint8_t mask = static_cast<uint8_t>((m_state.sr & SR_I) >> 4);
  if (m_irq_level <= mask)
    return false;

  static uint32_t s_qf_irq_logs = 0;
  if (s_qf_irq_logs < 64)
  {
    s_qf_irq_logs++;
  }

  m_state.r[15] -= 4;
  WriteLong(m_state.r[15], m_state.sr & SR_MASK);
  m_state.r[15] -= 4;
  WriteLong(m_state.r[15], m_state.pc);
  m_state.sr = (m_state.sr & ~SR_I) | (static_cast<uint32_t>(m_irq_level) << 4);
  m_state.pc = ReadLong(m_state.vbr + static_cast<uint32_t>(m_irq_vector) * 4u);
  m_sleeping = false;
  return true;
}

uint32_t CPU::Step()
{
  if (ServiceInterrupt())
    return 5;
  if (m_sleeping)
    return 1;

  const bool was_delay = m_delay_pending;
  const uint32_t delayed_target = m_delay_target;
  m_executing_delay_slot = was_delay;
  if (was_delay)
    m_delay_pending = false;

  const uint32_t pc = m_state.pc;
  const uint16_t op = FetchWord(pc);
  m_last_pc = pc;
  m_last_opcode = op;
  m_state.pc = pc + 2;

  uint32_t cycles = 1;
  Execute(op, pc, cycles);

  if (was_delay && m_executing_delay_slot)
    m_state.pc = delayed_target;
  m_executing_delay_slot = false;
  return cycles;
}

uint64_t CPU::Run(uint64_t cycles)
{
  uint64_t used = 0;
  while (used < cycles)
    used += Step();
  return used;
}

void CPU::Execute(uint16_t op, uint32_t pc, uint32_t& cycles)
{
  const uint32_t n = (op >> 8) & 0xF;
  const uint32_t m = (op >> 4) & 0xF;
  auto& R = m_state.r;
  auto set_t = [&](bool v) { m_state.sr = (m_state.sr & ~SR_T) | (v ? SR_T : 0u); };
  auto get_t = [&]() -> uint32_t { return m_state.sr & SR_T ? 1u : 0u; };
  auto set_q = [&](bool v) { m_state.sr = (m_state.sr & ~SR_Q) | (v ? SR_Q : 0u); };
  auto get_q = [&]() -> uint32_t { return m_state.sr & SR_Q ? 1u : 0u; };
  auto set_m = [&](bool v) { m_state.sr = (m_state.sr & ~SR_M) | (v ? SR_M : 0u); };
  auto get_m = [&]() -> uint32_t { return m_state.sr & SR_M ? 1u : 0u; };

  // Single-opcode instructions first.
  switch (op)
  {
    case 0x0008: set_t(false); return; // CLRT
    case 0x0009: return;               // NOP
    case 0x000B: ScheduleDelayedBranch(m_state.pr); cycles = 2; return; // RTS
    case 0x0018: set_t(true); return;  // SETT
    case 0x0019: m_state.sr &= ~(SR_M | SR_Q | SR_T); return; // DIV0U
    case 0x001B: m_sleeping = true; return; // SLEEP
    case 0x0028: m_state.mach = m_state.macl = 0; return; // CLRMAC
    case 0x002B: // RTE
    {
      if (InDelaySlot()) { RaiseException(6, pc); return; }
      const uint32_t target = ReadLong(R[15]);
      const uint32_t sr = ReadLong(R[15] + 4);
      R[15] += 8;
      m_state.sr = sr & SR_MASK;
      ScheduleDelayedBranch(target);
      cycles = 4;
      return;
    }
    default: break;
  }

  // Group selected by high nibble.
  switch (op >> 12)
  {
    case 0x1: // MOV.L Rm,@(disp,Rn)
      WriteLong(R[n] + (op & 0xF) * 4u, R[m]); return;
    case 0x5: // MOV.L @(disp,Rm),Rn
      R[n] = ReadLong(R[m] + (op & 0xF) * 4u); return;
    case 0x7: // ADD #imm,Rn
      R[n] += static_cast<uint32_t>(Sext8(op & 0xFF)); return;
    case 0x9: // MOV.W @(disp,PC),Rn
      R[n] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ReadWord(pc + 4u + (op & 0xFFu) * 2u)))); return;
    case 0xA: // BRA
      ScheduleDelayedBranch(pc + 4u + static_cast<uint32_t>(Sext12(op & 0xFFFu) * 2)); cycles = 2; return;
    case 0xB: // BSR
      m_state.pr = pc + 4u; ScheduleDelayedBranch(pc + 4u + static_cast<uint32_t>(Sext12(op & 0xFFFu) * 2)); cycles = 2; return;
    case 0xD: // MOV.L @(disp,PC),Rn
      R[n] = ReadLong(((pc + 4u) & ~3u) + (op & 0xFFu) * 4u); return;
    case 0xE: // MOV #imm,Rn
      R[n] = static_cast<uint32_t>(Sext8(op & 0xFF)); return;
    default: break;
  }

  // 0x8xxx and 0xCxxx groups.
  switch (op >> 8)
  {
    case 0x80: WriteByte(R[m] + (op & 0xF), static_cast<uint8_t>(R[0])); return;
    case 0x81: WriteWord(R[m] + (op & 0xF) * 2u, static_cast<uint16_t>(R[0])); return;
    case 0x84: R[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(ReadByte(R[m] + (op & 0xF))))); return;
    case 0x85: R[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ReadWord(R[m] + (op & 0xF) * 2u)))); return;
    case 0x88: set_t(R[0] == static_cast<uint32_t>(Sext8(op & 0xFF))); return;
    case 0x89: if (get_t()) m_state.pc = pc + 4u + static_cast<uint32_t>(Sext8(op & 0xFF) * 2); return;
    case 0x8B: if (!get_t()) m_state.pc = pc + 4u + static_cast<uint32_t>(Sext8(op & 0xFF) * 2); return;
    case 0x8D: if (get_t()) { ScheduleDelayedBranch(pc + 4u + static_cast<uint32_t>(Sext8(op & 0xFF) * 2)); cycles = 2; } return;
    case 0x8F: if (!get_t()) { ScheduleDelayedBranch(pc + 4u + static_cast<uint32_t>(Sext8(op & 0xFF) * 2)); cycles = 2; } return;
    case 0xC0: WriteByte(m_state.gbr + (op & 0xFF), static_cast<uint8_t>(R[0])); return;
    case 0xC1: WriteWord(m_state.gbr + (op & 0xFF) * 2u, static_cast<uint16_t>(R[0])); return;
    case 0xC2: WriteLong(m_state.gbr + (op & 0xFF) * 4u, R[0]); return;
    case 0xC3: // TRAPA
      if (InDelaySlot()) { RaiseException(6, pc); return; }
      R[15] -= 4; WriteLong(R[15], m_state.sr & SR_MASK);
      R[15] -= 4; WriteLong(R[15], pc + 2u);
      m_state.pc = ReadLong(m_state.vbr + (op & 0xFFu) * 4u); cycles = 8; return;
    case 0xC4: R[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(ReadByte(m_state.gbr + (op & 0xFF))))); return;
    case 0xC5: R[0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ReadWord(m_state.gbr + (op & 0xFF) * 2u)))); return;
    case 0xC6: R[0] = ReadLong(m_state.gbr + (op & 0xFF) * 4u); return;
    case 0xC7: R[0] = ((pc + 4u) & ~3u) + (op & 0xFFu) * 4u; return;
    case 0xC8: set_t((R[0] & (op & 0xFFu)) == 0); return;
    case 0xC9: R[0] &= op & 0xFFu; return;
    case 0xCA: R[0] ^= op & 0xFFu; return;
    case 0xCB: R[0] |= op & 0xFFu; return;
    case 0xCC: { const uint32_t a = m_state.gbr + R[0]; set_t((ReadByte(a) & (op & 0xFFu)) == 0); return; }
    case 0xCD: { const uint32_t a = m_state.gbr + R[0]; WriteByte(a, ReadByte(a) & static_cast<uint8_t>(op)); return; }
    case 0xCE: { const uint32_t a = m_state.gbr + R[0]; WriteByte(a, ReadByte(a) ^ static_cast<uint8_t>(op)); return; }
    case 0xCF: { const uint32_t a = m_state.gbr + R[0]; WriteByte(a, ReadByte(a) | static_cast<uint8_t>(op)); return; }
    default: break;
  }

  switch (op & 0xF00F)
  {
    case 0x0004: WriteByte(R[n] + R[0], static_cast<uint8_t>(R[m])); return;
    case 0x0005: WriteWord(R[n] + R[0], static_cast<uint16_t>(R[m])); return;
    case 0x0006: WriteLong(R[n] + R[0], R[m]); return;
    case 0x0007: m_state.macl = R[n] * R[m]; cycles = 2; return;
    case 0x000C: R[n] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(ReadByte(R[m] + R[0])))); return;
    case 0x000D: R[n] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ReadWord(R[m] + R[0])))); return;
    case 0x000E: R[n] = ReadLong(R[m] + R[0]); return;
    case 0x000F: // MAC.L
    {
      const int64_t a = static_cast<int32_t>(ReadLong(R[m])); R[m] += 4;
      const int64_t b = static_cast<int32_t>(ReadLong(R[n])); R[n] += 4;
      int64_t acc = (static_cast<int64_t>(static_cast<int32_t>(m_state.mach)) << 32) | m_state.macl;
      acc += a * b;
      if (m_state.sr & SR_S)
      {
        constexpr int64_t lo = -(1ll << 47), hi = (1ll << 47) - 1;
        acc = std::clamp(acc, lo, hi);
      }
      m_state.mach = static_cast<uint32_t>(static_cast<uint64_t>(acc) >> 32);
      m_state.macl = static_cast<uint32_t>(acc); cycles = 3; return;
    }

    case 0x2000: WriteByte(R[n], static_cast<uint8_t>(R[m])); return;
    case 0x2001: WriteWord(R[n], static_cast<uint16_t>(R[m])); return;
    case 0x2002: WriteLong(R[n], R[m]); return;
    case 0x2004: { const uint8_t v = static_cast<uint8_t>(R[m]); R[n] -= 1; WriteByte(R[n], v); return; }
    case 0x2005: { const uint16_t v = static_cast<uint16_t>(R[m]); R[n] -= 2; WriteWord(R[n], v); return; }
    case 0x2006: { const uint32_t v = R[m]; R[n] -= 4; WriteLong(R[n], v); return; }
    case 0x2007: set_q((R[n] >> 31) != 0); set_m((R[m] >> 31) != 0); set_t(get_q() != get_m()); return;
    case 0x2008: set_t((R[n] & R[m]) == 0); return;
    case 0x2009: R[n] &= R[m]; return;
    case 0x200A: R[n] ^= R[m]; return;
    case 0x200B: R[n] |= R[m]; return;
    case 0x200C: { const uint32_t t = R[n] ^ R[m]; set_t(((t & 0xFF000000u) == 0) || ((t & 0x00FF0000u) == 0) || ((t & 0x0000FF00u) == 0) || ((t & 0x000000FFu) == 0)); return; }
    case 0x200D: R[n] = (R[n] >> 16) | (R[m] << 16); return;
    case 0x200E: m_state.macl = static_cast<uint16_t>(R[n]) * static_cast<uint16_t>(R[m]); cycles = 2; return;
    case 0x200F: m_state.macl = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(R[n])) * static_cast<int32_t>(static_cast<int16_t>(R[m]))); cycles = 2; return;

    case 0x3000: set_t(R[n] == R[m]); return;
    case 0x3002: set_t(R[n] >= R[m]); return;
    case 0x3003: set_t(static_cast<int32_t>(R[n]) >= static_cast<int32_t>(R[m])); return;
    case 0x3004: // DIV1
    {
      const uint32_t old_q = get_q();
      set_q((R[n] >> 31) != 0);
      R[n] = (R[n] << 1) | get_t();
      const uint32_t before = R[n];
      if (!old_q)
      {
        if (!get_m()) { R[n] -= R[m]; set_q(get_q() ? !(R[n] > before) : (R[n] > before)); }
        else { R[n] += R[m]; set_q(get_q() ? !(R[n] < before) : (R[n] < before)); }
      }
      else
      {
        if (!get_m()) { R[n] += R[m]; set_q(get_q() ? (R[n] < before) : !(R[n] < before)); }
        else { R[n] -= R[m]; set_q(get_q() ? (R[n] > before) : !(R[n] > before)); }
      }
      set_t(get_q() == get_m()); return;
    }
    case 0x3005: { const uint64_t v = static_cast<uint64_t>(R[n]) * R[m]; m_state.mach = static_cast<uint32_t>(v >> 32); m_state.macl = static_cast<uint32_t>(v); cycles = 2; return; }
    case 0x3006: set_t(R[n] > R[m]); return;
    case 0x3007: set_t(static_cast<int32_t>(R[n]) > static_cast<int32_t>(R[m])); return;
    case 0x3008: R[n] -= R[m]; return;
    case 0x300A: { const uint32_t a = R[n], b = a - R[m]; R[n] = b - get_t(); set_t(a < b || b < R[n]); return; }
    case 0x300B: { const int32_t a = static_cast<int32_t>(R[n]), b = static_cast<int32_t>(R[m]); const int32_t r = static_cast<int32_t>(R[n] - R[m]); R[n] = static_cast<uint32_t>(r); set_t(((a ^ b) < 0) && ((a ^ r) < 0)); return; }
    case 0x300C: R[n] += R[m]; return;
    case 0x300D: { const int64_t v = static_cast<int64_t>(static_cast<int32_t>(R[n])) * static_cast<int64_t>(static_cast<int32_t>(R[m])); m_state.mach = static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32); m_state.macl = static_cast<uint32_t>(v); cycles = 2; return; }
    case 0x300E: { const uint32_t a = R[n], b = a + R[m]; R[n] = b + get_t(); set_t(a > b || b > R[n]); return; }
    case 0x300F: { const int32_t a = static_cast<int32_t>(R[n]), b = static_cast<int32_t>(R[m]); const int32_t r = static_cast<int32_t>(R[n] + R[m]); R[n] = static_cast<uint32_t>(r); set_t(((a ^ b) >= 0) && ((a ^ r) < 0)); return; }

    case 0x6000: R[n] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(ReadByte(R[m])))); return;
    case 0x6001: R[n] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ReadWord(R[m])))); return;
    case 0x6002: R[n] = ReadLong(R[m]); return;
    case 0x6003: R[n] = R[m]; return;
    case 0x6004: { const uint32_t a = R[m]; R[n] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(ReadByte(a)))); if (n != m) R[m] += 1; return; }
    case 0x6005: { const uint32_t a = R[m]; R[n] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ReadWord(a)))); if (n != m) R[m] += 2; return; }
    case 0x6006: { const uint32_t a = R[m]; R[n] = ReadLong(a); if (n != m) R[m] += 4; return; }
    case 0x6007: R[n] = ~R[m]; return;
    case 0x6008: R[n] = (R[m] & 0xFFFF0000u) | ((R[m] & 0xFFu) << 8) | ((R[m] >> 8) & 0xFFu); return;
    case 0x6009: R[n] = (R[m] << 16) | (R[m] >> 16); return;
    case 0x600A: { const uint32_t t = 0u - R[m]; R[n] = t - get_t(); set_t(t != 0 || R[n] > t); return; }
    case 0x600B: R[n] = 0u - R[m]; return;
    case 0x600C: R[n] = static_cast<uint8_t>(R[m]); return;
    case 0x600D: R[n] = static_cast<uint16_t>(R[m]); return;
    case 0x600E: R[n] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(R[m]))); return;
    case 0x600F: R[n] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(R[m]))); return;
    default: break;
  }

  switch (op & 0xF0FF)
  {
    case 0x0002: R[n] = m_state.sr & SR_MASK; return;
    case 0x0003: m_state.pr = pc + 4u; ScheduleDelayedBranch(pc + 4u + R[n]); cycles = 2; return;
    case 0x000A: R[n] = m_state.mach; return;
    case 0x0012: R[n] = m_state.gbr; return;
    case 0x001A: R[n] = m_state.macl; return;
    case 0x0022: R[n] = m_state.vbr; return;
    case 0x0023: ScheduleDelayedBranch(pc + 4u + R[n]); cycles = 2; return;
    case 0x0029: R[n] = get_t(); return;
    case 0x002A: R[n] = m_state.pr; return;

    case 0x4000: set_t((R[n] >> 31) != 0); R[n] <<= 1; return;
    case 0x4001: set_t((R[n] & 1) != 0); R[n] >>= 1; return;
    case 0x4002: R[n] -= 4; WriteLong(R[n], m_state.mach); return;
    case 0x4003: R[n] -= 4; WriteLong(R[n], m_state.sr & SR_MASK); return;
    case 0x4004: { const uint32_t c = R[n] >> 31; R[n] = (R[n] << 1) | c; set_t(c); return; }
    case 0x4005: { const uint32_t c = R[n] & 1; R[n] = (R[n] >> 1) | (c << 31); set_t(c); return; }
    case 0x4006: m_state.mach = ReadLong(R[n]); R[n] += 4; return;
    case 0x4007: m_state.sr = ReadLong(R[n]) & SR_MASK; R[n] += 4; return;
    case 0x4008: R[n] <<= 2; return;
    case 0x4009: R[n] >>= 2; return;
    case 0x400A: m_state.mach = R[n]; return;
    case 0x400B: m_state.pr = pc + 4u; ScheduleDelayedBranch(R[n]); cycles = 2; return;
    case 0x400E: m_state.sr = R[n] & SR_MASK; return;
    case 0x4010: R[n]--; set_t(R[n] == 0); return;
    case 0x4011: set_t(static_cast<int32_t>(R[n]) >= 0); return;
    case 0x4012: R[n] -= 4; WriteLong(R[n], m_state.macl); return;
    case 0x4013: R[n] -= 4; WriteLong(R[n], m_state.gbr); return;
    case 0x4015: set_t(static_cast<int32_t>(R[n]) > 0); return;
    case 0x4016: m_state.macl = ReadLong(R[n]); R[n] += 4; return;
    case 0x4017: m_state.gbr = ReadLong(R[n]); R[n] += 4; return;
    case 0x4018: R[n] <<= 8; return;
    case 0x4019: R[n] >>= 8; return;
    case 0x401A: m_state.macl = R[n]; return;
    case 0x401B: { const uint8_t v = ReadByte(R[n]); set_t(v == 0); WriteByte(R[n], v | 0x80); cycles = 4; return; }
    case 0x401E: m_state.gbr = R[n]; return;
    case 0x4020: set_t((R[n] >> 31) != 0); R[n] <<= 1; return;
    case 0x4021: { const uint32_t c = R[n] & 1; R[n] = static_cast<uint32_t>(static_cast<int32_t>(R[n]) >> 1); set_t(c); return; }
    case 0x4022: R[n] -= 4; WriteLong(R[n], m_state.pr); return;
    case 0x4023: R[n] -= 4; WriteLong(R[n], m_state.vbr); return;
    case 0x4024: { const uint32_t c = R[n] >> 31; R[n] = (R[n] << 1) | get_t(); set_t(c); return; }
    case 0x4025: { const uint32_t c = R[n] & 1; R[n] = (R[n] >> 1) | (get_t() << 31); set_t(c); return; }
    case 0x4026: m_state.pr = ReadLong(R[n]); R[n] += 4; return;
    case 0x4027: m_state.vbr = ReadLong(R[n]); R[n] += 4; return;
    case 0x4028: R[n] <<= 16; return;
    case 0x4029: R[n] >>= 16; return;
    case 0x402A: m_state.pr = R[n]; return;
    case 0x402B: ScheduleDelayedBranch(R[n]); cycles = 2; return;
    case 0x402E: m_state.vbr = R[n]; return;
    default: break;
  }

  if ((op & 0xF00F) == 0x400F) // MAC.W @Rm+,@Rn+
  {
    const int32_t a = static_cast<int16_t>(ReadWord(R[m])); R[m] += 2;
    const int32_t b = static_cast<int16_t>(ReadWord(R[n])); R[n] += 2;
    int64_t acc = (static_cast<int64_t>(static_cast<int32_t>(m_state.mach)) << 32) | m_state.macl;
    acc += static_cast<int64_t>(a) * b;
    if (m_state.sr & SR_S)
    {
      const int64_t sat = std::clamp<int64_t>(static_cast<int64_t>(static_cast<int32_t>(m_state.macl)) + static_cast<int64_t>(a) * b,
                                               std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
      m_state.macl = static_cast<uint32_t>(sat);
    }
    else
    {
      m_state.mach = static_cast<uint32_t>(static_cast<uint64_t>(acc) >> 32);
      m_state.macl = static_cast<uint32_t>(acc);
    }
    cycles = 3;
    return;
  }

  if (!m_illegal_instruction)
  {
    WARNING_LOG(
      "SH2 FIRST-ILLEGAL pc={:08X} opcode={:04X} delay_slot={} sp={:08X} pr={:08X} vbr={:08X} "
      "sr={:08X} r0={:08X} r1={:08X} r2={:08X} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} "
      "r8={:08X} r9={:08X} r10={:08X} r11={:08X} r12={:08X} r13={:08X} r14={:08X}",
      pc, op, InDelaySlot(), m_state.r[15], m_state.pr, m_state.vbr, m_state.sr,
      m_state.r[0], m_state.r[1], m_state.r[2], m_state.r[3], m_state.r[4], m_state.r[5],
      m_state.r[6], m_state.r[7], m_state.r[8], m_state.r[9], m_state.r[10], m_state.r[11],
      m_state.r[12], m_state.r[13], m_state.r[14]);
  }

  m_illegal_instruction = true;
  RaiseException(InDelaySlot() ? 6 : 4, pc);
}
} // namespace ArcadeDuck::SH2
