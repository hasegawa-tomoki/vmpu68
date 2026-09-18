// SPDX-License-Identifier: MIT
#include "config.h"
#include <fatfs/ff.h>
#include <circle/util.h>

static char s_Name[CFG_NAME_MAX] = "";
static unsigned s_RamMB;                // ram=<MB>, 0 = auto
static unsigned s_MHz;                  // mhz=<n>, 0 = unlimited
static unsigned s_WB = 1;               // wb=<0|1>, default write-back
static unsigned s_JIT = 1;              // jit=<0|1>, default on (1.1.0; was off)
static unsigned s_SramBoot = 0;         // sramboot=<0|1>, default off
static char s_HW[12] = "";             // hw=<rev>, e.g. 2.1; empty = from the board profile
static unsigned s_Board;                // board=<1|2> GPIO layout, 0 = auto
static unsigned s_SMI = 1;              // smi=<0|1>: SMI register transport on core 2.x

static void set_name (const char *v)
{
    unsigned n = 0;
    while (v[n] && v[n] != '\r' && v[n] != '\n' && n + 1 < sizeof s_Name)
        n++;
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))     // trailing blanks
        n--;
    memcpy (s_Name, v, n);
    s_Name[n] = 0;
}

void cfg_load (void)
{
    s_Name[0] = 0;
    s_RamMB = 0;
    s_MHz = 0;
    s_WB = 1;
    s_Board = 0;
    s_SMI = 1;
    s_JIT = 1;
    FIL f;
    if (f_open (&f, CFG_FILE, FA_READ) != FR_OK)
        return;
    char line[128];
    while (f_gets (line, sizeof line, &f))
    {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == 0)
            continue;
        if (strncmp (p, "name=", 5) == 0)
            set_name (p + 5);
        else if (strncmp (p, "ram=", 4) == 0)
        {
            unsigned n = 0;
            for (const char *q = p + 4; *q >= '0' && *q <= '9'; q++)
                n = n * 10 + (*q - '0');
            s_RamMB = n <= 12 ? n : 0;      // X68000 main RAM tops out at 12 MB
        }
        else if (strncmp (p, "mhz=", 4) == 0)
        {
            unsigned n = 0;
            for (const char *q = p + 4; *q >= '0' && *q <= '9'; q++)
                n = n * 10 + (*q - '0');
            s_MHz = n <= 1000 ? n : 0;
        }
        else if (strncmp (p, "wb=", 3) == 0)
            s_WB = p[3] == '0' ? 0 : 1;
        else if (strncmp (p, "jit=", 4) == 0)
            s_JIT = p[4] == '1' ? 1 : 0;
        else if (strncmp (p, "sramboot=", 9) == 0)
            s_SramBoot = p[9] == '1' ? 1 : 0;
        else if (strncmp (p, "hw=", 3) == 0)
        {
            unsigned n = 0;
            for (const char *q = p + 3; *q > ' ' && n < sizeof s_HW - 1; q++) s_HW[n++] = *q;
            s_HW[n] = 0;
        }
        else if (strncmp (p, "board=", 6) == 0)
            s_Board = (p[6] == '1' || p[6] == '2') ? (unsigned) (p[6] - '0') : 0;
        else if (strncmp (p, "smi=", 4) == 0)
            s_SMI = p[4] == '0' ? 0 : 1;
        // unknown keys are ignored (forward compatible)
    }
    f_close (&f);
}

unsigned cfg_wb (void) { return s_WB; }
unsigned cfg_jit (void) { return s_JIT; }
unsigned cfg_sramboot (void) { return s_SramBoot; }
void cfg_set_sramboot (unsigned on) { s_SramBoot = on ? 1 : 0; }
const char *cfg_hw (void) { return s_HW; }
unsigned cfg_board (void) { return s_Board; }
unsigned cfg_smi (void) { return s_SMI; }
void cfg_set_smi (unsigned on) { s_SMI = on ? 1 : 0; }
void cfg_set_board (unsigned id) { s_Board = (id == 1 || id == 2) ? id : 0; }
void cfg_set (unsigned mhz, unsigned wb, unsigned jit) { s_MHz = mhz <= 1000 ? mhz : 0; s_WB = wb ? 1 : 0; s_JIT = jit ? 1 : 0; }
void cfg_set_name (const char *v) { set_name (v); }
void cfg_set_ram (unsigned mb) { s_RamMB = mb <= 12 ? mb : 0; }

// Rewrite the file: lines with the keys we own (name=, ram=, mhz=, wb=, jit=, sramboot=, board=, smi=)
// are replaced, everything else (comments, hw=, unknown keys) is kept as it was.
int cfg_save (void)
{
    static char keep[16][128]; unsigned nk = 0;
    FIL f;
    if (f_open (&f, CFG_FILE, FA_READ) == FR_OK)
    {
        char line[128];
        while (nk < 16 && f_gets (line, sizeof line, &f))
        {
            const char *p = line; while (*p == ' ' || *p == '\t') p++;
            if (strncmp (p, "mhz=", 4) == 0 || strncmp (p, "wb=", 3) == 0 || strncmp (p, "jit=", 4) == 0 || strncmp (p, "sramboot=", 9) == 0 || strncmp (p, "board=", 6) == 0
             || strncmp (p, "name=", 5) == 0 || strncmp (p, "ram=", 4) == 0 || strncmp (p, "smi=", 4) == 0) continue;
            unsigned n = 0; while (line[n] && line[n] != '\r' && line[n] != '\n') n++;
            memcpy (keep[nk], line, n); keep[nk][n] = 0; nk++;
        }
        f_close (&f);
    }
    if (f_open (&f, CFG_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;
    UINT bw; int ok = 1;
    for (unsigned i = 0; i < nk; i++)
    {
        char l[132]; unsigned n = 0; while (keep[i][n]) { l[n] = keep[i][n]; n++; } l[n++] = '\n';
        ok = ok && f_write (&f, l, n, &bw) == FR_OK;
    }
    char tail[160]; unsigned n = 0;
    const char *a;
    if (s_Name[0]) { a = "name="; while (*a) tail[n++] = *a++; a = s_Name; while (*a) tail[n++] = *a++; tail[n++] = '\n'; }
    if (s_RamMB)   { a = "ram=";  while (*a) tail[n++] = *a++; { char d[8]; unsigned k = 0, v = s_RamMB; do { d[k++] = (char) ('0' + v % 10); v /= 10; } while (v); while (k) tail[n++] = d[--k]; } tail[n++] = '\n'; }
    a = "mhz="; while (*a) tail[n++] = *a++;
    { char d[8]; unsigned k = 0, v = s_MHz; do { d[k++] = (char) ('0' + v % 10); v /= 10; } while (v); while (k) tail[n++] = d[--k]; }
    tail[n++] = '\n';
    a = "wb="; while (*a) tail[n++] = *a++; tail[n++] = (char) ('0' + s_WB); tail[n++] = '\n';
    a = "jit="; while (*a) tail[n++] = *a++; tail[n++] = (char) ('0' + s_JIT); tail[n++] = '\n';
    a = "sramboot="; while (*a) tail[n++] = *a++; tail[n++] = (char) ('0' + s_SramBoot); tail[n++] = '\n';
    if (s_Board) { a = "board="; while (*a) tail[n++] = *a++; tail[n++] = (char) ('0' + s_Board); tail[n++] = '\n'; }
    if (!s_SMI)  { a = "smi=0\n"; while (*a) tail[n++] = *a++; }
    ok = ok && f_write (&f, tail, n, &bw) == FR_OK;
    f_sync (&f); f_close (&f);
    return ok ? 0 : -1;
}

const char *cfg_name (void)
{
    return s_Name;
}

unsigned cfg_ram_mb (void)
{
    return s_RamMB;
}

unsigned cfg_mhz (void)
{
    return s_MHz;
}

// ---------------- bus timing classes ----------------
static cfg_bus_class s_Bus[CFG_BUS_MAX];
static unsigned s_BusN;
static const char *s_BusSource = "built-in";

static void bus_defaults (void)
{
    s_BusN = 2;
    s_Bus[0] = { 0,   -1, 500, 10, 1 };        // 10 MHz machines
    s_Bus[1] = { 130, -1, 500, 16, 1 };        // XVI at 16 MHz: wr_setup probed (2 in practice)
    s_BusSource = "built-in";
}

// minimal JSON reader for the flat shape above: tolerant of whitespace and
// newlines, ignores unknown keys, stops at the first malformed object
static const char *skip_ws (const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }
static const char *parse_num10 (const char *p, unsigned *out, int tenths)   // 16.7 -> 167 when tenths
{
    unsigned v = 0, frac = 0; int seen = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; seen = 1; }
    if (*p == '.') { p++; if (*p >= '0' && *p <= '9') { frac = *p - '0'; p++; if (*p >= '5' && *p <= '9') frac++; while (*p >= '0' && *p <= '9') p++; } }
    if (!seen) return 0;
    if (frac >= 10) { frac = 0; v++; }
    *out = tenths ? v * 10 + frac : v + (frac >= 5 ? 1 : 0);
    return p;
}
static int parse_class (const char *p, const char **end, cfg_bus_class *c)
{
    *c = { 0, -1, 500, 10, 1 };
    int have_min = 0;
    p = skip_ws (p);
    if (*p != '{') return 0;
    p++;
    for (;;)
    {
        p = skip_ws (p);
        if (*p == '}') { *end = p + 1; return have_min; }
        if (*p == ',') { p++; continue; }
        if (*p != '"') return 0;
        const char *k = ++p;
        while (*p && *p != '"') p++;
        if (!*p) return 0;
        unsigned klen = (unsigned) (p - k);
        p = skip_ws (p + 1);
        if (*p != ':') return 0;
        p = skip_ws (p + 1);
        unsigned num = 0; int is_auto = 0, is_true = 0, is_false = 0;
        if (*p == '"')
        {
            const char *v = ++p;
            while (*p && *p != '"') p++;
            if (!*p) return 0;
            if (p - v == 4 && strncmp (v, "auto", 4) == 0) is_auto = 1;
            p++;
        }
        else if (strncmp (p, "true", 4) == 0) { is_true = 1; p += 4; }
        else if (strncmp (p, "false", 5) == 0) { is_false = 1; p += 5; }
        else
        {
            const char *q = parse_num10 (p, &num, strncmp (k, "min_mhz", klen) == 0 && klen == 7);
            if (!q) return 0;
            p = q;
        }
        if (klen == 7 && strncmp (k, "min_mhz", 7) == 0)       { c->min_mhz10 = num; have_min = 1; }
        else if (klen == 8 && strncmp (k, "wr_setup", 8) == 0) c->wr_setup = is_auto ? -1 : (int) (num & 3);
        else if (klen == 7 && strncmp (k, "wait_ns", 7) == 0)  c->wait_ns = num;
        else if (klen == 6 && strncmp (k, "io_mhz", 6) == 0)   c->io_mhz = num;
        else if (klen == 2 && strncmp (k, "ai", 2) == 0)       c->ai = is_true ? 1 : is_false ? 0 : (num != 0);
        // unknown keys are ignored
    }
}

void cfg_bus_load (void)
{
    bus_defaults ();
    FIL f;
    if (f_open (&f, CFG_BUS_FILE, FA_READ) != FR_OK)
        return;
    static char buf[4096];
    UINT n = 0;
    f_read (&f, buf, sizeof buf - 1, &n);
    f_close (&f);
    buf[n] = 0;
    const char *p = strstr (buf, "\"bus\"");
    if (!p) return;
    p = skip_ws (p + 5);
    if (*p != ':') return;
    p = skip_ws (p + 1);
    if (*p != '[') return;
    p++;
    cfg_bus_class cls[CFG_BUS_MAX];
    unsigned cnt = 0;
    for (;;)
    {
        p = skip_ws (p);
        if (*p == ']' || *p == 0) break;
        if (*p == ',') { p++; continue; }
        const char *e;
        if (cnt >= CFG_BUS_MAX || !parse_class (p, &e, &cls[cnt])) break;
        cnt++;
        p = e;
    }
    if (!cnt) return;
    // sort by min_mhz (insertion sort, tiny) so cfg_bus_select can walk it
    for (unsigned i = 1; i < cnt; i++)
        for (unsigned j = i; j > 0 && cls[j - 1].min_mhz10 > cls[j].min_mhz10; j--)
        { cfg_bus_class t = cls[j]; cls[j] = cls[j - 1]; cls[j - 1] = t; }
    memcpy (s_Bus, cls, cnt * sizeof cls[0]);
    s_BusN = cnt;
    s_BusSource = "vmpu68-bus.json";
}
unsigned cfg_bus_count (void) { return s_BusN; }
const cfg_bus_class *cfg_bus_class_at (unsigned i) { return i < s_BusN ? &s_Bus[i] : nullptr; }
const cfg_bus_class *cfg_bus_select (unsigned mhz10)
{
    const cfg_bus_class *best = &s_Bus[0];
    for (unsigned i = 0; i < s_BusN; i++)
        if (s_Bus[i].min_mhz10 <= mhz10) best = &s_Bus[i];
    return best;
}
const char *cfg_bus_source (void) { return s_BusSource; }
