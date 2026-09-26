// license:BSD-3-Clause
// Standalone adaptation of MAME's ADSP-21xx core, copyright Aaron Giles.
#include "adsp2181_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define INLINE static inline
#define LSB_FIRST 1
#define CLEAR_LINE 0
#define ASSERT_LINE 1
#define CHIP_TYPE_ADSP2101 1
#define CHIP_TYPE_ADSP2181 5
#define ADSP2101_IRQ0 0
#define ADSP2101_IRQ1 1
#define ADSP2101_IRQ2 2
#define ADSP2101_SPORT0_RX 3
#define ADSP2101_SPORT0_TX 4
#define PC_STACK_DEPTH 16
#define CNTR_STACK_DEPTH 4
#define STAT_STACK_DEPTH 4
#define LOOP_STACK_DEPTH 4

typedef uint8_t UINT8;
typedef int8_t INT8;
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef uint32_t UINT32;
typedef int32_t INT32;
typedef uint64_t UINT64;
typedef int64_t INT64;

#define logerror(...) fprintf(stderr, __VA_ARGS__)

/* 16-bit registers that can be loaded signed or unsigned */
typedef union
{
	UINT16	u;
	INT16	s;
} ADSPREG16;


/* the SHIFT result register is 32 bits */
typedef union
{
#ifdef LSB_FIRST
	struct { ADSPREG16 sr0, sr1; } srx;
#else
	struct { ADSPREG16 sr1, sr0; } srx;
#endif
	UINT32 sr;
} SHIFTRESULT;


/* the MAC result register is 40 bits */
typedef union
{
#ifdef LSB_FIRST
	struct { ADSPREG16 mr0, mr1, mr2, mrzero; } mrx;
	struct { UINT32 mr0, mr1; } mry;
#else
	struct { ADSPREG16 mrzero, mr2, mr1, mr0; } mrx;
	struct { UINT32 mr1, mr0; } mry;
#endif
	UINT64 mr;
} MACRESULT;

/* there are two banks of "core" registers */
typedef struct ADSPCORE
{
	/* ALU registers */
	ADSPREG16	ax0, ax1;
	ADSPREG16	ay0, ay1;
	ADSPREG16	ar;
	ADSPREG16	af;

	/* MAC registers */
	ADSPREG16	mx0, mx1;
	ADSPREG16	my0, my1;
	MACRESULT	mr;
	ADSPREG16	mf;

	/* SHIFT registers */
	ADSPREG16	si;
	ADSPREG16	se;
	ADSPREG16	sb;
	SHIFTRESULT	sr;

	/* dummy registers */
	ADSPREG16	zero;
} ADSPCORE;


/* ADSP-2100 Registers */
struct adsp2181
{
	/* Core registers, 2 banks */
	ADSPCORE	core;
	ADSPCORE	alt;

	/* Memory addressing registers */
	UINT32		i[8];
	INT32		m[8];
	UINT32		l[8];
	UINT32		lmask[8];
	UINT32		base[8];
	UINT8		px;

	/* other CPU registers */
	UINT32		pc;
	UINT32		ppc;
	UINT32		loop;
	UINT32		loop_condition;
	UINT32		cntr;
	UINT8		cntr_valid;

	/* status registers */
	UINT32		astat;
	UINT32		sstat;
	UINT32		mstat;
	UINT32		mstat_prev;
	UINT32		astat_clear;
	UINT32		idle;

	/* stacks */
	UINT32		loop_stack[LOOP_STACK_DEPTH];
	UINT32		cntr_stack[CNTR_STACK_DEPTH];
	UINT32		pc_stack[PC_STACK_DEPTH];
	UINT16		stat_stack[STAT_STACK_DEPTH][3];
	INT32		pc_sp;
	INT32		cntr_sp;
	INT32		stat_sp;
	INT32		loop_sp;
	/* Stack depths as they stood when adsp2181_call() last injected a
	 * service call, so an abandoned one can be unwound. See there. */
	INT32		inject_pc_sp;
	INT32		inject_cntr_sp;
	INT32		inject_stat_sp;
	INT32		inject_loop_sp;
	UINT16		inject_return;
	UINT8		inject_valid;
	/* Which of the four SSTAT overflow bits have already been reported, so
	 * the warning is one line per stack per card rather than one per push.
	 * See warn_stack_over() in 2100ops.inc. */
	UINT8		stack_over_warned;

	/* external I/O */
	UINT8		flagout;
	UINT8		flagin;
	UINT8		fl0;
	UINT8		fl1;
	UINT8		fl2;
	UINT16		idma_addr;
	UINT16		idma_cache;
	UINT8		idma_offs;
	UINT8		idma_pending_write;
	UINT8		idma_boot_hold;
	UINT8		idma_boot_mode;
	UINT16		sport_rx[2];
	UINT16		sport_tx[2];
	/* How often the firmware wrote each transmit latch since the counter was
	 * cleared. sport_tx alone cannot answer that: it holds the last value
	 * written whenever it was written, so a page that publishes its transmit
	 * sample through a DM pointer instead leaves a stale word behind. */
	UINT32		sport_tx_written[2];

	/* interrupt handling */
	UINT16		imask;
	UINT8		icntl;
	UINT16		ifc;
	UINT16		pmovlay;
	UINT16		dmovlay;
    UINT8   	irq_state[ADSP2181_IRQ_COUNT];
    UINT8   	irq_latch[ADSP2181_IRQ_COUNT];
    UINT8   	interrupts_enabled;
    void *irq_callback;

	/* other internal states */
    int			icount;
	int			chip_type;
	int			mstat_mask;
	int			imask_mask;

	/* register maps */
	void *		alu_xregs[8];
	void *		alu_yregs[4];
	void *		mac_xregs[8];
	void *		mac_yregs[4];
	void *		shift_xregs[8];

    /* other callbacks */
	adsp2181_rx_cb sport_rx_callback;
	adsp2181_tx_cb sport_tx_callback;
	adsp2181_timer_cb timer_fired;
	adsp2181_dm_read_cb dm_read_callback;
	adsp2181_dm_write_cb dm_write_callback;
	adsp2181_io_read_cb io_read_callback;
	adsp2181_io_write_cb io_write_callback;

	/* memory spaces */
    UINT32 program[0x4000];
    UINT16 data[0x4000];
    UINT32 program_overlay[2][0x2000];
    UINT16 data_overlay[2][0x2000];
    UINT16 io[0x800];

	/* reverse-engineering instrumentation */
    UINT8 watch_dm[0x4000];
    /* Events still to be logged for a watched DM address, or 0 for no limit.
     * Counts reads and writes together: a hung loop can touch every word of
     * low DM millions of times (Session 114y), so an unbounded watch on an
     * address it sweeps is not affordable. */
    UINT32 watch_dm_left[0x4000];
    /* Watch writes only.  Verifying "nothing writes into this range" needs the
     * reads suppressed: the firmware reads low DM constantly, and a read would
     * spend the per-address budget before the write that matters arrives. */
    UINT8 watch_dm_wonly[0x4000];
    UINT8 watch_pm[0x4000];
    /* Hold a PM word at a chosen value against DSP stores.  EICON_FORCE_DM
     * writes at overlay-load time, which is useless for a word the firmware
     * rewrites afterwards: Session 188l's PM 0x3805 is patched 14,000 cycles
     * after 0x0267 lands.  A pin re-imposes the value after every store, so the
     * A/B is "what if that patch had not stuck" rather than "what if the image
     * had shipped differently".  Overlay loads and host writes bypass
     * WWORD_PGM entirely and so bypass this too -- same caveat as the watch. */
    UINT8 pin_pm[0x4000];
    UINT32 pin_pm_value[0x4000];
    UINT32 pin_pm_hits[0x4000];
    /* The same thing for data memory.  EICON_FORCE_DM writes once per 8 kHz
     * frame, before the page runs, so it cannot reach a word the firmware
     * writes and reads again inside one frame -- and three of the words worth
     * A/Bing are exactly that shape: DM(0x3FBB) and DM(0x170B) (Session 193)
     * and DM(0x0f6d..0x0f72) (Session 199), each written ~1,100 cycles before
     * the code that consumes it, in the same frame.  Forcing them reads as a
     * clean negative while actually testing nothing.  A pin re-imposes the
     * value after every store, which is the difference between "the firmware
     * did not compute that" and "the experiment never ran". */
    UINT8 pin_dm[0x4000];
    UINT16 pin_dm_value[0x4000];
    UINT32 pin_dm_hits[0x4000];
    /* PC-stack depth extremes since the caller last read them.  The overflow
     * warning says depth reached 16 but not how it got there, and that is the
     * whole question for Session 188o: a depth that climbs and never comes back
     * down is frames being pushed and not popped, while one that spikes and
     * recovers is genuine interrupt nesting.  Sampled per 8 kHz frame by the
     * caller, so both extremes inside a frame have to be kept here. */
    UINT8 pcsp_window_min;
    UINT8 pcsp_window_max;
    /* State immediately before SPORT0 assertion and after its execution
     * allowance.  Unlike a Python-side sample, these bracket the interrupt
     * inside this translation unit and therefore cannot accidentally describe
     * setup before it or host continuation work after it. */
    UINT32 sport_entry_snapshot[21];
    UINT32 sport_return_snapshot[21];
    /* Master gate for every watch.  A PM address means a different instruction
     * on each resident page, so a watch armed for one page also fires on all
     * the others: Session 188l read "0x378e never executes in the V.32 window"
     * and "0x3805 never executes in the V.32 window" off limits that earlier
     * pages had already spent, and both were wrong.  Gating on residency spends
     * the limit where the question is.  Defaults on, so an ungated caller sees
     * exactly the old behaviour. */
    UINT8 watch_gate;
    /* Stop-on-publish.  A run-to-idle page marks the end of one sample's work
     * with IDLE; the V.34 page never idles, so the caller can only give it a
     * fixed instruction budget and take whatever the transmit word holds at
     * the cut.  Measured on the loopback rig that is 9-12 runs of the transmit
     * chain per 8 kHz sample, so the line gets an aliased tenth of a waveform.
     * Arming this on the transmit word makes the write itself the boundary:
     * execute() returns as soon as the page has published one sample. */
    UINT16 stop_dm_addr;
    UINT8 stop_dm_armed;
    UINT8 stop_dm_hit;
    /* Publishes still to see before stopping. The generator's loop arms
     * CNTR = 3 -- one symbol's polyphase set -- and stopping on the first
     * store abandons the other two, which is what leaves the carrier buried in
     * splatter (Session 169). Counting to the group size lets the loop finish. */
    UINT16 stop_dm_left;
    UINT16 stop_dm_group;
    /* Latch-on-publish. Stopping the core at the transmit publish keeps it out
     * of IDLE, so the caller's continuation never runs and the kernel
     * foreground starves (Session 165: PM 0x02a9 344,933 -> 39,910). Latching
     * takes the *first* published value of the tick and lets the frame run to
     * completion instead, which gives one sample per tick without altering
     * execution flow at all. */
    UINT16 latch_dm_addr;
    UINT8 latch_dm_armed;
    UINT8 latch_dm_have;
    UINT16 latch_dm_value;
    /* How many times the tick wrote that word, not just the first value. One
     * publish per tick is the contract the whole transmit path assumes, and
     * page 8 broke it by publishing 9-12 times (Session 149's transmitter
     * decimated by ten). The V.32/LEC arm has never been able to check it: a
     * write watch on a per-sample address changes the run (Session 188s), so
     * the count has to be free. This is. */
    UINT32 latch_dm_count;
    /* Treat a stop-on-publish as a yield rather than a halt: run the caller's
     * continuation and then leave the core where the frame stopped, so the next
     * sample resumes the page's foreground instead of restarting it. Without
     * this the continuation is skipped on every paced tick, because
     * adsp2181_modem_sample() only runs it out of IDLE (Session 165). */
    UINT8 yield_on_stop;
    /* Inject the per-frame continuation even when the budget expired with the
     * core still in the page's foreground. See adsp2181_modem_sample(). */
    UINT8 continue_non_idle;
    UINT8 watch_exec[0x4000];
    /* Executions still to be logged for a watched address, or 0 for no limit.
     * A hot address can execute hundreds of millions of times in one call --
     * PM 0x2e1b reached 941 M in Session 114u -- so the first few executions
     * have to be obtainable without the rest. */
    UINT32 watch_exec_left[0x4000];
    UINT16 exec_history[64];
    UINT8 exec_history_pos;
    UINT8 exec_history_enabled;
    UINT8 watch_irqs;
    /* Per-address DM write census.  coverage[] answers "which instructions ran"
     * and the DM watches answer "who wrote this one word"; neither answers
     * "which word is written at the symbol rate", which is what finding a
     * software symbol clock needs.  3429 baud against 8 kHz is 3 symbols per 7
     * samples, so a symbol-rate counter shows up as 0.4286 writes per sample
     * and nothing else does -- but only if every address is counted at once,
     * because the candidate set is the whole page. */
    UINT8 dm_census_on;
    UINT64 dm_census[0x4000];
    UINT64 cycles;
    UINT64 coverage[0x4000];
    UINT8 coverage_on;
    INT64 trace_budget;

};
typedef struct adsp2181 adsp2100_state;



/***************************************************************************
    PRIVATE GLOBAL VARIABLES
***************************************************************************/

static UINT16 *reverse_table = 0;
static UINT16 *mask_table = 0;
static UINT8 *condition_table = 0;

static UINT16 *reverse_table;
static UINT16 *mask_table;
static UINT8 *condition_table;
static void check_irqs(adsp2100_state *adsp);
/* Spend one event from a limited DM watch, clearing the flag on the last one
 * so the hot path costs only the array test afterwards.  Returns 1 while the
 * event should still be logged. */
INLINE int watch_dm_charge(adsp2100_state *a, UINT32 x)
{
    UINT32 *left = &a->watch_dm_left[x];
    if (*left && --*left == 0)
        a->watch_dm[x] = 0;
    return 1;
}

INLINE UINT16 RWORD_DATA(adsp2100_state *a, UINT32 x)
{
    UINT16 v;
    x &= 0x3fff;
    if (x < 0x2000 && a->dmovlay >= 1 && a->dmovlay <= 2)
        v = a->data_overlay[a->dmovlay - 1][x];
    else
        v = a->data[x];
    if (a->watch_gate && a->watch_dm[x] && !a->watch_dm_wonly[x]
        && watch_dm_charge(a, x))
        logerror("[WATCH] dm r %04x=%04x pc=%04x ov=%u cyc=%llu\n", x, v,
                 (unsigned)(a->pc & 0x3fff), (unsigned)a->dmovlay,
                 (unsigned long long)a->cycles);
    if (a->dm_read_callback)
        v = a->dm_read_callback(a, (UINT16)x, v);
    return v;
}
INLINE void WWORD_DATA(adsp2100_state *a, UINT32 x, UINT16 v)
{
    x &= 0x3fff;
    /* A pinned word takes the store and has it undone, so anything watching
     * the address still sees the firmware's value while the memory keeps the
     * pinned one. Logged the first eight times, because a pin that never fires
     * is an A/B that silently tested nothing. */
    if (a->pin_dm[x]) {
        if (v != a->pin_dm_value[x] && a->pin_dm_hits[x] < 8) {
            a->pin_dm_hits[x]++;
            logerror("[PIN] dm %04x store=%04x held at %04x ppc=%04x pc=%04x "
                     "cyc=%llu\n", x, v, a->pin_dm_value[x],
                     (unsigned)(a->ppc & 0x3fff), (unsigned)(a->pc & 0x3fff),
                     (unsigned long long)a->cycles);
        }
        v = a->pin_dm_value[x];
    }
    if (a->latch_dm_armed && x == a->latch_dm_addr)
    {
        if (!a->latch_dm_have)
        {
            a->latch_dm_value = v;
            a->latch_dm_have = 1;
        }
        a->latch_dm_count++;
    }
    if (a->stop_dm_armed && x == a->stop_dm_addr)
    {
        /* Let the store itself complete -- the value is what the caller is
         * waiting for -- and end the run once the whole group has been
         * published. */
        if (a->stop_dm_left > 1) {
            a->stop_dm_left--;
        } else {
            a->stop_dm_hit = 1;
            a->icount = 0;
        }
    }
    if (a->dm_census_on)
        a->dm_census[x]++;
    if (a->watch_gate && a->watch_dm[x] && watch_dm_charge(a, x))
    {
        /* pmov is the PMOVLAY in force at the store. PM at or above 0x2000
         * is overlaid, so a writer PC up there does not identify the code
         * without it -- Session 155 could not say which overlay PM 0x3792
         * belonged to for exactly this reason. */
        logerror("[WATCH] dm w %04x=%04x ppc=%04x pc=%04x ov=%u pmov=%u cyc=%llu "
                 "i0=%04x i4=%04x m0=%04x m4=%04x m7=%04x "
                 "ar=%04x af=%04x mr0=%04x mr1=%04x sr0=%04x sr1=%04x\n", x, v,
                 (unsigned)(a->ppc & 0x3fff), (unsigned)(a->pc & 0x3fff),
                 (unsigned)a->dmovlay, (unsigned)a->pmovlay,
                 (unsigned long long)a->cycles,
                 (unsigned)(a->i[0] & 0x3fff), (unsigned)(a->i[4] & 0x3fff),
                 (unsigned)(a->m[0] & 0xffff), (unsigned)(a->m[4] & 0xffff),
                 (unsigned)(a->m[7] & 0xffff),
                 a->core.ar.u, a->core.af.u, a->core.mr.mrx.mr0.u,
                 a->core.mr.mrx.mr1.u, a->core.sr.srx.sr0.u,
                 a->core.sr.srx.sr1.u);
        logerror("[WATCH] prior pcs:");
        for (unsigned n = 24; n > 0; n--)
            logerror(" %04x", a->exec_history[(a->exec_history_pos - n) & 63]);
        logerror("\n");
    }
    if (x < 0x2000 && a->dmovlay >= 1 && a->dmovlay <= 2)
        a->data_overlay[a->dmovlay - 1][x] = v;
    else
        a->data[x] = v;
    if (a->dm_write_callback)
        a->dm_write_callback(a, (UINT16)x, v);
}
INLINE UINT16 RWORD_IO(adsp2100_state *a, UINT32 x)
{
    UINT16 v = a->io[x & 0x7ff];
    return a->io_read_callback ? a->io_read_callback(a, (UINT16)(x & 0x7ff), v) : v;
}
INLINE void WWORD_IO(adsp2100_state *a, UINT32 x, UINT16 v)
{
    x &= 0x7ff;
    a->io[x] = v;
    if (a->io_write_callback)
        a->io_write_callback(a, (UINT16)x, v);
}
INLINE UINT32 RWORD_PGM(adsp2100_state *a, UINT32 x)
{
    x &= 0x3fff;
    if (x >= 0x2000 && a->pmovlay >= 1 && a->pmovlay <= 2)
        return a->program_overlay[a->pmovlay - 1][x - 0x2000] & 0xffffff;
    return a->program[x] & 0xffffff;
}
INLINE void WWORD_PGM(adsp2100_state *a, UINT32 x, UINT32 v)
{
    x &= 0x3fff;
    /* adsp2181_watch_pm() has set this flag since the core was imported and
     * nothing ever read it, so "no PM writer was found" has never been
     * evidence of anything. It fires here now. Session 188 needs it because
     * PM 0x1d8e holds 0x8f7545 on both ends when the V.32 page loads and
     * executes as 0x66e002 on one of them later: something writes code. */
    if (a->watch_gate && a->watch_pm[x])
    {
        UINT32 old = (x >= 0x2000 && a->pmovlay >= 1 && a->pmovlay <= 2)
                   ? a->program_overlay[a->pmovlay - 1][x - 0x2000] & 0xffffff
                   : a->program[x] & 0xffffff;
        if (old != (v & 0xffffff))
            logerror("[WATCH] pm w %04x=%06x was=%06x ppc=%04x pc=%04x "
                     "pmov=%u dmov=%u cyc=%llu i4=%04x i5=%04x m5=%04x "
                     "m7=%04x ar=%04x\n",
                     x, (unsigned)(v & 0xffffff), (unsigned)old,
                     (unsigned)(a->ppc & 0x3fff), (unsigned)(a->pc & 0x3fff),
                     (unsigned)a->pmovlay, (unsigned)a->dmovlay,
                     (unsigned long long)a->cycles,
                     (unsigned)(a->i[4] & 0x3fff), (unsigned)(a->i[5] & 0x3fff),
                     (unsigned)(a->m[5] & 0x3fff), (unsigned)(a->m[7] & 0x3fff),
                     a->core.ar.u);
    }
    /* A pinned word takes the store and then has it undone, so the write still
     * happens for anything watching it and the value the core executes is the
     * pinned one.  Logged the first eight times: a pin that never fires is an
     * A/B that silently tested nothing, which is the failure mode to catch. */
    if (a->pin_pm[x]) {
        UINT32 pinned = a->pin_pm_value[x] & 0xffffff;
        if ((v & 0xffffff) != pinned && a->pin_pm_hits[x] < 8) {
            a->pin_pm_hits[x]++;
            logerror("[PIN] pm %04x store=%06x held at %06x ppc=%04x pc=%04x "
                     "cyc=%llu\n", x, (unsigned)(v & 0xffffff),
                     (unsigned)pinned, (unsigned)(a->ppc & 0x3fff),
                     (unsigned)(a->pc & 0x3fff),
                     (unsigned long long)a->cycles);
        }
        v = pinned;
    }
    if (x >= 0x2000 && a->pmovlay >= 1 && a->pmovlay <= 2)
        a->program_overlay[a->pmovlay - 1][x - 0x2000] = v & 0xffffff;
    else
        a->program[x] = v & 0xffffff;
}

/* ------------------------------------------------------------------
 * Diagnostic: PC-gated MAME carry on "Y - 1" (ALU case 0x08).
 *
 * 7d756ba corrected AC on this op to the datasheet's rule (DECREMENT,
 * Instruction Set Reference p.15-37: AC set when a carry is generated, so
 * AC = 1 for yop >= 1 and 0 only at yop == 0). That fix is right and stays,
 * but it unmasks a second emulator defect somewhere else -- the loopback
 * caller no longer leaves its dial page. These hooks make the old MAME rule
 * selectable per instruction address so the sites the failure actually
 * depends on can be named:
 *
 *   EICON_YM1_MAME_ALL=1        every Y-1 uses MAME's rule
 *   EICON_YM1_MAME_PCS=a,b,...  only Y-1 executed at these PM addresses
 *   EICON_YM1_LOG=N             log the first N Y-1 evaluations
 *
 * All inert unless set; with nothing set the datasheet rule runs everywhere.
 * ------------------------------------------------------------------ */
int ym1_mame_all = -1;
unsigned char ym1_mame_pc[0x4000];
long ym1_log_budget = -1;

static void ym1_env_init(void)
{
    const char *s;
    if (ym1_mame_all >= 0) return;
    ym1_mame_all = 0;
    s = getenv("EICON_YM1_MAME_ALL");
    if (s && *s && *s != '0') ym1_mame_all = 1;
    s = getenv("EICON_YM1_MAME_PCS");
    if (s && *s) {
        const char *p = s;
        while (*p) {
            char *end;
            long lo = strtol(p, &end, 0), hi;
            if (end == p) break;
            hi = lo;
            if (*end == '-') {            /* LO-HI selects a whole range, so a
                                           * bisection needs no PC list first */
                p = end + 1;
                hi = strtol(p, &end, 0);
            }
            if (lo < 0) lo = 0;
            if (hi > 0x3fff) hi = 0x3fff;
            for (; lo <= hi; lo++) ym1_mame_pc[lo] = 1;
            p = (*end == ',') ? end + 1 : end;
        }
    }
    s = getenv("EICON_YM1_LOG");
    ym1_log_budget = (s && *s) ? strtol(s, NULL, 0) : 0;
}

#define ROPCODE(a) RWORD_PGM((a), (a)->pc)

#include "2100ops.inc"

static int generate_irq(adsp2100_state *adsp, int which, int priority)
{
    if (!(adsp->imask & (0x200 >> priority)))
        return 0;
    if (adsp->watch_irqs)
        logerror("[IRQ] take=%d priority=%d from=%04x cyc=%llu imask=%03x icntl=%02x pcsp=%u statsp=%u\n",
                 which, priority, (unsigned)(adsp->pc & 0x3fff),
                 (unsigned long long)adsp->cycles, (unsigned)adsp->imask,
                 (unsigned)adsp->icntl, (unsigned)adsp->pc_sp,
                 (unsigned)adsp->stat_sp);
    adsp->irq_latch[which] = 0;
    pc_stack_push(adsp);
    stat_stack_push(adsp);
    adsp->pc = 0x04 + priority * 4;
    adsp->idle = 0;
    if (adsp->icntl & 0x10)
        adsp->imask &= ~(0x3ff >> priority);
    else
        adsp->imask &= ~0x3ff;
    return 1;
}

static void check_irqs(adsp2100_state *adsp)
{
    UINT8 check;
    if (!adsp->interrupts_enabled)
        return;
#define TRY_IRQ(which, priority, expression) \
    do { check = (expression); if (check && generate_irq(adsp, (which), (priority))) return; } while (0)
    TRY_IRQ(ADSP2181_IRQ2,      0, (adsp->icntl & 4) ? adsp->irq_latch[ADSP2181_IRQ2] : adsp->irq_state[ADSP2181_IRQ2]);
    TRY_IRQ(ADSP2181_IRQL1,     1, adsp->irq_state[ADSP2181_IRQL1]);
    TRY_IRQ(ADSP2181_IRQL0,     2, adsp->irq_state[ADSP2181_IRQL0]);
    TRY_IRQ(ADSP2181_SPORT0_TX, 3, adsp->irq_latch[ADSP2181_SPORT0_TX]);
    TRY_IRQ(ADSP2181_SPORT0_RX, 4, adsp->irq_latch[ADSP2181_SPORT0_RX]);
    TRY_IRQ(ADSP2181_IRQE,      5, adsp->irq_latch[ADSP2181_IRQE]);
    TRY_IRQ(ADSP2181_BDMA,      6, adsp->irq_latch[ADSP2181_BDMA]);
    TRY_IRQ(ADSP2181_IRQ1,      7, (adsp->icntl & 2) ? adsp->irq_latch[ADSP2181_IRQ1] : adsp->irq_state[ADSP2181_IRQ1]);
    TRY_IRQ(ADSP2181_IRQ0,      8, (adsp->icntl & 1) ? adsp->irq_latch[ADSP2181_IRQ0] : adsp->irq_state[ADSP2181_IRQ0]);
    TRY_IRQ(ADSP2181_TIMER,     9, adsp->irq_latch[ADSP2181_TIMER]);
#undef TRY_IRQ
}

static void execute(adsp2100_state *adsp)
{

	check_irqs(adsp);

	do
	{
		UINT32 temp;
		UINT32 op;

		/* debugging */
		adsp->ppc = adsp->pc;	/* copy PC to previous PC */

		/* instruction fetch */
		op = ROPCODE(adsp);
		if (adsp->exec_history_enabled)
			adsp->exec_history[adsp->exec_history_pos++ & 63] = adsp->pc & 0x3fff;
        /* Gated, because a count summed over a whole call is not a count of
         * anything: the pages are swapped into the same PM by download, not
         * selected by PMOVLAY, so PM 0x3768 holds the V.34 generator's
         * `DO $3792 UNTIL NOT CE`, the INFO page's `NOP, AY0 = DM(I1,M0)` and
         * something else again in V.8, all at the same address and the same
         * overlay. Only the caller knows which page is resident, so only the
         * caller can say when the count means the page it is asking about.
         * Session 169 read a generator loop rate off an ungated count.
         * Defaults on, so callers that never gate keep the old behaviour. */
        if (adsp->coverage_on)
            adsp->coverage[adsp->pc & 0x3fff]++;

        if (adsp->watch_gate && adsp->watch_exec[adsp->pc & 0x3fff]) {
            /* A limited watch clears its own flag on the last logged
             * execution, so the hot path costs the same array test as before
             * once the budget is spent. */
            UINT32 *left = &adsp->watch_exec_left[adsp->pc & 0x3fff];
            if (*left && --*left == 0) {
                adsp->watch_exec[adsp->pc & 0x3fff] = 0;
                /* Say so, loudly.  A spent limit and an address that never runs
                 * both produce no further output, and Session 188l read the
                 * first as the second twice.  With this line, silence after it
                 * means "stopped looking" and silence without it means "did not
                 * happen". */
                logerror("[EXEC] limit spent for pc=%04x at cyc=%llu -- no "
                         "further executions of this address will be logged\n",
                         (unsigned)(adsp->pc & 0x3fff),
                         (unsigned long long)adsp->cycles);
            }
            unsigned ret = adsp->pc_sp ? pc_stack_top(adsp) & 0x3fff : 0xffff;
            /* ax1/ar/mr1 carry the control-channel correlator magnitude at the
             * PM 0x3515 decision seam: PM 0x350b puts |MR1| in AR, PM 0x350d
             * copies it to AX1, and the bit in DM(0x060f) is that magnitude
             * thresholded at 0x0578.  Neither value is ever stored to DM. */
            /* pmovlay and the fetched word are logged together because a PM
             * address at or above 0x2000 means a different instruction on
             * each overlay page: the pair says which page actually ran. */
            logerror("[EXEC] pc=%04x from=%04x ret=%04x pmovlay=%u dmovlay=%u op=%06x "
                     "cyc=%llu cntr=%04x psp=%d csp=%d lsp=%d astat=%02x mstat=%02x "
                     /* i6/i7 are the DAG2 pointers a PM-resident data
                      * stream is read through: PM 0x338c does
                      * `SR1 = PM(I7,M5)`, and without I7 the log says the
                      * sample was zero but not which address it came from. */
                     "i0=%04x i1=%04x i4=%04x i5=%04x i6=%04x i7=%04x "
                     "m1=%04x m3=%04x "
                     "l0=%04x b0=%04x l1=%04x b1=%04x "
                     /* the DAG2 side of DM(I4,M5): the stride the block-loader's
                      * field unpacker at PM 0x2e24 walks its record with, and
                      * the L4/B4 pair that decides whether I4 wraps inside a
                      * circular buffer or runs on through data memory. */
                     "m5=%04x l4=%04x b4=%04x "
                     "ax0=%04x ax1=%04x ay0=%04x ay1=%04x mx0=%04x mx1=%04x "
                     "my0=%04x my1=%04x af=%04x ar=%04x mr0=%04x mr1=%04x "
                     "sr0=%04x sr1=%04x si=%04x se=%04x rx0=%04x "
                     "state=%04x event=%04x span=%04x count=%04x stride=%04x "
                     "istate=%04x analysis=%04x dmi1=%04x\n",
                     (unsigned)(adsp->pc & 0x3fff),
                     adsp->exec_history[(adsp->exec_history_pos - 2) & 63], ret,
                     (unsigned)adsp->pmovlay, (unsigned)adsp->dmovlay,
                     (unsigned)op,
                     (unsigned long long)adsp->cycles, (unsigned)(adsp->cntr & 0x3fff),
                     (int)adsp->pc_sp, (int)adsp->cntr_sp, (int)adsp->loop_sp,
                     (unsigned)(adsp->astat & 0xff),
                     (unsigned)(adsp->mstat & 0xff),
                     adsp->i[0] & 0x3fff, adsp->i[1] & 0x3fff,
                     adsp->i[4] & 0x3fff, adsp->i[5] & 0x3fff,
                     adsp->i[6] & 0x3fff, adsp->i[7] & 0x3fff,
                     adsp->m[1] & 0x3fff,
                     adsp->m[3] & 0x3fff, adsp->l[0] & 0x3fff,
                     adsp->base[0] & 0x3fff, adsp->l[1] & 0x3fff,
                     adsp->base[1] & 0x3fff,
                     adsp->m[5] & 0x3fff, adsp->l[4] & 0x3fff,
                     adsp->base[4] & 0x3fff,
                     adsp->core.ax0.u & 0xffff,
                     adsp->core.ax1.u & 0xffff, adsp->core.ay0.u & 0xffff,
                     adsp->core.ay1.u & 0xffff,
                     adsp->core.mx0.u & 0xffff, adsp->core.mx1.u & 0xffff,
                     adsp->core.my0.u & 0xffff, adsp->core.my1.u & 0xffff,
                     adsp->core.af.u & 0xffff, adsp->core.ar.u & 0xffff,
                     /* mr0 carries the candidate record pointer the sequencer
                      * loads from DM(0x1692..0x1695) before testing its
                      * condition; at PM 0x334d it is the record selected. */
                     adsp->core.mr.mrx.mr0.u & 0xffff,
                     adsp->core.mr.mrx.mr1.u & 0xffff,
                     adsp->core.sr.srx.sr0.u & 0xffff,
                     adsp->core.sr.srx.sr1.u & 0xffff,
                     adsp->core.si.u & 0xffff, adsp->core.se.u & 0xffff,
                     adsp->sport_rx[0] & 0xffff,
                     adsp->data[0x16bd], adsp->data[0x198e], adsp->data[0x16c5],
                     adsp->data[0x16c6], adsp->data[0x16c7],
                     /* the INFO sequencer's internal state and the analysis
                      * counter its record conditions compare against */
                     adsp->data[0x1652], adsp->data[0x06e6],
                     ((adsp->i[1] & 0x3fff) < 0x2000 && adsp->dmovlay >= 1
                      && adsp->dmovlay <= 2)
                         ? adsp->data_overlay[adsp->dmovlay - 1]
                                             [adsp->i[1] & 0x3fff]
                         : adsp->data[adsp->i[1] & 0x3fff]);
            /* A single `from=` cannot distinguish a jump into the middle of a
             * loop body from the loop's own back-edge, because the back-edge
             * is `pc = pc_stack_top()` and shows the last body instruction
             * either way. The trail and the loop stack do distinguish them,
             * which is what Session 188's "what enters 0x1d90 without the DO
             * at 0x1d8f" needs: `loop` is the end address the sequencer
             * compares PC against, and it is the one piece of state that can
             * outlive the DO that installed it. */
            logerror("[EXEC] prior pcs:");
            for (unsigned n = 24; n > 0; n--)
                logerror(" %04x",
                         adsp->exec_history[(adsp->exec_history_pos - n) & 63]);
            logerror("\n[EXEC] loop=%04x cond=%u lsp=%d cntr=%04x cvalid=%u"
                     " pcstacktop=%04x stack=",
                     (unsigned)(adsp->loop & 0xffff),
                     (unsigned)adsp->loop_condition, (int)adsp->loop_sp,
                     (unsigned)(adsp->cntr & 0x3fff),
                     (unsigned)adsp->cntr_valid,
                     (unsigned)(adsp->pc_sp ? pc_stack_top(adsp) & 0x3fff
                                            : 0xffff));
            for (int n = 0; n < (int)adsp->loop_sp && n < LOOP_STACK_DEPTH; n++)
                logerror(" [%d]end=%04x,cond=%u", n,
                         (unsigned)((adsp->loop_stack[n] >> 4) & 0xffff),
                         (unsigned)(adsp->loop_stack[n] & 15));
            logerror("\n");
        }

		if (adsp->trace_budget > 0) {
			adsp->trace_budget--;
			logerror("[TRACE] pc=%04x op=%06x ar=%04x ax0=%04x ax1=%04x "
				 "ay0=%04x ay1=%04x mx0=%04x mx1=%04x my0=%04x my1=%04x "
				 "mr0=%04x mr1=%04x sr0=%04x sr1=%04x astat=%02x i1=%04x "
				 "i4=%04x i5=%04x i6=%04x i7=%04x cyc=%llu\n",
				 (unsigned)(adsp->pc & 0x3fff), op,
				 adsp->core.ar.u & 0xffff,
				 adsp->core.ax0.u & 0xffff, adsp->core.ax1.u & 0xffff,
				 adsp->core.ay0.u & 0xffff, adsp->core.ay1.u & 0xffff,
				 adsp->core.mx0.u & 0xffff, adsp->core.mx1.u & 0xffff,
				 adsp->core.my0.u & 0xffff, adsp->core.my1.u & 0xffff,
				 adsp->core.mr.mrx.mr0.u & 0xffff,
				 adsp->core.mr.mrx.mr1.u & 0xffff,
				 adsp->core.sr.srx.sr0.u & 0xffff,
				 adsp->core.sr.srx.sr1.u & 0xffff, adsp->astat,
				 adsp->i[1] & 0x3fff,
				 adsp->i[4] & 0x3fff, adsp->i[5] & 0x3fff,
				 adsp->i[6] & 0x3fff, adsp->i[7] & 0x3fff,
				 (unsigned long long)adsp->cycles);
		}

		/* advance to the next instruction */
		if (adsp->pc != adsp->loop)
			adsp->pc++;

		/* handle looping */
		else
		{
			/* condition not met, keep looping */
			if (CONDITION(adsp, adsp->loop_condition))
				adsp->pc = pc_stack_top(adsp);

			/* condition met; pop the PC and loop stacks and fall through */
			else
			{
				loop_stack_pop(adsp);
				pc_stack_pop_val(adsp);
				adsp->pc++;
			}
		}

		/* parse the instruction */
		switch (op >> 16)
		{
			case 0x00:
				/* 00000000 00000000 00000000  NOP */
				break;
			case 0x01:
				/* 00000001 0xxxxxxx xxxxxxxx  dst = IO(x) */
				/* 00000001 1xxxxxxx xxxxxxxx  IO(x) = dst */
				/* ADSP-218x only */
				if (adsp->chip_type >= CHIP_TYPE_ADSP2181)
				{
					if ((op & 0x008000) == 0x000000)
						WRITE_REG(adsp, 0, op & 15, RWORD_IO(adsp, (op >> 4) & 0x7ff));
					else
						WWORD_IO(adsp, (op >> 4) & 0x7ff, READ_REG(adsp, 0, op & 15));
				}
				break;
			case 0x02:
				/* 00000010 0000xxxx xxxxxxxx  modify flag out */
				/* 00000010 10000000 00000000  idle */
				/* 00000010 10000000 0000xxxx  idle (n) */
				if (op & 0x008000)
				{
					adsp->idle = 1;
					adsp->icount = 0;
				}
				else
				{
					if (CONDITION(adsp, op & 15))
					{
						if (op & 0x020) adsp->flagout = 0;
						if (op & 0x010) adsp->flagout ^= 1;
						if (adsp->chip_type >= CHIP_TYPE_ADSP2101)
						{
							if (op & 0x080) adsp->fl0 = 0;
							if (op & 0x040) adsp->fl0 ^= 1;
							if (op & 0x200) adsp->fl1 = 0;
							if (op & 0x100) adsp->fl1 ^= 1;
							if (op & 0x800) adsp->fl2 = 0;
							if (op & 0x400) adsp->fl2 ^= 1;
						}
					}
				}
				break;
			case 0x03:
				/* 00000011 xxxxxxxx xxxxxxxx  call or jump on flag in */
				if (op & 0x000002)
				{
					if (adsp->flagin)
					{
						if (op & 0x000001)
							pc_stack_push(adsp);
						adsp->pc = ((op >> 4) & 0x0fff) | ((op << 10) & 0x3000);
					}
				}
				else
				{
					if (!adsp->flagin)
					{
						if (op & 0x000001)
							pc_stack_push(adsp);
						adsp->pc = ((op >> 4) & 0x0fff) | ((op << 10) & 0x3000);
					}
				}
				break;
			case 0x04:
				/* ADSP-217x/218x global interrupt control occupies two of
				 * the bits reserved by the older stack-control encoding:
				 * 0x040040 = DIS INTS, 0x040060 = ENA INTS (User's Manual
				 * pp. 15-90..91). It masks servicing without changing IMASK. */
				if ((op & 0x00ffff) == 0x0040)
				{
					adsp->interrupts_enabled = 0;
					break;
				}
				if ((op & 0x00ffff) == 0x0060)
				{
					adsp->interrupts_enabled = 1;
					check_irqs(adsp);
					break;
				}
				/* 00000100 00000000 000xxxxx  stack control */
				if (op & 0x000010) pc_stack_pop_val(adsp);
				if (op & 0x000008) loop_stack_pop(adsp);
				if (op & 0x000004) cntr_stack_pop(adsp);
				if (op & 0x000002)
				{
					if (op & 0x000001) stat_stack_pop(adsp);
					else stat_stack_push(adsp);
				}
				break;
			case 0x05:
				/* 00000101 00000000 00000000  saturate MR */
				if (GET_MV)
				{
					if (adsp->core.mr.mrx.mr2.u & 0x80)
						adsp->core.mr.mrx.mr2.u = 0xffff, adsp->core.mr.mrx.mr1.u = 0x8000, adsp->core.mr.mrx.mr0.u = 0x0000;
					else
						adsp->core.mr.mrx.mr2.u = 0x0000, adsp->core.mr.mrx.mr1.u = 0x7fff, adsp->core.mr.mrx.mr0.u = 0xffff;
					normalize_mr(adsp);
				}
				break;
			case 0x06:
				/* 00000110 000xxxxx 00000000  DIVS */
				{
					int xop = (op >> 8) & 7;
					int yop = (op >> 11) & 3;

					xop = ALU_GETXREG_UNSIGNED(adsp, xop);
					yop = ALU_GETYREG_UNSIGNED(adsp, yop);

					temp = xop ^ yop;
					adsp->astat = (adsp->astat & ~QFLAG) | ((temp >> 10) & QFLAG);
					adsp->core.af.u = (yop << 1) | (adsp->core.ay0.u >> 15);
					adsp->core.ay0.u = (adsp->core.ay0.u << 1) | (temp >> 15);
				}
				break;
			case 0x07:
				/* 00000111 00010xxx 00000000  DIVQ */
				{
					int xop = (op >> 8) & 7;
					int res;

					xop = ALU_GETXREG_UNSIGNED(adsp, xop);

					if (GET_Q)
						res = adsp->core.af.u + xop;
					else
						res = adsp->core.af.u - xop;

					temp = res ^ xop;
					adsp->astat = (adsp->astat & ~QFLAG) | ((temp >> 10) & QFLAG);
					adsp->core.af.u = ((UINT32)res << 1) | (adsp->core.ay0.u >> 15);
					adsp->core.ay0.u = (adsp->core.ay0.u << 1) | ((~temp >> 15) & 0x0001);
				}
				break;
			case 0x08:
				/* 00001000 00000000 0000xxxx  reserved */
				break;
			case 0x09:
				/* 00001001 00000000 000xxxxx  modify address register */
				temp = (op >> 2) & 4;
				modify_address(adsp, temp + ((op >> 2) & 3), temp + (op & 3));
				break;
			case 0x0a:
				/* 00001010 00000000 000xxxxx  conditional return */
				if (CONDITION(adsp, op & 15))
				{
					pc_stack_pop(adsp);

					/* RTI case */
					if (op & 0x000010)
						stat_stack_pop(adsp);
				}
				break;
			case 0x0b:
				/* 00001011 00000000 xxxxxxxx  conditional jump (indirect address) */
				if (CONDITION(adsp, op & 15))
				{
					if (op & 0x000010)
						pc_stack_push(adsp);
					adsp->pc = adsp->i[4 + ((op >> 6) & 3)] & 0x3fff;
				}
				break;
			case 0x0c:
				/* 00001100 xxxxxxxx xxxxxxxx  mode control */
				if (adsp->chip_type >= CHIP_TYPE_ADSP2101)
				{
					if (op & 0x000008) adsp->mstat = (adsp->mstat & ~MSTAT_GOMODE) | ((op << 5) & MSTAT_GOMODE);
					if (op & 0x002000) adsp->mstat = (adsp->mstat & ~MSTAT_INTEGER) | ((op >> 8) & MSTAT_INTEGER);
					if (op & 0x008000) adsp->mstat = (adsp->mstat & ~MSTAT_TIMER) | ((op >> 9) & MSTAT_TIMER);
				}
				if (op & 0x000020) adsp->mstat = (adsp->mstat & ~MSTAT_BANK) | ((op >> 4) & MSTAT_BANK);
				if (op & 0x000080) adsp->mstat = (adsp->mstat & ~MSTAT_REVERSE) | ((op >> 5) & MSTAT_REVERSE);
				if (op & 0x000200) adsp->mstat = (adsp->mstat & ~MSTAT_STICKYV) | ((op >> 6) & MSTAT_STICKYV);
				if (op & 0x000800) adsp->mstat = (adsp->mstat & ~MSTAT_SATURATE) | ((op >> 7) & MSTAT_SATURATE);
				update_mstat(adsp);
				break;
			case 0x0d:
				/* 00001101 0000xxxx xxxxxxxx  internal data move */
				WRITE_REG(adsp, (op >> 10) & 3, (op >> 4) & 15, READ_REG(adsp, (op >> 8) & 3, op & 15));
				break;
			case 0x0e:
				/* 00001110 0xxxxxxx xxxxxxxx  conditional shift */
				if (CONDITION(adsp, op & 15)) shift_op(adsp, op);
				break;
			case 0x0f:
				/* 00001111 0xxxxxxx xxxxxxxx  shift immediate */
				shift_op_imm(adsp, op);
				break;
			case 0x10:
				/* 00010000 0xxxxxxx xxxxxxxx  shift with internal data register move.
                 * Parallel-move sources are sampled before either destination
                 * is written.  This matters when the move reads SR while the
                 * shift writes SR (INFO PM 0x25fc). */
				temp = READ_REG(adsp, 0, op & 15);
				shift_op(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, temp);
				break;
			case 0x11:
				/* 00010001 xxxxxxxx xxxxxxxx  shift with pgm memory read/write */
				if (op & 0x8000)
				{
					pgm_write_dag2(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
					shift_op(adsp, op);
				}
				else
				{
					shift_op(adsp, op);
					WRITE_REG(adsp, 0, (op >> 4) & 15, pgm_read_dag2(adsp, op));
				}
				break;
			case 0x12:
				/* 00010010 xxxxxxxx xxxxxxxx  shift with data memory read/write DAG1 */
				if (op & 0x8000)
				{
					data_write_dag1(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
					shift_op(adsp, op);
				}
				else
				{
					shift_op(adsp, op);
					WRITE_REG(adsp, 0, (op >> 4) & 15, data_read_dag1(adsp, op));
				}
				break;
			case 0x13:
				/* 00010011 xxxxxxxx xxxxxxxx  shift with data memory read/write DAG2 */
				if (op & 0x8000)
				{
					data_write_dag2(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
					shift_op(adsp, op);
				}
				else
				{
					shift_op(adsp, op);
					WRITE_REG(adsp, 0, (op >> 4) & 15, data_read_dag2(adsp, op));
				}
				break;
			case 0x14: case 0x15: case 0x16: case 0x17:
				/* 000101xx xxxxxxxx xxxxxxxx  do until */
				loop_stack_push(adsp, op & 0x3ffff);
				pc_stack_push(adsp);
				break;
			case 0x18: case 0x19: case 0x1a: case 0x1b:
				/* 000110xx xxxxxxxx xxxxxxxx  conditional jump (immediate addr) */
				if (CONDITION(adsp, op & 15))
				{
					adsp->pc = (op >> 4) & 0x3fff;
					/* check for a busy loop */
					if (adsp->pc == adsp->ppc)
						adsp->icount = 0;
				}
				break;
			case 0x1c: case 0x1d: case 0x1e: case 0x1f:
				/* 000111xx xxxxxxxx xxxxxxxx  conditional call (immediate addr) */
				if (CONDITION(adsp, op & 15))
				{
					pc_stack_push(adsp);
					adsp->pc = (op >> 4) & 0x3fff;
				}
				break;
			case 0x20: case 0x21:
				/* 0010000x xxxxxxxx xxxxxxxx  conditional MAC to MR */
				if (CONDITION(adsp, op & 15))
				{
					if (adsp->chip_type >= CHIP_TYPE_ADSP2181 && (op & 0x0018f0) == 0x000010)
						mac_op_mr_xop(adsp, op);
					else
						mac_op_mr(adsp, op);
				}
				break;
			case 0x22: case 0x23:
				/* 0010001x xxxxxxxx xxxxxxxx  conditional ALU to AR */
				if (CONDITION(adsp, op & 15))
				{
					if (adsp->chip_type >= CHIP_TYPE_ADSP2181 && (op & 0x000010) == 0x000010)
						alu_op_ar_const(adsp, op);
					else
						alu_op_ar(adsp, op);
				}
				break;
			case 0x24: case 0x25:
				/* 0010010x xxxxxxxx xxxxxxxx  conditional MAC to MF */
				if (CONDITION(adsp, op & 15))
				{
					if (adsp->chip_type >= CHIP_TYPE_ADSP2181 && (op & 0x0018f0) == 0x000010)
						mac_op_mf_xop(adsp, op);
					else
						mac_op_mf(adsp, op);
				}
				break;
			case 0x26: case 0x27:
				/* 0010011x xxxxxxxx xxxxxxxx  conditional ALU to AF */
				if (CONDITION(adsp, op & 15))
				{
					if (adsp->chip_type >= CHIP_TYPE_ADSP2181 && (op & 0x000010) == 0x000010)
						alu_op_af_const(adsp, op);
					else
						alu_op_af(adsp, op);
				}
				break;
			case 0x28: case 0x29:
				/* 0010100x xxxxxxxx xxxxxxxx  MAC to MR with internal data register move */
				temp = READ_REG(adsp, 0, op & 15);
				mac_op_mr(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, temp);
				break;
			case 0x2a: case 0x2b:
				/* 0010101x xxxxxxxx xxxxxxxx  ALU to AR with internal data register move */
				if (adsp->chip_type >= CHIP_TYPE_ADSP2181 && (op & 0x0000ff) == 0x0000aa)
					alu_op_none(adsp, op);
				else
				{
					temp = READ_REG(adsp, 0, op & 15);
					alu_op_ar(adsp, op);
					WRITE_REG(adsp, 0, (op >> 4) & 15, temp);
				}
				break;
			case 0x2c: case 0x2d:
				/* 0010110x xxxxxxxx xxxxxxxx  MAC to MF with internal data register move */
				temp = READ_REG(adsp, 0, op & 15);
				mac_op_mf(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, temp);
				break;
			case 0x2e: case 0x2f:
				/* 0010111x xxxxxxxx xxxxxxxx  ALU to AF with internal data register move */
				temp = READ_REG(adsp, 0, op & 15);
				alu_op_af(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, temp);
				break;
			case 0x30: case 0x31: case 0x32: case 0x33:
				/* 001100xx xxxxxxxx xxxxxxxx  load non-data register immediate (group 0) */
				WRITE_REG(adsp, 0, op & 15, (INT32)(op << 14) >> 18);
				break;
			case 0x34: case 0x35: case 0x36: case 0x37:
				/* 001101xx xxxxxxxx xxxxxxxx  load non-data register immediate (group 1) */
				WRITE_REG(adsp, 1, op & 15, (INT32)(op << 14) >> 18);
				break;
			case 0x38: case 0x39: case 0x3a: case 0x3b:
				/* 001110xx xxxxxxxx xxxxxxxx  load non-data register immediate (group 2) */
				WRITE_REG(adsp, 2, op & 15, (INT32)(op << 14) >> 18);
				break;
			case 0x3c: case 0x3d: case 0x3e: case 0x3f:
				/* 001111xx xxxxxxxx xxxxxxxx  load non-data register immediate (group 3) */
				WRITE_REG(adsp, 3, op & 15, (INT32)(op << 14) >> 18);
				break;
			case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
			case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
				/* 0100xxxx xxxxxxxx xxxxxxxx  load data register immediate */
				WRITE_REG(adsp, 0, op & 15, (op >> 4) & 0xffff);
				break;
			case 0x50: case 0x51:
				/* 0101000x xxxxxxxx xxxxxxxx  MAC to MR with pgm memory read */
				mac_op_mr(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, pgm_read_dag2(adsp, op));
				break;
			case 0x52: case 0x53:
				/* 0101001x xxxxxxxx xxxxxxxx  ALU to AR with pgm memory read */
				alu_op_ar(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, pgm_read_dag2(adsp, op));
				break;
			case 0x54: case 0x55:
				/* 0101010x xxxxxxxx xxxxxxxx  MAC to MF with pgm memory read */
				mac_op_mf(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, pgm_read_dag2(adsp, op));
				break;
			case 0x56: case 0x57:
				/* 0101011x xxxxxxxx xxxxxxxx  ALU to AF with pgm memory read */
				alu_op_af(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, pgm_read_dag2(adsp, op));
				break;
			case 0x58: case 0x59:
				/* 0101100x xxxxxxxx xxxxxxxx  MAC to MR with pgm memory write */
				pgm_write_dag2(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
				mac_op_mr(adsp, op);
				break;
			case 0x5a: case 0x5b:
				/* 0101101x xxxxxxxx xxxxxxxx  ALU to AR with pgm memory write */
				pgm_write_dag2(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
				alu_op_ar(adsp, op);
				break;
			case 0x5c: case 0x5d:
				/* 0101110x xxxxxxxx xxxxxxxx  ALU to MR with pgm memory write */
				pgm_write_dag2(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
				mac_op_mf(adsp, op);
				break;
			case 0x5e: case 0x5f:
				/* 0101111x xxxxxxxx xxxxxxxx  ALU to MF with pgm memory write */
				pgm_write_dag2(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
				alu_op_af(adsp, op);
				break;
			case 0x60: case 0x61:
				/* 0110000x xxxxxxxx xxxxxxxx  MAC to MR with data memory read DAG1 */
				mac_op_mr(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, data_read_dag1(adsp, op));
				break;
			case 0x62: case 0x63:
				/* 0110001x xxxxxxxx xxxxxxxx  ALU to AR with data memory read DAG1 */
				alu_op_ar(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, data_read_dag1(adsp, op));
				break;
			case 0x64: case 0x65:
				/* 0110010x xxxxxxxx xxxxxxxx  MAC to MF with data memory read DAG1 */
				mac_op_mf(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, data_read_dag1(adsp, op));
				break;
			case 0x66: case 0x67:
				/* 0110011x xxxxxxxx xxxxxxxx  ALU to AF with data memory read DAG1 */
				alu_op_af(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, data_read_dag1(adsp, op));
				break;
			case 0x68: case 0x69:
				/* 0110100x xxxxxxxx xxxxxxxx  MAC to MR with data memory write DAG1 */
				data_write_dag1(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
				mac_op_mr(adsp, op);
				break;
			case 0x6a: case 0x6b:
				/* 0110101x xxxxxxxx xxxxxxxx  ALU to AR with data memory write DAG1 */
				data_write_dag1(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
				alu_op_ar(adsp, op);
				break;
			case 0x6c: case 0x6d:
				/* 0111110x xxxxxxxx xxxxxxxx  MAC to MF with data memory write DAG1 */
				data_write_dag1(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
				mac_op_mf(adsp, op);
				break;
			case 0x6e: case 0x6f:
				/* 0111111x xxxxxxxx xxxxxxxx  ALU to AF with data memory write DAG1 */
				data_write_dag1(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
				alu_op_af(adsp, op);
				break;
			case 0x70: case 0x71:
				/* 0111000x xxxxxxxx xxxxxxxx  MAC to MR with data memory read DAG2 */
				mac_op_mr(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, data_read_dag2(adsp, op));
				break;
			case 0x72: case 0x73:
				/* 0111001x xxxxxxxx xxxxxxxx  ALU to AR with data memory read DAG2 */
				alu_op_ar(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, data_read_dag2(adsp, op));
				break;
			case 0x74: case 0x75:
				/* 0111010x xxxxxxxx xxxxxxxx  MAC to MF with data memory read DAG2 */
				mac_op_mf(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, data_read_dag2(adsp, op));
				break;
			case 0x76: case 0x77:
				/* 0111011x xxxxxxxx xxxxxxxx  ALU to AF with data memory read DAG2 */
				alu_op_af(adsp, op);
				WRITE_REG(adsp, 0, (op >> 4) & 15, data_read_dag2(adsp, op));
				break;
			case 0x78: case 0x79:
				/* 0111100x xxxxxxxx xxxxxxxx  MAC to MR with data memory write DAG2 */
				data_write_dag2(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
				mac_op_mr(adsp, op);
				break;
			case 0x7a: case 0x7b:
				/* 0111101x xxxxxxxx xxxxxxxx  ALU to AR with data memory write DAG2 */
				data_write_dag2(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
				alu_op_ar(adsp, op);
				break;
			case 0x7c: case 0x7d:
				/* 0111110x xxxxxxxx xxxxxxxx  MAC to MF with data memory write DAG2 */
				data_write_dag2(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
				mac_op_mf(adsp, op);
				break;
			case 0x7e: case 0x7f:
				/* 0111111x xxxxxxxx xxxxxxxx  ALU to AF with data memory write DAG2 */
				data_write_dag2(adsp, op, READ_REG(adsp, 0, (op >> 4) & 15));
				alu_op_af(adsp, op);
				break;
			case 0x80: case 0x81: case 0x82: case 0x83:
				/* 100000xx xxxxxxxx xxxxxxxx  read data memory (immediate addr) to reg group 0 */
				WRITE_REG(adsp, 0, op & 15, RWORD_DATA(adsp, (op >> 4) & 0x3fff));
				break;
			case 0x84: case 0x85: case 0x86: case 0x87:
				/* 100001xx xxxxxxxx xxxxxxxx  read data memory (immediate addr) to reg group 1 */
				WRITE_REG(adsp, 1, op & 15, RWORD_DATA(adsp, (op >> 4) & 0x3fff));
				break;
			case 0x88: case 0x89: case 0x8a: case 0x8b:
				/* 100010xx xxxxxxxx xxxxxxxx  read data memory (immediate addr) to reg group 2 */
				WRITE_REG(adsp, 2, op & 15, RWORD_DATA(adsp, (op >> 4) & 0x3fff));
				break;
			case 0x8c: case 0x8d: case 0x8e: case 0x8f:
				/* 100011xx xxxxxxxx xxxxxxxx  read data memory (immediate addr) to reg group 3 */
				WRITE_REG(adsp, 3, op & 15, RWORD_DATA(adsp, (op >> 4) & 0x3fff));
				break;
			case 0x90: case 0x91: case 0x92: case 0x93:
				/* 1001xxxx xxxxxxxx xxxxxxxx  write data memory (immediate addr) from reg group 0 */
				WWORD_DATA(adsp, (op >> 4) & 0x3fff, READ_REG(adsp, 0, op & 15));
				break;
			case 0x94: case 0x95: case 0x96: case 0x97:
				/* 1001xxxx xxxxxxxx xxxxxxxx  write data memory (immediate addr) from reg group 1 */
				WWORD_DATA(adsp, (op >> 4) & 0x3fff, READ_REG(adsp, 1, op & 15));
				break;
			case 0x98: case 0x99: case 0x9a: case 0x9b:
				/* 1001xxxx xxxxxxxx xxxxxxxx  write data memory (immediate addr) from reg group 2 */
				WWORD_DATA(adsp, (op >> 4) & 0x3fff, READ_REG(adsp, 2, op & 15));
				break;
			case 0x9c: case 0x9d: case 0x9e: case 0x9f:
				/* 1001xxxx xxxxxxxx xxxxxxxx  write data memory (immediate addr) from reg group 3 */
				WWORD_DATA(adsp, (op >> 4) & 0x3fff, READ_REG(adsp, 3, op & 15));
				break;
			case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7:
			case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
				/* 1010xxxx xxxxxxxx xxxxxxxx  data memory write (immediate) DAG1 */
				data_write_dag1(adsp, op, (op >> 4) & 0xffff);
				break;
			case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
			case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
				/* 1011xxxx xxxxxxxx xxxxxxxx  data memory write (immediate) DAG2 */
				data_write_dag2(adsp, op, (op >> 4) & 0xffff);
				break;
			case 0xc0: case 0xc1:
				/* 1100000x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX0 & pgm read to AY0 */
				mac_op_mr(adsp, op);
				adsp->core.ax0.u = data_read_dag1(adsp, op);
				adsp->core.ay0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xc2: case 0xc3:
				/* 1100001x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX0 & pgm read to AY0 */
				alu_op_ar(adsp, op);
				adsp->core.ax0.u = data_read_dag1(adsp, op);
				adsp->core.ay0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xc4: case 0xc5:
				/* 1100010x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX1 & pgm read to AY0 */
				mac_op_mr(adsp, op);
				adsp->core.ax1.u = data_read_dag1(adsp, op);
				adsp->core.ay0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xc6: case 0xc7:
				/* 1100011x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX1 & pgm read to AY0 */
				alu_op_ar(adsp, op);
				adsp->core.ax1.u = data_read_dag1(adsp, op);
				adsp->core.ay0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xc8: case 0xc9:
				/* 1100100x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX0 & pgm read to AY0 */
				mac_op_mr(adsp, op);
				adsp->core.mx0.u = data_read_dag1(adsp, op);
				adsp->core.ay0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xca: case 0xcb:
				/* 1100101x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX0 & pgm read to AY0 */
				alu_op_ar(adsp, op);
				adsp->core.mx0.u = data_read_dag1(adsp, op);
				adsp->core.ay0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xcc: case 0xcd:
				/* 1100110x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX1 & pgm read to AY0 */
				mac_op_mr(adsp, op);
				adsp->core.mx1.u = data_read_dag1(adsp, op);
				adsp->core.ay0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xce: case 0xcf:
				/* 1100111x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX1 & pgm read to AY0 */
				alu_op_ar(adsp, op);
				adsp->core.mx1.u = data_read_dag1(adsp, op);
				adsp->core.ay0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xd0: case 0xd1:
				/* 1101000x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX0 & pgm read to AY1 */
				mac_op_mr(adsp, op);
				adsp->core.ax0.u = data_read_dag1(adsp, op);
				adsp->core.ay1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xd2: case 0xd3:
				/* 1101001x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX0 & pgm read to AY1 */
				alu_op_ar(adsp, op);
				adsp->core.ax0.u = data_read_dag1(adsp, op);
				adsp->core.ay1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xd4: case 0xd5:
				/* 1101010x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX1 & pgm read to AY1 */
				mac_op_mr(adsp, op);
				adsp->core.ax1.u = data_read_dag1(adsp, op);
				adsp->core.ay1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xd6: case 0xd7:
				/* 1101011x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX1 & pgm read to AY1 */
				alu_op_ar(adsp, op);
				adsp->core.ax1.u = data_read_dag1(adsp, op);
				adsp->core.ay1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xd8: case 0xd9:
				/* 1101100x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX0 & pgm read to AY1 */
				mac_op_mr(adsp, op);
				adsp->core.mx0.u = data_read_dag1(adsp, op);
				adsp->core.ay1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xda: case 0xdb:
				/* 1101101x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX0 & pgm read to AY1 */
				alu_op_ar(adsp, op);
				adsp->core.mx0.u = data_read_dag1(adsp, op);
				adsp->core.ay1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xdc: case 0xdd:
				/* 1101110x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX1 & pgm read to AY1 */
				mac_op_mr(adsp, op);
				adsp->core.mx1.u = data_read_dag1(adsp, op);
				adsp->core.ay1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xde: case 0xdf:
				/* 1101111x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX1 & pgm read to AY1 */
				alu_op_ar(adsp, op);
				adsp->core.mx1.u = data_read_dag1(adsp, op);
				adsp->core.ay1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xe0: case 0xe1:
				/* 1110000x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX0 & pgm read to MY0 */
				mac_op_mr(adsp, op);
				adsp->core.ax0.u = data_read_dag1(adsp, op);
				adsp->core.my0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xe2: case 0xe3:
				/* 1110001x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX0 & pgm read to MY0 */
				alu_op_ar(adsp, op);
				adsp->core.ax0.u = data_read_dag1(adsp, op);
				adsp->core.my0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xe4: case 0xe5:
				/* 1110010x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX1 & pgm read to MY0 */
				mac_op_mr(adsp, op);
				adsp->core.ax1.u = data_read_dag1(adsp, op);
				adsp->core.my0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xe6: case 0xe7:
				/* 1110011x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX1 & pgm read to MY0 */
				alu_op_ar(adsp, op);
				adsp->core.ax1.u = data_read_dag1(adsp, op);
				adsp->core.my0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xe8: case 0xe9:
				/* 1110100x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX0 & pgm read to MY0 */
				mac_op_mr(adsp, op);
				adsp->core.mx0.u = data_read_dag1(adsp, op);
				adsp->core.my0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xea: case 0xeb:
				/* 1110101x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX0 & pgm read to MY0 */
				alu_op_ar(adsp, op);
				adsp->core.mx0.u = data_read_dag1(adsp, op);
				adsp->core.my0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xec: case 0xed:
				/* 1110110x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX1 & pgm read to MY0 */
				mac_op_mr(adsp, op);
				adsp->core.mx1.u = data_read_dag1(adsp, op);
				adsp->core.my0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xee: case 0xef:
				/* 1110111x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX1 & pgm read to MY0 */
				alu_op_ar(adsp, op);
				adsp->core.mx1.u = data_read_dag1(adsp, op);
				adsp->core.my0.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xf0: case 0xf1:
				/* 1111000x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX0 & pgm read to MY1 */
				mac_op_mr(adsp, op);
				adsp->core.ax0.u = data_read_dag1(adsp, op);
				adsp->core.my1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xf2: case 0xf3:
				/* 1111001x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX0 & pgm read to MY1 */
				alu_op_ar(adsp, op);
				adsp->core.ax0.u = data_read_dag1(adsp, op);
				adsp->core.my1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xf4: case 0xf5:
				/* 1111010x xxxxxxxx xxxxxxxx  MAC to MR with data read to AX1 & pgm read to MY1 */
				mac_op_mr(adsp, op);
				adsp->core.ax1.u = data_read_dag1(adsp, op);
				adsp->core.my1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xf6: case 0xf7:
				/* 1111011x xxxxxxxx xxxxxxxx  ALU to AR with data read to AX1 & pgm read to MY1 */
				alu_op_ar(adsp, op);
				adsp->core.ax1.u = data_read_dag1(adsp, op);
				adsp->core.my1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xf8: case 0xf9:
				/* 1111100x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX0 & pgm read to MY1 */
				mac_op_mr(adsp, op);
				adsp->core.mx0.u = data_read_dag1(adsp, op);
				adsp->core.my1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xfa: case 0xfb:
				/* 1111101x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX0 & pgm read to MY1 */
				alu_op_ar(adsp, op);
				adsp->core.mx0.u = data_read_dag1(adsp, op);
				adsp->core.my1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xfc: case 0xfd:
				/* 1111110x xxxxxxxx xxxxxxxx  MAC to MR with data read to MX1 & pgm read to MY1 */
				mac_op_mr(adsp, op);
				adsp->core.mx1.u = data_read_dag1(adsp, op);
				adsp->core.my1.u = pgm_read_dag2(adsp, op >> 4);
				break;
			case 0xfe: case 0xff:
				/* 1111111x xxxxxxxx xxxxxxxx  ALU to AR with data read to MX1 & pgm read to MY1 */
				alu_op_ar(adsp, op);
				adsp->core.mx1.u = data_read_dag1(adsp, op);
				adsp->core.my1.u = pgm_read_dag2(adsp, op >> 4);
				break;
		}

		adsp->icount--;
		adsp->cycles++;
	} while (adsp->icount > 0);
}

static int create_tables(void)
{
    if (reverse_table) return 1;
    reverse_table = (UINT16 *)calloc(0x4000, sizeof(*reverse_table));
    mask_table = (UINT16 *)calloc(0x4000, sizeof(*mask_table));
    condition_table = (UINT8 *)calloc(0x1000, sizeof(*condition_table));
    if (!reverse_table || !mask_table || !condition_table) return 0;
    for (int i = 0; i < 0x4000; i++) {
        UINT16 r = 0;
        for (int b = 0; b < 14; b++) r |= ((i >> b) & 1) << (13-b);
        reverse_table[i] = r;
        if      (i > 0x2000) mask_table[i] = 0x0000;
        else if (i > 0x1000) mask_table[i] = 0x2000;
        else if (i > 0x0800) mask_table[i] = 0x3000;
        else if (i > 0x0400) mask_table[i] = 0x3800;
        else if (i > 0x0200) mask_table[i] = 0x3c00;
        else if (i > 0x0100) mask_table[i] = 0x3e00;
        else if (i > 0x0080) mask_table[i] = 0x3f00;
        else if (i > 0x0040) mask_table[i] = 0x3f80;
        else if (i > 0x0020) mask_table[i] = 0x3fc0;
        else if (i > 0x0010) mask_table[i] = 0x3fe0;
        else if (i > 0x0008) mask_table[i] = 0x3ff0;
        else if (i > 0x0004) mask_table[i] = 0x3ff8;
        else if (i > 0x0002) mask_table[i] = 0x3ffc;
        else if (i > 0x0001) mask_table[i] = 0x3ffe;
        else                 mask_table[i] = 0x3fff;
    }
    for (int i = 0; i < 0x100; i++) {
        int az = !!(i & ZFLAG), an = !!(i & NFLAG), av = !!(i & VFLAG);
        int ac = !!(i & CFLAG), mv = !!(i & MVFLAG), as = !!(i & SFLAG);
        condition_table[i | 0x000] = az;
        condition_table[i | 0x100] = !az;
        condition_table[i | 0x200] = !((an ^ av) | az);
        condition_table[i | 0x300] = (an ^ av) | az;
        condition_table[i | 0x400] = an ^ av;
        condition_table[i | 0x500] = !(an ^ av);
        condition_table[i | 0x600] = av;
        condition_table[i | 0x700] = !av;
        condition_table[i | 0x800] = ac;
        condition_table[i | 0x900] = !ac;
        condition_table[i | 0xa00] = as;
        condition_table[i | 0xb00] = !as;
        condition_table[i | 0xc00] = mv;
        condition_table[i | 0xd00] = !mv;
        condition_table[i | 0xf00] = 1;
    }
    return 1;
}

static void setup_register_maps(adsp2100_state *a)
{
#define P(arr,n,x) (arr)[n] = &(x)
    P(a->alu_xregs,0,a->core.ax0); P(a->alu_xregs,1,a->core.ax1); P(a->alu_xregs,2,a->core.ar);
    P(a->alu_xregs,3,a->core.mr.mrx.mr0); P(a->alu_xregs,4,a->core.mr.mrx.mr1); P(a->alu_xregs,5,a->core.mr.mrx.mr2);
    P(a->alu_xregs,6,a->core.sr.srx.sr0); P(a->alu_xregs,7,a->core.sr.srx.sr1);
    P(a->alu_yregs,0,a->core.ay0); P(a->alu_yregs,1,a->core.ay1); P(a->alu_yregs,2,a->core.af); P(a->alu_yregs,3,a->core.zero);
    P(a->mac_xregs,0,a->core.mx0); P(a->mac_xregs,1,a->core.mx1); P(a->mac_xregs,2,a->core.ar);
    P(a->mac_xregs,3,a->core.mr.mrx.mr0); P(a->mac_xregs,4,a->core.mr.mrx.mr1); P(a->mac_xregs,5,a->core.mr.mrx.mr2);
    P(a->mac_xregs,6,a->core.sr.srx.sr0); P(a->mac_xregs,7,a->core.sr.srx.sr1);
    P(a->mac_yregs,0,a->core.my0); P(a->mac_yregs,1,a->core.my1); P(a->mac_yregs,2,a->core.mf); P(a->mac_yregs,3,a->core.zero);
    P(a->shift_xregs,0,a->core.si); P(a->shift_xregs,1,a->core.si); P(a->shift_xregs,2,a->core.ar);
    P(a->shift_xregs,3,a->core.mr.mrx.mr0); P(a->shift_xregs,4,a->core.mr.mrx.mr1); P(a->shift_xregs,5,a->core.mr.mrx.mr2);
    P(a->shift_xregs,6,a->core.sr.srx.sr0); P(a->shift_xregs,7,a->core.sr.srx.sr1);
#undef P
}

adsp2181_t *adsp2181_create(void)
{
    adsp2100_state *a = (adsp2100_state *)calloc(1, sizeof(*a));
    if (!a || !create_tables()) { free(a); return NULL; }
    a->chip_type = CHIP_TYPE_ADSP2181; a->mstat_mask = 0x7f; a->imask_mask = 0x3ff;
    setup_register_maps(a); adsp2181_reset(a); return a;
}
void adsp2181_destroy(adsp2181_t *a) { free(a); }
void adsp2181_reset(adsp2181_t *a)
{
    a->core.zero.u = a->alt.zero.u = 0;
    wr_l0(a, a->l[0]); wr_i0(a, a->i[0]);
    wr_l1(a, a->l[1]); wr_i1(a, a->i[1]);
    wr_l2(a, a->l[2]); wr_i2(a, a->i[2]);
    wr_l3(a, a->l[3]); wr_i3(a, a->i[3]);
    wr_l4(a, a->l[4]); wr_i4(a, a->i[4]);
    wr_l5(a, a->l[5]); wr_i5(a, a->i[5]);
    wr_l6(a, a->l[6]); wr_i6(a, a->i[6]);
    wr_l7(a, a->l[7]); wr_i7(a, a->i[7]);
    a->pc=0; a->ppc=0xffffffff; a->cntr_valid=0; a->loop=0xffff; a->loop_condition=0;
    a->astat_clear=~(CFLAG|VFLAG|NFLAG|ZFLAG); a->mstat=0; a->sstat=0x55; a->idle=0;
    a->pmovlay=0; a->dmovlay=0;
    memset(a->sport_rx, 0, sizeof(a->sport_rx));
    memset(a->sport_tx, 0, sizeof(a->sport_tx));
    memset(a->sport_tx_written, 0, sizeof(a->sport_tx_written));
    a->stop_dm_armed = 0;
    a->stop_dm_hit = 0;
    a->stop_dm_group = 1;
    a->stop_dm_left = 1;
    a->latch_dm_armed = 0;
    a->latch_dm_have = 0;
    a->latch_dm_count = 0;
    a->yield_on_stop = 0;
    a->continue_non_idle = 0;
    update_mstat(a);
    a->pc_sp=a->cntr_sp=a->stat_sp=a->loop_sp=0; a->inject_valid=0; a->imask=0; a->icntl=0; a->interrupts_enabled=1;
    a->pcsp_window_min=0xff; a->pcsp_window_max=0; a->watch_gate=1;
    /* Per card, so a run that boots several reports each one's first
     * overflow rather than only the first card's. */
    a->stack_over_warned = 0;
    a->coverage_on = 1;
    memset(a->irq_state, 0, sizeof(a->irq_state));
    memset(a->irq_latch, 0, sizeof(a->irq_latch));
}
int adsp2181_run(adsp2181_t *a, int cycles) { if (!a || cycles<=0 || a->idma_boot_hold) return 0; a->icount=cycles; execute(a); return cycles-a->icount; }
/* Put the core in IDMA boot hold: it executes nothing until an IDMA write
 * commits a word to program memory location 0, which starts it at PM 0.
 * This is what keeps a DSP from running its own half-overwritten image
 * while the host streams a download into it. */
void adsp2181_set_idma_boot_hold(adsp2181_t *a, int on)
{
    if (!a) return;
    a->idma_boot_hold = on != 0;
    if (on) a->idma_boot_mode = 1;
}
int adsp2181_idma_boot_held(const adsp2181_t *a) { return a ? a->idma_boot_hold : 0; }
void adsp2181_set_callbacks(adsp2181_t *a, adsp2181_rx_cb r, adsp2181_tx_cb t, adsp2181_timer_cb f) { a->sport_rx_callback=r; a->sport_tx_callback=t; a->timer_fired=f; }
void adsp2181_set_memory_callbacks(adsp2181_t *a, adsp2181_dm_read_cb dr, adsp2181_dm_write_cb dw, adsp2181_io_read_cb ir, adsp2181_io_write_cb iw)
{
    if (!a) return;
    a->dm_read_callback = dr;
    a->dm_write_callback = dw;
    a->io_read_callback = ir;
    a->io_write_callback = iw;
}
uint32_t *adsp2181_pm(adsp2181_t *a) { return a->program; }
uint16_t *adsp2181_dm(adsp2181_t *a) { return a->data; }
uint16_t *adsp2181_io(adsp2181_t *a) { return a->io; }
uint16_t adsp2181_i(const adsp2181_t *a, int index) { return (a && index >= 0 && index < 8) ? (uint16_t)(a->i[index] & 0x3fff) : 0; }
int16_t adsp2181_m(const adsp2181_t *a, int index) { return (a && index >= 0 && index < 8) ? (int16_t)a->m[index] : 0; }
uint16_t adsp2181_l(const adsp2181_t *a, int index) { return (a && index >= 0 && index < 8) ? (uint16_t)(a->l[index] & 0x3fff) : 0; }
void adsp2181_set_i(adsp2181_t *a, int index, uint16_t value)
{
    if (!a || index < 0 || index >= 8) return;
    a->i[index] = value & 0x3fff;
    update_i(a, index);
}
void adsp2181_request_stop(adsp2181_t *a) { if (a) a->icount = 0; }
uint32_t *adsp2181_pm_overlay(adsp2181_t *a, int overlay)
{
    return a && overlay >= 1 && overlay <= 2 ? a->program_overlay[overlay - 1] : NULL;
}
uint16_t *adsp2181_dm_overlay(adsp2181_t *a, int overlay)
{
    return a && overlay >= 1 && overlay <= 2 ? a->data_overlay[overlay - 1] : NULL;
}
/* IDMA address bit 14 is the ADSP-2181 "destination type" the datasheet
 * describes ("a 14-bit address and 1-bit destination type"): 0 selects the
 * 24-bit program memory, 1 selects the 16-bit data memory.
 *
 * Three independent facts in the shipping Eicon firmware fix this polarity:
 *
 *  - the host-port helpers (te_dmlt.pm 0x80082950 write / 0x80082920 read)
 *    use the two-access form for addresses *below* 0x4000 and a single
 *    access at or above it (both use `bnel`/`beqz` on `addr < 0x4000`), and
 *    a 24-bit PM word is exactly what needs two 16-bit accesses.  The
 *    unconditional 24-bit accessors at 0x80082974 / 0x80082994 confirm the
 *    two-access form is `(hi << 8) | (lo & 0xff)`.
 *  - the symbol resolver (0x800a6204) adds 0x4000 when the target memory
 *    block's `memory_type & 1` is clear, and the combifile memory-block
 *    tables give type 0 to the DM blocks (kernel block 0 @0x0000, block 2
 *    @0x2f80) and type 1 to the PM blocks (block 1 @0x0900, block 3
 *    @0x0580).  So DM gets the flag, PM does not.
 *  - the same resolver's fixed-segment path adds 0x4000 for segments 0 and
 *    2 and not for 1 and 3, matching those blocks' DM/PM split.
 *
 * A previous revision had this inverted, which is why a single PM write
 * needed a commit-on-address-change workaround to make the DSP presence
 * check pass: the check writes DM, and DM needs no workaround.
 */
void adsp2181_idma_addr_write(adsp2181_t *a, uint16_t address)
{
    /* A PM word takes two data accesses.  If only the first *write* half
     * has arrived, commit it as value<<8 before the address changes; the
     * helper's second access supplies a zero pad byte anyway.  A dangling
     * read half carries no data and must not write anything back. */
    if (!(a->idma_addr & 0x4000) && a->idma_offs && a->idma_pending_write)
        WWORD_PGM(a, a->idma_addr & 0x3fff, (UINT32)a->idma_cache << 8);
    a->idma_addr = address; a->idma_offs = 0; a->idma_pending_write = 0;
}
uint16_t adsp2181_idma_addr_read(const adsp2181_t *a) { return a->idma_addr; }
void adsp2181_idma_data_write(adsp2181_t *a, uint16_t value)
{
    if (a->idma_addr & 0x4000) {
        WWORD_DATA(a, a->idma_addr++ & 0x3fff, value);
    } else if (!a->idma_offs) {
        a->idma_cache = value; a->idma_offs = 1; a->idma_pending_write = 1;
    } else {
        uint16_t pm_addr = a->idma_addr & 0x3fff;
        WWORD_PGM(a, pm_addr, (a->idma_cache << 8) | (value & 0xff));
        a->idma_addr++;
        a->idma_offs = 0; a->idma_pending_write = 0;
        /* IDMA boot (BMODE=1, MMAP=0): "Program execution is held off until
         * on-chip program memory location 0 is written to."  The Eicon
         * download streams the image from PM 0x0001 up and releases the core
         * with a final write to PM 0.
         *
         * Any other program-memory write re-arms the hold: it means a code
         * download is under way, and a core executing its own half-replaced
         * image corrupts the transfer and then runs wild.  Data memory is
         * left alone, so mailboxes and command rings can be written to a
         * running DSP as usual.
         *
         * Only cores put in IDMA boot mode behave this way; a core whose
         * image was staged directly (the single-DSP harnesses) keeps
         * running through host PM writes. */
        if (a->idma_boot_mode) {
            if (pm_addr == 0) {
                if (a->idma_boot_hold) {
                    a->idma_boot_hold = 0;
                    a->pc = 0; a->ppc = 0xffffffff;
                }
            } else {
                a->idma_boot_hold = 1;
            }
        }
    }
}
uint16_t adsp2181_idma_data_read(adsp2181_t *a)
{
    uint16_t result;
    if (a->idma_addr & 0x4000) {
        result = RWORD_DATA(a, a->idma_addr++ & 0x3fff);
    } else if (!a->idma_offs) {
        result = RWORD_PGM(a, a->idma_addr & 0x3fff) >> 8;
        a->idma_offs = 1; a->idma_pending_write = 0;
    } else {
        result = RWORD_PGM(a, a->idma_addr++ & 0x3fff) & 0xff;
        a->idma_offs = 0;
    }
    return result;
}
/* Whole-word host-port access with the Eicon helper semantics: bit 14 set
 * is one 16-bit DM access; bit 14 clear is a 24-bit PM word carried by two
 * accesses, of which the helper supplies the value first and a zero pad
 * byte second. */
void adsp2181_host_write(adsp2181_t *a, uint16_t addr, uint16_t value)
{
    if (addr & 0x4000)
        WWORD_DATA(a, addr & 0x3fff, value);
    else
        WWORD_PGM(a, addr & 0x3fff, (UINT32)value << 8);
}
uint16_t adsp2181_host_read(adsp2181_t *a, uint16_t addr)
{
    if (addr & 0x4000)
        return RWORD_DATA(a, addr & 0x3fff);
    return (uint16_t)(RWORD_PGM(a, addr & 0x3fff) >> 8);
}
/* Log only the first `limit` events (reads plus writes) on addr; 0 = no limit. */
void adsp2181_watch_dm_limited(adsp2181_t *a, uint16_t addr, uint32_t limit)
{
    if (a) {
        a->watch_dm[addr & 0x3fff] = 1;
        a->watch_dm_left[addr & 0x3fff] = limit;
        a->watch_dm_wonly[addr & 0x3fff] = 0;
        a->exec_history_enabled = 1;
    }
}

/* As above, but reads are neither logged nor charged: the instrument for
 * asserting that a range of DM is never written. */
void adsp2181_watch_dm_writes(adsp2181_t *a, uint16_t addr, uint32_t limit)
{
    if (a) {
        a->watch_dm[addr & 0x3fff] = 1;
        a->watch_dm_left[addr & 0x3fff] = limit;
        a->watch_dm_wonly[addr & 0x3fff] = 1;
        a->exec_history_enabled = 1;
    }
}

void adsp2181_watch_dm(adsp2181_t *a, uint16_t addr, int on)
{
    if (a) {
        a->watch_dm[addr & 0x3fff] = on != 0;
        a->watch_dm_left[addr & 0x3fff] = 0;
        a->watch_dm_wonly[addr & 0x3fff] = 0;
        if (on) a->exec_history_enabled = 1;
    }
}
void adsp2181_watch_pm(adsp2181_t *a, uint16_t addr, int on)
{
    if (a) a->watch_pm[addr & 0x3fff] = on != 0;
}

/* Min and max PC-stack depth since the last call, packed as (min<<8)|max, and
 * reset to the depth right now.  The current depth is folded in first, because
 * the primitives record the depth entering each push/pop and the last change in
 * a window would otherwise go unseen until the next one. */
/* Arm or disarm every watch at once.  See the watch_gate comment in the state
 * struct: a limit spent by an earlier page is the difference between "this does
 * not run here" and "you did not look here". */
void adsp2181_watch_gate(adsp2181_t *a, int on)
{
    if (a) a->watch_gate = on != 0;
}

uint32_t adsp2181_pcsp_window(adsp2181_t *a)
{
    unsigned lo, hi;
    if (!a) return 0;
    if (a->pc_sp > a->pcsp_window_max) a->pcsp_window_max = (UINT8)a->pc_sp;
    if (a->pc_sp < a->pcsp_window_min) a->pcsp_window_min = (UINT8)a->pc_sp;
    lo = a->pcsp_window_min;
    hi = a->pcsp_window_max;
    a->pcsp_window_min = a->pcsp_window_max = (UINT8)a->pc_sp;
    return (lo << 8) | hi;
}

/* Hold PM[addr] at `value` against DSP stores.  See the pin_pm comment in the
 * state struct: this is the counterfactual lever for a word the firmware
 * rewrites at run time, which EICON_FORCE_DM cannot reach. */
void adsp2181_pin_pm(adsp2181_t *a, uint16_t addr, uint32_t value, int on)
{
    if (!a) return;
    a->pin_pm[addr & 0x3fff] = on != 0;
    a->pin_pm_value[addr & 0x3fff] = value & 0xffffff;
    a->pin_pm_hits[addr & 0x3fff] = 0;
}

/* How many times a pin actually undid a store.  Zero means the A/B tested
 * nothing, which the caller has to be able to tell from a real null result. */
/* Hold DM[addr] at `value` against DSP stores.  See the pin_dm comment in the
 * state struct: EICON_FORCE_DM cannot reach a word written and read again
 * inside one 8 kHz frame, and this can. */
void adsp2181_pin_dm(adsp2181_t *a, uint16_t addr, uint16_t value, int on)
{
    if (!a) return;
    a->pin_dm[addr & 0x3fff] = on != 0;
    a->pin_dm_value[addr & 0x3fff] = value;
    a->pin_dm_hits[addr & 0x3fff] = 0;
}

uint32_t adsp2181_pin_dm_hits(const adsp2181_t *a, uint16_t addr)
{
    return a ? a->pin_dm_hits[addr & 0x3fff] : 0;
}

uint32_t adsp2181_pin_pm_hits(const adsp2181_t *a, uint16_t addr)
{
    return a ? a->pin_pm_hits[addr & 0x3fff] : 0;
}

/* Pace a continuously-running page by its own transmit publish rather than by
 * an instruction count: execute() returns as soon as `addr` is written.  See
 * the stop_dm_addr comment in the state struct for why page 8 needs it. */
void adsp2181_stop_on_dm_write(adsp2181_t *a, uint16_t addr, int on)
{
    adsp2181_stop_on_dm_write_n(a, addr, 1, on);
}

/* Stop after `group` writes to `addr` rather than after the first, so a
 * producer loop that emits a fixed group per pass is allowed to finish it. */
void adsp2181_stop_on_dm_write_n(adsp2181_t *a, uint16_t addr, int group,
                                 int on)
{
    if (a) {
        a->stop_dm_addr = addr & 0x3fff;
        a->stop_dm_group = group > 0 ? (uint16_t)group : 1;
        a->stop_dm_left = a->stop_dm_group;
        a->stop_dm_armed = on != 0;
        a->stop_dm_hit = 0;
    }
}

/* Arm the latch on `addr` and discard any value held from the previous tick.
 * Call once per sample before running the frame. */
void adsp2181_yield_on_stop(adsp2181_t *a, int on)
{
    if (a) a->yield_on_stop = on != 0;
}

void adsp2181_continue_non_idle(adsp2181_t *a, int on)
{
    if (a) a->continue_non_idle = on != 0;
}

void adsp2181_latch_dm_write(adsp2181_t *a, uint16_t addr, int on)
{
    if (a) {
        a->latch_dm_addr = addr & 0x3fff;
        a->latch_dm_armed = on != 0;
        a->latch_dm_have = 0;
        a->latch_dm_count = 0;
    }
}

/* How many times the latched word was written since it was armed. Zero and one
 * are both normal -- a quiet page publishes nothing, a healthy one publishes
 * once per tick. Anything above one means the caller is reading one sample out
 * of a group the page produced, and the difference is thrown away. */
uint32_t adsp2181_latched_dm_writes(const adsp2181_t *a)
{
    return a ? a->latch_dm_count : 0;
}

/* The first value written to the latched word since it was armed, or -1 if the
 * frame published nothing -- which is a real state, not an error: a page that
 * is deliberately quiet publishes nothing and the caller should hold. */
int32_t adsp2181_latched_dm_write(const adsp2181_t *a)
{
    if (!a || !a->latch_dm_have) return -1;
    return (int32_t)a->latch_dm_value;
}

/* Did the last run stop because the watched word was published?  0 means it
 * ran out of budget instead, which is the caller's signal that the page did
 * not produce a sample this tick. */
int adsp2181_stop_dm_hit(adsp2181_t *a)
{
    int hit = a ? a->stop_dm_hit : 0;
    if (a) a->stop_dm_hit = 0;
    return hit;
}

void adsp2181_watch_exec(adsp2181_t *a, uint16_t addr, int on)
{
    if (a) {
        a->watch_exec[addr & 0x3fff] = on != 0;
        a->watch_exec_left[addr & 0x3fff] = 0;
        if (on) a->exec_history_enabled = 1;
    }
}

/* Watch an address for its first `limit` executions only.  limit == 0 is the
 * same as adsp2181_watch_exec(cpu, addr, 1): log every one. */
void adsp2181_watch_exec_limited(adsp2181_t *a, uint16_t addr, uint32_t limit)
{
    if (a) {
        a->watch_exec[addr & 0x3fff] = 1;
        a->watch_exec_left[addr & 0x3fff] = limit;
        a->exec_history_enabled = 1;
    }
}

void adsp2181_watch_irqs(adsp2181_t *a, int on)
{
    if (a) a->watch_irqs = on != 0;
}

/* The PMOVLAY/DMOVLAY page selectors.  Written by the firmware through the
 * register map (2100ops.inc wr_pmovlay/wr_dmovlay), so a caller that wants
 * to know which page a PM address above 0x2000 resolved to has to read them
 * at the same instant as the fetch, not afterwards. */
/* Did the firmware drive the SPORT0 transmit latch during the last
 * adsp2181_sport0_tdm_frame()?  A caller acting as the line side needs this
 * to tell a real transmit sample from a stale latch left by an earlier page. */
int adsp2181_sport0_tx_written(const adsp2181_t *a)
{
    return a ? a->sport_tx_written[0] != 0 : 0;
}

uint32_t adsp2181_sport0_tx_writes(const adsp2181_t *a)
{
    return a ? a->sport_tx_written[0] : 0;
}

uint16_t adsp2181_pmovlay(const adsp2181_t *a) { return a ? a->pmovlay : 0; }
uint16_t adsp2181_dmovlay(const adsp2181_t *a) { return a ? a->dmovlay : 0; }

/* Read a PM word the way the core would right now: resolved through the
 * current PMOVLAY, not out of the resident image. */
uint32_t adsp2181_read_pm(adsp2181_t *a, uint16_t addr)
{
    return a ? RWORD_PGM(a, addr) : 0;
}
uint64_t adsp2181_cycles(const adsp2181_t *a) { return a ? a->cycles : 0; }

#define SPORT_SNAPSHOT_WORDS 21

static void take_sport_snapshot(const adsp2181_t *a, UINT32 *out)
{
    UINT32 state = 0, latch = 0;
    for (unsigned n = 0; n < ADSP2181_IRQ_COUNT; ++n) {
        state |= (a->irq_state[n] != 0) << n;
        latch |= (a->irq_latch[n] != 0) << n;
    }
    out[0] = a->pc & 0x3fff;
    out[1] = a->ppc & 0x3fff;
    out[2] = a->idle != 0;
    out[3] = (UINT32)a->cycles;
    out[4] = (UINT32)(a->cycles >> 32);
    out[5] = state;
    out[6] = latch;
    out[7] = a->imask;
    out[8] = a->icntl;
    out[9] = a->interrupts_enabled != 0;
    out[10] = (UINT32)a->pc_sp;
    out[11] = (UINT32)a->stat_sp;
    out[12] = (UINT32)a->loop_sp;
    out[13] = (UINT32)a->cntr_sp;
    out[14] = a->astat;
    out[15] = a->mstat;
    out[16] = a->sstat;
    out[17] = (UINT32)a->icount;
    out[18] = a->sport_rx[0];
    out[19] = a->sport_tx[0];
    out[20] = a->sport_tx_written[0] != 0;
}

int adsp2181_sport_snapshot(const adsp2181_t *a, int after,
                            UINT32 *out, unsigned words)
{
    if (!a || !out || words < SPORT_SNAPSHOT_WORDS)
        return 0;
    memcpy(out, after ? a->sport_return_snapshot : a->sport_entry_snapshot,
           SPORT_SNAPSHOT_WORDS * sizeof(*out));
    return SPORT_SNAPSHOT_WORDS;
}
void adsp2181_coverage_clear(adsp2181_t *a)
{
    if (a) memset(a->coverage, 0, sizeof(a->coverage));
}
void adsp2181_coverage_gate(adsp2181_t *a, int on)
{
    if (a) a->coverage_on = on != 0;
}
uint64_t adsp2181_coverage_count(const adsp2181_t *a, uint16_t pc)
{
    return a ? a->coverage[pc & 0x3fff] : 0;
}
void adsp2181_dm_census(adsp2181_t *a, int on)
{
    if (a) a->dm_census_on = on != 0;
}
void adsp2181_dm_census_clear(adsp2181_t *a)
{
    if (a) memset(a->dm_census, 0, sizeof(a->dm_census));
}
uint64_t adsp2181_dm_census_count(const adsp2181_t *a, uint16_t addr)
{
    return a ? a->dm_census[addr & 0x3fff] : 0;
}
void adsp2181_trace_budget(adsp2181_t *a, int64_t n) { if (a) a->trace_budget = n; }
uint16_t adsp2181_pc(const adsp2181_t *a) { return a->pc & 0x3fff; }
void adsp2181_set_pc(adsp2181_t *a, uint16_t pc) { a->pc = pc & 0x3fff; a->idle = 0; }
static void discard_stale_synthetic_returns(adsp2181_t *a,
                                            uint16_t return_pc)
{
    /* Driver-injected service calls use the resident IDLE instruction as a
     * synthetic return address. Firmware reached through those entries does
     * not always execute RTS back to it -- the task yields by jumping to the
     * kernel service slot, and the kernel idles there -- so every frame the
     * injected call leaves its own sentinel plus whatever the firmware pushed
     * on top of it. The Analog task does exactly this: two entries a frame
     * leak two words, the 16-word PC stack saturates within ~25 frames, and
     * from then on pushes are dropped and DO loops return through unrelated
     * callers (PM 0x1749, 0x177a, 0x3f79 all overflowed this way).
     *
     * Those frames exist only because the call was synthesised, so unwind
     * them: if the core is idle and the sentinel this function pushed last
     * time is still sitting at the depth it was pushed at, the whole call was
     * abandoned and everything from there up is dead. The loop, counter and
     * status stacks are unwound with it, since a DO loop or CNTR the
     * abandoned path pushed is dead for the same reason. This is the
     * across-calls form of the save/restore adsp2181_sport1_frame() already
     * does around its own injected continuation.
     *
     * The guard is deliberately narrow: nothing happens unless the core
     * idled, and nothing happens if the firmware unwound past that depth by
     * itself, which is the ordinary case and what the PRI task does. */
    if (!a->idle || !a->inject_valid)
        return;
    if (a->inject_pc_sp < 0 || a->pc_sp <= a->inject_pc_sp)
        return;
    if ((a->pc_stack[a->inject_pc_sp] & 0x3fff) != (return_pc & 0x3fff))
        return;
    a->pc_sp = a->inject_pc_sp;
    if (a->cntr_sp > a->inject_cntr_sp) a->cntr_sp = a->inject_cntr_sp;
    if (a->stat_sp > a->inject_stat_sp) a->stat_sp = a->inject_stat_sp;
    if (a->loop_sp > a->inject_loop_sp) a->loop_sp = a->inject_loop_sp;
    if (a->pc_sp == 0) a->sstat |= PC_EMPTY;
    a->sstat &= ~PC_OVER;
    a->stack_over_warned = 0;
}

void adsp2181_call(adsp2181_t *a, uint16_t entry, uint16_t return_pc)
{
    if (!a) return;
    discard_stale_synthetic_returns(a, return_pc);
    a->inject_pc_sp = a->pc_sp;
    a->inject_cntr_sp = a->cntr_sp;
    a->inject_stat_sp = a->stat_sp;
    a->inject_loop_sp = a->loop_sp;
    a->inject_return = return_pc & 0x3fff;
    a->inject_valid = 1;
    pc_stack_push_val(a, return_pc & 0x3fff);
    a->pc = entry & 0x3fff;
    a->idle = 0;
}
void adsp2181_set_irq(adsp2181_t *a, int irq, int asserted)
{
    if (!a || irq < 0 || irq >= ADSP2181_IRQ_COUNT) return;
    if (asserted && !a->irq_state[irq]) {
        a->irq_latch[irq] = 1;
        if (a->sport_rx_callback) {
            if (irq == ADSP2181_SPORT0_RX)
                a->sport_rx[0] = (UINT16)a->sport_rx_callback(a, 0);
            else if (irq == ADSP2181_SPORT1_RX ||
                     irq == ADSP2181_SPORT1_TX)
                /* ADSP-2181 SPORT1 uses one autobuffer interrupt for the
                 * simultaneous RX/TX frame.  Board firmware commonly enables
                 * the TX alias while its ISR reads RX1 before writing TX1. */
                a->sport_rx[1] = (UINT16)a->sport_rx_callback(a, 1);
        }
    }
    a->irq_state[irq] = asserted != 0;
    check_irqs(a);
}

uint32_t adsp2181_sport1_frame(adsp2181_t *a, uint16_t receive_word,
                               int cycles)
{
    if (!a || cycles <= 0) return 0;
    /* SPORT1 RX/TX share the IRQ1 vector in the 2181. Load the receive shift
     * register and latch one edge, then report only writes made by this ISR;
     * the TX latch itself intentionally retains the preceding frame. */
    a->sport_rx[1] = receive_word;
    a->sport_tx_written[1] = 0;
    adsp2181_set_irq(a, ADSP2181_SPORT1_TX, 1);
    adsp2181_set_irq(a, ADSP2181_SPORT1_TX, 0);
    adsp2181_run(a, cycles);
    return (a->sport_tx[1] & 0xffff) |
           (a->sport_tx_written[1] ? 0x10000u : 0);
}

uint16_t adsp2181_sport0_tdm_frame(adsp2181_t *a, int active_slot,
                                   int dispatch_slot, uint16_t active_word,
                                   uint16_t idle_word, int cycles_per_slot)
{
    if (!a || active_slot < 0 || active_slot >= 32 || dispatch_slot < 0 ||
        dispatch_slot >= 32 || cycles_per_slot <= 0)
        return 0;
    /* PM 02b9 is the kernel foreground dispatch slot. TIKRNL patches it to
     * CALL its continuation. Keep that call only on the assigned PRI
     * timeslot; the other 31 slots run the ordinary empty host dispatcher. */
    const UINT32 task_dispatch = RWORD_PGM(a, 0x02b9);
    const UINT32 task_isr = RWORD_PGM(a, 0x00b5);
    uint16_t selected_tx = 0;
    /* The closed MIPS channel assignment filters the 32-slot TDM stream
     * before this task dispatch: one call receives one 8 kHz slot, not 32
     * task invocations. Model that selected descriptor directly. */
    WWORD_PGM(a, 0x02b9, task_dispatch);
    WWORD_PGM(a, 0x00b5, task_isr);
    a->sport_rx[0] = active_word;
    a->sport_tx_written[0] = 0;
    /* The compatibility control below invokes only the selected descriptor and
     * duplicates its word across the kernel's 64-word TDM history. A real PRI
     * frame presents 31 idle slots before the selected slot comes around
     * again. Exercise that chronology as an explicit A/B until it is qualified
     * against the private descriptor. Ordering the selected slot last lets the
     * existing publication counter continue to describe that slot only. */
    static int full_tdm = -1;
    if (full_tdm < 0)
        full_tdm = getenv("EICON_SPORT_FULL_TDM") != NULL;
    if (full_tdm) {
        for (int pass = 1; pass <= 32; ++pass) {
            int slot = (dispatch_slot + pass) & 31;
            int selected = slot == dispatch_slot;
            WWORD_PGM(a, 0x02b9, selected ? task_dispatch : 0x000000);
            /* PM 0x00b5 is arithmetic inside the resident SPORT ISR, not the
             * selected foreground call. It must execute on every timeslot. */
            WWORD_PGM(a, 0x00b5, task_isr);
            a->sport_rx[0] = slot == active_slot ? active_word : idle_word;
            if (selected) {
                a->sport_tx_written[0] = 0;
                take_sport_snapshot(a, a->sport_entry_snapshot);
            }
            a->irq_latch[ADSP2181_SPORT0_RX] = 1;
            a->irq_state[ADSP2181_SPORT0_RX] = 1;
            check_irqs(a);
            a->irq_state[ADSP2181_SPORT0_RX] = 0;
            a->icount = cycles_per_slot;
            execute(a);
            if (selected) {
                take_sport_snapshot(a, a->sport_return_snapshot);
                selected_tx = a->sport_tx[0];
            }
        }
    } else {
        for (UINT16 address = 0x2e00; address < 0x2e40; ++address)
            WWORD_DATA(a, address, active_word);
        take_sport_snapshot(a, a->sport_entry_snapshot);
        a->irq_latch[ADSP2181_SPORT0_RX] = 1;
        a->irq_state[ADSP2181_SPORT0_RX] = 1;
        check_irqs(a);
        a->irq_state[ADSP2181_SPORT0_RX] = 0;
        a->icount = cycles_per_slot;
        execute(a);
        take_sport_snapshot(a, a->sport_return_snapshot);
        selected_tx = a->sport_tx[0];
    }
    WWORD_PGM(a, 0x02b9, task_dispatch);
    WWORD_PGM(a, 0x00b5, task_isr);
    return selected_tx;
}

uint16_t adsp2181_modem_sample(adsp2181_t *a, uint16_t active_word,
                               uint16_t idle_word, int cycles_per_pass,
                               uint16_t continuation, uint16_t return_pc)
{
    uint16_t tx = adsp2181_sport0_tdm_frame(
        a, 0, 0, active_word, idle_word, cycles_per_pass);
    if (!a)
        return tx;
    if (a->idle) {
        discard_stale_synthetic_returns(a, return_pc);
        pc_stack_push_val(a, return_pc & 0x3fff);
        a->pc = continuation & 0x3fff;
        a->idle = 0;
        a->icount = cycles_per_pass;
        execute(a);
    } else if ((a->yield_on_stop && a->stop_dm_hit) || a->continue_non_idle) {
        /* Two ways to get here, one mechanism.
         *
         * The frame stopped mid-page at the transmit publish. Run the
         * continuation anyway, then put the core back where it stopped: the
         * next sample's SPORT interrupt is taken on top of the page's own
         * foreground and returns into it, which is what the hardware does.
         * Disarm the stop across the continuation so it cannot re-trigger
         * inside it and leave the core somewhere unrelated.
         *
         * Or the frame simply did not finish inside its allowance and the core
         * is still in the page's foreground. Hardware does not care: SPORT is
         * an interrupt and it lands whatever the foreground is doing. Skipping
         * the continuation there is Session 165's blocker -- the page is never
         * dispatched again, so it stays in whichever routine it was suspended
         * in for the rest of the call, which is what V.34 does at 0x00b0 and
         * what V.32 does with its echo canceller (Session 187). Same treatment:
         * inject, then restore. Session 188.
         *
         * The continuation is an ordinary call, not an interrupt, so it runs
         * with the page's registers live and would corrupt the computation it
         * interrupted. Save and restore everything volatile around it; only
         * memory and the instrumentation counters are shared, which is what the
         * two halves are supposed to communicate through. */
        UINT16 resume = a->pc;
        UINT8 armed = a->stop_dm_armed;
        static int cni_trace = -1;
        if (cni_trace < 0)
            cni_trace = getenv("EICON_CNI_TRACE") != NULL;
        if (cni_trace) {
            static unsigned long n = 0;
            if ((n++ % 8000) == 0)
                fprintf(stderr, "[cni] injection %lu at pc=%04x cont=%04x "
                        "idle=%d hit=%d\n", n, resume, continuation,
                        a->idle, a->stop_dm_hit);
        }
        ADSPCORE saved_core = a->core, saved_alt = a->alt;
        UINT32 saved_i[8], saved_l[8], saved_lmask[8], saved_base[8];
        INT32 saved_m[8];
        UINT32 saved_loop = a->loop, saved_cond = a->loop_condition;
        UINT32 saved_cntr = a->cntr, saved_astat = a->astat;
        UINT32 saved_sstat = a->sstat, saved_mstat = a->mstat;
        UINT32 saved_mstat_prev = a->mstat_prev, saved_ppc = a->ppc;
        UINT8 saved_cntr_valid = a->cntr_valid, saved_px = a->px;
        UINT32 saved_loop_stack[LOOP_STACK_DEPTH];
        UINT32 saved_cntr_stack[CNTR_STACK_DEPTH];
        UINT32 saved_pc_stack[PC_STACK_DEPTH];
        UINT16 saved_stat_stack[STAT_STACK_DEPTH][3];
        INT32 saved_pc_sp = a->pc_sp, saved_cntr_sp = a->cntr_sp;
        INT32 saved_stat_sp = a->stat_sp, saved_loop_sp = a->loop_sp;
        memcpy(saved_i, a->i, sizeof saved_i);
        memcpy(saved_m, a->m, sizeof saved_m);
        memcpy(saved_l, a->l, sizeof saved_l);
        memcpy(saved_lmask, a->lmask, sizeof saved_lmask);
        memcpy(saved_base, a->base, sizeof saved_base);
        memcpy(saved_loop_stack, a->loop_stack, sizeof saved_loop_stack);
        memcpy(saved_cntr_stack, a->cntr_stack, sizeof saved_cntr_stack);
        memcpy(saved_pc_stack, a->pc_stack, sizeof saved_pc_stack);
        memcpy(saved_stat_stack, a->stat_stack, sizeof saved_stat_stack);

        a->stop_dm_armed = 0;
        pc_stack_push_val(a, return_pc & 0x3fff);
        a->pc = continuation & 0x3fff;
        a->icount = cycles_per_pass;
        execute(a);

        a->core = saved_core; a->alt = saved_alt;
        memcpy(a->i, saved_i, sizeof saved_i);
        memcpy(a->m, saved_m, sizeof saved_m);
        memcpy(a->l, saved_l, sizeof saved_l);
        memcpy(a->lmask, saved_lmask, sizeof saved_lmask);
        memcpy(a->base, saved_base, sizeof saved_base);
        memcpy(a->loop_stack, saved_loop_stack, sizeof saved_loop_stack);
        memcpy(a->cntr_stack, saved_cntr_stack, sizeof saved_cntr_stack);
        memcpy(a->pc_stack, saved_pc_stack, sizeof saved_pc_stack);
        memcpy(a->stat_stack, saved_stat_stack, sizeof saved_stat_stack);
        a->loop = saved_loop; a->loop_condition = saved_cond;
        a->cntr = saved_cntr; a->cntr_valid = saved_cntr_valid;
        a->astat = saved_astat; a->sstat = saved_sstat;
        a->mstat = saved_mstat; a->mstat_prev = saved_mstat_prev;
        a->ppc = saved_ppc; a->px = saved_px;
        a->pc_sp = saved_pc_sp; a->cntr_sp = saved_cntr_sp;
        a->stat_sp = saved_stat_sp; a->loop_sp = saved_loop_sp;
        a->stop_dm_armed = armed;
        a->pc = resume;
        a->idle = 0;
    }
    return tx;
}

int adsp2181_g711_encode_block(adsp2181_t *a, const int16_t *samples,
                               uint8_t *codes, size_t count,
                               uint16_t entry, uint16_t return_pc,
                               int cycles_per_sample)
{
    if (!a || !samples || !codes || cycles_per_sample <= 0 ||
        a->idma_boot_hold)
        return -1;
    for (size_t i = 0; i < count; ++i) {
        a->core.ar.u = (uint16_t)samples[i];
        pc_stack_push_val(a, return_pc & 0x3fff);
        a->pc = entry & 0x3fff;
        a->idle = 0;
        a->icount = cycles_per_sample;
        execute(a);
        /* PM 0x1810 returns serial-wire bit order in SR1. Reverse it to the
         * conventional G.711 RTP octet, exactly as the scalar caller. */
        uint8_t value = (uint8_t)a->core.sr.srx.sr1.u;
        value = (uint8_t)(((value & 0x55u) << 1) | ((value >> 1) & 0x55u));
        value = (uint8_t)(((value & 0x33u) << 2) | ((value >> 2) & 0x33u));
        codes[i] = (uint8_t)((value << 4) | (value >> 4));
    }
    return 0;
}

uint16_t adsp2181_imask(const adsp2181_t *a) { return a->imask; }
void adsp2181_set_imask(adsp2181_t *a, uint16_t imask) { if (a) a->imask = imask & 0x3ff; }
void adsp2181_set_flagin(adsp2181_t *a, int asserted) { if (a) a->flagin = asserted ? 1 : 0; }
int adsp2181_flagin(const adsp2181_t *a) { return a ? a->flagin : 0; }
uint16_t adsp2181_icntl(const adsp2181_t *a) { return a->icntl; }
int adsp2181_idle(const adsp2181_t *a) { return a->idle != 0; }
void adsp2181_set_ar(adsp2181_t *a, uint16_t value)
{
    if (a) a->core.ar.u = value;
}
void adsp2181_set_sr1(adsp2181_t *a, uint16_t value)
{
    if (a) a->core.sr.srx.sr1.u = value;
}
uint16_t adsp2181_sr0(const adsp2181_t *a)
{
    return a ? a->core.sr.srx.sr0.u : 0;
}
uint16_t adsp2181_sr1(const adsp2181_t *a)
{
    return a ? a->core.sr.srx.sr1.u : 0;
}
