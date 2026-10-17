# Emulador

Consoles of other times on the board, from the card: the **NES** and the
**Game Boy Advance** for now, more to come (each console is a core behind
one small table, `main/emu_core.h`). An app of the card (`emulador.so`); it
needs no firmware of its own.

## On the card

```
/sdcard/emulador/
    ajustes.txt             the app's settings (it rewrites this file)
    recientes.txt           when each game was last played, and for how long
    nes/                    the games of a console (subfolders too, two levels)
        bios/               system files the console may use
        saves/<game>.sav    battery saves: the raw .sav other emulators use
        states/<game>.st0   save states: 0 the automatic one, 1..4 by hand,
        states/<game>.st0.thm   each with the picture it was saved on
    gba/ ...
```

The app makes the folders (with a `LEEME.txt` in each) the first time it
opens. Games go in by copying them to the card, over USB disk mode, or from
the **Emulador page of the portal** (`web/emulador.js`): drop the files on
it. The page also shows each game with the picture it was left on, takes and
gives battery saves (to bring a game from another emulator) and the BIOS
files.

| Console | Files | BIOS (in `bios/`) |
|---|---|---|
| NES | `.nes` `.fds` `.nsf` | optional: `disksys.rom` for the Famicom Disk System |
| Game Boy Advance | `.gba` `.agb` `.bin` | optional: `gba_bios.bin`; without it gpSP's free replacement BIOS runs |

No game or BIOS comes with the app.

## Playing

- **The library:** the last game played on top, with the picture it was
  left on (*Continuar*); the consoles as chips; the games sorted by recent
  or A-Z. A tap plays from where the game was left (its automatic state);
  a long press offers to start from the beginning.
- **Upright** the picture fills the width (GBA x3 = 720x480, NES x2.8125)
  and the console's pad is under it; **lying down** the picture is in the
  middle and the pad's halves are at the thumbs. The board turns without
  stopping the game.
- **The pad on the screen** takes two fingers: the d-pad follows the
  thumb's angle (eight ways, it keeps it while the thumb slides), and A/B
  are one cluster, so a thumb between them presses both.
- **⏸** opens the menu, **⏭** is fast forward (no sound, a third of the
  pictures).
- **The menu:** Continue, Reset, Quit (the automatic state is saved), the
  save states (four slots plus the automatic one, each with its picture)
  and the options: the picture's size (fitted, whole multiples, stretched),
  frame skip, sound, frames a second on screen, resume on open, and the
  console's own (the NES palette, overscan and sprite limit; the GBA's
  BIOS).
- **USB gamepads:** pad 1 is player 1 (together with the screen), pad 2
  player 2 (the NES has two). The buttons as a SNES-style pad numbers them:
  X=1, A=2, B=3, Y=4, L=5, R=6, SELECT=9, START=10, with X/Y doubling as
  A/B on consoles without them. **SELECT+START** held opens the menu, and
  the d-pad + A go through it (B or START closes it).
- Battery saves are written when leaving and every 30 s while playing, if
  the game changed them.

## How it is built

| File | What |
|---|---|
| `main/emulador.c` | the app: life cycle, icon, which screen is up |
| `main/em_lib.c` | the library |
| `main/em_game.c` | the game screen: layout, the pad, gamepads, presenting frames |
| `main/em_menu.c` | the pause menu |
| `main/em_run.c` | the console in the worker (core 0): pacing by the speaker, frame skip, fast forward, frame slots, commands |
| `main/em_files.c` | the card: folders, games, saves, states, thumbnails, recents, settings |
| `main/emu_core.h` | **the contract with a console's core** |
| `cores/<id>/` | a core: its sources, `board.cmake` and `sim.cmake` |
| `sim.cmake` | the simulator's side (included by `sim/CMakeLists.txt`) |
| `web/emulador.js` | the portal's page |

**Threads.** The console runs in the HAL's worker on core 0 (LVGL is on core
1). The speaker is the clock: the worker waits while more than three frames
of sound are queued, so emulation and sound never drift; with the sound off
it is the microsecond clock. When a frame runs late the next ones are not
drawn (up to three in a row) so the game keeps its speed. Frames go to the
screen through three slots, as in Doom's and Duke's ports; the LVGL side
blits the newest with the PPA into the panel's free buffer, brings the pad
up to date there from the buffer on screen, and flips.

**A core** is a static library of its own (`cores/emu_cores.cmake`,
`emu_core_board_lib()`), compiled with its own flags (C, C++, assembly,
-O2), then linked into its module `build/modules/<dir>.so`; the app's own
files go through `project_so()`'s -Os. A core exports only its
`emu_core_<id>` tables. In the simulator the cores are linked into the app
(`sim.cmake`). `EMU_ONLY="nes gba"` builds just those.

**Adding a console:** a `cores/<dir>/` with the core, its adapter
implementing `emu_core_t`, `board.cmake` and `sim.cmake` (both globbed; set
`EMU_CORE_IDS` for several consoles in one folder); for the simulator its
id in `main/emu_cores.c`, and a skin in `em_files.c`. The texts it shows
go in `main/em_strings.c` for the catalogs.

## What it takes from the firmware

Since the firmware's commit `8bdf0da` (docs/APPS-P4.md, "Emulators"):

- **Modules:** each core is its own `apps/emulador/<dir>.so` on the card
  (`aos_hal_module_open`), loaded only while one of its consoles plays. The
  app (60 KB) finds them when it opens and keeps what their tables say in
  `emulador/.modulos.txt`, so the library opens none. A module exports
  `emu_module_abi()` and `emu_module_core(i)` (generated from
  `cores/emu_module.c.in`); one built against another `EMU_CORE_ABI` is
  refused. `tools/build_apps.sh` builds and checks them
  (`build/modules/*.so`) and `tools/install_apps.sh` installs them.
- **The GBA's recompiler:** blocks of the PSRAM heap and
  `aos_hal_code_sync` (setting `jit` in the menu, on by default).
- **A player on the keyboard:** `aos_hal_usb_kbd_held`. The arrows, X = A,
  Z = B, S = X, A = Y, Q = L, W = R, Enter = START, Backspace or the right
  Shift = SELECT; Esc opens the menu, Tab held is fast forward. It plays as
  player 2 where the console has two (setting), so one plays on the screen
  and one on the keyboard.
- **The panel at the console's rate** while a game plays
  (`aos_hal_display_refresh_request`): 60 Hz needs the panel's 80 MHz
  clock (`POST /api/panel/qos?mhz=80`, from the next boot); otherwise ~51.

## Third-party code

| Core | Source | License |
|---|---|---|
| NES | nofrendo, as Retro-Go carries it (`cores/nes/`) | GPL-2.0 (LGPL-2 headers), changes in `cores/nes/README.md` |
| GBA | gpSP with the RV32 recompiler of gpsp-esp32 (`cores/gba/`) | GPL-2.0-or-later / GPL-3.0-or-later, changes in `cores/gba/README.md` |

`emulador.so` as a whole is GPL-3.0-or-later; the repository at the
release's tag is its source. The app's own files (`main/`, `web/`) are MIT
like the rest of the repository.
