/* SPDX-License-Identifier: MIT
 *
 * emu68k — Musashi 68000 core glued to the vmpu68 hardware layer.
 *
 * Memory model:
 *   [0 .. ram_size)   shadow RAM in Pi memory (fast); optional write-through
 *                     (ram_size: 2MB at init, then what the kernel probes on
 *                     the bus at each machine boot - emu68k_set_ram_size)
 *   $ED0000-$ED3FFF   SRAM: read cache refreshed by the CPU's own writes
 *                     to the real bus keeps X68000 main RAM coherent
 *   [ram_size .. 16M) pass-through: every access becomes a real FPGA bus
 *                     cycle (I/O registers, VRAM, ROM, ...)
 *
 * DMA coherence: call emu68k_snoop_apply() periodically; it drains the
 * FPGA snoop FIFO and patches the shadow.  A JIT would also invalidate
 * translations here (hook point marked in the .c).
 */
#ifndef EMU68K_H
#define EMU68K_H

#include <stdint.h>

int  emu68k_init(uint32_t ram_size, int write_through);
uint32_t emu68k_ram_size(void);
void emu68k_set_ram_size(uint32_t n);   /* core stopped; caller resyncs the shadow */
void emu68k_load(uint32_t addr, const uint8_t *data, uint32_t len);
unsigned emu68k_sram_write(uint32_t addr, const uint8_t *data, uint32_t len);   /* battery SRAM, write-enable handled; returns bad bytes */
void emu68k_sram_read(uint32_t addr, uint8_t *buf, uint32_t len);               /* from the chip (cache refilled) */
void emu68k_reset(void);
int  emu68k_run(int cycles);          /* returns cycles actually executed */
void emu68k_poll_irq(void);           /* read FPGA IPL -> core */
int  emu68k_snoop_apply(void);        /* returns records applied */
void emu68k_fault_info(uint32_t *count, uint32_t *addr, uint32_t *pc);   /* bus-error diagnostics */
uint32_t emu68k_trace_get(uint32_t back);   /* PC history, back=0 newest */
uint32_t emu68k_trace_count(void);
/* IACK anomaly: spurious result or sticky fault flag found set before the
 * IACK cycle; ops = copy of the hw_ops ring taken right after it */
typedef struct { uint32_t us, ppc, pc, op_n; uint16_t sr, st_pre, st; uint8_t level, r, vec;
                 struct { uint32_t addr; uint16_t data; uint8_t kind, st; } ops[64]; } emu68k_anom_t;
uint32_t emu68k_iack_stale(void);   /* IACKs dropped because the level was no longer requested */
uint32_t emu68k_iack_late(void);    /* ... noticed only by the IACK cycle itself (BERR, level gone) */
uint32_t emu68k_anom_count(void);
int  emu68k_anom_get(uint32_t back, emu68k_anom_t *a);
typedef struct { uint32_t us, pc; uint16_t sr, st; uint8_t level, r, vec; } emu68k_iack_lo_t;
uint32_t emu68k_iack_lo_count(void);   /* IACKs of levels 1-4 only, with time/pc */
int      emu68k_iack_lo_get(uint32_t back, emu68k_iack_lo_t *e);
typedef struct { uint32_t us, dt, addr, a2, n, pc; uint8_t wr, word; } emu68k_slow_t;   /* wr=2: a burst addr..a2 of n accesses */
uint32_t emu68k_slow_count(void);      /* bus accesses that took >= 6us */
int      emu68k_slow_get(uint32_t back, emu68k_slow_t *e);
void     emu68k_slow_freeze(int on);
void     emu68k_set_pmax(unsigned n);   /* posted-write depth outside FDC use (see FDC_PMAX) */
int      emu68k_fdc_tight(void);
int      emu68k_adpcm_tight(void);        /* the DMAC is feeding the ADPCM: posted queue at the ADPCM depth */
void     emu68k_set_pmax_adpcm(unsigned n);
unsigned emu68k_pmax_adpcm(void);
unsigned emu68k_pmax_loose(void);
int      emu68k_fdc_busy(void);
extern int iack_log_all;           /* eiq logs every level, not just 1-4 (diagnostics) */    /* FDC/DMAC ch0 touched within FDC_TAIL_MS: a floppy transfer may be running */
uint32_t emu68k_iack_count(void);      /* IACK history: {vec[15:8], result[5:4], level[2:0]} */
unsigned emu68k_iack_get(uint32_t back);
/* debug logs: all DMA snoop records (1M deep) and CPU writes to one watched
 * word; seq numbers are shared so the two can be ordered against each other */
typedef struct { uint32_t seq, pc, addr; uint16_t data; uint8_t size, rd; uint32_t us, cnt, instr; } emu68k_watch_t;   /* instr: instructions executed so far */
typedef struct { uint32_t ppc, pc, us; uint16_t sr; uint8_t vec; } emu68k_exc_t;   /* exception taken: vector < 0x20 */
uint32_t emu68k_exc_count(void);
int  emu68k_exc_get(uint32_t back, emu68k_exc_t *e);
void emu68k_joy_set(int port, unsigned val, unsigned ms);   /* inject a joystick read ($E9A001/3); val active-low, ms hold (0=release) */
/* phantom watch (docs 43): read [a, a+2*words) back after a bus write, at most once per ival_us
 * (default 1000); a change the CPU did not make freezes the watch ring and stops the core */
typedef struct { uint32_t us, seq, pc, wa, addr, instr; uint16_t wv, old, now, n; uint8_t word; } emu68k_pw_t;
void emu68k_pw_set(uint32_t a, uint32_t words, uint32_t ival_us);   /* a = 0: off */
int  emu68k_pw_get(emu68k_pw_t *e, uint32_t *addr, uint32_t *words, uint32_t *checks);   /* returns the hit count */
extern int unset_vec_halt;         /* 1: stop the core on an interrupt through an IPL-unset vector (eex shows it as sr=8xxx, xxx = level) */
int  emu68k_diag_halt_req(void);   /* 1 once after a pw hit: the caller stops the emulator (erun 1 resumes) */
void emu68k_set_watch(uint32_t a, uint32_t len);
void emu68k_set_watch2(uint32_t a, uint32_t len);   /* second range, same ring */
void emu68k_set_watch_flags(int noread);              /* 1: record writes only */
uint32_t emu68k_watch_count(void);
int  emu68k_watch_get(uint32_t back, emu68k_watch_t *w);
int  emu68k_snlog_get(uint32_t back, uint32_t *seq, uint32_t *addr, uint16_t *data, unsigned *flags, uint32_t *us);
uint32_t emu68k_snlog_count(void);
void emu68k_snoop_stats(uint32_t n[4], uint32_t fc[8], uint32_t *nostrobe, uint32_t *dropped);
void emu68k_snoop_ovf_stats(uint32_t *ranges, uint32_t *repaired, uint32_t *inwait);  /* FIFO overflow repairs */
uint32_t emu68k_sus_count(void);
int  emu68k_sus_get(uint32_t back, uint32_t *seq, uint32_t *addr, uint16_t *data, unsigned *flags);
int  emu68k_snlog_find(uint32_t a, uint32_t *back, uint32_t *seq, uint16_t *data, unsigned *flags);
int  emu68k_stopped(void);
int  emu68k_ext_reset(void);          /* stopped by the machine's reset button (until emu68k_reset) */
int  emu68k_poweroff_pending(void);   /* number of $E8E00F power-off requests since reset */
void emu68k_set_break(uint32_t pc);
void emu68k_set_wbreak(uint32_t lo, uint32_t hi);
void emu68k_set_wbreak_pc(uint32_t pclo, uint32_t pchi);  /* only when the writing PC is in [pclo,pchi] */  /* stop after a write into [lo,hi]; lo=0xFFFFFFFF off */
uint32_t emu68k_wbrk_pc(void);   /* PC of the write that tripped the write-break */
int  emu68k_brk_hit(void);      /* 1 once the PC breakpoint fired (kernel stops the core; ebrk re-arms) */
unsigned emu68k_peek16(uint32_t a);   /* emulator-visible word (fills ROM cache) */            /* 1 after the crash breakpoint fired */
int  emu68k_sync_shadow(uint32_t from, uint32_t len);  /* bus -> shadow */
/* write-back main RAM (docs 31): CPU writes stay in the shadow until a DMAC channel start needs them */
int  emu68k_wb_enabled(void);
void emu68k_wb_set(int on);            /* off: writes back everything dirty, then write-through as before */
void emu68k_wb_flush_all(void);
void emu68k_wb_set_slow_ns(unsigned ns);
void emu68k_set_pace_mhz(unsigned mhz);    /* emulated clock limit, fine-grained (0 = unlimited) */
unsigned emu68k_pace_mhz(void);
void emu68k_set_irq_entry_ns(unsigned ns);   /* hold after an IACK: 68000 exception entry time (0 = none) */
unsigned emu68k_irq_entry_ns(void);
void emu68k_set_irq_pace(unsigned mhz, unsigned us);   /* pace at mhz for us after every IACK (0 = off) */
void emu68k_irq_pace(unsigned *mhz, unsigned *us);
int  emu68k_scr_get(uint32_t back, uint32_t *us, uint32_t *dt, uint32_t *addr, uint16_t *data, unsigned *vec, unsigned *hs);   /* scroll register write log */
void emu68k_scr_reset(void);   /* experiment: per-write spin under write-back (0 = none) */
void emu68k_wb_stats(uint32_t *o);     /* [0] DMA starts [1] words written back [2] chain blocks [3] pages turned WT [4] WT writes [5] WB writes [6] dirty words now [7] WT pages now */
int  emu68k_poke16(uint32_t a, unsigned v);   /* main RAM word: shadow + bus (no fault handling) */
/* ROM read cache: pages filled while the X68000 was off / powering down
 * hold garbage.  rom_check() compares the cached pages with the bus and
 * returns the number of mismatching pages (first one in *first, or ~0u);
 * rom_invalidate() drops the whole cache (refilled lazily, ~2us/word). */
unsigned emu68k_rom_check(uint32_t *first);
/* spot-check the RAM shadow against the bus (8 x 64 words, ~1 ms): a
 * mismatch means the DRAM lost its content (power cycle) - returns the
 * number of mismatching words */
unsigned emu68k_shadow_check(void);
void emu68k_rom_invalidate(void);
uint32_t emu68k_pc(void);
void emu68k_set_pc(uint32_t pc);      /* diagnostic: jump the (stopped) core */

/* Xellent30 accelerator emulation: control port (bit2 = 68030, bit1 = local
 * SRAM), 256KB local SRAM at $BC0000, 16MB high memory at $01000000 */
void emu68k_xt30_config(int on, uint32_t port);   /* port 0 = keep */
void emu68k_xt30_info(int *on, uint32_t *port, unsigned *ctl);
int  emu68k_cpu30(void);              /* 1 while the 68030 is selected */
/* I/O timing: $E80000-$EFFFFF accesses keep the spacing of an <mhz> 68000 (0 = off) */
void emu68k_set_io_mhz(unsigned mhz, unsigned max_cyc);   /* max_cyc 0 = default cap */
unsigned emu68k_io_mhz(void);
void emu68k_info_set(const void *p, unsigned len);
const char *emu68k_info_name(void);   /* text VMPU68.X wrote to the port's +$C0-$DF (host name), NUL-terminated */
int  emu68k_info_take_cmd(unsigned *off, unsigned *val);   /* a setting written to the port's +$F0.. by the X68000 (once) */
uint32_t emu68k_info_wlog(uint32_t back, uint32_t *entry);   /* writes to the port: {offset<<16 | value}, back=0 newest; returns total */   /* $ECFF00 information port record (<= 256 bytes) */
void emu68k_crtc_shadow(uint16_t *out, unsigned n);   /* CRTC R0.. as written by the 68000 (write-only on the hardware) */
unsigned emu68k_io_max_cyc(void);
void emu68k_set_cpu30(int on);        /* MPU selection for the next emu68k_reset() */

typedef struct {
    uint32_t d[8], a[8];
    uint32_t pc, sr, usp, isp;
} emu68k_regs_t;
void emu68k_get_regs(emu68k_regs_t *r);   /* racy while running: diagnostic */

void emu68k_jit_account(int n);           /* JIT: cycles completed inside jit_run (for cyc_now) */

/* jprof: instruction/basic-block/access profile for the JIT plan (docs/design/jit-plan.md) */
int emu68k_prof_set(int on);                 /* 1 = start (counters cleared), 0 = stop; returns the new state */
int emu68k_prof_on(void);
const uint32_t *emu68k_prof_ops(void);       /* 65536 per-opcode counts (NULL until first enabled) */
const uint32_t *emu68k_prof_blocks(uint64_t *instr, uint64_t *blocks);   /* 64 bins of instructions per basic block */
extern uint32_t emu68k_prof_acc[4];          /* reads shadow/other, writes shadow/other */

#endif
