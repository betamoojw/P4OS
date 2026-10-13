/*
 * P4OS - Python: the app, a console.
 *
 * MicroPython 1.29 embedded (main/upy/, made by tools/gen_upy.sh) on a
 * thread of its own (p4py_rt.c). The screen is a REPL like the one on a
 * computer: what was typed and what it printed above (p4py_term.c), the
 * line being typed with its prompt, and a keyboard for Python
 * (p4py_kbd.c) that hides when a USB or Bluetooth keyboard is used.
 *
 *   >>>   a statement; an expression's value is printed
 *   ...   the statement goes on (def, for, an open bracket): Enter on an
 *         empty line runs it. The indent is kept and grows after a colon.
 *   input() shows its own prompt and waits for the line.
 *
 * Up and down bring back the lines typed before (kept in
 * data/python_hist.txt). Stop, or Ctrl+C on a keyboard, raises
 * KeyboardInterrupt.
 *
 * The portal can type too, on the live channel ("line <text>", "stop",
 * "clear"): the #python page will, and it is how the console is tested.
 */
#include "p4py_kbd.h"
#include "p4py_rt.h"
#include "p4py_term.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_mono.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAP_BYTES   (12u * 1024 * 1024)
#define INPUT_MAX     1024
#define HIST_MAX     200
#define LIVE_ID      "aos.python"

bool mp_repl_continue_with_input(const char *input);   /* py/repl.h: plain text, no interpreter */

/* Python's logo: two snakes, the same shape turned half a turn about the
 * centre, blue above and yellow below, each with its eye; a thin line of
 * the background between them. Percent of the icon, from its centre. */
static const uint8_t PY_ICON[] = {
    AIC_HEADER,
    /* blue: the head on top, the body to the left */
    AIC_RECT(AIC_CENTER,  -2, -21, 34, 18, 8, AIC_C_LIT(0x3776AB), 255),
    AIC_GRAD(AIC_C_LIT(0x4B8BBE), AIC_GRAD_VER),
    AIC_RECT(AIC_CENTER, -14,  -2, 32, 20, 8, AIC_C_LIT(0x3776AB), 255),
    AIC_GRAD(AIC_C_LIT(0x306998), AIC_GRAD_VER),
    AIC_RECT(AIC_CENTER,  -4, -11, 14, 12, 0, AIC_C_LIT(0x3776AB), 255),
    AIC_RECT(AIC_CENTER, -11, -25,  5,  5, AIC_CIRCLE, AIC_C_TEXT, 255),
    /* yellow: the same, turned */
    AIC_RECT(AIC_CENTER,  14,   2, 32, 20, 8, AIC_C_LIT(0xFFD43B), 255),
    AIC_GRAD(AIC_C_LIT(0xFFE873), AIC_GRAD_VER),
    AIC_BORDER(2, AIC_C_LIT(0xF5F7FA), 255),
    AIC_RECT(AIC_CENTER,   2,  21, 34, 18, 8, AIC_C_LIT(0xFFD43B), 255),
    AIC_RECT(AIC_CENTER,   4,  11, 14, 12, 0, AIC_C_LIT(0xFFD43B), 255),
    AIC_RECT(AIC_CENTER,  11,  25,  5,  5, AIC_CIRCLE, AIC_C_TEXT, 255),
    AIC_END
};

typedef struct {
    aos_app_t  *app;
    lv_obj_t   *root, *term, *inbox, *status, *stop, *kbd, *kbd_btn;
    lv_timer_t *timer;
    bool        landscape, kbd_hidden, started, was_busy, was_input;
    /* the line being typed */
    char        line[INPUT_MAX];
    int         len, cur;           /* bytes; the cursor is a byte offset */
    int         hscroll;            /* the first character shown */
    /* a statement over several lines, until it is complete */
    char       *block;
    size_t      block_n;
    /* the lines typed before */
    char       *hist[HIST_MAX];
    int         nhist, hpos;        /* hpos == nhist: the line being typed */
    char        saved[INPUT_MAX];    /* it, while going through the history */
    /* the console's text as the portal last got it */
    uint32_t    con_seq, con_ms;
} py_t;

static py_t *P;

/* --- the history ------------------------------------------------------------------ */

static void hist_path(char *out, size_t n)
{
    snprintf(out, n, "%s/python_hist.txt", aos_hal_path_data());
}

static void hist_add(py_t *p, const char *s)
{
    if (!*s) return;
    if (p->nhist && !strcmp(p->hist[p->nhist - 1], s)) return;
    if (p->nhist == HIST_MAX) {
        free(p->hist[0]);
        memmove(p->hist, p->hist + 1, sizeof(char *) * (HIST_MAX - 1));
        p->nhist--;
    }
    size_t n = strlen(s);
    char *c = malloc(n + 1);
    if (!c) return;
    memcpy(c, s, n + 1);
    p->hist[p->nhist++] = c;
}

static void hist_load(py_t *p)
{
    char path[200];
    hist_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[INPUT_MAX];
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        hist_add(p, buf);
    }
    fclose(f);
}

static void hist_save(py_t *p)
{
    char path[200];
    hist_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < p->nhist; i++) fprintf(f, "%s\n", p->hist[i]);
    fclose(f);
}

/* --- the line being typed ------------------------------------------------------------ */

static const char *prompt(py_t *p)
{
    if (p4py_wants_input()) return "";          /* input() printed its own */
    return p->block_n ? "... " : ">>> ";
}

static int u8len(const char *s, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) c += ((unsigned char)s[i] & 0xC0) != 0x80;
    return c;
}

static void inbox_draw(lv_event_t *e)
{
    py_t *p = P;
    if (!p) return;
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_content_coords(o, &c);
    const lv_font_t *f = &aos_mono_22;
    int cw = lv_font_get_glyph_width(f, 'M', 0), lh = lv_font_get_line_height(f);
    int cols = lv_area_get_width(&c) / cw;
    const char *pr = prompt(p);
    int plen = (int)strlen(pr);
    int avail = cols - plen - 1;
    if (avail < 4) avail = 4;
    int cc = u8len(p->line, p->cur);                 /* the cursor's column in the text */
    if (cc < p->hscroll) p->hscroll = cc;
    if (cc > p->hscroll + avail) p->hscroll = cc - avail;
    /* the text from hscroll, at most avail characters */
    const char *s = p->line;
    int i = 0, ch = 0;
    while (i < p->len && ch < p->hscroll) {
        i++;
        while (i < p->len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
        ch++;
    }
    int j = i, shown = 0;
    while (j < p->len && shown < avail) {
        j++;
        while (j < p->len && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
        shown++;
    }
    char buf[INPUT_MAX + 8];
    snprintf(buf, sizeof buf, "%s%.*s", pr, j - i, s + i);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.text = buf;
    d.text_local = 1;
    d.color = lv_color_hex(0xF2F2F7);
    int y = c.y1 + (lv_area_get_height(&c) - lh) / 2;
    lv_area_t a = { c.x1, y, c.x2, y + lh };
    lv_draw_label(layer, &d, &a);
    /* the cursor: a bar before the character it stands on */
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(0x64D2FF);
    int x = c.x1 + (plen + cc - p->hscroll) * cw;
    lv_area_t ca = { x, y + 2, x + 2, y + lh - 2 };
    lv_draw_rect(layer, &r, &ca);
}

static void inbox_refresh(py_t *p)
{
    if (p->inbox) lv_obj_invalidate(p->inbox);
}

static void line_set(py_t *p, const char *s)
{
    snprintf(p->line, sizeof p->line, "%s", s);
    p->len = p->cur = (int)strlen(p->line);
    p->hscroll = 0;
    inbox_refresh(p);
}

static void line_insert(py_t *p, const char *s)
{
    int n = (int)strlen(s);
    if (p->len + n >= INPUT_MAX) return;
    memmove(p->line + p->cur + n, p->line + p->cur, p->len - p->cur + 1);
    memcpy(p->line + p->cur, s, n);
    p->len += n;
    p->cur += n;
    inbox_refresh(p);
}

static void line_backspace(py_t *p)
{
    if (!p->cur) return;
    int i = p->cur - 1;
    while (i > 0 && ((unsigned char)p->line[i] & 0xC0) == 0x80) i--;
    /* a whole indent level at once, in the leading spaces */
    bool lead = true;
    for (int k = 0; k < p->cur; k++) lead &= p->line[k] == ' ';
    if (lead && p->cur >= 4 && p->cur % 4 == 0) i = p->cur - 4;
    memmove(p->line + i, p->line + p->cur, p->len - p->cur + 1);
    p->len -= p->cur - i;
    p->cur = i;
    inbox_refresh(p);
}

static void line_move(py_t *p, int dir)
{
    if (dir < 0 && p->cur > 0) {
        p->cur--;
        while (p->cur > 0 && ((unsigned char)p->line[p->cur] & 0xC0) == 0x80) p->cur--;
    } else if (dir > 0 && p->cur < p->len) {
        p->cur++;
        while (p->cur < p->len && ((unsigned char)p->line[p->cur] & 0xC0) == 0x80) p->cur++;
    }
    inbox_refresh(p);
}

static void hist_move(py_t *p, int dir)
{
    if (!p->nhist) return;
    if (p->hpos == p->nhist) snprintf(p->saved, sizeof p->saved, "%s", p->line);
    int n = p->hpos + dir;
    if (n < 0) n = 0;
    if (n > p->nhist) n = p->nhist;
    p->hpos = n;
    line_set(p, n == p->nhist ? p->saved : p->hist[n]);
}

/* --- running ----------------------------------------------------------------------- */

static void echo(py_t *p, const char *pr, const char *s)
{
    term_write(p->term, pr, strlen(pr), TERM_IN);
    term_write(p->term, s, strlen(s), TERM_IN);
    term_write(p->term, "\n", 1, TERM_IN);
}

static void block_add(py_t *p, const char *s)
{
    size_t n = strlen(s);
    char *nb = realloc(p->block, p->block_n + n + 2);
    if (!nb) return;
    p->block = nb;
    memcpy(p->block + p->block_n, s, n);
    p->block_n += n;
    p->block[p->block_n++] = '\n';
    p->block[p->block_n] = 0;
}

static void enter(py_t *p)
{
    char line[INPUT_MAX];
    snprintf(line, sizeof line, "%s", p->line);
    if (p4py_wants_input()) {
        echo(p, "", line);
        p4py_give_input(line);
        line_set(p, "");
        return;
    }
    if (p4py_busy()) return;                     /* the line waits in the box */
    echo(p, prompt(p), line);
    hist_add(p, line);
    p->hpos = p->nhist;
    block_add(p, line);
    /* the block without its last newline is what the REPL looks at */
    p->block[p->block_n - 1] = 0;
    bool more = mp_repl_continue_with_input(p->block);
    p->block[p->block_n - 1] = '\n';
    if (more) {
        /* the same indent, and one more after a colon */
        int ind = 0;
        while (line[ind] == ' ') ind++;
        int e = (int)strlen(line);
        while (e > 0 && line[e - 1] == ' ') e--;
        if (e > 0 && line[e - 1] == ':') ind += 4;
        if (ind > 60) ind = 60;
        char sp[64];
        memset(sp, ' ', ind);
        sp[ind] = 0;
        line_set(p, sp);
        return;
    }
    p->block[--p->block_n] = 0;
    if (p->block_n) p4py_run(p->block, "<stdin>", P4PY_SINGLE);
    free(p->block);
    p->block = NULL;
    p->block_n = 0;
    line_set(p, "");
}

static void stop(py_t *p)
{
    if (p4py_busy()) {
        p4py_interrupt();
    } else if (p->block_n) {
        /* Ctrl+C on a half-typed block: drop it, as the computer's REPL */
        free(p->block);
        p->block = NULL;
        p->block_n = 0;
        term_write(p->term, "KeyboardInterrupt\n", 18, TERM_ERR);
        line_set(p, "");
    }
}

static void key(void *user, const char *k)
{
    py_t *p = user;
    if (!strcmp(k, "/enter")) enter(p);
    else if (!strcmp(k, "/bs")) line_backspace(p);
    else if (!strcmp(k, "/tab")) line_insert(p, "    ");
    else if (!strcmp(k, "/left")) line_move(p, -1);
    else if (!strcmp(k, "/right")) line_move(p, 1);
    else if (!strcmp(k, "/up")) hist_move(p, -1);
    else if (!strcmp(k, "/down")) hist_move(p, 1);
    else line_insert(p, k);
}

static bool hw_key(uint32_t k, uint8_t mods)
{
    py_t *p = P;
    if (!p) return false;
    bool ctrl = (mods & 0x11) != 0;
    if (ctrl && (k == 'c' || k == 'C')) { stop(p); return true; }
    if (ctrl && (k == 'l' || k == 'L')) { term_clear(p->term); return true; }
    switch (k) {
    case AOS_KEY_ENTER:     enter(p); return true;
    case AOS_KEY_BACKSPACE: line_backspace(p); return true;
    case AOS_KEY_DEL:
        if (p->cur < p->len) { line_move(p, 1); line_backspace(p); }
        return true;
    case AOS_KEY_LEFT:      line_move(p, -1); return true;
    case AOS_KEY_RIGHT:     line_move(p, 1); return true;
    case AOS_KEY_UP:        hist_move(p, -1); return true;
    case AOS_KEY_DOWN:      hist_move(p, 1); return true;
    case AOS_KEY_HOME:      p->cur = 0; inbox_refresh(p); return true;
    case AOS_KEY_END:       p->cur = p->len; inbox_refresh(p); return true;
    case AOS_KEY_NEXT:      line_insert(p, "    "); return true;
    case AOS_KEY_ESC:       line_set(p, ""); return true;
    default: break;
    }
    if (k >= 0x20 && k < AOS_KEY_CONSUMER && k != AOS_KEY_DEL) {
        /* a code point, typed as UTF-8 */
        char u[5] = { 0 };
        if (k < 0x80) u[0] = (char)k;
        else if (k < 0x800) { u[0] = (char)(0xC0 | (k >> 6)); u[1] = (char)(0x80 | (k & 0x3F)); }
        else if (k < 0x10000) { u[0] = (char)(0xE0 | (k >> 12)); u[1] = (char)(0x80 | ((k >> 6) & 0x3F)); u[2] = (char)(0x80 | (k & 0x3F)); }
        else { u[0] = (char)(0xF0 | (k >> 18)); u[1] = (char)(0x80 | ((k >> 12) & 0x3F)); u[2] = (char)(0x80 | ((k >> 6) & 0x3F)); u[3] = (char)(0x80 | (k & 0x3F)); }
        line_insert(p, u);
        if (!p->kbd_hidden && p->kbd) {
            /* a keyboard of its own is being used: the screen's goes away */
            p->kbd_hidden = true;
            lv_obj_add_flag(p->kbd, LV_OBJ_FLAG_HIDDEN);
        }
        return true;
    }
    return false;
}

/* --- the portal --------------------------------------------------------------------- */

static void live_take(py_t *p)
{
    char msg[AOS_LIVE_MSG_MAX + 1];
    int n;
    while ((n = aos_hal_live_take(LIVE_ID, msg, sizeof msg - 1)) > 0) {
        msg[n] = 0;
        if (!strncmp(msg, "line ", 5) || !strcmp(msg, "line")) {
            line_set(p, n > 5 ? msg + 5 : "");
            enter(p);
        } else if (!strcmp(msg, "stop")) {
            stop(p);
        } else if (!strcmp(msg, "clear")) {
            term_clear(p->term);
        }
    }
}

/* the console's text for the page, key "con", while one is looking (at
 * most five times a second) */
static void live_put(py_t *p)
{
    uint32_t seq = term_seq(p->term), now = (uint32_t)aos_hal_uptime_ms();
    if (seq == p->con_seq || now - p->con_ms < 200 || aos_hal_live_idle_ms(LIVE_ID) > 10000) return;
    char *txt = term_text(p->term, AOS_LIVE_BLOB_MAX);
    if (!txt) return;
    aos_hal_live_put(LIVE_ID, "con", "text/plain; charset=utf-8", txt, strlen(txt));
    free(txt);
    p->con_seq = seq;
    p->con_ms = now;
}

/* --- the frame --------------------------------------------------------------------- */

static void status_update(py_t *p)
{
    bool busy = p4py_busy(), in = p4py_wants_input();
    const char *st = !p->started ? _("MicroPython no arrancó")
                   : in          ? _("esperando una línea")
                   : busy        ? _("corriendo…")
                   :               "";
    if (strcmp(lv_label_get_text(p->status), st)) lv_label_set_text(p->status, st);
    if (busy) lv_obj_remove_flag(p->stop, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(p->stop, LV_OBJ_FLAG_HIDDEN);
    if (busy != p->was_busy || in != p->was_input) inbox_refresh(p);   /* the prompt changes */
    p->was_busy = busy;
    p->was_input = in;
}

static void tick(lv_timer_t *t)
{
    py_t *p = lv_timer_get_user_data(t);
    static char buf[4096];
    size_t n;
    while ((n = p4py_take_output(buf, sizeof buf)) > 0) term_write(p->term, buf, n, TERM_OUT);
    live_take(p);
    live_put(p);
    status_update(p);
}

/* --- the view ------------------------------------------------------------------------ */

static void stop_cb(lv_event_t *e) { (void)e; if (P) stop(P); }
static void clear_cb(lv_event_t *e) { (void)e; if (P) term_clear(P->term); }

static void kbd_toggle(lv_event_t *e)
{
    (void)e;
    py_t *p = P;
    if (!p || !p->kbd) return;
    p->kbd_hidden = !p->kbd_hidden;
    if (p->kbd_hidden) lv_obj_add_flag(p->kbd, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(p->kbd, LV_OBJ_FLAG_HIDDEN);
}

static void inbox_click(lv_event_t *e)
{
    (void)e;
    py_t *p = P;
    if (p && p->kbd_hidden && p->kbd) {
        p->kbd_hidden = false;
        lv_obj_remove_flag(p->kbd, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *bar_button(lv_obj_t *bar, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(bar);
    lv_obj_set_height(b, 50);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_set_style_radius(b, 25, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_center(l);
    return b;
}

static void build(py_t *p, lv_obj_t *root)
{
    p->root = root;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_flex_flow(root, p->landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 10, 0);
    lv_obj_set_style_pad_gap(root, 10, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* upright everything is one column; lying down the console is on the
     * left at full height and the keyboard on the right */
    lv_obj_t *col = root;
    if (p->landscape) {
        col = lv_obj_create(root);
        lv_obj_remove_style_all(col);
        lv_obj_set_height(col, LV_PCT(100));
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 8, 0);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *bar = lv_obj_create(col);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 52);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "Python");
    lv_obj_set_style_text_font(title, aos_font_body, 0);
    p->status = lv_label_create(bar);
    lv_label_set_text(p->status, "");
    lv_obj_set_flex_grow(p->status, 1);
    lv_obj_set_style_text_color(p->status, AOS_C_DIM, 0);
    lv_obj_set_style_text_font(p->status, aos_font_small, 0);
    p->stop = bar_button(bar, _("Detener"), stop_cb);
    lv_obj_set_style_bg_color(p->stop, AOS_C_RED, 0);
    lv_obj_add_flag(p->stop, LV_OBJ_FLAG_HIDDEN);
    bar_button(bar, _("Limpiar"), clear_cb);
    p->kbd_btn = bar_button(bar, LV_SYMBOL_KEYBOARD, kbd_toggle);

    if (!p->term) {
        p->term = term_create(col, &aos_mono_22);
    } else {
        lv_obj_set_parent(p->term, col);
    }
    lv_obj_set_width(p->term, LV_PCT(100));
    lv_obj_set_flex_grow(p->term, 1);

    p->inbox = lv_obj_create(col);
    lv_obj_remove_style_all(p->inbox);
    lv_obj_set_size(p->inbox, LV_PCT(100), 56);
    lv_obj_set_style_bg_color(p->inbox, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(p->inbox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p->inbox, 12, 0);
    lv_obj_set_style_pad_hor(p->inbox, 12, 0);
    lv_obj_set_style_border_width(p->inbox, 2, 0);
    lv_obj_set_style_border_color(p->inbox, lv_color_hex(0x3776AB), 0);
    lv_obj_add_flag(p->inbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p->inbox, inbox_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(p->inbox, inbox_click, LV_EVENT_CLICKED, NULL);

    p->kbd = kbd_create(root, p->landscape, key, p);
    if (p->landscape) {
        lv_obj_set_size(p->kbd, LV_PCT(48), LV_PCT(100));
        lv_obj_set_flex_align(p->kbd, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    }
    if (p->kbd_hidden) lv_obj_add_flag(p->kbd, LV_OBJ_FLAG_HIDDEN);
}

static void *py_create(aos_app_t *self, lv_obj_t *root)
{
    py_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    P = p;
    p->app = self;
    p->landscape = lv_obj_get_width(root) > lv_obj_get_height(root);
    hist_load(p);
    p->hpos = p->nhist;
    build(p, root);
    p->started = p4py_start(HEAP_BYTES);
    const char *banner = _("MicroPython 1.29 en P4OS · help() para empezar\n");
    term_write(p->term, banner, strlen(banner), TERM_INFO);
    aos_ui_hwkbd_handler(hw_key);
    p->timer = lv_timer_create(tick, 30, p);
    return p;
}

static bool py_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    py_t *p = inst;
    /* the terminal keeps its lines: it moves to the new root, the rest is new */
    lv_obj_set_parent(p->term, lv_layer_top());
    lv_obj_clean(root);
    p->landscape = lv_obj_get_width(root) > lv_obj_get_height(root);
    build(p, root);
    return true;
}

static void py_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    py_t *p = inst;
    aos_ui_hwkbd_handler(NULL);
    lv_timer_delete(p->timer);
    aos_hal_live_clear(LIVE_ID);
    p4py_stop();
    hist_save(p);
    for (int i = 0; i < p->nhist; i++) free(p->hist[i]);
    free(p->block);
    P = NULL;
    free(p);
}

static bool py_init(aos_app_t *app)
{
    app->desc.id       = "aos.python";
    app->desc.name     = "Python";
    app->desc.icon     = LV_SYMBOL_EDIT;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0xF5F7FA;
    app->desc.color_b  = 0xD3DCE6;
    app->desc.order    = 205;
    app->desc.flags    = AOS_APP_FLAG_KEEP;     /* a session kept while other apps are used */
    aos_icon_set_ops(app, PY_ICON, sizeof PY_ICON);
    app->create  = py_create;
    app->resize  = py_resize;
    app->destroy = py_destroy;
    return true;
}

AOS_APP_ENTRY(py_init);
