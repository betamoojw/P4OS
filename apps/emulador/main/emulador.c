/*
 * P4OS - Emulador: consoles of other times, from the card.
 *
 * The library of games (em_lib.c) and, once one is chosen, the console
 * running it (em_game.c, em_run.c) with its pad on the screen, a USB
 * gamepad per player, save states and a pause menu (em_menu.c). Each
 * console is a core behind emu_core.h: the NES (nofrendo) and the Game Boy
 * Advance (gpSP) for now, in cores/.
 *
 * Upright and lying down, both: the screen lays itself out again when the
 * board turns, without stopping the game.
 *
 * The games, saves and states live on the card under /sdcard/emulador, a
 * folder per console (em_files.c); the app's page in the portal uploads
 * games there.
 */
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "lvgl.h"

#include "emu.h"

#include <stdio.h>
#include <stdlib.h>

#define TICK_MS 8

static void tick_cb(lv_timer_t *t)
{
    emu_app_t *a = lv_timer_get_user_data(t);
    if (emu_game_is_open(a)) emu_game_tick(a);
    else emu_lib_tick(a);
}

static void *emu_create(aos_app_t *self, lv_obj_t *root)
{
    emu_app_t *a = lv_malloc_zeroed(sizeof *a);
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    uint32_t hi = 0, hp = 0;
    aos_hal_heap_info(&hi, &hp);
    int n = 0;
    while (emu_cores[n]) n++;
    aos_hal_log(EMU_TAG, "opening: %d consoles | internal %u B, psram %u B", n, (unsigned)hi, (unsigned)hp);

    lv_obj_set_style_bg_color(root, lv_color_hex(EMU_C_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    emu_folders_make();
    emu_lib_show(a);
    a->timer = lv_timer_create(tick_cb, TICK_MS, a);
    return a;
}

static void emu_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    emu_app_t *a = inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    a->timer = NULL;
    emu_game_destroy(a);
    if (a->M) lv_free(a->M);
    a->M = NULL;
    emu_lib_destroy(a);
    lv_obj_clean(a->root);
    lv_free(a);
}

static bool emu_back(aos_app_t *self, void *inst)
{
    (void)self;
    emu_app_t *a = inst;
    return a && emu_game_back(a);
}

static bool emu_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)root;
    emu_app_t *a = inst;
    if (!a) return false;
    lv_obj_update_layout(a->root);
    if (emu_game_is_open(a)) emu_game_resize(a);
    else emu_lib_resize(a);
    return true;
}

/* The launcher icon: an arcade cabinet, its marquee lit, a joystick and
 * two buttons on the panel. */
static const uint8_t EMU_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 2, 46, 68, 7, AIC_C_LIT(0x14111E), 255),          /* the cabinet   */
    AIC_INTO,
    AIC_RECT(AIC_TOP_MID, 0, 4, 38, 11, 3, AIC_C_LIT(0xFFD60A), 255),         /* the marquee   */
    AIC_GRAD(AIC_C_LIT(0xFF8A00), AIC_GRAD_HOR),
    AIC_RECT(AIC_TOP_MID, 0, 19, 36, 25, 4, AIC_C_LIT(0x40C8E0), 255),        /* the screen    */
    AIC_GRAD(AIC_C_LIT(0x7B5CFF), AIC_GRAD_VER),
    AIC_INTO,
    AIC_RECT(AIC_CENTER, -7, -2, 6, 6, 1, AIC_C_LIT(0xFFFFFF), 255),          /* a sprite      */
    AIC_RECT(AIC_CENTER,  7, -2, 6, 6, 1, AIC_C_LIT(0xFFFFFF), 255),
    AIC_RECT(AIC_CENTER,  0,  6, 16, 4, 1, AIC_C_LIT(0xFFFFFF), 200),
    AIC_OUT,
    AIC_RECT(AIC_TOP_MID, 0, 47, 42, 11, 2, AIC_C_LIT(0x2C2C40), 255),        /* the panel     */
    AIC_RECT(AIC_TOP_LEFT, 9, 44, 4, 8, 1, AIC_C_LIT(0x9090A8), 255),         /* stick         */
    AIC_RECT(AIC_TOP_LEFT, 7, 41, 8, 8, AIC_CIRCLE, AIC_C_LIT(0xFF453A), 255),/* ball          */
    AIC_RECT(AIC_TOP_LEFT, 25, 50, 6, 6, AIC_CIRCLE, AIC_C_LIT(0xFF2D95), 255),/* buttons      */
    AIC_RECT(AIC_TOP_LEFT, 33, 50, 6, 6, AIC_CIRCLE, AIC_C_LIT(0x30D158), 255),
    AIC_OUT,
    AIC_END
};

static bool emu_init(aos_app_t *app)
{
    app->desc.id       = "demo.emulador";
    app->desc.name     = "Emulador";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, EMU_ICON, sizeof EMU_ICON);
    app->desc.color_a  = 0xFF2D95;
    app->desc.color_b  = 0x5B2EFF;
    app->desc.order    = 164;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN |
                         AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;

    app->create  = emu_create;
    app->destroy = emu_destroy;
    app->back    = emu_back;
    app->resize  = emu_resize;
    return true;
}

AOS_APP_ENTRY(emu_init);
