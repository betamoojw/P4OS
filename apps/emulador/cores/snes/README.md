# Super Nintendo core (Snes9x, snes9x2005 line)

The Emulador's Super Nintendo / Super Famicom: Snes9x as
[Retro-Go](https://github.com/ducalex/retro-go) carries it (commit
`4ced120669750ca7228fd0414211430c1d923166`, 2026-01-19,
`retro-core/components/snes9x/src`), behind the `emu_core_t` table of
`main/emu_core.h`. It is the snes9x2005 / CATSFC branch (Snes9x 1.43 with
the later fixes libretro and the DS port brought in), with Snes9x's own
SPC700 and DSP (`soundux.c`), not blargg's. On the board it is the module
`snes.so`.

## Licence

Read before distributing. Every source file says only `This file is part of
Snes9x. See LICENSE file.` The `snes9x/LICENSE` file is two licences in one:

- a GPL-2.0-or-later notice for the NDS port's authors (dking, BassAceGold,
  ShadauxCat, Nebuleon) and a GPL-2.0-or-later block for the ZSNES code it
  includes (the C4 routines);
- "Based on" Snes9x, (c) 1996-2010 Gary Henderson, Jerremy Koot and the
  others listed, under **the Snes9x licence**: copying, modifying and
  distributing for **non-commercial purposes** only, with the notice kept;
  commercial use needs the copyright holders' permission.

The GPL notice cannot relicense the Snes9x code under it, so the core as a
whole is under the non-commercial Snes9x licence, which is not compatible
with the GPL (it adds a restriction). It therefore must not be linked into
one binary with GPL code: it is fine as its own module (`snes.so`, which
links nothing but its own files and the firmware's libc), as the app builds
it since the modules came in. The same holds for RetroESP32-P4's copy (its
repository says MIT in the README, but its `components/snes9x/LICENSE` is
this same file) and for every other snes9x2002/2005/2010 derivative. No
GPL SNES core light enough for a 360 MHz RV32 was found: bsnes/higan/ares
and Mesen-S are far too heavy, ZSNES is x86 assembly, LakeSnes (MIT) draws
pixel by pixel and is several times slower than Snes9x.

## Why this copy

Compared, with RetroESP32-P4's (`components/snes9x`, "hand-tuned to hold
60 FPS on the P4") and Retro-Go's, both snes9x2005 with the same 65816,
SPC700 and DSP code:

- RetroESP32-P4's speed comes from things a `.so` on the card cannot use or
  that the app does already: the hot opcodes and tile code in IRAM
  (`IRAM_ATTR`), the display and the sound mixing on core 1 in tasks of its
  own, and a frame pacer that skips up to 3 drawn frames in a row. Its tile
  cache is turned off (fcipaq's ESP32 memory saving: every tile is decoded
  again each time it is drawn), its SRAM sits in internal RAM, `memmap.c`
  is two whole copies of itself under `#ifdef NO_PSRAM`, and its SuperFX is
  a stub (`fxstub.c` is what it builds; `fxemu.c`, with its core-1 task,
  is not built, and the driver's debug prints show it did not run games).
- Retro-Go's is the same core, kept up: constant tables (the tile bitmasks)
  instead of RAM, the memory map on the heap, the tile cache on (512 KB,
  cheap in PSRAM), DSP-1/2/3/4 in one file, a save state of its own.

So Retro-Go's, with the second-core idea of RetroESP32-P4 redone through
the host (below).

## Files

- `snes_core.c`: the adapter, `emu_core_snes`. Video, sound, the two pads,
  the options, the battery save, the save states, the split sound.
- `sources.cmake`, `board.cmake`, `sim.cmake`: the build, the same sources
  for the board and the simulator. `board.cmake` has two switches at the
  top (`EMU_SNES_FAST_WORD`, `EMU_SNES_SPLIT_SOUND`, below).
- `snes9x/`: the vendored core with its `LICENSE`. Left out of Retro-Go's
  copy: `apu_blargg.c/.h` and `blargg_endian.h` (the other APU, not built)
  and `snapshot.c/.h` (the adapter has its own states), and Retro-Go's
  `README.md`/`CMakeLists.txt`/`snes9x.h` wrapper.

## The table

| field | value |
|---|---|
| id / name / short_name | `snes` / `Super Nintendo` / `SNES` |
| exts | `.sfc .smc .swc .fig` (a 512-byte copier header is skipped) |
| max_w x max_h | 256 x 239 |
| pixel_aspect | 8/7 (1.143: 256 pixels on a 4:3 picture) |
| players | 2 |
| buttons | d-pad, A B X Y, L R, SELECT START |
| face | `EMU_FACE_DIAMOND` |
| labels | `SELECT`, `START`; the rest the letters |
| fps | 32000 / 533 = 60.04 NTSC (the console: 60.10), 32000 / 640 = 50.00 PAL |
| sample_rate | 32000 (the DSP's own), stereo |

### The picture

256 x 224 (or 239 with the console's overscan mode) RGB565 straight from
Snes9x's buffer (it draws R in the top bits in the CPU's byte order); the
usual frame is handed over without a copy, with a 1024-byte stride.

The high-resolution modes (5 and 6, pseudo-hires transparencies, interlace)
are drawn 512 wide and, interlaced, 448/478 high. The frame the app gets
is still 256 wide: each pair of pixels averaged (which is what those
transparencies look like on a TV), and one field of an interlaced picture.
That keeps one size for the app's layout and thumbnails. `stats()` says how
many frames were 512 wide. Offering them whole would need a max_w of 512,
which the app's thumbnails take as the picture's shape.

### Options

| key | label | values | default |
|---|---|---|---|
| `sound_quality` | Calidad del sonido | Alta, Normal, Rápida, Sin sonido | Alta |
| `overscan` | Recortar bordes | Sí, No | Sí |

- Alta: interpolated samples and echo. Normal: no interpolation. Rápida: no
  echo either (some games lose their reverb). Sin sonido: the DSP is not
  mixed at all (the sound CPU still runs: games wait on it), silence goes
  out. The speed cost of each, on the Mac with `music.sfc`: mixing is 26 %
  of a frame (0.017 of 0.065 ms).
- Recortar bordes: a game in the 239-line mode shows the middle 224 lines
  (7 off the top, 8 off the bottom), so the picture keeps one size. No:
  239 lines then.

No frame skip of its own: with `draw` false `IPPU.RenderThisFrame` is off
and nothing is drawn, the cheapest skip there is (on the Mac a frame not
drawn costs 10-40 % of a drawn one).

### Errors (Spanish keys for `_()`)

- `No se pudo abrir el archivo`, `No se pudo leer el archivo`
- `No es un juego de Super Nintendo` (shorter than 32 KB, or Snes9x refuses it)
- `El archivo es demasiado grande` (over 8 MB + header)
- `No hay memoria`
- `Este juego usa el chip Super FX, que el emulador no tiene`
- `Este juego usa el chip SA-1, que el emulador no tiene`
- `Este juego usa el chip S-DD1, que el emulador no tiene`
- `Este juego usa el chip SPC7110, que el emulador no tiene`

## The chips

Work: DSP-1 (Pilotwings, Super Mario Kart...), DSP-2, DSP-3, DSP-4, C4
(Mega Man X2/X3), OBC1 (Metal Combat), S-RTC. Refused at load: Super FX
(Star Fox, Yoshi's Island, Doom...), SA-1 (Super Mario RPG, Kirby Super
Star...), S-DD1 (Star Ocean, Street Fighter Alpha 2), SPC7110 (Far East
of Eden Zero, Momotarou Dentetsu Happy). Not detected and not emulated:
the Seta ST010/ST011 (F1 ROC II, two shogi games), the BS-X and Sufami
Turbo adapters, the Super Game Boy, the mouse, Super Scope and Justifier.

## Battery saves

The cartridge's RAM, raw, the size the header says (1 KB << n, up to
128 KB): the `.srm` of Snes9x and the `.sav`/`.srm` of most emulators.
Read at load (as far as the file goes), written by `sram_flush()` only
when it differs from what was last read or written. A reset (the power
switch) keeps it.

## Save states

The adapter's own format: `P4SNES01`, a word made from the sizes of
Snes9x's structs and the pointer size (a state is for this build: the
board's and the simulator's differ), the game's 64 header bytes and size,
then the blocks (CPU, ICPU, PPU, DMA, VRAM, WRAM, SRAM, the registers'
RAM, APU, IAPU, the sound RAM, the DSP state, the mixer's own state with
the echo history, the DSP chip's struct if there is one, and a block of
what lives elsewhere: the CPU's pointers as offsets, the HDMA bits, the
VRAM read latch, the key-on latches, OBC1's registers, whether the frame's
sound is mixed). The whole file is read and every block checked (tag,
length, all of them, nothing after) before anything is touched; a file for
another game or build, short, or damaged is refused and the game goes on.
Written to `path.tmp` and renamed. Measured: 400 frames after a load, in
the same process or a new one, are the same frames and the same sound,
sample for sample, as the 400 that followed the save (ten ROMs, HDMA and
mode 7 among them).

## The second core

The contract runs a core on one worker and a core must not make tasks.
Where the time goes, on the Mac (a drawn frame): the 65816 and SPC700
interpreters, the renderer (from 10 % of the frame in a quiet scene to
95 % with colour math, `Transparency.sfc`), and the DSP mixing (up to a
quarter of the frame with 8 voices and echo). The renderer is the biggest
part but has no safe split: Snes9x draws lines in batches whenever a PPU
register changes, through globals (`GFX`, `BG`, the tile cache) that the
next batch overwrites. The mixing does: RetroESP32-P4 mixes on core 1
while core 0 runs the next frame. Here that is done deterministically,
through the host:

**What the host should provide** (proposed `emu_host_t` member, `NULL`
when there is none):

```c
/* Runs fn(arg, 1) on a helper pinned to the other core and fn(arg, 0) on
 * the calling thread, and returns when both have returned. Maps to
 * aos_hal_worker_split(). The helper needs a stack of 4 KB (it only
 * mixes sound; 8 KB to be safe for other cores) in PSRAM is fine; it never
 * calls the host's callbacks. */
void (*parallel)(void (*fn)(void *arg, int half), void *arg);
```

Until it exists, `snes_core.c` uses `snes_parallel_stub()`, which calls
`fn(arg, 1)` then `fn(arg, 0)` on the worker. When the host has it, the one
line in `snes_load()` that sets `s_parallel` takes `host->parallel` when
not NULL.

**What the core does with it** (`snes_split()`): half 0 runs the console
through this frame and draws it; half 1 mixes the sound of the frame
before. They share nothing while they run:

- the sound CPU's writes to the DSP are queued (`S9xSetAPUDSP()`, up to
  16384 a frame, more than it can make) and made in order after both halves
  are done, which is when Snes9x would have made them for its mix anyway;
- its reads of the DSP come from a picture of the DSP taken before the
  halves start, kept up to date with its own queued writes;
- the mixer reads its samples from a copy of the sound RAM, brought up to
  date between frames from a map of the 32-byte pieces the sound CPU
  stored to (its two first pages, zero page, I/O and stack, are copied
  whole). The ROM-shadow copies of `$FFC0` mark it too.

So the result does not depend on the order or on the halves overlapping:
the stub run 1-then-0, run 0-then-1, and two real threads (a pthread
`parallel()` in the Mac harness, also under ThreadSanitizer, no report)
give the same picture and the same sound, bit for bit. The price: the
sound goes out one frame (16.7 ms) later than the picture, and the sound
CPU sees the mixer's ENDX/ENVX/OUTX one frame later than Snes9x alone
shows them (the music test ROMs come out identical, one frame shifted;
a driver that times samples by polling ENDX could be 16 ms late). The
cost on core 0: about 600 bytes copied per frame plus the pieces the sound
CPU wrote, and the queue. With `EMU_SNES_SPLIT_SOUND 0` the core mixes
right after its frame, as Snes9x does, with no queue and no copy.

## Changes to Snes9x

Every change is marked `P4OS` in the source.

- `memmap.c`, `memmap.h`: `S9xInitMemory()` no longer allocates a 6 MB ROM
  buffer (the adapter reads the file into one of its own size, at least
  2 MB + 64 KB for the maps and the WWF patch, and calls `LoadROM(NULL)`);
  `SRAM_SIZE` back to 128 KB and the SRAM cap at 128 KB instead of
  Retro-Go's 32 KB; the `printf()`s gone; `sprintf` -> `snprintf`.
- `apu.c`, `apu.h`: `S9xSetAPUDSP()`'s static key-on latches moved to file
  scope, reset by `S9xResetAPU()` and saved in the states; the split sound
  (the deferred DSP writes, the DSP picture, the mixer's RAM copy and its
  dirty map); `rand()` (not in the firmware's table, used only with the
  APU stopped) replaced by a small generator of its own; the `#include`
  of the blargg APU replaced by an `#error`.
- `spc700.c`: `S9xAPUSetByte()` marks the 32-byte piece it stores to.
- `soundux.c`, `soundux.h`: `S9xDeinitSound()` (its buffers were never
  freed), the mixer's state for the save states, the channels' pointers put
  right after a load, the samples read from the mixer's RAM.
- `dsp.c`, `dsp.h`: `S9xDeinitDSP()` (the chip's struct was never freed, and
  a second game leaked the first's), `S9xDSPState()` for the states, the
  reset skips a chip whose allocation failed, `__builtin_clz` (libgcc's
  `__clzsi2` on rv32imafc, not in the firmware) -> `s9x_clz32()`.
- `port.h`: `stddef.h` and `stdio.h` (Retro-Go had them from
  `rg_system.h`), `s9x_clz32()`.

Found and worked around in the adapter (Snes9x as it is):

- snapshot.c called `S9xReschedule()` after a load; `CPU.WhichEvent` is in
  the state already and rescheduling again skipped line 0's HDMA: the first
  frame after a load had its top line and every HDMA effect one line off.
- A tile clipped at the left edge reads the depth buffer one byte before
  its start (AddressSanitizer, mode 5): the screen buffers have 64 bytes of
  margin on each side.
- `InitROM()` cuts the last letter of a 21-letter name ("breakou"): only
  the log shows it, left as is.

## Performance

Measured on the Mac (Apple Silicon, -O2, ms per frame; the board will be
some tens of times slower, not measured yet):

| ROM | drawn | sound off | not drawn |
|---|---|---|---|
| music.sfc (8 voices) | 0.065 | 0.048 | 0.047 |
| likemario.sfc | 0.119 | 0.109 | 0.058 |
| Mode7Perspective.sfc | 0.077 | 0.081 | 0.012 |
| Transparency.sfc | 0.173 | 0.169 | 0.007 |
| Mode5.sfc (512 px) | 0.076 | 0.075 | 0.006 |

Built with -O2 (spc700.c asks for O3 itself); not -O3, the code runs from
PSRAM through a 16 KB instruction cache. Memory, all heap (PSRAM): the
screens 2.4 MB, the ROM (at least 2.1 MB), the tile cache 512 KB + 4 KB,
WRAM 128 KB, SRAM 128 KB, VRAM 64 KB, registers 32 KB, sound RAM 64 KB
(+64 KB copy), the mixer's state 115 KB, the DSP queue 32 KB: about 6 MB
for a small game. Nothing in internal RAM.

`EMU_SNES_FAST_WORD` (board.cmake) is off: Snes9x reads 16-bit values with
one halfword load at any address when it is on, and nobody has checked
that this chip does misaligned loads in hardware (ESP-IDF has no trap
handler for them: a panic). Worth trying on the board: if a misaligned
`lhu` works, turn it on (the Mac and the simulator have it on).

## Tests

Mac harness (scratchpad, not in the repo): a fake host, frames to PNG,
sound to WAV, hashes of every frame and of the sound. ROMs: PVSnesLib
4.6.0's prebuilt examples (MIT, 70 ROMs: hello_world, breakout, likemario,
slopemario, Mode0/1/3/5/7, Mode7Perspective, HDMAGradient, Transparency,
WindowCircular, music, musicHiROM, sram, controller, typeconsole_pal...)
and undisbeliever's snes-test-roms (zlib). No commercial games.

- Pictures looked at: colours right, mode 7 with perspective, colour math,
  windows, the 512-pixel mode averaged, sprites.
- Sound: stereo, music at the expected level and pitch content.
- Pads: START pauses breakout, A shows on the controller test, RIGHT and B
  move and jump in likemario.
- Save states: identical frames and sound after a load (same process and a
  new one); truncated, random, other game's, layout-changed, empty and
  missing files refused with the game untouched.
- Battery: the sram example writes 0xCAFE with A, `sram_flush()` writes a
  2 KB `.sav`, a new run reads it back with B; nothing is written when
  nothing changed.
- load -> unload -> load, also between different games: the same frames
  as a fresh start; no allocation left after `unload()` (counted).
- AddressSanitizer + UBSan over all 70 ROMs with a state save/load, and
  six random "ROMs" of 256 KB-1.5 MB run for 700 frames: no memory error
  (UBSan reports the misaligned halfword reads of FAST_LSB_WORD_ACCESS and
  signed shifts in the mixer, both Snes9x's own and harmless on these
  CPUs).
- Simulator: the emulator lists the SNES games, opens Mode7Perspective at
  714x546 (8:7) with the SNES pad.
- Board: `tools/build_apps.sh emulador` passes, `snes.so` 260 KB, its
  undefined symbols all in the firmware's table.

## Limits

- No Super FX, SA-1, S-DD1, SPC7110, Seta DSPs (above).
- Snes9x 1.43-era accuracy: a few games known to need later Snes9x
  timing fixes misbehave (Snes9x2005's own compatibility list applies).
- High resolution comes out at 256 wide (averaged).
- The split sound's extra frame of DSP read lag (above).
