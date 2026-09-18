/* buscheck — first-light bring-up test for the vmpu68 board.
 *
 * Run on the Pi with the board in the X68000 (or on the bench with a bus
 * responder).  Exercises: signature, RAM read/write, IPL, snoop drain.
 */
#include <stdio.h>
#include <stdlib.h>
#include "vmpu68.h"

int main(void)
{
    uint16_t v, st;
    if (vmpu68_open()) return 1;
    vmpu68_reg_write(VREG_STATUS, VSTW_AUTOSTART);   /* REG1 write starts cycles */

    v = vmpu68_reg_read(VREG_CTRL);
    printf("signature: %04x (%s)\n", v, (v >> 8) == 0x56 ? "OK" : "BAD");
    if ((v >> 8) != 0x56) return 1;

    st = vmpu68_reg_read(VREG_STATUS);
    printf("status: %04x  ipl=%u reset_in=%d halt_in=%d\n",
           st, VST_IPL(st), !!(st & VST_RESET_IN), !!(st & VST_HALT_IN));

    /* pattern test against work RAM at 0x001000 */
    for (int i = 0; i < 8; i++)
        vmpu68_bus_write(0x001000 + 2 * i, 1, (uint16_t)(0xA500 + i));
    int bad = 0;
    for (int i = 0; i < 8; i++) {
        st = vmpu68_bus_read(0x001000 + 2 * i, 1, &v);
        if (st & VST_FAULT) { printf("fault @%d st=%04x\n", i, st); bad++; }
        else if (v != 0xA500 + i) { printf("mismatch @%d: %04x\n", i, v); bad++; }
    }
    printf("ram test: %s\n", bad ? "FAIL" : "OK");

    vmpu68_snoop_t s;
    int n = 0;
    while (vmpu68_snoop_pop(&s) && n < 100) {
        printf("snoop: %06x = %04x (uds=%d lds=%d)\n", s.addr, s.data, s.uds, s.lds);
        n++;
    }
    printf("snoop records drained: %d\n", n);

    vmpu68_close();
    return bad ? 1 : 0;
}
