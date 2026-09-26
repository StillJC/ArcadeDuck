// SPDX-FileCopyrightText: Olivier Galibert
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: BSD-3-Clause
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once

#include "core/types.h"

#include <array>

namespace KonamiGQTMS57002 {

class Core
{
public:
  static constexpr u32 EXTERNAL_RAM_SIZE = 0x40000;

  Core();

  void Reset();
  void PLoadWrite(bool state);
  void CLoadWrite(bool state);
  void SetResetReleased(bool released);

  u8 DataRead();
  void DataWrite(u8 data);

  bool DataReady() const;
  bool ProgramCounterNonZero() const;
  bool UpdateFIFOEmpty() const;
  bool IsResetReleased() const { return m_reset_released; }

  void SetSerialInputs(s32 input0, s32 input1, s32 input2, s32 input3);
  s32 GetSerialOutput(u32 channel) const;
  void Sync();
  int Execute(int cycles);

  u8 GetProgramCounter() const { return pc; }
  u64 GetExecutedCycles() const { return m_total_cycles; }
  u64 GetSyncCount() const { return m_sync_count; }
  u64 GetExternalReads() const { return m_external_reads; }
  u64 GetExternalWrites() const { return m_external_writes; }

private:
  enum
  {
    IN_PLOAD = 0x00000001,
    IN_CLOAD = 0x00000002,
    SU_CVAL  = 0x00000004,
    SU_MASK  = 0x00000018,
    SU_ST0   = 0x00,
    SU_ST1   = 0x08,
    SU_PRG   = 0x10,
    S_IDLE   = 0x00000020,
    S_READ   = 0x00000040,
    S_WRITE  = 0x00000080,
    S_BRANCH = 0x00000100,
    S_HOST   = 0x00000200,
    S_UPDATE = 0x00000400,
  };

  enum
  {
    ST0_INCS = 0x000001,
    ST0_DIRI = 0x000002,
    ST0_FI   = 0x000004,
    ST0_SIM  = 0x000008,
    ST0_PLRI = 0x000020,
    ST0_PBCI = 0x000040,
    ST0_DIRO = 0x000080,
    ST0_FO   = 0x000100,
    ST0_SOM  = 0x000600,
    ST0_PLRO = 0x000800,
    ST0_PBCO = 0x001000,
    ST0_CNS  = 0x002000,
    ST0_WORD = 0x004000,
    ST0_SEL  = 0x008000,
    ST0_M    = 0x030000,
    ST0_M_64K  = 0x000000,
    ST0_M_256K = 0x010000,
    ST0_M_1M   = 0x020000,
    ST0_SRAM = 0x200000,

    ST1_AOV  = 0x000001,
    ST1_SFAI = 0x000002,
    ST1_SFAO = 0x000004,
    ST1_AOVM = 0x000008,
    ST1_MOVM = 0x000020,
    ST1_MOV  = 0x000040,
    ST1_SFMA = 0x000180,
    ST1_SFMA_SHIFT = 7,
    ST1_SFMO = 0x001800,
    ST1_SFMO_SHIFT = 11,
    ST1_RND  = 0x038000,
    ST1_RND_SHIFT = 15,
    ST1_CRM  = 0x0C0000,
    ST1_CRM_SHIFT = 18,
    ST1_CRM_32  = 0x000000,
    ST1_CRM_16H = 0x040000,
    ST1_CRM_16L = 0x080000,
    ST1_DBP  = 0x100000,
    ST1_CAS  = 0x200000,

    ST1_CACHE = ST1_SFAI | ST1_SFAO | ST1_MOVM | ST1_SFMA | ST1_SFMO | ST1_RND | ST1_CRM | ST1_DBP
  };

  enum { BR_UB, BR_CB, BR_IDLE };
  enum { IBS = 8192, HBS = 4096 };
  enum { INC_CA = 1, INC_ID = 2 };

  struct icd
  {
    u16 op;
    s16 next;
    u8 param;
  };

  struct hcd
  {
    u32 st1;
    s16 ipc;
    s16 next;
  };

  struct cd
  {
    s16 hashbase[256];
    hcd hashnode[HBS];
    icd inst[IBS];
    int hused;
    int iused;
  };

  struct cstate
  {
    int branch;
    int inc;
    s16 hnode;
    s16 ipc;
  };

  s64 macc = 0;
  s64 macc_read = 0;
  s64 macc_write = 0;

  u32 cmem[256] = {};
  u32 dmem0[256] = {};
  u32 dmem1[32] = {};

  u32 si[4] = {};
  u32 so[4] = {};

  u32 st0 = 0;
  u32 st1 = 0;
  u32 sti = S_IDLE;
  u32 aacc = 0;
  u32 xoa = 0;
  u32 xba = 0;
  u32 xwr = 0;
  u32 xrd = 0;
  u32 txrd = 0;
  u32 creg = 0;

  u8 pc = 0;
  u8 hpc = 0;
  u8 ca = 0;
  u8 id = 0;
  u8 ba0 = 0;
  u8 ba1 = 0;
  u8 rptc = 0;
  u8 rptc_next = 0;
  u8 sa = 0;

  u32 xm_adr = 0;

  u8 host[4] = {};
  u8 hidx = 0;
  u8 allow_update = 0;

  u32 update[16] = {};
  u8 update_counter_head = 0;
  u8 update_counter_tail = 0;

  cd cache = {};
  std::array<u32, 256> m_program = {};
  std::array<u8, EXTERNAL_RAM_SIZE> m_external_ram = {};
  int icount = 0;
  int unsupported_inst_warning = 0;
  bool m_reset_released = false;
  u64 m_total_cycles = 0;
  u64 m_sync_count = 0;
  u64 m_external_reads = 0;
  u64 m_external_writes = 0;

  void reset_internal();
  void pload_w(int state);
  void cload_w(int state);
  u8 data_r();
  void data_w(u8 data);
  int dready_r();
  int pc0_r();
  int empty_r();
  void sync_w(int state);

  void decode_error(u32 opcode);
  void decode_cat1(u32 opcode, u16* op, cstate* cs);
  void decode_cat2_pre(u32 opcode, u16* op, cstate* cs);
  void decode_cat3(u32 opcode, u16* op, cstate* cs);
  void decode_cat2_post(u32 opcode, u16* op, cstate* cs);

  int xmode(u32 opcode, char type, cstate* cs);
  int sfao(u32 value);
  int dbp(u32 value);
  int crm(u32 value);
  int sfai(u32 value);
  int sfmo(u32 value);
  int rnd(u32 value);
  int movm(u32 value);
  int sfma(u32 value);

  void update_dready();
  void update_pc0();
  void update_empty();

  void xm_init();
  void xm_step_read();
  void xm_step_write();
  s64 macc_to_output_0(s64 rounding, u64 rmask);
  s64 macc_to_output_1(s64 rounding, u64 rmask);
  s64 macc_to_output_2(s64 rounding, u64 rmask);
  s64 macc_to_output_3(s64 rounding, u64 rmask);
  s64 macc_to_output_0s(s64 rounding, u64 rmask);
  s64 macc_to_output_1s(s64 rounding, u64 rmask);
  s64 macc_to_output_2s(s64 rounding, u64 rmask);
  s64 macc_to_output_3s(s64 rounding, u64 rmask);
  s64 check_macc_overflow_0();
  s64 check_macc_overflow_1();
  s64 check_macc_overflow_2();
  s64 check_macc_overflow_3();
  s64 check_macc_overflow_0s();
  s64 check_macc_overflow_1s();
  s64 check_macc_overflow_2s();
  s64 check_macc_overflow_3s();
  void cache_flush();
  void add_one(cstate* cs, u16 op, u8 param);
  void decode_one(u32 opcode, cstate* cs, void (Core::*decoder)(u32 opcode, u16* op, cstate* cs));
  s16 get_hash(u8 address, u32 value, s16* previous_node);
  s16 get_hashnode(u8 address, u32 value, s16 previous_node);
  int decode_get_pc();
  u32 get_cmem(u8 address);

#define CINTRPDECL
#include "core/arcade/systems/konami/gq/konami_gq_tms57002.hxx"
#undef CINTRPDECL
};

} // namespace KonamiGQTMS57002
