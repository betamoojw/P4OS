#ifndef _SHARED_H_
#define _SHARED_H_

/* P4OS: fixed-width types (uint32 was 'unsigned long', 64 bits on the
 * Mac, which broke the renderer's 32-bit writes there); no <malloc.h>
 * (not on the Mac); the log goes to the host through sms_log()
 * (sms_core.c); asserts compile away, the adapter checks every allocation
 * after power on; Retro-Go's IRAM_ATTR means nothing in a card app. */
#include <stdint.h>

typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;

typedef int8_t  int8;
typedef int16_t int16;
typedef int32_t int32;

#ifndef NDEBUG
#define NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>

void sms_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
uint32 sms_crc32(uint32 crc, const uint8 *buf, size_t len);

#define LOG_PRINTF(level, x...) sms_log(level, x)
#define crc32_le(a, b, c) sms_crc32(a, b, c)
#define IRAM_ATTR

#define MESSAGE_ERROR(x, ...) LOG_PRINTF(1, "!! %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_WARN(x, ...)  LOG_PRINTF(2, "** %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_INFO(x, ...)  LOG_PRINTF(3, " * %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_DEBUG(x, ...) LOG_PRINTF(4, ">> %s: " x, __func__, ## __VA_ARGS__)

/* P4OS: the core's own Z80 (../z80/z80.h) and sound (sound/sound.h, around
 * ../sms_psg.c) instead of MAME's Z80, sn76489.c and the FM files. */
#include "z80.h"
#include "memz80.h"
#include "loadrom.h"
#include "pio.h"
#include "render.h"
#include "sms.h"
#include "system.h"
#include "tms.h"
#include "vdp.h"
#include "sound/sound.h"

#endif /* _SHARED_H_ */
