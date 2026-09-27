#pragma once

#include "common/types.h"

#include <array>
#include <optional>

namespace Arcade::H8 {

class H83002Peripherals
{
public:
  static constexpr u32 ADC_VECTOR = 60;
  static constexpr u32 WATCHDOG_VECTOR = 20;

  H83002Peripherals() { Reset(); }

  void Reset()
  {
    m_adc_results.fill(0);
    m_adc_inputs.fill(0);
    m_adcsr = 0;
    m_adcr = 0;
    m_adc_active = false;
    m_adc_trigger_high = true;
    m_adc_first = true;
    m_adc_remaining = 0;
    m_adc_channel = 0;
    m_adc_start_channel = 0;
    m_adc_end_channel = 0;
    m_adc_repeat = false;
    m_adc_rotate = false;

    m_watchdog_tcnt = 0;
    m_watchdog_tcsr = 0;
    m_watchdog_rst = 0;
    m_watchdog_cycles = 0;
    m_watchdog_key = 0;
    m_rst_key = 0;

    m_syscr = UINT8_C(0x09);
    m_rtmcsr = 0;
    m_icr = 0;
    m_pending_vectors = 0;
  }

  void SetADCInput(u32 channel, u16 value)
  {
    if (channel < m_adc_inputs.size())
      m_adc_inputs[channel] = static_cast<u16>(value & UINT16_C(0x03FF));
  }

  u8 SYSCR() const { return m_syscr; }

  bool Read(u16 reg, u8& value)
  {
    if (reg >= UINT16_C(0xFFE0) && reg <= UINT16_C(0xFFE7))
    {
      const u32 byte = static_cast<u32>(reg - UINT16_C(0xFFE0));
      const u16 sample = m_adc_results[byte >> 1];
      value = (byte & 1) ? static_cast<u8>(sample << 6) : static_cast<u8>(sample >> 2);
      return true;
    }

    switch (reg)
    {
      case UINT16_C(0xFFE8):
        value = m_adcsr;
        return true;

      case UINT16_C(0xFFE9):
        value = m_adcr;
        return true;

      case UINT16_C(0xFFA8):
        value = static_cast<u8>(m_watchdog_tcsr | UINT8_C(0x10));
        return true;

      case UINT16_C(0xFFA9):
        value = m_watchdog_tcnt;
        return true;

      case UINT16_C(0xFFAA):
        value = 0;
        return true;

      case UINT16_C(0xFFAB):
        value = static_cast<u8>(m_watchdog_rst | UINT8_C(0x3F));
        return true;

      case UINT16_C(0xFFAD):
        value = static_cast<u8>(m_rtmcsr | UINT8_C(0x80));
        return true;

      case UINT16_C(0xFFF2):
        value = m_syscr;
        return true;

      case UINT16_C(0xFFF8):
        value = static_cast<u8>(m_icr);
        return true;

      case UINT16_C(0xFFF9):
        value = static_cast<u8>(m_icr >> 8);
        return true;

      default:
        return false;
    }
  }

  bool Write(u16 reg, u8 data)
  {
    switch (reg)
    {
      case UINT16_C(0xFFE8):
        WriteADCSR(data);
        return true;

      case UINT16_C(0xFFE9):
        m_adcr = data;
        UpdateADCMode();
        return true;

      case UINT16_C(0xFFA8):
        m_watchdog_key = data;
        return true;

      case UINT16_C(0xFFA9):
        WriteWatchdogData(data);
        return true;

      case UINT16_C(0xFFAA):
        m_rst_key = data;
        return true;

      case UINT16_C(0xFFAB):
        // H8 "H" watchdog RSTCSR writes are key-protected. MAME currently
        // logs these but does not model additional reset behavior.
        m_rst_key = 0;
        return true;

      case UINT16_C(0xFFAD):
        m_rtmcsr = data;
        return true;

      case UINT16_C(0xFFF2):
        m_syscr = data;
        return true;

      case UINT16_C(0xFFF8):
        m_icr = static_cast<u16>((m_icr & UINT16_C(0xFF00)) | data);
        return true;

      case UINT16_C(0xFFF9):
        m_icr = static_cast<u16>((m_icr & UINT16_C(0x00FF)) |
                                 (static_cast<u16>(data) << 8));
        return true;

      default:
        return false;
    }
  }

  void SetADCTrigger(bool state)
  {
    if (state == m_adc_trigger_high)
      return;

    const bool falling = !state && m_adc_trigger_high;
    m_adc_trigger_high = state;

    if (falling && (m_adcr & UINT8_C(0x80)) != 0 &&
        (m_adcsr & UINT8_C(0x20)) == 0)
    {
      m_adcsr = static_cast<u8>(m_adcsr | UINT8_C(0x20));
      StartADCConversion();
    }
  }

  void Advance(u64 clocks)
  {
    AdvanceADC(clocks);
    AdvanceWatchdog(clocks);
  }

  std::optional<u32> TakePendingInterrupt()
  {
    if (m_pending_vectors == 0)
      return std::nullopt;

    // H8H INTC priority comes from ICR slots. Select the highest ICR priority,
    // then the lowest vector at equal priority, matching MAME's scan order.
    std::optional<u32> selected;
    int selected_priority = -1;

    for (u32 vector = 0; vector < 64; vector++)
    {
      if ((m_pending_vectors & (UINT64_C(1) << vector)) == 0)
        continue;

      const int priority = VectorPriority(vector);
      if (!selected || priority > selected_priority)
      {
        selected = vector;
        selected_priority = priority;
      }
    }

    if (selected)
      m_pending_vectors &= ~(UINT64_C(1) << *selected);

    return selected;
  }

private:
  static constexpr std::array<int, 64> VECTOR_TO_SLOT = {
    -1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1, 0, 1, 2, 2,
     3, 3, 3, 3, 4, 4, 4, 4,
     5, 5, 5, 5, 6, 6, 6, 6,
     7, 7, 7, 7, 8, 8, 8, 8,
     9, 9, 9, 9,10,10,10,10,
    11,11,11,11,12,12,12,12,
    13,13,13,13,14,14,14,14
  };

  int VectorPriority(u32 vector) const
  {
    if (vector == 7)
      return 2;

    if (vector >= VECTOR_TO_SLOT.size())
      return 0;

    const int slot = VECTOR_TO_SLOT[vector];
    if (slot < 0)
      return 0;

    return (m_icr >> (slot ^ 7)) & 1;
  }

  void Raise(u32 vector)
  {
    if (vector < 64)
      m_pending_vectors |= UINT64_C(1) << vector;
  }

  void UpdateADCMode()
  {
    m_adc_repeat = (m_adcsr & UINT8_C(0x10)) != 0;
    m_adc_rotate = m_adc_repeat;

    if (m_adc_rotate)
    {
      m_adc_start_channel = static_cast<u8>(m_adcsr & UINT8_C(0x04));
      m_adc_end_channel = static_cast<u8>(m_adcsr & UINT8_C(0x07));
    }
    else
    {
      m_adc_start_channel = static_cast<u8>(m_adcsr & UINT8_C(0x07));
      m_adc_end_channel = m_adc_start_channel;
    }
  }

  void WriteADCSR(u8 data)
  {
    const u8 previous = m_adcsr;

    // MAME: ADF can only remain set by writing it as 1; bits 0-6 are direct.
    m_adcsr = static_cast<u8>((data & UINT8_C(0x7F)) |
                              (m_adcsr & data & UINT8_C(0x80)));
    UpdateADCMode();

    if ((previous & UINT8_C(0x20)) == 0 &&
        (m_adcsr & UINT8_C(0x20)) != 0)
    {
      StartADCConversion();
    }
  }

  u32 ADCConversionCycles(bool first) const
  {
    // H8 ADC 3337 timing used by H83002 in MAME.
    if (first)
      return (m_adcsr & UINT8_C(0x08)) ? 134u : 266u;
    return (m_adcsr & UINT8_C(0x08)) ? 128u : 256u;
  }

  void StartADCConversion()
  {
    UpdateADCMode();
    m_adc_active = true;
    m_adc_first = true;
    m_adc_channel = m_adc_start_channel;
    m_adc_remaining = ADCConversionCycles(true);
  }

  void AdvanceADC(u64 clocks)
  {
    while (m_adc_active && clocks > 0)
    {
      if (clocks < m_adc_remaining)
      {
        m_adc_remaining -= clocks;
        return;
      }

      clocks -= m_adc_remaining;
      CommitADCChannel();

      if (m_adc_rotate && m_adc_channel != m_adc_end_channel)
      {
        m_adc_channel = static_cast<u8>((m_adc_channel + 1) & UINT8_C(0x07));
        m_adc_first = false;
        m_adc_remaining = ADCConversionCycles(false);
        continue;
      }

      m_adcsr = static_cast<u8>(m_adcsr | UINT8_C(0x80));
      if ((m_adcsr & UINT8_C(0x40)) != 0)
        Raise(ADC_VECTOR);

      if (m_adc_repeat)
      {
        m_adc_channel = m_adc_start_channel;
        m_adc_first = false;
        m_adc_remaining = ADCConversionCycles(false);
      }
      else
      {
        m_adc_active = false;
        m_adc_remaining = 0;
        m_adcsr = static_cast<u8>(m_adcsr & static_cast<u8>(~UINT8_C(0x20)));
      }
    }
  }

  void CommitADCChannel()
  {
    const u32 reg = static_cast<u32>(m_adc_channel & UINT8_C(0x03));
    m_adc_results[reg] = m_adc_inputs[m_adc_channel & UINT8_C(0x07)];
  }

  void WriteWatchdogData(u8 data)
  {
    if (m_watchdog_key == UINT8_C(0xA5))
    {
      const bool was_enabled = (m_watchdog_tcsr & UINT8_C(0x20)) != 0;
      const bool enable = (data & UINT8_C(0x20)) != 0;

      m_watchdog_tcsr =
        static_cast<u8>((m_watchdog_tcsr & data & UINT8_C(0x80)) |
                        (data & UINT8_C(0x7F)));

      if (!was_enabled && enable)
        m_watchdog_cycles = 0;
    }
    else if (m_watchdog_key == UINT8_C(0x5A))
    {
      if ((m_watchdog_tcsr & UINT8_C(0x20)) != 0)
      {
        m_watchdog_tcnt = data;
        m_watchdog_cycles = 0;
      }
    }

    m_watchdog_key = 0;
  }

  void AdvanceWatchdog(u64 clocks)
  {
    if ((m_watchdog_tcsr & UINT8_C(0x20)) == 0)
    {
      m_watchdog_tcnt = 0;
      m_watchdog_cycles = 0;
      return;
    }

    static constexpr std::array<u8, 8> DIV_SHIFT = {1,5,6,7,8,9,11,12};
    const u8 shift = DIV_SHIFT[m_watchdog_tcsr & UINT8_C(0x07)];
    const u64 divider = UINT64_C(1) << shift;

    m_watchdog_cycles += clocks;
    const u64 increments = m_watchdog_cycles / divider;
    m_watchdog_cycles %= divider;

    if (increments == 0)
      return;

    const u64 total = static_cast<u64>(m_watchdog_tcnt) + increments;
    m_watchdog_tcnt = static_cast<u8>(total);

    if (total < UINT64_C(0x100))
      return;

    if ((m_watchdog_tcsr & UINT8_C(0x40)) == 0)
    {
      if ((m_watchdog_tcsr & UINT8_C(0x80)) == 0)
      {
        m_watchdog_tcsr = static_cast<u8>(m_watchdog_tcsr | UINT8_C(0x80));
        Raise(WATCHDOG_VECTOR);
      }
    }
    // Watchdog-reset mode is intentionally not converted into a board reset
    // here; the H83002 reset controller is a separate machine-level action.
  }

  std::array<u16, 4> m_adc_results{};
  std::array<u16, 8> m_adc_inputs{};
  u8 m_adcsr = 0;
  u8 m_adcr = 0;
  bool m_adc_active = false;
  bool m_adc_trigger_high = true;
  bool m_adc_first = true;
  u64 m_adc_remaining = 0;
  u8 m_adc_channel = 0;
  u8 m_adc_start_channel = 0;
  u8 m_adc_end_channel = 0;
  bool m_adc_repeat = false;
  bool m_adc_rotate = false;

  u8 m_watchdog_tcnt = 0;
  u8 m_watchdog_tcsr = 0;
  u8 m_watchdog_rst = 0;
  u64 m_watchdog_cycles = 0;
  u8 m_watchdog_key = 0;
  u8 m_rst_key = 0;

  u8 m_syscr = UINT8_C(0x09);
  u8 m_rtmcsr = 0;
  u16 m_icr = 0;
  u64 m_pending_vectors = 0;
};

} // namespace Arcade::H8