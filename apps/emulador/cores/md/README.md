# Mega Drive core (Gwenesis)

The Emulador's Mega Drive / Genesis: Gwenesis, by bzhxx, as
[RetroESP32-P4](https://github.com/giltal/RetroESP32-P4) carries it
(commit `339f17f`, `components/gwenesis`, itself a snapshot of
github.com/bzhxx/gwenesis `168e466` by way of Retro-Go), behind the
`emu_core_t` table of `main/emu_core.h`: `emu_core_md`, built into the module
`md.so`.

## Licences

- Gwenesis: GNU Affero General Public License v3 (`gwenesis/LICENSE`; the
  files' headers say GPLv3). It can be combined with the app's
  GPL-3.0-or-later code (GPLv3 section 13 / AGPLv3 section 13); being a
  module of its own (`md.so`), it also stays in a file apart.
- `gwenesis/src/cpus/M68K`: Musashi by Karl Stenerud, in Genesis Plus GX's
  version. Its `readme.txt` still has the old "free for any non-commercial
  purpose" notice; upstream Musashi has been MIT since 2008.
- `gwenesis/src/cpus/Z80`: Marat Fayzullin's Z80 emulator, whose header says
  "You are not allowed to distribute this software commercially". That is a
  restriction the GPL does not allow; Retro-Go and RetroESP32-P4 ship it the
  same way. If the app's distribution needs a clean licence, this is the
  file to replace (the interface is small: `ExecZ80`, `RdZ80`/`WrZ80`,
  `IntZ80`; an MIT Z80 core such as superzazu/z80 would fit).

## Files

- `md_core.c`: the adapter. The frame loop (262 or 313 lines, the line and
  frame interrupts, as Retro-Go's driver), the memory every part of
  Gwenesis works in, pads, battery RAM, save states, the sound mix and its
  resampling to 48 kHz, the options.
- `md_glue.h`: forced into every file; Gwenesis's `printf` goes to the
  host's log.
- `sources.cmake`, `board.cmake`, `sim.cmake`: the build, the same sources
  for the board and the simulator. Both link the core into one object that
  keeps only `emu_core_md` global (the GBA core's way), since Gwenesis's
  names are generic (`VRAM`, `m68k`, `zclk`, `load_cartridge`...).
- `tools/gen_m68k_dispatch.py`: makes `m68k_dispatch.h` and
  `m68k_handlers.h` (below).
- `tools/gen_ym2612_tables.py`: makes `ym2612_tables.h` (below).
- `gwenesis/`: the vendored core. Left out: the Z80 debugger (`Debug.c`,
  `ConDebug.c`), the gen68k adapter RetroESP32-P4 did not build, and
  Musashi's static tables (`m68ki_instruction_jump_table*.h`,
  `m68ki_cycles*.h`), replaced by the generated dispatch table.

## The table

| field | value |
|---|---|
| id / name / short_name | `md` / `Mega Drive` / `MD` |
| exts | `.md .gen .bin .smd` (SMD dumps are de-interleaved at load) |
| max_w x max_h | 320 x 240. Each frame is 320 (H40) or 256 (H32) wide and 224 or 240 (V30, PAL games) high, reported per frame. Nothing is cropped: the console draws no garbage at the edges. |
| pixel_aspect | 32/35 (H40 on a 4:3 TV). H32 frames have 8/7 pixels: see "For the frontend". |
| players | 2 |
| buttons | D-pad, A B C, START, X Y Z, SELECT (MODE) |
| face | `EMU_FACE_ROW3` (see "For the frontend" for the 6-button pad) |
| labels | SELECT is `MODE`; the others are their letters |
| fps | 59.92 NTSC, 49.70 PAL (from the resampler's step: exact) |
| sample_rate | 48000 (the speaker's bus rate; stereo, both channels equal) |

## Options

| key | label | values | default |
|---|---|---|---|
| `pad` | Mando | 3 botones, 6 botones | 3 botones |
| `region` | Región | Automática, Japón, América, Europa | Automática |
| `sound` | Sonido | Completo, Rápido, Sin FM | Completo |

- **Mando**: the six-button pad as Genesis Plus GX reads it (the TH
  counter, reset at the end of each frame). Some old games misread a
  six-button pad, which is why three is the default. With six, X Y Z are
  the top row and SELECT is MODE. Takes effect at once.
- **Región**: Automática reads the header (USA first, then Europe, then
  Japan, as Gwenesis did). Europe is a PAL console: 313 lines, 50 Hz.
  The region sets the version register ($A10001, as Genesis Plus GX: bit 7
  overseas, bit 6 PAL, bit 5 no Mega-CD) and the VDP's PAL bit. A change
  applies right after load() (where the app sets the options) and
  otherwise from the next load, because the app has read fps() by then.
  A state saved under the other region plays on in the current one.
- **Sonido**: Completo makes the YM2612's every sample (53267 Hz NTSC);
  Rápido makes every other one, with phases, envelopes, LFO and timers
  still moving at the chip's pace (about 40 % less YM2612 work on the Mac,
  the music unchanged in pitch and timing, a little rougher); Sin FM keeps
  only the PSG (the YM2612's registers and timers still work, so sound
  drivers do not hang). Takes effect at once; the output rate does not
  change.

There is no frame-skip option: Gwenesis has nothing better than not
drawing the lines, which is what `run(pads, false)` does.

## Sound and timing

The YM2612 and the PSG are sampled every 1008 master clocks (7 x 144, the
chip's own rate) and brought up to the CPU's time on every write to them
(`GWENESIS_AUDIO_ACCURATE=1`), so the DAC's sampled drums and voices come out
when they were written. The remainder of a frame's clocks carries to the
next (888 or 889 samples a frame NTSC). The mono mix (YM2612 + PSG) goes to
48 kHz by linear interpolation with a constant 16.16 step; `fps()` is worked
out from that step, so picture and sound never drift: 801.03 samples a frame
NTSC, 965.8 PAL, measured.

## Battery saves

Gwenesis had none. The 64 KB of battery RAM sit at their address & 0xFFFF
(even and odd bytes), as Genesis Plus GX keeps them, so the `.sav` is that
emulator's (RetroArch's `.srm` for it): the file holds the range the header
declares ("RA" at $1B0; 16 KB for the usual $200001-$203FFF), and any size
up to 64 KB is read. Cartridges with no "RA" but of 2 MB or less get
$200000-$20FFFF, as in Genesis Plus GX. $A130F1 maps the RAM over the ROM on
bigger cartridges. `sram_flush()` writes (to `.tmp`, then renamed) only when
the RAM differs from what was last read or written; a game that never
touches it leaves no file. Reset keeps it. EEPROM saves (NBA Jam, Wonder Boy
in Monster World, Mega Man: The Wily Wars...) are not emulated.

## Save states

Gwenesis's named records, written to memory, then to `path.tmp`, renamed:
first a version record of ours, Gwenesis's records, the frame's carried
clocks and the resampler's place, then a CRC-32. To load, the file is read
whole and compared with what this build would write (every record's name
and size, in order) and its CRC before any of it reaches the console; a
short, damaged or foreign file (or one from the simulator: pointers and
struct sizes differ) is refused and the game goes on untouched. A loaded
state replays frame for frame, picture and sound.

## Changes to Gwenesis

Every change is marked `P4OS` in the source.

- **The 68000's dispatch** (`m68kcpu.c`, `m68kcpu.h`, `m68kops.h`): one
  table of 65536 16-bit words (handler number | cycles / 2) and an array of
  the 1701 handlers, generated from Musashi's own handler table and checked
  equal to the static tables Gwenesis shipped. 128 KB of read-only data with
  no relocations instead of 320 KB and 65536 relocations in the module, one
  load per instruction instead of two. The static jump table also stopped
  at $EFBF: line-F opcodes ($Fxxx) called a NULL pointer; they now take the
  line-F exception (`m68k_op_1111` is always built).
- **"dbf dn,\*" delay loops** (`m68k_op_dbf_16`): the turns that would start
  before the end of the time slice are taken at once, with their count and
  cycles: exact (the same picture and sound, checked by hash over 600 frames
  on five programs), and without running them one by one. Off with
  `-DP4OS_NO_DBF_SKIP`.
- **ROM reads** (`m68k.h`, `m68kcpu.h`): one compare against `gw_rom_fast`
  (the ROM's size, or where the battery RAM starts when it is mapped over
  it); past it, the bus.
- **Battery RAM and $A130F1** (`gwenesis_bus.c/.h`), as above, and in the
  states.
- **Region** (`gwenesis_bus.c`): `set_region()` takes the version register
  from the adapter; the header reading is `gw_region_detect()`, without its
  `printf`s. The PSG's clock and rate come from the adapter (PAL or NTSC).
- **Sample rate** (`gwenesis_bus.h`): `AUDIO_FREQ_DIVISOR` 1008 (was 1009);
  `ym2612_run()` and `gwenesis_SN76489_run()` move their clock by the
  samples made, so it can start a frame below zero.
- **The Z80's reset** (`z80inst.c`): it starts again only when its reset
  line is released after being held. Gwenesis reset it on every write of
  1, and SGDK's sound drivers, which write 1 again and again, never got past
  their start-up: no sound at all from any SGDK program.
- **The Z80's opcode fetch** (`Z80.c`): `Z80_RAM` was declared as an array of
  page pointers while it is one pointer: an opcode fetched from $2000 up
  read a pointer from past it (a crash, seen with SGDK's XGM driver).
- **The six-button pad** (`gwenesis_io.c/.h`), and `gwenesis_io_reset()`
  for the next game.
- **The YM2612** (`ym2612.c/.h`): the power and sine tables come from
  `ym2612_tables.h` (the firmware lends no double-precision libm; the float
  functions round two entries differently); the tables are built on every
  start (they were built once per program run, and the next game, in freshly
  allocated memory, had no FM); the struct's pointers go into states as
  numbers and come back as pointers (they were saved raw); `OPNREGS` is
  saved whole (it was `sizeof(pointer)` bytes); the half-rate mode
  (`ym2612_set_half_rate`). `gwenesis_SN76489_SetRate()` changes the PSG's
  rate without resetting it.
- **The VDP** (`gwenesis_vdp_gfx.c`, `gwenesis_vdp.h`): lines are drawn in
  RGB565 straight into the frame (they were palette indices turned into
  colours once the frame was over, with the palette as it was then: a
  palette changed between lines was lost); shadow and highlight in colour,
  with the priority of transparent tiles (it drew the plain colours: SGDK's
  spotlight sample now shows its spotlights); a display blanked by register
  1 shows the backdrop colour (it drew the planes); register 0 bit 5 hides
  the left column; interlace mode 1 is drawn as progressive (it drew
  nothing); the backdrop index is masked to 6 bits.
- `esp_attr.h` dropped where included; `stdint.h`/`stdlib.h` included where
  the ESP-IDF headers brought them in.

RetroESP32-P4's own changes to Gwenesis (kept): the RAMs and the chips'
state behind pointers, ROM reads bounded by the ROM's size, the TMSS index
fix, the YM2612 output clamp, address errors off, `VRAM` renamed.

## Performance (measured on the Mac only)

The board was not touched. On the Mac (Apple Silicon, -O2), SGDK's platformer
with its music: 0.25 ms a frame drawn, 0.18 ms not drawn; by function
(`sample`): VDP lines 29 %, YM2612 27 %, Z80 14 %, 68000 about 25 % (half of
it the game waiting for the vertical blank on the VDP's status). Sound
Rápido: YM2612 about 40 % cheaper; Sin FM: no YM2612 at all.

RetroESP32-P4 holds 60 fps on the P4 by putting the 68000's and Z80's RAM,
the VRAM, the CPU state, the palettes, the line buffers and the YM2612's
state and tables in internal RAM (about 180 KB), running YM2612 + PSG
synthesis on core 1 for a whole frame at once (3.7 ms a frame there: it
loses the DAC's timing, which is why its sampled drums are off), converting
and pushing the picture on core 1, and skipping up to 3 frames' drawing
when a frame's own work runs over. Here everything runs on core 0 from
PSRAM; expect the frontend's frame skip to be needed on busy games.

### Internal RAM (a proposal, not done)

`md_core.c` carves everything hot out of one block, `s_hot_pieces`, ordered
by worth. If the app can offer internal memory (an `emu_host_t` callback,
say `void *(*fast_alloc)(size_t)`), the front of that list is what to give:

| tier | pieces | size | why |
|---|---|---|---|
| A | 68000 state, VDP registers, CRAM565, CRAM, VSRAM, FIFO, sprite cache, 2 line buffers, YM2612 state, OPN registers, sine table | 16 KB | touched on every instruction, pixel or sample |
| B | + Z80 RAM (8 KB), YM2612 power table (26 KB) | 50 KB | every Z80 opcode; 24 random lookups a sample |
| C | + 68000 RAM (64 KB), VRAM (64 KB) | 178 KB | what RetroESP32-P4 has internal |

Tier B is the one worth asking for. The 68000 state is 5.6 KB because of
Musashi's unused memory map; its hot part is the first 200 bytes.

### The second core

The natural split is the sound: log the YM2612/PSG writes with their
master-clock time during the frame (the timers and the status the CPUs read
are cheap and stay on core 0), and let core 1 synthesise the frame from the
log while core 0 runs the next one; same output as now, about a quarter of
the frame off core 0. The VDP's lines depend on registers, VRAM, CRAM and
VSRAM changed mid-frame, so moving them needs those writes logged too.

## Test notes

On the Mac, with a harness in the scratchpad (fake host, scripted pads,
PPM/WAV output), and SGDK's MIT-licensed sample ROMs (from its repository's
`sample/*/out/release/rom.bin`), plus a 1 KB test ROM of our own for the
battery RAM. Checked: pictures (colours, H32 and H40, line scroll, H-int
effects, shadow/highlight spotlights, PAL), the 3- and 6-button pads in
SGDK's joy-test (detected as 6-button on both ports, X Y Z MODE read), sound
(SGDK's PCM driver and the platformer's music; full and fast mode give the
same waveform), the battery save (counter incremented across three loads,
16 KB `.sav`, none for games that never write it), state round-trip (the 150
frames after a load hash equal to continuing, picture and sound), short /
flipped / foreign / empty / missing states refused with the game untouched,
load -> unload -> load and reset giving the same hashes as the first load,
frames per second 59.9225 / 49.7015.

## Known limits

- No Mega-CD, no 32X (refused with a message: "Los juegos de 32X no se
  pueden jugar"), no Power Base Converter (Master System games).
- No SSF2 mapper (Super Street Fighter II, 5 MB): ROMs past 4 MB without it
  read wrong; no EEPROM saves; no lock-on (Sonic & Knuckles + Sonic 3).
- Interlace mode 3 (two-player Sonic 2) is not drawn.
- Gwenesis's VDP has no per-line sprite limit timing and no mid-line
  effects; its 68000 has no address errors.
- The ROM and RAM reads rely on unaligned 32-bit loads, as RetroESP32-P4 does
  on the same chip.
- 60 fps on the board is not measured.

## Mega-CD and 32X (out of scope)

- **Mega-CD**: a second 68000 (12.5 MHz), 512 KB program RAM, 256 KB word
  RAM with its two modes, the graphics ASIC (rotation/scaling), the Ricoh
  RF5C164 PCM chip, CD-DA audio from the disc (BIN/CUE or CHD reading, ISO
  tracks), the BIOS files (`bios_CD_U.bin`...), and the main/sub CPU
  communication registers polled tightly. Two 68000s interleaved finely
  would roughly double the CPU cost; Genesis Plus GX's scd code is the
  reference. Realistic only with the 68000 much faster than Musashi here.
- **32X**: two SH-2 at 23 MHz plus a 32X VDP (framebuffer, 15-bit colour) and
  PWM sound, mixed over the Mega Drive picture. PicoDrive does it on ARM with
  a recompiler for the SH-2s; on this chip it would need an SH-2 recompiler
  for RISC-V. Not on the cards.
