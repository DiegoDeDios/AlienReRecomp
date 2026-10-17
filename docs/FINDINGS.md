# Reverse-engineering findings

Observations about the *Alien Resurrection* (USA) disc and executables,
gathered while building the port. Addresses are KSEG0 (`0x80…`); the
executables are actually linked in KUSEG (`0x000…`), so pointers stored in
game data look like `0x000A63FC` rather than `0x800A63FC`.

## Disc layout (track 1, MODE2/2352)

| LBA | File | Notes |
| --- | --- | --- |
| 23 | `SYSTEM.CNF` | `BOOT = cdrom:\SLUS_006.33;1`, TCB 4, EVENT 10, stack `0x801FFF00` |
| 24 | `SLUS_006.33` | boot loader, 66 KiB |
| 58 | `ALIEN.BIN` | main game, 610 KiB |
| 364 | `PACK.BIN` | 113 MiB archive of all game assets |
| 58044 | `MOVIE/SPLASH.BIN` | 320×240×24-bit image (+12-byte header) |
| 58157… | `MOVIE/0.STR … 40.STR` | MDEC video with interleaved XA audio (raw 2352-byte sectors) |
| 114961 | `FEMUSIC.RAW` | ISO entry that points at **CD-DA track 2** (front-end music) |
| 121463 | `CREDITS.RAW` | points at CD-DA track 3 |
| 129348 | `SILENCE.WAV` | points at CD-DA track 4 |

## `PACK.BIN` archive

* `u32 count` (182) followed by `count` entries of 20 bytes:
  `char name[12]; u32 size; u32 offset` (offsets 2 KiB-aligned).
* Asset types by extension:
  * `L??S??.WAD` — level `??` section `??` data (levels 00–12 and 30–33; the
    `S00` file of each level is the largest, likely shared level data).
  * `*.CVS` — character/cutscene data (`RIPLEY`, `CALL`, `CHRISTIE`,
    `DISTEPHA`, `MARINES`, `NEWBORN`, `SQUEEN`, `UFATHER`, `ALIEN1`).
  * `*.TIM` — loading screens `ELOADnn`, `ECALIB` (not TIMs, see below)
    and standard PS1 TIM images for the front end (`FEGRUKnn`).
  * `*.TXF` — HUD panels and end sequences (`PANELn`, `ENDSEQn`).
  * `*.TUK` — UK-English text banks (`TEXTUK`, `LEVnnUK`, `CRUDUK`); the
    executable builds names from `LEV%02d%c%c.TUK` / `TEXT%c%c.TUK` with a
    two-letter language code (`UKFRGRITSPJP` table in the executable).
  * `DEMOn.DEM` — attract-mode input recordings.
  * `FRONTEND.CVB`, `UFATHER.CVD`.

## Textures and images

* **Level files** (`L??S??.WAD`): a 2 KiB header with the chunk count at
  `+0x10` and, from `+0x20`, 8-byte directory entries `{char tag[4]
  (stored reversed); u32 size}`. Chunks follow from `+0x800`, each 2 KiB
  aligned: `TXF` (textures), `MAP`, `CVB`, `BIN` (starts with `CMD `) and
  one `CVD` per character set (`RIPLEY`, `MARINES`, `ALIEN1`, …).
* **TXF** (level `TXF` chunks, `PANELn.TXF` HUD and effects, `ENDSEQn.TXF`):
  `u8 2, u8 3, u16 page count`, then at `+0x14` one 8-byte entry per page
  `{u16 0; u16 x; u16 y; u16 0}`, the page's VRAM position in 16-bit units.
  In level chunks the pages (64×256 VRAM units = 32 KiB each) follow
  consecutively from `+0x800` and are copied into VRAM unchanged (checked
  byte for byte for `L01S00`); a table of unknown purpose comes after them,
  its size at `+0x08`. The `PANEL` files' page data is not laid out
  consecutively (not decoded further).
* **VRAM in level 1:** two framebuffers top left; textures shared by the
  whole level (`L01S00`) at x 0–319, y 256; the current section's textures
  at x 448–703, y 256; HUD, fonts, item icons and effects (`PANEL1`) at
  x 768–1023.
* **Texture organisation:** level pages are 4-bit sheets of mostly 64×64
  tiles (some 32×32), each tile drawn with its own 16-colour palette. The
  palettes sit in the bottom rows of the pages (y ≈ 420–511 at x = 0, 448,
  768 and 960; also y ≈ 222–251 at x = 960), and one tile can be drawn with
  several palettes (one level page uses 18). HUD pages mix 4- and 8-bit. The
  page, palette and UVs of each polygon come from the geometry, not from
  the TXF; all textured drawing seen in level 1 uses polygons, no sprites.
* **Loading screens** (`ELOADnn.TIM`, `ECALIB.TIM`): `u16 width, u16
  height`, 8 bytes, then 15-bit pixels (512×240 and 384×240).
  `FEGRUK05/06.TIM` are standard 16-bit 256×256 TIMs placed at VRAM
  (768, 256).
* The character files (`*.CVS`) hold no texture data; character skins are
  probably in each level's TXF (inferred, not yet checked with a character
  on screen).

## Boot loader `SLUS_006.33`

* Text `0x80010000–0x80020800`, entry `0x800155A4`, `gp = 0x80020568`.
* PsyQ libraries (libgpu sys.c v1.140, libcd, libpress, libpad, libcard).
* Shows `SPLASH.BIN`, plays `\MOVIE\%d.STR` with a libcd ring-buffer stream
  player, checks the memory card, then `LoadExec("cdrom:\ALIEN.BIN;1")`.
* Movie player main loop at `0x80011610`: per frame `VSync(0)`, read pad,
  START edge skips (after a minimum frame count), stops when the STR frame
  number reaches the last frame (`gp+0x68` vs `gp+0x70`) or when
  `StGetNext` polling times out (`gp+0x74`).
* `0x8001004C` is libpress' hand-written VLC decoder (uses GTE `LZCS/LZCR`
  for leading-zero counting, scratchpad lookup tables, and `jal`/`jr ra` as
  intra-function computed gotos).

## Main executable `ALIEN.BIN`

* Text loaded at `0x80010100`, entry `0x8008656C`, `gp = 0x800A4574`; built
  "Sep 11 2000 14:16:50" (string in the executable).
* BSS cleared from `0x800A8680` to `0x8011FF90`; the heap starts after it
  (`InitHeap(0x8011FF94, 0xDF068)`).
* Code regions:
  * `0x80010100–0x80081D7C` game code (C, gcc 2.x), `0x80081D7C–0x800912F0`
    Sony PsyQ libraries (≈320 functions identified by signature);
  * `0x800A4A2C–0x800A8680`: **hand-written assembly** placed after `.data`
    — the polygon/model rendering core (scratchpad based, heavy GTE use),
    fast `memcpy` (`0x800A7934`), `memset0` (`0x800A79B8`, 79 call sites),
    GTE setup helpers (`0x800A7A14`, `0x800A7A2C`) and a multiply helper
    (`0x800A7890`, 419 call sites).
  * Model renderer dispatch table at `0x800A473C`:
    `{0x800A5F84, 0x800A66C4, 0x800A63FC, 0x800A63FC}` called through a
    function pointer from `0x80035E84`.
* `main` is `0x80020350`; 1,767 functions in total after analysis.
* BIOS services used: string/memory routines, `malloc/free/InitHeap`,
  `printf`, events, `HookEntryInt`/`ReturnFromException`,
  `SysEnqIntRP` (libpad VBlank handler), memory card file API on `bu00:`,
  `LoadExec`. No threads, no overlays.
* Text: character names table at `0x800A48xx` (`RIPLEY`, `CALL`, `JOHNER`,
  `MARINES`, `GEDIMAN`, `VRIESS`, `HUGGER`, `BURSTER`, `NEWBORN`, `SQUEEN`,
  `CLONE1–7`, `EGG SAC`, `FATHER`…). Save file name prefix `BASLUS-00633A`.

## Library behaviour that matters for emulation

* **libetc interrupts**: `startIntr` (`0x800837A0` in `ALIEN.BIN`) calls
  `setjmp`, overwrites the saved `sp` with its own interrupt stack and
  installs the buffer with `HookEntryInt`. Every interrupt re-enters the
  middle of `startIntr` (after `setjmp`) with `v0 = 1`, which calls the
  dispatcher (`0x80083878`). The dispatcher acknowledges `I_STAT` bits,
  calls per-source callbacks, then `ReturnFromException`.
* **libpad** registers a VBlank chain entry with `SysEnqIntRP(2, …)`: the
  first function returns non-zero when VBlank is pending, the second runs the
  SIO controller transaction. It enters DualShock configuration mode
  (`0x43`, `0x45`, …) to detect analog support.
* **libcd streaming** (`StCdInterrupt`): per sector it DMAs an 8-word STR
  header into a 32-byte ring slot and 504 words of data into the ring. It
  toggles the CD DMA completion IRQ (DICR byte 2) so that only the *last*
  sector of a frame raises the callback that marks the frame complete. It
  relies on burst DMA halting the CPU.
* **libpress**: queues the next frame's `DecDCTin` before the previous
  frame's output has been read; the MDEC must behave like a FIFO.
* The CD controller is expected to come up with all interrupt sources enabled
  (the real BIOS leaves it that way), libcd never writes the enable register.

## Rendering and timing (gameplay)

* Ordering tables: two double-buffered OTs of 4,096 slots at
  `0x800C1D30` and `0x800C5D30`. Slot use observed: menu text and the logo
  banner at 0, menu highlight at 2, HUD at 4, subtitles at 0; particles from
  ~80, world geometry from ~188 onwards.
* Near-camera polygons have screen coordinates that are not direct GTE
  outputs (clipped or subdivided in screen space by the assembly renderer).
* Frame step: the per-frame routine at `0x8001FBE0` reads `VSync(-1)`,
  computes vblanks elapsed since the previous frame, clamps it to 1..8 and
  stores it at `gp+0x4130` for the game logic (`gp+0x4134` holds the last
  counter). Demo playback (`0x800A48C4` set) uses recorded steps instead.
* At original CPU speed the attract demo's combat scenes run at 20–30 fps;
  at 300% they hold 60 fps.
* libetc `VSync` wait helper: `0x80082C4C` in `ALIEN.BIN` (vblank counter at
  `0x8009D240`), `0x800163D4` in `SLUS_006.33` (counter at `0x8001F6C8`);
  timeout after `n << 15` loop iterations.
* The game supports the PlayStation Mouse (Options → Control → Mouse
  options: swap buttons, speed, acceleration) — not emulated yet.

## Player and input (gameplay)

* Controller input: libpad receive buffers at `0x800BDB90` (34 bytes per
  port: status, id `0x73`, buttons, RX, RY, LX, LY). The game turns them
  into a per-pad analog state at `0x800BDAF0` (stride 80; axes normalised
  to ±`0x1000`, with a 3-sample history) and then into the gameplay
  command block at `0x800BDBE0`: `+0x10` move direction (angle relative to
  facing), `+0x14` move magnitude (`0x1000` = full), `+0x38`/`+0x3C`
  buttons this/last frame.
* Player object: pointer at `gp+0x474` (`0x800A49E8`). Fields: `+0x30/34/38`
  position (20.12 fixed point), `+0x3C/3E` pitch/yaw (4096 = 360°),
  `+0x44/48/4C` velocity, `+0x50` pushed impulse (decays ×0.9 per vblank),
  `+0x88` current move speed, `+0x15A` speed factor (clamped to 0–`0x7FF`,
  `0x800` ≈ 1.0), `+0x171` stance (0 standing; selects the slow crouch
  speed and a per-stance camera height — meaning of values 1–3 inferred).
* Player update `0x8005ECBC`: target move speed = magnitude × 7.2 units per
  vblank standing (`0x7333`), 2.8 crouched (`0x2CCC`), 3.5 swimming
  (`0x3800`); `0x8005E8E8` ramps `+0x88` towards it (scaled 0.75–1.0 by the
  stick direction, so backing up is slower) and returns the displacement,
  then the collision routine `0x80026264` resolves the move.
* `0x8006215C` updates a second object (pointer at `0x800A49FC`) from the
  player's animated skeleton and runs it through the same collision; it
  sits about 95 units above the player, probably the camera anchor
  (inferred).
* `0x800A81BC` (hand-written): screen-space subdivision of a textured quad
  packet (`a0`) into `2^a2 × 2^a2` quads by averaging corner positions,
  colours and UVs.

## Inventory and items (gameplay)

* Text bank `TEXT%c%c.TUK` (`TEXTUK.TUK` in `PACK.BIN`) is loaded at
  `0x800BC1B0` (loader `0x800301F0`): `u32 count` (322), then `count`
  offsets from the start of the file to NUL-terminated strings, which the
  loader turns into pointers in place (string `i` = `[0x800BC1B4 + 4i]`).
  Item names start at index 27 (`PISTOL`); also there: the hidden
  `CHEAT MENU` items (`ALL GUNS`, `INFINITE AMMO`, `INFINITE HEALTH`,
  level/section select).
* Item codes (name = text index `c − 21`): weapons 48–55, ammo 56–63,
  medikits 64–66, keycards 67–73 (names per level from a table at
  `0x80094138`), fire extinguisher 74, detonator 75, flashlight 76, a
  second `FLASHLIGHT` 77, battery 78, autodoc 79, save point 80, motion
  tracker 81, text communicator 82.
* Inventory object: pointer at `gp+0x47C` (`0x800A49F0`), the static block
  `0x8011BCB8` (set up per level by `0x8005281C`). `+0x00` selected item
  code (s16), `+0x04`/`+0x08` owned-item bit masks (bit `c − 48`), `+0x0C +
  2 × (c − 48)` amount of item `c` (ammo, count or charge).
  `0x80052410(c)` = owned, `0x80052444(c)` = amount or −2 if not owned,
  `0x8005248C(c, n)` gives an item. Level setup gives the flashlight from
  level 2 on (level number byte `0x800A4612`).
* Per-item maximum table at `0x800A7A44 + (c + 57) × 4` (−1 = no maximum;
  76 and 77: 1200); the HUD shows amounts with a maximum as a percentage.
  The same table area holds gameplay constants, e.g. `+0xCC` flashlight
  capacity (1200), `+0xD0..+0xE0` flashlight flicker parameters.
* HUD (`0x80034BA4` per frame): `0x80052AEC` rebuilds two lists from the
  inventory, weapons at `0x8010C430` and items at `0x8010C588` (`int
  selected; int count; 8-byte slots {u16 code; …; s16 amount}`), and
  `0x80033AA4` draws them; the motion tracker is `0x800343FC`.
* Use item (`0x80052FA0`, from the player update when the gameplay command
  bit `0x0004` — Circle — is pressed): jump table at `0x800A2F34` indexed
  by `code − 64`. The flashlight case (`0x800530B4`) switches the light off
  if it is on, or, when the battery is not empty, creates a light object
  (`0x8001C6FC`) and stores it at player state `+0x40` (player state =
  player `+0x64` = `[gp+0x478]`), setting flag `0x1000` in state `+0x3C`.
* Flashlight update `0x800625CC(player, state)`, every frame: with a light
  on, battery (`inventory+0x44`) −= vblanks elapsed; at 0 the light is
  removed. The light's intensity pulses between 640 and 768 (state
  `+0x44`/`+0x46`) plus a random 0–24, independent of the charge. With the
  light off the battery recharges by 2 per vblank up to 1200
  (`0x800367B4(x, step, lo, hi)` = clamp(x + step × vblanks, lo, hi)).

## Memory card and options

* Save files are `bu00:BASLUS-00633A` + a 7-character suffix, 2 blocks
  (16 KiB). The game keeps a 16 KiB save image at `0x800BDD30`
  (pointer at `0x800A4574`); options are stored at `+0x210`.
* Card routines: `0x8001083C` (card status: `_card_info`, new-card
  handling, `_card_read` of sector 0 to check the `MC` header),
  `0x800109BC` (lists saves with `firstfile`/`nextfile` on `bu%c0:*`),
  `0x80010F40…` (read 128 bytes at offset `0x200` of a save).
* libapi's `firstfile()` (`0x80082834`, FIRST.OBJ v4.2) is not a plain BIOS
  call: it looks up the device in the kernel's device control block table
  (`[0x150]` = table, `[0x154]` = size, 0x50-byte entries, `+0x00` name,
  `+0x34` firstfile handler), temporarily swaps the handler for its own
  wrapper and then calls `B(42h)`. Without a `bu` entry it returns "no
  files" — the cause of saves not showing up on the Load screen.
* At boot the game reads the options from the first save it finds and
  copies them (`0x800586AC`, 48 bytes from save image `+0x210`) into the
  options block at `0x80096BBC`. Option bytes: `+0x0E` Invert Y (1 = on;
  the executable's default is on).
