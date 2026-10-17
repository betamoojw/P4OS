# Neo Geo core (gngeo)

The Emulador's Neo Geo (MVS arcade boards and AES home consoles): gngeo as
[RetroESP32-P4](https://github.com/giltal/RetroESP32-P4) carries it (commit
`339f17ff74eea4fe250d749fdcf0b5e544519c2a`, `components/gngeo`), behind the
`emu_core_t` table of `main/emu_core.h`, built as the module `neogeo.so`.

## Licences, part by part

| Part | Files | Licence |
|---|---|---|
| gngeo (Mathieu Peponas) and RetroESP32's changes to it | `gngeo/*.c`, `gngeo/*.h` except below | GPL-2.0-or-later (`gngeo/COPYING`); RetroESP32-P4 says MIT for its own repository but ships no licence file |
| Generator, the 68000 (James Ponder) | `gngeo/generator68k/` | GPL-2 (Generator's licence; the files only say "(c) James Ponder") |
| YM2610 (Jarek Burczynski, Tatsuyuki Satoh) | `gngeo/ym2610/ym2610.c` | **no licence in the file**: an old MAME `fm.c`/`ymdeltat.c` (MAME licence, non-commercial, at the time). The same authors' later version in MAME is GPL-2.0+ (`fm.cpp`, `ymdeltat.cpp`) |
| Neo Geo decryption and bootleg code (MAME: S. Smith, D. Haywood, F. Priuli...) | `gngeo/neocrypt.c`, `gngeo/neoboot.c` | **no licence in the files** (old MAME). The same code in today's MAME (`prot_cmc.cpp`, `prot_pcm2.cpp`...) is BSD-3-Clause |
| uPD4990A clock | `gngeo/pd4990a.c` | **no licence in the file** (old MAME); MAME's current `upd1990a.cpp` is BSD-3-Clause, a rewrite |
| Z80 (superzazu) | `z80/` | MIT (`z80/LICENSE`) |
| tinfl (miniz 2.1.0) | `tinfl.c`, `tinfl.h` | MIT (in the files) |
| The game list (`ng_drivers.c`) | generated from gngeo's `gngeo_data.zip` | GPL-2 data of gngeo |
| The adapter, loader, zip reader, platform | `ng_*.c`, `ng_*.h`, `tools/` | P4OS |

Left out on purpose: gngeo's MAME Z80 (`mamez80/`, "freeware for
non-commercial purposes", GPL-incompatible), replaced by superzazu's, and
Musashi (old non-commercial licence; Generator is used instead).

## Files

- `ng_core.c`: the adapter, `emu_core_neogeo`. The frame (the 68000 and the
  Z80 in 16 slices, the YM2610's timers between them; line by line for the
  games that change the picture mid-screen), the picture, the sound, the
  pads, the options, the battery RAM, the save states.
- `ng_load.c`: from the zip to a console ready to start: the game's set, its
  parent's (for clones), the BIOS, the caches on the card (below).
- `ng_zip.c`: a zip reader with streams (tinfl), two at a time.
- `ng_drivers.c` / `.h`: the 237 games gngeo knows (ROM files, regions,
  sizes) and the CMC decryption tables, made by `tools/gen_drivers.py` from
  gngeo's `gngeo_data.zip` (not in the repository). RetroESP32 asks for
  that zip on the card; here it is compiled in.
- `ng_platform.c`: gngeo's `printf` to the log (80 lines a game), the jump
  that replaces `exit()`, the allocator with the core's PSRAM budget, the
  save states' buffer.
- `ng_z80.c`: the sound CPU on superzazu's Z80, gngeo's `cpu_z80_*`.
- `ng_p4os.h`: forced into gngeo's files: `printf`, `strdup`, `exit` to the
  core's own (none of them is lent by the firmware, or they must not stop
  the app).
- `gngeo/`: the vendored files, `z80/`, `tinfl.*`.
- `sources.cmake`, `board.cmake`, `sim.cmake`. Like the GBA core, the board's
  library and the simulator's are linked with `-r` into one object that
  keeps only `emu_core_neogeo` global (gngeo's names are generic:
  `memory`, `conf`, `regs`...).

## On the card

```
/sdcard/emulador/neogeo/
    bios/neogeo.zip         MAME's BIOS set (and/or aes.zip), or its files loose
    mslug.zip               a game, MAME's set, named as in MAME
    mslug/                  the core's cache for that game (made once)
```

- **The BIOS is required** and not included. `load()` fails with "Falta la
  BIOS de Neo Geo: neogeo.zip de MAME (sp-s2.sp1, sfix.sfix) en la carpeta
  bios" without it. The core takes, from `neogeo.zip`, `aes.zip` or loose
  files in `bios/`: an MVS BIOS by region (`sp-s2.sp1`, `sp-s.sp1`,
  `sp-e.sp1` Europe; `sp-u2.sp1`, `sp1-u2`, `sp1-u4.bin` USA; `vs-bios.rom`,
  `sp-j2.sp1`, `sp1.jipan.1024`, `sp-j3.sp1`, `japan-j3.bin` Japan;
  `sp-45.sp1`, `asia-s3.rom` Asia; `neo-geo.rom`), an AES BIOS (`neo-epo.bin`,
  `neo-po.bin`, `aes-bios.bin`) or the Universe BIOS (`uni-bios*.rom`), and
  `sfix.sfix` (or `sfix.sfx`, `ng-sfix.rom`). The zoom table `000-lo.lo` is
  used when present and made by the core otherwise. When the system asked
  for has no BIOS, the core takes any other it finds.
- **Games** are MAME `.zip` sets, non-merged or split: a clone needs its
  parent's zip next to it. Files are matched by CRC, then name, then name
  without the extension (gngeo's list uses the old `.bin` names). The zip's
  name picks the game: a renamed zip is "No es un juego de Neo Geo conocido".
- **The cache**: the sprite ROMs (4 to 64 MB) and the sample ROMs (2 to
  16 MB) of most games do not fit in the core's 15.5 MB with everything
  else. The first time such a game opens, the core writes them, converted,
  to `<game>/`: `<game>.ctile` (the sprite tiles in gngeo's layout),
  `<game>.cusage` (2 bits a tile: which are blank), `<game>.vroma` (and
  `.vromb`). It takes as long as inflating the zip and writing the files
  (tens of seconds for a 64 MB game on the card, estimated: not measured on
  the board) and then the game plays from them: a PSRAM cache of
  4 KB banks for the sprites (filled before each frame is drawn, in file
  order, runs of banks in one read), a 2 or 4 MB page cache for the samples.
  A clone uses its parent's cache when its own is missing and the sizes
  match. Delete the folder if the zip changes. The files are the same as
  RetroESP32-P4's `gen_cache.py` writes, so a cache made there works.
- **Encrypted games** (CMC42/CMC50 sprites: King of Fighters '99 to 2003,
  Garou, Metal Slug 3 to 5, Rage of the Dragons, Matrimelee and the others
  in `roms.c`'s `init_func_table` except kof98) need their whole sprite ROM
  in memory to decrypt, which never fits: they need a cache made on a
  computer (RetroESP32-P4's `gen_cache.py`; it does not decrypt the PCM2
  sample ROMs, so the later ones among them have wrong samples). Without
  one: "Este juego tiene los gráficos cifrados: hace falta su caché hecha en
  una computadora". Their program and sound ROMs are decrypted at load.
- **Battery save** (`saves/<game>.sav`): the MVS's 64 KB backup RAM (high
  scores, the board's settings and coin counts) followed by the 2 KB memory
  card (the AES's saves), as the 68000 sees them (gngeo's `.nv` plus its
  `memcard`). A 64 KB file is read as the backup RAM alone. Written by
  `sram_flush()` only when it changed (the MVS BIOS changes it often, so
  about every 30 s while playing).

## The table

| field | value |
|---|---|
| id / name / short_name | `neogeo` / `Neo Geo` / `NEO` |
| exts | `.zip` |
| max_w × max_h | 320 × 224 (304 × 224 by default, 8 columns cropped each side) |
| pixel_aspect | 0 (square) |
| players | 2 |
| buttons | d-pad, A B C D, SELECT, START |
| face | `EMU_FACE_ARC4` |
| labels | NULL (A B C D are the console's own) |
| fps | rate / samples a frame: 59.115 (22 kHz, 373), 59.195 (44.1 kHz, 745), 59.274 (11 kHz, 186); the MVS is 59.186 |
| sample_rate | 22050 by default |

SELECT is the coin on the MVS (player 1 coin 1, player 2 coin 2) and SELECT
on the AES.

## Options

| key | label | values | default |
|---|---|---|---|
| `system` | Sistema | MVS (arcade), AES (consola), Universe BIOS | MVS |
| `region` | Región | Europa, EE. UU., Japón, Asia | Europa |
| `crop` | Recortar costados | Sí, No | Sí (304 wide) |
| `sound` | Calidad de sonido | Alta (44 kHz), Media (22 kHz), Baja (11 kHz) | Media |
| `spr_cache` | Caché de sprites | Automática, 4 MB, 8 MB, 12 MB | Automática |

System and region swap the BIOS and start the console again (the app gives
the kept values right after `load()`, before the first frame); a system
whose BIOS is missing keeps the one loaded. The sound rate is taken when the
sound starts (the first frame or state after a load): a change later waits
for the next game. The sprite cache's size is for the next game; Automática
is what the budget leaves (about 7 MB with a 2 MB program and the samples
streamed), capped at 12 MB. Speed against accuracy: the sound rate (the
YM2610's work grows with it) and the sprite cache (smaller: more reads from
the card when a scene changes).

## Save states

`NGST`, a version, the game's name, the length and CRC-32 of the rest, then
each part's fields one by one: the 68000's registers and bank, the Z80's
registers and its 64 KB, the work RAM, video RAM, palettes and the video
chip's registers, the backup RAM and memory card, the latches of
`memory.c`, the YM2610 (gngeo's own field list), the timers, the clock
chip. About 380 KB. The whole file is read and checked (magic, version,
game, length, CRC) before anything is touched: a short, damaged or another
game's file is refused and the game goes on. Written to `path.tmp` and
renamed.

## Memory

Everything big comes from `ng_big_alloc()`, which keeps the core under
15.5 MB of PSRAM (`NG_BUDGET` in `ng_core.h`; the simulator, where pointers
are twice as big, gets the difference so it decides as the board): the
68000's decode tables (768 KB) and its block cache (2 MB, 1 MB if short),
the memory map's tables (112 KB), gngeo's `neo_mem` (300 KB), the YM2610's
tables (154 KB), the Z80's 64 KB, the picture (352 × 256), the ROMs and the
caches. All of them were `.bss` in RetroESP32 (a firmware of its own); here
they are allocated by `load()` and freed by `unload()`. The core's own
`.bss` is 39 KB (the YM2610's state, the PVC protection's 8 KB). Nothing is
in internal RAM. Which games fit in memory without the card: those whose
sprite + sample ROMs fit in what the budget leaves (about 9 MB with a 1 MB
program: the 1990-1992 games); the rest play from the cache.

## Changes to the vendored files

Every change is marked `P4OS` in the source.

- `include/SDL.h`: cut to the types, the surface, the byte swaps and
  `SDL_FillRect` (the ESP-IDF timing, audio and event stubs gone).
- `emu.h`: the desktop's key, joystick and hotkey fields and globals gone;
  emu.c's prototypes reduced to `neogeo_reset()` (emu.c is not vendored).
- `memory.h` / `memory.c`: `neo_mem` allocated (`memory` is `(*ng_mem)`);
  the beam's line from the cycles in integers (`/ 768`, a double division
  before); the sound command goes to the Z80 on the same thread (an NMI and
  300 cycles, as gngeo) instead of RetroESP32's queue to core 1; a coin
  trace removed; the memory map's tables are pointers; `ng_mem_state()` and
  `ng_mem_reset_statics()` added.
- `video.h` / `video.c`: the sprite cache reads its banks with `fread` from
  the `.ctile` file (`ng_spr_read_banks()`) instead of `disk_read` through a
  FATFS sector map; RetroESP32's prefetch kept, its reads sorted and
  batched by file order; invisible tiles no longer read their bank; the
  cache's blocks under the budget; hit/miss counters for `stats()`; no
  on-screen message, fps text, timing probes or `screen_update()`.
- `roms.c`: the loader (from `allocate_region()` to the end) removed,
  replaced by `ng_load.c`; `init_roms()`, `game_needs_gfx_decrypt()`,
  `convert_roms_tile()` made non-static; `convert_all_tile()` removed.
- `neocrypt.c`: the CMC tables from `ng_drivers.c` instead of
  `gngeo_data.zip`.
- `mame_layer.c`: comment: `exit()` there is `ng_fatal()`.
- `timer.c` / `timer.h`: `ng_timer_init()` (the step from the MVS's frame,
  and a clean start for every game; it was set once per process at 60 Hz);
  `ng_timer_state()`.
- `pd4990a.c`: `localtime_r`; the year's tens for years past 1999 (12 tens
  in a BCD nibble before); a clean start per game; `ng_rtc_state()`.
- `adpcm_cache.c`: its blocks under the budget.
- `ym2610/ym2610.c`: `tl_tab` and `lfo_pm_table` allocated
  (`ng_ym_alloc()`); `powf`/`sinf`/`logf`/`floorf` in the table set-up (the
  double functions are not lent by the firmware); RetroESP32's
  per-sample timing (five clock reads a sample) and the unused 64 KB
  mixing buffer removed; the state on `ng_st_t` (`ng_ym_state()`, all of
  `BusyExpire`).
- `ym2610/2610intf.c`: `ng_ym_timers_relink()` after a state is read.
- `generator68k_interf.c`: the tables allocated (`cpu_68k_alloc()`); the
  first memory map entry ended at `0x1000`, one past the tables (found with
  AddressSanitizer); the clocks count from the frame's start
  (`cpu_68k_frame_start()`) so the frame can run in slices; the registers'
  state field by field (`ng_68k_state()`); `cpu_68k_init_bios()`; the
  console debugger and disassembler compiled out.
- `generator68k/cpu68k.c`, `cpu68k.h`, `mem68k.h`, `reg68k.c`: the decode
  tables and the block hash allocated, the block pool 2 MB from the budget
  at load (3.5 MB, allocated lazily, before); a runaway block is cut
  instead of `exit()`; flush counter; perf counters removed; the IPC printer
  (needs the disassembler) compiled out.
- `z80/z80.c`, `z80.h`: 16-bit ports (the sound program banks with the
  upper byte), a flat 64 KB memory, no stdio.

## How it runs

One frame: 202752 cycles of the 68000 and 67584 of the Z80 in 16 slices,
the YM2610's timers after each Z80 slice, then the picture (gngeo's
renderer into a 352 × 256 buffer; the host gets a pointer into it with its
stride, no copy), the vertical interrupt, and one frame of sound. The
games in RetroESP32's raster list (Riding Hero, League Bowling, Super
Sidekicks 1-4, Neo Turf Masters, Ironclad) run line by line for the timer
interrupt. A natural split for the second core, as RetroESP32 does it: the
Z80, the YM2610 and its sound on core 1, the 68000 and the picture on
core 0, the sound commands queued between them; not done here (the brief:
no tasks of the core's own).

## Tests

On the Mac with a harness (in the session's scratchpad, not in the repo):
a test cartridge written for this (68000 and Z80 assembly, vasm), in the
`puzzledp` set's layout and in `mslug`'s (16 MB of sprites, 8 MB of
samples, to make the core stream), booted by **nullbios 0.5** (ngdevkit's
open BIOS, LGPL-3.0, from its Debian package; no SNK BIOS was used). It
checks the text layer, palettes, sprites with shrinking and sticky chains,
both pads, the coin, the sound commands, the SSG and an ADPCM-A sample,
the backup RAM (a boot counter kept across loads in the `.sav`), save
states (frames after a reload identical to continuing; damaged, short and
other games' states refused with the game untouched), load→unload→load
(nothing left allocated), the cache's creation, frame-for-frame identical
pictures between the sprites in memory and streamed (with tiles changing
every frame across 2 MB with a 4 MB cache, and across 8 MB with 4 MB and
7 MB caches: same frames), the three sound rates, AES/Universe/region BIOS
switching, the raster path, and AddressSanitizer clean. Also in the
simulator: the library lists it as NEO, the game plays with the ARC4 pad.
No commercial game nor SNK BIOS was run: **the board test needs the user's
BIOS and games**.

On the Mac (Apple Silicon) a heavy synthetic frame (96 full-height sprites,
about 1300 tiles, plus a busy 68000 and Z80) takes 0.24 ms (0.15 ms of it
drawing). The board is not measured.

## Known limits

- Encrypted (CMC) games need a cache made on a computer; PCM2 sample
  decryption is not done for them from such a cache.
- The first open of a big game writes its cache (tens of MB on the card,
  tens of seconds) with no progress shown but the log.
- gngeo's accuracy: no MVS multi-slot, no memory card management beyond the
  2 KB, the raster effects only for the listed games, sprite limits not
  emulated.
- The 68000's block cache does not see code written to RAM (gngeo
  interprets RAM code without caching, as before).
- The SD reads of the sprite cache go through stdio/FATFS: a seek backwards
  walks the file's cluster chain (FATFS's fast seek is off in the firmware).

## Measured on the board: Metal Slug (2026-10-17)

MAME `mslug.zip` (16 MB of sprite ROM, 8 MB of samples), MVS BIOS, the
board with nothing else open (~17.5 MB of PSRAM free):

| | sprite cache | hits | card reads | frames run / drawn a second | ms a frame |
|---|---|---|---|---|---|
| as ported | 4-5 MB | 52-66 % | ~600 a second | 1.8 / 0.9 | 560 |
| frame-aware eviction | 5.5 MB | 91-96 % | ~400 a second | 17.8 / 4.8 | 55 |
| + budget from the free PSRAM, 68k pool and sample cache 1 MB each | 7.9 MB | 97-99 % | 0-5 a second | 59.1 / 57-59 | 13-17 |

What changed (all marked `P4OS`):

- **Eviction** (`gngeo/video.c`): the cache replaced banks round-robin, so
  a frame whose banks filled most of it (Metal Slug: ~900 of 1268-1390
  slots) pushed out banks it was about to draw and read them back over and
  over. Each slot now carries the number of the frame that last needed it
  (the prefetch marks every bank the frame will draw), and eviction passes
  over the slots of the frame being drawn.
- **Budget** (`ng_platform.c`, `ng_core.c`): from the PSRAM free when the
  game loads, minus 1.2 MB for the system (8 to 17 MB), instead of a fixed
  15.5 MB; the sprite cache's "Automática" takes all that is left.
- **Smaller neighbours:** the Generator 68000's block pool 1 MB (it was 2;
  Metal Slug flushes it a few times either way), the ADPCM-A cache 1 MB
  (it was 2).
- **A cache that does not fit** is tried 2 MB smaller at a time, down to
  1 MB (`ng_load.c`).

A game started while the core was too slow (before these changes) may
have saved the MVS BIOS's "CALENDAR ERROR" screen in its automatic state:
start it from the beginning (a long press in the library).
