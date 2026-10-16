# Duke Nukem 3D

[Chocolate Duke3D](https://github.com/fabiensanglard/chocolate_duke3D), Ken
Silverman's Build engine and 3D Realms' game code, as an ordinary app from
the card: a 690 KB `.so` that the firmware loads like any other, with no line
of the firmware changed for it. It was the step after [Doom](../doom/README.md)
to see how far the ESP32-P4 goes: a true 2.5D engine with sloped floors,
rooms over rooms, mirrors, a scripting language for every actor, and the
Sound Blaster's FM music synthesised on the board.

It is always in landscape, like Doom: the game's picture scaled into the
960x600 box in the middle of the 1280x720 screen, the controls in the bands
around it.

## Measured on the board

ESP32-P4 rev 1.3 at 360 MHz, E1L1 of the shareware, the time the game's core
spends on one frame: the game's logic, the renderer, the palette pass and the
sound mix together (the log line's `frame N ms of game`).

| Mode | Frame | Frame rate |
|---|---|---|
| 320x200 | 16 ms | 30 fps (the game's own rate; 59 uncapped) |
| 480x300 | 29 ms | 30 fps (34 uncapped) |
| 640x400 | 50-69 ms | 15-21 fps |
| 640x480 | 57 ms | 15-18 fps |

- **One frame per step of the game's logic.** Duke moves the world 30 times
  a second and draws without interpolating, so the port waits for the next
  step before drawing again. Uncapped, E1L1 drew 56 frames a second at
  320x200 and the menu 78, half of them copies.
- **Not the instructions.** Building the renderer at `-O2` instead of `-Os`
  moved nothing (640x400: 48-52 ms). Build draws walls a column at a time,
  reading textures from a 6.6 MB tile cache and writing an 8-bit frame, all
  in PSRAM: the time goes in waiting for memory.
- **640x400 and 640x480 use all of core 0**, and on the firmware of 0.15.1
  that flashed the screen blue a few times a minute. It looked like PSRAM
  bandwidth and was not: the task watchdog found core 0's idle task starved
  and printed its dump from its interrupt, and the panel's refresh, which an
  interrupt restarts every frame within the vertical blanking, came late. The
  firmware no longer watches core 0's idle task and has a wider blanking
  (51 Hz instead of 55) since `8b3dde5` and `fcde1e5` (docs/MEMORY.md, "Not
  every blue flash is bandwidth"); with it every mode plays clean. **On an
  older firmware, stay at 480x300.**
- **The sound**, effects and music together, takes 7 to 14 % of core 0, the
  music most of it (OPL3, see below).
- **Loading** to the title takes about 2 s; a level, under a second.
- **Memory:** the `.so` brings 2.1 MB of statics (the engine's maps and
  tables), the tile cache is as big as the GRP's art (6.6 MB for the
  shareware) or what PSRAM can spare, and closing the app gives everything
  back: 17,461 KB of PSRAM free before opening and after closing.

## The GRP

The game's data lives in `DUKE3D.GRP`, which is **not** part of this app, of
the repository or of the releases: copy it to the card's `duke3d/` folder,
with the card in a reader or through the portal's file explorer
(`PUT /api/fs/put?path=/duke3d/DUKE3D.GRP`). Without one the app says which
folder it looked in. The config (`duke3d.cfg`) and the saves land in the same
folder.

The GRP is told apart by its size, and the game is given the CRC that size
stands for (Chocolate read the whole file for its CRC: 4 s for the
shareware's 11 MB, 15 s for the Atomic Edition's 44 MB). Any other size gets
its real CRC, read in pieces.

### What it was tested with

**The shareware `DUKE3D.GRP`, version 1.3D**: episode 1, *L.A. Meltdown*,
11,035,779 bytes, CRC32 `983AD923`, SHA-1
`a58bdbfaf28416528a0d9a4452f896f46774a806`. It comes in 3D Realms'
shareware package (`3dduke13.zip`), which the Internet Archive keeps as
[3dduke13SW](https://archive.org/details/3dduke13SW).

Its `LICENSE.TXT` lets individuals give copies away without charge and free
web sites offer it for download, **whole and unmodified**, with all of its
files. That is why it is not bundled here: the app is not the shareware
package. This is a summary to explain the choice, not legal advice.

The shareware's demos (`DEMO1-3.DMO`) do not play: Chocolate Duke3D reads
the demo format of its own versions (27, 28, 29, 119), and 1.3D's are older.

### The full game (in theory)

Chocolate Duke3D plays the registered 1.3D (26,524,524 bytes), the Plutonium
Pak 1.4 (44,348,015) and the Atomic Edition 1.5 (44,356,548); its GRP is
`DUKE3D.GRP` too, in the same folder. **It is not tested**, and three things
to know:

- The commercial GRPs are **not** redistributable: yours, for your board.
- The engine reads the GRP from the card a file at a time, it never loads it
  whole, so the 44 MB of the Atomic Edition are not a problem.
- Its art is bigger than the shareware's: the tile cache takes what PSRAM
  can spare, leaving 3 MB, and re-reads from the card what does not fit.

### Starting somewhere else

`duke3d/args.txt`, one line, is the DOS executable's command line:
`/v1 /l1 /s2` starts episode 1, level 1, at skill 2 (*Let's Rock*), straight
from the app, with no menus. None by default.

## Playing

The screen turns to landscape when Duke opens and back when it closes. Both
thumbs play at once: each finger owns what it landed on until it lifts.

```
 +---------------+---------------------------------------+---------+
 | MENU    MAP   |                                       | WEAPON  |
 | ITEM    NEXT  |                                       | STRAFE  |
 |               |      the picture, 960x600 at most     |  JUMP   |
 |    stick      |      pressing it fires                |  USE    |
 |               |                                       |  FIRE   |
 |               |                                       | CROUCH  |
 +---------------+---------------------------------------+---------+
```

- **The stick** appears where the left thumb lands. Up and down walk,
  sideways turns; turning is squared, so a small push aims finely and the
  edge turns fast. Duke always runs.
- **FIRE**, and pressing the picture. **USE** opens doors and presses
  switches, **JUMP** and **CROUCH** are Duke's own.
- **STRAFE** is a latch: on, the stick sidesteps instead of turning.
- **ITEM** uses the item selected in the inventory, **NEXT** selects the
  next one (the medkit, the jetpack, the night vision goggles...).
- **WEAPON** is the next weapon, **MAP** the automap.
- **MENU** is Duke's own menu. In a menu the stick moves the cursor, FIRE
  picks (and answers *yes*), USE goes back (*no*). FIRE also passes the
  screens that wait for a key.
- **Leaving:** Quit in Duke's menu (the shareware shows its ordering screens
  on the way out; FIRE passes them), or **holding MENU for two seconds**.
  While a game runs the system's edge gestures are off.

**A USB gamepad** plays too: the stick or the d-pad walks and turns (the
arrows in a menu), A is FIRE, B is USE, L jumps, R crouches, L and R
together change weapon, START is MENU and, held two seconds, leaves.

## How it is put together

```
main/duke3d.c        the app: the layout, the pad, the blit, the life cycle (LVGL's task)
main/duke_port.h     the seam between the two sides, nothing but plain memory
main/port/           the platform layer
    dk_compat.h      what the engine sees instead of the C library
    dk_platform.h    engine/platform.h's branch for the board
    dk_system.c      the game's life: worker, exit(), memory, files, paths, stdout
    dk_display.c     engine/display.c's replacement: the frame, the palette, the timer
    dk_input.c       the pad as the game's functions, and as keys in the menus
    dk_game.c        the port's questions to the game (what mode, where)
    dk_sound.c       audiolib/dsl.c's replacement: the Apogee mixer into the speaker
    dk_music.c       the MIDI on the OPL chip, as a Sound Blaster played it
    dk_opl.c, dbopl.c  DOSBox's OPL emulator (Doom's copy)
    dk_net.c         one player, no network
    SDL.h            the few SDL names the game still uses, as nothing
main/engine/         the Build engine, vendored
main/game/           the game and the Apogee Sound System, vendored
```

**The same frame of mind as Doom's port.** The game runs in the app's worker
on core 0 at priority 3 and never touches LVGL. Its `main()` (renamed
`dk_game_main`) never returns: `exit()`, Duke's `Error()` and a stop asked by
the app all `longjmp` back to the top of the worker, which closes every file
and frees every block the game took, since the heap outlives the `.so`. Each
frame goes through the palette into one of three RGB565 slots, and LVGL's
task takes the newest and has the PPA scale it into the panel's free buffer,
then flips (the same `present_flip` as Doom's).

**Files.** Duke opens its files by bare name (`GAME.CON`, `GAME0.SAV`) and
builds some paths with backslashes: every relative path is made one inside
the card's `duke3d/` folder, and backslashes become slashes. The GRP is read
with `open`/`read`/`lseek`, so descriptors are tracked as well as `FILE`s.

**Input.** While playing, the pad's buttons are the game's *functions*:
`control.c`'s `ACTION()` asks the port first, so FIRE is `gamefunc_Fire`
whatever keys the config binds. A function the game "clears" after acting on
it (the map, the next weapon) stays off until its button is let go and
pressed again, as a key would. In menus and the screens that wait for a key
the buttons are keys, fed to the game's own `keyhandler()` as the SDL driver
fed it. The stick is analogue, added to the game's input after the mouse.

**The music is the Sound Blaster's.** Duke's music is General MIDI, which on
a Sound Blaster went through Apogee's AdLib driver onto the FM chip with the
instruments of `D3DTIMBR.TMB`, in the GRP. `midi.c` and `al_midi.c` are
those drivers, with their port I/O turned into register writes on DBOPL and
TaskMan's timer interrupt into the sample count: `dk_music_mix()` generates
the chip up to each MIDI tick, runs the tick, and goes on, so the tempo holds
whatever the frame rate does. OPL3 in mono, 18 voices, as on an SB16: the
songs use 10 to 17 voices and reach 18, so an OPL2's 9 would drop notes
(`DK_MUSIC_OPL3=0` builds the OPL2, at about half the cost). Generated at
22050 Hz. The timing is TaskMan's own arithmetic, whole PIT counts, so a song
runs about 0.1 % fast, as on a PC.

**Effects.** The Apogee mixer (`multivoc.c`) works as on DOS: a ring of
small buffers and a service routine that mixes the next one when the card has
played one. Here the game's own task calls it from `dk_sound_update()`, at
each frame and in every wait, keeping the speaker about 100 ms ahead, and the
music is added on the same clock. Mono, 16 bit, 22050 Hz, 16 voices.

## The engine

`main/engine/` and `main/game/` are Chocolate Duke3D at
`1cfa1909aa1f5a5ab159301044ca499673bc2283` (2019), without the SDL driver
(`display.c`, `dsl.c`, `sdl_midi.c`), the networking (`mmulti.c`, enet) and
the DOS sound card drivers; `midi.c`, `al_midi.c` and `gmtimbre.c` are
Apogee's, from the same tree. Every vendored `.c` file includes
`port/dk_compat.h` first (its first line, marked `P4OS`), and every other
change is marked `P4OS:` in the source.

| File | Change |
|---|---|
| `engine/platform.h` | a branch for the board (`port/dk_platform.h`) |
| `engine/display.h` | `_argc`/`_argv` declared, not defined, in the header |
| `engine/filesystem.c` | the GRP's CRC by its size; `game_dir` defined once |
| `engine/tiles.c` | the tile cache capped by the PSRAM free (`dk_cache_budget`) |
| `engine/draw.c` | the pixel budget (see below); `-O2` |
| `engine/engine.c` | the pixel budget refilled every frame; `-O2` |
| `game/game.c` | `main()` renamed; the GRP the app found; no executable to checksum; no `d_namlen` |
| `game/global.c` | `Error()` through the port; `FixFilePath` only turns backslashes |
| `game/config.c` | mono 16-bit 22050 Hz, 16 voices; music on; 320x200 by default |
| `game/control.c` | `ACTION()`, `CONTROL_ClearAction()` and `CONTROL_GetInput()` ask the port |
| `game/player.c` | the stick's walking added after xDuke zeroes the mouse's |
| `game/keyboard.c` | `KB_KeyWaiting()` tells the port the game waits for a key |
| `game/audiolib/mv_mix.c`, `multivoc.c`, `mvreverb.c` | `float` for `double` |
| `game/audiolib/midi.c`, `al_midi.c` | port I/O to DBOPL, TaskMan to the sample clock (marked `P4OS:`) |

And `usrhooks.c` left out: `sounds.c` has the same two functions.

The app's `CMakeLists.txt` adds `-fcommon -fwrapv -fwrapv-pointer
-fno-strict-aliasing` for every file of the `.so` (see the traps).

## Traps

- **Not in the simulator.** The game keeps pointers in 32-bit integers: the
  CON scripts' compiled bytecode holds jump targets and actions as `int32_t`,
  and the savegame code turns them into offsets and back. On the board's
  32-bit RISC-V that is what the code meant; on the 64-bit Mac it is not, and
  porting it to offsets is a rewrite of `gamedef.c` and `actors.c`. So the
  app is not in the simulator's list (`sim/CMakeLists.txt`); built in by
  hand, it would only draw its pad and say it needs the board. Everything
  was tried on the board.
- **`int32_t` is `long` here.** The riscv32-esp toolchain makes `int32_t` a
  `long`; the code mixes `int` and `int32_t` in declarations and definitions
  (`kread32(int32_t, int32_t *)` called with an `int *`), which a PC never
  saw. `dk_compat.h` makes `int32_t` an `int` for the vendored files, before
  any system header: both are 32 bits with the same calling convention.
- **C of 1996 and gcc 14.** Pointer and integer conversions, functions used
  undeclared and `int` versus `int32_t` are errors in gcc 14; `dk_compat.h`
  makes them warnings, what the code always meant. Globals defined in headers
  needed `-fcommon` back. Signed arithmetic that wraps, pointers walked past
  zero (the column loops in `draw.c` run a pointer up from minus the height
  until it wraps) and the same memory read as bytes and as longs need
  `-fwrapv -fwrapv-pointer -fno-strict-aliasing`: Chocolate itself built
  `premap.c` at `-O0` because optimising broke it.
- **The pixel budget.** `draw.c` counts pixels down from
  `pixelsAllowed = 10000000000`, a debugging aid ("draw only the first N"),
  and stops drawing at zero. 10^10 does not fit in an `int32_t`: it was 1.4
  billion, a few minutes of play before the walls stopped drawing. It is
  refilled every frame now.
- **The stick that did not walk.** xDuke's FIX_00021 sets the forward motion
  read from `CONTROL_GetInput()` back to 0, so that moving a mouse up and
  down does not walk. The stick's walking went through there and vanished;
  turning worked. It is added after that line now (`dk_analog_walk`).
- **The include that ended in a slash.** `FixFilePath()` fixes a name's case
  by scanning the working directory, which the board does not have; failing,
  it wrote a separator over the string's end, `DEFS.CON` became `DEFS.CON/`
  and `GAME.CON`'s include failed. The card is FAT and ignores case: it only
  turns backslashes now.
- **The screens that wait for a key.** The way out of the shareware (its two
  ordering screens), the end of an episode and the score after a level are
  loops of `while (!KB_KeyWaiting())`, with the game still in `MODE_GAME`,
  so FIRE went out as `gamefunc_Fire` and never as a key, and quitting from
  the menu hung on the first screen. The port counts calls to
  `KB_KeyWaiting()` without a frame drawn: past 64 the game is waiting, the
  pad sends keys, and each call sleeps a millisecond and mixes (a loop that
  never blocks on core 0 starves the idle task).
- **The blue flashes were not the PSRAM.** Uncapped at 320x200 and at
  640x400 the screen flashed; both use all of core 0, and the culprit was
  the task watchdog's report from its interrupt delaying the panel's restart
  (see the measurements above). The cap to the game's 30 fps is still right,
  for its own reason: the frames beyond it were copies.
- **Doubles.** The Apogee mixer of Chocolate mixes in `double`, which the
  P4 does in software; it is `float` here, with the hardware's FPU.
- **libgcc.** `__ashrdi3` (a 64-bit shift, from the fixed-point maths) and
  `__bswapsi2` are not lent by the firmware: the port supplies them, written
  so that gcc cannot turn them into calls to themselves (Doom's trap).

## A test aid

With `duke3d/autowalk.txt` on the card (any content), the stick is held
forward while playing, read when the game starts: walking can be checked with
nobody at the board. Delete it to play.

## Licences

- The game's code (`main/game/`, 3D Realms' 2003 release, and the Apogee
  Sound System) and this port are under the **GNU GPL v2**
  (`main/game/LICENSE`); the rest of P4OS is MIT.
- The Build engine (`main/engine/`) is under **Ken Silverman's Build
  licence** (`main/engine/BUILDLIC.TXT`): modified versions may be
  distributed only over the Internet, only free of charge, never for
  commercial use, with that file and his notice. "Build Engine & Tools"
  Copyright (c) 1993-1997 Ken Silverman; http://www.advsys.net/ken.
- DBOPL is DOSBox's, GPL v2, as in Doom.
- The GRP is 3D Realms' data and is not included. Duke Nukem is a trademark
  of Gearbox Software; this app is unofficial.
