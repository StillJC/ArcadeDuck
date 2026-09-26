// license:BSD-3-Clause
// Based on MAME's ADSP-21xx core, copyright Aaron Giles.
#ifndef EICON_ADSP2181_CORE_H
#define EICON_ADSP2181_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct adsp2181 adsp2181_t;
typedef int32_t (*adsp2181_rx_cb)(adsp2181_t *, int port);
typedef void (*adsp2181_tx_cb)(adsp2181_t *, int port, int32_t value);
typedef void (*adsp2181_timer_cb)(adsp2181_t *, int enabled);
typedef uint16_t (*adsp2181_dm_read_cb)(adsp2181_t *, uint16_t address, uint16_t fallback);
typedef void (*adsp2181_dm_write_cb)(adsp2181_t *, uint16_t address, uint16_t value);
typedef uint16_t (*adsp2181_io_read_cb)(adsp2181_t *, uint16_t address, uint16_t fallback);
typedef void (*adsp2181_io_write_cb)(adsp2181_t *, uint16_t address, uint16_t value);

enum {
    ADSP2181_IRQ0 = 0,
    ADSP2181_SPORT1_RX = 0,
    ADSP2181_IRQ1 = 1,
    ADSP2181_SPORT1_TX = 1,
    ADSP2181_IRQ2 = 2,
    ADSP2181_SPORT0_RX = 3,
    ADSP2181_SPORT0_TX = 4,
    ADSP2181_TIMER = 5,
    ADSP2181_IRQE = 6,
    ADSP2181_BDMA = 7,
    ADSP2181_IRQL1 = 8,
    ADSP2181_IRQL0 = 9,
    ADSP2181_IRQ_COUNT = 10
};

adsp2181_t *adsp2181_create(void);
void adsp2181_destroy(adsp2181_t *cpu);
void adsp2181_reset(adsp2181_t *cpu);
int adsp2181_run(adsp2181_t *cpu, int cycles);
uint16_t adsp2181_sport0_tdm_frame(adsp2181_t *cpu, int active_slot,
                                   int dispatch_slot, uint16_t active_word,
                                   uint16_t idle_word, int cycles_per_slot);
/* One simultaneous SPORT1 receive/transmit frame. Low 16 bits are TX1 and
 * bit 16 says firmware wrote TX1 during this frame. */
uint32_t adsp2181_sport1_frame(adsp2181_t *cpu, uint16_t receive_word,
                               int cycles);
/* Selected-channel SPORT frame followed, when the ISR yielded, by the modem
 * task's no-host continuation. Combining these avoids three FFI crossings per
 * 8 kHz sample without changing either execution budget or sample count. */
uint16_t adsp2181_modem_sample(adsp2181_t *cpu, uint16_t active_word,
                               uint16_t idle_word, int cycles_per_pass,
                               uint16_t continuation, uint16_t return_pc);
/* Run the resident firmware G.711 encoder for a block in one host call. This
 * models the hardware SPORT compander without changing sample accounting. */
int adsp2181_g711_encode_block(adsp2181_t *cpu, const int16_t *samples,
                               uint8_t *codes, size_t count,
                               uint16_t entry, uint16_t return_pc,
                               int cycles_per_sample);
/* IDMA boot hold (BMODE=1, MMAP=0): the core executes nothing until an IDMA
 * write commits program memory location 0, then starts at PM 0. */
void adsp2181_set_idma_boot_hold(adsp2181_t *cpu, int on);
int adsp2181_idma_boot_held(const adsp2181_t *cpu);
void adsp2181_set_callbacks(adsp2181_t *cpu, adsp2181_rx_cb rx,
                            adsp2181_tx_cb tx, adsp2181_timer_cb timer);
void adsp2181_set_memory_callbacks(adsp2181_t *cpu,
                                    adsp2181_dm_read_cb dm_read,
                                    adsp2181_dm_write_cb dm_write,
                                    adsp2181_io_read_cb io_read,
                                    adsp2181_io_write_cb io_write);
uint32_t *adsp2181_pm(adsp2181_t *cpu);
uint16_t *adsp2181_dm(adsp2181_t *cpu);
uint16_t *adsp2181_io(adsp2181_t *cpu);
uint16_t adsp2181_i(const adsp2181_t *cpu, int index);
int16_t adsp2181_m(const adsp2181_t *cpu, int index);
uint16_t adsp2181_l(const adsp2181_t *cpu, int index);
void adsp2181_set_i(adsp2181_t *cpu, int index, uint16_t value);
void adsp2181_request_stop(adsp2181_t *cpu);
uint32_t *adsp2181_pm_overlay(adsp2181_t *cpu, int overlay);
uint16_t *adsp2181_dm_overlay(adsp2181_t *cpu, int overlay);
void adsp2181_idma_addr_write(adsp2181_t *cpu, uint16_t address);
uint16_t adsp2181_idma_addr_read(const adsp2181_t *cpu);
void adsp2181_idma_data_write(adsp2181_t *cpu, uint16_t value);
uint16_t adsp2181_idma_data_read(adsp2181_t *cpu);
void adsp2181_host_write(adsp2181_t *cpu, uint16_t addr, uint16_t value);
uint16_t adsp2181_host_read(adsp2181_t *cpu, uint16_t addr);
void adsp2181_watch_dm(adsp2181_t *cpu, uint16_t addr, int on);
/* Log only the first `limit` events (reads plus writes) on addr; 0 = no limit.
 * Needed for addresses a hung loop sweeps millions of times. */
void adsp2181_watch_dm_limited(adsp2181_t *cpu, uint16_t addr, uint32_t limit);
/* As above but writes only -- the instrument for asserting that a range of DM
 * is never written, where reads would otherwise spend the budget. */
void adsp2181_watch_dm_writes(adsp2181_t *cpu, uint16_t addr, uint32_t limit);
void adsp2181_watch_pm(adsp2181_t *cpu, uint16_t addr, int on);
/* Hold PM[addr] at `value` against DSP stores: the store lands and is then
 * undone, so the core always executes `value`. Needed because EICON_FORCE_DM
 * writes at overlay-load time and cannot reach a word the firmware rewrites
 * later. Overlay loads and host writes bypass it, as they bypass the watch. */
void adsp2181_pin_pm(adsp2181_t *cpu, uint16_t addr, uint32_t value, int on);
/* The same for data memory. EICON_FORCE_DM writes once per 8 kHz frame, before
 * the page runs, so it cannot reach a word the firmware writes and reads again
 * inside one frame -- forcing such a word reads as a clean negative while
 * testing nothing (Sessions 193, 199). */
void adsp2181_pin_dm(adsp2181_t *cpu, uint16_t addr, uint16_t value, int on);
/* Times each pin undid a store; 0 means the A/B exercised nothing. */
uint32_t adsp2181_pin_dm_hits(const adsp2181_t *cpu, uint16_t addr);
uint32_t adsp2181_pin_pm_hits(const adsp2181_t *cpu, uint16_t addr);
/* Min and max PC-stack depth since the last call, packed (min<<8)|max, then
 * reset to the current depth. Sampled per frame this says whether depth climbs
 * and stays up (frames pushed and never popped) or spikes and recovers
 * (genuine interrupt nesting) -- the two look identical in an overflow warning
 * and have entirely different causes. */
uint32_t adsp2181_pcsp_window(adsp2181_t *cpu);
/* Arm or disarm every watch (exec, DM, PM) at once; defaults armed. A PM
 * address is a different instruction on each resident page, so an ungated watch
 * fires on all of them and a limit is spent long before the page you meant.
 * Gate on residency and the limit is spent where the question is. */
void adsp2181_watch_gate(adsp2181_t *cpu, int on);
/* Pace a page that never idles by its own transmit publish: once armed, a run
 * returns as soon as `addr` is written, so one call yields exactly one
 * published sample instead of an instruction budget's worth of them. */
void adsp2181_stop_on_dm_write(adsp2181_t *cpu, uint16_t addr, int on);
/* As above but stop only after `group` writes, so a producer emitting a fixed
 * group per pass finishes it instead of being cut mid-group. */
void adsp2181_stop_on_dm_write_n(adsp2181_t *cpu, uint16_t addr, int group,
                                 int on);
/* Take the first value a frame writes to `addr` without interrupting it. The
 * pacing this gives costs nothing in execution flow, unlike stop_on_dm_write,
 * which keeps the core out of IDLE and starves the caller's continuation. */
void adsp2181_latch_dm_write(adsp2181_t *cpu, uint16_t addr, int on);
/* Make adsp2181_modem_sample() treat a stop-on-publish as a yield: run the
 * continuation, then resume the frame where it stopped on the next sample. */
void adsp2181_yield_on_stop(adsp2181_t *cpu, int on);
void adsp2181_continue_non_idle(adsp2181_t *cpu, int on);
/* The latched value, or -1 if the frame published nothing. */
int32_t adsp2181_latched_dm_write(const adsp2181_t *cpu);
/* How many writes the tick made to the latched word. >1 means the caller is
 * keeping one sample of a group and discarding the rest. */
uint32_t adsp2181_latched_dm_writes(const adsp2181_t *cpu);
/* 1 if the last run ended on that publish, 0 if it ran out of budget. Reading
 * it clears it. */
int adsp2181_stop_dm_hit(adsp2181_t *cpu);
void adsp2181_watch_exec(adsp2181_t *cpu, uint16_t addr, int on);
/* Log only the first `limit` executions of addr; 0 means no limit. Needed for
 * addresses that run hundreds of millions of times in a call. */
void adsp2181_watch_exec_limited(adsp2181_t *cpu, uint16_t addr,
                                 uint32_t limit);
void adsp2181_watch_irqs(adsp2181_t *cpu, int on);
int adsp2181_sport0_tx_written(const adsp2181_t *cpu);
/* Number of TX0 latch publications since the most recent SPORT frame began. */
uint32_t adsp2181_sport0_tx_writes(const adsp2181_t *cpu);
uint16_t adsp2181_pmovlay(const adsp2181_t *cpu);
uint16_t adsp2181_dmovlay(const adsp2181_t *cpu);
uint32_t adsp2181_read_pm(adsp2181_t *cpu, uint16_t addr);
uint64_t adsp2181_cycles(const adsp2181_t *cpu);
/* Copy the fixed 21-word state vector captured immediately before SPORT0
 * assertion (`after=0`) or after its execution allowance (`after!=0`). Returns
 * 21, or zero when the output buffer is too short. */
int adsp2181_sport_snapshot(const adsp2181_t *cpu, int after,
                            uint32_t *out, unsigned words);
/* Execution coverage used to reduce firmware opcode audits to instructions
 * actually reached by a replay. Counts are keyed by resident PM address. */
void adsp2181_coverage_clear(adsp2181_t *cpu);
uint64_t adsp2181_coverage_count(const adsp2181_t *cpu, uint16_t pc);
/* Count only while on; defaults on. Pages are swapped into the same PM by
 * download rather than selected by PMOVLAY, so an ungated count at a given
 * address sums every page that was ever resident there. Only the caller knows
 * which page is loaded, so only the caller can scope the count to one. */
void adsp2181_coverage_gate(adsp2181_t *cpu, int on);
/* Per-address DM write census. The watches say who wrote one word; this says
 * how often every word is written, which is what identifying a rate -- a
 * software symbol clock, say -- needs when the candidate set is a whole page. */
void adsp2181_dm_census(adsp2181_t *cpu, int on);
void adsp2181_dm_census_clear(adsp2181_t *cpu);
uint64_t adsp2181_dm_census_count(const adsp2181_t *cpu, uint16_t addr);
void adsp2181_trace_budget(adsp2181_t *cpu, int64_t n);
uint16_t adsp2181_pc(const adsp2181_t *cpu);
void adsp2181_set_pc(adsp2181_t *cpu, uint16_t pc);
void adsp2181_call(adsp2181_t *cpu, uint16_t entry, uint16_t return_pc);
void adsp2181_set_irq(adsp2181_t *cpu, int irq, int asserted);
uint16_t adsp2181_imask(const adsp2181_t *cpu);
void adsp2181_set_imask(adsp2181_t *cpu, uint16_t imask);
void adsp2181_set_flagin(adsp2181_t *cpu, int asserted);
int adsp2181_flagin(const adsp2181_t *cpu);
uint16_t adsp2181_icntl(const adsp2181_t *cpu);
int adsp2181_idle(const adsp2181_t *cpu);
void adsp2181_set_ar(adsp2181_t *cpu, uint16_t value);
/* The line sample the sample-continuation entry expects: both TIKRNL builds
 * read it from SR1 (PM 0x0700 F34 / PM 0x0715 ANA) before storing it through
 * ShellInptr. A driver that injects that entry has to present it. */
void adsp2181_set_sr1(adsp2181_t *cpu, uint16_t value);
uint16_t adsp2181_sr0(const adsp2181_t *cpu);
uint16_t adsp2181_sr1(const adsp2181_t *cpu);

#ifdef __cplusplus
}
#endif
#endif
