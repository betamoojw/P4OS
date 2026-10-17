# Pinball 3D

A pinball in the manner of *3D Pinball Space Cadet*, upright only: the table
is seen from behind the flippers, pitched down, rendered in Blender from the
game's own camera, with a dot-matrix display above it. Six tables come with
it, each with its own layout, art, toys, buttons, sounds and music, chosen
in a carousel:

| Table | id | What it is |
|---|---|---|
| Noche de Zombis | `zombis` | a graveyard at night; the crypt's ramp, the tombs' drop targets, the mausoleum's saucer |
| Robots en Órbita | `robots` | a space station; the rocket ramp on the left, the reactor's drop targets, the launch pad |
| El Templo de la Jungla | `jungla` | the lost temple; the temple's mouth on the left, the idols in the middle, the river ramp |
| El Gran Circo | `circo` | the big top; the cannon's ramp, three rings of bumpers, jugglers on the orbit's wall |
| Casino Real | `casino` | green felt; four cards, three dice, the roulette saucer, the high rollers' ramp |
| La Isla Pirata | `piratas` | the treasure chest, the cannon ramp, the barrels |

Everything the player sees once a table is chosen is that table's: the
DMD's colour, the launch button (a skull, a rocket, a tiki mask, a cannon, a
roulette wheel, a ship's wheel), the pause button, the panels' colours with
the table's logo, the sounds and the music.

```bash
# the art and the sounds (Blender 3.3 on an M2: ~1-2 min a table at 96 samples)
python3 apps/pinball/tools/render_table.py --all            # -> apps/pinball/assets/<id>/
python3 apps/pinball/tools/pack_p4.py                       # -> apps/pinball/build/pinball_<id>_p4.pak
# without Blender (flat colours, for trying a layout)
python3 apps/pinball/tools/pack_p4.py --flat zombis

# the simulator (the packs in sim/sim_fs/apps)
cp apps/pinball/build/pinball_*_p4.pak sim/sim_fs/apps/
cmake -S sim -B sim/build-pinball -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS="hello_app;pinball"
cmake --build sim/build-pinball -j8
cd sim && P4_SIM_SCRIPT="wait 1500; open demo.pinball" ./build-pinball/p4os_sim

# the .so
tools/build_apps.sh pinball                                 # apps/pinball/build/pinball.so
```

On the card everything goes in `/apps`: `pinball.so` and one
`pinball_<id>_p4.pak` per table (2-3 MB each); `tools/install_apps.sh`
uploads them all. Neither the renders (`assets/`) nor the packs are in git.

## Playing

- **Flippers:** the lower half of the screen; the left half is the left
  flipper, the right half the right one, both at once with two fingers.
- **Launch:** when a ball waits on the plunger the table's launch button
  shows in the bottom right corner. A tap launches it hard; held, a ring
  fills and empties, and letting go launches with that strength (a soft
  launch drops into the top lanes: the blinking one is a skill shot).
- **Nudge:** a quick swipe on the upper part of the table shakes it that
  way. Too much and it is TILT: the flippers die and the ball's bonus is
  lost.
- **Pause:** the button in the DMD's corner, the system's back, or the BOOT
  button. The pause panel restarts, shows the tutorial again, turns effects
  and music on or off, or goes back to the tables.
- **USB gamepad** (`aos_pad.h`): L or left and R or right are the flippers,
  A is the plunger (tap or hold, as on the screen), up nudges, START
  pauses. In the carousel left/right change the table, A or START plays,
  up opens the settings; in the panels the d-pad moves and A presses.

The tutorial (the zones drawn over the table) shows the first time any table
is played; Settings in the carousel brings it back.

## The rules: one ruleset, the tables' names

Every table plays by the same rules (`main/pb_game.c`), the way real
machines share their bones, with the table's own words on the DMD:

- the **top lanes**: rolling through lights them, the flippers move the lit
  ones round; all three raise the bonus multiplier (up to x5). At each
  launch one blinks: rolling through it first is the **skill shot**;
- the **inlanes**, both lit, light the **kickback** in the left outlane (lit
  at the start of a game);
- the **pop bumpers** grow every 25 hits;
- the **drop targets**, all down, light a **mission** at the saucer;
- the **saucer** starts the lit mission: the table's own (five each), each
  asking for N hits of one kind (bumpers, ramps, orbits, drop targets,
  standups, saucer) in so many seconds;
- the **standup targets**, all hit, light the **lock** at the ramp; two
  locks start **multiball** (three balls), with the jackpot at the ramp and
  the saucer;
- **orbits** (in one lane and out the other) double the spinner, up to x4,
  and score three times after a ramp (a **combo**);
- five ramps, or three missions, light the **extra ball** at the saucer;
- all the missions played start the **final mode**: a multiball where
  everything scores five times;
- a **ball saver** for the first ten seconds of each ball (fifteen in
  multiball), and the **bonus** counted at the end of each ball.

The best score of each table is kept (preference `pb_b_<id>`).

## Adding a table

A table is a Python module in `tools/tables/` whose `build()` returns a
`pbtable.Table`; the game knows nothing of it but what its pack says, so a
new table needs no new code in the app, only a pack on the card:

1. **Copy a table module** (`zombis.py` is the most commented). Every table
   starts with `standard_shell()` (the cabinet, the top arch, the plunger
   lane, the two orbit lanes, the spinner) and `standard_lower()` (slings,
   inlanes, outlanes with their guard posts, flippers, kickback), and
   `top_lanes()`. What goes between is the table's: `bumper`, `ramp`
   (a mouth and a 3D path), `drops`, `target`, `saucer`, `post`, `wall`,
   `rollover`, `lamp` (inserts), `flasher`, and `deco` for toys.
2. **Theme:** `t.theme` has the colours (DMD, accent, panel, plastics,
   wood, rails, flippers, field gradient); the theme's id (`art`, `sound`,
   default the table's id) picks its painters in `paint.py` (playfield and
   backboard), its toys and buttons in `blender/build_table.py` (`DECO`,
   `ICONS`) and its sound kit in `sounds.py` (`KITS`). A theme without them
   gets plain gradients, generic toys, a plain button and bells.
3. **Missions and words:** `t.mission(names, goal, count, secs, points)`,
   and `t.text(key, es, en, de)` for the DMD's callouts (`jackpot`,
   `multiball`, `wizard`, `lock`, `drain`, `start`). The table's name and
   description come in the three languages too: no catalog to touch.
4. **Try the geometry:** `tools/preview.py <id> out.png` draws it through
   the camera and from above; `tools/harness.sh <id> 600` plays 600 balls
   with a bot and prints how long a ball lasts, ramps made, missions, and
   where balls got stuck (a pocket the ball cannot leave, two posts too
   close); `SKILL=0.9` makes the bot better, `TRAJ=file` with
   `tools/trajview.py` draws the last second of every ball lost in an
   outlane, and `heatview.py` where balls spend their time. Every table
   here was tuned until no ball stuck in 300.
5. **Render and pack:** `render_table.py <id>` then `pack_p4.py <id>`.
6. **Order** in the carousel is `Table(..., order=N)`.

Things to keep, learned tuning these six:

- **No flat tops facing up the table**: gravity rests a ball on any wall
  across the slope (a bank's backing wall); make it a roof (`jungla.py`).
- **Gaps under 30 mm make pockets**: a post next to another, a ramp's wall
  next to an orbit's. The harness finds them.
- **Toys stand where the ball never goes**: the cabinet's corners above the
  arch and the back shelf (`z=60`), the slingshots' tops and the apron
  (`z=30`). A toy on the playfield would let the ball pass through it.
- The inlane guide ends **tangent to the flipper's pivot** (`tangent_point`)
  or the ball sits in the notch.

### Lamps

The rules light lamps by name; a lamp the table does not have is simply not
shown. Names: `lane1..3`, `inL`, `inR`, `outL`, `outR`, `kickback`,
`drop<n>` (lit while its drop target is up), `target<n>`, `lock`, `ramp1`,
`saucer`, `mission1..5`, `x2..x5`, `extraball`, `shootagain`, `jackpot`,
`spinner`, `orbitL`, `orbitR`, `bumper<n>` and `sling<L|R>` (they flash on a
hit), `flash1..3` (flashers on big events).

### Objects and roles

| Type | Physics | Role |
|---|---|---|
| `bumper` | a circle that kicks | bumper |
| `sling` | a triangle whose long face kicks (up the table a little) | sling |
| `drop` | a segment that falls when hit; banks by name | drop |
| `target` | a segment that reports hits | target |
| `rollover` | a circle the ball rolls over | lane, inlane, outlane, orbit |
| `spinner` | a line; spins by the ball's speed | spinner |
| `saucer` | catches a slow ball, holds it, kicks it out at an angle | saucer |
| `ramp` | a mouth line; the ball follows a 3D path, gravity and friction decide if it gets to the end | ramp |
| `gate` | one way | |
| `kickback` | a line that kicks the ball back up when lit | kickback |

## How it works

- **The pack** (`main/pb_pak.c`, `tools/packlib.py`): LZ4 entries; the
  definition is text (`pbtable.Table.def_text`, parsed by `pb_table.c`).
- **Physics** (`main/pb_phys.c`): substeps of a millisecond (a ball at 5 m/s
  moves 5 mm a step, a third of its radius); segments and circles in a 40 mm
  grid; the flippers are tapered capsules turning at 34 rad/s whose surface
  speed goes into the bounce; friction only in a knock, so a ball rests and
  rolls on a flipper; ramps in 3D along their path; balls collide in
  multiball; a ball that sits still 4 s is shaken loose.
- **The picture** (`main/pb_render.c`): the table rendered once with every
  lamp off, plus the depth of each pixel; each lamp is the light it adds
  (lit render minus unlit, of the same crop), a drop target down is its
  pixels and depth replaced; the flippers and the spinner are frames
  rendered with everything else held out and the playfield catching their
  shadow; the ball is a chrome ball rendered once and drawn at the size its
  depth gives, **only where it is nearer than the scene** (walls, ramps,
  posts and flippers in front of it hide it, and it shows on top of a ramp),
  with a soft shadow on the playfield under it. Only what changed is drawn:
  the frame lists its dirty rectangles.
- **To the panel** (`main/pinball.c`): page flipping; each of the panel's
  three buffers gets the rows it lacks (the union of the dirty rows of the
  frames since it was last shown, `aos_hal_display_back_age`) by the DMA
  (`aos_hal_display_blit_into`), then a flip. Under a toast, or in the
  simulator, the rows go through the canvas. Over a system panel, the
  switcher or the lock screen the game pauses itself.
- **The DMD** (`main/pb_dmd.c`): 128 x 32 dots of 16 levels, a 5 x 7 font
  with the Spanish and German capitals, drawn as round dots of the table's
  colour over the DMD's surround.
- **Sound** (`main/pb_audio.c`, `tools/sounds.py`): every effect and the
  music synthesised with numpy for the theme (nothing recorded), mu-law at
  16 kHz in the pack; a ten-voice mixer on the streaming speaker.

Memory (PSRAM): ~13 MB with a table loaded (the frame, the depth of the
frame, the table's picture and its depth twice, the art); the carousel holds
the six cards' thumbnails (~4 MB) only while it is up. Internal RAM: none of
its own.

The log says every 5 s while playing:
`pinball: 60.0 fps, 1.10 ms a frame, 180 rows/frame pushed, flip 1, 13319 KB art`.

## Simulator switches

`PB_TABLE=<n>` opens the n-th table straight away, `PB_NOTUT=1` skips the
tutorial, `PB_DEBUG=1` draws the physics' shapes over the picture.

## Pending

- Measure on the board: fps, the rows pushed, the time a frame takes.
- Tune the difficulty with real players (ball time, outlanes, missions).
