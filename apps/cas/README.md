# CAS

A computer algebra system for P4OS, on the card. The engine is
[Giac](https://www-fourier.univ-grenoble-alpes.fr/~parisse/giac.html), the CAS
of Xcas, as its author ported it to calculators (KhiCAS): exact arithmetic,
algebra, calculus, equations, matrices, statistics.

**State (2026-10-12):** the engine runs on the board (the feasibility test
below), and the app has its two first views:

- **Calculate:** a history of inputs and results drawn as a book prints
  them, a two-dimensional editor (fractions, exponents, roots, integrals,
  sums, derivatives, limits, matrices as templates) and a maths keypad of
  four pages. Tapping an input puts it back in the editor; tapping a result
  inserts it at the cursor. A USB or Bluetooth keyboard types too ("/" a
  fraction, "^" an exponent).
- **Graph:** up to six functions f1..f6 on a canvas that pans with one
  finger, zooms with two (or a double tap), and traces a curve with a long
  press. Each function is also defined in Giac, so the calculator can use
  f1(x).

- **Statistics:** four lists L1..L4 in a sheet (a tap on a cell types into
  it and moves down) with the summary of the chosen list; histogram, box
  plots, or L1 against L2; regressions (linear, quadratic, cubic,
  exponential, logarithmic, power) with r and r², sent to the grapher with
  one button; distributions (normal, t, χ², F, binomial, Poisson,
  exponential): P(a ≤ X ≤ b) or the inverse with the area shaded; tests
  and intervals (t for a mean, z for a proportion, Welch's t for two means).
- **Finance:** TVM (fill four, "=?" on the fifth), loans (French, German,
  American) with the table and a chart of interest and principal, rates
  (TNA with its compounding days, TEA, TEM, daily; a fixed-term deposit;
  the real rate against inflation), and NPV, IRR and payback of cash flows.

Statistics and finance are computed in `main/cas_num.c` (double, as each
value is typed, no engine; checked against tables and the financial
calculators' examples). A field takes a value the way the keypad writes it
(1/3, 2π, √2) through `mx_text_value()`.

The history, the functions, the lists and the finance values are kept in
`data/cas_hist.txt`, `cas_graph.txt`, `cas_stats.txt` and `cas_fin.txt`. The menu has *Stop*, *Clear the history* and the
bench, which now writes its results into the history as well as the log.

## Building

```bash
apps/cas/engine/build_engine.sh     # once: downloads and builds the engine (~4 min)
tools/build_apps.sh cas             # apps/cas/build/cas.so, ~5 MB
```

`build_engine.sh` downloads three source archives into `~/.cache/p4os-cas`
(checked against their SHA-256) and leaves `engine/lib/` (ignored by git):

| Source | What comes from it | License |
|---|---|---|
| KhiCAS for the NumWorks (`khiext.tar.bz2`) | Giac 1.9, GMP 6.2.1, MPFR 4.1.0, MPFI 1.5.4 | GPL-3.0 (Giac), LGPL-3.0 (GMP, MPFR, MPFI) |
| GCC 14.2.0 | libstdc++, libsupc++, libgcc's soft-fp | GPL-3.0 with the runtime exception |
| openlibm 0.8.8 | the double and long double maths | MIT / BSD |

`main/tlsf.c` is Matthew Conte's TLSF 3.1 (BSD-3-Clause), as KhiCAS carries
it. `cas.so` as a whole is GPL-3.0; the firmware that loads it stays MIT.

In the simulator the app runs against a stand-in (`main/cas_rt_sim.c`): the
engine is built for the board only.

## The interface (main/mx_*.c)

One tree of boxes serves for typing and for showing (`mx.h`):

    text  --mx_parse-->  AST  --mx_ast_to_row-->  boxes  --mx_view-->  screen
    boxes --mx_row_text--> text for Giac
    AST   --mx_compile-->  stack code over float for the grapher

- **mx_edit.c**: keys are strings; "/" takes the operand before the cursor
  as numerator (as in Desmos), backspace goes into a structure rather than
  deleting it, a tap places the cursor.
- **mx_layout.c**: sizes from the font size alone; radicals, tall brackets,
  integral and sum signs drawn with lines, so they grow with their content;
  brackets one line high use the font's glyphs.
- **mx_parse.c**: a Pratt parser for Giac's Xcas syntax (the engine is set
  to it with `python_compat(0)`), the book-style rendering (2*x as 2x, a/b
  stacked, exp(x) as e^x, lists of equal lists as matrices), and the
  grapher's evaluator: about a hundred times faster than asking Giac for
  each point. A function it cannot compile (diff(), something defined in
  the calculator) goes to Giac as simplify(...) and the answer is compiled.
- The fonts are the firmware's Inter, which already carries Greek, arrows
  and the Mathematical Operators block: no font pack.
- **cas_plot.c** is the canvas the grapher and the charts share: RGB565
  written directly, with its own anti-aliased thick lines, discs and
  translucent rectangles (a full redraw is a few milliseconds), and the
  numbers on the axes through LVGL.
- **cas_form.c**: numeric fields, the entry sheet (the same editor and
  keypad, on the top layer), a segmented choice, cards and result rows.

## How the engine fits in a .so

- **Everything is rebuilt as PIC.** ld refuses non-PIC code in a shared
  object, and the libstdc++, libgcc and libm of the toolchain are not PIC.
  The firmware exports neither the C++ runtime nor double-precision maths.
- **One archive, allocations renamed.** `build_engine.sh` merges it all into
  `libcasengine.a` and renames `malloc`/`free`/... to `cas_malloc`/... with
  objcopy (project_so() takes no `-Wl,--wrap`).
- **The arena.** `cas_malloc` is TLSF over 2 MB pools of PSRAM, up to 24 MB.
  Closing the app frees the pools whole: Giac keeps things in globals for
  the life of the program, and here that life ends with the `.so`.
- **Constructors.** The loader does not run `.init_array`; the engine's
  thread runs it between two markers: `.init_array.00000` in `cas_rt.c`
  (sorted first) and `engine/cas_end.c` (the last archive of the link).
- **Exports.** `engine/cas_exports.ld` goes into the link as an input file
  named `.o` (ld reads it as an implicit script) and leaves only
  `aos_app_abi`/`aos_app_init` global. Without it the engine's ~12,000
  internal references were named relocations, and the loader looks names up
  in the firmware's table first: a firmware `operator new` or `floor` would
  have replaced the engine's. With it: 31,759 relative relocations, 149
  named ones (the firmware's own functions).
- **The thread.** Giac runs on its own task (256 KB stack in PSRAM, core 0,
  priority 3). `back_key_pressed()` - what Giac's `control_c()` asks on the
  NumWorks - yields every 100 ms, and stops the computation on *Parar* or
  after 60 s.
- **KhiCAS's interface** (`kdisplay.cc`, ~680 KB of code) is left out.
  What Giac asks of it is answered by `engine/cas_kshim.cc` ("nothing on
  screen, cancelled"); its turtle commands are cut out of it by
  `build_engine.sh` into `cas_turtle.cc`, unchanged, since Giac's command
  table points at them. `print()` in a Giac program writes to
  `dConsolePut()`, which keeps the text and hands it over with the result.
  The calculator's own hooks (keys, screen, files) are stubs in `cas_rt.c`.

## Measured on the board (2026-10-12)

| | |
|---|---|
| Boot scan, `cas.so` alone | 536 ms at 5.0 MB (765 ms at 5.9 MB, with KhiCAS's interface); the whole scan 2.6 s, 45 modules |
| Opening the app | ~470 ms at 5.0 MB (~660 at 5.9): dlopen + relocations; constructors and Giac's context 3 ms |
| The bench, 23 lines | all right; 1 to 92 ms each (`integrate(x*sin(x),x,0,pi)` 92, `sum(1/k^2,k,1,inf)` 84, `laplace` 79, `expand((x+y)^12)` 33, `factor(x^4-1)` 10) |
| Arena | 2 MB pool, 21 KB at the peak |
| A 360-month loan's table | 228 ms to fill an `lv_table` (1440 cells, each measured); with `cas_grid`, which draws only the rows in view, an invalidation |
| Saving after each value | up to a second on the LVGL task (open/truncate/close on the card's FAT); now `cas_save` writes on a thread |

Statistics and finance were checked on the board against hand computation:
summary and quartiles, histogram, linear regression, normal and t, the
t test with its interval, TVM (payment and N), the loan, rate equivalents,
NPV/IRR and payback.

The bench gives the same 23 results, in the same times, with and without
KhiCAS's interface. The turtle commands (cut out of it) and `print()`
through the console have not been tried on the board yet.

Still to see: closing the app (the arena freed, the .so unloaded), Stop on a
long computation, and the 60 s limit. Giac prints in its Python-compatible
syntax (`x**2`, `list[1,2,3]`, `True`): the app will choose Xcas syntax.
