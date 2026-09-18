#!/usr/bin/env python3
# scrdiag.py: 1 回ずつスクロールさせ、ファンクションキー行(表示 496〜511 行)が変化したら、その変化を面/列/行で報告する
import sys, os, time, urllib.request, urllib.parse, struct
HOST = os.environ.get('VMPU68_BOARD', '192.168.0.20')
def get(path):
    with urllib.request.urlopen(f'http://{HOST}{path}', timeout=30) as r: return r.read()
def key(t): get('/api/key?text=' + urllib.parse.quote(t, safe='') + '&enter=1')
def regs(): d = get('/api/screen/regs'); return struct.unpack(f'>{len(d)//2}H', d)
def rows(plane, y0, n):
    a = 0xE00000 + plane * 0x20000; y0 &= 1023
    if y0 + n <= 1024: return get(f'/api/bus/dump?addr=0x{a + y0*128:X}&words={n*64}')
    n1 = 1024 - y0
    return get(f'/api/bus/dump?addr=0x{a + y0*128:X}&words={n1*64}') + get(f'/api/bus/dump?addr=0x{a:X}&words={(n-n1)*64}')
def fkrows():
    sc = regs()[11] & 1023
    return sc, [rows(p, sc + 496, 16) for p in range(2)]
key('CLS'); time.sleep(0.5); key('DIR A:\\SYS'); time.sleep(4)
sc, ref = fkrows(); print('基準 scroll', sc)
for n in range(int(sys.argv[1]) if len(sys.argv) > 1 else 80):
    key(''); time.sleep(0.25)
    sc, cur = fkrows()
    changed = []
    for p in range(2):
        for r in range(16):
            a = ref[p][r*128:(r+1)*128]; b = cur[p][r*128:(r+1)*128]
            if a != b:
                cols = [i for i in range(128) if a[i] != b[i]]
                changed.append((p, r, cols[0], cols[-1], len(cols)))
    if changed:
        print(f'{n+1} 回目のスクロールで変化 (scroll={sc}):')
        for p, r, c0, c1, k in changed: print(f'   面{p} 行{r}: バイト {c0}〜{c1} ({k} バイト)')
        # 参考: 直前の表示行(表示 480〜495 行 = 30 行目)の面 0 の内容の有無
        above = rows(0, sc + 480, 16); print('   30 行目(面0)の非零バイト:', sum(1 for x in above if x))
        break
else:
    print('変化なし')
