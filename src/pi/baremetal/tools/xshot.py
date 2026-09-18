#!/usr/bin/env python3
# xshot.py [out.png] [--gonly|--tonly] [--host 192.168.0.20]
# X68000 の表示画面を合成して PNG にする(Web UI の screen.js と同じ手順の Python 版)。
# /api/screen/regs で CRTC・VC・パレットを、/api/bus/dump で GVRAM・TVRAM・スプライト
# レジスタ・BG 制御・PCG(BG マップ込み)を読み、グラフィック(512x512 16/256/65536 色、
# 1024x1024 16 色)、テキスト、スプライト/BG(8x8・16x16 タイル、PRW とスプライト番号の
# 優先順)を合成する。3 面の前後は VC R1、色 0 は透明(テキスト・スプライト・BG)。
# 再現しないもの: 半透明・特殊プライオリティ、ラスタ割込みによる途中変更。
# 読出しはバスを借りるだけで副作用はない(全部で 0.1〜0.3 秒のバス占有)。
import sys, json, struct, zlib, urllib.request, time

HOST = '192.168.0.20'; OUT = 'xscreen.png'; mode = 'both'
args = sys.argv[1:]
while args:
    a = args.pop(0)
    if a == '--gonly': mode = 'g'
    elif a == '--tonly': mode = 't'
    elif a == '--host': HOST = args.pop(0)
    else: OUT = a

nwords = 0
def get(path):
    with urllib.request.urlopen(f'http://{HOST}{path}', timeout=20) as r:
        d = r.read()
        # 基板がバスを借りられないとき(fd_busy: フロッピー転送中)はデータの代わりに JSON のエラーが返る
        if r.headers.get('Content-Type', '').startswith('application/json'):
            e = json.loads(d)
            if e.get('ok') is False: raise SystemExit(f'読み取り拒否({e.get("error")}): {e.get("message")}')
        return d
def dump(addr, n):
    global nwords
    out = b''
    while n:
        k = min(n, 32768)
        d = get(f'/api/bus/dump?addr=0x{addr:X}&words={k}')
        if len(d) != 2 * k: raise RuntimeError(f'{addr:X} の読み取りが途中で切れました({len(d)} バイト)')
        out += d; addr += 2 * k; n -= k; nwords += k
    return out
def words(b): return struct.unpack(f'>{len(b) // 2}H', b)
def rows(base, rw, total, sy, h):
    # 1 行 rw ワード・total 行の面から行 [sy, sy+h) を読む(下端で折り返す)
    sy &= total - 1; n1 = min(h, total - sy)
    d = dump(base + sy * rw * 2, n1 * rw)
    if n1 < h: d += dump(base, (h - n1) * rw)
    return words(d)
def rgb(v):
    return (((v >> 6) & 31) * 255 // 31, ((v >> 11) & 31) * 255 // 31, ((v >> 1) & 31) * 255 // 31)

t0 = time.time()
R = words(get('/api/screen/regs'))
crtc = R[:24]; vc0, vc1, vc2 = R[24], R[25], R[26]
gpal = [rgb(v) for v in R[27:283]]; spal = [rgb(v) for v in R[283:539]]; tpal = spal[:16]
r20 = crtc[20]; W = (256, 512, 768, 768)[r20 & 3]; H = 512 if r20 & 4 else 256
cmode = vc0 & 3; big = (vc0 >> 2) & 1
gr_on = mode != 't' and ((vc2 & 0x10) if big else (vc2 & 15))   # GON は 1024x1024 用、512x512 は各ページの ON
tx_on = mode != 'g' and (vc2 & 0x20)
sp_on = mode != 't' and (vc2 & 0x40) and W != 768                # 768 ドットのモードにスプライトはない
print(f'CRTC R20={r20:04X} VC R0={vc0:04X} R1={vc1:04X} R2={vc2:04X} -> {W}x{H} '
      f'{"1024x1024" if big else "512x512"} cmode={cmode}', flush=True)

N = W * H
g = [None] * N; t = [0] * N; s = [None] * N
desc = f'{W}x{H}'

if gr_on:
    if big:
        desc += ' / 1024x1024 16色'; sx = crtc[12] & 1023; d = rows(0xC00000, 1024, 1024, crtc[13], H)
        for y in range(H):
            o = y * 1024; b = y * W
            for x in range(W):
                v = d[o + ((x + sx) & 1023)] & 15
                if v: g[b + x] = gpal[v]
    elif cmode == 3:
        desc += ' / 65536色'; sx = crtc[12] & 511; d = rows(0xC00000, 512, 512, crtc[13], H)
        for y in range(H):
            o = y * 512; b = y * W
            for x in range(W): g[b + x] = rgb(d[o + ((x + sx) & 511)])
    else:
        n = 2 if cmode == 1 else 4; mask = 255 if cmode == 1 else 15
        desc += ' / 256色' if cmode == 1 else ' / 16色'
        # VC R1 bit1-0, 3-2, 5-4, 7-6 = 手前から順のページ番号
        order = []
        for i in range(4):
            p = (vc1 >> (2 * i)) & 3
            if p < n and p not in order: order.append(p)
        for p in range(n):
            if p not in order: order.append(p)
        en = [vc2 & 3, vc2 & 12] if cmode == 1 else [vc2 & 1, vc2 & 2, vc2 & 4, vc2 & 8]
        for p in order:
            if not en[p]: continue
            sx = crtc[12 + 2 * p] & 511; d = rows(0xC00000 + p * 0x80000, 512, 512, crtc[13 + 2 * p], H)
            for y in range(H):
                o = y * 512; b = y * W
                for x in range(W):
                    i = b + x
                    if g[i] is not None: continue
                    v = d[o + ((x + sx) & 511)] & mask
                    if v: g[i] = gpal[v]
    print(f'GVRAM {time.time() - t0:.1f}s', flush=True)

if tx_on:
    tsx = crtc[10] & 1023
    pl = [rows(0xE00000 + p * 0x20000, 64, 1024, crtc[11], H) for p in range(4)]
    for y in range(H):
        b = y * W; o0 = y * 64
        for x in range(W):
            rx = (x + tsx) & 1023; o = o0 + (rx >> 4); sh = 15 - (rx & 15)
            t[b + x] = ((pl[0][o] >> sh) & 1) | (((pl[1][o] >> sh) & 1) << 1) | (((pl[2][o] >> sh) & 1) << 2) | (((pl[3][o] >> sh) & 1) << 3)
    print(f'TVRAM {time.time() - t0:.1f}s', flush=True)

if sp_on:
    # スプライトレジスタ 128 個、BG 制御、PCG 32 KB(BG マップ込み)。奥から
    # PRW=1 のスプライト → BG1 → PRW=2 → BG0 → PRW=3、同じ PRW では番号の小さい方が手前
    sr = words(dump(0xEB0000, 512)); bg = words(dump(0xEB0800, 9)); pcg = words(dump(0xEB8000, 16384))
    ctl = bg[4]; t16 = (bg[8] & 3) != 0
    if ctl & 0x200:
        desc += ' + スプライト/BG'
        # 16x16 パターン n は 8x8 ブロック 4 個(左上・左下・右上・右下)、8x8 パターン n は 16x16 パターン n>>2 のブロック n&3
        def p16(n, x, y): return (pcg[n * 64 + ((x >> 3) * 2 + (y >> 3)) * 16 + (y & 7) * 2 + ((x & 7) >> 2)] >> (12 - 4 * (x & 3))) & 15
        def p8(n, x, y): return (pcg[n * 16 + y * 2 + (x >> 2)] >> (12 - 4 * (x & 3))) & 15
        def sprites(prw):
            for i in range(127, -1, -1):
                if (sr[4 * i + 3] & 3) != prw: continue
                x0 = (sr[4 * i] & 1023) - 16; y0 = (sr[4 * i + 1] & 1023) - 16; c = sr[4 * i + 2]
                n = c & 255; pb = ((c >> 8) & 15) * 16
                for py in range(16):
                    sy = y0 + py
                    if sy < 0 or sy >= H: continue
                    for px in range(16):
                        sx = x0 + px
                        if sx < 0 or sx >= W: continue
                        v = p16(n, 15 - px if c & 0x4000 else px, 15 - py if c & 0x8000 else py)
                        if v: s[sy * W + sx] = spal[pb + v]
        def bglayer(on, area, X, Y):
            if not on: return
            ts = 16 if t16 else 8; m = 64 * ts - 1; mb = 0x2000 + (area & 1) * 0x1000
            pat = p16 if t16 else p8
            for y in range(H):
                my = (y + Y) & m; ty = (my // ts) * 64; tpy = my % ts; b = y * W
                for x in range(W):
                    mx = (x + X) & m; tile = pcg[mb + ty + mx // ts]
                    tx = mx % ts; ty2 = tpy
                    if tile & 0x4000: tx = ts - 1 - tx
                    if tile & 0x8000: ty2 = ts - 1 - ty2
                    # マップの語形式は 8x8/16x16 共通: bit7-0 パターン番号、bit11-8 パレット(実機の星空で確認)
                    v = pat(tile & 255, tx, ty2)
                    if v: s[b + x] = spal[((tile >> 8) & 15) * 16 + v]
        sprites(1); bglayer(ctl & 8, ctl >> 4, bg[2] & 1023, bg[3] & 1023)
        sprites(2); bglayer(ctl & 1, ctl >> 1, bg[0] & 1023, bg[1] & 1023); sprites(3)
        print(f'SPR ctl={ctl:04X} res={bg[8]:04X} tile={16 if t16 else 8} {time.time() - t0:.1f}s', flush=True)

# VC R1 bit13-12 スプライト、11-10 テキスト、9-8 グラフィック: 値が小さい方が手前
ord_ = [L for _, L in sorted([((vc1 >> 12) & 3, 2), ((vc1 >> 10) & 3, 1), ((vc1 >> 8) & 3, 0)])]
back = gpal[0] if gr_on else (0, 0, 0)
raw = bytearray()
for y in range(H):
    raw.append(0); b = y * W
    for x in range(W):
        i = b + x; c = None
        for L in ord_:
            if L == 2:
                if s[i] is not None: c = s[i]; break
            elif L == 1:
                if t[i]: c = tpal[t[i]]; break
            elif g[i] is not None: c = g[i]; break
        raw += bytes(c if c is not None else back)
def chunk(tag, d): return struct.pack('>I', len(d)) + tag + d + struct.pack('>I', zlib.crc32(tag + d) & 0xffffffff)
png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b''))
open(OUT, 'wb').write(png)
print(f'saved {OUT}: {desc}{" + テキスト" if tx_on else ""} ({time.time() - t0:.1f}s、{nwords} ワード)')
