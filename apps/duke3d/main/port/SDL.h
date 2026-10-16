/*
 * P4OS - Duke Nukem 3D: the little of SDL the game still names.
 *
 * Chocolate Duke3D ran on SDL 1.2. Its video, input and timer lived in
 * engine/display.c and its sound in audiolib/dsl.c, both replaced by the
 * port (port/dk_display.c, port/dk_sound.c). What is left are a few calls
 * in the game's own files: grabbing the mouse in a menu, quitting SDL, and
 * a mutex around the reverb, which mattered when SDL mixed in its own
 * thread; here the game's task mixes, so there is nothing to lock.
 * Defined away rather than deleted, so the vendored files stay as they were.
 */
#ifndef DK_SDL_STUB_H
#define DK_SDL_STUB_H

typedef struct SDL_Surface SDL_Surface;
typedef void *SDL_mutex;

#define SDL_GRAB_QUERY          (-1)
#define SDL_GRAB_OFF            0
#define SDL_GRAB_ON             1
#define SDL_INIT_VIDEO          0x20

#define SDL_WM_GrabInput(x)     ((void)(x), SDL_GRAB_OFF)
#define SDL_ShowCursor(x)       ((void)(x), 0)
#define SDL_QuitSubSystem(x)    ((void)(x))
#define SDL_Quit()              ((void)0)
#define SDL_CreateMutex()       ((SDL_mutex *)0)
#define SDL_DestroyMutex(m)     ((void)(m))
#define SDL_mutexP(m)           ((void)(m), 0)
#define SDL_mutexV(m)           ((void)(m), 0)

#endif
