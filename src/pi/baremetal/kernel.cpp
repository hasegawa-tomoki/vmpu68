// SPDX-License-Identifier: MIT
//
// vmpu68 bare-metal firmware — mgmtd port, stage 2.
// - interactive console on UART and USB CDC gadget (st/rd/wr/led/id/sn/...)
// - firmware self-update (fw) and generic file upload (put) over the console
// - WLAN + HTTP status server (degrades gracefully when WLAN is not set up)
#include "kernel.h"
#include "webserver.h"
#include "discovery.h"
#include "config.h"
#include "version.h"
#include <circle/usb/usbserial.h>
#include <circle/string.h>
#include <circle/version.h>
#include <circle/bcm2835.h>
#include <circle/memio.h>
#include <circle/startup.h>
#include <circle/alloc.h>
#include <circle/net/netconfig.h>
#include <circle/net/ipaddress.h>
#include <circle/bcmpropertytags.h>
#include <circle/util.h>
#include <circle/gpioclock.h>
#include <circle/gpiopin.h>
#include <circle/synchronize.h>

extern "C" {
#include "../mgmtd/hw.h"
#ifdef VMPU68_WITH_EMU
#include "../core/emu68k.h"
#include "../core/jit.h"
#include "sramboot_bin.h"
#endif
void vmpu68_set_gpio_base(volatile unsigned int *base);
void vmpu68_pace_bench(unsigned n);
}

#ifdef VMPU68_WITH_EMU
static boolean s_gpclk_on = FALSE;   // gpclk <div> active (PLL test clock on BCM4/AD2)
static const char *s_board_how = "?";  // how the board layout was chosen (vmpu68.cfg / probed / default)
static boolean s_emu_inited;
static volatile boolean  s_emu_run;            // erun: free-running emulator (core 1)
static volatile boolean  s_auto = TRUE;        // supervise X68000 power/reset and run automatically
static boolean           s_x68_on;             // PLL locked = X68000 clock present
// X68000 supervision state (main loop; here so that "auto" can show it)
static struct {
    unsigned nLastSup, nRuns;
    unsigned nDeadSince, nAliveSince, nDeadMs = ~0u;   // clock stop/restart times (ms); ~0 = never seen stopped
    boolean  bEverDead, bWasAlive, bBounceLogged;
    boolean  bRstSeen;                                 // RESET_IN observed while the machine was (coming) on
    boolean  bBtnReset;                                // external RESET_IN seen by the 10 ms sampler (reset button)
    unsigned nLastRstTick, nPwrOff;
    unsigned nLastSig = 0xFF, nLastLock, nLastSt;
} s_sup;
static volatile boolean  s_emu_active;         // core 1 is inside emu68k_run()
static volatile uint64_t s_emu_cycles;         // total emulated cycles (erun)
static volatile unsigned s_pause_depth;        // vmpu68_emu_pause nesting (Web requests)
static volatile boolean  s_pause_was_running;
static volatile unsigned s_emu_mhz;            // espd: emulated clock limit in MHz (0 = unlimited)
static unsigned s_bus_mhz10;                   // measured bus clock in 0.1 MHz (167 = 16.7), see BootX68
static const cfg_bus_class *s_bus_cls;         // the timing class in use (config.h)
static int s_bus_setup_auto = -1;              // wr_setup found by the boot probe (-1 = not probed)
static const char *probed_str (void);          // " (probed: n)" for bcls

// pace the emulator against the 1 MHz system timer so that ROM/software delay loops
// calibrated for a 10 MHz 68000 do not collapse (e.g. SCSI ROM selection timeout)
static void emu_pace (unsigned cycles)
{
    static uint64_t lim_cycles;
    static unsigned lim_t0;
    static boolean  lim_valid;
    unsigned mhz = emu68k_pace_mhz () ? 0 : s_emu_mhz;   // the core paces itself (fine-grained) since 0.1.18
    if (!mhz || !cycles)                       // 'cycles == 0' resynchronises (erun / espd)
    {
        lim_valid = FALSE;
        return;
    }
    if (!lim_valid)
    {
        lim_valid = TRUE;
        lim_cycles = 0;
        lim_t0 = CTimer::GetClockTicks ();
    }
    lim_cycles += cycles;
    unsigned due = (unsigned) (lim_cycles / mhz);          // us of emulated time consumed
    unsigned el = CTimer::GetClockTicks () - lim_t0;
    if (el > due + 20000)                                  // fell behind (I/O bound): no burst credit
        lim_t0 += el - due;
    else
        while (CTimer::GetClockTicks () - lim_t0 < due)
            ;
}

// stop core 1 and wait until it is outside the core before touching emulator state
// Is the X68000 bus clock running?  The PLL's LOCK output is NOT usable for
// this: with the Pi powered separately its 5V back-feeds the FPGA through
// the ideal diode, so an X68000 power-off just freezes the FPGA clock with
// LOCK (and every register) stuck at its last value.  Watch the heartbeat
// bits instead: hb_clk16 toggles every 2^19 bus clocks (33 ms at 16 MHz,
// 52 ms at 10 MHz) and hb_fast every 2^21 PLL clocks (~35/55 ms), so a
// 128 ms poll that never sees either change means the clock is stopped.
// Costs <= 55 ms (typically ~15 ms) per call while the machine is on.
static boolean x68_clock_alive (void)
{
    const unsigned mask = VDIAG_HB_FAST | VDIAG_HB_CLK16;
    unsigned h0 = hw_reg_read (2) & mask;
    for (unsigned i = 0; i < 64; i++)
    {
        CTimer::SimpleusDelay (2000);
        if ((hw_reg_read (2) & mask) != h0)
            return TRUE;
    }
    return FALSE;
}

// Stop the emulator core and wait for it to leave emu68k_run().  Bounded:
// with the X68000 off the FPGA clock is frozen and a bus command may never
// complete (the wait in vmpu68_io gives up after ~5 s), so the main loop must
// not hang on it.  Callers that go on to reset the emulator run again after
// the clock is back, when the core has long stopped.
static void emu_quiesce (void)
{
    s_emu_run = FALSE;
    DataSyncBarrier ();
    unsigned n = 0;
    while (s_emu_active)
    {
        if (++n > 60000)
            break;                             // ~6 s: the core is stuck on a frozen bus
        CTimer::SimpleusDelay (100);
    }
    hw_wq_sync ();                             // its queued writes land before anyone else touches the bus
}

// Reboot the Pi the way the machine's reset button looks from the outside:
// stop the emulator and hold RESET+HALT on the X68000 bus first, so the
// peripherals (CRTC, video) reset and the screen goes dark right away instead
// of freezing on the last frame for the 8-10 s the Pi takes to come back.
// The FPGA keeps driving the lines across the Pi reboot; the new kernel's
// BootX68 releases them after its own 100 ms pulse.
static void tryboot_reboot (void);
static void pi_reboot (boolean bTry)
{
    emu_quiesce ();
    if (!hw_is_mock ())
    {
        hw_set_drv (1, 1);
        CLogger::Get ()->Write ("vmpu68", LogNotice, "reboot: X68000 held in reset%s", bTry ? " (tryboot)" : "");
    }
    if (bTry) tryboot_reboot ();
    else      reboot ();
}

void CEmuCore::Run (unsigned nCore)
{
    if (nCore == 2)
        hw_wq_worker ();                       // posted-write queue: drives the GPIO strobes for core 1 (never returns)
    if (nCore != 1)
        for (;;) asm volatile ("wfe");
    for (;;)
    {
        if (s_emu_run)
        {
            s_emu_active = TRUE;
            DataSyncBarrier ();
            if (s_emu_run)                     // re-check after publishing active
            {
                unsigned n = (unsigned) emu68k_run (2000);
                s_emu_cycles += n;
                if (emu68k_diag_halt_req ()) { s_emu_run = FALSE; DataSyncBarrier (); }  // pw hit: freeze for a dump (erun 1 resumes)
                if (emu68k_brk_hit ())     { s_emu_run = FALSE; DataSyncBarrier (); }   // ebrk: PC breakpoint fired (ebrk <pc> re-arms, erun 1 resumes)
                emu68k_snoop_apply ();
                emu_pace (n);
            }
            s_emu_active = FALSE;
            DataSyncBarrier ();
        }
        else
        {
            // Paused by a Web request (screen dump): keep draining the snoop
            // FIFO.  Once it is nearly full the FPGA stops granting the bus,
            // and an FDC DMA then overruns: the FDC ends the sector with a
            // result phase while the DMAC still holds its latched request,
            // whose late DACK read consumes ST0, clears the FDC interrupt,
            // and the IOCS waits for it forever ("GAME OVER" freeze seen
            // during a floppy load).  A real 68000 never withholds the bus
            // that long.
            if (s_pause_depth && s_pause_was_running)
            {
                s_emu_active = TRUE;
                DataSyncBarrier ();
                emu68k_snoop_apply ();
                s_emu_active = FALSE;
                DataSyncBarrier ();
            }
            emu_pace (0);
            CTimer::SimpleusDelay (200);
        }
    }
}
#define EMU_RAM_SIZE 0x200000                  // 2MB shadow until BootX68 probes the real size
// shared with the web server (webserver.cpp)
void vmpu68_emu_ensure (void)
{
    if (!s_emu_inited)
    {
        emu68k_init (EMU_RAM_SIZE, 1);
        s_emu_inited = TRUE;
    }
}
#define emu_ensure vmpu68_emu_ensure

// Pause the emulator (core 1) around a Web request that reads the bus or
// pokes the keyboard buffer: the word-by-word alternative shares the bus
// lock with the running emulator, which breaks the prefetch stream (3 us
// per word instead of 0.4) and, seen on the screen viewer, slips words in
// TVRAM rows.  Nested; a stopped emulator (ers, reset button) stays stopped.
void vmpu68_emu_pause (int on)
{
    if (on)
    {
        if (s_pause_depth++ == 0)
        {
            s_pause_was_running = s_emu_run;
            emu_quiesce ();
        }
    }
    else if (s_pause_depth && --s_pause_depth == 0 && s_pause_was_running)
    {
        s_emu_run = TRUE;
        DataSyncBarrier ();
    }
}

// A floppy transfer may be running (FDC/DMAC ch0 touched within the last
// 200 ms while the emulator runs).  Pausing core 1 then starves the FDC's
// DMA and hangs the IOCS (docs §27), so screen reads wait or refuse.
int vmpu68_emu_fd_busy (void)
{
    return s_emu_run && emu68k_fdc_busy ();
}

// Machine reset from the Web UI: like the console 'ers' but leaves the
// emulator running.  Core 1 must be stopped first - m68k_pulse_reset() on a
// core that is executing leaves a garbage PC (mid-instruction, odd) and the
// machine dies within a second of the reset.
void vmpu68_emu_reset (void)
{
    emu_ensure ();
    emu_quiesce ();
    emu68k_reset ();
    s_emu_run = TRUE;
    DataSyncBarrier ();
}

// Type text into Human68k's keyboard buffer (console 'ekey', Web UI key
// box).  IOCS keyboard work area (from the MFP receive handler at $FF191E):
//   $812.w count (max 64), $814.l last written slot, ring $81C..$89A of
//   words {scancode, character}; the scancode->character tables live in
//   ROM ($FF1CD0 unshifted / $FF1D50 shifted, shift only below $35).
// ^M ^[ ^C.. stand for control characters, ^^ for '^'; characters without
// a key (non-ASCII) are dropped.  Returns the number of keys queued.
unsigned vmpu68_emu_type (const char *p, int enter, unsigned *pDropped)
{
    emu_ensure ();
    static uint8_t tn[0x80], ts[0x35];
    for (unsigned i = 0; i < sizeof tn; i += 2) { unsigned w = emu68k_peek16 (0xFF1CD0 + i); tn[i] = w >> 8; tn[i + 1] = w; }
    for (unsigned i = 0; i < sizeof ts - 1; i += 2) { unsigned w = emu68k_peek16 (0xFF1D50 + i); ts[i] = w >> 8; ts[i + 1] = w; }
    unsigned sent = 0, dropped = 0;
    for (;;)
    {
        unsigned c;
        if (*p == 0) { if (!enter) break; c = '\r'; enter = 0; }
        else if (*p == '^' && p[1]) { c = p[1] == '^' ? '^' : (p[1] & 0x1F); p += 2; }
        else c = (uint8_t) *p++;
        unsigned key = (c >= 'a' && c <= 'z') ? c - 0x20 : (c < 0x20 && c != '\r' && c != 0x1B && c != 8 && c != 9) ? c + 0x40 : c;  // control: the letter's key
        unsigned sc = 0;
        for (unsigned i = 1; i < sizeof tn && !sc; i++) if (tn[i] == key) sc = i;
        for (unsigned i = 1; i < sizeof ts && !sc; i++) if (ts[i] == key) sc = i;
        if (!sc) { dropped++; continue; }
        unsigned t0 = CTimer::GetClockTicks ();
        while (emu68k_peek16 (0x812) >= 64 && CTimer::GetClockTicks () - t0 < 2000000) ;   // buffer full: give the reader 2 s
        if (emu68k_peek16 (0x812) >= 64) { dropped++; continue; }
        unsigned slot = ((emu68k_peek16 (0x814) << 16) | emu68k_peek16 (0x816)) + 2;
        if (slot < 0x81C || slot >= 0x89C) slot = 0x81C;
        emu68k_poke16 (slot, (sc << 8) | c);
        emu68k_poke16 (0x814, slot >> 16);
        emu68k_poke16 (0x816, slot & 0xFFFF);
        emu68k_poke16 (0x812, emu68k_peek16 (0x812) + 1);
        sent++;
    }
    *pDropped = dropped;
    return sent;
}
#endif

static const char FromKernel[] = "vmpu68";

// firmware mailbox tag: pass reboot flags to the bootloader; bit0 = tryboot
// (load tryboot.txt / the try kernel on the next boot only).  Writing the
// PM_RSTS partition bits does NOT work on Pi 4 — the firmware does not
// preserve RSTS across a watchdog reset.
#define PROPTAG_SET_REBOOT_FLAGS    0x00038064

static void tryboot_reboot (void)
{
    CBcmPropertyTags Tags;
    TPropertyTagSimple Flags;
    Flags.nValue = 1;                          // tryboot
    Tags.GetTag (PROPTAG_SET_REBOOT_FLAGS, &Flags, sizeof Flags, 4);
    reboot ();
}

#define FIRMWARE_PATH   "SD:/firmware/"
#define WPA_CONFIG_FILE "SD:/wpa_supplicant.conf"
#define PROVISION_FILE   "SD:/vmpu68.vpk"          // release package on a freshly written card (first-boot FPGA programming)
#define PROVISION_FAILED "SD:/provision.failed"   // marker: first-boot programming failed, do not retry

CKernel::CKernel (void)
:   m_pSerial (nullptr),
    m_Timer (&m_Interrupt),
    m_Logger (m_Options.GetLogLevel (), &m_Timer),
    m_CDCGadget (&m_Interrupt),
    m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED),
    m_WLAN (FIRMWARE_PATH),
    m_Net (0, 0, 0, 0, "vmpu68bm", NetDeviceTypeWLAN),
    m_WPASupplicant (WPA_CONFIG_FILE),
    m_EmuCore (CMemorySystem::Get ()),
    m_bNetOK (FALSE),
    m_bWebStarted (FALSE)
{
    m_ActLED.Blink (3);
}

CKernel::~CKernel (void)
{
}

static void ab_boot_check (void);          // A/B tryboot promote/rollback (defined below)
static void fpga_version_fixup (void);     // SD:/fpga.sum version from SD:/update.vpk (defined below)

// JIT code buffer: circle maps everything above _etext execute-never (PXN)
// for EL1, so generated code in the heap takes an instruction abort
// (EC 0x21, permission fault).  Clear PXN on the buffer's 64 KB pages in the
// level-3 table circle built (64 KB granule: TTBR0 -> level 2, 512 MB per
// entry -> level 3, 8192 pages) and flush the TLBs on all cores.
extern "C" int jit_host_make_exec (void *p, unsigned long n)
{
    u64 ttbr; asm volatile ("mrs %0, ttbr0_el1" : "=r" (ttbr));
    u64 *l2 = (u64 *) (ttbr & 0x0000FFFFFFFF0000ull);
    for (uintptr a = (uintptr) p & ~0xFFFFul; a < (uintptr) p + n; a += 0x10000) {
        u64 d2 = l2[a >> 29];
        if ((d2 & 3) != 3) return -1;                 // not a table descriptor
        u64 *l3 = (u64 *) (d2 & 0x0000FFFFFFFF0000ull);
        u64 *d3 = &l3[(a >> 16) & 8191];
        if ((*d3 & 3) != 3) return -2;
        *d3 &= ~(1ull << 53);                          // PXN
        asm volatile ("dc cvac, %0" :: "r" (d3) : "memory");
    }
    asm volatile ("dsb ish\n\ttlbi vmalle1is\n\tdsb ish\n\tisb" ::: "memory");
    return 0;
}

// Panic (CPU exception): the console serial is interrupt driven, so the
// exception dump circle just logged never leaves its TX queue.  Push the tail
// of the log ring to the console UART by polling, and name the JIT block if
// the faulting PC lies in generated code.
static void PanicPutc (uintptr base, char c)
{
    while (read32 (base + 0x18) & (1 << 5)) ;      // PL011 FR.TXFF
    write32 (base + 0x00, (u32) (u8) c);
}
static void PanicPuts (uintptr base, const char *s) { for (; *s; s++) { if (*s == '\n') PanicPutc (base, '\r'); PanicPutc (base, *s); } }
static void PanicFlush (void)
{
    static char buf[LOGGER_BUFSIZE + 1];
    unsigned uart = vmpu68_board ()->uart;
    uintptr base = uart == 0 ? ARM_UART0_BASE : ARM_IO_BASE + 0x201400 + 0x200 * (uart - 2);
    int n = CLogger::Get ()->Read (buf, LOGGER_BUFSIZE, FALSE);
    if (n < 0) n = 0;
    buf[n] = 0;
    const char *tail = buf + (n > 1500 ? n - 1500 : 0);
    PanicPuts (base, "\n[panic] log tail:\n");
    PanicPuts (base, tail);
    // "PC 0x..." in the dump -> which JIT block (if any)
    for (const char *p = buf; *p; p++) {
        if (p[0] == 'P' && p[1] == 'C' && p[2] == ' ' && p[3] == '0' && p[4] == 'x') {
            uintptr v = 0; const char *q = p + 5;
            for (; ; q++) { char c = *q; unsigned d;
                if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else break;
                v = (v << 4) | d; }
            char out[160]; jit_debug_locate (v, out, sizeof out);
            PanicPuts (base, "[panic] jit: "); PanicPuts (base, out); PanicPuts (base, "\n");
            char hist[200]; jit_debug_hist (hist, sizeof hist);
            PanicPuts (base, "[panic] recent blocks: "); PanicPuts (base, hist); PanicPuts (base, "\n");
        }
    }
    PanicPuts (base, "[panic] end\n");
}
// VMPU68 information port ($ECFF00): refresh the record the X68000 side
// reads (VMPU68.X shows it).  Big-endian, see emu68k.c.
const cfg_bus_class *vmpu68_bus_class (void);   // defined below
static unsigned s_equiv_mhz10;                 // 68000-equivalent MHz x10 over the last second
static void put16 (u8 *p, unsigned v) { p[0] = (u8) (v >> 8); p[1] = (u8) v; }
static void info_update (void)
{
    static u64 last_cyc; static unsigned last_t;
    unsigned now = CTimer::GetClockTicks ();            // 1 MHz
    u64 cyc = s_emu_cycles;
    if (last_t)
    {
        unsigned dt = now - last_t;
        if (dt >= 500000)                                 // >= 0.5 s: cycles / us = MHz
            s_equiv_mhz10 = (unsigned) ((cyc - last_cyc) * 10 / dt);
        else
            return;
    }
    last_t = now; last_cyc = cyc;
    u8 rec[256]; memset (rec, 0, sizeof rec);
    memcpy (rec, "VMPU", 4);
    put16 (rec + 8, s_equiv_mhz10);
    put16 (rec + 10, s_bus_mhz10);
    put16 (rec + 12, emu68k_ram_size () >> 20);
    unsigned flags = (hw_wr_ai () ? 1 : 0) | ((hw_wr_setup () & 3) << 1) | (s_bus_setup_auto >= 0 ? 8 : 0) | ((emu68k_io_mhz () & 0xFF) << 8);
    put16 (rec + 14, flags);
    strncpy ((char *) rec + 16, VMPU68_VERSION, 15);
    strncpy ((char *) rec + 32, VMPU68_BUILD, 31);
    CString T;
    T.Format ("bus class >= %u.%u MHz (%s), wait %u ns, io %u MHz, up %u s",
              vmpu68_bus_class () ? vmpu68_bus_class ()->min_mhz10 / 10 : 0,
              vmpu68_bus_class () ? vmpu68_bus_class ()->min_mhz10 % 10 : 0,
              vmpu68_bus_class () ? cfg_bus_source () : "-",
              vmpu68_get_wait_ns (), emu68k_io_mhz (), CTimer::Get ()->GetUptime ());
    strncpy ((char *) rec + 64, (const char *) T, 127);
    strncpy ((char *) rec + 0xC0, cfg_name (), 31);   // host name (VMPU68.X -n writes a new one here and sets $FA)
    put16 (rec + 0xF0, s_emu_mhz);            // settings, readable and writable by VMPU68.X
    put16 (rec + 0xF2, emu68k_wb_enabled () ? 1 : 0);
    put16 (rec + 0xF4, jit_enabled () ? 1 : 0);
    put16 (rec + 0xF6, cfg_sramboot () ? 1 : 0);
    put16 (rec + 0xF8, cfg_ram_mb ());
    emu68k_info_set (rec, sizeof rec);
}
unsigned vmpu68_equiv_mhz10 (void) { return s_equiv_mhz10; }
unsigned vmpu68_mhz_limit (void) { return s_emu_mhz; }

// X68030-style boot screen: the IPL's SRAM boot ($ED0018 = $B000) calls the
// program at $ED0100 (first byte $60 = BRA) and, when it returns, boots as
// usual (IPL ROM $FF0310 / $FF01E6).  The program (x68k/vmpu68x/vmpu68.s
// -DSRAMMODE, embedded as sramboot_bin) draws the SHARP/BIOS/MPU/CLOCK lines
// from the information port and holds them 1.5 s.  Battery SRAM keeps it, but
// BootX68 re-checks every machine boot (dead battery, other software).
#define SRAM_BOOT_DEV   0xED0018u
#define SRAM_BOOT_PROG  0xED0100u
int vmpu68_sramboot_state (void)
{
    if (hw_is_mock ()) return -1;
    static u8 cur[SRAMBOOT_LEN];
    u8 dev[2];
    emu68k_sram_read (SRAM_BOOT_PROG, cur, sizeof cur);
    emu68k_sram_read (SRAM_BOOT_DEV, dev, 2);
    if (cur[0] != 0x60) return 0;                                   // nothing runnable there
    if (memcmp (cur + 4, "VMPU68SB", 8) != 0) return 2;             // someone else's SRAM program
    if (dev[0] != 0xB0 || dev[1] != 0x00) return 0;                 // ours, but not armed
    if (memcmp (cur, sramboot_bin, sizeof cur) != 0) return 3;      // ours, but not this build (whole program compared)
    return 1;
}
static int sramboot_install (int on)
{
    unsigned bad = 0;
    if (on)
    {
        static const u8 dev[2] = { 0xB0, 0x00 };
        bad += emu68k_sram_write (SRAM_BOOT_PROG, sramboot_bin, SRAMBOOT_LEN);
        bad += emu68k_sram_write (SRAM_BOOT_DEV, dev, 2);
    }
    else
    {
        static const u8 zero[2] = { 0, 0 };
        if (vmpu68_sramboot_state () == 2) return 0;               // not ours: leave it alone
        bad += emu68k_sram_write (SRAM_BOOT_DEV, zero, 2);          // STD boot again
        bad += emu68k_sram_write (SRAM_BOOT_PROG, zero, 2);         // no BRA: the IPL's SRAM check skips it
    }
    return (int) bad;
}
int vmpu68_sramboot_set (int on, boolean save)
{
    int rc = -1;
    if (!hw_is_mock ())
    {
        emu_quiesce ();
        rc = sramboot_install (on);
        s_emu_run = TRUE;
        DataSyncBarrier ();
    }
    if (save) { cfg_set_sramboot (on); cfg_save (); }
    CLogger::Get ()->Write (FromKernel, LogNotice, "sramboot: %s%s%s", on ? "installed" : "removed",
                    rc ? " - SRAM WRITE FAILED" : "", save ? " (saved)" : "");
    return rc;
}
// Hardware revision shown by vhd68/vfd68 as "HW x.y": the probe only tells
// 1.x from 2.x, so the built boards' revisions are assumed (core 1.0 boards
// No.1-5, core 2.x); vmpu68.cfg hw= names the exact one (hw=2.1).
const char *vmpu68_hw_rev (void)
{
    if (cfg_hw ()[0]) return cfg_hw ();
    return vmpu68_board_id () == 1 ? "1.0" : "2.0";
}

// Runtime settings (Web UI /api/config, VMPU68.X through the information
// port, console): emulated clock limit and main RAM write-back.  Called on
// core 0.  save writes SD:/vmpu68.cfg so the choice survives a reboot.
void vmpu68_apply_settings (int mhz, int wb, int jit, boolean save)
{
    if (mhz >= 0)
    {
        s_emu_mhz = (unsigned) mhz;
        emu68k_set_pace_mhz (s_emu_mhz);
    }
    if (wb >= 0 && (wb != 0) != (emu68k_wb_enabled () != 0))
    {
        emu_quiesce ();
        emu68k_wb_set (wb);
        s_emu_run = TRUE;
        DataSyncBarrier ();
    }
    if (jit >= 0 && (jit != 0) != (jit_enabled () != 0))
    {
        emu_quiesce ();                     // switch between blocks: the core must not be inside generated code
        jit_set_enabled (jit);
        jit_flush_all ();
        s_emu_run = TRUE;
        DataSyncBarrier ();
    }
    int rc = 0;
    if (save) { cfg_set (s_emu_mhz, emu68k_wb_enabled (), jit_enabled ()); rc = cfg_save (); }
    CLogger::Get ()->Write (FromKernel, LogNotice, "settings: speed %u MHz (0 = unlimited), JIT %s, main RAM %s%s", s_emu_mhz,
                    jit_enabled () ? "on" : "off", emu68k_wb_enabled () ? "write-back" : "write-through", save ? (rc ? " - cfg save FAILED" : " - saved") : "");
    info_update ();
}

static boolean ab_promote (void);

boolean CKernel::Initialize (void)
{
    boolean bOK = TRUE;

    // The console UART depends on the board (core 1.x: UART0 on GPIO14/15,
    // core 2.x: UART2 on GPIO0/1 - GPIO14/15 carry AD6/AD7 there), so the
    // SD card and the board probe come first; the few log lines the timer
    // and the SD driver emit meanwhile go to the log buffer only.
    if (bOK) bOK = m_Interrupt.Initialize ();
    if (bOK) bOK = m_Timer.Initialize ();
#ifdef VMPU68_WATCHDOG
    if (bOK) m_Watchdog.Start (15);     // cover init too: a try kernel that hangs here still resets -> fallback
#endif
    if (bOK) bOK = m_EMMC.Initialize ();
    if (bOK) bOK = (f_mount (&m_FileSystem, "SD:", 1) == FR_OK);
    if (bOK) cfg_load ();               // SD:/vmpu68.cfg (name=, ram=, mhz=, board=)
    if (bOK) cfg_bus_load ();           // SD:/vmpu68-bus.json (bus timing per clock class)
    if (bOK) fpga_version_fixup ();     // SD:/fpga.sum without a version: take it from SD:/update.vpk when the bitstream matches

    // board GPIO layout: vmpu68.cfg board= wins; otherwise ask the FPGA
    // through both layouts (a configured FPGA answers 0x56 on either
    // board); a blank FPGA (new board) leaves nothing to probe -> 2.x
    vmpu68_set_gpio_base ((volatile unsigned int *) ARM_GPIO_BASE);
    const char *how = "vmpu68.cfg";
    int board = (int) cfg_board ();
    if (!board) { board = vmpu68_probe_board (); how = board ? "probed" : "default"; }
    if (!board) board = 2;
    vmpu68_set_board (board);
    s_board_how = how;

    m_pSerial = new CSerialDevice (&m_Interrupt, TRUE, vmpu68_board ()->uart);
    if (bOK) bOK = m_pSerial->Initialize (115200);
    if (bOK) bOK = m_Logger.Initialize (m_pSerial);
    m_Logger.RegisterPanicHandler (PanicFlush);
    CLogger::Get ()->Write (FromKernel, LogNotice, "board: %s (%s), console UART%u",
                    vmpu68_board ()->name, how, vmpu68_board ()->uart);
    if (bOK) bOK = m_CDCGadget.Initialize ();
    if (bOK) ab_boot_check ();            // A/B: promote/roll back a tryboot kernel (before the risky init)
    s_emu_mhz = cfg_mhz ();             // emulated clock limit (0 = unlimited), like the console espd
    emu68k_set_pace_mhz (s_emu_mhz);
    emu68k_wb_set (cfg_wb ());          // main RAM write-back (wb=, default on)
    jit_set_enabled (cfg_jit ());       // execution by translation (jit=, default off)

    // Wi-Fi is brought up later, from the main loop (StartWLAN): the
    // firmware download takes 6-7 s and used to sit here in front of the
    // emulator start, so an X68000 power-on waited that long for nothing
    // the machine needs (the network is management only).
    m_bNetOK = FALSE;
    m_bWLANStarted = FALSE;

    return bOK;
}

// WLAN bring-up, deferred from Initialize (see there).  Blocks core 0 for
// a few seconds; the emulator runs on its own core meanwhile.
void CKernel::StartWLAN (void)
{
    m_bWLANStarted = TRUE;
    CLogger::Get ()->Write (FromKernel, LogNotice, "wlan: starting (deferred)");
    // power-cycle the WLAN chip (firmware GPIO expander pin 1 =
    // WL_REG_ON): after a warm reboot the chip keeps its old state and
    // firmware download fails ("connecting..." forever); cold boots
    // are unaffected
    CBcmPropertyTags Tags;
    TPropertyTagGPIOState GPIOState;
    GPIOState.nGPIO = EXP_GPIO_BASE + 1;
    GPIOState.nState = 0;
    Tags.GetTag (PROPTAG_SET_SET_GPIO_STATE, &GPIOState, sizeof GPIOState, 8);
    m_Timer.MsDelay (100);
    GPIOState.nGPIO = EXP_GPIO_BASE + 1;
    GPIOState.nState = 1;
    Tags.GetTag (PROPTAG_SET_SET_GPIO_STATE, &GPIOState, sizeof GPIOState, 8);
    m_Timer.MsDelay (150);

    // network bring-up is best-effort: without firmware files or
    // wpa_supplicant.conf on the SD card the console still works
#ifdef VMPU68_WATCHDOG
    m_Watchdog.Start (15);
#endif
    FILINFO fi;
    if (f_stat (WPA_CONFIG_FILE, &fi) != FR_OK)
    {
        // no Wi-Fi configuration: do not bring the WLAN up at all, so the
        // status LED shows green (no Wi-Fi) instead of blinking blue forever
        m_bNetOK = FALSE;
        CLogger::Get ()->Write (FromKernel, LogNotice, "no %s - WLAN not started (console only)", WPA_CONFIG_FILE);
    }
    else
    {
        m_bNetOK = m_WLAN.Initialize ()
                && m_Net.Initialize (FALSE)
                && m_WPASupplicant.Initialize ();
        if (!m_bNetOK)
            CLogger::Get ()->Write (FromKernel, LogWarning,
                            "WLAN not started (missing SD:/firmware or bad config?)");
    }
}

// ---------------- status LED (LED1, RGB) ----------------
// The same ladder as vfd68's / vhd68's status lamp, highest first:
//   locate (/api/locate)  every colour, 4 Hz blink, for the requested time
//   (FPGA flash access - provisioning, updates - is shown on the Pi's own ACT LED
//   instead, fast blink: the RGB LED hangs off the FPGA, which is held in reset then)
//   booting               white, solid (until the main loop starts)
//   SD access (<120 ms)   red
//   Wi-Fi joining         blue, 2 Hz blink (for the first 60 s; then green until it connects)
//   Wi-Fi up              blue, solid
//   otherwise             green (no wpa_supplicant.conf, or the WLAN failed to start)
// hw_set_led() takes bit2=R bit1=G bit0=B; 'led <n>' on the console holds a
// colour by hand, 'led auto' hands the lamp back to this ladder.
static int      s_led_manual = -1;         // -1 = automatic
static boolean  s_led_booted;
static unsigned s_led_sd_ms;               // last SD read/write (ms since boot)
static unsigned s_led_locate_until;        // ms since boot; 0 = off
static unsigned s_led_cur = 0xFF;          // last colour written (0xFF: force)

static unsigned now_ms (void) { return CTimer::Get ()->GetClockTicks () / 1000; }   // 1 MHz clock -> ms (wraps, differences only)
static void led_apply (unsigned rgb)
{
    if (rgb == s_led_cur) return;
    s_led_cur = rgb;
    hw_set_led (rgb & 7);
}
static void statusled_sd_activity (void) { s_led_sd_ms = now_ms () | 1; }
void vmpu68_led_locate (unsigned ms) { s_led_locate_until = (now_ms () + ms) | 1; }
#define LED_NET_GIVEUP_MS 60000   // 設定はあるのに 60 秒つながらない: 緑(Wi-Fi なし扱い)にする
static void statusled_task (boolean bNetOK, CNetSubSystem *pNet)
{
    unsigned now = now_ms ();
    if (s_led_locate_until)
    {
        if ((int) (now - s_led_locate_until) < 0) { led_apply ((now / 125) & 1 ? 7 : 0); return; }
        s_led_locate_until = 0;
    }
    if (s_led_manual >= 0) { led_apply ((unsigned) s_led_manual); return; }
    unsigned rgb;
    if (!s_led_booted)                                   rgb = 7;   // 起動中: 白
    else if (s_led_sd_ms && now - s_led_sd_ms < 120)     rgb = 4;   // SD アクセス: 赤
    else if (bNetOK && pNet && !pNet->IsRunning ())
        rgb = now < LED_NET_GIVEUP_MS ? ((now / 250) & 1 ? 1 : 0) : 2;   // 接続試行中: 青点滅。一定時間つながらなければ緑(接続できればいつでも青に戻る)
    else if (bNetOK && pNet)                             rgb = 1;   // 接続済み: 青
    else                                                 rgb = 2;   // Wi-Fi なし: 緑
    led_apply (rgb);
}

// ---------------- tiny console ----------------

static unsigned parse_num (const char *&p)
{
    while (*p == ' ') p++;
    unsigned v = 0;
    int hex = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { hex = 1; p += 2; }
    for (;;)
    {
        char c = *p;
        if (c >= '0' && c <= '9') v = hex ? (v << 4) | (unsigned)(c - '0') : v * 10 + (unsigned)(c - '0');
        else if (hex && c >= 'a' && c <= 'f') v = (v << 4) | (unsigned)(c - 'a' + 10);
        else if (hex && c >= 'A' && c <= 'F') v = (v << 4) | (unsigned)(c - 'A' + 10);
        else break;
        p++;
    }
    return v;
}

typedef void (*put_fn) (void *ctx, const char *s);

static unsigned s_xfer_len;
static char     s_xfer_path[128];
static unsigned s_xfer_addr;
static unsigned s_fpga_us = 10;                // flash bit-bang pace (us/phase)

// returns: 0 none, 1 reboot, 2 receive file of s_xfer_len into s_xfer_path,
//          3 receive s_xfer_len bytes into emulator memory at s_xfer_addr
static u32 file_sum_range (FIL *f, u32 off, u32 len, boolean *pOK);   // fwd (defined below)
static void smi_experiment (put_fn out, void *ctx, unsigned divi, unsigned strobe, unsigned setup, unsigned hold, unsigned n);   // fwd

// ---------------- A/B boot: tryboot promote / roll back ----------------
// config.txt boots kernel8-rpi4.img (stable); tryboot.txt boots
// kernel8-try.img (try); a tryboot reboot loads the try kernel for exactly
// one boot.  "upd ... try" writes the try kernel + a marker
// SD:/tryboot.pending "<count> <trysum> <label>" (count 0), then tryboots.
// The marker's count is the boot ordinal since install: the try boot reads 0
// -> 1 (this IS the try kernel: promote when healthy); if it crashes or hangs
// the next boot is stable (tryboot is one-shot) and reads 1 -> 2 (the try did
// not stick: roll back).  Self-identification is not needed - the counter
// carries it.  Promotion copies kernel8-try.img over kernel8-rpi4.img.
#define AB_MARKER "SD:/tryboot.pending"
#define AB_STABLE "SD:/kernel8-rpi4.img"
#define AB_TRY    "SD:/kernel8-try.img"
static int  s_ab_arm_promote;              // this boot is the try boot
static char s_ab_label[40];

static u32 file_sum_path (const char *path)
{
    FIL f; if (f_open (&f, path, FA_READ) != FR_OK) return 0;
    boolean ok; u32 s = file_sum_range (&f, 0, f_size (&f), &ok); f_close (&f);
    return ok ? s : 0;
}
static void ab_write_marker (unsigned count, u32 trysum, const char *label)
{
    FIL f; if (f_open (&f, AB_MARKER, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    CString L; L.Format ("%u %08X %s\n", count, (unsigned) trysum, label ? label : "");
    UINT bw; f_write (&f, (const char *) L, L.GetLength (), &bw); f_sync (&f); f_close (&f);
}
static void ab_boot_check (void)
{
    FIL f; char line[80] = "";
    if (f_open (&f, AB_MARKER, FA_READ) != FR_OK) return;
    f_gets (line, sizeof line, &f); f_close (&f);
    unsigned count = 0; u32 trysum = 0; char label[40] = "";
    const char *q = line; while (*q == ' ') q++;
    while (*q >= '0' && *q <= '9') count = count * 10 + (*q++ - '0');
    while (*q == ' ') q++;
    for (;;) { char c = *q; unsigned d;
        if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else break; trysum = trysum * 16 + d; q++; }
    while (*q == ' ') q++;
    { unsigned n = 0; while (*q && *q != '\r' && *q != '\n' && n + 1 < sizeof label) label[n++] = *q++; label[n] = 0; }
    strncpy (s_ab_label, label, sizeof s_ab_label - 1); s_ab_label[sizeof s_ab_label - 1] = 0;

    if (trysum == file_sum_path (AB_STABLE))    // already promoted (or identical): clean up
    {
        f_unlink (AB_MARKER); f_unlink (AB_TRY);
        CLogger::Get ()->Write (FromKernel, LogNotice, "A/B: try '%s' already in stable slot - cleaned up", label);
        return;
    }
    if (++count >= 2)                           // fell back to stable: the try did not stick
    {
        f_unlink (AB_MARKER); f_unlink (AB_TRY);
        CLogger::Get ()->Write (FromKernel, LogWarning, "A/B: try '%s' did not boot cleanly - rolled back to stable", label);
        return;
    }
    ab_write_marker (count, trysum, label);     // count == 1: this is the try boot
    s_ab_arm_promote = 1;
    CLogger::Get ()->Write (FromKernel, LogNotice, "A/B: running try kernel '%s' - promote after health check", label);
}
static boolean ab_promote (void)
{
    FIL in, out; static u8 buf[16384];
    if (f_open (&in, AB_TRY, FA_READ) != FR_OK) return FALSE;
    u32 len = f_size (&in);
    boolean ok = f_open (&out, "SD:/kernel8.new", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    for (u32 done = 0; ok && done < len; )
    {
        UINT want = len - done > sizeof buf ? sizeof buf : len - done, br = 0, bw = 0;
        statusled_sd_activity ();
        ok = f_read (&in, buf, want, &br) == FR_OK && br == want
          && f_write (&out, buf, br, &bw) == FR_OK && bw == br;
        done += br;
    }
    f_close (&in);
    if (f_close (&out) != FR_OK) ok = FALSE;
    if (ok) { boolean sok; FIL v; ok = f_open (&v, "SD:/kernel8.new", FA_READ) == FR_OK;
              if (ok) { ok = file_sum_range (&v, 0, len, &sok) == file_sum_path (AB_TRY) && sok; f_close (&v); } }
    if (ok) { f_unlink (AB_STABLE); ok = f_rename ("SD:/kernel8.new", AB_STABLE) == FR_OK; }
    if (!ok) { f_unlink ("SD:/kernel8.new");
               CLogger::Get ()->Write (FromKernel, LogError, "A/B: promote FAILED (SD write) - will roll back on next boot"); return FALSE; }
    f_unlink (AB_MARKER); f_unlink (AB_TRY);
    CLogger::Get ()->Write (FromKernel, LogNotice, "A/B: try '%s' promoted to stable", s_ab_label);
    return TRUE;
}

static int exec_cmd (const char *line, put_fn out, void *ctx,
                     boolean bNetOK, CNetSubSystem *pNet)
{
    int action = 0;
    CString R;
    const char *p = line;
    while (*p == ' ') p++;

    if (p[0] == 's' && p[1] == 't')                        // st: status
    {
        unsigned st = hw_status ();
        R.Format ("status=%04X busy=%u fault=%u snoop=%u ipl=%u vpa=%u ovf=%u reset_in=%u halt_in=%u rst_seen=%d %s\r\n",
                  st, !!(st & VST_BUSY), !!(st & VST_FAULT), !!(st & VST_SNOOP),
                  VST_IPL (st), !!(st & VST_VPA), !!(st & VST_SNOOP_OVF),
                  !!(st & VST_RESET_IN), !!(st & VST_HALT_IN), hw_rst_seen (0),
                  hw_is_mock () ? "(MOCK)" : "");
    }
    else if (p[0] == 'r' && p[1] == 'd' && p[2] != 'u')    // rd <addr> [b]
    {
        p += 2;
        unsigned addr = parse_num (p);
        int word = !(*p == ' ' && p[1] == 'b');
        uint16_t v = 0;
        uint16_t st = hw_bus_read (addr, word, &v);
        R.Format ("[%06X] = %04X%s\r\n", addr, v, (st & VST_FAULT) ? " FAULT" : "");
    }
    else if (p[0] == 'w' && p[1] == 'r' && p[2] != 'n')    // wr <addr> <val> [b]
    {
        p += 2;
        unsigned addr = parse_num (p);
        unsigned val = parse_num (p);
        int word = !(*p == ' ' && p[1] == 'b');
        uint16_t st = hw_bus_write (addr, word, (uint16_t) val);
        R.Format ("[%06X] <- %04X%s\r\n", addr, val, (st & VST_FAULT) ? " FAULT" : "");
    }
    else if (p[0] == 'l' && p[1] == 'e')                   // led <0-7> | led auto: hold a colour (bit2=R bit1=G bit0=B) / back to the status ladder
    {
        p += 3;
        while (*p == ' ') p++;
        if (*p == 'a') { s_led_manual = -1; R = "led: auto (status ladder)\r\n"; }
        else if (*p >= '0' && *p <= '9') { s_led_manual = (int) (parse_num (p) & 7); R.Format ("led: held at %d\r\n", s_led_manual); }
        else R.Format ("led: %s (%u)\r\n", s_led_manual < 0 ? "auto" : "held", s_led_cur & 7);
    }
    else if (p[0] == 'r' && p[1] == 'd' && p[2] == 'u')    // rdump <addr> <words>: stream hex (16 words/line)
    {
        p += 5;
        unsigned a = parse_num (p) & 0xFFFFFE;
        unsigned n = parse_num (p);
        unsigned alt = parse_num (p);          // optional: a word read of this address before every word
        unsigned chk = parse_num (p);          // optional: 1 = read every word twice and report unstable ones
        CString L;
        unsigned faults = 0;
        for (unsigned i = 0; i < n; i++)
        {
            uint16_t v = 0, v2 = 0;
            if (alt) hw_bus_read (alt & 0xFFFFFE, 1, &v);
            unsigned op0 = hw_op_n;
            if (hw_bus_read (a + 2 * i, 1, &v) & VST_FAULT) { faults++; hw_clear_fault (); }
            if (chk) hw_bus_read (a + 2 * i, 1, &v2);
            if (chk && v != v2)                // unstable word: show the bus ops around it
            {
                CString T;
                T.Format ("\r\n@%06X %04X/%04X ops:", a + 2 * i, v, v2);
                for (unsigned k = (hw_op_n > op0 + HW_OPS) ? hw_op_n - HW_OPS : op0; k < hw_op_n; k++)
                {
                    const hw_op_t &o = hw_ops[k & (HW_OPS - 1)];
                    CString E;
                    E.Format (" %c%c%06X=%04X", "rwWi"[o.kind & 3], (o.kind & 4) ? 'w' : 'b', o.addr, o.data);
                    T.Append (E);
                }
                T.Append ("\r\n");
                out (ctx, (const char *) T);
            }
            L.Format ("%04X%s", v, ((i % 16) == 15 || i == n - 1) ? "\r\n" : "");
            out (ctx, (const char *) L);
            if ((i % 16) == 15)
                CTimer::SimpleusDelay (6500);  // pace to the 115200bps UART drain rate
        }
        R.Format ("rdump done %u words faults=%u\r\n", n, faults);
    }
    else if (p[0] == 'r' && p[1] == 'r')                   // rr <reg>: raw reg read
    {
        p += 2;
        unsigned reg = parse_num (p) & 3;
        R.Format ("reg%u = %04X\r\n", reg, hw_reg_read (reg));
    }
    else if (p[0] == 'r' && p[1] == 'w')                   // rw <reg> <val>: raw reg write
    {
        p += 2;
        unsigned reg = parse_num (p) & 3;
        unsigned val = parse_num (p);
        vmpu68_reg_write (reg, (uint16_t) val);
        R.Format ("reg%u <- %04X\r\n", reg, val);
    }
    else if (p[0] == 'w' && p[1] == 'r' && p[2] == 'n')    // wrn <0|1>: force WR# level
    {
        p += 3;
        unsigned lvl = parse_num (p);
        // park REG_A on 0 so the eventual rising edge only lands a
        // harmless DATA-latch write in the FPGA
        const vmpu68_board_t *b = vmpu68_board ();
        write32 (ARM_GPIO_BASE + 0x28, (1u << b->rega0) | (1u << b->rega1));
        if (lvl) write32 (ARM_GPIO_BASE + 0x1C, 1u << b->wr);
        else     write32 (ARM_GPIO_BASE + 0x28, 1u << b->wr);
        unsigned lev = (read32 (ARM_GPIO_BASE + 0x34) >> b->wr) & 1;
        R.Format ("WR# driven %u, pad reads %u\r\n", lvl, lev);
    }
    else if (p[0] == 'g' && p[1] == 'p')                   // gpclk <div>: 750MHz/div on BCM4 (1.x: AD2, 2.x: REG_A1); 0 = off
    {
        p += 5;
        static CGPIOClock *s_clk;
        unsigned div = parse_num (p);
        if (div >= 2)
        {
            if (!s_clk) s_clk = new CGPIOClock (GPIOClock0, GPIOClockSourcePLLD);
            s_clk->Start (div, 0, 0);
            CGPIOPin (4, GPIOModeAlternateFunction0);   // BCM4 = AD2
            s_gpclk_on = TRUE;      // the mock re-probe would rewrite GPFSEL0 and kill the clock
            R.Format ("gpclk: %u.%03u MHz on BCM4 (AD2)\r\n", 750 / div, (750 * 1000 / div) % 1000);
        }
        else
        {
            if (s_clk) s_clk->Stop ();
            CGPIOPin (4, GPIOModeInput);
            s_gpclk_on = FALSE;
            R = "gpclk: off, AD2 input\r\n";
        }
    }
    else if (p[0] == 'g' && p[1] == 'l')                   // glev: raw GPIO levels (diag)
    {
        unsigned lev = read32 (ARM_GPIO_BASE + 0x34);
        R.Format ("glev=%08X irq(22)=%u ad3(5)=%u ad4(6)=%u ad2(4)=%u\r\n", lev,
                  (lev >> 22) & 1, (lev >> 5) & 1, (lev >> 6) & 1, (lev >> 4) & 1);
    }
    else if (p[0] == 'h' && p[1] == 'w' && p[2] == 'p')    // hwp: re-probe the FPGA register file
    {
        int mock = hw_init ();
        R.Format ("hwp: FPGA %s\r\n", mock ? "NOT detected (mock)" : "detected (sig 0x56)");
    }
    else if (p[0] == 'c' && p[1] == 'r' && p[2] == 's')    // crst: pulse CRESET_B (reconfigure)
    {
        vmpu68_set_gpio_base ((volatile unsigned int *) ARM_GPIO_BASE);
        vmpu68_open ();
        vmpu68_flash_begin ();
        vmpu68_flash_end ();
        CTimer::SimpleusDelay (1500000);
        int mock = hw_init ();
        R.Format ("crst: FPGA %s\r\n", mock ? "NOT detected" : "detected (sig 0x56)");
    }
    else if (p[0] == 'b' && p[1] == 'e' && p[2] == 'n')    // bench [wbase]: GPIO/bus micro-benchmarks (writes to wbase, default E00000)
    {
        p += 3; while (*p == 'c' || *p == 'h') p++;
        unsigned wb = parse_num (p) & 0xFFE000; if (wb == 0) wb = 0xE00000;
        emu_quiesce ();
        hw_set_irq_en (0);          // a stopped 68000 leaves an interrupt pending, which would hold PI_IRQ
        const unsigned N = 10000;
        unsigned t0, t1;
        CString L;
        t0 = CTimer::GetClockTicks ();                      // 1MHz ticks
        for (unsigned i = 0; i < N; i++) (void) vmpu68_irq_pin ();
        t1 = CTimer::GetClockTicks ();
        L.Format ("GPLEV read : %u ns/op\r\n", (t1 - t0) * 1000 / N); out (ctx, (const char *) L);
        t0 = CTimer::GetClockTicks ();
        for (unsigned i = 0; i < N; i++) vmpu68_reg_write (VREG_DATA, (uint16_t) i);
        t1 = CTimer::GetClockTicks ();
        L.Format ("reg_write : %u ns/op\r\n", (t1 - t0) * 1000 / N); out (ctx, (const char *) L);
        t0 = CTimer::GetClockTicks ();
        for (unsigned i = 0; i < N; i++) (void) vmpu68_reg_read (VREG_STATUS);
        t1 = CTimer::GetClockTicks ();
        L.Format ("reg_read  : %u ns/op\r\n", (t1 - t0) * 1000 / N); out (ctx, (const char *) L);
        t0 = CTimer::GetClockTicks ();                      // a bus read's GPIO traffic without the bus cycle
        for (unsigned i = 0; i < N; i++) { vmpu68_reg_write (VREG_DATA, (uint16_t) i); (void) vmpu68_reg_read (VREG_STATUS); (void) vmpu68_reg_read (VREG_STATUS); }
        t1 = CTimer::GetClockTicks ();
        L.Format ("write+2 reads (dir turnaround) : %u ns/op\r\n", (t1 - t0) * 1000 / N); out (ctx, (const char *) L);
        t0 = CTimer::GetClockTicks ();
        for (unsigned i = 0; i < N; i++) hw_bus_write (wb + 2 * (i & 0xFFF), 1, (uint16_t) i);
        t1 = CTimer::GetClockTicks ();
        L.Format ("bus_write (posted) @%06X : %u ns/op\r\n", wb, (t1 - t0) * 1000 / N); out (ctx, (const char *) L);
        t0 = CTimer::GetClockTicks ();                      // same without the hw layer (lock/profile)
        for (unsigned i = 0; i < N; i++) { vmpu68_bus_write_posted (wb + 2 * (i & 0xFFF), 1, (uint16_t) i); if ((i & 31) == 31) vmpu68_bus_drain (); }
        t1 = CTimer::GetClockTicks ();
        L.Format ("  raw posted (drain/32) : %u ns/op\r\n", (t1 - t0) * 1000 / N); out (ctx, (const char *) L);
        t0 = CTimer::GetClockTicks ();
        for (unsigned i = 0; i < N; i++) vmpu68_bus_write (wb + 2 * (i & 0xFFF), 1, (uint16_t) i);
        t1 = CTimer::GetClockTicks ();
        L.Format ("  raw sync write        : %u ns/op\r\n", (t1 - t0) * 1000 / N); out (ctx, (const char *) L);
        uint16_t v;
        t0 = CTimer::GetClockTicks ();                      // same without the hw layer (lock/profile/op log)
        for (unsigned i = 0; i < N; i++) vmpu68_bus_read (0xFF0000 + 2 * (i & 0xFFF), 1, &v);
        t1 = CTimer::GetClockTicks ();
        L.Format ("  raw bus_read ROM      : %u ns/op\r\n", (t1 - t0) * 1000 / N); out (ctx, (const char *) L);
        for (int pf = 0; pf < 2; pf++)                      // plain reads, then with the prefetch stream
        {
            vmpu68_pf_enable (pf);
            t0 = CTimer::GetClockTicks ();
            for (unsigned i = 0; i < N; i++) hw_bus_read (wb + 2 * (i & 0xFFF), 1, &v);
            t1 = CTimer::GetClockTicks ();
            L.Format ("bus_read seq TVRAM %s: %u ns/op\r\n", pf ? "pf " : "   ", (t1 - t0) * 1000 / N); out (ctx, (const char *) L);
            t0 = CTimer::GetClockTicks ();
            for (unsigned i = 0; i < N; i++) hw_bus_read (0xFF0000 + 2 * (i & 0xFFF), 1, &v);
            t1 = CTimer::GetClockTicks ();
            L.Format ("bus_read seq ROM   %s: %u ns/op\r\n", pf ? "pf " : "   ", (t1 - t0) * 1000 / N); out (ctx, (const char *) L);
            t0 = CTimer::GetClockTicks ();                  // stride 1024: no stream ever starts
            for (unsigned i = 0; i < N; i++) hw_bus_read (0xE00000 + 1024 * (i & 0x1FF), 1, &v);
            t1 = CTimer::GetClockTicks ();
            L.Format ("bus_read stride    %s: %u ns/op\r\n", pf ? "pf " : "   ", (t1 - t0) * 1000 / N); out (ctx, (const char *) L);
            t0 = CTimer::GetClockTicks ();                  // 4 sequential words then a jump (streams start and abort)
            for (unsigned i = 0; i < N; i++) hw_bus_read (0xE00000 + 1024 * ((i >> 2) & 0x1FF) + 2 * (i & 3), 1, &v);
            t1 = CTimer::GetClockTicks ();
            L.Format ("bus_read 4-runs    %s: %u ns/op\r\n", pf ? "pf " : "   ", (t1 - t0) * 1000 / N); out (ctx, (const char *) L);
        }
        hw_set_irq_en (1);
        R = "bench done\r\n";
    }
    else if (p[0] == 'l' && p[1] == 'a' && p[2] == 't')    // lat <addr> [n]: PI_IRQ line up/down latency of a waited read (ns, avg of n)
    {
        p += 3;
        uint32_t a = parse_num (p);
        unsigned n = parse_num (p); if (!n) n = 1000;
        emu_quiesce ();
        hw_set_irq_en (0);
        uint64_t acc[4] = {0, 0, 0, 0};
        uint32_t mx[4] = {0, 0, 0, 0}, st = 0;
        for (unsigned i = 0; i < n; i++)
        {
            uint32_t o[4];
            vmpu68_lat_probe (a, o);
            st = o[3] >> 20; o[3] &= 0xFFFFF;
            for (int k = 0; k < 4; k++) { acc[k] += o[k]; if (o[k] > mx[k]) mx[k] = o[k]; }
            if (st & VST_FAULT) { hw_clear_fault (); }
        }
        hw_set_irq_en (1);
        R.Format ("lat %06X: line up %u ns (max %u)  down %u ns (max %u)  status %u ns  polls %u  st=%04X\r\n", a,
                  (unsigned) (acc[0] * 1000 / 54 / n), mx[0] * 1000 / 54,
                  (unsigned) (acc[1] * 1000 / 54 / n), mx[1] * 1000 / 54,
                  (unsigned) (acc[2] * 1000 / 54 / n), (unsigned) (acc[3] / n), st);
    }
    else if (p[0] == 'v' && p[1] == 'f' && p[2] == 'y')    // vfy <addr> <words> [seed]: posted-write then verify
    {
        p += 3;
        unsigned base = parse_num (p) & 0xFFFFFE;
        unsigned n = parse_num (p); if (n == 0) n = 4096;
        unsigned seed = parse_num (p);
        emu_quiesce ();
        hw_clear_fault ();                                  // a sticky fault makes posted writes drop
        for (unsigned i = 0; i < n; i++)
        {
            hw_bus_write (base + 2 * i, 1, (uint16_t) ((i * 0x9E37u) ^ seed ^ (i >> 3)));
            if (seed & 0x10000u) hw_status ();   // seed bit16 = drain every write (non-posted)
        }
        unsigned bad = 0, faults = 0, unstable = 0, first = 0xFFFFFFFF; uint16_t fv = 0, fv2 = 0, fe = 0;
        unsigned sbad = 0;                                  // pass 1: one sequential sweep (prefetch stream)
        for (unsigned i = 0; i < n; i++)
        {
            uint16_t v = 0, e = (uint16_t) ((i * 0x9E37u) ^ seed ^ (i >> 3));
            if (hw_bus_read (base + 2 * i, 1, &v) & VST_FAULT) { faults++; hw_clear_fault (); }
            if (v != e) { sbad++; if (first == 0xFFFFFFFF) { first = base + 2 * i; fv = v; fv2 = v; fe = e; } }
        }
        for (unsigned i = 0; i < n; i++)                    // pass 2: every word twice (stability)
        {
            uint16_t v = 0, v2 = 0, e = (uint16_t) ((i * 0x9E37u) ^ seed ^ (i >> 3));
            if (hw_bus_read (base + 2 * i, 1, &v) & VST_FAULT) { faults++; hw_clear_fault (); }
            hw_bus_read (base + 2 * i, 1, &v2);
            if (v != v2) unstable++;
            if (v != e) { bad++; if (first == 0xFFFFFFFF) { first = base + 2 * i; fv = v; fv2 = v2; fe = e; } }
        }
        R.Format ("vfy %06X x%u: seq-bad=%u bad=%u unstable=%u faults=%u first@%06X got %04X/%04X want %04X\r\n",
                  base, n, sbad, bad, unstable, faults, first, fv, fv2, fe);
    }
    else if (p[0] == 'i' && p[1] == 'a')                   // iack <level>: run an IACK cycle
    {
        p += 4;
        unsigned lvl = parse_num (p) & 7;
        uint16_t v = 0;
        uint16_t st = vmpu68_bus_read_fc (0xFFFFF0u | (lvl << 1) | 1u, 0, 7, &v);
        hw_clear_fault ();
        R.Format ("iack%u: data=%04X status=%04X%s%s\r\n", lvl, v, st,
                  (st & VST_FAULT) ? " FAULT" : "", (st & VST_VPA) ? " VPA" : "");
    }
    else if (p[0] == 'i' && p[1] == 'd')                   // id: flash JEDEC
    {
        uint8_t id[3];
        hw_flash_id (id);
        R.Format ("flash id: %02X %02X %02X\r\n", id[0], id[1], id[2]);
    }
    else if (p[0] == 'i' && p[1] == 'p')                   // ip: network state
    {
        if (!bNetOK)
            R = "wlan: not started\r\n";
        else if (!pNet->IsRunning ())
            R = "wlan: connecting...\r\n";
        else
        {
            CString IP;
            pNet->GetConfig ()->GetIPAddress ()->Format (&IP);
            R.Format ("wlan: up, http://%s/\r\n", (const char *) IP);
        }
    }
    else if (p[0] == 's' && p[1] == 'n' && p[2] == 'd')    // snd: drain all snoop records (count only), ~1s
    {
        vmpu68_snoop_t rec;
        unsigned n = 0, t0 = CTimer::Get ()->GetTicks ();
        while (CTimer::Get ()->GetTicks () - t0 < 100)
            if (hw_snoop_pop (&rec)) n++;
        R.Format ("%u record(s) drained\r\n", n);
    }
    else if (p[0] == 's' && p[1] == 'n' && p[2] != 'l' && p[2] != 's' && p[2] != 'r')    // sn: drain snoop
    {
        vmpu68_snoop_t rec;
        int n = 0;
        while (n < 8 && hw_snoop_pop (&rec))
        {
            CString L;
            L.Format ("snoop %06X = %04X (u%ul%u)\r\n", rec.addr, rec.data, rec.uds, rec.lds);
            out (ctx, (const char *) L);
            n++;
        }
        R.Format ("%d record(s)\r\n", n);
    }
    else if (p[0] == 'b' && p[1] == 'o' && p[2] == 'a')    // board [1|2|auto]: GPIO layout (saved to vmpu68.cfg, applied at the next boot)
    {
        p += 5; while (*p == ' ') p++;
        if (*p == '1' || *p == '2' || *p == 'a')
        {
            cfg_set_board (*p == 'a' ? 0 : (unsigned) (*p - '0'));
            int rc = cfg_save ();
            R.Format ("board: cfg %s (%s) - reboot to apply\r\n", *p == 'a' ? "auto" : (*p == '1' ? "1" : "2"), rc ? "SAVE FAILED" : "saved");
        }
        else
            R.Format ("board: %s (%s), console UART%u, cfg board=%u (0 = auto)\r\n",
                      vmpu68_board ()->name, s_board_how, vmpu68_board ()->uart, cfg_board ());
    }
    else if (p[0] == 'j' && p[1] == 'i' && p[2] == 't')    // jit [0|1|flush]: 68000 -> ARM64 dynamic translation (docs/design/jit-plan.md)
    {
        p += 3; while (*p == ' ') p++;
        emu_ensure ();
        if (*p == '0' || *p == '1') vmpu68_apply_settings (-1, -1, *p - '0', TRUE);   /* saved to vmpu68.cfg like the Web UI */
        else if (*p == 'f') { emu_quiesce (); jit_flush_all (); s_emu_run = TRUE; DataSyncBarrier (); }
        else if (*p == 't')                                // jit t [pc]: translate only, dump the code (emulator stays paused)
        {
            p++; while (*p == ' ') p++;
            emu_quiesce ();
            emu68k_regs_t rg; emu68k_get_regs (&rg);
            uint32_t pc = (*p >= '0') ? parse_num (p) : rg.pc;
            static char big[16384];
            jit_debug_translate (pc, big, sizeof big);
            R = "";
            for (char *q = big; *q; q++) { if (*q == '\n') R.Append ("\r\n"); else { char c[2] = {*q, 0}; R.Append (c); } }
            goto jit_done;
        }
        else if (*p == 's')                                // jit s: run one JIT block then stop (erg to inspect)
        {
            emu_quiesce ();
            jit_set_enabled (1); jit_set_once (1);
            s_emu_run = TRUE; DataSyncBarrier ();
            for (unsigned i = 0; i < 200 && !jit_once_done (); i++) CTimer::SimpleusDelay (1000);
            emu_quiesce ();
            R.Format ("jit s: %s\r\n", jit_once_done () ? "done (emu stopped, see erg)" : "TIMEOUT");
            char sbuf[256]; jit_stats (sbuf, sizeof sbuf); R.Append (sbuf); R.Append ("\r\n");
            goto jit_done;
        }
        { char buf[256]; jit_stats (buf, sizeof buf); R.Format ("%s\r\n", buf); }
    jit_done: ;
    }
    else if (p[0] == 'j' && p[1] == 'p')                   // jprof [0|1]: instruction/block/access profile for the JIT plan (Web: /api/jprof binary)
    {
        p += 5; while (*p == ' ') p++;
        emu_ensure ();
        if (*p == '0' || *p == '1') emu68k_prof_set (*p - '0');
        uint64_t ni = 0, nb = 0; const uint32_t *blk = emu68k_prof_blocks (&ni, &nb);
        const uint32_t *ops = emu68k_prof_ops ();
        R.Format ("jprof: %s, %llu instr, %llu blocks (mean %u.%u instr), reads shadow %u other %u, writes shadow %u other %u\r\n",
                  emu68k_prof_on () ? "on" : "off", (unsigned long long) ni, (unsigned long long) nb,
                  nb ? (unsigned) (ni / nb) : 0, nb ? (unsigned) (ni * 10 / nb % 10) : 0,
                  emu68k_prof_acc[0], emu68k_prof_acc[1], emu68k_prof_acc[2], emu68k_prof_acc[3]);
        if (ops && ni)
        {
            // top 12 opcodes
            unsigned top[12]; unsigned nt = 0;
            for (unsigned o = 0; o < 65536; o++)
            {
                if (!ops[o]) continue;
                unsigned i = nt < 12 ? nt++ : 11;
                if (nt == 12 && i == 11 && ops[o] <= ops[top[11]]) continue;
                top[i] = o;
                while (i > 0 && ops[top[i]] > ops[top[i - 1]]) { unsigned t = top[i]; top[i] = top[i - 1]; top[i - 1] = t; i--; }
            }
            for (unsigned i = 0; i < nt; i++)
            {
                CString L; L.Format ("  %04X %u (%u.%u%%)\r\n", top[i], ops[top[i]], (unsigned) ((uint64_t) ops[top[i]] * 100 / ni), (unsigned) ((uint64_t) ops[top[i]] * 1000 / ni % 10));
                R.Append (L);
            }
            CString L; L.Append ("  block len:");
            for (unsigned i = 1; i < 64; i++) if (blk[i]) { CString T; T.Format (" %u:%u", i, blk[i]); L.Append (T); }
            L.Append ("\r\n"); R.Append (L);
        }
    }
    else if (p[0] == 'v' && p[1] == 'e')                   // ver
    {
        CCPUThrottle *t = CCPUThrottle::Get ();
        R.Format ("vmpu68 %s (" VMPU68_BUILD ", circle %s) name \"%s\" cpu %u MHz %u C\r\n",
                  VMPU68_VERSION, CIRCLE_VERSION_STRING, cfg_name (),
                  t ? t->GetClockRate () / 1000000 : 0, t ? t->GetTemperature () : 0);
    }
    else if (p[0] == 'c' && p[1] == 'p' && p[2] == 'u')    // cpu <0|1>: ARM clock low (600MHz) / maximum
    {
        p += 3;
        unsigned m = parse_num (p);
        CCPUThrottle *t = CCPUThrottle::Get ();
        if (t) t->SetSpeed (m ? CPUSpeedMaximum : CPUSpeedLow);
        R.Format ("cpu %u MHz %u C\r\n", t ? t->GetClockRate () / 1000000 : 0, t ? t->GetTemperature () : 0);
    }
    else if (p[0] == 'e' && p[1] == 'b' && p[2] == 'm')    // ebm [addr]: bus timing benchmark (1000 word reads/writes at addr, default $ED0000;
    {                                                      //  an unpopulated addr measures the machine's bus-error timeout: reads only, faults counted)
        p += 3;
        unsigned addr = parse_num (p) & 0xFFFFFE;
        if (!addr) addr = 0xED0000;
        unsigned t0 = CTimer::GetClockTicks ();
        vmpu68_pace_bench (10000);
        unsigned t1 = CTimer::GetClockTicks ();
        uint16_t v = 0; unsigned faults = 0;
        for (unsigned i = 0; i < 1000; i++) if (hw_bus_read (addr, 1, &v) & VST_FAULT) faults++;
        unsigned t2 = CTimer::GetClockTicks ();
        unsigned t3 = t2;
        if (!faults) { for (unsigned i = 0; i < 1000; i++) hw_bus_write (addr, 1, v); t3 = CTimer::GetClockTicks (); }
        R.Format ("pace %u ns (set %u)  [%06X] bus read %u ns (%u/1000 faults)  bus write %s\r\n",
                  (t1 - t0) / 10, vmpu68_get_pace_ns (), addr, t2 - t1, faults, faults ? "skipped (faulting address)" : "");
        if (!faults) { CString W; W.Format ("%u ns\r\n", t3 - t2); R.Append (W); }
    }
    else if (p[0] == 'e' && p[1] == 'i' && p[2] == 'o')    // eio [MHz [maxcyc]]: I/O access timing (0 = unpaced), spacing cap in cycles
    {
        emu_ensure ();
        p += 3;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9')
        {
            unsigned mhz = parse_num (p);
            emu68k_set_io_mhz (mhz, parse_num (p));
        }
        R.Format ("io timing %u MHz%s, spacing kept up to %u cycles\r\n", emu68k_io_mhz (), emu68k_io_mhz () ? "" : " (unpaced)", emu68k_io_max_cyc ());
    }
    else if (p[0] == 'e' && p[1] == 'p' && p[2] == 'a')    // epace [ns]: GPIO strobe pacing
    {
        p += 5;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9')
        {
            emu_quiesce ();
            vmpu68_set_pace_ns (parse_num (p));
        }
        R.Format ("pace %u ns\r\n", vmpu68_get_pace_ns ());
    }
    else if (p[0] == 'e' && p[1] == 'p' && p[2] == 's')    // eps <ms>: pause core 1 like a Web request (diagnostics)
    {
        p += 3;
        unsigned ms = parse_num (p);
        if (!ms) ms = 100;
        vmpu68_emu_pause (1);
        CTimer::SimpleMsDelay (ms);
        vmpu68_emu_pause (0);
        R.Format ("paused %u ms\r\n", ms);
    }
    else if (p[0] == 'p' && p[1] == 'm')                   // pmax [n]: outstanding posted writes before a drain
    {
        p += 4;
        unsigned n = parse_num (p);
        if (n) emu68k_set_pmax (n);
        R.Format ("posted max %u (fdc %s)\r\n", hw_set_posted_max (0), emu68k_fdc_tight () ? "tight" : "idle");
    }
    else if (p[0] == 'p' && p[1] == 'f')                   // pf [0|1]: prefetch stream on/off; shows and clears the counters
    {
        p += 2;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') { emu_quiesce (); vmpu68_pf_enable (parse_num (p)); }
        uint32_t c[6];
        vmpu68_pf_stats (c, 1);
        R.Format ("pf %s  hits=%u misses=%u polls=%u aborts=%u nodata=%u\r\n", vmpu68_pf_enabled () ? "on" : "off", c[0], c[1], c[2], c[3], c[4]);
    }
    else if (p[0] == 'e' && p[1] == 'w' && p[2] == 'a')    // ewait [ns]: delay before the first STATUS poll; shows the poll histogram
    {
        p += 5;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') vmpu68_set_wait_ns (parse_num (p));
        uint32_t h[VMPU68_POLL_HIST];
        vmpu68_poll_hist (h, 1);
        R.Format ("wait %u ns  line=%u polls:", vmpu68_get_wait_ns (), h[0]);
        for (unsigned i = 1; i < VMPU68_POLL_HIST; i++)
        {
            CString L;
            L.Format (" %u%s=%u", i, i == VMPU68_POLL_HIST - 1 ? "+" : "", h[i]);
            R.Append (L);
        }
        uint64_t wp[12];
        vmpu68_wait_prof (wp, 1);
        if (wp[8])
        {
            CString L;
            L.Format ("\r\nai writes: n=%u 1-strobe=%u (%u%%) 2=%u 3=%u", (unsigned) wp[8], (unsigned) wp[9],
                      (unsigned) (wp[9] * 100 / wp[8]), (unsigned) wp[10], (unsigned) wp[11]);
            R.Append (L);
        }
        if (wp[4])
        {
            CString L;
            unsigned n = (unsigned) wp[4];
            L.Format ("\r\nread phases (ns, n=%u): start %u  pre %u  poll %u  data %u  hold ok=%u up-miss=%u down-miss=%u", n,
                      (unsigned) (wp[0] * 1000 / 54 / n), (unsigned) (wp[1] * 1000 / 54 / n),
                      (unsigned) (wp[2] * 1000 / 54 / n), (unsigned) (wp[3] * 1000 / 54 / n),
                      (unsigned) wp[5], (unsigned) wp[6], (unsigned) wp[7]);
            R.Append (L);
        }
        R.Append ("\r\n");
    }
    else if (p[0] == 'f' && p[1] == 'w')                   // fw <len>: self-update
    {
        p += 2;
        s_xfer_len = parse_num (p);
        if (s_xfer_len < 1024 || s_xfer_len > (8u << 20))
            R = "bad length\r\n";
        else
        {
            strcpy (s_xfer_path, "SD:/kernel8-rpi4.img");
            action = 2;
        }
    }
    else if (p[0] == 'w' && p[1] == 'i' && p[2] == 'f' && p[3] == 'i')   // wifi <ssid> <passphrase>: write SD:/wpa_supplicant.conf
    {
        p += 4; while (*p == ' ') p++;
        char ssid[64], psk[80]; unsigned i = 0;
        while (*p && *p != ' ' && i < sizeof ssid - 1) ssid[i++] = *p++; ssid[i] = 0;
        while (*p == ' ') p++;
        i = 0; while (*p && *p != '\r' && *p != '\n' && i < sizeof psk - 1) psk[i++] = *p++; psk[i] = 0;
        if (!ssid[0] || !psk[0])
        {
            FILINFO fi;
            R.Format ("usage: wifi <ssid> <passphrase>   (writes %s; %s)\r\n", WPA_CONFIG_FILE, f_stat (WPA_CONFIG_FILE, &fi) == FR_OK ? "a file exists" : "no file yet");
        }
        else
        {
            FIL f; CString S; S.Format ("country=JP\nnetwork={\n\tssid=\"%s\"\n\tpsk=\"%s\"\n\tproto=WPA2\n\tkey_mgmt=WPA-PSK\n}\n", ssid, psk);
            UINT bw; boolean ok = f_open (&f, WPA_CONFIG_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
            if (ok) { ok = f_write (&f, (const char *) S, S.GetLength (), &bw) == FR_OK; f_close (&f); }
            R.Format ("wifi: %s %s - reboot to connect\r\n", ok ? "wrote" : "FAILED to write", WPA_CONFIG_FILE);
        }
    }
    else if (p[0] == 'r' && p[1] == 'm' && p[2] == ' ')     // rm SD:/path
    {
        p += 3; while (*p == ' ') p++;
        FRESULT fr = f_unlink (p);
        R.Format ("rm %s: %s\r\n", p, fr == FR_OK ? "ok" : fr == FR_NO_FILE ? "no such file" : "failed");
    }
    else if (p[0] == 'p' && p[1] == 'u' && p[2] == 't')    // put <len> <path>
    {
        p += 3;
        s_xfer_len = parse_num (p);
        while (*p == ' ') p++;
        if (s_xfer_len == 0 || s_xfer_len > (8u << 20) || *p == 0)
            R = "usage: put <len> SD:/path\r\n";
        else
        {
            unsigned i = 0;
            while (p[i] && p[i] != ' ' && i < sizeof s_xfer_path - 1)
            {
                s_xfer_path[i] = p[i];
                i++;
            }
            s_xfer_path[i] = 0;
            action = 2;
        }
    }
    else if (p[0] == 'f' && p[1] == 'p' && p[4] == 'd')    // fpgadiag
    {
        action = 7;
    }
    else if (p[0] == 'f' && p[1] == 'p')                   // fpga SD:/path.bin
    {
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (*p == 0)
            R = "usage: fpga SD:/path.bin\r\n";
        else
        {
            unsigned i = 0;
            while (p[i] && p[i] != ' ' && i < sizeof s_xfer_path - 1)
            {
                s_xfer_path[i] = p[i];
                i++;
            }
            s_xfer_path[i] = 0;
            p += i;
            unsigned us = parse_num (p);       // optional pace override
            s_fpga_us = (us >= 1 && us <= 200) ? us : 10;
            action = 6;
        }
    }
    else if (p[0] == 'y' && p[1] == 't')                   // yt <us> <secs>: yield-rate test
    {
        p += 2;
        s_xfer_len = parse_num (p);
        s_xfer_addr = parse_num (p);
        action = 9;
    }
    else if (p[0] == 'u' && p[1] == 'p' && p[2] == 'd')    // upd [SD:/pkg.vpk] [try] [force]
    {
        p += 3;
        strcpy (s_xfer_path, UPDATE_FILE);
        g_Update.try_mode = g_Update.force = 0;
        for (;;)
        {
            while (*p == ' ') p++;
            if (*p == 0) break;
            unsigned i = 0;
            char w[128];
            while (p[i] && p[i] != ' ' && i < sizeof w - 1) { w[i] = p[i]; i++; }
            w[i] = 0;
            p += i;
            if (strcmp (w, "try") == 0)        g_Update.try_mode = 1;
            else if (strcmp (w, "force") == 0) g_Update.force = 1;
            else                               strcpy (s_xfer_path, w);
        }
        action = 8;
    }
#ifdef VMPU68_WITH_EMU
    else if (p[0] == 'e' && p[1] == 'l' && p[2] == 'd')    // eld <len> <addr>
    {
        p += 3;
        s_xfer_len = parse_num (p);
        s_xfer_addr = parse_num (p);
        if (s_xfer_len == 0 || s_xfer_len > (1u << 20))
            R = "usage: eld <len> <addr>\r\n";
        else
            action = 3;
    }
    else if (p[0] == 'b' && p[1] == 'i')                   // bi [0|1]: bus completion on the PI_IRQ line
    {
        p += 2;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') { emu_quiesce (); hw_set_busy_irq (parse_num (p) ? 1 : 0); }
        R.Format ("busy_irq %s\r\n", hw_busy_irq () ? "on" : "off (STATUS polling)");
    }
    else if (p[0] == 'a' && p[1] == 'i')                   // ai [0|1]: 1-strobe sequential writes (FPGA auto-advance)
    {
        p += 2;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') { emu_quiesce (); hw_set_wr_ai (parse_num (p) ? 1 : 0); }
        R.Format ("wr_ai %s\r\n", hw_wr_ai () ? "on" : "off (3-write)");
    }
    else if (p[0] == 's' && p[1] == 'l' && p[2] == 'o')    // slow [0|1]: 5-tick bus cycles (0.1.16 and before) / 4-tick 68000-like (default since 0.1.17, docs 30); stops the emulator
    {
        p += 4;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') { emu_quiesce (); hw_set_bus_slow (parse_num (p) ? 1 : 0); }
        R.Format ("bus cycle %s\r\n", hw_bus_slow () ? "5 clocks (slow)" : "4 clocks");
    }
    else if (p[0] == 'a' && p[1] == 's')                   // as <0|1>: 2-write bus cycle mode (faster)
    {
        p += 2;
        hw_set_autostart (parse_num (p) ? 1 : 0);
        R = "autostart set\r\n";
    }
    else if (p[0] == 'a' && p[1] == 'u' && p[2] == 't')    // auto [0|1]: X68000 power/reset supervision
    {
        p += 4;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9')                        // no argument: just show
            s_auto = parse_num (p) ? TRUE : FALSE;
        R.Format ("auto %s: runs=%u on=%u emu_run=%u active=%u lock=%u st=%04X drv=%04X alive_since=%u dead_since=%u dead_ms=%u ever_dead=%u was_alive=%u sig=%u pwroff=%u\r\n",
                  s_auto ? "on" : "off", s_sup.nRuns, s_x68_on, s_emu_run, s_emu_active, s_sup.nLastLock, s_sup.nLastSt, hw_drv_state (),
                  s_sup.nAliveSince, s_sup.nDeadSince, s_sup.nDeadMs, s_sup.bEverDead, s_sup.bWasAlive, s_sup.nLastSig, s_sup.nPwrOff);
    }
    else if (p[0] == 'e' && p[1] == 'r' && p[2] == 'u')    // erun <0|1>: free-run on/off (core 1)
    {
        p += 4;
        while (*p == ' ') p++;
        emu_ensure ();
        if (*p == '1')      { s_emu_run = TRUE; DataSyncBarrier (); }
        else if (*p == '0') emu_quiesce ();
        /* no argument: report only - a bare "erun" used to stop the core, which
         * made every status poll a silent halt (docs 32.3) */
        R.Format ("emu %s (pc=%06X)\r\n", s_emu_run ? "running" : "stopped", emu68k_pc ());
    }
    else if (p[0] == 'b' && p[1] == 'c' && p[2] == 'l')    // bcls: bus clock class in use + the table
    {
        R.Format ("bus clock %u.%u MHz, classes from %s:\r\n", s_bus_mhz10 / 10, s_bus_mhz10 % 10, cfg_bus_source ());
        for (unsigned i = 0; i < cfg_bus_count (); i++)
        {
            const cfg_bus_class *c = cfg_bus_class_at (i);
            CString L;
            if (c->wr_setup < 0)
                L.Format ("  %s>= %u.%u MHz: wr_setup auto%s, wait %u ns, io %u MHz, ai %s\r\n", c == s_bus_cls ? "*" : " ",
                          c->min_mhz10 / 10, c->min_mhz10 % 10, c == s_bus_cls && s_bus_setup_auto >= 0 ? probed_str () : "",
                          c->wait_ns, c->io_mhz, c->ai ? "on" : "off");
            else
                L.Format ("  %s>= %u.%u MHz: wr_setup %d, wait %u ns, io %u MHz, ai %s\r\n", c == s_bus_cls ? "*" : " ",
                          c->min_mhz10 / 10, c->min_mhz10 % 10, c->wr_setup, c->wait_ns, c->io_mhz, c->ai ? "on" : "off");
            R.Append (L);
        }
    }
    else if (p[0] == 'w' && p[1] == 's' && p[2] == 'e')    // wsetup [0|1]: extra write-data setup tick before AS (REG3 bit15)
    {
        p += 6;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') hw_set_wr_setup (parse_num (p));
        R.Format ("wsetup %d tick(s) of write-data setup before AS\r\n", hw_wr_setup ());
    }
    else if (p[0] == 'p' && p[1] == 'w' && (p[2] == ' ' || p[2] == 0))   // pw [addr [words]]: phantom watch (docs 32): read the block back after every bus write, stop on a change the CPU did not make
    {
        p += 2;
        while (*p == ' ') p++;
        emu_ensure ();
        if (*p >= '0' && *p <= '9')
        {
            unsigned a = parse_num (p);
            unsigned n = parse_num (p);
            unsigned us = parse_num (p);                   // pw addr words [interval_us]
            emu68k_pw_set (a, n ? n : 1, us);
            R.Format ("pw: %s %06X words %u every %u us\r\n", a ? "watching" : "off", a, n ? n : 1, us ? us : 1000);
        }
        else
        {
            emu68k_pw_t e; uint32_t a, n, checks;
            int hits = emu68k_pw_get (&e, &a, &n, &checks);
            R.Format ("pw: %s %06X words %u, checks %u, hits %d\r\n", a ? "watching" : "off", a, n, checks, hits);
            if (hits)
            {
                CString L;
                L.Format ("  first: +%u us seq %u pc %06X after %s write [%06X] <- %04X : [%06X] %04X -> %04X (%u words differ), instr %u\r\n",
                          e.us, e.seq, e.pc, e.word ? "word" : "byte", e.wa, e.wv, e.addr, e.old, e.now, e.n, e.instr);
                R.Append (L);
            }
        }
    }
    else if (p[0] == 'w' && p[1] == 'b' && p[2] == 's')    // wbs <ns>: experiment - spin per write-back RAM write (0 = off)
    {
        p += 3;
        unsigned ns = parse_num (p);
        emu68k_wb_set_slow_ns (ns);
        R.Format ("wb slow: %u ns per write\r\n", ns);
    }
    else if (p[0] == 'w' && p[1] == 'b')                   // wb [0|1]: write-back main RAM (docs 31) on/off + statistics
    {
        p += 2;
        while (*p == ' ') p++;
        emu_ensure ();
        if (*p >= '0' && *p <= '9') { emu_quiesce (); emu68k_wb_set (parse_num (p) ? 1 : 0); s_emu_run = TRUE; DataSyncBarrier (); }
        uint32_t o[8];
        emu68k_wb_stats (o);
        R.Format ("wb %s: DMA starts %u, words written back %u, chain blocks %u, pages turned WT %u, writes WT %u / WB %u, dirty now %u words, WT pages now %u\r\n",
                  emu68k_wb_enabled () ? "on" : "off", o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7]);
    }
    else if (p[0] == 'w' && p[1] == 'q')                   // wq [0|1|<n>]: posted-write queue on core 2 (0 = write directly, 1 = on, n>=2 = depth limit); stats
    {
        p += 2;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9')
        {
            unsigned n = parse_num (p);
            if (n >= 2) hw_wq_set_limit (n);
            else { emu_quiesce (); hw_wq_enable (n ? 1 : 0); s_emu_run = TRUE; DataSyncBarrier (); }
        }
        hw_wq_stats_t w;
        hw_wq_stats (&w);
        R.Format ("wq %s: limit %u, depth %u (max %u) issued %llu, full waits %u, syncs %u (waited %u, %llu us, longest %llu us, timeouts %u), worker strobe time %llu ms\r\n",
                  w.on ? "on" : "off", w.limit, w.depth, w.max_depth, (unsigned long long) w.issued, w.full_waits, w.syncs, w.sync_waits,
                  (unsigned long long) (w.sync_t / 54), (unsigned long long) (w.sync_max_t / 54), w.sync_timeouts, (unsigned long long) (w.issue_t / 54000));
    }
    else if (p[0] == 'e' && p[1] == 's' && p[2] == 'c')    // esc [n]: last n CRTC scroll register writes: time, us since the last IACK (vector), HSYNC level at the write
    {
        p += 3;
        unsigned n = parse_num (p); if (!n || n > 200) n = 40;
        if (*p == 'r' || (p[0] == ' ' && p[1] == 'r')) { emu68k_scr_reset (); R = "cleared\r\n"; }
        else
        {
            CString L;
            for (int i = (int) n - 1; i >= 0; i--)
            {
                uint32_t us, dt, a; uint16_t d; unsigned vec, hs;
                if (!emu68k_scr_get ((uint32_t) i, &us, &dt, &a, &d, &vec, &hs)) continue;
                L.Format ("%u.%06u %06X iack+%u us vec=%02X hs=%u polls=%u\r\n", (us / 1000000) % 1000, us % 1000000, a, dt, vec, hs, d);
                out (ctx, (const char *) L);
            }
            R = "";
        }
    }
    else if (p[0] == 'e' && p[1] == 'i' && p[2] == 'd')    // eid [us]: hold after an interrupt acknowledge (68000 exception entry time); 0 = none
    {
        p += 3;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') emu68k_set_irq_entry_ns (parse_num (p) * 1000);
        R.Format ("eid: %u.%u us after an IACK\r\n", emu68k_irq_entry_ns () / 1000, (emu68k_irq_entry_ns () % 1000) / 100);
    }
    else if (p[0] == 'e' && p[1] == 'i' && p[2] == 'p')    // eip [MHz [us]]: pace at MHz for us after each interrupt acknowledge (raster handlers at the machine's speed); 0 = off
    {
        p += 3;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') { unsigned m = parse_num (p); unsigned us = 120; while (*p == ' ') p++; if (*p >= '0' && *p <= '9') us = parse_num (p); emu68k_set_irq_pace (m, us); }
        unsigned m, us; emu68k_irq_pace (&m, &us);
        R.Format ("eip: %u MHz for %u us after an IACK%s\r\n", m, us, m ? "" : " (off)");
    }
    else if (p[0] == 'e' && p[1] == 's' && p[2] == 'p')    // espd [MHz]: emulated clock limit (0 = unlimited)
    {
        p += 4;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9')
        {
            s_emu_mhz = parse_num (p);
            emu68k_set_pace_mhz (s_emu_mhz);
            DataSyncBarrier ();
        }
        R.Format ("espd %u MHz%s\r\n", s_emu_mhz, s_emu_mhz ? "" : " (unlimited)");
    }
    else if (p[0] == 'e' && p[1] == 'c' && p[2] == 'p')    // ecpu [0|30]: MPU selection (applied at the next reset)
    {
        emu_ensure ();
        p += 4;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9')
        {
            unsigned m = parse_num (p);
            emu_quiesce ();
            emu68k_set_cpu30 (m == 30 || m == 3 || m == 1);
        }
        int on; uint32_t port; unsigned ctl;
        emu68k_xt30_info (&on, &port, &ctl);
        R.Format ("cpu %s  xellent30 %s port %06X ctl=%04X\r\n",
                  emu68k_cpu30 () ? "68030" : "68000", on ? "on" : "off", port, ctl);
    }
    else if (p[0] == 'x' && p[1] == 'e' && p[2] == 'l')    // xel <0|1> [port]: Xellent30 port emulation
    {
        emu_ensure ();
        p += 3;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9')
        {
            unsigned on = parse_num (p);
            uint32_t port = 0;
            while (*p == ' ') p++;
            if (*p) port = parse_num (p);
            emu_quiesce ();
            emu68k_xt30_config (on != 0, port);
        }
        int on; uint32_t port; unsigned ctl;
        emu68k_xt30_info (&on, &port, &ctl);
        R.Format ("xellent30 %s port %06X ctl=%04X cpu %s\r\n",
                  on ? "on" : "off", port, ctl, emu68k_cpu30 () ? "68030" : "68000");
    }
    else if (p[0] == 'e' && p[1] == 's' && p[2] == 'h')    // esh <addr> [words]: emulator-visible memory
    {
        p += 3;
        unsigned a = parse_num (p) & 0xFFFFFE;
        unsigned n = parse_num (p); if (n == 0 || n > 64) n = 16;
        emu_ensure ();
        CString L;
        for (unsigned i = 0; i < n; i++)
        {
            L.Format ("%s%04X%s", (i % 8) ? " " : "", emu68k_peek16 (a + 2 * i), ((i % 8) == 7 || i == n - 1) ? "\r\n" : "");
            out (ctx, (const char *) L);
        }
    }
    else if (p[0] == 'j' && p[1] == 'o' && p[2] == 'y')    // joy [port] [val] [ms]: inject a joystick read ($E9A001/3), val active-low (default 0xDF = button A), ms hold (default 200)
    {
        p += 3;
        unsigned port = parse_num (p);
        const char *q = p; unsigned val = parse_num (p);
        unsigned ms = parse_num (p);
        emu_ensure ();
        emu68k_joy_set ((int) port, q == p ? 0xDF : val, ms ? ms : 200);
        R.Format ("joy%u = 0x%02X for %u ms\r\n", port, (q == p ? 0xDF : val) & 0xFF, ms ? ms : 200);
    }
    else if (p[0] == 'e' && p[1] == 'k' && p[2] == 'e')    // ekey [-n] <text>: type text into the IOCS key buffer (+Enter unless -n); ^M ^[ ^C.. ^^
    {
        p += 4;
        while (*p == ' ') p++;
        bool enter = true;
        if (p[0] == '-' && p[1] == 'n') { enter = false; p += 2; while (*p == ' ') p++; }
        unsigned dropped;
        unsigned sent = vmpu68_emu_type (p, enter, &dropped);
        R.Format ("ekey: %u keys%s\r\n", sent, dropped ? " (some dropped)" : "");
    }
    else if (p[0] == 'e' && p[1] == 'p' && p[2] == 'c')    // epc <addr>: set PC (core must be stopped)
    {
        p += 3;
        unsigned a = parse_num (p);
        emu_ensure ();
        emu_quiesce ();
        emu68k_set_pc (a);
        R.Format ("pc=%06X\r\n", emu68k_pc ());
    }
    else if (p[0] == 't' && p[1] == 'c' && p[2] == 't')    // tct: MFP Timer-C stopwatch test (cpupower sequence, Pi-timed)
    {
        emu_quiesce ();
        uint16_t cr = 0, v;
        hw_bus_read (0xE8801D, 0, &cr);
        hw_bus_write (0xE8801D, 0, cr & 0x0F);             // stop Timer-C
        hw_bus_write (0xE88023, 0, 0);                      // counter := 0 (256)
        // MC68901 returns the counter captured on the previous DS rising
        // edge: read twice (first read = capture) to get the current value
        hw_bus_read (0xE88023, 0, &v); hw_bus_read (0xE88023, 0, &v);
        CString L;
        L.Format ("tcdcr=%02X stopped tcdr=%02X\r\n", cr, v); out (ctx, (const char *) L);
        hw_bus_write (0xE8801D, 0, cr | 0x70);             // restart, prescale /200 (50us)
        unsigned t0 = CTimer::GetClockTicks ();
        static const unsigned at[] = {0, 100, 200, 500, 1000, 2000, 5000, 10000, 12000, 15000, 25000};
        for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++)
        {
            while (CTimer::GetClockTicks () - t0 < at[i]) ;
            hw_bus_read (0xE88023, 0, &v); hw_bus_read (0xE88023, 0, &v);
            unsigned t = CTimer::GetClockTicks () - t0;
            L.Format ("  +%5uus tcdr=%02X (elapsed counts %3u)\r\n", t, v, (256 - v) & 0xFF); out (ctx, (const char *) L);
        }
        hw_bus_write (0xE8801D, 0, cr & 0x0F);
        hw_bus_write (0xE88023, 0, 200);                    // Human68k: 10ms tick
        hw_bus_write (0xE8801D, 0, cr);
        R = "tct done\r\n";
    }
    else if (p[0] == 'e' && p[1] == 'b' && p[2] == 'r')    // ebrk <pc>: breakpoint (0 = off)
    {
        p += 4;
        unsigned a = parse_num (p);
        emu_ensure ();
        emu68k_set_break (a ? a : 0xFFFFFFFF);
        R.Format ("break %s %06X\r\n", a ? "at" : "off", a);
    }
    else if (!memcmp (p, "ewb", 3))                        // ewb <lo> [hi]: break right after a CPU write into [lo,hi] (0 = off)
    {
        p += 3;
        while (*p == ' ') p++;
        emu_ensure ();
        if (*p == 0)                                       // no argument: report only, never disarm
            R.Format ("write-break: last hit pc %06X, %s\r\n", emu68k_wbrk_pc (), emu68k_brk_hit () ? "HIT" : "armed/idle");
        else
        {
            unsigned lo = parse_num (p); unsigned hi = parse_num (p);
            unsigned pclo = parse_num (p); unsigned pchi = parse_num (p);
            emu68k_set_wbreak (lo ? lo : 0xFFFFFFFF, hi ? hi : lo);
            emu68k_set_wbreak_pc (pclo, pchi ? pchi : (pclo ? 0xFFFFFF : 0xFFFFFFFF));
            R.Format ("write-break %s %06X..%06X pc %06X..%06X\r\n", lo ? "at" : "off", lo, hi ? hi : lo, pclo, pchi ? pchi : (pclo ? 0xFFFFFF : 0xFFFFFFFF));
        }
    }
    else if (p[0] == 'e' && p[1] == 't' && p[2] == 'r')    // etr [n]: last n PCs (newest last)
    {
        p += 3;
        unsigned n = parse_num (p); if (n == 0 || n > 200) n = 48;
        emu_ensure ();
        CString L;
        for (int i = (int) n - 1; i >= 0; i--)
        {
            L.Format ("%06X%s", emu68k_trace_get ((uint32_t) i), (i % 8) ? " " : "\r\n");
            out (ctx, (const char *) L);
        }
        R.Format ("(%u instructions total%s)\r\n", emu68k_trace_count (), emu68k_stopped () ? ", STOPPED at vector area" : "");
    }
    else if (p[0] == 'e' && p[1] == 'i' && p[2] == 'l')    // eil [n]: last n IACKs (newest last): L<level>:<vec>/A(utovector)/S(purious)
    {
        p += 3;
        unsigned n = parse_num (p); if (n == 0 || n > 64) n = 32;
        emu_ensure ();
        CString L;
        for (int i = (int) n - 1; i >= 0; i--)
        {
            unsigned e = emu68k_iack_get ((uint32_t) i);
            unsigned res = (e >> 4) & 3;
            if (res == 0) L.Format ("L%u:%02X%s", e & 7, e >> 8, (i % 8) ? " " : "\r\n");
            else          L.Format ("L%u:%c %s", e & 7, res == 1 ? 'A' : 'S', (i % 8) ? " " : "\r\n");
            out (ctx, (const char *) L);
        }
        R.Format ("(%u IACKs total)\r\n", emu68k_iack_count ());
    }
    else if (p[0] == 'e' && p[1] == 'i' && p[2] == 'a' && p[3] == 'l')   // eial <0|1>: eiq logs every level (diagnostics)
    {
        p += 4;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') iack_log_all = parse_num (p) ? 1 : 0;
        R.Format ("eial %d\r\n", iack_log_all);
    }
    else if (p[0] == 'e' && p[1] == 'i' && p[2] == 'q')    // eiq [n]: last n IACKs of levels 1-4 (newest last): time level result vector pc sr STATUS
    {
        p += 3;
        unsigned n = parse_num (p); if (n == 0 || n > 32) n = 16;
        emu_ensure ();
        CString L;
        for (int i = (int) n - 1; i >= 0; i--)
        {
            emu68k_iack_lo_t e;
            if (!emu68k_iack_lo_get ((uint32_t) i, &e)) continue;
            L.Format ("%u.%06u L%u %s vec=%02X pc=%06X sr=%04X st=%04X\r\n", (e.us / 1000000) % 1000, e.us % 1000000,
                      e.level, e.r == 0 ? "VEC " : e.r == 1 ? "AUTO" : "SPUR", e.vec, e.pc, e.sr, e.st);
            out (ctx, (const char *) L);
        }
        R.Format ("(%u low-level IACKs total, stale=%u, late=%u)\r\n", emu68k_iack_lo_count (), emu68k_iack_stale (), emu68k_iack_late ());
    }
    else if (p[0] == 'e' && p[1] == 'i' && p[2] == 'a')    // eia [n]: IACK anomalies (spurious / fault flag set before the IACK) with the preceding bus ops
    {
        p += 3;
        unsigned n = parse_num (p); if (n == 0 || n > 8) n = 8;
        emu_ensure ();
        static emu68k_anom_t a;
        CString L;
        for (int i = (int) n - 1; i >= 0; i--)
        {
            if (!emu68k_anom_get ((uint32_t) i, &a)) continue;
            L.Format ("[-%d] %uus L%u %s vec=%02X st_pre=%04X st=%04X sr=%04X ppc=%06X pc=%06X ops(newest last):",
                      i, a.us, a.level, a.r == 0 ? "VEC" : a.r == 1 ? "AUTO" : "SPUR", a.vec, a.st_pre, a.st, a.sr, a.ppc, a.pc);
            out (ctx, (const char *) L);
            unsigned first = a.op_n > 64 ? a.op_n - 64 : 0;
            for (unsigned k = first; k < a.op_n; k++)
            {
                const auto &o = a.ops[k & 63];
                L.Format ("%s%c%c%06X=%04X/%02X", ((k - first) % 6) ? " " : "\r\n  ", "rwWi"[o.kind & 3], (o.kind & 4) ? 'w' : 'b', o.addr, o.data, o.st);
                out (ctx, (const char *) L);
            }
            out (ctx, "\r\n");
        }
        R.Format ("(%u anomalies, prefault=%u, stale=%u, late=%u, IACKs=%u)\r\n", emu68k_anom_count (), hw_iack_prefault, emu68k_iack_stale (), emu68k_iack_late (), emu68k_iack_count ());
    }
    else if (!memcmp (p, "euv", 3))                        // euv <0|1>: stop the core on an interrupt through an IPL-unset vector (eex logs it either way)
    {
        p += 3;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') unset_vec_halt = parse_num (p) ? 1 : 0;
        R.Format ("unset-vector halt %s\r\n", unset_vec_halt ? "on" : "off");
    }
    else if (p[0] == 'e' && p[1] == 'e' && p[2] == 'x')    // eex [n]: last n exceptions with vector < 0x20 (newest last)
    {
        p += 3;
        unsigned n = parse_num (p); if (n == 0 || n > 32) n = 16;
        emu_ensure ();
        CString L;
        for (int i = (int) n - 1; i >= 0; i--)
        {
            emu68k_exc_t e;
            if (!emu68k_exc_get ((uint32_t) i, &e)) continue;
            L.Format ("%u.%06u vec %02X at %06X (next %06X) sr=%04X\r\n", (e.us / 1000000) % 1000, e.us % 1000000, e.vec, e.ppc, e.pc, e.sr);
            out (ctx, (const char *) L);
        }
        R.Format ("(%u exceptions total)\r\n", emu68k_exc_count ());
    }
    else if (p[0] == 'e' && p[1] == 'w' && p[2] == 't')    // ewt <addr> [len]: watch CPU accesses to [addr, addr+len) (0 = off)
    {
        p += 3;
        unsigned a = parse_num (p);
        unsigned len = parse_num (p);
        unsigned a2 = parse_num (p);                       // ewt a len a2 len2: two ranges into the same ring
        unsigned len2 = parse_num (p);
        emu_ensure ();
        emu68k_set_watch (a ? a : 0xFFFFFFFF, len);
        if (a2) emu68k_set_watch2 (a2, len2);
        R.Format ("watch %s %06X len %u%s\r\n", a ? "at" : "off", a, len ? len : 2, a2 ? " (+2nd range)" : "");
    }
    else if (p[0] == 'e' && p[1] == 'w' && p[2] == 'f')    // ewf <0|1>: 1 = the watch records writes only
    {
        p += 3;
        unsigned f = parse_num (p);
        emu_ensure ();
        emu68k_set_watch_flags ((int) f);
        R.Format ("watch %s\r\n", f ? "writes only" : "reads and writes");
    }
    else if (p[0] == 'e' && p[1] == 'w' && p[2] == 'l')    // ewl [n]: last n watched accesses (newest last): seq +us pc R/W addr data
    {
        p += 3;
        unsigned n = parse_num (p); if (n == 0 || n > 256) n = 32;
        unsigned skip = parse_num (p);                     // ewl n skip: leave out the newest skip records
        emu_ensure ();
        CString L;
        if (n + skip > emu68k_watch_count ()) n = emu68k_watch_count () > skip ? emu68k_watch_count () - skip : 0;
        uint32_t prev_us = 0;
        for (int i = (int) (n + skip) - 1; i >= (int) skip; i--)
        {
            emu68k_watch_t w;
            if (!emu68k_watch_get ((uint32_t) i, &w)) break;
            unsigned dt = i == (int) (n + skip) - 1 ? 0 : w.us - prev_us;
            if (w.size == 1)
                L.Format ("#%u %u.%06u +%u us pc=%06X %c %06X %02X x%u i=%u\r\n", w.seq, (w.us / 1000000) % 1000, w.us % 1000000, dt, w.pc, w.rd ? 'R' : 'W', w.addr, w.data & 0xFF, w.cnt, w.instr);
            else
                L.Format ("#%u %u.%06u +%u us pc=%06X %c %06X %04X x%u i=%u\r\n", w.seq, (w.us / 1000000) % 1000, w.us % 1000000, dt, w.pc, w.rd ? 'R' : 'W', w.addr, w.data, w.cnt, w.instr);
            prev_us = w.us;
            out (ctx, (const char *) L);
        }
        R.Format ("(%u watched accesses total, now %u.%06u)\r\n", emu68k_watch_count (),
                  (unsigned) ((vmpu68_ticks () / vmpu68_tick_hz ()) % 1000), (unsigned) (vmpu68_ticks () * 1000000ull / vmpu68_tick_hz () % 1000000));
    }
    else if (p[0] == 's' && p[1] == 'n' && p[2] == 's')    // sns: snoop statistics + suspicious records (1-sample or no-strobe)
    {
        emu_ensure ();
        uint32_t n[4], fc[8], ns, dr;
        emu68k_snoop_stats (n, fc, &ns, &dr);
        CString L;
        L.Format ("samples: 1:%u 2:%u 3:%u 4+:%u  fc: 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u  nostrobe:%u dropped:%u\r\n",
                  n[0], n[1], n[2], n[3], fc[0], fc[1], fc[2], fc[3], fc[4], fc[5], fc[6], fc[7], ns, dr);
        out (ctx, (const char *) L);
        uint32_t rg, rp, iw;
        emu68k_snoop_ovf_stats (&rg, &rp, &iw);
        L.Format ("ストリーム v2: %s  取りこぼし範囲: %u 回 (%u ワード再読込)  バス待ち中の取り出し: %u\r\n",
                  hw_snoop2 () ? "on" : "off", rg, rp, iw);
        out (ctx, (const char *) L);
        unsigned c = emu68k_sus_count (); if (c > 32) c = 32;
        for (int i = (int) c - 1; i >= 0; i--)
        {
            uint32_t seq, a; uint16_t d; unsigned f;
            if (!emu68k_sus_get ((uint32_t) i, &seq, &a, &d, &f)) break;
            L.Format ("#%u %06X %04X %s%s fc=%u n=%u rw=%u\r\n", seq, a, d, (f & 2) ? "u" : "-", (f & 1) ? "l" : "-",
                      (f >> 4) & 7, ((f >> 2) & 3) + 1, (f >> 7) & 1);
            out (ctx, (const char *) L);
        }
        R.Format ("(%u suspicious total)\r\n", emu68k_sus_count ());
    }
    else if (p[0] == 'e' && p[1] == 's' && p[2] == 'l')    // esl [n] [skip]: slow bus accesses (>= 6us), newest last
    {
        p += 3;
        while (*p == ' ') p++;
        if (*p == 'f' || *p == 'u')
        {
            emu68k_slow_freeze (*p == 'f');
            R.Format ("slow log %s\r\n", *p == 'f' ? "frozen" : "running");
        }
        else
        {
            unsigned n = parse_num (p); if (n == 0 || n > 64) n = 16;
            unsigned skip = parse_num (p);
            emu_ensure ();
            CString L;
            emu68k_slow_t e;
            for (int i = (int) (n + skip) - 1; i >= (int) skip; i--)
            {
                if (!emu68k_slow_get ((uint32_t) i, &e)) continue;
                if (e.wr == 2)
                    L.Format ("%u.%06u burst %06X..%06X n=%u %u us pc=%06X\r\n", e.us / 1000000, e.us % 1000000,
                              e.addr, e.a2, e.n, e.dt, e.pc);
                else
                    L.Format ("%u.%06u %s%c %06X %u us pc=%06X\r\n", e.us / 1000000, e.us % 1000000,
                              e.wr ? "wr" : "rd", e.word ? 'w' : 'b', e.addr, e.dt, e.pc);
                out (ctx, (const char *) L);
            }
            R.Format ("(%u slow accesses logged)\r\n", emu68k_slow_count ());
        }
    }
    else if (p[0] == 's' && p[1] == 'n' && p[2] == 'r')    // snr [n] [skip]: the newest snoop records (newest last), with time
    {
        p += 3;
        unsigned n = parse_num (p); if (n == 0 || n > 64) n = 16;
        unsigned skip = parse_num (p);
        emu_ensure ();
        CString L;
        uint32_t seq, a, us, prev = 0; uint16_t d; unsigned f;
        for (int i = (int) (n + skip) - 1; i >= (int) skip; i--)
        {
            if (!emu68k_snlog_get ((uint32_t) i, &seq, &a, &d, &f, &us)) continue;
            L.Format ("#%u %u.%06u +%u us %06X %04X %s%s fc=%u n=%u\r\n", seq, us / 1000000, us % 1000000, prev ? us - prev : 0,
                      a, d, (f & 2) ? "u" : "-", (f & 1) ? "l" : "-", (f >> 4) & 7, ((f >> 2) & 3) + 1);
            prev = us;
            out (ctx, (const char *) L);
        }
        R.Format ("(%u snoop records logged)\r\n", emu68k_snlog_count ());
    }
    else if (p[0] == 's' && p[1] == 'n' && p[2] == 'l')    // snl <addr> [n]: snoop-log records for one word (newest last): seq data u/l
    {
        p += 3;
        unsigned a = parse_num (p);
        unsigned n = parse_num (p); if (n == 0 || n > 64) n = 16;
        emu_ensure ();
        CString L;
        uint32_t back = 0, seq; uint16_t d; unsigned f;
        unsigned hits[64][3]; unsigned cnt = 0;
        while (cnt < n && emu68k_snlog_find (a, &back, &seq, &d, &f)) { hits[cnt][0] = seq; hits[cnt][1] = d; hits[cnt][2] = f; cnt++; back++; }
        for (int i = (int) cnt - 1; i >= 0; i--)
        {
            L.Format ("#%u %04X %s%s fc=%u n=%u rw=%u\r\n", hits[i][0], hits[i][1], (hits[i][2] & 2) ? "u" : "-", (hits[i][2] & 1) ? "l" : "-",
                      (hits[i][2] >> 4) & 7, ((hits[i][2] >> 2) & 3) + 1, (hits[i][2] >> 7) & 1);
            out (ctx, (const char *) L);
        }
        R.Format ("(%u hits shown, %u snoop records logged)\r\n", cnt, emu68k_snlog_count ());
    }
    else if (p[0] == 'h' && p[1] == 'b')                   // hb: sample the REG2 heartbeat bits (2 ms x 64)
    {
        R = "";
        for (unsigned i = 0; i < 64; i++)
        {
            CString t; t.Format ("%02X ", hw_reg_read (2) & 0xFF); R.Append (t);
            CTimer::SimpleusDelay (2000);
        }
        R.Append (x68_clock_alive () ? "alive\r\n" : "STOPPED\r\n");
    }
    else if (!memcmp (p, "rstw", 4))                       // rstw: 15 s trace of STATUS ipl/reset/halt transitions (front buttons)
    {
        // also: PLL lock (bit 8), emulator bus-fault count changes (bit 9) and
        // the raw fault flag (bit 1) - what else a front button might do
        unsigned t0 = CTimer::GetClockTicks (), n = 0, nTr = 0;
        uint32_t nF0, nFa, nFp; emu68k_fault_info (&nF0, &nFa, &nFp);
        auto sample = [&] () -> unsigned {
            unsigned st = hw_status () & 0xFA;
            if ((st & 0x38) != 0x38) st &= 0xC2;
            unsigned d = hw_reg_read (2);
            if (d & VDIAG_PLL_LOCK) st |= 0x100;
            if (d & VDIAG_RST_SEEN) st |= 0x400;
            uint32_t nF; emu68k_fault_info (&nF, &nFa, &nFp);
            if (nF != nF0) { st |= 0x200; nF0 = nF; }
            return st;
        };
        unsigned last = sample ();
        R.Format ("rstw: start st=%03X\r\n", last);
        while (CTimer::GetClockTicks () - t0 < 15000000 && nTr < 12)
        {
            unsigned st = sample (); n++;
            if (st != last)
            {
                CString t; t.Format ("  +%u us: %03X -> %03X (ipl=%u reset=%u halt=%u fault=%u lock=%u rst_seen=%u%s)\r\n", CTimer::GetClockTicks () - t0, last, st,
                                     (st >> 3) & 7, (st >> 6) & 1, (st >> 7) & 1, (st >> 1) & 1, (st >> 8) & 1, (st >> 10) & 1,
                                     (st & 0x200) ? " EMU-FAULT" : "");
                R.Append (t); last = st & ~0x200u; nTr++;
                if (st & 0x200) { t.Format ("      fault @%06X pc=%06X\r\n", nFa, nFp); R.Append (t); }
            }
        }
        CString t; t.Format ("  %u transitions, %u samples\r\n", nTr, n); R.Append (t);
    }
    else if (!memcmp (p, "xrst", 4))                       // xrst: reset the X68000 itself - drive RESET+HALT for 200 ms
    {                                                      // (like the front button), then bootstrap as after the button
        emu_ensure ();
        emu_quiesce ();
        hw_set_drv (1, 1);
        unsigned t0 = CTimer::GetClockTicks ();
        while (CTimer::GetClockTicks () - t0 < 200000) ;
        hw_set_drv (0, 0);
        s_sup.bBtnReset = TRUE;                            // the supervisor runs BootX68 ("X68000 reset")
        R = "xrst: RESET+HALT driven 200 ms, bootstrapping\r\n";
    }
    else if (!memcmp (p, "hltt", 4))                       // hltt: HALT_N loopback - drive our HALT for 50 ms, does HALT_IN read it back?
    {                                                      // (HALT alone resets nothing on the machine; the core ignores halt_in)
        unsigned nIn = 0, nOut = 0, nRst = 0, n = 0;
        unsigned t0 = CTimer::GetClockTicks ();
        hw_set_drv (-1, 1);
        while (CTimer::GetClockTicks () - t0 < 50000)
        {
            unsigned st = hw_status (); n++;
            if (st & VST_HALT_IN) nIn++;
            if (st & VST_RESET_IN) nRst++;
        }
        hw_set_drv (-1, 0);
        t0 = CTimer::GetClockTicks ();
        while (CTimer::GetClockTicks () - t0 < 50000)
            if (hw_status () & VST_HALT_IN) nOut++;
        R.Format ("hltt: while driving halt_in=%u/%u samples (reset_in=%u), after release halt_in=%u\r\n", nIn, n, nRst, nOut);
    }
    else if (p[0] == 'e' && p[1] == 'r' && p[2] == 's')    // ers: emu reset
    {
        emu_ensure ();
        emu_quiesce ();
        emu68k_reset ();
        R = "emu reset\r\n";
    }
    else if (p[0] == 'e' && p[1] == 'r' && p[2] == 'n')    // ern <cycles>
    {
        p += 3;
        unsigned cyc = parse_num (p);
        if (cyc == 0) cyc = 10000;
        emu_ensure ();
        emu_quiesce ();
        int done = emu68k_run ((int) cyc);
        emu68k_poll_irq ();
        int sn = emu68k_snoop_apply ();
        R.Format ("ran %d cycles, pc=%06X, snoops=%d\r\n",
                  done, emu68k_pc (), sn);
    }
    else if (p[0] == 'e' && p[1] == 'r' && p[2] == 'g')    // erg: emu regs
    {
        emu_ensure ();
        emu68k_regs_t r;
        emu68k_get_regs (&r);
        CString L;
        L.Format ("pc=%08X sr=%04X usp=%08X isp=%08X\r\n", r.pc, r.sr, r.usp, r.isp);
        out (ctx, (const char *) L);
        L.Format ("d0-3 %08X %08X %08X %08X\r\n", r.d[0], r.d[1], r.d[2], r.d[3]);
        out (ctx, (const char *) L);
        L.Format ("a0-3 %08X %08X %08X %08X\r\n", r.a[0], r.a[1], r.a[2], r.a[3]);
        out (ctx, (const char *) L);
        uint32_t fc, fa, fp;
        emu68k_fault_info (&fc, &fa, &fp);
        L.Format ("a4-7 %08X %08X %08X %08X  faults=%u last@%06X pc=%06X cyc=%u\r\n",
                  r.a[4], r.a[5], r.a[6], r.a[7], fc, fa, fp, (unsigned) s_emu_cycles);
        out (ctx, (const char *) L);
        L.Format ("prof: wr n=%u %ums | rd n=%u %ums | st n=%u %ums (54MHz ticks/54000)\r\n",
                  (unsigned) hw_prof.wr_n, (unsigned) (hw_prof.wr_t / 54000),
                  (unsigned) hw_prof.rd_n, (unsigned) (hw_prof.rd_t / 54000),
                  (unsigned) hw_prof.status_n, (unsigned) (hw_prof.status_t / 54000));
        out (ctx, (const char *) L);
        L.Format ("wr pattern: w+2=%u w-2=%u wsame=%u b+1=%u b-1=%u other=%u\r\n",
                  (unsigned) hw_prof.wr_seq[0], (unsigned) hw_prof.wr_seq[1], (unsigned) hw_prof.wr_seq[2],
                  (unsigned) hw_prof.wr_seq[3], (unsigned) hw_prof.wr_seq[4], (unsigned) hw_prof.wr_seq[5]);
        out (ctx, (const char *) L);
    }
#endif
    else if (p[0] == 'i' && p[1] == 'r' && p[2] == 'q')    // irq <level>: mock IPL
    {
        p += 3;
        unsigned lvl = parse_num (p);
        hw_mock_set_ipl (lvl);
        R.Format ("mock ipl=%u\r\n", lvl & 7);
    }
    else if (p[0] == 'h' && p[1] == 'a' && p[2] == 'n' && p[3] == 'g')  // hang: watchdog test
    {
        R = "hanging now (watchdog should reset in <=15s)\r\n";
        action = 5;
    }
    else if (p[0] == 'a' && p[1] == 'b')                   // ab: A/B boot state (SD kernels, config, pending marker)
    {
        R = "";
        static const char *files[] = { "SD:/kernel8-rpi4.img", "SD:/kernel8-try.img", "SD:/kernel_2712.img", "SD:/kernel8.img" };
        for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++)
        {
            FIL f; CString L;
            if (f_open (&f, files[i], FA_READ) == FR_OK)
            {
                boolean ok; u32 sz = f_size (&f); u32 sum = file_sum_range (&f, 0, sz, &ok); f_close (&f);
                L.Format ("  %s: %u bytes sum=%08X\r\n", files[i], sz, sum);
            }
            else L.Format ("  %s: (none)\r\n", files[i]);
            R.Append (L);
        }
        static const char *txt[] = { "SD:/config.txt", "SD:/tryboot.txt" };
        for (unsigned i = 0; i < 2; i++)
        {
            FIL f; CString L; char line[128];
            if (f_open (&f, txt[i], FA_READ) == FR_OK)
            {
                L.Format ("  %s:\r\n", txt[i]); R.Append (L);
                while (f_gets (line, sizeof line, &f))
                {
                    char *q = line; while (*q == ' ' || *q == '\t') q++;
                    if (*q == '#' || *q == '\r' || *q == '\n' || *q == 0) continue;
                    for (char *e = q; *e; e++) if (*e == '\r' || *e == '\n') { *e = 0; break; }
                    CString T; T.Format ("      %s\r\n", q); R.Append (T);
                }
                f_close (&f);
                L = "";
            }
            else L.Format ("  %s: (none)\r\n", txt[i]);
            R.Append (L);
        }
        FIL f; CString L; char line[160] = "";
        if (f_open (&f, "SD:/tryboot.pending", FA_READ) == FR_OK) { f_gets (line, sizeof line, &f); f_close (&f); for (char *e = line; *e; e++) if (*e == '\r' || *e == '\n') { *e = 0; break; } L.Format ("  pending marker: %s\r\n", line); }
        else L = "  pending marker: (none)\r\n";
        R.Append (L);
        L.Format ("  running: %s %s%s\r\n", VMPU68_VERSION, VMPU68_BUILD, s_ab_arm_promote ? " (try boot, will promote at 20s)" : "");
        R.Append (L);
        if (p[2] == 'p')                       // abp: promote the try kernel now
        { s_ab_arm_promote = 0; R.Append (ab_promote () ? "  -> promoted\r\n" : "  -> promote failed\r\n"); }
        else if (p[2] == 'x')                  // abx: discard the try kernel (roll back intent)
        { s_ab_arm_promote = 0; f_unlink (AB_MARKER); f_unlink (AB_TRY); R.Append ("  -> try slot discarded\r\n"); }
    }
    else if (p[0] == 's' && p[1] == 'm' && p[2] == 'i')    // smi [divi strobe setup hold n]: SMI bring-up + write-strobe train (experiment; reboots the bus)
    {
        p += 3;
        unsigned divi = parse_num (p), strobe = parse_num (p), setup = parse_num (p), hold = parse_num (p), n = parse_num (p);
#ifdef VMPU68_WITH_EMU
        emu_quiesce ();
#endif
        out (ctx, "SMI experiment: taking over the AD pins, FPGA held in reset (reboot afterwards)\r\n");
        smi_experiment (out, ctx, divi, strobe, setup, hold, n);
        R = "";
    }
    else if (p[0] == 'i' && p[1] == 'n' && p[2] == 'f' && p[3] == 'o')    // info: $ECFF00 record as the X68000 sees it
    {
        info_update ();
        R.Format ("equiv %u.%u MHz (68000 cycles/us over the last second), bus %u.%u MHz, ram %u MB\r\n",
                  s_equiv_mhz10 / 10, s_equiv_mhz10 % 10, s_bus_mhz10 / 10, s_bus_mhz10 % 10,
                  (unsigned) (emu68k_ram_size () >> 20));
        uint32_t e, total = emu68k_info_wlog (0, &e);
        CString t; t.Format ("port writes: %u\r\n", total); R.Append (t);
        for (uint32_t b = 0; b < 32 && emu68k_info_wlog (b, &e); b++)
        {
            t.Format ("  -%u: +%02X = %04X\r\n", b, e >> 16, e & 0xFFFF); R.Append (t);
        }
    }
    else if (p[0] == 't' && p[1] == 'b' && p[2] == 'r')    // tbr: tryboot reboot
    {
        R = "tryboot reboot...\r\n";
        action = 4;
    }
    else if (p[0] == 'r' && p[1] == 'e' && p[2] == 'b')    // reboot
    {
        out (ctx, "rebooting\r\n");
        action = 1;
    }
    else if (*p != 0)
    {
        R = "cmds: st | rd <a> [b] | wr <a> <v> [b] | led <n> | id | ip | sn |\r\n"
            "      fw <len> | put <len> SD:/path | fpga SD:/path.bin | upd [SD:/pkg.vpk] [try] [force] |\r\n"
            "      reboot | tbr | ver | ab [p|x] (A/B boot state; abp promote, abx discard try) |\r\n"
            "      eld <len> <addr> | ers | erun <0|1> | ern <cycles> | erg |\r\n"
            "      irq <n> | hang | yt <us> <secs> (yield-rate test) |\r\n"
            "      bench | vfy <a> <n> | as <0|1> | pf [0|1] | bi [0|1] | ewait [ns] | epace [ns] | eio [MHz] | auto [0|1] | hb | eex [n] | eiq [n]\r\n";
    }

    if (R.GetLength () > 0)
        out (ctx, (const char *) R);
    return action;
}

static void dev_put (void *ctx, const char *s)
{
    CDevice *pDev = (CDevice *) ctx;
    size_t n = 0;
    while (s[n]) n++;
    pDev->Write (s, n);
}

// receive nLength raw bytes from pDev; nAddr!=~0u -> emulator memory,
// else store at pPath on the SD
void CKernel::FileReceive (CDevice *pDev, const char *pPath, unsigned nLength)
{
    u8 *buf = (u8 *) malloc (nLength);
    if (!buf)
    {
        dev_put (pDev, "no mem\r\n");
        return;
    }
    dev_put (pDev, "GO\r\n");

    // chunked flow control: host sends at most XFER_CHUNK bytes, then waits
    // for a '.' ACK.  The CDC gadget's RX ring silently drops data on
    // overrun (sticky error breaks the console too), so never let the host
    // run ahead of us.
    const unsigned XFER_CHUNK = 2048;
    unsigned got = 0;
    unsigned next_ack = nLength < XFER_CHUNK ? nLength : XFER_CHUNK;
    u32 sum = 0;
    unsigned last = m_Timer.GetUptime ();
    while (got < nLength)
    {
#ifdef VMPU68_WATCHDOG
        m_Watchdog.Start (15);                 // reload during long transfers
#endif
        m_CDCGadget.UpdatePlugAndPlay ();
        m_Scheduler.Yield ();
        int n = pDev->Read (buf + got,
                            nLength - got > XFER_CHUNK ? XFER_CHUNK : nLength - got);
        if (n > 0)
        {
            for (int i = 0; i < n; i++)
                sum = sum * 31 + buf[got + i];
            got += (unsigned) n;
            last = m_Timer.GetUptime ();
            if (got >= next_ack)
            {
                pDev->Write (".", 1);
                next_ack = got + XFER_CHUNK;
                if (next_ack > nLength)
                    next_ack = nLength;
            }
        }
        else if (m_Timer.GetUptime () - last > 10)
        {
            dev_put (pDev, "timeout\r\n");
            free (buf);
            return;
        }
    }

#ifdef VMPU68_WITH_EMU
    if (pPath == nullptr)                    // emulator memory load
    {
        emu_ensure ();
        emu68k_load (s_xfer_addr, buf, nLength);
        free (buf);
        CString R;
        R.Format ("ok %u bytes sum=%08X -> emu %06X\r\n", nLength, sum, s_xfer_addr);
        dev_put (pDev, (const char *) R);
        return;
    }
#endif

    // create the parent directory if the path has one (best effort)
    {
        char dir[128];
        int slash = -1;
        for (int i = 0; pPath[i]; i++)
            if (pPath[i] == '/') slash = i;
        if (slash > 3)
        {
            for (int i = 0; i < slash; i++) dir[i] = pPath[i];
            dir[slash] = 0;
            f_mkdir (dir);
        }
    }

    FIL f;
    UINT bw = 0;
    statusled_sd_activity ();
    FRESULT fr = f_open (&f, pPath, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr == FR_OK)
    {
        fr = f_write (&f, buf, nLength, &bw);
        f_close (&f);
    }
    free (buf);

    CString R;
    if (fr == FR_OK && bw == nLength)
        R.Format ("ok %u bytes sum=%08X -> %s\r\n", bw, sum, pPath);
    else
        R.Format ("WRITE FAILED (fr=%d bw=%u)\r\n", (int) fr, bw);
    dev_put (pDev, (const char *) R);
}

// ---------------- FPGA configuration flash programming ----------------
// Talks to the W25Q32 directly (bypassing hw.c, whose flash entry points
// are redirected to a RAM image in mock mode - and mock is exactly the
// state we are in while the flash is still blank).  Uses the paced
// bit-bang: on this board the link fails above ~100kHz (RC-limited,
// see fpgadiag), so every transfer takes an explicit per-phase delay.

static void sspi_xfer (const u8 *tx, u8 *rx, unsigned n, unsigned us);
static void act_fast_tick (void);

static u8 sfl_status (void)
{
    u8 tx[2] = {0x05, 0}, rx[2];
    sspi_xfer (tx, rx, 2, s_fpga_us);
    act_fast_tick ();
    return rx[1];
}

static void sfl_wren (void)
{
    u8 c = 0x06;
    sspi_xfer (&c, 0, 1, s_fpga_us);
}

// keep the watchdog, the network and the USB gadget alive during long
// blocking operations (FPGA programming takes about a minute); HTTP
// requests are answered from the scheduler run here, so the installer's
// progress can be polled
// FPGA flash access (compare / erase / program): the RGB LED cannot show it
// (the FPGA is held in reset), so the Pi's ACT LED blinks at ~8 Hz meanwhile.
// Called from every yield and from every flash status poll.
static CKernel *s_pKernel = nullptr;
static boolean s_act_fast;
static void act_fast_tick (void)
{
    if (!s_act_fast || !s_pKernel) return;
    if ((now_ms () / 60) & 1) s_pKernel->ActLedOn (); else s_pKernel->ActLedOff ();
}
void CKernel::ServiceDuringUpdate (void)
{
#ifdef VMPU68_WATCHDOG
    m_Watchdog.Start (15);
#endif
    act_fast_tick ();                      // FPGA flash access: the Pi's ACT LED blinks fast (the RGB LED is dark: the FPGA is held in reset)
    m_CDCGadget.UpdatePlugAndPlay ();
    if (g_pDiscovery)
        g_pDiscovery->Poll ();
    m_Scheduler.Yield ();
}

// "<sum> <len>" of the bitstream last programmed, so that a package whose
// bitstream is unchanged skips the (slow) reprogramming
static u32 s_fpga_ver_flags;               // version of the bitstream being installed (vpk header flags: major<<16|minor<<8|patch), 0 = unknown
static void fpga_sum_write (u32 sum, unsigned len)
{
    FIL f;
    if (f_open (&f, FPGA_SUM_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;
    CString S;
    if (s_fpga_ver_flags)
        S.Format ("%08X %u %u.%u.%u\n", sum, len, s_fpga_ver_flags >> 16, (s_fpga_ver_flags >> 8) & 255, s_fpga_ver_flags & 255);
    else
        S.Format ("%08X %u\n", sum, len);
    UINT bw;
    f_write (&f, (const char *) S, S.GetLength (), &bw);
    f_close (&f);
}

// The bitstream version comes from the package header (flags) and is kept in
// FPGA_SUM_FILE as a third field.  A bitstream written by the console's
// "fpga SD:/x.bin" has no version; when the last package on the SD
// (UPDATE_FILE) carries the same bitstream, take the version from there.
static boolean vpk_hdr_read (const char *pPath, vpk_hdr_t *pHdr)
{
    FIL f; UINT br = 0; u8 hdr[VPK_HDR_SIZE];
    if (f_open (&f, pPath, FA_READ) != FR_OK) return FALSE;
    FRESULT fr = f_read (&f, hdr, sizeof hdr, &br);
    f_close (&f);
    if (fr != FR_OK || br != sizeof hdr || memcmp (hdr, VPK_MAGIC, 4) != 0) return FALSE;
    memcpy (pHdr, hdr, sizeof *pHdr);
    u32 hsum = 0;
    for (unsigned i = 0; i < VPK_HDR_SIZE - 4; i++) hsum = hsum * 31 + hdr[i];
    return pHdr->hdr_size == VPK_HDR_SIZE && hsum == pHdr->hdr_sum;
}
static boolean fpga_sum_read (u32 *pSum, unsigned *pLen);
static void fpga_version_fixup (void)
{
    FIL f; UINT br = 0; char line[48];
    if (f_open (&f, FPGA_SUM_FILE, FA_READ) != FR_OK) return;
    f_read (&f, line, sizeof line - 1, &br);
    f_close (&f);
    line[br] = 0;
    unsigned sp = 0;                            // fields: "<sum> <len> [<version>]"
    for (unsigned i = 0; line[i] && line[i] != '\r' && line[i] != '\n'; i++) if (line[i] == ' ' && line[i + 1] > ' ') sp++;
    if (sp >= 2) return;                        // version already recorded
    u32 sum; unsigned len; vpk_hdr_t h;
    if (!fpga_sum_read (&sum, &len)) return;
    u32 flags = 0; const char *from = UPDATE_FILE;
    if (vpk_hdr_read (UPDATE_FILE, &h) && h.fpga_len == len && h.fpga_sum == sum) flags = h.flags;
    if (!flags)
    {
        // released bitstreams (release/vmpu68-<ver>.vpk): sum -> the release that introduced it
        static const struct { u32 sum; u32 ver; } known[] = {
            { 0x83F73070, 0x000100 }, { 0x9610EB35, 0x000101 }, { 0x016736A9, 0x000102 }, { 0xCE273913, 0x000103 },
            { 0x4A3402A2, 0x000105 }, { 0x60AFBB62, 0x000106 }, { 0x8B345498, 0x000107 }, { 0x6EE83BCE, 0x000108 },
            { 0xA54FC955, 0x000109 }, { 0x6714AB36, 0x00010D }, { 0xF2B1BAC5, 0x000111 }, { 0x64DA5713, 0x010000 },   // 1.0.0 (same bitstream as 0.1.18)
        };
        for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++) if (known[i].sum == sum && len == 135100) { flags = known[i].ver; from = "release table"; }
    }
    if (!flags) return;
    s_fpga_ver_flags = flags;
    fpga_sum_write (sum, len);
    CLogger::Get ()->Write (FromKernel, LogNotice, "fpga: bitstream %08X is %u.%u.%u (from %s)", sum,
                    flags >> 16, (flags >> 8) & 255, flags & 255, from);
}

static boolean fpga_sum_read (u32 *pSum, unsigned *pLen)
{
    FIL f;
    char buf[32];
    UINT br = 0;
    if (f_open (&f, FPGA_SUM_FILE, FA_READ) != FR_OK)
        return FALSE;
    f_read (&f, buf, sizeof buf - 1, &br);
    f_close (&f);
    buf[br] = 0;
    const char *p = buf;
    u32 sum = 0;
    while ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F'))
    {
        sum = (sum << 4) | (u32) (*p <= '9' ? *p - '0' : *p - 'A' + 10);
        p++;
    }
    if (*p != ' ')
        return FALSE;
    *pSum = sum;
    *pLen = parse_num (p);
    return TRUE;
}

void CKernel::FpgaFlash (CDevice *pDev, const char *pPath)
{
    FIL f;
    if (f_open (&f, pPath, FA_READ) != FR_OK)
    {
        dev_put (pDev, "fpga: cannot open file\r\n");
        return;
    }
    unsigned len = f_size (&f);
    if (len == 0 || len > (1u << 20))
    {
        f_close (&f);
        dev_put (pDev, "fpga: bad file size\r\n");
        return;
    }
    u8 *img = (u8 *) malloc (len);
    UINT br = 0;
    FRESULT fr = img ? f_read (&f, img, len, &br) : FR_INT_ERR;
    f_close (&f);
    if (fr != FR_OK || br != len)
    {
        free (img);
        dev_put (pDev, "fpga: read failed\r\n");
        return;
    }
#ifdef VMPU68_WITH_EMU
    emu_quiesce ();                            // the bus goes away with the FPGA
#endif
    boolean bOK = FpgaFlashImage (pDev, img, len);
    free (img);
    if (!bOK)
        return;
    m_Timer.MsDelay (1500);
    ServiceDuringUpdate ();
    // re-probe the register file; configuration from flash can take tens of
    // seconds, so retry for up to 90 s instead of silently staying in mock mode
    int mock = 1;
    unsigned waited = 0;
    for (; waited < 90 && (mock = hw_init ()) != 0; waited++)
    {
        m_Timer.MsDelay (1000);
        ServiceDuringUpdate ();
    }
    CString R;
    R.Format ("fpga: %s after %us\r\n",
              mock ? "FPGA NOT detected" : "FPGA detected (sig 0x56)", waited);
    dev_put (pDev, (const char *) R);
}

// erase + program + verify the configuration flash from a RAM image;
// leaves CRESET_B released (FPGA reconfigures from the new image).
// Progress goes to pDev (one char per step) and to g_Update.
// called every 32 bytes of a transfer while installing: the WLAN stack
// stops responding when the main loop stays busy for more than ~20 ms
// between yields (measured: 20 ms fine, 30 ms degraded, 60 ms dead), and
// a page program at 10 us pace is ~60 ms
static void (*s_pXferYield) (void) = nullptr;

static void xfer_yield (void) { s_pKernel->ServiceDuringUpdate (); }

// Read the configuration flash and compare it with img (nothing is written).
// Holds CRESET_B low meanwhile, so the FPGA reconfigures afterwards: call
// before hw_init() / the emulator.  0 = identical, 1 = differs, -1 = no chip.
int CKernel::FpgaFlashCompare (CDevice *pDev, const u8 *img, unsigned len)
{
    const unsigned VCH = 16384;
    u8 *vtx = (u8 *) malloc (VCH + 4);
    u8 *vrx = (u8 *) malloc (VCH + 4);
    if (!vtx || !vrx) { free (vtx); free (vrx); return -1; }
    vmpu68_set_gpio_base ((volatile unsigned int *) ARM_GPIO_BASE);
    if (vmpu68_open () != 0) { free (vtx); free (vrx); return -1; }
    vmpu68_flash_begin ();
    s_pKernel = this;
    s_pXferYield = xfer_yield;
    s_act_fast = TRUE;
    { u8 w = 0xAB; sspi_xfer (&w, 0, 1, s_fpga_us); }       // release power-down
    CTimer::SimpleusDelay (5000);
    u8 tx[4] = {0x9F, 0, 0, 0}, rx[4];
    sspi_xfer (tx, rx, 4, s_fpga_us);
    int r = 0;
    if (rx[1] == 0x00 || rx[1] == 0xFF) r = -1;
    for (u32 a = 0; a < len && r == 0; a += VCH)
    {
        ServiceDuringUpdate ();
        unsigned n = len - a > VCH ? VCH : len - a;
        memset (vtx, 0, 4 + n);
        vtx[0] = 0x03; vtx[1] = (u8)(a >> 16); vtx[2] = (u8)(a >> 8); vtx[3] = (u8)a;
        sspi_xfer (vtx, vrx, 4 + n, s_fpga_us);
        if (memcmp (img + a, vrx + 4, n) != 0) r = 1;
    }
    s_pXferYield = nullptr; s_act_fast = FALSE; if (s_pKernel) s_pKernel->ActLedOff ();
    vmpu68_flash_end ();                       // CRESET_B released: the FPGA reconfigures from the flash
    free (vtx); free (vrx);
    CString R; R.Format ("fpga: flash id %02X %02X %02X, compare with %u bytes: %s\r\n", rx[1], rx[2], rx[3], len, r == 0 ? "identical" : r == 1 ? "differs" : "no chip");
    dev_put (pDev, (const char *) R);
    return r;
}

boolean CKernel::FpgaFlashImage (CDevice *pDev, const u8 *img, unsigned len)
{
    const unsigned VCH = 16384;                // verify/read chunk (sspi_xfer yields inside)
    u8 *vtx = (u8 *) malloc (VCH + 4);
    u8 *vrx = (u8 *) malloc (VCH + 4);
    if (!vtx || !vrx)
    {
        free (vtx); free (vrx);
        dev_put (pDev, "fpga: no mem\r\n");
        return FALSE;
    }

    // mock-mode hw_init() has closed the GPIO interface - reopen it
    vmpu68_set_gpio_base ((volatile unsigned int *) ARM_GPIO_BASE);
    if (vmpu68_open () != 0)
    {
        free (vtx); free (vrx);
        dev_put (pDev, "fpga: gpio open failed\r\n");
        return FALSE;
    }

    vmpu68_flash_begin ();                     // CRESET_B low: FPGA off
    s_pKernel = this;
    s_pXferYield = xfer_yield;
    s_act_fast = TRUE;

    // the iCE40 puts the flash into deep power-down (0xB9) after a
    // (failed) configuration attempt; in that state the chip ignores
    // everything except Release Power-down (0xAB) and MISO floats high
    { u8 w = 0xAB; sspi_xfer (&w, 0, 1, s_fpga_us); }
    CTimer::SimpleusDelay (5000);              // tRES1 >= 3us

    u8 tx[4] = {0x9F, 0, 0, 0}, rx[4];
    sspi_xfer (tx, rx, 4, s_fpga_us);          // real JEDEC id, not the mock's
    CString R;
    R.Format ("fpga: flash id %02X %02X %02X, image %u bytes, pace %uus\r\n",
              rx[1], rx[2], rx[3], len, s_fpga_us);
    dev_put (pDev, (const char *) R);
    if (rx[1] == 0x00 || rx[1] == 0xFF)
    {
        s_pXferYield = nullptr; s_act_fast = FALSE; if (s_pKernel) s_pKernel->ActLedOff ();
        vmpu68_flash_end ();
        free (vtx); free (vrx);
        dev_put (pDev, "fpga: no flash chip answer\r\n");
        return FALSE;
    }

    g_Update.phase = "fpga erase"; g_Update.done = 0; g_Update.total = (len + 65535) / 65536;
    for (u32 a = 0; a < len; a += 65536)       // 64KB block erase
    {
        ServiceDuringUpdate ();
        sfl_wren ();
        u8 e[4] = {0xD8, (u8)(a >> 16), (u8)(a >> 8), (u8)a};
        sspi_xfer (e, 0, 4, s_fpga_us);
        while (sfl_status () & 1)              // block erase: up to ~1 s
            ServiceDuringUpdate ();
        pDev->Write ("E", 1);
        g_Update.done++;
    }
    g_Update.phase = "fpga program"; g_Update.done = 0; g_Update.total = len;
    u8 page[260];
    for (u32 a = 0; a < len; a += 256)         // page program
    {
        // the HTTP status poll only advances one TCP step per yield, so
        // yield every page (~60 ms), not every 16 KB (~4 s)
        ServiceDuringUpdate ();
        if ((a & 0x3FFF) == 0)
            pDev->Write (".", 1);
        unsigned n = len - a > 256 ? 256 : len - a;
        sfl_wren ();
        page[0] = 0x02;
        page[1] = (u8)(a >> 16); page[2] = (u8)(a >> 8); page[3] = (u8)a;
        memcpy (page + 4, img + a, n);
        sspi_xfer (page, 0, 4 + n, s_fpga_us);
        while (sfl_status () & 1) ;
        g_Update.done = a + n;
    }
    g_Update.phase = "fpga verify"; g_Update.done = 0; g_Update.total = len;
    int bad = 0;
    for (u32 a = 0; a < len && !bad; a += VCH) // read back + verify
    {
        ServiceDuringUpdate ();
        unsigned n = len - a > VCH ? VCH : len - a;
        memset (vtx, 0, 4 + n);
        vtx[0] = 0x03;
        vtx[1] = (u8)(a >> 16); vtx[2] = (u8)(a >> 8); vtx[3] = (u8)a;
        sspi_xfer (vtx, vrx, 4 + n, s_fpga_us);
        if (memcmp (img + a, vrx + 4, n) != 0)
            bad = 1;
        if ((a & 0x3FFF) == 0)
            pDev->Write ("v", 1);
        g_Update.done = a + n;
    }
    s_pXferYield = nullptr; s_act_fast = FALSE; if (s_pKernel) s_pKernel->ActLedOff ();
    vmpu68_flash_end ();                       // CRESET_B released: FPGA boots
    free (vtx); free (vrx);
    if (bad)
    {
        dev_put (pDev, "\r\nfpga: VERIFY FAILED\r\n");
        return FALSE;
    }
    u32 sum = 0;
    for (unsigned i = 0; i < len; i++)
        sum = sum * 31 + img[i];
    { vpk_hdr_t h;                              // version known only when the last package carries this bitstream
      if (!(vpk_hdr_read (UPDATE_FILE, &h) && h.fpga_sum == sum && h.fpga_len == len)) s_fpga_ver_flags = 0; else s_fpga_ver_flags = h.flags; }
    fpga_sum_write (sum, len);
    dev_put (pDev, "\r\nfpga: verify ok, waiting for configuration\r\n");
    return TRUE;
}

// ---------------- release package (.vpk) install ----------------

TUpdateState g_Update = { 0, 0, 0, "idle", 0, 0, "" };

static u32 file_sum_range (FIL *f, u32 off, u32 len, boolean *pOK)
{
    static u8 buf[4096];
    u32 sum = 0;
    UINT br;
    *pOK = FALSE;
    if (f_lseek (f, off) != FR_OK)
        return 0;
    for (u32 done = 0; done < len; done += br)
    {
        UINT want = len - done > sizeof buf ? sizeof buf : len - done;
        if (f_read (f, buf, want, &br) != FR_OK || br != want)
            return 0;
        for (UINT i = 0; i < br; i++)
            sum = sum * 31 + buf[i];
    }
    *pOK = TRUE;
    return sum;
}

static void msg_set (char *pMsg, unsigned nMsg, const char *pText)
{
    unsigned i = 0;
    while (pText[i] && i < nMsg - 1) { pMsg[i] = pText[i]; i++; }
    pMsg[i] = 0;
}

boolean vmpu68_vpk_check (const char *pPath, vpk_hdr_t *pHdr, boolean *pFpgaSame,
                          boolean *pKernelSame, char *pMsg, unsigned nMsg)
{
    FIL f;
    UINT br = 0;
    if (f_open (&f, pPath, FA_READ) != FR_OK)
    {
        msg_set (pMsg, nMsg, "package file not found");
        return FALSE;
    }
    u8 hdr[VPK_HDR_SIZE];
    FRESULT fr = f_read (&f, hdr, sizeof hdr, &br);
    if (fr != FR_OK || br != sizeof hdr || memcmp (hdr, VPK_MAGIC, 4) != 0)
    {
        f_close (&f);
        msg_set (pMsg, nMsg, "not a vpk package");
        return FALSE;
    }
    memcpy (pHdr, hdr, sizeof *pHdr);          // header is packed little-endian = ARM layout
    u32 hsum = 0;
    for (unsigned i = 0; i < VPK_HDR_SIZE - 4; i++)
        hsum = hsum * 31 + hdr[i];
    unsigned size = f_size (&f);
    if (pHdr->hdr_size != VPK_HDR_SIZE || hsum != pHdr->hdr_sum)
    {
        f_close (&f);
        msg_set (pMsg, nMsg, "bad package header");
        return FALSE;
    }
    pHdr->label[VPK_LABEL_LEN - 1] = 0;
    if (pHdr->kernel_len < 1024 || pHdr->kernel_len > (8u << 20)
        || pHdr->kernel_off + pHdr->kernel_len > size
        || pHdr->fpga_len > (1u << 20)
        || (pHdr->fpga_len && pHdr->fpga_off + pHdr->fpga_len > size))
    {
        f_close (&f);
        msg_set (pMsg, nMsg, "package truncated");
        return FALSE;
    }
    boolean ok;
    u32 sum = file_sum_range (&f, pHdr->kernel_off, pHdr->kernel_len, &ok);
    if (!ok || sum != pHdr->kernel_sum)
    {
        f_close (&f);
        msg_set (pMsg, nMsg, "kernel checksum mismatch");
        return FALSE;
    }
    if (pHdr->fpga_len)
    {
        sum = file_sum_range (&f, pHdr->fpga_off, pHdr->fpga_len, &ok);
        if (!ok || sum != pHdr->fpga_sum)
        {
            f_close (&f);
            msg_set (pMsg, nMsg, "fpga checksum mismatch");
            return FALSE;
        }
    }
    f_close (&f);
    u32 csum; unsigned clen;
    *pFpgaSame = pHdr->fpga_len
              && fpga_sum_read (&csum, &clen)
              && csum == pHdr->fpga_sum && clen == pHdr->fpga_len;
    // installed kernel identical?  (summed from the SD, not from a note:
    // the image may have been replaced over serial or by swapping the card)
    *pKernelSame = FALSE;
    if (f_open (&f, "SD:/kernel8-rpi4.img", FA_READ) == FR_OK)
    {
        if (f_size (&f) == pHdr->kernel_len)
            *pKernelSame = file_sum_range (&f, 0, pHdr->kernel_len, &ok) == pHdr->kernel_sum && ok;
        f_close (&f);
    }
    msg_set (pMsg, nMsg, "ok");
    return TRUE;
}

// install a package: FPGA first (skipped when the bitstream is the one
// already programmed, unless bForce), then the kernel as stable or try
// image (skipped likewise when identical to the stable image).  Nothing to
// do at all is reported as an error ("already installed").  Returns TRUE when the caller should reboot; on failure g_Update
// carries the reason and nothing has been half-installed (the kernel copy
// is verified before the old image is replaced).
boolean CKernel::Update (CDevice *pDev, const char *pPath, boolean bTry, boolean bForce)
{
    vpk_hdr_t h;
    boolean same = FALSE, ksame = FALSE;
    CString R;

    g_Update.busy = 1;
    g_Update.phase = "check"; g_Update.done = g_Update.total = 0;
    if (!vmpu68_vpk_check (pPath, &h, &same, &ksame, g_Update.msg, sizeof g_Update.msg))
    {
        R.Format ("update: %s\r\n", g_Update.msg);
        dev_put (pDev, (const char *) R);
        g_Update.phase = "error"; g_Update.busy = 0;
        return FALSE;
    }
    boolean bKernel = bForce || bTry || !ksame;
    boolean bFpga = h.fpga_len && (bForce || !same);
    if (!bKernel && !bFpga)
    {
        msg_set (g_Update.msg, sizeof g_Update.msg, "already installed");
        dev_put (pDev, "update: already installed (use force)\r\n");
        g_Update.phase = "error"; g_Update.busy = 0;
        return FALSE;
    }
    R.Format ("update: %s, kernel %u bytes%s, fpga %u bytes%s -> %s\r\n",
              h.label, h.kernel_len, bKernel ? "" : " (unchanged, skip)", h.fpga_len,
              h.fpga_len ? (bFpga ? " (program)" : " (unchanged, skip)") : "",
              bTry ? "try" : "stable");
    dev_put (pDev, (const char *) R);
#ifdef VMPU68_WITH_EMU
    emu_quiesce ();
#endif

    s_fpga_ver_flags = h.flags;
    if (!bFpga && h.fpga_len && h.flags)
        fpga_sum_write (h.fpga_sum, h.fpga_len);   // same bitstream already programmed: just note its version
    if (bFpga)
    {
        g_Update.phase = "fpga read"; g_Update.done = 0; g_Update.total = h.fpga_len;
        u8 *img = (u8 *) malloc (h.fpga_len);
        FIL f;
        UINT br = 0;
        boolean ok = img && f_open (&f, pPath, FA_READ) == FR_OK;
        if (ok)
        {
            ok = f_lseek (&f, h.fpga_off) == FR_OK
              && f_read (&f, img, h.fpga_len, &br) == FR_OK && br == h.fpga_len;
            f_close (&f);
        }
        if (ok)
            ok = FpgaFlashImage (pDev, img, h.fpga_len);
        free (img);
        if (!ok)
        {
            msg_set (g_Update.msg, sizeof g_Update.msg, "fpga programming failed");
            g_Update.phase = "error"; g_Update.busy = 0;
            return FALSE;
        }
    }

    // kernel: copy to a temporary file, verify, then rename over the target
    const char *target = bTry ? "SD:/kernel8-try.img" : "SD:/kernel8-rpi4.img";
    const char *tmp = "SD:/kernel8.new";
    g_Update.phase = "kernel copy"; g_Update.done = 0; g_Update.total = h.kernel_len;
    if (bKernel)
    {
        FIL in, out;
        static u8 buf[16384];
        boolean ok = f_open (&in, pPath, FA_READ) == FR_OK;
        if (ok && f_open (&out, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        {
            f_close (&in);
            ok = FALSE;
        }
        if (ok)
        {
            ok = f_lseek (&in, h.kernel_off) == FR_OK;
            for (u32 done = 0; ok && done < h.kernel_len; )
            {
                UINT want = h.kernel_len - done > sizeof buf ? sizeof buf : h.kernel_len - done;
                UINT br = 0, bw = 0;
                ok = f_read (&in, buf, want, &br) == FR_OK && br == want
                  && f_write (&out, buf, br, &bw) == FR_OK && bw == br;
                done += br;
                g_Update.done = done;
                ServiceDuringUpdate ();
            }
            f_close (&in);
            if (f_close (&out) != FR_OK)
                ok = FALSE;
        }
        if (ok)
        {
            g_Update.phase = "kernel verify";
            boolean sok;
            ok = f_open (&in, tmp, FA_READ) == FR_OK;
            if (ok)
            {
                ok = file_sum_range (&in, 0, h.kernel_len, &sok) == h.kernel_sum && sok
                  && f_size (&in) == h.kernel_len;
                f_close (&in);
            }
        }
        if (ok)
        {
            f_unlink (target);
            ok = f_rename (tmp, target) == FR_OK;
        }
        if (!ok)
        {
            f_unlink (tmp);
            msg_set (g_Update.msg, sizeof g_Update.msg, "kernel install failed (SD write)");
            g_Update.phase = "error"; g_Update.busy = 0;
            dev_put (pDev, "update: kernel install failed\r\n");
            return FALSE;
        }
    }
    if (bTry)
        ab_write_marker (0, h.kernel_sum, h.label);   // A/B: boot ordinal 0; the try boot bumps it to 1
    else
    { f_unlink (AB_MARKER); f_unlink (AB_TRY); }       // direct install supersedes any pending try
    f_unlink (pPath);                          // installed: drop the package
    R.Format ("update: %s installed, %s\r\n", h.label, bTry ? "tryboot" : "rebooting");
    dev_put (pDev, (const char *) R);
    msg_set (g_Update.msg, sizeof g_Update.msg, "installed");
    g_Update.phase = "reboot";
    return TRUE;
}

// SPI-flash link diagnostics: raw GPIO bit-bang at two speeds, idle-level
// probes, and multi-opcode reads to tell "dead wire" from "misdecoded
// command" from "bus contention with the iCE40 config master".
#define GP_SET0 (ARM_GPIO_BASE + 0x1C)
#define GP_CLR0 (ARM_GPIO_BASE + 0x28)
#define GP_LEV0 (ARM_GPIO_BASE + 0x34)


// ---------------- SMI (Secondary Memory Interface) experiment ----------------
// SMI is the SoC's parallel-bus engine (address SAx, strobes SOE/SWE, data
// SDx, DMA-fed, ns-programmable timing).  Fits a register bus, but its pins
// are fixed (ALT1) and do NOT match this board - a board 1.1 re-route is
// needed to run the real bus (docs/design/smi-plan.md).  Here we bring SMI
// up and emit a write-strobe train for a scope.  Destructive: it takes over
// the AD pins and holds the FPGA in reset, so reboot afterwards.
#define SMI_BASE   (ARM_IO_BASE + 0x600000)
#define SMICS_R    (SMI_BASE + 0x00)
#define SMIDSW0_R  (SMI_BASE + 0x14)
#define SMIDCS_R   (SMI_BASE + 0x34)
#define SMIDA_R    (SMI_BASE + 0x38)
#define SMIDD_R    (SMI_BASE + 0x3c)
#define CM_SMICTL  (ARM_IO_BASE + 0x1010b0)
#define CM_SMIDIV  (ARM_IO_BASE + 0x1010b4)
#define CM_PWD     (0x5au << 24)
static void smi_set_fsel (unsigned pin, unsigned mode)
{
    unsigned reg = ARM_GPIO_BASE + (pin / 10) * 4, sh = (pin % 10) * 3;
    unsigned v = read32 (reg); v &= ~(7u << sh); v |= (mode & 7) << sh; write32 (reg, v);
}
static void smi_experiment (put_fn out, void *ctx, unsigned divi, unsigned strobe, unsigned setup, unsigned hold, unsigned n)
{
    if (!divi) divi = 1; if (!strobe) strobe = 2; if (!setup) setup = 1; if (!hold) hold = 1; if (!n) n = 200000;
    CString R;
    smi_set_fsel (vmpu68_board ()->creset, 1); write32 (GP_CLR0, 1u << vmpu68_board ()->creset);
    CTimer::SimpleusDelay (1000);
    write32 (CM_SMICTL, CM_PWD | (1u << 5));
    CTimer::SimpleusDelay (10);
    write32 (CM_SMIDIV, CM_PWD | ((divi & 0xfff) << 12));
    write32 (CM_SMICTL, CM_PWD | 1u);
    CTimer::SimpleusDelay (10);
    write32 (CM_SMICTL, CM_PWD | (1u << 4) | 1u);
    for (unsigned i = 0; i < 1000 && !(read32 (CM_SMICTL) & (1u << 7)); i++) CTimer::SimpleusDelay (1);
    smi_set_fsel (5, 5); smi_set_fsel (6, 5); smi_set_fsel (7, 5);
    for (unsigned g = 8; g <= 13; g++) smi_set_fsel (g, 5);   // SD0-5 only: keep GPIO14/15 as the serial console
    unsigned dsw = (1u << 30) | ((setup & 0x3f) << 24) | ((hold & 0x3f) << 16) | ((1u & 0x7f) << 8) | (strobe & 0x7f);
    write32 (SMIDSW0_R, dsw);
    write32 (SMICS_R, 0);
    write32 (SMIDA_R, 0);
    unsigned cyc = setup + strobe + hold + 1;
    unsigned clk_khz = 19200 / divi;
    R.Format ("SMI clk %u.%03u MHz (osc/%u), write cycle = setup %u + strobe %u + hold %u + pace 1 = %u cyc = %u ns\r\n",
              clk_khz / 1000, clk_khz % 1000, divi, setup, strobe, hold, cyc, (cyc * 1000000u + clk_khz / 2) / clk_khz);
    out (ctx, (const char *) R);
    R.Format ("SMIDSW0=%08X  emitting %u writes on SWE=GPIO7, SD0..5=GPIO8..13 ...\r\n", dsw, n); out (ctx, (const char *) R);
    unsigned t0 = CTimer::GetClockTicks ();
    for (unsigned i = 0; i < n; i++)
    {
        write32 (SMIDCS_R, (1u << 3) | (1u << 1) | 1u);
        write32 (SMIDD_R, (u16) (i & 0xffff));
        unsigned g = 0; while (!(read32 (SMIDCS_R) & (1u << 2)) && ++g < 100000) ;
        write32 (SMIDCS_R, (1u << 2));
    }
    unsigned dt = CTimer::GetClockTicks () - t0;
    R.Format ("done: %u writes in %u us = %u ns/write (CPU-paced direct mode; DMA would be faster)\r\n",
              n, dt, n ? (unsigned) ((unsigned long long) dt * 1000 / n) : 0); out (ctx, (const char *) R);
    out (ctx, "SMI pins ALT1, FPGA held in reset - reboot to restore the bus.\r\n");
}

static void sspi_xfer (const u8 *tx, u8 *rx, unsigned n, unsigned us)
{
    write32 (GP_CLR0, 1u << vmpu68_board ()->ss);
    CTimer::SimpleusDelay (us * 2);
    for (unsigned i = 0; i < n; i++)
    {
        if (s_pXferYield && i && (i & 31) == 0)
            s_pXferYield ();                   // /CS stays low: SPI flash does not mind
        u8 o = tx ? tx[i] : 0, in = 0;
        for (int b = 7; b >= 0; b--)
        {
            if ((o >> b) & 1) write32 (GP_SET0, 1u << vmpu68_board ()->mosi);
            else              write32 (GP_CLR0, 1u << vmpu68_board ()->mosi);
            CTimer::SimpleusDelay (us);
            write32 (GP_SET0, 1u << vmpu68_board ()->sck);
            CTimer::SimpleusDelay (us);
            in = (u8)((in << 1) | ((read32 (GP_LEV0) >> vmpu68_board ()->miso) & 1));
            write32 (GP_CLR0, 1u << vmpu68_board ()->sck);
            CTimer::SimpleusDelay (us);
        }
        if (rx) rx[i] = in;
    }
    CTimer::SimpleusDelay (us);
    write32 (GP_SET0, 1u << vmpu68_board ()->ss);
    CTimer::SimpleusDelay (us * 2);
}

static void diag_round (CDevice *pDev, const char *tag, unsigned us)
{
    CString R;
    u8 sr[2], id[8], rd[8], sfdp[8];
    { u8 tx[2] = {0x05, 0}; sspi_xfer (tx, sr, 2, us); }
    { u8 tx[8] = {0x9F, 0, 0, 0, 0, 0, 0, 0}; sspi_xfer (tx, id, 8, us); }
    { u8 tx[8] = {0x03, 0, 0, 0, 0, 0, 0, 0}; sspi_xfer (tx, rd, 8, us); }
    { u8 tx[8] = {0xAB, 0, 0, 0, 0, 0, 0, 0}; sspi_xfer (tx, sfdp, 8, us); }
    R.Format ("%s sr=%02X id=%02X %02X %02X %02X %02X %02X %02X"
              " rd0=%02X %02X %02X %02X res=%02X\r\n",
              tag, sr[1], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
              rd[4], rd[5], rd[6], rd[7], sfdp[4]);
    dev_put (pDev, (const char *) R);
}

void CKernel::FpgaDiag (CDevice *pDev)
{
    vmpu68_set_gpio_base ((volatile unsigned int *) ARM_GPIO_BASE);
    if (vmpu68_open () != 0)
    {
        dev_put (pDev, "diag: gpio open failed\r\n");
        return;
    }
    CString R;

    // levels with the FPGA free-running (CRESET_B released)
    u32 lev = read32 (GP_LEV0);
    R.Format ("diag: released: miso=%u creset=%u\r\n",
              (lev >> vmpu68_board ()->miso) & 1, (lev >> vmpu68_board ()->creset) & 1);
    dev_put (pDev, (const char *) R);

    vmpu68_flash_begin ();                     // CRESET_B low, FPGA off
    CTimer::SimpleusDelay (20000);
    lev = read32 (GP_LEV0);
    R.Format ("diag: creset held low: miso_idle=%u creset=%u (ss high)\r\n",
              (lev >> vmpu68_board ()->miso) & 1, (lev >> vmpu68_board ()->creset) & 1);
    dev_put (pDev, (const char *) R);

    diag_round (pDev, "diag: fast(~1u)", 1);
    diag_round (pDev, "diag: slow(20u)", 20);
    diag_round (pDev, "diag: slow(100u)", 100);

    // wake attempt (Release Power-down 0xAB), then retry the id
    { u8 tx[1] = {0xAB}; sspi_xfer (tx, 0, 1, 20); }
    CTimer::SimpleusDelay (5000);
    diag_round (pDev, "diag: after-wake", 20);

    lev = read32 (GP_LEV0);
    R.Format ("diag: end: miso_idle=%u\r\n", (lev >> vmpu68_board ()->miso) & 1);
    dev_put (pDev, (const char *) R);

    vmpu68_flash_end ();                       // release the FPGA again
    dev_put (pDev, "diag: done (creset released)\r\n");
}

// package install requested from the console (upd) or the web UI
// diagnostic: busy-wait nUs between yields for nSecs, to see how the HTTP
// server copes with a main loop that yields at that rate (as the FPGA
// programming loop does)
void CKernel::YieldTest (CDevice *pDev, unsigned nUs, unsigned nSecs)
{
    unsigned t0 = m_Timer.GetTicks (), n = 0;
    g_Update.busy = 1; g_Update.phase = "yield test"; g_Update.done = 0; g_Update.total = nSecs;
    while (m_Timer.GetTicks () - t0 < nSecs * HZ)
    {
        CTimer::SimpleusDelay (nUs);
        ServiceDuringUpdate ();
        n++;
        g_Update.done = (m_Timer.GetTicks () - t0) / HZ;
    }
    g_Update.busy = 0; g_Update.phase = "idle";
    CString R;
    R.Format ("yt: %u yields in %u s (%u us busy each)\r\n", n, nSecs, nUs);
    dev_put (pDev, (const char *) R);
}

void CKernel::RunUpdate (CDevice *pDev, const char *pPath)
{
    if (Update (pDev, pPath, g_Update.try_mode != 0, g_Update.force != 0))
    {
        m_Scheduler.MsSleep (500);             // let the last /api/status reply out
        pi_reboot (g_Update.try_mode != 0);
    }
}

// Main RAM size: read the last word of every megabyte up to 12 MB ($C00000
// Main RAM size: read the last word of every megabyte up to 12 MB ($C00000
// is GVRAM); the first one that ends in a bus error (VST_FAULT, the X68000
// asserts BERR for unpopulated addresses) marks the end.  Read-only, ~12 bus
// cycles.  0 = nothing readable (machine not up), keep what we have.
static uint32_t probe_main_ram (void)
{
    uint32_t size = 0;
    for (uint32_t mb = 1; mb <= 12; mb++)
    {
        uint16_t v = 0;
        hw_clear_fault ();
        uint16_t st = hw_bus_read (mb * 0x100000 - 2, 1, &v);
        if (st & VST_FAULT)
        {
            hw_clear_fault ();
            break;
        }
        size = mb * 0x100000;
    }
    return size;
}

// Bus clock in MHz, from the FPGA's heartbeat: VDIAG_HB_CLK16 toggles every
// 2^19 bus clocks (32.8 ms at 16 MHz, 52.4 ms at 10 MHz), so two toggles are
// 2^20 clocks.  Takes 70-110 ms; 0 when the clock is not running.
unsigned vmpu68_bus_mhz (void) { return (s_bus_mhz10 + 5) / 10; }
unsigned vmpu68_bus_mhz10 (void) { return s_bus_mhz10; }
const cfg_bus_class *vmpu68_bus_class (void) { return s_bus_cls; }
int vmpu68_bus_setup_auto (void) { return s_bus_setup_auto; }
static const char *probed_str (void)
{
    static char buf[24];
    CString S; S.Format (" (probed: %d)", s_bus_setup_auto);
    strncpy (buf, (const char *) S, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    return buf;
}
static unsigned measure_bus_mhz10 (void)
{
    unsigned h = hw_reg_read (2) & VDIAG_HB_CLK16, t0 = 0, n = 0;
    unsigned start = CTimer::GetClockTicks ();       // 1 MHz
    while (CTimer::GetClockTicks () - start < 250000)
    {
        unsigned v = hw_reg_read (2) & VDIAG_HB_CLK16;
        if (v != h)
        {
            h = v;
            unsigned t = CTimer::GetClockTicks ();
            if (n == 0) t0 = t;
            else if (n == 2) return (unsigned) (((1u << 20) * 10ull + (t - t0) / 2) / (t - t0));
            n++;
        }
        CTimer::SimpleusDelay (100);
    }
    return 0;
}

// Probe the write-data setup the DRAM needs at this clock: with the machine
// just out of reset and the emulator stopped the IPL has not run, so the top
// 32 KB of main RAM can be used (saved and put back).  Writes $0000 then
// $FFFF, 1 us apart (a floated bus, the worst case seen at 16 MHz), for
// wr_setup 0..3 and takes the first with no lost word.  16384 words resolve
// the ~1e-4 loss rate one tick below the edge (16 MHz: 0 and 1 fail, 2 is
// clean over 100K+ words of testing).  ~300 ms.
static int probe_wr_setup (void)
{
    const unsigned n = 16384;
    static uint16_t save[16384], buf[16384];
    uint32_t base = emu68k_ram_size () - 2 * n;
    hw_bus_read_block (base, n, save);
    int chosen = -1;
    for (int st = 0; st < 4 && chosen < 0; st++)
    {
        hw_set_wr_setup (st);
        for (unsigned i = 0; i < n; i++) { hw_bus_write (base + 2 * i, 1, 0x0000); CTimer::SimpleusDelay (1); }
        hw_bus_write_sync (base + 2 * (n - 1), 1, 0x0000);
        for (unsigned i = 0; i < n; i++) { hw_bus_write (base + 2 * i, 1, 0xFFFF); CTimer::SimpleusDelay (1); }
        hw_bus_write_sync (base + 2 * (n - 1), 1, 0xFFFF);
        hw_bus_read_block (base, n, buf);
        unsigned bad = 0;
        for (unsigned i = 0; i < n; i++) if (buf[i] != 0xFFFF) bad++;
        CLogger::Get ()->Write (FromKernel, LogNotice, "wr_setup probe: +%d tick(s): %u/%u words lost", st, bad, n);
        if (!bad) chosen = st;
    }
    if (chosen < 0)
    {
        CLogger::Get ()->Write (FromKernel, LogWarning, "wr_setup probe: main RAM writes lose words even at +3 ticks - keeping 3");
        chosen = 3;
    }
    hw_set_wr_setup (3);
    for (unsigned i = 0; i < n; i++) hw_bus_write (base + 2 * i, 1, save[i]);
    // margin: the probe writes main RAM only; TVRAM byte writes at 16 MHz were seen to
    // lose bits one tick above the first loss-free setting (dots on the text screen),
    // so keep one extra tick when there is room
    if (chosen >= 0 && chosen < 3)
    {
        CLogger::Get ()->Write (FromKernel, LogNotice, "wr_setup probe: using +%d tick(s) (+1 margin)", chosen + 1);
        chosen++;
    }
    hw_bus_write_sync (base + 2 * (n - 1), 1, save[n - 1]);
    hw_set_wr_setup (chosen);
    return chosen;
}

// (Re)start the emulated 68000 after an external reset or a power-on.
//
// The emulator does not stop when the machine is switched off: the bus clock
// runs on for seconds while the supplies decay, then the FPGA (kept alive by
// the Pi's 5V through the ideal diode) freezes with the clock.  Everything
// the emulator fetched from the bus in that time is garbage - and the ROM
// read cache keeps it across a CPU reset, so the next IPL run executed junk
// (exception dialog on a striped screen).  The RAM shadow also no longer
// matches the DRAM, which lost its content.  So: wait for the machine's own
// reset to end, drop the ROM cache, and if the DRAM does not match the
// shadow (power cycle) re-read it like a cold 68000 would see it.
void CKernel::BootX68 (const char *pWhy, unsigned nStatus, boolean bPowerOn)
{
    emu_quiesce ();
    if (bPowerOn)
        hw_reinit ();                          // register writes were lost while frozen
    unsigned waited = 0;
    while ((hw_status () & VST_RESET_IN) && waited < 5000)
    {
        m_Timer.MsDelay (10);
        waited += 10;
    }
    hw_rst_seen (1);                           // release PI_IRQ (rst_seen) before the bus work below
    // what a real 68000 does first after reset: fetch SSP/PC from 0 (the
    // X68000 mirrors the IPL ROM there) and then from the ROM proper - the
    // mirror ends on that.  Logged to see how the machine behaves.
    uint16_t m0 = 0, m1 = 0, m2 = 0, m3 = 0;
    hw_bus_read (0x000000, 1, &m0);
    hw_bus_read (0x000004, 1, &m1);
    hw_bus_read (0xFF0010, 1, &m2);
    hw_bus_read (0x000000, 1, &m3);
    hw_clear_fault ();
    m_Timer.MsDelay (bPowerOn ? 200 : 20);     // settle: supplies at power-on, peripherals after a button reset
    // main RAM size: vmpu68.cfg "ram=" or the bus probe; a change remaps the
    // shadow (the resync below then reads the whole of it)
    uint32_t ram = cfg_ram_mb () ? cfg_ram_mb () << 20 : probe_main_ram ();
    boolean bRamChanged = FALSE;
    if (ram && ram != emu68k_ram_size ())
    {
        emu68k_set_ram_size (ram);
        bRamChanged = TRUE;
    }
    CLogger::Get ()->Write (FromKernel, LogNotice, "main RAM %u MB (%s)%s", emu68k_ram_size () >> 20,
                    cfg_ram_mb () ? "vmpu68.cfg" : ram ? "probed" : "probe found nothing, kept",
                    bRamChanged ? " - shadow remapped" : "");
    // Bus clock: the XVI's 10/16 MHz switch (or an overclock).  The timing
    // class for the measured clock comes from SD:/vmpu68-bus.json (or the
    // built-in 10/16 MHz defaults): I/O spacing, the first-poll wait, ai
    // writes and the write-data setup - at 16 MHz main RAM lost about 1 in
    // 2000 written words until 2 ticks of setup were added (docs §27.13);
    // "auto" probes it here on the free bus.
    s_bus_mhz10 = measure_bus_mhz10 ();
    s_bus_cls = cfg_bus_select (s_bus_mhz10);
    const cfg_bus_class *c = s_bus_cls;
    hw_set_wr_ai (c->ai);
    vmpu68_set_wait_ns (c->wait_ns);
    emu68k_set_io_mhz (c->io_mhz, 0);
    s_bus_setup_auto = -1;
    int setup = c->wr_setup >= 0 ? hw_set_wr_setup (c->wr_setup) : (s_bus_setup_auto = probe_wr_setup ());
    CLogger::Get ()->Write (FromKernel, LogNotice, "bus clock %u.%u MHz: class >= %u.%u MHz (%s) - wr_setup +%d%s, wait %u ns, io %u MHz, ai %s",
                    s_bus_mhz10 / 10, s_bus_mhz10 % 10, c->min_mhz10 / 10, c->min_mhz10 % 10, cfg_bus_source (),
                    setup, c->wr_setup < 0 ? " (probed)" : "", c->wait_ns, c->io_mhz, c->ai ? "on" : "off");
    emu68k_snoop_apply ();
    unsigned rambad = emu68k_wb_enabled () ? 0 : emu68k_shadow_check ();   // write-back: the real RAM lags the shadow by design
    uint32_t first = ~0u;
    unsigned rombad = 0;
    unsigned t0 = m_Timer.GetTicks ();
    int sync = 0;
    if (rambad || bPowerOn || bRamChanged)
    {
        rombad = emu68k_rom_check (&first);    // diagnostics: how many pages were poisoned
        sync = emu68k_sync_shadow (0, emu68k_ram_size ());
    }
    emu68k_rom_invalidate ();
    CLogger::Get ()->Write (FromKernel, LogNotice, "%s: booting (st=%04x, reset_in released after %u ms%s, [0]=%04x [4]=%04x rom=%04x [0]'=%04x, ram check bad=%u%s, rom cache bad=%u first=%06X, %u ms)",
                    pWhy, nStatus, waited, waited >= 5000 ? " - TIMEOUT" : "", m0, m1, m2, m3, rambad,
                    (rambad || bPowerOn || bRamChanged) ? (sync ? " - resync FAILED" : " - resynced") : "",
                    rombad, first, (m_Timer.GetTicks () - t0) * 1000 / HZ);
    emu68k_reset ();
    if (cfg_sramboot ())                       // boot screen program: (re)install when missing or outdated
    {
        int st = vmpu68_sramboot_state ();
        if (st == 0 || st == 3) { int rc = sramboot_install (1); CLogger::Get ()->Write (FromKernel, LogNotice, "sramboot: %s (state %d)%s", rc ? "install failed" : "installed", st, rc ? " - SRAM WRITE FAILED" : ""); }
        else if (st == 2) CLogger::Get ()->Write (FromKernel, LogWarning, "sramboot: another SRAM program is installed - leaving it");
    }
    hw_rst_seen (1);                           // the reset that got us here (or the machine's power-on reset) is handled
    s_sup.bBtnReset = FALSE;
    s_emu_run = TRUE;
    DataSyncBarrier ();
}

TShutdownMode CKernel::Run (void)
{
    CLogger::Get ()->Write (FromKernel, LogNotice,
                    "vmpu68 %s (circle %s) name \"%s\"", VMPU68_VERSION, CIRCLE_VERSION_STRING, cfg_name ());

    // the firmware leaves the ARM at its idle clock (600MHz on a Pi 4):
    // run the emulator core at full speed; the main loop below throttles
    // at 80 C.  The bus protocol pacing is GPIO-round-trip based and does
    // not depend on the CPU clock.
    m_CPUThrottle.SetSpeed (CPUSpeedMaximum);
    CLogger::Get ()->Write (FromKernel, LogNotice, "cpu: %u MHz, %u C",
                    m_CPUThrottle.GetClockRate () / 1000000, m_CPUThrottle.GetTemperature ());

    // First boot of a freshly written SD card (release image): the package
    // SD:/vmpu68.vpk is there but SD:/fpga.sum is not, so this card has not
    // programmed the board's FPGA yet (a new board, or a rebuild after a
    // brick).  Read the configuration flash and compare it with the package:
    // identical (the usual rebuild case) -> just record the version in
    // fpga.sum; different or unreadable -> program it from the package (LED
    // blinks red, ~1 min) and reboot.  A failure leaves SD:/provision.failed
    // so the next boot does not loop (delete it, or use "upd", to retry).
    // Before hw_init(): the flash read holds the FPGA in reset.
    {
        FILINFO fi;
        if (f_stat (PROVISION_FILE, &fi) == FR_OK && f_stat (FPGA_SUM_FILE, &fi) != FR_OK && f_stat (PROVISION_FAILED, &fi) != FR_OK)
        {
            CLogger::Get ()->Write (FromKernel, LogNotice, "provision: fresh card (%s, no %s): checking the FPGA against the package", PROVISION_FILE, FPGA_SUM_FILE);
            int cmp = -1;
            vpk_hdr_t h;
            if (vpk_hdr_read (PROVISION_FILE, &h) && h.fpga_len)
            {
                u8 *img = (u8 *) malloc (h.fpga_len);
                FIL f; UINT br = 0;
                if (img && f_open (&f, PROVISION_FILE, FA_READ) == FR_OK)
                {
                    if (f_lseek (&f, h.fpga_off) == FR_OK && f_read (&f, img, h.fpga_len, &br) == FR_OK && br == h.fpga_len)
                        cmp = FpgaFlashCompare (m_pSerial, img, h.fpga_len);
                    f_close (&f);
                }
                free (img);
            }
            if (cmp == 0)
            {
                s_fpga_ver_flags = h.flags;
                fpga_sum_write (h.fpga_sum, h.fpga_len);
                CLogger::Get ()->Write (FromKernel, LogNotice, "provision: the FPGA already holds this bitstream (%u.%u.%u) - recorded, nothing to program",
                                h.flags >> 16, (h.flags >> 8) & 255, h.flags & 255);
                m_Scheduler.MsSleep (300);     // the FPGA reconfigures after the flash read
            }
            else
            {
                CLogger::Get ()->Write (FromKernel, LogNotice, "provision: %s - programming the FPGA from the package", cmp == 1 ? "bitstream differs" : "flash not readable");
                g_Update.try_mode = 0; g_Update.force = 0;
                if (Update (m_pSerial, PROVISION_FILE, FALSE, FALSE))
                {
                    CLogger::Get ()->Write (FromKernel, LogNotice, "provision: done - rebooting");
                    m_Scheduler.MsSleep (300);
                    pi_reboot (FALSE);
                }
                FIL f;
                if (f_open (&f, PROVISION_FAILED, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) { UINT bw; f_write (&f, g_Update.msg, strlen (g_Update.msg), &bw); f_close (&f); }
                CLogger::Get ()->Write (FromKernel, LogError, "provision: FAILED (%s) - marker %s written, not retrying", g_Update.msg, PROVISION_FAILED);
            }
        }
    }

    vmpu68_set_gpio_base ((volatile unsigned int *) ARM_GPIO_BASE);
    int mock = hw_init ();
    CLogger::Get ()->Write (FromKernel, LogNotice, "hw: %s backend",
                    mock ? "MOCK (no FPGA answer)" : "FPGA");
    vmpu68_set_snoop_hook_core (1);            // the snoop hook writes the shadow: emulator core only
    hw_wq_enable (0);                          // core-2 write queue off by default since 0.1.18 (docs 31: latency for raster/timer IRQ handlers); 'wq 1' turns it on for experiments
#ifdef VMPU68_WITH_EMU
    emu_ensure ();                             // allocate the shadow on core 0
    if (!m_EmuCore.Initialize ())
        CLogger::Get ()->Write (FromKernel, LogError, "multicore init failed");
#endif

    CUSBSerialDevice *pCDC = nullptr;
    char lineU[160], lineC[160];
    unsigned nU = 0, nC = 0;
    unsigned nLastBlink = 0;
    unsigned nMockProbe = 0;
    unsigned &nLastSup = s_sup.nLastSup, &nDeadSince = s_sup.nDeadSince,
             &nAliveSince = s_sup.nAliveSince, &nDeadMs = s_sup.nDeadMs, &nLastSig = s_sup.nLastSig, &nSupRuns = s_sup.nRuns;
    boolean  &bEverDead = s_sup.bEverDead, &bWasAlive = s_sup.bWasAlive, &bBounceLogged = s_sup.bBounceLogged, &bRstSeen = s_sup.bRstSeen;
    unsigned nLastThrottle = 0;
    boolean  bCPUThrottled = FALSE;

#ifdef VMPU68_WATCHDOG
    m_Watchdog.Start (15);                     // hang -> auto reset (tryboot
                                               // falls back to stable kernel)
#endif
    for (;;)
    {
#ifdef VMPU68_WATCHDOG
        m_Watchdog.Start (15);                 // reload
#endif
        m_Scheduler.Yield ();                  // run network tasks

        // Wi-Fi after the machine is served: once the emulator runs (or the
        // machine is off / no FPGA, after a short grace period)
        if (!m_bWLANStarted && (s_emu_run || m_Timer.GetUptime () >= 6))
            StartWLAN ();

        if (m_Timer.GetUptime () - nLastThrottle >= 3)   // thermal management
        {
            // Not CCPUThrottle::Update(): its limit is the socmaxtemp option,
            // 60 C by default, with 3 C of hysteresis - the SoC idles at
            // 58-60 C in the case, so the ARM clock flipped between 1500 and
            // 600 MHz every minute or so (writes 500 -> 870 ns/op, emulated
            // clock 28 -> 12 MHz).  The firmware throttles at 80 C anyway.
            nLastThrottle = m_Timer.GetUptime ();
            unsigned nTemp = m_CPUThrottle.GetTemperature ();
            if (nTemp >= 80 && !bCPUThrottled)
            {
                bCPUThrottled = TRUE;
                m_CPUThrottle.SetSpeed (CPUSpeedLow);
                CLogger::Get ()->Write (FromKernel, LogWarning, "cpu: %u C - throttled to %u MHz",
                                nTemp, m_CPUThrottle.GetClockRate () / 1000000);
            }
            else if (nTemp <= 75 && bCPUThrottled)
            {
                bCPUThrottled = FALSE;
                m_CPUThrottle.SetSpeed (CPUSpeedMaximum);
                CLogger::Get ()->Write (FromKernel, LogNotice, "cpu: %u C - back to %u MHz",
                                nTemp, m_CPUThrottle.GetClockRate () / 1000000);
            }
        }

        info_update ();                        // $ECFF00 information port (once a second)
        {
            unsigned off, val;                 // VMPU68.X wrote a setting into the information port
            while (emu68k_info_take_cmd (&off, &val))   // VMPU68.X may queue several settings at once
            {
                if (off == 0xF0)      vmpu68_apply_settings ((int) (val <= 1000 ? val : 0), -1, -1, TRUE);
                else if (off == 0xF2) vmpu68_apply_settings (-1, val ? 1 : 0, -1, TRUE);
                else if (off == 0xF4) vmpu68_apply_settings (-1, -1, val ? 1 : 0, TRUE);
                else if (off == 0xF6) vmpu68_sramboot_set (val ? 1 : 0, TRUE);
                else if (off == 0xF8) { cfg_set_ram (val); cfg_save (); CLogger::Get ()->Write (FromKernel, LogNotice, "cfg: ram=%u (VMPU68.X, applies at the next boot)", cfg_ram_mb ()); }
                else if (off == 0xFA) { cfg_set_name (emu68k_info_name ()); cfg_save (); CLogger::Get ()->Write (FromKernel, LogNotice, "cfg: name=\"%s\" (VMPU68.X)", cfg_name ()); }
            }
        }
        s_led_booted = TRUE;
        statusled_task (m_bNetOK, &m_Net);     // LED1: the vfd68/vhd68 status ladder

        // A/B: the try kernel has run this long without hanging or crashing
        // (the watchdog would have reset it) - promote it to the stable slot
        if (s_ab_arm_promote && m_Timer.GetUptime () >= 20)
        {
            s_ab_arm_promote = 0;
            ab_promote ();
        }

        // deferred actions requested over HTTP (response already sent)
        if (g_WebAction)
        {
            int act = g_WebAction;
            g_WebAction = 0;
            m_Scheduler.MsSleep (500);         // let the response drain: the net
                                               // task only transmits when we sleep
            if (act == 2)
                pi_reboot (TRUE);
            else if (act == 3)
                for (;;) ;                     // watchdog test
            else if (act == 5)
                RunUpdate (m_pSerial, UPDATE_FILE);
            else
                pi_reboot (FALSE);
        }

        boolean bPnP = m_CDCGadget.UpdatePlugAndPlay ();
        if (bPnP || pCDC == nullptr)
            pCDC = (CUSBSerialDevice *) m_DeviceNameService.GetDevice ("utty1", FALSE);

        // start the web server once the network is up
        if (m_bNetOK && !m_bWebStarted && m_Net.IsRunning ())
        {
            CString IP;
            m_Net.GetConfig ()->GetIPAddress ()->Format (&IP);
            CLogger::Get ()->Write (FromKernel, LogNotice, "WLAN up: http://%s/",
                            (const char *) IP);
            new CVmpuWebServer (&m_Net);
            m_bWebStarted = TRUE;
            g_pDiscovery = new CDiscovery (&m_Net);   // LAN beacon (vhd68/vfd68 style)
            if (!g_pDiscovery->Initialize ())
            {
                delete g_pDiscovery;
                g_pDiscovery = nullptr;
                CLogger::Get ()->Write (FromKernel, LogWarning, "discovery: UDP bind failed");
            }
        }
        if (g_pDiscovery)
            g_pDiscovery->Poll ();

        // UART console
        char buf[64];
        int n = m_pSerial->Read (buf, sizeof buf);
        for (int i = 0; i < n; i++)
        {
            char c = buf[i];
            if (c == '\r' || c == '\n')
            {
                lineU[nU] = 0; nU = 0;
                int act = exec_cmd (lineU, dev_put, m_pSerial, m_bNetOK, &m_Net);
                if (act == 2) FileReceive (m_pSerial, s_xfer_path, s_xfer_len);
                if (act == 3) FileReceive (m_pSerial, nullptr, s_xfer_len);
                if (act == 6) FpgaFlash (m_pSerial, s_xfer_path);
                if (act == 7) FpgaDiag (m_pSerial);
                if (act == 8) RunUpdate (m_pSerial, s_xfer_path);
                if (act == 9) YieldTest (m_pSerial, s_xfer_len, s_xfer_addr);
                if (act == 4) { m_Timer.MsDelay (100); pi_reboot (TRUE); }
                if (act == 5) { m_Timer.MsDelay (100); for (;;) ; }
                if (act == 1) { m_Timer.MsDelay (100); pi_reboot (FALSE); }
            }
            else if (nU < sizeof lineU - 1)
                lineU[nU++] = c;
        }

        // CDC console
        if (pCDC != nullptr)
        {
            n = pCDC->Read (buf, sizeof buf);
            for (int i = 0; i < n; i++)
            {
                char c = buf[i];
                if (c == '\r' || c == '\n')
                {
                    lineC[nC] = 0; nC = 0;
                    int act = exec_cmd (lineC, dev_put, pCDC, m_bNetOK, &m_Net);
                    if (act == 2) FileReceive (pCDC, s_xfer_path, s_xfer_len);
                    if (act == 3) FileReceive (pCDC, nullptr, s_xfer_len);
                    if (act == 6) FpgaFlash (pCDC, s_xfer_path);
                    if (act == 7) FpgaDiag (pCDC);
                    if (act == 8) RunUpdate (pCDC, s_xfer_path);
                    if (act == 9) YieldTest (pCDC, s_xfer_len, s_xfer_addr);
                    if (act == 4) { m_Timer.MsDelay (100); pi_reboot (TRUE); }
                    if (act == 5) { m_Timer.MsDelay (100); for (;;) ; }
                    if (act == 1) { m_Timer.MsDelay (100); pi_reboot (FALSE); }
                }
                else if (nC < sizeof lineC - 1)
                    lineC[nC++] = c;
            }
        }

#ifdef VMPU68_WITH_EMU
        // X68000 supervision (5Hz): PLL lock = machine on -> reset + run;
        // lock lost = machine off -> stop; external RESET (front button,
        // not our own driver) -> reset + run
        unsigned nTicks = m_Timer.GetTicks ();
        boolean bSupTick = FALSE;
        if (nTicks - nLastSup >= 20)
        {
            nLastSup = nTicks;
            bSupTick = TRUE;
            if (hw_is_mock () && !s_gpclk_on)   // (not while gpclk drives AD2: the probe rewrites GPFSEL0)
            {
                // the Pi booted while the X68000 (which powers the FPGA) was off:
                // keep probing instead of staying in mock mode for good
                if (++nMockProbe >= 5)
                {
                    nMockProbe = 0;
                    vmpu68_set_gpio_base ((volatile unsigned int *) ARM_GPIO_BASE);
                    if (hw_init () == 0)
                    {
                        CLogger::Get ()->Write (FromKernel, LogNotice, "hw: FPGA detected late - leaving mock mode");
                        hw_wq_enable (1);
                    }
                }
            }
            else if (hw_alive () < 0)
            {
                // the FPGA was reloaded behind our back (X68000 power cycle):
                // its registers are at power-up values, the emulator's state is
                // meaningless.  Re-program, and let the supervision below boot
                // again as if the machine had just been switched on.
                CLogger::Get ()->Write (FromKernel, LogNotice, "hw: FPGA reconfigured (X68000 power cycle?) - re-initialising");
                emu_quiesce ();
                hw_reinit ();
                s_x68_on = FALSE;
                bEverDead = FALSE; bWasAlive = FALSE; // a reconfiguration is a real power cycle
            }
        }
        // the reset button: the machine's reset circuit asserts RESET_IN and
        // HALT_IN for only ~25 us (measured on an XVI) - enough for a real
        // 68000, invisible to any polling.  The FPGA latches it
        // (VDIAG_RST_SEEN); poll the latch once per 10 ms tick.  Without the
        // latch (old bitstream) the level itself is sampled, which only
        // catches long resets (power-on).
        if (s_auto && !hw_is_mock () && s_x68_on && nTicks != s_sup.nLastRstTick)
        {
            s_sup.nLastRstTick = nTicks;
            int seen = hw_rst_seen (1);
            if (seen > 0 || emu68k_ext_reset ()
                || (seen < 0 && (hw_status () & VST_RESET_IN) && !(hw_drv_state () & VSTW_DRV_RESET)))
                s_sup.bBtnReset = TRUE;
            // handle it here rather than on the 200 ms tick when the clock is
            // plainly running (x68_clock_alive returns in 2 ms then): the
            // emulator has stopped itself and every 100 ms is visible as
            // boot delay.  A stopped clock is left to the block below.
            if (s_sup.bBtnReset && (hw_reg_read (2) & VDIAG_PLL_LOCK) && x68_clock_alive ())
            {
                unsigned st = hw_status ();
                s_sup.bBtnReset = FALSE;
                BootX68 ("X68000 reset", st, FALSE);
            }
        }
        // once per 200 ms (the block above just advanced nLastSup).  Note the
        // main loop iterates many times per 10 ms tick: anything counted per
        // iteration here would not be time.
        if (s_auto && !hw_is_mock () && bSupTick)
        {
            boolean bLock = (hw_reg_read (2) & VDIAG_PLL_LOCK) && x68_clock_alive ();
            unsigned st = hw_status ();
            boolean bExtReset = s_sup.bBtnReset || ((st & VST_RESET_IN) && !(hw_drv_state () & VSTW_DRV_RESET));
            s_sup.bBtnReset = FALSE;
            unsigned nNowMs = nTicks * (1000 / HZ);
            nSupRuns++; s_sup.nLastLock = bLock; s_sup.nLastSt = st;
            unsigned nPwrOff = s_emu_run ? (unsigned) emu68k_poweroff_pending () : 0;
            if (nPwrOff != s_sup.nPwrOff)
            {
                // the IPL asked the machine to power off (see io_write in
                // emu68k.c): the core holds for the delay loop's real duration
                // and then reboots like a real 68000 whose supply stayed on.
                // The "off" path below follows when the clock stops.
                if (nPwrOff) CLogger::Get ()->Write (FromKernel, LogNotice, "X68000 power-off requested ($E8E00F) #%u: holding 1.8 s", nPwrOff);
                s_sup.nPwrOff = nPwrOff;
            }
            // signal timeline (the power-down/up sequence is what this whole
            // block is about)
            unsigned nSig = (bLock ? 4 : 0) | ((st & VST_RESET_IN) ? 2 : 0) | ((st & VST_HALT_IN) ? 1 : 0);
            if (nSig != nLastSig)
                CLogger::Get ()->Write (FromKernel, LogNotice, "X68000 clock=%u reset_in=%u halt_in=%u", nSig >> 2, (nSig >> 1) & 1, nSig & 1);
            nLastSig = nSig;
            if (bLock && !s_x68_on)
            {
                // power-on: the bus clock can come and go while the machine's
                // supplies ramp, and its own power-on reset outlasts our
                // 100 ms RESET drive.  Want the clock stable for 1 s, then
                // RESET_IN released, then a settle time (with the FPGA frozen
                // while the machine was off, register writes made in that
                // state were lost: hw_reinit re-programs everything).
                //
                // The clock also BOUNCES while the machine powers down: it
                // stops ~4 s after a (soft or hard) power-off, comes back
                // about a second later for a few seconds and then dies for
                // good - with RESET_IN asserted by the machine's own supply
                // monitor, so a reset is no evidence of a power-on.  Booting
                // on that bounce runs the IPL on a dying machine (measured:
                // FDD seeks, the IPL re-issued the power-off sequence, and
                // one real boot after that died with a stray exception).  A
                // clock that returns after a short gap is therefore only
                // trusted once it has stayed up far longer than any bounce.
                if (st & VST_RESET_IN) bRstSeen = TRUE;
                if (!bWasAlive) { nAliveSince = nNowMs; bWasAlive = TRUE; nDeadMs = bEverDead ? nNowMs - nDeadSince : ~0u; }
                unsigned nUpMs = nNowMs - nAliveSince;
                if (nUpMs >= 1000 && (nDeadMs >= 3000 || nUpMs >= 10000))
                {
                    CString Why;
                    if (nDeadMs == ~0u) Why.Format ("X68000 on (up %u ms%s)", nUpMs, bRstSeen ? ", reset seen" : "");
                    else                Why.Format ("X68000 on (clock dead %u ms, up %u ms%s)", nDeadMs, nUpMs, bRstSeen ? ", reset seen" : "");
                    bRstSeen = FALSE; bEverDead = FALSE;
                    s_x68_on = TRUE;
                    BootX68 (Why, st, TRUE);
                }
                else if (nUpMs >= 1000 && !bBounceLogged)
                {
                    bBounceLogged = TRUE;
                    CLogger::Get ()->Write (FromKernel, LogNotice, "X68000 clock back after only %u ms%s - waiting (power-off bounce?)", nDeadMs, bRstSeen ? ", reset seen" : "");
                }
            }
            else if (!bLock)
            {
                if (bWasAlive || !bEverDead) { nDeadSince = nNowMs; bEverDead = TRUE; }
                bWasAlive = FALSE; bBounceLogged = FALSE;
                if (s_x68_on)
                {
                    s_x68_on = FALSE;
                    bRstSeen = FALSE;
                    CLogger::Get ()->Write (FromKernel, LogNotice, "X68000 off: emulator stopped");
                    emu_quiesce ();
                }
            }
            else if (bLock && bExtReset)
            {
                // the reset button - or the power-on reset of a machine whose
                // clock never looked stopped (it runs on for ~8 s after the
                // switch is turned off, so a short off period is only seen
                // here): BootX68 tells the two apart by the RAM contents
                BootX68 ("X68000 reset", st, FALSE);
            }
        }
#endif

        // alive blink (1s)
        unsigned nNow = m_Timer.GetUptime ();
        if (nNow != nLastBlink)
        {
            nLastBlink = nNow;
            m_ActLED.On ();
            m_Timer.MsDelay (30);
            m_ActLED.Off ();
        }
    }

    return ShutdownHalt;
}
