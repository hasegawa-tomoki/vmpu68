/* SPDX-License-Identifier: MIT
 * jit_a64_emit.h — AArch64 命令エンコーダ(JIT 用の最小集合)。
 * 各関数は 32 ビット命令語を 1 つ書く。レジスタ番号は 0-30、31 は zr/sp(命令による)。
 * 検証: jit_emit_test.c(aarch64-linux-gnu-as の出力と突き合わせ)。 */
#ifndef JIT_A64_EMIT_H
#define JIT_A64_EMIT_H
#include <stdint.h>

typedef struct {
    uint32_t *buf;      /* 出力先 */
    uint32_t  n;        /* 書いた命令数 */
    uint32_t  cap;      /* 容量(命令数) */
    int       overflow;
} a64_t;

static inline void a64_put(a64_t *e, uint32_t w)
{
    if (e->n < e->cap) e->buf[e->n] = w; else e->overflow = 1;
    e->n++;
}
static inline uint32_t *a64_pc(a64_t *e) { return e->buf + e->n; }   /* 次の命令の位置 */

enum { XZR = 31, WZR = 31, SP = 31 };
/* 条件コード */
enum { CC_EQ = 0, CC_NE = 1, CC_CS = 2, CC_CC = 3, CC_MI = 4, CC_PL = 5, CC_VS = 6, CC_VC = 7,
       CC_HI = 8, CC_LS = 9, CC_GE = 10, CC_LT = 11, CC_GT = 12, CC_LE = 13, CC_AL = 14 };

/* ---- ロード/ストア(符号なしイミディエイト、スケール済み) ---- */
static inline void a64_ldr_w (a64_t *e, unsigned rt, unsigned rn, unsigned off) { a64_put(e, 0xB9400000u | ((off / 4) << 10) | (rn << 5) | rt); }
static inline void a64_str_w (a64_t *e, unsigned rt, unsigned rn, unsigned off) { a64_put(e, 0xB9000000u | ((off / 4) << 10) | (rn << 5) | rt); }
static inline void a64_ldr_x (a64_t *e, unsigned rt, unsigned rn, unsigned off) { a64_put(e, 0xF9400000u | ((off / 8) << 10) | (rn << 5) | rt); }
static inline void a64_str_x (a64_t *e, unsigned rt, unsigned rn, unsigned off) { a64_put(e, 0xF9000000u | ((off / 8) << 10) | (rn << 5) | rt); }
static inline void a64_ldrh_w(a64_t *e, unsigned rt, unsigned rn, unsigned off) { a64_put(e, 0x79400000u | ((off / 2) << 10) | (rn << 5) | rt); }
static inline void a64_strh_w(a64_t *e, unsigned rt, unsigned rn, unsigned off) { a64_put(e, 0x79000000u | ((off / 2) << 10) | (rn << 5) | rt); }
static inline void a64_ldrb_w(a64_t *e, unsigned rt, unsigned rn, unsigned off) { a64_put(e, 0x39400000u | (off << 10) | (rn << 5) | rt); }
static inline void a64_strb_w(a64_t *e, unsigned rt, unsigned rn, unsigned off) { a64_put(e, 0x39000000u | (off << 10) | (rn << 5) | rt); }
static inline void a64_ldrsh_w(a64_t *e, unsigned rt, unsigned rn, unsigned off) { a64_put(e, 0x79C00000u | ((off / 2) << 10) | (rn << 5) | rt); }
static inline void a64_ldrsb_w(a64_t *e, unsigned rt, unsigned rn, unsigned off) { a64_put(e, 0x39C00000u | (off << 10) | (rn << 5) | rt); }
/* レジスタオフセット: [xn, xm] */
static inline void a64_ldr_w_r (a64_t *e, unsigned rt, unsigned rn, unsigned rm) { a64_put(e, 0xB8606800u | (rm << 16) | (rn << 5) | rt); }
static inline void a64_str_w_r (a64_t *e, unsigned rt, unsigned rn, unsigned rm) { a64_put(e, 0xB8206800u | (rm << 16) | (rn << 5) | rt); }
static inline void a64_ldrh_w_r(a64_t *e, unsigned rt, unsigned rn, unsigned rm) { a64_put(e, 0x78606800u | (rm << 16) | (rn << 5) | rt); }
static inline void a64_strh_w_r(a64_t *e, unsigned rt, unsigned rn, unsigned rm) { a64_put(e, 0x78206800u | (rm << 16) | (rn << 5) | rt); }
static inline void a64_ldrb_w_r(a64_t *e, unsigned rt, unsigned rn, unsigned rm) { a64_put(e, 0x38606800u | (rm << 16) | (rn << 5) | rt); }
static inline void a64_strb_w_r(a64_t *e, unsigned rt, unsigned rn, unsigned rm) { a64_put(e, 0x38206800u | (rm << 16) | (rn << 5) | rt); }
/* STP/LDP x, [sp, #imm]! / [sp], #imm(imm は 8 の倍数、-512..504) */
static inline void a64_stp_pre (a64_t *e, unsigned rt, unsigned rt2, unsigned rn, int imm) { a64_put(e, 0xA9800000u | (((uint32_t)(imm / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt); }
static inline void a64_ldp_post(a64_t *e, unsigned rt, unsigned rt2, unsigned rn, int imm) { a64_put(e, 0xA8C00000u | (((uint32_t)(imm / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt); }
static inline void a64_stp_off (a64_t *e, unsigned rt, unsigned rt2, unsigned rn, int imm) { a64_put(e, 0xA9000000u | (((uint32_t)(imm / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt); }
static inline void a64_ldp_off (a64_t *e, unsigned rt, unsigned rt2, unsigned rn, int imm) { a64_put(e, 0xA9400000u | (((uint32_t)(imm / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt); }

/* ---- 即値ロード ---- */
static inline void a64_movz_w(a64_t *e, unsigned rd, unsigned imm16, unsigned hw) { a64_put(e, 0x52800000u | (hw << 21) | (imm16 << 5) | rd); }
static inline void a64_movk_w(a64_t *e, unsigned rd, unsigned imm16, unsigned hw) { a64_put(e, 0x72800000u | (hw << 21) | (imm16 << 5) | rd); }
static inline void a64_movn_w(a64_t *e, unsigned rd, unsigned imm16, unsigned hw) { a64_put(e, 0x12800000u | (hw << 21) | (imm16 << 5) | rd); }
static inline void a64_movz_x(a64_t *e, unsigned rd, unsigned imm16, unsigned hw) { a64_put(e, 0xD2800000u | (hw << 21) | (imm16 << 5) | rd); }
static inline void a64_movk_x(a64_t *e, unsigned rd, unsigned imm16, unsigned hw) { a64_put(e, 0xF2800000u | (hw << 21) | (imm16 << 5) | rd); }
/* 32 ビット定数を最短でロード */
static inline void a64_mov_w_imm(a64_t *e, unsigned rd, uint32_t v)
{
    if ((v & 0xFFFF0000u) == 0) a64_movz_w(e, rd, v & 0xFFFF, 0);
    else if ((v & 0xFFFF) == 0) a64_movz_w(e, rd, v >> 16, 1);
    else if ((v & 0xFFFF0000u) == 0xFFFF0000u) a64_movn_w(e, rd, (~v) & 0xFFFF, 0);
    else { a64_movz_w(e, rd, v & 0xFFFF, 0); a64_movk_w(e, rd, v >> 16, 1); }
}
static inline void a64_mov_x_imm(a64_t *e, unsigned rd, uint64_t v)
{
    a64_movz_x(e, rd, v & 0xFFFF, 0);
    if ((v >> 16) & 0xFFFF) a64_movk_x(e, rd, (v >> 16) & 0xFFFF, 1);
    if ((v >> 32) & 0xFFFF) a64_movk_x(e, rd, (v >> 32) & 0xFFFF, 2);
    if ((v >> 48) & 0xFFFF) a64_movk_x(e, rd, (v >> 48) & 0xFFFF, 3);
}

/* ---- 算術・論理(レジスタ) ---- */
static inline void a64_add_w (a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x0B000000u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_adds_w(a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x2B000000u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_sub_w (a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x4B000000u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_subs_w(a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x6B000000u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_add_x (a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x8B000000u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_and_w (a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x0A000000u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_ands_w(a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x6A000000u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_orr_w (a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x2A000000u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_eor_w (a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x4A000000u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_orn_w (a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x2A200000u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_mov_w (a64_t *e, unsigned rd, unsigned rm) { a64_put(e, 0x2A0003E0u | (rm << 16) | rd); }
static inline void a64_mvn_w (a64_t *e, unsigned rd, unsigned rm) { a64_put(e, 0x2A2003E0u | (rm << 16) | rd); }
static inline void a64_neg_w (a64_t *e, unsigned rd, unsigned rm) { a64_put(e, 0x4B0003E0u | (rm << 16) | rd); }
static inline void a64_negs_w(a64_t *e, unsigned rd, unsigned rm) { a64_put(e, 0x6B0003E0u | (rm << 16) | rd); }
static inline void a64_cmp_w (a64_t *e, unsigned rn, unsigned rm) { a64_subs_w(e, WZR, rn, rm); }
static inline void a64_tst_w (a64_t *e, unsigned rn, unsigned rm) { a64_ands_w(e, WZR, rn, rm); }
/* 即値(12 ビット、シフトなし) */
static inline void a64_add_w_imm (a64_t *e, unsigned rd, unsigned rn, unsigned imm12) { a64_put(e, 0x11000000u | (imm12 << 10) | (rn << 5) | rd); }
static inline void a64_adds_w_imm(a64_t *e, unsigned rd, unsigned rn, unsigned imm12) { a64_put(e, 0x31000000u | (imm12 << 10) | (rn << 5) | rd); }
static inline void a64_sub_w_imm (a64_t *e, unsigned rd, unsigned rn, unsigned imm12) { a64_put(e, 0x51000000u | (imm12 << 10) | (rn << 5) | rd); }
static inline void a64_subs_w_imm(a64_t *e, unsigned rd, unsigned rn, unsigned imm12) { a64_put(e, 0x71000000u | (imm12 << 10) | (rn << 5) | rd); }
static inline void a64_add_x_imm (a64_t *e, unsigned rd, unsigned rn, unsigned imm12) { a64_put(e, 0x91000000u | (imm12 << 10) | (rn << 5) | rd); }
static inline void a64_sub_x_imm (a64_t *e, unsigned rd, unsigned rn, unsigned imm12) { a64_put(e, 0xD1000000u | (imm12 << 10) | (rn << 5) | rd); }
static inline void a64_cmp_w_imm (a64_t *e, unsigned rn, unsigned imm12) { a64_subs_w_imm(e, WZR, rn, imm12); }
/* 論理即値(ビットマスク形式、N=0 の 32 ビット): immr/imms を直接指定 */
static inline void a64_and_w_bm(a64_t *e, unsigned rd, unsigned rn, unsigned immr, unsigned imms) { a64_put(e, 0x12000000u | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
static inline void a64_orr_w_bm(a64_t *e, unsigned rd, unsigned rn, unsigned immr, unsigned imms) { a64_put(e, 0x32000000u | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
static inline void a64_eor_w_bm(a64_t *e, unsigned rd, unsigned rn, unsigned immr, unsigned imms) { a64_put(e, 0x52000000u | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
/* よく使うマスク: 0xFF = (immr 0, imms 7)、0xFFFF = (0, 15)、0x80 = (25, 0)、0x100 = (24, 0) */
static inline void a64_and_w_ff  (a64_t *e, unsigned rd, unsigned rn) { a64_and_w_bm(e, rd, rn, 0, 7); }
static inline void a64_and_w_ffff(a64_t *e, unsigned rd, unsigned rn) { a64_and_w_bm(e, rd, rn, 0, 15); }
static inline void a64_and_w_80  (a64_t *e, unsigned rd, unsigned rn) { a64_and_w_bm(e, rd, rn, 25, 0); }
static inline void a64_and_w_100 (a64_t *e, unsigned rd, unsigned rn) { a64_and_w_bm(e, rd, rn, 24, 0); }

/* ---- シフト・ビットフィールド ---- */
static inline void a64_lsl_w_imm(a64_t *e, unsigned rd, unsigned rn, unsigned s) { a64_put(e, 0x53000000u | (((32 - s) & 31) << 16) | ((31 - s) << 10) | (rn << 5) | rd); }
static inline void a64_lsr_w_imm(a64_t *e, unsigned rd, unsigned rn, unsigned s) { a64_put(e, 0x53007C00u | (s << 16) | (rn << 5) | rd); }
static inline void a64_asr_w_imm(a64_t *e, unsigned rd, unsigned rn, unsigned s) { a64_put(e, 0x13007C00u | (s << 16) | (rn << 5) | rd); }
static inline void a64_ubfx_w(a64_t *e, unsigned rd, unsigned rn, unsigned lsb, unsigned width) { a64_put(e, 0x53000000u | (lsb << 16) | ((lsb + width - 1) << 10) | (rn << 5) | rd); }
static inline void a64_sbfx_w(a64_t *e, unsigned rd, unsigned rn, unsigned lsb, unsigned width) { a64_put(e, 0x13000000u | (lsb << 16) | ((lsb + width - 1) << 10) | (rn << 5) | rd); }
static inline void a64_bfi_w (a64_t *e, unsigned rd, unsigned rn, unsigned lsb, unsigned width) { a64_put(e, 0x33000000u | (((32 - lsb) & 31) << 16) | ((width - 1) << 10) | (rn << 5) | rd); }
static inline void a64_bfxil_w(a64_t *e, unsigned rd, unsigned rn, unsigned lsb, unsigned width) { a64_put(e, 0x33000000u | (lsb << 16) | ((lsb + width - 1) << 10) | (rn << 5) | rd); }
static inline void a64_sxtb_w(a64_t *e, unsigned rd, unsigned rn) { a64_sbfx_w(e, rd, rn, 0, 8); }
static inline void a64_sxth_w(a64_t *e, unsigned rd, unsigned rn) { a64_sbfx_w(e, rd, rn, 0, 16); }
static inline void a64_uxtb_w(a64_t *e, unsigned rd, unsigned rn) { a64_ubfx_w(e, rd, rn, 0, 8); }
static inline void a64_uxth_w(a64_t *e, unsigned rd, unsigned rn) { a64_ubfx_w(e, rd, rn, 0, 16); }
static inline void a64_lslv_w(a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x1AC02000u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_lsrv_w(a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x1AC02400u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_asrv_w(a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x1AC02800u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_rorv_w(a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x1AC02C00u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_rev16_w(a64_t *e, unsigned rd, unsigned rn) { a64_put(e, 0x5AC00400u | (rn << 5) | rd); }
static inline void a64_rev_w  (a64_t *e, unsigned rd, unsigned rn) { a64_put(e, 0x5AC00800u | (rn << 5) | rd); }
static inline void a64_ror_w_imm(a64_t *e, unsigned rd, unsigned rn, unsigned s) { a64_put(e, 0x13800000u | (rn << 16) | (s << 10) | (rn << 5) | rd); }   /* EXTR rd, rn, rn, #s */
/* ---- 乗除 ---- */
static inline void a64_mul_w  (a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x1B007C00u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_smull_x(a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x9B207C00u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_umull_x(a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x9BA07C00u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_udiv_w (a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x1AC00800u | (rm << 16) | (rn << 5) | rd); }
static inline void a64_sdiv_w (a64_t *e, unsigned rd, unsigned rn, unsigned rm) { a64_put(e, 0x1AC00C00u | (rm << 16) | (rn << 5) | rd); }
/* ---- 条件 ---- */
static inline void a64_cset_w(a64_t *e, unsigned rd, unsigned cond) { a64_put(e, 0x1A9F07E0u | ((cond ^ 1) << 12) | rd); }
static inline void a64_csel_w(a64_t *e, unsigned rd, unsigned rn, unsigned rm, unsigned cond) { a64_put(e, 0x1A800000u | (rm << 16) | (cond << 12) | (rn << 5) | rd); }
static inline void a64_mrs_nzcv(a64_t *e, unsigned rt) { a64_put(e, 0xD53B4200u | rt); }
/* ---- 分岐(オフセットはバイト、命令位置基準) ---- */
static inline void a64_b(a64_t *e, int32_t off)      { a64_put(e, 0x14000000u | (((uint32_t)off >> 2) & 0x3FFFFFFu)); }
static inline void a64_bcond(a64_t *e, unsigned cond, int32_t off) { a64_put(e, 0x54000000u | ((((uint32_t)off >> 2) & 0x7FFFFu) << 5) | cond); }
static inline void a64_cbz_w (a64_t *e, unsigned rt, int32_t off) { a64_put(e, 0x34000000u | ((((uint32_t)off >> 2) & 0x7FFFFu) << 5) | rt); }
static inline void a64_cbnz_w(a64_t *e, unsigned rt, int32_t off) { a64_put(e, 0x35000000u | ((((uint32_t)off >> 2) & 0x7FFFFu) << 5) | rt); }
static inline void a64_tbz_w (a64_t *e, unsigned rt, unsigned bit, int32_t off) { a64_put(e, 0x36000000u | (bit << 19) | ((((uint32_t)off >> 2) & 0x3FFFu) << 5) | rt); }
static inline void a64_tbnz_w(a64_t *e, unsigned rt, unsigned bit, int32_t off) { a64_put(e, 0x37000000u | (bit << 19) | ((((uint32_t)off >> 2) & 0x3FFFu) << 5) | rt); }
static inline void a64_blr(a64_t *e, unsigned rn) { a64_put(e, 0xD63F0000u | (rn << 5)); }
static inline void a64_br (a64_t *e, unsigned rn) { a64_put(e, 0xD61F0000u | (rn << 5)); }
static inline void a64_ret(a64_t *e)             { a64_put(e, 0xD65F03C0u); }
static inline void a64_nop(a64_t *e)             { a64_put(e, 0xD503201Fu); }
/* 後から埋める分岐: 位置 idx の命令のオフセットを、現在位置向けに書き直す */
static inline void a64_patch_b(a64_t *e, uint32_t idx)      { int32_t off = (int32_t)(e->n - idx) * 4; e->buf[idx] = (e->buf[idx] & 0xFC000000u) | (((uint32_t)off >> 2) & 0x3FFFFFFu); }
static inline void a64_patch_b19(a64_t *e, uint32_t idx)    { int32_t off = (int32_t)(e->n - idx) * 4; e->buf[idx] = (e->buf[idx] & 0xFF00001Fu) | ((((uint32_t)off >> 2) & 0x7FFFFu) << 5); }
static inline void a64_patch_b14(a64_t *e, uint32_t idx)    { int32_t off = (int32_t)(e->n - idx) * 4; e->buf[idx] = (e->buf[idx] & 0xFFF8001Fu) | ((((uint32_t)off >> 2) & 0x3FFFu) << 5); }
#endif
