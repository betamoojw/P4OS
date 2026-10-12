/*
 * P4OS - CAS: a widget that shows a tree of boxes.
 *
 * Two uses: a result (the view owns its row and sizes itself to it) and the
 * editor (the row is the editor's, a cursor blinks in it and a tap places
 * the cursor). Drawing happens in the object's own draw event, so a long
 * history of results costs only what is on screen.
 */
#include "mx_view.h"

#include "aos_theme.h"

#include <string.h>

typedef struct {
    mx_row_t   *row;            /* owned unless ed */
    mx_ed_t    *ed;
    mx_style_t  st;
    int         px;
    int         pad;
    bool        right;          /* aligned right in its width        */
    bool        blink_on;
    bool        focused;
    lv_timer_t *blink;
    mx_view_tap_cb_t tap_cb;
    void       *tap_user;
} view_t;

static view_t *get(lv_obj_t *obj)
{
    return (view_t *)lv_obj_get_user_data(obj);
}

static void origin(lv_obj_t *obj, const view_t *v, int *ox, int *oy)
{
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const mx_row_t *r = v->ed ? v->ed->root : v->row;
    int w = lv_area_get_width(&a);
    *ox = a.x1 + v->pad;
    if (v->right && r && r->w + 2 * v->pad < w) {
        *ox = a.x2 - v->pad - r->w;
    }
    *oy = a.y1 + v->pad;
}

static void draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    view_t *v = get(obj);
    if (!v) {
        return;
    }
    const mx_row_t *r = v->ed ? v->ed->root : v->row;
    if (!r) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    int ox, oy;
    origin(obj, v, &ox, &oy);
    mx_draw(layer, r, ox, oy, &v->st);
    if (v->ed && v->focused && v->blink_on) {
        mx_draw_cursor(layer, &v->ed->cur, ox, oy, v->st.accent);
    }
}

static void blink_cb(lv_timer_t *t)
{
    lv_obj_t *obj = lv_timer_get_user_data(t);
    view_t *v = get(obj);
    if (v && v->focused) {
        v->blink_on = !v->blink_on;
        lv_obj_invalidate(obj);
    }
}

static void click_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    view_t *v = get(obj);
    if (!v) {
        return;
    }
    if (v->ed) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        int ox, oy;
        origin(obj, v, &ox, &oy);
        mx_ed_tap(v->ed, p.x - ox, p.y - oy);
        v->blink_on = true;
        lv_obj_invalidate(obj);
    }
    if (v->tap_cb) {
        v->tap_cb(obj, v->tap_user);
    }
}

static void delete_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    view_t *v = get(obj);
    if (!v) {
        return;
    }
    if (v->blink) {
        lv_timer_delete(v->blink);
    }
    if (!v->ed) {
        mx_row_free(v->row);
    }
    lv_obj_set_user_data(obj, NULL);
    mx_free(v);
}

lv_obj_t *mx_view_create(lv_obj_t *parent, int px)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    view_t *v = mx_alloc(sizeof *v);
    if (!v) {
        return obj;
    }
    v->px = px;
    v->pad = px / 4;
    v->st.text = AOS_C_TEXT;
    v->st.dim = AOS_C_DIM;
    v->st.accent = AOS_C_ACCENT;
    v->st.placeholder = AOS_C_DIM;
    lv_obj_set_user_data(obj, v);
    lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(obj, click_cb, LV_EVENT_CLICKED, NULL);
    return obj;
}

static void fit(lv_obj_t *obj, view_t *v)
{
    mx_row_t *r = v->ed ? v->ed->root : v->row;
    if (!r) {
        return;
    }
    mx_layout(r, v->px);
    lv_obj_set_height(obj, r->asc + r->dsc + 2 * v->pad);
    if (!v->ed || lv_obj_get_style_width(obj, 0) != LV_PCT(100)) {
        lv_obj_set_width(obj, r->w + 2 * v->pad + (v->ed ? v->px : 0));
    }
    lv_obj_invalidate(obj);
}

void mx_view_set_row(lv_obj_t *obj, mx_row_t *row)
{
    view_t *v = get(obj);
    if (!v) {
        mx_row_free(row);
        return;
    }
    if (!v->ed) {
        mx_row_free(v->row);
    }
    v->ed = NULL;
    v->row = row;
    fit(obj, v);
}

void mx_view_set_editor(lv_obj_t *obj, mx_ed_t *ed)
{
    view_t *v = get(obj);
    if (!v) {
        return;
    }
    if (!v->ed) {
        mx_row_free(v->row);
        v->row = NULL;
    }
    v->ed = ed;
    v->st.edit = true;
    v->focused = true;
    v->blink_on = true;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    if (!v->blink) {
        v->blink = lv_timer_create(blink_cb, 530, obj);
    }
    fit(obj, v);
}

void mx_view_refresh(lv_obj_t *obj)
{
    view_t *v = get(obj);
    if (v) {
        v->blink_on = true;
        fit(obj, v);
    }
}

void mx_view_set_color(lv_obj_t *obj, lv_color_t text)
{
    view_t *v = get(obj);
    if (v) {
        v->st.text = text;
        lv_obj_invalidate(obj);
    }
}

void mx_view_set_align_right(lv_obj_t *obj, bool right)
{
    view_t *v = get(obj);
    if (v) {
        v->right = right;
        lv_obj_invalidate(obj);
    }
}

void mx_view_set_focus(lv_obj_t *obj, bool focused)
{
    view_t *v = get(obj);
    if (v) {
        v->focused = focused;
        v->blink_on = focused;
        lv_obj_invalidate(obj);
    }
}

void mx_view_set_placeholders(lv_obj_t *obj, bool show)
{
    view_t *v = get(obj);
    if (v) {
        v->st.edit = show;
        lv_obj_invalidate(obj);
    }
}

void mx_view_on_tap(lv_obj_t *obj, mx_view_tap_cb_t cb, void *user)
{
    view_t *v = get(obj);
    if (v) {
        v->tap_cb = cb;
        v->tap_user = user;
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
}

const mx_row_t *mx_view_row(lv_obj_t *obj)
{
    view_t *v = get(obj);
    return v ? (v->ed ? v->ed->root : v->row) : NULL;
}

int mx_view_cursor_x(lv_obj_t *obj)
{
    view_t *v = get(obj);
    if (!v || !v->ed || !v->ed->cur.row) {
        return 0;
    }
    const mx_cur_t *c = &v->ed->cur;
    const mx_row_t *r = c->row;
    int x = c->pos < r->n ? r->it[c->pos]->x : r->x + r->w;
    return x + v->pad;
}
