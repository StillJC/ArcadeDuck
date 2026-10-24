// SPDX-License-Identifier: ISC
// Adapted from the ares SH-2 interpreter (ares team, Near et al.).
#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace ArcadeDuck::SH2
{
class CPU
{
public:
  using Read8 = std::function<uint8_t(uint32_t)>;
  using Read16 = std::function<uint16_t(uint32_t)>;
  using Read32 = std::function<uint32_t(uint32_t)>;
  using Write8 = std::function<void(uint32_t, uint8_t)>;
  using Write16 = std::function<void(uint32_t, uint16_t)>;
  using Write32 = std::function<void(uint32_t, uint32_t)>;

  struct Bus
  {
    Read8 read8;
    Read16 read16;
    Read32 read32;
    Write8 write8;
    Write16 write16;
    Write32 write32;
  };

  struct State
  {
    uint32_t r[16]{};
    uint32_t pc = 0;
    uint32_t pr = 0;
    uint32_t gbr = 0;
    uint32_t vbr = 0;
    uint32_t mach = 0;
    uint32_t macl = 0;
    uint32_t sr = 0;
  };

  explicit CPU(Bus bus);

  void Reset(bool power_on = true);
  uint32_t Step();
  uint64_t Run(uint64_t cycles);
  void SetInterrupt(uint8_t level, uint8_t vector, bool asserted);
  void SetInstructionFetchRegion(uint32_t index, uint32_t base, uint32_t size, uint8_t* data);

  const State& GetState() const { return m_state; }
  State& GetState() { return m_state; }
  bool IsSleeping() const { return m_sleeping; }
  bool HadIllegalInstruction() const { return m_illegal_instruction; }
  uint16_t LastOpcode() const { return m_last_opcode; }
  uint32_t LastPC() const { return m_last_pc; }

private:
  static constexpr uint32_t SR_T = 1u << 0;
  static constexpr uint32_t SR_S = 1u << 1;
  static constexpr uint32_t SR_I = 0xFu << 4;
  static constexpr uint32_t SR_Q = 1u << 8;
  static constexpr uint32_t SR_M = 1u << 9;
  static constexpr uint32_t SR_MASK = SR_T | SR_S | SR_I | SR_Q | SR_M;

  struct InstructionFetchRegion
  {
    uint32_t base = 0;
    uint32_t size = 0;
    uint8_t* data = nullptr;
  };

  const InstructionFetchRegion* FindDirectRegion(uint32_t a, uint32_t width) const;
  InstructionFetchRegion* FindDirectRegion(uint32_t a, uint32_t width);

  uint8_t ReadByte(uint32_t a) const;
  uint16_t ReadWord(uint32_t a) const;
  uint32_t ReadLong(uint32_t a) const;
  uint16_t FetchWord(uint32_t a) const;
  void WriteByte(uint32_t a, uint8_t v);
  void WriteWord(uint32_t a, uint16_t v);
  void WriteLong(uint32_t a, uint32_t v);

  bool InDelaySlot() const { return m_executing_delay_slot; }
  void ScheduleDelayedBranch(uint32_t target);
  void RaiseException(uint8_t vector, uint32_t saved_pc);
  bool ServiceInterrupt();
  void Execute(uint16_t op, uint32_t instruction_pc, uint32_t& cycles);

  static int32_t Sext8(uint32_t v) { return static_cast<int8_t>(v); }
  static int32_t Sext12(uint32_t v) { return (v & 0x800u) ? static_cast<int32_t>(v | 0xFFFFF000u) : static_cast<int32_t>(v); }

  Bus m_bus;
  std::array<InstructionFetchRegion, 3> m_instruction_fetch_regions{};
  State m_state{};

  bool m_delay_pending = false;
  uint32_t m_delay_target = 0;
  bool m_executing_delay_slot = false;
  bool m_sleeping = false;
  bool m_illegal_instruction = false;

  bool m_irq_asserted = false;
  uint8_t m_irq_level = 0;
  uint8_t m_irq_vector = 0;

  uint16_t m_last_opcode = 0;
  uint32_t m_last_pc = 0;
};
} // namespace ArcadeDuck::SH2
