/*
 * P4OS - 3D printers: the editor of a machine, and the search on the LAN.
 *
 * The fields follow the kind: each driver says which it uses (p3d_driver_t's
 * fields), what its key is called and where the printer shows it. The
 * search asks mDNS for the services printers announce (OctoPrint's own,
 * Moonraker's with [zeroconf], and plain HTTP ones whose name gives them
 * away); a tap on one fills the form. Bambu and Elegoo announce themselves
 * by SSDP and UDP broadcast, which an app cannot listen to: those are typed.
 */
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_text_safe.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "p3d_app.h"

#include <stdio.h>
#include <stdlib.h>

#define A p3d_app
#define FOUND_MAX  16
#define ARM_MS     3000

typedef struct {
    char name[64];
    char host[64];
    int  port;
    int  kind;
} found_t;

static struct {
    int        index;           /* -1: a new one */
    p3d_cfg_t  m;
    lv_obj_t  *page, *form, *kb;
    lv_obj_t  *name, *host, *port, *key, *user, *serial, *camera, *tls_sw;
    lv_obj_t  *found_box, *del_btn;
    uint64_t   del_armed;
    bool       searching;
} E;

/* the search's thread writes these; the UI reads them once it is done */
static found_t        s_found[FOUND_MAX];
static volatile int   s_nfound;
static volatile bool  s_scan_done;
static volatile int   s_scan_threads;

int p3d_edit_threads(void)
{
    return s_scan_threads;
}

/* ---- the search ------------------------------------------------------------------------------- */

static int guess_kind(const aos_mdns_svc_t *s)
{
    char low[240];
    snprintf(low, sizeof low, "%.63s %.63s %.95s", s->instance, s->host, s->txt);
    for (char *p = low; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    if (!strcmp(s->type, "_octoprint._tcp") || strstr(low, "octoprint") || strstr(low, "octopi")) return P3D_OCTOPRINT;
    if (!strcmp(s->type, "_moonraker._tcp") || strstr(low, "mainsail") || strstr(low, "fluidd") || strstr(low, "klipper") ||
        strstr(low, "moonraker")) {
        return P3D_MOONRAKER;
    }
    if (strstr(low, "prusa") || strstr(low, "mk4") || strstr(low, "core one")) return P3D_PRUSALINK;
    if (strstr(low, "duet")) return P3D_DUET;
    if (strstr(low, "flashforge") || strstr(low, "adventurer")) return P3D_FLASHFORGE;
    if (strstr(low, "creality") || strstr(low, "k1")) return P3D_CREALITY;
    return -1;
}

static void scan_thread(void *arg)
{
    (void)arg;
    static const char *const TYPES[] = { "_octoprint._tcp", "_moonraker._tcp", "_prusa-link._tcp", "_http._tcp" };
    aos_mdns_svc_t *svc = p3d_psram(sizeof(aos_mdns_svc_t) * 48);
    int n = svc ? aos_hal_mdns_browse(TYPES, 4, 3000, svc, 48) : -1;
    int k = 0;
    for (int i = 0; i < n && k < FOUND_MAX; i++) {
        int kind = guess_kind(&svc[i]);
        if (kind < 0 || !svc[i].ip) continue;
        found_t *f = &s_found[k];
        p3d_copy(f->name, sizeof f->name, svc[i].instance);
        uint32_t ip = svc[i].ip;
        snprintf(f->host, sizeof f->host, "%u.%u.%u.%u", (unsigned)(ip >> 24) & 255, (unsigned)(ip >> 16) & 255,
                 (unsigned)(ip >> 8) & 255, (unsigned)ip & 255);
        f->port = svc[i].port;
        f->kind = kind;
        bool dup = false;
        for (int j = 0; j < k; j++) dup |= !strcmp(s_found[j].host, f->host) && s_found[j].kind == f->kind;
        if (!dup) k++;
    }
    free(svc);
    s_nfound = n < 0 ? -1 : k;
    __sync_synchronize();
    s_scan_done = true;
    __sync_fetch_and_sub(&s_scan_threads, 1);
}

static void found_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nfound) return;
    p3d_edit_keep();
    const found_t *f = &s_found[i];
    E.m.kind = f->kind;
    p3d_copy(E.m.host, sizeof E.m.host, f->host);
    const p3d_driver_t *d = p3d_driver(f->kind);
    E.m.port = f->port == d->port ? 0 : f->port;
    if (!E.m.name[0]) p3d_copy(E.m.name, sizeof E.m.name, f->name);
    if (E.m.kind == P3D_PRUSALINK && !E.m.user[0]) p3d_copy(E.m.user, sizeof E.m.user, "maker");
    s_nfound = 0;
    p3d_build();
}

static void found_show(void)
{
    if (!E.found_box) return;
    lv_obj_clean(E.found_box);
    if (E.searching) {
        aos_make_decorative(aos_label(E.found_box, _("Buscando en la red..."), aos_font_small, AOS_C_DIM));
        return;
    }
    if (s_nfound < 0) {
        aos_make_decorative(aos_label(E.found_box, _("No hay red"), aos_font_small, AOS_C_ORANGE));
        return;
    }
    for (int i = 0; i < s_nfound; i++) {
        char b[160], safe[160];
        snprintf(b, sizeof b, "%.60s  ·  %s  ·  %.60s:%d", s_found[i].name, p3d_driver(s_found[i].kind)->label,
                 s_found[i].host, s_found[i].port);
        aos_text_safe(safe, sizeof safe, b);
        p3d_chip(E.found_box, safe, false, found_cb, (void *)(intptr_t)i);
    }
}

static void search_cb(lv_event_t *e)
{
    (void)e;
    if (E.searching) return;
    if (aos_hal_net_state() != AOS_NET_CONNECTED) {
        aos_ui_toast(_("No hay red"), 1500);
        return;
    }
    s_scan_done = false;
    s_nfound = 0;
    __sync_fetch_and_add(&s_scan_threads, 1);
    if (!aos_hal_thread_start("p3dscan", scan_thread, NULL, 8 * 1024, 3)) {
        __sync_fetch_and_sub(&s_scan_threads, 1);
        return;
    }
    E.searching = true;
    found_show();
}

/* ---- the form ------------------------------------------------------------------------------------ */

static void kb_show(lv_obj_t *ta)
{
    if (!E.kb) return;
    if (ta) {
        lv_keyboard_set_textarea(E.kb, ta);
        lv_keyboard_set_mode(E.kb, ta == E.port ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_remove_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(E.form, A.H - lv_obj_get_height(E.kb));
        lv_obj_scroll_to_view(ta, LV_ANIM_ON);
    } else {
        lv_obj_add_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(E.form, A.H);
    }
}

static void ta_focus_cb(lv_event_t *e)
{
    kb_show(lv_event_get_target(e));
}

static void kb_cb(lv_event_t *e)
{
    (void)e;
    kb_show(NULL);
}

static lv_obj_t *label(lv_obj_t *form, const char *t, lv_color_t c)
{
    lv_obj_t *l = aos_label(form, t, aos_font_caption, c);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    aos_make_decorative(l);
    return l;
}

static lv_obj_t *field(lv_obj_t *form, const char *name, const char *value, int max)
{
    lv_obj_t *l = label(form, name, AOS_C_DIM);
    lv_obj_set_style_margin_top(l, 8, 0);
    lv_obj_t *ta = lv_textarea_create(form);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (uint32_t)max);
    lv_textarea_set_text(ta, value);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_style_text_font(ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 20, 0);
    lv_obj_set_style_pad_hor(ta, 24, 0);
    lv_obj_set_style_pad_ver(ta, 16, 0);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void get_text(lv_obj_t *ta, char *out, size_t n)
{
    if (!ta) return;
    const char *t = lv_textarea_get_text(ta);
    while (*t == ' ') t++;
    p3d_copy(out, n, t);
    for (char *p = out + strlen(out); p > out && p[-1] == ' ';) *--p = '\0';
}

void p3d_edit_keep(void)
{
    if (!E.page) return;
    get_text(E.name, E.m.name, sizeof E.m.name);
    get_text(E.host, E.m.host, sizeof E.m.host);
    get_text(E.key, E.m.key, sizeof E.m.key);
    get_text(E.user, E.m.user, sizeof E.m.user);
    get_text(E.serial, E.m.serial, sizeof E.m.serial);
    get_text(E.camera, E.m.camera, sizeof E.m.camera);
    if (E.port) E.m.port = atoi(lv_textarea_get_text(E.port));
    if (E.tls_sw) E.m.tls = lv_obj_has_state(E.tls_sw, LV_STATE_CHECKED);
}

static void kind_cb(lv_event_t *e)
{
    p3d_edit_keep();
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k == E.m.kind) return;
    E.m.kind = k;
    E.m.port = 0;
    if (k == P3D_PRUSALINK && !E.m.user[0]) p3d_copy(E.m.user, sizeof E.m.user, "maker");
    p3d_build();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    if (E.index >= 0) p3d_go_detail(E.index);
    else p3d_go_list();
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    p3d_edit_keep();
    p3d_cfg_t m = E.m;
    const p3d_driver_t *d = p3d_driver(m.kind);
    for (char *p = m.name; *p; p++) if (*p == '[' || *p == ']') *p = ' ';
    /* "http://host:port/..." pasted whole */
    char *h = m.host;
    if (!strncmp(h, "https://", 8)) { memmove(h, h + 8, strlen(h + 8) + 1); m.tls = true; }
    else if (!strncmp(h, "http://", 7)) memmove(h, h + 7, strlen(h + 7) + 1);
    char *sl = strchr(h, '/');
    if (sl) *sl = '\0';
    char *colon = strchr(h, ':');
    if (colon && !strchr(colon + 1, ':')) {
        m.port = atoi(colon + 1);
        *colon = '\0';
    }
    if (!m.host[0]) {
        aos_ui_toast(_("Falta la dirección"), 2000);
        return;
    }
    if ((d->fields & P3D_F_KEY_NEEDED) && !m.key[0]) {
        char b[96];
        snprintf(b, sizeof b, _("Falta: %s"), _(d->key_label));
        aos_ui_toast(b, 2200);
        return;
    }
    if ((d->fields & P3D_F_SERIAL_NEEDED) && !m.serial[0]) {
        aos_ui_toast(_("Falta el número de serie"), 2200);
        return;
    }
    if (m.port < 0 || m.port > 65535 || m.port == (m.tls ? d->tls_port : d->port)) m.port = 0;
    if (!m.name[0]) p3d_copy(m.name, sizeof m.name, m.host);
    if (!(d->fields & P3D_F_KEY)) m.key[0] = '\0';
    if (!(d->fields & P3D_F_USER)) m.user[0] = '\0';
    if (!(d->fields & P3D_F_SERIAL)) m.serial[0] = '\0';
    if (!(d->fields & P3D_F_TLS)) m.tls = false;
    int at = E.index;
    if (at < 0) {
        if (A.cfg.count >= P3D_MAX) return;
        at = A.cfg.count++;
    }
    A.cfg.m[at] = m;
    if (!p3d_cfg_commit()) {
        aos_ui_toast(_("No se pudo guardar en la tarjeta"), 2500);
        return;
    }
    aos_ui_toast(_("Guardada"), 1200);
    E.page = NULL;
    p3d_go_detail(at);
}

static void del_cb(lv_event_t *e)
{
    (void)e;
    uint64_t now = aos_hal_uptime_ms();
    if (now - E.del_armed > ARM_MS) {
        E.del_armed = now;
        lv_obj_t *l = lv_obj_get_child(E.del_btn, 1);
        if (l) lv_label_set_text(l, _("Tocá otra vez para borrar"));
        return;
    }
    for (int i = E.index; i < A.cfg.count - 1; i++) A.cfg.m[i] = A.cfg.m[i + 1];
    A.cfg.count--;
    p3d_cfg_commit();
    E.page = NULL;
    p3d_go_list();
}

void p3d_edit_open(int i)
{
    memset(&E, 0, sizeof E);
    E.index = i;
    if (i >= 0 && i < A.cfg.count) E.m = A.cfg.m[i];
    else {
        E.index = -1;
        p3d_cfg_defaults(&E.m, P3D_OCTOPRINT);
    }
    s_nfound = 0;
}

void p3d_edit_build(void)
{
    E.page = p3d_box(A.root, A.W, A.H);
    lv_obj_set_style_bg_color(E.page, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(E.page, LV_OPA_COVER, 0);
    lv_obj_t *form = p3d_box(E.page, A.W, A.H);
    E.form = form;
    lv_obj_add_flag(form, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(form, LV_DIR_VER);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    int32_t side = A.land ? A.W / 6 : AOS_UI_PAD;
    lv_obj_set_style_pad_hor(form, side, 0);
    lv_obj_set_style_pad_top(form, 120, 0);
    lv_obj_set_style_pad_bottom(form, 60, 0);
    lv_obj_set_style_pad_row(form, 10, 0);

    lv_obj_t *bk = p3d_round_btn(E.page, AOS_SYM_CHEVRON_LEFT, back_cb, NULL);
    lv_obj_align(bk, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 14);
    lv_obj_t *t = aos_label(E.page, E.index < 0 ? _("Nueva impresora") : _("Impresora"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, AOS_UI_PAD + AOS_UI_TAP_MIN + 20, 28);
    aos_make_decorative(t);

    const p3d_driver_t *d = p3d_driver(E.m.kind);
    label(form, _("Qué es"), AOS_C_DIM);
    lv_obj_t *kr = p3d_box(form, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(kr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(kr, 12, 0);
    for (int k = 0; k < P3D_KIND_N; k++) p3d_chip(kr, p3d_driver(k)->label, k == E.m.kind, kind_cb, (void *)(intptr_t)k);
    label(form, _(d->makers), lv_color_hex(0xD1D1D6));

    lv_obj_t *sr = p3d_box(form, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(sr, 12, 0);
    lv_obj_set_style_margin_top(sr, 8, 0);
    p3d_btn(sr, AOS_SYM_MAGNIFY, _("Buscar en la red"), AOS_C_CARD2, search_cb, NULL);
    E.found_box = p3d_box(form, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(E.found_box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(E.found_box, 10, 0);
    found_show();

    char port[12] = "";
    if (E.m.port > 0) snprintf(port, sizeof port, "%d", E.m.port);
    E.name = field(form, _("Nombre"), E.m.name, sizeof E.m.name - 1);
    lv_textarea_set_placeholder_text(E.name, "Prusa 1");
    E.host = field(form, _("Dirección (IP o nombre)"), E.m.host, sizeof E.m.host - 1);
    lv_textarea_set_placeholder_text(E.host, "192.168.0.50");
    char ph[16];
    snprintf(ph, sizeof ph, "%d", E.m.tls ? d->tls_port : d->port);
    E.port = field(form, _("Puerto (vacío: el de siempre)"), port, 5);
    lv_textarea_set_placeholder_text(E.port, ph);
    lv_textarea_set_accepted_chars(E.port, "0123456789");
    E.key = E.user = E.serial = NULL;
    E.tls_sw = NULL;
    if (d->fields & P3D_F_USER) {
        E.user = field(form, _("Usuario"), E.m.user, sizeof E.m.user - 1);
    }
    if (d->fields & P3D_F_KEY) {
        E.key = field(form, _(d->key_label), E.m.key, sizeof E.m.key - 1);
        if (E.m.kind != P3D_OCTOPRINT && E.m.kind != P3D_MOONRAKER) lv_textarea_set_password_mode(E.key, true);
        if (d->key_hint) label(form, _(d->key_hint), AOS_C_DIM);
    }
    if (d->fields & P3D_F_SERIAL) {
        E.serial = field(form, E.m.kind == P3D_ELEGOO ? _("Número de la placa (MainboardID, opcional)") : _("Número de serie"),
                         E.m.serial, sizeof E.m.serial - 1);
        if (E.m.kind == P3D_BAMBU) label(form, _("En la impresora: Ajustes, Dispositivo; o en Bambu Studio"), AOS_C_DIM);
        if (E.m.kind == P3D_FLASHFORGE) label(form, _("En la impresora: Ajustes, Acerca de"), AOS_C_DIM);
    }
    if (d->fields & P3D_F_TLS) {
        lv_obj_t *vr = p3d_box(form, lv_pct(100), 80);
        lv_obj_t *l = aos_label(vr, _("HTTPS (con certificado válido)"), aos_font_body, AOS_C_TEXT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, A.W - 2 * side - 120);
        aos_make_decorative(l);
        E.tls_sw = lv_switch_create(vr);
        lv_obj_set_size(E.tls_sw, 96, 54);
        lv_obj_align(E.tls_sw, LV_ALIGN_RIGHT_MID, 0, 0);
        if (E.m.tls) lv_obj_add_state(E.tls_sw, LV_STATE_CHECKED);
    }
    E.camera = field(form, _("Cámara (vacío: la que diga la impresora; off: ninguna)"), E.m.camera, sizeof E.m.camera - 1);
    lv_textarea_set_placeholder_text(E.camera, "http://.../?action=snapshot");
    if (d->fields & P3D_F_KEY) {
        label(form, _("La clave queda guardada en la tarjeta (impresoras.txt) y se puede leer."), AOS_C_ORANGE);
    }

    lv_obj_t *btns = p3d_box(form, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(btns, 14, 0);
    lv_obj_set_style_margin_top(btns, 20, 0);
    p3d_btn(btns, AOS_SYM_CHECK, _("Guardar"), AOS_C_ACCENT, save_cb, NULL);
    E.del_btn = E.index >= 0 ? p3d_btn(btns, AOS_SYM_DELETE, _("Borrar"), lv_color_hex(0x9B1C1C), del_cb, NULL) : NULL;

    E.kb = lv_keyboard_create(E.page);
    aos_keyboard_style(E.kb, aos_font_body);
    lv_obj_set_size(E.kb, A.W, A.land ? A.H * 45 / 100 : A.H * 34 / 100);
    lv_obj_align(E.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(E.kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(E.kb, kb_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
}

void p3d_edit_tick(void)
{
    if (E.searching && s_scan_done) {
        E.searching = false;
        found_show();
        if (s_nfound == 0) aos_ui_toast(_("No se encontró ninguna (Bambu y Elegoo no se anuncian así)"), 3000);
    }
}

bool p3d_edit_back(void)
{
    if (E.kb && !lv_obj_has_flag(E.kb, LV_OBJ_FLAG_HIDDEN)) {
        kb_show(NULL);
        return true;
    }
    if (E.index >= 0) {
        p3d_go_detail(E.index);
        return true;
    }
    return false;
}

void p3d_edit_close(void)
{
    E.page = NULL;
    E.kb = NULL;
}
