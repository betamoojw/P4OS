/*
 * P4OS - CAS: the tree of boxes, and the text Giac reads from it.
 */
#include "mx.h"

#include <string.h>

/* --- rows and nodes ------------------------------------------------------- */

mx_row_t *mx_row_new(void)
{
    return mx_alloc(sizeof(mx_row_t));
}

void mx_row_clear(mx_row_t *r)
{
    for (int i = 0; i < r->n; i++) {
        mx_node_free(r->it[i]);
    }
    r->n = 0;
}

void mx_row_free(mx_row_t *r)
{
    if (!r) {
        return;
    }
    mx_row_clear(r);
    mx_free(r->it);
    mx_free(r);
}

bool mx_row_empty(const mx_row_t *r)
{
    return !r || r->n == 0;
}

void mx_row_insert(mx_row_t *r, int pos, mx_node_t *n)
{
    if (!n) {
        return;
    }
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 8;
        mx_node_t **it = mx_realloc(r->it, sizeof(*it) * (size_t)cap);
        if (!it) {
            mx_node_free(n);
            return;
        }
        r->it = it;
        r->cap = cap;
    }
    if (pos < 0 || pos > r->n) {
        pos = r->n;
    }
    memmove(r->it + pos + 1, r->it + pos, sizeof(*r->it) * (size_t)(r->n - pos));
    r->it[pos] = n;
    r->n++;
    n->parent = r;
}

mx_node_t *mx_row_take(mx_row_t *r, int pos)
{
    if (pos < 0 || pos >= r->n) {
        return NULL;
    }
    mx_node_t *n = r->it[pos];
    memmove(r->it + pos, r->it + pos + 1, sizeof(*r->it) * (size_t)(r->n - pos - 1));
    r->n--;
    n->parent = NULL;
    return n;
}

void mx_row_append_row(mx_row_t *dst, int pos, mx_row_t *src)
{
    while (src->n) {
        mx_node_t *n = mx_row_take(src, 0);
        mx_row_insert(dst, pos++, n);
    }
}

mx_node_t *mx_sym(const char *tok, uint8_t flags)
{
    mx_node_t *n = mx_alloc(sizeof *n);
    if (n) {
        n->kind = MX_SYM;
        n->flags = flags;
        strncpy(n->text, tok, sizeof n->text - 1);
    }
    return n;
}

mx_node_t *mx_node_new(mx_kind_t kind, int nkid)
{
    mx_node_t *n = mx_alloc(sizeof *n);
    if (!n) {
        return NULL;
    }
    n->kind = (uint8_t)kind;
    n->nkid = (uint8_t)nkid;
    if (nkid) {
        n->kid = mx_alloc(sizeof(mx_row_t *) * (size_t)nkid);
        for (int i = 0; i < nkid; i++) {
            n->kid[i] = mx_row_new();
            if (n->kid[i]) {
                n->kid[i]->owner = n;
            }
        }
    }
    if (kind == MX_PAREN) {
        n->text[0] = '(';
    }
    return n;
}

void mx_node_free(mx_node_t *n)
{
    if (!n) {
        return;
    }
    for (int i = 0; i < n->nkid; i++) {
        mx_row_free(n->kid[i]);
    }
    mx_free(n->kid);
    mx_free(n);
}

mx_row_t *mx_row_copy(const mx_row_t *r)
{
    mx_row_t *c = mx_row_new();
    for (int i = 0; c && i < r->n; i++) {
        mx_row_insert(c, c->n, mx_node_copy(r->it[i]));
    }
    return c;
}

mx_node_t *mx_node_copy(const mx_node_t *n)
{
    mx_node_t *c = mx_node_new((mx_kind_t)n->kind, 0);
    if (!c) {
        return NULL;
    }
    c->flags = n->flags;
    c->rows = n->rows;
    c->cols = n->cols;
    memcpy(c->text, n->text, sizeof c->text);
    if (n->nkid) {
        c->kid = mx_alloc(sizeof(mx_row_t *) * n->nkid);
        c->nkid = n->nkid;
        for (int i = 0; i < n->nkid; i++) {
            c->kid[i] = mx_row_copy(n->kid[i]);
            if (c->kid[i]) {
                c->kid[i]->owner = c;
            }
        }
    }
    return c;
}

/* --- the text for Giac ---------------------------------------------------- */

/* What a token says to Giac when it is drawn as something else. */
static const char *const SYM_MAP[][2] = {
    { "\xC3\x97", "*" },           /* × */
    { "\xC2\xB7", "*" },           /* · */
    { "\xC3\xB7", "/" },           /* ÷ */
    { "\xE2\x88\x92", "-" },       /* − */
    { "\xCF\x80", "pi" },          /* π */
    { "\xE2\x88\x9E", "inf" },     /* ∞ */
    { "\xE2\x89\xA4", "<=" },      /* ≤ */
    { "\xE2\x89\xA5", ">=" },      /* ≥ */
    { "\xE2\x89\xA0", "!=" },      /* ≠ */
    { "\xE2\x86\x92", "->" },      /* → */
    { "\xC2\xB0", "*pi/180" },     /* ° */
    { "\xCE\xB1", "alpha" }, { "\xCE\xB2", "beta" }, { "\xCE\xB3", "gamma" },
    { "\xCE\xB4", "delta" }, { "\xCE\xB5", "epsilon" }, { "\xCE\xB6", "zeta" },
    { "\xCE\xB7", "eta" }, { "\xCE\xB8", "theta" }, { "\xCE\xB9", "iota" },
    { "\xCE\xBA", "kappa" }, { "\xCE\xBB", "lambda" }, { "\xCE\xBC", "mu" },
    { "\xCE\xBD", "nu" }, { "\xCE\xBE", "xi" }, { "\xCF\x81", "rho" },
    { "\xCF\x83", "sigma" }, { "\xCF\x84", "tau" }, { "\xCF\x86", "phi" },
    { "\xCF\x87", "chi" }, { "\xCF\x88", "psi" }, { "\xCF\x89", "omega" },
    { "\xCE\x93", "Gamma" }, { "\xCE\x94", "Delta" }, { "\xCE\x98", "Theta" },
    { "\xCE\x9B", "Lambda" }, { "\xCE\xA3", "Sigma" }, { "\xCE\xA6", "Phi" },
    { "\xCE\xA8", "Psi" }, { "\xCE\xA9", "Omega" },
};

const char *mx_sym_giac(const char *tok)
{
    for (size_t i = 0; i < sizeof SYM_MAP / sizeof SYM_MAP[0]; i++) {
        if (!strcmp(tok, SYM_MAP[i][0])) {
            return SYM_MAP[i][1];
        }
    }
    return tok;
}

/* What an item looks like to its neighbours, for the implicit products:
 * "2x" is 2*x, "x(" is a call, ")(" is a product. */
enum { C_OP, C_NUM, C_ID, C_FUNC, C_VAL, C_OPEN };

static bool is_digit_tok(const char *t)
{
    return (t[0] >= '0' && t[0] <= '9') || t[0] == '.';
}

static bool is_alpha_tok(const char *t)
{
    unsigned char c = (unsigned char)t[0];
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

static int cls_start(const mx_node_t *n)
{
    switch (n->kind) {
    case MX_SYM:
        if (n->flags & (MX_F_OP | MX_F_TEXT)) return C_OP;
        if (n->flags & MX_F_FUNC) return C_FUNC;
        if (n->flags & MX_F_ATOM) return C_VAL;
        if (is_digit_tok(n->text)) return C_NUM;
        if (is_alpha_tok(n->text)) return C_ID;
        return C_OP;                                /* ! ' , and the like */
    case MX_POW:
    case MX_SUB:
        return C_OP;                                /* they stick to what is before */
    case MX_PAREN:
        return n->text[0] == '(' ? C_OPEN : C_VAL;
    default:
        return C_VAL;
    }
}

static int cls_end(const mx_node_t *n)
{
    if (n->kind == MX_SYM) {
        if (n->flags & (MX_F_OP | MX_F_TEXT)) return C_OP;
        if (n->flags & MX_F_FUNC) return C_FUNC;
        if (n->flags & MX_F_ATOM) return C_VAL;
        if (is_digit_tok(n->text)) return C_NUM;
        if (is_alpha_tok(n->text)) return C_ID;
        if (!strcmp(n->text, "!") || !strcmp(n->text, "'")) return C_VAL;
        return C_OP;
    }
    return C_VAL;
}

static bool needs_times(int a, int b)
{
    switch (a) {
    case C_NUM: return b == C_ID || b == C_FUNC || b == C_VAL || b == C_OPEN;
    case C_ID:  return b == C_FUNC || b == C_VAL;
    case C_VAL: return b == C_NUM || b == C_ID || b == C_FUNC || b == C_VAL || b == C_OPEN;
    default:    return false;
    }
}

static void row_text(mx_str_t *b, const mx_row_t *r);

static void kid_text(mx_str_t *b, const mx_node_t *n, int i)
{
    if (i < n->nkid) {
        row_text(b, n->kid[i]);
    }
}

static bool simple_sub(const mx_row_t *r)
{
    for (int i = 0; i < r->n; i++) {
        if (r->it[i]->kind != MX_SYM || (r->it[i]->flags & ~MX_F_ATOM) ||
            !(is_alpha_tok(r->it[i]->text) || is_digit_tok(r->it[i]->text))) {
            return false;
        }
    }
    return r->n > 0;
}

static void node_text(mx_str_t *b, const mx_node_t *n)
{
    switch (n->kind) {
    case MX_SYM:
        mx_str_add(b, mx_sym_giac(n->text));
        break;
    case MX_FRAC:
        mx_str_add(b, "((");
        kid_text(b, n, 0);
        mx_str_add(b, ")/(");
        kid_text(b, n, 1);
        mx_str_add(b, "))");
        break;
    case MX_ROOT:
        if (n->nkid > 1) {
            mx_str_add(b, "surd(");
            kid_text(b, n, 0);
            mx_str_add(b, ",");
            kid_text(b, n, 1);
            mx_str_add(b, ")");
        } else {
            mx_str_add(b, "sqrt(");
            kid_text(b, n, 0);
            mx_str_add(b, ")");
        }
        break;
    case MX_POW:
        mx_str_add(b, "^(");
        kid_text(b, n, 0);
        mx_str_add(b, ")");
        break;
    case MX_SUB:
        if (simple_sub(n->kid[0])) {
            mx_str_add(b, "_");
            kid_text(b, n, 0);
        } else {
            mx_str_add(b, "[");
            kid_text(b, n, 0);
            mx_str_add(b, "]");
        }
        break;
    case MX_PAREN:
        switch (n->text[0]) {
        case '[': mx_str_add(b, "[");     kid_text(b, n, 0); mx_str_add(b, "]"); break;
        case '{': mx_str_add(b, "set[");  kid_text(b, n, 0); mx_str_add(b, "]"); break;
        case '|': mx_str_add(b, "abs(");  kid_text(b, n, 0); mx_str_add(b, ")"); break;
        default:  mx_str_add(b, "(");     kid_text(b, n, 0); mx_str_add(b, ")"); break;
        }
        break;
    case MX_INT:
        mx_str_add(b, "integrate(");
        if (n->nkid == 4) {
            kid_text(b, n, 2);
            mx_str_add(b, ",");
            kid_text(b, n, 3);
            mx_str_add(b, ",");
            kid_text(b, n, 0);
            mx_str_add(b, ",");
            kid_text(b, n, 1);
        } else {
            kid_text(b, n, 0);
            mx_str_add(b, ",");
            kid_text(b, n, 1);
        }
        mx_str_add(b, ")");
        break;
    case MX_SUM:
    case MX_PROD:
        mx_str_add(b, n->kind == MX_SUM ? "sum(" : "product(");
        kid_text(b, n, 3);
        mx_str_add(b, ",");
        kid_text(b, n, 0);
        mx_str_add(b, ",");
        kid_text(b, n, 1);
        mx_str_add(b, ",");
        kid_text(b, n, 2);
        mx_str_add(b, ")");
        break;
    case MX_DERIV:
        mx_str_add(b, "diff(");
        kid_text(b, n, 1);
        mx_str_add(b, ",");
        kid_text(b, n, 0);
        mx_str_add(b, ")");
        break;
    case MX_LIMIT:
        mx_str_add(b, "limit(");
        kid_text(b, n, 2);
        mx_str_add(b, ",");
        kid_text(b, n, 0);
        mx_str_add(b, ",");
        kid_text(b, n, 1);
        mx_str_add(b, ")");
        break;
    case MX_MAT:
        mx_str_add(b, "[");
        for (int r = 0; r < n->rows; r++) {
            mx_str_add(b, r ? ",[" : "[");
            for (int c = 0; c < n->cols; c++) {
                if (c) {
                    mx_str_add(b, ",");
                }
                kid_text(b, n, r * n->cols + c);
            }
            mx_str_add(b, "]");
        }
        mx_str_add(b, "]");
        break;
    }
}

static void row_text(mx_str_t *b, const mx_row_t *r)
{
    for (int i = 0; i < r->n; i++) {
        if (i && needs_times(cls_end(r->it[i - 1]), cls_start(r->it[i]))) {
            mx_str_add(b, "*");
        }
        node_text(b, r->it[i]);
    }
}

char *mx_row_text(const mx_row_t *r)
{
    mx_str_t b = { 0 };
    row_text(&b, r);
    return mx_str_take(&b);
}
