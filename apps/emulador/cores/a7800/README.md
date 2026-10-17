# Atari 7800 core (ProSystem)

The Emulador's Atari 7800: ProSystem as
[RetroESP32-P4](https://github.com/giltal/RetroESP32-P4) carries it (commit
`339f17ff`, `components/prosystem`, by way of the odroid-go port), behind
the `emu_core_t` table of `main/emu_core.h`. Built as the module
`a7800.so`.

ProSystem is (c) 2003, 2004 Greg Stanton, under the GNU General Public
License version 2 or later (the notice in each source file; the licence's
text in `prosystem/COPYING`, as RetroESP32-P4's copy carries none).

## Files

- `a7800_core.c`: the adapter, `emu_core_a7800`. Video to RGB565, the TIA
  and POKEY mixed to 16-bit stereo, the two joysticks, the console switches,
  the options, the optional BIOS, the save states.
- `prosystem/include/P4State.h` (ours): the save-state walk every module
  now has (`<module>_State(fn, ctx)`), the memory map's description, and the
  release functions.
- `compat/esp_attr.h`, `compat/esp_heap_caps.h`: stand-ins for ESP-IDF's
  (an empty `IRAM_ATTR`; `heap_caps_malloc`/`free` declared on the board,
  `malloc`/`free` in the simulator).
- `sources.cmake`, `board.cmake`, `sim.cmake`, `one_object.cmake`: the build,
  as the 2600's (one object, `emu_core_a7800` its only global).

Not vendored: the runner `prosystem_run.c/.h`, and `include/boolean.h`
(unused).

## What it plays

`.a78` (with the 128-byte header) and headerless `.bin` images the database
knows; headerless ones must be at least 8 KB and whole 4 KB pages, so a 2600
cartridge is refused ("No es un juego de Atari 7800"; ProSystem has no 2600
mode). Cartridge types: normal (up to 48 KB), SuperGame (with RAM, with
extra ROM, large), Absolute and Activision; POKEY sound. A type and size
ProSystem could not map without reading past the image fail the load with
"Tipo de cartucho no soportado". NTSC or PAL from the header or the
database. Light guns are not emulated. The High Score Cartridge is not
emulated, so there are no battery saves.

## The table

| field | value |
|---|---|
| id / name / short_name | `a7800` / Atari 7800 / `7800` |
| exts | `.a78 .bin` |
| max_w × max_h | 320 × 272 (320 × 223 NTSC, MARIA's visible area) |
| pixel_aspect | 6/7 = 0.857 (MARIA's 7.16 MHz pixels on a 240-line picture) |
| players | 2 |
| buttons | d-pad, B (button 1), A (button 2), R (RESET), SELECT, START (PAUSE) |
| face | `EMU_FACE_ROW2` |
| labels | B = "1", A = "2", R = "RESET", SELECT = "SELECT", START = "PAUSE" |
| fps | 31400 / 524 = 59.92 (NTSC), 31112 / 624 = 49.86 (PAL) |
| sample rate | 31400 Hz NTSC, 31112 Hz PAL: the chips' own two samples a line |

The mapping: the ProLine joystick's left button is button 1, the right one
button 2, so on the screen's pad B (left) is "1" and A (right) is "2"; in
the one-button games either fires. The console's three keys: SELECT is
SELECT, START is PAUSE, and RESET (which starts most games) is the right
shoulder. The difficulty switches are options.

Sound: ProSystem samples the TIA and POKEY twice a line, 524 samples a frame
NTSC and 624 PAL. They are handed on as they are (no resampling: the
runner's nearest-neighbour resampler is gone), mixed, and a one-pole
high-pass filter takes out the DC (the chips only output positive levels).

## Options

| key | label | values | default |
|---|---|---|---|
| `diff_left` | Dificultad izquierda | A (difícil), B (fácil) | B |
| `diff_right` | Dificultad derecha | A (difícil), B (fácil) | A (RetroESP32-P4's choice: Tower Toppler needs it) |
| `bios` | BIOS de la 7800 | No usar, Usar si está | No usar |

The BIOS is optional: ProSystem starts the cartridge directly. With "Usar
si está" it looks in the console's `bios` folder for `7800 BIOS (U).rom`
(also `7800bios.bin`, `7800.rom`) or, for a PAL game, `7800 BIOS (E).rom`
(also `7800pal.bin`), 4 or 16 KB; the game then starts over from the BIOS
(the logo, and its check of the cartridge's signature: an unsigned homebrew
then does not start, which is why it is off by default). No BIOS file is
distributed.

## Save states

`P4A78ST\0`, a version, the payload's length and CRC-32, the cartridge's
MD5, then every module's variables (CPU, MARIA, RIOT, TIA, POKEY, cartridge
registers, the 16 KB of RAM and 16 KB of cartridge RAM, ProSystem's frame
counters), the 4 KB memory map as (region, offset) pairs, the BIOS flag and
the sound filter. ProSystem's own `prosystem_Save()` kept only the CPU, the
bank and the RAM (and aborted on SuperGame RAM). A file whose header, CRC,
MD5 or length do not match is refused untouched; one whose memory map does
not fit the cartridge puts the console back as it was.

## Changes to ProSystem

Every one is marked `P4OS` in the source.

- Every module (`Sally.c`, `Maria.c`, `Riot.c`, `Tia.c`, `Pokey.c`,
  `Cartridge.c`, `Memory.c`, `ProSystem.c`) has a `_State()` walk;
  `Memory.c` describes and installs the memory map (`memory_MapOut/In`);
  `Cartridge.c` and `Bios.c` say where their images are.
- `Memory.c`: no `abort()` on a failed allocation (the adapter checks);
  `memory_Release()`; the cartridge RAM cleared at power-on; writes to the
  SuperGame cartridge RAM reach it (the port's fast page map had left it
  read-only).
- `Cartridge.c`: the type, POKEY flag, controllers, region and bank are
  reset before reading a new file (a headerless game inherited the last
  one's); the image allocation is checked; the scratch page was allocated
  per game and never freed, and is now freed with the cartridge; the image
  freed with `heap_caps_free` (it came from `heap_caps_malloc`); no
  `printf`/`abort`.
- `Pokey.c`: the 17-bit polynomial from POKEY's shift register instead of
  `rand()` (not exported, and it made every power-on sound different);
  `pokey_Ready()`, `pokey_Release()`; no `abort()`.
- `Palette.c`: `palette_Release()`; no `abort()`.
- `Database.c`: digest and title fields cut from 256 bytes each (60 KB of
  zeros); no `printf`.
- `ProSystem.c`: no `abort()` in `prosystem_Save()`.

The adapter also snapshots every module's variables as the program starts
and puts them back at each power-on: ProSystem's resets left MARIA's line
buffer, the RIOT's timers and the sound counters as they were, so a reset
or a second game did not start from the same machine.

## Test notes (Mac, scratchpad harness)

A test program written for this port (a `.a78`, 32 KB, assembled in the
scratchpad): sixteen display-list zones in 160A mode, a sprite moved by the
joystick, stripes, two-button mode with button 1 cycling the background and
button 2 sounding a TIA tone, SELECT swapping colours, PAUSE and the left
difficulty switch recolouring the stripes, RESET centring the sprite; also
as a headerless `.bin` and with the POKEY flag set. Checked:

- pictures looked at: colours, the sprite's orientation, every key and
  switch;
- the headerless image draws the same frames as the `.a78`;
- state round trip at several frames: 120 frames identical in picture and
  sound; damaged, truncated and foreign files refused;
- unload → load and reset: the same frames (after the power-on snapshot);
- a 2600 cartridge refused; the BIOS option without a BIOS logs it and
  plays on;
- AddressSanitizer and UBSan clean;
- ~2,300-3,300 frames/s on the Mac.

Not tested with commercial games or the real BIOS (none may be
downloaded).

## Known limits

- ProSystem 1.3-era accuracy (this port, not the libretro one): no light
  gun, no High Score Cartridge, no XM; MARIA's DMA timing approximated.
- The BIOS's own 2600 mode is not emulated.
