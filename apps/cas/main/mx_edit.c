/*
 * P4OS - CAS: editing the tree of boxes.
 *
 * Keys are strings (mx.h). Besides tokens:
 *
 *   "sin("            a function: its name and a pair of parentheses
 *   "@π" "@e" "@i"    a constant: a name of its own (never glued to x)
 *   "/frac"           a fraction; what is right before the cursor (a number,
 *                     a name, a bracket) becomes its numerator, as in Desmos
 *   "/pow" "/sq"      an exponent (empty, or 2) on what is before
 *   "/sqrt" "/root" "/sub" "/paren" "/abs" "/brack" "/set"
 *   "/int" "/defint" "/sum" "/prod" "/deriv" "/limit" "/exp" "/mat2" "/mat3"
 *   "/left" "/right" "/up" "/down" "/bs" "/clear"
 *
 * Backspace on a structure goes into it rather than deleting it whole; at
 * the start of a structure's first box it takes the structure off and keeps
 * what was inside.
 */
#include "mx.h"

#include <stdlib.h>
#include <string.h>

void mx_ed_init(mx_ed_t *e)
{
    e->root = mx_row_new();
    e->cur.row = e->root;
    e->cur.pos = 0;
}

void mx_ed_free(mx_ed_t *e)
{
    mx_row_free(e->root);
    e->root = NULL;
    e->cur.row = NULL;
}

void mx_ed_clear(mx_ed_t *e)
{
    mx_row_clear(e->root);
    e->cur.row = e->root;
    e->cur.pos = 0;
}

void mx_ed_set_row(mx_ed_t *e, mx_row_t *r)
{
    mx_row_free(e->root);
    e->root = r ? r : mx_row_new();
    e->root->owner = NULL;
    e->cur.row = e->root;
    e->cur.pos = e->root->n;
}

/* --- navigation order of a node's boxes ----------------------------------- */

static int nav_count(const mx_node_t *n)
{
    return n->nkid;
}

static int nav_kid(const mx_node_t *n, int i)
{
    /* an n-th root is entered by its index, which is drawn first */
    if (n->kind == MX_ROOT && n->nkid == 2) {
        return i == 0 ? 1 : 0;
    }
    return i;
}

static int nav_index(const mx_node_t *n, const mx_row_t *r)
{
    for (int i = 0; i < nav_count(n); i++) {
        if (n->kid[nav_kid(n, i)] == r) {
            return i;
        }
    }
    return -1;
}

static int index_in(const mx_row_t *r, const mx_node_t *n)
{
    for (int i = 0; i < r->n; i++) {
        if (r->it[i] == n) {
            return i;
        }
    }
    return -1;
}

static void go_end(mx_ed_t *e, mx_row_t *r)
{
    e->cur.row = r;
    e->cur.pos = r->n;
}

static void go_start(mx_ed_t *e, mx_row_t *r)
{
    e->cur.row = r;
    e->cur.pos = 0;
}

static void move_right(mx_ed_t *e)
{
    mx_row_t *r = e->cur.row;
    if (e->cur.pos < r->n) {
        mx_node_t *n = r->it[e->cur.pos];
        if (n->nkid) {
            go_start(e, n->kid[nav_kid(n, 0)]);
        } else {
            e->cur.pos++;
        }
        return;
    }
    mx_node_t *o = r->owner;
    if (!o) {
        return;
    }
    int i = nav_index(o, r);
    if (i >= 0 && i + 1 < nav_count(o)) {
        go_start(e, o->kid[nav_kid(o, i + 1)]);
    } else {
        e->cur.row = o->parent;
        e->cur.pos = index_in(o->parent, o) + 1;
    }
}

static void move_left(mx_ed_t *e)
{
    mx_row_t *r = e->cur.row;
    if (e->cur.pos > 0) {
        mx_node_t *n = r->it[e->cur.pos - 1];
        if (n->nkid) {
            go_end(e, n->kid[nav_kid(n, nav_count(n) - 1)]);
        } else {
            e->cur.pos--;
        }
        return;
    }
    mx_node_t *o = r->owner;
    if (!o) {
        return;
    }
    int i = nav_index(o, r);
    if (i > 0) {
        go_end(e, o->kid[nav_kid(o, i - 1)]);
    } else {
        e->cur.row = o->parent;
        e->cur.pos = index_in(o->parent, o);
    }
}

static int cursor_x(const mx_cur_t *c)
{
    const mx_row_t *r = c->row;
    if (c->pos < r->n) {
        return r->it[c->pos]->x;
    }
    return r->x + r->w;
}

static int pos_at_x(const mx_row_t *r, int x)
{
    for (int i = 0; i < r->n; i++) {
        if (x < r->it[i]->x + r->it[i]->w / 2) {
            return i;
        }
    }
    return r->n;
}

/* Up and down by the layout: the box of the nearest enclosing structure that
 * is above (or below) the cursor's box. */
static void move_vert(mx_ed_t *e, int dir)
{
    int x = cursor_x(&e->cur);
    mx_row_t *r = e->cur.row;
    while (r->owner) {
        mx_node_t *o = r->owner;
        mx_row_t *best = NULL;
        int bestd = 1 << 30;
        for (int i = 0; i < o->nkid; i++) {
            mx_row_t *k = o->kid[i];
            int dy = (k->y - r->y) * dir;
            if (k == r || dy <= 0) {
                continue;
            }
            int dx = abs(x - (k->x + k->w / 2));
            int d = dy * 4 + dx;
            if (d < bestd) {
                bestd = d;
                best = k;
            }
        }
        if (best) {
            e->cur.row = best;
            e->cur.pos = pos_at_x(best, x);
            return;
        }
        r = o->parent;
    }
}

/* --- inserting ------------------------------------------------------------ */

static void put(mx_ed_t *e, mx_node_t *n)
{
    mx_row_insert(e->cur.row, e->cur.pos, n);
    e->cur.pos++;
}

static void put_into(mx_ed_t *e, mx_node_t *n, int kid)
{
    mx_row_insert(e->cur.row, e->cur.pos, n);
    go_start(e, n->kid[kid]);
}

static bool is_operand(const mx_node_t *n)
{
    if (n->kind != MX_SYM) {
        return true;
    }
    if (n->flags & (MX_F_OP | MX_F_TEXT)) {
        return false;
    }
    return strcmp(n->text, ",") != 0;
}

/* The operand right before the cursor, for a fraction: back to the
 * previous operator. A function's name goes with its parentheses. */
static int operand_start(const mx_row_t *r, int pos)
{
    int i = pos;
    while (i > 0 && is_operand(r->it[i - 1])) {
        i--;
    }
    return i;
}

static const char *const OPS[] = {
    "+", "-", "\xE2\x88\x92", "\xC3\x97", "\xC2\xB7", "\xC3\xB7", "*", "/", "=",
    "<", ">", "\xE2\x89\xA4", "\xE2\x89\xA5", "\xE2\x89\xA0", ",", ":=",
    "\xE2\x86\x92", "==", "!=", "<=", ">=",
};

static bool is_op(const char *k)
{
    for (size_t i = 0; i < sizeof OPS / sizeof OPS[0]; i++) {
        if (!strcmp(k, OPS[i])) {
            return true;
        }
    }
    return false;
}

static void insert_mat(mx_ed_t *e, int rows, int cols)
{
    mx_node_t *n = mx_node_new(MX_MAT, rows * cols);
    if (!n) {
        return;
    }
    n->rows = (uint8_t)rows;
    n->cols = (uint8_t)cols;
    put_into(e, n, 0);
}

static void template_key(mx_ed_t *e, const char *k)
{
    mx_row_t *r = e->cur.row;
    mx_node_t *n = NULL;

    if (!strcmp(k, "frac")) {
        n = mx_node_new(MX_FRAC, 2);
        if (!n) return;
        int from = operand_start(r, e->cur.pos);
        int count = e->cur.pos - from;
        for (int i = 0; i < count; i++) {
            mx_row_insert(n->kid[0], n->kid[0]->n, mx_row_take(r, from));
        }
        e->cur.pos = from;
        put_into(e, n, count ? 1 : 0);
    } else if (!strcmp(k, "pow")) {
        n = mx_node_new(MX_POW, 1);
        if (n) put_into(e, n, 0);
    } else if (!strcmp(k, "sq")) {
        n = mx_node_new(MX_POW, 1);
        if (!n) return;
        mx_row_insert(n->kid[0], 0, mx_sym("2", 0));
        put(e, n);
    } else if (!strcmp(k, "inv")) {
        n = mx_node_new(MX_POW, 1);
        if (!n) return;
        mx_row_insert(n->kid[0], 0, mx_sym("\xE2\x88\x92", MX_F_OP));
        mx_row_insert(n->kid[0], 1, mx_sym("1", 0));
        put(e, n);
    } else if (!strcmp(k, "exp")) {
        put(e, mx_sym("e", MX_F_ATOM));
        n = mx_node_new(MX_POW, 1);
        if (n) put_into(e, n, 0);
    } else if (!strcmp(k, "10pow")) {
        put(e, mx_sym("1", 0));
        put(e, mx_sym("0", 0));
        n = mx_node_new(MX_POW, 1);
        if (n) put_into(e, n, 0);
    } else if (!strcmp(k, "sqrt")) {
        n = mx_node_new(MX_ROOT, 1);
        if (n) put_into(e, n, 0);
    } else if (!strcmp(k, "root")) {
        n = mx_node_new(MX_ROOT, 2);
        if (n) put_into(e, n, 1);
    } else if (!strcmp(k, "sub")) {
        n = mx_node_new(MX_SUB, 1);
        if (n) put_into(e, n, 0);
    } else if (!strcmp(k, "paren") || !strcmp(k, "abs") || !strcmp(k, "brack") || !strcmp(k, "set")) {
        n = mx_node_new(MX_PAREN, 1);
        if (!n) return;
        n->text[0] = k[0] == 'p' ? '(' : k[0] == 'a' ? '|' : k[0] == 'b' ? '[' : '{';
        put_into(e, n, 0);
    } else if (!strcmp(k, "int")) {
        n = mx_node_new(MX_INT, 2);
        if (!n) return;
        mx_row_insert(n->kid[1], 0, mx_sym("x", 0));
        put_into(e, n, 0);
    } else if (!strcmp(k, "defint")) {
        n = mx_node_new(MX_INT, 4);
        if (!n) return;
        mx_row_insert(n->kid[3], 0, mx_sym("x", 0));
        put_into(e, n, 0);
    } else if (!strcmp(k, "sum") || !strcmp(k, "prod")) {
        n = mx_node_new(k[0] == 's' ? MX_SUM : MX_PROD, 4);
        if (!n) return;
        mx_row_insert(n->kid[0], 0, mx_sym("k", 0));
        put_into(e, n, 1);
    } else if (!strcmp(k, "deriv")) {
        n = mx_node_new(MX_DERIV, 2);
        if (!n) return;
        mx_row_insert(n->kid[0], 0, mx_sym("x", 0));
        put_into(e, n, 1);
    } else if (!strcmp(k, "limit")) {
        n = mx_node_new(MX_LIMIT, 3);
        if (!n) return;
        mx_row_insert(n->kid[0], 0, mx_sym("x", 0));
        put_into(e, n, 1);
    } else if (!strcmp(k, "mat2")) {
        insert_mat(e, 2, 2);
    } else if (!strcmp(k, "mat3")) {
        insert_mat(e, 3, 3);
    } else if (!strcmp(k, "left")) {
        move_left(e);
    } else if (!strcmp(k, "right")) {
        move_right(e);
    } else if (!strcmp(k, "up")) {
        move_vert(e, -1);
    } else if (!strcmp(k, "down")) {
        move_vert(e, 1);
    } else if (!strcmp(k, "home")) {
        go_start(e, e->root);
    } else if (!strcmp(k, "end")) {
        go_end(e, e->root);
    } else if (!strcmp(k, "clear")) {
        mx_ed_clear(e);
    }
}

static bool all_empty(const mx_node_t *n)
{
    for (int i = 0; i < n->nkid; i++) {
        if (n->kid[i]->n) {
            return false;
        }
    }
    return true;
}

/* Takes structure 'o' off its row and leaves what it had inside in its
 * place; the cursor goes where the structure was. */
static void unwrap(mx_ed_t *e, mx_node_t *o)
{
    mx_row_t *pr = o->parent;
    int at = index_in(pr, o);
    mx_row_take(pr, at);
    int pos = at;
    for (int i = 0; i < nav_count(o); i++) {
        mx_row_t *k = o->kid[nav_kid(o, i)];
        if (i && k->n && o->kind == MX_FRAC) {
            mx_row_insert(pr, pos++, mx_sym("\xC3\xB7", MX_F_OP));
        }
        int had = k->n;
        mx_row_append_row(pr, pos, k);
        pos += had;
    }
    mx_node_free(o);
    e->cur.row = pr;
    e->cur.pos = at;
}

static void backspace(mx_ed_t *e)
{
    mx_row_t *r = e->cur.row;
    if (e->cur.pos > 0) {
        mx_node_t *p = r->it[e->cur.pos - 1];
        if (!p->nkid || all_empty(p)) {
            mx_node_free(mx_row_take(r, e->cur.pos - 1));
            e->cur.pos--;
        } else {
            go_end(e, p->kid[nav_kid(p, nav_count(p) - 1)]);
        }
        return;
    }
    mx_node_t *o = r->owner;
    if (!o) {
        return;
    }
    if (all_empty(o)) {
        mx_row_t *pr = o->parent;
        int at = index_in(pr, o);
        mx_node_free(mx_row_take(pr, at));
        e->cur.row = pr;
        e->cur.pos = at;
        return;
    }
    int i = nav_index(o, r);
    if (i <= 0) {
        unwrap(e, o);
    } else {
        go_end(e, o->kid[nav_kid(o, i - 1)]);
    }
}

void mx_ed_key(mx_ed_t *e, const char *k)
{
    if (!k || !*k || !e->root) {
        return;
    }
    if (k[0] == '/' && k[1]) {
        if (!strcmp(k + 1, "bs")) {
            backspace(e);
        } else {
            template_key(e, k + 1);
        }
        return;
    }
    if (k[0] == '@' && k[1]) {
        put(e, mx_sym(k + 1, MX_F_ATOM));
        return;
    }
    size_t len = strlen(k);
    if (len > 1 && k[len - 1] == '(') {
        char name[16];
        size_t n = len - 1 < sizeof name - 1 ? len - 1 : sizeof name - 1;
        memcpy(name, k, n);
        name[n] = 0;
        put(e, mx_sym(name, MX_F_FUNC));
        mx_node_t *p = mx_node_new(MX_PAREN, 1);
        if (p) put_into(e, p, 0);
        return;
    }
    if (!strcmp(k, "(")) {
        template_key(e, "paren");
        return;
    }
    if (!strcmp(k, ")")) {
        /* out of the parentheses the cursor is in, if any */
        for (mx_row_t *r = e->cur.row; r && r->owner; r = r->owner->parent) {
            if (r->owner->kind == MX_PAREN) {
                e->cur.row = r->owner->parent;
                e->cur.pos = index_in(r->owner->parent, r->owner) + 1;
                return;
            }
        }
        return;
    }
    if (!strcmp(k, "/")) {
        template_key(e, "frac");
        return;
    }
    if (!strcmp(k, "^")) {
        template_key(e, "pow");
        return;
    }
    if (!strcmp(k, "-")) {
        put(e, mx_sym("\xE2\x88\x92", MX_F_OP));
        return;
    }
    if (!strcmp(k, "*")) {
        put(e, mx_sym("\xC3\x97", MX_F_OP));
        return;
    }
    put(e, mx_sym(k, is_op(k) ? MX_F_OP : 0));
}

void mx_ed_insert_row(mx_ed_t *e, const mx_row_t *r)
{
    mx_row_t *c = mx_row_copy(r);
    if (!c) {
        return;
    }
    int n = c->n;
    mx_row_append_row(e->cur.row, e->cur.pos, c);
    e->cur.pos += n;
    mx_row_free(c);
}

/* --- placing the cursor with a finger ------------------------------------ */

typedef struct {
    mx_row_t *row;
    int       d;
} hit_t;

static int row_dist(const mx_row_t *r, int x, int y)
{
    int top = r->y - r->asc, bot = r->y + r->dsc;
    int dx = x < r->x ? r->x - x : x > r->x + r->w ? x - (r->x + r->w) : 0;
    int dy = y < top ? top - y : y > bot ? y - bot : 0;
    return dx + dy * 2;
}

static void hit(mx_row_t *r, int x, int y, hit_t *best)
{
    int d = row_dist(r, x, y);
    /* a deeper box wins a tie: tapping on a numerator means the numerator */
    if (d <= best->d) {
        best->d = d;
        best->row = r;
    }
    for (int i = 0; i < r->n; i++) {
        mx_node_t *n = r->it[i];
        if (!n->nkid) {
            continue;
        }
        if (x < n->x - 8 || x > n->x + n->w + 8 || y < n->y - n->asc - 8 || y > n->y + n->dsc + 8) {
            continue;
        }
        for (int k = 0; k < n->nkid; k++) {
            hit(n->kid[k], x, y, best);
        }
    }
}

bool mx_ed_tap(mx_ed_t *e, int x, int y)
{
    hit_t best = { NULL, 1 << 30 };
    hit(e->root, x, y, &best);
    if (!best.row) {
        return false;
    }
    e->cur.row = best.row;
    e->cur.pos = pos_at_x(best.row, x);
    return true;
}
