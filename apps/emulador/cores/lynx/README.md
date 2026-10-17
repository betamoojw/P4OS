# Atari Lynx core (Handy)

The Emulador's Atari Lynx: Handy as [Retro-Go](https://github.com/ducalex/retro-go)
carries it (commit `4ced120`, `retro-core/components/handy`, with its
high-level BIOS), behind the `emu_core_t` table of `main/emu_core.h`. Built
as the module `lynx.so`.

Handy is (c) 1996-2004 K. Wilkins, under the zlib licence
(`handy/license.txt`; GPL-compatible). The BIOS routines are Retro-Go's (the
loader decryption, `lynxdec.cpp`, from Handy's libretro lineage).

## Files

- `lynx_core.cpp`: the adapter, `emu_core_lynx`. The time slices, the
  picture (double-buffered, rotated when asked), the sound, the buttons, the
  rotation option, the EEPROM save, the save states.
- `lynx_shim.c`, `compat/rg_system.h`: Retro-Go's log and CRC-32 for Handy
  (built with `RETRO_GO`); the log goes to the host's and stops after 40
  lines a game (Handy logs every illegal opcode).
- `lynx_rt.cpp` (board only): `operator new`/`delete` (into PSRAM) and
  `__cxa_pure_virtual`; Handy needs nothing else of a C++ runtime.
- `sources.cmake`, `board.cmake`, `sim.cmake`, `one_object.cmake`: the build,
  as the 2600's (one object, `emu_core_lynx` its only global; C++17).

## What it plays

`.lnx` (with the 64-byte header), `.lyx` and other raw images (64, 128, 256
or 512 KB), and `.o` homebrew (BS93, loaded straight into RAM). The bank
sizes and a homebrew's load address are checked before Handy reads them
("No es un juego de Atari Lynx" otherwise).

**No BIOS file is needed.** Retro-Go's Handy replaces the boot ROM with a
high-level version of its routines (the cartridge loader's decryption):
`lynxboot.img` is neither required nor read. This differs from the brief,
which asked for an error naming it; the source chosen does not need it.

Time: a Lynx game sets its own frame rate with Mikie's timers (75 Hz is
common). One `run()` is therefore a fixed slice of time, 266,500 cycles of
the 16 MHz clock = exactly 533 sound frames at 32 kHz, and `fps()` is
32000 / 533 = 60.04: sound and picture never drift. Each `run()` shows the
last frame the game finished (a 75 Hz game shows four of every five); the
stats line gives the game's own rate. Lines are always drawn (`draw` only
decides whether the host gets the picture), so a frame begun during a
skipped run is whole.

## The table

| field | value |
|---|---|
| id / name / short_name | `lynx` / Atari Lynx / `Lynx` |
| exts | `.lnx .lyx .o` |
| max_w × max_h | 160 × 160 (160 × 102 upright, 102 × 160 turned) |
| pixel_aspect | 0 (square) |
| players | 1 |
| buttons | d-pad, A, B, L (OPTION 1), R (OPTION 2), START (PAUSE) |
| face | `EMU_FACE_DIAG2` |
| labels | L = "OPTION 1", R = "OPTION 2", START = "PAUSE" |
| fps | 32000 / 533 = 60.04 (time slices, see above) |
| sample rate | 32000 Hz, stereo |

## Options

| key | label | values | default |
|---|---|---|---|
| `rotation` | Girar la pantalla | Automática, No, Izquierda, Derecha | Automática |

Automática: the `.lnx` header's rotation byte, or Retro-Go's list of
sideways games whose header does not say so (Centipede, Lexis, NFL
Football, Raiden left; Gauntlet, Gauntlet: The Third Encounter, Klax
right). The picture is reported already turned (102 × 160) and the d-pad
turns with it.

## Battery saves

The cartridge's EEPROM (93C46 to 93C86, when the `.lnx` header says it has
one), raw, as other emulators keep it: read at load, written by
`sram_flush()` only when it changed, kept across a reset.

## Save states

`P4LNXST\0`, a version, the payload's length and CRC-32, the cartridge's
CRC-32, then Handy's `ContextSave()` and a block of ours (the slice's
overrun, which buffer Mikie draws into and where, the last finished frame,
both buffers). Handy loads device by device into the running console, so
the console is copied before a load and put back if the state stops half
way; a file whose header, CRC or length do not match is refused untouched.

## Changes to Handy

Every one is marked `P4OS` in the source.

- `system.h`, `system.cpp`: the save states go through memory again
  (Handy's own `LSS_FILE`, which Retro-Go had swapped for `FILE*`), bounded
  both ways; `CSystem::RunCycles()` (the time slices, seeing through Mikie's
  cycle-counter wrap, ending a slice whose CPU sleeps with no timer to wake
  it instead of hanging as `UpdateFrame()` does) and `FlushSound()`; the
  high-level loader reads at most the five blocks its buffers hold (a file
  that is not a cartridge asked for 256 and wrote past the stack).
- `mikie.cpp`, `mikie.h`: `FlushSound()`, and the display pointer as an
  offset for the save states; the screen DMA's line, counter and address
  are saved (the first frame after a loaded state was drawn from wherever
  the console happened to be).
- `susie.cpp`, `susie.h`: the quad offsets of `PaintSprites()` are members,
  reset and saved (they were function statics).
- `cart.cpp`: the address strobe's previous value is `mStrobe` itself (it
  was a function static).
- `eeprom.h`: `Data()`, the EEPROM's contents for the battery save.
- `ram.h`: a BS93 header's address and size read big-endian (as cc65 writes
  them and Handy read them); Retro-Go read them little-endian.

## Test notes (Mac, scratchpad harness)

A BS93 test program written for this port (assembled in the scratchpad):
a 16-colour palette, sixteen bars drawn by the CPU into the screen buffer
with a marker in the top-left corner, a literal 4-bit sprite drawn by Susie
and moved by the joystick, a tone on A, B reversing the sprite's pens,
OPTION 1, OPTION 2 and PAUSE each recolouring a pen. Checked:

- pictures looked at: palette, the marker's corner, the sprite, the three
  rotations (upright, left, right);
- state round trip at several frames: 120 frames identical in picture and
  sound (after the Mikie and Susie fixes above); damaged, truncated and
  foreign files refused;
- unload → load and reset: the same frames;
- bad files refused (bank size, homebrew address); a `.lnx` with a random
  image and an EEPROM, and a random 128 KB raw image, run garbage without
  crashing or hanging (slowly: the CPU executes noise), and the EEPROM's
  `.sav` is not rewritten while unchanged;
- AddressSanitizer and UBSan clean;
- ~3,900 frames/s on the Mac.

Not tested with commercial games or encrypted commercial-format carts (none
may be downloaded); the high-level loader decryption is Retro-Go's,
unchanged.

## Known limits

- One player: ComLynx (the link cable) is not offered.
- The 75 Hz games are shown at 60: one frame in five is not shown.
- A file of garbage plays garbage slowly (Handy has no way to know).
