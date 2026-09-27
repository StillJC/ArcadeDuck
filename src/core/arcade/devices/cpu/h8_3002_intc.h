#pragma once

#include "common/types.h"

#include <array>
#include <optional>

namespace Arcade::H8 {

class H83002Intc
{
public:
  static constexpr u16 ISCR = UINT16_C(0xFFF4);
  static constexpr u16 IER  = UINT16_C(0xFFF5);
  static constexpr u16 ISR  = UINT16_C(0xFFF6);
  static constexpr u16 ICRL = UINT16_C(0xFFF8);
  static constexpr u16 ICRH = UINT16_C(0xFFF9);

  static constexpr u32 IRQ_VECTOR_BASE = 12;
  static constexpr u32 IRQ_VECTOR_COUNT = 8;
  static constexpr u32 NMI_VECTOR = 7;

  H83002Intc() { Reset(); }

  void Reset()
  {
    m_pending = 0;
    m_irq_input = 0;
    m_ier = 0;
    m_isr = 0;
    m_iscr = 0;
    m_icr = 0;
  }

  bool Handles(u16 reg) const
  {
    return reg == ISCR || reg == IER || reg == ISR || reg == ICRL || reg == ICRH;
  }

  u8 Read(u16 reg) const
  {
    switch (reg)
    {
      case ISCR: return m_iscr;
      case IER: return m_ier;
      case ISR: return m_isr;
      case ICRL: return static_cast<u8>(m_icr);
      case ICRH: return static_cast<u8>(m_icr >> 8);
      default: return UINT8_C(0xFF);
    }
  }

  void Write(u16 reg, u8 data)
  {
    switch (reg)
    {
      case ISCR:
        m_iscr = data;
        CheckLevelIRQs();
        RefreshExternalPending();
        break;

      case IER:
        m_ier = data;
        RefreshExternalPending();
        break;

      case ISR:
        // MAME h8h_intc_device::isr_w(): writing zero clears the bit.
        m_isr = static_cast<u8>(m_isr & data);
        CheckLevelIRQs();
        RefreshExternalPending();
        break;

      case ICRL:
        m_icr = static_cast<u16>((m_icr & UINT16_C(0xFF00)) | data);
        break;

      case ICRH:
        m_icr = static_cast<u16>((m_icr & UINT16_C(0x00FF)) |
                                 (static_cast<u16>(data) << 8));
        break;

      default:
        break;
    }
  }

  void SetInput(u32 irq, bool asserted)
  {
    if (irq >= IRQ_VECTOR_COUNT)
      return;

    const u8 mask = static_cast<u8>(UINT8_C(1) << irq);
    const bool was_asserted = (m_irq_input & mask) != 0;
    const bool edge_falling = (m_iscr & mask) != 0;

    bool set = false;
    if (edge_falling)
      set = asserted && !was_asserted;
    else
      set = asserted;

    if (asserted)
      m_irq_input = static_cast<u8>(m_irq_input | mask);
    else
      m_irq_input = static_cast<u8>(m_irq_input & static_cast<u8>(~mask));

    if (set)
      m_isr = static_cast<u8>(m_isr | mask);

    RefreshExternalPending();
  }

  void InternalInterrupt(u32 vector)
  {
    if (vector < 64)
      m_pending |= UINT64_C(1) << vector;
  }

  std::optional<u32> PeekNext(bool ccr_i, bool ccr_ui, u8 syscr) const
  {
    const int filter = InterruptFilter(ccr_i, ccr_ui, syscr);

    std::optional<u32> best;
    int best_level = -1;

    for (u32 vector = 0; vector < 64; vector++)
    {
      if ((m_pending & (UINT64_C(1) << vector)) == 0)
        continue;

      const int level = Priority(vector);
      if (level < filter)
        continue;

      if (!best || level > best_level)
      {
        best = vector;
        best_level = level;
      }
    }

    return best;
  }

  void Acknowledge(u32 vector)
  {
    if (vector >= 64)
      return;

    m_pending &= ~(UINT64_C(1) << vector);

    if (vector >= IRQ_VECTOR_BASE && vector < IRQ_VECTOR_BASE + IRQ_VECTOR_COUNT)
    {
      const u32 irq = vector - IRQ_VECTOR_BASE;
      const u8 mask = static_cast<u8>(UINT8_C(1) << irq);
      const bool edge_falling = (m_iscr & mask) != 0;
      const bool line_asserted = (m_irq_input & mask) != 0;

      // MAME interrupt_taken(): edge requests clear on acceptance. A level-low
      // request remains active while the physical line is still asserted.
      if (edge_falling || !line_asserted)
        m_isr = static_cast<u8>(m_isr & static_cast<u8>(~mask));

      RefreshExternalPending();
    }
  }

  u8 GetISCR() const { return m_iscr; }
  u8 GetIER() const { return m_ier; }
  u8 GetISR() const { return m_isr; }
  u16 GetICR() const { return m_icr; }
  u64 GetPendingMask() const { return m_pending; }

private:
  // MAME h8h_intc_device::vector_to_slot.
  inline static constexpr std::array<int, 64> VECTOR_TO_SLOT = {
    -1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1, 0, 1, 2, 2,
     3, 3, 3, 3, 4, 4, 4, 4,
     5, 5, 5, 5, 6, 6, 6, 6,
     7, 7, 7, 7, 8, 8, 8, 8,
     9, 9, 9, 9,10,10,10,10,
    11,11,11,11,12,12,12,12,
    13,13,13,13,14,14,14,14
  };

  int Priority(u32 vector) const
  {
    if (vector == NMI_VECTOR)
      return 2;
    if (vector >= VECTOR_TO_SLOT.size())
      return 0;

    const int slot = VECTOR_TO_SLOT[vector];
    if (slot < 0)
      return 0;

    return (m_icr >> (slot ^ 7)) & 1;
  }

  static int InterruptFilter(bool ccr_i, bool ccr_ui, u8 syscr)
  {
    // MAME h83002_device::update_irq_filter(). SYSCR bit 3 selects the H8H
    // one-mask/two-mask interrupt model.
    if ((syscr & UINT8_C(0x08)) == 0)
    {
      if (ccr_i && ccr_ui)
        return 2;
      if (ccr_i)
        return 1;
      return 0;
    }

    return ccr_i ? 2 : 0;
  }

  void CheckLevelIRQs()
  {
    for (u32 irq = 0; irq < IRQ_VECTOR_COUNT; irq++)
    {
      const u8 mask = static_cast<u8>(UINT8_C(1) << irq);
      const bool edge_falling = (m_iscr & mask) != 0;
      if (!edge_falling && (m_irq_input & mask) != 0)
        m_isr = static_cast<u8>(m_isr | mask);
    }
  }

  void RefreshExternalPending()
  {
    const u64 external_mask = UINT64_C(0xFF) << IRQ_VECTOR_BASE;
    m_pending &= ~external_mask;
    m_pending |= static_cast<u64>(m_isr & m_ier) << IRQ_VECTOR_BASE;
  }

  u64 m_pending = 0;
  u8 m_irq_input = 0;
  u8 m_ier = 0;
  u8 m_isr = 0;
  u8 m_iscr = 0;
  u16 m_icr = 0;
};

} // namespace Arcade::H8