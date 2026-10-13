# Python

MicroPython 1.29 for P4OS in one `.so`: a console with its own keyboard,
the card as Python's filesystem, the network with `requests` and `mip`, and
every `.py` in `/sdcard/python/apps` an app of its own in the launcher,
drawing with the `aos` module the same way Lua's scripts do. A mistake in a
program is a traceback, never a reboot.

```bash
# in the simulator (sim_fs is the fake card, git-ignored)
mkdir -p sim/sim_fs/python/apps && cp apps/python/scripts/*.py sim/sim_fs/python/apps/
cmake -S sim -B sim/build-py -DP4OS_SIM_APPS=python && cmake --build sim/build-py -j8
cd sim && P4_SIM_SCRIPT="wait 1200; open py.hola" ./build-py/p4os_sim      # an app
P4_SIM_SCRIPT="wait 1200; open aos.python" ./build-py/p4os_sim              # the console

# the .so for the card
tools/build_apps.sh python                    # apps/python/build/python.so
# card: /apps/python.so; the first boot with it writes the samples to /python/apps
```

MicroPython itself is in `main/upy/`, written by `tools/gen_upy.sh` from the
release tarball (checked by its hash) with this app's `main/mpconfigport.h`;
`main/upy/VENDOR.md` says what the script changes. It is committed, like
Lua's tree: the CI needs no download. Run the script again after changing
`mpconfigport.h`, a `main/mod_*.c` with new names, or a module in `frozen/`.

## The console

A REPL as on a computer: `def`, `for`, `if`... go on with `...` and an
empty line runs them, Tab and indentation as you would expect, up and down
bring back the lines typed before (kept in `data/python_hist.txt`). The
keyboard on screen has a page of signs and a bar with the words typed most;
a USB or Bluetooth keyboard hides it (Ctrl+C stops, Ctrl+L clears). Stop
raises `KeyboardInterrupt`. `input()` asks the console for a line.
`help()` and `help('modules')` speak the interface's language.

The portal can type too: `POST /api/live?app=aos.python` with `line <text>`
(the indentation is yours to send), `stop` or `clear`; `GET
/api/live?app=aos.python&key=con` is the console's text. That is how it is
tested.

## The portal's page (`#python`)

`web/python.js`, installed with the app (`docs/PORTAL-PAGES.md`):

- **The board's console from the browser**: its text, a line to type into
  (with history; several lines pasted go one by one, closing the block),
  Detener, Limpiar, and whether it runs or waits for `input()`. If the
  console is not open, typing opens it.
- **Programas** (`/python`), **Apps** (`/python/apps`) and **Bibliotecas**
  (`/python/lib`), with an editor (Tab and Shift+Tab indent four spaces,
  Enter keeps the indent and adds four after a colon, ⌘S saves, ⌘↩ saves and
  runs). A program runs in the console ("run <path>": its tracebacks name
  the file); an app opens on the board, and what it prints and its error
  show under the editor. Uploads by drag and drop, downloads, deletes (a
  library's folder with what it holds).
- **Instalar una biblioteca**: a name for `mip.install()`, typed into the
  console on the board.

The console's side of it is the live channel: lines in (`line`, `run`,
`stop`, `clear`), its text (`con`) and its state (`st`: `idle`, `busy`,
`input`, `off`) out, published again whenever a page starts looking. An
app puts what it printed under its own id (`py.<name>`), the error between
the bytes 0x01 and 0x02. The live channel takes ids of 24 characters at
most: an app whose file name (without `.py`) is longer than 21 runs, but the
page does not see its output.

## Files

The card is `/` (a VFS of the app's own, `main/mod_card.c`) and the current
directory starts at `/python`: `open()`, `os.listdir()`, `os.mkdir()`,
`os.remove()`, `os.rename()`, `os.stat()`, `os.chdir()`, `os.statvfs()` and
`import` work as on any MicroPython board. `sys.path` is `['',
'/python/lib', '.frozen']`. Precompiled `.mpy` files load (bytecode, made
with `mpy-cross` 1.29).

The card's FAT commits what was written when the file closes. `with
open(...)` closes it; a file left to the collector
(`open('f', 'w').write(...)`) is closed after the line that opened it. There
is no `f.flush()` to the card yet (no `fsync` in the firmware's table).

## Network and libraries

`socket` (`main/mod_net.c`) is a TCP client over the firmware's connection,
and `tls`/`ssl` put it under TLS checked against the system's certificate
store: https:// works with no certificates of Python's own. On top,
micropython-lib's `requests` and `mip`, frozen into the `.so` (`frozen/`,
its `VENDOR.md` has the commit and the one change):

```python
import requests
r = requests.get("https://example.org/data.json")
r.status_code, r.json()

import mip
mip.install("umqtt.simple")        # into /python/lib, with its dependencies
```

Clients only: no `bind`/`listen`, no UDP. `tools/fake_mip.py` is a fake
package index and web server to test all of this without any outside
server (HTTPS too, with a throwaway CA, see its docstring).

Also there: `json`, `re`, `math`/`cmath` (doubles), `random`, `time` (with
the system's clock: `time.time()`, `localtime()`), `hashlib` (sha256),
`binascii`, `deflate`, `struct`, `array`, `collections`, `heapq`,
`framebuf`, `gc`, `os.urandom()`.

## Apps

Every `.py` in `/sdcard/python/apps` is an entry in the launcher (one whose
name starts with `_` is a module for the others, not an app). Its first
lines may say:

```python
# @name Gatitos                 the launcher's name (else the file's)
# @canvas 360x640               its own canvas, else the screen at x3
# @orientation portrait         or landscape: the screen turns for it
```

An `.aic` beside it is its icon (`python3 tools/aic.py asm`). The first
time the app finds no `/python/apps` it makes it with the samples
(`scripts/`, carried inside the `.so` as `main/py_samples.c`, written by
`tools/gen_samples.py`); a folder that is there, even empty, is left alone. A new file
shows in the launcher after a restart (the card is scanned at boot). While
an app runs it watches its file and starts again when it changes, so saving
is the whole step (`PUT /api/fs/put?path=/python/apps/x.py`). An error
shows over the canvas with its line; tapping it (or A on a pad) runs the
script again. `sys.exit()` closes the app. Leaving stops the script, even
one that catches `KeyboardInterrupt`.

Two ways to write one, mixed if you like:

```python
import aos                          # a loop of its own
while True:
    aos.clear(0x101820)
    x, y, down = aos.touch()
    aos.disc(x, y, 10, 0xFFD43B)
    aos.frame()                     # shows it, waits for the next frame
```

```python
import aos                          # functions the app calls, as in Lua
def init(): ...
def tick(dt): ...                   # dt in ms
def draw(): ...
def touch(x, y, ev): ...            # "down", "move", "up"
def gesture(ev, x, y, *a): ...      # "tap", "double", "long", "drag" (dx, dy),
                                    # "release" (vx, vy), "pinchstart",
                                    # "pinch" (scale, dx, dy), "pinchend"
def resize(w, h): ...               # the screen turned; without it the
                                    # script starts again
```

`touch()`, `gesture()` and `resize()` are called from `aos.frame()` in
either style. A script with neither a loop nor functions draws once and its
picture stays.

### The canvas

The OS's retro canvas (`docs/RETRO.md`), scaled by a whole number: by
default 240x426 at x3 upright, 426x240 lying down. Only what changed goes to
the screen: every primitive marks its box. The frame's ceiling is 50 a
second.

### The `aos` module

| | |
|---|---|
| `aos.W`, `aos.H` | the canvas |
| `aos.clear(c)` | colours are `0xRRGGBB` |
| `aos.pixel(x, y, c)` | |
| `aos.rect(x, y, w, h, c)` / `aos.box(...)` | filled / outline (`aos.frame(x, y, w, h, c)`, Lua's name, too) |
| `aos.round(x, y, w, h, c, cut=2)` | corners bitten |
| `aos.line(x0, y0, x1, y1, c)` | |
| `aos.disc(cx, cy, r, c)` / `aos.ring(...)` | filled / outline |
| `aos.ellipse(cx, cy, rx, ry, c)` | |
| `aos.shade(x, y, w, h, f)` | darker (f < 0) or lighter, in sixteenths |
| `aos.text(x, y, s, c, scale=1)` | 8x8, ASCII (Spanish letters lose their accent); returns the width |
| `aos.textw(s, scale=1)` | |
| `aos.blit(x, y, w, h, data, key=None)` | RGB565 pixels, 2 bytes each; `key` (0xRRGGBB) is not drawn |
| `aos.background()` | what is drawn becomes the world: each frame starts with it restored where the last one drew |
| `aos.frame()` | shows what was drawn, waits for the next frame, calls `touch()`/`gesture()`/`resize()`; returns the ms since the last |
| `aos.events()` | what came with that frame, as tuples (`("down", x, y)`, `("drag", x, y, dx, dy)`, `("resize", w, h)`...) |
| `aos.touch()` | `x, y, down` |
| `aos.fingers()` | `[(id, x, y), ...]`, every finger down |
| `aos.pad()` | `held, pressed, x, y, on`: a USB gamepad; `aos.PAD_UP`... `PAD_START` as in Lua |
| `aos.pad(mask)` | `isdown, waspressed` |
| `aos.ms()` | since the script started |
| `aos.beep(hz, ms)` | |
| `aos.stats()` | `script_ms`, `screen_ms`, `frame_ms`, `rows`, `pixels` of the last frame |
| `aos.fb` | MicroPython's `framebuf.FrameBuffer` over the same canvas (`blit`, `scroll`, `poly`, `text`...); what it draws is not marked, so from then on every frame goes out whole |
| `aos.rgb565(c)` | `0xRRGGBB` as framebuf's colour |

## The samples (`scripts/`, to `/sdcard/python/apps/`)

| File | Shows |
|---|---|
| `hola.py` | Draws once and answers the finger: Mila, a black kitten, and a heart where you touch. |
| `pelotas.py` | A loop of its own with `aos.frame()`, a grid frozen with `aos.background()`, `aos.stats()` and `aos.events()`. |
| `estrellas.py` | Made of functions (`init`, `tick`, `draw`, `gesture`), and `resize()` to go on through a turn. |
| `reloj.py` | `time.localtime()` in big letters, `@orientation landscape`. |
| `pintar.py` | `@canvas 360x640` (x2) and `aos.fingers()`: every finger paints. |
| `atrapa.py` | A game: Mila catches fish, by finger or USB gamepad (`aos.pad()`). |
| `marquesina.py` | `aos.fb`: framebuf's text, a FrameBuffer of its own with `scroll()` and `blit()`. |

## How it runs

- **One interpreter on a thread of its own** (`main/p4py_rt.c`): 128 KB of
  stack in PSRAM, core 0, priority 3. Every 64 jumps of the bytecode it
  checks the clock and gives the core away for a millisecond every 50, so a
  program that never waits does not starve the system's small tasks (it
  did: the hang watchdog restarted the board).
- **The heap starts at 2 MB and grows** in areas as a program asks (to
  12 MB, leaving the system 3 MB of PSRAM); emptied areas go back, and all
  of it when the app closes.
- **All the apps of `python.so` share one copy of the module** (the
  firmware's loader gives the same copy to all of them), so one interpreter:
  the app in front has it.
- **An app's frame** (`main/py_script.c`, `main/mod_aos.c`): the script
  draws into a buffer of its own; `aos.frame()` hands it to the LVGL task,
  which copies what changed onto the canvas and hands back the finger, the
  pad and the events. One word, with acquire and release, says who owns
  what, as in Lua's app.

## Measured on the board

`fib(24)` 481 ms; a megabyte downloaded and written to the card in 2 s;
`pelotas.py` at 50 frames a second (only the balls go out), `atrapa.py` at
48; a script that redraws the whole canvas every frame, ~24 (the CPU scales
it: 28 ms; `retro_hw` would put the PPA there), and `marquesina.py`, all of
it through framebuf, ~16; 8 MB in one `bytearray` and a dict of
50,000 strings with the heap growing to fit.

## Waiting for the firmware

In `docs/internal/HANDOFF-NUEVAS-APPS.md`: an app's module is unloaded when
another app of the same module closes (the console is not kept in the
background until that is fixed: `AOS_APP_FLAG_KEEP` goes back in
`main/py_app.c`), `fsync`/`ftruncate`, memory to run native code from
(`@micropython.native`, `viper`, native `.mpy`), the connection's API made
public with STARTTLS, UDP and servers.

Python® is a registered trademark of the Python Software Foundation; this
app uses the name, and an icon after its logo, to say what it runs, and is
not affiliated with or endorsed by the PSF. MicroPython is Damien George's
project, MIT-licensed (`main/upy/LICENSE`).
