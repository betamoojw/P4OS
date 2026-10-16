/*
 * P4OS - Duke Nukem 3D: engine/platform.h's branch for the board.
 *
 * engine/unix_compat.h with what does not apply taken out: no SDL, no UDP
 * networking (USER_DUMMY_NETWORK: one player). It still says PLATFORM_UNIX,
 * the branch of the game's code that fits a POSIX file system (dukeunix.h).
 * kmalloc and company are in dk_compat.h, which every file has already.
 */
#ifndef DK_PLATFORM_H
#define DK_PLATFORM_H

#include "dk_compat.h"

#ifdef FP_OFF
#undef FP_OFF
#endif
#define FP_OFF(x) ((int32_t) (x))

#ifndef max
#define max(x, y)  (((x) > (y)) ? (x) : (y))
#endif
#ifndef min
#define min(x, y)  (((x) < (y)) ? (x) : (y))
#endif

#include <inttypes.h>
#define __int64 int64_t

#define O_BINARY 0
#define PLATFORM_UNIX 1

#define stricmp strcasecmp
#define strcmpi strcasecmp

#define S_IREAD S_IRUSR

#define USER_DUMMY_NETWORK 1

#endif
