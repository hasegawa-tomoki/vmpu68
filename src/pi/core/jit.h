/* SPDX-License-Identifier: MIT
 * jit.h — 68000 → AArch64 動的翻訳(JIT)のインタフェース。設計: docs/design/jit-plan.md
 * emu68k.c(エミュレータ本体)と jit_a64.c(翻訳器)の間の取り決め。 */
#ifndef JIT_H
#define JIT_H
#include <stdint.h>
#include <stddef.h>

/* ---- jit_a64.c が提供 ---- */
int  jit_available(void);                 /* このホストで翻訳器が組み込まれているか */
int  jit_run(int cycles);                 /* 68000 を cycles 分だけ JIT で走らせる(消費サイクル数を返す) */
void jit_set_enabled(int on);
int  jit_enabled(void);
void jit_flush_all(void);                 /* 翻訳済みコードを全て捨てる(リセット、再同期、ROM 無効化) */
void jit_note_write(uint32_t a);          /* シャドウの a が書き換わった(jit_code_map[a>>8] が立っているときだけ呼ぶ) */
extern uint8_t jit_code_map[65536];       /* 256 バイト単位: 翻訳済みコードを含む */
unsigned jit_stats(char *buf, unsigned n);
int  jit_debug_translate(uint32_t pc, char *out, unsigned n);   /* 翻訳だけ行い生成コードを 16 進で返す(デバッグ) */
void jit_set_once(int on);                /* 次の jit_run で 1 ブロック(または 1 命令)だけ実行して戻る */
int  jit_once_done(void);
int  jit_host_make_exec(void *p, unsigned long n);              /* ベアメタル側が提供: コードバッファのページを実行可能にする(0 = 成功) */
int  jit_debug_locate(uintptr_t host, char *out, unsigned n);   /* ホストアドレス → ブロック(デバッグ) */
void jit_debug_hist(char *out, unsigned n);                     /* 直近に実行したブロックの 68000 PC */
extern int jit_debug_trace;

/* ---- emu68k.c が JIT に提供 ---- */
int      emu68k_jit_fetch16(uint32_t pc);     /* 命令語(シャドウ RAM か充填済み ROM)、翻訳不可なら -1 */
int      emu68k_step_interp(void);            /* Musashi で 1 命令実行(割込み受付・例外もここ)。消費サイクル */
void     emu68k_jit_periodic(uint32_t pc);    /* 解釈実行の 8 命令ごとの処理(割込み線、DMA バッファ、停止判定) */
int      emu68k_jit_stop_req(void);           /* コアを止める要求(ベクタ領域到達、外部リセット、ブレーク) */
void     emu68k_jit_trace(uint32_t pc);       /* PC 履歴リングへ記録(ベクタ領域・ブレークの停止判定も) */
void     emu68k_jit_account(int n);           /* ブロック完了分のサイクルを cyc_now に反映 */
uint32_t jit_rd8 (uint32_t a);                /* メモリアクセス: 既存の経路をそのまま使い、フォールトを jit_fault に立てる */
uint32_t jit_rd16(uint32_t a);
uint32_t jit_rd32(uint32_t a);
void     jit_wr8 (uint32_t a, uint32_t v);
void     jit_wr16(uint32_t a, uint32_t v);
void     jit_wr32(uint32_t a, uint32_t v);
extern uint32_t jit_fault;                    /* 生成コードがヘルパ呼び出し後に確認する */
#endif
