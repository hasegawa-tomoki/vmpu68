#!/usr/bin/env python3
# scrtest.py [設定名]: X68000 で B:\SCRTEST.R(400 行の文字→40 行の空行でスクロール)を走らせ、
#   表示領域(0〜495 行)に残ったドットと、ファンクションキー行(496〜511 行)の基準との差を数える。
import sys, os, time, urllib.request, urllib.parse, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
HOST = os.environ.get('VMPU68_BOARD', '192.168.0.20')
def get(path):
    with urllib.request.urlopen(f'http://{HOST}{path}', timeout=30) as r: return r.read()
def key(t): get('/api/key?text=' + urllib.parse.quote(t, safe='') + '&enter=1')
def regs():
    d = get('/api/screen/regs'); w = struct.unpack(f'>{len(d)//2}H', d); return w
def tvram_rows(plane, y0, n):        # 128 バイト/行、1024 行のリング
    out = b''
    for y in range(y0, y0 + n):
        yy = y & 1023
        if not out or yy == 0 or (y - y0) % 256 == 0:
            pass
    # まとめて読む(2 分割で折り返し対応)
    a = 0xE00000 + plane * 0x20000
    y0 &= 1023
    if y0 + n <= 1024:
        return get(f'/api/bus/dump?addr=0x{a + y0*128:X}&words={n*64}')
    n1 = 1024 - y0
    return get(f'/api/bus/dump?addr=0x{a + y0*128:X}&words={n1*64}') + get(f'/api/bus/dump?addr=0x{a:X}&words={(n-n1)*64}')
def snapshot():
    r = regs(); scroll = r[11] & 1023          # CRTC R11: テキスト Y スクロール
    vis = [tvram_rows(p, scroll, 496) for p in range(2)]
    fk = [tvram_rows(p, scroll + 496, 16) for p in range(2)]
    return vis, fk
def bits(b): return sum(bin(x).count('1') for x in b)
label = sys.argv[1] if len(sys.argv) > 1 else ''
key('CLS'); time.sleep(0.8)
_, fk_ref = snapshot()
key('B:\\SCRTEST.R'); time.sleep(20)
for _ in range(3): key('DIR A:\\SYS'); time.sleep(4)
for _ in range(2): key('DIR A:\\'); time.sleep(4)
key('CLS'); time.sleep(1)
for _ in range(3): key('DIR B:\\TOOL'); time.sleep(4)
for _ in range(45): key(''); time.sleep(0.15)
time.sleep(2)
vis, fk = snapshot()
dots = sum(bits(v) for v in vis)
diff = sum(bits(bytes(a ^ b for a, b in zip(x, y))) for x, y in zip(fk, fk_ref))
print(f'{label}: 表示領域の残りドット {dots}、ファンクションキー行の差分ビット {diff}', flush=True)
