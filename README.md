# Alien Resurrection (PS1) — native port

A native macOS build of *Alien Resurrection* (USA, SLUS-00633) produced by
reverse engineering the original PlayStation executables. The game code is
statically recompiled from MIPS R3000A to C, and runs on a native runtime that
replaces the PS1 BIOS and models the hardware the Sony libraries talk to. A
modern GPU renderer (Metal via the SDL3 GPU API) draws the game at a higher
internal resolution.

You need your own copy of the game disc image (`.cue` + `.bin` tracks) in
`AlienResurrectionBinary/`. No game data is part of this repository.

## Status

| Area | State |
| --- | --- |
| Boot loader (`SLUS_006.33`): legal screen, FMV intro, LoadExec | working |
| Main executable (`ALIEN.BIN`): menus, options, level 1 gameplay | working (playtesting) |
| Hardware renderer (Metal, upscaled) | working |
| Widescreen (16:9 / 16:10 / 21:9) | working (hardware renderer) |
| Steady 60 fps (CPU headroom), vsync without tearing | working |
| Precision geometry (no vertex wobble, perspective-correct textures) | working (hardware renderer) |
| Sprint (hold Shift) | working |
| Launcher: disc selection, settings, control rebinding | working |
| Software renderer (reference) | working |
| FMV (STR/MDEC), XA audio, CD-DA | working (audio needs listening tests) |
| Memory card saves (`bu00:`): save, load, overwrite | working |
| DualShock analog + rumble protocol | implemented (no rumble output yet) |

## Build

Requirements: Xcode command line tools, CMake, Ninja, Python 3, SDL3
(`brew install sdl3 cmake ninja`).

```bash
tools/regen.sh
```

```bash
cmake -S . -B build -G Ninja && ninja -C build
```

`tools/regen.sh` extracts the disc filesystem into `disc/`, identifies the
Sony PsyQ library functions, and regenerates the recompiled C code in
`src/gen/`. It needs `reference/psx_psyq_signatures`
(https://github.com/lab313ru/psx_psyq_signatures).

## Run

```bash
./build/alien
```

The launcher opens first:

* **Select disc image**: pick your own `Alien Resurrection (USA).cue` (or one
  of its `.bin` tracks, or the folder). The launcher checks that it is the
  USA release (SLUS-00633) and then shows the game's title art, read from
  your disc. No game data ships with the port.
* **Settings**: widescreen and aspect ratio, internal resolution,
  precision geometry (PGXP), smooth scaling, vsync, fullscreen, frame rate
  display, sprint and sprint speed, skip intro movies, frame rate boost
  (emulated CPU speed).
* **Controls**: rebind every PlayStation button, the keyboard stick
  directions, sprint and fast forward, for keyboard/mouse (two keys per
  action) and controller. Mouse look and sensitivity are there too.
* **Start game**.

Everything is navigable with the mouse, the keyboard (arrows, Enter, Esc)
or a controller (D-pad, A, B). Settings are saved to `config.ini` next to
the memory cards (see below).

Command-line options override `config.ini` for one run (useful for
testing):

| Option | Meaning |
| --- | --- |
| `--play` | start the game directly, without the launcher |
| `--disc PATH` | disc image: the `.cue` file or its folder (default `AlienResurrectionBinary/` in the current folder) |
| `--skip-intro` | skip the boot loader and intro movies (same as `--exe ALIEN.BIN`) |
| `--aspect W:H` | display aspect: `16:9` (default), `16:10`, `21:9`, or `4:3` for the original look |
| `--scale N` | internal resolution multiplier for the GPU renderer (default 4) |
| `--cpu-speed PCT` | emulated CPU speed (default 300 = steady 60 fps; 100 = original hardware) |
| `--no-vsync` | present without vsync (lowest latency, may tear) |
| `--no-pgxp` | original PS1 geometry: integer vertices and affine (warping) textures |
| `--sprint-speed PCT` | player speed while sprinting (default 175; 100 turns sprint off) |
| `--fullscreen`, `--show-fps`, `--sharp`, `--mouse-sens N` | as in the launcher |
| `--save-dir DIR` | folder for memory cards and `config.ini` (default below) |
| `--renderer sw` | use the software rasterizer (pixel-accurate reference) |
| `--headless --frames N --shot F:file.png` | run without a window, write screenshots (ignores `config.ini`) |
| `--press F:button[:len]`, `--stick F:len:lx:ly:rx:ry`, `--fuzz F:seed` | scripted / random input (testing; `sprint` is a button name too) |
| `--launcher-shot file.png [--launcher-page settings\|controls]`, `--launcher-keys K1,K2,…` | render a launcher page to PNG / drive it with scripted keys (testing, docs) |
| `--wav file.wav` | record the audio output |
| `--verify ADDR` | lockstep-check a recompiled function against the interpreter |
| `--watch ADDR[:LEN]` | log every store into a RAM range (debugging) |
| `--trace-bios`, `--log 3` | trace BIOS calls / verbose hardware logging |

The game uses the DualShock "Dual Stick" scheme (left stick moves/strafes,
right stick looks/turns). Default keyboard and mouse bindings (all
changeable in **Controls**):

| Key | Pad | In game |
| --- | --- | --- |
| W A S D | left stick | move / strafe |
| Shift (hold) | | sprint (added; not in the original game) |
| mouse (click window to capture, F1 releases) | right stick | look / turn |
| arrows | d-pad | menu navigation, look / turn |
| left click / Left Ctrl | R1 | fire |
| right click / F | R2 | action |
| E | Circle | use |
| Space / C | Cross | menu accept, crouch |
| X / Backspace | Triangle | menu back, next item |
| Z | Square | previous item |
| R / Q | L1 / L2 | next / previous weapon |
| Esc / Enter | Start | pause |
| Tab | Select | look hold |
| V / B (middle click) | L3 / R3 | centre view / quick turn |
| ` (backtick) | | fast forward |

Fixed keys: F11 or Cmd+Ctrl+F toggles fullscreen, F12 takes a screenshot
(Shift+F12 dumps VRAM), F1 releases the mouse. Quit with Cmd+Q or by
closing the window.

Controllers are detected automatically; their sticks always drive the
analog sticks and the buttons follow the Controls page (default: the
PlayStation layout, left stick click = sprint, which latches until the stick
returns to centre; the game's L3 "centre view" is on V).

Saves are stored as standard 128 KiB memory card images in
`~/Library/Application Support/AlienResurrectionPort/AlienResurrection/`,
together with `config.ini`.
The game keeps its options (Invert Y, crosshair, vibration…) inside each save
and restores them from the newest save at startup. Invert Y defaults to off
in this port (the original defaulted to on).

## Layout

```
tools/            Python: ISO extraction, PS-X EXE loader, MIPS decoder,
                  control-flow analysis, PsyQ signature matching, recompiler
src/gen/          generated C (one function per MIPS function) — not committed
src/runtime/      CPU glue, scheduler, interrupts, HLE BIOS, GTE, interpreter,
                  lockstep verifier, disc/ISO9660 and memory card storage
src/hw/           PS1 hardware: DMA, timers, GPU command processor + software
                  rasterizer, CD-ROM, SPU, MDEC, controller port
src/render/       Metal/SDL_GPU hardware renderer and presentation
src/platform/     SDL3 frontend: launcher (Dear ImGui), settings/config.ini,
                  input bindings, window, audio, pacing, screenshots
third_party/      Dear ImGui (MIT), vendored
docs/             architecture and reverse-engineering notes
```

See `docs/ARCHITECTURE.md` for how the port works and `docs/FINDINGS.md` for
what has been learned about the game binary so far.
