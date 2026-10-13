/*
 * P4OS - Python: MicroPython's configuration for the app.
 *
 * Read twice: by tools/gen_upy.sh, when MicroPython's embed port turns its
 * sources into main/upy/ (the qstrs and the module list come out of it),
 * and by the build of python.so. A change here that adds or removes a
 * module or a feature needs gen_upy.sh again.
 *
 * The level is "extra features" (what the bigger MicroPython boards ship):
 * long integers of any size, double-precision floats, complex numbers,
 * f-strings, sets, slices with steps, the usual builtins. The board has
 * PSRAM to spare and the code runs from it, so size is not what decides.
 */
#pragma once

#include <port/mpconfigport_common.h>

/* the HAL is the app's: the embed port's has no interrupt character */
#undef MICROPY_MPHALPORT_H
#define MICROPY_MPHALPORT_H                 "p4py_hal.h"

#define MICROPY_CONFIG_ROM_LEVEL            (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

#define MICROPY_ENABLE_COMPILER             (1)
#define MICROPY_ENABLE_GC                   (1)
#define MICROPY_PY_GC                       (1)
/* the C stack is checked against a limit the app sets (its thread's) */
#define MICROPY_STACK_CHECK                 (1)
/* registers for the GC by setjmp: the generic helper knows no RISC-V */
#define MICROPY_GCREGS_SETJMP               (1)
/* exceptions by setjmp/longjmp (the firmware lends them): MicroPython's
 * RISC-V assembly jumps to a global function, which a .so cannot do */
#define MICROPY_NLR_SETJMP                  (1)

/* speed: the bytecode loop dispatches by computed goto (a table of label
 * addresses instead of a switch), and attribute lookups are cached */
#define MICROPY_OPT_COMPUTED_GOTO           (1)
#define MICROPY_OPT_MAP_LOOKUP_CACHE_SIZE   (1024)

/* numbers: Python's own */
#define MICROPY_LONGINT_IMPL                (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_FLOAT_IMPL                  (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_PY_BUILTINS_COMPLEX         (1)
#define MICROPY_PY_CMATH                    (1)
#define MICROPY_PY_MATH_SPECIAL_FUNCTIONS   (1)
#define MICROPY_PY_MATH_FACTORIAL           (1)
#define MICROPY_PY_MATH_ISCLOSE             (1)
/* the "exact" float formatting needs long double (128-bit, in software,
 * helpers the firmware does not lend); "approx" is MicroPython's other one,
 * right but for the last digit now and then */
#define MICROPY_FLOAT_FORMAT_IMPL           (MICROPY_FLOAT_FORMAT_IMPL_APPROX)
/* struct's 'e' (half float) by MicroPython's own code, not _Float16 */
#define MICROPY_FLOAT_USE_NATIVE_FLT16      (0)

/* errors that say where: file, line, and the source line */
#define MICROPY_ENABLE_SOURCE_LINE          (1)
#define MICROPY_ERROR_REPORTING             (MICROPY_ERROR_REPORTING_DETAILED)
#define MICROPY_CPYTHON_COMPAT              (1)
#define MICROPY_KBD_EXCEPTION               (1)     /* Ctrl-C: the app's stop */
#define MICROPY_ENABLE_SCHEDULER            (1)

/* the REPL's help: what "1 +" needs to continue on the next line */
#define MICROPY_REPL_EVENT_DRIVEN           (0)
#define MICROPY_HELPER_REPL                 (1)
#define MICROPY_REPL_AUTO_INDENT            (1)

/* input(): MicroPython's own reads the serial port through its readline;
 * the app will lend one that asks its console */
#define MICROPY_PY_BUILTINS_INPUT           (0)

/* modules (extmod's are copied by gen_upy.sh) */
#define MICROPY_PY_SYS                      (1)
#define MICROPY_PY_SYS_PLATFORM             "p4os"
#define MICROPY_PY_SYS_STDFILES             (0)
#define MICROPY_PY_IO                       (1)
#define MICROPY_PY_RANDOM                   (1)
#define MICROPY_PY_RANDOM_EXTRA_FUNCS       (1)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC    (p4py_random_seed())
#define MICROPY_PY_JSON                     (1)
#define MICROPY_PY_RE                       (1)
#define MICROPY_PY_HEAPQ                    (1)
#define MICROPY_PY_TIME                     (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (0)
#define MICROPY_PY_TIME_TIME_TIME_NS        (0)
#define MICROPY_PY_BINASCII                 (0)
#define MICROPY_PY_HASHLIB                  (0)
#define MICROPY_PY_DEFLATE                  (0)
#define MICROPY_PY_OS                       (0)
#define MICROPY_PY_SELECT                   (0)
#define MICROPY_PY_PLATFORM                 (0)
#define MICROPY_PY_MACHINE                  (0)
#define MICROPY_PY_THREAD                   (0)
#define MICROPY_PY_ASYNCIO                  (0)
#define MICROPY_PY_UCTYPES                  (0)
#define MICROPY_PY_FRAMEBUF                 (0)
#define MICROPY_PY_BTREE                    (0)
#define MICROPY_PY_ONEWIRE                  (0)
#define MICROPY_PY_WEBSOCKET                (0)
#define MICROPY_PY_WEBREPL                  (0)
#define MICROPY_PY_SOCKET                   (0)
#define MICROPY_PY_SSL                      (0)
#define MICROPY_PY_NETWORK                  (0)
#define MICROPY_PY_BLUETOOTH                (0)
#define MICROPY_PY_LWIP                     (0)
#define MICROPY_PY_VFS                      (0)
#define MICROPY_VFS                         (0)
#define MICROPY_PY_CRYPTOLIB                (0)
#define MICROPY_PY_OPENAMP                  (0)

/* the random seed comes from the chip (p4py_rt.c) */
#ifndef __ASSEMBLER__
unsigned int p4py_random_seed(void);
#endif

/* what MicroPython's core asks of a port */
#define MICROPY_ALLOC_PATH_MAX              (256)
#define MP_SSIZE_MAX                        (0x7fffffff)
#define MICROPY_HW_BOARD_NAME               "P4OS"
#define MICROPY_HW_MCU_NAME                 "ESP32-P4"
