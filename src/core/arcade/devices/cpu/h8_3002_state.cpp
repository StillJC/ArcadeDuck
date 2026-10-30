// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/cpu/h8_3002_intc.h"
#include "core/arcade/devices/cpu/h8_3002_peripherals.h"
#include "core/arcade/devices/cpu/h8_3002_timer16.h"
#include "core/arcade/devices/cpu/h8_300h_state.h"

extern "C" {
#include "core/arcade/third_party/h8_300h/system.h"
}

#include "util/state_wrapper.h"

namespace Arcade::H8 {

bool DoCoreState(StateWrapper& sw, h8_system_t& cpu)
{
  if (!sw.DoMarker("H8Core"))
    return false;

  u32 pc = cpu.cpu.pc;
  sw.Do(&pc);
  sw.DoBytes(&cpu.cpu.ccr, sizeof(cpu.cpu.ccr));
  sw.DoBytes(cpu.cpu.regs, sizeof(cpu.cpu.regs));
  sw.DoBytes(&cpu.dbus, sizeof(cpu.dbus));
  sw.DoBytes(cpu.vmem.raw, sizeof(cpu.vmem.raw));
  sw.DoBytes(&cpu.error_code, sizeof(cpu.error_code));
  sw.DoBytes(&cpu.error_line, sizeof(cpu.error_line));
  sw.DoBytes(&cpu.cycles, sizeof(cpu.cycles));
  sw.DoBytes(&cpu.ir, sizeof(cpu.ir));
  sw.DoBytes(&cpu.sleep, sizeof(cpu.sleep));
  if (sw.IsReading())
  {
    if (sw.HasError() || cpu.ir.rx_len > H8_IR_BUFFER_LEN || cpu.ir.tx_len > H8_IR_BUFFER_LEN ||
        static_cast<unsigned>(cpu.error_code) >= static_cast<unsigned>(H8_DEBUG_SIZE))
    {
      return false;
    }
    cpu.cpu.pc = pc & 0x00ffffffu;
  }

  return !sw.HasError();
}

bool H83002Intc::DoState(StateWrapper& sw)
{
  sw.Do(&m_pending);
  sw.Do(&m_irq_input);
  sw.Do(&m_ier);
  sw.Do(&m_isr);
  sw.Do(&m_iscr);
  sw.Do(&m_icr);
  return !sw.HasError();
}

bool H83002Timer16::DoState(StateWrapper& sw)
{
  for (Channel& c : m_channels)
  {
    sw.Do(&c.tcr);
    sw.Do(&c.tier);
    sw.Do(&c.ier);
    sw.Do(&c.isr);
    sw.Do(&c.pending);
    sw.Do(&c.tcnt);
    sw.DoArray(c.tgr.data(), c.tgr.size());
    sw.DoArray(c.tbr.data(), c.tbr.size());
    sw.Do(&c.last_clock_update);
    sw.Do(&c.phase);
    sw.Do(&c.counter_cycle);
    sw.Do(&c.clear_source);
    sw.Do(&c.clock_divider);
    sw.Do(&c.internal_clock);
    sw.Do(&c.incrementing);
    sw.Do(&c.enabled);

    // UpdateTCR() selects counter clearing by TGRA/TGRB (0/1), none or the
    // external input, and an internal prescaler shift of 0..3.
    if (sw.IsReading() &&
        (c.clear_source < CLEAR_EXTERNAL || c.clear_source >= static_cast<int>(c.tgr.size()) || c.clock_divider > 3))
    {
      return false;
    }
  }
  sw.Do(&m_total_clocks);
  sw.Do(&m_next_update_clock);
  sw.Do(&m_pending_channels);
  sw.Do(&m_tstr);
  return !sw.HasError();
}

bool H83002Peripherals::DoState(StateWrapper& sw)
{
  sw.DoArray(m_adc_results.data(), m_adc_results.size());
  sw.DoArray(m_adc_inputs.data(), m_adc_inputs.size());
  sw.Do(&m_adcsr);
  sw.Do(&m_adcr);
  sw.Do(&m_adc_active);
  sw.Do(&m_adc_trigger_high);
  sw.Do(&m_adc_first);
  sw.Do(&m_adc_remaining);
  sw.Do(&m_adc_channel);
  sw.Do(&m_adc_start_channel);
  sw.Do(&m_adc_end_channel);
  sw.Do(&m_adc_repeat);
  sw.Do(&m_adc_rotate);
  sw.Do(&m_watchdog_tcnt);
  sw.Do(&m_watchdog_tcsr);
  sw.Do(&m_watchdog_rst);
  sw.Do(&m_watchdog_cycles);
  sw.Do(&m_watchdog_key);
  sw.Do(&m_rst_key);
  sw.Do(&m_syscr);
  sw.Do(&m_rtmcsr);
  sw.Do(&m_icr);
  sw.Do(&m_pending_vectors);
  return !sw.HasError();
}

} // namespace Arcade::H8
