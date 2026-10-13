/*
 * P4OS - Python: what MicroPython's core asks of the port's HAL
 * (mpconfigport.h points MICROPY_MPHALPORT_H here; p4py_rt.c has them).
 */
#pragma once

/* no pins here: there is no machine module */
#define mp_hal_pin_obj_t

/* Ctrl-C is the app's Stop button (p4py_interrupt), not a character */
static inline void mp_hal_set_interrupt_char(int c) { (void)c; }

/* os.urandom(): the chip's generator */
#include <stddef.h>
void mp_hal_get_random(size_t n, void *buf);

/* a system call, again if a signal cut it (extmod/vfs_posix_file.c; as the
 * unix port has it) */
#include <errno.h>
#define MP_HAL_RETRY_SYSCALL(ret, syscall, raise) { \
        for (;;) { \
            ret = syscall; \
            if (ret == -1) { \
                int err = errno; \
                if (err == EINTR) { \
                    mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS); \
                    continue; \
                } \
                raise; \
            } \
            break; \
        } \
}
