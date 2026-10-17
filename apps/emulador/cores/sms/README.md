# Sega 8-bit core (SMS Plus): Master System, Game Gear, ColecoVision

One library, three consoles of the Emulador: `emu_core_sms` (Master System,
which also takes SG-1000 cartridges), `emu_core_gg` (Game Gear) and
`emu_core_col` (ColecoVision), behind the `emu_core_t` tables of
`main/emu_core.h`. `board.cmake` and `sim.cmake` set `EMU_CORE_IDS sms gg
col`; on the board the library is the module `sms.so`.

The video, the mappers, the I/O chip and the TMS9918 modes are SMS Plus as
[Retro-Go](https://github.com/ducalex/retro-go) carries it (commit
`4ced120`, 2026-01-19, `retro-core/components/smsplus`). The Z80 and the
SN76489 are new, written for this core (see "Licences").

## Licences

- **SMS Plus** (`smsplus/`): (c) 1998-2007 Charles MacDonald, with code by
  Eke-Eke from SMS Plus GX; GNU GPL version 2 or later (`smsplus/COPYING`,
  `smsplus/CREDITS`).
- **The Z80** (`z80/`) and **the SN76489** (`sms_psg.c`), and the adapter:
  written for P4OS, GPL-2.0-or-later like SMS Plus.

Left out of Retro-Go's copy, on purpose:

- `cpu/z80.c`, Juergen Buchmueller's "Portable Z80 emulator" 3.9 from MAME.
  Its notice allows non-commercial use only, which the GPL does not permit
  to add. It is also not the original any more: Retro-Go's rewritten macros
  read past the parity table in RLC/RRC (an unmasked index), leave H clear
  on INC to 80h and treat a DD/FD prefix before an ordinary opcode as a
  no-op.
- `sound/sn76489.c` (Maxim's): no licence is stated anywhere in its
  sources.
- `sound/emu2413.c`, `ym2413.c`, `fmintf.c` (the FM unit): disabled with
  `#if 0` in Retro-Go's copy anyway, and of other licences.
- `coleco_bios.h`: the ColecoVision BIOS, Coleco's code, built in. The core
  reads it from the card instead.
- `state.c` (replaced by the adapter's state files), `loadrom.c`'s file
  reader (the adapter reads the file).

## Files

- `sms_core.c`: the adapter, the three tables. Reads the cartridge and the
  ColecoVision BIOS, converts the 8-bit picture to RGB565, the pads and the
  ColecoVision keypad, the options, the battery save, the state files.
- `z80/z80.c`, `z80/z80.h`: the Z80, with the interface SMS Plus expects of
  MAME's (`z80_execute()`, the `Z80` register block, `cpu_readmap`,
  `z80_set_irq_line()`...), so SMS Plus's sources call it unchanged.
- `sms_psg.c`, `sms_psg.h`: the SN76489.
- `smsplus/`: the vendored part. `sources.cmake` (shared), `board.cmake`,
  `sim.cmake`: the build.

Symbols: SMS Plus's globals have generic names (`sms`, `vdp`, `cart`,
`option`, `render_line`, `z80_execute`...). As the GBA core does, the
library's objects are linked with `-r` into one and every global but the
three tables is made local (`objcopy --keep-global-symbol` on the board,
`ld -r -exported_symbol` on the Mac).

## The Z80

Every documented instruction with the undocumented parts games and test
programs rely on: the X/Y flags, IXH/IXL/IYH/IYL, SLL, the DD CB register
copies, the ED mirrors (NEG, RETN, IM), `IN F,(C)`/`OUT (C),0`, MEMPTR for
`BIT n,(HL)`, the block instructions' flags (LDI/CPI/INI/OUTI and their
repeats, one pass per instruction). Interrupts: IM 0 (RST from the bus,
FFh = RST 38h on these consoles), IM 1, IM 2, NMI on the rising edge, the EI
shadow, HALT (the rest of the slice is spent at once: nothing inside a slice
can interrupt it). T-states per instruction, no wait states.

It passes **ZEXDOC and ZEXALL** (Frank Cringle's exercisers, every test
"OK", run in a CP/M stand-in on the Mac). The stack starts at DFF0h, where
the BIOS leaves it (Shadow Dancer and Ace of Aces count on it).

## The SN76489

Three square waves and a noise channel, 2 dB volume steps. Sega's variant
(Master System, Game Gear: 16-bit noise register, taps 0 and 3) or the
SN76489A (ColecoVision, SG-1000: 15 bits, taps 0 and 1); the Game Gear's
stereo register. The chip counts at clock / 16 and each output sample
averages the counts it spans (a box filter); periods of 6 or less are held
high, so the volume-written samples games play come out. A 35 Hz high-pass,
as the consoles' output capacitor, takes the DC away. It renders a scanline
at a time, so writes land where they happened.

## Changes to SMS Plus

Every change is marked `P4OS` in the source.

- `shared.h`: fixed-width types (`uint32` was `unsigned long`, 64 bits on
  the Mac, and the renderer's 32-bit writes wrote 8 bytes), no
  `<malloc.h>`, the log to the host (`sms_log`), asserts compiled out (the
  adapter checks every allocation after power on), the new Z80 and sound.
- `sound/sound.c`, `sound/sound.h`: rewritten around `sms_psg.c`: one
  interleaved stereo buffer of a whole number of samples a frame, rendered
  per line; the FM unit is never found (Japanese games play their PSG
  music).
- `render.c`: RGB565 in the CPU's byte order (Retro-Go swapped it for its
  SPI panels); Retro-Go's 256 KB bit-plane table replaced by a 1 KB one
  (the same packed row from four lookups); the 64 KB priority table made
  once and freed with the TMS tables at shutdown (they leaked at every
  load); the background row written through a byte pointer (with a fine
  scroll it is not 4-byte aligned); a reset clears the line state; state
  helpers for the sprites parsed for the next line.
- `tms.c`: no assert on the tables; freeing them; its latched sprites for
  the state files.
- `pio.c`: the paddle and light-gun latches cleared at reset; state helpers
  for the I/O chip's pins.
- `memz80.c`: the open-bus read took the page with `pc >> 13` (should be
  `>> 10`); the BIOS auto-detection wrote into a NULL BIOS buffer; with no
  BIOS loaded, selecting the BIOS slot keeps the cartridge.
- `sms.c`: mapper writes with no ROM in the slot do nothing (they built
  pointers from NULL).
- `loadrom.c`: no built-in ColecoVision BIOS, no `abort()` on a failed
  allocation, `load_rom_file()` removed.

## The tables

| | `sms` | `gg` | `col` |
|---|---|---|---|
| name / short | Master System / SMS | Game Gear / GG | ColecoVision / Coleco |
| exts | `.sms .sg` | `.gg` | `.col` |
| max | 256 x 240 | 256 x 192 | 256 x 192 |
| pixel aspect | 8/7 | 6/5 | 8/7 |
| players | 2 | 1 | 2 |
| face | ROW2 | DIAG2 | ROW2 |
| buttons | d-pad, A, B, START | d-pad, A, B, START | d-pad, A, B, L, R, SELECT, START |
| labels | B "1", A "2", START "PAUSE" | B "1", A "2", START "START" | B "Izq", A "Der", L "1", R "2", SELECT "*", START "#" |

- Pictures: 256 x 192 (the Master System's 224- and 240-line modes when a
  game sets them), the Game Gear's 160 x 144 window (256 x 192 for the Game
  Gear games in Master System mode), ColecoVision 256 x 192. No overscan
  border; the Master System's blanked left column is shown as the game
  leaves it (the backdrop colour).
- **Master System**: START is the console's PAUSE button (an NMI), from
  either pad. No RESET button (the app's reset is a power cycle). `.sg`:
  SG-1000 cartridges (TMS9918 modes, SN76489A), untested with a real one.
- **Game Gear**: START; in Master System mode it is PAUSE as well. Region
  never makes it PAL (every Game Gear is 60 Hz).
- **ColecoVision**: the left and right side buttons are B and A; four of
  the keypad's twelve keys are on the pad: **L = 1, R = 2, SELECT = *,
  START = #** (enough to pick a skill level and start: "1" for most games).
  Needs the BIOS in the console's `bios` folder: `coleco.rom` (or
  `colecovision.rom`), 8 KB; without it the load fails with "Falta la BIOS
  de ColecoVision (coleco.rom)". Cartridges up to 32 KB (no mega-carts).
  Super Game Module RAM as SMS Plus has it (ports 53h/7Fh), no AY sound.

## Sound and timing

44100 Hz, a whole number of samples a frame worked out from the console's
own frame (CPU cycles per frame over its clock): 736 a frame for NTSC, so
`fps()` = 44100 / 736 = **59.92** (the console's 59.922), 887 for PAL =
**49.72** (49.70). Stereo (the Game Gear's stereo register; equal channels
otherwise).

PAL or NTSC: the region option, or SMS Plus's database (the European games
that need 50 Hz) and the cartridge's header. The region is set at load: the
app's `set_option` right after `load()` applies it; a change later takes
effect on the next load (the app reads `fps()` once).

## Options

| key | label | values | default | tables |
|---|---|---|---|---|
| `region` | Región | Automática, Estados Unidos, Europa, Japón | Automática | sms, gg |
| `sprite_limit` | Límite de sprites | Sí, No | Sí (8 a line, 4 in TMS modes; No: less flicker) | all |
| `palette` | Paleta | Sean Young, Omar Cornut, Richard Drushel | Richard Drushel | col |

## Battery saves

Master System and Game Gear: the 32 KB of cartridge RAM the Sega mapper
can map, raw (what SMS Plus GX and Genesis Plus GX write; an 8 KB file from
another emulator loads into the first 8 KB). Written by `sram_flush()` only
when the game has mapped its RAM (or a file existed) and the RAM differs
from what was last read or written. A reset keeps it. ColecoVision and
SG-1000: none.

## Save states

SMS Plus's structures as they are in memory (the console, the VDP and its
16 KB, the cartridge's registers and RAM, the Z80, the cycle count carried
into the next frame, the PSG, the renderer's per-line sprites, the I/O
chip), behind a header with the build's structure sizes, the console and
the cartridge's CRC. The whole file is read and compared before anything is
touched: a short, damaged or foreign file is refused and the game goes on.
Written to `path.tmp`, then renamed. A loaded state replays frame for frame
(picture and sound) as the saved game would have.

## Tests (on the Mac, `scratchpad/harness`, not in the repo)

A fake host loads a ROM, runs frames with scripted input, writes PNG
frames and a WAV, saves a state, runs 300 frames, loads it, runs them
again and compares (picture and sound hashes), feeds a truncated and a
damaged state, unloads and reloads (same frames), and times 3000 frames.
Also built with AddressSanitizer and UBSan over every ROM below: clean.

- ZEXDOC, ZEXALL (GPL, Frank Cringle): all OK.
- *Raposa do Sol* v0.20 (tibonev, MIT), *Stalactites* 0.6 and *River
  Strike* 0.10 (haroldo-ok, MIT): Master System, title screens and play,
  colours right; Raposa do Sol maps its cartridge RAM (the .sav path was
  exercised with it: a change written as 32 KB, read back at the next
  load).
- *Tunnel* (liamhays, GPL-3.0): Game Gear, 160 x 144, START.
- ColecoVision: a stand-in 8 KB BIOS and a test cartridge assembled for the
  test (VDP Graphics I, a sprite moved by the stick, the backdrop set from
  the keypad: 1 * # checked, a 440 Hz tone measured). No real BIOS or
  commercial game was used.
- Every table: state round trip identical, refused bad states, reload
  identical. Speed on the Mac: ~4,600 frames/s drawing, ~20,000 skipping
  (the board not measured yet).

## Known limits

- No FM sound (Japanese Master System / Mark III FM unit).
- No light gun, paddle or Sports Pad (the database still selects them for
  those games, which then cannot be controlled), no 3D glasses (SMS Plus
  shows one eye's frame).
- No Game Gear link cable, no Master System BIOS.
- ColecoVision: four keypad keys only, no mega-carts, no Super Game Module
  AY sound.
- The palette is taken at the end of the frame (mid-frame palette changes
  show the last one), as SMS Plus does.
- Second-core split: none here; the Z80 and the line renderer interleave
  per scanline (writes to the VDP render the line first), so a renderer on
  the other core would need a per-line VRAM snapshot.
