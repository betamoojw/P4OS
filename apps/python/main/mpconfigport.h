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

/* a long program hands the core back now and then (p4py_vm_hook(),
 * p4py_rt.c): the interpreter's thread is above the system's small tasks on
 * its core, the tick that feeds the hang watchdog among them, and a loop
 * that never waits starved them until the board restarted. As MicroPython's
 * ESP32 port hooks its own. */
#define MICROPY_VM_HOOK_COUNT               (64)
#define MICROPY_VM_HOOK_INIT                static unsigned int vm_hook_divisor = MICROPY_VM_HOOK_COUNT;
#define MICROPY_VM_HOOK_POLL                if (--vm_hook_divisor == 0) { \
        vm_hook_divisor = MICROPY_VM_HOOK_COUNT; \
        p4py_vm_hook(); \
}
#define MICROPY_VM_HOOK_LOOP                MICROPY_VM_HOOK_POLL
#define MICROPY_VM_HOOK_RETURN              MICROPY_VM_HOOK_POLL
#ifndef __ASSEMBLER__
void p4py_vm_hook(void);
#endif

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
/* objects can have a __del__: a file left open closes when collected */
#define MICROPY_ENABLE_FINALISER            (1)

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
/* sys.stdin/stdout/stderr are the console's (mod_p4.c) */
#define MICROPY_PY_SYS_STDFILES             (1)
#define MICROPY_PY_SYS_STDIO_BUFFER         (0)     /* VfsPosix's, on descriptors */
#define MICROPY_PY_IO                       (1)
#define MICROPY_PY_RANDOM                   (1)
#define MICROPY_PY_RANDOM_EXTRA_FUNCS       (1)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC    (p4py_random_seed())
#define MICROPY_PY_JSON                     (1)
#define MICROPY_PY_RE                       (1)
#define MICROPY_PY_HEAPQ                    (1)
#define MICROPY_PY_TIME                     (1)
/* the clock is the system's, set by the network (p4py_time.h) */
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_TIME_TIME_NS        (1)
#define MICROPY_PY_TIME_INCLUDEFILE         "p4py_time.h"
#define MICROPY_EPOCH_IS_1970               (1)
#define MICROPY_PY_BINASCII                 (1)
#define MICROPY_PY_HASHLIB                  (1)     /* sha256 (mip checks with it) */
#define MICROPY_PY_DEFLATE                  (1)     /* gzip and zlib streams */
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
#define MICROPY_PY_CRYPTOLIB                (0)
#define MICROPY_PY_OPENAMP                  (0)

/* files: the card is mounted at "/" (mod_card.c, a filesystem of the
 * app's own over the firmware's POSIX calls, with a current directory that
 * starts at /python); open(), os and import go through it as on any
 * MicroPython board. The file objects are extmod/vfs_posix_file.c's. */
#define MICROPY_VFS                         (1)
#define MICROPY_PY_VFS                      (0)     /* its module includes FatFs headers */
#define MICROPY_READER_VFS                  (1)
#define MICROPY_PY_OS                       (1)
#define MICROPY_PY_OS_STATVFS               (1)
#define MICROPY_PY_OS_UNAME                 (1)
#define MICROPY_PY_OS_URANDOM               (1)
#define MICROPY_PY_OS_SEP                   (1)
/* precompiled modules (.mpy, from mpy-cross): bytecode only for now, native
 * code needs memory the firmware can execute from */
#define MICROPY_PERSISTENT_CODE_LOAD        (1)

/* Python modules inside the app (frozen/: requests, mip, tls, ssl), found
 * under ".frozen" in sys.path */
#define MICROPY_MODULE_FROZEN_STR           (1)
/* none, but on: with strings alone 1.29 never loads one (py/frozenmod.c) */
#define MICROPY_MODULE_FROZEN_MPY           (1)

/* the port's builtins (mod_p4.c): input() asks the app's console, and
 * help() speaks the interface's language (MicroPython's own help text is a
 * fixed array, so its help is off and the port's takes the name) */
#define MICROPY_PY_BUILTINS_HELP            (0)
#define MICROPY_PORT_BUILTINS \
    { MP_ROM_QSTR(MP_QSTR_input), MP_ROM_PTR(&p4py_input_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_help), MP_ROM_PTR(&p4py_help_obj) },
#ifndef __ASSEMBLER__
extern const struct _mp_obj_fun_builtin_var_t p4py_input_obj;
extern const struct _mp_obj_fun_builtin_var_t p4py_help_obj;
#endif

/* the random seed comes from the chip (p4py_rt.c) */
#ifndef __ASSEMBLER__
unsigned int p4py_random_seed(void);
#endif

/* what MicroPython's core asks of a port */
#define MICROPY_ALLOC_PATH_MAX              (256)
#define MP_SSIZE_MAX                        (0x7fffffff)
#define MICROPY_HW_BOARD_NAME               "P4OS"
#define MICROPY_HW_MCU_NAME                 "ESP32-P4"
