#!/usr/bin/env python3
# con.py: 基板のコンソール(J3 USB シリアル)にコマンドを送って応答を返す小道具
#   import con; print(con.cmd('erg'))
# シリアルデバイスは環境変数 VMPU68_TTY で変更できる(既定 /dev/cu.usbserial-10)
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vmpu68_serial import open_console, drain
fd = open_console(os.environ.get('VMPU68_TTY', '/dev/cu.usbserial-10'))
def cmd(c, wait=0.6):
    os.write(fd, c.encode() + b"\r")
    r = drain(fd, wait).decode(errors='replace')
    return '\n'.join(l.strip() for l in r.splitlines() if l.strip() and 'httpd:' not in l and l.strip() != c)
if __name__ == '__main__':
    print(cmd(' '.join(sys.argv[1:]) or 'st', 1.0))
