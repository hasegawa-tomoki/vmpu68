/* SPDX-License-Identifier: MIT
 *
 * vmpu68 management daemon — hardware abstraction.
 *
 * Real backend: Raspberry Pi GPIO via sw/pi/vmpu68_io.c.
 * Mock backend: used automatically when /dev/gpiomem is unavailable
 * (development on a PC).  Simulates the FPGA register interface, 64KB of
 * X68000 RAM, the snoop FIFO and the W25Q32 flash.
 *
 * This layer is what the circle (bare-metal) port re-implements; everything
 * above it (HTTP router, web UI, protocol) is platform independent.
 */
#ifndef VMPU68_HW_H
#define VMPU68_HW_H

#include <stdint.h>
#include "../pi/vmpu68.h"

int  hw_init(void);              /* returns 1 if running on mock */
int  hw_alive(void);             /* 0: FPGA silent, 1: answering and still initialised, -1: answering but reconfigured (lost its registers) */
void hw_reinit(void);            /* re-program the FPGA registers after a reconfiguration */
int  hw_is_mock(void);

uint16_t hw_reg_read(unsigned reg);
uint16_t hw_status(void);
/* like hw_bus_write but waits for the cycle to finish (I/O timing) */
uint16_t hw_bus_write_sync(uint32_t addr, int word, uint16_t data);
uint16_t hw_bus_read (uint32_t addr, int word, uint16_t *out);
uint16_t hw_bus_write(uint32_t addr, int word, uint16_t data);
/* n sequential words for the screen viewer / dumps: the lock is held for
 * runs of 32 words so the prefetch stream is not broken by the emulator
 * (word-by-word hw_bus_read while Human68k runs: ~3us/word; here ~0.5us).
 * A faulting word reads as 0.  Returns the number of faults. */
unsigned hw_bus_read_block(uint32_t addr, unsigned n, uint16_t *out);
int      hw_snoop_pop(vmpu68_snoop_t *rec);
/* for the snoop hook only: it runs inside a bus access, the lock is held.
 * The hook is called from the completion wait whenever a snoop record is
 * pending while the bus is busy (a DMA burst holding the bus) and may do
 * nothing but pop records. */
int      hw_snoop_pop_nolock(vmpu68_snoop_t *rec);
void     hw_set_snoop_hook(void (*fn)(void));

/* control shadow: led[2:0], drv_reset, drv_halt, irq_en */
void hw_set_led(unsigned rgb);
void hw_set_drv(int reset, int halt);      /* -1 = leave unchanged */
int  hw_drv_state(void);
void hw_set_autostart(int on);             /* 1 = 2-write bus cycles (faster, DRAM-margin sensitive) */
int  hw_set_busy_irq(int on);              /* completion on PI_IRQ (returns the effective mode) */
int  hw_busy_irq(void);
int  hw_set_smi(int on);           /* SMI transport for the register bus (core 2.x): 0 ok, -1 unavailable, -2 the FPGA did not answer (reverted) */
int  hw_smi(void);
int  hw_set_wr_ai(int on);         /* ai write mode: 1 strobe per sequential write (needs the bitstream) */
int  hw_wr_ai(void);
unsigned hw_set_posted_max(unsigned n);  /* outstanding posted writes before a drain (0 = query) */
void hw_set_bus_slow(int on);      /* 5-tick bus cycles (0.1.5-0.1.16 timing) instead of the 68000-like 4 (docs 30) */
int  hw_bus_slow(void);
int  hw_set_wr_setup(int n);       /* extra bus clocks of write-data setup before AS, 0-3 (VSTW_WR_SETUP) */
int  hw_wr_setup(void);
int  hw_set_snoop2(int on);        /* snoop stream v2 (needs the bitstream; returns the effective mode) */
int  hw_snoop2(void);
void hw_clear_fault(void);                 /* clear sticky BERR/timeout flag */
int  hw_rst_seen(int clear);               /* sticky external reset (VDIAG_RST_SEEN); clear=1 also clears it.
                                              -1 = the bitstream has no latch (poll VST_RESET_IN instead) */
void hw_snoop_reset(void);                 /* flush the snoop FIFO, clear overflow, re-align phases */
void hw_set_irq_en(int en);                /* drive PI_IRQ from ipl/snoop state */
int  hw_irq_pending(void);                 /* PI_IRQ line (fast, no bus cycle) */

/* posted-write queue drained by a worker core (circle: core 2).  With it
 * hw_bus_write only stores the write; every other entry point first waits
 * for the writes queued before it (hw_wq_sync).  hw_prof.wr_t then counts
 * the enqueue only; the strobe time moves to hw_wq_stats_t.issue_t. */
void hw_wq_worker(void);           /* the worker core's loop, never returns */
int  hw_wq_enable(int on);         /* 0 = write directly like before (drains the queue first) */
int  hw_wq_enabled(void);
void hw_wq_sync(void);             /* wait until the queued writes have been issued */
typedef struct { int on; uint32_t depth, max_depth, full_waits, syncs, sync_waits, sync_timeouts, limit; uint64_t issued, sync_t, issue_t, sync_max_t; } hw_wq_stats_t;
void hw_wq_stats(hw_wq_stats_t *o);    /* max_depth and sync_max_t reset on read */
unsigned hw_wq_set_limit(unsigned n);  /* entries the producer may run ahead (2..WQ_N); returns the value set */
unsigned hw_wq_limit(void);

typedef struct {                           /* profiling (ticks of the 54MHz generic timer) */
    uint64_t wr_t, rd_t, status_t;
    uint64_t wr_n, rd_n, status_n;
    uint64_t wr_seq[6];       /* write address pattern: word +2, word -2, word same data, byte +1, byte -1, other */
} hw_prof_t;
extern hw_prof_t hw_prof;

/* interrupt acknowledge cycle (FC=7).  Returns one of: */
#define HW_IACK_VECTOR      0   /* device supplied *vec via DTACK */
#define HW_IACK_AUTOVECTOR  1   /* VPA terminated: use autovector 24+level */
#define HW_IACK_SPURIOUS    2   /* bus error / timeout */
int hw_iack(unsigned level, uint8_t *vec);
extern uint16_t hw_iack_st_pre, hw_iack_st;   /* STATUS before / after the last IACK cycle */
extern uint32_t hw_iack_prefault;             /* IACKs that found the sticky fault flag set */

/* diagnostics: ring of the last bus operations (kind: 0 rd, 1 wr posted,
 * 2 wr sync, 3 iack; +4 = word); hw_op_n counts all operations */
#define HW_OPS 64
typedef struct { uint32_t addr; uint16_t data; uint8_t kind, st; } hw_op_t;
extern hw_op_t  hw_ops[HW_OPS];
extern unsigned hw_op_n;

/* flash: JEDEC id, full-image program with verify */
int hw_flash_id(uint8_t id[3]);
int hw_flash_program(const uint8_t *img, size_t len, char *err, size_t errlen);

/* mock-only test hooks */
void hw_mock_dma(uint32_t addr, uint16_t data0, int n);
void hw_mock_set_ipl(unsigned level);   /* inject IPL into mock STATUS */
unsigned hw_mock_bus_ops(void);
void hw_mock_io_copy(uint8_t *buf, int restore);   /* 4 KB mock I/O RAM snapshot (tests) */      /* number of bus read/write cycles */


#endif
