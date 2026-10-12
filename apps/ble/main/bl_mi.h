/*
 * BLE - activating a Xiaomi sensor from the board (bl_mi.c): the "register"
 * of Xiaomi's standard auth over service 0xFE95, which leaves the sensor
 * with a new bind key, the one its encrypted advertisements use. No LVGL:
 * apps/ble/test runs it against the simulator's sensor on the Mac.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "aos_hal.h"

#ifndef N_
#define N_(s) (s)
#endif

typedef enum {
    BL_MI_IDLE = 0,
    BL_MI_SUB,          /* listening to 0x0010 and 0x0019 */
    BL_MI_INFO,         /* asked for its id */
    BL_MI_KEYS,         /* exchanging the ECDH public keys */
    BL_MI_DID,          /* sending its id back, encrypted */
    BL_MI_CONFIRM,      /* waiting for its verdict */
    BL_MI_DONE,
    BL_MI_FAILED,
} bl_mi_state_t;

/* Whether the attributes have Xiaomi's auth: 0x0010 and 0x0019 in 0xFE95. */
bool bl_mi_can(const aos_ble_attr_t *at, int n);
/* Starts on the connection's attributes; false if they lack it. */
bool bl_mi_start(const aos_ble_attr_t *at, int n, uint32_t now_ms);
/* Every GATT event of the connection goes through here while it runs. */
void bl_mi_event(const aos_ble_gatt_ev_t *e, uint32_t now_ms);
/* From the app's tick: the steps that wait, and the time-out. */
void bl_mi_tick(uint32_t now_ms);
void bl_mi_reset(void);

bl_mi_state_t bl_mi_state(void);
bool bl_mi_busy(void);
/* What failed, N_() text (in Spanish; the caller translates), and its code
 * (the sensor's answer or an ATT error), 0 if none. */
const char *bl_mi_why(void);
int bl_mi_code(void);
/* The new bind key, once done. */
bool bl_mi_key(uint8_t key[16]);
/* The id the sensor gave (e.g. "blt.3.1abc..."), printable, "" if none. */
const char *bl_mi_id(void);
