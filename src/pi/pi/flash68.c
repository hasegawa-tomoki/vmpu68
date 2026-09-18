/* flash68 — program the vmpu68 W25Q32 configuration flash from the Pi.
 *
 * Usage:
 *   flash68 id                    read JEDEC ID
 *   flash68 write <bitstream>     erase + program + verify, then boot FPGA
 *   flash68 read  <file> <len>    dump flash
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vmpu68.h"

static void cmd(uint8_t c) { vmpu68_flash_xfer(&c, NULL, 1, 0); }

static uint8_t status1(void)
{
    uint8_t tx[2] = {0x05, 0}, rx[2];
    vmpu68_flash_xfer(tx, rx, 2, 0);
    return rx[1];
}

static void wait_wip(void) { while (status1() & 1) ; }

static void addr_cmd(uint8_t op, uint32_t a, const uint8_t *tx, uint8_t *rx,
                     unsigned n)
{
    uint8_t hdr[4] = {op, (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a};
    vmpu68_flash_xfer(hdr, NULL, 4, 1);
    if (n) vmpu68_flash_xfer(tx, rx, n, 1);
    vmpu68_flash_xfer(NULL, NULL, 0, 0);   /* raise SS */
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s id|write|read ...\n", argv[0]); return 1; }
    if (vmpu68_open()) return 1;
    vmpu68_flash_begin();
    {   /* release deep power-down: the iCE40 parks the flash with 0xB9 */
        uint8_t w = 0xAB;
        vmpu68_flash_xfer(&w, NULL, 1, 0);
        usleep(10);                        /* tRES1 >= 3us */
    }

    if (!strcmp(argv[1], "id")) {
        uint8_t tx[4] = {0x9F, 0, 0, 0}, rx[4];
        vmpu68_flash_xfer(tx, rx, 4, 0);
        printf("JEDEC ID: %02x %02x %02x\n", rx[1], rx[2], rx[3]);
    } else if (!strcmp(argv[1], "write") && argc == 3) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) { perror(argv[2]); return 1; }
        static uint8_t buf[4 << 20];
        size_t len = fread(buf, 1, sizeof buf, f);
        fclose(f);
        printf("bitstream: %zu bytes\n", len);

        for (uint32_t a = 0; a < len; a += 65536) {
            cmd(0x06);                          /* WREN */
            addr_cmd(0xD8, a, NULL, NULL, 0);   /* 64K block erase */
            wait_wip();
            printf("erase %06x\r", a); fflush(stdout);
        }
        for (uint32_t a = 0; a < len; a += 256) {
            unsigned n = len - a > 256 ? 256 : (unsigned)(len - a);
            cmd(0x06);
            addr_cmd(0x02, a, buf + a, NULL, n);
            wait_wip();
            if (!(a & 0xFFF)) { printf("prog  %06x\r", a); fflush(stdout); }
        }
        static uint8_t vbuf[4 << 20];
        addr_cmd(0x03, 0, NULL, vbuf, (unsigned)len);
        if (memcmp(buf, vbuf, len)) { printf("\nVERIFY FAILED\n"); return 1; }
        printf("\nverify OK\n");
    } else if (!strcmp(argv[1], "read") && argc == 4) {
        unsigned len = (unsigned)strtoul(argv[3], NULL, 0);
        static uint8_t buf[4 << 20];
        if (len > sizeof buf) len = sizeof buf;
        addr_cmd(0x03, 0, NULL, buf, len);
        FILE *f = fopen(argv[2], "wb");
        fwrite(buf, 1, len, f);
        fclose(f);
        printf("read %u bytes -> %s\n", len, argv[2]);
    } else {
        fprintf(stderr, "bad args\n"); return 1;
    }

    vmpu68_flash_end();
    vmpu68_close();
    return 0;
}
