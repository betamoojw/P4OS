/*
 * P4OS - CAS: the maths keypad.
 *
 * A template key (fraction, power, root, integral...) is labelled with the
 * template itself, drawn by mx_view from a sample built with the same keys
 * it sends, so the key shows exactly what it inserts.
 */
#include "cas_keypad.h"

#include "mx_view.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <string.h>

enum { K_NUM, K_FN, K_OP, K_ENTER, K_DEL, K_SHIFT };

typedef struct {
    const char *label;          /* NULL: drawn from 'sample' */
    const char *key;
    const char *sample;         /* keys for the label, '|' between them */
    uint8_t     kind;
    uint8_t     span;           /* columns, 0 = 1 */
} kp_key_t;

#define BS     "\xE2\x8C\xAB"   /* ⌫ */
#define ENTER  "\xE2\x86\xB5"   /* ↵ */
#define PI     "\xCF\x80"
#define DIV    "\xC3\xB7"
#define TIMES  "\xC3\x97"
#define MINUS  "\xE2\x88\x92"

static const kp_key_t PAGE_MAIN[] = {
    { "x", "x", 0, K_FN, 0 }, { "y", "y", 0, K_FN, 0 }, { 0, "/frac", "/frac", K_FN, 0 },
    { 0, "/pow", "x|/pow", K_FN, 0 }, { 0, "/sqrt", "/sqrt", K_FN, 0 }, { BS, "/bs", 0, K_DEL, 0 },

    { "(", "(", 0, K_FN, 0 }, { ")", ")", 0, K_FN, 0 }, { "7", "7", 0, K_NUM, 0 },
    { "8", "8", 0, K_NUM, 0 }, { "9", "9", 0, K_NUM, 0 }, { DIV, DIV, 0, K_OP, 0 },

    { PI, "@" PI, 0, K_FN, 0 }, { "e", "@e", 0, K_FN, 0 }, { "4", "4", 0, K_NUM, 0 },
    { "5", "5", 0, K_NUM, 0 }, { "6", "6", 0, K_NUM, 0 }, { TIMES, "*", 0, K_OP, 0 },

    { 0, "/sq", "x|/sq", K_FN, 0 }, { ",", ",", 0, K_FN, 0 }, { "1", "1", 0, K_NUM, 0 },
    { "2", "2", 0, K_NUM, 0 }, { "3", "3", 0, K_NUM, 0 }, { MINUS, "-", 0, K_OP, 0 },

    { "ans", "/ans", 0, K_FN, 0 }, { "=", "=", 0, K_FN, 0 }, { "0", "0", 0, K_NUM, 0 },
    { ".", ".", 0, K_NUM, 0 }, { "+", "+", 0, K_OP, 0 }, { ENTER, "/enter", 0, K_ENTER, 0 },
};

static const kp_key_t PAGE_FUNC[] = {
    { "sin", "sin(", 0, K_FN, 0 }, { "cos", "cos(", 0, K_FN, 0 }, { "tan", "tan(", 0, K_FN, 0 },
    { "ln", "ln(", 0, K_FN, 0 }, { "log", "log10(", 0, K_FN, 0 }, { 0, "/exp", "/exp", K_FN, 0 },

    { "asin", "asin(", 0, K_FN, 0 }, { "acos", "acos(", 0, K_FN, 0 }, { "atan", "atan(", 0, K_FN, 0 },
    { 0, "/abs", "/abs", K_FN, 0 }, { 0, "/root", "/root", K_FN, 0 }, { 0, "/10pow", "/10pow", K_FN, 0 },

    { "sinh", "sinh(", 0, K_FN, 0 }, { "cosh", "cosh(", 0, K_FN, 0 }, { "tanh", "tanh(", 0, K_FN, 0 },
    { "n!", "!", 0, K_FN, 0 }, { "\xCE\xB8", "@\xCE\xB8", 0, K_FN, 0 }, { "\xE2\x88\x9E", "@\xE2\x88\x9E", 0, K_FN, 0 },

    { "i", "@i", 0, K_FN, 0 }, { "\xC2\xB0", "\xC2\xB0", 0, K_FN, 0 }, { "floor", "floor(", 0, K_FN, 0 },
    { "ceil", "ceil(", 0, K_FN, 0 }, { "round", "round(", 0, K_FN, 0 }, { 0, "/inv", "x|/inv", K_FN, 0 },

    { "nCr", "comb(", 0, K_FN, 0 }, { "nPr", "perm(", 0, K_FN, 0 }, { N_("mcd"), "gcd(", 0, K_FN, 0 },
    { N_("mcm"), "lcm(", 0, K_FN, 0 }, { N_("primos"), "ifactor(", 0, K_FN, 0 }, { "mod", "irem(", 0, K_FN, 0 },
};

static const kp_key_t PAGE_CALC[] = {
    { 0, "/int", "/int", K_FN, 0 }, { 0, "/defint", "/defint", K_FN, 0 }, { 0, "/deriv", "/deriv", K_FN, 0 },
    { 0, "/limit", "/limit", K_FN, 0 }, { 0, "/sum", "/sum", K_FN, 0 }, { 0, "/prod", "/prod", K_FN, 0 },

    { "solve", "solve(", 0, K_FN, 0 }, { "factor", "factor(", 0, K_FN, 0 }, { "expand", "expand(", 0, K_FN, 0 },
    { "simplify", "simplify(", 0, K_FN, 0 }, { "partfrac", "partfrac(", 0, K_FN, 0 }, { "\xE2\x89\x88", "approx(", 0, K_FN, 0 },

    { 0, "/mat2", "/mat2", K_FN, 0 }, { 0, "/mat3", "/mat3", K_FN, 0 }, { "det", "det(", 0, K_FN, 0 },
    { "inv", "inv(", 0, K_FN, 0 }, { "trn", "tran(", 0, K_FN, 0 }, { 0, "/brack", "/brack", K_FN, 0 },

    { "<", "<", 0, K_FN, 0 }, { ">", ">", 0, K_FN, 0 }, { "\xE2\x89\xA4", "\xE2\x89\xA4", 0, K_FN, 0 },
    { "\xE2\x89\xA5", "\xE2\x89\xA5", 0, K_FN, 0 }, { "\xE2\x89\xA0", "\xE2\x89\xA0", 0, K_FN, 0 }, { ":=", ":=", 0, K_FN, 0 },

    { 0, "/sub", "x|/sub", K_FN, 0 }, { "'", "'", 0, K_FN, 0 }, { "fsolve", "fsolve(", 0, K_FN, 0 },
    { N_("media"), "mean(", 0, K_FN, 0 }, { N_("desv"), "stddev(", 0, K_FN, 0 }, { "desolve", "desolve(", 0, K_FN, 0 },
};

static const kp_key_t PAGE_ABC[] = {
    { "q", "q", 0, K_NUM, 0 }, { "w", "w", 0, K_NUM, 0 }, { "e", "e", 0, K_NUM, 0 }, { "r", "r", 0, K_NUM, 0 },
    { "t", "t", 0, K_NUM, 0 }, { "y", "y", 0, K_NUM, 0 }, { "u", "u", 0, K_NUM, 0 }, { "i", "i", 0, K_NUM, 0 },
    { "o", "o", 0, K_NUM, 0 }, { "p", "p", 0, K_NUM, 0 },

    { "a", "a", 0, K_NUM, 0 }, { "s", "s", 0, K_NUM, 0 }, { "d", "d", 0, K_NUM, 0 }, { "f", "f", 0, K_NUM, 0 },
    { "g", "g", 0, K_NUM, 0 }, { "h", "h", 0, K_NUM, 0 }, { "j", "j", 0, K_NUM, 0 }, { "k", "k", 0, K_NUM, 0 },
    { "l", "l", 0, K_NUM, 0 }, { BS, "/bs", 0, K_DEL, 0 },

    { "\xE2\x87\xA7", "/shift", 0, K_SHIFT, 0 }, { "z", "z", 0, K_NUM, 0 }, { "x", "x", 0, K_NUM, 0 },
    { "c", "c", 0, K_NUM, 0 }, { "v", "v", 0, K_NUM, 0 }, { "b", "b", 0, K_NUM, 0 }, { "n", "n", 0, K_NUM, 0 },
    { "m", "m", 0, K_NUM, 0 }, { "_", "_", 0, K_NUM, 0 }, { "\"", "\"", 0, K_NUM, 0 },

    { "(", "(", 0, K_FN, 0 }, { ")", ")", 0, K_FN, 0 }, { ",", ",", 0, K_FN, 0 }, { ":=", ":=", 0, K_FN, 0 },
    { "=", "=", 0, K_FN, 0 }, { "f(x)", "f(", 0, K_FN, 2 }, { ENTER, "/enter", 0, K_ENTER, 3 },
};

typedef struct {
    const kp_key_t *keys;
    int             n, cols;
    const char     *tab;
} kp_page_t;

static const kp_page_t PAGES[] = {
    { PAGE_MAIN, sizeof PAGE_MAIN / sizeof PAGE_MAIN[0], 6, "123" },
    { PAGE_FUNC, sizeof PAGE_FUNC / sizeof PAGE_FUNC[0], 6, "f(x)" },
    { PAGE_CALC, sizeof PAGE_CALC / sizeof PAGE_CALC[0], 6, "\xE2\x88\xAB \xCE\xA3" },
    { PAGE_ABC,  sizeof PAGE_ABC / sizeof PAGE_ABC[0], 10, "abc" },
};
#define NPAGES (int)(sizeof PAGES / sizeof PAGES[0])

static const char *const ARROWS[][2] = {
    { "\xE2\x86\x90", "/left" }, { "\xE2\x86\x92", "/right" },
    { "\xE2\x86\x91", "/up" }, { "\xE2\x86\x93", "/down" },
};

typedef struct {
    lv_obj_t       *root, *grid, *tabs[NPAGES];
    int             page;
    bool            shift;
    cas_keypad_cb_t cb;
    void           *user;
} kp_t;

static const int32_t COLS6[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                 LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
static const int32_t COLS10[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                  LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                  LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
static const int32_t ROWS5[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                 LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
static const int32_t ROWS4[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                 LV_GRID_TEMPLATE_LAST };

static lv_color_t key_bg(int kind)
{
    switch (kind) {
    case K_NUM:   return lv_color_hex(0x3A3A3C);
    case K_ENTER: return AOS_C_ACCENT;
    default:      return AOS_C_CARD2;
    }
}

static lv_color_t key_fg(int kind)
{
    switch (kind) {
    case K_OP:  return AOS_C_ORANGE;
    case K_DEL: return lv_color_hex(0xFF9F8A);
    default:    return AOS_C_TEXT;
    }
}

static void build_page(kp_t *k);

static void key_cb(lv_event_t *e)
{
    kp_t *k = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    const kp_key_t *key = lv_obj_get_user_data(b);
    if (!key) {
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED_REPEAT && key->kind != K_DEL) {
        return;
    }
    if (key->kind == K_SHIFT) {
        k->shift = !k->shift;
        build_page(k);
        return;
    }
    const char *s = key->key;
    char up[8];
    if (k->shift && k->page == 3 && s[0] >= 'a' && s[0] <= 'z' && !s[1]) {
        up[0] = (char)(s[0] - 32);
        up[1] = 0;
        s = up;
        k->shift = false;
        build_page(k);
    }
    k->cb(s, k->user);
}

static void sample_label(lv_obj_t *btn, const char *sample)
{
    mx_ed_t ed;
    mx_ed_init(&ed);
    char buf[64];
    strncpy(buf, sample, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *p = buf, *q; p && *p; p = q) {
        q = strchr(p, '|');
        if (q) {
            *q++ = 0;
        }
        mx_ed_key(&ed, p);
    }
    lv_obj_t *v = mx_view_create(btn, 24);
    mx_row_t *r = ed.root;
    ed.root = NULL;
    mx_ed_free(&ed);
    mx_view_set_row(v, r);
    mx_view_set_placeholders(v, true);      /* the empty boxes, as in the editor */
    mx_view_set_color(v, AOS_C_TEXT);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(v);
}

static void build_page(kp_t *k)
{
    lv_obj_clean(k->grid);
    const kp_page_t *pg = &PAGES[k->page];
    bool ten = pg->cols == 10;
    lv_obj_set_grid_dsc_array(k->grid, ten ? COLS10 : COLS6, ten ? ROWS4 : ROWS5);
    int col = 0, row = 0;
    for (int i = 0; i < pg->n; i++) {
        const kp_key_t *key = &pg->keys[i];
        int span = key->span ? key->span : 1;
        lv_obj_t *b = lv_button_create(k->grid);
        lv_obj_set_user_data(b, (void *)key);
        lv_obj_set_grid_cell(b, LV_GRID_ALIGN_STRETCH, col, span, LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_set_style_bg_color(b, key_bg(key->kind), 0);
        if (key->kind == K_SHIFT && k->shift) {
            lv_obj_set_style_bg_color(b, AOS_C_ACCENT, 0);
        }
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, key_cb, LV_EVENT_CLICKED, k);
        if (key->kind == K_DEL) {
            lv_obj_add_event_cb(b, key_cb, LV_EVENT_LONG_PRESSED_REPEAT, k);
        }
        if (key->label) {
            lv_obj_t *l = lv_label_create(b);
            const char *text = key->label;
            char up[8];
            if (k->shift && k->page == 3 && text[0] >= 'a' && text[0] <= 'z' && !text[1]) {
                up[0] = (char)(text[0] - 32);
                up[1] = 0;
                text = up;
            }
            if (text == key->label) {
                text = _(text);     /* the few that are words (mcd, media...) */
            }
            lv_label_set_text(l, mx_sym_shown(text, 28));
            int px = (key->kind == K_NUM && !ten) ? 32 : 28;
            if (strlen(text) > 5) {
                px = 24;
            }
            lv_obj_set_style_text_font(l, mx_font(px), 0);
            lv_obj_set_style_text_color(l, key_fg(key->kind), 0);
            lv_obj_center(l);
        } else if (key->sample) {
            sample_label(b, key->sample);
        }
        col += span;
        if (col >= pg->cols) {
            col = 0;
            row++;
        }
    }
    for (int i = 0; i < NPAGES; i++) {
        lv_obj_set_style_bg_color(k->tabs[i], i == k->page ? AOS_C_ACCENT : AOS_C_CARD, 0);
    }
}

static void tab_cb(lv_event_t *e)
{
    kp_t *k = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    int page = (int)(intptr_t)lv_obj_get_user_data(b);
    if (page != k->page) {
        k->page = page;
        k->shift = false;
        build_page(k);
    }
}

static void arrow_cb(lv_event_t *e)
{
    kp_t *k = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    k->cb((const char *)lv_obj_get_user_data(b), k->user);
}

static void delete_cb(lv_event_t *e)
{
    mx_free(lv_event_get_user_data(e));
}

static lv_obj_t *strip_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, LV_PCT(100));
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, mx_sym_shown(text, 24));
    lv_obj_set_style_text_font(l, mx_font(24), 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t *cas_keypad_create(lv_obj_t *parent, cas_keypad_cb_t cb, void *user)
{
    kp_t *k = mx_alloc(sizeof *k);
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 8, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    if (!k) {
        return root;
    }
    k->root = root;
    k->cb = cb;
    k->user = user;
    lv_obj_add_event_cb(root, delete_cb, LV_EVENT_DELETE, k);

    lv_obj_t *strip = lv_obj_create(root);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, LV_PCT(100), 52);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(strip, 6, 0);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < NPAGES; i++) {
        k->tabs[i] = strip_button(strip, PAGES[i].tab);
        lv_obj_set_user_data(k->tabs[i], (void *)(intptr_t)i);
        lv_obj_add_event_cb(k->tabs[i], tab_cb, LV_EVENT_CLICKED, k);
    }
    for (size_t i = 0; i < sizeof ARROWS / sizeof ARROWS[0]; i++) {
        lv_obj_t *b = strip_button(strip, ARROWS[i][0]);
        lv_obj_set_user_data(b, (void *)ARROWS[i][1]);
        lv_obj_add_event_cb(b, arrow_cb, LV_EVENT_CLICKED, k);
        lv_obj_add_event_cb(b, arrow_cb, LV_EVENT_LONG_PRESSED_REPEAT, k);
    }

    k->grid = lv_obj_create(root);
    lv_obj_remove_style_all(k->grid);
    lv_obj_set_width(k->grid, LV_PCT(100));
    lv_obj_set_flex_grow(k->grid, 1);
    lv_obj_set_layout(k->grid, LV_LAYOUT_GRID);
    lv_obj_set_style_pad_row(k->grid, 8, 0);
    lv_obj_set_style_pad_column(k->grid, 8, 0);
    lv_obj_remove_flag(k->grid, LV_OBJ_FLAG_SCROLLABLE);
    build_page(k);
    return root;
}

bool cas_hwkey_str(uint32_t key, uint8_t mods, char out[8])
{
    (void)mods;
    const char *s = NULL;
    switch (key) {
    case AOS_KEY_ENTER:     s = "/enter"; break;
    case AOS_KEY_BACKSPACE: s = "/bs"; break;
    case AOS_KEY_LEFT:      s = "/left"; break;
    case AOS_KEY_RIGHT:     s = "/right"; break;
    case AOS_KEY_UP:        s = "/up"; break;
    case AOS_KEY_DOWN:      s = "/down"; break;
    case AOS_KEY_HOME:      s = "/home"; break;
    case AOS_KEY_END:       s = "/end"; break;
    case AOS_KEY_ESC:       s = "/clear"; break;
    default: break;
    }
    if (s) {
        strcpy(out, s);
        return true;
    }
    if (key < 0x20 || key == 0x7F || key == ' ' || key >= AOS_KEY_CONSUMER) {
        return false;
    }
    /* the character itself, in UTF-8: mx_ed_key() turns "/" into a
     * fraction, "^" into an exponent, "*" and "-" into × and − */
    if (key < 0x80) {
        out[0] = (char)key;
        out[1] = 0;
    } else if (key < 0x800) {
        out[0] = (char)(0xC0 | (key >> 6));
        out[1] = (char)(0x80 | (key & 63));
        out[2] = 0;
    } else if (key < 0x10000) {
        out[0] = (char)(0xE0 | (key >> 12));
        out[1] = (char)(0x80 | ((key >> 6) & 63));
        out[2] = (char)(0x80 | (key & 63));
        out[3] = 0;
    } else {
        return false;
    }
    return true;
}
