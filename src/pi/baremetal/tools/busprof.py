#!/usr/bin/env python3
# busprof.py [秒数] [ラベル]: 一定時間の「命令数/秒」と「バスで消えた時間の割合」を測る
#   erg の prof 行(wr/rd/st の累計回数と時間)と etr の累計命令数の差分から求める。
#   エミュレータは止めない(erg/etr は読むだけ)。import して measure(秒, ラベル) でも使える。
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import con
def snap():
    for _ in range(3):
        r = con.cmd('erg', 0.6); t = con.cmd('etr 1', 0.3); now = time.time()
        m = re.search(r'prof: wr n=(\d+) (\d+)ms \| rd n=(\d+) (\d+)ms \| st n=(\d+) (\d+)ms', r)
        i = re.search(r'\((\d+) instructions', t)
        pc = re.search(r'pc=([0-9A-F]{8})', r)
        if m and i: return now, [int(x) for x in m.groups()], int(i.group(1)), pc.group(1) if pc else '?'
    raise SystemExit('erg/etr の出力を読めませんでした')
def measure(secs, label=''):
    a = snap(); time.sleep(secs); b = snap()
    dt = b[0] - a[0]; di = b[2] - a[2]
    wr_n, wr_ms, rd_n, rd_ms, st_n, st_ms = [y - x for x, y in zip(a[1], b[1])]
    bus_ms = wr_ms + rd_ms + st_ms
    print(f'[{label}] {dt:.1f} s  pc={a[3]}→{b[3]}')
    print(f'  命令: {di:,} ({di/dt/1e6:.2f} M命令/s)')
    print(f'  バス書込み: {wr_n:,} 回 {wr_ms} ms ({wr_ms*1000/wr_n if wr_n else 0:.2f} µs/回)')
    print(f'  バス読出し: {rd_n:,} 回 {rd_ms} ms ({rd_ms*1000/rd_n if rd_n else 0:.2f} µs/回)')
    print(f'  STATUS 読み: {st_n:,} 回 {st_ms} ms')
    print(f'  バスで消えた時間: {bus_ms} ms / {dt*1000:.0f} ms = {bus_ms/dt/10:.1f}%')
    if di: print(f'  1 命令あたり: 全体 {dt*1e6/di:.3f} µs、うちバス {bus_ms*1000/di:.3f} µs、コア側 {(dt*1000-bus_ms)*1000/di:.3f} µs', flush=True)
if __name__ == '__main__':
    measure(float(sys.argv[1]) if len(sys.argv) > 1 else 10, sys.argv[2] if len(sys.argv) > 2 else '')
