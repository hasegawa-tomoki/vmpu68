/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "hw.h"
#include "hw_port.h"

/* The emulator thread and the management handlers share the GPIO
 * interface; every public hw_ entry point takes the port lock
 * (pthread mutex on Linux, spinlock on circle). */
#define HWLOCK()   hw_port_lock()
#define HWUNLOCK() hw_port_unlock()
/* public entry points that must stay ordered after queued writes */
#define HWLOCK_SYNC() do { wq_sync(); hw_port_lock(); } while (0)
static void wq_sync(void);

/* profiling: cycle counter per entry point (ARM generic timer, 54MHz) */
#if defined(__aarch64__)
static inline uint64_t hw_now(void) { uint64_t t; asm volatile("mrs %0, cntvct_el0" : "=r"(t)); return t; }
#else
static inline uint64_t hw_now(void) { return 0; }
#endif
hw_prof_t hw_prof;
#define PROF(field, expr) do { uint64_t _t0 = hw_now(); expr; hw_prof.field##_t += hw_now() - _t0; hw_prof.field##_n++; } while (0)

static int mock;
static uint16_t ctrl_shadow;     /* REG3 write image */

/* ---------------- posted-write queue, drained by a worker core ----------------
 * A posted write costs the emulator core ~0.5 us of GPIO strobes even though
 * it never waits for the bus cycle (60% of its time at the Human68k prompt,
 * 30% in a game).  With the queue the emulator only stores {addr, data}
 * (~20 ns) and a worker core (hw_wq_worker, core 2 on circle) drives the
 * strobes.  Ordering: every other bus operation first waits until the
 * writes queued before it have been issued (wq_sync), so reads, STATUS,
 * synchronous I/O writes and IACKs see the same order as before; the FPGA
 * keeps the order from there.  A DMA write that lands while a CPU write to
 * the same word is still queued is the one (rare) thing that can differ. */
#define WQ_N 16384                      /* 128 KB: absorbs a screen clear; a sync waits at most ~8 ms */
typedef struct { uint32_t addr; uint16_t data; uint16_t word; } wq_ent_t;
static wq_ent_t wq[WQ_N] __attribute__((aligned(64)));
/* single producer (emulator core) / single consumer (worker): each side
 * keeps its own copy of the other's index and re-reads the published one
 * only when it runs out (empty / full), so the two cores do not bounce a
 * cache line on every write.  Everything a core writes often sits on its
 * own line. */
static struct { uint32_t wp, rp_cache, max_depth, full_waits, syncs, sync_waits; uint64_t sync_t; } wq_p __attribute__((aligned(64)));
static volatile uint32_t wq_wp_pub __attribute__((aligned(64)));   /* published by the producer */
static struct { uint32_t rp, wp_cache; uint64_t issued, issue_t; } wq_c __attribute__((aligned(64)));
static volatile uint32_t wq_rp_pub __attribute__((aligned(64)));   /* published by the consumer, after the port lock is released */
static volatile int wq_on __attribute__((aligned(64)));            /* accept writes into the queue */
/* How far the producer may run ahead of the bus.  The ring holds WQ_N, but
 * a synchronous access (I/O write, read, IACK's first bus access) waits for
 * everything queued before it: at 10 MHz bus a full ring is ~13 ms, which
 * lands a raster interrupt's CRTC write lines late (flicker at a split
 * screen) and stretches music timers.  A few hundred entries keep the bus
 * saturated just the same (it is the bottleneck either way) while a sync
 * waits well under a millisecond.  Runtime: 'wq <n>'. */
#define WQ_LIMIT_DEFAULT 512
static volatile uint32_t wq_limit = WQ_LIMIT_DEFAULT;
static uint64_t wq_sync_max_t;                    /* longest single sync wait since the last stats read */
static inline uint32_t wq_load(volatile uint32_t *v) { return __atomic_load_n(v, __ATOMIC_ACQUIRE); }
static inline void wq_store(volatile uint32_t *v, uint32_t x) { __atomic_store_n(v, x, __ATOMIC_RELEASE); }
static inline void wq_relax(void) {
#if defined(__aarch64__)
    asm volatile("yield");
#endif
}
/* the queue has exactly one producer, the emulator core (1).  Writes from
 * other cores (console eld/key injection, boot-time RAM sync) bypass it. */
static inline int on_producer_core(void)
{
#if defined(__aarch64__)
    uint64_t m; asm volatile("mrs %0, mpidr_el1" : "=r"(m)); return (m & 0xFF) == 1;
#else
    return 1;
#endif
}
static uint32_t wq_sync_timeouts;
/* wait until every write queued before `target` has been issued to the FPGA
 * (the worker publishes rp after releasing the port lock, so a caller that
 * then takes the lock is ordered after those strobes) */
static void wq_sync_to(uint32_t target)
{
    if ((int32_t)(wq_load(&wq_rp_pub) - target) >= 0) return;
    wq_p.syncs++;
    uint64_t t0 = hw_now();
    /* bounded (~2 s): a desynchronised index must not hang the emulator core
     * for good; the worker cannot be behind by more than the queue anyway */
    while ((int32_t)(wq_load(&wq_rp_pub) - target) < 0) {
        wq_relax();
        if (hw_now() - t0 > 108000000ull) { wq_sync_timeouts++; hw_port_log("wq: sync timeout (index desync?)"); break; }
    }
    wq_p.sync_waits++;
    uint64_t dt = hw_now() - t0;
    wq_p.sync_t += dt;
    if (dt > wq_sync_max_t) wq_sync_max_t = dt;
}
static void wq_sync(void)
{
    if (!wq_on) return;
    wq_sync_to(wq_load(&wq_wp_pub));
}
/* per device class, so a sprite register read does not wait for a GVRAM
 * fill: 0 GVRAM $C00000-$DFFFFF, 1 TVRAM $E00000-$E7FFFF, 2 sprite/PCG
 * $EB0000-$EBFFFF, 3 the rest of $E80000-$EFFFFF (SRAM; I/O writes are
 * synchronous and never queued).  Main RAM (-1) is never read over the bus
 * by the emulator; a RAM read from elsewhere (dumps, boot checks) waits
 * for everything. */
static volatile uint32_t wq_dev_wp[4];
static inline int wq_class(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a < 0xC00000) return -1;
    if (a < 0xE00000) return 0;
    if (a < 0xE80000) return 1;
    if (a >= 0xF00000) return -1;          /* ROM: read cache, writes ignored */
    return (a >> 16) == 0xEB ? 2 : 3;
}
static inline void wq_sync_reads(uint32_t a)
{
    if (!wq_on) return;
    int c = wq_class(a);
    uint32_t wp = wq_load(&wq_wp_pub);
    if (c < 0) { wq_sync_to(wp); return; }
    /* A class never written since boot keeps its target at 0.  Once the
     * indices pass 2^31 the wrap-safe compare reads that 0 as "2^31 ahead"
     * and every read of the class (all I/O for class 3) hits the 2 s timeout:
     * Human68k at the prompt froze ~22 minutes after boot (docs 31).  A
     * target can never be ahead of the published wp, so clamp it. */
    uint32_t t = wq_load(&wq_dev_wp[c]);
    if ((int32_t)(t - wp) > 0) t = wp;
    wq_sync_to(t);
}
void hw_wq_sync(void) { wq_sync(); }
static void wq_sync_force(void)
{
    uint32_t target = wq_load(&wq_wp_pub);
    while ((int32_t)(wq_load(&wq_rp_pub) - target) < 0) wq_relax();
}

/* ---------------- mock state ----------------
 * 0x000000-0x00FFFF main RAM, 0xE80000-0xE80FFF I/O-ish RW region,
 * 0xF00000-0xF0FFFF ROM (read-only, address-derived pattern). */
static uint8_t  mk_ram[0x10000];     /* X68000 RAM 0x000000-0x00FFFF */
static uint8_t  mk_io[0x1000];
static unsigned mk_ops;
static uint16_t mk_status;
static vmpu68_snoop_t mk_fifo[1024];
static unsigned mk_rp, mk_wp;
static uint8_t *mk_flash;
#define MK_FLASH_SIZE (4u << 20)

int hw_is_mock(void) { return mock; }
void hw_set_autostart(int on)
{
    if (mock) return;
    HWLOCK();
    ctrl_shadow = (uint16_t)((ctrl_shadow & ~VSTW_AUTOSTART) | (on ? VSTW_AUTOSTART : 0));
    vmpu68_reg_write(VREG_STATUS, ctrl_shadow);
    vmpu68_set_autostart_io(on);
    HWUNLOCK();
}
int hw_drv_state(void) { return ctrl_shadow & (VSTW_DRV_RESET | VSTW_DRV_HALT); }

/* ai write mode (one strobe per sequential write): only with a bitstream
 * that reports VDIAG_WR_AI.  Older ones have the RD# synchronizer level in
 * that bit, which can read 1 early in the strobe: ask three times. */
static void hw_set_wr_ai_u(void)
{
    int ok = 1;
    for (int i = 0; i < 3; i++)
        if (!(vmpu68_reg_read(VREG_CTRL) & VDIAG_WR_AI)) ok = 0;
    vmpu68_set_wr_ai_io(ok && (ctrl_shadow & VSTW_AUTOSTART));
}

/* bus cycle timing (REG3 bit12; older bitstreams ignore it).  The default
 * is the 5-clock timing of the 0.1.5 bitstream: the 4-clock, 68000-like
 * cycle ('slow 0') shortens a synchronous read by 50-80ns but the emulator
 * is bound by the Pi's GPIO cost, not the bus (0.1.6: 28.0MHz either way),
 * and it leaves the devices half a bus clock less between DTACK and the
 * data latch.  Kept for experiments. */
void hw_set_bus_slow(int on)
{
    if (mock) return;
    HWLOCK();
    ctrl_shadow = (uint16_t)((ctrl_shadow & ~VSTW_BUS_SLOW) | (on ? VSTW_BUS_SLOW : 0));
    vmpu68_reg_write(VREG_STATUS, ctrl_shadow);
    HWUNLOCK();
}
int hw_bus_slow(void) { return (ctrl_shadow & VSTW_BUS_SLOW) != 0; }
int hw_set_wr_setup(int n)
{
    if (mock) return 0;
    HWLOCK();
    ctrl_shadow = (uint16_t)((ctrl_shadow & ~VSTW_WR_SETUP_MASK) | VSTW_WR_SETUP(n));
    vmpu68_reg_write(VREG_STATUS, ctrl_shadow);
    HWUNLOCK();
    return n & 3;
}
int hw_wr_setup(void) { return (ctrl_shadow >> 14) & 3; }

int hw_set_wr_ai(int on)
{
    if (mock) return 0;
    HWLOCK();
    if (on) hw_set_wr_ai_u(); else vmpu68_set_wr_ai_io(0);
    on = vmpu68_wr_ai_io();
    HWUNLOCK();
    return on;
}
int hw_wr_ai(void) { return mock ? 0 : vmpu68_wr_ai_io(); }

/* SMI transport: switch, then prove the FPGA still answers through it -
 * the signature, and the hello marker toggled twice (a write whose data
 * arrived one transfer late would show up here as a shifted pattern). */
int hw_set_smi(int on)
{
    if (mock) return -1;
    int r;
    HWLOCK();
    r = vmpu68_set_smi(on);
    if (r == 0 && on) {
        int ok = 1;
        for (unsigned i = 0; i < 4 && ok; i++) {
            uint16_t v = (uint16_t)(i & 1 ? (ctrl_shadow | VSTW_HELLO) : (ctrl_shadow & ~VSTW_HELLO));
            vmpu68_reg_write(VREG_STATUS, v);
            uint16_t d = vmpu68_reg_read(VREG_CTRL);
            if ((d >> 8) != 0x56 || ((d & VDIAG_HELLO) != 0) != ((v & VSTW_HELLO) != 0)) ok = 0;
        }
        vmpu68_reg_write(VREG_STATUS, ctrl_shadow);
        if (!ok) { vmpu68_set_smi(0); r = -2; }
    }
    HWUNLOCK();
    return r;
}
int hw_smi(void) { return mock ? 0 : vmpu68_smi_io(); }

/* snoop stream v2 (REG3 bit13, echoed at VDIAG_SNOOP2).  Older bitstreams
 * have the idle WR# level, 1, in that bit: the probe writes the bit as 0
 * first and only trusts an echo that follows the write both ways. */
static int hw_set_snoop2_u(int on)
{
    if (mock) return 0;
    ctrl_shadow &= (uint16_t)~VSTW_SNOOP2;
    vmpu68_reg_write(VREG_STATUS, ctrl_shadow);
    int ok = !(vmpu68_reg_read(VREG_CTRL) & VDIAG_SNOOP2);
    if (on && ok) {
        ctrl_shadow |= VSTW_SNOOP2;
        vmpu68_reg_write(VREG_STATUS, ctrl_shadow);
        if (!(vmpu68_reg_read(VREG_CTRL) & VDIAG_SNOOP2)) {
            ctrl_shadow &= (uint16_t)~VSTW_SNOOP2;
            vmpu68_reg_write(VREG_STATUS, ctrl_shadow);
            ok = 0;
        }
    }
    on = on && ok;
    vmpu68_set_snoop2_io(on);
    /* the mode switch re-aligns nothing by itself: flush the stream */
    vmpu68_reg_write(VREG_STATUS, (uint16_t)(ctrl_shadow | VSTW_SNOOP_RST));
    return on;
}
int hw_set_snoop2(int on) { int r; HWLOCK(); r = hw_set_snoop2_u(on); HWUNLOCK(); return r; }
int hw_snoop2(void) { return mock ? 0 : vmpu68_snoop2_io(); }

/* busy-on-PI_IRQ completion; only if the bitstream echoes the mode bit */
static int hw_set_busy_irq_u(int on)
{
    if (mock) return 0;
    ctrl_shadow = (uint16_t)((ctrl_shadow & ~VSTW_BUSY_IRQ) | (on ? VSTW_BUSY_IRQ : 0));
    vmpu68_reg_write(VREG_STATUS, ctrl_shadow);
    if (on && !(vmpu68_reg_read(VREG_STATUS) & VST_BUSY_IRQ)) {
        ctrl_shadow &= (uint16_t)~VSTW_BUSY_IRQ;
        on = 0;
    }
    vmpu68_set_busy_irq_io(on);
    return on;
}
int hw_set_busy_irq(int on) { int r; HWLOCK(); r = hw_set_busy_irq_u(on); HWUNLOCK(); return r; }
int hw_busy_irq(void) { return vmpu68_busy_irq_io(); }

int hw_init(void)
{
    if (vmpu68_open() == 0) {
        uint16_t sig = vmpu68_reg_read(VREG_CTRL);
        if ((sig >> 8) == 0x56) {
            mock = 0;
            /* 2-write (autostart) cycles; 'as 0' falls back to 3-write.  Keep
             * irq_en: a reflash ('fpga') re-probes here while the emulator,
             * which asserted it once in emu68k_init(), is still alive.
             * Bus cycles are the 68000-like 4-clock kind (VSTW_BUS_SLOW off,
             * 'slow 0'): the 5-clock cycle keeps the strobes half a bus clock
             * longer after DTACK, and at 16 MHz the XVI's text VRAM controller
             * then runs phantom cycles that scribble rows of display data into
             * the text screen (function key row garbage, docs 30). */
            ctrl_shadow = (uint16_t)((ctrl_shadow & VSTW_IRQ_EN) | VSTW_AUTOSTART | VSTW_HELLO);
            vmpu68_reg_write(VREG_STATUS, ctrl_shadow);
            vmpu68_set_autostart_io(1);
            /* completion-on-PI_IRQ ('bi 1'): on by default since the hold
             * read (vmpu68_bus_read_fc) takes completion and data from one
             * GPIO sample - a read no longer pays the separate DATA cycle.
             * 'bi 0' returns to STATUS polling. */
            hw_set_busy_irq_u(1);
            hw_set_wr_ai_u();
            hw_port_log(vmpu68_wr_ai_io() ? "hw: アドレス自動更新書込み (連続書込みは 1 ストローブ)"
                                          : "hw: ビットストリームはアドレス自動更新書込み非対応 - 3 書込み方式");
            hw_port_log(hw_set_snoop2_u(1) ? "hw: スヌープストリーム v2 (取りこぼし範囲の通知・連続レコード)"
                                           : "hw: ビットストリームはスヌープストリーム v2 非対応");
            return 0;
        }
        hw_port_log("hw: FPGA シグネチャ不正 - モックモード");
        vmpu68_close();
    }
    mock = 1;
    if (!mk_flash) {                    /* hw_init() is retried while the FPGA is unpowered */
        mk_flash = calloc(1, MK_FLASH_SIZE);
        memset(mk_flash, 0xFF, MK_FLASH_SIZE);
    }
    return 1;
}

/* The FPGA is powered by the X68000, the Pi is not: an X68000 power cycle
 * reloads the bitstream and every register (autostart, irq_en, the CTRL
 * fields...) silently returns to its power-up value while the host still
 * believes its shadows.  hw_init() sets VSTW_HELLO; the FPGA echoes it in
 * the diagnostics byte, so a clean reconfiguration reads back as 0. */
static int rst_latch_ok = -2;              /* hw_rst_seen: -2 = not probed yet */

int hw_alive(void)
{
    if (mock) return 1;
    HWLOCK();
    uint16_t d = vmpu68_reg_read(VREG_CTRL);
    HWUNLOCK();
    if ((d >> 8) != 0x56) return 0;
    return (d & VDIAG_HELLO) ? 1 : -1;
}

void hw_reinit(void)
{
    if (mock) return;
    HWLOCK();
    rst_latch_ok = -2;                     /* a different bitstream may have been loaded */
    /* keep the host's idea of the mode bits; the drive/pulse bits restart low */
    ctrl_shadow = (uint16_t)((ctrl_shadow & (VSTW_IRQ_EN | VSTW_AUTOSTART | VSTW_LED(7) | VSTW_BUS_SLOW)) | VSTW_HELLO);
    vmpu68_reg_write(VREG_STATUS, ctrl_shadow);
    vmpu68_ctrl_invalidate();           /* the CTRL fields are 0 again: force a rewrite */
    vmpu68_set_autostart_io((ctrl_shadow & VSTW_AUTOSTART) != 0);
    hw_set_busy_irq_u(vmpu68_busy_irq_io());
    if (vmpu68_wr_ai_io()) hw_set_wr_ai_u();
    hw_set_snoop2_u(1);
    vmpu68_reg_write(VREG_STATUS, (uint16_t)(ctrl_shadow | VSTW_SNOOP_RST | VSTW_FLAG_CLR));
    vmpu68_snoop_resync();
    HWUNLOCK();
}

static void ctrl_write(uint16_t v)
{
    ctrl_shadow = v;
    if (!mock) vmpu68_reg_write(VREG_STATUS, v);
}

static void hw_set_led_u(unsigned rgb)
{
    ctrl_write((uint16_t)((ctrl_shadow & ~VSTW_LED(7)) | VSTW_LED(rgb)));
}

static void hw_set_drv_u(int reset, int halt)
{
    uint16_t v = ctrl_shadow;
    if (reset >= 0) v = (uint16_t)((v & ~VSTW_DRV_RESET) | (reset ? VSTW_DRV_RESET : 0));
    if (halt >= 0)  v = (uint16_t)((v & ~VSTW_DRV_HALT)  | (halt  ? VSTW_DRV_HALT  : 0));
    ctrl_write(v);
}

static void hw_set_irq_en_u(int en)
{
    ctrl_write((uint16_t)((ctrl_shadow & ~VSTW_IRQ_EN) | (en ? VSTW_IRQ_EN : 0)));
    vmpu68_set_irq_en_io(en);
}

static int hw_irq_pending_u(void)
{
    if (mock) return VST_IPL(mk_status) != 0 || mk_rp != mk_wp;
    return vmpu68_irq_pin();
}

static void hw_snoop_reset_u(void)
{
    if (mock) { mk_rp = mk_wp = 0; return; }
    vmpu68_reg_write(VREG_STATUS, (uint16_t)(ctrl_shadow | VSTW_SNOOP_RST));   /* pulse */
    vmpu68_snoop_resync();
}

static void hw_clear_fault_u(void)
{
    if (mock) { mk_status &= (uint16_t)~VST_FAULT; return; }
    /* bit2 is a pulse in the FPGA: write it once, do not keep it in the shadow */
    vmpu68_reg_write(VREG_STATUS, (uint16_t)(ctrl_shadow | VSTW_FLAG_CLR));
}

/* The latch bit was the raw WR# level (idle: 1) in bitstreams before it
 * existed.  Tell them apart once: a clear followed by a read with RESET_IN
 * not asserted gives 0 on a real latch and 1 on the old diagnostic. */
static int hw_rst_seen_u(int clear)
{
    if (mock) return 0;
    if (rst_latch_ok == -2) {
        vmpu68_reg_write(VREG_STATUS, (uint16_t)(ctrl_shadow | VSTW_RST_CLR));
        uint16_t d = vmpu68_reg_read(VREG_CTRL);
        if (!(vmpu68_reg_read(VREG_STATUS) & VST_RESET_IN))
            rst_latch_ok = (d & VDIAG_RST_SEEN) ? 0 : 1;
        if (rst_latch_ok == -2) return -1;  /* reset asserted right now: decide later */
    }
    if (!rst_latch_ok) return -1;
    int seen = (vmpu68_reg_read(VREG_CTRL) & VDIAG_RST_SEEN) ? 1 : 0;
    if (seen && clear)
        vmpu68_reg_write(VREG_STATUS, (uint16_t)(ctrl_shadow | VSTW_RST_CLR));
    return seen;
}

static uint16_t hw_reg_read_u(unsigned reg)
{
    if (!mock) return vmpu68_reg_read(reg);
    if (reg == VREG_CTRL) return 0x5600;
    if (reg == VREG_STATUS) return (uint16_t)(mk_status | ((mk_rp != mk_wp) ? VST_SNOOP : 0));
    return 0;
}

static uint16_t hw_status_u(void) { return hw_reg_read_u(VREG_STATUS); }

/* posted writes: the FPGA command FIFO holds 64 entries, but a bus request
 * (DMAC) is only granted once the commands posted before it have landed,
 * so the queue depth bounds the DMA latency.  Keep at most posted_max
 * outstanding (the emulator lowers it while the FDC is in use, see
 * FDC_PMAX in emu68k.c); any read (which waits for the queue) resets the
 * count */
#define POSTED_MAX 48
static unsigned posted;
static unsigned posted_max = POSTED_MAX;
unsigned hw_set_posted_max(unsigned n)
{
    if (n >= 1 && n <= 48) posted_max = n;
    return posted_max;
}

/* word reads from memory-like regions (main RAM, GVRAM/TVRAM, ROM) go
 * through the FPGA's sequential prefetch stream; the I/O area
 * ($E80000-$EFFFFF) has read side effects and is never streamed */
static inline int pf_region(uint32_t addr)
{
    addr &= 0xFFFFFF;
    return addr < 0xE80000u || addr >= 0xF00000u;
}

static uint16_t hw_bus_read_u(uint32_t addr, int word, uint16_t *out)
{
    if (!mock) {
        posted = 0;
        if (word && pf_region(addr)) return vmpu68_bus_read_pf(addr, out);
        return vmpu68_bus_read(addr, word, out);
    }
    addr &= 0xFFFFFF;
    mk_ops++;
    uint8_t *m = NULL;
    if (addr < sizeof mk_ram) m = &mk_ram[addr];
    else if (addr >= 0xE80000 && addr < 0xE81000) m = &mk_io[addr - 0xE80000];
    else if (addr >= 0xF00000 && addr < 0xF10000) {
        uint16_t v = (uint16_t)(((addr >> 1) ^ 0xA5A5) & 0xFFFF);
        if (word) *out = v;
        else      *out = (addr & 1) ? (v & 0xFF) : (v >> 8);
        return 0;
    }
    if (!m) { *out = 0xFFFF; return VST_FAULT; }
    if (word) { uint8_t *e = m - (addr & 1); *out = (uint16_t)((e[0] << 8) | e[1]); }
    else      *out = *m;
    return 0;
}

static inline void wr_pattern(uint32_t addr, int word, uint16_t data)
{
    static uint32_t wr_last; static uint16_t wr_ldata;   /* pattern statistics (see hw_prof) */
    if (word) {
        if (addr == wr_last + 2) hw_prof.wr_seq[0]++;
        else if (addr == wr_last - 2) hw_prof.wr_seq[1]++;
        else hw_prof.wr_seq[5]++;
        if (data == wr_ldata) hw_prof.wr_seq[2]++;
    } else {
        if (addr == wr_last + 1) hw_prof.wr_seq[3]++;
        else if (addr == wr_last - 1) hw_prof.wr_seq[4]++;
        else hw_prof.wr_seq[5]++;
    }
    wr_last = addr; wr_ldata = data;
}

static uint16_t hw_bus_write_u(uint32_t addr, int word, uint16_t data)
{
    if (!mock) {
        wr_pattern(addr, word, data);
        if (posted >= posted_max) { posted = 0; uint16_t st = vmpu68_bus_drain(); if (st & VST_FAULT) return st; }
        vmpu68_bus_write_posted(addr, word, data);
        posted++;
        return 0;
    }
    addr &= 0xFFFFFF;
    mk_ops++;
    uint8_t *m = NULL;
    if (addr < sizeof mk_ram) m = &mk_ram[addr];
    else if (addr >= 0xE80000 && addr < 0xE81000) m = &mk_io[addr - 0xE80000];
    else if (addr >= 0xF00000 && addr < 0xF10000) return 0;   /* ROM: ignored */
    if (!m) return VST_FAULT;
    if (word) { uint8_t *e = m - (addr & 1); e[0] = (uint8_t)(data >> 8); e[1] = (uint8_t)data; }
    else      *m = (uint8_t)data;
    return 0;
}

static int hw_snoop_pop_u(vmpu68_snoop_t *rec)
{
    if (!mock) return vmpu68_snoop_pop(rec);
    if (mk_rp == mk_wp) return 0;
    *rec = mk_fifo[mk_rp++ & 1023];
    return 1;
}

static void hw_mock_set_ipl_u(unsigned level)
{
    /* STATUS bits 5:3 = IPL (VST_IPL() is the read accessor) */
    mk_status = (uint16_t)((mk_status & ~(7u << 3)) | ((level & 7u) << 3));
}

static void hw_mock_dma_u(uint32_t addr, uint16_t data0, int n)
{
    for (int i = 0; i < n; i++) {
        vmpu68_snoop_t r = {
            .addr = (addr + 2u * (unsigned)i) & 0xFFFFFEu,
            .data = (uint16_t)(data0 + i),
            .uds = 1, .lds = 1,
        };
        mk_fifo[mk_wp++ & 1023] = r;
        if (r.addr + 1 < sizeof mk_ram) {
            mk_ram[r.addr] = (uint8_t)(r.data >> 8);
            mk_ram[r.addr + 1] = (uint8_t)r.data;
        }
    }
}

/* diagnostics: STATUS seen before / after the last IACK cycle, and how many
 * IACKs found the sticky fault flag already set (left by a queued write that
 * completed with BERR/timeout after its hw_bus_write returned) */
uint16_t hw_iack_st_pre, hw_iack_st;
uint32_t hw_iack_prefault;

static int hw_iack_u(unsigned level, uint8_t *vec)
{
    if (mock) { *vec = 0; return HW_IACK_AUTOVECTOR; }
    /* 68000 IACK: FC=7, A3:1 = level, byte read on LDS (odd address) */
    uint16_t v = 0;
    posted = 0;
    /* the fault flag is sticky: one left by an earlier queued write would
     * make this acknowledge look spurious.  Wait for the queue, note the
     * flag and clear it so the result below is the IACK cycle's own */
    hw_iack_st_pre = vmpu68_bus_drain();
    if (hw_iack_st_pre & VST_FAULT) { hw_iack_prefault++; hw_clear_fault_u(); }
    uint16_t st = vmpu68_bus_read_fc(0xFFFFF0u | (level << 1) | 1u, 0, 7, &v);
    hw_iack_st = st;
    if (st & VST_FAULT) { hw_clear_fault_u(); return HW_IACK_SPURIOUS; }   /* flag is sticky */
    if (st & VST_VPA)   return HW_IACK_AUTOVECTOR;
    *vec = (uint8_t)v;
    return HW_IACK_VECTOR;
}

/* ---------------- flash ---------------- */

/* the iCE40 parks the flash in deep power-down (0xB9) after configuration
 * (and after failed attempts); in that state the chip ignores everything
 * except Release Power-down (0xAB) and MISO floats high (reads 0xFF) */
static void fl_wake(void)
{
    uint8_t w = 0xAB;
    vmpu68_flash_xfer(&w, NULL, 1, 0);
    hw_port_usleep(10);                    /* tRES1 >= 3us */
}

static int hw_flash_id_u(uint8_t id[3])
{
    if (mock) { id[0] = 0xEF; id[1] = 0x40; id[2] = 0x16; return 0; }
    uint8_t tx[4] = {0x9F, 0, 0, 0}, rx[4];
    vmpu68_flash_begin();
    fl_wake();
    vmpu68_flash_xfer(tx, rx, 4, 0);
    vmpu68_flash_end();
    id[0] = rx[1]; id[1] = rx[2]; id[2] = rx[3];
    return 0;
}

static void fl_cmd(uint8_t c) { vmpu68_flash_xfer(&c, NULL, 1, 0); }
static uint8_t fl_status(void)
{
    uint8_t tx[2] = {0x05, 0}, rx[2];
    vmpu68_flash_xfer(tx, rx, 2, 0);
    return rx[1];
}
static void fl_addr(uint8_t op, uint32_t a, const uint8_t *tx, uint8_t *rx, unsigned n)
{
    uint8_t hdr[4] = {op, (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a};
    vmpu68_flash_xfer(hdr, NULL, 4, 1);
    if (n) vmpu68_flash_xfer(tx, rx, n, 1);
    vmpu68_flash_xfer(NULL, NULL, 0, 0);
}

static int hw_flash_program_u(const uint8_t *img, size_t len, char *err, size_t errlen)
{
    if (len == 0 || len > MK_FLASH_SIZE) {
        if (errlen) { const char *m = "bad length"; size_t i = 0;
            while (m[i] && i + 1 < errlen) { err[i] = m[i]; i++; } err[i] = 0; }
        return -1;
    }
    if (mock) {
        memset(mk_flash, 0xFF, MK_FLASH_SIZE);
        memcpy(mk_flash, img, len);
        return 0;
    }
    vmpu68_flash_begin();
    fl_wake();
    for (uint32_t a = 0; a < len; a += 65536) {
        fl_cmd(0x06);
        fl_addr(0xD8, a, NULL, NULL, 0);
        while (fl_status() & 1) ;
    }
    for (uint32_t a = 0; a < len; a += 256) {
        unsigned n = len - a > 256 ? 256 : (unsigned)(len - a);
        fl_cmd(0x06);
        fl_addr(0x02, a, img + a, NULL, n);
        while (fl_status() & 1) ;
    }
    uint8_t *vbuf = malloc(len);
    fl_addr(0x03, 0, NULL, vbuf, (unsigned)len);
    int bad = memcmp(img, vbuf, len);
    free(vbuf);
    vmpu68_flash_end();               /* CRESET released: FPGA reboots */
    if (bad) {
        if (errlen) { const char *m = "verify failed"; size_t i = 0;
            while (m[i] && i + 1 < errlen) { err[i] = m[i]; i++; } err[i] = 0; }
        return -1;
    }
    return 0;
}

/* ---------------- locked public wrappers ---------------- */
/* diagnostics: the last HW_OPS bus operations of all cores, in lock order */
hw_op_t  hw_ops[HW_OPS];
unsigned hw_op_n;
static inline void op_log(uint32_t a, int w, unsigned kind, uint16_t d, uint16_t st)
{
    hw_op_t *o = &hw_ops[hw_op_n++ & (HW_OPS - 1)];
    o->addr = a & 0xFFFFFF; o->data = d; o->kind = (uint8_t)(kind | (w ? 4 : 0)); o->st = (uint8_t)st;
}
uint16_t hw_reg_read(unsigned reg) { HWLOCK(); uint16_t r = hw_reg_read_u(reg); HWUNLOCK(); return r; }
uint16_t hw_status(void)           { uint16_t r; HWLOCK(); PROF(status, r = hw_status_u()); HWUNLOCK(); return r; }   /* IPL/snoop sampling: independent of queued writes */
uint16_t hw_bus_read(uint32_t a, int w, uint16_t *o)  { uint16_t r; wq_sync_reads(a); HWLOCK(); PROF(rd, r = hw_bus_read_u(a, w, o));  op_log(a, w, 0, *o, r); HWUNLOCK(); return r; }
uint16_t hw_bus_write(uint32_t a, int w, uint16_t d)
{
    if (wq_on && on_producer_core()) {
        /* queue it: the worker core issues the strobes (hw_wq_worker).
         * wr_t/wr_n now measure only this enqueue (and full-queue waits) */
        uint64_t t0 = hw_now();
        uint32_t wp = wq_p.wp;
        uint32_t lim = wq_limit;
        if (wp - wq_p.rp_cache >= lim) {
            wq_p.rp_cache = wq_load(&wq_rp_pub);
            while (wp - wq_p.rp_cache >= lim) { wq_p.full_waits++; wq_relax(); wq_p.rp_cache = wq_load(&wq_rp_pub); }
        }
        wq_ent_t *e = &wq[wp & (WQ_N - 1)];
        e->addr = a; e->data = d; e->word = (uint16_t)w;
        int c = wq_class(a);
        if (c >= 0) wq_dev_wp[c] = wp + 1;   /* reads of that class must follow it */
        wq_p.wp = wp + 1;
        wq_store(&wq_wp_pub, wp + 1);
        uint32_t depth = wp + 1 - wq_p.rp_cache;
        if (depth > wq_p.max_depth) wq_p.max_depth = depth;
        wr_pattern(a, w, d);
        hw_prof.wr_t += hw_now() - t0; hw_prof.wr_n++;
        return 0;
    }
    uint16_t r; HWLOCK(); PROF(wr, r = hw_bus_write_u(a, w, d)); op_log(a, w, 1, d, r); HWUNLOCK(); return r;
}

/* wait for the FPGA's command queue without holding the port lock across
 * the polls (the emulator core must be able to read STATUS and pop snoop
 * records meanwhile; the snoop hook itself only runs on the emulator core).
 * Bounded like busy_poll: a frozen bus (X68000 off) must not hang the
 * worker, or every wq_sync() would hang with it. */
static void wq_drain_fpga(void)
{
    for (unsigned n = 0; n < (1u << 22); n++) {
        HWLOCK();
        uint16_t st = hw_status_u();
        HWUNLOCK();
        if (!(st & VST_BUSY)) break;
    }
}

/* the worker core's loop: never returns.  Issues up to 8 writes per lock
 * hold (core 1 samples STATUS in between) and publishes rp once per batch. */
void hw_wq_worker(void)
{
    for (;;) {
        uint32_t rp = wq_c.rp;
        if (rp == wq_c.wp_cache) {
            wq_c.wp_cache = wq_load(&wq_wp_pub);
            if (rp == wq_c.wp_cache || mock) { wq_relax(); continue; }
        }
        unsigned n = 0;
        HWLOCK();
        while (rp != wq_c.wp_cache && n < 8) {
            wq_ent_t e = wq[rp & (WQ_N - 1)];
            if (posted >= posted_max) {
                HWUNLOCK();
                wq_drain_fpga();
                HWLOCK();
                posted = 0;
            }
            uint64_t t0 = hw_now();
            vmpu68_bus_write_posted(e.addr, e.word, e.data);
            posted++;
            wq_c.issue_t += hw_now() - t0;
            rp++; n++;
        }
        HWUNLOCK();
        wq_c.rp = rp; wq_c.issued += n;
        wq_store(&wq_rp_pub, rp);
    }
}
int hw_wq_enable(int on)
{
    if (mock) on = 0;
    if (!on && wq_on) { wq_on = 0; __atomic_thread_fence(__ATOMIC_SEQ_CST); wq_sync_force(); }
    else wq_on = on ? 1 : 0;
    return wq_on;
}
int hw_wq_enabled(void) { return wq_on; }
void hw_wq_stats(hw_wq_stats_t *o)
{
    o->on = wq_on; o->depth = wq_load(&wq_wp_pub) - wq_load(&wq_rp_pub); o->max_depth = wq_p.max_depth;
    o->issued = wq_c.issued; o->full_waits = wq_p.full_waits; o->syncs = wq_p.syncs; o->sync_waits = wq_p.sync_waits;
    o->sync_t = wq_p.sync_t; o->issue_t = wq_c.issue_t; o->sync_timeouts = wq_sync_timeouts;
    o->limit = wq_limit; o->sync_max_t = wq_sync_max_t;
    wq_p.max_depth = 0; wq_sync_max_t = 0;
}
unsigned hw_wq_set_limit(unsigned n)
{
    if (n < 2) n = 2;
    if (n > WQ_N) n = WQ_N;
    wq_limit = n;
    return n;
}
unsigned hw_wq_limit(void) { return wq_limit; }
/* Block read (screen dumps).  The prefetch stream never polls STATUS, so
 * the in-wait snoop hook is not reached and a DMA running meanwhile (FDC
 * floppy load) fills the snoop FIFO; once it is nearly full the FPGA stops
 * granting the bus and the FDC overruns.  The emulator core is paused for
 * the dump and cannot drain it either: it would need this same lock, which
 * the chunk loop re-takes at once.  So drain it here, after every chunk. */
static void (*snoop_fn)(void);
unsigned hw_bus_read_block(uint32_t a, unsigned n, uint16_t *o)
{
    unsigned faults = 0;
    wq_sync();
    for (unsigned i = 0; i < n; i += 32) {
        unsigned k = n - i < 32 ? n - i : 32;
        HWLOCK();
        for (unsigned j = 0; j < k; j++) {
            uint16_t r = hw_bus_read_u(a + 2 * (i + j), 1, &o[i + j]);
            if (r & VST_FAULT) { o[i + j] = 0; faults++; hw_clear_fault_u(); }
        }
        if (snoop_fn && !mock) snoop_fn();
        HWUNLOCK();
    }
    return faults;
}
/* I/O writes whose order against queued RAM/VRAM writes does not matter,
 * so they need not wait for the queue: the CRTC scroll registers R10-R19
 * ($E80014-$E80027), the MFP and the OPM.  A raster/HSYNC interrupt
 * handler writes exactly these once per scan line (63 us at 15 kHz) and
 * a queue wait of even a few tens of us there shows as a wobbling split
 * screen and uneven music.  R20+ (modes), R21-R24/the operation port
 * (they qualify TVRAM writes), the video controller, DMAC, FDC, SCSI and
 * the rest keep the full sync. */
static inline int wq_io_bypass(uint32_t a)
{
    a &= 0xFFFFFF;
    return (a >= 0xE80014 && a < 0xE80028)      /* CRTC R10-R19 (text/graphic scroll) */
        || (a >= 0xE88000 && a < 0xE88040)      /* MFP */
        || (a >= 0xE90000 && a < 0xE90004);     /* OPM */
}
uint16_t hw_bus_write_sync(uint32_t a, int w, uint16_t d)
{
    uint16_t r;
    if (wq_io_bypass(a)) hw_port_lock(); else HWLOCK_SYNC();
    if (mock) r = hw_bus_write_u(a, w, d);
    else { posted = 0; PROF(wr, r = vmpu68_bus_write(a, w, d)); }
    op_log(a, w, 2, d, r);
    HWUNLOCK();
    return r;
}
int  hw_snoop_pop(vmpu68_snoop_t *rec) { HWLOCK(); int r = hw_snoop_pop_u(rec); HWUNLOCK(); return r; }
int  hw_snoop_pop_nolock(vmpu68_snoop_t *rec) { return hw_snoop_pop_u(rec); }
void hw_set_snoop_hook(void (*fn)(void)) { snoop_fn = fn; vmpu68_set_snoop_hook(fn); }
void hw_set_led(unsigned rgb)          { HWLOCK(); hw_set_led_u(rgb);          HWUNLOCK(); }
void hw_set_drv(int reset, int halt)   { HWLOCK_SYNC(); hw_set_drv_u(reset, halt);  HWUNLOCK(); }
int  hw_rst_seen(int clear)            { HWLOCK(); int r = hw_rst_seen_u(clear); HWUNLOCK(); return r; }
void hw_clear_fault(void)              { HWLOCK(); hw_clear_fault_u();          HWUNLOCK(); }
void hw_snoop_reset(void)              { HWLOCK(); hw_snoop_reset_u();          HWUNLOCK(); }
void hw_set_irq_en(int en)             { HWLOCK(); hw_set_irq_en_u(en);         HWUNLOCK(); }
int  hw_irq_pending(void)              { return hw_irq_pending_u(); }   /* lock-free: one GPIO read */
void hw_mock_dma(uint32_t a, uint16_t d, int n) { HWLOCK(); hw_mock_dma_u(a, d, n); HWUNLOCK(); }
unsigned hw_mock_bus_ops(void) { return mk_ops; }
/* test support: copy the mock I/O RAM ($E80000-$E80FFF) out or back in */
void hw_mock_io_copy(uint8_t *buf, int restore) { if (restore) memcpy(mk_io, buf, sizeof mk_io); else memcpy(buf, mk_io, sizeof mk_io); }
int  hw_iack(unsigned level, uint8_t *vec) { HWLOCK(); int r = hw_iack_u(level, vec); op_log(level, 0, 3, *vec, (uint16_t)r); HWUNLOCK(); return r; }
void hw_mock_set_ipl(unsigned level) { HWLOCK(); hw_mock_set_ipl_u(level); HWUNLOCK(); }
int  hw_flash_id(uint8_t id[3]) { HWLOCK_SYNC(); int r = hw_flash_id_u(id); HWUNLOCK(); return r; }
int  hw_flash_program(const uint8_t *img, size_t len, char *err, size_t errlen)
{ HWLOCK(); int r = hw_flash_program_u(img, len, err, errlen); HWUNLOCK(); return r; }
