/*
 * P4OS - Emulador: the console, running in the worker.
 *
 * The worker (core 0, beside Wi-Fi; LVGL is on core 1) loads the game and
 * then runs the console a frame at a time. Nothing here touches LVGL: the
 * game screen (em_game.c) talks to this file through a few volatile
 * fields, the frame slots and the command box.
 *
 * Pacing. The microsecond clock sets the deadline of each frame, and the
 * speaker's ring nudges it: a ring holding more than twice PACE_FRAMES
 * frames of sound moves the deadline later, one under half of it earlier,
 * so the console runs exactly as fast as the speaker plays, with no drift
 * between the two (a speaker that drains nothing, as the simulator's,
 * leaves the plain clock). When a frame takes longer than its share (a
 * heavy scene, a slow core) the next ones are run without drawing (frame
 * skip), up to SKIP_MAX in a row, so the game keeps its speed and the
 * picture drops frames instead; with the sound on that is what keeps the
 * ring from running dry.
 *
 * Fast forward: no waiting, no sound, a picture every TURBO_DRAW frames.
 *
 * The frames: the core hands each picture to video(), which copies it into
 * one of three slots, the same scheme as Doom's and Duke's ports:
 *
 *   FREE -> WRITING (worker) -> READY -> SHOWN (screen, blitting) -> FREE
 *
 * The worker never waits for the screen: a READY frame not taken yet is
 * simply overwritten by a newer one.
 *
 * Commands (save, load, reset, an option, the battery save) are posted by
 * the screen with a ticket and done here between two frames, where the
 * core is at rest.
 */
#include "emu.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS           3
#define PACE_FRAMES     3       /* frames of sound kept queued ahead            */
#define SKIP_MAX        3       /* frames in a row left undrawn, at most        */
#define TURBO_DRAW      3       /* fast forward: one picture every this many    */
#define STACK_BYTES     (32 * 1024)     /* PSRAM; gpSP's recompiler wants 16 KB */
#define STATS_MS        2000
#define FLUSH_MS        30000   /* the battery save, if the game changed it */

enum { SLOT_FREE = 0, SLOT_WRITING, SLOT_READY, SLOT_SHOWN };

static struct {
    const emu_core_t   *core;
    emu_game_t          game;
    emu_host_t          host;

    volatile emu_run_state_t state;
    char                err[160];

    /* the screen's side */
    volatile bool       paused;
    volatile bool       turbo;
    volatile bool       sound;
    volatile int        skip_mode;
    volatile uint32_t   pads[EMU_MAX_PLAYERS];

    /* frames */
    uint16_t           *slot[SLOTS];
    volatile int        st[SLOTS];
    volatile uint32_t   seq[SLOTS];
    volatile uint16_t   sw[SLOTS], sh[SLOTS];
    uint32_t            frame_seq;
    int                 shown;
    int                 max_w, max_h;

    /* sound */
    bool                spk;
    bool                spk_seen;   /* its ring has held something: it is a real one */
    int                 rate;
    int16_t            *mono;
    int                 mono_n;

    /* the command box */
    volatile uint32_t   cmd_ticket;     /* posted */
    volatile uint32_t   cmd_done;       /* last done */
    volatile bool       cmd_ok;
    emu_cmd_t           cmd;
    int                 cmd_arg;
    char                cmd_key[32];

    /* measuring */
    uint32_t            n_frames, n_drawn, n_skipped, n_under;
    uint64_t            run_us;
    uint32_t            run_us_max;
    uint32_t            period_ms;
    emu_run_stats_t     stats;
    bool                stop_save_auto;
    float               fps;
} R;

static bool cas(volatile int *p, int from, int to)
{
    int expected = from;
    return __atomic_compare_exchange_n(p, &expected, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* ---------------------------------------------------------------------------
 * What the core calls back
 * ------------------------------------------------------------------------ */

static void host_log(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    aos_hal_log(EMU_TAG, "%s: %s", R.core ? R.core->id : "?", line);
}

static int grab_slot(void)
{
    for (int i = 0; i < SLOTS; i++) {
        if (cas(&R.st[i], SLOT_FREE, SLOT_WRITING)) return i;
    }
    /* none free: the oldest frame nobody took yet */
    int best = -1;
    uint32_t bs = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (R.st[i] == SLOT_READY && (best < 0 || R.seq[i] < bs)) {
            best = i;
            bs = R.seq[i];
        }
    }
    if (best >= 0 && cas(&R.st[best], SLOT_READY, SLOT_WRITING)) return best;
    return -1;
}

static void host_video(const uint16_t *px, int w, int h, int stride)
{
    if (!px || w <= 0 || h <= 0 || w > R.max_w || h > R.max_h) return;
    int i = grab_slot();
    if (i < 0) return;                  /* the screen holds them all: drop it */
    uint16_t *d = R.slot[i];
    if (stride == w * 2) {
        memcpy(d, px, (size_t)w * h * 2);
    } else {
        const uint8_t *s = (const uint8_t *)px;
        for (int y = 0; y < h; y++, s += stride, d += w) memcpy(d, s, (size_t)w * 2);
    }
    R.sw[i] = (uint16_t)w;
    R.sh[i] = (uint16_t)h;
    R.seq[i] = ++R.frame_seq;
    __atomic_store_n(&R.st[i], SLOT_READY, __ATOMIC_RELEASE);
    R.n_drawn++;
}

static void host_audio(const int16_t *lr, int frames)
{
    if (!R.spk || !R.sound || R.turbo || frames <= 0) return;
    while (frames > 0) {
        int n = frames < R.mono_n ? frames : R.mono_n;
        for (int i = 0; i < n; i++) {
            R.mono[i] = (int16_t)(((int32_t)lr[2 * i] + lr[2 * i + 1]) >> 1);
        }
        aos_hal_spk_write(R.mono, n);
        lr += 2 * n;
        frames -= n;
    }
}

/* Executable memory for a recompiler (gpSP's for the GBA): blocks of the
 * PSRAM heap, which the P4 runs code from as it runs the apps' own, and
 * the firmware's cache sync before a block written runs (aos_hal.h). The
 * simulator runs nothing recompiled: the cores get NULL there and
 * interpret. */
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"

static void *exec_alloc(size_t align, size_t size)
{
    if (align < 256) align = 256;           /* a cache line, for the sync */
    return heap_caps_aligned_alloc(align, size, MALLOC_CAP_SPIRAM);
}

static void exec_free(void *code)
{
    heap_caps_free(code);
}

static bool exec_publish(void *code, size_t size)
{
    return aos_hal_code_sync(code, size);
}
#define EXEC_ALLOC   exec_alloc
#define EXEC_FREE    exec_free
#define EXEC_PUBLISH exec_publish
#else
#define EXEC_ALLOC   NULL
#define EXEC_FREE    NULL
#define EXEC_PUBLISH NULL
#endif

/* ---------------------------------------------------------------------------
 * The frames, from the screen's side
 * ------------------------------------------------------------------------ */

const uint16_t *emu_frame_take(int *w, int *h, int *stride)
{
    int best = -1;
    uint32_t bs = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (R.st[i] == SLOT_READY && (best < 0 || R.seq[i] > bs)) {
            best = i;
            bs = R.seq[i];
        }
    }
    if (best < 0 || !cas(&R.st[best], SLOT_READY, SLOT_SHOWN)) return NULL;
    R.shown = best;
    *w = R.sw[best];
    *h = R.sh[best];
    *stride = R.sw[best];
    return R.slot[best];
}

void emu_frame_release(void)
{
    if (R.shown >= 0) {
        __atomic_store_n(&R.st[R.shown], SLOT_FREE, __ATOMIC_RELEASE);
        R.shown = -1;
    }
}

/* The newest frame drawn, for a thumbnail: from the worker only, where no
 * slot can be being written. */
static int newest_slot(void)
{
    int best = -1;
    uint32_t bs = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (R.st[i] != SLOT_FREE && R.st[i] != SLOT_WRITING && R.seq[i] > bs) {
            best = i;
            bs = R.seq[i];
        }
    }
    if (best < 0) {
        for (int i = 0; i < SLOTS; i++) {
            if (R.seq[i] > bs) {
                best = i;
                bs = R.seq[i];
            }
        }
    }
    return best;
}

/* ---------------------------------------------------------------------------
 * The worker
 * ------------------------------------------------------------------------ */

static void slots_free(void)
{
    for (int i = 0; i < SLOTS; i++) {
        free(R.slot[i]);
        R.slot[i] = NULL;
        R.st[i] = SLOT_FREE;
        R.seq[i] = 0;
    }
    R.shown = -1;
}

static bool slots_alloc(void)
{
    size_t bytes = (size_t)R.max_w * R.max_h * 2;
    for (int i = 0; i < SLOTS; i++) {
        R.slot[i] = emu_psram(bytes);
        if (!R.slot[i]) {
            slots_free();
            return false;
        }
        memset(R.slot[i], 0, bytes);
        R.st[i] = SLOT_FREE;
        R.seq[i] = 0;
    }
    R.shown = -1;
    R.frame_seq = 0;
    return true;
}

static bool save_state(int slot)
{
    char path[EMU_PATH_MAX];
    emu_game_state(&R.game, slot, path, sizeof path);
    uint64_t t0 = aos_hal_uptime_us();
    bool ok = R.core->state_save(path);
    if (ok) {
        int i = newest_slot();
        if (i >= 0 && R.seq[i]) {
            char thumb[EMU_PATH_MAX];
            emu_game_thumb(&R.game, slot, thumb, sizeof thumb);
            emu_thumb_write(thumb, R.slot[i], R.sw[i], R.sh[i], R.sw[i] * 2);
        }
    }
    aos_hal_log(EMU_TAG, "state %d %s in %u ms", slot, ok ? "saved" : "NOT saved",
                (unsigned)((aos_hal_uptime_us() - t0) / 1000));
    return ok;
}

static bool load_state(int slot)
{
    char path[EMU_PATH_MAX];
    emu_game_state(&R.game, slot, path, sizeof path);
    if (!emu_file_exists(path, NULL)) return false;
    bool ok = R.core->state_load(path);
    aos_hal_log(EMU_TAG, "state %d %s", slot, ok ? "loaded" : "NOT loaded");
    return ok;
}

static void do_command(void)
{
    uint32_t t = R.cmd_ticket;
    if (t == R.cmd_done) return;
    bool ok = true;
    switch (R.cmd) {
    case EMU_CMD_SAVE:   ok = save_state(R.cmd_arg); break;
    case EMU_CMD_LOAD:   ok = load_state(R.cmd_arg); break;
    case EMU_CMD_RESET:  R.core->reset(); break;
    case EMU_CMD_OPTION: if (R.core->set_option) R.core->set_option(R.cmd_key, R.cmd_arg); break;
    case EMU_CMD_FLUSH:  ok = R.core->sram_flush ? R.core->sram_flush() : true; break;
    default: break;
    }
    R.cmd_ok = ok;
    __atomic_store_n(&R.cmd_done, t, __ATOMIC_RELEASE);
}

static void apply_options(void)
{
    if (!R.core->set_option) return;
    for (int i = 0; i < R.core->n_options; i++) {
        const emu_option_t *o = &R.core->options[i];
        R.core->set_option(o->key, emu_core_pref(R.core, o->key, o->def));
    }
}

static void stats_roll(uint32_t now)
{
    uint32_t dt = now - R.period_ms;
    if (dt < STATS_MS) return;
    emu_run_stats_t s = { 0 };
    s.fps_emu = R.n_frames * 1000.0f / dt;
    s.fps_drawn = R.n_drawn * 1000.0f / dt;
    s.run_ms = R.n_frames ? (float)(R.run_us / R.n_frames) / 1000.0f : 0;
    s.run_ms_max = R.run_us_max / 1000.0f;
    s.skipped = R.n_skipped;
    s.underruns = R.n_under;
    R.stats = s;
    R.period_ms = now;
    R.n_frames = R.n_drawn = R.n_skipped = R.n_under = 0;
    R.run_us = 0;
    R.run_us_max = 0;
}

static void worker(void *arg)
{
    (void)arg;
    char rom[EMU_PATH_MAX], save[EMU_PATH_MAX], dir[EMU_PATH_MAX], bios[EMU_PATH_MAX];
    emu_game_rom(&R.game, rom, sizeof rom);
    emu_game_save(&R.game, save, sizeof save);
    emu_console_dir(R.core, dir, sizeof dir);
    snprintf(bios, sizeof bios, "%.300s/bios", dir);

    emu_load_t req = { .rom = rom, .bios_dir = bios, .save = save };
    uint64_t t0 = aos_hal_uptime_us();
    char err[160] = "";
    if (!R.core->load(&R.host, &req, err, sizeof err)) {
        snprintf(R.err, sizeof R.err, "%s", err[0] ? _(err) : _("No se pudo cargar el juego"));
        aos_hal_log(EMU_TAG, "load failed: %s (%s)", err, rom);
        R.state = EMU_RUN_ERROR;
        return;
    }
    apply_options();
    R.rate = R.core->sample_rate();
    float fps = R.core->fps();
    if (fps < 20 || fps > 120) fps = 60;
    R.fps = fps;
    aos_hal_log(EMU_TAG, "%s loaded in %u ms: %.2f fps, sound %d Hz", R.game.title,
                (unsigned)((aos_hal_uptime_us() - t0) / 1000), (double)fps, R.rate);

    /* the speaker, at the core's rate (the board's mixer resamples) */
    R.mono_n = 1024;
    R.mono = emu_psram(R.mono_n * sizeof(int16_t));
    R.spk_seen = false;
    R.spk = R.mono && R.rate > 0 && aos_hal_spk_open((uint32_t)R.rate);
    if (!R.spk) aos_hal_log(EMU_TAG, "no speaker: playing silent");

    if (emu_pref("resume", 1)) {
        char st[EMU_PATH_MAX];
        emu_game_state(&R.game, 0, st, sizeof st);
        if (emu_file_exists(st, NULL)) load_state(0);
    }
    R.state = EMU_RUN_RUNNING;

    const uint32_t period_us = (uint32_t)(1000000.0f / fps);
    const int pace_samples = (int)(R.rate * PACE_FRAMES / fps);
    uint64_t next = aos_hal_uptime_us();
    int skipped_row = 0;
    bool draw_next = true;
    uint32_t turbo_n = 0;
    uint32_t last_yield = (uint32_t)aos_hal_uptime_ms();
    uint32_t last_flush = last_yield;
    R.period_ms = last_yield;

    while (!aos_hal_worker_should_stop()) {
        do_command();
        uint32_t now_ms = (uint32_t)aos_hal_uptime_ms();
        /* a game saved in its battery RAM survives the board losing power
         * without the app being closed */
        if (now_ms - last_flush > FLUSH_MS) {
            last_flush = now_ms;
            if (R.core->sram_flush) R.core->sram_flush();
        }
        if (R.paused) {
            aos_hal_worker_sleep(10);
            next = aos_hal_uptime_us();
            R.period_ms = now_ms;
            continue;
        }

        uint32_t pads[EMU_MAX_PLAYERS];
        for (int i = 0; i < EMU_MAX_PLAYERS; i++) pads[i] = R.pads[i];

        bool turbo = R.turbo;
        bool draw = draw_next;
        if (turbo) draw = (++turbo_n % TURBO_DRAW) == 0;

        uint64_t t = aos_hal_uptime_us();
        R.core->run(pads, draw);
        uint32_t took = (uint32_t)(aos_hal_uptime_us() - t);
        R.run_us += took;
        if (took > R.run_us_max) R.run_us_max = took;
        R.n_frames++;
        if (!draw) R.n_skipped++;

        if (turbo) {
            next = aos_hal_uptime_us();
            draw_next = true;
            /* core 0's idle task is watched: one short sleep a second */
            if (now_ms - last_yield > 1000) {
                last_yield = now_ms;
                aos_hal_worker_sleep(1);
            }
            stats_roll(now_ms);
            continue;
        }

        /* the clock: the microsecond clock, nudged by the speaker's ring.
         * A ring fuller than PACE_FRAMES frames means the console runs
         * ahead of the sound: the deadline moves later. One nearly dry
         * means it runs behind: earlier. So the two never drift, and a
         * speaker that drains nothing (the simulator's) leaves the plain
         * clock. */
        next += period_us;
        bool spk_on = R.spk && R.sound;
        if (spk_on) {
            int q = aos_hal_spk_queued();
            if (q > 0) R.spk_seen = true;
            if (R.spk_seen) {
                if (q == 0) R.n_under++;
                if (q > pace_samples * 2) next += period_us / 8;
                else if (q < pace_samples / 2) next -= period_us / 8;
            }
        }
        {
            uint64_t n = aos_hal_uptime_us();
            if (n < next) {
                uint32_t wait = (uint32_t)(next - n) / 1000;
                if (wait) aos_hal_worker_sleep(wait);
                last_yield = now_ms;
            }
        }

        /* behind: draw less, never slow the game down */
        uint64_t n = aos_hal_uptime_us();
        int64_t late = (int64_t)(n - next);
        int mode = R.skip_mode;
        if (mode == EMU_SKIP_AUTO) {
            bool behind = late > (int64_t)period_us ||
                          (spk_on && R.spk_seen && aos_hal_spk_queued() < pace_samples / 3);
            if (behind && skipped_row < SKIP_MAX) {
                draw_next = false;
                skipped_row++;
            } else {
                draw_next = true;
                skipped_row = 0;
            }
        } else {
            int every = mode - EMU_SKIP_0;     /* 0, 1 or 2 undrawn between drawn */
            if (every > 0 && skipped_row < every) {
                draw_next = false;
                skipped_row++;
            } else {
                draw_next = true;
                skipped_row = 0;
            }
        }
        if (late > 8 * (int64_t)period_us) next = n;    /* hopelessly behind: start over */

        if (now_ms - last_yield > 1000) {
            last_yield = now_ms;
            aos_hal_worker_sleep(1);
        }
        stats_roll(now_ms);
    }

    /* leaving: the automatic state and the battery save, then everything */
    if (R.stop_save_auto) save_state(0);
    if (R.core->sram_flush && !R.core->sram_flush()) aos_hal_log(EMU_TAG, "battery save NOT written");
    R.core->unload();
    if (R.spk) aos_hal_spk_close();
    R.spk = false;
    free(R.mono);
    R.mono = NULL;
    R.state = EMU_RUN_IDLE;
}

/* ---------------------------------------------------------------------------
 * From the screen
 * ------------------------------------------------------------------------ */

bool emu_run_start(const emu_core_t *core, const emu_game_t *game)
{
    if (R.state != EMU_RUN_IDLE && R.state != EMU_RUN_ERROR) return false;
    aos_hal_worker_stop();
    slots_free();
    memset(&R.stats, 0, sizeof R.stats);
    R.core = core;
    R.game = *game;
    R.err[0] = 0;
    R.paused = false;
    R.turbo = false;
    R.cmd_ticket = R.cmd_done = 0;
    for (int i = 0; i < EMU_MAX_PLAYERS; i++) R.pads[i] = 0;
    R.max_w = core->max_w;
    R.max_h = core->max_h;
    R.stop_save_auto = false;
    R.n_frames = R.n_drawn = R.n_skipped = R.n_under = 0;
    R.run_us = 0;
    R.run_us_max = 0;

    R.host = (emu_host_t){
        .video = host_video,
        .audio = host_audio,
        .log = host_log,
        .exec_alloc = emu_pref("jit", 1) ? EXEC_ALLOC : NULL,
        .exec_free = emu_pref("jit", 1) ? EXEC_FREE : NULL,
        .exec_publish = emu_pref("jit", 1) ? EXEC_PUBLISH : NULL,
    };
    if (!slots_alloc()) {
        snprintf(R.err, sizeof R.err, "%s", _("Falta memoria"));
        R.state = EMU_RUN_ERROR;
        return false;
    }
    R.state = EMU_RUN_LOADING;
    if (!aos_hal_worker_start_on("emulador", worker, NULL, STACK_BYTES, 0, 3)) {
        snprintf(R.err, sizeof R.err, "%s", _("Otra app está usando el procesador"));
        R.state = EMU_RUN_ERROR;
        slots_free();
        return false;
    }
    return true;
}

void emu_run_stop(bool save_auto)
{
    if (!R.core) return;
    R.stop_save_auto = save_auto && R.state == EMU_RUN_RUNNING;
    R.paused = false;
    aos_hal_worker_stop();
    if (R.state == EMU_RUN_ERROR) {
        /* load() failed: it freed what it took; nothing ran */
    }
    R.state = EMU_RUN_IDLE;
    slots_free();
    R.core = NULL;
}

emu_run_state_t emu_run_state(void) { return R.state; }
float emu_run_fps(void) { return R.fps; }
const char *emu_run_error(void) { return R.err; }

void emu_run_pause(bool paused) { R.paused = paused; }
bool emu_run_paused(void) { return R.paused; }
void emu_run_turbo(bool on) { R.turbo = on; }
void emu_run_sound(bool on) { R.sound = on; }
void emu_run_skip(int mode) { R.skip_mode = mode; }

void emu_run_pads(const uint32_t pads[EMU_MAX_PLAYERS])
{
    for (int i = 0; i < EMU_MAX_PLAYERS; i++) R.pads[i] = pads[i];
}

uint32_t emu_run_command(emu_cmd_t cmd, int arg, const char *key)
{
    /* one at a time: the screen waits for each before posting the next */
    R.cmd = cmd;
    R.cmd_arg = arg;
    snprintf(R.cmd_key, sizeof R.cmd_key, "%s", key ? key : "");
    uint32_t t = R.cmd_ticket + 1;
    __atomic_store_n(&R.cmd_ticket, t, __ATOMIC_RELEASE);
    return t;
}

bool emu_run_done(uint32_t ticket, bool *ok)
{
    if (R.state != EMU_RUN_RUNNING) {
        if (ok) *ok = false;
        return true;
    }
    if ((int32_t)(R.cmd_done - ticket) < 0) return false;
    if (ok) *ok = R.cmd_ok;
    return true;
}

void emu_run_stats(emu_run_stats_t *out)
{
    *out = R.stats;
}
