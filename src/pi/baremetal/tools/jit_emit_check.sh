#!/bin/sh
# jit_emit_check.sh: エンコーダの出力を aarch64 アセンブラの結果と突き合わせる(Linux 機で実行)
set -e
cd "$(dirname "$0")/../../core"
gcc -O1 -o /tmp/jit_emit_test jit_emit_test.c
/tmp/jit_emit_test > /tmp/emit_out.txt
fail=0; n=0
while IFS="$(printf '\t')" read -r asm words; do
    n=$((n+1))
    printf '%s\n' "$asm" > /tmp/e.s
    aarch64-linux-gnu-as -o /tmp/e.o /tmp/e.s 2>/dev/null || { echo "ASM FAIL: $asm"; fail=$((fail+1)); continue; }
    exp=$(aarch64-linux-gnu-objdump -d /tmp/e.o | awk '/^ +[0-9a-f]+:/{print $2}' | tr '\n' ' ')
    got=$(printf '%s' "$words" | sed 's/ *$//')
    exp=$(printf '%s' "$exp" | sed 's/ *$//')
    if [ "$got" != "$exp" ]; then echo "MISMATCH: $asm  got=[$got] expected=[$exp]"; fail=$((fail+1)); fi
done < /tmp/emit_out.txt
echo "checked $n, failures $fail"
[ "$fail" -eq 0 ]
