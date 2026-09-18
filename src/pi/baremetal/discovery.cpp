// SPDX-License-Identifier: MIT
// UDP beacon peer discovery (see discovery.h for the wire format).
#include "discovery.h"
#include "config.h"
#include "version.h"
#include <circle/net/in.h>
#include <circle/netdevice.h>
#include <circle/net/netconfig.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>

CDiscovery *g_pDiscovery = nullptr;

static unsigned now_ms (void)
{
    return CTimer::Get ()->GetTicks () * (1000 / HZ);
}

CDiscovery::CDiscovery (CNetSubSystem *pNet)
:   m_pNet (pNet), m_pSocket (nullptr), m_nNextTx (0)
{
    memset (m_Peers, 0, sizeof m_Peers);
}

CDiscovery::~CDiscovery (void)
{
    delete m_pSocket;
}

boolean CDiscovery::Initialize (void)
{
    m_pSocket = new CSocket (m_pNet, IPPROTO_UDP);
    if (m_pSocket->Bind (DISC_PORT) < 0
        || m_pSocket->SetOptionBroadcast (TRUE) < 0)
    {
        delete m_pSocket;
        m_pSocket = nullptr;
        return FALSE;
    }
    m_nNextTx = now_ms () + 500;               // first beacon shortly after link-up
    return TRUE;
}

// "Sep  6 2026" + "21:47:23" -> "20260906-2147" (same shape as the vpk label;
// the beacon's version field must not contain spaces)
const char *CDiscovery::BuildLabel (void)
{
    static char s_Label[16] = "";
    if (s_Label[0])
        return s_Label;
    static const char *const s_Mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__, *t = __TIME__;
    unsigned m = 0;
    for (unsigned i = 0; i < 12; i++)
        if (strncmp (d, s_Mon + i * 3, 3) == 0)
            m = i + 1;
    unsigned day = (d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0');
    CString S;
    S.Format ("%c%c%c%c%02u%02u-%c%c%c%c", d[7], d[8], d[9], d[10], m, day,
              t[0], t[1], t[3], t[4]);
    strncpy (s_Label, (const char *) S, sizeof s_Label - 1);
    return s_Label;
}

// one whitespace-separated token; returns pointer past it (and its trailing blanks)
static const char *token (const char *p, char *pOut, unsigned nOut)
{
    unsigned n = 0;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r')
    {
        if (n + 1 < nOut)
            pOut[n++] = *p;
        p++;
    }
    pOut[n] = 0;
    while (*p == ' ')
        p++;
    return p;
}

void CDiscovery::Poll (void)
{
    if (!m_pSocket)
        return;
    unsigned now = now_ms ();

    // beacons received: "x68pico <device> <version> <port> [name]"
    for (unsigned k = 0; k < 8; k++)
    {
        u8 buf[FRAME_BUFFER_SIZE];
        CIPAddress From;
        u16 nPort;
        int n = m_pSocket->ReceiveFrom (buf, sizeof buf - 1, MSG_DONTWAIT, &From, &nPort);
        if (n <= 0)
            break;
        buf[n] = 0;
        char magic[16], dev[16], ver[16], port[8];
        const char *p = token ((const char *) buf, magic, sizeof magic);
        p = token (p, dev, sizeof dev);
        p = token (p, ver, sizeof ver);
        p = token (p, port, sizeof port);
        if (strcmp (magic, "x68pico") != 0 || !dev[0] || !ver[0] || !port[0])
            continue;
        if (From == *m_pNet->GetConfig ()->GetIPAddress ())
            continue;                          // our own broadcast

        int free_i = -1;
        for (int i = 0; i < DISC_MAX_PEERS; i++)
        {
            if (m_Peers[i].used && From == m_Peers[i].ip) { free_i = i; break; }
            if (!m_Peers[i].used && free_i < 0) free_i = i;
        }
        if (free_i < 0)
            continue;                          // table full
        TDiscPeer *pe = &m_Peers[free_i];
        pe->used = TRUE;
        From.CopyTo (pe->ip);
        pe->http_port = 0;
        for (const char *q = port; *q >= '0' && *q <= '9'; q++)
            pe->http_port = pe->http_port * 10 + (*q - '0');
        strncpy (pe->device, dev, sizeof pe->device - 1);   pe->device[sizeof pe->device - 1] = 0;
        strncpy (pe->version, ver, sizeof pe->version - 1); pe->version[sizeof pe->version - 1] = 0;
        unsigned nn = 0;                       // rest of the line = name (may hold spaces)
        while (*p && *p != '\n' && *p != '\r' && nn + 1 < sizeof pe->name)
            pe->name[nn++] = *p++;
        pe->name[nn] = 0;
        pe->last_seen_ms = now;
    }

    // expire silent peers
    for (int i = 0; i < DISC_MAX_PEERS; i++)
        if (m_Peers[i].used && now - m_Peers[i].last_seen_ms > DISC_EXPIRE_MS)
            m_Peers[i].used = FALSE;

    // our own beacon
    if ((int) (now - m_nNextTx) < 0)
        return;
    m_nNextTx = now + DISC_PERIOD_MS;
    CString Line;
    // "x68pico vmpu68 <version> 80 [name]" — same shape as vhd68/vfd68
    Line.Format ("x68pico vmpu68 %s 80%s%s", VMPU68_VERSION,
                 cfg_name ()[0] ? " " : "", cfg_name ());
    m_pSocket->SendTo ((const char *) Line, Line.GetLength (), MSG_DONTWAIT,
                       *m_pNet->GetConfig ()->GetBroadcastAddress (), DISC_PORT);
}

static char *jstr (char *o, char *end, const char *s)
{
    for (; *s && o + 6 < end; s++)
    {
        if (*s == '"' || *s == '\\') { *o++ = '\\'; *o++ = *s; }
        else if ((u8) *s < 0x20) *o++ = ' ';
        else *o++ = *s;
    }
    return o;
}

unsigned CDiscovery::FormatPeers (char *pBuf, unsigned nSize) const
{
    char *o = pBuf, *end = pBuf + nSize - 8;
    CString S ("{\"ok\":true,\"peers\":[");
    strcpy (o, S); o += S.GetLength ();
    boolean first = TRUE;
    for (int i = 0; i < DISC_MAX_PEERS; i++)
    {
        const TDiscPeer *p = &m_Peers[i];
        if (!p->used)
            continue;
        S.Format ("%s{\"device\":\"%s\",\"version\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"port\":%u,\"name\":\"",
                  first ? "" : ",", p->device, p->version,
                  p->ip[0], p->ip[1], p->ip[2], p->ip[3], p->http_port);
        first = FALSE;
        if (o + S.GetLength () + 40 >= end)
            break;
        strcpy (o, S); o += S.GetLength ();
        o = jstr (o, end, p->name);
        *o++ = '"'; *o++ = '}';
    }
    *o++ = ']'; *o++ = '}'; *o = 0;
    return o - pBuf;
}
