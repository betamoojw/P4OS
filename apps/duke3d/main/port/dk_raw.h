/*
 * P4OS - Duke Nukem 3D: memory that is NOT the game's.
 *
 * The frame slots and the palette outlive the game's run (the app is still
 * blitting the last frame when the game leaves and frees all of its own
 * blocks), so they come from here, off the list, and the app gives them
 * back from destroy().
 */
#ifndef DK_RAW_H
#define DK_RAW_H

#include <stdbool.h>
#include <stddef.h>

void *dk_raw_alloc(size_t n, bool internal);
void  dk_raw_free(void *p);

#endif
