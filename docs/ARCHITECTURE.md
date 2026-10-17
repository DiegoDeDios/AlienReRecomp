# Architecture

The port keeps the original game logic intact by translating the MIPS
machine code to C, and replaces everything *underneath* the game (BIOS,
hardware) with native code. Over time individual recompiled functions can be
replaced by hand-written, understood C.

```
 disc image ──► tools/extract_iso.py ──► disc/ (SLUS_006.33, ALIEN.BIN, PACK.BIN, MOVIE/…)
                                              │
                    tools/analyze.py ◄────────┤  control flow, functions, jump tables
                    tools/psyq_match_all.py ◄─┘  Sony library identification
                              │
                    tools/recomp.py  ──► src/gen/*.c   (MIPS → C)
                              │
   ┌──────────────────────────┴───────────────────────────────┐
   │ native executable                                         │
   │  recompiled game code ──► runtime (CPU glue, HLE BIOS, GTE)│
   │                           hardware model (GPU/SPU/CD/MDEC…) │
   │                           renderer (Metal) + SDL3 frontend  │
   └───────────────────────────────────────────────────────────┘
```

## Static recompiler (`tools/`)

* `psx/mips.py` decodes MIPS I + GTE (COP2) instructions.
* `analyze.py` discovers functions from the entry point, `jal` targets, PsyQ
  symbols, code pointers in data, `lui`/`addiu` address pairs and gaps between
  known functions. It resolves gcc switch jump tables (`sltiu` bound check,
  `lw` from a `lui`/`addiu` table base) and classifies indirect jumps. Extra
  code regions (hand-written assembly placed after `.data`) are supported.
* `recomp.py` emits one C function per MIPS function operating on a shared
  `CPU` context (`src/runtime/recomp.h`):
  * branch delay slots are executed after the condition is evaluated;
  * load delay slots are honoured when the next instruction reads the target
    (none occur in this game's compiled code);
  * `jal` becomes a direct C call; `jr ra` a return (checked against the
    return address seen on entry);
  * hand-written code that uses `jal`/`jr ra` as a computed goto inside one
    function ("local jal") is compiled to `ra = link; goto` with `jr ra`
    dispatching over the link addresses;
  * switch tables become C `switch` statements over label addresses;
  * calls through registers go through a function table (`call_indirect`);
  * jumps to the BIOS vectors `A0h/B0h/C0h` become HLE calls;
  * functions that call `setjmp` get *resume entry points*, because the
    PsyQ interrupt library re-enters its own init function through
    `HookEntryInt`'s jump buffer;
  * every basic block charges cycles (`ADD_CYCLES`) and backward branches
    poll the event scheduler (`CHECK_IRQ`) so interrupts and hardware timing
    are delivered while the game waits in loops.

## Runtime (`src/runtime/`)

* **Memory**: 2 MiB RAM (mirrored), 1 KiB scratchpad, I/O via `bus.c`.
* **Scheduler**: absolute-cycle events for vblank, timers, CD, SPU, DMA,
  controller ACK and deferred BIOS events.
* **Interrupts**: when `I_STAT & I_MASK` is pending and `SR.IEc/IM2` allow it,
  the runtime saves the register file, runs the kernel interrupt chains
  (`SysEnqIntRP`), then "longjmps" into the handler installed with
  `HookEntryInt` (resuming mid-function via the generated resume entry).
  `ReturnFromException` unwinds back with a host `longjmp` and restores the
  registers.
* **HLE BIOS** (`bios.c`): string/memory functions, heap, `printf`, events
  (`OpenEvent`/`TestEvent`/`DeliverEvent`), critical sections (syscalls),
  memory card file API and low-level sector I/O, `LoadExec` (switches the
  active recompiled module), kernel table pointers for libraries that patch
  the kernel, and a device control block table (`[0x150]`) with `cdrom` and
  `bu` entries, which PsyQ's `firstfile()` searches before calling the BIOS.
* **GTE** (`gte.c`): full COP2 implementation with 44-bit MAC overflow
  detection, saturation flags and the UNR reciprocal divider.
* **Interpreter** (`interp.c`): reference MIPS interpreter used as a fallback
  for unknown code addresses and by the lockstep verifier (`verify.c`).

## Hardware model (`src/hw/`)

* **DMA**: burst (sync 0) transfers complete synchronously and stall the CPU,
  block/linked-list transfers complete after a size-dependent delay; DICR
  supports byte-lane writes (libcd toggles the CD-DMA IRQ enable with a
  single byte store).
* **GPU**: GP0/GP1 command processor; rasterization is delegated to a backend:
  `gpu_sw.c` (software, pixel-accurate reference) or the hardware renderer.
* **CD-ROM**: full command set used by libcd, queued interrupts with
  hardware-like acknowledgement rules, 1x/2x sector timing, XA-ADPCM decoding
  and CD-DA playback into a 44.1 kHz ring buffer.
* **SPU**: 24 ADPCM voices, ADSR, pitch modulation, noise, reverb, CD audio
  mixing, transfers and the address-match IRQ.
* **MDEC**: run-length/IDCT/YUV decoder with an output FIFO.
* **SIO0**: DualShock protocol including configuration mode (analog switch,
  vibration mapping).

## Renderer (`src/render/gpu_hw.c`)

VRAM is an RGBA8 render target at `1024·S × 512·S`. Each texel stores a PS1
16-bit pixel (5-5-5 + mask in alpha), so the fragment shader can rebuild raw
VRAM words and decode 4/8/15-bit textures and CLUTs exactly, while
primitives are rasterized at `S` times native resolution. A second copy of
VRAM is sampled for textures and refreshed only when a draw reads an area that
was rendered to (render-to-texture). Semi-transparency maps to blend states;
textured semi-transparent primitives are drawn in two passes so only STP
texels blend. CPU→VRAM uploads, VRAM copies and fills are GPU operations; VRAM
→CPU reads download from the GPU.

## Frontend (`src/platform/`)

* **Launcher** (`launcher.cpp`): Dear ImGui on the SDL GPU API, drawn in
  the game window before the emulated machine starts; it creates its own
  GPU device and releases the window to the game renderer when the player
  presses Start. It validates the chosen disc image (`discinfo.c`: cue sheet
  resolution, `SYSTEM.CNF` must boot `SLUS_006.33`) and uses a loading
  screen from the player's `PACK.BIN` as background art. Navigation is
  handled by the pages themselves (rows, columns in the bindings table) so
  keyboard and controller behave the same; mouse hover and clicks work too.
  Fonts come from the system (macOS) with ImGui's built-in font as fallback.
* **Settings** (`settings.c`): one `Settings` struct, read from and written
  to `config.ini` (INI sections `game`, `video`, `gameplay`, `mouse`,
  `keyboard`, `controller`). Layering: defaults, then `config.ini`, then
  command-line flags (which are not saved). Headless runs ignore
  `config.ini` so tests are reproducible.
* **Input**: every action (PlayStation buttons, keyboard stick directions,
  sprint, fast forward) has two keyboard/mouse bindings and one controller
  binding; `input_read()` turns them into the pad state the emulated
  controller port reports. Controller sticks map directly to the analog
  sticks.

## Verification

* `--verify ADDR` snapshots CPU + RAM when the function is entered, runs it
  once in the reference interpreter and once as recompiled C, and diffs
  registers and all memory.
* REA (`verify_reconstruction`) can compare control-flow graphs once a
  Ghidra provider is configured; `tools/exe2elf.py` wraps the executable into
  a MIPS ELF with the recovered function symbols for that purpose.

## Enhancements over the original

### Widescreen (`src/runtime/widescreen.c`)

* **3D**: the GTE's horizontal perspective projection (`RTPS`/`RTPT`) is
  scaled by `k = (4/3) / aspect`, so the game's framebuffer holds a wider
  field of view, and the image is displayed at the wider aspect.
* **2D overlays**: the HUD, text, menus and 2D backgrounds would come out
  stretched. The game inserts them into the nearest ordering-table slots
  (0–15; 3D starts at ~80), so primitives from those slots (or drawn outside
  an OT) are squashed back around the screen centre in the hardware renderer
  — unless every vertex is an exact GTE projection (true near-camera 3D).
  Matching vertices against GTE outputs alone is not reliable here: the
  hand-written renderer clips/subdivides near polygons in screen space.
* **Display aspect per frame**: the window is only shown wide while 3D is
  being drawn; pure 2D screens (loading screens, bitmaps) and FMV stay 4:3,
  pillarboxed.
* **Primitive rejection**: the PS1 GPU drops triangles spanning ≥1024 pixels
  horizontally; the limit is applied to the unscaled span so near-camera
  polygons the game relies on being discarded do not reappear as stretched
  "explosions".

### Frame pacing and frame rate

* Presentation uses vsync. When the display runs at ~60 Hz, frames are paced
  to the display refresh (smooth, no judder) and the audio stream's playback
  rate is nudged to keep ~60 ms buffered; otherwise a timer paces at the PS1
  rate (59.82 Hz).
* The emulated CPU runs at 300% by default (`--cpu-speed`), which keeps heavy
  scenes at 60 fps. This is safe because the game scales its logic by the
  number of vblanks elapsed since the previous frame.
* libetc's `VSync` wait helper is replaced natively (`src/runtime/native.c`,
  listed in `tools/overrides_*.txt`): it fast-forwards emulated time to the
  next event instead of spinning, so its iteration-count timeout cannot fire
  at higher CPU speeds.

### Precision geometry (PGXP, `src/runtime/pgxp.c`)

The PS1 wobble has two causes: the GTE rounds every projected vertex to a
whole pixel, and the GPU interpolates texture coordinates affinely (no
perspective division). The port keeps a floating-point shadow of each
vertex and lets the renderer use it:

* **GTE**: every `RTPS`/`RTPT` also computes the projected position in
  floating point (same formula as the fixed-point unit, including the
  widescreen scale) plus the depth, and pushes it onto a shadow copy of the
  `SXY` FIFO.
* **Tracking**: the recompiler emits hooks for the instructions that move
  vertices around — `MFC2`/`MTC2`/`SWC2`/`LWC2`, `LW`/`SW` and register
  moves — so the shadow follows the value through registers, RAM and the
  scratchpad. Each shadow entry stores the 32-bit `SXY` word it describes;
  when the GPU reads a packet word from RAM, the shadow is only used if it
  still matches that word exactly. Anything the game computed itself
  (clipped or 2D vertices) therefore falls back to the original integers.
* **Renderer**: vertices with precise data are placed at sub-pixel
  positions; triangles whose three vertices are precise are textured with
  perspective correction (the vertex shader outputs `w` = depth). The
  1024-pixel span rejection is skipped for them, since their coordinates
  are exact rather than wrapped.
* **Subdivision**: the hand-written renderer splits big near-camera quads
  into up to 4×4 pieces in screen space (`0x800A81BC`) to hide affine
  warping. With precise corners that is unnecessary and the averaged
  midpoints would lose precision, so the native wrapper asks for level 0.

The shadow lookups cost about 0.25 ms per frame. `--no-pgxp` disables the
feature; the software renderer always draws the original way.

### Sprint (`src/runtime/native.c`)

Holding Shift, or clicking the left stick on a controller (latched until the
stick returns to centre; replaces L3), makes the player move faster
(`--sprint-speed`, default 175 %). The player update (`0x8005ECBC`) converts the left stick into a
target speed and calls a helper (`0x8005E8E8`) that accelerates the
player's current speed towards it and produces the frame's displacement,
which then goes through the normal collision code. The native wrapper of
that helper scales the target speed and the acceleration for the player
object while it is standing (not crouched) and no attract demo is playing,
so walls, slopes and doors behave as usual and the speed ramps up and down
smoothly.

### Option defaults

`main` (`0x80020350`) is wrapped to set the "Invert Y" option to off before
the game starts; the original shipped with inverted look. Options saved in a
player's save game still take precedence (the game restores them at boot).
