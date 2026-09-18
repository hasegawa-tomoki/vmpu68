// SPDX-License-Identifier: MIT
#include "webserver.h"
#include <circle/string.h>
#include <circle/version.h>
#include <circle/util.h>
#include <fatfs/ff.h>
#include <circle/net/netdevlayer.h>
#include <circle/macaddress.h>
#include <circle/timer.h>
#include <circle/sched/scheduler.h>
#include "update_page.h"
#include "screen_js.h"
#include "discovery.h"
#include "config.h"
#include "version.h"

extern "C" {
#include "../mgmtd/hw.h"
#ifdef VMPU68_WITH_EMU
#include "../core/emu68k.h"
#include "../core/jit.h"
#endif
}

#ifdef VMPU68_WITH_EMU
void vmpu68_emu_ensure (void);               // defined in kernel.cpp
unsigned vmpu68_emu_type (const char *text, int enter, unsigned *pDropped);   // keys into Human68k (kernel.cpp)
void vmpu68_emu_pause (int on);              // stop/resume core 1 around bus work (kernel.cpp)
void vmpu68_emu_reset (void);                // machine reset with core 1 stopped meanwhile (kernel.cpp)
int  vmpu68_emu_fd_busy (void);              // a floppy transfer may be running (kernel.cpp)
unsigned vmpu68_bus_mhz (void);              // measured X68000 bus clock (kernel.cpp)
unsigned vmpu68_bus_mhz10 (void);
unsigned vmpu68_equiv_mhz10 (void);          // 68000-equivalent MHz x10 (kernel.cpp)
int vmpu68_bus_setup_auto (void);
struct cfg_bus_class; const cfg_bus_class *vmpu68_bus_class (void);
#define EMU_PAUSE(on) vmpu68_emu_pause (on)
#else
#define EMU_PAUSE(on) ((void) 0)
#endif

// Screen reads pause the emulator.  During a floppy transfer that pause
// starves the FDC's DMA (the snoop FIFO fills, the FPGA stops granting the
// bus, the FDC overruns) and the IOCS then waits forever for a result
// phase nobody reads (docs §27).  So a read first waits for the FDC to go
// quiet and, if it does not within max_ms, is refused with FD_BUSY_JSON
// (HTTP 200 + JSON: circle's daemon replaces the body of any other status
// with its own page, and the update API reports errors the same way).
#ifdef VMPU68_WITH_EMU
#include "config.h"
static unsigned bus_mhz10 (void)   { return vmpu68_bus_mhz10 (); }
static unsigned cls_min10 (void)   { const cfg_bus_class *c = vmpu68_bus_class (); return c ? c->min_mhz10 : 0; }
static const char *cls_source (void) { return vmpu68_bus_class () ? cfg_bus_source () : "-"; }
static int setup_auto (void)       { const cfg_bus_class *c = vmpu68_bus_class (); return c && c->wr_setup < 0; }
static unsigned io_mhz_now (void)  { return emu68k_io_mhz (); }
#else
static unsigned bus_mhz10 (void) { return 0; }
static unsigned cls_min10 (void) { return 0; }
static const char *cls_source (void) { return "-"; }
static int setup_auto (void) { return 0; }
static unsigned io_mhz_now (void) { return 0; }
#endif
static unsigned ram_mb (void)
{
#ifdef VMPU68_WITH_EMU
    return emu68k_ram_size () >> 20;
#else
    return 0;
#endif
}

static boolean wait_fd_idle (unsigned max_ms)
{
#ifdef VMPU68_WITH_EMU
    for (unsigned t = 0; vmpu68_emu_fd_busy (); t += 10)
    {
        if (t >= max_ms)
            return FALSE;
        CScheduler::Get ()->MsSleep (10);
    }
#endif
    return TRUE;
}
#define FD_BUSY_WAIT_MS 1000
#define FD_BUSY_JSON "{\"ok\":false,\"error\":\"fd_busy\",\"message\":" \
    "\"フロッピーの転送中(FDC 使用中)なので画面を読みませんでした。読込みが終わってからやり直してください\"}"

// response/content buffer per worker.  16 KB was enough for an 8 KB hex
// chunk plus form overhead; 64 KB lets /api/bus/dump hand the screen viewer
// 32K words per request (a TVRAM plane's visible rows in one go)
#define MAX_CONTENT_SIZE 65536

volatile int g_WebAction = 0;

CVmpuWebServer::CVmpuWebServer (CNetSubSystem *pNetSubSystem, CSocket *pSocket)
:   // receive timeout: circle's default (0) waits forever, and a client that
    // connects without completing a request (browser preconnect, a dropped
    // Wi-Fi link) then holds its worker for good - after 10 of them the
    // listener refuses everything ("Too many clients") until a reboot
    CHTTPDaemon (pNetSubSystem, pSocket, MAX_CONTENT_SIZE, HTTP_PORT, 0, 15),
    m_pNet (pNetSubSystem)
{
}

CVmpuWebServer::~CVmpuWebServer (void)
{
}

CHTTPDaemon *CVmpuWebServer::CreateWorker (CNetSubSystem *pNetSubSystem, CSocket *pSocket)
{
    return new CVmpuWebServer (pNetSubSystem, pSocket);
}

// ---- tiny helpers -------------------------------------------------------

static unsigned param_num (const char *pParams, const char *pKey, unsigned nDefault)
{
    if (!pParams) return nDefault;
    size_t kl = strlen (pKey);
    for (const char *p = pParams; *p; p++)
    {
        if ((p == pParams || p[-1] == '&') && strncmp (p, pKey, kl) == 0 && p[kl] == '=')
        {
            p += kl + 1;
            unsigned v = 0;
            int hex = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));
            if (hex) p += 2;
            while ((*p >= '0' && *p <= '9') ||
                   (hex && ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'f')))
            {
                unsigned d = (*p <= '9') ? (unsigned)(*p - '0')
                            : (unsigned)((*p | 0x20) - 'a' + 10);
                v = hex ? (v << 4) | d : v * 10 + d;
                p++;
            }
            return v;
        }
    }
    return nDefault;
}

static boolean param_str (const char *pParams, const char *pKey, char *pOut, size_t nOut)
{
    if (!pParams) return FALSE;
    size_t kl = strlen (pKey);
    for (const char *p = pParams; *p; p++)
    {
        if ((p == pParams || p[-1] == '&') && strncmp (p, pKey, kl) == 0 && p[kl] == '=')
        {
            p += kl + 1;
            size_t i = 0;
            while (*p && *p != '&' && i < nOut - 1)
            {
                char c = *p;
                if (c == '%' && p[1] && p[2])       // %xx unescape
                {
                    unsigned hi = (p[1] <= '9') ? (unsigned)(p[1] - '0') : (unsigned)((p[1] | 0x20) - 'a' + 10);
                    unsigned lo = (p[2] <= '9') ? (unsigned)(p[2] - '0') : (unsigned)((p[2] | 0x20) - 'a' + 10);
                    c = (char) ((hi << 4) | lo);
                    p += 2;
                }
                pOut[i++] = c;
                p++;
            }
            pOut[i] = 0;
            return TRUE;
        }
    }
    return FALSE;
}

// decode "data=<hex...>" from the form body into pOut; returns byte count
// or -1 on malformed input
static int form_hex_data (const char *pFormData, u8 *pOut, unsigned nMax)
{
    if (!pFormData) return -1;
    const char *p = strstr (pFormData, "data=");
    if (!p) return -1;
    p += 5;
    unsigned n = 0;
    while (p[0] && p[0] != '&' && p[1] && p[1] != '&' && n < nMax)
    {
        unsigned hi, lo;
        char a = p[0], b = p[1];
        if (a >= '0' && a <= '9') hi = (unsigned)(a - '0');
        else if ((a | 0x20) >= 'a' && (a | 0x20) <= 'f') hi = (unsigned)((a | 0x20) - 'a' + 10);
        else return -1;
        if (b >= '0' && b <= '9') lo = (unsigned)(b - '0');
        else if ((b | 0x20) >= 'a' && (b | 0x20) <= 'f') lo = (unsigned)((b | 0x20) - 'a' + 10);
        else return -1;
        pOut[n++] = (u8) ((hi << 4) | lo);
        p += 2;
    }
    return (int) n;
}

// running checksum over an SD file (same sum=sum*31+byte as the console
// transfer protocol), computed on demand for the last chunk
static boolean file_checksum (const char *pPath, u32 *pSum, unsigned *pSize)
{
    FIL f;
    if (f_open (&f, pPath, FA_READ) != FR_OK)
        return FALSE;
    static u8 buf[4096];
    u32 sum = 0;
    unsigned total = 0;
    UINT br;
    while (f_read (&f, buf, sizeof buf, &br) == FR_OK && br > 0)
    {
        for (UINT i = 0; i < br; i++)
            sum = sum * 31 + buf[i];
        total += br;
    }
    f_close (&f);
    *pSum = sum;
    *pSize = total;
    return TRUE;
}

// ---- request handling ---------------------------------------------------

THTTPStatus CVmpuWebServer::GetContent (const char *pPath, const char *pParams,
                                        const char *pFormData, u8 *pBuffer,
                                        unsigned *pLength, const char **ppContentType)
{
    CString Content;
    *ppContentType = "application/json";

    if (strcmp (pPath, "/api/status") == 0)
    {
        // no hardware or SD access while the installer owns the bus/flash
        unsigned st = g_Update.busy ? 0 : hw_status ();
        CString Mac ("");
        if (m_pNet)
            m_pNet->GetNetDeviceLayer ()->GetMACAddress ()->Format (&Mac);
        CString Name ("");                     // cfg name, JSON-escaped; "vmpu68" when unset
        for (const char *p = cfg_name ()[0] ? cfg_name () : "vmpu68"; *p; p++)
        {
            if (*p == '"' || *p == '\\')
                Name.Append ("\\");
            char c[2] = {*p, 0};
            if ((unsigned char) *p >= 0x20)
                Name.Append (c);
        }
        char fsum[16] = "", fver[16] = "";
        if (!g_Update.busy)
        {
            FIL f; UINT br = 0; char line[48];
            if (f_open (&f, FPGA_SUM_FILE, FA_READ) == FR_OK)
            {
                f_read (&f, line, sizeof line - 1, &br);
                f_close (&f);
            }
            line[br] = 0;
            // "<sum> <len> [<version>]"
            unsigned i = 0, o = 0;
            while (line[i] && line[i] != ' ' && o < 8) fsum[o++] = line[i++];
            fsum[o] = 0;
            while (line[i] == ' ') i++;
            while (line[i] && line[i] != ' ') i++;          // len
            while (line[i] == ' ') i++;
            o = 0;
            while (line[i] && line[i] != ' ' && line[i] != '\r' && line[i] != '\n' && o < 15) fver[o++] = line[i++];
            fver[o] = 0;
        }
        Content.Format ("{\"device\":\"vmpu68\",\"name\":\"%s\",\"mac\":\"%s\","
                        "\"status\":%u,\"busy\":%s,\"fault\":%s,\"snoop_avail\":%s,"
                        "\"ipl\":%u,\"vpa\":%s,\"mock\":%s,\"board\":%d,\"firmware\":\"baremetal\","
                        "\"version\":\"" VMPU68_VERSION "\",\"pi_version\":\"" VMPU68_PI_VERSION "\",\"fpga_version\":\"%s\",\"build\":\"" VMPU68_BUILD "\",\"uptime\":%u,\"fpga_sum\":\"%s\",\"ram_mb\":%u,\"bus_mhz\":%u.%u,\"bus_class_min_mhz\":%u.%u,\"bus_class_source\":\"%s\",\"wr_ai\":%s,\"wr_setup\":%u,\"wr_setup_auto\":%s,\"io_mhz\":%u,\"wait_ns\":%u,\"equiv_mhz\":%u.%u,\"mhz_limit\":%u,\"wb\":%s,\"jit\":%s,\"sramboot\":%s,\"hw\":\"%s\","
                        "\"update\":{\"busy\":%s,\"phase\":\"%s\",\"done\":%u,\"total\":%u,\"msg\":\"%s\"}}",
                        (const char *) Name, (const char *) Mac, st,
                        (st & VST_BUSY) ? "true" : "false",
                        (st & VST_FAULT) ? "true" : "false",
                        (st & VST_SNOOP) ? "true" : "false",
                        VST_IPL (st),
                        (st & VST_VPA) ? "true" : "false",
                        hw_is_mock () ? "true" : "false",
                        vmpu68_board_id (),
fver, CTimer::Get ()->GetUptime (), fsum, ram_mb (), bus_mhz10 () / 10, bus_mhz10 () % 10, cls_min10 () / 10, cls_min10 () % 10, cls_source (), hw_wr_ai () ? "true" : "false", hw_wr_setup (),
                        setup_auto () ? "true" : "false", io_mhz_now (), vmpu68_get_wait_ns (), vmpu68_equiv_mhz10 () / 10, vmpu68_equiv_mhz10 () % 10, vmpu68_mhz_limit (), emu68k_wb_enabled () ? "true" : "false", jit_enabled () ? "true" : "false", cfg_sramboot () ? "true" : "false", vmpu68_hw_rev (),
                        g_Update.busy ? "true" : "false", g_Update.phase,
                        g_Update.done, g_Update.total, g_Update.msg);
        // circle's daemon has no header hook: ride the CORS header on the
        // content type so that the LAN finder page (file://) can read it
        *ppContentType = "application/json\r\nAccess-Control-Allow-Origin: *";
    }
    else if (strcmp (pPath, "/api/peers") == 0)
    {
        char buf[1024];
        if (g_pDiscovery)
            g_pDiscovery->FormatPeers (buf, sizeof buf);
        else
            strcpy (buf, "{\"ok\":true,\"peers\":[]}");
        Content = buf;
        *ppContentType = "application/json\r\nAccess-Control-Allow-Origin: *";
    }
    else if (g_Update.busy && strncmp (pPath, "/api/", 5) == 0)
    {
        Content = "{\"error\":\"update in progress\"}";
    }
    else if (strcmp (pPath, "/api/update") == 0)
    {
        char mode[16] = "stable";
        param_str (pParams, "mode", mode, sizeof mode);
        vpk_hdr_t h;
        boolean same = FALSE, ksame = FALSE;
        char msg[96];
        int try_mode = strcmp (mode, "try") == 0;
        int force = param_num (pParams, "force", 0) != 0;
        if (!vmpu68_vpk_check (UPDATE_FILE, &h, &same, &ksame, msg, sizeof msg))
            Content.Format ("{\"ok\":false,\"error\":\"%s\"}", msg);
        else if (!force && !try_mode && ksame && (!h.fpga_len || same))
            Content = "{\"ok\":false,\"error\":\"already installed\"}";
        else
        {
            g_Update.try_mode = try_mode;
            g_Update.force = force;
            g_Update.busy = 1;                 // claimed now: the main loop takes over
            g_Update.phase = "queued"; g_Update.done = g_Update.total = 0;
            g_Update.msg[0] = 0;
            g_WebAction = 5;
            Content.Format ("{\"ok\":true,\"label\":\"%s\",\"kernel\":%u,\"fpga\":%u,"
                            "\"kernel_action\":\"%s\",\"fpga_action\":\"%s\",\"mode\":\"%s\"}",
                            h.label, h.kernel_len, h.fpga_len,
                            (ksame && !force && !try_mode) ? "same" : "copy",
                            !h.fpga_len ? "none" : (same && !force) ? "same" : "flash",
                            try_mode ? "try" : "stable");
        }
    }
    else if (strcmp (pPath, "/api/bus/read") == 0)
    {
        unsigned addr = param_num (pParams, "addr", 0);
        uint16_t v = 0;
        uint16_t st = hw_bus_read (addr, 1, &v);
        Content.Format ("{\"addr\":%u,\"value\":%u,\"fault\":%s}",
                        addr, v, (st & VST_FAULT) ? "true" : "false");
    }
    else if (strcmp (pPath, "/api/jprof") == 0)
    {
        // binary, in parts (the reply buffer is 64 KB): part=0 -> u64 instr, u64 blocks,
        // u32 acc[4], u32 blk[64]; part=1..8 -> u32 ops[8192] for opcodes (part-1)*8192..
        vmpu68_emu_ensure ();
        unsigned part = param_num (pParams, "part", 0);
        uint64_t ni = 0, nb = 0; const uint32_t *blk = emu68k_prof_blocks (&ni, &nb);
        const uint32_t *ops = emu68k_prof_ops ();
        *ppContentType = "application/octet-stream";
        if (!ops || part > 8 || *pLength < 8192 * 4) *pLength = 0;
        else if (part == 0)
        {
            u8 *q = pBuffer;
            memcpy (q, &ni, 8); q += 8; memcpy (q, &nb, 8); q += 8;
            memcpy (q, emu68k_prof_acc, 16); q += 16;
            memcpy (q, blk, 256); q += 256;
            *pLength = 16 + 16 + 256;
        }
        else
        {
            memcpy (pBuffer, ops + (part - 1) * 8192, 8192 * 4);
            *pLength = 8192 * 4;
        }
        return HTTPOK;                         // binary reply: skip the JSON tail
    }
    else if (strcmp (pPath, "/api/watch") == 0)
    {
        // binary dump of the emulator's access watch ring (console ewt):
        // skip=<newest records to leave out> n=<records>, oldest first,
        // each record the emu68k_watch_t struct as laid out in memory
        // (seq pc addr u32, data u16, size rd u8, us cnt instr u32); the
        // first 4 bytes are the total count so the reader can tell the
        // ring's position.  The emulator keeps running (records may be
        // appended meanwhile but the ones returned are consistent).
        unsigned skip = param_num (pParams, "skip", 0);
        unsigned n = param_num (pParams, "n", 1024);
        vmpu68_emu_ensure ();
        uint32_t total = emu68k_watch_count ();
        if (n > (*pLength - 4) / sizeof (emu68k_watch_t)) n = (*pLength - 4) / sizeof (emu68k_watch_t);
        if (skip >= total) n = 0;
        else if (n > total - skip) n = total - skip;
        memcpy (pBuffer, &total, 4);
        emu68k_watch_t *w = (emu68k_watch_t *) (pBuffer + 4);
        unsigned got = 0;
        for (int i = (int) (n + skip) - 1; i >= (int) skip; i--)
            if (emu68k_watch_get ((uint32_t) i, &w[got])) got++; else break;
        *pLength = 4 + got * sizeof (emu68k_watch_t);
        *ppContentType = "application/octet-stream";
        return HTTPOK;
    }
    else if (strcmp (pPath, "/api/bus/dump") == 0)
    {
        // binary word dump of the real bus (VRAM screenshots, verification):
        // addr=<byte addr> words=<n>, n <= half the content buffer.  While a
        // floppy transfer is running the reply is FD_BUSY_JSON instead
        // (Content-Type application/json) - see wait_fd_idle().  A word
        // that faults reads as 0; the sticky flag is cleared.  Block read:
        // word-by-word hw_bus_read while the emulator runs costs ~3 us/word
        // (its accesses break the prefetch stream), the block ~0.5 us.
        unsigned addr = param_num (pParams, "addr", 0) & 0xFFFFFE;
        unsigned words = param_num (pParams, "words", 256);
        if (words > *pLength / 2)
            words = *pLength / 2;
        uint16_t *w = (uint16_t *) pBuffer;         // in place, then byte-swapped
        unsigned nopause = param_num (pParams, "nopause", 0);   // diagnostics: share the bus with the running emulator
        if (emu68k_wb_enabled () && addr + 2 * words <= emu68k_ram_size ())
        {
            // main RAM under write-back: the shadow is the truth (the real
            // DRAM lags it until a DMA needs it); no pause, no bus cycles
            for (unsigned i = 0; i < words; i++)
            {
                unsigned v = emu68k_peek16 (addr + 2 * i);
                pBuffer[2 * i] = (u8) (v >> 8);
                pBuffer[2 * i + 1] = (u8) v;
            }
            *pLength = 2 * words;
            *ppContentType = "application/octet-stream";
            return HTTPOK;
        }
        if (!nopause && !wait_fd_idle (FD_BUSY_WAIT_MS))
        {
            Content = FD_BUSY_JSON;                 // falls through to the JSON tail
            *ppContentType = "application/json";
        }
        else
        {
            if (!nopause) EMU_PAUSE (1);            // ~25 ms for 32K words
            hw_bus_read_block (addr, words, w);
            if (!nopause) EMU_PAUSE (0);
            for (unsigned i = 0; i < words; i++)
            {
                uint16_t v = w[i];
                pBuffer[2 * i] = (u8) (v >> 8);
                pBuffer[2 * i + 1] = (u8) v;
            }
            *pLength = 2 * words;
            *ppContentType = "application/octet-stream";
            return HTTPOK;
        }
    }
    else if (strcmp (pPath, "/api/screen/regs") == 0)
    {
        // what the screen viewer needs besides the VRAM, as big-endian
        // words: CRTC R0-R23 (R0-R19 are write-only on the hardware, so
        // they come from the emulator's copy), video controller R0-R2,
        // graphic palette (256), text/sprite palette (256)
        uint16_t w[24 + 3 + 512];
        memset (w, 0, sizeof w);
        if (!wait_fd_idle (FD_BUSY_WAIT_MS))
        {
            Content = FD_BUSY_JSON;
            *ppContentType = "application/json";
        }
        else
        {
#ifdef VMPU68_WITH_EMU
            emu68k_crtc_shadow (w, 20);
#endif
            EMU_PAUSE (1);
            for (unsigned i = 20; i < 24; i++)
                hw_bus_read (0xE80000 + 2 * i, 1, &w[i]);
            hw_bus_read (0xE82400, 1, &w[24]);
            hw_bus_read (0xE82500, 1, &w[25]);
            hw_bus_read (0xE82600, 1, &w[26]);
            hw_bus_read_block (0xE82000, 512, &w[27]);
            hw_clear_fault ();
            EMU_PAUSE (0);
            for (unsigned i = 0; i < sizeof w / 2; i++)
            {
                pBuffer[2 * i] = (u8) (w[i] >> 8);
                pBuffer[2 * i + 1] = (u8) w[i];
            }
            *pLength = sizeof w;
            *ppContentType = "application/octet-stream";
            return HTTPOK;
        }
    }
    else if (strcmp (pPath, "/screen.js") == 0)
    {
        unsigned n = sizeof s_ScreenJs - 1;
        if (n > *pLength)
            return HTTPInternalServerError;
        memcpy (pBuffer, s_ScreenJs, n);
        *pLength = n;
        *ppContentType = "application/javascript; charset=utf-8";
        return HTTPOK;
    }
    else if (strcmp (pPath, "/api/put") == 0)
    {
        char path[128];
        if (!param_str (pParams, "path", path, sizeof path))
            return HTTPBadRequest;
        unsigned off = param_num (pParams, "off", 0);
        unsigned last = param_num (pParams, "last", 0);

        static u8 chunk[MAX_CONTENT_SIZE / 2];
        int n = form_hex_data (pFormData, chunk, sizeof chunk);
        if (n < 0)
            return HTTPBadRequest;

        FIL f;
        FRESULT fr = f_open (&f, path,
                             off == 0 ? (FA_WRITE | FA_CREATE_ALWAYS)
                                      : (FA_WRITE | FA_OPEN_EXISTING));
        if (fr == FR_OK && off > 0)
            fr = f_lseek (&f, off);
        UINT bw = 0;
        if (fr == FR_OK && n > 0)
            fr = f_write (&f, chunk, (UINT) n, &bw);
        if (fr == FR_OK)
            fr = f_close (&f);
        if (fr != FR_OK)
        {
            Content.Format ("{\"error\":\"fatfs %d\"}", (int) fr);
        }
        else if (last)
        {
            u32 sum = 0;
            unsigned size = 0;
            file_checksum (path, &sum, &size);
            Content.Format ("{\"ok\":true,\"size\":%u,\"sum\":\"%08X\"}", size, sum);
        }
        else
            Content.Format ("{\"ok\":true,\"off\":%u,\"wrote\":%d}", off, n);
    }
    else if (strcmp (pPath, "/api/config") == 0)   // /api/config?mhz=<0|10|16|24>&wb=<0|1>&jit=<0|1>: apply and save (absent = unchanged)
    {
        int mhz = (int) param_num (pParams, "mhz", 0xFFFF); if (mhz == 0xFFFF || mhz > 1000) mhz = -1;
        int wb  = (int) param_num (pParams, "wb", 0xFFFF);  if (wb == 0xFFFF) wb = -1; else wb = wb ? 1 : 0;
        int jit = (int) param_num (pParams, "jit", 0xFFFF); if (jit == 0xFFFF) jit = -1; else jit = jit ? 1 : 0;
        int sb  = (int) param_num (pParams, "sramboot", 0xFFFF); if (sb == 0xFFFF) sb = -1; else sb = sb ? 1 : 0;
        if (mhz >= 0 || wb >= 0 || jit >= 0) vmpu68_apply_settings (mhz, wb, jit, TRUE);
        int sbrc = 0;
        if (sb >= 0) sbrc = vmpu68_sramboot_set (sb, TRUE);
        char name[40];
        if (param_str (pParams, "name", name, sizeof name))   // /api/config?name=<urlencoded>: host name (31 bytes max)
        {
            name[31] = 0;
            cfg_set_name (name); cfg_save ();
        }
        CString N; for (const char *q = cfg_name (); *q; q++) { if (*q == '"' || *q == '\\') N.Append ("\\"); char c[2] = { *q, 0 }; N.Append (c); }
        Content.Format ("{\"ok\":%s,\"mhz_limit\":%u,\"wb\":%s,\"jit\":%s,\"sramboot\":%s,\"sramboot_state\":%d,\"name\":\"%s\"%s}", sbrc ? "false" : "true", vmpu68_mhz_limit (), emu68k_wb_enabled () ? "true" : "false", jit_enabled () ? "true" : "false",
                        cfg_sramboot () ? "true" : "false", vmpu68_sramboot_state (), (const char *) N, sbrc ? ",\"error\":\"SRAM write failed (X68000 off?)\"" : "");
    }
    else if (strcmp (pPath, "/api/locate") == 0)   // /api/locate[?ms=10000]: blink every colour (vfd68/vhd68 style)
    {
        unsigned ms = param_num (pParams, "ms", 10000);
        if (ms == 0 || ms > 600000) ms = 10000;
        vmpu68_led_locate (ms);
        Content.Format ("{\"ok\":true,\"ms\":%u}", ms);
    }
    else if (strcmp (pPath, "/api/reboot") == 0)
    {
        g_WebAction = 1;
        Content = "{\"ok\":true,\"action\":\"reboot\"}";
    }
    else if (strcmp (pPath, "/api/tbr") == 0)
    {
        g_WebAction = 2;
        Content = "{\"ok\":true,\"action\":\"tryboot\"}";
    }
    else if (strcmp (pPath, "/api/hang") == 0)
    {
        g_WebAction = 3;                     // deliberate hang: watchdog test
        Content = "{\"ok\":true,\"action\":\"hang\"}";
    }
    else if (strcmp (pPath, "/api/emu/irq") == 0)
    {
        unsigned lvl = param_num (pParams, "level", 0);
        hw_mock_set_ipl (lvl);
        Content.Format ("{\"ok\":true,\"ipl\":%u}", lvl & 7);
    }
#ifdef VMPU68_WITH_EMU
    else if (strcmp (pPath, "/api/emu/load") == 0)
    {
        unsigned addr = param_num (pParams, "addr", 0);
        static u8 chunk[MAX_CONTENT_SIZE / 2];
        int n = form_hex_data (pFormData, chunk, sizeof chunk);
        if (n < 0)
            return HTTPBadRequest;
        vmpu68_emu_ensure ();
        emu68k_load (addr, chunk, (unsigned) n);
        Content.Format ("{\"ok\":true,\"addr\":%u,\"wrote\":%d}", addr, n);
    }
    else if (strcmp (pPath, "/api/emu/reset") == 0)
    {
        vmpu68_emu_reset ();
        Content = "{\"ok\":true}";
    }
    else if (strcmp (pPath, "/api/emu/run") == 0)
    {
        unsigned cycles = param_num (pParams, "cycles", 10000);
        vmpu68_emu_ensure ();
        EMU_PAUSE (1);                              // single-step only with core 1 stopped
        int done = emu68k_run ((int) cycles);
        emu68k_poll_irq ();
        int sn = emu68k_snoop_apply ();
        EMU_PAUSE (0);
        Content.Format ("{\"ok\":true,\"cycles\":%d,\"pc\":%u,\"snoops\":%d}",
                        done, emu68k_pc (), sn);
    }
    else if (strcmp (pPath, "/api/joy") == 0)
    {
        // port=<0|1> btn=<a|b|up|down|left|right|start> (or val=<active-low byte>)
        // ms=<hold, default 200>: press a joystick button by intercepting reads
        // of $E9A001/$E9A003 (Dracula and many games scan the pad directly, not
        // through the IOCS keyboard buffer that /api/key fills).
        unsigned port = param_num (pParams, "port", 0);
        unsigned ms = param_num (pParams, "ms", 200);
        unsigned val = 0xFF;
        char btn[32];
        if (param_str (pParams, "btn", btn, sizeof btn))
        {
            if (strstr (btn, "up"))    val &= ~0x01;
            if (strstr (btn, "down"))  val &= ~0x02;
            if (strstr (btn, "left"))  val &= ~0x04;
            if (strstr (btn, "right")) val &= ~0x08;
            if (strstr (btn, "a") || strstr (btn, "trig")) val &= ~0x20;
            if (strstr (btn, "b"))     val &= ~0x40;
            if (strstr (btn, "start")) val &= ~0x60;   // A+B, some titles use this
        }
        else
            val = param_num (pParams, "val", 0xDF);    // default: button A (bit5)
        vmpu68_emu_ensure ();
        emu68k_joy_set ((int) port, val, ms);
        Content.Format ("{\"ok\":true,\"port\":%u,\"val\":\"0x%02X\",\"ms\":%u}", port, val & 0xFF, ms);
    }
    else if (strcmp (pPath, "/api/key") == 0)
    {
        // text=<url-encoded ASCII> enter=<0|1>: type into Human68k's keyboard
        // buffer (screen viewer's key box; same escapes as the console ekey)
        char text[256];
        if (!param_str (pParams, "text", text, sizeof text))
            text[0] = 0;
        unsigned dropped;
        EMU_PAUSE (1);                              // the IOCS handler must not update the buffer meanwhile
        unsigned sent = vmpu68_emu_type (text, param_num (pParams, "enter", 1) != 0, &dropped);
        EMU_PAUSE (0);
        Content.Format ("{\"ok\":true,\"sent\":%u,\"dropped\":%u}", sent, dropped);
    }
    else if (strcmp (pPath, "/api/emu/regs") == 0)
    {
        emu68k_regs_t r;
        vmpu68_emu_ensure ();
        emu68k_get_regs (&r);
        Content.Format ("{\"pc\":%u,\"sr\":%u,"
                        "\"d\":[%u,%u,%u,%u,%u,%u,%u,%u],"
                        "\"a\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
                        r.pc, r.sr,
                        r.d[0], r.d[1], r.d[2], r.d[3], r.d[4], r.d[5], r.d[6], r.d[7],
                        r.a[0], r.a[1], r.a[2], r.a[3], r.a[4], r.a[5], r.a[6], r.a[7]);
    }
#endif
    else if (strcmp (pPath, "/") == 0 || strcmp (pPath, "/index.html") == 0)
    {
        unsigned n = sizeof s_UpdatePage - 1;
        if (n > *pLength)
            return HTTPInternalServerError;
        memcpy (pBuffer, s_UpdatePage, n);
        *pLength = n;
        *ppContentType = "text/html; charset=utf-8";
        return HTTPOK;
    }
    else
        return HTTPNotFound;

    unsigned nLength = Content.GetLength ();
    if (nLength > *pLength)
        return HTTPInternalServerError;
    memcpy (pBuffer, (const char *) Content, nLength);
    *pLength = nLength;
    return HTTPOK;
}
