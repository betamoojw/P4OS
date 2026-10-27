/*
 * P4OS - Cartero: the inside of the JavaScript interpreter, shared by the
 * language (ct_js.c) and the system it runs in (ct_pm.c: pm, chai's expect,
 * console, the old Postman globals).
 *
 * A tree walker: the program is parsed into nodes in the job's arena and
 * run from there. A throw is a longjmp to the nearest try (or to the top of
 * ct_js_run); everything lives in the arena, so nothing has to be undone on
 * the way.
 */
#pragma once

#include "ct.h"

#include <setjmp.h>

/* ---- the tree ------------------------------------------------------------------------------------ */

enum {
    N_NUM = 1, N_STR, N_TEMPLATE, N_IDENT, N_THIS, N_ARRAY, N_OBJECT, N_PROP, N_FUNC, N_CALL, N_NEW,
    N_MEMBER, N_INDEX, N_UNARY, N_UPDATE, N_BINARY, N_LOGICAL, N_COND, N_ASSIGN, N_SEQ, N_SPREAD,
    N_OBJPAT, N_ARRPAT, N_PATPROP,
    /* statements */
    N_VAR, N_DECL, N_EXPR, N_BLOCK, N_IF, N_FOR, N_FOROF, N_FORIN, N_WHILE, N_DO, N_RETURN, N_BREAK,
    N_CONTINUE, N_THROW, N_TRY, N_SWITCH, N_CASE, N_EMPTY, N_FUNCDECL,
};

enum {
    OP_ADD = 1, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_EXP, OP_EQ, OP_NE, OP_SEQ, OP_SNE, OP_LT, OP_GT, OP_LE,
    OP_GE, OP_AND, OP_OR, OP_NULLISH, OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR, OP_USHR, OP_IN, OP_INSTANCEOF,
    OP_NOT, OP_NEG, OP_POS, OP_BNOT, OP_TYPEOF, OP_VOID, OP_DELETE, OP_INC, OP_DEC, OP_SET,
};

/* node flags */
#define NF_OPTIONAL  0x01    /* ?. */
#define NF_PREFIX    0x02    /* ++x */
#define NF_ARROW     0x04
#define NF_EXPRBODY  0x08    /* x => x + 1 */
#define NF_COMPUTED  0x10    /* { [k]: v } */
#define NF_CONST     0x20
#define NF_REST      0x40    /* ...args */
#define NF_SHORT     0x80    /* an optional chain stops here when it met null */
/* on a block: it declares something, so it needs a scope of its own; on a
 * loop: a function is made inside, so each turn needs its own bindings */
#define NF_DECLS     0x02
#define NF_HASFN     0x04

typedef struct ct_node node_t;
struct ct_node {
    uint8_t   k, op, flag;
    int       line;
    node_t   *a, *b, *c, *d;
    node_t  **list;
    int       n;
    ct_str_t *s;
    double    num;
};

/* ---- the machine -------------------------------------------------------------------------------- */

typedef struct ct_scope scope_t;
typedef struct {
    ct_str_t *name;
    ctv_t     v;
    bool      konst;
} var_t;
struct ct_scope {
    scope_t  *up;
    var_t    *v;
    uint32_t  n, cap;
};

enum { C_NORMAL = 0, C_RETURN, C_BREAK, C_CONTINUE };

typedef struct {
    ct_str_t **slot;
    uint32_t   cap, n;
} intern_t;

struct ct_vm {
    ct_arena_t  *ar;
    ct_script_t *ctx;
    jmp_buf     *jb;            /* where a throw lands */
    ctv_t        thrown;
    ctv_t        ret;           /* the value of a return */
    ctv_t        this_v;
    scope_t     *global;
    intern_t     names;
    long         steps;
    uint64_t     t0;
    int          depth;
    int          line;
    bool         shorted;       /* an optional chain met a null and is being cut short */
    ctv_t        legacy_tests;  /* the old tests["name"] = bool object */
    ctv_t        timeouts;      /* setTimeout's callbacks, run when the script ends */
    uint32_t     rnd;
    /* the parser's state */
    const char  *src, *p, *end;
    int          pline;
    int          pdepth;
    int          nfuncs;        /* functions parsed so far: a loop knows if its body makes closures */
};

/* ---- shared helpers --------------------------------------------------------------------------- */

void       ct_throw(ct_vm_t *vm, ctv_t v) __attribute__((noreturn));
void       ct_throw_err(ct_vm_t *vm, const char *name, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));
ctv_t      ct_make_error(ct_vm_t *vm, const char *name, const char *msg);

ctv_t      vm_str(ct_vm_t *vm, const char *p, size_t n);
ctv_t      vm_cstr(ct_vm_t *vm, const char *p);
ctv_t      vm_arr(ct_vm_t *vm);
ctv_t      vm_obj(ct_vm_t *vm);
ctv_t      vm_fn(ct_vm_t *vm, ct_native_t f, void *ud, const char *name);
void       vm_push(ct_vm_t *vm, ctv_t arr, ctv_t v);
void       vm_set(ct_vm_t *vm, ctv_t obj, const char *k, ctv_t v);
void       vm_method(ct_vm_t *vm, ctv_t obj, const char *k, ct_native_t f, void *ud);
ct_str_t  *vm_to_str(ct_vm_t *vm, ctv_t v);
ct_str_t  *ct_intern(ct_vm_t *vm, const char *p, size_t n);

ctv_t      ct_call(ct_vm_t *vm, ctv_t fn, ctv_t self, int argc, ctv_t *argv);
ctv_t      ct_get_prop(ct_vm_t *vm, ctv_t obj, const char *k, size_t kl);
bool       ct_strict_eq(ctv_t a, ctv_t b);
bool       ct_loose_eq(ct_vm_t *vm, ctv_t a, ctv_t b);
void       ct_inspect(ct_sb_t *b, ctv_t v, int depth);     /* chai's and console's way of showing a value */
void       ct_declare(ct_vm_t *vm, scope_t *s, ct_str_t *name, ctv_t v, bool konst);

/* time since the epoch, in ms; the uptime when the board has no clock yet */
double     ct_now_ms(void);
uint32_t   ct_rand(ct_vm_t *vm);

/* ct_pm.c */
void       ct_pm_install(ct_vm_t *vm);
void       ct_pm_finish(ct_vm_t *vm);      /* the old tests["..."] object read back */

static inline ctv_t arg(int argc, ctv_t *argv, int i)
{
    return i < argc ? argv[i] : ct_undef();
}
