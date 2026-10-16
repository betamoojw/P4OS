/*
 * P4OS - Duke Nukem 3D: the Sound Blaster's FM chip, in software.
 *
 * DOSBox's DBOPL (port/dbopl.c), behind the three calls dk_music.h
 * declares. It lives apart from dk_music.c because its header and the
 * game's do not mix (Channel, Operator, OFF...).
 *
 * Apogee's driver used an OPL3 as a mono chip: both outputs on for every
 * voice (register C0 bits 4-5). DBOPL in OPL3 mode always renders the two
 * outputs, so the left one is the mono signal and the right one is the
 * same again.
 */
#pragma GCC optimize("O2")

#include "dk_compat.h" /* P4OS: the C library as the board has it */
#include "dk_music.h"
#include "dbopl.h"

static Chip s_chip;

void dk_opl_init(int rate)
{
    DBOPL_InitTables();             /* once; it keeps its own flag */
    /* Chip__Chip() sets only some fields; a chip used before (a game
     * started again in the same load of the .so) starts from nothing */
    memset(&s_chip, 0, sizeof s_chip);
    Chip__Chip(&s_chip);
    Chip__Setup(&s_chip, (Bit32u)rate);
}

void dk_opl_write(unsigned reg, unsigned val)
{
    Chip__WriteReg(&s_chip, reg & 0x1ff, (Bit8u)val);
}

int dk_opl_generate(int32_t *buf, int n)
{
    if (s_chip.opl3Active) {
        Chip__GenerateBlock3(&s_chip, (Bitu)n, (Bit32s *)buf);
        return 2;
    }
    Chip__GenerateBlock2(&s_chip, (Bitu)n, (Bit32s *)buf);
    return 1;
}
