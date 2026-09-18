/* SPDX-License-Identifier: MIT
 *
 * emu68k — Musashi 68000 core glued to the vmpu68 hardware layer,
 * with the real X68000 memory map.
 *
 * Region policy (8KB pages):
 *   MAP_SHADOW  main RAM: reads from local shadow, writes to shadow and
 *               (write-through) to the real bus, so X68000-side masters
 *               always see current memory.  DMA writes are folded back
 *               via emu68k_snoop_apply().
 *   MAP_BUS     VRAM / I/O: every access is a real bus cycle.
 *   MAP_SRAM    $ED0000-$ED3FFF: read cache like MAP_ROM, updated by the
 *               CPU's own writes while $E8E00D = $31 (else re-read).
 *               VRAM must NOT be cached: the CRTC raster-copy and other
 *               video operations modify it without any bus cycle we
 *               could snoop.
 *   MAP_ROM     CGROM/IPL: lazily page-filled read cache; writes are
 *               forwarded to the bus (and ignored by real ROM).
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "m68k.h"
#include "emu68k.h"
#include "jit.h"
#include "../mgmtd/hw.h"
#include "../mgmtd/hw_port.h"

#define PAGE_SHIFT  13                  /* 8KB pages */
#define PAGE_SIZE   (1u << PAGE_SHIFT)
#define ADDR_BITS   25                  /* 24-bit X68000 bus + 16MB local high memory (68030 mode) */
#define ADDR_SPACE  (1u << ADDR_BITS)
#define N_PAGES     (1u << (ADDR_BITS - PAGE_SHIFT))

/* MAP_LOCAL: memory that only exists on the accelerator (Xellent30 local
 * SRAM, high memory): no bus cycle at all, nothing on the X68000 side can
 * see or modify it */
enum { MAP_BUS = 0, MAP_SHADOW = 1, MAP_ROM = 2, MAP_LOCAL = 3, MAP_SRAM = 4 };

static uint8_t *shadow;                 /* 32MB backing store */
static uint8_t  map[N_PAGES];
static uint8_t  rom_valid[N_PAGES];
static int      wthrough;
static uint32_t ram_size;
/* ---- write-back main RAM (docs 31) ----
 * The emulator never reads main RAM over the bus (the shadow is the truth),
 * so a CPU write only has to reach the real DRAM before another bus master
 * reads it there - and the only such master on an X68000 is the DMAC.  A
 * write therefore stays in the shadow and sets a dirty bit; when the CPU
 * starts a DMAC channel (CCR STR/CNT) the ranges that transfer reads
 * (MAR/DAR, and the chain table plus its blocks) are written back first and
 * their pages switch to write-through for a while, so a driver that keeps
 * feeding a running chain (ADPCM) stays visible without a start event.
 * DMA writes into RAM are snooped as before. */
static int      wb_on = 1;
static uint8_t *dirty;                  /* one bit per word: shadow newer than the real RAM */
static uint32_t wt_until[N_PAGES];      /* page write-through until this ms (0 = write-back) */
static uint32_t wb_now_ms, ticks_per_ms;
static uint8_t  dmac_sh[256];           /* DMAC register image as written by the CPU */
static struct { uint32_t starts, words, chains, wt_pages, wt_writes, wb_writes; } wb_stat;
static uint32_t wb_slow_ticks;          /* experiment: spin this long per write-back write (mimics the write-through cost) */
static inline void wb_slow(void) { if (wb_slow_ticks) { uint64_t t = vmpu68_ticks() + wb_slow_ticks; while ((int64_t)(t - vmpu68_ticks()) > 0) ; } }
void emu68k_wb_set_slow_ns(unsigned ns) { wb_slow_ticks = (uint32_t)((uint64_t)ns * vmpu68_tick_hz() / 1000000000ull); }
#define WB_WT_HOLD_MS 3000
static inline void dirty_set(uint32_t a) { __atomic_fetch_or(&dirty[a >> 4], (uint8_t)(1u << ((a >> 1) & 7)), __ATOMIC_RELAXED); }
static inline void dirty_clr(uint32_t a) { __atomic_fetch_and(&dirty[a >> 4], (uint8_t)~(1u << ((a >> 1) & 7)), __ATOMIC_RELAXED); }
static inline int  dirty_get(uint32_t a) { return (dirty[a >> 4] >> ((a >> 1) & 7)) & 1; }
static inline int  page_wt(uint32_t a)   { uint32_t u = wt_until[a >> PAGE_SHIFT]; return u && (int32_t)(wb_now_ms - u) < 0; }
static int      sram_we;                /* last write to $E8E00D was $31: SRAM writes land */
static int      in_execute;             /* bus errors may only longjmp inside m68k_execute */

/* a real bus cycle ended in BERR or timed out: the FPGA flag is sticky,
 * clear it and raise a 68000 bus error exception when we are executing */
static uint32_t fault_count, fault_addr, fault_pc;
/* a fault while Musashi is already building the bus-error frame (double
 * fault: wild SP) makes it read $FFFF01 and halt; if that read faults too we
 * must not raise another exception or we recurse forever.  Set on the first
 * fault, cleared by the instruction hook once the CPU runs an instruction */
static int      in_fault;       /* nesting depth: 1 = building the frame, 2 = double fault (Musashi halts) */
static inline int fault_raise(void)
{
#if defined(__linux__) && !defined(VMPU68_BAREMETAL)
    extern int jit_debug_trace; if (jit_debug_trace) { printf("fault_raise: addr %06X in_execute %d in_fault %d\n", fault_addr, in_execute, in_fault); fflush(stdout); }
#endif
    if (!in_execute || in_fault >= 2) return 0;
    in_fault++;
    m68k_pulse_bus_error();     /* longjmps out (or, on a double fault, halts and returns) */
    return 1;
}

static void bus_fault_at(uint32_t a)
{
    fault_count++;
    fault_addr = a;
    fault_pc = m68k_get_reg(NULL, M68K_REG_PPC);
    hw_clear_fault();
    fault_raise();
}
#define bus_fault() bus_fault_at(a)

/* PC history ring (instruction hook) for post-mortem tracing */
#define TRACE_N 256
static uint32_t trace_ring[TRACE_N];
static uint32_t trace_wp;
static int trace_stop;                  /* set when PC enters the vector table (crash) */
static int ext_reset;                   /* the machine's reset button (FPGA latch seen on PI_IRQ): stopped */
static uint32_t trace_brk = 0xFFFFFFFF;  /* optional PC breakpoint */
static int brk_hit;                       /* the breakpoint fired: the kernel stops the core (ebrk re-arms) */
static uint32_t wbrk_pc;                  /* PC of the instruction that tripped the write-break */
static int      trace_frozen;             /* keep the PC ring as it was at the write-break */
void emu68k_set_break(uint32_t pc) { trace_brk = pc; brk_hit = 0; }
static uint32_t wbrk_addr = 0xFFFFFFFF, wbrk_lo, wbrk_hi, wbrk_pclo = 0, wbrk_pchi = 0xFFFFFFFF;  /* stop after a write into [lo,hi] whose PC is in [pclo,pchi] */
void emu68k_set_wbreak(uint32_t lo, uint32_t hi) { wbrk_lo = lo & ~1u; wbrk_hi = hi; wbrk_addr = (lo == 0xFFFFFFFF) ? 0xFFFFFFFF : lo; brk_hit = 0; trace_frozen = 0; }
void emu68k_set_wbreak_pc(uint32_t pclo, uint32_t pchi) { wbrk_pclo = pclo; wbrk_pchi = pchi; }
uint32_t emu68k_wbrk_pc(void) { return wbrk_pc; }
static inline void wbrk_check(uint32_t a) {
    if (a - wbrk_lo <= wbrk_hi - wbrk_lo && wbrk_addr != 0xFFFFFFFF && !brk_hit) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF;
        if (pc - wbrk_pclo <= wbrk_pchi - wbrk_pclo) { brk_hit = 1; wbrk_pc = pc; trace_frozen = 1; m68k_end_timeslice(); }
    }
}
int  emu68k_brk_hit(void) { return brk_hit; }
static unsigned cur_irq, hook_ctr;
static int rng_pending;               /* a lost-write range waits to be re-read (see sn_repair) */
static void sn_inwait_hook(void);
static inline void fdc_expire(void);
static inline void pace_tick(void);   /* defined with the pacing state below */
/* ---- jprof: instruction / basic-block / access profile for the JIT plan (docs jit-plan.md) ---- */
static uint32_t *prof_op;               /* per-opcode counts (65536) */
static uint32_t  prof_blk[64];          /* instructions per basic block, 63 = 63 or more */
uint32_t         emu68k_prof_acc[4];    /* reads shadow/other, writes shadow/other (counted in the memory paths) */
static unsigned  prof_on, prof_len;
static uint64_t  prof_instr, prof_blocks;
static inline int prof_is_flow(unsigned op)
{
    if ((op & 0xF000) == 0x6000) return 1;                 /* Bcc/BRA/BSR */
    if ((op & 0xF0F8) == 0x50C8) return 1;                 /* DBcc */
    if ((op & 0xFF80) == 0x4E80) return 1;                 /* JSR/JMP */
    if ((op & 0xFFF0) == 0x4E40) return 1;                 /* TRAP */
    if (op >= 0x4E70 && op <= 0x4E77) return 1;            /* RESET/NOP/STOP/RTE/RTD/RTS/TRAPV/RTR */
    return 0;
}
static inline void prof_tick(unsigned pc)
{
    uint32_t p = (pc & (ADDR_SPACE - 1)) >> PAGE_SHIFT;
    unsigned m = map[p];
    if (!(m == MAP_SHADOW || m == MAP_LOCAL || ((m == MAP_ROM || m == MAP_SRAM) && rom_valid[p]))) return;
    unsigned op = ((unsigned)shadow[pc & (ADDR_SPACE - 1)] << 8) | shadow[(pc + 1) & (ADDR_SPACE - 1)];
    prof_op[op]++;
    prof_instr++;
    prof_len++;
    if (prof_is_flow(op)) { prof_blk[prof_len < 63 ? prof_len : 63]++; prof_blocks++; prof_len = 0; }
}
int emu68k_prof_set(int on)
{
    if (on && !prof_op) prof_op = calloc(65536, sizeof(uint32_t));
    if (on && prof_op) { memset(prof_op, 0, 65536 * sizeof(uint32_t)); memset(prof_blk, 0, sizeof prof_blk); memset(emu68k_prof_acc, 0, sizeof emu68k_prof_acc); prof_instr = prof_blocks = 0; prof_len = 0; }
    prof_on = on && prof_op;
    return prof_on;
}
int emu68k_prof_on(void) { return prof_on; }
const uint32_t *emu68k_prof_ops(void) { return prof_op; }
const uint32_t *emu68k_prof_blocks(uint64_t *instr, uint64_t *blocks) { if (instr) *instr = prof_instr; if (blocks) *blocks = prof_blocks; return prof_blk; }

static void hook_periodic(unsigned int pc);
void emu68k_trace_hook(unsigned int pc)
{
    in_fault = 0;
    if (!trace_frozen) trace_ring[trace_wp++ & (TRACE_N - 1)] = pc;
    if (prof_on) prof_tick(pc);
    /* IPL sampling every 8 instructions (~1us, comparable to a real
     * 68000's interrupt latency): the PI_IRQ line is one GPIO read
     * (~100ns), too costly per instruction; the (slow) status register is
     * read only when the line is high */
    pace_tick();                        /* every instruction: raster effects live on 10 MHz instruction timing */
    if ((++hook_ctr & 7) != 0)
        return;
    hook_periodic(pc);
}
/* the every-8th-instruction work: IPL sampling, DMA write buffer, stops.  The JIT calls it per ~64 cycles */
static void hook_periodic(unsigned int pc)
{
    fdc_expire();
    if (ticks_per_ms) wb_now_ms = (uint32_t)(vmpu68_ticks() / ticks_per_ms);
    /* a lost-write range noted by the in-wait hook: re-read it now, before
     * the next instruction can read stale shadow */
    if (rng_pending) emu68k_snoop_apply();
    if (!hw_irq_pending()) {
        if (cur_irq) { cur_irq = 0; m68k_set_irq(0); }
    } else {
        /* the line also signals pending DMA snoop records: fold them into
         * the shadow before the next instruction so CPU and DMA writes to
         * RAM stay ordered (a late snoop would overwrite newer CPU data) */
        unsigned st = hw_status();
        if (st & VST_SNOOP) { emu68k_snoop_apply(); st = hw_status(); }
        /* the level itself can change while the line stays high (MFP
         * cleared, SCC still pending): read it every instruction, or the
         * core acknowledges a stale level and the X68000 glue hands the
         * IACK to whichever device is pending (wrong vector) */
        unsigned lvl = VST_IPL(st);
        /* nothing else holds the line: the external reset latch (a real
         * 68000 stops at once; we would run Human68k on a reset machine
         * for up to 10 ms otherwise - bus error dialogs before the reboot) */
        if (!lvl && !(st & (VST_SNOOP | VST_FAULT)) && !ext_reset && hw_rst_seen(0) > 0) {
            ext_reset = 1;
            m68k_end_timeslice();
        }
        if (lvl != cur_irq) {
            /* like the 68000's two-sample IPL qualification: a single
             * corrupted register read must not turn into an NMI */
            if (VST_IPL(hw_status()) == lvl) { cur_irq = lvl; m68k_set_irq(lvl); }
        }
    }
    if (pc == trace_brk && !brk_hit) {      /* real breakpoint: stop the core at this PC */
        brk_hit = 1;
        m68k_end_timeslice();
    }
    if (pc < 0x400 && !trace_stop) {
        trace_stop = 1;
        m68k_end_timeslice();
    }
}
int emu68k_stopped(void) { return trace_stop; }

/* ---- JIT glue (jit.h) ---- */
uint32_t jit_fault;
static int jit_cyc_run;                 /* cycles completed inside the current jit_run() (cyc_now adds them) */
/* Like Musashi (ADDRESS_68K) the 68000's 24-bit address bus drops the top
 * byte of the effective address.  (Musashi is built without address-error
 * emulation, so odd word addresses are plain accesses here as in the
 * interpreter.) */
#define JIT_AMASK 0x00FFFFFFu
uint32_t jit_rd8 (uint32_t a) { uint32_t f = fault_count; uint32_t v = m68k_read_memory_8(a & JIT_AMASK);  if (fault_count != f) jit_fault = 1; return v; }
uint32_t jit_rd16(uint32_t a) { uint32_t f = fault_count; uint32_t v = m68k_read_memory_16(a & JIT_AMASK); if (fault_count != f) jit_fault = 1; return v; }
uint32_t jit_rd32(uint32_t a) { uint32_t f = fault_count; uint32_t v = m68k_read_memory_32(a & JIT_AMASK); if (fault_count != f) jit_fault = 1; return v; }
void jit_wr8 (uint32_t a, uint32_t v) { uint32_t f = fault_count; m68k_write_memory_8(a & JIT_AMASK, v);  if (fault_count != f) jit_fault = 1; }
void jit_wr16(uint32_t a, uint32_t v) { uint32_t f = fault_count; m68k_write_memory_16(a & JIT_AMASK, v); if (fault_count != f) jit_fault = 1; }
void jit_wr32(uint32_t a, uint32_t v) { uint32_t f = fault_count; m68k_write_memory_32(a & JIT_AMASK, v); if (fault_count != f) jit_fault = 1; }
int emu68k_jit_fetch16(uint32_t pc)
{
    pc &= ADDR_SPACE - 1;
    uint32_t p = pc >> PAGE_SHIFT;
    unsigned m = map[p];
    if (!(m == MAP_SHADOW || m == MAP_LOCAL || ((m == MAP_ROM || m == MAP_SRAM) && rom_valid[p]))) return -1;
    return ((int)shadow[pc] << 8) | shadow[pc + 1];
}
int emu68k_step_interp(void)
{
    in_execute = 1;
    int n = m68k_execute(1);
    in_execute = 0;
    jit_cyc_run += n;
    return n;
}
void emu68k_jit_account(int n) { jit_cyc_run += n; }
void emu68k_jit_periodic(uint32_t pc) { hook_periodic(pc); }
int  emu68k_jit_stop_req(void) { return trace_stop || ext_reset || brk_hit || emu68k_diag_halt_req(); }
void emu68k_jit_trace(uint32_t pc)
{
    if (!trace_frozen) trace_ring[trace_wp++ & (TRACE_N - 1)] = pc;
    if (pc < 0x400 && !trace_stop) trace_stop = 1;
    if (pc == trace_brk && !brk_hit) brk_hit = 1;
}
int emu68k_ext_reset(void) { return ext_reset; }
uint32_t emu68k_trace_get(uint32_t back)      /* back=0: most recent */
{
    return trace_ring[(trace_wp - 1 - back) & (TRACE_N - 1)];
}
uint32_t emu68k_trace_count(void) { return trace_wp; }
unsigned emu68k_peek16(uint32_t a) { return m68k_read_memory_16(a); }

void emu68k_fault_info(uint32_t *count, uint32_t *addr, uint32_t *pc)
{
    *count = fault_count; *addr = fault_addr; *pc = fault_pc;
}

/* ---------------- region setup ---------------- */

static void map_range(uint32_t from, uint32_t to, uint8_t kind)
{
    for (uint32_t p = from >> PAGE_SHIFT; p <= ((to - 1) >> PAGE_SHIFT); p++)
        map[p] = kind;
}

/* ---------------- Xellent30 accelerator emulation ----------------
 *
 * The Xellent30 (Tokyo System Research, 1995) replaces the XVI's 68000 by
 * a 68EC030 and is driven by SRAM-resident software (ch30_omake.sys,
 * XT30DRV.X, mpusw.r).  The software-visible hardware is small:
 *   - a word-wide control port at $EC0000/$EC4000/$EC8000/$ECC000 (DIP
 *     switches); the board is detected by reading it without bus error.
 *     bit 2: MPU select (1 = 68030), bit 1: local SRAM enable.
 *     Writing a different MPU selection resets the newly selected CPU,
 *     which then fetches SSP/PC from RAM $0/$4 (no system reset, so the
 *     IPL ROM mirror at 0 is not involved); the other CPU halts.
 *   - 256KB of local SRAM at $BC0000-$BFFFFF.
 * We emulate the port, the local SRAM and (with 32-bit 68030 addressing)
 * 16MB of "high memory" at $01000000 that HIMEM.SYS-style drivers can use;
 * everything above that is a bus error like on the X68030.
 */
#define XT30_LSRAM_BASE 0xBC0000
#define XT30_LSRAM_SIZE 0x40000
#define XT30_HIMEM_BASE 0x1000000
#define XT30_HIMEM_SIZE 0x1000000

static int      xt30_on = 0;
static uint32_t xt30_port = 0xECC000;
static uint16_t xt30_ctl;
static int      cpu30;                  /* MPU currently selected: 0 = 68000, 1 = 68030 */
static int      cpu30_pending = -1;     /* selection written to the port, applied after the instruction */

static void build_x68k_map(void)
{
    map_range(0x000000, ADDR_SPACE, MAP_BUS);       /* default: everything on the bus */
    if (ram_size)
        map_range(0x000000, ram_size, MAP_SHADOW);  /* main RAM */
    /* 0xC00000-0xDFFFFF GVRAM, 0xE00000-0xE7FFFF TVRAM,
     * 0xE80000-0xEFFFFF I/O + sprite + SRAM: stay MAP_BUS */
    map_range(0xF00000, 0x1000000, MAP_ROM);        /* CGROM + IPL ROM */
    /* 16KB battery-backed SRAM: read cache like the ROM (code that runs from
     * SRAM would otherwise fetch every word over the bus, ~5x slower than a
     * 68000), kept current by the CPU's own writes - see m68k_write_memory_8 */
    map_range(0xED0000, 0xED4000, MAP_SRAM);
    map_range(XT30_HIMEM_BASE, XT30_HIMEM_BASE + XT30_HIMEM_SIZE, MAP_LOCAL);
    if (xt30_on && (xt30_ctl & 2))
        map_range(XT30_LSRAM_BASE, XT30_LSRAM_BASE + XT30_LSRAM_SIZE, MAP_LOCAL);
}

static void apply_cpu_type(void)
{
    jit_flush_all();
    m68k_set_cpu_type(cpu30 ? M68K_CPU_TYPE_68030 : M68K_CPU_TYPE_68000);
}

static void xt30_write(unsigned v)
{
    xt30_ctl = (uint16_t)v;
    build_x68k_map();                   /* local SRAM enable may have changed */
    if (((v >> 2) & 1) != (unsigned)cpu30) {
        cpu30_pending = (v >> 2) & 1;
        m68k_end_timeslice();           /* switch after this instruction completes */
    }
}

void emu68k_xt30_config(int on, uint32_t port)
{
    xt30_on = on;
    if (port) xt30_port = port & 0xFFFFFE;
    build_x68k_map();
}

void emu68k_xt30_info(int *on, uint32_t *port, unsigned *ctl)
{
    *on = xt30_on; *port = xt30_port; *ctl = xt30_ctl;
}

int emu68k_cpu30(void) { return cpu30; }

/* select the MPU for the next emu68k_reset(): like flipping the board's
 * selection while the machine is held in reset */
void emu68k_set_cpu30(int on)
{
    cpu30 = on ? 1 : 0;
    xt30_ctl = (uint16_t)((xt30_ctl & ~4u) | (cpu30 ? 4 : 0));
}

static inline int xt30_hit(uint32_t a) { return xt30_on && (a & ~1u) == xt30_port; }

/* VMPU68 information port, $ECFF00-$ECFFFF (user I/O area, 256 bytes).
 * Software on the X68000 reads a record the kernel refreshes about once a
 * second (emu68k_info_set): "VMPU" magic, a live microsecond counter at +4
 * (reading the high word at +4 latches the value, +6 returns the low word),
 * then 68000-equivalent speed, bus clock, RAM size, flags, version, build
 * and free text.  All values big-endian.  Writes are accepted and ignored. */
#define INFO_BASE 0xECFF00u
static uint8_t  info_buf[256];
static uint32_t info_us_latch;
static inline int info_hit(uint32_t a) { return (a & 0xFFFFFF00u) == INFO_BASE; }
static uint16_t info_read16(uint32_t a)
{
    unsigned o = a & 0xFE;
    if (o == 4) {
        info_us_latch = (uint32_t)(vmpu68_ticks() * 1000000ull / vmpu68_tick_hz());
        return (uint16_t)(info_us_latch >> 16);
    }
    if (o == 6) return (uint16_t)info_us_latch;
    return (uint16_t)(((unsigned)info_buf[o] << 8) | info_buf[o + 1]);
}
void emu68k_info_set(const void *p, unsigned len)
{
    if (len > sizeof info_buf) len = sizeof info_buf;
    memcpy(info_buf, p, len);
}
/* writes to the port are logged (debug aid for X68000-side software) */
#define INFO_WLOG_N 64
static uint32_t info_wlog[INFO_WLOG_N];      /* {offset[7:0], value[15:0]} */
static uint32_t info_wlog_n;
/* settings commands from VMPU68.X: a small queue, so that several options on
 * one command line (-n ... -i0 -m2) all reach the kernel, not just the last */
#define INFO_CMD_N 8
static volatile uint32_t info_cmdq[INFO_CMD_N];   /* [23:16] offset, [15:0] value */
static volatile unsigned info_cmd_wp, info_cmd_rp;
static char info_wbuf[32];                    /* $ECFFC0-$ECFFDF: text written by VMPU68.X (the host name) */
static void info_write16(uint32_t a, uint16_t v)
{
    info_wlog[info_wlog_n++ % INFO_WLOG_N] = ((a & 0xFF) << 16) | v;
    unsigned o = a & 0xFF;
    if (o >= 0xC0 && o < 0xE0) {              /* name buffer: byte or word writes */
        if (a & 1) info_wbuf[o - 0xC0] = (char)v;
        else { info_wbuf[o - 0xC0] = (char)(v >> 8); if (o + 1 < 0xE0) info_wbuf[o - 0xC0 + 1] = (char)v; }
        return;
    }
    /* $ECFFF0 (speed, MHz, 0 = unlimited), $ECFFF2 (write-back 0/1),
     * $ECFFF4 (JIT 0/1), $ECFFF6 (boot screen 0/1), $ECFFF8 (main RAM MB,
     * 0 = probe) and $ECFFFA (1 = take the name from $C0) are settings:
     * VMPU68.X writes them, the kernel applies and saves them */
    o &= 0xFE;
    if (o >= 0xF0 && o <= 0xFA) {
        unsigned wp = info_cmd_wp;
        if (wp - info_cmd_rp < INFO_CMD_N) {
            info_cmdq[wp % INFO_CMD_N] = (o << 16) | v;
            __atomic_store_n(&info_cmd_wp, wp + 1, __ATOMIC_RELEASE);
        }
    }
}
const char *emu68k_info_name(void) { info_wbuf[31] = 0; return info_wbuf; }
int emu68k_info_take_cmd(unsigned *off, unsigned *val)
{
    unsigned rp = info_cmd_rp;
    if (rp == __atomic_load_n(&info_cmd_wp, __ATOMIC_ACQUIRE)) return 0;
    uint32_t c = info_cmdq[rp % INFO_CMD_N];
    __atomic_store_n(&info_cmd_rp, rp + 1, __ATOMIC_RELEASE);
    *off = (c >> 16) & 0xFF; *val = c & 0xFFFF;
    return 1;
}
uint32_t emu68k_info_wlog(uint32_t back, uint32_t *entry)
{
    if (back >= info_wlog_n || back >= INFO_WLOG_N) return 0;
    *entry = info_wlog[(info_wlog_n - 1 - back) % INFO_WLOG_N];
    return info_wlog_n;
}

static void rom_fill(uint32_t page)
{
    uint32_t base = page << PAGE_SHIFT;
    for (uint32_t o = 0; o < PAGE_SIZE; o += 2) {
        uint16_t v = 0;
        if (hw_bus_read(base + o, 1, &v) & VST_FAULT) { bus_fault_at(base + o); return; }
        shadow[base + o] = (uint8_t)(v >> 8);
        shadow[base + o + 1] = (uint8_t)v;
    }
    rom_valid[page] = 1;
}

/* ---------------- memory callbacks (big-endian 68000 view) ------------- */

/* MFP timer data registers (TADR..TDDR, $E8801F/21/23/25): the MC68901
 * returns the counter value captured on the last rising edge of its DS
 * pin (§5.2.1), which on the X68000 is the CPU's LDS, so a real 68000
 * always reads a value at most one bus cycle old.  Our main-RAM fetches
 * come from the shadow without any bus cycle, so a busy-loop followed by
 * a timer read (cpupower.x) would get the value captured at the last
 * I/O access, e.g. the 0 written to restart the timer.  Issue one dummy
 * bus read first so the capture happens right before the real read. */
static inline void mfp_timer_capture(uint32_t a, int word)
{
    if ((a | 1) >= 0xE8801F && (a | 1) <= 0xE88025) {
        uint16_t d;
        hw_bus_read(a, word, &d);
    }
}

/* debug: every DMA snoop record goes to a large log, and CPU writes to one
 * watched word are logged too; both carry a shared sequence number so the
 * two histories can be interleaved after a failure. */
#define SNLOG_N (1u << 20)
typedef struct { uint32_t seq, addr; uint16_t data, flags; uint32_t us; } snlog_t;
static snlog_t *snlog;
static uint32_t snlog_wp;
static uint32_t evseq;
static uint16_t crtc_sh[32];              /* CRTC register image (defined with the I/O code below) */
/* ---- joystick injection (no real pad on the board; intercept reads of the
 * X68000 joystick ports so the Web UI / console can press buttons).  Values
 * are active-low like the real port (idle 0xFF, a pressed bit reads 0). ---- */
static uint8_t  joy_val[2] = { 0xFF, 0xFF };
static uint64_t joy_until[2];
void emu68k_joy_set(int port, unsigned val, unsigned ms)
{
    if (port < 0 || port > 1) return;
    joy_val[port] = (uint8_t) val;
    joy_until[port] = ms ? vmpu68_ticks() + (uint64_t) ms * vmpu68_tick_hz() / 1000 : 0;
}
static inline int joy_hit(uint32_t a, uint8_t *out)
{
    int port = a == 0xE9A001 ? 0 : a == 0xE9A003 ? 1 : -1;
    if (port < 0) return 0;
    if (joy_until[port] && (int64_t)(vmpu68_ticks() - joy_until[port]) >= 0) { joy_until[port] = 0; joy_val[port] = 0xFF; }
    if (joy_val[port] == 0xFF) return 0;
    *out = joy_val[port];
    return 1;
}

static uint32_t watch_addr = 0xFFFFFFFF, watch_end;   /* [addr, end) */
static uint32_t watch_addr2 = 0xFFFFFFFF, watch_end2; /* second range (ewt a len a2 len2) */
#define WATCH_N 16384
static emu68k_watch_t watch_ring[WATCH_N];
static uint32_t watch_wp;
static int      watch_frozen;      /* ring kept as it is (fk watchdog hit) */
static int      watch_noread;      /* ewf 1: writes only (polling loops flood the ring) */

static inline int watch_hit(uint32_t a) { return a - watch_addr < watch_end - watch_addr || a - watch_addr2 < watch_end2 - watch_addr2; }
static void watch_log(uint32_t a, unsigned v, unsigned size, int rd)
{
    if (watch_frozen || (rd && watch_noread)) return;
    /* a polling loop reading the same value is folded into one record */
    if (watch_wp) {
        emu68k_watch_t *l = &watch_ring[(watch_wp - 1) & (WATCH_N - 1)];
        if (l->addr == a && l->data == v && l->rd == rd && l->pc == m68k_get_reg(NULL, M68K_REG_PPC)) {
            l->cnt++;
            return;
        }
    }
    emu68k_watch_t *w = &watch_ring[watch_wp++ & (WATCH_N - 1)];
    w->cnt = 1;
    w->instr = trace_wp;
    w->seq = evseq++;
    w->pc = m68k_get_reg(NULL, M68K_REG_PPC);
    w->addr = a;
    w->data = (uint16_t)v;
    w->size = (uint8_t)size;
    w->rd = (uint8_t)rd;
    w->us = (uint32_t)(vmpu68_ticks() * 1000000ull / vmpu68_tick_hz());
}
void emu68k_set_watch(uint32_t a, uint32_t len)
{
    watch_addr = a & ~1u; watch_end = watch_addr + (len ? len : 2); watch_wp = 0;
    watch_addr2 = 0xFFFFFFFF; watch_end2 = 0xFFFFFFFF;
    watch_frozen = 0;
}
void emu68k_set_watch_flags(int noread) { watch_noread = noread; }
void emu68k_set_watch2(uint32_t a, uint32_t len)   /* adds a second range (ring not reset) */
{
    watch_addr2 = a & ~1u; watch_end2 = watch_addr2 + (len ? len : 2);
}
uint32_t emu68k_watch_count(void) { return watch_wp; }
int emu68k_watch_get(uint32_t back, emu68k_watch_t *w)
{
    if (back >= watch_wp || back >= WATCH_N) return 0;
    *w = watch_ring[(watch_wp - 1 - back) & (WATCH_N - 1)];
    return 1;
}

/* Slow bus accesses: any real bus cycle (RAM write, I/O read/write) whose
 * wall time exceeds SLOW_US is logged with its address and duration - to
 * find what holds the bus when a cycle-steal DMA misses its deadline. */
#define SLOW_N  (1u << 20)
#define SLOW_US 10
static emu68k_slow_t *slow_ring;         /* allocated with the snoop log */
static uint32_t slow_wp;
static uint64_t slow_t0;
static int slow_frozen;
void emu68k_slow_freeze(int on) { slow_frozen = on; }
static inline void slow_begin(void) { slow_t0 = vmpu68_ticks(); }
static uint64_t burst_t0, burst_t1;      /* first access start / last access end */
static uint32_t burst_a0, burst_a1, burst_n, burst_pc;
static void slow_end(uint32_t a, int wr, int word)
{
    uint64_t t1 = vmpu68_ticks();
    uint64_t hz = vmpu68_tick_hz();
    uint32_t dt = (uint32_t)((t1 - slow_t0) * 1000000ull / hz);
    if (!slow_ring || slow_frozen) return;
    if (dt >= SLOW_US) {
        emu68k_slow_t *e = &slow_ring[slow_wp++ & (SLOW_N - 1)];
        e->us = (uint32_t)(t1 * 1000000ull / hz);
        e->dt = dt; e->addr = a; e->a2 = a; e->n = 1;
        e->pc = m68k_get_reg(NULL, M68K_REG_PPC);
        e->wr = (uint8_t)wr; e->word = (uint8_t)word;
    }
    /* a run of accesses with < 2us between them is a burst: another bus
     * master cannot get in until it ends; log the ones >= 12us */
    if (burst_n && slow_t0 - burst_t1 > hz * 2 / 1000000) {
        uint32_t len = (uint32_t)((burst_t1 - burst_t0) * 1000000ull / hz);
        if (len >= 12) {
            emu68k_slow_t *e = &slow_ring[slow_wp++ & (SLOW_N - 1)];
            e->us = (uint32_t)(burst_t0 * 1000000ull / hz);
            e->dt = len; e->addr = burst_a0; e->a2 = burst_a1; e->n = burst_n;
            e->pc = burst_pc;
            e->wr = 2; e->word = 0;
        }
        burst_n = 0;
    }
    if (!burst_n) { burst_t0 = slow_t0; burst_a0 = a; }
    burst_t1 = t1; burst_a1 = a; burst_n++;
    burst_pc = m68k_get_reg(NULL, M68K_REG_PPC);
}
uint32_t emu68k_slow_count(void) { return slow_wp; }
int emu68k_slow_get(uint32_t back, emu68k_slow_t *e)
{
    if (!slow_ring || back >= slow_wp || back >= SLOW_N) return 0;
    *e = slow_ring[(slow_wp - 1 - back) & (SLOW_N - 1)];
    return 1;
}

/* I/O timing fidelity.  Peripherals were designed for the bus-cycle spacing
 * of a 10MHz 68000; here instruction fetches come from the shadow and
 * writes are posted, so two consecutive I/O accesses can reach the chip
 * ~0.3us apart where a real machine has an opcode fetch in between.  The
 * SCSI ROM trips over this: it clears INTS ($E96009) and immediately polls
 * it, and the MB89352 has not cleared the bit yet, so the ROM misreads a
 * stale Time Out (hang at $FC07A8; ch30_omake.sys patches the same spot
 * with a Timer-C wait).  So every I/O access waits until at least the
 * emulated cycles since the previous I/O access have elapsed in real time
 * at io_mhz.  Only $E80000-$EFFFFF is paced; RAM/VRAM run at full speed. */
/* The SCSI ROM also relies on software delays between accesses: after the
 * select command it spins dbf #24 (~250 cycles, 27us) before checking
 * SSTS.INIT, and re-issues the select if INIT is still clear -- which
 * clears the completion of the first select and hangs the target for its
 * 0.5s disconnect timeout.  So spacing is kept up to IO_PACE_MAX_CYC
 * cycles; beyond that a long computation must not turn into a wait, or
 * code touching I/O every few hundred cycles would run at io_mhz. */
/* What is kept is the real machine's GAP between the END of the previous
 * access and the START of the next: the emulated cycles between them less
 * the previous access's own bus cycle (4 clocks; a wait-stated device
 * makes the real gap shorter still).  io_last_ticks is taken when the
 * previous access completed, and our access is 2-3x longer than a real
 * bus cycle, so counting the full distance from that point (as the first
 * versions did) made a polling loop of 22 cycles take 0.4 us more than
 * the real 2.2 us on top of the ~1.2 us the access itself costs (si.x
 * "system performance": 65% of a 10 MHz XVI).
 * A read after a read is not paced at all: nothing has been written that
 * a device could still be digesting, and the reads are synchronous, so
 * their natural spacing (>= 1 us) already exceeds the real gap of any
 * instruction pair up to ~14 cycles apart.  Polling loops (RTC seconds,
 * MFP GPIP for vsync, OPM busy) then run at the access cost alone. */
#define IO_PACE_MAX_CYC 512
#define IO_BUS_CYC 4
static unsigned io_mhz = 10;
static unsigned io_max_cyc = IO_PACE_MAX_CYC;
static uint64_t io_last_ticks, io_last_cyc;
static int      io_last_wr;              /* the previous I/O access was a write */
static uint64_t cyc_base;                /* cycles completed by earlier timeslices */

static inline uint64_t cyc_now(void) { return cyc_base + (uint64_t)jit_cyc_run + (in_execute ? (uint64_t)m68k_cycles_run() : 0); }
/* Emulated clock limit with fine granularity (espd / cfg mhz=): every 8
 * instructions the cycles Musashi has consumed are compared with the wall
 * clock and the core spins until it is back on schedule.  A spin is a few
 * us at most (8 instructions' worth), so interrupts are still taken within
 * a scan line - unlike the old per-timeslice pacing (2000 cycles = 200 us
 * at 10 MHz), which wrecked raster effects.  A pending interrupt ends the
 * spin early; the debt is paid afterwards.  Games that draw against the
 * beam (software raster scrolls) need this: with write-back RAM the core
 * runs at 150+ MHz equivalent and their timing assumptions break. */
static unsigned pace_mhz;               /* espd: whole-time limit (0 = off) */
static unsigned ipace_mhz = 0, ipace_us = 120;   /* interrupt window: pace at this clock for ipace_us after an IACK */
static uint64_t pace_due, pace_cyc_last, ipace_until;
static int      pace_valid;
void emu68k_set_pace_mhz(unsigned mhz) { pace_mhz = mhz; pace_valid = 0; }
unsigned emu68k_pace_mhz(void) { return pace_mhz; }
void emu68k_set_irq_pace(unsigned mhz, unsigned us) { ipace_mhz = mhz; ipace_us = us; }
void emu68k_irq_pace(unsigned *mhz, unsigned *us) { *mhz = ipace_mhz; *us = ipace_us; }
/* Per instruction: the wall clock must not run ahead of the emulated
 * cycles at `mhz`; when it is behind (a slow bus access) the schedule is
 * reset to now - no burst credit, so the core never runs faster than
 * `mhz` even briefly.  Raster code lives on that. */
static inline void pace_tick(void)
{
    unsigned mhz = pace_mhz;
    uint64_t now = vmpu68_ticks();
    if (ipace_mhz && (int64_t)(ipace_until - now) > 0 && (!mhz || ipace_mhz < mhz)) mhz = ipace_mhz;
    if (!mhz) { pace_valid = 0; return; }
    uint64_t cyc = cyc_now();
    if (!pace_valid) { pace_valid = 1; pace_due = now; pace_cyc_last = cyc; return; }
    pace_due += (cyc - pace_cyc_last) * vmpu68_tick_hz() / ((uint64_t)mhz * 1000000ull);
    pace_cyc_last = cyc;
    if ((int64_t)(pace_due - now) <= 0) { pace_due = now; return; }
    while ((int64_t)(pace_due - vmpu68_ticks()) > 0) ;
}
/* $E80000-$EFFFFF except the sprite/PCG RAM at $EB0000 (memory, no settling) */
static inline int io_range(uint32_t a)
{
    return io_mhz && a >= 0xE80000 && a < 0xF00000 && (a >> 16) != 0xEB;
}

/* before an I/O access: keep the emulated gap from the previous one */
static void io_wait(int wr)
{
    if (!wr && !io_last_wr) return;
    uint64_t dcyc = cyc_now() - io_last_cyc;
    if (dcyc > io_max_cyc) dcyc = io_max_cyc;
    dcyc = dcyc > IO_BUS_CYC ? dcyc - IO_BUS_CYC : 0;
    uint64_t due = dcyc * vmpu68_tick_hz() / (io_mhz * 1000000ull);
    while (vmpu68_ticks() - io_last_ticks < due) ;
}

/* after it completed on the bus (I/O writes are synchronous for this) */
static void io_done(int wr)
{
    io_last_ticks = vmpu68_ticks();
    io_last_cyc = cyc_now();
    io_last_wr = wr;
}


/* CRTC raster copy vs. HSYNC polling.  The IOCS scroll loop polls MFP GPIP
 * bit 7 (HSYNC) and, the moment it reads 1, writes CRTC R22 (source/dest
 * raster) and the operation port ($E80480 = 8), then polls again.  Two
 * things go wrong with our ~0 instruction time (rastest.s at 16 MHz:
 * 4-row rasters half copied or copied from the wrong source; fragments of
 * text lines in the function key row):
 *  - rewriting R22 within the first ~2 us of the blanking disturbs the copy
 *    the CRTC is executing right then (a real 68000 needs >= 2.4 us from
 *    the poll read to the write): R22/operation writes that follow a GPIP
 *    read within CRTC_GPIP_US are held until that much time has passed;
 *  - the loop comes back to the poll while HSYNC is still high and requests
 *    a second copy in the same blanking (a real 68000 is ~7 us per loop and
 *    always misses the pulse): after an operation port write the next GPIP
 *    read waits for HSYNC to drop first, so one request per line as on the
 *    real machine.  Both figures are CRTC (video clock) properties.
 *  - the write queue: with a line of text still queued, the (synchronous)
 *    R22/operation writes reach the bus late; the GPIP read drains the
 *    queue first (rastest.s is clean without this, the console is not). */
#define CRTC_GPIP_US 3
static uint64_t gpip_read_ticks;
static int      crtc_op_armed;
/* Scroll registers R10-R19 ($E80014-$E80027): a raster effect writes them
 * from the HSYNC interrupt, one line at a time.  A real 68000 reaches the
 * write ~10 us after the HSYNC edge (exception entry plus the move),
 * i.e. after the pulse; we get there in 2-4 us, still inside it, and the
 * CRTC then takes the new value for the current line or the next one
 * depending on a few hundred ns - a wobbling split screen.  So a scroll
 * write that arrives during the HSYNC pulse waits for the pulse to end.
 * One GPIP read (~1.2 us) per scroll write; CRTC property, bus-clock
 * independent. */
#define SCR_N 512
typedef struct { uint32_t us, dt_iack, addr; uint16_t data; uint8_t vec, hs; } scr_log_t;
static scr_log_t scr_log[SCR_N];
static uint32_t scr_wp; static uint64_t last_iack_ticks; static uint8_t last_iack_vec;
static uint32_t irq_entry_ticks;        /* eid: hold after an IACK (68000 exception entry time) */
void emu68k_set_irq_entry_ns(unsigned ns) { irq_entry_ticks = (uint32_t)((uint64_t)ns * vmpu68_tick_hz() / 1000000000ull); }
unsigned emu68k_irq_entry_ns(void) { return (unsigned)((uint64_t)irq_entry_ticks * 1000000000ull / vmpu68_tick_hz()); }
int emu68k_scr_get(uint32_t back, uint32_t *us, uint32_t *dt, uint32_t *addr, uint16_t *data, unsigned *vec, unsigned *hs)
{
    if (back >= scr_wp || back >= SCR_N) return 0;
    scr_log_t *e = &scr_log[(scr_wp - 1 - back) % SCR_N];
    *us = e->us; *dt = e->dt_iack; *addr = e->addr; *data = e->data; *vec = e->vec; *hs = e->hs;
    return 1;
}
void emu68k_scr_reset(void) { scr_wp = 0; }
static uint32_t gpip_polls;             /* GPIP reads since the last IACK (the handler's HSYNC poll loop) */
static inline void scroll_after_hsync(uint32_t a)
{
    int vc = (a & 0xFFFFFE) == 0xE82500 || (a & 0xFFFFFE) == 0xE82600;   /* video controller R1/R2: logged, not held */
    if (!vc && (a < 0xE80014 || a >= 0xE80028)) return;
    uint16_t v = 0;
    hw_bus_read(0xE88000, 1, &v);
    if (vc) {
        scr_log_t *e = &scr_log[scr_wp++ % SCR_N];
        uint64_t now = vmpu68_ticks(), hz = vmpu68_tick_hz();
        e->us = (uint32_t)(now * 1000000ull / hz); e->dt_iack = (uint32_t)((now - last_iack_ticks) * 1000000ull / hz);
        e->addr = a; e->data = (uint16_t)gpip_polls; e->vec = last_iack_vec; e->hs = (v >> 7) & 1;
        return;
    }
    {   /* diagnostics: when, relative to the last IACK and to HSYNC, did the scroll write happen */
        scr_log_t *e = &scr_log[scr_wp++ % SCR_N];
        uint64_t now = vmpu68_ticks(), hz = vmpu68_tick_hz();
        e->us = (uint32_t)(now * 1000000ull / hz); e->dt_iack = (uint32_t)((now - last_iack_ticks) * 1000000ull / hz);
        e->addr = a; e->data = 0; e->vec = last_iack_vec; e->hs = (v >> 7) & 1;
    }
    if (!(v & 0x80)) return;
    uint64_t t0 = vmpu68_ticks(), lim = vmpu68_tick_hz() * 12 / 1000000ull;
    do { hw_bus_read(0xE88000, 1, &v); }
    while ((v & 0x80) && vmpu68_ticks() - t0 < lim);
}
static inline void crtc_after_gpip(uint32_t a)
{
    if (a == 0xE8002C || (a & ~1u) == 0xE80480) {
        uint64_t due = gpip_read_ticks + vmpu68_tick_hz() * CRTC_GPIP_US / 1000000ull;
        while ((int64_t)(due - vmpu68_ticks()) > 0) ;
        if ((a & ~1u) == 0xE80480) crtc_op_armed = 1;
    }
}
static inline void gpip_before_read(uint32_t a)
{
    if ((a & ~1u) != 0xE88000) return;
    gpip_polls++;
    /* everything queued so far (the text just drawn) must be on the bus
     * before the pulse is sampled: the R22/operation writes that follow are
     * synchronous and would otherwise go out only after the queue drained,
     * possibly after the blanking, and collide with the next line's copy */
    hw_wq_sync();
    if (crtc_op_armed) {                 /* consume the current HSYNC pulse */
        uint64_t t0 = vmpu68_ticks(), lim = vmpu68_tick_hz() * 40 / 1000000ull;
        uint16_t v = 0;
        do { hw_bus_read(0xE88000, 1, &v); }
        while ((v & 0x80) && vmpu68_ticks() - t0 < lim);
        crtc_op_armed = 0;
    }
    gpip_read_ticks = vmpu68_ticks();
}

#define EXC_N 32
static emu68k_exc_t exc_ring[EXC_N];
static uint32_t exc_wp;
static volatile int diag_halt;
int unset_vec_halt;               /* console: stop the core on such an interrupt */
int emu68k_diag_halt_req(void) { int h = diag_halt; diag_halt = 0; return h; }

/* Phantom watch (docs 32): after every bus write, read a block of text
 * VRAM back; if it changed without the CPU having written it, the CRTC did
 * (a phantom cycle).  The first hit freezes the watch ring (ewt/ewl) and
 * stops the core like the fk watchdog, so the accesses that preceded the
 * phantom can be read off. */
int emu68k_fdc_busy(void);
static uint32_t pw_addr;            /* 0 = off */
static uint32_t pw_words;
static uint16_t pw_exp[256];
static int      pw_have;
static uint32_t pw_checks;
static emu68k_pw_t pw_hit;
static int      pw_hits;
static uint64_t pw_last, pw_ival;   /* read back at most once per interval (ticks): every write was far too slow for a game */
static int      pw_dirty;           /* the CPU wrote into the block since the last read-back: re-baseline */
static void pw_check(uint32_t a, int word, uint16_t v)
{
    uint32_t end = a + (word ? 2 : 1);
    if (a < pw_addr + 2 * pw_words && end > pw_addr) { pw_dirty = 1; return; }   /* the CPU wrote into the block itself */
    uint64_t now = vmpu68_ticks();
    if (now - pw_last < pw_ival) return;
    pw_last = now;
    uint16_t cur[256];
    hw_wq_sync();
    hw_bus_read_block(pw_addr, pw_words, cur);
    pw_checks++;
    if (!pw_have || pw_dirty) { memcpy(pw_exp, cur, 2 * pw_words); pw_have = 1; pw_dirty = 0; return; }
    int first = -1, n = 0;
    for (uint32_t i = 0; i < pw_words; i++)
        if (cur[i] != pw_exp[i]) { if (first < 0) first = (int)i; n++; }
    if (first < 0) return;
    pw_hits++;
    if (pw_hits == 1) {
        pw_hit.us = (uint32_t)(vmpu68_ticks() * 1000000ull / vmpu68_tick_hz());
        pw_hit.seq = evseq; pw_hit.pc = m68k_get_reg(NULL, M68K_REG_PPC);
        pw_hit.wa = a; pw_hit.wv = v; pw_hit.word = (uint8_t)word;
        pw_hit.addr = pw_addr + 2 * (uint32_t)first; pw_hit.old = pw_exp[first]; pw_hit.now = cur[first]; pw_hit.n = (uint16_t)n;
        pw_hit.instr = trace_wp;
        watch_frozen = 1;
        diag_halt = 1; m68k_end_timeslice();
    }
    memcpy(pw_exp, cur, 2 * pw_words);
}
void emu68k_pw_set(uint32_t a, uint32_t words, uint32_t ival_us)
{
    pw_addr = a & ~1u; pw_words = words ? (words > 256 ? 256 : words) : 1;
    pw_have = 0; pw_dirty = 0; pw_checks = 0; pw_hits = 0; memset(&pw_hit, 0, sizeof pw_hit);
    pw_ival = (uint64_t)(ival_us ? ival_us : 1000) * vmpu68_tick_hz() / 1000000; pw_last = 0;
    if (!a) pw_addr = 0;
}
int emu68k_pw_get(emu68k_pw_t *e, uint32_t *addr, uint32_t *words, uint32_t *checks)
{
    *e = pw_hit; *addr = pw_addr; *words = pw_words; *checks = pw_checks;
    return pw_hits;
}

/* $E8E00F <- $0F (system port #8, mirrored every 16 bytes over
 * $E8E000-$E8FFFF): the machine's power-off request (IPL FF1308, reached
 * from the front switch, IOCS _POWEROFF, ...).  The IPL then spins in a
 * delay loop of 1M iterations - 1.8 s on a real 10 MHz 68000 - and, if the
 * power has not gone away by then, jumps to the reset entry and boots again.
 * Our loop runs from the ROM cache at Pi speed and is over in ~20 ms, so the
 * IPL re-booted on a machine that was shutting down (FDD seeks audible, the
 * supply then bounced).  Stopping for good after the request was tried too
 * (0.1.5rc3/rc4): the supply then never went away at all - the machine sat
 * with a red LED, bus clock and MFP state intact.  So do what the real CPU
 * does, at the real CPU's pace: hold execution for the delay loop's real
 * duration, then carry on (reboot).  Measured with that: the IPL comes up
 * again, sees the switch off before it touches FDD or screen, writes the
 * sequence a second time and the supply is cut ~0.8 s later (2.6 s after
 * the first request) - the real machine's path.  Bus activity during the
 * hold makes no difference (tried), a long RESET+HALT drive between the
 * two sequences prevents the cut.  pwroff_cnt lets the host log it. */
static volatile unsigned pwroff_cnt;
static uint64_t pwroff_until;
#define PWROFF_HOLD_MS 1800
int emu68k_poweroff_pending(void) { return (int)pwroff_cnt; }

/* CRTC R0-R23 ($E80000-$E8002F) read back as 0 on the hardware; the screen
 * viewer needs the scroll registers, so keep what the 68000 wrote */
void emu68k_crtc_shadow(uint16_t *out, unsigned n)
{
    for (unsigned i = 0; i < n; i++) out[i] = i < 32 ? crtc_sh[i] : 0;
}

/* FDC DMA latency.  The FPGA grants a bus request only after the posted
 * writes queued before it have landed, so the queue depth bounds the DMA
 * latency; the FDC (500 kbps, DMA cycle steal) overruns after ~13us, i.e.
 * ~8 write cycles.  Keep the queue shallow while the FDC is in use - any
 * access to the FDC or DMAC channel 0 registers (the transfer cannot start
 * without a START write to $E84007, and the IOCS polls $E94001 during it)
 * and a tail after the last one - and deep otherwise: the shallow queue
 * costs ~10% in write-bound code, and the other DMA users (SCSI handshake,
 * ADPCM at >= 64us per byte) tolerate the deep one */
#define FDC_PMAX    6
#define FDC_TAIL_MS 200
static unsigned pmax_loose = 48;
static uint64_t fdc_until;
static int      fdc_tight;
void emu68k_set_pmax(unsigned n) { pmax_loose = n; if (!fdc_tight) hw_set_posted_max(n); }
int  emu68k_fdc_tight(void) { return fdc_tight; }
static inline void fdc_touch(uint32_t a)
{
    if ((a & 0xFFFFC0) != 0xE84000 && (a & 0xFFFFFC) != 0xE94000) return;
    fdc_until = vmpu68_ticks() + (uint64_t)FDC_TAIL_MS * vmpu68_tick_hz() / 1000;
    if (!fdc_tight) { fdc_tight = 1; hw_set_posted_max(FDC_PMAX); }
}
static inline void fdc_expire(void)
{
    if (fdc_tight && vmpu68_ticks() > fdc_until) { fdc_tight = 0; hw_set_posted_max(pmax_loose); }
}
/* for the Web server (core 0): the FDC or DMAC channel 0 was touched within
 * the last FDC_TAIL_MS, so a floppy transfer may be running.  Read-only -
 * fdc_expire() on core 1 owns the state */
int emu68k_fdc_busy(void) { return fdc_tight && vmpu68_ticks() <= fdc_until; }

/* write back the dirty words of [lo, hi) (clipped to RAM); posted writes,
 * the caller's synchronous access drains them */
static void wb_flush_range(uint32_t lo, uint32_t hi)
{
    if (!dirty) return;
    lo &= ~1u;
    if (hi > ram_size) hi = ram_size;
    for (uint32_t a = lo; a < hi; a += 2) {
        if (map[a >> PAGE_SHIFT] != MAP_SHADOW) { a = (((a >> PAGE_SHIFT) + 1) << PAGE_SHIFT) - 2; continue; }
        if (!dirty[a >> 4]) { a = (a | 15) - 1; continue; }     /* 8 clean words at once */
        if (!dirty_get(a)) continue;
        dirty_clr(a);
        hw_bus_write(a, 1, (uint16_t)((shadow[a] << 8) | shadow[a + 1]));
        wb_stat.words++;
    }
}
static void wb_mark_wt(uint32_t lo, uint32_t hi)
{
    if (lo >= ram_size) return;
    if (hi > ram_size) hi = ram_size;
    if (hi <= lo) return;
    uint32_t until = wb_now_ms + WB_WT_HOLD_MS;
    if (!until) until = 1;
    for (uint32_t p = lo >> PAGE_SHIFT; p <= ((hi - 1) >> PAGE_SHIFT) && p < N_PAGES; p++) {
        if (!wt_until[p]) wb_stat.wt_pages++;
        wt_until[p] = until;
    }
}
static inline uint32_t sh32(uint32_t a) { a &= ADDR_SPACE - 1; return ((uint32_t)shadow[a] << 24) | ((uint32_t)shadow[a + 1] << 16) | ((uint32_t)shadow[a + 2] << 8) | shadow[a + 3]; }
static inline uint32_t sh16(uint32_t a) { a &= ADDR_SPACE - 1; return ((uint32_t)shadow[a] << 8) | shadow[a + 1]; }
static inline uint32_t dm32(unsigned o) { return ((uint32_t)dmac_sh[o] << 24) | ((uint32_t)dmac_sh[o + 1] << 16) | ((uint32_t)dmac_sh[o + 2] << 8) | dmac_sh[o + 3]; }
static inline uint32_t dm16(unsigned o) { return ((uint32_t)dmac_sh[o] << 8) | dmac_sh[o + 1]; }
static void wb_flush_block(uint32_t addr, uint32_t count, unsigned bytes)
{
    addr &= 0xFFFFFF;
    if (addr >= ram_size) return;
    uint32_t len = count * bytes;
    if (!len) len = 0x10000 * bytes;        /* a count of 0 means 65536 */
    wb_flush_range(addr, addr + len);
    wb_mark_wt(addr, addr + len);
}
/* HD63450 channel start (CCR STR or CNT): write back what the transfer may
 * read.  OCR (+5) bits 5:4 = operand size, 3:2 = chaining (10 array, 11 linked
 * array).  MAR is flushed whatever the direction (harmless when the device
 * writes memory), DAR too when it points into RAM (memory-to-memory). */
static void wb_dma_start(unsigned ch)
{
    unsigned b = (ch & 3) * 0x40;
    unsigned ocr = dmac_sh[b + 5];          /* HD63450: +4 DCR, +5 OCR, +6 SCR, +7 CCR */
    unsigned size = (ocr >> 4) & 3, bytes = size == 1 ? 2 : size == 2 ? 4 : 1;
    unsigned chain = (ocr >> 2) & 3;
    wb_stat.starts++;
    wb_flush_block(dm32(b + 0x0C), dm16(b + 0x0A), bytes);
    wb_flush_block(dm32(b + 0x14), dm16(b + 0x0A), bytes);
    if (chain == 2) {                       /* array chaining: BTC entries of {addr.l, count.w} at BAR */
        uint32_t bar = dm32(b + 0x1C) & 0xFFFFFF, btc = dm16(b + 0x1A);
        if (bar < ram_size && btc && btc <= 4096) {
            wb_stat.chains++;
            wb_flush_block(bar, btc * 6, 1);
            for (uint32_t i = 0; i < btc; i++) wb_flush_block(sh32(bar + i * 6), sh16(bar + i * 6 + 4), bytes);
        }
    } else if (chain == 3) {                /* linked array chaining: {addr.l, count.w, link.l}, link 0 ends */
        uint32_t e = dm32(b + 0x1C) & 0xFFFFFF;
        for (unsigned n = 0; n < 4096 && e && e < ram_size; n++) {
            wb_stat.chains++;
            wb_flush_block(e, 10, 1);
            wb_flush_block(sh32(e), sh16(e + 4), bytes);
            e = sh32(e + 6) & 0xFFFFFF;
        }
    }
}
void emu68k_wb_flush_all(void) { wb_flush_range(0, ram_size); }
int  emu68k_wb_enabled(void) { return wb_on; }
void emu68k_wb_set(int on)
{
    if (!on && wb_on) { wb_on = 0; wb_flush_range(0, ram_size); }
    else wb_on = on ? 1 : 0;
}
void emu68k_wb_stats(uint32_t *o)
{
    o[0] = wb_stat.starts; o[1] = wb_stat.words; o[2] = wb_stat.chains; o[3] = wb_stat.wt_pages; o[4] = wb_stat.wt_writes; o[5] = wb_stat.wb_writes;
    uint32_t nd = 0, np = 0;
    if (dirty) for (uint32_t i = 0; i < ram_size / 16; i++) nd += (uint32_t)__builtin_popcount(dirty[i]);
    for (uint32_t p = 0; p < N_PAGES; p++) if (wt_until[p] && (int32_t)(wb_now_ms - wt_until[p]) < 0) np++;
    o[6] = nd; o[7] = np;
}

static uint16_t io_write(uint32_t a, int word, uint16_t v)
{
    if (wb_on && (a & 0xFFFF00) == 0xE84000) {      /* DMAC: keep the register image, catch channel starts */
        unsigned o = a & 0xFF;
        if (word) { dmac_sh[o] = (uint8_t)(v >> 8); dmac_sh[o + 1] = (uint8_t)v; }
        else dmac_sh[o] = (uint8_t)v;
        unsigned cc = word ? o + 1 : o;
        if ((cc & 0x3F) == 0x07 && (v & 0xC0)) wb_dma_start(cc >> 6);
    }
    if (watch_hit(a)) watch_log(a, v, word ? 2 : 1, 0);
    crtc_after_gpip(a);
    scroll_after_hsync(a);
    /* SRAM write enable ($E8E00D = $31, IOCS _SRAM_WRITE style): decides
     * whether an SRAM write also updates the read cache */
    if (a == 0xE8E00D || (word && a == 0xE8E00C)) sram_we = (v & 0xFF) == 0x31;
    if ((a & 0xFFFFC0) == 0xE80000) {
        unsigned i = (a >> 1) & 31;
        if (word)       crtc_sh[i] = v;
        else if (a & 1) crtc_sh[i] = (uint16_t)((crtc_sh[i] & 0xFF00) | (v & 0xFF));
        else            crtc_sh[i] = (uint16_t)((crtc_sh[i] & 0x00FF) | (v << 8));
    }
    uint16_t st;
    if (!io_range(a)) {
        slow_begin();
        st = hw_bus_write(a, word, v);
        slow_end(a, 1, word);
    } else {
        fdc_touch(a);
        io_wait(1);
        slow_begin();
        st = hw_bus_write_sync(a, word, v);
        slow_end(a, 1, word);
        io_done(1);
    }
    /* phantom watch: only after text VRAM / CRTC / video controller writes, and never
     * while the FDC is transferring (the read-back would starve its DMA) */
    if (pw_addr && a < 0xE84000 && !emu68k_fdc_busy()) pw_check(a, word, v);
    if ((a & 0xFFFFE00F) == 0xE8E00F && !word && (v & 0x0F) == 0x0F) {
        pwroff_cnt++;
        pwroff_until = vmpu68_ticks() + (uint64_t)PWROFF_HOLD_MS * vmpu68_tick_hz() / 1000;
        m68k_end_timeslice();
    }
    return st;
}

void emu68k_set_io_mhz(unsigned mhz, unsigned max_cyc)
{
    io_mhz = mhz;
    io_max_cyc = max_cyc ? max_cyc : IO_PACE_MAX_CYC;
}
unsigned emu68k_io_max_cyc(void) { return io_max_cyc; }
unsigned emu68k_io_mhz(void) { return io_mhz; }

/* the 68000 core masks addresses to 24 bits itself; in 68030 mode the full
 * 32-bit address arrives here and anything outside the emulated space is a
 * bus error (nothing answers there on an X68030 either) */
static void soft_fault(uint32_t a)
{
    fault_count++;
    fault_addr = a;
    fault_pc = m68k_get_reg(NULL, M68K_REG_PPC);
    fault_raise();
}
#define ADDR_CHECK(a) do { if ((a) >= ADDR_SPACE) { soft_fault(a); return 0; } } while (0)
#define ADDR_CHECK_W(a) do { if ((a) >= ADDR_SPACE) { soft_fault(a); return; } } while (0)

unsigned int m68k_read_memory_8(unsigned int a)
{
    ADDR_CHECK(a);
    uint32_t p = a >> PAGE_SHIFT;
    switch (map[p]) {
    case MAP_SHADOW:
    case MAP_LOCAL:  emu68k_prof_acc[0]++; return shadow[a];
    case MAP_ROM:
    case MAP_SRAM:   if (!rom_valid[p]) rom_fill(p);
                     emu68k_prof_acc[0]++; return shadow[a];
    default: {
        uint16_t v = 0;
        emu68k_prof_acc[1]++;
        { uint8_t jv; if (joy_hit(a, &jv)) return jv; }
        if (info_hit(a)) { uint16_t w = info_read16(a); return (a & 1) ? (w & 0xFF) : (w >> 8); }
        if (xt30_hit(a)) return (a & 1) ? (xt30_ctl & 0xFF) : (xt30_ctl >> 8);
        int io = io_range(a);
        if (io) { fdc_touch(a); io_wait(0); }
        mfp_timer_capture(a, 0);
        gpip_before_read(a);
        slow_begin();
        uint16_t st = hw_bus_read(a, 0, &v);
        slow_end(a, 0, 0);
        if (io) io_done(0);
        if (st & VST_FAULT) bus_fault();
        if (watch_hit(a)) watch_log(a, v & 0xFF, 1, 1);
        return v & 0xFF;
    }
    }
}

unsigned int m68k_read_memory_16(unsigned int a)
{
    a &= ~1u;
    ADDR_CHECK(a);
    uint32_t p = a >> PAGE_SHIFT;
    switch (map[p]) {
    case MAP_SHADOW:
    case MAP_LOCAL:  emu68k_prof_acc[0]++; return ((unsigned)shadow[a] << 8) | shadow[a + 1];
    case MAP_ROM:
    case MAP_SRAM:   if (!rom_valid[p]) rom_fill(p);
                     emu68k_prof_acc[0]++; return ((unsigned)shadow[a] << 8) | shadow[a + 1];
    default: {
        uint16_t v = 0;
        emu68k_prof_acc[1]++;
        if (info_hit(a)) return info_read16(a);
        if (xt30_hit(a)) return xt30_ctl;
        int io = io_range(a);
        if (io) { fdc_touch(a); io_wait(0); }
        mfp_timer_capture(a, 1);
        gpip_before_read(a);
        slow_begin();
        uint16_t st = hw_bus_read(a, 1, &v);
        slow_end(a, 0, 1);
        if (io) io_done(0);
        if (st & VST_FAULT) bus_fault();
        if (watch_hit(a)) watch_log(a, v, 2, 1);
        return v;
    }
    }
}

unsigned int m68k_read_memory_32(unsigned int a)
{
    return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2);
}

uint32_t emu68k_snlog_count(void) { return snlog_wp; }
/* suspicious records: single-sample cycles or no strobe in the sample */
#define SUS_N 64
static snlog_t sus_ring[SUS_N];
static uint32_t sus_wp, sn_stat_n[4], sn_stat_fc[8], sn_stat_nostrobe, sn_stat_dropped;
void emu68k_snoop_stats(uint32_t n[4], uint32_t fc[8], uint32_t *nostrobe, uint32_t *dropped)
{
    for (int i = 0; i < 4; i++) n[i] = sn_stat_n[i];
    for (int i = 0; i < 8; i++) fc[i] = sn_stat_fc[i];
    *nostrobe = sn_stat_nostrobe;
    *dropped = sn_stat_dropped;
}
static uint32_t sn_stat_ranges, sn_stat_repaired, sn_stat_inwait;
void emu68k_snoop_ovf_stats(uint32_t *ranges, uint32_t *repaired, uint32_t *inwait)
{
    *ranges = sn_stat_ranges; *repaired = sn_stat_repaired; *inwait = sn_stat_inwait;
}
uint32_t emu68k_sus_count(void) { return sus_wp; }
int emu68k_sus_get(uint32_t back, uint32_t *seq, uint32_t *addr, uint16_t *data, unsigned *flags)
{
    if (back >= sus_wp || back >= SUS_N) return 0;
    snlog_t *r = &sus_ring[(sus_wp - 1 - back) & (SUS_N - 1)];
    *seq = r->seq; *addr = r->addr; *data = r->data; *flags = r->flags;
    return 1;
}
/* the back-th newest record (0 = newest): seq, address, data, flags, time */
int emu68k_snlog_get(uint32_t back, uint32_t *seq, uint32_t *addr, uint16_t *data, unsigned *flags, uint32_t *us)
{
    if (!snlog || back >= snlog_wp || back >= SNLOG_N) return 0;
    snlog_t *r = &snlog[(snlog_wp - 1 - back) & (SNLOG_N - 1)];
    *seq = r->seq; *addr = r->addr; *data = r->data; *flags = r->flags; *us = r->us;
    return 1;
}
/* scan backwards from *back for a record at word address a; returns 1 and
 * leaves *back at the hit (caller adds 1 to continue) */
int emu68k_snlog_find(uint32_t a, uint32_t *back, uint32_t *seq, uint16_t *data, unsigned *flags)
{
    if (!snlog) return 0;
    a &= ~1u;
    uint32_t lim = snlog_wp < SNLOG_N ? snlog_wp : SNLOG_N;
    for (uint32_t b = *back; b < lim; b++) {
        snlog_t *r = &snlog[(snlog_wp - 1 - b) & (SNLOG_N - 1)];
        if (r->addr == a) {
            *back = b; *seq = r->seq; *data = r->data; *flags = r->flags;
            return 1;
        }
    }
    return 0;
}

void m68k_write_memory_8(unsigned int a, unsigned int v)
{
    ADDR_CHECK_W(a);
    wbrk_check(a);
    emu68k_prof_acc[map[a >> PAGE_SHIFT] == MAP_SHADOW || map[a >> PAGE_SHIFT] == MAP_LOCAL ? 2 : 3]++;
    switch (map[a >> PAGE_SHIFT]) {
    case MAP_SHADOW:
        if (watch_hit(a)) watch_log(a, v, 1, 0);
        shadow[a] = (uint8_t)v;
        if (jit_code_map[a >> 8]) jit_note_write(a);
        if (!wthrough) return;
        if (wb_on && !page_wt(a)) { dirty_set(a); wb_stat.wb_writes++; wb_slow(); return; }
        wb_stat.wt_writes++;
        break;
    case MAP_LOCAL:
        shadow[a] = (uint8_t)v;
        if (jit_code_map[a >> 8]) jit_note_write(a);
        return;
    case MAP_ROM:                       /* real ROM ignores writes anyway */
        break;
    case MAP_SRAM:                      /* bus write below; the cache follows only when the write lands */
        if (sram_we) shadow[a] = (uint8_t)v;
        else rom_valid[a >> PAGE_SHIFT] = 0;   /* write-enable off or unknown: re-read the page next time */
        break;
    default:
        if (info_hit(a)) { info_write16(a, (uint16_t)(v & 0xFF)); return; }
        if (xt30_hit(a)) {
            xt30_write((a & 1) ? ((xt30_ctl & 0xFF00) | (v & 0xFF))
                               : ((xt30_ctl & 0xFF) | ((v & 0xFF) << 8)));
            return;
        }
        break;
    }
    if (io_write(a, 0, (uint16_t)(v & 0xFF)) & VST_FAULT) bus_fault();
}

void m68k_write_memory_16(unsigned int a, unsigned int v)
{
    a &= ~1u;
    ADDR_CHECK_W(a);
    wbrk_check(a);
    emu68k_prof_acc[map[a >> PAGE_SHIFT] == MAP_SHADOW || map[a >> PAGE_SHIFT] == MAP_LOCAL ? 2 : 3]++;
    switch (map[a >> PAGE_SHIFT]) {
    case MAP_SHADOW:
        if (watch_hit(a)) watch_log(a, v, 2, 0);
        shadow[a] = (uint8_t)(v >> 8);
        shadow[a + 1] = (uint8_t)v;
        if (jit_code_map[a >> 8]) jit_note_write(a);
        if (!wthrough) return;
        if (wb_on && !page_wt(a)) { dirty_set(a); wb_stat.wb_writes++; wb_slow(); return; }
        wb_stat.wt_writes++;
        break;
    case MAP_LOCAL:
        shadow[a] = (uint8_t)(v >> 8);
        shadow[a + 1] = (uint8_t)v;
        if (jit_code_map[a >> 8]) jit_note_write(a);
        return;
    case MAP_ROM:
        break;
    case MAP_SRAM:
        if (sram_we) { shadow[a] = (uint8_t)(v >> 8); shadow[a + 1] = (uint8_t)v; }
        else rom_valid[a >> PAGE_SHIFT] = 0;
        break;
    default:
        if (info_hit(a)) { info_write16(a, (uint16_t)v); return; }
        if (xt30_hit(a)) { xt30_write(v & 0xFFFF); return; }
        break;
    }
    if (io_write(a, 1, (uint16_t)v) & VST_FAULT) bus_fault();
}

void m68k_write_memory_32(unsigned int a, unsigned int v)
{
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v & 0xFFFF);
}

/* ---------------- interrupt acknowledge ---------------- */

/* IACK history: {level[2:0], result[5:4], vector[15:8]} per acknowledge */
#define IACK_N 64
static uint16_t iack_ring[IACK_N];
static uint32_t iack_wp;
uint32_t emu68k_iack_count(void) { return iack_wp; }
unsigned emu68k_iack_get(uint32_t back)
{
    return iack_ring[(iack_wp - 1 - back) & (IACK_N - 1)];
}
/* the same for levels 1-4 only (IOC: FDC/FDD/HDC/PRN, DMAC, SCSI...): the
 * MFP/SCC levels 5-6 flush the main ring within a second, so a lost or
 * mis-vectored device interrupt was gone before it could be looked at */
#define IACK_LO_N 32
static emu68k_iack_lo_t iack_lo_ring[IACK_LO_N];
static uint32_t iack_lo_wp;
uint32_t emu68k_iack_lo_count(void) { return iack_lo_wp; }
int emu68k_iack_lo_get(uint32_t back, emu68k_iack_lo_t *e)
{
    if (back >= iack_lo_wp || back >= IACK_LO_N) return 0;
    *e = iack_lo_ring[(iack_lo_wp - 1 - back) & (IACK_LO_N - 1)];
    return 1;
}

/* exception history: vectors below 0x20 only (bus/address error, illegal,
 * zero divide, ..., spurious, autovectors) minus line A/F - TRAPs, F-line
 * DOS calls and the device vectors (>= 0x40) would flood it.  Musashi calls the hook after the frame has
 * been pushed, so PPC is the instruction that caused it (or that was
 * interrupted) and the pushed PC is the next one. */
void emu68k_exception_hook(unsigned int vector)
{
    if (vector >= 0x20 || vector == 0x0A || vector == 0x0B) {   // TRAPs, device vectors, line A/F (Human68k DOS calls):
        /* ... except when the vector table entry still carries the IPL's
         * "unset" marker (vector number in the top byte, handler = the
         * error screen): that is the one device exception worth seeing */
        uint32_t h = ((uint32_t)shadow[vector * 4] << 24) | ((uint32_t)shadow[vector * 4 + 1] << 16) | ((uint32_t)shadow[vector * 4 + 2] << 8) | shadow[vector * 4 + 3];
        if (!(h >> 24)) return;
    }
    emu68k_exc_t *e = &exc_ring[exc_wp++ & (EXC_N - 1)];
    e->vec = (uint8_t)vector;
    e->ppc = m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF;
    e->pc  = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
    e->sr  = (uint16_t)m68k_get_reg(NULL, M68K_REG_SR);
    e->us  = (uint32_t)(vmpu68_ticks() * 1000000ull / vmpu68_tick_hz());
}
uint32_t emu68k_exc_count(void) { return exc_wp; }
int emu68k_exc_get(uint32_t back, emu68k_exc_t *e)
{
    if (back >= exc_wp || back >= EXC_N) return 0;
    *e = exc_ring[(exc_wp - 1 - back) & (EXC_N - 1)];
    return 1;
}

/* IACK anomalies: spurious results, and acknowledges that found the sticky
 * fault flag already set, with the bus operations that preceded them */
#define ANOM_N 8
static emu68k_anom_t anom_ring[ANOM_N];
static uint32_t anom_wp;
uint32_t emu68k_anom_count(void) { return anom_wp; }
int emu68k_anom_get(uint32_t back, emu68k_anom_t *a)
{
    if (back >= anom_wp || back >= ANOM_N) return 0;
    *a = anom_ring[(anom_wp - 1 - back) & (ANOM_N - 1)];
    return 1;
}

int iack_log_all;                       /* eiq: also log levels 5-7 (diagnostics, floods with Timer-C) */
static uint32_t iack_stale;             /* acknowledges dropped: level no longer requested */
static uint32_t iack_late;              /* ... found out only by the IACK cycle itself (BERR, level gone) */
uint32_t emu68k_iack_stale(void) { return iack_stale; }
uint32_t emu68k_iack_late(void)  { return iack_late; }

static int int_ack(int level)
{
    uint8_t vec = 0;
    /* a real 68000 samples IPL at every instruction; we sample every 8.
     * A handler that clears its device and returns inside that window
     * (Human68k's DMAC handler: CSR write, 2 instructions, RTE) leaves
     * Musashi a level nobody requests any more; acknowledging it would
     * end in BERR = spurious interrupt.  Re-sample first (two reads, like
     * the 68000's IPL qualification) and, if the level changed, give the
     * core the current one and return an out-of-range vector, which makes
     * Musashi drop the exception without touching any state */
    unsigned lvl = VST_IPL(hw_status());
    if (lvl != (unsigned)level && VST_IPL(hw_status()) == lvl) {
        iack_stale++;
        cur_irq = lvl;
        m68k_set_irq(lvl);
        return 0x100;
    }
    uint32_t pre = hw_iack_prefault;
    int r = hw_iack((unsigned)level, &vec);
    iack_ring[iack_wp++ & (IACK_N - 1)] =
        (uint16_t)((vec << 8) | ((r & 3) << 4) | (level & 7));
    if (level <= 4 || iack_log_all) {
        emu68k_iack_lo_t *e = &iack_lo_ring[iack_lo_wp++ & (IACK_LO_N - 1)];
        e->us = (uint32_t)(vmpu68_ticks() * 1000000ull / vmpu68_tick_hz());
        e->pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
        e->sr = (uint16_t)m68k_get_reg(NULL, M68K_REG_SR);
        e->level = (uint8_t)level; e->r = (uint8_t)r; e->vec = vec;
        e->st = hw_iack_st;
    }
    if (r == HW_IACK_SPURIOUS || pre != hw_iack_prefault) {
        emu68k_anom_t *a = &anom_ring[anom_wp++ & (ANOM_N - 1)];
        a->us = (uint32_t)(vmpu68_ticks() * 1000000ull / vmpu68_tick_hz());
        a->ppc = m68k_get_reg(NULL, M68K_REG_PPC);
        a->pc = m68k_get_reg(NULL, M68K_REG_PC);
        a->sr = (uint16_t)m68k_get_reg(NULL, M68K_REG_SR);
        a->st_pre = hw_iack_st_pre; a->st = hw_iack_st;
        a->level = (uint8_t)level; a->r = (uint8_t)r; a->vec = vec;
        a->op_n = hw_op_n;
        _Static_assert(sizeof a->ops == sizeof hw_ops, "anomaly op copy");
        memcpy(a->ops, hw_ops, sizeof a->ops);
    }
    /* The re-sample above can still lose: the DMAC negates IRQ some time
     * after the CSR write that clears it has completed, and the emulated
     * RTE takes ~50ns where a real one takes 2us, so both samples may
     * see the old level and the acknowledge cycle then finds nobody -
     * BERR.  STATUS after that cycle tells the level is gone: same stale
     * case, drop it instead of handing Human68k a spurious interrupt
     * (its default handler stops the machine with an error dialog). */
    if (r == HW_IACK_SPURIOUS && VST_IPL(hw_iack_st) != (unsigned)level) {
        iack_late++;
        cur_irq = VST_IPL(hw_iack_st);
        m68k_set_irq(cur_irq);
        return 0x100;
    }
    /* the real 68000 samples IPL continuously; Musashi keeps the level we
     * last gave it, so refresh it right after the acknowledge or it would
     * immediately re-take an interrupt the device has already dropped */
    cur_irq = VST_IPL(hw_status());
    m68k_set_irq(cur_irq);
    last_iack_ticks = vmpu68_ticks(); last_iack_vec = (uint8_t)vec; gpip_polls = 0;
    /* A real 68000 spends 44 clocks on the interrupt exception plus the rest
     * of the current instruction before the handler's first instruction runs:
     * 4-7 us at 10 MHz, 3-5 us at 16 MHz.  We would be there in well under
     * 1 us, and a raster handler that then polls HSYNC catches the state
     * *before* the pulse the interrupt was raised for instead of waiting
     * for its edge (Akumajou Dracula: one wrong line at every split).  So
     * hold here for that long (eid <us>, docs 31.6). */
    if (irq_entry_ticks) { uint64_t due = vmpu68_ticks() + irq_entry_ticks; while ((int64_t)(due - vmpu68_ticks()) > 0) ; }
    if (ipace_mhz) { ipace_until = vmpu68_ticks() + (uint64_t)ipace_us * vmpu68_tick_hz() / 1000000ull; pace_valid = 0; }
    {   /* diagnostics (docs 44): an interrupt whose vector table entry still
         * carries the IPL's "unset" marker (vector number in the top byte,
         * handler = the error screen) is about to end in "エラーが発生しました":
         * log it as an exception record and stop the core so the state can be read */
        unsigned v = r == HW_IACK_AUTOVECTOR ? 24u + (unsigned)level : r == HW_IACK_SPURIOUS ? 24u : (unsigned)vec;
        uint32_t h = ((uint32_t)shadow[v * 4] << 24) | ((uint32_t)shadow[v * 4 + 1] << 16) | ((uint32_t)shadow[v * 4 + 2] << 8) | shadow[v * 4 + 3];
        if (h >> 24) {
            emu68k_exc_t *e = &exc_ring[exc_wp++ & (EXC_N - 1)];
            e->vec = (uint8_t)v; e->ppc = m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF; e->pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            e->sr = (uint16_t)(0x8000 | level); e->us = (uint32_t)(vmpu68_ticks() * 1000000ull / vmpu68_tick_hz());
            if (unset_vec_halt) { diag_halt = 1; m68k_end_timeslice(); }
        }
    }
    if (r == HW_IACK_AUTOVECTOR) return M68K_INT_ACK_AUTOVECTOR;
    if (r == HW_IACK_SPURIOUS)   return M68K_INT_ACK_SPURIOUS;
    return vec;
}

void emu68k_trace_hook(unsigned int pc);
int  emu68k_snoop_apply(void);

/* ---------------- public API ---------------- */

int emu68k_init(uint32_t main_ram, int write_through)
{
    shadow = calloc(1, ADDR_SPACE);
    if (!shadow) return -1;
    snlog = calloc(SNLOG_N, sizeof(snlog_t));   /* optional: logging is skipped if NULL */
    slow_ring = calloc(SLOW_N, sizeof(emu68k_slow_t));
    ram_size = main_ram;
    wthrough = write_through;
    dirty = calloc(ADDR_SPACE / 16, 1);
    ticks_per_ms = (uint32_t)(vmpu68_tick_hz() / 1000);
    memset(rom_valid, 0, sizeof rom_valid);
    build_x68k_map();
    m68k_init();
    apply_cpu_type();
    m68k_set_int_ack_callback(int_ack);
    m68k_set_instr_hook_callback(emu68k_trace_hook);
    hw_set_snoop_hook(sn_inwait_hook);
    hw_set_irq_en(1);
    return 0;
}

uint32_t emu68k_ram_size(void) { return ram_size; }

/* main RAM size found by the kernel's bus probe (BootX68): remap while the
 * core is stopped.  Pages that leave the shadow become bus accesses, pages
 * that join it are resynced by the caller (emu68k_sync_shadow) */
void emu68k_set_ram_size(uint32_t n)
{
    jit_flush_all();
    if (dirty) memset(dirty, 0, ADDR_SPACE / 16);
    memset(wt_until, 0, sizeof wt_until);
    ram_size = n;
    build_x68k_map();
}

/* Battery-backed SRAM ($ED0000-$ED3FFF): writes need the write-enable
 * ($E8E00D = $31) like on the machine; the SRAM read cache follows the writes.
 * Call with the emulator core stopped.  Returns bytes that read back wrong. */
unsigned emu68k_sram_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    m68k_write_memory_8(0xE8E00D, 0x31);
    for (uint32_t i = 0; i < len; i++) m68k_write_memory_8(addr + i, data[i]);
    m68k_write_memory_8(0xE8E00D, 0x00);
    /* verify against the chip, not the cache */
    uint8_t *buf = malloc(len);
    unsigned bad = len;
    if (buf) { emu68k_sram_read(addr, buf, len); bad = 0; for (uint32_t i = 0; i < len; i++) if (buf[i] != data[i]) bad++; free(buf); }
    return bad;
}
void emu68k_sram_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    for (uint32_t p = addr >> PAGE_SHIFT; p <= ((addr + len - 1) >> PAGE_SHIFT); p++) rom_valid[p] = 0;   /* refill from the bus */
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)m68k_read_memory_8(addr + i);
}

void emu68k_load(uint32_t addr, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        m68k_write_memory_8(addr + i, data[i]);
}

void emu68k_reset(void)
{
    jit_flush_all();
    in_fault = 0;
    /* behave like the X68000 reset button: hold RESET+HALT for 100ms so
     * every peripheral (SCC, SPC, DMAC...) restarts from a clean state */
    hw_set_drv(1, 1);
    hw_port_usleep(100000);
    hw_set_drv(0, 0);
    hw_port_usleep(10000);
    hw_snoop_reset();
    hw_set_irq_en(1);
    cur_irq = 0;
    m68k_set_irq(0);
    /* the X68000 mirrors the IPL ROM at address 0 right after reset, so the
     * 68000 fetches SSP/PC from $FF0000: reproduce that in the shadow */
    for (int i = 0; i < 8; i++)
        shadow[i] = (uint8_t)m68k_read_memory_8(0xFF0000 + i);
    trace_stop = 0; ext_reset = 0;
    pwroff_cnt = 0; pwroff_until = 0;
    cpu30_pending = -1;
    apply_cpu_type();                   /* the board keeps its MPU selection across a system reset */
    m68k_pulse_reset();
}

/* JIT only where it is faithful: full speed, 68000, no per-instruction diagnostics armed */
static int jit_allowed(void)
{
    return !pace_mhz && !ipace_mhz && !cpu30 && !prof_on && watch_addr == 0xFFFFFFFF && watch_addr2 == 0xFFFFFFFF
        && wbrk_addr == 0xFFFFFFFF && trace_brk == 0xFFFFFFFF && !trace_frozen;
}
int emu68k_run(int cycles)
{
    if (trace_stop || ext_reset) return 0;
    if (pwroff_until) {
        if (vmpu68_ticks() < pwroff_until) return 0;   /* the IPL's delay loop, at 10 MHz pace */
        pwroff_until = 0;
    }
    int n;
    if (jit_enabled() && jit_allowed()) {
        jit_cyc_run = 0;
        n = jit_run(cycles);
        jit_cyc_run = 0;
    } else {
        in_execute = 1;
        n = m68k_execute(cycles);
        in_execute = 0;
    }
    cyc_base += (uint64_t)n;
    if (cpu30_pending >= 0) {
        /* Xellent30 MPU switch: the selected CPU comes out of reset and
         * takes SSP/PC from RAM $0/$4 (the software put them there) */
        cpu30 = cpu30_pending;
        cpu30_pending = -1;
        apply_cpu_type();
        m68k_pulse_reset();
    }
    return n;
}

/* RESET instruction: the 68000 drives RESET low for 124 clocks so the
 * peripherals restart (the IPL does this first thing) */
/* RESET instruction: the 68000 drives RESET low for 124 clocks (7-12 us) so
 * the peripherals restart; the IPL does this first thing.  Until 0.1.19 the
 * Musashi callback was never registered (M68K_EMULATE_RESET was OPT_ON, i.e.
 * the runtime callback, which nothing set), so a software restart through
 * the ROM entry left every peripheral as it was: a mouse byte the IPL had
 * polled for stayed in the SCC with its interrupt request pending, and the
 * IPL - which lowers the mask before it sets the SCC vectors - died in its
 * error screen (vfd68 boot stub -> "エラーが発生しました", docs 44). */
void emu68k_reset_hook(void)
{
    hw_set_drv(1, -1);
    hw_port_usleep(20);
    hw_set_drv(0, -1);
}
uint32_t emu68k_pc(void) { return m68k_get_reg(NULL, M68K_REG_PC); }
void emu68k_set_pc(uint32_t pc) { m68k_set_reg(M68K_REG_PC, pc); }

void emu68k_get_regs(emu68k_regs_t *r)
{
    static const int dr[8] = {M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3,
                              M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7};
    static const int ar[8] = {M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3,
                              M68K_REG_A4, M68K_REG_A5, M68K_REG_A6, M68K_REG_A7};
    for (int i = 0; i < 8; i++) {
        r->d[i] = m68k_get_reg(NULL, dr[i]);
        r->a[i] = m68k_get_reg(NULL, ar[i]);
    }
    r->pc  = m68k_get_reg(NULL, M68K_REG_PC);
    r->sr  = m68k_get_reg(NULL, M68K_REG_SR);
    r->usp = m68k_get_reg(NULL, M68K_REG_USP);
    r->isp = m68k_get_reg(NULL, M68K_REG_ISP);
}

void emu68k_poll_irq(void)
{
    cur_irq = VST_IPL(hw_status());
    m68k_set_irq(cur_irq);
}

/* FIFO overflow repair: the FPGA reports the address range of the writes
 * it had to drop as two strobe-less records (low bound, high bound), and
 * we re-read that range from the real RAM.  The bounds arrive through the
 * same stream as the data, possibly in the in-wait hook below where no bus
 * command may be issued: they are only noted there and the re-read is done
 * from the trace hook. */
static uint32_t rng_lo;             /* low bound waiting for its high bound */
static int      rng_have_lo;
static uint32_t rep_lo, rep_hi;     /* range waiting to be re-read (rng_pending, above) */
/* sn_stat_ranges/repaired/inwait: ranges seen, words re-read, records
 * popped inside a bus wait (declared with the other statistics above) */

static void sn_apply(const vmpu68_snoop_t *r)
{
    if (snlog) {
        snlog_t *e = &snlog[snlog_wp++ & (SNLOG_N - 1)];
        e->seq = evseq++; e->addr = r->addr & ~1u; e->data = r->data;
        e->us = (uint32_t)(vmpu68_ticks() * 1000000ull / vmpu68_tick_hz());
        e->flags = (uint16_t)((r->uds ? 2 : 0) | (r->lds ? 1 : 0) | (r->aux << 2));
        sn_stat_n[VSNOOP_N(r->aux)]++;
        sn_stat_fc[(r->aux >> 2) & 7]++;
        if (!r->uds && !r->lds) sn_stat_nostrobe++;
        if (VSNOOP_N(r->aux) != 3 || (!r->uds && !r->lds))
            sus_ring[sus_wp++ & (SUS_N - 1)] = *e;
    }
    if (!r->uds && !r->lds) {
        if (VSNOOP_N(r->aux) == VSNOOP_RANGE_LO) { rng_lo = r->addr; rng_have_lo = 1; }
        else if (VSNOOP_N(r->aux) == VSNOOP_RANGE_HI && rng_have_lo) {
            /* a second range before the first was re-read: merge */
            if (!rng_pending || rng_lo < rep_lo) rep_lo = rng_lo;
            if (!rng_pending || r->addr > rep_hi) rep_hi = r->addr;
            rng_have_lo = 0; rng_pending = 1; sn_stat_ranges++;
        }
        return;
    }
    /* a real 68000-style bus cycle lasts >= 4 bus clocks (~11 samples);
     * shorter "cycles" are the bus floating while the DMAC takes or
     * releases it and must not touch the shadow */
    if (VSNOOP_N(r->aux) != 3) { sn_stat_dropped++; return; }
    if (map[r->addr >> PAGE_SHIFT] == MAP_SHADOW) {
        if (r->uds) shadow[r->addr] = (uint8_t)(r->data >> 8);
        if (r->lds) shadow[r->addr + 1] = (uint8_t)r->data;
        if (dirty && r->uds && r->lds) dirty_clr(r->addr);   /* real RAM now equals the shadow */
        if (jit_code_map[r->addr >> 8]) jit_note_write(r->addr);
    }
}

static void sn_repair(void)
{
    uint32_t lo = rep_lo & ~1u, hi = rep_hi & ~1u;
    rng_pending = 0;
    for (uint32_t a = lo; a <= hi && a < ADDR_SPACE; a += 2) {
        if (map[a >> PAGE_SHIFT] != MAP_SHADOW) { a = (((a >> PAGE_SHIFT) + 1) << PAGE_SHIFT) - 2; continue; }
        if (dirty && dirty_get(a)) continue;    /* the shadow is newer than the real RAM there */
        uint16_t v;
        if (hw_bus_read(a, 1, &v) & VST_FAULT) { hw_clear_fault(); continue; }
        shadow[a] = (uint8_t)(v >> 8);
        shadow[a + 1] = (uint8_t)v;
        if (jit_code_map[a >> 8]) jit_note_write(a);
        sn_stat_repaired++;
    }
}

/* inside a bus wait (lock held, the DMA holds the bus): keep the FIFO from
 * filling.  Bounded so a never-ending burst cannot starve the caller's
 * completion poll for too long. */
static void sn_inwait_hook(void)
{
    vmpu68_snoop_t r;
    for (int i = 0; i < 64 && hw_snoop_pop_nolock(&r); i++) {
        sn_apply(&r);
        sn_stat_inwait++;
    }
}

int emu68k_snoop_apply(void)
{
    vmpu68_snoop_t r;
    int n = 0;
    while (hw_snoop_pop(&r)) {
        sn_apply(&r);
        n++;
    }
    if (rng_pending) sn_repair();
    return n;
}

unsigned emu68k_rom_check(uint32_t *first)
{
    unsigned bad = 0;
    if (first) *first = ~0u;
    for (uint32_t p = 0; p < N_PAGES; p++) {
        if (map[p] != MAP_ROM || !rom_valid[p]) continue;
        uint32_t base = p << PAGE_SHIFT;
        for (uint32_t o = 0; o < PAGE_SIZE; o += 2) {
            uint16_t v = 0;
            if (hw_bus_read(base + o, 1, &v) & VST_FAULT) { hw_clear_fault(); v = 0xFFFF; }
            if (shadow[base + o] != (uint8_t)(v >> 8) || shadow[base + o + 1] != (uint8_t)v) {
                if (first && *first == ~0u) *first = base + o;
                bad++;
                break;
            }
        }
    }
    return bad;
}

unsigned emu68k_shadow_check(void)
{
    static const uint32_t spots[] = { 0x000000, 0x001000, 0x010000, 0x040000,
                                      0x080000, 0x100000, 0x180000, 0x1FF000,
                                      0x3FF000, 0x5FF000, 0x7FF000, 0xBFF000 };
    unsigned bad = 0;
    for (unsigned i = 0; i < sizeof spots / sizeof spots[0]; i++) {
        if (spots[i] + 128 > ram_size) continue;
        for (uint32_t o = 0; o < 128; o += 2) {
            uint16_t v = 0;
            uint32_t a = spots[i] + o;
            if (hw_bus_read(a, 1, &v) & VST_FAULT) { hw_clear_fault(); bad++; continue; }
            if (shadow[a] != (uint8_t)(v >> 8) || shadow[a + 1] != (uint8_t)v) bad++;
        }
    }
    return bad;
}

void emu68k_rom_invalidate(void)
{
    jit_flush_all();
    memset(rom_valid, 0, sizeof rom_valid);
}

/* initial sync: read current X68000 main RAM into the shadow (slow: one
 * bus cycle per word — run once at attach time) */
/* write one word of main RAM from another core (console key injection):
 * shadow and bus, without the CPU's fault handling (a bus fault here must
 * not longjmp into the emulator core) */
int emu68k_poke16(uint32_t a, unsigned v)
{
    a &= ~1u;
    if (a + 1 >= ram_size || map[a >> PAGE_SHIFT] != MAP_SHADOW) return -1;
    shadow[a] = (uint8_t)(v >> 8);
    shadow[a + 1] = (uint8_t)v;
    if (jit_code_map[a >> 8]) jit_note_write(a);
    if (!wthrough) return 0;
    if (wb_on && dirty) { dirty_set(a); return 0; }
    return (hw_bus_write(a, 1, (uint16_t)v) & VST_FAULT) ? -1 : 0;
}

int emu68k_sync_shadow(uint32_t from, uint32_t len)
{
    jit_flush_all();
    if (from + len > ram_size) return -1;
    for (uint32_t o = 0; o < len; o += 2) {
        uint16_t v = 0;
        if (hw_bus_read(from + o, 1, &v) & VST_FAULT) return -1;
        shadow[from + o] = (uint8_t)(v >> 8);
        shadow[from + o + 1] = (uint8_t)v;
        if (dirty) dirty_clr(from + o);
    }
    return 0;
}
