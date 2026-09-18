#!/usr/bin/env python3
# rastest.py: rastest.s を条件ごとにアセンブルして eld で走らせ、転送先ラスタ(行 640〜767)を照合する
import sys, os, time, struct, subprocess, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import con
from vmpu68_serial import drain
HOST = os.environ.get('VMPU68_BOARD', '192.168.0.20')
V = os.path.expanduser('~/.cache/vmpu68/tools/vasm/vasmm68k_mot'); SRCF = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../x68k/vmpu68x/rastest.s')
def load(addr, data):
    for _ in range(3):
        os.write(con.fd, f'eld {len(data)} 0x{addr:X}\r'.encode()); r = b''; t1 = time.time()
        while b'GO' not in r and time.time() - t1 < 2: r += drain(con.fd, 0.1)
        if b'GO' not in r: continue
        os.write(con.fd, data); r = b''; t1 = time.time()
        while b'.ok' not in r and time.time() - t1 < 3: r += drain(con.fd, 0.1)
        if b'.ok' in r: return True
    return False
def dump(a, n):
    out = urllib.request.urlopen(f'http://{HOST}/api/bus/dump?addr=0x{a:X}&words={n}', timeout=20).read()
    return struct.unpack(f'>{len(out)//2}H', out)
def build(delay, edge):
    out = '/tmp/rastest.bin'
    subprocess.run([V, '-Fbin', '-m68000', '-quiet', f'-DDELAY={delay}', f'-DEDGE={edge}', '-o', out, SRCF], check=True)
    return open(out, 'rb').read()
def run(p, base=0x1FF000, wait=0.4):
    con.cmd('erun 0', 0.5)
    if not load(base, p): raise SystemExit('eld の転送に失敗しました')
    con.cmd('epc 0x%X' % base, 0.3); con.cmd('erun 1', 0.05); time.sleep(wait)
SRC, DST, N = 200, 160, 32
def check():
    done = dump(0x1FE000, 1)[0]
    p0 = dump(0xE00000 + DST*4*128, N*4*64); p1 = dump(0xE20000 + DST*4*128, N*4*64)
    badrows = []
    for r in range(N*4):
        srow = SRC*4 + r; exp0 = ((srow & 0xFF) << 8) | (srow & 0xFF); exp1 = (~exp0) & 0xFFFF
        w0 = p0[r*64:(r+1)*64]; w1 = p1[r*64:(r+1)*64]
        if any(w != exp0 for w in w0) or any(w != exp1 for w in w1):
            got = w0[0]; badrows.append(f'{r}:{(got>>8):02X}')
    return done, badrows
cases = [(0,0),(0,1),(5,0),(20,0),(50,0),(100,0),(200,0),(20,1),(100,1)]
if len(sys.argv) > 1: cases = [tuple(map(int, c.split(','))) for c in sys.argv[1:]]
for delay, edge in cases:
    res = []
    for rep in range(3):
        run(build(delay, edge)); done, bad = check()
        res.append(f'{"ok" if done == 0x1234 else "未完"} 化け行 {len(bad):3d}' + (' [' + ' '.join(bad[:6]) + ']' if bad else ''))
    print(f'DELAY={delay:3d} EDGE={edge}:  ' + ' | '.join(res), flush=True)
con.cmd('ers', 1.0); con.cmd('erun 1', 0.3)
print('終了(ers で Human68k を再起動)')
