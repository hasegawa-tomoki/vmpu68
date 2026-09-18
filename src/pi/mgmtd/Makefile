# SPDX-License-Identifier: MIT
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
MUS      = ../../third_party/Musashi
MFLAGS   = -I$(MUS) -I../core/gen -DM68K_EMULATE_INT_ACK=M68K_OPT_ON

mgmtd: mgmtd.c hw.c hw_port_linux.c hw.h web_ui.h ../pi/vmpu68_io.c ../pi/vmpu68.h ../core/emu68k.c
	$(MAKE) -C ../core gen/m68kops.c
	$(CC) $(CFLAGS) $(MFLAGS) -o $@ mgmtd.c hw.c hw_port_linux.c ../pi/vmpu68_io.c \
	  ../core/emu68k.c ../core/gen/m68kops.c $(MUS)/m68kcpu.c \
	  $(MUS)/softfloat/softfloat.c -lm -lpthread

web_ui.h: web/index.html
	python3 -c "import sys; d=open('web/index.html','rb').read(); \
	  f=open('web_ui.h','w'); f.write('/* generated from web/index.html */\n'); \
	  f.write('static const unsigned char web_ui_html[] = {'); \
	  f.write(','.join(str(b) for b in d)); \
	  f.write('};\nstatic const unsigned long web_ui_html_len = %d;\n' % len(d))"

clean:
	rm -f mgmtd web_ui.h

.PHONY: clean
