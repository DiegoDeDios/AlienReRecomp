# Building

Building is not needed to play: the [releases](https://github.com/DiegoDeDios/AlienReRecomp/releases)
have ready-made packages. This page is for working on the port.

## Contents

* [Game code: recompiled or interpreted](#game-code-recompiled-or-interpreted)
* [macOS](#macos)
* [Windows](#windows)
* [Linux](#linux)
* [Shaders](#shaders)
* [Releases and pre-releases](#releases-and-pre-releases)
* [Command-line options](#command-line-options)
* [Repository layout](#repository-layout)

## Game code: recompiled or interpreted

The CMake option `GAME_CODE` decides how the game's code runs:

* `recompiled` (the default when `src/gen` exists): the C that
  `tools/regen.sh` translated from your disc is compiled in. Fastest, and
  what development uses. Such a binary contains the game's code, so it is
  for your own use only.
* `interpreted` (the default otherwise): no game code in the binary. The
  game's executables are read from the player's disc image when they play
  and run by the port's MIPS interpreter, with the same enhancements
  (native hooks, precision geometry). This is what the releases use; it
  needs no disc image to build and runs the game at 2.5× real time or more
  on an Apple M1.

`--interpret` runs a recompiled build through the interpreter, for
comparison.

## macOS

Requirements: macOS, Xcode command line tools, CMake, Ninja, Python 3, SDL3
(`brew install sdl3 cmake ninja`).

```bash
tools/regen.sh
```

```bash
cmake -S . -B build -G Ninja && ninja -C build
```

`tools/regen.sh` extracts the disc filesystem into `disc/`, identifies the
Sony PsyQ library functions, and regenerates the recompiled C code in
`src/gen/`. It reads your disc image from `AlienResurrectionBinary/` and
needs `reference/psx_psyq_signatures`
(https://github.com/lab313ru/psx_psyq_signatures). Skip it for an
interpreted build.

```bash
tools/build_macos_app.sh
```

builds the release package `build-mac/release/AlienResurrection-<version>-macos.zip`:
a universal (Apple Silicon and Intel, macOS 11+) `AlienResurrection.app`
with SDL3's official framework inside, ad-hoc signed (not notarized), plus
a README and the licenses. The app icon is original artwork
(`tools/make_icons.sh`).

## Windows

The Windows release is cross-compiled from macOS (or Linux) with
MinGW-w64:

```bash
brew install mingw-w64
```

```bash
tools/build_windows.sh
```

The script downloads SDL3's official MinGW development package (pinned
version, checksum verified), builds into `build-win/` (interpreted; use
`GAME_CODE=recompiled` for a local recompiled build), and packages
`build-win/release/AlienResurrection-<version>-windows-x64.zip`: the
executable, `SDL3.dll`, a short README and the licenses. The executable
needs no other DLLs (Windows 10/11 ship the C runtime). Building natively on
Windows works the same way from an MSYS2 MinGW shell; MSVC is not supported
(the generated code uses GNU C extensions).

On Windows the renderer uses Vulkan and falls back to Direct3D 12 (Settings
→ Graphics API, or `--gpu-driver`); saves and `config.ini` live in
`%APPDATA%\AlienResurrectionPort\AlienResurrection\`, along with `alien.log`
when the game is not started from a terminal.

## Linux

```bash
tools/build_linux.sh
```

builds `build-linux-x86_64/release/AlienResurrection-<version>-linux-x86_64.tar.gz`
on a Linux machine (Ubuntu/Debian build dependencies:
`sudo tools/linux/install_deps_ubuntu.sh`). It compiles SDL3 from the
official source release (pinned, checksum verified) and ships it in `lib/`
next to the program, which finds it through its `$ORIGIN` run path; the C++
runtime is linked statically. Build on an old distribution so the result
runs on newer ones: the release is built on Ubuntu 22.04 (glibc 2.35).

On macOS (or anywhere with Docker, e.g. `brew install colima docker`),
`tools/build_linux_docker.sh` runs the same build in an Ubuntu 22.04
container (`ARCH=arm64` for arm64).

The renderer uses Vulkan; settings and saves live in
`~/.local/share/AlienResurrectionPort/AlienResurrection/`.

## Shaders

The renderer's shaders are written once in GLSL (`src/render/shaders/`)
and compiled for every backend into `src/render/shaders_gen.h` (SPIR-V for
Vulkan, HLSL for Direct3D 12 compiled to DXBC at start-up, MSL for Metal).
The generated header is committed; after editing a shader run
`python3 tools/compile_shaders.py` (needs `glslang` and `spirv-cross`).

## Releases and pre-releases

The release packages are `GAME_CODE=interpreted` builds; no disc image is
needed, so the GitHub Actions workflow (`.github/workflows/release.yml`)
builds them too: for every pull request into `main`, every push to `main`
and on demand. A pull request can only be merged once **Build (macOS)**,
**Build (Windows)** and **Build (Linux)** have passed (required checks of
`main`, with **Version bump** below); the packages of each run are kept as
workflow artifacts for 14 days, handy for trying a pull request before
merging it.

**Every merged pull request is a release.** The version lives in one place,
`PORT_VERSION` in `src/platform/launcher.h`, and each pull request into
`main` must raise it (the **Version bump** check,
`.github/workflows/version-check.yml`, which runs
`tools/check_version_bump.sh`; it can be run locally the same way). When
the merge reaches `main`, the release workflow tags the commit
`v<version>` and creates a **draft** GitHub release, **ARR v<version>**,
with the macOS, Windows and Linux packages, `packaging/RELEASE_NOTES.md`
and the list of pull requests since the last release. Review the draft and
publish it by hand. Pushing a `v*` tag by hand still works too (it must
match `PORT_VERSION`).

**Pre-releases** let people try a feature branch before its pull request
is merged. Run **Pre-release** (`.github/workflows/pre-release.yml`) on the
branch: *Actions > Pre-release > Run workflow* with *Use workflow from* set
to the branch, or `gh workflow run pre-release.yml --ref <branch>`. The
branch must contain `main` (merge `main` into it first) and raise
`PORT_VERSION` to a version not released yet. The workflow builds the
branch's head with the version marked `<version>-pre` (package names,
launcher, quick saves), tags it `v<version>-pre` and publishes the GitHub
pre-release **ARR v<version>-pre** with the packages and the branch's
commits. Running it again replaces that pre-release and moves the tag to
the branch's new head; the release of the merged pull request is not
affected.

## Command-line options

Options override `config.ini` for one run; most are for testing.

| Option | Meaning |
| --- | --- |
| `--play` | start the game directly, without the launcher |
| `--disc PATH` | disc image: the `.cue` file or its folder (default `AlienResurrectionBinary/` in the current folder) |
| `--skip-intro` | skip the boot loader and intro movies (same as `--exe ALIEN.BIN`) |
| `--aspect W:H` | display aspect: `16:9` (default), `16:10`, `21:9`, or `4:3` for the original look |
| `--fov DEG` | horizontal field of view in degrees, up to 100 (default `original`: 67 at 16:9, 53 at 4:3) |
| `--scale N` | internal resolution multiplier for the GPU renderer (default 4) |
| `--cpu-speed PCT` | emulated CPU speed (default 300 = steady 60 fps; 100 = original hardware) |
| `--no-vsync` | present without vsync (lowest latency, may tear) |
| `--no-pgxp` | original PS1 geometry: integer vertices and affine (warping) textures |
| `--sprint-speed PCT` | player speed while sprinting (default 190; 100 turns sprint off) |
| `--no-infinite-flashlight` | the flashlight battery runs down and recharges as in the original |
| `--enemy-damage PCT`, `--enemy-damage GROUP=PCT` | damage taken from all enemies / one kind (`alien_warriors`, `facehuggers`, `chestbursters`, `queen`, `newborn`, `marines`, `sentry_tank`, `acid_blood`), 0–200, default 100; repeatable |
| `--fullscreen`, `--show-fps`, `--sharp`, `--mouse-sens N` | as in the launcher |
| `--gpu-driver API` | `auto` (default), `vulkan` or `direct3d12` |
| `--original-stick` | both sticks through the game's own input handling (delay, 38% dead zone) |
| `--save-dir DIR` | folder for memory cards, `config.ini` and the quick save |
| `--renderer sw` | use the software rasterizer (pixel-accurate reference) |
| `--interpret` | run a recompiled build's game code through the interpreter |
| `--headless --frames N --shot F:file.png` | run without a window, write screenshots (ignores `config.ini`) |
| `--record F:N:STEP:PREFIX` | write N screenshots, one every STEP frames from frame F, as `PREFIX_0000.png`… |
| `--press F:button[:len]`, `--stick F:len:lx:ly:rx:ry`, `--mouse F:len:dx:dy`, `--fuzz F:seed` | scripted / random input (testing; `sprint` is a button name too; mouse motion in counts per frame) |
| `--launcher-shot file.png [--launcher-page settings\|controls]`, `--launcher-keys K1,K2,…` | render a launcher page to PNG / drive it with scripted keys (testing, docs) |
| `--overlay-keys F:T1,T2,…`, `--overlay-shot PREFIX` | from frame F, drive the in-game overlay with tokens: SDL key names (`F2`, `Down`, `Return`…), `pad:back+start[:ticks]` (a virtual controller), `WAITn`, and `SHOT` (writes the window with the overlay to `PREFIX_N.png`) |
| `--wav file.wav` | record the audio output |
| `--verify ADDR` | lockstep-check a recompiled function against the interpreter |
| `--watch ADDR[:LEN]` | log every store into a RAM range (debugging) |
| `--poke F:ADDR:VALUE[:SIZE]` | write a value (hex; 1, 2 or 4 bytes, default 4) into RAM at frame F (testing) |
| `--save-state F:PATH`, `--load-state F:PATH` | quick save to / load from PATH at frame F (testing; the frame counter is part of the state, so after a load frame-numbered options continue from the saved frame) |
| `--trace-bios`, `--log 3` | trace BIOS calls / verbose hardware logging |

## Repository layout

```
tools/            Python: ISO extraction, PS-X EXE loader, MIPS decoder,
                  control-flow analysis, PsyQ signature matching, recompiler
src/gen/          generated C (one function per MIPS function) — not committed
src/runtime/      CPU glue, scheduler, interrupts, HLE BIOS, GTE, interpreter,
                  lockstep verifier, disc/ISO9660 and memory card storage,
                  game enhancements (native.c)
src/hw/           PS1 hardware: DMA, timers, GPU command processor + software
                  rasterizer, CD-ROM, SPU, MDEC, controller port
src/render/       SDL GPU hardware renderer and presentation; shaders/ (GLSL)
                  and shaders_gen.h (compiled for Vulkan, Direct3D 12, Metal)
src/platform/     SDL3 frontend: launcher (Dear ImGui), settings/config.ini,
                  input bindings, window, audio, pacing, screenshots
third_party/      Dear ImGui (MIT), vendored
cmake/, packaging/  Windows cross-compilation toolchain and release files
docs/             building, architecture and reverse-engineering notes
docs/media/       README screenshots and animations
```

The README media are captured from your own disc with
`tools/make_media.sh` (headless runs of the port, scripted input, and
ffmpeg), so they can be regenerated after changes.
