#!/usr/bin/env python3
# jprof.py <host> [start|stop|dump]: JIT 計画用の命令プロファイルを取得して集計する
#   start: 計測開始(カウンタ初期化)  stop: 停止  dump(既定): 集計表示
import sys, struct, urllib.request, collections

def get(host, path):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=20) as r:
        return r.read()

def mnemonic(op):
    """68000 オペコードの粗い分類(命令名 + 主要な形)"""
    hi = op >> 12
    if hi == 0:
        if op & 0x0100 or (op & 0xF1C0) == 0x0108: return "BTST/BCHG/BCLR/BSET(dyn)/MOVEP"
        return {0x00:"ORI",0x02:"ANDI",0x04:"SUBI",0x06:"ADDI",0x08:"BTST/BCHG/BCLR/BSET(imm)",0x0A:"EORI",0x0C:"CMPI"}.get((op>>8)&0x0E,"line0?")
    if hi in (1,2,3):
        sz = {1:"B",2:"L",3:"W"}[hi]
        if (op & 0x01C0) == 0x0040 and hi != 1: return f"MOVEA.{sz}"
        return f"MOVE.{sz}"
    if hi == 4:
        if (op & 0x0FC0) == 0x01C0: return "LEA"
        if (op & 0x0FC0) == 0x0180: return "CHK"
        if (op & 0xFFC0) == 0x4EC0: return "JMP"
        if (op & 0xFFC0) == 0x4E80: return "JSR"
        if (op & 0xFFF8) == 0x4E50: return "LINK"
        if (op & 0xFFF8) == 0x4E58: return "UNLK"
        if (op & 0xFFF0) == 0x4E60: return "MOVE USP"
        if (op & 0xFFF0) == 0x4E40: return "TRAP"
        if op == 0x4E71: return "NOP"
        if op == 0x4E75: return "RTS"
        if op == 0x4E73: return "RTE"
        if op == 0x4E77: return "RTR"
        if op == 0x4E72: return "STOP"
        if op == 0x4E70: return "RESET"
        if op == 0x4E76: return "TRAPV"
        if (op & 0xFB80) == 0x4880 and (op & 0x0038) != 0: return "MOVEM"
        if (op & 0xFFB8) == 0x4880: return "EXT"
        if (op & 0xFFF8) == 0x4840: return "SWAP"
        if (op & 0xFFC0) == 0x4840: return "PEA"
        if (op & 0xFFC0) == 0x4AC0: return "TAS"
        if (op & 0xFF00) == 0x4A00: return "TST"
        if (op & 0xFF00) == 0x4200: return "CLR"
        if (op & 0xFF00) == 0x4400: return "NEG"
        if (op & 0xFF00) == 0x4600: return "NOT"
        if (op & 0xFF00) == 0x4000: return "NEGX"
        if (op & 0xFFC0) == 0x40C0: return "MOVE from SR"
        if (op & 0xFFC0) == 0x44C0: return "MOVE to CCR"
        if (op & 0xFFC0) == 0x46C0: return "MOVE to SR"
        if (op & 0xFFC0) == 0x4800: return "NBCD"
        return "line4?"
    if hi == 5:
        if (op & 0x00F8) == 0x00C8: return "DBcc"
        if (op & 0x00C0) == 0x00C0: return "Scc"
        return "SUBQ" if op & 0x0100 else "ADDQ"
    if hi == 6:
        c = (op >> 8) & 0xF
        return {0:"BRA",1:"BSR"}.get(c, "Bcc")
    if hi == 7: return "MOVEQ"
    if hi == 8:
        if (op & 0x01C0) == 0x00C0: return "DIVU"
        if (op & 0x01C0) == 0x01C0: return "DIVS"
        if (op & 0x01F0) == 0x0100: return "SBCD"
        return "OR"
    if hi == 9:
        if (op & 0x00C0) == 0x00C0: return "SUBA"
        if (op & 0x0130) == 0x0100: return "SUBX"
        return "SUB"
    if hi == 0xB:
        if (op & 0x00C0) == 0x00C0: return "CMPA"
        if (op & 0x0138) == 0x0108: return "CMPM"
        if op & 0x0100: return "EOR"
        return "CMP"
    if hi == 0xC:
        if (op & 0x01C0) == 0x00C0: return "MULU"
        if (op & 0x01C0) == 0x01C0: return "MULS"
        if (op & 0x01F0) == 0x0100: return "ABCD"
        if (op & 0x01F8) in (0x0140, 0x0148, 0x0188): return "EXG"
        return "AND"
    if hi == 0xD:
        if (op & 0x00C0) == 0x00C0: return "ADDA"
        if (op & 0x0130) == 0x0100: return "ADDX"
        return "ADD"
    if hi == 0xE:
        kind = ["AS","LS","ROX","RO"][(op >> 3) & 3] if (op & 0x00C0) != 0x00C0 else ["AS","LS","ROX","RO"][(op >> 9) & 3]
        d = "L" if op & 0x0100 else "R"
        return f"{kind}{d}"
    if hi == 0xA: return "line A"
    if hi == 0xF: return "line F"
    return "?"

def ea_mode(op):
    m = (op >> 3) & 7; r = op & 7
    names = ["Dn","An","(An)","(An)+","-(An)","d16(An)","d8(An,Xn)"]
    if m < 7: return names[m]
    return {0:"abs.W",1:"abs.L",2:"d16(PC)",3:"d8(PC,Xn)",4:"#imm"}.get(r, "?")

def main():
    host = sys.argv[1]; cmd = sys.argv[2] if len(sys.argv) > 2 else "dump"
    if cmd in ("start", "stop"):
        import subprocess, os
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import con
        print(con.cmd("jprof 1" if cmd == "start" else "jprof 0", 1.0)); return
    hdr = get(host, "/api/jprof?part=0")
    if len(hdr) < 288: print("プロファイル未取得(jprof 1 で開始してください)"); return
    instr, blocks = struct.unpack_from("<QQ", hdr, 0)
    acc = struct.unpack_from("<4I", hdr, 16)
    blk = struct.unpack_from("<64I", hdr, 32)
    ops = []
    for part in range(1, 9):
        d = get(host, f"/api/jprof?part={part}")
        ops += list(struct.unpack(f"<{len(d)//4}I", d))
    print(f"命令数 {instr:,}  基本ブロック {blocks:,}  平均 {instr/blocks if blocks else 0:.1f} 命令/ブロック")
    print(f"読み: シャドウ {acc[0]:,} / バス {acc[1]:,} ({acc[1]*100/max(1,acc[0]+acc[1]):.1f}% がバス)   書き: シャドウ {acc[2]:,} / バス {acc[3]:,} ({acc[3]*100/max(1,acc[2]+acc[3]):.1f}% がバス)")
    print("ブロック長の分布(命令数: ブロック数):", " ".join(f"{i}:{blk[i]}" for i in range(1, 64) if blk[i]))
    by_mn = collections.Counter(); by_mn_ea = collections.Counter()
    for op, n in enumerate(ops):
        if not n: continue
        mn = mnemonic(op); by_mn[mn] += n
        if mn.startswith("MOVE.") or mn in ("ADD","SUB","CMP","AND","OR","TST","CLR","LEA","ADDQ","SUBQ","MOVEA.W","MOVEA.L"):
            by_mn_ea[(mn, ea_mode(op))] += n
    print("\n命令別(上位 30、動的頻度):")
    cum = 0
    for mn, n in by_mn.most_common(30):
        cum += n; print(f"  {mn:24s} {n:>12,} {n*100/instr:6.2f}%  累積 {cum*100/instr:6.2f}%")
    print("\n命令 × ソース側アドレッシング(上位 20):")
    for (mn, ea), n in by_mn_ea.most_common(20):
        print(f"  {mn:12s} {ea:12s} {n:>12,} {n*100/instr:6.2f}%")
    print("\nオペコード上位 15:")
    top = sorted(range(65536), key=lambda o: -ops[o])[:15]
    for o in top:
        if ops[o]: print(f"  {o:04X} {mnemonic(o):18s} {ea_mode(o):10s} {ops[o]:>12,}")

if __name__ == "__main__":
    main()
