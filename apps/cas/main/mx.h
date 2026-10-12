/*
 * P4OS - CAS: the maths core of the interface ("mx").
 *
 * One tree of boxes serves both for typing and for showing: the editor
 * edits it, a result arrives as Giac's linear text and is parsed into the
 * same kind of tree, and the view widget lays it out and draws it the way a
 * book would (fractions stacked, exponents raised, radicals, integrals).
 *
 *   text  --mx_parse-->  AST  --mx_ast_to_row-->  boxes  --mx_view-->  screen
 *   boxes --mx_row_text--> text for Giac
 *   AST   --mx_compile-->  bytecode for the grapher's evaluator (float)
 *
 * All of it is C and runs the same in the simulator, where there is no
 * engine.
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- memory (PSRAM on the board) and strings ------------------------------ */

void *mx_alloc(size_t n);               /* zeroed */
void *mx_realloc(void *p, size_t n);
void  mx_free(void *p);
char *mx_strdup(const char *s);

typedef struct {
    char  *s;
    size_t n, cap;
} mx_str_t;

void mx_str_add(mx_str_t *b, const char *s);
void mx_str_addn(mx_str_t *b, const char *s, size_t n);
void mx_str_addc(mx_str_t *b, char c);
void mx_str_free(mx_str_t *b);
char *mx_str_take(mx_str_t *b);         /* the buffer, "" if empty; b is reset */

/* --- fonts ----------------------------------------------------------------
 * The firmware's Inter (Latin, Greek, arrows, maths operators) at the sizes
 * the maths is drawn in: 20, 24, 28, 32, 48. */

const lv_font_t *mx_font(int px);
bool             mx_font_has(uint32_t cp, int px);
int              mx_text_width(const lv_font_t *f, const char *s);
uint32_t         mx_utf8_next(const char **p);

/* --- boxes ---------------------------------------------------------------- */

typedef enum {
    MX_SYM,     /* a token: "2", "x", "+", "sin", "π"                       */
    MX_FRAC,    /* num, den                                                 */
    MX_ROOT,    /* radicand, [index]                                        */
    MX_POW,     /* exponent: raised after the item before it               */
    MX_SUB,     /* subscript: lowered after the item before it              */
    MX_PAREN,   /* inside; text[0] is the bracket: ( [ { |                  */
    MX_INT,     /* body, var  |  lower, upper, body, var  (definite)        */
    MX_SUM,     /* var, lower, upper, body                                  */
    MX_PROD,    /* var, lower, upper, body                                  */
    MX_DERIV,   /* var, body                                                */
    MX_LIMIT,   /* var, to, body                                            */
    MX_MAT,     /* rows x cols cells, row by row                            */
} mx_kind_t;

/* SYM flags */
#define MX_F_OP     0x01    /* binary operator: spaced                       */
#define MX_F_FUNC   0x02    /* function name (upright, a call follows)       */
#define MX_F_TEXT   0x04    /* a word (and, or, undef): spaced, upright      */
#define MX_F_DIM    0x08    /* drawn dim (a string's quotes...)              */
#define MX_F_GLYPH  0x40    /* MX_PAREN, set by the layout: drawn with the font */
#define MX_F_ATOM   0x10    /* a constant or Greek letter: a name of its own,
                             * never glued to the letters around it         */

typedef struct mx_row  mx_row_t;
typedef struct mx_node mx_node_t;

struct mx_row {
    mx_node_t **it;
    int         n, cap;
    mx_node_t  *owner;          /* the node this row belongs to; NULL = root */
    /* layout: x is the left edge, y the baseline, in the root's frame */
    int16_t     x, y, w, asc, dsc;
    uint8_t     px;             /* font size it is laid out at              */
};

struct mx_node {
    uint8_t    kind;
    uint8_t    flags;
    uint8_t    rows, cols;      /* MX_MAT */
    char       text[16];        /* MX_SYM: the token (UTF-8); MX_PAREN: the bracket */
    mx_row_t **kid;
    uint8_t    nkid;
    mx_row_t  *parent;
    int16_t    x, y, w, asc, dsc;
    uint8_t    px;
};

typedef struct {
    mx_row_t *row;
    int       pos;              /* 0..row->n: before item pos               */
} mx_cur_t;

mx_row_t  *mx_row_new(void);
void       mx_row_free(mx_row_t *r);
void       mx_row_clear(mx_row_t *r);
mx_row_t  *mx_row_copy(const mx_row_t *r);
void       mx_row_insert(mx_row_t *r, int pos, mx_node_t *n);
mx_node_t *mx_row_take(mx_row_t *r, int pos);       /* removes, does not free */
void       mx_row_append_row(mx_row_t *dst, int pos, mx_row_t *src);  /* moves src's items */
mx_node_t *mx_sym(const char *tok, uint8_t flags);
mx_node_t *mx_node_new(mx_kind_t kind, int nkid);
void       mx_node_free(mx_node_t *n);
mx_node_t *mx_node_copy(const mx_node_t *n);
bool       mx_row_empty(const mx_row_t *r);

/* The text Giac reads, e.g. "((x^(2)+1)/(2))". */
char *mx_row_text(const mx_row_t *r);
const char *mx_sym_giac(const char *tok);   /* "π" -> "pi"; others as they are */
const char *mx_sym_shown(const char *tok, int px);  /* tok, or its name if the font lacks it */

/* --- editing --------------------------------------------------------------
 * Keys are strings, the same ones the keypad and the physical keyboard
 * send: a token to insert ("7", "x", "+", "π"), a function ("sin(" opens
 * its parentheses), a template ("/frac", "/pow", "/sqrt" ...) or a move
 * ("/left", "/bs" ...). See mx_edit.c. */

typedef struct {
    mx_row_t *root;
    mx_cur_t  cur;
} mx_ed_t;

void mx_ed_init(mx_ed_t *e);
void mx_ed_free(mx_ed_t *e);
void mx_ed_clear(mx_ed_t *e);
void mx_ed_key(mx_ed_t *e, const char *key);
void mx_ed_insert_row(mx_ed_t *e, const mx_row_t *r);     /* a copy, at the cursor */
void mx_ed_set_row(mx_ed_t *e, mx_row_t *r);              /* takes it; cursor at the end */
bool mx_ed_tap(mx_ed_t *e, int x, int y);                 /* root frame, after layout */

/* --- layout and drawing --------------------------------------------------- */

typedef struct {
    lv_color_t text, dim, accent, placeholder;
    bool       edit;            /* placeholders for empty rows             */
} mx_style_t;

void mx_layout(mx_row_t *r, int px);        /* sets every x,y,w,asc,dsc   */
void mx_draw(lv_layer_t *layer, const mx_row_t *r, int ox, int oy, const mx_style_t *st);
void mx_draw_cursor(lv_layer_t *layer, const mx_cur_t *c, int ox, int oy, lv_color_t col);

/* --- parsing Giac's text -------------------------------------------------- */

typedef enum {
    A_NUM, A_ID, A_STR,
    A_ADD, A_SUB, A_MUL, A_DIV, A_NEG, A_POW, A_FACT, A_PRIME,
    A_CALL, A_LIST, A_SET, A_INDEX, A_SEQ,
    A_EQ, A_CMP, A_AND, A_OR, A_NOT, A_ASSIGN, A_ARROW,
} mx_aop_t;

typedef struct mx_ast {
    uint8_t         op;
    uint8_t         n;
    char           *s;          /* A_NUM/A_ID/A_STR/A_CALL name/A_CMP operator */
    struct mx_ast **a;
} mx_ast_t;

mx_ast_t *mx_parse(const char *text, const char **err);  /* NULL on error */
void      mx_ast_free(mx_ast_t *t);
mx_row_t *mx_ast_to_row(const mx_ast_t *t);
mx_row_t *mx_text_to_row(const char *text);             /* parse; raw text if it fails */

/* --- the grapher's evaluator ---------------------------------------------- */

typedef struct {
    uint8_t *code;
    double  *k;                 /* constants: double, so a typed 1234567.89 stays itself */
    int      ncode, nk;
} mx_prog_t;

bool  mx_compile(const mx_ast_t *t, const char *var, mx_prog_t *p);
void  mx_prog_free(mx_prog_t *p);
float  mx_eval(const mx_prog_t *p, float x);        /* the grapher: fast */
double mx_eval_d(const mx_prog_t *p, double x);     /* forms: exact enough for money */
/* A number typed or pasted as text: "1234.5", "1/3", "2pi", "sqrt(2)*10^3".
 * ±∞ counts. false when it is not a number. */
bool   mx_text_value(const char *text, double *out);
