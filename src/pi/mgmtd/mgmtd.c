/* SPDX-License-Identifier: MIT
 *
 * vmpu68 management daemon (mgmtd) — Linux build.
 *
 * One-process HTTP/JSON server providing:
 *   GET  /                       web UI
 *   GET  /api/status             FPGA + board status
 *   GET  /api/bus/read?addr=&word=1
 *   POST /api/bus/write?addr=&word=1&data=
 *   GET  /api/snoop?max=100      drain DMA-write snoop records
 *   GET  /api/fpga/flashid       W25Q32 JEDEC id
 *   POST /api/fpga/flash         body = bitstream -> erase/program/verify/boot
 *   POST /api/ota                body = firmware image -> staged to disk
 *   POST /api/led?v=0..7
 *   POST /api/drv?reset=0|1&halt=0|1
 *   GET  /api/log
 *   POST /api/mock/dma?addr=&data=&n=    (mock mode only, for UI testing)
 *
 * Design note: everything except main()/socket setup avoids Linux-specific
 * APIs so the request router and handlers move to the circle (bare-metal)
 * build unchanged; the transport (TCP here, CDC/UART there) is pluggable.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "hw.h"
#include "../core/emu68k.h"

#include "web_ui.h"    /* generated: web_ui_html[], web_ui_html_len */

#define MAX_BODY (16u << 20)

static char ota_path[512] = "/tmp/vmpu68-ota.bin";

/* ---------------- log ring ---------------- */
static char logbuf[64][160];
static unsigned logn;

static void vlogf(const char *fmt, ...)
{
    va_list ap;
    char line[144];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(logbuf[logn++ & 63], sizeof logbuf[0], "%02d:%02d:%02d %s",
             tm.tm_hour, tm.tm_min, tm.tm_sec, line);
    fprintf(stderr, "%s\n", line);
}

/* ---------------- tiny request/response helpers ---------------- */
typedef struct {
    char method[8];
    char path[256];
    char query[512];
    uint8_t *body;
    size_t body_len;
} req_t;

static long qnum(const req_t *r, const char *key, long def)
{
    char pat[64];
    snprintf(pat, sizeof pat, "%s=", key);
    const char *p = strstr(r->query, pat);
    if (!p) return def;
    return strtol(p + strlen(pat), NULL, 0);
}

static void send_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

static void resp(int fd, int code, const char *ctype, const void *body, size_t n)
{
    char hdr[256];
    int h = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        code, code == 200 ? "OK" : "Error", ctype, n);
    send_all(fd, hdr, (size_t)h);
    send_all(fd, body, n);
}

static void jresp(int fd, int code, const char *fmt, ...)
{
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    resp(fd, code, "application/json", body, (size_t)n);
}

/* ---------------- emulator thread ----------------
 * The emulator always lives inside mgmtd; the control plane can start,
 * stop and reset it at any time — the loop re-checks emu_run between
 * execution slices, so a runaway 68k program is always recoverable
 * from the outside.  (In the circle build this loop owns its own core.)
 */
static volatile int emu_inited, emu_run;
static volatile uint64_t emu_cycles, emu_snoops;

static void emu_ensure_init(void)
{
    if (!emu_inited) {
        emu68k_init(0x10000, 1);         /* 64KB shadow, write-through */
        emu_inited = 1;
        vlogf("emu: core initialised (64KB shadow, write-through)");
    }
}

static void *emu_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (emu_inited && emu_run) {
            emu_cycles += (uint64_t)emu68k_run(20000);
            emu68k_poll_irq();
            emu_snoops += (uint64_t)emu68k_snoop_apply();
        } else
            usleep(2000);
    }
    return NULL;
}

static void h_emu_state(int fd)
{
    emu68k_regs_t r;
    memset(&r, 0, sizeof r);
    if (emu_inited) emu68k_get_regs(&r);
    char body[1024];
    size_t n = 0;
    n += (size_t)snprintf(body + n, sizeof body - n,
        "{\"inited\":%s,\"running\":%s,\"cycles\":%llu,\"snoops\":%llu,"
        "\"pc\":%u,\"sr\":%u,\"usp\":%u,\"isp\":%u,\"d\":[",
        emu_inited ? "true" : "false", emu_run ? "true" : "false",
        (unsigned long long)emu_cycles, (unsigned long long)emu_snoops,
        r.pc, r.sr, r.usp, r.isp);
    for (int i = 0; i < 8; i++)
        n += (size_t)snprintf(body + n, sizeof body - n, "%s%u", i ? "," : "", r.d[i]);
    n += (size_t)snprintf(body + n, sizeof body - n, "],\"a\":[");
    for (int i = 0; i < 8; i++)
        n += (size_t)snprintf(body + n, sizeof body - n, "%s%u", i ? "," : "", r.a[i]);
    n += (size_t)snprintf(body + n, sizeof body - n, "]}");
    resp(fd, 200, "application/json", body, n);
}

/* ---------------- handlers ---------------- */
static void h_status(int fd)
{
    uint16_t sig = hw_reg_read(VREG_CTRL);
    uint16_t st = hw_status();
    jresp(fd, 200,
        "{\"signature\":%u,\"sig_ok\":%s,\"status\":%u,\"busy\":%s,"
        "\"fault\":%s,\"snoop_avail\":%s,\"ipl\":%u,\"reset_in\":%s,"
        "\"halt_in\":%s,\"vpa\":%s,\"snoop_ovf\":%s,\"cmd_full\":%s,"
        "\"cmd_ovf\":%s,\"mock\":%s}",
        sig, (sig >> 8) == 0x56 ? "true" : "false", st,
        (st & VST_BUSY) ? "true" : "false",
        (st & VST_FAULT) ? "true" : "false",
        (st & VST_SNOOP) ? "true" : "false",
        VST_IPL(st),
        (st & VST_RESET_IN) ? "true" : "false",
        (st & VST_HALT_IN) ? "true" : "false",
        (st & VST_VPA) ? "true" : "false",
        (st & VST_SNOOP_OVF) ? "true" : "false",
        (st & VST_CMD_FULL) ? "true" : "false",
        (st & VST_CMD_OVF) ? "true" : "false",
        hw_is_mock() ? "true" : "false");
}

static void h_bus_read(int fd, const req_t *r)
{
    uint32_t addr = (uint32_t)qnum(r, "addr", 0);
    int word = (int)qnum(r, "word", 1);
    uint16_t v = 0;
    uint16_t st = hw_bus_read(addr, word, &v);
    jresp(fd, 200, "{\"addr\":%u,\"value\":%u,\"fault\":%s}",
          addr, v, (st & VST_FAULT) ? "true" : "false");
}

static void h_bus_write(int fd, const req_t *r)
{
    uint32_t addr = (uint32_t)qnum(r, "addr", 0);
    int word = (int)qnum(r, "word", 1);
    uint16_t data = (uint16_t)qnum(r, "data", 0);
    uint16_t st = hw_bus_write(addr, word, data);
    vlogf("bus write %06x = %04x%s", addr, data, (st & VST_FAULT) ? " FAULT" : "");
    jresp(fd, 200, "{\"fault\":%s}", (st & VST_FAULT) ? "true" : "false");
}

static void h_snoop(int fd, const req_t *r)
{
    long max = qnum(r, "max", 100);
    char *body = malloc(65536);
    size_t n = 0;
    n += (size_t)sprintf(body + n, "{\"records\":[");
    vmpu68_snoop_t rec;
    int k = 0;
    while (k < max && n < 60000 && hw_snoop_pop(&rec)) {
        n += (size_t)sprintf(body + n, "%s{\"addr\":%u,\"data\":%u,\"uds\":%u,\"lds\":%u}",
                             k ? "," : "", rec.addr, rec.data, rec.uds, rec.lds);
        k++;
    }
    n += (size_t)sprintf(body + n, "],\"count\":%d}", k);
    resp(fd, 200, "application/json", body, n);
    free(body);
}

static void h_flash(int fd, const req_t *r)
{
    char err[128] = "";
    vlogf("fpga flash: %zu bytes", r->body_len);
    if (hw_flash_program(r->body, r->body_len, err, sizeof err) == 0)
        jresp(fd, 200, "{\"ok\":true,\"bytes\":%zu}", r->body_len);
    else
        jresp(fd, 500, "{\"ok\":false,\"error\":\"%s\"}", err);
}

static void h_ota(int fd, const req_t *r)
{
    FILE *f = fopen(ota_path, "wb");
    if (!f) {
        jresp(fd, 500, "{\"ok\":false,\"error\":\"cannot open %s\"}", ota_path);
        return;
    }
    fwrite(r->body, 1, r->body_len, f);
    fclose(f);
    vlogf("ota staged: %zu bytes -> %s", r->body_len, ota_path);
    jresp(fd, 200, "{\"ok\":true,\"bytes\":%zu,\"staged\":\"%s\"}", r->body_len, ota_path);
}

static void h_log(int fd)
{
    char body[64 * 168];
    size_t n = 0;
    unsigned start = logn > 64 ? logn - 64 : 0;
    for (unsigned i = start; i < logn; i++)
        n += (size_t)snprintf(body + n, sizeof body - n, "%s\n", logbuf[i & 63]);
    resp(fd, 200, "text/plain", body, n);
}

static void route(int fd, req_t *r)
{
    int post = strcmp(r->method, "POST") == 0;
    if (!strcmp(r->path, "/"))
        resp(fd, 200, "text/html; charset=utf-8", web_ui_html, web_ui_html_len);
    else if (!strcmp(r->path, "/api/status"))    h_status(fd);
    else if (!strcmp(r->path, "/api/bus/read"))  h_bus_read(fd, r);
    else if (!strcmp(r->path, "/api/bus/write") && post) h_bus_write(fd, r);
    else if (!strcmp(r->path, "/api/snoop"))     h_snoop(fd, r);
    else if (!strcmp(r->path, "/api/fpga/flashid")) {
        uint8_t id[3];
        hw_flash_id(id);
        jresp(fd, 200, "{\"id\":[%u,%u,%u]}", id[0], id[1], id[2]);
    }
    else if (!strcmp(r->path, "/api/fpga/flash") && post) h_flash(fd, r);
    else if (!strcmp(r->path, "/api/ota") && post) h_ota(fd, r);
    else if (!strcmp(r->path, "/api/led") && post) {
        hw_set_led((unsigned)qnum(r, "v", 0));
        jresp(fd, 200, "{\"ok\":true}");
    }
    else if (!strcmp(r->path, "/api/drv") && post) {
        hw_set_drv((int)qnum(r, "reset", -1), (int)qnum(r, "halt", -1));
        jresp(fd, 200, "{\"ok\":true}");
    }
    else if (!strcmp(r->path, "/api/log")) h_log(fd);
    else if (!strcmp(r->path, "/api/emu/state")) h_emu_state(fd);
    else if (!strcmp(r->path, "/api/emu/load") && post) {
        emu_ensure_init();
        uint32_t addr = (uint32_t)qnum(r, "addr", 0);
        emu68k_load(addr, r->body, (uint32_t)r->body_len);
        vlogf("emu: loaded %zu bytes at %06x", r->body_len, addr);
        jresp(fd, 200, "{\"ok\":true,\"bytes\":%zu}", r->body_len);
    }
    else if (!strcmp(r->path, "/api/emu/reset") && post) {
        emu_ensure_init();
        emu68k_reset();
        vlogf("emu: reset");
        jresp(fd, 200, "{\"ok\":true}");
    }
    else if (!strcmp(r->path, "/api/emu/start") && post) {
        emu_ensure_init();
        emu_run = 1;
        vlogf("emu: start");
        jresp(fd, 200, "{\"ok\":true}");
    }
    else if (!strcmp(r->path, "/api/emu/stop") && post) {
        emu_run = 0;
        vlogf("emu: stop");
        jresp(fd, 200, "{\"ok\":true}");
    }
    else if (!strcmp(r->path, "/api/mock/dma") && post && hw_is_mock()) {
        hw_mock_dma((uint32_t)qnum(r, "addr", 0x2000),
                    (uint16_t)qnum(r, "data", 0xD000), (int)qnum(r, "n", 3));
        jresp(fd, 200, "{\"ok\":true}");
    }
    else jresp(fd, 404, "{\"error\":\"not found\"}");
}

/* ---------------- HTTP plumbing (Linux transport) ---------------- */
static int read_request(int fd, req_t *r)
{
    static char hdr[8192];
    size_t n = 0;
    char *bodystart = NULL;
    while (n < sizeof hdr - 1) {
        ssize_t k = read(fd, hdr + n, sizeof hdr - 1 - n);
        if (k <= 0) return -1;
        n += (size_t)k;
        hdr[n] = 0;
        if ((bodystart = strstr(hdr, "\r\n\r\n"))) break;
    }
    if (!bodystart) return -1;
    bodystart += 4;

    char *q;
    if (sscanf(hdr, "%7s %255s", r->method, r->path) != 2) return -1;
    r->query[0] = 0;
    if ((q = strchr(r->path, '?'))) {
        *q = 0;
        snprintf(r->query, sizeof r->query, "%s", q + 1);
    }

    size_t clen = 0;
    const char *cl = strcasestr(hdr, "Content-Length:");
    if (cl) clen = (size_t)strtoul(cl + 15, NULL, 10);
    if (clen > MAX_BODY) return -1;

    r->body = NULL;
    r->body_len = clen;
    if (clen) {
        r->body = malloc(clen);
        size_t have = n - (size_t)(bodystart - hdr);
        if (have > clen) have = clen;
        memcpy(r->body, bodystart, have);
        while (have < clen) {
            ssize_t k = read(fd, r->body + have, clen - have);
            if (k <= 0) { free(r->body); return -1; }
            have += (size_t)k;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int port = 6800;
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--port")) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ota-path")) snprintf(ota_path, sizeof ota_path, "%s", argv[++i]);
    }

    int is_mock = hw_init();
    vlogf("mgmtd start: port %d, %s backend", port, is_mock ? "MOCK" : "hardware");

    pthread_t et;
    pthread_create(&et, NULL, emu_thread, NULL);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = INADDR_ANY };
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8)) {
        perror("bind/listen");
        return 1;
    }

    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        req_t r;
        if (read_request(c, &r) == 0) {
            route(c, &r);
            free(r.body);
        }
        close(c);
    }
}
