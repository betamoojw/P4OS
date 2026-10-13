/*
 * P4OS - Python: the console's keyboard.
 *
 * One button matrix for the keys (LVGL draws it as one object, however
 * many keys) whose map changes with the page: lower case, upper case (it
 * stays until "abc": constants are written in capitals), and the digits
 * and signs.
 * Above it, a row of words that slides sideways, and Tab and the history.
 */
#include "p4py_kbd.h"

#include "aos_mono.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <string.h>

enum { PG_LOWER, PG_UPPER, PG_SIGNS };

typedef struct {
    lv_obj_t *bm;
    kbd_cb_t  cb;
    void     *user;
    int       page;
} kbd_t;

#define BS     LV_SYMBOL_BACKSPACE
#define ENTER  LV_SYMBOL_NEW_LINE
#define LEFT   LV_SYMBOL_LEFT
#define RIGHT  LV_SYMBOL_RIGHT

static const char *const MAP_LOWER[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", BS, "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", ":", ENTER, "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", ",", ".", "_", "\n",
    "123", "(", ")", "\"", "'", "=", " ", "#", LEFT, RIGHT, "",
};
static const char *const MAP_UPPER[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", BS, "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", ":", ENTER, "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "_", "\n",
    "123", "(", ")", "\"", "'", "=", " ", "#", LEFT, RIGHT, "",
};
static const char *const MAP_SIGNS[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", BS, "\n",
    "+", "-", "*", "/", "%", "=", "<", ">", "!", "?", ENTER, "\n",
    "[", "]", "{", "}", ":", ";", ",", ".", "\"", "'", "_", "\n",
    "abc", "(", ")", "@", "&", "|", " ", "^", "~", "\\", LEFT, RIGHT, "",
};

/* widths: the space bar wider, the action keys a bit */
static const lv_buttonmatrix_ctrl_t CTRL_LETTERS[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5 | LV_BUTTONMATRIX_CTRL_CHECKED,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5 | LV_BUTTONMATRIX_CTRL_CHECKED,
    5 | LV_BUTTONMATRIX_CTRL_CHECKED, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    5 | LV_BUTTONMATRIX_CTRL_CHECKED, 4, 4, 4, 4, 4, 10, 4, 4, 4,
};
static const lv_buttonmatrix_ctrl_t CTRL_SIGNS[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5 | LV_BUTTONMATRIX_CTRL_CHECKED,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5 | LV_BUTTONMATRIX_CTRL_CHECKED,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    5 | LV_BUTTONMATRIX_CTRL_CHECKED, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4,
};

static void set_page(kbd_t *k, int page)
{
    k->page = page;
    if (page == PG_SIGNS) {
        lv_buttonmatrix_set_map(k->bm, MAP_SIGNS);
        lv_buttonmatrix_set_ctrl_map(k->bm, CTRL_SIGNS);
    } else {
        lv_buttonmatrix_set_map(k->bm, page == PG_UPPER ? MAP_UPPER : MAP_LOWER);
        lv_buttonmatrix_set_ctrl_map(k->bm, CTRL_LETTERS);
    }
}

static void bm_cb(lv_event_t *e)
{
    kbd_t *k = lv_event_get_user_data(e);
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *t = lv_buttonmatrix_get_button_text(bm, id);
    if (!t) return;
    if (!strcmp(t, "ABC")) {
        set_page(k, PG_UPPER);
        return;
    }
    if (!strcmp(t, "abc")) {
        set_page(k, PG_LOWER);
        return;
    }
    if (!strcmp(t, "123")) {
        set_page(k, PG_SIGNS);
        return;
    }
    if (!strcmp(t, BS)) k->cb(k->user, "/bs");
    else if (!strcmp(t, ENTER)) k->cb(k->user, "/enter");
    else if (!strcmp(t, LEFT)) k->cb(k->user, "/left");
    else if (!strcmp(t, RIGHT)) k->cb(k->user, "/right");
    else k->cb(k->user, t);
}

/* the bar of words: a key's text is what it types */
static const char *const WORDS[][2] = {
    { "Tab", "/tab" }, { LV_SYMBOL_UP, "/up" }, { LV_SYMBOL_DOWN, "/down" },
    { "print(", "print(" }, { "def", "def " }, { "for", "for " }, { "in", " in " },
    { "range(", "range(" }, { "if", "if " }, { "elif", "elif " }, { "else:", "else:" },
    { "while", "while " }, { "return", "return " }, { "import", "import " },
    { "len(", "len(" }, { "lambda", "lambda " }, { "True", "True" }, { "False", "False" },
    { "None", "None" }, { "and", " and " }, { "or", " or " }, { "not", "not " },
    { "class", "class " }, { "self", "self" }, { "try:", "try:" }, { "except", "except " },
    { "with", "with " }, { "as", " as " }, { "f\"", "f\"" }, { "math.", "math." },
};

static void word_cb(lv_event_t *e)
{
    kbd_t *k = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_current_target(e);
    const char *t = lv_obj_get_user_data(b);
    if (t) k->cb(k->user, t);
}

static void free_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

lv_obj_t *kbd_create(lv_obj_t *parent, bool landscape, kbd_cb_t cb, void *user)
{
    kbd_t *k = lv_malloc_zeroed(sizeof *k);
    k->cb = cb;
    k->user = user;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 6, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, free_cb, LV_EVENT_DELETE, k);

    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 52);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_set_scroll_dir(bar, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++) {
        lv_obj_t *b = lv_button_create(bar);
        lv_obj_set_height(b, 50);
        lv_obj_set_style_pad_hor(b, 14, 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, i < 3 ? AOS_C_CARD2 : lv_color_hex(0x1D2B3A), 0);
        lv_obj_set_user_data(b, (void *)WORDS[i][1]);
        lv_obj_add_event_cb(b, word_cb, LV_EVENT_CLICKED, k);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, WORDS[i][0]);
        /* the arrows are the system font's symbols; the words, Python's */
        lv_obj_set_style_text_font(l, i < 3 ? aos_font_small : &aos_mono_22, 0);
        lv_obj_set_style_text_color(l, i < 3 ? AOS_C_TEXT : lv_color_hex(0x64D2FF), 0);
        lv_obj_center(l);
    }

    k->bm = lv_buttonmatrix_create(root);
    /* lying down the keyboard has the screen's height beside the console */
    lv_obj_set_size(k->bm, LV_PCT(100), landscape ? 520 : 290);
    lv_obj_set_style_bg_opa(k->bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(k->bm, 0, 0);
    lv_obj_set_style_pad_all(k->bm, 0, 0);
    lv_obj_set_style_pad_gap(k->bm, 6, 0);
    lv_obj_set_style_bg_color(k->bm, AOS_C_CARD, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(k->bm, AOS_C_CARD2, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(k->bm, AOS_C_ACCENT, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(k->bm, AOS_C_TEXT, LV_PART_ITEMS);
    /* the system font: it has the symbols of the action keys too */
    lv_obj_set_style_text_font(k->bm, aos_font_small, LV_PART_ITEMS);
    lv_obj_set_style_radius(k->bm, 10, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(k->bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(k->bm, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(k->bm, bm_cb, LV_EVENT_VALUE_CHANGED, k);
    set_page(k, PG_LOWER);
    return root;
}
