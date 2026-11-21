// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "core/arcade/devices/cpu/z80_state.h"
#include "core/arcade/third_party/z80_superzazu/z80.h"

#include "util/state_wrapper.h"

namespace Arcade::Z80State {

bool DoState(StateWrapper& sw, z80& z)
{
  u64 cycles = z.cyc;
  sw.Do(&cycles);
  sw.Do(&z.pc);
  sw.Do(&z.sp);
  sw.Do(&z.ix);
  sw.Do(&z.iy);
  sw.Do(&z.mem_ptr);
  sw.Do(&z.a);
  sw.Do(&z.b);
  sw.Do(&z.c);
  sw.Do(&z.d);
  sw.Do(&z.e);
  sw.Do(&z.h);
  sw.Do(&z.l);
  sw.Do(&z.a_);
  sw.Do(&z.b_);
  sw.Do(&z.c_);
  sw.Do(&z.d_);
  sw.Do(&z.e_);
  sw.Do(&z.h_);
  sw.Do(&z.l_);
  sw.Do(&z.f_);
  sw.Do(&z.i);
  sw.Do(&z.r);
  sw.Do(&z.iff_delay);
  sw.Do(&z.interrupt_mode);
  sw.Do(&z.int_data);

  // Flags and interrupt latches are bit-fields in the core.
  u16 bits = static_cast<u16>(z.sf | (z.zf << 1) | (z.yf << 2) | (z.hf << 3) | (z.xf << 4) | (z.pf << 5) | (z.nf << 6) |
                              (z.cf << 7) | (z.iff1 << 8) | (z.iff2 << 9) | (z.halted << 10) | (z.int_pending << 11) |
                              (z.nmi_pending << 12));
  sw.Do(&bits);
  if (sw.IsReading())
  {
    if (sw.HasError() || (bits >> 13) != 0)
      return false;

    z.cyc = static_cast<unsigned long>(cycles);
    z.sf = (bits >> 0) & 1;
    z.zf = (bits >> 1) & 1;
    z.yf = (bits >> 2) & 1;
    z.hf = (bits >> 3) & 1;
    z.xf = (bits >> 4) & 1;
    z.pf = (bits >> 5) & 1;
    z.nf = (bits >> 6) & 1;
    z.cf = (bits >> 7) & 1;
    z.iff1 = (bits >> 8) & 1;
    z.iff2 = (bits >> 9) & 1;
    z.halted = (bits >> 10) & 1;
    z.int_pending = (bits >> 11) & 1;
    z.nmi_pending = (bits >> 12) & 1;
  }

  return !sw.HasError();
}

} // namespace Arcade::Z80State
