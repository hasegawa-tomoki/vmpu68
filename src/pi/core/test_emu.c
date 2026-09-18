/* SPDX-License-Identifier: MIT
 *
 * test_emu — run a hand-assembled 68000 program on Musashi through the
 * vmpu68 glue against the mock hardware, exercising the same code path
 * the real board will use (shadow RAM + write-through bus cycles).
 */
#include <stdio.h>
#include <string.h>
#include "emu68k.h"
#include "../mgmtd/hw.h"

static int errors;

static void check(const char *name, unsigned got, unsigned exp)
{
    if (got != exp) {
        printf("FAIL %s: got %04x expected %04x\n", name, got, exp);
        errors++;
    } else
        printf("pass %s: %04x\n", name, got);
}

int main(void)
{
    if (!hw_init()) {
        fprintf(stderr, "expected mock hw\n");
        return 1;
    }
    emu68k_init(0x10000, 1);

    /* vectors: SSP = 0x2000, PC = 0x400 */
    static const uint8_t vecs[] = {0x00,0x00,0x20,0x00, 0x00,0x00,0x04,0x00};
    emu68k_load(0, vecs, sizeof vecs);

    /* moveq #0,d0; lea $1000.w,a0
     * loop: move.w d0,(a0)+; addq.w #1,d0; cmpi.w #$10,d0; bne.s loop
     * bra.s * */
    static const uint8_t prog[] = {
        0x70,0x00,
        0x41,0xF8,0x10,0x00,
        0x30,0xC0,
        0x52,0x40,
        0x0C,0x40,0x00,0x10,
        0x66,0xF6,
        0x60,0xFE
    };
    emu68k_load(0x400, prog, sizeof prog);

    emu68k_reset();
    emu68k_run(2000);

    /* verify through the BUS side (write-through must have landed) */
    uint16_t v;
    int ok = 1;
    for (int i = 0; i < 16; i++) {
        hw_bus_read(0x1000 + 2u * i, 1, &v);
        if (v != i) { ok = 0; printf("FAIL ram[%d]=%04x\n", i, v); errors++; }
    }
    if (ok) printf("pass 68k program output x16\n");
    check("final pc (spin)", emu68k_pc() & 0xFFFF, 0x0410);

    /* DMA snoop -> shadow coherence */
    hw_mock_dma(0x3000, 0xABCD, 3);
    check("snoop applied", (unsigned)emu68k_snoop_apply(), 3);
    /* shadow must now serve the DMA'd value without a bus cycle */
    extern unsigned int m68k_read_memory_16(unsigned int);
    extern void m68k_write_memory_16(unsigned int, unsigned int);
    check("shadow updated", m68k_read_memory_16(0x3002), 0xABCE);

    /* ---- X68000 memory map behaviour ---- */
    unsigned ops0, ops1;

    /* shadow reads must not generate bus cycles */
    ops0 = hw_mock_bus_ops();
    (void)m68k_read_memory_16(0x001234);
    check("ram read is local", hw_mock_bus_ops() - ops0, 0);

    /* I/O region (0xE80xxx): every access is a bus cycle, uncached */
    m68k_write_memory_16(0xE80010, 0x1234);
    ops0 = hw_mock_bus_ops();
    check("io read 1", m68k_read_memory_16(0xE80010), 0x1234);
    check("io read 2", m68k_read_memory_16(0xE80010), 0x1234);
    ops1 = hw_mock_bus_ops();
    check("io reads hit the bus", ops1 - ops0, 2);

    /* ROM region: first access fills the 8KB page, later reads are local */
    ops0 = hw_mock_bus_ops();
    unsigned rom_v = m68k_read_memory_16(0xF00100);
    ops1 = hw_mock_bus_ops();
    check("rom pattern", rom_v, ((0xF00100 >> 1) ^ 0xA5A5) & 0xFFFF);
    check("rom page filled", ops1 - ops0, 4096);
    ops0 = hw_mock_bus_ops();
    check("rom cached", m68k_read_memory_16(0xF00102),
          ((0xF00102 >> 1) ^ 0xA5A5) & 0xFFFF);
    check("rom read is local", hw_mock_bus_ops() - ops0, 0);

    /* ROM writes are forwarded (and ignored) — content unchanged */
    m68k_write_memory_16(0xF00100, 0xDEAD);
    check("rom write ignored", m68k_read_memory_16(0xF00100),
          ((0xF00100 >> 1) ^ 0xA5A5) & 0xFFFF);

    /* explicit shadow sync from the "real" machine */
    hw_bus_write(0x4000, 1, 0x7777);          /* behind the emulator's back */
    check("stale before sync", m68k_read_memory_16(0x4000) == 0x7777 ? 1 : 0, 0);
    emu68k_sync_shadow(0x4000, 0x100);
    check("synced", m68k_read_memory_16(0x4000), 0x7777);

    if (errors == 0) printf("EMU ALL PASS\n");
    return errors ? 1 : 0;
}
