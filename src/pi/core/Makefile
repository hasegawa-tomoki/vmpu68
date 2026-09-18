# SPDX-License-Identifier: MIT
MUS     = ../../third_party/Musashi
CC     ?= gcc
CFLAGS ?= -O2 -Wall
MCFLAGS = $(CFLAGS) -I$(MUS) -Igen -DM68K_EMULATE_INT_ACK=M68K_OPT_ON

test_emu: gen/m68kops.c emu68k.c test_emu.c ../mgmtd/hw.c ../pi/vmpu68_io.c
	$(CC) $(MCFLAGS) -o $@ test_emu.c emu68k.c gen/m68kops.c $(MUS)/m68kcpu.c \
	  $(MUS)/softfloat/softfloat.c ../mgmtd/hw.c ../mgmtd/hw_port_linux.c ../pi/vmpu68_io.c -lm -lpthread

# m68kmake runs on the BUILD machine even when cross-compiling
HOSTCC ?= cc
# JIT の差分テスト(aarch64 Linux バイナリ、qemu-aarch64 で実行)
XCC ?= aarch64-linux-gnu-gcc
jit_test: gen/m68kops.c emu68k.c jit_a64.c jit_test.c ../mgmtd/hw.c ../pi/vmpu68_io.c
	$(XCC) -O1 -g -fno-omit-frame-pointer -static -Wall -I$(MUS) -Igen -DM68K_EMULATE_INT_ACK=M68K_OPT_ON -o $@ jit_test.c emu68k.c jit_a64.c gen/m68kops.c $(MUS)/m68kcpu.c \
	  $(MUS)/softfloat/softfloat.c ../mgmtd/hw.c ../mgmtd/hw_port_linux.c ../pi/vmpu68_io.c -lm -lpthread

gen/m68kops.c: $(MUS)/m68kmake.c $(MUS)/m68k_in.c
	mkdir -p gen
	$(HOSTCC) -O2 -o gen/m68kmake $(MUS)/m68kmake.c
	gen/m68kmake gen $(MUS)/m68k_in.c

clean:
	rm -rf gen test_emu

.PHONY: clean
