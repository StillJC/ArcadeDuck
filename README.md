<img width="800" height="358" alt="arcadeduckbanner" src="https://github.com/user-attachments/assets/0ba3b928-5d72-4f01-831c-23d21a669306" />


# ArcadeDuck

ArcadeDuck is an arcade-focused emulator for hardware derived from the original PlayStation architecture.

It began as an attempt to improve *The Simpsons Bowling* on Windows. That was apparently not enough trouble, so it expanded into Konami GV, SCSI controllers, flash chips, EEPROMs, lightguns, trackballs, daughterboards, printers, and several other decisions made by arcade engineers in the 1990s.

ArcadeDuck is **not** intended to replace DuckStation as a retail PlayStation emulator. DuckStation already does that job extremely well. ArcadeDuck is for the strange arcade boards built around similar hardware and all the extra machinery attached to them.

## Current Status

ArcadeDuck is under active development and remains experimental.

The current focus is **Konami GV**. Implemented work includes:

- MAME-style ROM ZIP and CHD loading
- Automatic arcade BIOS selection
- Arcade game-title identification
- EEPROM, flash, and per-game NVRAM persistence
- NCR53CF96 SCSI CD-ROM communication and DMA
- Red Book CDDA playback
- Joystick, trackball, and lightgun controls
- *The Simpsons Bowling* flash and trackball daughterboard support
- *Tokimeki Memorial Oshiete Your Heart* sensor heartbeat and printer support
- Arcade-specific display geometry and 4:3 presentation
- Removal or hiding of console-oriented setup where it does not belong

Most of the Konami GV library is now running. Current work is centered on the remaining SCSI-related blockers, full-library regression testing, display cleanup, and preparing a stable GV preview build.

Some things work. Some things almost work. Some things are waiting for a 1990s peripheral to stop being mysterious.

## Target Hardware

Planned hardware families include:

- Konami GV
- Konami GQ
- Konami System 573
- Sony ZN-1 and ZN-2
- Namco System 11
- Namco System 12
- Other practical PS1-derived arcade systems

Support will be added one hardware family at a time. ArcadeDuck is not trying to become every emulator at once. That way lies madness, and we already have enough SCSI.

## Intended User Experience

The goal is straightforward:

1. Place the required BIOS files in the BIOS directory.
2. Place MAME-style arcade sets in the ROM directory.
3. Scan for games.
4. Select a game.
5. Play the damn game.

ArcadeDuck should handle system-specific BIOS selection, CHD discovery, EEPROM, flash, NVRAM, and board configuration automatically wherever practical.

## ROM and NVRAM Layout

ArcadeDuck uses a MAME-style set layout:

```text
roms/
  simpbowl.zip
  simpbowl/
    829uaa02.chd
```

Persistent board data is stored separately by set:

```text
nvram/
  simpbowl/
    eeprom
    flash0
    flash1
    flash2
    flash3
```

These are treated as arcade machines, not as loose PlayStation discs wearing fake mustaches.

## Controls

ArcadeDuck supports arcade-oriented input configurations, including:

- Joysticks and buttons
- Coin, service, and test inputs
- Trackballs
- Lightguns
- Analog controls
- Game-specific cabinet hardware

Trackballs should behave like trackballs. Lightguns should support normal binding, multiple players, proper aiming, and off-screen reload without requiring hardcoded mouse nonsense.

## Roadmap

Development is proceeding in controlled stages:

1. Finish the remaining Konami GV blockers.
2. Complete a full GV regression and presentation pass.
3. Release a clean GV-focused preview build.
4. Migrate the completed GV implementation to the final GPL-era DuckStation baseline.
5. Stabilize the migrated code before reorganizing arcade hardware under `src/core/arcade/`.
6. Continue with additional systems, beginning with the next selected hardware family.

One active implementation phase at a time. New hardware is exciting, but unfinished foundations are how future-me ends up throwing code into traffic.

## Codebase Provenance

ArcadeDuck was initialized from a source import of [t-dollaz's Simpsons Bowling Baby Phoenix DuckStation](https://github.com/t-dollaz/simpsons-bowling-baby-phoenix-duckstation), an intermediate desktop-oriented modification of [Arcade1Up's DuckStation Simpsons Bowling port](https://github.com/Arcade1Up/duckstation-sb).

The immediate source lineage is:

1. [DuckStation](https://github.com/stenzek/duckstation)
2. [Arcade1Up/duckstation-sb](https://github.com/Arcade1Up/duckstation-sb)
3. [t-dollaz/simpsons-bowling-baby-phoenix-duckstation](https://github.com/t-dollaz/simpsons-bowling-baby-phoenix-duckstation)
4. ArcadeDuck

The original Git ancestry was not preserved through that chain, so this is recorded as source provenance rather than direct fork ancestry.

Source-tree fingerprinting places ArcadeDuck's inherited DuckStation baseline in early-to-mid October 2021:

- Last confirmed included upstream commit: `a7096f033ecc` — October 2, 2021
- First confirmed excluded upstream commit: `98a1d4fe990b` — October 20, 2021

The exact intervening upstream commit cannot currently be proven.

ArcadeDuck is planned to migrate to the final GPL-era DuckStation baseline:

- Target commit: `25bc8a64803df7e702db66e0f11d7b7d0fdc99f2`
- Upstream date: December 23, 2023

ArcadeDuck does not and will not incorporate later non-GPL DuckStation source code.

## Credits

ArcadeDuck builds on work by:

- The DuckStation developers and contributors
- Arcade1Up's Simpsons Bowling DuckStation port contributors
- [t-dollaz](https://github.com/t-dollaz) for the intermediate desktop/GV source project used to initialize ArcadeDuck
- MAME developers whose hardware documentation and device implementations provide essential research references

Existing copyright, attribution, and license notices remain with their respective source files and projects.

## License

ArcadeDuck is distributed under the GNU General Public License. See [LICENSE](LICENSE) for details.

Use backups. Expect bugs. Verify your BIOS files.

Do not yell at the duck.
