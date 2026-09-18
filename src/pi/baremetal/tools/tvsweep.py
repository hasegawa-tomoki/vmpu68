#!/usr/bin/env python3
# tvsweep.py: バス設定を変えながら tvstress.s を走らせ、テキスト VRAM 4 面全体(512 KB)の「試験が書いた行以外」の
# 変化を数える(docs §30/§32 の CRTC の意図しない書込みの計測)。fksweep.py の 256 ライン対応版。
#   環境変数: MODE(既定 3 = 256x256 15 kHz、-1 = 変えない)、LINE(既定 100)、REPS(既定 2)、WAIT(秒、既定 8)、N
#   引数: 設定の組(コンソールコマンドをカンマ区切り)。例: "btim 0" "btim 3,slow 1"。無ければ base のみ。
#   出力: 設定ごとに 面別の変化バイト数と、変化した行の範囲。
import sys, os, time, struct, subprocess, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import con
from x68load import load, dump, HOST
V = os.path.expanduser('~/.cache/vmpu68/tools/vasm/vasmm68k_mot')
SRCF = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../x68k/vmpu68x/tvstress.s')
MODE = int(os.environ.get('MODE', '3')); LINE = int(os.environ.get('LINE', '100'))
KIND = int(os.environ.get('KIND', '0'))
R20 = os.environ.get('R20'); VC0 = os.environ.get('VC0'); IRQON = os.environ.get('IRQON'); ADPCM = os.environ.get('ADPCM')
REPS = int(os.environ.get('REPS', '2')); WAIT = float(os.environ.get('WAIT', '8')); N = int(os.environ.get('N', '400000'))
def build():
    out = '/tmp/tvstress.bin'
    args = [V, '-Fbin', '-m68000', '-quiet', f'-DMODE={MODE}', f'-DLINE={LINE}', f'-DN={N}', f'-DKIND={KIND}']
    if R20: args.append(f'-DR20=${int(R20, 16):X}')
    if VC0: args.append(f'-DVC0={int(VC0)}')
    if IRQON: args.append('-DIRQON=1')
    if ADPCM: args.append('-DADPCM=1')
    subprocess.run(args + ['-o', out, SRCF], check=True)
    return open(out, 'rb').read()
def tvram():
    return b''.join(dump(0xE00000 + i * 0x10000, 32768) for i in range(8))   # 4 面 × 128 KB
def diff(ref, cur):
    """面ごとの (変化バイト数, 変化行の集合)。試験が書く行 LINE〜LINE+15 は除く"""
    res = []
    for p in range(4):
        a = ref[p * 0x20000:(p + 1) * 0x20000]; b = cur[p * 0x20000:(p + 1) * 0x20000]
        rows = {}
        for i in range(len(a)):
            if a[i] != b[i]:
                r = i // 128
                if KIND == 0 and LINE <= r < LINE + 16: continue
                if KIND == 1 and 104 <= r < 112: continue
                rows[r] = rows.get(r, 0) + 1
        res.append((sum(rows.values()), rows))
    return res
def run_once(prog):
    con.cmd('erun 0', 0.5); con.cmd('wr 0x1FE000 0', 0.3)
    if not load(0x1FF000, prog): raise SystemExit('eld の転送に失敗しました')
    con.cmd('epc 0x1FF000', 0.3); con.cmd('erun 1', 0.05)
    # FILL が終わるまで少し待ってから基準を取る(埋めるのに約 1 秒)
    time.sleep(2.0)
    con.cmd('erun 0', 0.5); ref = tvram(); con.cmd('erun 1', 0.05)
    time.sleep(WAIT); con.cmd('erun 0', 0.5)
    done = struct.unpack('>H', dump(0x1FE000, 1))[0] == 0x1234
    cur = tvram()
    if KIND == 1:   # 転送結果の照合: 面 p 行 104+r 列 c == 面 3 の帯 c(行 512 から 32 バイトずつ)の r*4+p バイト目
        bad = 0; ex = []
        for c in range(32):
            src = cur[3 * 0x20000 + 0x10000 + c * 32: 3 * 0x20000 + 0x10000 + c * 32 + 32]
            for r in range(8):
                for p in range(4):
                    v = cur[p * 0x20000 + (104 + r) * 128 + c]
                    if v != src[r * 4 + p]:
                        bad += 1
                        if len(ex) < 6: ex.append(f'面{p}行{104+r}列{c}:{v:02X}!={src[r*4+p]:02X}')
        print(f'    転送結果の不一致 {bad} バイト ' + ' '.join(ex), flush=True)
    return done, diff(ref, cur)
if __name__ == '__main__':
    cfgs = [a.split(',') for a in sys.argv[1:]] or [['base']]
    prog = build()
    for cfg in cfgs:
        con.cmd('erun 0', 0.5)
        for c in cfg:
            if c != 'base': print('  >', c, con.cmd(c, 0.3).strip()[:60], flush=True)
        tot = 0; parts = []
        for rep in range(REPS):
            done, d = run_once(prog)
            n = sum(x[0] for x in d); tot += n
            rows = sorted(set().union(*[set(x[1]) for x in d]))
            parts.append(f'{"完" if done else "未"}{n:4d} [' + ' '.join(str(x[0]) for x in d) + ']' + (f' 行{rows[0]}-{rows[-1]}' if rows else ''))
        print(f'{",".join(cfg):16s}: 計 {tot:5d} バイト  ' + ' | '.join(parts), flush=True)
    con.cmd('erun 0', 0.5); con.cmd('ers', 1.0); con.cmd('erun 1', 0.3)
    print('終了(ers)')
