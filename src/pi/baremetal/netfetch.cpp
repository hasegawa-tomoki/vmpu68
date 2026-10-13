// SPDX-License-Identifier: MIT
// LAN file pull: see netfetch.h.  Plain HTTP/1.0 GET over a CSocket, the body
// streamed to the file in 32 KB pieces (circle's CHTTPClient keeps the whole
// document in memory, which rules it out for disk images).
#include "netfetch.h"
#include "sdimg.h"
#include "vscsi.h"
#include "config.h"
#include "imgfs.h"
#include <circle/net/socket.h>
#include <circle/net/ipaddress.h>
#include <circle/net/in.h>
#include <circle/sched/task.h>
#include <circle/sched/scheduler.h>
#include <circle/logger.h>
#include <circle/util.h>
#include <circle/timer.h>
#include <fatfs/ff.h>

static CNetSubSystem *s_pNet;
static struct
{
    volatile int state;                 // 0 idle, 1 running, 2 done, 3 error
    char     ip[24], path[256], dst[128], err[80], name[64], img[128];
    unsigned port, dir, t; int slot;
    volatile u32 done, total;
} s_job;

static boolean parse_ip (const char *s, u8 ip[4])
{
    unsigned v = 0, n = 0;
    for (const char *p = s;; p++)
    {
        if (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); if (v > 255) return FALSE; }
        else if (*p == '.' || *p == 0) { if (n >= 4) return FALSE; ip[n++] = (u8) v; v = 0; if (*p == 0) break; }
        else return FALSE;
    }
    return n == 4;
}

// Receive that gives up after 30 s without data (a stalled peer must not hold
// the only transfer slot forever): >0 bytes, 0 closed, -1 error, -2 timeout
static int recv_wait (CSocket &sock, u8 *buf, unsigned n)
{
    unsigned idle = 0;
    for (;;)
    {
        int r = sock.Receive (buf, n, MSG_DONTWAIT);
        if (r > 0) return r;
        if (r == 0) { CScheduler::Get ()->MsSleep (10); if (++idle >= 3000) return -2; continue; }   // nothing yet (or closed: circle reports 0 either way)
        return -1;
    }
}

class CNetfetchTask : public CTask
{
public:
    CNetfetchTask (void) : CTask (32768) { SetName ("netfetch"); }
    void Run (void) override
    {
        const char *why = run ();
        if (why) { strncpy (s_job.err, why, sizeof s_job.err - 1); s_job.err[sizeof s_job.err - 1] = 0; s_job.state = 3;
                   CLogger::Get ()->Write ("netfetch", LogWarning, "%s: %s", s_job.dst, why); }
        else if (s_job.img[0])                       // into an image: inject the temp file, then drop it
        {
            const char *e = "";
            char fp[160]; int rc = imgfs_inject (s_job.img, s_job.dst, s_job.name, s_job.dir, 1, s_job.t, &e);
            f_unlink (sdimg_path (s_job.dst, fp, sizeof fp));
            if (rc) { strncpy (s_job.err, e, sizeof s_job.err - 1); s_job.err[sizeof s_job.err - 1] = 0; s_job.state = 3; }
            else { s_job.state = 2; CLogger::Get ()->Write ("netfetch", LogNotice, "%s -> %s in %s", s_job.name, s_job.img, s_job.ip); }
        }
        else
        {
            if (s_job.slot >= 0 && s_job.slot < 7)
            {
                int rc = vscsi_mount ((unsigned) s_job.slot, s_job.dst, FALSE);
                if (rc == 0) { cfg_set_vscsi ((unsigned) s_job.slot, s_job.dst, 0); cfg_save (); }
                else { strncpy (s_job.err, "transferred, but it does not mount", sizeof s_job.err - 1); s_job.state = 3; return; }
            }
            s_job.state = 2;
            CLogger::Get ()->Write ("netfetch", LogNotice, "%s: %u bytes from %s", s_job.dst, s_job.done, s_job.ip);
        }
    }
private:
    const char *run (void)
    {
        u8 ipb[4]; if (!parse_ip (s_job.ip, ipb)) return "bad ip";
        CIPAddress ip (ipb);
        CSocket sock (s_pNet, IPPROTO_TCP);
        if (sock.Connect (ip, (u16) s_job.port) < 0) return "cannot connect";
        CString req; req.Format ("GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", s_job.path, s_job.ip);
        if (sock.Send ((const char *) req, req.GetLength (), 0) < 0) return "cannot send the request";
        u8 *buf = new u8[32768]; if (!buf) return "out of memory";
        char fp[160]; sdimg_path (s_job.dst, fp, sizeof fp);
        FIL f; boolean open = FALSE; const char *why = 0;
        // header: read until the blank line, keep what follows as body
        unsigned have = 0, hdr_end = 0; boolean hdr_done = FALSE; u32 clen = 0xFFFFFFFF; int status = 0;
        while (!hdr_done)
        {
            if (have >= 8192) { why = "header too long"; break; }
            int n = recv_wait (sock, buf + have, 32768 - have);
            if (n <= 0) { why = n == -2 ? "no answer (timeout)" : "connection closed in the header"; break; }
            have += n;
            for (unsigned i = 3; i < have; i++)
                if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') { hdr_end = i + 1; hdr_done = TRUE; break; }
        }
        if (!why)
        {
            buf[hdr_end - 1] = 0;
            const char *h = (const char *) buf;
            if (strncmp (h, "HTTP/1.", 7) == 0) status = (h[9] - '0') * 100 + (h[10] - '0') * 10 + (h[11] - '0');
            for (const char *p = h; *p; p++)
                if ((p == h || p[-1] == '\n') && (strncmp (p, "Content-Length:", 15) == 0 || strncmp (p, "content-length:", 15) == 0))
                { clen = 0; for (p += 15; *p == ' '; p++); for (; *p >= '0' && *p <= '9'; p++) clen = clen * 10 + (*p - '0'); break; }
            if (status != 200) why = "the peer did not serve the file";
            else if (clen == 0xFFFFFFFF) why = "no Content-Length";
        }
        if (!why)
        {
            s_job.total = clen;
            if (f_open (&f, fp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) why = "cannot create the file";
            else open = TRUE;
        }
        u32 written = 0;
        if (!why && have > hdr_end)
        {
            UINT bw = 0;
            if (f_write (&f, buf + hdr_end, have - hdr_end, &bw) != FR_OK || bw != have - hdr_end) why = "write failed";
            written += bw; s_job.done = written;
        }
        while (!why && written < clen)
        {
            int n = recv_wait (sock, buf, 32768);
            if (n == -2) { why = "stalled (no data for 30 s)"; break; }
            if (n < 0) { why = "receive failed"; break; }
            if ((u32) n > clen - written) n = (int) (clen - written);
            UINT bw = 0;
            if (f_write (&f, buf, (UINT) n, &bw) != FR_OK || bw != (UINT) n) { why = "write failed"; break; }
            written += bw; s_job.done = written;
            CScheduler::Get ()->Yield ();
        }
        if (open) { f_close (&f); if (why) f_unlink (fp); }
        delete [] buf;
        return why;
    }
};

void netfetch_init (CNetSubSystem *pNet) { s_pNet = pNet; }

int netfetch_start (const char *ip, unsigned port, const char *path, const char *dst, int slot, const char *img, const char *name, unsigned dir, unsigned t)
{
    if (!s_pNet || !ip || !ip[0] || !path || path[0] != '/' || !dst || dst[0] != '/' || strlen (dst) < 2) return -2;
    if (s_job.state == 1) return -1;
    strncpy (s_job.ip, ip, sizeof s_job.ip - 1); s_job.ip[sizeof s_job.ip - 1] = 0;
    strncpy (s_job.path, path, sizeof s_job.path - 1); s_job.path[sizeof s_job.path - 1] = 0;
    strncpy (s_job.dst, dst, sizeof s_job.dst - 1); s_job.dst[sizeof s_job.dst - 1] = 0;
    s_job.port = port ? port : 80; s_job.slot = slot; s_job.done = 0; s_job.total = 0; s_job.err[0] = 0;
    s_job.img[0] = 0; s_job.name[0] = 0; s_job.dir = dir; s_job.t = t;
    if (img && name) { strncpy (s_job.img, img, sizeof s_job.img - 1); s_job.img[sizeof s_job.img - 1] = 0; strncpy (s_job.name, name, sizeof s_job.name - 1); s_job.name[sizeof s_job.name - 1] = 0; }
    s_job.state = 1;
    new CNetfetchTask;                                   // runs on core 0 under the scheduler, deletes itself when done
    return 0;
}

const char *netfetch_busy_path (void) { return s_job.state == 1 ? s_job.dst : ""; }

void netfetch_status_json (CString &Out)
{
    switch (s_job.state)
    {
    case 1:  Out.Format ("{\"ok\":true,\"state\":\"running\",\"bytes\":%u,\"total\":%u,\"dst\":\"%s\"}", s_job.done, s_job.total, s_job.dst); break;   // vhd68/vfd68 wording
    case 2:  Out.Format ("{\"ok\":true,\"state\":\"done\",\"bytes\":%u,\"dst\":\"%s\"}", s_job.done, s_job.dst); s_job.state = 0; break;   // one poll takes the result
    case 3:  Out.Format ("{\"ok\":true,\"state\":\"error\",\"error\":\"%s\",\"dst\":\"%s\"}", s_job.err, s_job.dst); s_job.state = 0; break;
    default: Out = "{\"ok\":true,\"state\":\"idle\"}";
    }
}
