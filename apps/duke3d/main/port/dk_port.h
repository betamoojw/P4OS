/*
 * P4OS - Duke Nukem 3D: what the port's files tell each other.
 */
#ifndef DK_PORT_H
#define DK_PORT_H

#include <stdbool.h>
#include <stdint.h>

bool dk_video_alloc(void);              /* the frame slots, before the worker */
void dk_video_release(void);            /* after it */
void dk_input_reset(void);
void dk_sound_update(void);             /* keeps the speaker fed; once a frame and in waits */
void dk_sound_shutdown(void);

/* game/control.c: the pad's share of the game's input */
int  dk_action(int gamefunc);           /* a pad button holds this function */
void dk_analog(int32_t *dyaw, int32_t *dx, int32_t *dz, int32_t *dpitch);
void dk_analog_walk(int32_t *dz);       /* player.c, after xDuke zeroes the mouse's walk */

/* engine/display.c's replacement feeds the pad's keys to the game */
void dk_keys_pump(void);
void dk_key_waiting(void);              /* keyboard.c's KB_KeyWaiting() was called */
void dk_frame_done(void);               /* a frame went out: not waiting for a key */
bool dk_waiting_key(void);              /* the game loops waiting for any key */

/* game side (port/dk_game.c, which sees the game's headers) */
int  dk_game_mode(void);                /* DK_MODE_* */
enum { DK_MODE_GAME = 0, DK_MODE_MENU, DK_MODE_YESNO, DK_MODE_SCREEN };
void dk_game_where(char *out, int len);
int  dk_game_main(int argc, char **argv);   /* game.c's main(), renamed */

#endif
