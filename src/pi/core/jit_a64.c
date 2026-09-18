/* SPDX-License-Identifier: MIT
 * jit_a64.c — 68000 → AArch64 動的翻訳(第 1〜2 段: レジスタはメモリ常駐、メモリアクセスは C ヘルパ、
 * フラグは Musashi の分割形式へ即時保存、ブロック末尾でディスパッチャへ戻る)。設計: docs/design/jit-plan.md
 *
 * 生成コードの約束:
 *   x19 = &m68ki_cpu        x24 = &jit_ctx(ヘルパ表・フォルトフラグ)
 *   x25/x26 = 命令内の一時値(ヘルパ呼び出しをまたぐ)   w27 = 実行中の命令の PC
 *   x0-x17 = 一時(ヘルパで壊れる)。ブロックは int block(void) の ABI(戻り値 = 終了理由)。
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include "jit.h"
#if defined(__aarch64__)
#include "m68kcpu.h"
#include "jit_a64_emit.h"

extern unsigned char m68ki_cycles[][0x10000];

/* ---- 設定 ---- */
#define CODE_WORDS   (8u << 20)        /* 32 MB のコード領域(命令数) */
#define MAX_BLOCKS   65536u
#define HASH_SIZE    16384u
#define BLOCK_MAX_INSTR 48
#define PERIODIC_CYC 64                /* 解釈実行の「8 命令ごと」に相当 */

/* ---- 終了理由 ---- */
enum { EXIT_NEXT = 0, EXIT_FAULT = 1, EXIT_STOPPED = 3 };

/* ---- ヘルパ表(x24 からの相対) ---- */
enum { H_RD8, H_RD16, H_RD32, H_WR8, H_WR16, H_WR32, H_SHIFT, H_MOVEM_ST, H_MOVEM_LD, H_N };
typedef struct {
    uint64_t fn[H_N];
    uint32_t fault;                    /* jit_fault のコピー先ではなく、生成コードは jit_fault を直接見る */
} jit_ctx_t;
static jit_ctx_t ctx;
#define OFF_FN(k) ((unsigned)offsetof(jit_ctx_t, fn) + 8u * (k))

/* ---- Musashi 状態のオフセット ---- */
#define OFF_D(n)   ((unsigned)offsetof(m68ki_cpu_core, dar) + 4u * (n))
#define OFF_A(n)   OFF_D(8 + (n))
#define OFF_PC     ((unsigned)offsetof(m68ki_cpu_core, pc))
#define OFF_N      ((unsigned)offsetof(m68ki_cpu_core, n_flag))
#define OFF_NOTZ   ((unsigned)offsetof(m68ki_cpu_core, not_z_flag))
#define OFF_V      ((unsigned)offsetof(m68ki_cpu_core, v_flag))
#define OFF_C      ((unsigned)offsetof(m68ki_cpu_core, c_flag))
#define OFF_X      ((unsigned)offsetof(m68ki_cpu_core, x_flag))

/* ---- ホストレジスタ ---- */
#define R_CPU 19
#define R_CTX 24
#define R_T0  25
#define R_T1  26
#define R_PC  27

/* ---- ブロック表 ---- */
typedef struct block {
    uint32_t pc;                       /* 開始 PC(24 ビット) */
    uint32_t end;                      /* 終了 PC(排他) */
    uint32_t cycles;                   /* 静的サイクル合計 */
    int (*code)(void);                 /* NULL = 解釈実行に任せる(先頭が非対応命令) */
    struct block *hnext;               /* ハッシュ連鎖 */
    struct block *gnext;               /* 開始グラニュール(256 B)の連鎖: 無効化用 */
    uint8_t  valid;
} block_t;
static block_t *blocks;                /* MAX_BLOCKS */
static uint32_t nblocks;
static block_t *hash[HASH_SIZE];
static block_t **granule;              /* 65536 個: グラニュールに始まるブロックの連鎖 */
uint8_t  jit_code_map[65536];
static uint32_t *code;                 /* コード領域 */
static uint32_t  code_used;
static int       enabled;
static struct { uint64_t blocks_run, translated, flushes, invalidated, fallbacks, faults, interp_steps; } st;

int  jit_available(void) { return 1; }
int  jit_enabled(void) { return enabled; }
void jit_set_enabled(int on) { enabled = on ? 1 : 0; }

static void icache_sync(void *start, void *end)
{
    uintptr_t p = (uintptr_t)start & ~63ull, e = (uintptr_t)end;
    for (; p < e; p += 64) __asm__ volatile ("dc cvau, %0" :: "r"(p) : "memory");
    __asm__ volatile ("dsb ish" ::: "memory");
    p = (uintptr_t)start & ~63ull;
    for (; p < e; p += 64) __asm__ volatile ("ic ivau, %0" :: "r"(p) : "memory");
    __asm__ volatile ("dsb ish\n\tisb" ::: "memory");
}

#ifdef __linux__
#include <sys/mman.h>
#endif
static int jit_init(void)
{
    if (code) return 1;
#ifdef __linux__
    code = (uint32_t *)mmap(NULL, CODE_WORDS * 4, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) code = NULL;
#else
    code = (uint32_t *)malloc(CODE_WORDS * 4);
    if (code && jit_host_make_exec(code, CODE_WORDS * 4) != 0) { free(code); code = NULL; }   /* ヒープは実行禁止(PXN)で写像されている */
#endif
    blocks = (block_t *)calloc(MAX_BLOCKS, sizeof(block_t));
    granule = (block_t **)calloc(65536, sizeof(block_t *));
    if (!code || !blocks || !granule) { enabled = 0; return 0; }
    ctx.fn[H_RD8] = (uint64_t)(uintptr_t)jit_rd8;   ctx.fn[H_RD16] = (uint64_t)(uintptr_t)jit_rd16;  ctx.fn[H_RD32] = (uint64_t)(uintptr_t)jit_rd32;
    ctx.fn[H_WR8] = (uint64_t)(uintptr_t)jit_wr8;   ctx.fn[H_WR16] = (uint64_t)(uintptr_t)jit_wr16;  ctx.fn[H_WR32] = (uint64_t)(uintptr_t)jit_wr32;
    return 1;
}

void jit_flush_all(void)
{
    if (!blocks) return;
    memset(hash, 0, sizeof hash);
    memset(granule, 0, 65536 * sizeof(block_t *));
    memset(jit_code_map, 0, sizeof jit_code_map);
    nblocks = 0; code_used = 0;
    st.flushes++;
}

static inline uint32_t hfn(uint32_t pc) { return (pc >> 1) & (HASH_SIZE - 1); }
static inline block_t *lookup(uint32_t pc)
{
    for (block_t *b = hash[hfn(pc)]; b; b = b->hnext) if (b->pc == pc && b->valid) return b;
    return NULL;
}
static void invalidate(block_t *b)
{
    b->valid = 0;
    block_t **pp = &hash[hfn(b->pc)];
    while (*pp) { if (*pp == b) { *pp = b->hnext; break; } pp = &(*pp)->hnext; }
    st.invalidated++;
}
void jit_note_write(uint32_t a)
{
    /* a を含むブロック = 開始グラニュールが a のグラニュール以前で、終端が a を超えるもの。
     * ブロックは最大 96 バイト程度なので、a のグラニュールとその手前 1 つを見れば足りる */
    uint32_t g = (a >> 8) & 0xFFFF;
    for (int k = 0; k < 2; k++) {
        uint32_t gg = (g - k) & 0xFFFF;
        for (block_t *b = granule[gg]; b; b = b->gnext)
            if (b->valid && a >= b->pc && a < b->end) invalidate(b);
    }
}

/* ============ 生成コードの部品 ============ */
typedef struct {
    a64_t e;
    uint32_t pc0;                      /* ブロック開始 PC */
    uint32_t pc;                       /* 翻訳中の命令の PC */
    uint32_t cycles;
    uint32_t fault_refs[BLOCK_MAX_INSTR * 4]; unsigned nfault;   /* フォルト脱出スタブへの分岐(後埋め) */
    uint32_t exit_refs[BLOCK_MAX_INSTR * 2];  unsigned nexit;    /* エピローグへの分岐(後埋め) */
    int      ended;                    /* 制御転移で終わった */
    int      bad;                      /* 翻訳失敗 */
} tr_t;

static void emit_prologue(tr_t *t)
{
    a64_t *e = &t->e;
    a64_stp_pre(e, 29, 30, SP, -64);
    a64_stp_off(e, 19, 24, SP, 16);
    a64_stp_off(e, 25, 26, SP, 32);
    a64_stp_off(e, 27, 28, SP, 48);
    a64_mov_x_imm(e, R_CPU, (uint64_t)(uintptr_t)&m68ki_cpu);
    a64_mov_x_imm(e, R_CTX, (uint64_t)(uintptr_t)&ctx);
}
/* エピローグ: w0 = 理由(呼び出し側が設定済み) */
static void emit_epilogue_body(a64_t *e)
{
    a64_ldp_off(e, 19, 24, SP, 16);
    a64_ldp_off(e, 25, 26, SP, 32);
    a64_ldp_off(e, 27, 28, SP, 48);
    a64_ldp_post(e, 29, 30, SP, 64);
    a64_ret(e);
}
static inline void st_pc_imm(tr_t *t, uint32_t pc) { a64_mov_w_imm(&t->e, 0, pc); a64_str_w(&t->e, 0, R_CPU, OFF_PC); }
/* ブロックを抜ける: PC は設定済み、理由 reason */
static void emit_exit(tr_t *t, unsigned reason)
{
    a64_mov_w_imm(&t->e, 0, reason);
    t->exit_refs[t->nexit++] = t->e.n;
    a64_b(&t->e, 0);
}
static void emit_exit_to(tr_t *t, uint32_t pc) { st_pc_imm(t, pc); emit_exit(t, EXIT_NEXT); }

/* ヘルパ呼び出し(引数は w0/w1 に置いてから) */
static void call_helper(tr_t *t, unsigned k)
{
    a64_ldr_x(&t->e, 16, R_CTX, OFF_FN(k));
    a64_blr(&t->e, 16);
}
static void check_fault(tr_t *t)
{
    a64_mov_x_imm(&t->e, 16, (uint64_t)(uintptr_t)&jit_fault);
    a64_ldr_w(&t->e, 1, 16, 0);
    t->fault_refs[t->nfault++] = t->e.n;
    a64_cbnz_w(&t->e, 1, 0);
}

/* ---- レジスタ ---- */
static inline void ld_reg(tr_t *t, unsigned wd, unsigned r) { a64_ldr_w(&t->e, wd, R_CPU, OFF_D(r)); }
static inline void st_reg32(tr_t *t, unsigned ws, unsigned r) { a64_str_w(&t->e, ws, R_CPU, OFF_D(r)); }
static inline void st_reg_sz(tr_t *t, unsigned ws, unsigned r, unsigned sz)
{
    if (sz == 0) a64_strb_w(&t->e, ws, R_CPU, OFF_D(r));
    else if (sz == 1) a64_strh_w(&t->e, ws, R_CPU, OFF_D(r));
    else a64_str_w(&t->e, ws, R_CPU, OFF_D(r));
}

/* ---- フラグ ----
 * Musashi: n_flag bit7 = N、not_z_flag != 0 で Z クリア、v_flag bit7 = V、c_flag bit8 = C、x_flag bit8 = X */
static void flags_logic(tr_t *t, unsigned wres, unsigned sz)     /* N,Z を結果から、V=C=0 */
{
    a64_t *e = &t->e;
    if (sz == 0) { a64_and_w_ff(e, 4, wres); a64_str_w(e, 4, R_CPU, OFF_N); a64_str_w(e, 4, R_CPU, OFF_NOTZ); }
    else if (sz == 1) { a64_lsr_w_imm(e, 4, wres, 8); a64_str_w(e, 4, R_CPU, OFF_N); a64_and_w_ffff(e, 4, wres); a64_str_w(e, 4, R_CPU, OFF_NOTZ); }
    else { a64_lsr_w_imm(e, 4, wres, 24); a64_str_w(e, 4, R_CPU, OFF_N); a64_str_w(e, wres, R_CPU, OFF_NOTZ); }
    a64_str_w(e, WZR, R_CPU, OFF_V);
    a64_str_w(e, WZR, R_CPU, OFF_C);
}
/* NZCV(直前の adds/subs)からフラグを保存。sub=1 なら C は借り(cc)。with_x: X も */
static void flags_nzcv(tr_t *t, int sub, int with_x)
{
    a64_t *e = &t->e;
    a64_cset_w(e, 4, CC_MI); a64_lsl_w_imm(e, 4, 4, 7); a64_str_w(e, 4, R_CPU, OFF_N);
    a64_cset_w(e, 4, CC_NE); a64_str_w(e, 4, R_CPU, OFF_NOTZ);
    a64_cset_w(e, 4, CC_VS); a64_lsl_w_imm(e, 4, 4, 7); a64_str_w(e, 4, R_CPU, OFF_V);
    a64_cset_w(e, 4, sub ? CC_CC : CC_CS); a64_lsl_w_imm(e, 4, 4, 8); a64_str_w(e, 4, R_CPU, OFF_C);
    if (with_x) a64_str_w(e, 4, R_CPU, OFF_X);
}
/* wres = wdst ± wsrc(サイズ sz)、フラグ設定。op: 0 add, 1 sub, 2 cmp(結果は捨てる、X 不変) */
static void arith(tr_t *t, unsigned wres, unsigned wdst, unsigned wsrc, unsigned sz, int op)
{
    a64_t *e = &t->e;
    unsigned sh = sz == 0 ? 24 : sz == 1 ? 16 : 0;
    if (sh) { a64_lsl_w_imm(e, 1, wsrc, sh); a64_lsl_w_imm(e, 2, wdst, sh); wsrc = 1; wdst = 2; }
    if (op == 0) a64_adds_w(e, 3, wdst, wsrc); else a64_subs_w(e, 3, wdst, wsrc);
    flags_nzcv(t, op != 0, op != 2);
    if (op != 2) { if (sh) a64_lsr_w_imm(e, wres, 3, sh); else if (wres != 3) a64_mov_w(e, wres, 3); }
}

/* ---- 実効アドレス ---- */
typedef struct {
    unsigned mode, reg, sz;            /* mode 0-7(7: reg = サブモード) */
    uint32_t imm;                      /* #imm / abs / d16 / d8 / PC 相対の定数 */
    unsigned ext;                      /* d8(An,Xn) の拡張語 */
    uint32_t pcref;                    /* PC 相対の基準(拡張語の番地) */
    unsigned nwords;                   /* 消費した拡張語数 */
} ea_t;
/* 拡張語を読んで ea を構成。戻り 0 = 翻訳不可 */
static int decode_ea(tr_t *t, ea_t *ea, unsigned mode, unsigned reg, unsigned sz, uint32_t *ppc)
{
    ea->mode = mode; ea->reg = reg; ea->sz = sz; ea->nwords = 0; ea->imm = 0; ea->ext = 0; ea->pcref = 0;
    uint32_t pc = *ppc;
    int w;
    switch (mode) {
    case 0: case 1: case 2: case 3: case 4: return 1;
    case 5: w = emu68k_jit_fetch16(pc); if (w < 0) return 0; ea->imm = (uint32_t)(int32_t)(int16_t)w; ea->nwords = 1; break;
    case 6: w = emu68k_jit_fetch16(pc); if (w < 0) return 0; ea->ext = (unsigned)w; ea->imm = (uint32_t)(int32_t)(int8_t)(w & 0xFF); ea->nwords = 1; break;
    case 7:
        switch (reg) {
        case 0: w = emu68k_jit_fetch16(pc); if (w < 0) return 0; ea->imm = (uint32_t)(int32_t)(int16_t)w; ea->nwords = 1; break;
        case 1: { int hi = emu68k_jit_fetch16(pc), lo = emu68k_jit_fetch16(pc + 2); if (hi < 0 || lo < 0) return 0; ea->imm = ((uint32_t)hi << 16) | (uint32_t)lo; ea->nwords = 2; break; }
        case 2: w = emu68k_jit_fetch16(pc); if (w < 0) return 0; ea->pcref = pc; ea->imm = pc + (uint32_t)(int32_t)(int16_t)w; ea->nwords = 1; break;
        case 3: w = emu68k_jit_fetch16(pc); if (w < 0) return 0; ea->pcref = pc; ea->ext = (unsigned)w; ea->imm = pc + (uint32_t)(int32_t)(int8_t)(w & 0xFF); ea->nwords = 1; break;
        case 4:
            if (sz == 2) { int hi = emu68k_jit_fetch16(pc), lo = emu68k_jit_fetch16(pc + 2); if (hi < 0 || lo < 0) return 0; ea->imm = ((uint32_t)hi << 16) | (uint32_t)lo; ea->nwords = 2; }
            else { w = emu68k_jit_fetch16(pc); if (w < 0) return 0; ea->imm = sz == 0 ? (uint32_t)(w & 0xFF) : (uint32_t)w; ea->nwords = 1; }
            break;
        default: return 0;
        }
        break;
    }
    *ppc = pc + 2 * ea->nwords;
    return 1;
}
static inline unsigned ea_bytes(const ea_t *ea) { return ea->sz == 0 ? (ea->reg == 7 ? 2 : 1) : ea->sz == 1 ? 2 : 4; }
static inline int ea_is_mem(const ea_t *ea) { return ea->mode >= 2 && !(ea->mode == 7 && ea->reg == 4); }
/* Xn の値(拡張語)を wd に: D/A、W なら符号拡張 */
/* pend: まだ確定していない An の更新(レジスタ番号 8-15、差分)。フォルト時に命令を
 * 解釈実行でやり直せるよう、メモリ書込みより前には An を書き戻さない。その間に
 * 同じ An を使う EA は更新後の値で計算する(MOVE.W -(A3),d16(A3) など) */
static inline void ld_reg_pend(tr_t *t, unsigned wd, unsigned r, int pend_reg, int pend_delta)
{
    ld_reg(t, wd, r);
    if ((int)r == pend_reg && pend_delta) { if (pend_delta > 0) a64_add_w_imm(&t->e, wd, wd, (unsigned)pend_delta); else a64_sub_w_imm(&t->e, wd, wd, (unsigned)-pend_delta); }
}
static void ld_index_p(tr_t *t, unsigned wd, unsigned ext, int pend_reg, int pend_delta)
{
    unsigned xr = (ext >> 12) & 15;
    ld_reg_pend(t, wd, xr, pend_reg, pend_delta);
    if (!(ext & 0x800)) a64_sxth_w(&t->e, wd, wd);
}
static void ld_index(tr_t *t, unsigned wd, unsigned ext) { ld_index_p(t, wd, ext, -1, 0); }
/* アドレスを wd に(An の更新は行わない: -(An) は減算後の値) */
static void ea_addr_p(tr_t *t, const ea_t *ea, unsigned wd, int pend_reg, int pend_delta)
{
    a64_t *e = &t->e;
    switch (ea->mode) {
    case 2: case 3: ld_reg_pend(t, wd, 8 + ea->reg, pend_reg, pend_delta); break;
    case 4: ld_reg_pend(t, wd, 8 + ea->reg, pend_reg, pend_delta); a64_sub_w_imm(e, wd, wd, ea_bytes(ea)); break;
    case 5: ld_reg_pend(t, wd, 8 + ea->reg, pend_reg, pend_delta); a64_mov_w_imm(e, 16, ea->imm); a64_add_w(e, wd, wd, 16); break;
    case 6: ld_reg_pend(t, wd, 8 + ea->reg, pend_reg, pend_delta); ld_index_p(t, 16, ea->ext, pend_reg, pend_delta); a64_add_w(e, wd, wd, 16); a64_mov_w_imm(e, 16, ea->imm); a64_add_w(e, wd, wd, 16); break;
    case 7:
        switch (ea->reg) {
        case 0: case 1: case 2: a64_mov_w_imm(e, wd, ea->imm); break;
        case 3: ld_index_p(t, 16, ea->ext, pend_reg, pend_delta); a64_mov_w_imm(e, wd, ea->imm); a64_add_w(e, wd, wd, 16); break;
        }
        break;
    }
}
static void ea_addr(tr_t *t, const ea_t *ea, unsigned wd) { ea_addr_p(t, ea, wd, -1, 0); }
/* (An)+ / -(An) の未確定の更新量(レジスタ番号と差分) */
static inline int ea_pend_reg(const ea_t *ea) { return (ea->mode == 3 || ea->mode == 4) ? (int)(8 + ea->reg) : -1; }
static inline int ea_pend_delta(const ea_t *ea) { return ea->mode == 3 ? (int)ea_bytes(ea) : ea->mode == 4 ? -(int)ea_bytes(ea) : 0; }
/* (An)+ / -(An) の An 更新 */
static void ea_commit(tr_t *t, const ea_t *ea)
{
    a64_t *e = &t->e;
    if (ea->mode == 3) { ld_reg(t, 16, 8 + ea->reg); a64_add_w_imm(e, 16, 16, ea_bytes(ea)); st_reg32(t, 16, 8 + ea->reg); }
    else if (ea->mode == 4) { ld_reg(t, 16, 8 + ea->reg); a64_sub_w_imm(e, 16, 16, ea_bytes(ea)); st_reg32(t, 16, 8 + ea->reg); }
}
/* オペランド読み → wd(メモリなら w0 経由。wd は 0-17 または 25/26)。副作用(An 更新)は commit で */
static void ea_read(tr_t *t, const ea_t *ea, unsigned wd)
{
    a64_t *e = &t->e;
    if (ea->mode == 0) { ld_reg(t, wd, ea->reg); return; }
    if (ea->mode == 1) { ld_reg(t, wd, 8 + ea->reg); return; }
    if (ea->mode == 7 && ea->reg == 4) { a64_mov_w_imm(e, wd, ea->imm); return; }
    ea_addr(t, ea, 0);
    call_helper(t, ea->sz == 0 ? H_RD8 : ea->sz == 1 ? H_RD16 : H_RD32);
    check_fault(t);
    if (wd != 0) a64_mov_w(e, wd, 0);
}
/* オペランド書き: 値は ws(メモリならアドレス計算後に w1 へ)。pend_* は未確定の An 更新 */
static void ea_write_p(tr_t *t, const ea_t *ea, unsigned ws, int pend_reg, int pend_delta)
{
    a64_t *e = &t->e;
    if (ea->mode == 0) { st_reg_sz(t, ws, ea->reg, ea->sz); return; }
    if (ea->mode == 1) { st_reg32(t, ws, 8 + ea->reg); return; }
    /* ws は 25/26 のいずれかである前提(アドレス計算で w0-w17 が壊れる) */
    ea_addr_p(t, ea, 0, pend_reg, pend_delta);
    a64_mov_w(e, 1, ws);
    call_helper(t, ea->sz == 0 ? H_WR8 : ea->sz == 1 ? H_WR16 : H_WR32);
    check_fault(t);
}
static void ea_write(tr_t *t, const ea_t *ea, unsigned ws) { ea_write_p(t, ea, ws, -1, 0); }
/* 読み書き先が同じ EA(RMW): アドレスを w26 に保持して再利用 */
static void ea_addr_keep(tr_t *t, const ea_t *ea) { ea_addr(t, ea, R_T1); }
static void ea_read_kept(tr_t *t, const ea_t *ea, unsigned wd)
{
    a64_mov_w(&t->e, 0, R_T1);
    call_helper(t, ea->sz == 0 ? H_RD8 : ea->sz == 1 ? H_RD16 : H_RD32);
    check_fault(t);
    if (wd != 0) a64_mov_w(&t->e, wd, 0);
}
static void ea_write_kept(tr_t *t, const ea_t *ea, unsigned ws)
{
    a64_mov_w(&t->e, 0, R_T1); a64_mov_w(&t->e, 1, ws);
    call_helper(t, ea->sz == 0 ? H_WR8 : ea->sz == 1 ? H_WR16 : H_WR32);
    check_fault(t);
}

/* ---- 条件(Musashi のフラグから w0 = 真偽) ---- */
static void cond_eval(tr_t *t, unsigned cc)
{
    a64_t *e = &t->e;
    switch (cc) {
    case 0: a64_mov_w_imm(e, 0, 1); break;                                             /* T */
    case 1: a64_mov_w_imm(e, 0, 0); break;                                             /* F */
    case 2: /* HI: !C && !Z */ a64_ldr_w(e, 0, R_CPU, OFF_C); a64_ubfx_w(e, 0, 0, 8, 1); a64_eor_w_bm(e, 0, 0, 0, 0);
            a64_ldr_w(e, 1, R_CPU, OFF_NOTZ); a64_cmp_w_imm(e, 1, 0); a64_cset_w(e, 1, CC_NE); a64_and_w(e, 0, 0, 1); break;
    case 3: /* LS: C || Z */  a64_ldr_w(e, 0, R_CPU, OFF_C); a64_ubfx_w(e, 0, 0, 8, 1);
            a64_ldr_w(e, 1, R_CPU, OFF_NOTZ); a64_cmp_w_imm(e, 1, 0); a64_cset_w(e, 1, CC_EQ); a64_orr_w(e, 0, 0, 1); break;
    case 4: /* CC */ a64_ldr_w(e, 0, R_CPU, OFF_C); a64_ubfx_w(e, 0, 0, 8, 1); a64_eor_w_bm(e, 0, 0, 0, 0); break;
    case 5: /* CS */ a64_ldr_w(e, 0, R_CPU, OFF_C); a64_ubfx_w(e, 0, 0, 8, 1); break;
    case 6: /* NE */ a64_ldr_w(e, 0, R_CPU, OFF_NOTZ); a64_cmp_w_imm(e, 0, 0); a64_cset_w(e, 0, CC_NE); break;
    case 7: /* EQ */ a64_ldr_w(e, 0, R_CPU, OFF_NOTZ); a64_cmp_w_imm(e, 0, 0); a64_cset_w(e, 0, CC_EQ); break;
    case 8: /* VC */ a64_ldr_w(e, 0, R_CPU, OFF_V); a64_ubfx_w(e, 0, 0, 7, 1); a64_eor_w_bm(e, 0, 0, 0, 0); break;
    case 9: /* VS */ a64_ldr_w(e, 0, R_CPU, OFF_V); a64_ubfx_w(e, 0, 0, 7, 1); break;
    case 10: /* PL */ a64_ldr_w(e, 0, R_CPU, OFF_N); a64_ubfx_w(e, 0, 0, 7, 1); a64_eor_w_bm(e, 0, 0, 0, 0); break;
    case 11: /* MI */ a64_ldr_w(e, 0, R_CPU, OFF_N); a64_ubfx_w(e, 0, 0, 7, 1); break;
    case 12: /* GE: !(N^V) */ a64_ldr_w(e, 0, R_CPU, OFF_N); a64_ldr_w(e, 1, R_CPU, OFF_V); a64_eor_w(e, 0, 0, 1); a64_ubfx_w(e, 0, 0, 7, 1); a64_eor_w_bm(e, 0, 0, 0, 0); break;
    case 13: /* LT */ a64_ldr_w(e, 0, R_CPU, OFF_N); a64_ldr_w(e, 1, R_CPU, OFF_V); a64_eor_w(e, 0, 0, 1); a64_ubfx_w(e, 0, 0, 7, 1); break;
    case 14: /* GT: GE && NE */ cond_eval(t, 12); a64_ldr_w(e, 1, R_CPU, OFF_NOTZ); a64_cmp_w_imm(e, 1, 0); a64_cset_w(e, 1, CC_NE); a64_and_w(e, 0, 0, 1); break;
    case 15: /* LE: LT || EQ */ cond_eval(t, 13); a64_ldr_w(e, 1, R_CPU, OFF_NOTZ); a64_cmp_w_imm(e, 1, 0); a64_cset_w(e, 1, CC_EQ); a64_orr_w(e, 0, 0, 1); break;
    }
}

/* ---- ヘルパ(C) ---- */
/* シフト/ローテート: kind = type(0 AS,1 LS,2 ROX,3 RO)<<2 | dir(1=左)<<1 | 0、sz、count。フラグは直接書く */
static uint32_t jit_shift(uint32_t v, uint32_t count, uint32_t kind_sz)
{
    unsigned type = (kind_sz >> 2) & 3, left = (kind_sz >> 1) & 1, sz = kind_sz >> 8;
    unsigned bits = sz == 0 ? 8 : sz == 1 ? 16 : 32;
    uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    uint32_t msb = 1u << (bits - 1);
    uint32_t val = v & mask, res = val;
    unsigned c = count, x = m68ki_cpu.x_flag & 0x100;
    uint32_t cflag = 0, vflag = 0, xflag = m68ki_cpu.x_flag;
    if (c == 0) {
        cflag = (type == 2) ? (x ? 0x100 : 0) : 0;
    } else if (type == 3) {                    /* ROL/ROR */
        unsigned r = c % bits;
        if (left) { res = r ? ((val << r) | (val >> (bits - r))) & mask : val; cflag = (res & 1) ? 0x100 : 0; }
        else      { res = r ? ((val >> r) | (val << (bits - r))) & mask : val; cflag = (res & msb) ? 0x100 : 0; }
    } else if (type == 2) {                    /* ROXL/ROXR */
        unsigned xb = x ? 1 : 0;
        for (unsigned i = 0; i < c; i++) {
            if (left) { unsigned out = (res & msb) ? 1 : 0; res = ((res << 1) | xb) & mask; xb = out; }
            else      { unsigned out = res & 1; res = (res >> 1) | (xb ? msb : 0); xb = out; }
        }
        cflag = xb ? 0x100 : 0; xflag = cflag;
    } else if (left) {                         /* ASL/LSL */
        if (c >= bits) { cflag = (c == bits && (val & 1)) ? 0x100 : 0; if (type == 0) vflag = val ? 0x80 : 0; res = 0; }
        else {
            res = (val << c) & mask;
            cflag = ((val >> (bits - c)) & 1) ? 0x100 : 0;
            if (type == 0) {                   /* ASL: 上位 c+1 ビットが全て同じでなければ V */
                uint32_t top = val >> (bits - c - 1);
                uint32_t all = (1u << (c + 1)) - 1;
                vflag = (top == 0 || top == all) ? 0 : 0x80;
            }
        }
        xflag = cflag;
    } else {                                   /* ASR/LSR */
        if (type == 0) {                       /* ASR */
            uint32_t sign = val & msb;
            if (c >= bits) { res = sign ? mask : 0; cflag = sign ? 0x100 : 0; }
            else { res = val >> c; if (sign) res |= mask & ~(mask >> c); cflag = ((val >> (c - 1)) & 1) ? 0x100 : 0; }
        } else {
            if (c > bits) { res = 0; cflag = 0; }
            else if (c == bits) { res = 0; cflag = (val & msb) ? 0x100 : 0; }
            else { res = val >> c; cflag = ((val >> (c - 1)) & 1) ? 0x100 : 0; }
        }
        xflag = cflag;
    }
    m68ki_cpu.n_flag = (res & msb) ? 0x80 : 0;
    m68ki_cpu.not_z_flag = res;
    m68ki_cpu.v_flag = vflag;
    m68ki_cpu.c_flag = cflag;
    m68ki_cpu.x_flag = xflag;
    return res;
}
/* MOVEM: レジスタ → メモリ。mode 4 は逆順・降順。戻り値 = 最終アドレス */
static uint32_t jit_movem_st(uint32_t addr, uint32_t mask_sz, uint32_t predec)
{
    unsigned mask = mask_sz & 0xFFFF, sz = (mask_sz >> 16) & 1;   /* sz 0=W 1=L */
    unsigned step = sz ? 4 : 2;
    if (predec) {
        for (int r = 15; r >= 0; r--) if (mask & (1u << (15 - r))) {
            addr -= step;
            if (sz) jit_wr32(addr, m68ki_cpu.dar[r]); else jit_wr16(addr, m68ki_cpu.dar[r] & 0xFFFF);
            if (jit_fault) return addr;
        }
    } else {
        for (int r = 0; r < 16; r++) if (mask & (1u << r)) {
            if (sz) jit_wr32(addr, m68ki_cpu.dar[r]); else jit_wr16(addr, m68ki_cpu.dar[r] & 0xFFFF);
            if (jit_fault) return addr;
            addr += step;
        }
    }
    return addr;
}
static uint32_t jit_movem_ld(uint32_t addr, uint32_t mask_sz, uint32_t unused)
{
    unsigned mask = mask_sz & 0xFFFF, sz = (mask_sz >> 16) & 1;
    unsigned step = sz ? 4 : 2;
    (void)unused;
    for (int r = 0; r < 16; r++) if (mask & (1u << r)) {
        uint32_t v = sz ? jit_rd32(addr) : (uint32_t)(int32_t)(int16_t)jit_rd16(addr);
        if (jit_fault) return addr;
        m68ki_cpu.dar[r] = v;
        addr += step;
    }
    return addr;
}

/* ============ 命令の翻訳 ============
 * 戻り値: 1 = 翻訳した(pc は次の命令へ進む)、0 = 非対応 */
static int tr_instr(tr_t *t, unsigned op)
{
    a64_t *e = &t->e;
    uint32_t pc = t->pc + 2;          /* 拡張語の位置 */
    ea_t src, dst;
    unsigned sz;
    t->cycles += m68ki_cycles[0][op];

    /* ---- MOVE / MOVEA ---- */
    if ((op & 0xC000) == 0 && (op & 0x3000) != 0) {
        sz = (op & 0x3000) == 0x1000 ? 0 : (op & 0x3000) == 0x3000 ? 1 : 2;
        unsigned dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
        if (!decode_ea(t, &src, (op >> 3) & 7, op & 7, sz, &pc)) return 0;
        if (dmode == 7 && dreg >= 2) return 0;
        if (!decode_ea(t, &dst, dmode, dreg, sz, &pc)) return 0;
        if (dmode == 1 && sz == 0) return 0;
        ea_read(t, &src, R_T0);
        if (dmode == 1) { ea_commit(t, &src); if (sz == 1) a64_sxth_w(e, R_T0, R_T0); st_reg32(t, R_T0, 8 + dreg); }
        else {
            /* 書込みがフォルトしたら解釈実行でやり直す: An の更新は書込みの後で確定する */
            ea_write_p(t, &dst, R_T0, ea_pend_reg(&src), ea_pend_delta(&src));
            flags_logic(t, R_T0, sz);                     /* Musashi と同じく書込みの後(バスエラー時の SR に見える) */
            ea_commit(t, &src); ea_commit(t, &dst);
        }
        t->pc = pc; return 1;
    }
    /* ---- MOVEQ ---- */
    if ((op & 0xF100) == 0x7000) {
        a64_mov_w_imm(e, R_T0, (uint32_t)(int32_t)(int8_t)(op & 0xFF));
        flags_logic(t, R_T0, 2);
        st_reg32(t, R_T0, (op >> 9) & 7);
        t->pc = pc; return 1;
    }
    /* ---- NOP ---- */
    if (op == 0x4E71) { t->pc = pc; return 1; }
    /* ---- LEA ---- */
    if ((op & 0xF1C0) == 0x41C0) {
        unsigned m = (op >> 3) & 7;
        if (m < 2 || m == 3 || m == 4 || (m == 7 && (op & 7) == 4)) return 0;
        if (!decode_ea(t, &src, m, op & 7, 2, &pc)) return 0;
        ea_addr(t, &src, R_T0); st_reg32(t, R_T0, 8 + ((op >> 9) & 7));
        t->pc = pc; return 1;
    }
    /* ---- PEA ---- */
    if ((op & 0xFFC0) == 0x4840 && ((op >> 3) & 7) >= 2) {
        unsigned m = (op >> 3) & 7;
        if (m == 3 || m == 4 || (m == 7 && (op & 7) == 4)) return 0;
        if (!decode_ea(t, &src, m, op & 7, 2, &pc)) return 0;
        ea_addr(t, &src, R_T0);
        ld_reg(t, 0, 15); a64_sub_w_imm(e, 0, 0, 4); a64_mov_w(e, R_T1, 0); a64_mov_w(e, 1, R_T0);
        call_helper(t, H_WR32); check_fault(t);
        st_reg32(t, R_T1, 15);
        t->pc = pc; return 1;
    }
    /* ---- CLR / TST / NOT / NEG ---- */
    if ((op & 0xFF00) == 0x4200 || (op & 0xFF00) == 0x4A00 || (op & 0xFF00) == 0x4600 || (op & 0xFF00) == 0x4400) {
        sz = (op >> 6) & 3; if (sz == 3) return 0;
        unsigned m = (op >> 3) & 7;
        if (m == 1) return 0;
        if (m == 7 && (op & 7) >= 2) return 0;   /* 書込み先は PC 相対不可。TST の PC 相対/即値は 68000 では不正命令 */
        if (!decode_ea(t, &dst, m, op & 7, sz, &pc)) return 0;
        if ((op & 0xFF00) == 0x4A00) {                  /* TST */
            ea_read(t, &dst, R_T0); ea_commit(t, &dst); flags_logic(t, R_T0, sz);
            t->pc = pc; return 1;
        }
        if (ea_is_mem(&dst)) { ea_addr_keep(t, &dst); ea_read_kept(t, &dst, R_T0); }
        else ea_read(t, &dst, R_T0);
        int after = 0;                                    /* フラグは書込みの後か(Musashi の順序: CLR/NOT は後、NEG は前) */
        if ((op & 0xFF00) == 0x4200) { a64_mov_w_imm(e, R_T0, 0); after = 1; }                             /* CLR(読んでから 0) */
        else if ((op & 0xFF00) == 0x4600) { a64_mvn_w(e, R_T0, R_T0); after = 1; }                         /* NOT */
        else { a64_mov_w_imm(e, 2, 0); arith(t, R_T0, 2, R_T0, sz, 1); }                                  /* NEG: 0 - x(X も) */
        if (ea_is_mem(&dst)) ea_write_kept(t, &dst, R_T0); else ea_write(t, &dst, R_T0);
        if (after) flags_logic(t, R_T0, sz);
        ea_commit(t, &dst);
        t->pc = pc; return 1;
    }
    /* ---- EXT ---- */
    if ((op & 0xFFB8) == 0x4880) {
        unsigned r = op & 7;
        ld_reg(t, R_T0, r);
        if (op & 0x40) { a64_sxth_w(e, R_T0, R_T0); flags_logic(t, R_T0, 2); st_reg32(t, R_T0, r); }
        else { a64_sxtb_w(e, R_T0, R_T0); flags_logic(t, R_T0, 1); st_reg_sz(t, R_T0, r, 1); }
        t->pc = pc; return 1;
    }
    /* ---- SWAP ---- */
    if ((op & 0xFFF8) == 0x4840) {
        unsigned r = op & 7;
        ld_reg(t, R_T0, r); a64_ror_w_imm(e, R_T0, R_T0, 16); flags_logic(t, R_T0, 2); st_reg32(t, R_T0, r);
        t->pc = pc; return 1;
    }
    /* ---- ADDQ / SUBQ ---- */
    if ((op & 0xF000) == 0x5000 && ((op >> 6) & 3) != 3) {      /* bit8: 0 = ADDQ、1 = SUBQ */
        sz = (op >> 6) & 3;
        unsigned q = (op >> 9) & 7; if (!q) q = 8;
        unsigned m = (op >> 3) & 7;
        if (m == 7 && (op & 7) >= 2) return 0;
        if (!decode_ea(t, &dst, m, op & 7, sz, &pc)) return 0;
        if (m == 1) {                                    /* An: フラグ不変、32 ビット */
            if (sz == 0) return 0;
            ld_reg(t, R_T0, 8 + (op & 7));
            if (op & 0x100) a64_sub_w_imm(e, R_T0, R_T0, q); else a64_add_w_imm(e, R_T0, R_T0, q);
            st_reg32(t, R_T0, 8 + (op & 7));
            t->pc = pc; return 1;
        }
        if (ea_is_mem(&dst)) { ea_addr_keep(t, &dst); ea_read_kept(t, &dst, R_T0); } else ea_read(t, &dst, R_T0);
        a64_mov_w_imm(e, 1, q);
        arith(t, R_T0, R_T0, 1, sz, (op & 0x100) ? 1 : 0);
        if (ea_is_mem(&dst)) ea_write_kept(t, &dst, R_T0); else ea_write(t, &dst, R_T0);
        ea_commit(t, &dst);
        t->pc = pc; return 1;
    }
    /* ---- ADDI / SUBI / CMPI / ANDI / ORI / EORI ---- */
    if ((op & 0xF000) == 0 && ((op & 0x0F00) == 0x0600 || (op & 0x0F00) == 0x0400 || (op & 0x0F00) == 0x0C00 ||
                               (op & 0x0F00) == 0x0200 || (op & 0x0F00) == 0x0000 || (op & 0x0F00) == 0x0A00) && ((op >> 6) & 3) != 3) {
        sz = (op >> 6) & 3;
        unsigned kind = (op >> 8) & 0xF;                 /* 6 ADDI 4 SUBI C CMPI 2 ANDI 0 ORI A EORI */
        unsigned m = (op >> 3) & 7;
        if (m == 1) return 0;
        if (m == 7 && (op & 7) >= 2) return 0;           /* CMPI の PC 相対も 68000 では不正命令 */
        if (m == 7 && (op & 7) == 4) return 0;           /* to CCR/SR は非対応 */
        ea_t immea; if (!decode_ea(t, &immea, 7, 4, sz, &pc)) return 0;
        if (!decode_ea(t, &dst, m, op & 7, sz, &pc)) return 0;
        int mem = ea_is_mem(&dst);
        if (mem) { ea_addr_keep(t, &dst); ea_read_kept(t, &dst, R_T0); } else ea_read(t, &dst, R_T0);
        a64_mov_w_imm(e, 1, immea.imm);
        if (kind == 0x6) arith(t, R_T0, R_T0, 1, sz, 0);
        else if (kind == 0x4) arith(t, R_T0, R_T0, 1, sz, 1);
        else if (kind == 0xC) { arith(t, R_T0, R_T0, 1, sz, 2); ea_commit(t, &dst); t->pc = pc; return 1; }
        else {
            if (kind == 0x2) a64_and_w(e, R_T0, R_T0, 1); else if (kind == 0x0) a64_orr_w(e, R_T0, R_T0, 1); else a64_eor_w(e, R_T0, R_T0, 1);
            if (kind == 0x2) flags_logic(t, R_T0, sz);   /* ANDI: 書込み前、ORI/EORI: 後(Musashi の順序) */
        }
        if (mem) ea_write_kept(t, &dst, R_T0); else ea_write(t, &dst, R_T0);
        if (kind == 0x0 || kind == 0xA) flags_logic(t, R_T0, sz);
        ea_commit(t, &dst);
        t->pc = pc; return 1;
    }
    /* ---- ADD / SUB / AND / OR / EOR / CMP(Dn 形式)、ADDA / SUBA / CMPA ---- */
    if ((op & 0xF000) == 0xD000 || (op & 0xF000) == 0x9000 || (op & 0xF000) == 0xC000 || (op & 0xF000) == 0x8000 || (op & 0xF000) == 0xB000) {
        unsigned line = op >> 12, opmode = (op >> 6) & 7, dn = (op >> 9) & 7;
        unsigned m = (op >> 3) & 7;
        if (opmode == 3 || opmode == 7) {                /* ADDA / SUBA / CMPA(.W = 符号拡張) */
            if (line == 0xC || line == 0x8) return 0;    /* MULU/MULS, DIVU/DIVS */
            sz = opmode == 3 ? 1 : 2;
            if (!decode_ea(t, &src, m, op & 7, sz, &pc)) return 0;
            ea_read(t, &src, R_T0); ea_commit(t, &src);
            if (sz == 1) a64_sxth_w(e, R_T0, R_T0);
            ld_reg(t, 2, 8 + dn);
            if (line == 0xB) { arith(t, 3, 2, R_T0, 2, 2); }
            else { if (line == 0xD) a64_add_w(e, 2, 2, R_T0); else a64_sub_w(e, 2, 2, R_T0); st_reg32(t, 2, 8 + dn); }
            t->pc = pc; return 1;
        }
        sz = opmode & 3;
        if (opmode < 4) {                                 /* Dn = Dn op EA */
            if (line == 0xC && (op & 0x1F0) == 0x100) return 0;   /* ABCD / EXG(0xC140 等) */
            if (line == 0xC && ((op & 0x1F8) == 0x140 || (op & 0x1F8) == 0x148 || (op & 0x1F8) == 0x188)) return 0;
            if (line == 0xB && m == 1 && sz == 0) return 0;
            if (line == 0x8 && (op & 0x1F0) == 0x100) return 0;   /* SBCD */
            if (!decode_ea(t, &src, m, op & 7, sz, &pc)) return 0;
            ea_read(t, &src, R_T0); ea_commit(t, &src);
            ld_reg(t, 2, dn);
            if (line == 0xD) { arith(t, 3, 2, R_T0, sz, 0); st_reg_sz(t, 3, dn, sz); }
            else if (line == 0x9) { arith(t, 3, 2, R_T0, sz, 1); st_reg_sz(t, 3, dn, sz); }
            else if (line == 0xB) { arith(t, 3, 2, R_T0, sz, 2); }
            else { if (line == 0xC) a64_and_w(e, 3, 2, R_T0); else a64_orr_w(e, 3, 2, R_T0); flags_logic(t, 3, sz); st_reg_sz(t, 3, dn, sz); }
            t->pc = pc; return 1;
        }
        /* opmode 4-6: EA = EA op Dn(メモリ先)、EOR は Dn 先も可、CMPM(line B, mode 1) は非対応 */
        if (line == 0xB) { if (m == 1) return 0; }      /* CMPM */
        else if (m < 2) return 0;                         /* ADDX/SUBX/ABCD/SBCD/EXG */
        if (m == 7 && (op & 7) >= 2) return 0;
        if (!decode_ea(t, &dst, m, op & 7, sz, &pc)) return 0;
        int mem = ea_is_mem(&dst);
        if (mem) { ea_addr_keep(t, &dst); ea_read_kept(t, &dst, R_T0); } else ea_read(t, &dst, R_T0);
        ld_reg(t, 1, dn);
        if (line == 0xD) arith(t, R_T0, R_T0, 1, sz, 0);
        else if (line == 0x9) arith(t, R_T0, R_T0, 1, sz, 1);
        else { if (line == 0xC) a64_and_w(e, R_T0, R_T0, 1); else if (line == 0x8) a64_orr_w(e, R_T0, R_T0, 1); else a64_eor_w(e, R_T0, R_T0, 1); if (line == 0xC) flags_logic(t, R_T0, sz); }
        if (mem) ea_write_kept(t, &dst, R_T0); else ea_write(t, &dst, R_T0);
        if (line == 0x8 || line == 0xB) flags_logic(t, R_T0, sz);   /* OR/EOR: 書込みの後(Musashi の順序) */
        ea_commit(t, &dst);
        t->pc = pc; return 1;
    }
    /* ---- シフト/ローテート(Dn) ---- */
    if ((op & 0xF000) == 0xE000 && ((op >> 6) & 3) != 3) {
        sz = (op >> 6) & 3;
        unsigned r = op & 7, type = (op >> 3) & 3, left = (op >> 8) & 1;
        ld_reg(t, 0, r);
        if (op & 0x20) { ld_reg(t, 1, (op >> 9) & 7); a64_and_w_bm(e, 1, 1, 0, 5); }   /* count = Dn & 63 */
        else { unsigned c = (op >> 9) & 7; a64_mov_w_imm(e, 1, c ? c : 8); }
        a64_mov_w_imm(e, 2, (sz << 8) | (type << 2) | (left << 1));
        call_helper(t, H_SHIFT);
        st_reg_sz(t, 0, r, sz);
        t->pc = pc; return 1;
    }
    /* ---- BTST / BCHG / BCLR / BSET ---- */
    if ((op & 0xFF00) == 0x0800 || ((op & 0xF100) == 0x0100 && ((op >> 3) & 7) != 1)) {
        int stat = (op & 0xFF00) == 0x0800;
        unsigned kind = (op >> 6) & 3;                  /* 0 BTST 1 BCHG 2 BCLR 3 BSET */
        unsigned m = (op >> 3) & 7;
        if (stat && m == 7 && (op & 7) == 4) return 0;
        if (kind != 0 && m == 7 && (op & 7) >= 2) return 0;
        if (m == 7 && (op & 7) == 4 && (stat || kind != 0)) return 0;   /* BTST #n,#imm は不正命令 */
        int bitimm = -1;
        if (stat) { int w = emu68k_jit_fetch16(pc); if (w < 0) return 0; bitimm = w & 0xFF; pc += 2; }
        sz = (m == 0) ? 2 : 0;
        if (!decode_ea(t, &dst, m, op & 7, sz, &pc)) return 0;
        int mem = ea_is_mem(&dst);
        if (mem) { ea_addr_keep(t, &dst); ea_read_kept(t, &dst, R_T0); } else ea_read(t, &dst, R_T0);
        /* ビット番号 → w1(mod 32 / mod 8) */
        if (stat) a64_mov_w_imm(e, 1, (unsigned)bitimm & (m == 0 ? 31 : 7));
        else { ld_reg(t, 1, (op >> 9) & 7); a64_and_w_bm(e, 1, 1, 0, m == 0 ? 4 : 2); }
        a64_mov_w_imm(e, 2, 1); a64_lslv_w(e, 2, 2, 1);           /* w2 = 1 << bit */
        a64_and_w(e, 3, R_T0, 2); a64_str_w(e, 3, R_CPU, OFF_NOTZ); /* Z = ビットが 0 */
        if (kind == 0) { ea_commit(t, &dst); t->pc = pc; return 1; }
        if (kind == 1) a64_eor_w(e, R_T0, R_T0, 2);
        else if (kind == 2) { a64_mvn_w(e, 2, 2); a64_and_w(e, R_T0, R_T0, 2); }
        else a64_orr_w(e, R_T0, R_T0, 2);
        if (mem) ea_write_kept(t, &dst, R_T0); else ea_write(t, &dst, R_T0);
        ea_commit(t, &dst);
        t->pc = pc; return 1;
    }
    /* ---- LINK / UNLK ---- */
    if ((op & 0xFFF8) == 0x4E50) {
        int w = emu68k_jit_fetch16(pc); if (w < 0) return 0; pc += 2;
        unsigned an = op & 7;
        ld_reg(t, 0, 15); a64_sub_w_imm(e, 0, 0, 4); a64_mov_w(e, R_T1, 0); ld_reg(t, 1, 8 + an);
        call_helper(t, H_WR32); check_fault(t);
        st_reg32(t, R_T1, 8 + an);
        a64_mov_w_imm(e, 1, (uint32_t)(int32_t)(int16_t)w); a64_add_w(e, R_T1, R_T1, 1); st_reg32(t, R_T1, 15);
        t->pc = pc; return 1;
    }
    if ((op & 0xFFF8) == 0x4E58) {
        unsigned an = op & 7;
        ld_reg(t, 0, 8 + an); a64_mov_w(e, R_T1, 0);
        call_helper(t, H_RD32); check_fault(t);
        st_reg32(t, 0, 8 + an);
        a64_add_w_imm(e, R_T1, R_T1, 4); st_reg32(t, R_T1, 15);
        t->pc = pc; return 1;
    }
    /* ---- MOVEM ---- */
    if ((op & 0xFB80) == 0x4880 && ((op >> 3) & 7) >= 2) {
        int tomem = !(op & 0x400);
        unsigned m = (op >> 3) & 7, lsz = (op >> 6) & 1;
        int w = emu68k_jit_fetch16(pc); if (w < 0) return 0; pc += 2;
        if (tomem && (m == 3 || (m == 7 && (op & 7) >= 2))) return 0;
        if (!tomem && m == 4) return 0;
        if (m == 7 && (op & 7) == 4) return 0;
        if (!decode_ea(t, &dst, m, op & 7, lsz ? 2 : 1, &pc)) return 0;
        unsigned mask = (unsigned)w & 0xFFFF;
        if (m == 4) { /* -(An): マスクは逆順、開始アドレスは An そのもの */
            ld_reg(t, 0, 8 + (op & 7));
            a64_mov_w_imm(e, 1, mask | (lsz << 16)); a64_mov_w_imm(e, 2, 1);
            call_helper(t, H_MOVEM_ST); check_fault(t);
            st_reg32(t, 0, 8 + (op & 7));
        } else {
            ea_addr(t, &dst, 0);
            a64_mov_w_imm(e, 1, mask | (lsz << 16)); a64_mov_w_imm(e, 2, 0);
            call_helper(t, tomem ? H_MOVEM_ST : H_MOVEM_LD); check_fault(t);
            if (m == 3) st_reg32(t, 0, 8 + (op & 7));
        }
        t->pc = pc; return 1;
    }
    /* ---- Bcc / BRA / BSR ---- */
    if ((op & 0xF000) == 0x6000) {
        unsigned cc = (op >> 8) & 0xF;
        int32_t disp = (int8_t)(op & 0xFF);
        uint32_t next = pc;
        if ((op & 0xFF) == 0) { int w = emu68k_jit_fetch16(pc); if (w < 0) return 0; disp = (int16_t)w; next = pc + 2; }
        uint32_t target = (t->pc + 2 + (uint32_t)disp) & 0xFFFFFF;
        if (cc == 1) {                                   /* BSR */
            ld_reg(t, 0, 15); a64_sub_w_imm(e, 0, 0, 4); a64_mov_w(e, R_T1, 0); a64_mov_w_imm(e, 1, next);
            call_helper(t, H_WR32); check_fault(t);
            st_reg32(t, R_T1, 15);
            emit_exit_to(t, target);
        } else if (cc == 0) {
            emit_exit_to(t, target);
        } else {
            cond_eval(t, cc);
            uint32_t ref = e->n; a64_cbz_w(e, 0, 0);
            emit_exit_to(t, target);
            a64_patch_b19(e, ref);
            emit_exit_to(t, next);
        }
        t->pc = next; t->ended = 1; return 1;
    }
    /* ---- DBcc ---- */
    if ((op & 0xF0F8) == 0x50C8) {
        unsigned cc = (op >> 8) & 0xF, r = op & 7;
        int w = emu68k_jit_fetch16(pc); if (w < 0) return 0;
        uint32_t next = pc + 2, target = (pc + (uint32_t)(int32_t)(int16_t)w) & 0xFFFFFF;
        cond_eval(t, cc);
        uint32_t ref = e->n; a64_cbnz_w(e, 0, 0);       /* 条件成立 → 抜ける */
        ld_reg(t, 0, r); a64_sub_w_imm(e, 0, 0, 1); a64_strh_w(e, 0, R_CPU, OFF_D(r));
        a64_and_w_ffff(e, 0, 0); a64_mov_w_imm(e, 1, 0xFFFF); a64_cmp_w(e, 0, 1);
        uint32_t ref2 = e->n; a64_bcond(e, CC_EQ, 0);   /* -1 になった → 抜ける */
        emit_exit_to(t, target);
        a64_patch_b19(e, ref); a64_patch_b19(e, ref2);
        emit_exit_to(t, next);
        t->pc = next; t->ended = 1; return 1;
    }
    /* ---- JMP / JSR ---- */
    if ((op & 0xFF80) == 0x4E80) {
        unsigned m = (op >> 3) & 7;
        if (m < 2 || m == 3 || m == 4 || (m == 7 && (op & 7) == 4)) return 0;
        if (!decode_ea(t, &src, m, op & 7, 2, &pc)) return 0;
        ea_addr(t, &src, R_T0);
        if (!(op & 0x40)) {                              /* JSR */
            ld_reg(t, 0, 15); a64_sub_w_imm(e, 0, 0, 4); a64_mov_w(e, R_T1, 0); a64_mov_w_imm(e, 1, pc);
            call_helper(t, H_WR32); check_fault(t);
            st_reg32(t, R_T1, 15);
        }
        a64_and_w_bm(e, R_T0, R_T0, 0, 23); a64_str_w(e, R_T0, R_CPU, OFF_PC);
        emit_exit(t, EXIT_NEXT);
        t->pc = pc; t->ended = 1; return 1;
    }
    /* ---- RTS ---- */
    if (op == 0x4E75) {
        ld_reg(t, 0, 15); a64_mov_w(e, R_T1, 0);
        call_helper(t, H_RD32); check_fault(t);
        a64_and_w_bm(e, 0, 0, 0, 23); a64_str_w(e, 0, R_CPU, OFF_PC);
        a64_add_w_imm(e, R_T1, R_T1, 4); st_reg32(t, R_T1, 15);
        emit_exit(t, EXIT_NEXT);
        t->pc = pc; t->ended = 1; return 1;
    }
    return 0;
}

/* ============ ブロックの翻訳 ============ */
static block_t *translate(uint32_t pc0)
{
    if (nblocks >= MAX_BLOCKS || code_used + 8192 > CODE_WORDS) jit_flush_all();
    block_t *b = &blocks[nblocks];
    memset(b, 0, sizeof *b);
    b->pc = pc0; b->end = pc0; b->valid = 1;
    tr_t t; memset(&t, 0, sizeof t);
    t.e.buf = code + code_used; t.e.cap = 8192; t.pc0 = pc0; t.pc = pc0;
    emit_prologue(&t);
    unsigned n = 0;
    while (!t.ended && n < BLOCK_MAX_INSTR) {
        int op = emu68k_jit_fetch16(t.pc);
        uint32_t ipc = t.pc;
        if (op < 0) break;
        uint32_t save_n = t.e.n, save_cyc = t.cycles, save_nf = t.nfault, save_ne = t.nexit;
        a64_mov_w_imm(&t.e, R_PC, ipc);
        if (!tr_instr(&t, (unsigned)op)) { t.e.n = save_n; t.cycles = save_cyc; t.nfault = save_nf; t.nexit = save_ne; t.pc = ipc; break; }
        n++;
    }
    if (n == 0) { b->code = NULL; b->end = pc0 + 2; st.fallbacks++; }
    else {
        if (!t.ended) emit_exit_to(&t, t.pc);
        /* フォルト脱出スタブ: PC = w27、理由 1 */
        uint32_t fstub = t.e.n;
        a64_str_w(&t.e, R_PC, R_CPU, OFF_PC);
        a64_mov_w_imm(&t.e, 0, EXIT_FAULT);
        /* エピローグ */
        uint32_t epi = t.e.n;
        emit_epilogue_body(&t.e);
        for (unsigned i = 0; i < t.nfault; i++) { int32_t off = (int32_t)(fstub - t.fault_refs[i]) * 4; t.e.buf[t.fault_refs[i]] = (t.e.buf[t.fault_refs[i]] & 0xFF00001Fu) | ((((uint32_t)off >> 2) & 0x7FFFFu) << 5); }
        for (unsigned i = 0; i < t.nexit; i++) { int32_t off = (int32_t)(epi - t.exit_refs[i]) * 4; t.e.buf[t.exit_refs[i]] = 0x14000000u | (((uint32_t)off >> 2) & 0x3FFFFFFu); }
        if (t.e.overflow) { b->code = NULL; b->end = pc0 + 2; st.fallbacks++; }
        else {
            b->code = (int (*)(void))(uintptr_t)t.e.buf;
            b->end = t.pc; b->cycles = t.cycles;
            code_used += t.e.n;
            icache_sync(t.e.buf, t.e.buf + t.e.n);
            for (uint32_t g = pc0 >> 8; g <= ((b->end - 1) >> 8); g++) jit_code_map[g & 0xFFFF] = 1;
            b->gnext = granule[(pc0 >> 8) & 0xFFFF]; granule[(pc0 >> 8) & 0xFFFF] = b;
            st.translated++;
        }
    }
    b->hnext = hash[hfn(pc0)]; hash[hfn(pc0)] = b;
    nblocks++;
    return b;
}

/* ============ ディスパッチャ ============ */
static int once_mode, once_done;   /* jit s: 1 ブロックだけ実行 */
static uint32_t trace_hist[16]; static unsigned trace_hp;   /* 直近に実行したブロック(デバッグ) */
int jit_run(int cycles)
{
    if (!jit_init()) return emu68k_step_interp();
    ctx.fn[H_SHIFT] = (uint64_t)(uintptr_t)jit_shift;
    ctx.fn[H_MOVEM_ST] = (uint64_t)(uintptr_t)jit_movem_st;
    ctx.fn[H_MOVEM_LD] = (uint64_t)(uintptr_t)jit_movem_ld;
    int done = 0; unsigned since = 0;
    while (done < cycles) {
        if (emu68k_jit_stop_req()) break;
        uint32_t pc = m68ki_cpu.pc & 0xFFFFFF;
        emu68k_jit_trace(pc);
        int n;
        if (m68ki_cpu.stopped) {            /* STOP/HALT 中: 翻訳コードは走らせない(割込み待ちは Musashi に任せる) */
            n = emu68k_step_interp(); st.interp_steps++; if (n <= 0) break;
            done += n; since += (unsigned)n; continue;
        }
        block_t *b = lookup(pc);
        if (!b) b = translate(pc);
        if (!b->code) { n = emu68k_step_interp(); st.interp_steps++; if (n <= 0) break; }
        else {
            jit_fault = 0;
            trace_hist[trace_hp++ & 15] = pc;
#ifdef __linux__
            if (jit_debug_trace) { printf("run %06X-%06X\n", b->pc, b->end); fflush(stdout); }
#endif
            int r = b->code();
            st.blocks_run++;
            n = (int)b->cycles;
            emu68k_jit_account(n);
            if (r == EXIT_FAULT) {
#ifdef __linux__
                if (jit_debug_trace) { printf("fault exit at %06X, reexec\n", m68ki_cpu.pc & 0xFFFFFF); fflush(stdout); }
#endif
                st.faults++; int k = emu68k_step_interp(); if (k > 0) n += k;
#ifdef __linux__
                if (jit_debug_trace) { printf("  after reexec pc=%06X\n", m68ki_cpu.pc & 0xFFFFFF); fflush(stdout); }
#endif
            }
        }
        done += n; since += (unsigned)n;
        if (once_mode) { once_done = 1; once_mode = 0; break; }
        if (since >= PERIODIC_CYC) {
            since = 0;
            emu68k_jit_periodic(m68ki_cpu.pc);
            /* 割込みの受付は Musashi の実行入口で行われる: 1 命令だけ解釈実行で通す */
            int k = emu68k_step_interp(); st.interp_steps++;
            if (k > 0) done += k; else break;
        }
    }
    return done;
}

/* デバッグ: ホストアドレス host を含むブロックを探す(0 = 不明)。out に 68000 範囲とコードのオフセット */
int jit_debug_locate(uintptr_t host, char *out, unsigned n)
{
    for (uint32_t i = 0; i < nblocks; i++) {
        block_t *b = &blocks[i];
        if (!b->code) continue;
        uintptr_t s = (uintptr_t)b->code;
        if (host >= s && host < s + 8192 * 4) {
            /* 次のブロックの開始より手前か */
            uintptr_t e = s + 8192 * 4;
            for (uint32_t j = 0; j < nblocks; j++) if (blocks[j].code && (uintptr_t)blocks[j].code > s && (uintptr_t)blocks[j].code < e) e = (uintptr_t)blocks[j].code;
            if (host < e) { snprintf(out, n, "block #%u 68k %06X-%06X, host offset +%u words", i, b->pc, b->end, (unsigned)((host - s) / 4)); return 1; }
        }
    }
    snprintf(out, n, "not in JIT code"); return 0;
}
int jit_debug_trace;                       /* Linux: 実行ブロックを標準出力へ */
void jit_debug_hist(char *out, unsigned n)
{
    unsigned o = 0;
    for (unsigned i = 0; i < 16 && o + 8 < n; i++) o += (unsigned)snprintf(out + o, n - o, "%06X ", trace_hist[(trace_hp + i) & 15]);
}

/* デバッグ: pc のブロックを翻訳し、生成コードを 16 進で out に(実行はしない)。戻り = 命令語数 */
void jit_set_once(int on) { once_mode = on; once_done = 0; }
int  jit_once_done(void) { return once_done; }
int jit_debug_translate(uint32_t pc, char *out, unsigned n)
{
    if (!jit_init()) { snprintf(out, n, "jit: init failed"); return -1; }
    ctx.fn[H_SHIFT] = (uint64_t)(uintptr_t)jit_shift;
    ctx.fn[H_MOVEM_ST] = (uint64_t)(uintptr_t)jit_movem_st;
    ctx.fn[H_MOVEM_LD] = (uint64_t)(uintptr_t)jit_movem_ld;
    block_t *b = lookup(pc & 0xFFFFFF);
    if (!b) b = translate(pc & 0xFFFFFF);
    unsigned o = 0;
    o += (unsigned)snprintf(out + o, n - o, "block %06X-%06X cycles %u code %s\n", b->pc, b->end, b->cycles, b->code ? "yes" : "none(fallback)");
    if (b->code) {
        const uint32_t *w = (const uint32_t *)(uintptr_t)b->code;
        unsigned cnt = 0;
        /* 終端 = ret(0xD65F03C0)まで */
        while (cnt < 4096 && w[cnt] != 0xD65F03C0u) cnt++;
        if (cnt < 4096) cnt++;
        for (unsigned i = 0; i < cnt && o + 12 < n; i++) o += (unsigned)snprintf(out + o, n - o, "%08x%s", w[i], (i % 8 == 7) ? "\n" : " ");
        if (o + 2 < n) { out[o++] = '\n'; out[o] = 0; }
        return (int)cnt;
    }
    return 0;
}

unsigned jit_stats(char *buf, unsigned n)
{
    return (unsigned)snprintf(buf, n, "jit: %s, blocks %u (translated %llu, fallback-only %llu, invalidated %llu, flushes %llu), code %u KB, runs %llu, faults %llu, interp steps %llu",
        enabled ? "on" : "off", nblocks, (unsigned long long)st.translated, (unsigned long long)st.fallbacks, (unsigned long long)st.invalidated,
        (unsigned long long)st.flushes, code_used / 256, (unsigned long long)st.blocks_run, (unsigned long long)st.faults, (unsigned long long)st.interp_steps);
}
#else
uint8_t jit_code_map[65536];
int  jit_available(void) { return 0; }
int  jit_run(int cycles) { (void)cycles; return emu68k_step_interp(); }
void jit_set_enabled(int on) { (void)on; }
int  jit_enabled(void) { return 0; }
void jit_flush_all(void) {}
void jit_note_write(uint32_t a) { (void)a; }
unsigned jit_stats(char *buf, unsigned n) { return (unsigned)snprintf(buf, n, "jit: not available on this host"); }
void jit_set_once(int on) { (void)on; }
int  jit_once_done(void) { return 1; }
int  jit_debug_translate(uint32_t pc, char *out, unsigned n) { (void)pc; snprintf(out, n, "n/a"); return -1; }
#endif
