<img width="800" height="358" alt="ArcadeDuck banner" src="https://github.com/user-attachments/assets/0ba3b928-5d72-4f01-831c-23d21a669306" />

# ArcadeDuck

**Proof of Concept Preview — Konami GV**

ArcadeDuck is an arcade-focused emulator for hardware derived from the original PlayStation architecture.

It began as an attempt to improve *The Simpsons Bowling* on Windows. That was apparently not enough trouble, so it expanded into Konami GV, SCSI controllers, flash chips, EEPROMs, lightguns, trackballs, daughterboards, printers, and several other decisions made by arcade engineers in the 1990s.

ArcadeDuck is **not** a replacement for DuckStation and is **not** intended to be a general PlayStation console emulator. DuckStation already handles retail PlayStation software extremely well. ArcadeDuck is for the unusual arcade boards built around related hardware and all the extra machinery attached to them.

## Preview Status

This release is a **proof of concept**, not an alpha or a long-term development build.

Its purpose is to demonstrate the current Konami GV implementation before that work is migrated to the final GPL-era DuckStation codebase. The inherited core is from 2021 and has known CPU timing and recompiler limitations.

This preview is intentionally limited:

- Konami GV is the only supported arcade family in this build.
- The **Interpreter** CPU mode is the default and recommended setting.
- The Recompiler remains available for testing, but is experimental and can cause boot, timing, or display-transition failures.
- *Crazy Cross* currently uses a narrowly scoped Timer 1 compatibility workaround. It is temporary and will be removed and reinvestigated after the codebase migration.
- Presentation and resolution transitions may not be perfect in every title or mode.
- Reports will be collected for the migration and future implementation. Only critical proof-of-concept fixes are expected on this inherited branch.

No ROMs, BIOS files, CHDs, keys, or other copyrighted game data are included.

## Implemented Konami GV Features

The current proof of concept includes:

- MAME-style ROM ZIP and CHD loading
- Direct Konami GV BIOS loading from `bios/konamigv.zip`
- Automatic arcade BIOS validation and selection
- Arcade game-title identification
- EEPROM, flash, and per-game NVRAM persistence
- NCR53CF96 SCSI CD-ROM communication and DMA
- Red Book CDDA playback
- Joystick, trackball, and lightgun controls
- *The Simpsons Bowling* flash and trackball daughterboard support
- *Tokimeki Memorial Oshiete Your Heart* heartbeat and printer support
- Arcade-specific display geometry and 4:3 presentation
- Optional 16:9 stretching
- Per-game bezel assignment and loading
- Arcade-oriented input and cabinet controls
- Removal or hiding of console-oriented setup where it does not belong

## Tested Games

The following MAME set names have been used for proof-of-concept regression testing:

- `simpbowl`
- `kdeadeye`
- `btchamp`
- `hyperath`
- `powyak96`
- `weddingr`
- `lacrazyc`
- `susume`
- `nagano98`
- `naganoj`
- `tmosh`
- `tmoshs`
- `tmoshsp`
- `tmoshspa`

A listed game being tested does not mean every edge case, peripheral, or presentation mode is complete.

## Quick Start

1. Extract ArcadeDuck to its own directory.
2. Place `konamigv.zip` in `bios/`. The ZIP must contain `999a01.7e`.
3. Place MAME-style non-merged game ZIPs in `roms/`.
4. Place each CHD in a subdirectory matching its set name.
5. Launch `ArcadeDuck.exe`.
6. Scan the ROM directory and start a game.

The packaged build uses portable mode, so settings, logs, NVRAM, and generated board data remain within the ArcadeDuck directory.

### Example ROM Layout

```text
roms/
  simpbowl.zip
  simpbowl/
    829uaa02.chd
```

### Example Persistent Data Layout

```text
nvram/
  simpbowl/
    eeprom
    flash0
    flash1
    flash2
    flash3
```

ArcadeDuck treats these as arcade machines, not as loose PlayStation discs wearing fake mustaches.

## CPU Mode

The proof-of-concept build defaults to:

```text
CPU Execution Mode: Interpreter
```

This is intentional. In the inherited CPU core, the Recompiler can fail in titles that work correctly under the Interpreter. It remains selectable for comparison and testing, but it is not the recommended mode for this preview.

Do not report a game as broken until it has been tested with the Interpreter.

## Controls

ArcadeDuck supports arcade-oriented input configurations, including:

- Joysticks and buttons
- Coin, service, and test inputs
- Trackballs
- Lightguns
- Analog controls
- Game-specific cabinet hardware

The standard arcade button layout is:

```text
Button 1: Cross
Button 2: Circle
Button 3: R2
Button 4: Square
Button 5: Triangle
Button 6: R1
```

Some games and cabinet devices require game-specific mappings.

## Logs and Issue Reports

The normal application log is:

```text
arcadeduck.log
```

When reporting a problem, include:

- The MAME set name
- Whether the ROM set is non-merged
- The CHD filename
- CPU execution mode
- Renderer
- ArcadeDuck build/tag
- Relevant section of `arcadeduck.log`
- Clear reproduction steps

Do not upload ROMs, BIOS files, CHDs, keys, or other copyrighted game data.

Issues, requests, and suggestions are welcome as reference material for the migration. This proof-of-concept branch is not the long-term development base, so reports may be documented without being fixed here.

## Target Hardware

Planned hardware families include:

- Konami GV
- Konami GQ
- Konami System 573
- Sony ZN-1 and ZN-2
- Namco System 11
- Namco System 12
- Other practical PS1-derived arcade systems

These are roadmap targets, not claims of support in the current preview.

## POC Preview Documentation

- [Konami GV compatibility](COMPATIBILITY.md)
- [Known issues and limitations](KNOWN-ISSUES.md)
- [Report a bug, compatibility result, or suggestion](https://github.com/StillJC/ArcadeDuck/issues/new/choose)
## Development Roadmap

1. Release and document the Konami GV proof of concept.
2. Migrate the completed GV implementation to the final GPL-era DuckStation baseline.
3. Fully regression-test and stabilize the migrated implementation.
4. Reorganize arcade hardware under `src/core/arcade/` only after migration stability.
5. Continue with additional arcade hardware families one system at a time.

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
