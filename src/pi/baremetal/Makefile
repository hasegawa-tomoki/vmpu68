# SPDX-License-Identifier: MIT
CIRCLEHOME = ../../third_party/circle

# declared first so it is the default goal; prerequisites added after Rules.mk
all:

# WITH_EMU=0 builds without the Musashi 68k emulator (debug/bisect aid)
WITH_EMU ?= 1

OBJS = main.o kernel.o webserver.o discovery.o config.o hw_port_circle.o hw.o vmpu68_io.o
ifeq ($(WITH_EMU),1)
OBJS += libc_stubs.o emu68k.o jit_a64.o m68kcpu.o m68kops.o softfloat.o
endif

LIBS = $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
       $(CIRCLEHOME)/addon/wlan/libwlan.a \
       $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/lib/net/libnet.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a
ifeq ($(WITH_EMU),1)
LIBS += /opt/arm-gnu-toolchain/aarch64-none-elf/lib/libm.a \
        libc-vmpu68.a
endif

DEFINE += -DVMPU68_BAREMETAL
# watchdog auto-reset (tryboot fallback); off until verified via tryboot
WITH_WATCHDOG ?= 0
ifeq ($(WITH_WATCHDOG),1)
DEFINE += -DVMPU68_WATCHDOG
endif
ifeq ($(WITH_EMU),1)
DEFINE += -DVMPU68_WITH_EMU
endif
# USB gadget vendor ID: 0x1209 (pid.codes) is acceptable for private testing;
# allocate a proper ID before distributing binaries
DEFINE += -DUSB_GADGET_VENDOR_ID=0x1209

include $(CIRCLEHOME)/Rules.mk

# kernel image + BSS must fit below KERNEL_MAX_SIZE (0x400000, set in
# circle's Config.mk) or the EL1 stack lands inside BSS and boot dies
# silently — this happened when Musashi's 512KB jump table joined BSS
all: $(TARGET).img check-size
.PHONY: all check-size
check-size: $(TARGET).img
	@end=$$(printf "%d" 0x$$($(PREFIX)nm $(TARGET).elf | awk '/ _end$$/{print $$1}')); \
	 lim=$$((0x80000 + 0x400000)); \
	 if [ $$end -ge $$lim ]; then \
	   echo "ERROR: _end ($$end) >= kernel region limit ($$lim) — raise KERNEL_MAX_SIZE"; \
	   exit 1; \
	 else echo "  SIZE  _end=$$end / limit=$$lim OK"; fi

hw.o: ../mgmtd/hw.c
	$(CC) $(CFLAGS) $(INCLUDE) -c -o $@ $<

vmpu68_io.o: ../pi/vmpu68_io.c
	$(CC) $(CFLAGS) $(INCLUDE) -c -o $@ $<

MUS = ../../third_party/Musashi
MUSFLAGS = -I$(MUS) -I../core/gen -DM68K_EMULATE_INT_ACK=M68K_OPT_ON

emu68k.o: ../core/emu68k.c
	$(CC) $(CFLAGS) $(INCLUDE) $(MUSFLAGS) -c -o $@ $<

jit_a64.o: ../core/jit_a64.c ../core/jit_a64_emit.h ../core/jit.h
	$(CC) $(CFLAGS) $(INCLUDE) $(MUSFLAGS) -c -o $@ $<

m68kcpu.o: $(MUS)/m68kcpu.c ../core/gen/m68kops.c
	$(CC) $(CFLAGS) $(INCLUDE) $(MUSFLAGS) -Wno-unused-variable -c -o $@ $<

m68kops.o: ../core/gen/m68kops.c
	$(CC) $(CFLAGS) $(INCLUDE) $(MUSFLAGS) -c -o $@ $<

softfloat.o: $(MUS)/softfloat/softfloat.c
	$(CC) $(CFLAGS) $(INCLUDE) $(MUSFLAGS) -c -o $@ $<

# private copy of newlib's libc with strtoul/strtoull localized: circle's
# util.o defines them too, and newlib's sscanf pulls in the same objects
# for _strtoul_r/_strtoull_r
libc-vmpu68.a: /opt/arm-gnu-toolchain/aarch64-none-elf/lib/libc.a
	cp $< $@
	$(PREFIX)objcopy -L strtoul -L strtoull $@

../core/gen/m68kops.c:
	$(MAKE) -C ../core gen/m68kops.c

-include $(DEPS)
