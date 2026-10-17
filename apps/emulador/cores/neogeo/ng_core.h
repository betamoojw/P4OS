/*
 * P4OS - Emulador, Neo Geo core: what the core's own files share.
 *
 * ng_core.c is the emu_core_t table and the frame; ng_load.c finds the
 * BIOS and the game, loads them and sets up the caches on the card;
 * ng_platform.c has what gngeo expects from its platform layer; ng_z80.c
 * the sound CPU; ng_zip.c the zip reader.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emu_core.h"

/* ---- options the loader needs (ng_core.c keeps them) ------------------ */

typedef enum { NG_SYS_MVS = 0, NG_SYS_AES, NG_SYS_UNI } ng_system_t;

typedef struct {
    ng_system_t system;
    int         region;         /* 0 Europe, 1 USA, 2 Japan, 3 Asia */
    int         spr_cache_mb;   /* 0: as much as the budget leaves */
    int         sample_rate;
} ng_load_opts_t;

/* What the loader leaves for the core: the frame rate's sound, the game
 * name for the logs, and whether the battery RAM is the MVS's. */
typedef struct {
    ng_system_t system;         /* the BIOS loaded (another when the one
                                   asked for is missing) */
    char  game[24];
    char  longname[64];
    bool  tiles_streamed, adpcma_streamed, adpcmb_streamed;
    bool  raster;
    uint32_t spr_cache_kb;
    uint32_t used_kb;           /* the core's big allocations */
} ng_load_info_t;

/* ng_load.c: everything from the zip to a console ready to reset. false
 * with a fixed Spanish message in err. */
bool ng_load_game(const emu_host_t *host, const emu_load_t *req, const ng_load_opts_t *opts,
                  ng_load_info_t *info, char *err, size_t errlen);
/* Frees what ng_load_game() took (also after it failed half way). */
void ng_unload_game(void);
/* Another BIOS for the loaded game (the option changed): read into the
 * 68000's BIOS region, raw. false (and the old one kept) when missing. */
bool ng_bios_switch(const char *bios_dir, ng_system_t sys, int region, ng_system_t *got);

/* ---- allocations under the core's budget ------------------------------ */

/* The core's big blocks (ROMs, caches, the 68000's tables) go through
 * these, which keep the total under NG_BUDGET so the board's PSRAM keeps
 * room for the app and the system, and so the simulator (where malloc
 * never fails) decides the same things as the board. */
#define NG_BUDGET (15u * 1024 * 1024 + 512u * 1024)
/* P4OS: the budget follows the free PSRAM at load (ng_big_budget) */
#define NG_BUDGET_MIN   (8u * 1024 * 1024)
#define NG_BUDGET_MAX   (17u * 1024 * 1024)
#define NG_SYSTEM_KEEP  (1228u * 1024)
void ng_big_budget(size_t free_psram);
void *ng_big_alloc(size_t n);       /* zeroed; NULL past the budget */
void  ng_big_free(void *p);
size_t ng_big_used(void);
size_t ng_big_left(void);

/* ---- logging ---------------------------------------------------------- */

void ng_platform_init(const emu_host_t *host);
void ng_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* ng_printf's lines per game (gngeo is chatty): reset at each load. */
void ng_printf_reset(void);

/* ---- save states ------------------------------------------------------ */

typedef struct {
    bool     writing;
    bool     failed;
    uint8_t *buf;
    uint32_t pos, len, cap;
} ng_st_t;

/* Copies n bytes to the state (writing) or from it (reading). Reading
 * past the end marks the state failed and leaves 'p' untouched. */
void ng_st_data(ng_st_t *st, void *p, uint32_t n);

void ng_z80_state(ng_st_t *st);
void ng_mem_state(ng_st_t *st);     /* memory.c, P4OS */
void ng_timer_state(ng_st_t *st);   /* timer.c, P4OS */
void ng_ym_state(ng_st_t *st);      /* ym2610.c, P4OS */
void ng_rtc_state(ng_st_t *st);     /* pd4990a.c, P4OS */

/* ---- the sound CPU (ng_z80.c) ----------------------------------------- */

bool ng_z80_alloc(void);
void ng_z80_free(void);

/* ---- the 68000 (generator68k_interf.c, P4OS) ------------------------- */

bool cpu_68k_alloc(void);
void cpu_68k_init_bios(void);       /* byte order of a BIOS read after init */
void cpu_68k_free(void);
void cpu_68k_frame_start(void);
void ng_68k_state(ng_st_t *st);

/* ---- the YM2610 (ym2610.c, P4OS) -------------------------------------- */

bool ng_ym_alloc(void);
void ng_ym_free(void);
