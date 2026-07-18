# Known Issues

ArcadeDuck POC Preview is an early proof of concept focused exclusively on Konami GV arcade hardware.

## CPU recompiler

The CPU Interpreter is the supported and default CPU mode for this preview.

The Recompiler remains experimental in the inherited codebase. Some games may fail to boot, behave incorrectly, or display incorrect geometry when it is enabled. Recompiler accuracy work is deferred to the planned codebase migration.

## Tokimeki peripheral emulation

The Tokimeki Memorial Oshiete Your Heart daughterboard, pulse sensor, GSR input, heartbeat, and printer interfaces are emulated sufficiently for gameplay.

Their responses are based on available hardware documentation, MAME references, gameplay observations, and practical approximation. They should not yet be considered cycle-accurate reproductions of the original peripherals.

## Save-state compatibility

Save states are intended for use only with the same ArcadeDuck build. Compatibility with future builds, especially builds produced after the DuckStation code migration, is not guaranteed.

EEPROM, flash, and other normal persistent storage should be preferred over save states.

## Supported hardware

This preview supports Konami GV only.

Konami GQ, System 573, Sony ZN, Namco System 11, Namco System 12, and other planned arcade platforms are not included in this release.

## Required game data

ArcadeDuck does not include copyrighted BIOS, ROM, CD-ROM, or CHD data.

Users must provide:

- `bios/konamigv.zip` containing `999a01.7e`
- Supported non-merged MAME ROM sets
- Matching CHD images where required

## Reporting problems

Reports and suggestions will be collected for reference during the migration to the newer GPL-era DuckStation codebase.

The POC codebase is entering maintenance-only status. Large features and architectural changes will generally not be implemented directly in this branch.
