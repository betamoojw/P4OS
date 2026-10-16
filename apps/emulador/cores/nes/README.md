# NES core (nofrendo)

The Emulador's NES, Famicom and Famicom Disk System console: nofrendo as
[Retro-Go](https://github.com/ducalex/retro-go) carries it (commit `4ced120`,
`retro-core/components/nofrendo`), behind the `emu_core_t` table of
`main/emu_core.h`.

nofrendo is (c) 1998-2000 Matthew Conte and the people in
`nofrendo/CREDITS`, under the GNU Library General Public License version 2
(`nofrendo/COPYING`). Its game database (`nofrendo/database.h`) comes from
Mesen's, by way of Retro-Go.

## Files

- `nes_core.c`: the adapter, `emu_core_nes`. Video to RGB565, sound, the two
  pads, the options, the battery save, the save states.
- `nes_rg_shim.c` / `.h`: the three Retro-Go functions nofrendo calls
  (`rg_system_log`, `rg_crc32`) and `strdup`, which the firmware does not
  export.
- `nes_rename.h`: forced into every file of the core; puts `nes_` in front of
  each of nofrendo's globals that had a generic name (`apu_init`,
  `input_update`, `mem_reset`, `ppu_*`, `map4_intf`...). The library exports
  `emu_core_nes` and names starting with `nes_`, `nes6502_` or `nofrendo_`,
  nothing else.
- `sources.cmake`, `board.cmake`, `sim.cmake`: the build, the same sources
  for the board and the simulator.
- `nofrendo/`: the vendored core. Left out of Retro-Go's copy: `docs/` (the
  test ROMs zip and the hardware texts). Left out of the build:
  `nes/dis.c` and the placeholder mapper files (`map074.c` and others that
  only point at `map004.c`).

## What it plays

- `.nes` (iNES), NTSC and PAL. PAL comes from the database or from `(E)`,
  `(Europe)`, `(A)`, `(Australia)` in the file name. The mappers are
  nofrendo's: 0-5, 7, 9-11, 15, 16, 18, 19, 21-25, 30, 32-34, 40-42, 46,
  50, 64-66, 70, 71, 73-76, 78, 79, 85, 87, 93, 94, 119, 160, 162, 163, 176,
  185, 191-195, 206, 228, 229, 231 (20 is the FDS, 31 the NSF player).
  Another one fails the load with "Mapper N no soportado".
- `.fds`: needs the Famicom Disk System BIOS in the console's `bios` folder
  as `disksys.rom` (or `fds_bios.bin`, Retro-Go's name). Without it the load
  fails with "Falta la BIOS del Famicom Disk System (disksys.rom)". What a
  game writes to its disk is not kept.
- `.nsf`: music. Any button goes to the next song; the picture is black.
  nofrendo's player puts its own code at `$6000`, so a tune whose driver uses
  that RAM (some Codemasters ones) plays nothing.

## Options

| key | label | values | default |
|---|---|---|---|
| `palette` | Paleta | Nofrendo, Compuesta, NES Classic, NTSC, PVM, Suave | PVM |
| `overscan` | Recortar bordes | Sí, No | Sí: 8 lines top and bottom for NTSC (256x224), none for PAL, as Retro-Go |
| `crop_sides` | Recortar costados | Sí, No | No (Sí: 8 columns each side, 240 wide) |
| `sprite_limit` | Límite de sprites | Sí, No | Sí (8 a line, as the console; No: less flicker) |

## Sound and timing

32000 Hz, stereo with both channels equal. nofrendo makes a whole number of
samples a frame (533 NTSC, 640 PAL), so `fps()` reports 32000 / 533 =
60.04 (the console's 60.10, 0.1 % slower) and 50.0 for PAL: picture and
sound stay together without the app resampling.

## Battery saves

For iNES games with the battery bit: the 8 KB of PRG-RAM, raw (the `.sav`
other emulators use). Read at load, written by `sram_flush()` only when it
differs from what was last read or written. A reset (which is a power
cycle, not the reset button) keeps it.

## Save states

nofrendo's SNSS file plus a `NOFR` block of our own (see below). The adapter
walks the file before handing it to nofrendo, so a truncated or foreign
file is refused and the game goes on untouched.

## Changes to nofrendo

Every change is marked `P4OS` in the source.

- `nes/utils.h`: with `RETRO_GO` it includes `nes_rg_shim.h` instead of
  Retro-Go's `rg_system.h`, and passes the message's level on to the log.
- `nes/nes.c`: `nes_shutdown()` frees the FDS BIOS path (it leaked); the
  failed BIOS load no longer calls `fclose(NULL)` (newlib crashes on it).
- `nes/state.c`: a `NOFR` block, written last, with the state the SNSS blocks
  leave out: the PPU's scroll latch and read buffers, the CPU cycles left
  over from the frame and its pending interrupt, the APU's channels and
  frame counter, the pads' read position. Without it the first frame after
  a load had the wrong scroll and cycle-timed games stayed off for good;
  with it a loaded state replays frame for frame (picture and sound). The
  APU's channel structs go in raw with their sizes, and a build whose
  structs differ skips that part.
- `mappers/map019.c`: the mapper's state is a static object instead of a
  `malloc` that was never freed.
- `mappers/map020.c`: the FDS `irq` struct is `static` (it was a global
  named `irq`).
- `mappers/map031.c`: the NSF player starts from the file's first song on
  every load (it kept the last song of the previous file).
