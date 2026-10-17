/*
 * P4OS - Emulador, Atari 7800 core: the save-state walk (P4OS's own file,
 * not ProSystem's).
 *
 * Every module of ProSystem that holds state has a <module>_State(fn, ctx)
 * that calls fn once for each of its variables, with its address and size,
 * always in the same order. The adapter's fn copies them out to a save
 * state or back in from one. ProSystem's own prosystem_Save()/Load() kept
 * only the CPU registers, the bank and the RAM: a loaded state came back
 * with MARIA, the RIOT timers and the sound chips as they happened to be.
 */
#ifndef P4STATE_H
#define P4STATE_H

#include <stdint.h>

typedef void (*p4_state_fn)(void *ctx, void *ptr, uint32_t size);

#define P4_STATE(fn, ctx, var) (fn)((ctx), &(var), (uint32_t)sizeof(var))

void sally_State(p4_state_fn fn, void *ctx);
void maria_State(p4_state_fn fn, void *ctx);
void riot_State(p4_state_fn fn, void *ctx);
void tia_State(p4_state_fn fn, void *ctx);
void pokey_State(p4_state_fn fn, void *ctx);
void cartridge_State(p4_state_fn fn, void *ctx);
void prosystem_State(p4_state_fn fn, void *ctx);

/* The memory map: RAM and cartridge RAM contents, and where each of the
 * sixteen 4 KB pages points, as (region, offset) pairs. memory_MapOut()
 * describes the map; memory_MapIn() checks a description and, if every page
 * lands inside its region, installs it (false and nothing changed if not). */
void memory_State(p4_state_fn fn, void *ctx);
void memory_MapOut(uint32_t map[16]);
int memory_MapIn(const uint32_t map[16]);

/* The regions the pages may point into (Cartridge.c and Bios.c own them). */
const uint8_t *cartridge_Image(uint32_t *size);
uint8_t *cartridge_PokeyRam(void);
const uint8_t *bios_Image(uint32_t *size);

/* Frees what memory_Reset(), palette_Load() and pokey_Reset() allocate;
 * pokey_Ready() says whether pokey_Reset() got its table. */
void memory_Release(void);
void palette_Release(void);
void pokey_Release(void);
#include <stdbool.h>
bool pokey_Ready(void);

#endif
