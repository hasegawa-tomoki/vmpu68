// SPDX-License-Identifier: MIT
// LAN peer discovery, wire-compatible with vhd68/vfd68 (their discovery.h):
// every 2 s one line is broadcast on UDP port 6868,
//
//     x68pico <device> <version> <http_port> [name]
//     e.g. "x68pico vmpu68 20260906-2147 80"
//
// and every beacon heard from another sender is kept in a small peer table
// (IP, device, version, name, last seen).  The Web UI lists the table via
// /api/peers and talks to each peer's own HTTP API (CORS is open on all of
// them).  Polled from the main loop; no task of its own.
#ifndef _discovery_h
#define _discovery_h

#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/types.h>

#define DISC_PORT        6868
#define DISC_PERIOD_MS   2000
#define DISC_EXPIRE_MS   10000       // drop a peer after 5 missed beacons
#define DISC_MAX_PEERS   8

struct TDiscPeer
{
    boolean  used;
    u8       ip[4];
    unsigned http_port;
    char     device[16];             // "vhd68" / "vfd68" / "vmpu68"
    char     version[16];
    char     name[32];               // display name, "" if the peer has none
    unsigned last_seen_ms;
};

class CDiscovery
{
public:
    CDiscovery (CNetSubSystem *pNet);
    ~CDiscovery (void);

    boolean Initialize (void);
    void Poll (void);                // main loop: send / receive / expire

    // peers as JSON: {"ok":true,"peers":[{device,version,ip,port,name},...]}
    unsigned FormatPeers (char *pBuf, unsigned nSize) const;

    static const char *BuildLabel (void);   // "YYYYMMDD-HHMM" from __DATE__/__TIME__

private:
    CNetSubSystem *m_pNet;
    CSocket       *m_pSocket;
    unsigned       m_nNextTx;
    TDiscPeer      m_Peers[DISC_MAX_PEERS];
};

extern CDiscovery *g_pDiscovery;     // set once the network is up, else nullptr

#endif
