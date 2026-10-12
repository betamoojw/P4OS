/*
 * P4OS - CAS: Giac's text into a tree, the tree into boxes and into code.
 *
 * Giac prints its results in linear Xcas syntax ("1/4*ln(abs(x-1))-
 * 1/2*atan(x)", "[[1,2],[3,4]]"). A small Pratt parser reads that back:
 *
 *   ,  :=  or  and  = == != < <= > >=  + -  * /  unary-  ^  !  '  call/index
 *
 * mx_ast_to_row() turns the tree into boxes the way a book would print it
 * (a/b stacked, 2*x as 2x, sqrt as a radical, exp(x) as e^x, lists of equal
 * lists as a matrix), with brackets only where precedence needs them.
 *
 * mx_compile() turns it into stack code over float for the grapher: about
 * a hundred times faster than asking Giac for each point, which a pan or a
 * pinch needs.
 */
#include "mx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --- tokens ---------------------------------------------------------------- */

enum { T_END, T_NUM, T_ID, T_STR, T_OP };

typedef struct {
    const char *p;
    int         type;
    char        tok[48];
    const char *err;
} lx_t;

static bool idchar(unsigned char c, bool first)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80 ||
           (!first && c >= '0' && c <= '9');
}

static void lx_next(lx_t *l)
{
    const char *p = l->p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    l->tok[0] = 0;
    if (!*p) {
        l->type = T_END;
        l->p = p;
        return;
    }
    size_t n = 0;
    unsigned char c = (unsigned char)*p;
    if ((c >= '0' && c <= '9') || (c == '.' && p[1] >= '0' && p[1] <= '9')) {
        while ((*p >= '0' && *p <= '9') || *p == '.') {
            if (n < sizeof l->tok - 1) l->tok[n++] = *p;
            p++;
        }
        if ((*p == 'e' || *p == 'E') &&
            ((p[1] >= '0' && p[1] <= '9') || ((p[1] == '-' || p[1] == '+') && p[2] >= '0' && p[2] <= '9'))) {
            if (n < sizeof l->tok - 1) l->tok[n++] = 'e';
            p++;
            if (*p == '-' || *p == '+') {
                if (n < sizeof l->tok - 1) l->tok[n++] = *p;
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                if (n < sizeof l->tok - 1) l->tok[n++] = *p;
                p++;
            }
        }
        l->type = T_NUM;
    } else if (idchar(c, true)) {
        while (idchar((unsigned char)*p, false)) {
            if (n < sizeof l->tok - 1) l->tok[n++] = *p;
            p++;
        }
        l->type = T_ID;
    } else if (c == '"') {
        p++;
        while (*p && *p != '"') {
            if (n < sizeof l->tok - 1) l->tok[n++] = *p;
            p++;
        }
        if (*p == '"') p++;
        l->type = T_STR;
    } else {
        static const char *const two[] = { ":=", "==", "!=", "<=", ">=", "->", "&&", "||", "=>" };
        l->type = T_OP;
        for (size_t i = 0; i < sizeof two / sizeof two[0]; i++) {
            if (p[0] == two[i][0] && p[1] == two[i][1]) {
                l->tok[n++] = p[0];
                l->tok[n++] = p[1];
                p += 2;
                goto done;
            }
        }
        l->tok[n++] = *p++;
    }
done:
    l->tok[n] = 0;
    l->p = p;
}

static bool is_op(const lx_t *l, const char *s)
{
    return l->type == T_OP && !strcmp(l->tok, s);
}

/* --- the tree -------------------------------------------------------------- */

static mx_ast_t *mk(int op, const char *s, int n)
{
    mx_ast_t *t = mx_alloc(sizeof *t);
    if (!t) return NULL;
    t->op = (uint8_t)op;
    t->n = (uint8_t)n;
    if (s) t->s = mx_strdup(s);
    if (n) t->a = mx_alloc(sizeof(mx_ast_t *) * (size_t)n);
    return t;
}

static mx_ast_t *mk2(int op, mx_ast_t *a, mx_ast_t *b)
{
    mx_ast_t *t = mk(op, NULL, 2);
    if (!t) return NULL;
    t->a[0] = a;
    t->a[1] = b;
    return t;
}

void mx_ast_free(mx_ast_t *t)
{
    if (!t) return;
    for (int i = 0; i < t->n; i++) {
        mx_ast_free(t->a[i]);
    }
    mx_free(t->a);
    mx_free(t->s);
    mx_free(t);
}

static void push_arg(mx_ast_t *t, mx_ast_t *a)
{
    mx_ast_t **v = mx_realloc(t->a, sizeof(*v) * (size_t)(t->n + 1));
    if (!v) {
        mx_ast_free(a);
        return;
    }
    t->a = v;
    t->a[t->n++] = a;
}

/* --- parser ----------------------------------------------------------------- */

static mx_ast_t *expr(lx_t *l, int rbp);

static int lbp(const lx_t *l)
{
    if (l->type == T_ID) {
        if (!strcmp(l->tok, "or")) return 30;
        if (!strcmp(l->tok, "and")) return 40;
        return 0;
    }
    if (l->type != T_OP) return 0;
    const char *t = l->tok;
    if (!strcmp(t, ":=")) return 20;
    if (!strcmp(t, "->")) return 25;
    if (!strcmp(t, "||")) return 30;
    if (!strcmp(t, "&&")) return 40;
    if (!strcmp(t, "=") || !strcmp(t, "==") || !strcmp(t, "!=") || !strcmp(t, "<") ||
        !strcmp(t, "<=") || !strcmp(t, ">") || !strcmp(t, ">=")) return 50;
    if (!strcmp(t, "+") || !strcmp(t, "-")) return 60;
    if (!strcmp(t, "*") || !strcmp(t, "/") || !strcmp(t, "%")) return 70;
    if (!strcmp(t, "^")) return 90;
    if (!strcmp(t, "!") || !strcmp(t, "'")) return 100;
    if (!strcmp(t, "(") || !strcmp(t, "[")) return 110;
    return 0;
}

/* a comma-separated list up to 'close' (already past the opener) */
static void args(lx_t *l, mx_ast_t *t, const char *close)
{
    if (is_op(l, close)) {
        lx_next(l);
        return;
    }
    for (;;) {
        mx_ast_t *a = expr(l, 11);
        if (!a) {
            l->err = l->err ? l->err : "argument";
            return;
        }
        push_arg(t, a);
        if (is_op(l, ",")) {
            lx_next(l);
            continue;
        }
        if (is_op(l, close)) {
            lx_next(l);
            return;
        }
        l->err = "missing bracket";
        return;
    }
}

static mx_ast_t *nud(lx_t *l)
{
    if (l->type == T_NUM) {
        mx_ast_t *t = mk(A_NUM, l->tok, 0);
        lx_next(l);
        return t;
    }
    if (l->type == T_STR) {
        mx_ast_t *t = mk(A_STR, l->tok, 0);
        lx_next(l);
        return t;
    }
    if (l->type == T_ID) {
        char name[48];
        strcpy(name, l->tok);
        lx_next(l);
        if (!strcmp(name, "not")) {
            mx_ast_t *t = mk(A_NOT, NULL, 1);
            if (t) t->a[0] = expr(l, 45);
            return t;
        }
        if ((!strcmp(name, "list") || !strcmp(name, "set") || !strcmp(name, "seq") ||
             !strcmp(name, "matrix")) && is_op(l, "[")) {
            lx_next(l);
            mx_ast_t *t = mk(name[0] == 's' && name[1] == 'e' && name[2] == 't' ? A_SET : A_LIST, NULL, 0);
            args(l, t, "]");
            return t;
        }
        return mk(A_ID, name, 0);
    }
    if (is_op(l, "-") || is_op(l, "+")) {
        bool neg = l->tok[0] == '-';
        lx_next(l);
        mx_ast_t *a = expr(l, 75);       /* -x^2 is -(x^2), -2*x is (-2)*x */
        if (!neg) return a;
        mx_ast_t *t = mk(A_NEG, NULL, 1);
        if (t) t->a[0] = a;
        return t;
    }
    if (is_op(l, "(")) {
        lx_next(l);
        mx_ast_t *a = expr(l, 0);
        if (is_op(l, ")")) {
            lx_next(l);
        } else {
            l->err = "missing )";
        }
        return a;
    }
    if (is_op(l, "[")) {
        lx_next(l);
        mx_ast_t *t = mk(A_LIST, NULL, 0);
        args(l, t, "]");
        return t;
    }
    if (is_op(l, "{")) {
        lx_next(l);
        mx_ast_t *t = mk(A_SET, NULL, 0);
        args(l, t, "}");
        return t;
    }
    l->err = "unexpected";
    return NULL;
}

static mx_ast_t *led(lx_t *l, mx_ast_t *left)
{
    char op[4];
    strncpy(op, l->tok, sizeof op - 1);
    op[sizeof op - 1] = 0;
    int bp = lbp(l);
    bool word = l->type == T_ID;
    lx_next(l);
    if (word) {
        return mk2(!strcmp(op, "or") ? A_OR : A_AND, left, expr(l, bp));
    }
    if (!strcmp(op, "(")) {
        mx_ast_t *t = mk(A_CALL, left && left->op == A_ID ? left->s : "?", 0);
        if (left && left->op != A_ID) {
            push_arg(t, left);   /* (f)(x): keep f */
        } else {
            mx_ast_free(left);
        }
        args(l, t, ")");
        return t;
    }
    if (!strcmp(op, "[")) {
        mx_ast_t *t = mk(A_INDEX, NULL, 0);
        push_arg(t, left);
        args(l, t, "]");
        return t;
    }
    if (!strcmp(op, "!")) {
        mx_ast_t *t = mk(A_FACT, NULL, 1);
        if (t) t->a[0] = left;
        return t;
    }
    if (!strcmp(op, "'")) {
        mx_ast_t *t = mk(A_PRIME, NULL, 1);
        if (t) t->a[0] = left;
        return t;
    }
    if (!strcmp(op, "^")) {
        return mk2(A_POW, left, expr(l, bp - 1));    /* right associative */
    }
    if (!strcmp(op, ":=")) {
        return mk2(A_ASSIGN, left, expr(l, bp - 1));
    }
    if (!strcmp(op, "->")) {
        return mk2(A_ARROW, left, expr(l, bp - 1));
    }
    mx_ast_t *right = expr(l, bp);
    if (!strcmp(op, "+")) return mk2(A_ADD, left, right);
    if (!strcmp(op, "-")) return mk2(A_SUB, left, right);
    if (!strcmp(op, "*")) return mk2(A_MUL, left, right);
    if (!strcmp(op, "/")) return mk2(A_DIV, left, right);
    if (!strcmp(op, "&&")) return mk2(A_AND, left, right);
    if (!strcmp(op, "||")) return mk2(A_OR, left, right);
    if (!strcmp(op, "=")) return mk2(A_EQ, left, right);
    mx_ast_t *t = mk2(A_CMP, left, right);
    if (t) t->s = mx_strdup(op);
    return t;
}

static mx_ast_t *expr(lx_t *l, int rbp)
{
    mx_ast_t *left = nud(l);
    while (left && !l->err && rbp < lbp(l)) {
        left = led(l, left);
    }
    return left;
}

mx_ast_t *mx_parse(const char *text, const char **err)
{
    lx_t l = { 0 };
    l.p = text;
    lx_next(&l);
    mx_ast_t *t = expr(&l, 0);
    if (t && !l.err && is_op(&l, ",")) {
        mx_ast_t *s = mk(A_SEQ, NULL, 0);
        push_arg(s, t);
        while (!l.err && is_op(&l, ",")) {
            lx_next(&l);
            push_arg(s, expr(&l, 11));
        }
        t = s;
    }
    if (!l.err && l.type != T_END) {
        l.err = "trailing text";
    }
    if (l.err || !t) {
        if (err) *err = l.err ? l.err : "empty";
        mx_ast_free(t);
        return NULL;
    }
    return t;
}

/* --- tree to boxes ----------------------------------------------------------- */

/* how tightly an AST node binds, for the brackets around it */
static int prec(const mx_ast_t *t)
{
    switch (t->op) {
    case A_SEQ: return 1;
    case A_ASSIGN: case A_ARROW: return 2;
    case A_OR: return 3;
    case A_AND: return 4;
    case A_NOT: return 5;
    case A_EQ: case A_CMP: return 6;
    case A_ADD: case A_SUB: return 7;
    case A_MUL: return 8;
    case A_DIV: return 20;          /* stacked: never needs brackets */
    case A_NEG: return 9;
    case A_POW: return 10;
    case A_FACT: case A_PRIME: return 11;
    case A_NUM: return t->s && t->s[0] == '-' ? 9 : 20;
    default: return 20;
    }
}

static const struct { const char *name, *glyph; } NAMES[] = {
    { "pi", "\xCF\x80" }, { "infinity", "\xE2\x88\x9E" }, { "inf", "\xE2\x88\x9E" },
    { "alpha", "\xCE\xB1" }, { "beta", "\xCE\xB2" }, { "gamma", "\xCE\xB3" },
    { "delta", "\xCE\xB4" }, { "epsilon", "\xCE\xB5" }, { "zeta", "\xCE\xB6" },
    { "eta", "\xCE\xB7" }, { "theta", "\xCE\xB8" }, { "iota", "\xCE\xB9" },
    { "kappa", "\xCE\xBA" }, { "lambda", "\xCE\xBB" }, { "mu", "\xCE\xBC" },
    { "nu", "\xCE\xBD" }, { "xi", "\xCE\xBE" }, { "rho", "\xCF\x81" },
    { "sigma", "\xCF\x83" }, { "tau", "\xCF\x84" }, { "phi", "\xCF\x86" },
    { "chi", "\xCF\x87" }, { "psi", "\xCF\x88" }, { "omega", "\xCF\x89" },
    { "Gamma", "\xCE\x93" }, { "Delta", "\xCE\x94" }, { "Theta", "\xCE\x98" },
    { "Lambda", "\xCE\x9B" }, { "Sigma", "\xCE\xA3" }, { "Phi", "\xCE\xA6" },
    { "Psi", "\xCE\xA8" }, { "Omega", "\xCE\xA9" },
};

static void emit(mx_row_t *r, const mx_ast_t *t, int need);

static void sym(mx_row_t *r, const char *s, uint8_t f)
{
    mx_row_insert(r, r->n, mx_sym(s, f));
}

static void op_sym(mx_row_t *r, const char *s)
{
    sym(r, s, MX_F_OP);
}

/* a run of text as one box per character: a name "abc" is one token, a
 * number "123.5" is one token */
static void word(mx_row_t *r, const char *s, uint8_t f)
{
    char buf[16];
    size_t n = strlen(s);
    while (n) {
        size_t k = n < sizeof buf - 1 ? n : sizeof buf - 1;
        /* do not cut a UTF-8 sequence */
        while (k < n && k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) {
            k--;
        }
        memcpy(buf, s, k);
        buf[k] = 0;
        sym(r, buf, f);
        s += k;
        n -= k;
    }
}

static mx_node_t *add_node(mx_row_t *r, mx_kind_t k, int nkid)
{
    mx_node_t *n = mx_node_new(k, nkid);
    mx_row_insert(r, r->n, n);
    return n;
}

static void emit_wrapped(mx_row_t *r, const mx_ast_t *t, int need)
{
    if (prec(t) < need) {
        mx_node_t *p = add_node(r, MX_PAREN, 1);
        if (p) emit(p->kid[0], t, 0);
    } else {
        emit(r, t, need);
    }
}

static void emit_args(mx_row_t *r, const mx_ast_t *t, int from)
{
    for (int i = from; i < t->n; i++) {
        if (i > from) op_sym(r, ",");
        emit(r, t->a[i], 0);
    }
}

static void emit_id(mx_row_t *r, const char *s)
{
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        if (!strcmp(s, NAMES[i].name)) {
            sym(r, NAMES[i].glyph, MX_F_ATOM);
            return;
        }
    }
    /* Euler's number and the imaginary unit: names of their own, so 2e or
     * x·e never glue into "xe" when put back in the editor */
    if (!strcmp(s, "e") || !strcmp(s, "i")) {
        sym(r, s, MX_F_ATOM);
        return;
    }
    if (!strcmp(s, "undef") || !strcmp(s, "true") || !strcmp(s, "false")) {
        sym(r, s, MX_F_TEXT);
        return;
    }
    /* c_0 -> c with 0 below */
    const char *us = strchr(s, '_');
    if (us && us != s && us[1]) {
        char base[32];
        size_t n = (size_t)(us - s) < sizeof base - 1 ? (size_t)(us - s) : sizeof base - 1;
        memcpy(base, s, n);
        base[n] = 0;
        emit_id(r, base);
        mx_node_t *sub = add_node(r, MX_SUB, 1);
        if (sub) word(sub->kid[0], us + 1, 0);
        return;
    }
    word(r, s, 0);
}

static void emit_num(mx_row_t *r, const char *s)
{
    /* 1.5e-07 -> 1.5×10⁻⁷ */
    const char *e = strchr(s, 'e');
    if (!e) {
        e = strchr(s, 'E');
    }
    if (e && e != s) {
        char m[32];
        size_t n = (size_t)(e - s) < sizeof m - 1 ? (size_t)(e - s) : sizeof m - 1;
        memcpy(m, s, n);
        m[n] = 0;
        sym(r, m, 0);
        op_sym(r, "\xC3\x97");
        sym(r, "10", 0);
        mx_node_t *p = add_node(r, MX_POW, 1);
        if (p) {
            const char *x = e + 1;
            if (*x == '+') x++;
            if (*x == '-') {
                sym(p->kid[0], "\xE2\x88\x92", 0);
                x++;
            }
            while (*x == '0' && x[1]) x++;
            sym(p->kid[0], x, 0);
        }
        return;
    }
    /* Giac writes .5 for 0.5 */
    if (s[0] == '.') {
        char b[40];
        b[0] = '0';
        strncpy(b + 1, s, sizeof b - 2);
        b[sizeof b - 1] = 0;
        word(r, b, 0);
        return;
    }
    word(r, s, 0);
}

/* The leftmost and rightmost leaves of a product's factors, for deciding
 * whether the product can be written without its dot. */
static const mx_ast_t *first_leaf(const mx_ast_t *t)
{
    while (t->op == A_POW || t->op == A_FACT || t->op == A_MUL || t->op == A_PRIME) {
        t = t->a[0];
    }
    return t;
}

static const mx_ast_t *last_leaf(const mx_ast_t *t)
{
    while (t->op == A_MUL) {
        t = t->a[1];
    }
    return t;
}

static bool is_glyph_name(const mx_ast_t *t)
{
    if (t->op != A_ID) return false;
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        if (!strcmp(t->s, NAMES[i].name)) return true;
    }
    return !strcmp(t->s, "e") || !strcmp(t->s, "i");
}

/* 2x, x sin(x), c0 e^x, (1/4)ln(x), x²y: as a book writes them. Two plain
 * names stay apart (x·y): written together they would read as the name
 * "xy" when the result is put back in the editor. */
static bool juxtapose(const mx_ast_t *a, const mx_ast_t *b)
{
    const mx_ast_t *la = last_leaf(a), *lb = first_leaf(b);
    if (lb->op == A_NUM || lb->op == A_NEG || lb->op == A_STR) return false;
    if (la->op == A_NUM) return true;
    if (lb->op == A_CALL || is_glyph_name(lb)) return true;
    if (lb->op == A_ID) {
        return la->op == A_DIV || la->op == A_POW || la->op == A_CALL || is_glyph_name(la);
    }
    return false;
}

static bool is_list_matrix(const mx_ast_t *t)
{
    if (t->op != A_LIST || t->n < 1) return false;
    int cols = -1;
    for (int i = 0; i < t->n; i++) {
        const mx_ast_t *row = t->a[i];
        if (row->op != A_LIST || row->n < 1 || row->n > 8) return false;
        if (cols >= 0 && row->n != cols) return false;
        cols = row->n;
    }
    return t->n <= 8;
}

static void emit_call(mx_row_t *r, const mx_ast_t *t)
{
    const char *f = t->s ? t->s : "";
    int n = t->n;
    if (!strcmp(f, "sqrt") && n == 1) {
        mx_node_t *k = add_node(r, MX_ROOT, 1);
        if (k) emit(k->kid[0], t->a[0], 0);
        return;
    }
    if ((!strcmp(f, "surd") || !strcmp(f, "root")) && n == 2) {
        mx_node_t *k = add_node(r, MX_ROOT, 2);
        if (!k) return;
        /* surd(x,n), root(n,x) */
        bool sd = f[0] == 's';
        emit(k->kid[0], t->a[sd ? 0 : 1], 0);
        emit(k->kid[1], t->a[sd ? 1 : 0], 0);
        return;
    }
    if (!strcmp(f, "abs") && n == 1) {
        mx_node_t *k = add_node(r, MX_PAREN, 1);
        if (!k) return;
        k->text[0] = '|';
        emit(k->kid[0], t->a[0], 0);
        return;
    }
    if (!strcmp(f, "exp") && n == 1) {
        sym(r, "e", MX_F_ATOM);
        mx_node_t *k = add_node(r, MX_POW, 1);
        if (k) emit(k->kid[0], t->a[0], 0);
        return;
    }
    if ((!strcmp(f, "integrate") || !strcmp(f, "int")) && (n == 2 || n == 4)) {
        mx_node_t *k = add_node(r, MX_INT, n);
        if (!k) return;
        if (n == 4) {
            emit(k->kid[0], t->a[2], 0);
            emit(k->kid[1], t->a[3], 0);
            emit_wrapped(k->kid[2], t->a[0], 7);
            emit(k->kid[3], t->a[1], 0);
        } else {
            emit_wrapped(k->kid[0], t->a[0], 7);
            emit(k->kid[1], t->a[1], 0);
        }
        return;
    }
    if ((!strcmp(f, "sum") || !strcmp(f, "product")) && n == 4) {
        mx_node_t *k = add_node(r, f[0] == 's' ? MX_SUM : MX_PROD, 4);
        if (!k) return;
        emit(k->kid[0], t->a[1], 0);
        emit(k->kid[1], t->a[2], 0);
        emit(k->kid[2], t->a[3], 0);
        emit_wrapped(k->kid[3], t->a[0], 7);
        return;
    }
    if (!strcmp(f, "diff") && n == 2) {
        mx_node_t *k = add_node(r, MX_DERIV, 2);
        if (!k) return;
        emit(k->kid[0], t->a[1], 0);
        emit(k->kid[1], t->a[0], 0);
        return;
    }
    if (!strcmp(f, "limit") && n == 3) {
        mx_node_t *k = add_node(r, MX_LIMIT, 3);
        if (!k) return;
        emit(k->kid[0], t->a[1], 0);
        emit(k->kid[1], t->a[2], 0);
        emit_wrapped(k->kid[2], t->a[0], 7);
        return;
    }
    if (!strcmp(f, "?") && n >= 1) {
        emit_wrapped(r, t->a[0], 20);
        mx_node_t *k = add_node(r, MX_PAREN, 1);
        if (k) emit_args(k->kid[0], t, 1);
        return;
    }
    sym(r, f, MX_F_FUNC);
    mx_node_t *k = add_node(r, MX_PAREN, 1);
    if (k) emit_args(k->kid[0], t, 0);
}

static void emit(mx_row_t *r, const mx_ast_t *t, int need)
{
    if (!t) {
        return;
    }
    if (prec(t) < need) {
        emit_wrapped(r, t, need);
        return;
    }
    switch (t->op) {
    case A_NUM:
        emit_num(r, t->s);
        break;
    case A_ID:
        emit_id(r, t->s);
        break;
    case A_STR: {
        sym(r, "\"", MX_F_DIM);
        word(r, t->s, 0);
        sym(r, "\"", MX_F_DIM);
        break;
    }
    case A_ADD:
        emit(r, t->a[0], 7);
        if (t->a[1]->op == A_NEG) {
            op_sym(r, "\xE2\x88\x92");
            emit(r, t->a[1]->a[0], 8);
        } else {
            op_sym(r, "+");
            emit(r, t->a[1], 7);
        }
        break;
    case A_SUB:
        emit(r, t->a[0], 7);
        op_sym(r, "\xE2\x88\x92");
        emit(r, t->a[1], 8);
        break;
    case A_NEG:
        sym(r, "\xE2\x88\x92", 0);
        emit(r, t->a[0], 9);
        break;
    case A_MUL: {
        const mx_ast_t *a = t->a[0], *b = t->a[1];
        emit(r, a, 8);
        bool juxt = juxtapose(a, b);
        if (!juxt) op_sym(r, "\xC2\xB7");
        emit(r, b, 9);
        break;
    }
    case A_DIV: {
        mx_node_t *k = add_node(r, MX_FRAC, 2);
        if (k) {
            emit(k->kid[0], t->a[0], 0);
            emit(k->kid[1], t->a[1], 0);
        }
        break;
    }
    case A_POW: {
        const mx_ast_t *b = t->a[0];
        if (b->op == A_CALL && b->s && !strcmp(b->s, "exp")) {
            emit_wrapped(r, b, 20);
        } else {
            emit_wrapped(r, b, 11);
        }
        mx_node_t *k = add_node(r, MX_POW, 1);
        if (k) emit(k->kid[0], t->a[1], 0);
        break;
    }
    case A_FACT:
        emit_wrapped(r, t->a[0], 11);
        sym(r, "!", 0);
        break;
    case A_PRIME:
        emit_wrapped(r, t->a[0], 11);
        sym(r, "'", 0);
        break;
    case A_CALL:
        emit_call(r, t);
        break;
    case A_LIST:
    case A_SET: {
        if (t->op == A_LIST && is_list_matrix(t)) {
            int rows = t->n, cols = t->a[0]->n;
            mx_node_t *k = add_node(r, MX_MAT, rows * cols);
            if (!k) break;
            k->rows = (uint8_t)rows;
            k->cols = (uint8_t)cols;
            for (int i = 0; i < rows; i++) {
                for (int j = 0; j < cols; j++) {
                    emit(k->kid[i * cols + j], t->a[i]->a[j], 0);
                }
            }
            break;
        }
        mx_node_t *k = add_node(r, MX_PAREN, 1);
        if (!k) break;
        k->text[0] = t->op == A_SET ? '{' : '[';
        emit_args(k->kid[0], t, 0);
        break;
    }
    case A_INDEX: {
        emit_wrapped(r, t->a[0], 11);
        mx_node_t *k = add_node(r, MX_PAREN, 1);
        if (!k) break;
        k->text[0] = '[';
        emit_args(k->kid[0], t, 1);
        break;
    }
    case A_SEQ:
        emit_args(r, t, 0);
        break;
    case A_EQ:
        emit(r, t->a[0], 7);
        op_sym(r, "=");
        emit(r, t->a[1], 7);
        break;
    case A_CMP: {
        const char *o = t->s;
        const char *g = !strcmp(o, "<=") ? "\xE2\x89\xA4" : !strcmp(o, ">=") ? "\xE2\x89\xA5" :
                        !strcmp(o, "!=") ? "\xE2\x89\xA0" : !strcmp(o, "==") ? "=" : o;
        emit(r, t->a[0], 7);
        op_sym(r, g);
        emit(r, t->a[1], 7);
        break;
    }
    case A_AND:
    case A_OR:
        emit(r, t->a[0], t->op == A_AND ? 5 : 4);
        sym(r, t->op == A_AND ? "and" : "or", MX_F_TEXT);
        emit(r, t->a[1], t->op == A_AND ? 5 : 4);
        break;
    case A_NOT:
        sym(r, "not", MX_F_TEXT);
        emit(r, t->a[0], 6);
        break;
    case A_ASSIGN:
        emit(r, t->a[0], 3);
        op_sym(r, ":=");
        emit(r, t->a[1], 2);
        break;
    case A_ARROW:
        emit(r, t->a[0], 3);
        op_sym(r, "\xE2\x86\x92");
        emit(r, t->a[1], 2);
        break;
    }
}

mx_row_t *mx_ast_to_row(const mx_ast_t *t)
{
    mx_row_t *r = mx_row_new();
    if (r) emit(r, t, 0);
    return r;
}

mx_row_t *mx_text_to_row(const char *text)
{
    mx_ast_t *t = mx_parse(text, NULL);
    if (t) {
        mx_row_t *r = mx_ast_to_row(t);
        mx_ast_free(t);
        return r;
    }
    /* not something the parser knows: the text as it came, one token per
     * piece of up to 15 bytes */
    mx_row_t *r = mx_row_new();
    if (r) word(r, text, 0);
    return r;
}

/* --- code for the grapher ---------------------------------------------------- */

enum {
    O_K, O_X, O_ADD, O_SUB, O_MUL, O_DIV, O_NEG, O_POW,
    O_LT, O_LE, O_GT, O_GE, O_EQ, O_NE, O_AND, O_OR, O_NOT,
    O_F1, O_F2, O_PIECE,
};

enum {
    F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN, F_SINH, F_COSH, F_TANH,
    F_ASINH, F_ACOSH, F_ATANH, F_EXP, F_LN, F_LOG10, F_SQRT, F_ABS, F_FLOOR,
    F_CEIL, F_ROUND, F_SIGN, F_SEC, F_CSC, F_COT, F_FRAC,
    G_ATAN2, G_MIN, G_MAX, G_SURD, G_LOGB, G_MOD,
};

static const struct { const char *name; uint8_t f, nargs; } FUNCS[] = {
    { "sin", F_SIN, 1 }, { "cos", F_COS, 1 }, { "tan", F_TAN, 1 },
    { "asin", F_ASIN, 1 }, { "acos", F_ACOS, 1 }, { "atan", F_ATAN, 1 },
    { "arcsin", F_ASIN, 1 }, { "arccos", F_ACOS, 1 }, { "arctan", F_ATAN, 1 },
    { "sinh", F_SINH, 1 }, { "cosh", F_COSH, 1 }, { "tanh", F_TANH, 1 },
    { "asinh", F_ASINH, 1 }, { "acosh", F_ACOSH, 1 }, { "atanh", F_ATANH, 1 },
    { "exp", F_EXP, 1 }, { "ln", F_LN, 1 }, { "log", F_LN, 1 }, { "log10", F_LOG10, 1 },
    { "sqrt", F_SQRT, 1 }, { "abs", F_ABS, 1 }, { "floor", F_FLOOR, 1 },
    { "ceil", F_CEIL, 1 }, { "round", F_ROUND, 1 }, { "sign", F_SIGN, 1 },
    { "sec", F_SEC, 1 }, { "csc", F_CSC, 1 }, { "cot", F_COT, 1 }, { "frac", F_FRAC, 1 },
    { "atan2", G_ATAN2, 2 }, { "min", G_MIN, 2 }, { "max", G_MAX, 2 },
    { "surd", G_SURD, 2 }, { "logb", G_LOGB, 2 }, { "irem", G_MOD, 2 }, { "mod", G_MOD, 2 },
};

typedef struct {
    mx_prog_t  *p;
    const char *var;
    int         cap, kcap;
    int         depth, maxdepth;
    bool        bad;
} cc_t;

static void emitb(cc_t *c, uint8_t b)
{
    mx_prog_t *p = c->p;
    if (p->ncode == c->cap) {
        int cap = c->cap ? c->cap * 2 : 64;
        uint8_t *q = mx_realloc(p->code, (size_t)cap);
        if (!q) {
            c->bad = true;
            return;
        }
        p->code = q;
        c->cap = cap;
    }
    p->code[p->ncode++] = b;
}

static void stack(cc_t *c, int d)
{
    c->depth += d;
    if (c->depth > c->maxdepth) c->maxdepth = c->depth;
}

static void konst(cc_t *c, float v)
{
    mx_prog_t *p = c->p;
    if (p->nk >= 255) {
        c->bad = true;
        return;
    }
    if (p->nk == c->kcap) {
        int cap = c->kcap ? c->kcap * 2 : 16;
        float *q = mx_realloc(p->k, sizeof(float) * (size_t)cap);
        if (!q) {
            c->bad = true;
            return;
        }
        p->k = q;
        c->kcap = cap;
    }
    p->k[p->nk] = v;
    emitb(c, O_K);
    emitb(c, (uint8_t)p->nk++);
    stack(c, 1);
}

static void comp(cc_t *c, const mx_ast_t *t);

static void bin(cc_t *c, const mx_ast_t *t, uint8_t op)
{
    comp(c, t->a[0]);
    comp(c, t->a[1]);
    emitb(c, op);
    stack(c, -1);
}

static void comp(cc_t *c, const mx_ast_t *t)
{
    if (!t || c->bad) {
        c->bad = true;
        return;
    }
    switch (t->op) {
    case A_NUM:
        konst(c, strtof(t->s, NULL));
        break;
    case A_ID:
        if (!strcmp(t->s, c->var)) {
            emitb(c, O_X);
            stack(c, 1);
        } else if (!strcmp(t->s, "pi")) {
            konst(c, 3.14159265358979f);
        } else if (!strcmp(t->s, "e")) {
            konst(c, 2.71828182845905f);
        } else if (!strcmp(t->s, "inf") || !strcmp(t->s, "infinity")) {
            konst(c, INFINITY);
        } else {
            c->bad = true;
        }
        break;
    case A_ADD: bin(c, t, O_ADD); break;
    case A_SUB: bin(c, t, O_SUB); break;
    case A_MUL: bin(c, t, O_MUL); break;
    case A_DIV: bin(c, t, O_DIV); break;
    case A_POW: bin(c, t, O_POW); break;
    case A_AND: bin(c, t, O_AND); break;
    case A_OR:  bin(c, t, O_OR); break;
    case A_NEG:
        comp(c, t->a[0]);
        emitb(c, O_NEG);
        break;
    case A_NOT:
        comp(c, t->a[0]);
        emitb(c, O_NOT);
        break;
    case A_CMP: {
        const char *o = t->s;
        uint8_t op = !strcmp(o, "<") ? O_LT : !strcmp(o, "<=") ? O_LE : !strcmp(o, ">") ? O_GT :
                     !strcmp(o, ">=") ? O_GE : !strcmp(o, "!=") ? O_NE : O_EQ;
        bin(c, t, op);
        break;
    }
    case A_CALL: {
        const char *f = t->s ? t->s : "";
        if ((!strcmp(f, "piecewise") || !strcmp(f, "when") || !strcmp(f, "ifte")) && t->n >= 2) {
            /* piecewise(c1,v1,c2,v2,...,[else]): folded from the right into
             * select(c, v, rest) */
            int pairs = t->n / 2;
            bool has_else = t->n % 2;
            if (has_else) {
                comp(c, t->a[t->n - 1]);
            } else {
                konst(c, NAN);
            }
            for (int i = pairs - 1; i >= 0; i--) {
                comp(c, t->a[2 * i]);
                comp(c, t->a[2 * i + 1]);
                emitb(c, O_PIECE);      /* else cond val -> cond ? val : else */
                stack(c, -2);
            }
            break;
        }
        for (size_t i = 0; i < sizeof FUNCS / sizeof FUNCS[0]; i++) {
            if (!strcmp(f, FUNCS[i].name) && t->n == FUNCS[i].nargs) {
                for (int a = 0; a < t->n; a++) {
                    comp(c, t->a[a]);
                }
                emitb(c, FUNCS[i].nargs == 1 ? O_F1 : O_F2);
                emitb(c, FUNCS[i].f);
                if (FUNCS[i].nargs == 2) stack(c, -1);
                return;
            }
        }
        c->bad = true;
        break;
    }
    default:
        c->bad = true;
        break;
    }
}

bool mx_compile(const mx_ast_t *t, const char *var, mx_prog_t *p)
{
    memset(p, 0, sizeof *p);
    cc_t c = { p, var, 0, 0, 0, 0, false };
    comp(&c, t);
    if (c.bad || c.maxdepth > 30 || c.depth != 1) {
        mx_prog_free(p);
        return false;
    }
    return true;
}

void mx_prog_free(mx_prog_t *p)
{
    mx_free(p->code);
    mx_free(p->k);
    memset(p, 0, sizeof *p);
}

static float f1(int f, float a)
{
    switch (f) {
    case F_SIN: return sinf(a);
    case F_COS: return cosf(a);
    case F_TAN: return tanf(a);
    case F_ASIN: return asinf(a);
    case F_ACOS: return acosf(a);
    case F_ATAN: return atanf(a);
    case F_SINH: return sinhf(a);
    case F_COSH: return coshf(a);
    case F_TANH: return tanhf(a);
    case F_ASINH: return logf(a + sqrtf(a * a + 1));
    case F_ACOSH: return logf(a + sqrtf(a * a - 1));
    case F_ATANH: return 0.5f * logf((1 + a) / (1 - a));
    case F_EXP: return expf(a);
    case F_LN: return logf(a);
    case F_LOG10: return log10f(a);
    case F_SQRT: return sqrtf(a);
    case F_ABS: return fabsf(a);
    case F_FLOOR: return floorf(a);
    case F_CEIL: return ceilf(a);
    case F_ROUND: return floorf(a + 0.5f);
    case F_SIGN: return a > 0 ? 1.0f : a < 0 ? -1.0f : 0.0f;
    case F_SEC: return 1.0f / cosf(a);
    case F_CSC: return 1.0f / sinf(a);
    case F_COT: return 1.0f / tanf(a);
    case F_FRAC: return a - floorf(a);
    default: return NAN;
    }
}

static float f2(int f, float a, float b)
{
    switch (f) {
    case G_ATAN2: return atan2f(a, b);
    case G_MIN: return a < b ? a : b;
    case G_MAX: return a > b ? a : b;
    case G_SURD: {
        float r = powf(fabsf(a), 1.0f / b);
        return (a < 0 && fmodf(b, 2.0f) != 0) ? -r : r;
    }
    case G_LOGB: return logf(a) / logf(b);
    case G_MOD: return a - b * floorf(a / b);
    default: return NAN;
    }
}

static float ipow(float a, float b)
{
    /* x^n for a negative x and a whole n, which powf gives as NaN */
    if (a < 0 && b == floorf(b)) {
        float r = powf(-a, b);
        return fmodf(b, 2.0f) != 0 ? -r : r;
    }
    /* x^(1/3) of a negative x: the real root, as a book would */
    if (a < 0 && b != 0) {
        float inv = 1.0f / b, n = floorf(inv + 0.5f);
        if (fabsf(inv - n) < 1e-5f && fmodf(n, 2.0f) != 0) {
            return -powf(-a, b);
        }
    }
    return powf(a, b);
}

float mx_eval(const mx_prog_t *p, float x)
{
    float s[32];
    int sp = 0;
    const uint8_t *c = p->code, *end = c + p->ncode;
    while (c < end) {
        switch (*c++) {
        case O_K:   s[sp++] = p->k[*c++]; break;
        case O_X:   s[sp++] = x; break;
        case O_ADD: sp--; s[sp - 1] += s[sp]; break;
        case O_SUB: sp--; s[sp - 1] -= s[sp]; break;
        case O_MUL: sp--; s[sp - 1] *= s[sp]; break;
        case O_DIV: sp--; s[sp - 1] /= s[sp]; break;
        case O_POW: sp--; s[sp - 1] = ipow(s[sp - 1], s[sp]); break;
        case O_NEG: s[sp - 1] = -s[sp - 1]; break;
        case O_LT:  sp--; s[sp - 1] = s[sp - 1] < s[sp]; break;
        case O_LE:  sp--; s[sp - 1] = s[sp - 1] <= s[sp]; break;
        case O_GT:  sp--; s[sp - 1] = s[sp - 1] > s[sp]; break;
        case O_GE:  sp--; s[sp - 1] = s[sp - 1] >= s[sp]; break;
        case O_EQ:  sp--; s[sp - 1] = s[sp - 1] == s[sp]; break;
        case O_NE:  sp--; s[sp - 1] = s[sp - 1] != s[sp]; break;
        case O_AND: sp--; s[sp - 1] = s[sp - 1] != 0 && s[sp] != 0; break;
        case O_OR:  sp--; s[sp - 1] = s[sp - 1] != 0 || s[sp] != 0; break;
        case O_NOT: s[sp - 1] = s[sp - 1] == 0; break;
        case O_F1:  s[sp - 1] = f1(*c++, s[sp - 1]); break;
        case O_F2:  sp--; s[sp - 1] = f2(*c++, s[sp - 1], s[sp]); break;
        case O_PIECE:
            /* else, cond, val */
            sp -= 2;
            s[sp - 1] = s[sp] != 0 ? s[sp + 1] : s[sp - 1];
            break;
        default:
            return NAN;
        }
    }
    return sp ? s[0] : NAN;
}
