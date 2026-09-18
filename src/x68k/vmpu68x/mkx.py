#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# mkx.py <in.bin> <out.x> [reloc-offset ...]
#   vasm の -Fbin 出力(ベースアドレス 0)を Human68k の X 形式実行ファイルに包む。
#   reloc-offset は、ロード先アドレスを足す必要がある 32 ビット値の位置(バイトオフセット)。
#   デバイスドライバではデバイスヘッダのストラテジ/割り込みルーチンのアドレス(+6, +10)がそれ。
#   Human68k の DEVICE= は X 形式のドライバを再配置してから組み込む(R 形式のヘッダは再配置されない)。
import struct, sys

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    relocs = sorted(int(a, 0) for a in sys.argv[3:])
    body = bytearray(open(src, 'rb').read())
    if len(body) & 1:
        body.append(0)
    # 再配置表: 前の再配置位置からの差分(16 ビット)。差分が 65535 を超えるときは 1 + 32 ビット差分。
    tbl = bytearray()
    prev = 0
    for off in relocs:
        d = off - prev
        if d > 0xFFFF:
            tbl += struct.pack('>HI', 1, d)
        else:
            tbl += struct.pack('>H', d)
        prev = off
    hdr = bytearray(64)
    struct.pack_into('>2sBBIIIIIIIIII', hdr, 0, b'HU', 0, 0,
                     0,            # ベースアドレス
                     0,            # 実行開始アドレス(ドライバでは未使用)
                     len(body),    # text
                     0,            # data
                     0,            # bss
                     len(tbl),     # 再配置表
                     0, 0, 0, 0)   # シンボル、SCD 行番号、SCD シンボル、SCD 文字列
    open(dst, 'wb').write(hdr + body + tbl)
    print(f'{dst}: text {len(body)} bytes, {len(relocs)} relocation(s)')

if __name__ == '__main__':
    main()
