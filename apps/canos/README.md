# Caños

A pipes game: join the plant's tank to its drain before the liquid runs.
Six plants, each with its own liquid that runs and looks its own way, ten
levels each, in two ways of playing and three difficulties.

```bash
# in the simulator
cmake -S sim -B sim/build-canos -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS="hello_app;canos"
cmake --build sim/build-canos -j8
cd sim && P4_SIM_SCRIPT="wait 1500; open demo.canos" ./build-canos/p4os_sim

# the .so for the card (no pack: everything is drawn in code)
tools/build_apps.sh canos                       # apps/canos/build/canos.so
tools/install_apps.sh p4os.local canos          # restarts the board
tools/install_lang.sh p4os.local en de
```

## How it plays

| Mode | |
|---|---|
| **Girar** | The board is full of pipes turned any way. A tap turns one a quarter clockwise; the run from the tank to the drain has to close. The source, the drain, the obstacles and (in Fácil and Normal) a few pipes already right are bolted down. |
| **Completar** | Only part of the run is there, bolted on plates. The tray under the board offers the missing pieces, each as it comes (a bend that joins north and east is not the one that joins east and south), plus a few that are not needed. Pick one in the tray, tap an empty cell to lay it; tap a laid piece to take it back. |

| Difficulty | |
|---|---|
| **Fácil** | No clock: open the valve (the button, or the tank's wheel) when the run looks right. |
| **Normal** | A countdown; when it runs out the valve opens by itself. The liquid then runs and you can still turn or lay what it has not reached. |
| **Difícil** | Less time and a faster liquid; in Completar more gaps and more decoys, in Girar no pipe comes right. |

Once it runs, "Acelerar" makes it run six times as fast. It wins when the
liquid goes down the drain and loses if it reaches an open end: an empty
cell, an obstacle, the board's edge or a pipe turned the wrong way. A cross
can be crossed twice (200 points).

**Stars:** one for getting there; one for few moves (turns within the
generator's own count plus a quarter, or pieces laid within one of the gaps);
one for being quick (in Fácil, under a time per pipe; in Normal and Difícil,
opening the valve before the countdown does).

**Unlocking:** level n + 1 opens when level n is done in any difficulty of
that mode; a world opens with five levels of the one before. The progress is
`/data/canos.dat` (stars and best score per mode, difficulty, world and
level, the last choices, the sound).

## The levels

Generated from their numbers (`cn_level_make` in `cn_game.c`), so they are
the same every time and on every board, and the three difficulties play the
same plant:

- 5 x 6 cells at level 1 up to 7 x 9 at level 7 and after (upright).
- The run is a random self-avoiding walk from the source, by depth-first
  search with a budget: from level 3 it may go straight across a straight
  it laid earlier (that cell becomes a cross). It is 30 % of the board long
  at the start of the first world and up to 62 % at the end.
- Girar: every cell off the run gets a random pipe (or an obstacle), and the
  run's pipes are turned away from their solution; the turns back are the
  par. Completar: 35/50/62 % of the run's pipes become gaps and go to the
  tray, with one to five decoys; off the run, empty cells, obstacles and
  bolted decoy pipes.
- Normal and Difícil give 22 s + 1.6 s or 12 s + 1.0 s per cell of the run
  (Girar) or per three gaps (Completar).

## The six worlds

| World | Liquid | Speed | What it looks like |
|---|---|---|---|
| Planta de agua | water | 1.0 | clear blue over the gutter, caustics, bubbles, foam at the front; painted steel on wet concrete |
| Refinería | fuel | 0.85 | amber with a rainbow film; steel with hazard bands on tread plate |
| Laboratorio | acid | 0.95 | glowing green in glass tubes, fizzing; green fumes when it gets out |
| Yacimiento | crude oil | 0.55 | black and glossy, a round front, a few big bubbles; old iron on packed earth |
| Fundición | lava | 0.45 | glowing, a dark crust drifting on it; refractory channels on basalt, sparks and smoke when it leaks |
| Criogenia | liquid nitrogen | 1.25 | nearly clear, boiling all the way; frosted chrome on icy steel, a cold mist when it leaks |

Speed is against water's; the difficulty multiplies it (0.55, 0.70, 0.90
cells a second). Every number of a world is in `cn_world.c`: the drawing and
the sound have no world of their own.

## How the liquid is drawn

The board is one RGB565 picture in PSRAM shown by an `lv_canvas`
(`cn_draw.c`). It has three layers the board's size: the floor (drawn once
per level), the base (floor plus pipes and their shadows, drawn again per
cell when a pipe turns or is laid) and what is shown (base plus liquid,
valve, drain and spill). Each frame only the cells that move are drawn and
handed to LVGL as dirty areas.

- **The pipes** are drawn in code per world at the cell's size (sprites
  with alpha): a cylinder lit from the top left, couplings with bolts at
  each end, the open gutter inside, and the world's material (rust, glass,
  ceramic joints, frost, hazard bands).
- **Each shape and turn has a list of its channel pixels**: where each one
  is across the channel (u), how far along it from its entry (s, in 1/16
  px), and how the tube lights it there. The liquid loop reads only those
  (a few KB a cell) and is all integers.
- **The front** is measured along the whole run, so it crosses from one
  cell into the next without a seam. It is a parabola: the middle of a pipe
  runs ahead of its walls (Poiseuille), by a world's own amount (water a
  little, crude a lot), and it wobbles.
- **Flow:** two layers of noise streaks are carried along at the liquid's
  speed and a little faster; where they cross, clear liquids get caustics.
  Bubbles ride in slots along the run (one table per frame, two slots
  looked at a pixel), the acid and the nitrogen fizz, foam piles at water's
  front, fuel carries a rainbow film, lava a crust with glowing seams.
- **A leak** throws drops (particles with drag, the liquid's own) that land
  into a height field; the field spreads by diffusion at the liquid's pace
  (lava and crude slowly) and is drawn as a puddle lit by its own slope, with
  a thin highlight where it curves. Acid adds fumes, lava sparks and smoke,
  nitrogen a mist.
- **The source** is a tank seen from above, full, swirling while it drains,
  with a handwheel that turns when the valve opens; **the drain** a grate
  over a funnel that the liquid fills in a spiral. Both use a table of the
  cell's polar coordinates instead of `atan2` and `sqrt`.

Full cells whose streaks only drift take turns when the frame cannot afford
them all: the app measures what a cell costs and keeps the board's drawing
near 14 ms (the front, the turning pipes and the spill are drawn every
frame). The log line every five seconds while the liquid runs says the fps,
the drawing time, the cells a frame, the cost of a cell and the turns.

## Sound

`cn_snd.c`, synthesised on the streaming speaker at 16 kHz (without it,
beeps): clicks for the pieces, a squeaky wheel, a gush out of the tank, the
countdown's last seconds, and the liquid itself while it runs: a bed of
filtered noise with gurgles on top, shaped by the world (water burbles,
fuel glugs, acid crackles, crude and lava plop low, nitrogen hisses).
The speaker pauses the music only while the app is in front.

## Gamepad

D-pad moves a frame over the board · A turns (Girar) or lays/takes a piece
(Completar), on the tank opens the valve · B opens the valve, then speeds
the liquid up · L/R pick a piece in the tray · START pauses. The menu, the
levels and the panels go through `aos_pad_menu` (B goes back from the
levels and resumes from the pause).

## The screen turning

Lying down, the upright grid is shown turned a quarter, with the HUD and the
tray in a column on the right, so a level plays the same both ways. The
screen can turn mid-game: the app is built again and the game, which lives
in a static, comes back paused.

## Trying it out

Switches for the simulator (the board's `getenv` gives nothing):

| | |
|---|---|
| `CANOS_PLAY=m,d,w,l` | straight into a level: mode 0/1, difficulty 0-2, world 0-5, level 0-9 |
| `CANOS_SOLVE=1` | with the solution on the board; `=2` also opens the valve |
| `CANOS_LEAK=1` | opens the valve at once (a scrambled board leaks) |
| `CANOS_UNLOCK=1` | every world and level open |
| `CANOS_BENCH=1` | with `CANOS_PLAY`: fills the whole run and logs the cost of 100 frames |

## Measured

On the board (2026-10-22, the first five levels of water and fuel and two of
acid, upright): 23-30 fps while the liquid runs, 11-32 ms of drawing a frame.
A full cell costs 4-8 ms (in the simulator on an M-series Mac, ~30 µs), far
more than the millisecond expected, so the full cells are already at the
longest turn (one frame in six) on long runs, and their streaks drift at
5 fps. Before the valve opens, the tank alone (the wheel's two passes in
floats) takes 11-15 ms a frame at 116-140 px cells. Both are where to look
first to make it smoother.
