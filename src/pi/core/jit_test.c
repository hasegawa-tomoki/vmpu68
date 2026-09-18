/* SPDX-License-Identifier: MIT
 * jit_test.c — JIT の差分テスト(Linux/qemu-aarch64、モック HW)。
 * ランダムな 68000 命令列を「解釈実行」と「JIT」で同じ初期状態から走らせ、レジスタ・フラグ・RAM を照合する。
 *   jit_test [回数] [乱数種]   終了コード 0 = 全一致 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu68k.h"
#include "jit.h"
#include "../mgmtd/hw.h"
#include "m68kcpu.h"
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
extern unsigned int m68k_read_memory_8(unsigned int);

#define RAM      0x10000u
#define CODE0    0x0400u
#define SUB_ADDR 0x0F00u             /* RTS だけのサブルーチン */
#define CRASH_ADDR 0x0F80u           /* 全例外ベクタの行き先(BRA *) */
#define STACK    0xE000u

static uint32_t rnd_state = 12345;
static uint32_t rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5; return rnd_state; }
static unsigned r(unsigned n) { return rnd() % n; }

int hw_port_lock_held(void);
static uint8_t prog[4096]; static unsigned plen;
static void w16(unsigned v) { prog[plen++] = (uint8_t)(v >> 8); prog[plen++] = (uint8_t)v; }
static void w32(uint32_t v) { w16(v >> 16); w16(v & 0xFFFF); }

/* EA を作る: mode, reg と拡張語。addr_ok: メモリ先 OK、sz */
static unsigned gen_ea(unsigned sz, int allow_imm, int allow_pc, int allow_an, unsigned *mode, unsigned *reg)
{
    for (;;) {
        unsigned m = r(8), rg = r(8);
        if (m == 1 && (!allow_an || sz == 0)) continue;
        if (m == 7) {
            rg = r(5);
            if (rg == 4 && !allow_imm) continue;
            if ((rg == 2 || rg == 3) && !allow_pc) continue;
        }
        if ((m == 3 || m == 4) && rg == 7) continue;   /* A7 のインクリメント/デクリメントは避ける(スタック) */
        if (m >= 2 && m <= 6 && rg == 7) continue;
        *mode = m; *reg = rg; return (m << 3) | rg;
    }
}
static void gen_ea_ext(unsigned mode, unsigned reg, unsigned sz)
{
    switch (mode) {
    case 5: w16((unsigned)(int16_t)(r(512) - 256)); break;
    case 6: w16(((r(4)) << 12) | (r(2) << 11) | ((unsigned)(int8_t)(r(128) - 64) & 0xFF)); break;   /* Xn = D0-D3、W/L */
    case 7:
        if (reg == 0) w16(0x1000 + r(0x6000));
        else if (reg == 1) w32(r(8) ? 0x1000 + r(0xC000) : 0x10000 + r(0x1000));   /* 1/8 は RAM 外(バスエラー → 例外経路の比較) */
        else if (reg == 2) w16((unsigned)(int16_t)(r(64) + 2));            /* d16(PC): 前方の少し先を読む */
        else if (reg == 3) w16(((r(4)) << 12) | (r(2) << 11) | (r(32) + 2));
        else { if (sz == 2) w32(rnd()); else w16(sz == 0 ? r(256) : r(65536)); }
        break;
    }
}
/* 1 命令を生成 */
static void gen_instr(void)
{
    unsigned k = r(24), sz, m, rg, m2, rg2;
    switch (k) {
    case 0: case 1: case 2: case 3: {                     /* MOVE / MOVEA */
        sz = r(3);
        unsigned ea = gen_ea(sz, 1, 1, 1, &m, &rg);
        unsigned dea; do { dea = gen_ea(sz, 0, 0, sz != 0, &m2, &rg2); } while (m2 == 1 && rg2 == 7);   /* MOVEA to A7 は作らない */
        unsigned szbits = sz == 0 ? 0x1000 : sz == 1 ? 0x3000 : 0x2000;
        w16(szbits | (rg2 << 9) | (m2 << 6) | ea);
        gen_ea_ext(m, rg, sz); gen_ea_ext(m2, rg2, sz);
        break; }
    case 4: w16(0x7000 | (r(8) << 9) | r(256)); break;    /* MOVEQ */
    case 5: {                                             /* ADD/SUB/AND/OR/CMP Dn,<ea> or <ea>,Dn */
        static const unsigned lines[] = { 0xD000, 0x9000, 0xC000, 0x8000, 0xB000 };
        unsigned line = lines[r(5)]; sz = r(3);
        int toea = r(2) && line != 0xB000;
        if (toea) { unsigned ea = gen_ea(sz, 0, 0, 0, &m, &rg); if (m < 2) { ea = gen_ea(sz, 0, 0, 0, &m, &rg); } if (m < 2) { w16(0x4E71); break; }
                    w16(line | (r(8) << 9) | ((4 + sz) << 6) | ea); gen_ea_ext(m, rg, sz); }
        else { unsigned ea = gen_ea(sz, 1, 1, (line == 0xD000 || line == 0x9000 || line == 0xB000) && sz != 0, &m, &rg);
               w16(line | (r(8) << 9) | (sz << 6) | ea); gen_ea_ext(m, rg, sz); }
        break; }
    case 6: {                                             /* EOR Dn,<ea> */
        sz = r(3); unsigned ea = gen_ea(sz, 0, 0, 0, &m, &rg); if (m == 1) { w16(0x4E71); break; }
        w16(0xB100 | (r(8) << 9) | (sz << 6) | ea); gen_ea_ext(m, rg, sz); break; }
    case 7: {                                             /* ADDI/SUBI/CMPI/ANDI/ORI/EORI */
        static const unsigned k2[] = { 0x0600, 0x0400, 0x0C00, 0x0200, 0x0000, 0x0A00 };
        unsigned kk = k2[r(6)]; sz = r(3);
        unsigned ea = gen_ea(sz, 0, kk == 0x0C00, 0, &m, &rg); if (m == 1) { w16(0x4E71); break; }
        w16(kk | (sz << 6) | ea);
        if (sz == 2) w32(rnd()); else w16(sz == 0 ? r(256) : r(65536));
        gen_ea_ext(m, rg, sz); break; }
    case 8: {                                             /* ADDQ/SUBQ */
        sz = r(3); unsigned ea = gen_ea(sz, 0, 0, sz != 0, &m, &rg);
        w16(0x5000 | (r(8) << 9) | (r(2) << 8) | (sz << 6) | ea); gen_ea_ext(m, rg, sz); break; }
    case 9: {                                             /* CLR/TST/NOT/NEG */
        static const unsigned k3[] = { 0x4200, 0x4A00, 0x4600, 0x4400 };
        unsigned kk = k3[r(4)]; sz = r(3);
        unsigned ea = gen_ea(sz, 0, kk == 0x4A00, 0, &m, &rg); if (m == 1) { w16(0x4E71); break; }
        w16(kk | (sz << 6) | ea); gen_ea_ext(m, rg, sz); break; }
    case 10: w16(0x4880 | (r(2) << 6) | r(8)); break;    /* EXT */
    case 11: w16(0x4840 | r(8)); break;                  /* SWAP */
    case 12: {                                            /* LEA */
        unsigned ea; do { ea = gen_ea(2, 0, 1, 0, &m, &rg); } while (m < 2 || m == 3 || m == 4);
        w16(0x41C0 | (r(7) << 9) | ea); gen_ea_ext(m, rg, 2); break; }
    case 13: {                                            /* shifts on Dn */
        sz = r(3); unsigned cnt = r(8), reg = r(8);
        w16(0xE000 | (cnt << 9) | (r(2) << 8) | (sz << 6) | (r(2) << 5) | (r(4) << 3) | reg); break; }
    case 14: {                                            /* BTST/BCHG/BCLR/BSET */
        unsigned kind = r(4);
        unsigned ea = gen_ea(0, 0, kind == 0, 0, &m, &rg); if (m == 1) { w16(0x4E71); break; }
        if (r(2)) { w16(0x0800 | (kind << 6) | ea); w16(r(256)); }
        else w16(0x0100 | (r(8) << 9) | (kind << 6) | ea);
        gen_ea_ext(m, rg, 0); break; }
    case 15: {                                            /* LINK/UNLK ペア(A5) */
        w16(0x4E55); w16((unsigned)(int16_t)(-(int)(r(32) * 2))); w16(0x4E5D); break; }
    case 16: {                                            /* MOVEM -(A7) / (A7)+ ペア */
        unsigned mask = r(65536) & 0x7FFE; unsigned l = r(2);   /* -(A7) では bit0 = A7: A7 自身は積まない */
        w16(0x48A0 | (l << 6) | 7); w16(mask);
        unsigned rmask = 0; for (int i = 0; i < 16; i++) if (mask & (1u << i)) rmask |= 1u << (15 - i);
        w16(0x4C98 | (l << 6) | 7); w16(rmask); break; }
    case 17: {                                            /* MOVEM d16(An) 往復 */
        unsigned mask = r(65536) & 0x7F7F; unsigned l = r(2), an = r(6);
        w16(0x48A8 | (l << 6) | an); w16(mask); w16(0x0100);
        w16(0x4CA8 | (l << 6) | an); w16(mask); w16(0x0100); break; }
    case 18: w16(0x4EB9); w32(SUB_ADDR); break;          /* JSR abs.L → RTS */
    case 19: { int16_t d = (int16_t)(SUB_ADDR - (CODE0 + plen + 2)); w16(0x6100); w16((unsigned)d & 0xFFFF); break; }   /* BSR.W */
    case 20: {                                            /* Bcc.S 次の 1 命令(NOP)を飛ばす */
        w16(0x6000 | (r(16) << 8) | 2); w16(0x4E71); break; }
    case 21: {                                            /* DBcc ループ: D6 を小さく初期化してループ */
        w16(0x7C00 | r(6));                              /* MOVEQ #n,D6 */
        w16(0x5240);                                     /* ADDQ.W #1,D0 (ループ本体) */
        w16(0x50CE | (r(16) << 8)); w16(0xFFFC);         /* DBcc D6,-4 */
        break; }
    case 22: { unsigned ea; do { ea = gen_ea(2, 0, 1, 0, &m, &rg); } while (m < 2 || m == 3 || m == 4);   /* PEA + ADDQ #4,A7 */
        w16(0x4840 | ea); gen_ea_ext(m, rg, 2); w16(0x588F); break; }
    case 23: w16(0x4E71); break;
    }
}

static uint8_t cur_prog[4096]; static unsigned cur_plen;
static void segv(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = (ucontext_t *)uc_;
    unsigned long pc = uc->uc_mcontext.pc;
    char buf[256]; int inblk = jit_debug_locate((uintptr_t)pc, buf, sizeof buf);
    printf("\nSIGNAL %d at host pc %lx fault addr %p: %s\n", sig, pc, si->si_addr, buf); fflush(stdout);
    printf("x0=%llx x1=%llx x2=%llx x16=%llx x19=%llx x24=%llx x25=%llx x26=%llx x27=%llx sp=%llx lr=%llx\n",
        (unsigned long long)uc->uc_mcontext.regs[0], (unsigned long long)uc->uc_mcontext.regs[1], (unsigned long long)uc->uc_mcontext.regs[2],
        (unsigned long long)uc->uc_mcontext.regs[16], (unsigned long long)uc->uc_mcontext.regs[19], (unsigned long long)uc->uc_mcontext.regs[24],
        (unsigned long long)uc->uc_mcontext.regs[25], (unsigned long long)uc->uc_mcontext.regs[26], (unsigned long long)uc->uc_mcontext.regs[27],
        (unsigned long long)uc->uc_mcontext.sp, (unsigned long long)uc->uc_mcontext.regs[30]);
    printf("&m68ki_cpu=%p 68k pc=%06X\n", (void *)&m68ki_cpu, m68ki_cpu.pc); fflush(stdout);
    jit_debug_hist(buf, sizeof buf); printf("recent blocks: %s\n", buf);
    { unsigned long long fp = uc->uc_mcontext.regs[29]; printf("frames:"); for (int i = 0; i < 24 && fp && fp > 0x10000 && (fp & 7) == 0; i++) { unsigned long long *f = (unsigned long long *)fp; printf(" %llx", f[1]); fp = f[0]; } printf("\n"); fflush(stdout); }
    printf("hw lock held: %d\n", hw_port_lock_held());
    printf("prog:"); for (unsigned i = 0; i < cur_plen; i++) printf("%s%02X", (i % 2) ? "" : " ", cur_prog[i]); printf("\n");
    fflush(stdout);
    if (inblk) { uint32_t *w = (uint32_t *)(pc & ~3ul); printf("code around:"); for (int i = -4; i < 4; i++) printf(" %08x", w[i]); printf("\n"); }
    fflush(stdout); _exit(2);
}
static m68ki_cpu_core snap_cpu;
static uint8_t ram_snap[RAM], ram_a[RAM], ram_b[RAM];
static uint8_t io_snap[0x1000], io_a[0x1000], io_b[0x1000];   /* モック I/O RAM($E80000-)も両段で同じ初期状態にして比較する */
static void ram_read(uint8_t *dst) { for (unsigned i = 0; i < RAM; i++) dst[i] = (uint8_t)m68k_read_memory_8(i); }
static inline int is_crash(uint32_t pc) { return pc >= CRASH_ADDR && pc < CRASH_ADDR + 512; }
static void run_until(uint32_t end, int *ok)
{
    uint64_t cyc = 0;
    for (;;) {
        uint32_t pc = m68ki_cpu.pc & 0xFFFFFF;
        if (pc == end || is_crash(pc) || cyc >= 60000) break;
        int n = emu68k_run(200); if (n <= 0) break; cyc += (uint64_t)n;
        pc = m68ki_cpu.pc & 0xFFFFFF;
        if (!((pc >= CODE0 && pc <= end) || pc == SUB_ADDR || pc == SUB_ADDR + 2 || is_crash(pc))) break;   /* 暴走: 打ち切り */
    }
    *ok = (m68ki_cpu.pc & 0xFFFFFF) == end ? 1 : is_crash(m68ki_cpu.pc & 0xFFFFFF) ? 2 : 0;   /* 2 = 例外で終了(それも比較する) */
}
static int cmp_state(const char *tag, m68ki_cpu_core *a, m68ki_cpu_core *b, uint8_t *ra, uint8_t *rb)
{
    int bad = 0;
    for (int i = 0; i < 16; i++) if (a->dar[i] != b->dar[i]) { printf("  %s: %c%d %08X vs %08X\n", tag, i < 8 ? 'D' : 'A', i & 7, a->dar[i], b->dar[i]); bad++; }
    if (a->pc != b->pc) { printf("  %s: PC %08X vs %08X\n", tag, a->pc, b->pc); bad++; }
    if ((a->n_flag & 0x80) != (b->n_flag & 0x80)) { printf("  %s: N %u vs %u\n", tag, (a->n_flag >> 7) & 1, (b->n_flag >> 7) & 1); bad++; }
    if (!a->not_z_flag != !b->not_z_flag) { printf("  %s: Z %u vs %u\n", tag, !a->not_z_flag, !b->not_z_flag); bad++; }
    if ((a->v_flag & 0x80) != (b->v_flag & 0x80)) { printf("  %s: V %u vs %u\n", tag, (a->v_flag >> 7) & 1, (b->v_flag >> 7) & 1); bad++; }
    if ((a->c_flag & 0x100) != (b->c_flag & 0x100)) { printf("  %s: C %u vs %u\n", tag, (a->c_flag >> 8) & 1, (b->c_flag >> 8) & 1); bad++; }
    if ((a->x_flag & 0x100) != (b->x_flag & 0x100)) { printf("  %s: X %u vs %u\n", tag, (a->x_flag >> 8) & 1, (b->x_flag >> 8) & 1); bad++; }
    unsigned md = 0; for (unsigned i = 0; i < RAM; i++) if (ra[i] != rb[i]) { if (md < 40) printf("  %s: RAM[%04X] %02X vs %02X\n", tag, i, ra[i], rb[i]); md++; }
    if (md) bad++;
    return bad;
}

int main(int argc, char **argv)
{
    unsigned iters = argc > 1 ? (unsigned)atoi(argv[1]) : 200;
    if (argc > 2) rnd_state = (uint32_t)atoi(argv[2]);
    { static uint8_t altstack[65536]; stack_t ss; ss.ss_sp = altstack; ss.ss_size = sizeof altstack; ss.ss_flags = 0; sigaltstack(&ss, NULL);
      struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGALRM, &sa, NULL); }
    if (getenv("JIT_TRACE")) jit_debug_trace = 1;
    int nojit = getenv("NOJIT") != NULL;
    int only = getenv("ONLY") ? atoi(getenv("ONLY")) : -1;
    if (!hw_init()) { fprintf(stderr, "expected mock hw\n"); return 1; }
    emu68k_init(RAM, 1);
    emu68k_set_io_mhz(0, 0);                              /* I/O ペーシングは切る(CPU の意味だけを比べる) */
    if (!jit_available()) { fprintf(stderr, "jit not available\n"); return 1; }
    unsigned failures = 0, skipped = 0, selfmod = 0;
    for (unsigned it = 0; it < iters; it++) {
        /* プログラム */
        plen = 0;
        unsigned ni = 4 + r(40);
        for (unsigned i = 0; i < ni; i++) gen_instr();
        uint32_t end = CODE0 + plen;
        w16(0x60FE);                                     /* BRA * */
        memcpy(cur_prog, prog, plen); cur_plen = plen;
        /* メモリ初期化: ベクタ、コード、サブルーチン(RTS)、乱数データ */
        static uint8_t img[RAM];
        for (unsigned i = 0; i < RAM; i++) img[i] = (uint8_t)rnd();
        img[0] = 0; img[1] = 0; img[2] = (uint8_t)(STACK >> 8); img[3] = (uint8_t)STACK;
        img[4] = 0; img[5] = 0; img[6] = (uint8_t)(CODE0 >> 8); img[7] = (uint8_t)CODE0;
        for (unsigned v = 2; v < 256; v++) {              /* 例外ベクタ v → CRASH_ADDR + 2v の BRA *(PC からベクタ番号が分かる) */
            uint32_t h = CRASH_ADDR + 2 * v;
            img[v * 4] = 0; img[v * 4 + 1] = 0; img[v * 4 + 2] = (uint8_t)(h >> 8); img[v * 4 + 3] = (uint8_t)h;
            img[h] = 0x60; img[h + 1] = 0xFE;
        }
        memcpy(img + CODE0, prog, plen);
        img[SUB_ADDR] = 0x4E; img[SUB_ADDR + 1] = 0x75;
        emu68k_load(0, img, RAM);
        jit_set_enabled(0);
        emu68k_reset();
        emu68k_run(1);                                    /* リセット後の状態を整える */
        m68ki_cpu.pc = CODE0;
        for (int i = 0; i < 8; i++) m68ki_cpu.dar[i] = (i < 4) ? (rnd() & 0xFF) : rnd();
        for (int i = 8; i < 15; i++) m68ki_cpu.dar[i] = r(16) ? 0x2000 + (rnd() % 0xB000 & ~1u) : 0x10000 + (rnd() % 0x1000 & ~1u);   /* 1/16 は RAM 外 */
        m68ki_cpu.dar[15] = STACK;
        m68ki_cpu.x_flag = r(2) ? 0x100 : 0; m68ki_cpu.c_flag = r(2) ? 0x100 : 0; m68ki_cpu.v_flag = r(2) ? 0x80 : 0;
        m68ki_cpu.n_flag = r(2) ? 0x80 : 0; m68ki_cpu.not_z_flag = r(2);
        snap_cpu = m68ki_cpu; ram_read(ram_snap); hw_mock_io_copy(io_snap, 0);
        if (only >= 0 && (int)it != only) continue;              /* ONLY=<n>: その反復だけ実行(乱数列は同じに進める) */
        if (getenv("END")) {                                     /* END=<hex>: その番地に BRA * を置いて途中で止めて比較 */
            end = (uint32_t)strtoul(getenv("END"), NULL, 16);
            m68k_write_memory_16(end, 0x60FE); ram_read(ram_snap);
        }
        printf("iter %u: %u instr, end %06X\n", it, ni, end); fflush(stdout);
        /* 解釈実行 */
        alarm(15); int oka; run_until(end, &oka); alarm(0);
        { uint32_t fc, fa, fp; emu68k_fault_info(&fc, &fa, &fp);
          printf("  interp done (%d) pc=%06X sp=%08X%s%u faults %u last %06X at pc %06X\n", oka, m68ki_cpu.pc & 0xFFFFFF, m68ki_cpu.dar[15], oka == 2 ? " vector " : "", oka == 2 ? ((m68ki_cpu.pc & 0xFFFFFF) - CRASH_ADDR) / 2 : 0, fc, fa, fp); fflush(stdout); }
        m68ki_cpu_core cpu_a = m68ki_cpu; ram_read(ram_a); hw_mock_io_copy(io_a, 0);
        if (!oka) { skipped++; continue; }                /* 暴走したプログラムは比較しない */
        if (memcmp(ram_a + CODE0, ram_snap + CODE0, plen + 2) != 0) { skipped++; selfmod++; continue; }   /* 自己書換え(直後の命令の変更はプリフェッチ次第で実機でも不定) */
        /* JIT */
        m68ki_cpu = snap_cpu; emu68k_load(0, ram_snap, RAM); hw_mock_io_copy(io_snap, 1);
        jit_set_enabled(!nojit); jit_flush_all();
        alarm(40); int okb; run_until(end, &okb); alarm(0);
        m68ki_cpu_core cpu_b = m68ki_cpu; ram_read(ram_b); hw_mock_io_copy(io_b, 0);
        jit_set_enabled(0);
        if (!oka) { skipped++; continue; }
        { uint32_t fc, fa, fp; emu68k_fault_info(&fc, &fa, &fp);
          printf("  jit done (%d) pc=%06X%s%u faults %u last %06X at pc %06X\n", okb, m68ki_cpu.pc & 0xFFFFFF, okb == 2 ? " vector " : "", okb == 2 ? ((m68ki_cpu.pc & 0xFFFFFF) - CRASH_ADDR) / 2 : 0, fc, fa, fp); }
        int bad = cmp_state("mismatch", &cpu_a, &cpu_b, ram_a, ram_b);
        { unsigned md = 0; for (unsigned i = 0; i < 0x1000; i++) if (io_a[i] != io_b[i]) { if (md < 8) printf("  mismatch: IO[E8%04X] %02X vs %02X\n", i, io_a[i], io_b[i]); md++; } if (md) bad++; }
        if (!okb || bad) {
            failures++;
            printf("FAIL iter %u\n", it);
            printf("FAIL iter %u (seed state) interp_end=%d jit_end=%d prog:", it, oka, okb);
            for (unsigned i = 0; i < plen; i++) printf("%s%02X", (i % 2) ? "" : " ", prog[i]);
            printf("\n");
            if (failures >= 5) break;
        }
    }
    char sb[256]; jit_stats(sb, sizeof sb);
    printf("%s\n", sb);
    printf("iters %u, failures %u, skipped %u (self-modifying %u)\n", iters, failures, skipped, selfmod);
    return failures ? 1 : 0;
}
