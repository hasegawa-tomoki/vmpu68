// SPDX-License-Identifier: MIT
// See lanlabel.h.  Tiny JSON scraping: enough for vhd68's "ids":[{...}] and
// vfd68's "xbar":..,"slots":[{...}] - the k-th object of the array is slot k.
#include "lanlabel.h"
#include "discovery.h"
#include "vscsi.h"
#include <circle/net/httpclient.h>
#include <circle/net/ipaddress.h>
#include <circle/util.h>
#include <circle/logger.h>
extern "C" {
#include "../core/emu68k.h"
}

static CNetSubSystem *s_pNet;
void lanlabel_init (CNetSubSystem *pNet) { s_pNet = pNet; }

static const char *find_key (const char *p, const char *key)
{
    unsigned n = strlen (key);
    for (; *p; p++) if (p[0] == '"' && strncmp (p + 1, key, n) == 0 && p[n + 1] == '"') return p + n + 2;
    return 0;
}
// the k-th {...} inside the array that starts at p (p points after '[')
static const char *array_item (const char *p, unsigned k, unsigned *len)
{
    int depth = 0; const char *start = 0; unsigned idx = 0;
    for (; *p; p++)
    {
        if (*p == '{') { if (depth++ == 0) start = p; }
        else if (*p == '}') { if (--depth == 0) { if (idx++ == k) { *len = (unsigned) (p - start + 1); return start; } } }
        else if (*p == ']' && depth == 0) break;
    }
    return 0;
}
static boolean item_bool (const char *obj, unsigned len, const char *key)
{
    const char *v = find_key (obj, key); if (!v || v >= obj + len) return FALSE;
    while (*v == ':' || *v == ' ') v++;
    return strncmp (v, "true", 4) == 0;
}
static void item_str (const char *obj, unsigned len, const char *key, char *out, unsigned n)
{
    out[0] = 0;
    const char *v = find_key (obj, key); if (!v || v >= obj + len) return;
    while (*v == ':' || *v == ' ') v++;
    if (*v != '"') return;
    unsigned k = 0; for (v++; *v && *v != '"' && k + 1 < n; v++) { if (*v == '\\' && v[1]) v++; out[k++] = *v; }
    out[k] = 0;
}

void lanlabel_poll (void)
{
    if (!emu68k_vscsi_label_requested ()) return;
    for (unsigned i = 0; i < 12; i++) emu68k_vscsi_set_label (i, "");
    for (unsigned id = 0; id < 7; id++)                       // our own virtual disks
    {
        const vscsi_slot_info *s = vscsi_slot (id);
        if (s && s->mounted) { CString L; const char *b = s->path; for (const char *p = s->path; *p; p++) if (*p == '/') b = p + 1; L.Format ("%-6s %s", "vmpu68", b); emu68k_vscsi_set_label (id, L); }
    }
    u8 *buf = s_pNet ? new u8[16384] : 0;
    for (unsigned i = 0; buf && g_pDiscovery && i < DISC_MAX_PEERS; i++)
    {
        const TDiscPeer *p = g_pDiscovery->Peer (i);
        if (!p) continue;
        boolean hd = strcmp (p->device, "vhd68") == 0, fd = strcmp (p->device, "vfd68") == 0;
        if (!hd && !fd) continue;
        CIPAddress ip (p->ip);
        CHTTPClient c (s_pNet, ip, (u16) (p->http_port ? p->http_port : 80));
        unsigned len = 16383;
        if (c.Get ("/api/status", buf, &len) != HTTPOK) continue;
        buf[len] = 0;
        const char *js = (const char *) buf;
        const char *arr = find_key (js, hd ? "ids" : "slots");
        if (!arr) continue;
        while (*arr == ':' || *arr == ' ') arr++;
        if (*arr != '[') continue;
        boolean xbar = fd ? item_bool (js, len, "xbar") : TRUE;
        for (unsigned k = 0; k < 8; k++)
        {
            unsigned olen; const char *obj = array_item (arr + 1, k, &olen);
            if (!obj) break;
            boolean mounted = item_bool (obj, olen, "mounted");
            if (hd && !mounted) continue;                        // an empty vhd68 ID answers nothing: no drive to label
            char name[40]; item_str (obj, olen, "name", name, sizeof name);
            const char *base = name; for (const char *q = name; *q; q++) if (*q == '/') base = q + 1;   // vfd68 reports the SD path
            CString L; L.Format ("%-6s %s", p->device, mounted ? base : "(no image)");   // "vhd68  x" / "vmpu68 x": names line up
            if (hd) emu68k_vscsi_set_label (k, L);
            else if (k < 2) emu68k_vscsi_set_label (8 + (xbar ? k : k + 2), L);   // vfd68: FDD 0/1 with the crossbar on, 2/3 off (shown even when empty)
        }
    }
    delete [] buf;
    emu68k_vscsi_labels_ready ();
}
