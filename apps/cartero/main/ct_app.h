/*
 * P4OS - Cartero: the app's own pieces, over the core of ct.h. The store
 * (the files on the card, ct_store.c), the jobs that send requests and run
 * collections in a thread (ct_job.c), and what the screens share (ct_ui*.c).
 */
#pragma once

#include "ct.h"

#include "aos_app.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define CT_COLL_MAX    32
#define CT_ENV_MAX     24
#define CT_DEPTH_MAX   8           /* folders inside folders */
#define CT_RUN_MAX     400         /* requests one run may send */
#define CT_HIST_MAX    50
#define CT_JOB_ARENA   (12u * 1024 * 1024)

/* ---- the store: the files on the card ---------------------------------------------------- */

typedef struct {
    ct_arena_t ar;
    ctv_t      root;
    char       slug[64];        /* the file's name without .json */
    char       name[96];
    uint32_t   mtime;
    uint32_t   size;
    bool       bad;             /* it did not parse: shown, not written */
    char       err[96];
} ct_doc_t;

typedef struct {
    ct_doc_t coll[CT_COLL_MAX];
    int      ncoll;
    ct_doc_t env[CT_ENV_MAX];
    int      nenv;
    ct_doc_t glob;
    ct_doc_t hist;
    int      active;            /* the environment in use, -1 none */
    int      timeout_ms;
    bool     follow;
    int      max_kb;
    uint32_t state_mtime;
    uint32_t dirs_sig;          /* the folders' listing, to notice a file come or go */
} ct_store_t;

const char *ct_dir(void);                       /* .../data/cartero */
void ct_path(char *out, size_t n, const char *rel);
bool ct_write_file(const char *path, const void *data, size_t len);
char *ct_read_file(const char *path, size_t max, size_t *len);   /* PSRAM, NUL-terminated */
void ct_slug(char *out, size_t n, const char *name);

void ct_store_load(ct_store_t *st);
void ct_store_free(ct_store_t *st);
bool ct_store_poll(ct_store_t *st);             /* true if something changed on the card */
bool ct_doc_save(ct_doc_t *d, const char *dir);
bool ct_doc_refresh(ct_doc_t *d, const char *dir);  /* read again if the file changed */
void ct_state_save(ct_store_t *st);
const char *ct_env_name(const ct_store_t *st);

int  ct_coll_new(ct_store_t *st, const char *name);
bool ct_coll_delete(ct_store_t *st, int c);
int  ct_env_new(ct_store_t *st, const char *name);
bool ct_env_delete(ct_store_t *st, int e);

/* a node of a collection: the collection itself is depth 0 */
typedef struct {
    int     coll;
    char    slug[64];           /* the collection's file: its index may move when files come and go */
    int     depth;
    uint8_t path[CT_DEPTH_MAX];
} ct_ref_t;

ctv_t       ct_node(const ct_store_t *st, const ct_ref_t *r);
bool        ct_is_folder(ctv_t item);
const char *ct_item_name(ctv_t item);
ctv_t       ct_items(ctv_t node);               /* the "item" array of a collection or folder */

/* the request of an item, read */
const char *ct_req_method(ctv_t item);
const char *ct_req_url(ct_arena_t *a, ctv_t item);   /* raw, rebuilt from the parts if need be */
ctv_t       ct_req_headers(ctv_t item);
ctv_t       ct_req_body(ctv_t item);
ctv_t       ct_req_auth(ctv_t item);
char       *ct_req_script(ct_arena_t *a, ctv_t item, const char *listen);

/* and written (on a draft, in its own arena) */
ctv_t ct_req_ensure(ct_arena_t *a, ctv_t item);       /* item.request, made if missing */
void  ct_req_set_url(ct_arena_t *a, ctv_t item, const char *raw);
void  ct_req_set_script(ct_arena_t *a, ctv_t item, const char *listen, const char *src);
ctv_t ct_new_request(ct_arena_t *a, const char *name);

/* a request put back into its collection, or added to one, or removed:
 * the file is read again first and only that item changes */
bool ct_coll_put(ct_store_t *st, const ct_ref_t *r, ctv_t item, const char *old_name);
bool ct_coll_add(ct_store_t *st, const ct_ref_t *parent, ctv_t item, ct_ref_t *out);
bool ct_coll_remove(ct_store_t *st, const ct_ref_t *r);
bool ct_coll_add_folder(ct_store_t *st, const ct_ref_t *parent, const char *name);

/* the variables of a document ("values" or "variable") as name -> value */
ctv_t ct_vars_obj(ct_arena_t *a, ctv_t values);
void  ct_vars_set(ct_arena_t *a, ctv_t values, const char *k, ctv_t v);
void  ct_vars_unset(ctv_t values, const char *k);
/* a job's changes ([scope, key, value]) played onto the files */
int   ct_store_apply(ct_store_t *st, int coll, ctv_t changes);

void  ct_hist_add(ct_store_t *st, ctv_t entry);
bool  ct_hist_clear(ct_store_t *st);

/* ---- jobs ------------------------------------------------------------------------------------ */

typedef enum { CT_JOB_SEND, CT_JOB_RUN, CT_JOB_WEB } ct_job_kind_t;

typedef struct {
    char       name[96];
    char       method[12];
    char      *url;             /* resolved */
    ct_resp_t  resp;
    ctv_t      sent_hdr;        /* [[k, v]] as sent */
    char      *sent_body;
    int        sent_len;
    ctv_t      tests;
    char      *console;
    char       pre_err[256];
    char       test_err[256];
    bool       skipped, sent;
    int        npass, nfail;
    int        iteration;
    ctv_t      json;            /* the body parsed, for a single send */
    bool       json_ok;
} ct_exec_t;

typedef struct ct_job {
    ct_arena_t    ar;
    ct_job_kind_t kind;
    /* what to do */
    ctv_t         coll;         /* a copy of the collection (undefined for a loose request) */
    ctv_t         item;         /* SEND: the request as edited */
    int           depth;        /* where it sits (its folders give auth and scripts); RUN: the folder */
    uint8_t       path[CT_DEPTH_MAX];
    ctv_t         env, glob, collvars, local;
    char          env_name[96];
    char          title[128];
    int           iterations, delay_ms;
    bool          stop_on_fail;
    int           timeout_ms, max_body;
    bool          follow;
    /* WEB: the portal's request, and its number */
    int           web_n;
    char          web_err[160];
    /* shared with the screen */
    volatile bool stop;
    volatile bool done;
    volatile int  nexec;
    volatile int  planned;
    ct_exec_t    *ex[CT_RUN_MAX];
    /* results */
    ctv_t         changes;
    int           ms, tests, passed, failed, errors;
    bool          capped;
    char          note[160];
    ct_conn_t     conn;
} ct_job_t;

ct_job_t *ct_job_new(ct_job_kind_t kind);
bool      ct_job_start(ct_job_t *j);
void      ct_job_free(ct_job_t *j);       /* only once done */
int       ct_job_threads(void);
/* the request of the portal (pedido.json) read into a WEB job */
ct_job_t *ct_job_web(const char *pedido_json, size_t len, char *err, size_t en);
/* a run's results as the JSON of the README, and the history entry of an exec */
void      ct_job_results(const ct_job_t *j, ct_sb_t *out, const char *coll_name, const char *folder);
ctv_t     ct_exec_hist(ct_arena_t *a, const ct_exec_t *e, ctv_t item);

/* ---- the app and its screens ------------------------------------------------------------------ */

typedef enum { CT_SCR_HOME, CT_SCR_REQ, CT_SCR_RESP, CT_SCR_ENV, CT_SCR_VARS, CT_SCR_HIST, CT_SCR_RUN } ct_screen_t;

typedef struct {
    aos_app_t   *self;
    lv_obj_t    *root;
    int32_t      W, H;
    bool         land, hidden;
    ct_screen_t  screen;
    ct_store_t   st;

    /* the request being edited: a draft in its own arena */
    ct_arena_t   dar;
    ctv_t        draft;
    ctv_t        dorig;             /* the draft as it was opened or last saved */
    bool         has_draft, dirty, loose;
    ct_ref_t     dref;              /* where it lives (loose: nowhere yet) */
    char         dname[96];         /* its name when it was opened, to find it again */

    ct_job_t    *job;               /* the last send or run, kept for the screens */
    ct_job_t    *web;               /* the portal's request through the board */
    int          view_exec;         /* the exec the response screen shows */
    ct_screen_t  resp_back;         /* where back goes from the response */
    ct_ref_t     run_ref;
    int          vars_env;          /* the environment the variables screen edits; -1 the globals */
    uint64_t     poll_t, live_t;
} ct_app_t;

extern ct_app_t ct_app;
#define CA ct_app

void ct_build(void);
void ct_go(ct_screen_t s);
void ct_open_request(const ct_ref_t *r);
void ct_open_loose(ctv_t request_like);         /* a request from the history, or a new one */
void ct_send(void);
void ct_run_start(const ct_ref_t *r, int iterations, int delay_ms, bool stop_on_fail);
void ct_job_finished(ct_job_t *j);

/* screens */
void ct_home_build(void);
void ct_req_build(void);
void ct_req_commit(void);                       /* the widgets' text into the draft */
bool ct_req_back(void);
void ct_req_close(void);
void ct_resp_build(void);
void ct_resp_tick(void);
bool ct_resp_back(void);
void ct_env_build(void);
void ct_vars_build(void);
bool ct_vars_back(void);
void ct_hist_build(void);
void ct_run_build(void);
void ct_run_tick(void);
bool ct_run_back(void);

/* pieces (ct_ui.c) */
lv_obj_t *ct_box(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *ct_page(void);
lv_obj_t *ct_header(lv_obj_t *page, const char *title, bool back);
void      ct_header_fit(lv_obj_t *right);
lv_obj_t *ct_scroll(lv_obj_t *page, int32_t top);
lv_obj_t *ct_round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud);
lv_obj_t *ct_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud);
lv_obj_t *ct_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud);
lv_obj_t *ct_card(lv_obj_t *parent);
lv_obj_t *ct_label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c);
lv_obj_t *ct_label_dots(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c, int32_t w);
lv_obj_t *ct_label_wrap(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c);
lv_obj_t *ct_textarea(lv_obj_t *parent, const char *text, bool one_line, bool mono);
void      ct_set_text(lv_obj_t *l, const char *t);
lv_color_t ct_method_color(const char *m);
lv_color_t ct_status_color(int status);
lv_obj_t *ct_method_badge(lv_obj_t *parent, const char *m, int32_t w);
void      ct_fmt_size(char *out, size_t n, long bytes);
void      ct_fmt_ms(char *out, size_t n, int ms);
const char *ct_err_text(const ct_resp_t *r);   /* why a request got no answer, for the screen */
lv_obj_t *ct_seg(lv_obj_t *parent, const char *const *labels, int n, int sel, lv_event_cb_t cb);

/* the keyboard of a screen: shown under the text area that has the focus */
void      ct_kb_attach(lv_obj_t *page, lv_obj_t *content);
void      ct_kb_watch(lv_obj_t *ta);
void      ct_kb_hide(void);
bool      ct_kb_visible(void);
void      ct_kb_forget(void);

/* a sheet from the bottom: a title and a list of choices */
typedef void (*ct_pick_cb_t)(int choice, void *ud);
void      ct_sheet(const char *title, const char *const *items, int n, int sel, ct_pick_cb_t cb, void *ud);
/* a question with a text to type */
typedef void (*ct_ask_cb_t)(const char *text, void *ud);
void      ct_ask(const char *title, const char *initial, ct_ask_cb_t cb, void *ud);
void      ct_confirm(const char *title, const char *yes, ct_pick_cb_t cb, void *ud);
void      ct_sheet_close(void);
bool      ct_sheet_open(void);
void      ct_sheet_forget(void);           /* the screen was rebuilt under it */
void      ct_sheet_cancel_async(void);     /* the app goes: nothing may be left queued */
