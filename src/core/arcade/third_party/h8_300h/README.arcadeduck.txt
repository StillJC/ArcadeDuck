ArcadeDuck H8/300H bootstrap core
================================

Upstream:
https://github.com/celerizer/libh8300h.git

Pinned commit:
bf58e61971da58fe0d6c36629f59d177dedb0cae

Upstream license:
MIT (see LICENSE.txt)

ArcadeDuck adaptations:
- optional external byte-wide bus callbacks
- H8/300H advanced-mode 24-bit effective addresses
- 32-bit reset-vector fetch for the 24-bit program counter
- System 12 external-ROM PC range
- read/modify/write operations routed through the host bus
- frontend/network/logger dependencies replaced with local no-op stubs

Purpose:
This is the initial low-level H8/3002 execution bootstrap for Namco System 12.
The core is deliberately scheduled with approximate instruction timing until
firmware execution, the 0x3163 -> 0x7601 shared-RAM handshake, SCI/JVS traffic,
and the first missing peripheral/opcode requirements are measured.