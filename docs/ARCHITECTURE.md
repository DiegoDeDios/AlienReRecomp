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
   │                           renderer (SDL GPU) + SDL3 frontend│
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

* **Graphics APIs** (`gpu_device.c`): Metal on macOS; on Windows Vulkan,
  with Direct3D 12 as the fallback (only offered if the system's
  `d3dcompiler_47.dll` compiles all of the renderer's shaders). The
  launcher creates its device the same way.
* **Shaders**: written once in GLSL (`shaders/`); `tools/compile_shaders.py`
  compiles them to SPIR-V with glslang and translates that SPIR-V to HLSL
  (shader model 5.1, compiled to DXBC at start-up on Windows) and MSL with
  spirv-cross, following SDL's resource binding rules. The translated MSL
  renders pixel-identically to the hand-written MSL it replaced, and the
  Windows build's Vulkan output under Wine/MoltenVK matches it too.
* **GPU memory**: at most two frames are in flight (`submit_frame` waits for
  the frame before last), also without a window, and uploads within a frame
  go to consecutive ranges of one staging buffer that is cycled once per
  frame. Without the cap, runs without a window queued frames faster than
  the GPU drew them; cycling on every upload made SDL allocate a buffer per
  flush. Together these grew a headless run to tens of gigabytes.

## Frontend (`src/platform/`)

* **Launcher** (`launcher.cpp`): Dear ImGui on the SDL GPU API, drawn in
  the game window before the emulated machine starts; it creates its own
  GPU device and releases the window to the game renderer when the player
  presses Start. It validates the chosen disc image (`discinfo.c`: cue sheet
  resolution, `SYSTEM.CNF` must boot `SLUS_006.33`) and uses a loading
  screen from the player's `PACK.BIN` as background art. Navigation is
  handled by the pages themselves (rows, columns in the bindings table) so
  keyboard and controller behave the same; mouse hover and clicks work too.
  Fonts come from the system (macOS: DIN, Helvetica; Windows: Bahnschrift,
  Segoe UI) with ImGui's built-in font as fallback.
* **In-game overlay** (F2, or Select+Start held for 20 frames): the
  launcher's Settings and Controls pages plus a pause menu, drawn with the
  game renderer's own SDL GPU device. `platform_vblank` runs the overlay
  loop modally inside the vblank event, so the emulated machine (and the
  audio stream) is paused while it is open and nothing needs saving or
  restoring. Each frame the renderer draws the frozen display area and then
  calls the overlay hook (`render_set_overlay`), which renders ImGui into
  the same swapchain image; the frozen frame keeps the aspect ratio it was
  drawn with (`render_hold_display_aspect`). Edits go to the run's settings
  and `config.ini`; `apply_live` in `main.c` diffs them against the
  settings in effect and applies PGXP (clearing the shadow state when it is
  switched back on), widescreen (`ws_configure`; the window is reshaped on
  resume), smooth scaling, fullscreen, frame-rate display, sprint, infinite
  flashlight, enemy damage, CPU speed, mouse and camera settings; bindings
  are read live by `input_read`.
  Internal resolution, VSync and skip intro are tagged "applies on next
  start". Controller buttons held when the overlay opens are ignored until
  released, and after it closes game buttons are held back until released,
  so the key that closed it (Esc/Start/Enter) does not reach the game.
  Select and Start never reach the game while both are held.
* **Settings** (`settings.c`): one `Settings` struct, read from and written
  to `config.ini` (INI sections `game`, `video`, `gameplay`,
  `enemy_damage`, `mouse`, `camera`, `keyboard`, `controller`). Layering:
  defaults, then `config.ini`, then command-line flags (which are not
  saved). Headless runs ignore `config.ini` so tests are reproducible.
* **Input**: every action (PlayStation buttons, keyboard stick directions,
  sprint, fast forward) has two keyboard/mouse bindings and one controller
  binding; `input_read()` turns them into the pad state the emulated
  controller port reports. Controller sticks map directly to the analog
  sticks. Mouse motion does not go through the pad: it is accumulated as
  camera angle (see Camera below). `--mouse F:LEN:DX:DY` scripts it for
  tests.

## Verification

* `--verify ADDR` snapshots CPU + RAM when the function is entered, runs it
  once in the reference interpreter and once as recompiled C, and diffs
  registers and all memory.
* UI tests: `--launcher-keys`/`--launcher-shot` drive the launcher, and
  `--overlay-keys`/`--overlay-shot` the in-game overlay (key events pushed
  into SDL's queue, a virtual SDL controller for `pad:` tokens, window
  captures including the overlay). Run them with `--save-dir` pointing at a
  scratch folder: they write `config.ini`.
* `--record F:N:STEP:PREFIX` writes a numbered sequence of screenshots;
  `tools/make_media.sh` uses it, with scripted input and `--poke`, to
  regenerate the README images and animations (`docs/media`) from the
  user's own disc.
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

### Field of view (`src/runtime/native.c`)

In game, the frame's camera setup (`0x8001EEC0`) loads the GTE projection
distance H = 384 + an s16 offset at `0x800A476E` that the game always
leaves at 0. The 3D screen is 384 × 240 (OFX 192, OFY 120) and the camera
matrix scales the vertical axis by 0.8, so the horizontal field of view is
2·atan(192 / (H·k)) (k = the widescreen scale): 53° at 4:3, 67° at 16:9.
The **Field of view** setting (degrees across the screen, up to 100; 0 =
the game's own) is turned into an H and written into that offset by a
wrapper before the setup runs, so everything the game projects in the
frame, PGXP included, uses it; menus set their own H and are not affected.

The first-person weapon is drawn by `0x80020848` (through `0x80020544`);
its wrapper puts the original H back in the GTE for that pass, because the
weapon models are built for the original framing and show their unfinished
back at a wider one. Level geometry, characters and decals keep working at
wide angles (the game culls in screen space); a wider view only changes
render-side data (ordering tables, packet buffers), not game logic, as far
as tested.

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
  `SXY` FIFO. It is kept in homogeneous form (x·z, y·z, z), so it stays
  exact where the hardware result is garbage: vertices far off-screen
  (`SXY` saturate at ±1024) and at or behind the camera (z ≤ 0, where the
  divider overflows and the vertex is clamped to the edge of the range).
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
* **Near-plane clipping**: the game has no near clipping. A wall that
  reaches past the camera (typically at the screen edge, more so with a
  wide field of view) has corners clamped to garbage positions, and the
  PS1 draws it smeared and bent, or the GPU's span limit drops it. When all
  three vertices of such a triangle are tracked, the renderer clips it at
  depth 1 in homogeneous space (texture coordinates and colours
  interpolated linearly in 3D) and draws the visible part as an exact,
  perspective-correct projection.
* **Back-face culling**: `NCLIP` on clamped coordinates can get the
  winding wrong, culling a visible near-camera polygon or keeping a back
  face. When a vertex is clamped, its sign comes from the homogeneous
  positions instead (`det[x y w]`, which keeps the true orientation as
  seen from the eye even with vertices behind it); other cases keep the
  hardware result.
* **Subdivision**: the hand-written renderer splits big near-camera quads
  into up to 4×4 pieces in screen space (`0x800A81BC`) to hide affine
  warping. With tracked corners that is unnecessary, and the midpoints
  (averages of clamped positions near the camera) would be garbage, so the
  native wrapper asks for level 0 and the renderer clips the whole quad.

The shadow lookups cost about 0.25 ms per frame. `--no-pgxp` disables the
feature; the software renderer always draws the original way.

### Sprint (`src/runtime/native.c`)

Holding Shift, or clicking the left stick on a controller (latched until the
stick returns to centre; replaces L3), makes the player move faster
(`--sprint-speed`, default 190 %). The player update (`0x8005ECBC`) converts the left stick into a
target speed and calls a helper (`0x8005E8E8`) that accelerates the
player's current speed towards it and produces the frame's displacement,
which then goes through the normal collision code. The native wrapper of
that helper scales the target speed and the acceleration for the player
object while it is standing (not crouched) and no attract demo is playing,
so walls, slopes and doors behave as usual and the speed ramps up and down
smoothly.

### Infinite flashlight

Ripley carries a flashlight from the second level on (inventory item 76,
switched on and off with Use while selected). Its battery is the item's
amount in the inventory object (`[0x800A49F0]+0x44`, capacity 1200). The
flashlight update (`0x800625CC`, called every frame for the player) drains
it by one per vblank while the light is on, switches the light off when it
is empty (about 20 seconds), and otherwise recharges it by two per vblank;
an empty flashlight cannot be switched on. With the option on (default;
`--no-infinite-flashlight` or the Settings page turn it off) the native
wrapper fills the battery before and after the update, so it never runs
down and the HUD shows 100%. Not during attract demos.

### Quick save (`src/runtime/savestate.c`)

F5 / F9 (bindable; also in the pause menu) save and restore the whole
emulated machine: CPU and GTE registers, RAM and scratchpad, the scheduler,
interrupt registers, BIOS bookkeeping, DMA, timers, GPU state and VRAM (read
back from the renderer, re-uploaded after a load), CD controller and its
audio buffer, SPU RAM and voices, MDEC, controller port, the widescreen
point sets and the frontend's frame counter. Memory cards are left out.
Each module has one `*_state(StateIO *)` function that both writes and
reads, with a section tag checked on load; a file header records the port
version, the build type, the disc and a CRC. A load first keeps the current
state and falls back on it if the file cannot be read.

Snapshots are taken and restored only at the entry of libetc's `VSync`
(wrapped in `native.c`): the menus wait in it and gameplay polls it every
frame, on the main thread. The host has to be in the same place as well:

* Recompiled builds mirror the game's call chain on the host stack, so a
  state is restored only where the return addresses on the game's stack
  (and `ra`, `sp`) match the ones it was saved with. The game runs its menus
  and levels in the same main loop, so this works in game and in the title
  menu.
* Interpreted builds run guest calls in one loop; only native overrides add
  host frames. A state is restored at any `VSync` call with the same
  overrides active, and the interpreter then continues at the restored
  return address (`g_state_loads`).

A request waits for such a point (not in an interrupt handler, no memory
card access in progress) and gives up after three seconds. The
interpreter's instruction counter is global so it can be saved too.
`--save-state` / `--load-state` make the round trip testable: a run that
saves at frame N and one that loads that state are byte-identical
afterwards, in RAM and on screen, with both renderers and in both build
types.

### Enemy damage

Settings › Enemy damage scales the damage the player takes from enemies:
an "All enemies" percentage times one per kind of enemy (alien warriors,
facehuggers, chestbursters, queen, newborn, marines and soldiers, sentry
tank, acid blood), 0–200 % each, default 100 % (`--enemy-damage PCT` or
`GROUP=PCT`; `[enemy_damage]` in `config.ini`). The game's difficulty
multiplier still applies afterwards. Two native wrappers do it:

* Message delivery (`0x8004DEA8`): enemy attacks reach the player as
  messages (code 4 hit, 7, and 402 explosion) whose damage is `msg+0x14`
  and whose sender's object type is `msg+0x0A`. When the target is the
  player and the sender type is an enemy creature (not the player, not a
  level object), the damage is scaled before the game reads it.
* Damage (`0x800519D0`): acid (kind 2: acid blood from an alien hit nearby,
  acid particles) is applied directly, so it is scaled there when the
  victim is the player.

Falls, drowning and explosions the player causes are not touched, and
nothing changes during attract demos (which run with the game's infinite
health cheat anyway). At 100 % the wrappers write nothing. See
[FINDINGS](FINDINGS.md#damage-and-messages-gameplay) for the routines.

### Camera and sticks

In the original, the sticks reach the game two frames after the controller
reports them, averaged over samples, and nothing happens until a stick is
38% of the way out (the game's STICK DEAD ZONE option). The camera stick is
a turning speed: full deflection turns 26 units (2.3°) per frame at the
default Turn speed. Mouse look that drives the stick inherits all of that:
slow movements vanish in the dead zone and fast ones are capped. Movement
loses the same two frames, and half a push walks at only 18% speed.

The sticks are read from the emulated controller port as the frontend set
them at the last vblank (`pad_get_sticks`), with a radial dead zone (default
10%; values at or past the edge are kept, so full diagonals stay full).
Two wrappers apply them (see
[FINDINGS](FINDINGS.md#camera-and-controller-input-gameplay)):

* **Movement** (`movement_stick`, wrapper of the command block builder
  `0x80032530`): the left stick's processed pad values (`+0x00`, `+0x04`)
  are replaced, linearly, right before the builder maps them through the
  control type and derives the move direction and magnitude, so the game
  does the rest as usual.
* **Camera stick** (`camera_stick`, wrapper of the player update
  `0x8005ECBC`): when the control type maps the right stick to turn/look,
  those command slots are computed with a response curve (Linear / Smooth
  1.5 / Precise 2.2) and a speed (default 150% of the game's). A larger
  d-pad value in the same slot still wins, as in the game.
* **Mouse look** (`camera_mouse`, same wrapper): the frontend accumulates
  mouse motion as camera angle (sensitivity × 0.2 game units, 4096 = a full
  turn, per count) in `g_cam_mouse_x/y`; the wrapper converts it to command
  values with the inverse of the game's turn/look rate for the current
  state, so the rounded result is exactly the wanted angle (the fraction
  carries over). No dead zone, no speed limit, no delay; Invert Y follows
  the game's option; the pitch limit is the game's. Motion is dropped if
  the game has not turned for 4 vblanks (menus, pause), so it is never
  applied late.

"Responsive sticks" (on by default) covers both sticks; `--original-stick`
leaves the game's handling alone. Nothing changes during attract demos, and
with the sticks centred and no mouse input the command block is what the
game built (headless runs are identical to the original).

### Option defaults

`main` (`0x80020350`) is wrapped to set the "Invert Y" option to off before
the game starts; the original shipped with inverted look. Options saved in a
player's save game still take precedence (the game restores them at boot).

## Windows

* Cross-compiled with MinGW-w64 GCC (`cmake/mingw-w64-x86_64.cmake`,
  `tools/build_windows.sh`); MSVC is not supported because the generated
  code uses GNU C extensions.
* GUI program (`SDL_main.h` supplies `WinMain`). Started from a terminal it
  attaches to that console; otherwise stdout/stderr go to `alien.log` in the
  data folder (`src/platform/win32.c`).
* The manifest selects UTF-8 as the process code page, so `fopen`,
  `opendir` and friends accept the UTF-8 paths SDL hands out (user names or
  disc folders with non-ASCII characters), plus per-monitor DPI awareness
  and long paths.
* Host non-local jumps (interrupt return, LoadExec) use GCC's
  `__builtin_setjmp`/`__builtin_longjmp`: the C library's `longjmp` on
  64-bit Windows unwinds the stack frame by frame (SEH) — slower, and not
  what the runtime expects.
* 16 MB main thread stack (Windows defaults to 1 MB) for deep chains of
  recompiled guest calls; the MinGW C/C++ runtime is linked statically, so
  the only DLL shipped is `SDL3.dll`.
* Paths accept `\` as well as `/` (cue sheet directory, launcher), and
  `realpath` becomes `_fullpath`.
* Verified under Wine 11: scripted headless runs (boot, FMV, LoadExec, new
  game) leave RAM, screenshots and memory cards byte-identical to the macOS
  build.

## Game code: recompiled or interpreted

`GAME_CODE` (CMake) selects how the game's MIPS code runs.

* **Recompiled** (development): the C translated by `tools/recomp.py` from
  the player's disc (`src/gen`) is linked in. The binary therefore contains
  the game's code.
* **Interpreted** (releases): `src/runtime/nogen.c` provides empty module
  tables instead, so the binary contains none of the game. `LoadExec` loads
  the executables from the disc into emulated RAM as always, and every call
  goes to the interpreter (`interp.c`, `g_interp_only`).
* **Native hooks** work in both: `cmake/overrides_table.cmake` turns the
  recompiler's `tools/overrides_*.txt` lists into `g_native_overrides`
  (module, address, native function). The interpreter checks every taken
  branch target against a bitmap of the active module's override addresses
  and calls the native function as a call or tail call; in interpreted
  builds each wrapper's `NAME__orig` interprets the original function.
* **PGXP**: the interpreter feeds the same register/memory moves of
  projected vertices as the recompiled code (`lw`/`sw`, `lwc2`/`swc2`,
  `mfc2`/`mtc2`, `addu`/`or`/`addiu`/`ori` register moves).
* **Equivalence**: the interpreter services events every 64 instructions
  rather than at loop back-edges, so RAM contents drift slightly from the
  recompiled build, but enhancements behave identically (sprint distance,
  option defaults) and hardware-rendered frames differ in under 0.1% of
  pixels. Interpreted builds are deterministic across platforms: macOS
  (arm64, x86_64) and Windows (under Wine) produce identical RAM, frames
  and memory cards for the same scripted session.
* **Speed** on an Apple M1: about 3× real time in gameplay, 2.5× in the
  heaviest stretch (4× internal resolution, hardware renderer); the Intel
  slice under Rosetta about 1.9×.

## Linux

* Built natively (GCC/Clang) by `tools/build_linux.sh`, or in an Ubuntu 22.04
  container by `tools/build_linux_docker.sh`; the release is built on Ubuntu
  22.04 so it needs glibc 2.35 or newer.
* SDL3 is compiled from the official source release and shipped as
  `lib/libSDL3.so.0`; the program's run path is `$ORIGIN/lib`, and the C++
  runtime is linked statically. SDL loads X11/Wayland, audio and input
  libraries at run time, so they are not dependencies of the package.
* Graphics: Vulkan (SPIR-V shaders).
* Launcher fonts: DejaVu, Noto or Liberation from the usual distribution
  paths, else whatever `fc-match` returns, else ImGui's built-in font.
