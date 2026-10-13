/*
 * P4OS - Python: what MicroPython's core asks of the port's HAL
 * (mpconfigport.h points MICROPY_MPHALPORT_H here; p4py_rt.c has them).
 */
#pragma once

/* no pins here: there is no machine module */
#define mp_hal_pin_obj_t

/* Ctrl-C is the app's Stop button (p4py_interrupt), not a character */
static inline void mp_hal_set_interrupt_char(int c) { (void)c; }
