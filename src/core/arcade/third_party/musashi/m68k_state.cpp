// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

// Save-state support for the single global Musashi context. Kept apart from m68k_bus.cpp because m68kcpu.h
// defines many short macros.

#include "core/arcade/third_party/musashi/m68k_bus.h"

#include "util/state_wrapper.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#endif
#include "m68kcpu.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

extern "C" int m68ki_initial_cycles;

bool MusashiBus::DoCPUState(StateWrapper& sw)
{
  m68ki_cpu_core& c = m68ki_cpu;

  // The cycle tables and callbacks are host configuration for this CPU type.
  u32 cpu_type = c.cpu_type;
  sw.Do(&cpu_type);
  if (sw.IsReading() && cpu_type != c.cpu_type)
    return false;

  sw.DoArray(c.dar, 16);
  sw.DoArray(c.dar_save, 16);
  sw.Do(&c.ppc);
  sw.Do(&c.pc);
  sw.DoArray(c.sp, 7);
  sw.Do(&c.vbr);
  sw.Do(&c.sfc);
  sw.Do(&c.dfc);
  sw.Do(&c.cacr);
  sw.Do(&c.caar);
  sw.Do(&c.ir);
  sw.Do(&c.t1_flag);
  sw.Do(&c.t0_flag);
  sw.Do(&c.s_flag);
  sw.Do(&c.m_flag);
  sw.Do(&c.x_flag);
  sw.Do(&c.n_flag);
  sw.Do(&c.not_z_flag);
  sw.Do(&c.v_flag);
  sw.Do(&c.c_flag);
  sw.Do(&c.int_mask);
  sw.Do(&c.int_level);
  sw.Do(&c.stopped);
  sw.Do(&c.pref_addr);
  sw.Do(&c.pref_data);
  sw.Do(&c.address_mask);
  sw.Do(&c.sr_mask);
  sw.Do(&c.instr_mode);
  sw.Do(&c.run_mode);
  sw.Do(&c.has_pmmu);
  sw.Do(&c.pmmu_enabled);
  sw.Do(&c.fpu_just_reset);
  sw.Do(&c.reset_cycles);
  sw.Do(&c.cyc_bcc_notake_b);
  sw.Do(&c.cyc_bcc_notake_w);
  sw.Do(&c.cyc_dbcc_f_noexp);
  sw.Do(&c.cyc_dbcc_f_exp);
  sw.Do(&c.cyc_scc_r_true);
  sw.Do(&c.cyc_movem_w);
  sw.Do(&c.cyc_movem_l);
  sw.Do(&c.cyc_shift);
  sw.Do(&c.cyc_reset);
  sw.Do(&c.virq_state);
  sw.Do(&c.nmi_pending);
  sw.Do(&c.mmu_crp_aptr);
  sw.Do(&c.mmu_crp_limit);
  sw.Do(&c.mmu_srp_aptr);
  sw.Do(&c.mmu_srp_limit);
  sw.Do(&c.mmu_tc);
  sw.Do(&c.mmu_sr);

  sw.Do(&m68ki_initial_cycles);
  sw.Do(&m68ki_remaining_cycles);
  sw.Do(&m68ki_tracing);
  sw.Do(&m68ki_address_space);
  sw.Do(&m68ki_aerr_address);
  sw.Do(&m68ki_aerr_write_mode);
  sw.Do(&m68ki_aerr_fc);

  return !sw.HasError();
}
