/* vmpu68 low-level GPIO driver.
 * Linux: mmap /dev/gpiomem.  Bare metal (VMPU68_BAREMETAL): the platform
 * passes the GPIO MMIO base via vmpu68_set_gpio_base(). */
#ifndef VMPU68_BAREMETAL
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#else
#include <stddef.h>
#endif
#include "vmpu68.h"

#define GPIO_LEN 0x1000
static volatile uint32_t *gpio;
static uint32_t last_ctrl;
static uint32_t last_data = 0xFFFFFFFFu;  /* REG0 (write-data latch) image; the FPGA keeps it between cycles */
static uint32_t pf_next = 1;               /* prefetch stream: see vmpu68_bus_read_pf */
static unsigned pf_have;
static uint32_t pf_last = 1;               /* last address read by vmpu68_bus_read_pf */
static unsigned pf_seq;                    /* consecutive sequential words before it */
static int      pf_on = 1;
static uint32_t pf_stat[6];                /* hits, misses, polls, aborts, ended-without-data, (spare) */

#ifdef VMPU68_BAREMETAL
void vmpu68_set_gpio_base(volatile uint32_t *base) { gpio = base; }
extern void hw_port_usleep(unsigned us);
#define VMPU68_USLEEP(us) hw_port_usleep(us)
#else
#define VMPU68_USLEEP(us) usleep(us)
#endif

#define GPFSEL   (gpio + 0)     /* 0x00: function select 0..5 */
#define GPSET0   (gpio[7])
#define GPCLR0   (gpio[10])
#define GPLEV0   (gpio[13])

static uint32_t fsel_in[3], fsel_out[3], fsel_msk[3];   /* AD direction images, see ad_dir_init */
static void fsel(unsigned pin, unsigned mode)   /* 0=in 1=out */
{
    volatile uint32_t *r = GPFSEL + pin / 10;
    unsigned sh = (pin % 10) * 3;
    *r = (*r & ~(7u << sh)) | ((uint32_t)mode << sh);
    if (pin / 10 < 3 && !(fsel_msk[pin / 10] & (7u << sh)))   /* not an AD pin: update the images too */
    {
        fsel_in[pin / 10]  = (fsel_in[pin / 10]  & ~(7u << sh)) | ((uint32_t)mode << sh);
        fsel_out[pin / 10] = (fsel_out[pin / 10] & ~(7u << sh)) | ((uint32_t)mode << sh);
    }
}

/* Minimum time between GPIO protocol edges.  A GPIO register read
 * completes every preceding posted write, but its round trip is NOT
 * clock independent on the Pi 4 (two reads: ~60ns at 600MHz, ~24ns at
 * 1.5GHz, shorter than the FPGA's 61MHz strobe synchroniser needs), so
 * the read is followed by a wait on the ARM generic counter (54MHz). */
static unsigned pace_ns = VMPU68_PACE_NS_DEFAULT;
static unsigned pace_ticks;

#if defined(__aarch64__)
/* the virtual counter is readable from EL0 on Linux as well as at EL1 on
 * bare metal (offset 0 there) */
static inline uint64_t cnt_now(void)
{
    uint64_t v;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}
uint64_t vmpu68_ticks(void) { return cnt_now(); }
uint64_t vmpu68_tick_hz(void)
{
    uint64_t f;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? f : 54000000;
}
static unsigned pace_calc(unsigned ns)
{
    /* +1: a delta of n ticks only proves n-1 full periods elapsed */
    return (unsigned)((ns * vmpu68_tick_hz() + 999999999ull) / 1000000000ull) + 1;
}
static inline void vmpu68_pace(void)
{
    /* device-nGnRnE ordering keeps the strobe writes in issue order; the
     * gap at the pins follows the gap between issues, so no barrier.  The
     * read keeps the old "one device round trip" behaviour as a floor. */
    (void)GPLEV0;
    if (!pace_ticks) pace_ticks = pace_calc(pace_ns);
    uint64_t t0 = cnt_now();
    while (cnt_now() - t0 < pace_ticks) ;
}
#else
static inline void vmpu68_pace(void)
{
    (void)GPLEV0; (void)GPLEV0;
}
uint64_t vmpu68_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t vmpu68_tick_hz(void) { return 1000000000ull; }
#endif

void vmpu68_set_pace_ns(unsigned ns)
{
    pace_ns = ns;
    pace_ticks = 0;
}

unsigned vmpu68_get_pace_ns(void) { return pace_ns; }

/* ---- board layouts (see vmpu68.h) ---- */
static const vmpu68_board_t board_1 = { 1, "core 1.x",
    { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1, 16, 17 },
    18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 0 };
static const vmpu68_board_t board_2 = { 2, "core 2.x",
    { 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23 },
    5, 4, 7, 6, 24, 3, 2, 25, 26, 27, 2 };
static const vmpu68_board_t *bd = &board_1;
/* derived once per layout: single-bit masks for the hot path */
static uint32_t ad_mask, rega_mask, rega0_bit, rega1_bit, wr_bit, rd_bit, irq_bit;
static uint32_t creset_bit, ss_bit, sck_bit, mosi_bit;
static unsigned irq_shift, miso_shift;
static int      ad_contig;                 /* AD0-15 on consecutive GPIOs (2.x): pack = shift */
static unsigned ad_shift;

const vmpu68_board_t *vmpu68_board(void) { return bd; }
int vmpu68_board_id(void) { return bd->id; }

/* ---- SMI transport (see vmpu68.h) ---- */
static volatile uint32_t *smi, *cm;        /* SMI block, clock manager (bare metal only) */
static int      smi_on;
/* Timing in SMI clocks (PLLD 750 MHz / 6 = 125 MHz: 8 ns).  The FPGA samples
 * the strobes at ~61 MHz (16 ns) and captures AD/REG_A on the first clock
 * that sees WR# low, so a write needs the strobe low for > 1 sample and the
 * data held through it; a read is sampled by the SMI at the end of its
 * strobe, after the FPGA has frozen its read mux (~2 clocks).  Verified on
 * core 2.0 (vfy of text/graphic VRAM, tvsweep, Human68k): write 1/3/2 and
 * 1/4/2 clean; kept 1/4/2 and read 1/9/1 for margin.  Tighter settings gain
 * nothing: a transfer costs ~200 ns (write) / ~340 ns (read) of MMIO
 * traffic - DONE polls at 73 ns per read - not strobe time. */
static unsigned smi_div = 6;
static unsigned smi_ws = 1, smi_wst = 4, smi_wh = 2;   /* write: setup / strobe / hold (cycles) */
static unsigned smi_rs = 1, smi_rst = 9, smi_rh = 1;   /* read:  setup / strobe / hold */
static unsigned smi_pace = 2;              /* idle cycles between transfers (strobe high time for the FPGA's synchroniser) */
static uint32_t smi_stat[4];
static int      smi_pend;                  /* a write transfer may still be in flight (completion checked lazily) */
static unsigned smi_flags;                 /* bit0: skip the SMIDA write when the register is unchanged;
                                              bit1: do not clear DONE after a transfer (START clears it) */
static unsigned smi_last_da = ~0u;
/* GPIO images for the hold read while the SMI owns the pins: the SMI cannot
 * keep RD# low while PI_IRQ is polled, so the AD/REG_A/RD# pins are handed
 * to the GPIO block for one hold read (GPFSEL writes are posted, a few ns
 * each) and given back afterwards.  smi_fsel: everything on ALT1;
 * hold_a: REG_A output, AD input, RD# still ALT1 (idle high);
 * hold_b: GPFSEL0 of hold_a with RD# output (its register value is low). */
static uint32_t smi_fsel[3], hold_fsel_a[3], hold_fsel_b;
static void smi_capture(void)
{
    for (unsigned r = 0; r < 3; r++) {
        uint32_t v = GPFSEL[r];
        smi_fsel[r] = v;
        for (int i = 0; i < 16; i++) {
            unsigned pin = bd->ad[i];
            if (pin / 10 == r) v &= ~(7u << ((pin % 10) * 3));       /* AD: input */
        }
        if (bd->rega0 / 10 == r) v = (v & ~(7u << ((bd->rega0 % 10) * 3))) | (1u << ((bd->rega0 % 10) * 3));
        if (bd->rega1 / 10 == r) v = (v & ~(7u << ((bd->rega1 % 10) * 3))) | (1u << ((bd->rega1 % 10) * 3));
        hold_fsel_a[r] = v;
    }
    hold_fsel_b = (hold_fsel_a[bd->rd / 10] & ~(7u << ((bd->rd % 10) * 3))) | (1u << ((bd->rd % 10) * 3));
}
static unsigned smi_rdelay_ticks;          /* spin after a read's START before the first DONE poll (the poll costs a 73 ns MMIO read) */
static unsigned smi_rdelay_ns;
void vmpu68_smi_rdelay(unsigned ns) { smi_rdelay_ns = ns; smi_rdelay_ticks = ns ? pace_calc(ns) : 0; }
unsigned vmpu68_smi_get_rdelay(void) { return smi_rdelay_ns; }
#define SMI_CS   (smi[0x00 / 4])
#define SMI_DSR0 (smi[0x10 / 4])
#define SMI_DSW0 (smi[0x14 / 4])
#define SMI_DC   (smi[0x30 / 4])
#define SMI_DCS  (smi[0x34 / 4])
#define SMI_DA   (smi[0x38 / 4])
#define SMI_DD   (smi[0x3C / 4])
#define SMI_DCS_ENABLE 1u
#define SMI_DCS_START  2u
#define SMI_DCS_DONE   4u
#define SMI_DCS_WRITE  8u
#define SMI_POLL_MAX   20000u
#define CM_SMICTL (cm[0xB0 / 4])
#define CM_SMIDIV (cm[0xB4 / 4])
#define CM_PWD    (0x5Au << 24)
#ifdef VMPU68_BAREMETAL
void vmpu68_set_smi_base(volatile uint32_t *smi_base, volatile uint32_t *cm_base) { smi = smi_base; cm = cm_base; }
#endif
int vmpu68_smi_io(void) { return smi_on; }
void vmpu68_smi_timing(unsigned div, unsigned wsetup, unsigned wstrobe, unsigned whold,
                       unsigned rsetup, unsigned rstrobe, unsigned rhold, unsigned pace)
{
    if (div)     smi_div = div;
    if (wsetup)  smi_ws  = wsetup;
    if (wstrobe) smi_wst = wstrobe;
    if (whold)   smi_wh  = whold;
    if (rsetup)  smi_rs  = rsetup;
    if (rstrobe) smi_rst = rstrobe;
    if (rhold)   smi_rh  = rhold;
    if (pace)    smi_pace = pace;
}
void vmpu68_smi_get(unsigned out[8])
{
    out[0] = smi_div; out[1] = smi_ws; out[2] = smi_wst; out[3] = smi_wh;
    out[4] = smi_rs; out[5] = smi_rst; out[6] = smi_rh; out[7] = smi_pace;
}
void vmpu68_smi_stats(uint32_t out[4], int clear)
{
    for (int i = 0; i < 4; i++) { out[i] = smi_stat[i]; if (clear) smi_stat[i] = 0; }
}
void vmpu68_smi_flags(unsigned f) { smi_flags = f; smi_last_da = ~0u; }
unsigned vmpu68_smi_get_flags(void) { return smi_flags; }
static inline void smi_da(unsigned reg)
{
    if ((smi_flags & 1u) && reg == smi_last_da) return;
    SMI_DA = reg;
    smi_last_da = reg;
}
static inline void smi_done_clr(void)
{
    if (!(smi_flags & 2u)) SMI_DCS = SMI_DCS_ENABLE | SMI_DCS_DONE;
}
static inline void smi_sync(void)
{
    if (!smi_pend) return;
    unsigned n = 0;
    while (!(SMI_DCS & SMI_DCS_DONE)) if (++n > SMI_POLL_MAX) { smi_stat[2]++; break; }
    smi_done_clr();
    smi_pend = 0;
}
static inline void smi_wr(unsigned reg, uint16_t val)
{
    if (!(smi_flags & 4u)) smi_sync();     /* bit2: back-to-back writes without waiting for DONE (experiment) */
    smi_da(reg);
    SMI_DD = val;
    SMI_DCS = SMI_DCS_ENABLE | SMI_DCS_WRITE | SMI_DCS_START;
    smi_pend = 1;
    smi_stat[0]++;
}
static inline uint16_t smi_rd(unsigned reg)
{
    smi_sync();
    smi_da(reg);
    SMI_DCS = SMI_DCS_ENABLE | SMI_DCS_START;
    unsigned n = 0;
    if (smi_rdelay_ticks) { uint64_t t0 = cnt_now(); while (cnt_now() - t0 < smi_rdelay_ticks) ; }
    while (!(SMI_DCS & SMI_DCS_DONE)) if (++n > SMI_POLL_MAX) { smi_stat[2]++; break; }
    smi_stat[3] += n;                      /* DONE polls that found the read still running */
    uint32_t v = SMI_DD;
    smi_done_clr();
    smi_stat[1]++;
    return (uint16_t)v;
}
/* micro-benchmark (ns per op): [0] SMIDCS read, [1] SMIDA write (posted, one
 * read at the end), [2] register write (CTRL, fields restored by the caller),
 * [3] register read (STATUS), [4] GPIO level read for comparison */
void vmpu68_smi_bench(uint32_t out[5])
{
    const unsigned N = 10000;
    uint64_t hz = vmpu68_tick_hz(), t0, t1;
    volatile uint32_t sink = 0;
    if (!smi_on) { for (int i = 0; i < 5; i++) out[i] = 0; return; }
    smi_sync();
    t0 = cnt_now(); for (unsigned i = 0; i < N; i++) sink += SMI_DCS; t1 = cnt_now();
    out[0] = (uint32_t)((t1 - t0) * 1000000000ull / hz / N);
    t0 = cnt_now(); for (unsigned i = 0; i < N; i++) SMI_DA = i & 3; sink += SMI_DCS; t1 = cnt_now();
    out[1] = (uint32_t)((t1 - t0) * 1000000000ull / hz / N);
    smi_last_da = ~0u;
    uint16_t c = (uint16_t)(last_ctrl & 0xFFFF);
    t0 = cnt_now(); for (unsigned i = 0; i < N; i++) smi_wr(VREG_CTRL, c); smi_sync(); t1 = cnt_now();
    out[2] = (uint32_t)((t1 - t0) * 1000000000ull / hz / N);
    t0 = cnt_now(); for (unsigned i = 0; i < N; i++) sink += smi_rd(VREG_STATUS); t1 = cnt_now();
    out[3] = (uint32_t)((t1 - t0) * 1000000000ull / hz / N);
    t0 = cnt_now(); for (unsigned i = 0; i < N; i++) sink += GPLEV0; t1 = cnt_now();
    out[4] = (uint32_t)((t1 - t0) * 1000000000ull / hz / N);
    (void)sink;
}

static void board_apply(void)
{
    ad_mask = 0;
    for (int i = 0; i < 16; i++) ad_mask |= 1u << bd->ad[i];
    rega0_bit = 1u << bd->rega0; rega1_bit = 1u << bd->rega1; rega_mask = rega0_bit | rega1_bit;
    wr_bit = 1u << bd->wr; rd_bit = 1u << bd->rd; irq_bit = 1u << bd->irq; irq_shift = bd->irq;
    creset_bit = 1u << bd->creset; ss_bit = 1u << bd->ss; sck_bit = 1u << bd->sck; mosi_bit = 1u << bd->mosi;
    miso_shift = bd->miso;
    ad_contig = 1;
    for (int i = 1; i < 16; i++) if (bd->ad[i] != bd->ad[0] + i) ad_contig = 0;
    ad_shift = bd->ad[0];
}

void vmpu68_set_board(int id)
{
    bd = (id == 2) ? &board_2 : &board_1;
    board_apply();
}

static inline uint32_t ad_pack(uint16_t v)
{
    if (ad_contig) return (uint32_t)v << ad_shift;
    return (((uint32_t)v & 0x0FFF) << 2) |        /* 1.x: AD0-11 -> 2..13, AD12/13 -> 0/1, AD14/15 -> 16/17 */
           (((uint32_t)v >> 12) & 3) |
           ((((uint32_t)v >> 14) & 3) << 16);
}

static inline uint16_t ad_unpack(uint32_t g)
{
    if (ad_contig) return (uint16_t)(g >> ad_shift);
    return (uint16_t)(((g >> 2) & 0x0FFF) |
                      ((g & 3) << 12) |
                      (((g >> 16) & 3) << 14));
}

static inline uint32_t rega_bits(unsigned reg)
{
    return ((reg & 1) ? rega0_bit : 0) | ((reg & 2) ? rega1_bit : 0);
}

/* AD pins span up to three GPFSEL registers (1.x: 0-13,16,17; 2.x: 8-23).
 * Precomputed register images make a direction switch two or three MMIO
 * writes instead of 16 read-modify-writes.  fsel() keeps the images in
 * step for the non-AD pins that share a register (2.x: CRESET/SS in
 * GPFSEL0, IRQ/SPI in GPFSEL2). */
static int ad_is_out;
static void smi_pins(int on);

static void ad_dir_init(void)
{
    for (unsigned r = 0; r < 3; r++)
    {
        uint32_t msk = 0, out = 0;
        for (int i = 0; i < 16; i++)
        {
            unsigned pin = bd->ad[i];
            if (pin / 10 != r) continue;
            msk |= 7u << ((pin % 10) * 3);
            out |= 1u << ((pin % 10) * 3);
        }
        fsel_msk[r] = msk;
        uint32_t base = GPFSEL[r] & ~msk;
        fsel_in[r]  = base;
        fsel_out[r] = base | out;
    }
    ad_is_out = 1;                            /* force the first switch */
}

static inline void ad_dir_out(int out)
{
    if (out == ad_is_out || smi_on) return;
    if (fsel_msk[0]) GPFSEL[0] = out ? fsel_out[0] : fsel_in[0];
    if (fsel_msk[1]) GPFSEL[1] = out ? fsel_out[1] : fsel_in[1];
    if (fsel_msk[2]) GPFSEL[2] = out ? fsel_out[2] : fsel_in[2];
    ad_is_out = out;
}

int vmpu68_open(void)
{
#ifndef VMPU68_BAREMETAL
    int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/gpiomem"); return -1; }
    gpio = mmap(NULL, GPIO_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (gpio == MAP_FAILED) { perror("mmap"); return -1; }
#else
    if (gpio == NULL) return -1;
#endif

    if (!ad_mask) board_apply();           /* default layout when nobody selected one */
    GPSET0 = wr_bit | rd_bit;
    fsel(bd->wr, 1);
    fsel(bd->rd, 1);
    fsel(bd->rega0, 1);
    fsel(bd->rega1, 1);
    fsel(bd->irq, 0);

    /* flash pins idle: CRESET_B released (input w/ external pullup), SS high */
    fsel(bd->creset, 0);
    GPSET0 = ss_bit;
    fsel(bd->ss, 1);
    fsel(bd->sck, 1);
    GPCLR0 = sck_bit;
    fsel(bd->mosi, 1);
    fsel(bd->miso, 0);
    /* AD direction images last: they capture the other pins' settings */
    ad_dir_init();
    ad_dir_out(0);
    last_ctrl = last_data = 0xFFFFFFFFu;   /* force CTRL/DATA writes on the first cycle */
    if (smi_on) { smi_pend = 0; smi_last_da = ~0u; smi_pins(1); }   /* re-opened (FPGA re-probe): SMI keeps the pins */
    return 0;
}

/* SMI pins: SA1/SA0 = GPIO4/5 (REG_A1/A0), SOE_N/SWE_N = GPIO6/7 (RD#/WR#),
 * SD0-15 = GPIO8-23 (AD0-15) - the 2.x layout was drawn for this (ALT1) */
static void smi_pins(int on)
{
    if (on) {
        for (int i = 0; i < 16; i++) fsel(bd->ad[i], 5);
        fsel(bd->rega0, 5); fsel(bd->rega1, 5);
        fsel(bd->wr, 5); fsel(bd->rd, 5);
        ad_is_out = -1;
        smi_capture();
    } else {
        GPSET0 = wr_bit | rd_bit;          /* idle high the moment they become outputs */
        fsel(bd->wr, 1); fsel(bd->rd, 1); fsel(bd->rega0, 1); fsel(bd->rega1, 1);
        ad_dir_init();                     /* AD back to input (images re-captured) */
        ad_dir_out(0);
    }
}

int vmpu68_set_smi(int on)
{
    if (!on) {
        if (!smi_on) return 0;
        smi_sync();
        SMI_DCS = 0;
        smi_on = 0;
        smi_pins(0);
        return 0;
    }
    if (smi_on) return 0;
    if (!smi || !cm || bd->id != 2 || bd->ad[0] != 8 || bd->rega0 != 5 || bd->rega1 != 4 || bd->rd != 6 || bd->wr != 7)
        return -1;
    /* SMI clock: PLLD (750 MHz) / smi_div.  Stop, set the divider, restart. */
    CM_SMICTL = CM_PWD | (1u << 5);        /* KILL: stop it whatever state it is in */
    VMPU68_USLEEP(10);
    CM_SMIDIV = CM_PWD | ((smi_div & 0xFFFu) << 12);
    CM_SMICTL = CM_PWD | 6u;               /* source PLLD */
    VMPU68_USLEEP(10);
    CM_SMICTL = CM_PWD | (1u << 4) | 6u;   /* ENAB */
    for (unsigned i = 0; i < 1000 && !(CM_SMICTL & (1u << 7)); i++) VMPU68_USLEEP(1);
    /* device 0 timing, 16-bit wide.  setup: SA/SD stable before the strobe;
     * strobe: SWE_N/SOE_N low; hold: SA/SD kept after; pace: idle before
     * the next transfer.  All in SMI clocks. */
    SMI_CS   = 0;
    SMI_DC   = 0;
    SMI_DSW0 = (1u << 30) | ((smi_ws & 0x3Fu) << 24) | ((smi_wh & 0x3Fu) << 16) | ((smi_pace & 0x7Fu) << 8) | (smi_wst & 0x7Fu);
    SMI_DSR0 = (1u << 30) | ((smi_rs & 0x3Fu) << 24) | ((smi_rh & 0x3Fu) << 16) | ((smi_pace & 0x7Fu) << 8) | (smi_rst & 0x7Fu);
    SMI_DCS  = SMI_DCS_ENABLE | SMI_DCS_DONE;
    SMI_DA   = 0;
    smi_pend = 0;
    smi_last_da = 0;
    /* strobes are idle high on the SMI side before the pins move over */
    smi_pins(1);
    smi_on = 1;
    return 0;
}

/* every pin of the current layout back to input (before trying another layout) */
static void release_pins(void)
{
    for (int i = 0; i < 16; i++) fsel(bd->ad[i], 0);
    fsel(bd->wr, 0); fsel(bd->rd, 0); fsel(bd->rega0, 0); fsel(bd->rega1, 0);
    fsel(bd->ss, 0); fsel(bd->sck, 0); fsel(bd->mosi, 0);
    ad_is_out = 0;
}

int vmpu68_probe_board(void)
{
    static const int order[2] = { 2, 1 };     /* 2.x first: its probe only touches AD lines of a 1.x board */
    for (unsigned i = 0; i < 2; i++)
    {
        vmpu68_set_board(order[i]);
        if (vmpu68_open() != 0) return 0;
        unsigned hits = 0;
        for (unsigned k = 0; k < 3; k++)
            if ((vmpu68_reg_read(VREG_CTRL) >> 8) == 0x56) hits++;
        if (hits == 3) return bd->id;
        release_pins();
    }
    return 0;
}

/* CTRL (fc/rw/word/addr[23:16]) is only rewritten when it changes; with
 * autostart the ADDR_LO write starts the cycle.  hw_init() enables the
 * mode in REG3 after the FPGA probe. */
static inline void set_ctrl(uint16_t ctrl)
{
    if (ctrl != last_ctrl) { vmpu68_reg_write(VREG_CTRL, ctrl); last_ctrl = ctrl; }
}
static int wr_valid;
void vmpu68_ctrl_invalidate(void) { last_ctrl = 0xFFFFFFFFu; last_data = 0xFFFFFFFFu; wr_valid = 0; }

static int autostart_mode = 1;         /* 1 = ADDR_LO write starts the cycle */
void vmpu68_set_autostart_io(int on) { autostart_mode = on; last_ctrl = 0xFFFFFFFFu; }

/* ai write mode (CTRL bit15, see pi_if.v): the DATA strobe starts the write
 * and the FPGA then steps its address by the access size, up or down
 * (VCTRL_WR_DEC).  wr_fpga mirrors that address; a write landing exactly
 * there with the same CTRL image is one strobe.  Anything else that writes
 * ADDR_LO or CTRL (reads, probes) makes the mirror stale: vmpu68_reg_write
 * drops wr_valid, issue_write sets it again.  Two thirds of the emulator's
 * writes are +-2 runs (stack frames, fills, copies). */
static int wr_ai_mode;
static uint32_t wr_fpga;               /* FPGA cyc_addr after its last ai write (24-bit) */
static uint32_t wr_last;               /* address of that write */
#define WR_AI_ON (wr_ai_mode && autostart_mode)
void vmpu68_set_wr_ai_io(int on) { wr_ai_mode = on; wr_valid = 0; last_ctrl = 0xFFFFFFFFu; }
int  vmpu68_wr_ai_io(void) { return wr_ai_mode; }

/* busy_irq completion (see vmpu68.h).  wt_bit is OR-ed into the CTRL of
 * every waited command so the FPGA raises PI_IRQ for it; posted writes
 * never carry it.  line_held: the other reasons for a high line (interrupt
 * pending, DMA snoop records, sticky fault), as of the last STATUS read
 * from anywhere - while one of them is set the line cannot signal
 * completion, so go straight to STATUS instead of burning VMPU68_PIN_POLLS
 * on every access.  It clears itself: the emulator reads STATUS every few
 * instructions while the line is high, and a waited access whose pin poll
 * fails ends in STATUS too. */
static int busy_irq_mode;
static uint16_t wt_bit;
static uint16_t line_held;
static uint16_t held_mask = VST_FAULT;    /* + ipl/snoop while irq_en is on */
void vmpu68_set_busy_irq_io(int on) { busy_irq_mode = on; wt_bit = on ? VCTRL_WAIT : 0; last_ctrl = 0xFFFFFFFFu; }
void vmpu68_set_irq_en_io(int en)
{
    held_mask = (uint16_t)(en ? (VST_FAULT | VST_SNOOP | (7u << 3)) : VST_FAULT);
    line_held &= held_mask;
}
int  vmpu68_busy_irq_io(void) { return busy_irq_mode; }

void vmpu68_close(void)
{
#ifndef VMPU68_BAREMETAL
    if (gpio) munmap((void *)gpio, GPIO_LEN);
#endif
    gpio = NULL;
}

static void set_rega(unsigned reg)
{
    uint32_t set = 0, clr = 0;
    if (reg & 1) set |= rega0_bit; else clr |= rega0_bit;
    if (reg & 2) set |= rega1_bit; else clr |= rega1_bit;
    if (set) GPSET0 = set;
    if (clr) GPCLR0 = clr;
}

void vmpu68_reg_write(unsigned reg, uint16_t val)
{
    /* REG0 is a plain latch sampled at the cycle start: a write of the value
     * it already holds is a no-op (fills, stack frames, cleared words) -
     * except in ai mode, where the strobe itself starts the cycle */
    if (reg == VREG_DATA) { if (val == last_data && !WR_AI_ON) return; last_data = val; }
    else if (reg != VREG_STATUS) wr_valid = 0;   /* ADDR_LO/CTRL: the FPGA's write address moved */
    if (smi_on) { smi_wr(reg, val); return; }
    /* REG_A rides in the same GPSET/GPCLR pair as the data (one MMIO write
     * to the GPIO block is ~30ns; a bus access is 6-8 of them) */
    uint32_t bits = ad_pack(val) | rega_bits(reg);
    ad_dir_out(1);                         /* stays output until the next read */
    GPSET0 = bits;
    GPCLR0 = ~bits & (ad_mask | rega_mask);
    vmpu68_pace();                         /* AD/REG_A settled at the FPGA before WR# */
    GPCLR0 = wr_bit;
    vmpu68_pace();                         /* WR# low ~100ns (FPGA syncs @61MHz) */
    GPSET0 = wr_bit;
}

uint16_t vmpu68_reg_read(unsigned reg)
{
    uint32_t v;
    if (smi_on) {
        v = smi_rd(reg);
        if (reg == VREG_STATUS) line_held = (uint16_t)(v & held_mask);
        return (uint16_t)v;
    }
    set_rega(reg);
    /* the FPGA (38MHz @ 10MHz bus) must sample the previous strobe HIGH
     * at least once and latch REG_A before RD# falls again: a ~30ns
     * turnaround made a STATUS read count as a REG1 read (snoop phase slip).
     * After a write the two GPFSEL writes of the direction switch (~60ns
     * at the pins, in issue order) already provide that gap. */
    if (ad_is_out) ad_dir_out(0);
    else           vmpu68_pace();
    GPCLR0 = rd_bit;
    vmpu68_pace(); vmpu68_pace();          /* RD# at the FPGA, mux settled */
    v = GPLEV0;
    GPSET0 = rd_bit;
    v = ad_unpack(v);
    if (reg == VREG_STATUS) line_held = (uint16_t)(v & held_mask);
    return (uint16_t)v;
}

int vmpu68_irq_pin(void)
{
    return (GPLEV0 >> irq_shift) & 1;
}
static inline int pin_poll(void) { return (GPLEV0 >> irq_shift) & 1; }

/* Delay before the first STATUS poll of wait_done().  The FPGA raises
 * busy ~5 clocks (~80ns) after the CTRL write is synchronised (polling
 * earlier returns a stale DATA), and the bus cycle itself takes at least
 * 4 CLK plus synchronisers, so every poll that finds busy set is a wasted
 * ~350ns GPIO round trip.  Waiting about one bus cycle up front makes a
 * read need a single poll in the common case.  0 = the original three
 * paces. */
static unsigned wait_ns = VMPU68_WAIT_NS_DEFAULT;
static unsigned wait_ticks;
static uint32_t poll_hist[VMPU68_POLL_HIST];  /* [n]: wait_done calls that took n polls */
/* phase profile of waited reads (ticks): 0 start (CTRL+ADDR writes), 1 wait_pre,
 * 2 poll/STATUS, 3 DATA read; [4] = count */
static uint64_t wprof[12];             /* + [5] hold reads completed on the line,
                                          [6] line never seen high, [7] seen high, not low;
                                          ai writes: [8] total, [9] 1 strobe, [10] 2 (ADDR+DATA),
                                          [11] 3 (CTRL too) */
void vmpu68_wait_prof(uint64_t out[12], int clear)
{
    for (int i = 0; i < 12; i++) { out[i] = wprof[i]; if (clear) wprof[i] = 0; }
}

void vmpu68_set_wait_ns(unsigned ns)
{
    wait_ns = ns;
    wait_ticks = 0;
}

unsigned vmpu68_get_wait_ns(void) { return wait_ns; }

void vmpu68_poll_hist(uint32_t *out, int clear)
{
    for (unsigned i = 0; i < VMPU68_POLL_HIST; i++) {
        out[i] = poll_hist[i];
        if (clear) poll_hist[i] = 0;
    }
}

static void wait_pre(void)
{
#if defined(__aarch64__)
    if (wait_ns) {
        (void)GPLEV0;                      /* the ADDR_LO strobe has reached the pins */
        if (!wait_ticks) wait_ticks = pace_calc(wait_ns);
        uint64_t t0 = cnt_now();
        while (cnt_now() - t0 < wait_ticks) ;
    } else
#endif
    { vmpu68_pace(); vmpu68_pace(); vmpu68_pace(); }
}

/* Busy polls are bounded (~5 s): when the X68000 is switched off the FPGA
 * clock stops (the Pi's 5V keeps the FPGA alive through the ideal diode) and
 * STATUS freezes - with busy=1 an unbounded wait would hang the caller until
 * the machine comes back.  A timed-out wait returns STATUS with busy still
 * set; the supervisor notices the stopped clock and stops the emulator. */
#define VMPU68_BUSY_POLL_MAX (1u << 24)

/* wait for the last command.  Returns STATUS, or 0 when completion was
 * seen on the PI_IRQ line (then: no fault, no VPA - callers needing the
 * VPA bit use wait_done_st()).  poll_hist[0] counts the line hits. */
/* the STATUS loop shared by every completion path; see vmpu68_set_snoop_hook */
static void (*snoop_hook)(void);
static int snoop_hook_core = -1;           /* -1 = any core may run the hook */
void vmpu68_set_snoop_hook(void (*fn)(void)) { snoop_hook = fn; }
void vmpu68_set_snoop_hook_core(int core) { snoop_hook_core = core; }
static inline int on_hook_core(void)
{
#if defined(__aarch64__)
    if (snoop_hook_core >= 0) { uint64_t m; asm volatile("mrs %0, mpidr_el1" : "=r"(m)); return (int)(m & 0xFF) == snoop_hook_core; }
#endif
    return 1;
}
static uint16_t busy_poll(void)
{
    uint16_t st;
    unsigned n = 0;
    /* the hook writes the emulator's RAM shadow: only its own core may run
     * it (the write-queue worker and the console/Web core poll here too) */
    int hook_ok = snoop_hook && on_hook_core();
    for (;;) {
        st = vmpu68_reg_read(VREG_STATUS); n++;
        if (!(st & VST_BUSY) || n >= VMPU68_BUSY_POLL_MAX) break;
        if ((st & VST_SNOOP) && hook_ok) snoop_hook();
    }
    poll_hist[n < VMPU68_POLL_HIST ? n : VMPU68_POLL_HIST - 1]++;
    return st;
}

static uint16_t wait_done_st(void)
{
    wait_pre();
    return busy_poll();
}

static uint16_t wait_done(void)
{
    uint16_t st;
    unsigned n;
    if (!busy_irq_mode || line_held)
        return wait_done_st();
    /* Two phases, no fixed delay: the strobe that started the command sits
     * in the CPU's write buffer for an unpredictable 100..400ns and the
     * FPGA raises the line ~100ns after it lands, so "low" only means
     * completion once the line has been seen high.  A miss of the high
     * phase (never seen on a >300ns bus cycle) just ends in STATUS. */
    uint64_t t0 = cnt_now();
    for (n = 0; n < VMPU68_PIN_UP_POLLS; n++)
        if (pin_poll()) break;
    uint64_t t1 = cnt_now();
    wprof[1] += t1 - t0;
    if (n < VMPU68_PIN_UP_POLLS)
        for (n = 0; n < VMPU68_PIN_POLLS; n++)
            if (!pin_poll()) { poll_hist[0]++; wprof[2] += cnt_now() - t1; return 0; }
    wait_pre();
    /* still high: a long cycle (DMA holding the bus, slow device), a
     * fault, or an interrupt/snoop record - STATUS tells */
    st = busy_poll();
    return st;                             /* line_held now reflects why it stayed high */
}

/* "hold read": the completion poll and the DATA read in one.  RD# is
 * taken low on REG0 right after the command is issued and stays low while
 * PI_IRQ is polled; the FPGA drives the DATA mux asynchronously, so the
 * GPLEV sample that first shows the line low already carries the word
 * (rdata / the FIFO head is latched on the edge that ends the cycle and the
 * line drops two clocks later).  Saves the separate DATA cycle (~350ns).
 * Returns 1 with *out on line completion (no fault, no VPA), 0 when the
 * line did not complete within the polls - RD# is released and the caller
 * falls back to STATUS (the extra RD# rise only pops a FIFO word the
 * command pickup flushes anyway). */
static int hold_read(uint16_t *out)
{
    uint32_t v;
    unsigned n;
    if (smi_on) {
        /* hybrid hold read: pins to the GPIO block for the wait (see smi_capture) */
        smi_sync();                            /* the command strobe has left the SMI */
        GPCLR0 = rega_mask | rd_bit;           /* REG_A = DATA, RD# register low (drives when it becomes an output) */
        GPFSEL[0] = hold_fsel_a[0];            /* REG_A out, AD8/9 in */
        GPFSEL[1] = hold_fsel_a[1];            /* AD in */
        GPFSEL[2] = hold_fsel_a[2];
        GPFSEL[0] = hold_fsel_b;               /* RD# falls, REG_A settled two writes earlier */
        uint64_t t0 = cnt_now();
        for (n = 0; n < VMPU68_PIN_UP_POLLS; n++)
            if (pin_poll()) break;
        uint64_t t1 = cnt_now();
        wprof[1] += t1 - t0;
        int ok = 0;
        if (n < VMPU68_PIN_UP_POLLS) {
            for (n = 0; n < VMPU68_PIN_POLLS; n++) {
                v = GPLEV0;
                if (!(v & irq_bit)) {
                    *out = ad_unpack(v);
                    poll_hist[0]++; wprof[2] += cnt_now() - t1; wprof[5]++;
                    ok = 1;
                    break;
                }
            }
            if (!ok) wprof[7]++;
        } else
            wprof[6]++;
        GPSET0 = rd_bit;                       /* RD# high as a GPIO, then back to the SMI (idle high) */
        GPFSEL[0] = smi_fsel[0];
        GPFSEL[1] = smi_fsel[1];
        GPFSEL[2] = smi_fsel[2];
        return ok;
    }
    set_rega(VREG_DATA);
    ad_dir_out(0);                         /* always: the command write left the pins output */
    /* REG_A reaches the FPGA before RD#: it was issued two GPFSEL writes
     * earlier, and the last strobe was the command's WR#, not RD# */
    GPCLR0 = rd_bit;
    uint64_t t0 = cnt_now();
    for (n = 0; n < VMPU68_PIN_UP_POLLS; n++)
        if (pin_poll()) break;
    uint64_t t1 = cnt_now();
    wprof[1] += t1 - t0;
    if (n < VMPU68_PIN_UP_POLLS) {
        for (n = 0; n < VMPU68_PIN_POLLS; n++) {
            v = GPLEV0;
            if (!(v & irq_bit)) {
                GPSET0 = rd_bit;
                *out = ad_unpack(v);
                poll_hist[0]++; wprof[2] += cnt_now() - t1; wprof[5]++;
                return 1;
            }
        }
        wprof[7]++;
    } else
        wprof[6]++;
    GPSET0 = rd_bit;
    return 0;
}

/* STATUS fallback of a failed hold_read: the strobe has long landed (the
 * polls drained the write buffer), so no wait_pre */
static uint16_t hold_read_fallback(uint16_t *out)
{
    uint16_t st = busy_poll();
    *out = vmpu68_reg_read(VREG_DATA);
    return st;
}

uint16_t vmpu68_bus_read_fc(uint32_t addr, int word, unsigned fc, uint16_t *out)
{
    uint16_t st;
    pf_next = 1;
    uint16_t ctrl = (uint16_t)(VCTRL_FC(fc) | VCTRL_RW_READ | (word ? VCTRL_WORD : 0) | ((addr >> 16) & 0xFF) | wt_bit);
    uint64_t t0 = cnt_now();
    if (autostart_mode) {
        set_ctrl(ctrl);
        vmpu68_reg_write(VREG_ADDR_LO, (uint16_t)addr);     /* starts the cycle */
    } else {
        vmpu68_reg_write(VREG_ADDR_LO, (uint16_t)addr);
        vmpu68_reg_write(VREG_CTRL, ctrl);
    }
    uint64_t t1 = cnt_now();
    wprof[0] += t1 - t0; wprof[4]++;
    if (fc == 5 && busy_irq_mode && !line_held) {
        if (hold_read(out)) return 0;
        uint64_t t2 = cnt_now();
        st = hold_read_fallback(out);
        wprof[3] += cnt_now() - t2;
        return st;
    }
    st = (fc == 5) ? wait_done() : wait_done_st();   /* IACK: the VPA bit matters */
    uint64_t t2 = cnt_now();
    *out = vmpu68_reg_read(VREG_DATA);
    wprof[3] += cnt_now() - t2;
    return st;
}

/* latency probe (bench): one waited byte read of addr; out[0] = ticks from
 * before the ADDR_LO write until PI_IRQ was first seen high, [1] until it
 * was seen low again, [2] until STATUS reported !busy (read after the line
 * dropped, so this only adds the STATUS read itself), [3] pin polls made.
 * Requires busy_irq mode and an idle line (irq_en off). */
void vmpu68_lat_probe(uint32_t addr, uint32_t out[4])
{
    uint16_t ctrl = (uint16_t)(VCTRL_FC(5) | VCTRL_RW_READ | ((addr >> 16) & 0xFF) | VCTRL_WAIT);
    uint32_t polls = 0;
    pf_next = 1;
    vmpu68_reg_write(VREG_CTRL, ctrl); last_ctrl = ctrl;
    (void)GPLEV0;                          /* drained: the timing starts at the pins */
    uint64_t t0 = cnt_now();
    vmpu68_reg_write(VREG_ADDR_LO, (uint16_t)addr);
    while (!pin_poll()) { if (++polls > 100000) break; }
    uint64_t t1 = cnt_now();
    while (pin_poll())  { if (++polls > 200000) break; }
    uint64_t t2 = cnt_now();
    uint16_t st;
    do { st = vmpu68_reg_read(VREG_STATUS); if (++polls > 300000) break; } while (st & VST_BUSY);
    uint64_t t3 = cnt_now();
    (void)vmpu68_reg_read(VREG_DATA);
    out[0] = (uint32_t)(t1 - t0); out[1] = (uint32_t)(t2 - t0); out[2] = (uint32_t)(t3 - t0);
    out[3] = polls | ((uint32_t)st << 20);
}

uint16_t vmpu68_bus_read(uint32_t addr, int word, uint16_t *out)
{
    pf_next = 1;                           /* any other command flushes the stream */
    return vmpu68_bus_read_fc(addr, word, 5, out);
}

/* ---- prefetch stream ----
 * pf_next: byte address the FPGA's FIFO head corresponds to (odd = none);
 * pf_have: words known to be buffered from the last STATUS read (a lower
 * bound: the FPGA keeps fetching, and only the Pi's own commands flush). */

void vmpu68_pf_enable(int on) { pf_on = on; pf_next = 1; pf_have = 0; }
int  vmpu68_pf_enabled(void)  { return pf_on; }
void vmpu68_pf_stats(uint32_t out[6], int clear)
{
    for (int i = 0; i < 6; i++) { out[i] = pf_stat[i]; if (clear) pf_stat[i] = 0; }
}

uint16_t vmpu68_bus_read_pf(uint32_t addr, uint16_t *out)
{
    uint16_t st, ctrl;
    unsigned n = 0;
    addr &= 0xFFFFFEu;
    if (!pf_on) { pf_next = 1; return vmpu68_bus_read_fc(addr, 1, 5, out); }
    if (addr == pf_next) {
        if (!pf_have) {
            /* the next fetch is normally done before the emulator gets here:
             * poll right away, no pre-delay */
            do { st = vmpu68_reg_read(VREG_STATUS); n++; }
            while (!VST_PF_CNT(st) && (st & VST_PF_ACTIVE) && n < 4096);
            pf_stat[2] += n;
            pf_have = VST_PF_CNT(st);
            if (!pf_have) { pf_stat[3]++; goto restart; }   /* stream ended (fault/BR/64KB) */
        }
        *out = vmpu68_reg_read(VREG_DATA);   /* pops the FIFO head */
        pf_have--;
        pf_next = addr + 2;
        pf_last = addr;
        pf_stat[0]++;
        return 0;
    }
restart:
    /* A stream costs bus time (the FPGA fetches up to 8 words ahead) and a
     * command arriving while a speculative cycle runs waits for it: only
     * start one on the third consecutive sequential word, so random
     * access patterns (vertical VRAM walks, table lookups) pay nothing. */
    pf_seq = (addr == pf_last + 2) ? pf_seq + 1 : 0;
    pf_last = addr;
    pf_have = 0;
    pf_next = 1;
    pf_stat[1]++;
    ctrl = (uint16_t)(VCTRL_FC(5) | VCTRL_RW_READ | VCTRL_WORD | ((addr >> 16) & 0xFF) | wt_bit);
    if (pf_seq >= 2) ctrl |= VCTRL_PF;
    if (autostart_mode) {
        set_ctrl(ctrl);
        vmpu68_reg_write(VREG_ADDR_LO, (uint16_t)addr);
    } else {
        vmpu68_reg_write(VREG_ADDR_LO, (uint16_t)addr);
        vmpu68_reg_write(VREG_CTRL, ctrl);
    }
    if (!(ctrl & VCTRL_PF) && busy_irq_mode && !line_held) {
        /* plain command: hold read.  NOT for a VCTRL_PF command: its word
         * goes to the FIFO, and releasing RD# on a miss pops the head -
         * if the cycle ended between the last poll and the release that
         * is our word (seen on hardware as FFFF / the next word, ~1% of
         * TVRAM reads with the emulator posting writes).  rdata cannot
         * be popped.  Stream restarts are rare (misses ~0.01%), so the
         * STATUS path costs nothing overall. */
        if (hold_read(out)) return 0;
        return hold_read_fallback(out);
    }
    if (!(ctrl & VCTRL_PF)) {
        st = wait_done();
        *out = vmpu68_reg_read(VREG_DATA);
        return st;
    }
    wait_pre();
    /* the command's own cycle: wait for busy to clear.  The FIFO is only
     * flushed when the engine picks the command up, so until then pf_cnt
     * still counts the words of the PREVIOUS stream (another thread's, or
     * our own before a jump) whenever a speculative fetch was in flight:
     * leaving on pf_cnt != 0 handed out one of those stale words.  Our word
     * lands in the FIFO in the same clock busy drops, so this costs nothing;
     * busy clearing with an empty FIFO means the cycle ended without data. */
    do { st = vmpu68_reg_read(VREG_STATUS); n++; }
    while ((st & VST_BUSY) && n < VMPU68_BUSY_POLL_MAX);
    poll_hist[n < VMPU68_POLL_HIST ? n : VMPU68_POLL_HIST - 1]++;
    if (!VST_PF_CNT(st)) { *out = 0xFFFF; return st; }
    *out = vmpu68_reg_read(VREG_DATA);
    pf_have = VST_PF_CNT(st) - 1;
    pf_next = addr + 2;
    return st;
}

/* one write command (posted unless wt); see vmpu68_set_wr_ai_io */
static void issue_write(uint32_t addr, int word, uint16_t data, uint16_t wt)
{
    uint16_t base = (uint16_t)(VCTRL_FC(5) | (word ? VCTRL_WORD : 0) | ((addr >> 16) & 0xFF) | wt);
    pf_next = 1;
    if (WR_AI_ON) {
        uint32_t step = word ? 2u : 1u;
        addr &= 0xFFFFFFu;
        /* keep the FPGA's direction unless this write reveals a run the
         * other way: the CTRL write then costs what the ADDR_LO write would */
        uint16_t ctrl = (uint16_t)(base | VCTRL_AI | (last_ctrl & VCTRL_WR_DEC));
        wprof[8]++;
        if (wr_valid && wr_fpga == addr && ctrl == last_ctrl) {
            wprof[9]++;
            vmpu68_reg_write(VREG_DATA, data);              /* starts the cycle at wr_fpga */
        } else {
            if (wr_valid && addr == ((wr_last + step) & 0xFFFFFFu))      ctrl = (uint16_t)(base | VCTRL_AI);
            else if (wr_valid && addr == ((wr_last - step) & 0xFFFFFFu)) ctrl = (uint16_t)(base | VCTRL_AI | VCTRL_WR_DEC);
            if (ctrl == last_ctrl) wprof[10]++; else wprof[11]++;
            set_ctrl(ctrl);
            vmpu68_reg_write(VREG_ADDR_LO, (uint16_t)addr); /* address only */
            vmpu68_reg_write(VREG_DATA, data);              /* starts the cycle */
        }
        wr_last  = addr;
        wr_fpga  = ((ctrl & VCTRL_WR_DEC) ? addr - step : addr + step) & 0xFFFFFFu;
        last_ctrl = (last_ctrl & 0xFF00u) | ((wr_fpga >> 16) & 0xFF);  /* CTRL[7:0] follows the FPGA's address */
        wr_valid = 1;
        return;
    }
    vmpu68_reg_write(VREG_DATA, data);
    if (autostart_mode) {
        set_ctrl(base);
        vmpu68_reg_write(VREG_ADDR_LO, (uint16_t)addr);     /* starts the cycle */
    } else {
        vmpu68_reg_write(VREG_ADDR_LO, (uint16_t)addr);
        vmpu68_reg_write(VREG_CTRL, base);
    }
}

uint16_t vmpu68_bus_write(uint32_t addr, int word, uint16_t data)
{
    issue_write(addr, word, data, wt_bit);
    return wait_done();
}

/* posted write: the FPGA queues up to 64 commands in order; the caller
 * must drain (vmpu68_bus_drain) before 64 are outstanding */
void vmpu68_bus_write_posted(uint32_t addr, int word, uint16_t data)
{
    issue_write(addr, word, data, 0);
}

uint16_t vmpu68_bus_drain(void) { return wait_done_st(); }   /* posted commands carry no wt */

/* diagnostics: n GPIO round trips (the protocol's pacing unit), so the
 * host can measure how long one pace takes under the current clocks */
void vmpu68_pace_bench(unsigned n)
{
    while (n--) vmpu68_pace();
}

static int      snoop2_mode;
static uint32_t snoop_last;               /* address of the last real record (v2 continuation base) */
void vmpu68_set_snoop2_io(int on) { snoop2_mode = on; snoop_last = 0; }
int  vmpu68_snoop2_io(void) { return snoop2_mode; }
void vmpu68_snoop_resync(void) { snoop_last = 0; }

int vmpu68_snoop_pop(vmpu68_snoop_t *rec)
{
    uint16_t hi, lo, da;
    uint32_t addr;
    if (snoop2_mode) {
        /* HI = 0 is "nothing there" (a real record always has n = 3, a
         * range bound n = 1 or 2); bit13 = continuation, no LO word */
        hi = vmpu68_reg_read(VREG_ADDR_LO);
        if (!hi) return 0;
        if (hi & 0x2000)
            addr = snoop_last + 2;
        else {
            lo = vmpu68_reg_read(VREG_ADDR_LO);
            addr = ((uint32_t)(hi & 0xFF) << 16) | lo;
        }
        da = vmpu68_reg_read(VREG_ADDR_LO);
        if ((hi & 0x0300) == 0x0300) snoop_last = addr;
    } else {
        if (!(vmpu68_reg_read(VREG_STATUS) & VST_SNOOP)) return 0;
        hi = vmpu68_reg_read(VREG_ADDR_LO);
        lo = vmpu68_reg_read(VREG_ADDR_LO);
        da = vmpu68_reg_read(VREG_ADDR_LO);
        addr = ((uint32_t)(hi & 0xFF) << 16) | lo;
    }
    rec->uds  = (hi >> 15) & 1;
    rec->lds  = (hi >> 14) & 1;
    rec->aux  = (hi >> 8) & 0x3F;
    rec->addr = addr & 0xFFFFFF;
    rec->data = da;
    return 1;
}

/* ---------------- bit-banged SPI flash (W25Q32) ---------------- */

void vmpu68_flash_begin(void)
{
    GPCLR0 = creset_bit;
    fsel(bd->creset, 1);           /* drive CRESET_B low: FPGA off */
    VMPU68_USLEEP(1000);
}

void vmpu68_flash_end(void)
{
    fsel(bd->creset, 0);           /* release: FPGA reconfigures */
    if (smi_on) smi_capture();     /* CRESET's function select changed: refresh the hold-read images */
}

void vmpu68_flash_xfer(const uint8_t *tx, uint8_t *rx, unsigned n, int cont)
{
    GPCLR0 = ss_bit;
    for (unsigned i = 0; i < n; i++) {
        uint8_t o = tx ? tx[i] : 0, in = 0;
        for (int b = 7; b >= 0; b--) {
            if ((o >> b) & 1) GPSET0 = mosi_bit;
            else              GPCLR0 = mosi_bit;
            GPSET0 = sck_bit;
            in = (uint8_t)((in << 1) | ((GPLEV0 >> miso_shift) & 1));
            GPCLR0 = sck_bit;
        }
        if (rx) rx[i] = in;
    }
    if (!cont) GPSET0 = ss_bit;
}
