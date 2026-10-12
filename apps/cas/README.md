# CAS

A computer algebra system for P4OS, on the card. The engine is
[Giac](https://www-fourier.univ-grenoble-alpes.fr/~parisse/giac.html), the CAS
of Xcas, as its author ported it to calculators (KhiCAS): exact arithmetic,
algebra, calculus, equations, matrices, statistics.

**State (2026-10-12): feasibility test, passed on the board** (firmware
0.13.5). The app is a line to type
into, the history and a bench (`Banco de pruebas`) that logs each
computation's time and the arena's size to the portal's log.

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

The bench gives the same 23 results, in the same times, with and without
KhiCAS's interface. The turtle commands (cut out of it) and `print()`
through the console have not been tried on the board yet.

Still to see: closing the app (the arena freed, the .so unloaded), Stop on a
long computation, and the 60 s limit. Giac prints in its Python-compatible
syntax (`x**2`, `list[1,2,3]`, `True`): the app will choose Xcas syntax.
