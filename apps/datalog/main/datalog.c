/*
 * P4OS - Datalog (aos.datalog): a data logger for the header's sensors
 * (apps/datalog/README.md, and dl.h for the pieces).
 *
 * The app has AOS_APP_FLAG_BACKGROUND: closing it hides it and the engine
 * goes on reading; its tick runs five times a second while hidden, and is
 * what turns an alarm into a sound and a message over whatever app is in
 * front (on the display's top layer). While it logs it asks to start with
 * the board (aos_ui_boot_start_set), so a power cut or an update does not
 * stop a log; paused, it stops asking.
 *
 * Its portal page (web/datalog.js) reads the CSVs and datalog.txt from the
 * card, and the latest readings through the live channel: "state", a JSON
 * put every second while the page looks and every five otherwise. The page
 * sends "run=0|1", "csv=comma|semicolon" and "reload" (after it wrote
 * datalog.txt).
 */
#include "dl.h"
#include "aos_icon_ops.h"

#include <stdlib.h>
#include <sys/stat.h>

dl_ui_t U;

/* A sheet of paper with a line chart and the red dot of a recorder */
static const uint8_t DL_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -2, 4, 66, 56, 10, AIC_C_TEXT, 255),
    AIC_INTO,
    AIC_RECT(AIC_BOTTOM_MID, 0, -14, 52, 3, 2, AIC_C_LIT(0x99F6E4), 255),
    AIC_RECT(AIC_CENTER, -16, 6, 22, 6, 3, AIC_C_LIT(0x0F766E), 255), AIC_ROT(-480),
    AIC_RECT(AIC_CENTER, 0, 2, 18, 6, 3, AIC_C_LIT(0x0F766E), 255), AIC_ROT(370),
    AIC_RECT(AIC_CENTER, 17, -5, 26, 6, 3, AIC_C_LIT(0x0F766E), 255), AIC_ROT(-480),
    AIC_OUT,
    AIC_RECT(AIC_TOP_RIGHT, -12, 12, 22, 22, AIC_CIRCLE, AIC_C_LIT(0xFF453A), 255),
    AIC_BORDER(3, AIC_C_TEXT, 255),
    AIC_END
};

/* ---- views ---- */

void dl_bar_clean(void)
{
    lv_obj_clean(U.bar);
    lv_obj_clean(U.content);
    U.clock = U.date = U.status = U.play = NULL;
    memset(U.c_val, 0, sizeof U.c_val);
    memset(U.c_when, 0, sizeof U.c_when);
    memset(U.c_tag, 0, sizeof U.c_tag);
    memset(U.c_chart, 0, sizeof U.c_chart);
    U.last_sec = -1;
}

void dl_go(int view, int ch)
{
    dl_sheet_close();
    if (U.view == DL_V_DETAIL && view != DL_V_DETAIL) dl_view_leave();
    U.view = view;
    U.sel = ch;
    if (view == DL_V_DETAIL) dl_view_build(ch);
    else if (view == DL_V_EDIT) dl_edit_open(ch);
    else dl_list_build();
    U.seen_seq = dl_seq();
}

static void layout(lv_obj_t *root)
{
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    U.bar_h = U.land ? 104 : 124;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    U.bar = dl_box(root, U.W, U.bar_h);
    lv_obj_set_style_pad_hor(U.bar, AOS_UI_PAD, 0);
    U.content = dl_box(root, U.W, U.H - U.bar_h);
    lv_obj_set_pos(U.content, 0, U.bar_h);
    lv_obj_set_style_pad_hor(U.content, AOS_UI_PAD, 0);
    lv_obj_update_layout(root);
}

/* ---- alarms on screen ---- */

#define PEND_MAX 6

static struct {
    dl_alert_t a[PEND_MAX];
    bool cleared[PEND_MAX];
    int n;
    int beeps;
    uint32_t next_beep;
    int badge;
} A;

static void alert_close(void)
{
    if (U.alert) lv_obj_delete(U.alert);
    U.alert = NULL;
    A.n = 0;
    A.beeps = 0;
}

static void alert_ok_cb(lv_event_t *e) { (void)e; alert_close(); }

static void alert_see_cb(lv_event_t *e)
{
    (void)e;
    int ch = A.n ? A.a[A.n - 1].ch : -1;
    alert_close();
    aos_ui_open(DL_APP_ID);
    if (ch >= 0) dl_go(DL_V_DETAIL, ch);
}

static void alert_build(void)
{
    if (U.alert) lv_obj_delete(U.alert);
    U.alert = NULL;
    if (!A.n) return;
    lv_obj_t *top = lv_layer_top();
    int32_t w = lv_obj_get_width(top), h = lv_obj_get_height(top);
    bool land = w > h;
    U.alert = dl_box(top, w, h);
    lv_obj_set_style_bg_color(U.alert, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.alert, LV_OPA_60, 0);
    lv_obj_add_flag(U.alert, LV_OBJ_FLAG_CLICKABLE);
    int32_t cw = land ? 720 : w - 2 * AOS_UI_PAD;
    lv_obj_t *c = dl_card(U.alert, cw, LV_SIZE_CONTENT);
    lv_obj_set_style_border_color(c, DL_C_HIGH, 0);
    lv_obj_set_style_border_width(c, 3, 0);
    lv_obj_set_style_pad_all(c, 28, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_center(c);
    lv_obj_t *head = dl_box(c, LV_PCT(100), 56);
    lv_obj_t *g = aos_label(head, AOS_SYM_ALERT_OUTLINE, &aos_sym_44, DL_C_HIGH);
    lv_obj_align(g, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *t = aos_label(head, _("Alarma de Datalog"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 62, 0);
    for (int i = A.n - 1; i >= 0 && i >= A.n - 3; i--) {
        lv_obj_t *r = dl_box(c, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *n = aos_label(r, A.a[i].name, aos_font_body,
                                A.cleared[i] ? AOS_C_DIM : A.a[i].alarm == DL_AL_LOW ? DL_C_LOW : DL_C_HIGH);
        (void)n;
        char line[140];
        if (A.cleared[i]) snprintf(line, sizeof line, "%s · %s", A.a[i].text, _("ya volvió a lo normal"));
        else dl_scpy(line, sizeof line, A.a[i].text);
        lv_obj_t *l = aos_label(r, line, aos_font_small, AOS_C_TEXT);
        lv_obj_set_width(l, cw - 56);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    if (A.n > 3) {
        char more[48];
        snprintf(more, sizeof more, _("y %d más"), A.n - 3);
        aos_label(c, more, aos_font_caption, AOS_C_DIM);
    }
    lv_obj_t *row = dl_box(c, LV_PCT(100), 80);
    lv_obj_t *see = dl_pill(row, AOS_SYM_CHART_LINE, _("Ver"), AOS_C_CARD2, alert_see_cb, NULL);
    lv_obj_align(see, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *ok = dl_pill(row, AOS_SYM_CHECK, _("Entendido"), lv_color_hex(0xB3261E), alert_ok_cb, NULL);
    lv_obj_align(ok, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void alerts_tick(void)
{
    dl_alert_t a;
    bool changed = false;
    while (dl_alert_pop(&a)) {
        if (a.alarm == DL_AL_NONE) {
            for (int i = 0; i < A.n; i++) if (A.a[i].ch == a.ch) { A.cleared[i] = true; changed = true; }
            continue;
        }
        if (A.n == PEND_MAX) {
            memmove(&A.a[0], &A.a[1], sizeof A.a[0] * (PEND_MAX - 1));
            memmove(&A.cleared[0], &A.cleared[1], sizeof A.cleared[0] * (PEND_MAX - 1));
            A.n--;
        }
        A.a[A.n] = a;
        A.cleared[A.n] = false;
        A.n++;
        A.beeps = 10;
        A.next_beep = (uint32_t)aos_hal_uptime_ms();
        changed = true;
        aos_hal_log("datalog", "alarm: %s %s", a.name, a.text);
    }
    if (changed) alert_build();
    bool live = false;
    for (int i = 0; i < A.n; i++) live |= !A.cleared[i];
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (U.alert && live && A.beeps > 0 && (int32_t)(now - A.next_beep) >= 0) {
        aos_hal_beep(2200, 140);
        A.beeps--;
        A.next_beep = now + 2000;
    }
    int on = dl_alarms_on();
    if (on != A.badge) {
        A.badge = on;
        aos_ui_set_badge(DL_APP_ID, on);
    }
}

/* ---- the portal's live channel ---- */

static size_t jstr(char *o, size_t n, const char *s)
{
    size_t k = 0;
    if (k < n) o[k++] = '"';
    for (; *s && k + 3 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c < 0x20) o[k++] = ' ';
        else o[k++] = (char)c;
    }
    if (k < n) o[k++] = '"';
    return k;
}

#define JS_MAX (12 * 1024)

static void live_put(void)
{
    static char js[JS_MAX];
    size_t o = 0;
    bool valid;
    dl_now(&valid);
    dl_lock();
    dl_cfg_t *c = dl_cfg();
    o += (size_t)snprintf(js + o, JS_MAX - o, "{\"running\":%s,\"clock\":%s,\"semicolon\":%s,\"lines\":[",
                          c->running ? "true" : "false", valid ? "true" : "false", c->semicolon ? "true" : "false");
    for (int i = 0; i < c->n && o + 600 < JS_MAX; i++) {
        const dl_ch_t *ch = &c->ch[i];
        const dl_rt_t *r = dl_rt(i);
        char src[96], sched[64], val[32];
        dl_src_desc(ch, src, sizeof src);
        dl_sched_text(ch, sched, sizeof sched);
        o += (size_t)snprintf(js + o, JS_MAX - o, "%s{\"id\":", i ? "," : "");
        o += jstr(js + o, JS_MAX - o, ch->id);
        o += (size_t)snprintf(js + o, JS_MAX - o, ",\"name\":");
        o += jstr(js + o, JS_MAX - o, ch->name);
        o += (size_t)snprintf(js + o, JS_MAX - o, ",\"unit\":");
        o += jstr(js + o, JS_MAX - o, dl_src_unit(ch));
        o += (size_t)snprintf(js + o, JS_MAX - o, ",\"src\":");
        o += jstr(js + o, JS_MAX - o, src);
        o += (size_t)snprintf(js + o, JS_MAX - o, ",\"sched\":");
        o += jstr(js + o, JS_MAX - o, sched);
        o += (size_t)snprintf(js + o, JS_MAX - o, ",\"when\":");
        o += jstr(js + o, JS_MAX - o, r->when);
        o += (size_t)snprintf(js + o, JS_MAX - o, ",\"err\":");
        o += jstr(js + o, JS_MAX - o, r->err);
        snprintf(val, sizeof val, "%.*f", dl_src_decimals(ch), (double)r->v);
        o += (size_t)snprintf(js + o, JS_MAX - o, ",\"dec\":%d,\"v\":%s,\"ok\":%s,\"reads\":%u,\"errors\":%u,\"alarm\":%d",
                              dl_src_decimals(ch), r->have && !isnan(r->v) ? val : "null", r->ok ? "true" : "false",
                              (unsigned)r->reads, (unsigned)r->errors, r->alarm);
        if (ch->lo_on) { snprintf(val, sizeof val, "%g", (double)ch->lo); o += (size_t)snprintf(js + o, JS_MAX - o, ",\"lo\":%s", val); }
        if (ch->hi_on) { snprintf(val, sizeof val, "%g", (double)ch->hi); o += (size_t)snprintf(js + o, JS_MAX - o, ",\"hi\":%s", val); }
        if (o < JS_MAX) js[o++] = '}';
    }
    dl_unlock();
    o += (size_t)snprintf(js + o, JS_MAX - o, "]}");
    if (o >= JS_MAX) o = JS_MAX - 1;
    aos_hal_live_put(DL_APP_ID, "state", "application/json", js, o);
}

static void live_tick(void)
{
    char msg[128];
    int n;
    while ((n = aos_hal_live_take(DL_APP_ID, msg, sizeof msg - 1)) > 0) {
        msg[n < (int)sizeof msg ? n : (int)sizeof msg - 1] = 0;
        msg[strcspn(msg, "\r\n")] = 0;
        if (!strcmp(msg, "run=1")) dl_set_running(true);
        else if (!strcmp(msg, "run=0")) dl_set_running(false);
        else if (!strcmp(msg, "csv=semicolon")) dl_set_semicolon(true);
        else if (!strcmp(msg, "csv=comma")) dl_set_semicolon(false);
        else if (!strcmp(msg, "reload")) dl_reload();
    }
    static uint32_t last;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    uint32_t every = aos_hal_live_idle_ms(DL_APP_ID) < 10000 ? 1000 : 5000;
    if ((int32_t)(now - last) >= (int32_t)every) {
        last = now;
        live_put();
    }
}

/* ---- datalog.txt written by someone else (the portal's Archivos) ---- */

static struct { long mtime, size; uint32_t next; } W;

static void file_tick(void)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if ((int32_t)(now - W.next) < 0) return;
    W.next = now + 2000;
    char path[256];
    dl_cfg_path(path, sizeof path);
    struct stat st;
    long mt = 0, sz = -1;
    if (stat(path, &st) == 0) { mt = (long)st.st_mtime; sz = (long)st.st_size; }
    if (mt == W.mtime && sz == W.size) return;
    bool first = W.size == 0 && W.mtime == 0;
    W.mtime = mt;
    W.size = sz;
    /* our own saves land here too: a reload of the same lines changes nothing */
    if (!first && sz >= 0) dl_reload();
}

/* ---- the app ---- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&U, 0, sizeof U);
    memset(&A, 0, sizeof A);
    memset(&W, 0, sizeof W);
    dl_engine_start();
    layout(root);
    U.view = DL_V_LIST;
    dl_go(DL_V_LIST, -1);
    file_tick();
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (U.view == DL_V_DETAIL) dl_view_leave();
    alert_close();
    aos_ui_set_badge(DL_APP_ID, 0);
    aos_hal_live_clear(DL_APP_ID);
    dl_engine_stop();
    memset(&U, 0, sizeof U);
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    int view = U.view, sel = U.sel;
    if (view == DL_V_DETAIL) dl_view_leave();
    dl_sheet_close();
    lv_obj_clean(root);
    layout(root);
    U.view = view;
    if (view == DL_V_EDIT) dl_edit_open(-2);    /* the same draft, laid out again */
    else dl_go(view, sel);
    if (U.alert) alert_build();
    return true;
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (U.overlay) { dl_sheet_close(); return true; }
    if (U.view == DL_V_EDIT) return dl_edit_back();
    if (U.view == DL_V_DETAIL) { dl_go(DL_V_LIST, -1); return true; }
    return false;
}

static void tick(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    alerts_tick();
    live_tick();
    file_tick();
    uint32_t seq = dl_seq();
    bool moved = seq != U.seen_seq;
    U.seen_seq = seq;
    if (U.view == DL_V_LIST) {
        if (moved || U.clock) dl_list_refresh();
    } else if (U.view == DL_V_DETAIL) {
        if (moved) dl_view_refresh();
        dl_view_tick();
    } else if (U.view == DL_V_EDIT) {
        dl_edit_tick();
    }
}

static bool dl_init(aos_app_t *app)
{
    app->desc.id       = "aos.datalog";     /* literal: gen_lang.py looks for it here */
    app->desc.name     = "Datalog";
    app->desc.icon     = AOS_SYM_CHART_LINE;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x2DD4BF;
    app->desc.color_b  = 0x0F766E;
    app->desc.flags    = AOS_APP_FLAG_BACKGROUND;
    app->desc.order    = 516;
    aos_icon_set_ops(app, DL_ICON, sizeof DL_ICON);

    app->create  = create;
    app->destroy = destroy;
    app->back    = back;
    app->tick    = tick;
    app->resize  = resize;
    return true;
}

AOS_APP_ENTRY(dl_init);
