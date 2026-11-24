# GdMap for Atari - build with the m68k-atari-mint cross toolchain
PREFIX  = m68k-atari-mint
CC      = $(PREFIX)-gcc
STRIP   = $(PREFIX)-strip
CFLAGS  = -O2 -Wall -fomit-frame-pointer
LIBS    = -lwindom -lgem -lldg

all: gdmap.prg gdmap020.prg gdmapcf.prg

# any 68000 machine (ST/STE, TT, Falcon, Hades, Milan, EmuTOS, ...)
gdmap.prg: gdmap.c
	$(CC) $(CFLAGS) -m68000 -o $@ $< $(LIBS)
	$(STRIP) $@

# 68020-68060 (TT, Falcon, Hades, Milan)
gdmap020.prg: gdmap.c
	$(CC) $(CFLAGS) -m68020-60 -o $@ $< $(LIBS)
	$(STRIP) $@

# ColdFire V4e (FireBee)
gdmapcf.prg: gdmap.c
	$(CC) $(CFLAGS) -mcpu=5475 -o $@ $< $(LIBS)
	$(STRIP) $@

clean:
	rm -f gdmap.prg gdmap020.prg gdmapcf.prg