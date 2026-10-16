#include "dk_compat.h" /* P4OS: the C library as the board has it */
/*
 * P4OS - Duke Nukem 3D: the port's questions to the game.
 *
 * The one port file that sees the game's headers (duke3d.h and its
 * thousand globals): what mode the game is in, for the pad, and where it
 * is, for the log.
 */
#include "duke3d.h"
#include "../duke_port.h"
#include "dk_port.h"

void dk_input_last(int *x, int *y, int32_t *dz, int32_t *dyaw);

int dk_game_mode(void)
{
    int gm = ps[myconnectindex].gm;
    if (gm & (MODE_MENU | MODE_TYPE)) return DK_MODE_MENU;
    if (dk_waiting_key()) return DK_MODE_SCREEN;   /* see dk_input.c */
    if (!(gm & MODE_GAME) || (gm & (MODE_DEMO | MODE_EOL | MODE_RESTART))) return DK_MODE_SCREEN;
    return DK_MODE_GAME;
}

void dk_game_where(char *out, int len)
{
    int gm = ps[myconnectindex].gm;
    const char *what;
    if (gm & MODE_MENU) what = "menu";
    else if (gm & MODE_DEMO) what = "demo";
    else if (gm & MODE_EOL) what = "end of level";
    else if (gm & MODE_GAME) what = "level";
    else what = "screens";
    if ((gm & MODE_GAME) && !(gm & MODE_DEMO)) {
        struct player_struct *p = &ps[myconnectindex];
        int sx, sy;
        int32_t dz, dyaw;
        dk_input_last(&sx, &sy, &dz, &dyaw);
        snprintf(out, (size_t)len, "%s E%dL%d %dx%d pos %d,%d,%d ang %d stick %d,%d dz %d", what,
                 ud.volume_number + 1, ud.level_number + 1, (int)xdim, (int)ydim, (int)p->posx,
                 (int)p->posy, (int)(p->posz >> 8), (int)p->ang, sx, sy, (int)dz);
    }
    else
        snprintf(out, (size_t)len, "%s %dx%d", what, (int)xdim, (int)ydim);
}

const char *dk_where(void)
{
    static char buf[160];
    dk_game_where(buf, sizeof buf);
    return buf;
}
