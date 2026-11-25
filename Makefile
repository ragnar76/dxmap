# DxMap for Atari - build with the m68k-atari-mint cross toolchain
PREFIX  = m68k-atari-mint
CC      = $(PREFIX)-gcc
STRIP   = $(PREFIX)-strip
CFLAGS  = -O2 -Wall -fomit-frame-pointer
LIBS    = -lwindom -lgem -lldg

all: dxmap.prg dxmap020.prg dxmapcf.prg

# any 68000 machine (ST/STE, TT, Falcon, Hades, Milan, EmuTOS, ...)
dxmap.prg: dxmap.c
	$(CC) $(CFLAGS) -m68000 -o $@ $< $(LIBS)
	$(STRIP) $@

# 68020-68060 (TT, Falcon, Hades, Milan)
dxmap020.prg: dxmap.c
	$(CC) $(CFLAGS) -m68020-60 -o $@ $< $(LIBS)
	$(STRIP) $@

# ColdFire V4e (FireBee)
dxmapcf.prg: dxmap.c
	$(CC) $(CFLAGS) -mcpu=5475 -o $@ $< $(LIBS)
	$(STRIP) $@

clean:
	rm -f dxmap.prg dxmap020.prg dxmapcf.prg