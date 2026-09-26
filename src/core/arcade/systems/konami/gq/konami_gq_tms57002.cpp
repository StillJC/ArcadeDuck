// SPDX-FileCopyrightText: Olivier Galibert
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: BSD-3-Clause
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "core/pch.h"
#include "core/arcade/systems/konami/gq/konami_gq_tms57002.h"

#include "common/log.h"

#include <algorithm>
#include <cstdint>

Log_SetChannel(KonamiGQTMS57002);

namespace KonamiGQTMS57002 {

Core::Core()
{
  cache_flush();
  reset_internal();
}

void Core::Reset()
{
  reset_internal();
  std::fill(m_external_ram.begin(), m_external_ram.end(), u8{0});
  m_total_cycles = 0;
  m_sync_count = 0;
  m_external_reads = 0;
  m_external_writes = 0;
  unsupported_inst_warning = 0;
}

void Core::PLoadWrite(bool state)
{
  pload_w(state ? 1 : 0);
}

void Core::CLoadWrite(bool state)
{
  cload_w(state ? 1 : 0);
}

void Core::SetResetReleased(bool released)
{
  if (m_reset_released == released)
    return;

  m_reset_released = released;
  if (!released)
    reset_internal();
}

u8 Core::DataRead()
{
  return data_r();
}

void Core::DataWrite(u8 data)
{
  data_w(data);
}

bool Core::DataReady() const
{
  return (sti & S_HOST) == 0;
}

bool Core::ProgramCounterNonZero() const
{
  return pc != 0;
}

bool Core::UpdateFIFOEmpty() const
{
  return update_counter_head == update_counter_tail;
}

void Core::SetSerialInputs(s32 input0, s32 input1, s32 input2, s32 input3)
{
  // Match MAME's device_sound_interface conversion: a normalized K054539 sample
  // is multiplied by 32768, and SIM adds a further 256x serial-input scale.
  const s64 input_scale = (st0 & ST0_SIM) ? 256 : 1;
  const auto encode_input = [input_scale](s32 input) {
    return static_cast<u32>(static_cast<s64>(input) * input_scale) & 0x00ffffffu;
  };

  si[0] = encode_input(input0);
  si[1] = encode_input(input1);
  si[2] = encode_input(input2);
  si[3] = encode_input(input3);
}

s32 Core::GetSerialOutput(u32 channel) const
{
  if (channel >= 4)
    return 0;

  // Sign-extend the 24-bit serial output and convert it to a 16-bit PCM scale,
  // matching MAME's so[channel] / 2^23 normalized stream conversion.
  return static_cast<s32>(so[channel] << 8) >> 16;
}

void Core::Sync()
{
  if (!m_reset_released)
    return;

  sync_w(1);
  m_sync_count++;
}

void Core::pload_w(int state)
{
	const u32 old_sti = sti;
	if (state)
		sti &= ~IN_PLOAD;
	else
		sti |= IN_PLOAD;
	if (old_sti ^ sti)
	{
		if (sti & IN_PLOAD)
		{
			hidx = 0;
			pc = 0;
			ca = 0;
			sti &= ~(SU_MASK);
		}
	}
}

void Core::cload_w(int state)
{
	const u32 old_sti = sti;
	if (state)
		sti &= ~IN_CLOAD;
	else
		sti |= IN_CLOAD;
	if (old_sti ^ sti)
	{
		if (sti & IN_CLOAD)
		{
			hidx = 0;
			//ca = 0; // Seems extremely dubious
		}
	}
}

void Core::reset_internal()
{
	sti = (sti & ~(SU_MASK|S_READ|S_WRITE|S_BRANCH|S_HOST|S_UPDATE)) | (SU_ST0|S_IDLE);
	pc = 0;
	ca = 0;
	hidx = 0;
	id = 0;
	ba0 = 0;
	ba1 = 0;
	sa = 0;
	rptc = 0;
	rptc_next = 0;
	update_counter_tail = 0;
	update_counter_head = 0;
	st0 &= ~(ST0_INCS | ST0_DIRI | ST0_FI | ST0_SIM | ST0_PLRI |
			ST0_PBCI | ST0_DIRO | ST0_FO | ST0_SOM | ST0_PLRO |
			ST0_PBCO | ST0_CNS);
	st1 &= ~(ST1_AOV | ST1_SFAI | ST1_SFAO | ST1_AOVM | ST1_MOVM | ST1_MOV |
			ST1_SFMA | ST1_SFMO | ST1_RND | ST1_CRM | ST1_DBP);
	update_dready();
	update_pc0();
	update_empty();

	xba = 0;
	xoa = 0;
	for (u32& input : si)
		input = 0;
	for (u32& output : so)
		output = 0;
	cache_flush();
}

void Core::data_w(u8 data)
{
	switch (sti & (IN_PLOAD|IN_CLOAD))
	{
	case 0:
		hidx = 0;
		sti &= ~SU_CVAL;
		break;
	case IN_PLOAD:
		host[hidx++] = data;
		if (hidx >= 3)
		{
			u32 val = (host[0]<<16) | (host[1]<<8) | host[2];
			hidx = 0;

			switch (sti & SU_MASK)
			{
			case SU_ST0:
				st0 = val;
				sti = (sti & ~SU_MASK) | SU_ST1;
				break;
			case SU_ST1:
				st1 = val;
				sti = (sti & ~SU_MASK) | SU_PRG;
				break;
			case SU_PRG:
				m_program[pc++] = val & 0x00ffffffu;
				update_pc0();
				break;
			}
		}
		break;
	case IN_CLOAD:
		if (sti & SU_CVAL)
		{
			host[hidx++] = data;
			if (hidx >= 4)
			{
				u32 val = (host[0]<<24) | (host[1]<<16) | (host[2]<<8) | host[3];
				sti &= ~SU_CVAL;
				update[update_counter_head] = val;
				update_counter_head = (update_counter_head + 1) & 0x0f;
				hidx = 1; // the write shouldn't really happen until CLOAD is high though
				update_empty();
			}
		}
		else
		{
			sa = data;
			hidx = 0;
			sti |= SU_CVAL;
		}

		break;
	case IN_PLOAD|IN_CLOAD:
		host[hidx++] = data;
		if (hidx >= 4)
		{
			u32 val = (host[0]<<24) | (host[1]<<16) | (host[2]<<8) | host[3];
			hidx = 0;
			cmem[ca++] = val;
		}
		break;
	};
}

u8 Core::data_r()
{
	u8 res;
	if (!(sti & S_HOST))
		return 0xff;

	res = host[hidx];
	hidx++;
	if (hidx == 4)
	{
		hidx = 0;
		sti &= ~S_HOST;
		update_dready();
	}

	return res;
}

int Core::dready_r()
{
	return sti & S_HOST ? 0 : 1;
}

void Core::update_dready()
{
}

int Core::pc0_r()
{
	return pc == 0 ? 0 : 1;
}

void Core::update_pc0()
{
}

int Core::empty_r()
{
	return (update_counter_head == update_counter_tail);
}

void Core::update_empty()
{
}

void Core::sync_w(int state)
{
	static_cast<void>(state);
	if (sti & (IN_PLOAD /*| IN_CLOAD*/))
		return;

	allow_update = 1;
	pc = 0;
	ca = 0;
	id = 0;
	if (!(st0 & ST0_INCS))
	{
		ba0--;
		ba1++;
	}
	xba = (xba-1) & 0x7ffff;
	st1 &= ~(ST1_AOV | ST1_MOV);
	sti &= ~S_IDLE;
}

void Core::xm_init()
{
	u32 adr = xoa + xba;
	u32 mask = 0;

	switch (st0 & ST0_M)
	{
	case ST0_M_64K:  mask = 0x0ffff; break;
	case ST0_M_256K: mask = 0x3ffff; break;
	case ST0_M_1M:   mask = 0xfffff; break;
	}
	if (st0 & ST0_WORD)
		adr <<= 2;
	else
		adr <<= 1;

	if (!(st0 & ST0_SEL))
		adr <<= 1;

	xm_adr = adr & mask;
}

inline void Core::xm_step_read()
{
	u32 adr = xm_adr;
	u8 v = m_external_ram[adr & (EXTERNAL_RAM_SIZE - 1)];
	m_external_reads++;
	int done;
	if (st0 & ST0_WORD)
	{
		if (st0 & ST0_SEL)
		{
			int off = 16 - ((adr & 3) << 3);
			txrd = (txrd & ~(0xff << off)) | (v << off);
			done = off == 0;
		}
		else
		{
			int off = 20 - ((adr & 7) << 2);
			txrd = (txrd & ~(0xf << off)) | ((v & 0xf) << off);
			done = off == 0;
		}
	}
	else
	{
		if (st0 & ST0_SEL)
		{
			int off = 16 - ((adr & 1) << 3);
			txrd = (txrd & ~(0xff << off)) | (v << off);
			done = off == 8;
			if (done)
				txrd &= 0xffff00;
		}
		else
		{
			int off = 20 - ((adr & 3) << 2);
			txrd = (txrd & ~(0xf << off)) | ((v & 0xf) << off);
			done = off == 8;
			if (done)
				txrd &= 0xffff00;
		}
	}
	if (done)
	{
		xrd = txrd;
		sti &= ~S_READ;
		xm_adr = 0;
	}
	else
		xm_adr = adr+1;
}

inline void Core::xm_step_write()
{
	u32 adr = xm_adr;
	u8 v;
	int done;
	if (st0 & ST0_WORD)
	{
		if (st0 & ST0_SEL)
		{
			int off = 16 - ((adr & 3) << 3);
			v = static_cast<u8>((xwr >> off) & 0xffu);
			done = off == 0;
		}
		else
		{
			int off = 20 - ((adr & 7) << 2);
			v = (xwr >> off) & 0xf;
			done = off == 0;
		}
	}
	else
	{
		if (st0 & ST0_SEL)
		{
			int off = 16 - ((adr & 1) << 3);
			v = static_cast<u8>((xwr >> off) & 0xffu);
			done = off == 8;
		}
		else
		{
			int off = 20 - ((adr & 3) << 2);
			v = (xwr >> off) & 0xf;
			done = off == 8;
		}
	}
	m_external_ram[adr & (EXTERNAL_RAM_SIZE - 1)] = v;
	m_external_writes++;
	if (done)
	{
		sti &= ~S_WRITE;
		xm_adr = 0;
	}
	else
		xm_adr = adr+1;
}

s64 Core::macc_to_output_0(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 Core::macc_to_output_1(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xfe00000000000ULL;
	if (m1 && m1 != 0xfe00000000000ULL)
		over = true;
	m <<= 2;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 Core::macc_to_output_2(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xff80000000000ULL;
	if (m1 && m1 != 0xff80000000000ULL)
		over = true;
	m <<= 4;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 Core::macc_to_output_3(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m >>= 8;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 Core::macc_to_output_0s(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 Core::macc_to_output_1s(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xfe00000000000ULL;
	if (m1 && m1 != 0xfe00000000000ULL)
		over = true;
	m <<= 2;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 Core::macc_to_output_2s(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xff80000000000ULL;
	if (m1 && m1 != 0xff80000000000ULL)
		over = true;
	m <<= 4;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 Core::macc_to_output_3s(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m >>= 8;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 Core::check_macc_overflow_0()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 Core::check_macc_overflow_1()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xfe00000000000ULL;
	if (m1 && m1 != 0xfe00000000000ULL)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 Core::check_macc_overflow_2()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xff80000000000ULL;
	if (m1 && m1 != 0xff80000000000ULL)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 Core::check_macc_overflow_3()
{
	return macc_read;
}

s64 Core::check_macc_overflow_0s()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 Core::check_macc_overflow_1s()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xfe00000000000ULL;
	if (m1 && m1 != 0xfe00000000000ULL)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 Core::check_macc_overflow_2s()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xff80000000000ULL;
	if (m1 && m1 != 0xff80000000000ULL)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 Core::check_macc_overflow_3s()
{
	return macc_read;
}

u32 Core::get_cmem(u8 addr)
{
	if (sa == addr && update_counter_head != update_counter_tail)
		sti |= S_UPDATE;

	if (sti & S_UPDATE)
	{
		cmem[addr] = update[update_counter_tail];
		update_counter_tail = (update_counter_tail + 1) & 0x0f;
		update_empty();

		if (update_counter_head == update_counter_tail)
			sti &= ~S_UPDATE;

		return cmem[addr]; // The value of crm is ignored during an update.
	}
	else
	{
		int crm = (st1 & ST1_CRM) >> ST1_CRM_SHIFT;
		u32 cvar = cmem[addr];
		if (crm == 1)
			return (cvar & 0xffff0000);
		else if (crm == 2)
			return (cvar << 16);
		return cvar;
	}
}

void Core::cache_flush()
{
	int i;
	cache.hused = cache.iused = 0;
	for (i = 0; i != 256; i++)
		cache.hashbase[i] = -1;
	for (i = 0; i != HBS; i++)
	{
		cache.hashnode[i].st1 = 0;
		cache.hashnode[i].ipc = -1;
		cache.hashnode[i].next = -1;
	}
	for (i = 0; i != IBS; i++)
	{
		cache.inst[i].op = 0;
		cache.inst[i].next = -1;
		cache.inst[i].param = 0;
	}
}

void Core::add_one(cstate *cs, u16 op, u8 param)
{
	const s16 ipc = static_cast<s16>(cache.iused++);
	cache.inst[ipc].op = op;
	cache.inst[ipc].param = param;
	cache.inst[ipc].next = -1;
	if (cs->ipc != -1)
		cache.inst[cs->ipc].next = ipc;
	cs->ipc = ipc;
	if (cs->hnode != -1)
	{
		cache.hashnode[cs->hnode].ipc = ipc;
		cs->hnode = -1;
	}
}

void Core::decode_one(u32 opcode, cstate *cs, void (Core::*dec)(u32 opcode, u16 *op, cstate *cs))
{
	u16 op = 0;
	(this->*dec)(opcode, &op, cs);
	if (!op)
		return;
	add_one(cs, op, opcode & 0xff);
}

s16 Core::get_hash(u8 adr, u32 value, s16 *pnode)
{
	s16 hnode;
	value &= ST1_CACHE;
	*pnode = -1;
	hnode = cache.hashbase[adr];
	while(hnode != -1)
	{
		if (cache.hashnode[hnode].st1 == value)
			return cache.hashnode[hnode].ipc;
		*pnode = hnode;
		hnode = cache.hashnode[hnode].next;
	}
	return -1;
}

s16 Core::get_hashnode(u8 adr, u32 value, s16 pnode)
{
	const s16 hnode = static_cast<s16>(cache.hused++);
	cache.hashnode[hnode].st1 = value & ST1_CACHE;
	cache.hashnode[hnode].ipc = -1;
	cache.hashnode[hnode].next = -1;
	if (pnode == -1)
		cache.hashbase[adr] = hnode;
	else
		cache.hashnode[pnode].next = hnode;
	return hnode;
}

int Core::decode_get_pc()
{
	s16 pnode, res;
	cstate cs;
	u8 adr = pc;

	res = get_hash(adr, st1, &pnode);
	if (res != -1)
		return res;

	if (HBS - cache.hused < 256 || IBS - cache.iused < 256*3)
	{
		cache_flush();
		pnode = -1;
	}

	cs.hnode = res = get_hashnode(adr, st1, pnode);
	cs.ipc = -1;
	cs.branch = 0;

	for (;;)
	{
		s16 ipc;
		u32 opcode = m_program[adr] & 0x00ffffffu;

		cs.inc = 0;

		if ((opcode & 0xfc0000) == 0xfc0000)
			decode_one(opcode, &cs, &Core::decode_cat3);
		else {
			decode_one(opcode, &cs, &Core::decode_cat2_pre);
			decode_one(opcode, &cs, &Core::decode_cat1);
			decode_one(opcode, &cs, &Core::decode_cat2_post);
		}
		add_one(&cs, static_cast<u16>(cs.inc), 0);

		if (cs.branch)
			break;

		adr++;
		ipc = get_hash(adr, st1, &pnode);
		if (ipc != -1)
		{
			cache.inst[cs.ipc].next = ipc;
			break;
		}
		cs.hnode = get_hashnode(adr, st1, pnode);
	}

	return cache.hashnode[res].ipc;
}

int Core::Execute(int cycles)
{
	const int requested_cycles = std::max(cycles, 0);
	icount = requested_cycles;
	int ipc = -1;

	while(icount > 0 && !(sti & (S_IDLE | IN_PLOAD /*| IN_CLOAD*/)))
	{
		int iipc;

			if (ipc == -1)
			ipc = decode_get_pc();

		iipc = ipc;

		if (sti & (S_READ|S_WRITE))
		{
			if (sti & S_READ)
				xm_step_read();
			else
				xm_step_write();
		}

		macc_read = macc_write;
		macc_write = macc;

		for (;;)
		{
			const icd *i = cache.inst + ipc;

			ipc = i->next;
			switch (i->op)
			{
			case 0:
				goto inst;

			case 1:
				++ca;
				goto inst;

			case 2:
				++id;
				goto inst;

			case 3:
				++ca, ++id;
				goto inst;

#define CINTRPSWITCH
#include "core/arcade/systems/konami/gq/konami_gq_tms57002.hxx"
#undef CINTRPSWITCH

			default:
				unsupported_inst_warning = 1;
				icount = 0;
				return requested_cycles;
			}
		}
	inst:
		icount--;

		if (rptc)
		{
			rptc--;
			ipc = iipc;
		}
		else if (sti & S_BRANCH)
		{
			sti &= ~S_BRANCH;
			ipc = -1;
		}
		else
		{
			pc++; // Wraps if it reaches 256, next wraps too
			update_pc0();
		}

		if (rptc_next)
		{
			rptc = rptc_next;
			rptc_next = 0;
		}
	}

	if (icount > 0)
		icount = 0;

	m_total_cycles += static_cast<u64>(requested_cycles);
	return requested_cycles;
}


inline int Core::xmode(u32 opcode, char type, cstate *cs)
{
	if (((opcode & 0x400) && (type == 'c')) || (!(opcode & 0x400) && (type == 'd')))
	{
		if (opcode & 0x100)
			return 0;
		else if (opcode & 0x80)
			cs->inc |= type == 'c' ? INC_CA : INC_ID;

		return 1;
	}
	else if (opcode & 0x200)
		cs->inc |= type == 'c' ? INC_CA : INC_ID;

	return 1;
}

inline int Core::sfao(u32 value)
{
	return value & ST1_SFAO ? 1 : 0;
}

inline int Core::dbp(u32 value)
{
	return value & ST1_DBP ? 1 : 0;
}

inline int Core::crm(u32 value)
{
	// value overridden during cvar update
	if (update_counter_head != update_counter_tail)
		return 0;

	int crm = (value & ST1_CRM) >> ST1_CRM_SHIFT;
	return crm <= 2 ? crm : 0;
}

inline int Core::sfai(u32 value)
{
	return value & ST1_SFAI ? 1 : 0;
}

inline int Core::sfmo(u32 value)
{
	return (value & ST1_SFMO) >> ST1_SFMO_SHIFT;
}

inline int Core::rnd(u32 value)
{
	int rnd = (value & ST1_RND) >> ST1_RND_SHIFT;
	return rnd <= 4 ? rnd : 0;
}

inline int Core::movm(u32 value)
{
	return value & ST1_MOVM ? 1 : 0;
}

inline int Core::sfma(u32 value)
{
	return (value & ST1_SFMA) >> ST1_SFMA_SHIFT;
}

void Core::decode_error(u32 opcode)
{
	if (unsupported_inst_warning)
		return;

	unsupported_inst_warning = 1;
	static_cast<void>(opcode);
}

void Core::decode_cat1(u32 opcode, unsigned short *op, cstate *cs)
{
	switch (opcode >> 18)
	{
	case 0x00: // nop
		break;

#define CDEC1
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
#include "core/arcade/systems/konami/gq/konami_gq_tms57002.hxx"
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
#undef CDEC1

	default:
		decode_error(opcode);
		break;
	}
}

void Core::decode_cat2_pre(u32 opcode, unsigned short *op, cstate *cs)
{
	switch ((opcode >> 11) & 0x7f)
	{
	case 0x00: // nop
		break;

#define CDEC2A
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
#include "core/arcade/systems/konami/gq/konami_gq_tms57002.hxx"
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
#undef CDEC2A

	default:
		decode_error(opcode);
		break;
	}
}

void Core::decode_cat2_post(u32 opcode, unsigned short *op, cstate *cs)
{
	static_cast<void>(cs);
	switch ((opcode >> 11) & 0x7f)
	{
	case 0x00: // nop
		break;

#define CDEC2B
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
#include "core/arcade/systems/konami/gq/konami_gq_tms57002.hxx"
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
#undef CDEC2B

	default:
		decode_error(opcode);
		break;
	}
}

void Core::decode_cat3(u32 opcode, unsigned short *op, cstate *cs)
{
	switch ((opcode >> 11) & 0x7f)
	{
	case 0x00: // nop
		break;

#define CDEC3
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
#include "core/arcade/systems/konami/gq/konami_gq_tms57002.hxx"
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
#undef CDEC3

	default:
		decode_error(opcode);
		break;
	}
}

} // namespace KonamiGQTMS57002
