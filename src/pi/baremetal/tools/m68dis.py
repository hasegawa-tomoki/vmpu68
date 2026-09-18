#!/usr/bin/env python3
# m68dis.py <16進アドレス> [バイト数]: /api/bus/dump で読んだメモリを capstone で逆アセンブル
# (副作用なし。`pip install capstone` が必要)
import sys, os, urllib.request
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000
HOST = os.environ.get('VMPU68_BOARD', '192.168.0.20')
base = int(sys.argv[1], 16); n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 128
code = urllib.request.urlopen(f'http://{HOST}/api/bus/dump?addr=0x{base:X}&words={(n+1)//2}', timeout=20).read()
for i in Cs(CS_ARCH_M68K, CS_MODE_M68K_000).disasm(code, base):
    print(f'{i.address:06X} {i.bytes.hex():16s} {i.mnemonic:8s} {i.op_str}')
