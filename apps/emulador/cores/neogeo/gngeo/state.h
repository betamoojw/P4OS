/*
 * gngeo's state.h. P4OS: cut to the two constants the vendored files still
 * name. gngeo's state.c (gzip files, a screenshot, the whole neo_mem
 * written as it lies in memory) is not vendored: the core's save states
 * are ng_core.c's, each part writing its own fields through ng_st_t
 * (ng_core.h).
 */
#ifndef _STATE_H_
#define _STATE_H_

#include "SDL.h"

#define STREAD  0
#define STWRITE 1

#endif
