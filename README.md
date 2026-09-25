# furb: Furbtendulator for Linux

Furbtendulator (https://github.com/FurblandChannel/Furbtendulator) is a fork of
NintendulatorNRS, the reference emulator for VT02/VT03/VT09/VT32/VT369
"OneBus" famiclones, UM6578 and the other obscure NES-compatible hardware and
bootleg mappers.  It is a Windows program.  This package builds it for Linux
as two programs from one set of emulator objects:

| Program | What it is |
|---------|------------|
| `furb` | Furbtendulator as you know it: its window, menus, dialogs, debugger, sound, keyboard and gamepads (GTK 3 and SDL 2) |
| `furb_cli` | headless: runs a ROM for a set number of frames with scripted input and writes frames, sound, video, savestates, movies and traces, for comparing against other emulators and for debugging without screenshots (made for PocketVT and AI-assisted debugging) |

The emulation code is Furbtendulator's own, unmodified; both programs run the
same machine and `furb_cli`'s output is byte-for-byte reproducible.  What
Windows supplies -- windows, dialogs, DirectDraw, DirectSound, DirectInput,
the registry -- is supplied by a compatibility layer (`compat/`, `gui/`).  It
builds natively on 64-bit Linux and as 32-bit x86, and the builds produce
identical output (tested on x86_64 and i386; arm64 and other 64-bit targets are
expected to work, untested).

## Running the included binaries

`bin/` holds x86_64 builds made on Ubuntu 24.04: they need glibc 2.38 or newer
(Ubuntu 24.04, Debian 13, Fedora 39 or equivalent; WSL's default Ubuntu is
fine).  `furb` also needs GTK 3 and SDL 2, which desktop systems have:

```
sudo apt install libgtk-3-0 libsdl2-2.0-0     # Debian / Ubuntu, if missing
bin/furb game.nes                             # the GUI
bin/furb_cli game.nes --frames 900 --dump 300,899 --out shots/game
```

On an older system, or another architecture, build from source (below; about
two minutes).  Keep `bin/` together: `bin/Mappers/*.so` are the iNES, FDS, NSF
and VS mapper packs, loaded at startup the way the Windows build loads
`Mappers\*.dll`, and the `.cfg` files, `BIOS/` and `samples/` sit next to the
programs as they do next to `Furbtendulator.exe`.

## furb: the GUI

```
furb [ROM] [--data-dir DIR] [--config FILE] [--set Name=Value]...
```

Everything is Furbtendulator's own: the menu bar, the accelerator keys (F2
run, F3 stop, F4/Shift+F4 reset, F5/F8 save/load state, F6/F7 state slot,
Ctrl+F frame step, Tab slowdown, Alt+Enter fullscreen, F10 header editor,
Ctrl+F1/Ctrl+F2 the CPU and PPU debuggers, Ctrl+D DIP switches,
Shift+Ctrl+C cheats, ...), and every dialog -- input setup with key capture,
the palette editor, the debuggers with their live pattern/nametable/sprite
views, the header editor, cheats, DIP switches, the NSF player, movies and
the rest.  They run Furbtendulator's dialog procedures against GTK widgets
built from its `.rc` dialog templates (the mapper packs' windows from the
packs' own `.rc` scripts).
ROMs can also be dropped on the window.

Settings are kept in `~/.config/furbtendulator/settings.reg` (the
`HKCU\SOFTWARE\Nintendulator` key as a regedit export, so a Windows export
can be copied in) and written on exit; `--config` uses another file and
`--set` overrides a value for this run.  Savestates, battery saves, FDS disk
changes and dumps go to `~/.local/share/furbtendulator/Nintendulator` (or
`DIR/Nintendulator` with `--data-dir`), the layout Furbtendulator uses under
`%APPDATA%\Nintendulator`.

On the first run, controller 1 is mapped to the keyboard: arrows, X = A,
Z = B, S and A = turbo A and B, Enter = Start, Right Shift = Select.  Input >
Setup changes it; joysticks and gamepads are listed there too.  Sound plays
through SDL, and the emulation is paced by it exactly as by DirectSound on
Windows.

Not available: AVI capture (it uses Video for Windows; `furb_cli --avi` records
video), CopyNES plugins, and the karaoke microphone's level (the Famicom
microphone, Ctrl+M, works).

## furb_cli: the headless program

```
furb_cli ROM --frames 900 --dump 300,899 --out shots/scr \
         --input "320-325:Start;500-900:Right" [--port 2] [--hashes] [--set VT03Palette=1]
```

ROM can be anything Furbtendulator opens: `.nes` (iNES/NES 2.0), `.unf`,
`.fds`/`.qd`, `.nsf`/`.nsfe`, Vs. System dumps. Run `furb_cli` with no
arguments for the option list.
### Frames

For each dumped frame it writes `PREFIX_fNNNN.ppm` (exactly the region
Furbtendulator's own screenshot saves), `.idx` (the PPU's raw uint16 palette
indices), `.txt` (`$2000-$20FF`, `$4100-$41FF` and palette RAM; the full
1024-entry palette on VT369) and `.ram` (CPU RAM). On VT369 in hi-res mode
(`$201C` bit 2) the `.idx` interleaves the even/odd half-pixel arrays, as the
GUI does. `--every K` dumps every Kth frame, `--hashes` prints one hash per
frame for frame-set tests, `--info` prints the ROM, its DIP switch and cheat
definitions and exits. Frames count from power-on starting at 0. The run is
deterministic: the same arguments give the same frames.
### Input script

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
### Devices

`--device PORT=TYPE` plugs a controller into `port1`, `port2`, `fs1`..`fs4`
or `exp`; `--list-devices` prints the type names (standard, zapper,
arkanoid, powerpad-a/b, fourscore, snes, vs-zapper, snes-mouse, subor-mouse,
and on the expansion port fami4play, hori-4play, family-basic-keyboard,
subor-keyboard, family-trainer-a/b, tablet, hyper shots, turbo-file,
sharp-c1-cassette and more). `port1=fourscore` or `exp=fami4play`/`hori-4play`
connects the extra pads so `p3:`/`p4:` work.
### Settings

Settings go through Furbtendulator's own registry loader, so every GUI
setting is available. `--config FILE` loads a regedit export of
`HKCU\SOFTWARE\Nintendulator` (UTF-16 or UTF-8, as regedit writes it) or a
plain `Name=value` file; `--set Name=Value` sets one value (numbers become
DWORDs, `"text"` becomes a string); `--save-config FILE` writes the settings
in effect at exit as a `.reg` file. `--palette FILE.pal` loads a custom
palette for every region.

`--data-dir DIR` (default `~/.furb_cli`) is the Windows GUI's data folder: battery
saves go to `DIR/SRAM` on exit, as they do in the GUI. `--no-save` writes none.
### Audio, video, states, movies

`--wav FILE` records the sound Furbtendulator's mixer produces (48 kHz,
mono, 16-bit, the samples DirectSound would have played). `--avi FILE`
writes uncompressed video plus that audio at the console's exact frame rate
(NTSC 60.0988, PAL/Dendy 50.007). `--load-state FILE` starts from a
savestate; `cmd:save=` and `cmd:load=` save and load mid-run.
`--movie-record FILE.nmv` and `--movie-play FILE.nmv` drive the movie
dialogs, so the files are the GUI's own format.
### Game options, cheats, debugging

`--dip VALUE` sets the DIP switches (see `--info` for the definitions from
`dip.cfg`). `--cheat NAME` enables every cheat from `cheats.cfg` whose name
contains NAME (`all` for all). `--header BYTE=VALUE`, `mapper=N` or
`submapper=N` patch a copy of the iNES header before loading (the original
is untouched). `--trace FILE` writes the debugger's CPU trace log, for
`--trace-frames A-B` only if given.
### NSF

Furbtendulator's NSF player loads a tune and then waits for Play, in the GUI
too. `--nsf-song N` presses Play on song N at frame 0; `cmd:nsf-song=N`
switches song later.
### Data files

`bin/` holds `*.cfg` (`cheats.cfg`, `dip.cfg`, `fastload.cfg`),
`BIOS/` and `samples/` from the Furbtendulator release, next to the binary
where the GUI keeps them (`build.py` copies them from `Furbtendulator-src/bin-data`). Firmware is not included. For FDS put
`DISKSYS.ROM` in `BIOS/`; VT369 carts that need their internal ROM want
`BIOS/VT369-00.BIN` (file names are matched case-insensitively).
### Self-test

```
python3 selftest.py [--rom testroms/Scramble.nes]
```

Builds tiny ROMs, an NSF, a battery ROM and an FDS image on the fly and
checks every feature above: pads, Four Score, both 4-player adapters,
Zapper, microphones, Vs. coins, savestates, config round trip, trace, header
patch, WAV/AVI, NSF songs, battery saves, palette, cheats, DIP switches, FDS
commands. With `--rom` it also checks that a recorded movie and a savestate
continuation replay frame-identically on a real game.

## Determinism and 32/64-bit builds

The Windows sources assume `long` is 32 bits; `prep_src.py` rewrites the
build copy's `long` to `int` (identical on i386), so the native 64-bit build
behaves exactly like the 32-bit one.  The palette generator, the sound filters
and the expansion-sound tables call `sin`, `pow`, `exp` and friends, whose last
bit differs between libm builds (i386 glibc uses x87 code); they are routed to
a bundled copy of musl's portable implementations (`compat/libm`, MIT
licensed, built without FMA contraction), so i386, x86_64 and arm64 compute
the same colours and samples.  Two upstream reads of uninitialised memory
(the first audio sample; CPU/PPU members the constructors never set) are
closed: the sound buffer and every `new` allocation start zeroed.

`tools/compare_builds.py A/furb_cli B/furb_cli ROMS...` runs two builds over
a set of ROMs and reports any difference in video hashes, WAV, RAM or
registers.  `tools/fuzzrom.py DIR` generates OneBus test ROMs for VT02, VT03,
VT09, VT32, VT369 and UM6578 that hammer the VT PPU/CPU registers, palettes,
4bpp modes and sprites.  The 32-bit and 64-bit builds agree on all 311 ROMs of
the public NES test-ROM collection (christopherpow/nes-test-roms) plus these.

## What is not tested

Real FDS disks (only a dummy BIOS was available), the keyboards, mice,
Arkanoid, tablet and data recorder (the plumbing works, no test ROM uses
them), and the VT369 hi-res dump path (Lucky Lawn Mower VT369 runs but never
enables hi-res). Anything tied to a real window (fullscreen, window size,
the palette editor's and debugger's views) has no meaning in `furb_cli`: the
settings those dialogs save can still be set with `--set` or `--config`, and
`furb` has them all.  The GUI's
sound was tested with SDL's dummy driver (no sound card here), its joystick
support without a joystick, and its windows under Xvfb with openbox (the
dialogs tried: input setup, controller config with key capture, palette,
header editor, CPU and PPU debuggers, cheats, DIP switches, NSF player, movie
recording, status window, file picker, fullscreen; the tape dialogs and the
mapper packs' game-specific configuration windows were not);
`tools/gui_smoke.py` repeats the basic checks (a ROM runs at 60 fps, the
keyboard reaches the pad, dialogs open and close, a clean exit saves the
settings).

## How the port works

`compat/` is a Win32/DirectX shim.  `compat/winapi.def` lists the Win32
functions Furbtendulator calls that need a host (windows, messages, menus,
dialogs, GDI, cursor, threads); `windows.h` declares them from it, the mapper
packs get forwarders to the executable generated from it, and the executable
exports them to the packs by name (`furb_host_lookup`, its only exported
symbol).  Each program brings its own host:

`furb_cli` (`furb_cli.cpp`, `compat/host_cli.cpp`) replaces `WinMain` with a
loop running the same CPU loop as `NES::Thread`.  Its window calls are inert;
dialogs are headless (a dialog script registered for a template ID runs the
real dialog procedure against fake controls, unscripted dialogs are
cancelled); file pickers are answered from a queue; DirectSound captures the
mixer's samples; pad buttons are mapped to virtual joysticks (DirectInput
device 2+port), so keyboard devices, which read real key codes, never collide
with them.

`furb` (`gui/`) runs Furbtendulator's own `WinMain` and emulation thread on a
real implementation: windows and a message queue on the GTK main loop, menus
and accelerator tables and dialog templates compiled from each module's `.rc`
script by `gui/rc2cpp.py` (a module's table is its `HINSTANCE`, so the packs'
dialogs come from their own `.rc`), the dialog controls as GTK widgets, GDI on
cairo, DirectDraw as memory surfaces shown in the window, DirectSound on SDL
audio, DirectInput from the windows' key and mouse events and SDL joysticks.
As on Windows, a window call from the emulation thread runs on the main thread
while the caller waits.

Both share the registry (`compat/host_reg.cpp`: a map loaded from and saved
to `.reg` files).  `prep_src.py` copies the sources to the build folder, fixes
`#include` case and backslashes, rewrites `long` (above) and patches four
constructs g++ rejects or that read uninitialised memory (listed in the file;
each patch must match exactly once, so an upstream change fails loudly).

## Rebuilding from source

```
sudo apt install python3 g++ libgtk-3-dev libsdl2-dev
python3 build.py            # uses ./Furbtendulator-src, writes build/furb, build/furb_cli, build/Mappers/
python3 selftest.py         # checks furb_cli
python3 tools/gui_smoke.py  # checks furb (needs xvfb xdotool openbox)
```

`--no-gui` builds `furb_cli` only (no GTK/SDL needed); `--m32` builds for
32-bit x86 (`g++-multilib`, and i386 GTK/SDL development packages for the
GUI).  The first build takes a few minutes and later builds are incremental.
The source file lists come from Furbtendulator's own Visual Studio project
files, so a newer Furbtendulator drops in: point `--furb` at its `src`
directory.  `build.py` never modifies the source tree; it copies it to
`build/src` and adjusts only that copy.

## What is in the package

| Path | Contents |
|------|----------|
| `bin/` | the prebuilt programs (`furb`, `furb_cli`), mapper packs and data files (x86_64, built on Ubuntu 24.04, GCC 13) |
| `Furbtendulator-src/` | Furbtendulator's complete source as released (minus Visual Studio's IntelliSense cache), plus `bin-data/` (the release's `.cfg` files and empty `BIOS/`, `samples/` folders) |
| `furb_cli.cpp` | the command-line front end (replaces `WinMain`) |
| `compat/` | the Win32/DirectX compatibility layer, `furb_cli`'s host, and the bundled musl math (`compat/libm`) |
| `gui/` | the GUI's Win32 layer on GTK 3 / SDL 2, and `rc2cpp.py`, the resource compiler |
| `exports.list` | the executables' one exported symbol |
| `build.py`, `prep_src.py` | build driver and source-copy fixups |
| `selftest.py` | `furb_cli` feature self-test |
| `tools/` | `compare_builds.py`, `fuzzrom.py`, `gui_smoke.py` |
| `LICENSE` | GNU GPL v2, Furbtendulator's license |

## License and credits

Furbtendulator is Copyright (C) 2023-24 Furbland. It is based on
NintendulatorNRS, Copyright (C) 2017-2023 NewRisingSun, which is based on
Nintendulator, Copyright (C) 2002-2019 QMT Productions. All of them are
licensed under the GNU General Public License, version 2 or later; see
`LICENSE`. The front ends, the compatibility layer, the GUI layer and the
build scripts were written for PocketVT and are distributed under the same
license as a derivative work.  `compat/libm` is a subset of musl 1.2.4,
Copyright (C) 2005-2020 Rich Felker et al., under the MIT license
(`compat/libm/COPYRIGHT.musl`).  This package includes the complete
corresponding source code for the included binaries.
