<img width="1254" height="1254" alt="arcadeduck" src="https://github.com/user-attachments/assets/bde407af-90f7-49b3-b299-2ca7277913ef" />

# ArcadeDuck

ArcadeDuck is a PlayStation-based arcade hardware emulator project built from DuckStation.

It started as a way to improve *The Simpsons Bowling* on a Windows PC. That seemed reasonable at the time. Unfortunately, it has since turned into an unhealthy obsession involving Konami GV, flash chips, EEPROMs, SCSI, lightguns, trackballs, and a growing suspicion that this may be a source of anxiety and stress for the rest of my life.

So, naturally, I’m making it worse.

ArcadeDuck is focused on arcade systems that used PS1-derived hardware, not retail PlayStation console emulation. DuckStation already does console PlayStation extremely well. ArcadeDuck is for the weird late-90s arcade boards with CD-ROM drives, mystery daughterboards, operator menus, and hardware decisions that feel like someone said “ship it” during a fire drill.

My sarcastic nature heavily influences everything I do. Consider yourself warned.

## The Goal

Load arcade ROM sets.  
Boot arcade hardware.  
Play arcade games.

No console-first setup.  
No living-room PlayStation baggage.  
No babysitting BIOS dropdowns like it’s 1998 and Windows just ate your sound card driver again.

The end user experience will be simple:

1. Put BIOS files in the BIOS folder.
2. Put arcade ROM sets in the ROM folder.
3. Scan games.
4. Pick a game.
5. Play the damn game.

ArcadeDuck will handle system-specific BIOS selection, CHD paths, EEPROM, flash, NVRAM, and arcade board setup in the background wherever possible.

## Target Hardware

ArcadeDuck is aimed at PS1-based arcade platforms, including:

- Konami GV
- Konami System 573
- Sony ZN-1 / ZN-2
- Namco System 11
- Namco System 12
- Other practical PS1-derived arcade hardware

The current focus is Konami GV, especially normal arcade-control games like *The Simpsons Bowling*, *Dead Eye*, and *Beat the Champ*.

The weird printer, camera, heartbeat, sensor, and other “what the hell were they doing?” hardware is interesting, but it can wait. First priority is getting the main arcade experience solid before chasing every cursed daughterboard ever invented.

## ROM Layout

ArcadeDuck is being built around a MAME-style arcade layout.

```text
roms/
  simpbowl.zip
  simpbowl/
    829uaa02.chd
```

Per-game save data will live separately:

```text
nvram/
  simpbowl/
    eeprom
    flash0
    flash1
    flash2
    flash3
```

The emulator treats these as arcade sets, not loose console discs.

## Current Work

Current development is focused on:

- MAME zip + CHD loading
- Arcade BIOS boot flow
- SCSI CD-ROM access where needed
- EEPROM and flash/NVRAM persistence
- Trackball and lightgun input
- Redbook/CDDA audio
- Arcade-focused UI cleanup

The UI is moving away from console emulator language and toward arcade hardware language. PlayStation disc wording, manual region BIOS selection, memory card clutter, DVD/media leftovers, and other console-first assumptions are being hidden or removed where they do not belong.

Internally, the PS1 core is still doing the heavy lifting. Externally, the user gets an arcade emulator.

That is the whole point.

## Controls

ArcadeDuck will use arcade-style input setups:

- Joystick + buttons
- Trackball
- Lightgun
- Analog controls
- Coin / service / test inputs
- Game-specific controls where needed

Trackballs will behave like trackballs, not like a mouse cursor having an identity crisis.

Lightguns will support proper aiming, multiple players, off-screen reload, and normal user binding instead of hardcoded mouse button nonsense.

## Visual Style

ArcadeDuck is meant to feel like the era these boards came from:

Late-90s arcade hardware.  
Conversion cabinets.  
CRT bezels.  
Gunmetal panels.  
PCB traces.  
JAMMA labels.  
SCSI cables.  
CD-ROM drives that sound one bad seek away from retirement.

The visual direction is not modern neon gamer sludge, a spaceship dashboard, or a fake e-sports overlay.

It should feel chunky, specific, slightly ugly, and somehow cooler because of it.

## About the Project

This is being built by someone learning as he goes, not pretending to be an emulator wizard.

I break things, test things, read code that occasionally looks like ancient curses, and use AI to double-check my fuck-ups before they become permanent architecture.

The goal is serious, even if the tone is not: build ArcadeDuck carefully, keep it arcade-first, and avoid creating a pile of hacks future-me wants to throw into traffic.

## Status

ArcadeDuck is experimental.

Some things work.  
Some things almost work.  
Some things explode because a 1990s arcade board expected a very specific flash chip to respond in a very specific way and nobody told the emulator yet.

That is the fun part, unfortunately.

Use backups. Expect bugs. Verify your BIOS files.

Do not yell at the duck.
