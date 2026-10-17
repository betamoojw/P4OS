# PC Engine core (pce-go)

The Emulador's PC Engine / TurboGrafx-16: pce-go as
[Retro-Go](https://github.com/ducalex/retro-go) carries it (commit
`4ced120`, 2026-01-19, `retro-core/components/pce-go`), behind the
`emu_core_t` table `emu_core_pce` of `main/emu_core.h`. HuCards only. On the
board the library is the module `pce.so`.

pce-go is Retro-Go's PC Engine emulator for microcontrollers, derived from
HuExpress (Alex von Gluck IV), itself a fork of Hu-Go! (Zeograd and others),
a fork of Bero's PCE emulator; GNU GPL version 2 (`pce-go/COPYING`, authors
in `pce-go/CREDITS`).

## Why pce-go and not HuExpress

RetroESP32-P4 carries HuExpress (`components/huexpress`). pce-go was taken
because:

- **Speed and size**: pce-go is HuExpress cut down for microcontrollers by
  Retro-Go (one `PCE` structure, a switch-dispatched HuC6280, a tile and
  sprite renderer that works on batches of lines between register
  changes). ~5,500 lines against HuExpress's ~12,000 in `engine/` plus its
  glue; Retro-Go runs it on the 240 MHz ESP32 (skipping every other frame),
  and the P4 has half again the clock and a second-level cache.
- **One frame at a time**: pce-go's loop body is `pce_run()`, a frame; the
  P4 port of HuExpress runs a blocking `RunPCE()` with video and sound
  through OSD callbacks and its own sound task, which the core contract
  (one `run()` a frame, from the app's worker) does not fit.
- **What HuExpress has more** is the CD-ROM², the Arcade Card and the
  System Card: none of it is in this core's scope (HuCards only).

## Files

- `pce_core.c`: the adapter, `emu_core_pce`. Reads the HuCard, the
  picture to RGB565 (and the wide modes to TV proportions), the pads, the
  backup RAM as a .sav, the state files through a temporary file.
- `pce-go/`: the vendored core. Left out: `docs/`, `ports/`, Retro-Go's
  `CMakeLists.txt`. `h6280_dbg.h` (the opcode names, for tracing) stays
  because `h6280.c` includes it; nothing references it when tracing is off.
- `sources.cmake` (shared), `board.cmake`, `sim.cmake`: the build. As the
  GBA core does, the objects are linked with `-r` into one and every global
  but `emu_core_pce` is made local (pce-go's names are generic: `PCE`,
  `gfx_run`, `psg_update`, `LoadState`...).

## Changes to pce-go

Every change is marked `P4OS` in the source.

- `pce-go.h`: the log to the host (`pce_log`, errors and warnings only) and
  the adapter's CRC; `RunPCE()`'s endless loop, `LoadFile()` and the
  `osd_input_read()`/`osd_vsync()` callbacks gone; `RunFramePCE()`, one
  frame with its sound.
- `pce.c`: `pce_run()` asks the PSG for samples a scanline at a time, so
  DDA (sampled sound) lands when the game wrote it, and the frame makes a
  whole number of samples; a reset clears what it left over (the carried
  cycles, the pads' multiplexer, the I/O buffer, the scroll offset) and
  re-seeds the noise generators, which its own PSG `memset` had zeroed
  (the noise channels were silent for good); the backup RAM (below).
- `pce.h`: the screen size held to the buffer (a game's 512-dot mode or a
  tall VDW wrote past the lines and past the buffer's end); 16-bit reads
  and writes of memory byte by byte (vectors and zero-page pointers sit at
  odd addresses, which RISC-V may not load as one word); the `BRAM` field.
- `gfx.c`: a reset clears the latched scroll context too.
- `psg.c`: the channel volume in integers (it multiplied by a `double`,
  soft-float on the board, for every channel of every update).
- `pce-go.c`: the state file (below); the Populous card's extra RAM
  allocation checked.

## Backup RAM and the .sav

pce-go has no backup RAM. This core adds the 2 KB of the Tennokoe Bank /
TurboDuo at bank F7h, formatted empty (the `HUBM` header the System Card
writes) and always unlocked, so the HuCard games that can keep their saves
in a Tennokoe Bank, TurboBooster-Plus or Duo do here. The .sav is those 2 KB raw, as Mednafen keeps it; written by
`sram_flush()` only when they changed; a reset keeps them.

## Save states

pce-go's tagged blocks (`"PCE_P4O1"` header, Retro-Go's was `"PCE_V010"`),
with more of the machine than Retro-Go saved: the whole CPU, VCE, VDC,
timer, pads and sound generator (the channels' DDA buffers and counters,
which it cut at 40 bytes), the scanline and scroll offset, the I/O buffer,
the backup RAM, and an END block. `LoadState()` reads the whole file and
walks every block before it applies any: each must be one this build knows,
with exactly its size, up to END and the end of the file, or the file is
refused and the game goes on. The Street Fighter II mapper's bank is put
back before the MMRs. Written to `path.tmp`, then renamed. A loaded state
replays frame for frame (picture and sound). Populous's 32 KB card RAM is
not in the state.

## The table

| field | value |
|---|---|
| id / name / short | `pce` / PC Engine / PCE |
| exts | `.pce` |
| max | 352 x 242 |
| pixel aspect | 8/7 (the 5.37 MHz dots of the 256 mode) |
| players | 2 (pce-go's multitap: pads 1 and 2) |
| face | ROW2 |
| buttons | d-pad, A, B, SELECT, START |
| labels | B "II" (left), A "I" (right), START "RUN" |
| fps / rate | 44100 / 737 = 59.84 (the console's 59.83) / 44100 Hz stereo |

## Width changes

A game picks its width with the VDC's HDR (256, 320, 336 or 352 dots; some
switch between screens), and the app re-lays the picture out when the
frame's size changes. The 7.16 MHz modes put a third more dots in the same
width of the TV than the 256 mode the table's pixel aspect is for, which
the contract's single `pixel_aspect` cannot express. So by default (option
"Modos anchos": "Proporción de TV") a frame wider than 256 is compressed
four dots to three (352 becomes 264, 336 becomes 252, 320 becomes 240,
weighted RGB565 averages): the geometry is the TV's. "Píxeles enteros"
sends the dots as they are (sharper, 33 % too wide). The 512-dot mode
(10.7 MHz, a handful of games) is cut at 352. The height is the VDC's
active lines (VDW+1, at most 242), 240 or 224 for most games.

## Options

| key | label | values | default |
|---|---|---|---|
| `wide` | Modos anchos | Proporción de TV, Píxeles enteros | Proporción de TV |

## Tests (on the Mac, `scratchpad/harness`, not in the repo)

No HuCard homebrew with an open licence and a released ROM turned up, so
the test card was assembled for the test: 256-dot mode, a palette, two
tiles over the whole map, a 16x16 sprite moved by the d-pad through the
vblank interrupt, the pads read through the multiplexer (I turns the
backdrop red, RUN switches to 352 dots, SELECT back to 256), a PSG channel
playing a square wave (440 Hz measured). Checked: the picture and colours,
the sprite following the pad, the 352 mode (compressed to 264), the
buttons; state round trip identical (picture and sound), truncated and
damaged states refused, reload identical, the .sav written and read back.
The same card padded to 384 KB, to 2.5 MB (the SF2 mapper's size) and with
a 512-byte header, built with AddressSanitizer and UBSan: clean. Speed on
the Mac: ~2,400 frames/s drawing, ~7,400 skipping (the board not measured
yet).

## Known limits

- HuCards only: no CD-ROM², no Arcade Card, no SuperGrafx.
- pce-go's renderer: whole batches of lines between register writes (mid-
  line raster effects are approximate), no sprite limit per line, sprite
  priority between overlapping background-priority sprites not exact.
- The PSG's DDA timing is approximate (pce-go's own), the palette keeps 8
  of the VCE's 9 bits of colour (one bit of blue).
- Not run with commercial games here (none was used); compatibility is
  pce-go's as Retro-Go ships it.
- Second-core split: a renderer on the other core would fit pce-go's shape
  (it renders batches of lines from a latched context), but the latch
  points come from the CPU's register writes; not done.
