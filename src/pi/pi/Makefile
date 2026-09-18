CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra

all: flash68 buscheck

flash68: flash68.c vmpu68_io.c vmpu68.h
	$(CC) $(CFLAGS) -o $@ flash68.c vmpu68_io.c

buscheck: buscheck.c vmpu68_io.c vmpu68.h
	$(CC) $(CFLAGS) -o $@ buscheck.c vmpu68_io.c

clean:
	rm -f flash68 buscheck

.PHONY: all clean
