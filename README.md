<img width="1254" height="1254" alt="arcadeduck" src="https://github.com/user-attachments/assets/bde407af-90f7-49b3-b299-2ca7277913ef" />

ArcadeDuck

ArcadeDuck is a PlayStation-based arcade hardware emulator project built from DuckStation.

It is focused on arcade systems that used PS1-derived hardware, not retail PlayStation console emulation. DuckStation already handles console stuff better than I ever will. ArcadeDuck is for the weird arcade boards from that late-90s window where everything had a CD-ROM drive, a fan, some mystery daughterboard, and at least one part that made arcade operators question their life choices.

The goal is simple:

Load arcade ROM sets.
Boot arcade hardware.
Play arcade games.

No console-first setup.
No living-room PlayStation baggage.
No babysitting BIOS dropdowns like it’s 1998 and Windows just ate your sound card driver again.

The Experience

ArcadeDuck is being built as an arcade-first emulator.

The end user experience will be:

1. Put BIOS files in the BIOS folder.
2. Put arcade ROM sets in the ROM folder.
3. Scan games.
4. Pick a game.
5. Play the damn game.

ArcadeDuck will handle system-specific BIOS selection, CHD paths, EEPROM, flash, NVRAM, and arcade board setup in the background wherever possible.

The interface will be built around arcade systems and arcade games, not PlayStation discs, memory cards, console regions, or other console clutter.

Target Hardware

ArcadeDuck is aimed at PS1-based arcade platforms, including:

- Konami GV
- Konami System 573
- Sony ZN-1 / ZN-2
- Namco System 11
- Namco System 12
- Other practical PS1-derived arcade hardware

The current focus is Konami GV, including games like:

- The Simpsons Bowling
- Dead Eye
- Beat the Champ
- Hyper Athlete
- Nagano Winter Olympics ’98

The weird sensor, printer, camera, heartbeat, and other “what the hell were they doing?” games are interesting, but they can wait. First priority is getting the normal arcade stuff solid before chasing every cursed daughterboard Konami felt like inventing.

ROM Layout

ArcadeDuck is being built around a MAME-style arcade layout.

roms/
  simpbowl.zip
  simpbowl/
    829uaa02.chd

Per-game save data will live separately:

nvram/
  simpbowl/
    eeprom
    flash0
    flash1
    flash2
    flash3

The emulator treats these as arcade sets, not loose console discs.

Current Development

The project is currently centered on Konami GV support.

Main work areas include:

- MAME zip + CHD loading
- Konami GV BIOS boot flow
- SCSI CD-ROM access
- EEPROM and flash/NVRAM handling
- Trackball and lightgun input
- Redbook/CDDA audio
- Arcade-focused UI cleanup

The Simpsons Bowling is the first major proof-of-concept, because apparently one fully working Simpsons bowling game was enough to justify forking an emulator. That sounds stupid until you remember this is exactly how hobby projects happen.

Controls

ArcadeDuck will use arcade-style input setups:

- Joystick + buttons
- Trackball
- Lightgun
- Analog controls
- Coin / service / test inputs
- Game-specific controls where needed

Trackballs will behave like trackballs, not like a mouse cursor pretending it has its life together.

Lightguns will support proper aiming, multiple players, off-screen reload, and normal user binding instead of hardcoded mouse button nonsense.

User Interface

The UI is moving away from console emulator language and toward arcade hardware language.

ArcadeDuck will hide or remove console-focused clutter where it does not belong:

- PlayStation retail disc wording
- Manual region BIOS selection
- Memory card screens unless a game actually needs something similar
- DVD/media playback leftovers
- Console-first assumptions

Internally, the PS1 core is still there doing the heavy lifting. Externally, the user gets an arcade emulator.

That is the whole point.

Visual Style

ArcadeDuck is meant to feel like the era these boards came from.

Late 90s arcade hardware.
Conversion cabinets.
CRT bezels.
Gunmetal panels.
PCB traces.
JAMMA labels.
SCSI cables.
CD-ROM drives that sound like they are one bad seek away from retirement.

The visual direction is not modern neon gamer sludge. It is not a spaceship dashboard. It is not a fake e-sports overlay.

It should feel like arcade hardware from that chunky, specific, slightly ugly, and somehow cooler era before everything tried to look like a smartphone app.

About the Project

This is being built by someone learning as he goes, not pretending to be an emulator wizard.

I break things, test things, read code that occasionally looks like ancient curses, and use AI to double-check my fuck-ups before they become permanent architecture.

The goal is still serious: build ArcadeDuck carefully, keep it arcade-first, and avoid creating a pile of hacks future-me wants to throw into traffic.

Near-Term Focus

Right now the focus is simple:

- Get Konami GV working well
- Finish key games like Dead Eye and Beat the Champ
- Clean up the code
- Improve arcade-style controls
- Keep stripping out console-emulator clutter

One board at a time. One weird 90s hardware problem at a time.

Status

ArcadeDuck is experimental.

Some things work.
Some things almost work.
Some things explode because a 1990s arcade board expected a very specific flash chip to respond in a very specific way and nobody told the emulator yet.

That is the fun part, unfortunately.

Use backups. Expect bugs. Verify your BIOS files.

Do not yell at the duck.
