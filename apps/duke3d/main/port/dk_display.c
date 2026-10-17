#include "dk_compat.h" /* P4OS: the C library as the board has it */
/*
 * P4OS - Duke Nukem 3D: the screen, the timer and the keys. Replaces
 * engine/display.c, Ryan C. Gordon's SDL 1.2 driver for the Build engine.
 *
 * The engine draws 8-bit colour into one buffer (frameplace, xdim x ydim)
 * that persists from frame to frame: its permanent sprites (the status bar)
 * are drawn into it once and stay. At each nextpage() the buffer goes through
 * a 256-entry palette of RGB565 (little-endian, LVGL's order) into one of
 * three slots, which the app blits from LVGL's task; the scaling into the
 * landscape screen is the PPA's. The worker never touches LVGL or the
 * framebuffer; the slots are the whole conversation, as in Doom's port:
 *
 *   FREE -> WRITING (game) -> READY -> SHOWN (app, blitting) -> FREE
 *
 * The game never waits for the app: a READY frame not taken yet is simply
 * replaced by a newer one.
 *
 * The timer is Build's own scheme (Ken's, kept by Chocolate): totalclock
 * counts 120 Hz tics, and sampletimer() brings it up to date from a
 * millisecond clock whenever the game asks. That clock is dk_ticks_ms(),
 * which stops while the app holds the game paused.
 *
 * The keys: the app's buttons reach the game two ways. While playing, as
 * the game's functions (port/dk_input.c answers control.c's ACTION()), so
 * the player's key config does not matter. In the menus, the y/n questions
 * and the screens between levels, as keyboard scancodes, the only thing
 * those read: they are queued by dk_input.c and fed to the game's
 * keyhandler() here, as the SDL driver fed SDL's key events.
 *
 * The 16-colour functions are the Build editor's 2D mode, which the game
 * never enters: kept as harmless stubs.
 */
#pragma GCC optimize("O2")

#include "platform.h"
#include "build.h"
#include "display.h"
#include "fixedPoint_math.h"
#include "engine.h"
#include "draw.h"
#include "cache.h"
#include "tiles.h"

#include "../duke_port.h"
#include "dk_port.h"
#include "dk_raw.h"
#include "aos_hal.h"

/* display.h used to define these in the header itself */
int    _argc;
char **_argv;

int32_t xres, yres, bytesperline, imageSize, maxpages;
uint8_t *frameplace;
uint8_t *frameoffset;
uint8_t *screen, vesachecked;
int32_t buffermode, origbuffermode, linearmode;
uint8_t permanentupdate = 0, vgacompatible;

int32_t total_render_time = 1;
int32_t total_rendered_frames = 0;

static uint8_t  *s_fb8;                 /* the engine's 8-bit frame */
static int       s_w, s_h;
static int32_t   s_last_render_ticks;
static uint8_t   s_pal6[768];           /* as the game set it, for VBE_getPalette */
uint8_t          lastPalette[768];      /* engine.c keeps a copy of the palette here */

void keyhandler(void);

/* --------------------------------------------------------------------------
 * The frame slots
 * -------------------------------------------------------------------------- */

enum { SLOT_FREE = 0, SLOT_WRITING, SLOT_READY, SLOT_SHOWN };

static uint16_t          *s_slot[DK_SLOTS];
static volatile int       s_state[DK_SLOTS];
static volatile uint32_t  s_seq[DK_SLOTS];
static volatile uint16_t  s_slot_w[DK_SLOTS], s_slot_h[DK_SLOTS];
static uint32_t           s_frame_seq;
static int                s_shown = -1;         /* app side */

static uint16_t          *s_pal;                /* 256 entries, internal RAM */
static uint32_t           s_last_yield;
static uint32_t           s_timer_calls;        /* sampletimer() since the last frame */

static bool cas(volatile int *p, int from, int to)
{
    int expected = from;
    return __atomic_compare_exchange_n(p, &expected, to, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

bool dk_video_alloc(void)
{
    for (int i = 0; i < DK_SLOTS; i++) {
        s_slot[i] = dk_raw_alloc((size_t)DK_MAX_W * DK_MAX_H * 2, false);
        if (!s_slot[i]) {
            dk_video_release();
            return false;
        }
        s_state[i] = SLOT_FREE;
        s_seq[i] = 0;
    }
    /* internal RAM: read for every pixel of every frame, 512 bytes */
    s_pal = dk_raw_alloc(256 * sizeof(uint16_t), true);
    if (!s_pal) {
        dk_video_release();
        return false;
    }
    memset(s_pal, 0, 256 * sizeof(uint16_t));
    s_frame_seq = 0;
    s_shown = -1;
    return true;
}

void dk_video_release(void)
{
    for (int i = 0; i < DK_SLOTS; i++) {
        dk_raw_free(s_slot[i]);
        s_slot[i] = NULL;
        s_state[i] = SLOT_FREE;
    }
    dk_raw_free(s_pal);
    s_pal = NULL;
}

const uint16_t *dk_frame_take(int *w, int *h)
{
    int best = -1;
    uint32_t bs = 0;
    for (int i = 0; i < DK_SLOTS; i++) {
        if (s_state[i] == SLOT_READY && (best < 0 || s_seq[i] > bs)) {
            best = i;
            bs = s_seq[i];
        }
    }
    if (best < 0 || !cas(&s_state[best], SLOT_READY, SLOT_SHOWN)) return NULL;
    s_shown = best;
    *w = s_slot_w[best];
    *h = s_slot_h[best];
    return s_slot[best];
}

void dk_frame_blitted(void)
{
    if (s_shown >= 0) {
        __atomic_store_n(&s_state[s_shown], SLOT_FREE, __ATOMIC_RELEASE);
        s_shown = -1;
    }
}

uint32_t dk_frames(void)
{
    return s_frame_seq;
}

static int grab_slot(void)
{
    for (;;) {
        for (int i = 0; i < DK_SLOTS; i++) {
            if (cas(&s_state[i], SLOT_FREE, SLOT_WRITING)) return i;
        }
        for (int i = 0; i < DK_SLOTS; i++) {
            if (cas(&s_state[i], SLOT_READY, SLOT_WRITING)) return i;
        }
        dk_poll_stop();
        aos_hal_worker_sleep(1);
    }
}

static void present(void)
{
    if (!s_fb8 || !s_pal || !s_slot[0]) return;
    int i = grab_slot();
    const uint16_t *pal = s_pal;
    const uint8_t *src = s_fb8;
    uint32_t *out = (uint32_t *)s_slot[i];
    /* two pixels per store; the slot is malloc's, so aligned, and every
     * mode's width is even */
    for (int n = s_w * s_h / 2; n; n--, src += 2) {
        *out++ = (uint32_t)pal[src[0]] | ((uint32_t)pal[src[1]] << 16);
    }
    s_slot_w[i] = (uint16_t)s_w;
    s_slot_h[i] = (uint16_t)s_h;
    s_seq[i] = ++s_frame_seq;
    __atomic_store_n(&s_state[i], SLOT_READY, __ATOMIC_RELEASE);

    /* core 0 also runs its idle task, which the task watchdog watches: a
     * render loop that never blocks starves it. One short sleep a second
     * costs nothing (measured by Turbo). */
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (now - s_last_yield > 1000) {
        s_last_yield = now;
        aos_hal_worker_sleep(1);
    }
}

/* --------------------------------------------------------------------------
 * Video modes
 * -------------------------------------------------------------------------- */

/* What the video menu offers; the app scales each to the landscape screen
 * keeping its shape. Measured in E1L1 (the game's whole frame on core 0:
 * logic, renderer, palette pass, sound): 320x200 16 ms, 480x300 29 ms,
 * 640x400 50 ms, 640x480 57 ms; at the two big ones the game uses all of
 * core 0 and runs at 17-21 fps. That used to flash the screen blue: the task
 * watchdog found core 0's idle task starved and printed from its interrupt,
 * which made the panel's per-frame restart late. The firmware stopped
 * watching core 0's idle task and widened the vertical blanking (8b3dde5,
 * fcde1e5, docs/MEMORY.md); with it every mode here plays clean. */
static const int MODES[][2] = {
    { 320, 200 }, { 400, 250 }, { 480, 300 }, { 640, 400 }, { 640, 480 },
};

void getvalidvesamodes(void)
{
    validmodecnt = 0;
    vidoption = 1;
    for (size_t i = 0; i < sizeof MODES / sizeof MODES[0]; i++) {
        validmode[validmodecnt] = validmodecnt;
        validmodexdim[validmodecnt] = MODES[i][0];
        validmodeydim[validmodecnt] = MODES[i][1];
        validmodecnt++;
    }
}

/* engine/display.c's init_new_res_vars(), without SDL's surface */
static void init_new_res_vars(int32_t davidoption)
{
    int i, j;

    xdim = xres = s_w;
    ydim = yres = s_h;
    printf("video mode %dx%d\n", xdim, ydim);

    bytesperline = s_w;
    vesachecked = 1;
    vgacompatible = 1;
    linearmode = 1;
    qsetmode = s_h;
    activepage = visualpage = 0;

    frameoffset = frameplace = s_fb8;

    if (screen != NULL) {
        screen = NULL;
    }

    (void)davidoption;
    j = ydim * 4 * sizeof(int32_t);     /* horizlookup and horizlookup2 */
    if (horizlookup) free(horizlookup);
    if (horizlookup2) free(horizlookup2);
    horizlookup = (int32_t *)malloc(j);
    horizlookup2 = (int32_t *)malloc(j);

    j = 0;
    for (i = 0; i <= ydim; i++) {
        ylookup[i] = j;
        j += bytesperline;
    }

    horizycent = ((ydim * 4) >> 1);

    /* drawrooms calls dosetaspect and works the rest out again */
    oxyaspect = oxdimen = oviewingrange = -1;

    setBytesPerLine(bytesperline);
    setview(0L, 0L, xdim - 1, ydim - 1);
    setbrightness(curbrightness, palette);

    if (searchx < 0) {
        searchx = halfxdimen;
        searchy = (ydimen >> 1);
    }
}

int _setgamemode(uint8_t davidoption, int32_t daxdim, int32_t daydim)
{
    getvalidvesamodes();
    bool valid = false;
    for (int i = 0; i < validmodecnt; i++) {
        if (validmodexdim[i] == daxdim && validmodeydim[i] == daydim) valid = true;
    }
    if (!valid) {
        printf("Resolution %dx%d unsupported. Changed to 320x200\n", daxdim, daydim);
        daxdim = 320;
        daydim = 200;
    }
    if (!s_fb8 || daxdim != s_w || daydim != s_h) {
        free(s_fb8);
        /* PSRAM: the renderer writes columns, strided; measured fast
         * enough, and 300 KB at 640x480 would not fit internal RAM */
        s_fb8 = (uint8_t *)malloc((size_t)daxdim * daydim);
        if (!s_fb8) {
            dk_fatal("no memory for the screen");
        }
        memset(s_fb8, 0, (size_t)daxdim * daydim);
        s_w = daxdim;
        s_h = daydim;
    }
    init_new_res_vars(davidoption);
    qsetmode = 200;
    s_last_render_ticks = getticks();
    return 0;
}

void setvmode(int mode)
{
    (void)mode;
}

void *get_framebuffer(void)
{
    return s_fb8;
}

void *_getVideoBase(void)
{
    return s_fb8;
}

void _platform_init(int argc, char **argv, const char *title, const char *iconName)
{
    (void)title;
    (void)iconName;
    _argc = argc;
    _argv = argv;
    srand(dk_ticks_ms());
}

void _uninitengine(void)
{
}

int screencapture(char *filename, uint8_t inverseit)
{
    (void)filename;
    (void)inverseit;
    return 0;
}

void fullscreen_toggle_and_change_driver(void)
{
}

/* --------------------------------------------------------------------------
 * The palette
 * -------------------------------------------------------------------------- */

/* Ken's format: 4 bytes per entry, blue, green, red (0..63), unused. */
int VBE_setPalette(uint8_t *palettebuffer)
{
    const uint8_t *p = palettebuffer;
    for (int i = 0; i < 256; i++, p += 4) {
        int b = p[0] & 63, g = p[1] & 63, r = p[2] & 63;
        s_pal6[i * 3] = (uint8_t)r;
        s_pal6[i * 3 + 1] = (uint8_t)g;
        s_pal6[i * 3 + 2] = (uint8_t)b;
        /* 6 bits to 5/6/5: red and blue drop their lowest bit, green is
         * exact */
        if (s_pal) s_pal[i] = (uint16_t)(((r >> 1) << 11) | (g << 5) | (b >> 1));
    }
    return 1;
}

int VBE_getPalette(int32_t start, int32_t num, uint8_t *palettebuffer)
{
    uint8_t *p = palettebuffer + start * 4;
    for (int i = start; i < start + num && i < 256; i++) {
        *p++ = s_pal6[i * 3 + 2];
        *p++ = s_pal6[i * 3 + 1];
        *p++ = s_pal6[i * 3];
        *p++ = 0;
    }
    return 1;
}

/* --------------------------------------------------------------------------
 * Frames
 * -------------------------------------------------------------------------- */

void _updateScreenRect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)x; (void)y; (void)w; (void)h;
}

void dk_running(void);

/* One frame per step of the game's logic. Duke moves the world 30 times a
 * second (TICRATE 120, TICSPERFRAME 4, duke3d.h) and draws without
 * interpolating, so any frame beyond that is the same picture again: the
 * engine measured 56 fps in E1L1 and 78 in the menu, half of them copies,
 * and each one a PPA pass and a whole core 0 for nothing. So a frame waits
 * here until the clock has moved a step since the last one, mixing sound
 * meanwhile, and at 320x200 core 0 rests half of the time. */
#define TICS_PER_FRAME  4

/* What a frame costs the game's core, the wait for the next step left out:
 * the game's logic, the renderer and the palette pass, from one frame's
 * end to the next one's present. Summed for the app's log line, which is
 * how much room the 30 fps leave. */
static volatile uint64_t s_busy_us;
static volatile uint32_t s_busy_n;
static uint64_t          s_frame_end_us;

void dk_frame_cost(uint64_t *us, uint32_t *n)
{
    *us = s_busy_us;
    *n = s_busy_n;
}

void _nextpage(void)
{
    static int32_t last_clock;
    uint64_t t0 = aos_hal_uptime_us();
    if (s_frame_end_us) {
        s_busy_us += t0 - s_frame_end_us;
        s_busy_n++;
    }
    dk_pause_wait();
    _handle_events();
    sampletimer();
    if (totalclock < last_clock) last_clock = totalclock;  /* the game reset it */
    while (totalclock - last_clock < TICS_PER_FRAME) {
        dk_sound_update();
        dk_poll_stop();
        aos_hal_worker_sleep(1);
        sampletimer();
    }
    last_clock = totalclock;
    uint64_t t1 = aos_hal_uptime_us();
    present();
    dk_frame_done();
    s_timer_calls = 0;
    s_frame_end_us = aos_hal_uptime_us();
    s_busy_us += s_frame_end_us - t1;           /* the palette pass counts */
    dk_sound_update();
    dk_running();

    uint32_t ticks = getticks();
    total_render_time = (int32_t)(ticks - s_last_render_ticks);
    if (total_render_time > 1000) {
        total_rendered_frames = 0;
        total_render_time = 1;
        s_last_render_ticks = ticks;
    }
    total_rendered_frames++;
}

uint8_t readpixel(uint8_t *offset)
{
    return *offset;
}

void drawpixel(uint8_t *location, uint8_t pixel)
{
    *location = pixel;
}

/* the editor's 2D mode */
void setcolor16(uint8_t col) { (void)col; }
void drawpixel16(int32_t offset) { (void)offset; }
void fillscreen16(int32_t offset, int32_t color, int32_t blocksize) { (void)offset; (void)color; (void)blocksize; }
void drawline16(int32_t XStart, int32_t YStart, int32_t XEnd, int32_t YEnd, uint8_t Color)
{
    (void)XStart; (void)YStart; (void)XEnd; (void)YEnd; (void)Color;
}
void clear2dscreen(void) {}

/* --------------------------------------------------------------------------
 * Events: the keys the pad queued, and the waits
 * -------------------------------------------------------------------------- */

static unsigned s_lastkey;

uint8_t _readlastkeyhit(void)
{
    return (uint8_t)s_lastkey;
}

/* A raw scancode as the keyboard sent it: 0xE0 first for the extended
 * ones, +0x80 on release. keyboard.c's keyhandler() reads it back. */
void dk_feed_scancode(unsigned code)
{
    s_lastkey = code;
    keyhandler();
}

void _handle_events(void)
{
    dk_poll_stop();
    dk_keys_pump();
}

void _idle(void)
{
    _handle_events();
    dk_sound_update();
    aos_hal_worker_sleep(1);
}

/* The mouse: never present. The stick reaches the game through control.c
 * (dk_analog), not as mouse motion: that would go through the player's
 * mouse sensitivity and the aiming mode. */
int setupmouse(void)
{
    moustat = 0;
    return 0;
}

void readmousexy(short *x, short *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
}

void readmousebstatus(short *bstatus)
{
    if (bstatus) *bstatus = 0;
}

/* The joystick: the USB gamepad is read by the app (aos_pad.h) and comes
 * in as the pad's buttons and stick, so the game sees none. */
void _joystick_init(void) {}
void _joystick_deinit(void) {}
int _joystick_update(void) { return 0; }
int _joystick_axis(int axis) { (void)axis; return 0; }
int _joystick_hat(int hat) { (void)hat; return -1; }
int _joystick_button(int button) { (void)button; return 0; }

void initkeys(void) {}
void uninitkeys(void) {}

/* --------------------------------------------------------------------------
 * The timer (Ken's sampletimer scheme, on dk_ticks_ms)
 * -------------------------------------------------------------------------- */

static int32_t timerfreq;
static int32_t timerlastsample;
static int     timerticspersec;
static void  (*usertimercallback)(void);

void (*installusertimercallback(void (*callback)(void)))(void)
{
    void (*old)(void) = usertimercallback;
    usertimercallback = callback;
    return old;
}

/* tics = ms * tps / 1000 in 32 bits: the product would overflow after
 * 2^31 / 120 ms, about 7 hours, so it is done in two parts */
static int32_t ms_to_tics(uint32_t ms)
{
    return (int32_t)((ms / 1000) * (uint32_t)timerticspersec +
                     (ms % 1000) * (uint32_t)timerticspersec / 1000);
}

int inittimer(int tickspersecond)
{
    if (timerfreq) return 0;
    timerfreq = 1000;
    timerticspersec = tickspersecond;
    timerlastsample = ms_to_tics(dk_ticks_ms());
    usertimercallback = NULL;
    return 0;
}

void uninittimer(void)
{
    timerfreq = 0;
    timerticspersec = 0;
}

/* sampletimer() is the body of the game's waits: while (totalclock < t)
 * getpackets(), while (Sound[x].lock >= 200) getpackets(). On DOS the timer
 * and the sound card moved on by interrupts meanwhile; here the game's own
 * task has to, and a loop that never blocks starves core 0 (the system's
 * hang watchdog sees it). So, called over and over without a frame, it mixes
 * the sound and sleeps a millisecond each time. Playing, it is called a
 * few times a frame. */
#define SPIN_CALLS  64

void sampletimer(void)
{
    if (!timerfreq) return;
    dk_pause_wait();
    if (++s_timer_calls > SPIN_CALLS) {
        dk_sound_update();
        aos_hal_worker_sleep(1);
    }
    int32_t n = ms_to_tics(dk_ticks_ms()) - timerlastsample;
    if (n > 0) {
        totalclock += n;
        timerlastsample += n;
    }
    if (usertimercallback) for (; n > 0; n--) usertimercallback();
}

uint32_t getticks(void)
{
    return dk_ticks_ms();
}

int gettimerfreq(void)
{
    return timerticspersec;
}

int TIMER_GetPlatformTicksInOneSecond(int64_t *t)
{
    *t = 1000;
    return 1;
}

void TIMER_GetPlatformTicks(int64_t *t)
{
    *t = dk_ticks_ms();
}
