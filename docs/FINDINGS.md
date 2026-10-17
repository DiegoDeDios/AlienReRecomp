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
  * `*.TIM` — standard PS1 TIM images (loading screens `ELOADnn`, `ECALIB`,
    front-end `FEGRUKnn`).
  * `*.TXF` — HUD panels and end sequences (`PANELn`, `ENDSEQn`).
  * `*.TUK` — UK-English text banks (`TEXTUK`, `LEVnnUK`, `CRUDUK`); the
    executable builds names from `LEV%02d%c%c.TUK` / `TEXT%c%c.TUK` with a
    two-letter language code (`UKFRGRITSPJP` table in the executable).
  * `DEMOn.DEM` — attract-mode input recordings.
  * `FRONTEND.CVB`, `UFATHER.CVD`.

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
