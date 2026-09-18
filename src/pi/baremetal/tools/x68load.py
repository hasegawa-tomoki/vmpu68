#!/usr/bin/env python3
# x68load.py: 試験プログラムをコンソール(eld)で X68000 のメイン RAM に置き、Web API(/api/bus/dump)で読み出す共通部品
#   load(addr, bytes) / dump(addr, words) / HOST(環境変数 VMPU68_BOARD、既定 192.168.0.20)
import sys, os, time, urllib.request
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
    return urllib.request.urlopen(f'http://{HOST}/api/bus/dump?addr=0x{a:X}&words={n}', timeout=30).read()
