TLCS-900/H interpreter imported from libretro/beetle-ngp-libretro commit
a50d5ac288a81f2104ddf43195a4efdd15c72227 (NeoPop-derived core).

ArcadeDuck changes:
- Neo Geo Pocket includes, BIOS HLE hook and reset defaults removed; memory,
  micro-DMA control registers and the reset vector are supplied by the host
  through tlcs900h_arcadeduck.c.
- HALT stops instruction fetch until the host accepts an interrupt.
- SWI n uses the generic TLCS-900/H vector at 0xFFFF00 + 4n instead of the
  Neo Geo Pocket system-call convention.
- Trailing whitespace removed.

The opcode implementation otherwise remains third-party code.
- ArcadeDuckTLCS900HDoState() (tlcs900h_arcadeduck.h/.c) adds save-state serialization of the global core.
