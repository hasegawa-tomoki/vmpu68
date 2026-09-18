#!/usr/bin/env python3
# hangtest.py <秒数>: 画面更新(Web UI と同じ /api/bus/dump)を繰り返しながら
# FDC 待ちループでの固まりを検出する
import sys, os, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import con
HOST = os.environ.get('VMPU68_BOARD', '192.168.0.20')
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 90
t0 = time.time(); n = 0; stuck = 0
while time.time() - t0 < dur:
    for a in (0xC00000, 0xC80000, 0xE00000):
        try:
            urllib.request.urlopen(f'http://{HOST}/api/bus/dump?addr=0x{a:X}&words=32768', timeout=10).read()
        except Exception as e:
            print('dump error', e)
    n += 1
    r = con.cmd('erg', 0.6)
    pc = r.split()[0] if r else '?'
    print(f'{time.time()-t0:6.1f}s refresh#{n} {pc}', flush=True)
    if 'pc=00FF93' in pc:
        stuck += 1
        if stuck >= 3:
            # FDC が結果フェーズ(MSR=$D0)のまま IOCS の待ちに居座っているときだけ固まりと判定
            # (長い FD 読込み中は MSR=$80 で FF93xx に居るのが正常)
            msr = con.cmd('rd 0xE94001 b', 0.5)
            if '00D0' not in msr:
                stuck = 0
            else:
                print('固まった?'); print(msr); print(con.cmd('rd 0xE84000 b', 0.5)); print(con.cmd('rd 0xE8400A', 0.5))
                print(con.cmd('eiq 6', 1.0)); break
    else:
        stuck = 0
    time.sleep(1.0)
