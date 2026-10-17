# Game Boy and Game Boy Color core (gnuboy)

The Emulador's Game Boy and Game Boy Color: gnuboy as
[Retro-Go](https://github.com/ducalex/retro-go) carries it (commit `4ced120`,
`retro-core/components/gnuboy`), behind two `emu_core_t` tables of
`main/emu_core.h`, `emu_core_gb` and `emu_core_gbc`, from one library.

gnuboy is by Laguna and Gilgamesh and the people in `gnuboy/CREDITS` (Alex Duchesne for the Retro-Go rework and its GBC
colourisation palettes), under the GNU General Public License version 2
(`gnuboy/COPYING`). The colourisation tables in `gnuboy.c` are adapted from
VisualBoyAdvance-M (GPL-2.0).

RetroESP32-P4 (`components/gnuboy`) was looked at for anything specific to
the ESP32-P4: it carries the older go-play gnuboy (a different code base) and
adds nothing but `#pragma GCC optimize ("O3")`, so nothing was taken from it.

## Files

- `gb_core.c`: the adapter, both tables. ROM loading, video, sound, the pad,
  the palette option, the battery save with the MBC3 clock, the save states.
- `sources.cmake`, `board.cmake`, `sim.cmake`: the build, the same sources
  for the board and the simulator; both set `EMU_CORE_IDS gb gbc`.
- `gnuboy/`: the vendored core: `cpu.c`, `gnuboy.c`, `hw.c`, `lcd.c`,
  `sound.c` and their headers, `tables.h`, `COPYING`, `CREDITS`. Left out of
  Retro-Go's copy: `docs/` (two PDFs), `tests/blargg.zip` and its
  `CMakeLists.txt`.

## The two tables

| | `emu_core_gb` | `emu_core_gbc` |
|---|---|---|
| id / name / short_name | `gb` / Game Boy / GB | `gbc` / Game Boy Color / GBC |
| exts | `.gb` | `.gbc .cgb .gb` |
| picture | 160x144, `pixel_aspect` 0 | same |
| face, buttons | `EMU_FACE_DIAG2`; D-pad, A, B, SELECT, START; no labels | same |
| players | 1 | 1 |
| palette default | Verde (DMG) | Color automático |

Both drive the same emulator. As on a real Game Boy Color, the hardware a
game runs on comes from its header: a colour game (byte 0x143 bit 7) runs as
a GBC on either table, a Game Boy game as a Game Boy with the option's
palette. A Game Boy game in the `gbc` folder starts on the colours the GBC's
boot ROM would give it (gnuboy's table, from VBA-M).

## Options

| key | label | values | default |
|---|---|---|---|
| `palette` | Paleta (gb) / Paleta (juegos de Game Boy) (gbc) | Verde (DMG), Gris, Pocket, Color automático | Verde (DMG) on gb, Color automático on gbc |

Verde (DMG) is Retro-Go's DMG palette, Gris its MGB0 (white to black),
Pocket a new one (`#C4CFA1 #8B956D #4D533C #1F1F1F`), Color automático the
GBC's colourisation. The palette has no effect on a colour game.

## Sound and timing

Stereo, 32768 Hz. gnuboy makes a sample every `2^21 / rate` double-speed
cycles, rounded: at 32768 that is 64 exactly, so the rate reported is the
rate it plays at (Retro-Go's 32000 really plays at 31775). A frame is 35112
of those cycles: `fps()` is 2^21 / 35112 = 59.7275, and there are 548.625
samples a frame on average (548 or 549 in each `run()`), so sound and
picture never drift.

## Battery saves (.sav)

Only for cartridges with a battery. The file is what VBA-M, mGBA, SameBoy and
others write: the cartridge RAM as the header declares it (2, 8, 32, 64 or
128 KB; 512 bytes for an MBC2; 8 KB when a battery cartridge declares none),
then for an MBC3 with a clock 48 bytes: seconds, minutes, hours, days (low 8
bits), control (halt, carry, day bit 8), the same five latched, and the Unix
time the file was written (64 bits). A shorter file is read as far as it
goes; a 44-byte clock (32-bit time) is read too.

- **The clock catches up.** On load the clock moves on by the real time
  since the file was written, as a cartridge's crystal would have, when both
  times are known: the board has no battery clock and `time()` is in 1970
  until SNTP answers, so a time before 2020 means "not known" (no catching
  up, and the file then says 0). Retro-Go's files carry a made-up time in
  2030: in the future, so they are not caught up.
- `sram_flush()` writes when the game changed its RAM (gnuboy's dirty bits)
  or set its clock, and once per session for a game with a clock (so the file
  carries the time it was written). To `<save>.tmp`, then remove + rename
  (FAT does not rename over a file).
- A reset is a power cycle: gnuboy's hard reset, keeping the cartridge RAM
  and the clock (gnuboy's clears both).

## Save states

gnuboy's format (Retro-Go's, version 0x107): a 4 KB header of tagged values,
then RAM, VRAM and cartridge RAM in 4 KB blocks. Before loading, the adapter
checks the file's size (which depends on GB/GBC and the cartridge RAM) and
its first tag (`GbSs`, version 0x107): a short, damaged or foreign file is
refused and the game goes on untouched. Retro-Go's states load here and ours
load in Retro-Go (it ignores the three new tags). Saving goes to
`<path>.tmp` and is renamed.

The cartridge's clock is real time: loading a state does not take it back
(day, hours, minutes, seconds, halt and carry stay; the latched registers
come from the state). A loaded state's cartridge RAM is marked to be written
to the `.sav`.

## Symbols

Every core ends up in `emulador.so` (and every app and core in one simulator
executable), and gnuboy's names are generic (`GB`, `cart`, `gnuboy_run`...).
As `cores/gba` does, both builds link the core's objects with `-r` into a
single object in which only `emu_core_gb` and `emu_core_gbc` are global:
`gcc -r` + `objcopy --keep-global-symbol` as a POST_BUILD step on the board,
`clang -r -Wl,-exported_symbol` in the simulator (objects built with
`-fno-common`).

## Changes to gnuboy

Every change is marked `P4OS` in the source.

- `gnuboy.h`: with `P4OS` defined the messages go to `gnuboy_p4os_log()`
  (the adapter, which passes errors and warnings to the host's log, at most
  40 a load) and `IRAM_ATTR` is empty. A new palette, `GB_PALETTE_POCKET`,
  before `GB_PALETTE_COUNT`.
- `tables.h`: the Pocket palette replaces the empty "Bleh" entry (index 37).
- `gnuboy.c`, `gnuboy_run()`:
  - a frame with the screen off ticks the clock, makes its sound and hands
    it on (upstream returned without either: the sound came in one burst when
    the screen came back);
  - a game that turns the screen off half way through a frame no longer
    makes the frame last until it turns it on again and reaches vblank (one
    `run()` of many frames, seconds for a game that loads with the screen
    off): the frame ends after a frame's worth of cycles.
- `gnuboy.c`, `gnuboy_load_bank()`: a counter instead of `rand()` (not in the
  firmware's table) and no `abort()` on a read error. The adapter keeps the
  whole ROM in memory, so this is never reached.
- `gnuboy.c`, states: the envelopes' current volumes of channels 1, 2 and 4
  are saved (`S1ev`, `S2ev`, `S4ev`) and restored after `gb_sound_dirty()`,
  which reset them to their starting values (a held note came back at the
  wrong volume); with them a loaded state replays frame for frame, picture
  and sound. The header searches stop at the 4 KB block's end.
- `sound.c`, `gb_sound_dirty()`: the wave channel's length is `<< 13`, as
  `gb_sound_write()` has it (it was `<< 20`, 128 times too long after a state
  load).
- `hw.c`, the MBC3 clock: the day counter has 9 bits and wraps at 512 with
  the carry flag (upstream: 365); the control register's bit 0 is the day's
  bit 8 when written (upstream put it at bit 9) and when latched (upstream
  latched the stored flags); writing the seconds restarts the second; and the
  clock counts 35112 cycles a frame against 2^21 a second instead of 60
  frames a second (0.45 % slow at 59.73 frames a second).
- `hw.h`, `readw()`/`writew()`: two byte accesses instead of a 16-bit one
  at what may be an odd address.

## Tests (on the Mac, with a harness outside the repo)

ROMs, all freely redistributable: blargg's test ROMs (from Retro-Go's
`tests/blargg.zip`), `dmg-acid2` and `cgb-acid2` (Matt Currie, MIT),
`rtc3test` v004 (aaaaaa123456789, Unlicense), µCity 1.3 (AntonioND, GPL-3,
an MBC5 GBC game with a 128 KB battery save).

- blargg `cpu_instrs`: passes all 11. `instr_timing` (#255), `mem_timing`
  and `halt_bug` fail: gnuboy advances its timers 8 cycles at a time
  (`COUNTERS_TICK_PERIOD`), upstream's speed trade-off.
- `dmg-acid2` with the four palettes and `cgb-acid2`: colours right, the
  faces mostly there; the known gnuboy renderer gaps (the object/window cases
  of the acid tests) as upstream.
- µCity on `gbc`: title, menus and the save-slot screens, colours right; its
  128 KB `.sav` written and read back.
- `rtc3test` basic tests: RTC on, RTC off, register writes, seconds
  increment, rollovers, overflow and overflow stickiness pass; "Tick"
  measures 1269 ms against the game's own timer (gnuboy's timer, not the
  clock: 600 frames are exactly 10 clock seconds).
- `.sav`: a pattern written to a 128 KB `.sav` is read into the cartridge
  RAM; no change, no write; a byte written by the game through the MBC is
  written and comes back after unload + load and survives a reset; a short
  file is read as far as it goes. Clock: a 48-byte clock-only `.sav` for
  `rtc3test` in VBA-M's layout with the current time; a file written 3700 s
  ago comes back 3700 s later; day 511 + 1 day is day 0 with the carry; a
  44-byte clock is read; a Retro-Go (2030) time is not caught up; a state
  load leaves the clock alone.
- States: on every ROM above, 240 frames after loading a state are identical,
  picture and sound, to the 240 frames after saving it. A short state file
  and a missing one are refused and the game goes on.
- `unload()` then `load()`: the first 30 frames identical, picture and sound.
- Refused: a missing file, a 100-byte file, an MBC7 header ("Este tipo de
  cartucho no está soportado"), a header with a garbage ROM size; a file
  shorter than its header says loads, padded with 0xFF.
- AddressSanitizer + UBSan over the save tests: clean but for gnuboy's
  memory-map pointer arithmetic (base minus address, by design).
- Speed on the Mac (Apple silicon, -O2, after the load): µCity's menus 15700
  frames a second drawing every frame and 27700 without drawing;
  `dmg-acid2` 19400 and 55400. Drawing is about half of a frame's work.

## Board build

`tools/build_apps.sh emulador` (2026-10-16): emulador.so built, every
undefined symbol in the firmware's table.

`emu_core_gb_one.o`: 52.0 KB of code, 0.4 KB of data, 1.8 KB of bss (the
CPU's switch is 15.5 KB of it). Undefined symbols, all in the firmware's
table: `calloc fclose fopen fread free fseek ftell fwrite malloc memcpy memset
remove rename snprintf strcmp strlen time vsnprintf` and the soft-double
helpers `__adddf3 __divdf3 __fixdfsi __floatsidf` (gnuboy works out its sound
step in double once per reset).

## Memory

The whole ROM is read into one PSRAM block at load (up to 8 MB; most games
are 1-2 MB). gnuboy's own buffers (32 KB of work RAM, 16 KB of VRAM, the
cartridge RAM up to 128 KB), the 45 KB picture and the 8 KB sound buffer are
`malloc`/`calloc`, PSRAM like the rest of the app. Nothing asks for internal
RAM.

## Known limits

- gnuboy's accuracy: timer-exact and some PPU-exact effects (see the tests),
  no link cable, no Super Game Boy borders or palettes, no boot ROM.
- Cartridges refused: MMM01, MBC6, MBC7 (tilt sensor), Pocket Camera, TAMA5.
  HuC1 and HuC3 run on gnuboy's guesses (HuC3 as an MBC3 without the clock),
  rumble is ignored.
- MBC2's RAM is 8 KB in gnuboy (not 512 half-bytes mirrored): the `.sav` is
  its first 512 bytes, as other emulators write it.
- With the screen off the picture stays the last one drawn (a real Game Boy
  goes blank).
- Not split across cores. The natural split would be the line renderer
  (`lcd_renderline`, about half of a frame's work on the Mac) on the other
  core, but
  it reads VRAM, OAM, the palettes and the scroll registers at the moment a
  line starts, which games change mid-frame: it would need a per-line copy of
  that state.
