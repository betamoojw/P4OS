# Game Boy Advance core (gpSP)

The Emulador's Game Boy Advance: gpSP as
[midworld/gpsp-esp32](https://github.com/midworld/gpsp-esp32) carries it
(commit `296be4af210a28716115f625badd25fbd4692ad6`, 2026-10-12,
`components/gpsp/core`), with its RV32 recompiler for the ESP32-P4, behind
the `emu_core_t` table of `main/emu_core.h`.

gpSP is (c) 2006 Exophase and its later authors (notaz, davidgfnet and
others), under the GNU General Public License version 2 or later
(`gpsp/COPYING`). The RV32 recompiler and the ESP32 parts are (c) 2026
Midworld Kim, GPL-2.0-or-later for the files under `riscv/` that say so and
GPL-3.0-or-later for the rest of gpsp-esp32's new files
(`gpsp/COPYING.GPL-3.0`, the repository's licence). The built-in BIOS
(`gpsp/bios/`) is an open replacement with its source in `gpsp/bios/source`.
libretro-common is MIT.

## Files

- `gba_core.c`: the adapter, `emu_core_gba`. It drives gpSP through its own
  libretro entry points (`retro_init`, `retro_load_game`, `retro_run`,
  `retro_serialize`...) and answers gpSP's environment calls itself.
- `board.cmake`: the board's library, with the recompiler, and the switches
  for the ISA extensions (see below).
- `sim.cmake`: the simulator's library, gpSP's portable interpreter.
- `gpsp/`: the vendored core. Left out of upstream's `core/`: the other
  architectures' recompilers (`x86/`, `arm/`, `mips/`), `3ds/`, `jni/`,
  `tests/`, `tools/`, the Makefiles and `link.T`, the empty `control` file and
  `gpsp_benchmark_timing.c` (only in benchmark builds).

## How it runs

- **Board, with executable memory** (`emu_host_t.exec_*` set): the RV32
  recompiler. Its code caches grow in 256 KiB chunks up to 2 MiB for code
  from the cartridge and 2 MiB for code from RAM, allocated with
  `host->exec_alloc` and made visible with `host->exec_publish` (the adapter
  forwards both straight to the host). If an allocation fails the cache stays
  at the size it reached; if the first one fails, the game is interpreted.
- **Board, without executable memory** (`exec_alloc == NULL`, as the app is
  until the firmware lends a cache-sync function): `gpsp_rv32_set_platform(NULL)`,
  so `gpsp_rv32_block_init()` fails at load and gpSP's own run loop
  (`execute_arm_translate`) calls the interpreter (`execute_arm`) every
  frame. No code is generated and none of the assembly stubs run. Verified
  on the Mac with the recompiler's build and every assembly symbol replaced
  by an abort(): same frames as the interpreter build.
- **Simulator**: built without the recompiler at all, the libretro core's
  interpreter path (`HAVE_DYNAREC` and `GPSP_RV32_JIT` undefined).

`stats()` says which one is running and how full the code caches are, and
how many 32 KiB pages of the game were read from the card on the board.

## Symbols

Every core ends up in `emulador.so` (and every app and core in one
simulator executable), and gpSP's names are generic: `reg`, `dma`, `timer`,
`retro_run`, `cpu_ticks`... Both builds link the core's objects with `-r`
into a single object in which only `emu_core_gba` is global:

- board: `gcc -r`, then `objcopy --keep-global-symbol=emu_core_gba`, as a
  POST_BUILD step that replaces the archive's contents (`board.cmake`);
- simulator: `clang -r -Wl,-exported_symbol,_emu_core_gba` on the Mac (ld64
  makes every other global local), `ld -r` + `objcopy` elsewhere
  (`sim.cmake`). The simulator's objects are built with `-fno-common`, since
  a common symbol cannot be made local.

Being local also makes gpSP's data non-preemptible in the shared object,
which the assembly's PC-relative references need (`lla`, `auipc`).

## The ISA extensions

The firmware is plain `rv32imafc` (+ PIE). gpSP uses Zba, Zbb, Zbs, Zcb,
Xesploop and XespV (the PIE vector unit, for affine backgrounds) both in its
compiled code and in the code it generates. Upstream measured them on a
P4 v3.1; nobody has run them on this rev 1.3 chip yet. `board.cmake` has one
switch for each at the top (`EMU_GBA_ZBA`... `EMU_GBA_AFFINE_PIE`), all on as
upstream builds for the P4. Turning one off removes it from the compiler's
`-march` and from the recompiler (`GPSP_RV32_USE_*`). gpsp-esp32's
`tests/isa-probe` is a stand-alone ESP-IDF firmware (IDF 6.1, set to need a
v3.1 chip by default) that runs each instruction from executable RAM and
reports the ones that trap.

## ROMs, saves, states

- Extensions `.gba .agb .bin`, up to 32 MiB. A game of up to 8 MiB
  (`GBA_ROM_BUFFER_MIB` in `gba_core.c`) is read whole; a bigger one keeps
  8 MiB of 32 KiB pages and reads the others from the card with stdio as the
  game asks for them (gpSP's clock replacement). The buffer only takes what
  the game needs (a 4 MiB game, 4 MiB).
- Battery saves: the raw `.sav` other emulators use (SRAM 32 KiB, Flash 64
  or 128 KiB, EEPROM 512 B or 8 KiB; a file with up to 64 bytes of trailer,
  as VBA writes with an RTC, is accepted). Read at load, written by
  `sram_flush()` only when its contents changed (a hash of the save),
  through `<save>.tmp` and a rename.
- Real-time clock: gpSP's, for the games it detects. It starts at the
  card's time when the game loads and follows the frames, so a state carries
  its time.
- States: gpSP's own format, 416 KiB, through `<path>.tmp` and a rename. A
  state from another build or a damaged one is refused before anything is
  touched.

## Options

| key | label | values | default |
|---|---|---|---|
| `bios` | BIOS | Automática, Integrada | Automática: `<bios_dir>/gba_bios.bin` when it is there and looks like the official BIOS, the built-in one otherwise (gpSP also forces the built-in one for non-standard headers and M1 hacks). A change applies at the next reset or game, at once if the game has not run yet. |
| `color` | Corrección de color | No, Sí | No (gpSP's GBA LCD colour table) |
| `mezcla` | Mezcla de cuadros | No, Sí | No (blends each frame with the previous one, for games that flicker sprites for transparency) |

## Sound and timing

32768 Hz stereo (gpSP's lower rate, `gpsp_sound_rate`), under the speaker's
48 kHz; 59.73 frames a second. With `draw` false the scanline renderer is
skipped altogether.

## Changes to the vendored files

All marked `P4OS` in the code.

1. `libretro/libretro.c`: `gpsp_frontend_skip`, set by the adapter for a
   frame the app wants no picture of; `retro_run()` then skips rendering as
   gpSP's own frame skip does.
2. `riscv/rv32_block_runtime.cc`: `gpsp_rv32_block_is_ready()`, for
   `stats()`; and the RAM unit tables (change 5) allocated and freed with the
   block cache.
3. `riscv/rv32_arm_io.S`, `riscv/rv32_thumb_ram_helper.S`,
   `riscv/rv32_thumb_ram_store.S`: data reached with `auipc` /
   `%pcrel_lo` instead of `lui %hi` / `%lo` (absolute addressing would need
   R_RISCV_HI20 relocations in a shared object, which the loader does not
   take), under `.option norelax`. Same number of instructions.
4. `riscv/rv32_thumb_ram_helper.S`: the ROM load target table in
   `.data.rel.ro` instead of `.rodata`, so its relocations stay out of text.
5. `riscv/rv32_ram_units.c` / `.h`: the RAM unit tables (units, nodes, refs,
   patches, links, copies: 1.9 MiB) on the heap instead of `.bss`, through
   `gpsp_rv32_ram_units_alloc()` / `_free()`. An app's `.bss` is taken while
   the app is open, also to play the NES or to interpret.
6. `libretro/libretro-common/vfs/vfs_implementation.c`: seek returns the new
   position, as libretro's VFS contract says (it returned fseek's 0, so
   gpSP's ROM signature scan failed for every game bigger than the buffer);
   and without `fseeko`/`ftello` when `P4OS_NO_FSEEKO` is defined (the board:
   the firmware lends `fseek`/`ftell` only).
7. `gba_memory.c`: `localtime_r` instead of `localtime` for the cartridge
   clock (the firmware lends the former).

`strdup`, which libretro-common uses and the firmware does not lend, is
defined in `gba_core.c` for the board (local to the core like the rest).

## Tested

On the Mac (interpreter), with a harness outside the repo: jsmolka's
gba-tests (MIT) and the Tonc demos (Cearn), with the built-in BIOS. ARM and
Thumb tests stop at 225 and 211 and the save tests at 6 (a halfword store
to SRAM), the same with `-O0`: gpSP's accuracy, not the port's. Colours,
buttons (Tonc's `key_demo`), sound, SRAM/Flash 64/Flash 128 saves, state
round trip (120 frames and the sound identical after loading), unload and
load again (identical frames), the BIOS option with a stand-in file, and a
3 MiB test ROM with a 1 MiB buffer reading 96 pages back and forth.
