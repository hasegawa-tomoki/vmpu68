#!/usr/bin/env python3
# ramwrtest.py [回数]: CPU 経路の連続書込み(move.w d1,(a0)+)で TVRAM / GVRAM / メイン RAM に
# 連番を書き、実バスから読み戻して化け・欠けを数える(書込みタイミングの検査)。
# 小さなプログラムを eld で $1FF000 に置いて走らせるので、Human68k の状態は壊れる(終了後リセット)。
import sys, os, time, re, struct, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import con
from vmpu68_serial import drain
HOST = os.environ.get('VMPU68_BOARD', '192.168.0.20')
def load(addr, data):
    for _ in range(3):
        os.write(con.fd, f'eld {len(data)} 0x{addr:X}\r'.encode()); r = b''; t1 = time.time()
        while b'GO' not in r and time.time() - t1 < 2: r += drain(con.fd, 0.1)
        if b'GO' not in r: continue                      # "GO" を待ってから送る(先に送るとコマンドとして解釈される)
        os.write(con.fd, data); r = b''; t1 = time.time()
        while b'.ok' not in r and time.time() - t1 < 3: r += drain(con.fd, 0.1)
        if b'.ok' in r: return True
    return False
def dump(a, n):
    out = b''
    while n:
        k = min(n, 32768); out += urllib.request.urlopen(f'http://{HOST}/api/bus/dump?addr=0x{a:X}&words={k}', timeout=20).read(); a += 2*k; n -= k
    return struct.unpack(f'>{len(out)//2}H', out)
def fill(base_addr, start, n):   # lea base,a0; move.w #start,d1; move.l #n,d0; loop: move.w d1,(a0)+; addq.w #1,d1; subq.l #1,d0; bne loop; bra *
    return b'\x41\xF9' + base_addr.to_bytes(4, 'big') + b'\x32\x3C' + start.to_bytes(2, 'big') + b'\x20\x3C' + n.to_bytes(4, 'big') + b'\x30\xC1' + b'\x52\x41' + b'\x53\x80' + b'\x66\xF8' + b'\x60\xFE'
def run(prog, base=0x1FF000, wait=0.6):
    con.cmd('erun 0', 0.5)
    if not load(base, prog): raise SystemExit('eld の転送に失敗しました')
    con.cmd('epc 0x%X' % base, 0.3); con.cmd('erun 1', 0.05); time.sleep(wait)
rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 3
print('設定:', con.cmd('ai', 0.5).strip()[:24], '|', con.cmd('as', 0.5).strip()[:16], '|', con.cmd('epace', 0.5).strip()[:16])
total = 0
for r in range(rounds):
    for name, addr, n, mask in (('TVRAM', 0xE00000, 65536, 0xFFFF), ('GVRAM', 0xC00000, 32768, 0xF), ('RAM $100000', 0x100000, 32768, 0xFFFF)):
        start = (int(time.time() * 7) + r * 977) & 0xFFFF
        run(fill(addr, start, n)); got = dump(addr, n)
        bad = [i for i, v in enumerate(got) if (v & mask) != ((start + i) & mask)]; total += len(bad)
        print(f'{r+1}: {name:12s} {n} 語: 化け {len(bad)} 語 ' + ' '.join(f'#{i}:{(start+i)&0xFFFF:04X}->{got[i]:04X}' for i in bad[:4]), flush=True)
con.cmd('ers', 1.0); con.cmd('erun 1', 0.3)
print('合計', total, '語(リセットして Human68k に戻しました)')
