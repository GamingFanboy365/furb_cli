# furb_cli: Furbtendulator, headless, for Linux

This package is Furbtendulator built as a command-line Linux program.
Furbtendulator is a fork of NintendulatorNRS, the reference emulator for
VT02/VT03/VT09/VT369 "OneBus" famiclones. It has no window. It runs a ROM for
a set number of frames with scripted input and writes out frames, sound,
video, savestates, movies and traces, so you can compare its output against
another emulator without taking screenshots. It was made for PocketVT (a GBA
emulator for VT famiclones) and works on its own.

The emulation code is Furbtendulator's own, unmodified. The additions are a
compatibility layer standing in for the Windows APIs and a `main()` that
drives frames. The GUI's features are reached through Furbtendulator's own
code: settings through its registry loader, controllers through its
controller code, sound through its mixer, movies through its movie dialogs.

## Running the included binary

`bin/furb_cli` is a 32-bit x86 Linux program. It needs 32-bit glibc 2.29 or
newer (Ubuntu 19.04+, Debian 11+ or equivalent). The C++ runtime is built in,
so nothing else is required. On Debian or Ubuntu, and under WSL:

```
sudo apt install libc6-i386          # or: sudo dpkg --add-architecture i386 && sudo apt install libc6:i386
bin/furb_cli game.nes --frames 900 --dump 300,899 --out shots/game
```

Keep `bin/` together: `bin/Mappers/*.so` are the iNES, FDS, NSF and VS mapper
packs, loaded at startup the way the Windows build loads `Mappers\*.dll`, and
the `.cfg` files, `BIOS/` and `samples/` sit next to the binary as they do
next to `Furbtendulator.exe`.

## Usage

```
furb_cli ROM --frames 900 --dump 300,899 --out shots/scr \
         --input "320-325:Start;500-900:Right" [--port 2] [--hashes] [--set VT03Palette=1]
```

ROM can be anything Furbtendulator opens: `.nes` (iNES/NES 2.0), `.unf`,
`.fds`/`.qd`, `.nsf`/`.nsfe`, Vs. System dumps. Run `furb_cli` with no
arguments for the option list.

## Frames

For each dumped frame it writes `PREFIX_fNNNN.ppm` (exactly the region
Furbtendulator's own screenshot saves), `.idx` (the PPU's raw uint16 palette
indices), `.txt` (`$2000-$20FF`, `$4100-$41FF` and palette RAM; the full
1024-entry palette on VT369) and `.ram` (CPU RAM). On VT369 in hi-res mode
(`$201C` bit 2) the `.idx` interleaves the even/odd half-pixel arrays, as the
GUI does. `--every K` dumps every Kth frame, `--hashes` prints one hash per
frame for frame-set tests, `--info` prints the ROM, its DIP switch and cheat
definitions and exits. Frames count from power-on starting at 0. The run is
deterministic: the same arguments give the same frames.

## Input script

`--input` takes `FRAMES:ACTION` items separated by `;` (the option can be
repeated). FRAMES is `N` or `FIRST-LAST`. Actions:

| Action | Meaning |
|--------|---------|
| `A+B+Select+Start+Up+Down+Left+Right+TurboA+TurboB`, `bN` | pad buttons (default pad: `--port`) |
| `p1:` .. `p4:`, `exp:` prefix | send them to that pad (Four Score / 4-player adapters: p1..p4) or the expansion device |
| `trigger` | Zapper and other light guns (button 0) |
| `key:NAME+NAME` | keyboard keys by DirectInput name (`a`, `return`, `space`, `f1`, `lshift`, ...) for Family BASIC, Subor and the other keyboards |
| `mouse:X,Y[+left][+right][+middle]` | cursor in NES pixels, for the Zapper aim, mice, Arkanoid paddle and tablet |
| `mic:LEVEL` | karaoke microphone peak level 0..1 |
| `cmd:reset`, `cmd:hardreset` | soft / hard reset |
| `cmd:coin1`, `cmd:coin2` | Vs. System coin slots |
| `cmd:button` | the plug-through device's button (the GUI's "press button" menu item) |
| `cmd:mic` | Famicom controller-2 microphone |
| `cmd:fds-insert`, `fds-eject`, `fds-next`, `fds-prev` | disk side changes |
| `cmd:save=FILE`, `cmd:load=FILE` | savestate to / from a file |
| `cmd:dip=VALUE` | set the DIP switches (hard-resets like the GUI) |
| `cmd:nsf-song=N` | select song N in the NSF player and press Play |
| `cmd:tape-play=FILE`, `tape-record=FILE`, `tape-stop` | Family BASIC / Subor data recorder |

## Devices

`--device PORT=TYPE` plugs a controller into `port1`, `port2`, `fs1`..`fs4`
or `exp`; `--list-devices` prints the type names (standard, zapper,
arkanoid, powerpad-a/b, fourscore, snes, vs-zapper, snes-mouse, subor-mouse,
and on the expansion port fami4play, hori-4play, family-basic-keyboard,
subor-keyboard, family-trainer-a/b, tablet, hyper shots, turbo-file,
sharp-c1-cassette and more). `port1=fourscore` or `exp=fami4play`/`hori-4play`
connects the extra pads so `p3:`/`p4:` work.

## Settings

Settings go through Furbtendulator's own registry loader, so every GUI
setting is available. `--config FILE` loads a regedit export of
`HKCU\SOFTWARE\Nintendulator` (UTF-16 or UTF-8, as regedit writes it) or a
plain `Name=value` file; `--set Name=Value` sets one value (numbers become
DWORDs, `"text"` becomes a string); `--save-config FILE` writes the settings
in effect at exit as a `.reg` file. `--palette FILE.pal` loads a custom
palette for every region.

`--data-dir DIR` (default `~/.furb_cli`) is the GUI's data folder: battery
saves go to `DIR/SRAM` on exit, as they do in the GUI. `--no-save` writes none.

## Audio, video, states, movies

`--wav FILE` records the sound Furbtendulator's mixer produces (48 kHz,
mono, 16-bit, the samples DirectSound would have played). `--avi FILE`
writes uncompressed video plus that audio at the console's exact frame rate
(NTSC 60.0988, PAL/Dendy 50.007). `--load-state FILE` starts from a
savestate; `cmd:save=` and `cmd:load=` save and load mid-run.
`--movie-record FILE.nmv` and `--movie-play FILE.nmv` drive the movie
dialogs, so the files are the GUI's own format.

## Game options, cheats, debugging

`--dip VALUE` sets the DIP switches (see `--info` for the definitions from
`dip.cfg`). `--cheat NAME` enables every cheat from `cheats.cfg` whose name
contains NAME (`all` for all). `--header BYTE=VALUE`, `mapper=N` or
`submapper=N` patch a copy of the iNES header before loading (the original
is untouched). `--trace FILE` writes the debugger's CPU trace log, for
`--trace-frames A-B` only if given.

## NSF

Furbtendulator's NSF player loads a tune and then waits for Play, in the GUI
too. `--nsf-song N` presses Play on song N at frame 0; `cmd:nsf-song=N`
switches song later.

## Data files

`bin/` holds `*.cfg` (`cheats.cfg`, `dip.cfg`, `fastload.cfg`),
`BIOS/` and `samples/` from the Furbtendulator release, next to the binary
where the GUI keeps them (`build.py` copies them from `Furbtendulator-src/bin-data`). Firmware is not included. For FDS put
`DISKSYS.ROM` in `BIOS/`; VT369 carts that need their internal ROM want
`BIOS/VT369-00.BIN` (file names are matched case-insensitively).

## Self-test

```
python3 selftest.py [--rom testroms/Scramble.nes]
```

Builds tiny ROMs, an NSF, a battery ROM and an FDS image on the fly and
checks every feature above: pads, Four Score, both 4-player adapters,
Zapper, microphones, Vs. coins, savestates, config round trip, trace, header
patch, WAV/AVI, NSF songs, battery saves, palette, cheats, DIP switches, FDS
commands. With `--rom` it also checks that a recorded movie and a savestate
continuation replay frame-identically on a real game.

## What is not tested

Real FDS disks (only a dummy BIOS was available), the keyboards, mice,
Arkanoid, tablet and data recorder (the plumbing works, no test ROM uses
them), and the VT369 hi-res dump path (Lucky Lawn Mower VT369 runs but never
enables hi-res). Anything tied to a real window (fullscreen, window size,
the palette editor's and debugger's views) has no meaning headless: the
settings those dialogs save can still be set with `--set` or `--config`.

## How the port works

`compat/` is a Win32/DirectX shim. Types and macros are real. The window,
DirectDraw and DirectInput are inert; the pieces the emulator depends on for
behaviour are implemented: the registry (a map loaded from and saved to
`.reg` files), dialogs (a dialog script registered for a template ID runs the
real dialog procedure against fake controls; unscripted dialogs are
cancelled), file pickers (answered from a queue), DirectSound (captures the
mixer's samples), the cursor and the microphone meter. Mapper packs (iNES,
FDS, NSF, VS) are built as `Mappers/*.so` and loaded through a dlopen-backed
`LoadLibrary`, exactly like the Windows `.dll`s; the executable exports only
`furb_host_lookup`, through which the packs reach the host's shim functions.
`furb_cli.cpp` replaces `WinMain` and runs the same CPU loop as `NES::Thread`.
Pad buttons are mapped to virtual joysticks (DirectInput device 2+port), so
keyboard devices, which read real key codes, never collide with them.
`prep_src.py` fixes `#include` case and backslashes for Linux and patches the
copy for three MSVC-only constructs g++ rejects (listed in the file; each
patch must match exactly once, so an upstream change fails loudly).

## Rebuilding from source

```
sudo apt install python3 g++-multilib
python3 build.py            # uses ./Furbtendulator-src, writes ./build/furb_cli and build/Mappers/
python3 selftest.py         # checks the fresh build
```

The first build takes a few minutes and later builds are incremental. The
source file lists come from Furbtendulator's own Visual Studio project files,
so a newer Furbtendulator drops in: point `--furb` at its `src` directory.
`build.py` never modifies the source tree; it copies it to `build/src` and
adjusts only that copy. The build is 32-bit (`-m32`) because the code assumes
Win32's 32-bit `long`.

## What is in the package

| Path | Contents |
|------|----------|
| `bin/` | the prebuilt program, mapper packs and data files (built on Ubuntu 20.04, GCC 9.4) |
| `Furbtendulator-src/` | Furbtendulator's complete source as released (minus Visual Studio's IntelliSense cache), plus `bin-data/` (the release's `.cfg` files and empty `BIOS/`, `samples/` folders) |
| `furb_cli.cpp` | the command-line front end (replaces `WinMain`) |
| `compat/`, `exports.list` | the Win32/DirectX compatibility layer |
| `build.py`, `prep_src.py` | build driver and source-copy fixups |
| `selftest.py` | feature self-test |
| `LICENSE` | GNU GPL v2, Furbtendulator's license |

Output is deterministic: the same arguments give the same frames, and this
GCC 9 build produces byte-identical dumps to a GCC 13 build of the same source.

## License and credits

Furbtendulator is Copyright (C) 2023-24 Furbland. It is based on
NintendulatorNRS, Copyright (C) 2017-2023 NewRisingSun, which is based on
Nintendulator, Copyright (C) 2002-2019 QMT Productions. All of them are
licensed under the GNU General Public License, version 2 or later; see
`LICENSE`. The furb_cli front end, the compatibility layer and the build
scripts were written for PocketVT and are distributed under the same license
as a derivative work. This package includes the complete corresponding source
code for the included binaries.
