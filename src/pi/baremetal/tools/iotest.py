#!/usr/bin/env python3
# iotest.py: CRTC レジスタ(R21 $E8002A / R22 $E8002C)へ 16 ビット値を書いて読み戻し、化けを wr_setup 0〜3 で数える。
#   68000 プログラム(eld)で「書く→読む→RAM に保存」を N 回繰り返し、RAM を /api/bus/dump で回収する。
import sys, os, time, struct, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import con
from vmpu68_serial import drain
HOST = os.environ.get('VMPU68_BOARD', '192.168.0.20')
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
def prog(reg, buf, start, n, byte=False):
    # lea reg,a0; lea buf,a1; move.w #start,d1; move.l #n,d0
    p  = b'\x41\xF9' + reg.to_bytes(4,'big') + b'\x43\xF9' + buf.to_bytes(4,'big') + b'\x32\x3C' + start.to_bytes(2,'big') + b'\x20\x3C' + n.to_bytes(4,'big')
    if byte:  # move.b d1,(a0) ; move.b 1(a0)... 下位バイトのみ書く: move.b d1,1(a0); move.w (a0),(a1)+
        body = b'\x11\x41\x00\x01' + b'\x32\xD0' + b'\x52\x41\x53\x80' + b'\x66\xF4'
    else:     # move.w d1,(a0); move.w (a0),(a1)+; addq.w #1,d1 ; ... (値は 3 ずつ進める)
        body = b'\x30\x81' + b'\x32\xD0' + b'\x52\x41\x52\x41\x52\x41' + b'\x53\x80' + b'\x66\xF2'
    return p + body + b'\x60\xFE'
def run(p, base=0x1FF000, wait=0.4):
    con.cmd('erun 0', 0.5)
    if not load(base, p): raise SystemExit('eld の転送に失敗しました')
    con.cmd('epc 0x%X' % base, 0.3); con.cmd('erun 1', 0.05); time.sleep(wait)
BUF = 0x1F8000; N = 2048
for ws in (0, 1, 2, 3):
    con.cmd(f'wsetup {ws}', 0.3); res = []
    for name, reg, mask, byte in (('R21', 0xE8002A, 0xFFFF, False), ('R22', 0xE8002C, 0xFFFF, False), ('R21.b', 0xE8002A, 0x00FF, True)):
        start = (int(time.time()*7) & 0x7FF) | 1
        run(prog(reg, BUF, start, N, byte)); got = dump(BUF, N)
        bad = 0; ex = []
        for i in range(N):
            exp = ((start + (i if byte else 3*i)) & 0xFFFF) & mask
            if (got[i] & mask) != exp: bad += 1; ex.append(f'#{i}:{exp:04X}->{got[i]&mask:04X}')
        res.append(f'{name}:{bad:4d}' + (' [' + ' '.join(ex[:3]) + ']' if bad else ''))
    print(f'wr_setup {ws}:  ' + '   '.join(res), flush=True)
con.cmd('wsetup 2', 0.3); con.cmd('ers', 1.0); con.cmd('erun 1', 0.3)
print('終了(ers で Human68k を再起動)')
