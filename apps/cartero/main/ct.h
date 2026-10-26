/*
 * P4OS - Cartero: the core, shared by the engine and the screens.
 *
 * Everything a request needs lives in values of one model, the one a
 * JavaScript program sees (ct_val.c): a collection read from its JSON file,
 * an environment, the response's JSON tree, the scripts' objects. Values are
 * allocated from arenas in PSRAM and never freed one by one: a collection
 * has its arena, a request in flight has its own (the job's), and each goes
 * away whole. No reference counts, no collector, and a script that leaks
 * cannot leak past its job.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CT_TAG     "cartero"
#define CT_APP_ID  "aos.cartero"

/* ---- memory --------------------------------------------------------------------------------- */

void *ct_psram(size_t n);                    /* malloc, in PSRAM on the board */
void *ct_psram_realloc(void *p, size_t n);
void  ct_copy(char *dst, size_t n, const char *src);

typedef struct ct_block ct_block_t;
typedef struct {
    ct_block_t *head;
    size_t      used;       /* bytes handed out */
    size_t      cap;        /* the ceiling, 0 for none */
    size_t      next;       /* the size of the next block */
    bool        full;       /* an allocation was refused */
} ct_arena_t;

void  ct_arena_init(ct_arena_t *a, size_t cap);
void *ct_alloc(ct_arena_t *a, size_t n);    /* zeroed, 8-aligned; NULL once over the ceiling */
void  ct_arena_free(ct_arena_t *a);

/* A growing text in PSRAM (not in an arena: it is handed to files and the
 * live channel). oom is set, and the text stays as it was, when it cannot
 * grow. */
typedef struct {
    char  *p;
    size_t n, cap;
    bool   oom;
} ct_sb_t;

void ct_sb_add(ct_sb_t *b, const char *s, size_t n);
void ct_sb_adds(ct_sb_t *b, const char *s);
void ct_sb_addc(ct_sb_t *b, char c);
void ct_sb_addf(ct_sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ct_sb_free(ct_sb_t *b);

/* ---- values --------------------------------------------------------------------------------- */

typedef enum { CT_UNDEF, CT_NULL, CT_BOOL, CT_NUM, CT_STR, CT_ARR, CT_OBJ, CT_FN } ct_type_t;

typedef struct ct_str ct_str_t;
typedef struct ct_arr ct_arr_t;
typedef struct ct_obj ct_obj_t;
typedef struct ct_fn  ct_fn_t;
typedef struct ct_vm  ct_vm_t;

typedef struct {
    uint8_t t;
    union {
        bool      b;
        double    n;
        ct_str_t *s;
        ct_arr_t *a;
        ct_obj_t *o;
        ct_fn_t  *f;
    };
} ctv_t;

struct ct_str {
    uint32_t len;
    char     p[];           /* NUL-terminated as well */
};

struct ct_arr {
    uint32_t n, cap;
    ctv_t   *v;
};

typedef struct {
    ct_str_t *k;
    ctv_t     v;
} ct_prop_t;

/* An object. 'get' and 'set', when there, are asked first: the objects the
 * scripts get from the system (pm.expect's chain, pm.response.to) answer
 * properties that are computed, or are assertions themselves
 * (".to.be.true"). found = false falls through to the plain properties. */
typedef ctv_t (*ct_get_t)(ct_vm_t *vm, ct_obj_t *o, const char *k, size_t kl, bool *found);

struct ct_obj {
    uint32_t  n, cap;
    ct_prop_t *p;
    uint16_t  kind;         /* CT_K_* below; 0 a plain object */
    void     *ud;
    ct_get_t  get;
};

enum { CT_K_PLAIN = 0, CT_K_ERROR, CT_K_DATE, CT_K_EXPECT, CT_K_SYS };

typedef ctv_t (*ct_native_t)(ct_vm_t *vm, ctv_t self, int argc, ctv_t *argv, void *ud);

struct ct_node;
struct ct_scope;
struct ct_fn {
    ct_native_t      native;    /* a built-in, or NULL for a script's function */
    void            *ud;
    ctv_t            self;      /* bound 'this' for a method taken off a value */
    bool             bound;
    struct ct_node  *node;      /* the function's code */
    struct ct_scope *scope;     /* the scope it closes over */
    const char      *name;
};

static inline ctv_t ct_undef(void) { ctv_t v; memset(&v, 0, sizeof v); return v; }
static inline ctv_t ct_null(void)  { ctv_t v = ct_undef(); v.t = CT_NULL; return v; }
static inline ctv_t ct_bool(bool b) { ctv_t v = ct_undef(); v.t = CT_BOOL; v.b = b; return v; }
static inline ctv_t ct_num(double n) { ctv_t v = ct_undef(); v.t = CT_NUM; v.n = n; return v; }

ctv_t     ct_strv(ct_arena_t *a, const char *p, size_t n);
ctv_t     ct_cstr(ct_arena_t *a, const char *p);
ct_str_t *ct_str_new(ct_arena_t *a, const char *p, size_t n);
ctv_t     ct_arr_new(ct_arena_t *a, uint32_t cap);
ctv_t     ct_obj_new(ct_arena_t *a, uint32_t cap);
ctv_t     ct_fn_native(ct_arena_t *a, ct_native_t f, void *ud, const char *name);

bool      ct_arr_push(ct_arena_t *a, ct_arr_t *arr, ctv_t v);
bool      ct_arr_insert(ct_arena_t *a, ct_arr_t *arr, uint32_t at, ctv_t v);
void      ct_arr_remove(ct_arr_t *arr, uint32_t at);
ctv_t    *ct_obj_find(ct_obj_t *o, const char *k, size_t kl);    /* own, plain; NULL when absent */
bool      ct_obj_set(ct_arena_t *a, ct_obj_t *o, const char *k, size_t kl, ctv_t v);
bool      ct_obj_del(ct_obj_t *o, const char *k, size_t kl);

/* Reading a document without ceremony: anything missing or of another type
 * gives undefined, "", 0 or NULL. */
ctv_t       ct_get(ctv_t v, const char *k);
ctv_t       ct_at(ctv_t v, uint32_t i);
uint32_t    ct_len(ctv_t v);                    /* items of an array, 0 otherwise */
const char *ct_s(ctv_t v);                      /* a string's text, "" otherwise */
bool        ct_is(ctv_t v, ct_type_t t);
bool        ct_truthy(ctv_t v);
void        ct_sets(ct_arena_t *a, ctv_t obj, const char *k, const char *s);
void        ct_setv(ct_arena_t *a, ctv_t obj, const char *k, ctv_t v);

ctv_t       ct_deep_copy(ct_arena_t *a, ctv_t v);   /* data only: functions become undefined */
bool        ct_deep_equal(ctv_t a, ctv_t b);

/* JavaScript's conversions */
void        ct_num_fmt(double d, char out[40]);
ct_str_t   *ct_to_str(ct_arena_t *a, ctv_t v);
double      ct_to_num(ctv_t v);
const char *ct_typeof(ctv_t v);

/* ---- JSON ----------------------------------------------------------------------------------- */

/* The whole text, or false with the place and the reason in err. Depth is
 * kept on a stack of its own, not the C one: the UI's thread parses too. */
bool ct_json_parse(ct_arena_t *a, const char *s, size_t n, ctv_t *out, char *err, size_t errn);
/* indent 0: compact; > 0 that many spaces; -1 a tab (Postman's files) */
void ct_json_write(ct_sb_t *b, ctv_t v, int indent);
void ct_json_str(ct_sb_t *b, const char *s, size_t n);   /* a quoted, escaped JSON string */
/* a text that may be JSON, laid out again with indent; false if it is not JSON */
bool ct_json_pretty(const char *s, size_t n, ct_sb_t *out, int indent);

/* ---- HTTP ----------------------------------------------------------------------------------- */

#define CT_HDR_MAX   64

typedef struct {
    const char *k, *v;
} ct_kv_t;

typedef struct {
    const char *method;
    const char *url;
    ct_kv_t     hdr[CT_HDR_MAX];
    int         nhdr;
    const char *body;
    int         blen;
    int         timeout_ms;
    bool        follow;
    int         max_body;
} ct_http_req_t;

typedef struct {
    bool     ok;                /* an answer came (any status) */
    int      status;
    char     reason[64];
    char     version[12];
    char    *hdr_buf;           /* PSRAM; the header block, cut into the pairs below */
    ct_kv_t  hdr[CT_HDR_MAX];
    int      nhdr;
    uint8_t *body;              /* PSRAM, NUL-terminated */
    int      len;
    bool     cut;               /* more than max_body came, or it was cut short */
    int      t_connect, t_send, t_wait, t_download, t_total;
    int      redirects;
    char     final_url[512];
    int      code;              /* AOS_HTTP_ERR_* when !ok */
    char     err[192];
} ct_resp_t;

/* ct_resp_t.code besides the AOS_HTTP_ERR_* of aos_hal.h */
#define CT_ERR_URL      (-20)   /* not http(s)://host... */
#define CT_ERR_TIMEOUT  (-21)
#define CT_ERR_STOPPED  (-22)
#define CT_ERR_TOOMANY  (-23)   /* redirects */

typedef struct {
    void    *c;
    char     host[128];
    int      port;
    bool     tls;
    uint64_t at;
} ct_conn_t;

/* One request, from a thread of the app (never LVGL's). It follows redirects
 * when asked, keeps the connection in cc for the next one to the same host,
 * and gives up when *stop turns true. */
void        ct_http_do(ct_conn_t *cc, const ct_http_req_t *rq, ct_resp_t *rs, volatile bool *stop);
void        ct_http_close(ct_conn_t *cc);
void        ct_resp_free(ct_resp_t *rs);
const char *ct_resp_header(const ct_resp_t *rs, const char *name);
bool        ct_body_is_text(const ct_resp_t *rs);
const char *ct_reason_text(int status);

/* the pieces of a URL; false if it is not http(s)://host... */
bool ct_url_split(const char *url, char *host, size_t hn, int *port, char *path, size_t pn, bool *tls);
size_t ct_url_enc(char *out, size_t n, const char *in, bool keep_slash);
size_t ct_b64(char *out, size_t n, const void *in, size_t len);
int    ct_b64_dec(uint8_t *out, size_t n, const char *in);

/* ---- scripts -------------------------------------------------------------------------------- */

/* What a script sees and leaves behind. The scopes are objects of
 * name -> value that the job owns; every set and unset is also written to
 * 'changes' ([scope, key, value | undefined], scope "environment",
 * "globals", "collectionVariables"), so the screen can play them onto the
 * files when the job ends. */
typedef struct {
    ct_arena_t *ar;
    ctv_t       env, glob, coll, local;
    ctv_t       changes;
    ctv_t       req;            /* {method, url, header: [{key, value, disabled}], body: {...}} */
    const ct_resp_t *resp;      /* NULL in a pre-request script */
    ctv_t       resp_json;      /* parsed once, on demand */
    bool        resp_json_done;
    ctv_t       tests;          /* [{name, ok, error}] */
    ct_sb_t    *console;
    const char *request_name;
    const char *event;          /* "prerequest" or "test" */
    int         iteration;
    char        next[96];       /* setNextRequest */
    bool        next_set, next_stop;
    bool        skip;           /* pm.execution.skipRequest() in a pre-request script */
    char        error[256];     /* the script failed: why (and the line) */
    volatile bool *stop;
} ct_script_t;

bool ct_js_run(ct_script_t *ctx, const char *src);

/* {{name}} resolved: the script's own, the environment, the collection's,
 * the globals; then the dynamic ones ($guid, $timestamp...). Unknown names
 * stay as they are. */
char *ct_resolve(ct_arena_t *a, const ct_script_t *ctx, const char *in);
ctv_t ct_var_get(const ct_script_t *ctx, const char *k);
