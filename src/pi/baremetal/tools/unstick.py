#!/usr/bin/env python3
# unstick.py: FDC の結果フェーズに残ったバイトを読み捨てて IOCS の待ちループを抜けさせる
# (MSR=$D0 のまま FF9382/FF93C4 で固まったときの復帰用。その後 IOCS はエラーを返す)
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import con
for i in range(7):
    msr = con.cmd('rd 0xE94001 b', 0.3)
    if '00D0' not in msr: break
    print(con.cmd('rd 0xE94003 b', 0.3).strip())
time.sleep(1.5)
print(con.cmd('erg', 0.8).split('\n')[0])
