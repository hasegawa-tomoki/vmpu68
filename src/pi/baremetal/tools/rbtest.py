#!/usr/bin/env python3
# rbtest.py: 「書込み直後のテキスト VRAM 読みが直前に書いた値を返す」現象の計測(tvstress.s KIND=3)
#   引数: 組合せを "WADDR,RADDR[,MODE]" で並べる(16 進、MODE 省略 = -1)。例: C9C574,E70200,10  E00200,E70200
#   環境変数 N(既定 200000)。出力: 不一致回数 / 総回数、最後の不一致値。
import sys, os, time, struct, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import con
from x68load import load, dump
V = os.path.expanduser('~/.cache/vmpu68/tools/vasm/vasmm68k_mot')
SRCF = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../x68k/vmpu68x/tvstress.s')
N = int(os.environ.get('N', '200000'))
def build(w, r, mode):
    out = '/tmp/tvs3.bin'
    subprocess.run([V, '-Fbin', '-m68000', '-quiet', f'-DMODE={mode}', '-DKIND=3', '-DFILL=0', f'-DN={N}',
                    f'-DWADDR=${w:X}', f'-DRADDR=${r:X}', '-o', out, SRCF], check=True)
    return open(out, 'rb').read()
def run(w, r, mode):
    prog = build(w, r, mode)
    con.cmd('erun 0', 0.5); con.cmd('wr 0x1FE000 0', 0.3); con.cmd('wr 0x1FE004 0', 0.2); con.cmd('wr 0x1FE006 0', 0.2)
    if not load(0x1FF000, prog): raise SystemExit('eld の転送に失敗しました')
    con.cmd('epc 0x1FF000', 0.3); con.cmd('erun 1', 0.05)
    time.sleep(max(2.0, N / 40000.0))     # メイン RAM はライトバックなので走行中はバスから見えない: 止めてから読む
    con.cmd('erun 0', 0.5)
    d = dump(0x1FE000, 7)
    done, _, bad, tot, last = struct.unpack('>HHIIH', d[:14]) if len(d) >= 14 else (0, 0, -1, 0, 0)
    return done == 0x1234, bad, last, tot
if __name__ == '__main__':
    for a in sys.argv[1:]:
        f = a.split(','); w = int(f[0], 16); r = int(f[1], 16); mode = int(f[2]) if len(f) > 2 else -1
        ok, bad, last, tot = run(w, r, mode)
        print(f'write {w:06X} -> read {r:06X} mode {mode:3d}: {"完" if ok else "未"} 不一致 {bad} / {tot}  最後の値 {last:04X}', flush=True)
    con.cmd('erun 0', 0.5); con.cmd('ers', 1.0); con.cmd('erun 1', 0.3)
    print('終了(ers)')
