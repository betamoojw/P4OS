/*
 * P4OS - Emulador: the consoles this build of the app has.
 *
 * A core is in only when its library was built (EMU_HAVE_<ID>, set by the
 * app's CMakeLists.txt and by sim.cmake), so a console being written does
 * not stop the others from building.
 */
#include "emu.h"

const emu_core_t *const emu_cores[] = {
#ifdef EMU_HAVE_NES
    &emu_core_nes,
#endif
#ifdef EMU_HAVE_GBA
    &emu_core_gba,
#endif
    NULL,
};

const emu_core_t *emu_core_by_id(const char *id)
{
    for (int i = 0; emu_cores[i]; i++) {
        if (strcmp(emu_cores[i]->id, id) == 0) return emu_cores[i];
    }
    return NULL;
}
