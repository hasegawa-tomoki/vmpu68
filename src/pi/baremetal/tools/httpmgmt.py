#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# HTTP management client for the vmpu68 bare-metal firmware.
#   httpmgmt.py <host> status
#   httpmgmt.py <host> put <local-file> <SD:/path>
#   httpmgmt.py <host> fw <kernel.img>      # install as stable + reboot
#   httpmgmt.py <host> try <kernel.img>     # install as try kernel + tryboot
#   httpmgmt.py <host> update <pkg.vpk> [try] [force]   # install a release package
#   httpmgmt.py <host> reboot | tbr
#   httpmgmt.py <host> emutest              # load+run the 68k test program
import sys, time, urllib.request, urllib.parse, json

CHUNK = 8000            # bytes per POST (hex fits the 16 KB HTTP_MAX_FORM_DATA cap)

def checksum(data):
    s = 0
    for b in data:
        s = (s * 31 + b) & 0xFFFFFFFF
    return s

def get(host, path):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=15) as r:
        return json.loads(r.read())

def post(host, path, body):
    # the firmware's TCP stack runs out of PCBs under a storm of short
    # connections; back off and retry instead of failing the upload
    for attempt in range(12):
        try:
            req = urllib.request.Request(f"http://{host}{path}", data=body.encode(),
                                         headers={"Content-Type": "application/x-www-form-urlencoded"})
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.loads(r.read())
        except (urllib.error.URLError, OSError) as e:
            if attempt == 11:
                raise
            time.sleep(5)

def upload(host, data, sdpath):
    q = urllib.parse.quote(sdpath, safe="")
    off = 0
    while off < len(data):
        n = min(CHUNK, len(data) - off)
        last = 1 if off + n >= len(data) else 0
        body = "data=" + data[off:off + n].hex()
        r = post(host, f"/api/put?path={q}&off={off}&last={last}", body)
        if not r.get("ok"):
            raise SystemExit(f"upload failed at {off}: {r}")
        off += n
        time.sleep(0.02)                 # pace connections
        if off % (CHUNK * 64) == 0 or last:
            print(f"  {off}/{len(data)}")
    want = f"{checksum(data):08X}"
    if r.get("sum") != want or r.get("size") != len(data):
        raise SystemExit(f"VERIFY FAILED: {r} expected size={len(data)} sum={want}")
    print(f"ok {len(data)} bytes sum={want} -> {sdpath}")

def main():
    host, cmd = sys.argv[1], sys.argv[2]
    if cmd == "status":
        print(json.dumps(get(host, "/api/status"), indent=1))
    elif cmd == "put":
        upload(host, open(sys.argv[3], "rb").read(), sys.argv[4])
    elif cmd == "fw":
        upload(host, open(sys.argv[3], "rb").read(), "SD:/kernel8-rpi4.img")
        print(get(host, "/api/reboot")); print("rebooting...")
    elif cmd == "try":
        upload(host, open(sys.argv[3], "rb").read(), "SD:/kernel8-try.img")
        print(get(host, "/api/tbr")); print("tryboot rebooting...")
    elif cmd == "update":
        # same flow as the browser page: upload, start, follow /api/status
        upload(host, open(sys.argv[3], "rb").read(), "SD:/update.vpk")
        mode = "try" if "try" in sys.argv[4:] else "stable"
        force = 1 if "force" in sys.argv[4:] else 0
        r = get(host, f"/api/update?mode={mode}&force={force}")
        print(r)
        if not r.get("ok"):
            raise SystemExit(1)
        last, down = None, 0
        while True:
            time.sleep(1)
            try:
                u = get(host, "/api/status").get("update", {})
            except Exception:
                down += 1
                if last == "reboot":
                    print(f"  rebooting... {down}s", end="\r")
                    if down > 180:
                        raise SystemExit("no answer after reboot")
                    continue
                if down > 5:
                    raise SystemExit("connection lost")
                continue
            # back after the reboot: phase is "reboot" only until the board
            # actually restarts, and a slow poll (timeout) can miss it, so
            # also accept "idle" once any install phase has been seen
            if (last == "reboot" and (down or u.get("phase") != "reboot")) \
               or (last not in (None, "idle") and u.get("phase") == "idle"):
                j = get(host, "/api/status")
                print(f"\nback: {j.get('version')} (uptime {j.get('uptime')}s)")
                break
            down = 0
            if u.get("phase") != last:
                last = u.get("phase")
                print(f"  [{last}] {u.get('msg', '')}")
            if last == "error":
                raise SystemExit(1)
            if u.get("total"):
                print(f"  {last} {u['done'] * 100 // u['total']}%", end="\r")
    elif cmd == "reboot":
        print(get(host, "/api/reboot"))
    elif cmd == "tbr":
        print(get(host, "/api/tbr"))
    elif cmd == "emutest":
        img = bytes(bytearray(0x400 + 18))
        img = bytes.fromhex("0000200000000400") + img[8:0x400] + \
              bytes.fromhex("700041F8100030C052400C40001066F660FE")
        off = 0
        while off < len(img):
            n = min(CHUNK, len(img) - off)
            r = post(host, f"/api/emu/load?addr={off}", "data=" + img[off:off+n].hex())
            assert r.get("ok"), r
            off += n
        print(f"loaded {len(img)} bytes")
        print(get(host, "/api/emu/reset"))
        r = get(host, "/api/emu/run?cycles=100000")
        print(r)
        assert r["pc"] == 0x410, f"PC={r['pc']:#x}, expected 0x410"
        print(get(host, "/api/emu/regs"))
        ok = True
        for i in range(16):
            v = get(host, f"/api/bus/read?addr={0x1000 + i*2}")["value"]
            if v != i:
                ok = False
                print(f"  MISMATCH at {0x1000+i*2:#x}: {v}")
        print("EMU HTTP", "ALL PASS" if ok else "FAIL")
    else:
        raise SystemExit("unknown command")

main()
