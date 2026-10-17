# Atari 2600 core (Stella)

The Emulador's Atari 2600: Stella 3.9.3 as
[RetroESP32-P4](https://github.com/giltal/RetroESP32-P4) carries it (commit
`339f17ff`, `components/stella`: Go-Play's port, already without
iostreams), behind the `emu_core_t` table of `main/emu_core.h`. Built as the
module `a2600.so`.

Stella is (c) 1995-2014 Bradford W. Mott, Stephen Anthony and the Stella
Team, under the GNU General Public License version 2 (`stella/license.txt`).
Its game database (`DefPropsP4.hxx`) is Stella's, repacked.

## Files

- `a2600_core.cpp`: the adapter, `emu_core_a2600`. Video to RGB565 (with
  Stella's phosphor blend when asked), sound, the two joysticks, the console
  switches, the options, the save states.
- `a2600_rt.cpp` (board only): what of the C++ runtime Stella needs and the
  firmware does not export: `operator new`/`delete` (into PSRAM: the
  firmware's malloc keeps blocks under 1 KB internal), `__cxa_pure_virtual`,
  libstdc++'s `__throw_*` hooks (they can only mean a bug: they trap) and an
  explicit instance of `std::string::_M_replace_cold`, which libstdc++ keeps
  extern even in C++20. No C++ library is linked: Stella is built as C++20,
  where libstdc++ instantiates `std::string` in the user's code.
- `a2600_libc.c` (board only): `toupper`, `rand` and `atexit` (a no-op),
  which the firmware does not export.
- `compat/esp_attr.h`: an empty `IRAM_ATTR` for the four Stella files that
  include ESP-IDF's.
- `tools/gen_defprops.py`: makes `stella/emucore/DefPropsP4.hxx` from
  Stella's `DefProps.hxx` (RetroESP32-P4's `components/stella/emucore/`):
  `python3 tools/gen_defprops.py <DefProps.hxx> stella/emucore/DefPropsP4.hxx`.
- `sources.cmake`, `board.cmake`, `sim.cmake`, `one_object.cmake`: the build.
  On the board the library's objects are linked with `-r` into one object
  that keeps `emu_core_a2600` its only global, every COMDAT group renamed
  with a suffix of this core's (`one_object.cmake` says why); in the
  simulator `ld64 -r -exported_symbol` does the same, and the simulator links
  libc++.

Not vendored from RetroESP32-P4's tree: its runner `stella_run.cpp`,
`stella.pro`, `M6502.m4`, `DefProps.hxx` (replaced, see above), and the
files nothing here builds or includes (AtariVox, SaveKey, MT24LC256, KidVid,
MindLink, CompuMate, MouseControl, StateManager, and the FA2, MC, CM and CTY
bank-switching schemes, which need exceptions or files of their own).

## What it plays

`.a26` and `.bin`, up to 512 KB. The bank-switching schemes: 2K, 4K, F8,
F6, F4 (and their SC versions), FA, FE, E0, E7, 3E, 3F, UA, CV, AR
(Supercharger, fast loading), DPC (Pitfall II) and, added here, 0840, 4A50,
4KSC, BF, BFSC, DF, DFSC, EF, EFSC, F0/MB, SB, X07 and DPC+ with its ARM
(Thumbulator). The type comes from the database or Stella's autodetection;
NTSC, PAL and SECAM from the database or 60 frames of autodetection. A
scheme it does not have fails the load with "Tipo de cartucho no soportado".
Paddles and driving controllers turn with left and right, fire is fire
(Stella maps the joystick events to them); keypads and other controllers
are not usable.

No battery saves: the 2600 had none (`sram_flush()` has nothing to write).

## The table

| field | value |
|---|---|
| id / name / short_name | `a2600` / Atari 2600 / `2600` |
| exts | `.a26 .bin` |
| max_w × max_h | 160 × 256 (210 lines for most NTSC games, 250+ PAL) |
| pixel_aspect | 1.75 (160 pixels a line shown 4:3 with 210 lines) |
| players | 2 |
| buttons | d-pad, A (fire), SELECT, START |
| face | `EMU_FACE_ONE` |
| labels | SELECT = "SELECT", START = "RESET" |
| fps | 31400 / 524 = 59.92 (NTSC), 31400 / 629 = 49.92 (PAL) |
| sample rate | 31400 Hz (the TIA's), stereo |

Sound: a whole number of samples a frame (524 NTSC, 629 PAL) from Stella's
SoundSDL, `fps()` worked out from it: no drift.

## Options

| key | label | values | default |
|---|---|---|---|
| `diff_left` | Dificultad izquierda | Según el juego, A (difícil), B (fácil) | Según el juego |
| `diff_right` | Dificultad derecha | same | Según el juego |
| `tv_type` | Televisor | Según el juego, Color, Blanco y negro | Según el juego |
| `phosphor` | Suavizar parpadeo | Según el juego, Sí, No | Según el juego (Stella's Display.Phosphor) |
| `palette` | Paleta | Stella, z26 | Stella |

"Según el juego" is the database entry, or B / B / colour / off for a game
it does not know.

## Save states

A file of our own: `P4A26ST\0`, a version, the length and CRC-32 of the
payload, the cartridge's MD5, then Stella's `Console::save()` into an
in-memory Serializer. A file whose header, CRC, MD5 or length do not match
is refused before anything is touched; if Stella still refuses it half way,
the console as it was just before is put back. Written to `path.tmp` and
renamed.

## Changes to Stella

Every one is marked `P4OS` in the source.

- `common/bspf.hxx`, `common/Variant.hxx`, `emucore/Cart.hxx`,
  `emucore/Cart.cpp`: the static `std::string` and `Variant` objects
  (`EmptyString`, `EmptyVariant`, `Cartridge::myAboutString`) became objects
  made on first use: they were constructors in `.init_array`, which P4OS's
  loader does not run.
- `emucore/PropsSet.cpp/.hxx`: no `std::map` (its red-black tree lives in
  libstdc++); only the built-in database is searched, in the compact form
  `tools/gen_defprops.py` makes (`DefPropsP4.hxx`: 60 KB instead of ~68,000
  pointers, each a run-time relocation in a shared object).
- `emucore/EventHandler.hxx`: `<map>` no longer included (unused; libc++'s
  pulls in `<ios>`, which bspf's stream macros break).
- `emucore/TIATables.cpp/.hxx`: `PxPosResetWhen`'s rows are one 200 KB block
  made once and freed by the new `releaseTables()` (it was 1,280 separate
  allocations made again by every TIA and never freed: 200 KB lost per game
  loaded); `ready()` says whether it was allocated. The precomputed copy
  that sat unused under `#else` (600 KB of source) is out.
- `emucore/TIA.cpp`: the `toggle*` debugging methods are no longer `inline`
  (Console.cpp calls them from outside the file).
- `emucore/TIA.hxx`: `getMilliSeconds()` converts through 32 bits (float to
  64 bits is `__fixunssfdi`, not exported).
- `emucore/TIASnd.cpp/.hxx`, `common/SoundSDL.cpp/.hxx`: the save state also
  keeps the sound generator's counters and the register writes still queued
  (Stella kept only the six registers): sound after a loaded state is the
  sound the game made.
- `emucore/Cart.cpp`: the schemes listed above are created (RetroESP32-P4
  had left them out).
- `emucore/CartEF.cpp`, `emucore/CartDPCPlus.cpp`,
  `emucore/Thumbulator.cpp`: no exceptions (`-fno-exceptions`): the
  `try`/`catch` and `throw`s are gone; the ARM emulation stops at its
  instruction limit or a HALT write instead of throwing.

## Test notes (Mac, scratchpad harness)

Freely redistributable ROMs only: two test programs written for this port
and assembled with a small assembler in the scratchpad (a 4 KB one and the
same as an 8 KB F8 cartridge): rainbow background, playfield, a sprite
moved by the joystick, a tone on fire, SELECT flipping the playfield, RESET
centring the sprite, the left difficulty switch colouring the playfield and
the TV switch turning the background grey. Checked:

- pictures looked at: colours, positions and every switch as programmed,
  with the options (difficulty A, B/W, z26, phosphor);
- the F8 build draws exactly the same frames as the 4 KB one;
- state round trip at several frames: the next 120 frames identical in
  picture and sound to continuing; a damaged, truncated or foreign file
  refused with the game untouched;
- unload → load: the same frames; reset;
- AddressSanitizer and UBSan clean (only Stella's harmless pointer
  arithmetic on the unused debugger table, `Cart4K.cpp:62`);
- a random 3 KB file "plays" (Stella pads it to 4K) without crashing;
  2600 files have no signature to refuse them by;
- ~4,000 frames/s on the Mac.

Not tested with commercial games (none may be downloaded).

## Known limits

- One pixel aspect per console in the contract: PAL games (250 lines) look
  a little narrow at 1.75 (theirs would be about 2.08).
- Keypad, Kid Vid, AtariVox/SaveKey, CompuMate and the FA2 (Star Castle
  flash), MC, CM and CTY schemes are not emulated. Stella 3.9.3 has no CDF.
- Soft-float `double` in a few places: DPC and DPC+ music clocks (per access
  of their registers), paddle reads (`TIA::dumpedInputPort`) and the frame
  rate at the end of each frame. Fine for Pitfall II and paddle games, but
  they cost more than the rest.
- The 2600 has no file signature: any file of a plausible size loads.
