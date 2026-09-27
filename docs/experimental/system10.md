# Namco System 10

**Status:** Experimental skeleton branch

This branch is the development area for Namco System 10 support in ArcadeDuck.

## Branch Base

System 10 currently branches from:

`experimental/gnet`

Base commit:

`54851f704 - Implement Taito G-Net communication link support`

## Current Status

No Namco System 10 hardware implementation is present yet.

The initial objective is to establish a clean, compile-safe framework that reuses ArcadeDuck's existing PlayStation-derived CPU, GPU, SPU, input, media, and arcade infrastructure where appropriate.

## Initial Development Areas

- System 10 machine configuration
- Memory map
- ROM / NAND / flash media handling
- Game-specific protection and decryption
- Standard arcade inputs
- Service / test handling
- NVRAM / persistent storage
- Audio routing
- Per-game hardware configuration

## Development Rules

- Keep the branch buildable whenever possible.
- Do not ungate games before meaningful boot progress exists.
- Avoid temporary hacks in shared emulator core code when behavior belongs in the System 10 implementation.
- Document unknown registers, protection behavior, timing assumptions, and approximations.
- Prefer small commits organized by subsystem.
- Experimental work should remain on this branch until ready to PR into `dev`.

## Suggested First Milestones

1. Establish the System 10 source directory and machine skeleton.
2. Reuse the existing ArcadeDuck PlayStation hardware implementation where applicable.
3. Implement the System 10 memory map.
4. Add ROM and nonvolatile media loading.
5. Reach deterministic startup logging.
6. Implement standard cabinet inputs and service functions.
7. Select one relatively simple title as the first boot target.
8. Add protection/decryption support as required by that title.

## Reference

Primary external reference:

MAME `src/mame/namco/namcos10.cpp`
