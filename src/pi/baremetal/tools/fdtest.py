#!/usr/bin/env python3
# fdtest.py <pause|dump|dumpnp|none> <秒数>: FD アクセス中(pc が FF93xx の FDC 待ち)に
# 一時停止だけ / 一時停止付きダンプ / 一時停止なしダンプ を繰り返し、固まり(FF9382 で
# MSR=D0 のまま)が出るか調べる。dumpnp は診断専用(稼働中のエミュレータと競合する)
import sys, os, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import con
HOST = os.environ.get('VMPU68_BOARD', '192.168.0.20')
mode = sys.argv[1]; dur = float(sys.argv[2]) if len(sys.argv) > 2 else 120
t0 = time.time(); acts = 0; fdc = 0; stuck = 0
def act():
    if mode == 'pause':
        con.cmd('eps 80', 0.15)
    elif mode in ('dump', 'dumpnp'):
        q = '&nopause=1' if mode == 'dumpnp' else ''
        urllib.request.urlopen(f'http://{HOST}/api/bus/dump?addr=0xC00000&words=32768{q}', timeout=10).read()
    else:
        time.sleep(0.1)
while time.time() - t0 < dur:
    r = con.cmd('erg', 0.3)
    pc = r.split()[0] if r else '?'
    if 'pc=00FF93' in pc:
        fdc += 1
        act(); acts += 1
        if 'pc=00FF9382' in pc:
            stuck += 1
        else:
            stuck = 0
        if stuck >= 12:
            msr = con.cmd('rd 0xE94001 b', 0.4)
            print(f'{time.time()-t0:6.1f}s 固まった? {msr.strip()} acts={acts}', flush=True)
            if '00D0' in msr:
                break
            stuck = 0
    else:
        stuck = 0
        time.sleep(0.2)
    if acts and acts % 20 == 0:
        print(f'{time.time()-t0:6.1f}s {pc} acts={acts}', flush=True)
print(f'done: fdc samples={fdc} acts={acts} stuck={stuck}')
