// SPDX-License-Identifier: MIT
// See imgfs.h.  human68k.c keeps one open image in static state, so every
// call here opens, works and closes; the streamed download holds the image
// open between calls and refuses a second one meanwhile.
#include "imgfs.h"
#include "sdimg.h"
#include "vscsi.h"
#include <circle/util.h>
#include <fatfs/ff.h>
extern "C" {
#include "human68k.h"
#include "iso9660.h"
}

static FIL     s_fil;                   // the image being worked on
static boolean s_busy;                  // a download holds s_fil open
static boolean s_get_iso; static h68k_file_t s_get_f; static u32 s_get_left;   // download state

static bool io_read (void *, uint64_t off, void *buf, uint32_t len)
{ UINT br; return f_lseek (&s_fil, (FSIZE_t) off) == FR_OK && f_read (&s_fil, buf, len, &br) == FR_OK && br == len; }
static bool io_write (void *, uint64_t off, const void *buf, uint32_t len)
{ UINT bw; return f_lseek (&s_fil, (FSIZE_t) off) == FR_OK && f_write (&s_fil, buf, len, &bw) == FR_OK && bw == len; }

static boolean is_iso (const char *p) { unsigned l = strlen (p); return l > 4 && (strcmp (p + l - 4, ".iso") == 0 || strcmp (p + l - 4, ".ISO") == 0); }
static boolean mounted (const char *img)
{
    char a[160], b[160]; sdimg_path (img, a, sizeof a);
    for (unsigned id = 0; id < 7; id++)
    {
        const vscsi_slot_info *s = vscsi_slot (id);
        if (s && s->mounted && strcmp (sdimg_path (s->path, b, sizeof b), a) == 0) return TRUE;
    }
    return FALSE;
}

static void json_bytes (CString &Out, const char *s)       // byte-exact (Shift-JIS names): only quote, backslash and controls escaped
{
    for (const unsigned char *p = (const unsigned char *) s; *p; p++)
    {
        char c[2] = { (char) *p, 0 };
        if (*p == '"' || *p == '\\') Out.Append ("\\");
        if (*p < 0x20) { CString u; u.Format ("\\u%04X", *p); Out.Append (u); continue; }
        Out.Append (c);
    }
}
static CString *s_out; static unsigned s_n;
static void h68k_cb (const h68k_entry_t *e, void *)
{
    if (s_out->GetLength () > 60000) return;
    CString t; if (s_n++) s_out->Append (",");
    s_out->Append ("{\"n\":\""); json_bytes (*s_out, e->name);
    t.Format ("\",\"s\":%u,\"d\":%u,\"c\":%u}", (unsigned) e->size, e->is_dir ? 1u : 0u, (unsigned) e->cluster); s_out->Append (t);
}
static void iso_cb (const iso_entry_t *e, void *)
{
    if (s_out->GetLength () > 60000) return;
    CString t; if (s_n++) s_out->Append (",");
    s_out->Append ("{\"n\":\""); json_bytes (*s_out, e->name);
    t.Format ("\",\"s\":%u,\"d\":%u,\"c\":%u}", (unsigned) e->size, e->is_dir ? 1u : 0u, (unsigned) e->extent); s_out->Append (t);
}

int imgfs_ls (const char *img, u32 clu, u32 sz, CString &Out)
{
    if (s_busy) { Out = "{\"ok\":false,\"error\":\"busy: a download is running\"}"; return -1; }
    char fp[160]; sdimg_path (img, fp, sizeof fp);
    if (f_open (&s_fil, fp, FA_READ) != FR_OK) { Out = "{\"ok\":false,\"error\":\"cannot open image\"}"; return -1; }
    Out = "{\"files\":["; s_out = &Out; s_n = 0; int r;
    if (is_iso (fp)) r = iso_list_dir (&s_fil, clu, sz, iso_cb, 0);
    else
    {
        h68k_io_t io = { io_read, 0, 0 };
        r = h68k_open (&io) == 0 ? h68k_list_dir (clu, h68k_cb, 0) : -2;
        h68k_close ();
    }
    f_close (&s_fil);
    if (r < 0) { Out = r == -2 ? "{\"ok\":false,\"error\":\"not a Human68k image\"}" : "{\"ok\":false,\"error\":\"not an ISO9660 image\"}"; return -1; }
    Out.Append ("]}");
    return 0;
}

int imgfs_get_open (const char *img, u32 clu, u32 sz, const char *name, u32 *size, const char **err)
{
    if (s_busy) { *err = "busy: another download running"; return -1; }
    char fp[160]; sdimg_path (img, fp, sizeof fp);
    if (f_open (&s_fil, fp, FA_READ) != FR_OK) { *err = "cannot open image"; return -1; }
    s_get_iso = is_iso (fp);
    if (s_get_iso)
    {
        iso_entry_t e;
        if (!iso_find (&s_fil, clu, sz, name, &e) || e.is_dir || f_lseek (&s_fil, (FSIZE_t) e.extent * ISO_SECTOR) != FR_OK)
        { f_close (&s_fil); *err = "file not found in image"; return -1; }
        s_get_left = e.size; *size = e.size;
    }
    else
    {
        h68k_io_t io = { io_read, 0, 0 };
        if (h68k_open (&io) != 0 || h68k_stat (clu, name, &s_get_f, size) != 0)
        { h68k_close (); f_close (&s_fil); *err = "file not found in image"; return -1; }
        s_get_left = *size;
    }
    s_busy = TRUE;
    return 0;
}
int imgfs_get_read (u8 *buf, u32 len)
{
    if (!s_busy || !s_get_left) return 0;
    if (len > s_get_left) len = s_get_left;
    int n;
    if (s_get_iso) { UINT br = 0; n = f_read (&s_fil, buf, len, &br) == FR_OK ? (int) br : -1; }
    else n = h68k_file_read (&s_get_f, buf, len);
    if (n > 0) s_get_left -= (u32) n;
    return n;
}
void imgfs_get_close (void)
{
    if (!s_busy) return;
    if (!s_get_iso) h68k_close ();
    f_close (&s_fil); s_busy = FALSE;
}

// ---- writes ----
static int open_rw (const char *img, const char **err)
{
    if (s_busy) { *err = "busy: a download is running"; return -1; }
    char fp[160]; sdimg_path (img, fp, sizeof fp);
    if (is_iso (fp)) { *err = "a CD image is read-only"; return -1; }
    if (mounted (img)) { *err = "image is mounted; eject that ID first"; return -1; }
    if (f_open (&s_fil, fp, FA_READ | FA_WRITE) != FR_OK) { *err = "cannot open image"; return -1; }
    static h68k_io_t io = { io_read, io_write, 0 };
    if (h68k_open (&io) != 0) { h68k_close (); f_close (&s_fil); *err = "not a Human68k image"; return -1; }
    return 0;
}
static void close_rw (void) { f_sync (&s_fil); h68k_close (); f_close (&s_fil); }

static FIL s_src;
static bool src_read (void *, uint32_t off, uint8_t *buf, uint32_t len)
{ UINT br; return f_lseek (&s_src, (FSIZE_t) off) == FR_OK && f_read (&s_src, buf, len, &br) == FR_OK && br == len; }

int imgfs_inject (const char *img, const char *src, const char *name, u32 dir, int ow, u32 t, const char **err)
{
    char sp[160]; sdimg_path (src, sp, sizeof sp);
    FILINFO fi;
    if (f_stat (sp, &fi) != FR_OK) { *err = "src not found"; return -1; }
    if (t) h68k_set_stamp ((uint16_t) t, (uint16_t) (t >> 16)); else h68k_set_stamp (fi.ftime, fi.fdate);
    if (f_open (&s_src, sp, FA_READ) != FR_OK) { *err = "src open failed"; return -1; }
    if (open_rw (img, err)) { f_close (&s_src); return -1; }
    h68k_file_t tf; uint32_t ts; int r;
    bool exists = h68k_stat (dir, name, &tf, &ts) == 0;
    if (exists && !ow) r = -6;
    else { if (exists) h68k_delete (dir, name); r = h68k_inject_stream_at (name, dir, (uint32_t) f_size (&s_src), src_read, 0); }
    close_rw (); f_close (&s_src);
    if (r == 0) return 0;
    *err = r == -6 ? "already exists" : r == -2 ? "bad name" : r == -4 ? "image is full" : "inject failed";
    return -1;
}
int imgfs_rm (const char *img, const char *name, u32 dir, const char **err)
{
    if (open_rw (img, err)) return -1;
    int r = h68k_delete (dir, name); close_rw ();
    if (r == 0) return 0;
    *err = r == -6 ? "directory not empty" : "not found"; return -1;
}
int imgfs_mkdir (const char *img, const char *name, u32 dir, u32 t, const char **err)
{
    h68k_set_stamp ((uint16_t) t, (uint16_t) (t >> 16));
    if (open_rw (img, err)) return -1;
    int r = h68k_mkdir (dir, name); close_rw ();
    if (r == 0) return 0;
    *err = r == -5 ? "already exists" : r == -2 ? "bad name" : r == -4 ? "directory is full" : "mkdir failed"; return -1;
}
int imgfs_rename (const char *img, const char *name, const char *to, u32 dir, const char **err)
{
    if (open_rw (img, err)) return -1;
    int r = h68k_rename (dir, name, to); close_rw ();
    if (r == 0) return 0;
    *err = r == -5 ? "same name exists" : r == -2 ? "bad name" : "not found"; return -1;
}
int imgfs_move (const char *img, const char *name, u32 from, u32 to, const char **err)
{
    if (open_rw (img, err)) return -1;
    int r = h68k_move (from, name, to); close_rw ();
    if (r == 0) return 0;
    *err = r == -5 ? "same name exists at destination" : r == -2 ? "directories cannot be moved" : r == -4 ? "destination directory is full" : "move failed"; return -1;
}
