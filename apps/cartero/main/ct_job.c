/*
 * P4OS - Cartero: a job is one send or one run of a collection, done in a
 * thread of its own so the screen never waits on the network.
 *
 * The job gets copies of everything it needs (the collection, the
 * variables) in its own arena, so the screen may go on and even reload the
 * files while it runs; what the scripts changed comes back as a list of
 * changes (ct_script_t.changes) that the screen plays onto the files when
 * the job is done. Each request goes as Postman sends it: the collection's,
 * the folders' and the request's pre-request scripts; the variables; the
 * auth taken from the nearest that has one; the body by its mode; then the
 * test scripts in the same order.
 *
 * The screen reads ex[0 .. nexec) while the job runs: an exec is published
 * (nexec grows) only once it is complete and never touched again.
 *
 * The third kind, WEB, is the portal's: a request already made by the
 * page's own scripts, which the board only carries (README, "Sending
 * through the board"). Its answer goes to the card and the live channel.
 */
#include "ct_app.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define STACK      (96 * 1024)
#define RUN_KEEP   (256 * 1024)        /* a run keeps this much of each body */
#define RUN_TOTAL  (6 * 1024 * 1024)
#define JSON_MAX   (4 * 1024 * 1024)   /* a single send parses this much for the tree */

static volatile int s_threads;

int ct_job_threads(void)
{
    return s_threads;
}

ct_job_t *ct_job_new(ct_job_kind_t kind)
{
    ct_job_t *j = ct_psram(sizeof *j);
    if (!j) return NULL;
    memset(j, 0, sizeof *j);
    ct_arena_init(&j->ar, CT_JOB_ARENA);
    j->kind = kind;
    j->iterations = 1;
    j->timeout_ms = 30000;
    j->follow = true;
    j->max_body = 4 * 1024 * 1024;
    j->env = ct_obj_new(&j->ar, 0);
    j->glob = ct_obj_new(&j->ar, 0);
    j->collvars = ct_obj_new(&j->ar, 0);
    j->local = ct_obj_new(&j->ar, 0);
    j->changes = ct_arr_new(&j->ar, 0);
    return j;
}

void ct_job_free(ct_job_t *j)
{
    if (!j) return;
    for (int i = 0; i < j->nexec; i++) if (j->ex[i]) ct_resp_free(&j->ex[i]->resp);
    ct_http_close(&j->conn);
    ct_arena_free(&j->ar);
    free(j);
}

/* ---- building a request ------------------------------------------------------------------- */

typedef struct {
    ctv_t item;
    ctv_t chain[CT_DEPTH_MAX + 1];  /* the collection, then each folder down to the item */
    int   nchain;
} plan_t;

static char *res(ct_job_t *j, ct_script_t *ctx, const char *s)
{
    char *r = ct_resolve(&j->ar, ctx, s);
    return r ? r : (char *)"";
}

static ctv_t find_auth(const plan_t *p)
{
    ctv_t a = ct_req_auth(p->item);
    if (a.t == CT_OBJ && strcmp(ct_s(ct_get(a, "type")), "inherit")) return a;
    for (int i = p->nchain - 1; i >= 0; i--) {
        a = ct_get(p->chain[i], "auth");
        if (a.t == CT_OBJ && strcmp(ct_s(ct_get(a, "type")), "inherit")) return a;
    }
    return ct_undef();
}

/* a parameter of an auth: v2.1's [{key, value}] or v2.0's {key: value} */
static const char *auth_param(ctv_t auth, const char *type, const char *key)
{
    ctv_t list = ct_get(auth, type);
    if (list.t == CT_OBJ) return ct_s(ct_get(list, key));
    for (uint32_t i = 0; i < ct_len(list); i++) {
        ctv_t e = ct_at(list, i);
        if (!strcmp(ct_s(ct_get(e, "key")), key)) {
            ctv_t v = ct_get(e, "value");
            return v.t == CT_STR ? v.s->p : "";
        }
    }
    return "";
}

static bool hdr_has(const ct_http_req_t *rq, const char *k)
{
    for (int i = 0; i < rq->nhdr; i++) if (!strcasecmp(rq->hdr[i].k, k)) return true;
    return false;
}

static void hdr_add(ct_http_req_t *rq, const char *k, const char *v)
{
    if (rq->nhdr < CT_HDR_MAX) {
        rq->hdr[rq->nhdr].k = k;
        rq->hdr[rq->nhdr].v = v;
        rq->nhdr++;
    }
}

static char *sb_take(ct_job_t *j, ct_sb_t *b)
{
    char *o = ct_alloc(&j->ar, b->n + 1);
    if (o) {
        if (b->p) memcpy(o, b->p, b->n);
        o[b->n] = '\0';
    }
    ct_sb_free(b);
    return o;
}

static char *url_enc(ct_job_t *j, const char *s)
{
    size_t n = strlen(s) * 3 + 1;
    char *o = ct_alloc(&j->ar, n);
    if (o) ct_url_enc(o, n, s, false);
    return o ? o : (char *)"";
}

/* the body by its mode, and the type it goes as */
static void build_body(ct_job_t *j, ct_script_t *ctx, ctv_t body, ct_http_req_t *rq, const char **ctype, char *note,
                       size_t nn)
{
    const char *mode = ct_s(ct_get(body, "mode"));
    *ctype = NULL;
    ct_sb_t b = { 0 };
    if (!strcmp(mode, "raw")) {
        const char *raw = ct_s(ct_get(body, "raw"));
        ct_sb_adds(&b, res(j, ctx, raw));
        const char *lang = ct_s(ct_get(ct_get(ct_get(body, "options"), "raw"), "language"));
        if (!strcmp(lang, "json")) *ctype = "application/json";
        else if (!strcmp(lang, "xml")) *ctype = "application/xml";
        else if (!strcmp(lang, "html")) *ctype = "text/html";
        else if (!strcmp(lang, "javascript")) *ctype = "application/javascript";
        else *ctype = "text/plain";
    } else if (!strcmp(mode, "urlencoded")) {
        ctv_t list = ct_get(body, "urlencoded");
        int k = 0;
        for (uint32_t i = 0; i < ct_len(list); i++) {
            ctv_t e = ct_at(list, i);
            if (ct_truthy(ct_get(e, "disabled"))) continue;
            if (k++) ct_sb_addc(&b, '&');
            ct_sb_adds(&b, url_enc(j, res(j, ctx, ct_s(ct_get(e, "key")))));
            ct_sb_addc(&b, '=');
            ct_sb_adds(&b, url_enc(j, res(j, ctx, ct_s(ct_get(e, "value")))));
        }
        *ctype = "application/x-www-form-urlencoded";
    } else if (!strcmp(mode, "formdata")) {
        char boundary[48];
        snprintf(boundary, sizeof boundary, "----CarteroBoundary%08x%04x", (unsigned)aos_hal_uptime_us(),
                 (unsigned)(aos_hal_uptime_ms() & 0xFFFF));
        ctv_t list = ct_get(body, "formdata");
        for (uint32_t i = 0; i < ct_len(list); i++) {
            ctv_t e = ct_at(list, i);
            if (ct_truthy(ct_get(e, "disabled"))) continue;
            if (!strcmp(ct_s(ct_get(e, "type")), "file")) {
                ct_copy(note, nn, "file fields are not sent from the board");
                continue;
            }
            ct_sb_addf(&b, "--%s\r\nContent-Disposition: form-data; name=\"", boundary);
            ct_sb_adds(&b, res(j, ctx, ct_s(ct_get(e, "key"))));
            ct_sb_adds(&b, "\"\r\n");
            const char *ct = ct_s(ct_get(e, "contentType"));
            if (ct[0]) ct_sb_addf(&b, "Content-Type: %.100s\r\n", ct);
            ct_sb_adds(&b, "\r\n");
            ct_sb_adds(&b, res(j, ctx, ct_s(ct_get(e, "value"))));
            ct_sb_adds(&b, "\r\n");
        }
        ct_sb_addf(&b, "--%s--\r\n", boundary);
        char *t = ct_alloc(&j->ar, 96);
        if (t) snprintf(t, 96, "multipart/form-data; boundary=%s", boundary);
        *ctype = t;
    } else if (!strcmp(mode, "graphql")) {
        ctv_t g = ct_get(body, "graphql");
        const char *q = res(j, ctx, ct_s(ct_get(g, "query")));
        const char *vars = res(j, ctx, ct_s(ct_get(g, "variables")));
        ct_sb_adds(&b, "{\"query\":");
        ct_json_str(&b, q, strlen(q));
        ct_sb_adds(&b, ",\"variables\":");
        ctv_t pv;
        const char *p = vars;
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        if (*p && ct_json_parse(&j->ar, vars, strlen(vars), &pv, NULL, 0)) ct_json_write(&b, pv, 0);
        else ct_sb_adds(&b, "{}");
        ct_sb_addc(&b, '}');
        *ctype = "application/json";
    } else if (!strcmp(mode, "file")) {
        ct_copy(note, nn, "a file body is not sent from the board");
    }
    if (b.n || b.p) {
        rq->blen = (int)b.n;
        rq->body = sb_take(j, &b);
    } else {
        ct_sb_free(&b);
    }
}

/* the pm.request a script sees and may change: method, the URL as text,
 * the headers and the body, copies of the item's */
static ctv_t req_obj(ct_job_t *j, ctv_t item)
{
    ct_arena_t *a = &j->ar;
    ctv_t r = ct_obj_new(a, 4);
    ct_sets(a, r, "method", ct_req_method(item));
    ct_sets(a, r, "url", ct_req_url(a, item));
    ctv_t h = ct_deep_copy(a, ct_req_headers(item));
    ct_setv(a, r, "header", h.t == CT_ARR ? h : ct_arr_new(a, 0));
    ctv_t b = ct_deep_copy(a, ct_req_body(item));
    ct_setv(a, r, "body", b.t == CT_OBJ ? b : ct_obj_new(a, 0));
    return r;
}

static void run_scripts(ct_job_t *j, ct_script_t *ctx, const plan_t *p, const char *listen, char *err, size_t en)
{
    ctx->event = listen;
    for (int i = 0; i <= p->nchain; i++) {
        ctv_t node = i < p->nchain ? p->chain[i] : p->item;
        char *src = ct_req_script(&j->ar, node, listen);
        if (!src || !src[0]) continue;
        if (!ct_js_run(ctx, src)) {
            const char *where = i == p->nchain ? "" : i == 0 ? "collection: " : "folder: ";
            snprintf(err, en, "%s%s", where, ctx->error);
            if (!strcmp(listen, "prerequest")) return;
        }
        if (j->stop) return;
    }
}

static void exec_one(ct_job_t *j, const plan_t *p, int iteration, ct_exec_t *e, char *next, bool *next_set,
                     bool *next_stop)
{
    ct_copy(e->name, sizeof e->name, ct_item_name(p->item));
    e->iteration = iteration;
    ct_sb_t con = { 0 };
    ct_script_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ar = &j->ar;
    ctx.env = j->env;
    ctx.glob = j->glob;
    ctx.coll = j->collvars;
    ctx.local = j->local;
    ctx.changes = j->changes;
    ctx.console = &con;
    ctx.request_name = e->name;
    ctx.iteration = iteration;
    ctx.stop = &j->stop;
    ctx.req = req_obj(j, p->item);

    run_scripts(j, &ctx, p, "prerequest", e->pre_err, sizeof e->pre_err);
    ct_copy(e->method, sizeof e->method, ct_s(ct_get(ctx.req, "method")));
    if (e->pre_err[0] || ctx.skip || j->stop) {
        e->skipped = ctx.skip;
        goto done;
    }

    /* the request, with its variables resolved */
    ct_http_req_t rq;
    memset(&rq, 0, sizeof rq);
    rq.method = e->method[0] ? e->method : "GET";
    ct_str_t *url = ct_to_str(&j->ar, ct_get(ctx.req, "url"));
    char *u = res(j, &ctx, url ? url->p : "");
    while (*u == ' ') u++;
    /* a URL without a scheme goes as http://, as Postman does */
    if (*u && !strstr(u, "://")) {
        size_t n = strlen(u) + 8;
        char *w = ct_alloc(&j->ar, n);
        if (w) {
            snprintf(w, n, "http://%s", u);
            u = w;
        }
    }
    ctv_t hs = ct_get(ctx.req, "header");
    for (uint32_t i = 0; i < ct_len(hs); i++) {
        ctv_t h = ct_at(hs, i);
        if (ct_truthy(ct_get(h, "disabled"))) continue;
        const char *k = res(j, &ctx, ct_s(ct_get(h, "key")));
        if (!k[0]) continue;
        ct_str_t *v = ct_to_str(&j->ar, ct_get(h, "value"));
        hdr_add(&rq, k, res(j, &ctx, v && ct_get(h, "value").t != CT_UNDEF ? v->p : ""));
    }
    /* auth */
    ctv_t auth = find_auth(p);
    const char *at = ct_s(ct_get(auth, "type"));
    if (!strcmp(at, "bearer") && !hdr_has(&rq, "Authorization")) {
        const char *tok = res(j, &ctx, auth_param(auth, "bearer", "token"));
        size_t n = strlen(tok) + 8;
        char *v = ct_alloc(&j->ar, n);
        if (v) {
            snprintf(v, n, "Bearer %s", tok);
            hdr_add(&rq, "Authorization", v);
        }
    } else if (!strcmp(at, "basic") && !hdr_has(&rq, "Authorization")) {
        const char *us = res(j, &ctx, auth_param(auth, "basic", "username"));
        const char *pw = res(j, &ctx, auth_param(auth, "basic", "password"));
        size_t n = strlen(us) + strlen(pw) + 2;
        char *up = ct_alloc(&j->ar, n);
        size_t bn = n * 4 / 3 + 16;
        char *v = ct_alloc(&j->ar, bn + 8);
        if (up && v) {
            snprintf(up, n, "%s:%s", us, pw);
            memcpy(v, "Basic ", 6);
            ct_b64(v + 6, bn, up, strlen(up));
            hdr_add(&rq, "Authorization", v);
        }
    } else if (!strcmp(at, "apikey")) {
        const char *k = res(j, &ctx, auth_param(auth, "apikey", "key"));
        const char *v = res(j, &ctx, auth_param(auth, "apikey", "value"));
        const char *in = auth_param(auth, "apikey", "in");
        if (k[0] && !strcmp(in, "query")) {
            size_t n = strlen(u) + strlen(k) * 3 + strlen(v) * 3 + 4;
            char *w = ct_alloc(&j->ar, n);
            if (w) {
                snprintf(w, n, "%s%c%s=%s", u, strchr(u, '?') ? '&' : '?', url_enc(j, k), url_enc(j, v));
                u = w;
            }
        } else if (k[0] && !hdr_has(&rq, k)) {
            hdr_add(&rq, k, v);
        }
    }
    rq.url = u;
    e->url = u;
    /* the body */
    const char *ctype = NULL;
    char note[96] = "";
    bool bodyless = !strcasecmp(rq.method, "GET") || !strcasecmp(rq.method, "HEAD");
    ctv_t body = ct_get(ctx.req, "body");
    if (!(bodyless && !ct_s(ct_get(body, "raw"))[0] && strcmp(ct_s(ct_get(body, "mode")), "urlencoded"))) {
        build_body(j, &ctx, body, &rq, &ctype, note, sizeof note);
    }
    if (ctype && rq.body && !hdr_has(&rq, "Content-Type")) hdr_add(&rq, "Content-Type", ctype);
    rq.timeout_ms = j->timeout_ms;
    rq.follow = j->follow;
    rq.max_body = j->max_body;

    /* what goes, for the screen: ours and the ones the client adds */
    e->sent_hdr = ct_arr_new(&j->ar, (uint32_t)rq.nhdr + 6);
    for (int i = 0; i < rq.nhdr; i++) {
        ctv_t pr = ct_arr_new(&j->ar, 2);
        ct_arr_push(&j->ar, pr.a, ct_cstr(&j->ar, rq.hdr[i].k));
        ct_arr_push(&j->ar, pr.a, ct_cstr(&j->ar, rq.hdr[i].v));
        ct_arr_push(&j->ar, e->sent_hdr.a, pr);
    }
    static const char *const AUTO[][2] = {
        { "User-Agent", "Cartero/1.0 (P4OS)" }, { "Accept", "*/*" }, { "Accept-Encoding", "identity" },
        { "Connection", "keep-alive" },
    };
    for (size_t i = 0; i < sizeof AUTO / sizeof AUTO[0]; i++) {
        if (hdr_has(&rq, AUTO[i][0])) continue;
        ctv_t pr = ct_arr_new(&j->ar, 2);
        ct_arr_push(&j->ar, pr.a, ct_cstr(&j->ar, AUTO[i][0]));
        ct_arr_push(&j->ar, pr.a, ct_cstr(&j->ar, AUTO[i][1]));
        ct_arr_push(&j->ar, e->sent_hdr.a, pr);
    }
    e->sent_body = (char *)rq.body;
    e->sent_len = rq.blen;
    if (note[0]) snprintf(e->pre_err, sizeof e->pre_err, "%s", note);

    ct_http_do(&j->conn, &rq, &e->resp, &j->stop);
    e->sent = true;
    if (j->stop) goto done;

    /* the tests */
    ctx.resp = &e->resp;
    ctx.resp_json_done = false;
    run_scripts(j, &ctx, p, "test", e->test_err, sizeof e->test_err);

done:
    j->env = ctx.env;
    j->glob = ctx.glob;
    j->collvars = ctx.coll;
    j->local = ctx.local;
    j->changes = ctx.changes;
    e->tests = ctx.tests;
    for (uint32_t i = 0; i < ct_len(e->tests); i++) {
        ctv_t t = ct_at(e->tests, i);
        if (ct_truthy(ct_get(t, "skipped"))) continue;
        if (ct_truthy(ct_get(t, "ok"))) e->npass++;
        else e->nfail++;
    }
    e->console = sb_take(j, &con);
    if (ctx.next_set) {
        *next_set = true;
        *next_stop = ctx.next_stop;
        ct_copy(next, 96, ctx.next);
    }
}

/* ---- plans ---------------------------------------------------------------------------------------- */

static int flatten(ctv_t node, const ctv_t *chain, int nchain, plan_t *out, int n, int max)
{
    ctv_t items = ct_items(node);
    for (uint32_t i = 0; i < ct_len(items) && n < max; i++) {
        ctv_t it = items.a->v[i];
        if (ct_is_folder(it)) {
            if (nchain >= CT_DEPTH_MAX) continue;
            ctv_t c2[CT_DEPTH_MAX + 1];
            memcpy(c2, chain, sizeof(ctv_t) * (size_t)nchain);
            c2[nchain] = it;
            n = flatten(it, c2, nchain + 1, out, n, max);
        } else {
            out[n].item = it;
            memcpy(out[n].chain, chain, sizeof(ctv_t) * (size_t)nchain);
            out[n].nchain = nchain;
            n++;
        }
    }
    return n;
}

static ct_exec_t *new_exec(ct_job_t *j)
{
    ct_exec_t *e = ct_alloc(&j->ar, sizeof *e);
    return e;
}

static void publish(ct_job_t *j, ct_exec_t *e)
{
    j->ex[j->nexec] = e;
    __sync_synchronize();
    j->nexec++;
}

static void pause_ms(ct_job_t *j, int ms)
{
    for (int t = 0; t < ms && !j->stop; t += 50) aos_hal_sleep_ms(50);
}

static void do_send(ct_job_t *j)
{
    plan_t p;
    memset(&p, 0, sizeof p);
    p.item = j->item;
    if (j->coll.t == CT_OBJ) {
        p.chain[p.nchain++] = j->coll;
        ctv_t n = j->coll;
        for (int i = 0; i + 1 < j->depth && p.nchain <= CT_DEPTH_MAX; i++) {
            n = ct_at(ct_items(n), j->path[i]);
            if (n.t != CT_OBJ) break;
            p.chain[p.nchain++] = n;
        }
    }
    j->planned = 1;
    ct_exec_t *e = new_exec(j);
    if (!e) {
        ct_copy(j->note, sizeof j->note, "out of memory");
        return;
    }
    char next[96];
    bool ns = false, nstop = false;
    exec_one(j, &p, 0, e, next, &ns, &nstop);
    /* the body as a tree, for the screen */
    if (e->sent && e->resp.ok && e->resp.len && e->resp.len <= JSON_MAX) {
        const char *b = (const char *)e->resp.body;
        int i = 0;
        while (i < e->resp.len && (b[i] == ' ' || b[i] == '\n' || b[i] == '\r' || b[i] == '\t')) i++;
        if (i < e->resp.len && (b[i] == '{' || b[i] == '[')) {
            e->json_ok = ct_json_parse(&j->ar, b, (size_t)e->resp.len, &e->json, NULL, 0);
        }
    }
    j->tests = e->npass + e->nfail;
    j->passed = e->npass;
    j->failed = e->nfail;
    j->errors = (e->pre_err[0] || e->test_err[0] || (e->sent && !e->resp.ok)) ? 1 : 0;
    publish(j, e);
}

static void do_run(ct_job_t *j)
{
    plan_t *plans = ct_psram(sizeof(plan_t) * CT_RUN_MAX);
    if (!plans) {
        ct_copy(j->note, sizeof j->note, "out of memory");
        return;
    }
    ctv_t chain[CT_DEPTH_MAX + 1];
    int nchain = 0;
    ctv_t node = j->coll;
    chain[nchain++] = node;
    for (int i = 0; i < j->depth; i++) {
        node = ct_at(ct_items(node), j->path[i]);
        if (node.t != CT_OBJ) break;
        chain[nchain++] = node;
    }
    int n = flatten(node, chain, nchain, plans, 0, CT_RUN_MAX);
    j->planned = n * (j->iterations > 0 ? j->iterations : 1);
    size_t kept = 0;
    for (int it = 0; it < j->iterations && !j->stop; it++) {
        int i = 0;
        while (i < n && !j->stop) {
            if (j->nexec >= CT_RUN_MAX) {
                j->capped = true;
                break;
            }
            ct_exec_t *e = new_exec(j);
            if (!e) {
                ct_copy(j->note, sizeof j->note, "out of memory");
                j->stop = true;
                break;
            }
            char next[96] = "";
            bool ns = false, nstop = false;
            exec_one(j, &plans[i], it, e, next, &ns, &nstop);
            /* a run keeps only the start of each body */
            if (e->resp.body && e->resp.len > RUN_KEEP) {
                e->resp.len = RUN_KEEP;
                e->resp.body[RUN_KEEP] = '\0';
                e->resp.cut = true;
            }
            kept += (size_t)e->resp.len;
            if (kept > RUN_TOTAL && e->resp.body) {
                free(e->resp.body);
                e->resp.body = NULL;
                e->resp.len = 0;
            }
            j->tests += e->npass + e->nfail;
            j->passed += e->npass;
            j->failed += e->nfail;
            bool err = e->pre_err[0] || e->test_err[0] || (e->sent && !e->resp.ok);
            if (err) j->errors++;
            publish(j, e);
            if (j->stop_on_fail && (e->nfail || err)) {
                j->stop = true;
                ct_copy(j->note, sizeof j->note, "stopped at the first failure");
                break;
            }
            if (ns) {
                if (nstop) {
                    i = n;
                } else {
                    int to = -1;
                    for (int k = 0; k < n; k++) if (!strcmp(ct_item_name(plans[k].item), next)) to = k;
                    if (to < 0) snprintf(j->note, sizeof j->note, "setNextRequest: '%.60s' is not in the run", next);
                    i = to >= 0 ? to : n;
                }
            } else {
                i++;
            }
            if (i < n && j->delay_ms > 0) pause_ms(j, j->delay_ms);
        }
        if (j->capped) {
            snprintf(j->note, sizeof j->note, "the run stopped at %d requests", CT_RUN_MAX);
            break;
        }
    }
    free(plans);
}

/* ---- the portal's request ------------------------------------------------------------------------- */

ct_job_t *ct_job_web(const char *text, size_t len, char *err, size_t en)
{
    ct_job_t *j = ct_job_new(CT_JOB_WEB);
    if (!j) {
        ct_copy(err, en, "no memory");
        return NULL;
    }
    char why[160];
    if (!ct_json_parse(&j->ar, text, len, &j->item, why, sizeof why) || j->item.t != CT_OBJ) {
        snprintf(err, en, "pedido.json: %s", why);
        ct_job_free(j);
        return NULL;
    }
    j->web_n = (int)ct_to_num(ct_get(j->item, "n"));
    ctv_t to = ct_get(j->item, "timeout");
    if (to.t == CT_NUM && to.n >= 500 && to.n <= 300000) j->timeout_ms = (int)to.n;
    ctv_t f = ct_get(j->item, "follow");
    if (f.t == CT_BOOL) j->follow = f.b;
    return j;
}

static const char *web_err(int code)
{
    switch (code) {
    case AOS_HTTP_ERR_DNS: return "no se encontró el servidor (DNS)";
    case AOS_HTTP_ERR_CONNECT: return "no se pudo conectar";
    case AOS_HTTP_ERR_SEND: return "no se pudo enviar el pedido";
    case AOS_HTTP_ERR_RECV: return "la respuesta se cortó";
    case AOS_HTTP_ERR_PROTO: return "la respuesta no es HTTP";
    case AOS_HTTP_ERR_MEM: return "no hay memoria";
    case AOS_HTTP_ERR_SIN_HORA: return "la placa todavía no tiene hora (para verificar el certificado)";
    case AOS_HTTP_ERR_TLS: return "el certificado del servidor no se pudo verificar";
    case CT_ERR_URL: return "la dirección no es http:// ni https://";
    case CT_ERR_TIMEOUT: return "se acabó el tiempo de espera";
    case CT_ERR_STOPPED: return "cancelado";
    case CT_ERR_TOOMANY: return "demasiadas redirecciones";
    default: return "falló el pedido";
    }
}

static void do_web(ct_job_t *j)
{
    ct_http_req_t rq;
    memset(&rq, 0, sizeof rq);
    rq.method = ct_s(ct_get(j->item, "method"));
    if (!rq.method[0]) rq.method = "GET";
    rq.url = ct_s(ct_get(j->item, "url"));
    ctv_t hs = ct_get(j->item, "headers");
    for (uint32_t i = 0; i < ct_len(hs); i++) {
        ctv_t h = ct_at(hs, i);
        const char *k = h.t == CT_ARR ? ct_s(ct_at(h, 0)) : ct_s(ct_get(h, "key"));
        const char *v = h.t == CT_ARR ? ct_s(ct_at(h, 1)) : ct_s(ct_get(h, "value"));
        if (k[0]) hdr_add(&rq, k, v);
    }
    ctv_t body = ct_get(j->item, "body");
    if (body.t == CT_STR) {
        rq.body = body.s->p;
        rq.blen = (int)body.s->len;
    }
    rq.timeout_ms = j->timeout_ms;
    rq.follow = j->follow;
    rq.max_body = j->max_body;
    ct_exec_t *e = new_exec(j);
    if (!e) return;
    ct_copy(e->method, sizeof e->method, rq.method);
    ct_http_do(&j->conn, &rq, &e->resp, &j->stop);
    e->sent = true;
    ct_resp_t *r = &e->resp;

    /* the body to the card, always; the answer to the live channel */
    char path[300];
    ct_path(path, sizeof path, "web/respuesta.bin");
    ct_write_file(path, r->body ? r->body : (const uint8_t *)"", (size_t)r->len);
    bool text = r->ok && r->body && ct_body_is_text(r);
    ct_sb_t b = { 0 };
    ct_sb_addf(&b, "{\"n\":%d,\"ok\":%s,\"status\":%d,\"reason\":", j->web_n, r->ok ? "true" : "false", r->status);
    ct_json_str(&b, r->reason, strlen(r->reason));
    ct_sb_addf(&b, ",\"ms\":%d,\"times\":{\"connect\":%d,\"send\":%d,\"wait\":%d,\"download\":%d},\"headers\":[",
               r->t_total, r->t_connect, r->t_send, r->t_wait, r->t_download);
    for (int i = 0; i < r->nhdr; i++) {
        ct_sb_adds(&b, i ? ",[" : "[");
        ct_json_str(&b, r->hdr[i].k, strlen(r->hdr[i].k));
        ct_sb_addc(&b, ',');
        ct_json_str(&b, r->hdr[i].v, strlen(r->hdr[i].v));
        ct_sb_addc(&b, ']');
    }
    ct_sb_addf(&b, "],\"size\":%d,\"cut\":%s,\"binary\":%s,\"redirects\":%d,\"final_url\":", r->len,
               r->cut ? "true" : "false", r->ok && !text ? "true" : "false", r->redirects);
    ct_json_str(&b, r->final_url, strlen(r->final_url));
    ct_sb_adds(&b, ",\"body\":");
    if (text && r->len < 48 * 1024) ct_json_str(&b, (const char *)r->body, (size_t)r->len);
    else ct_sb_adds(&b, "\"\"");
    ct_sb_adds(&b, ",\"file\":\"/data/cartero/web/respuesta.bin\",\"error\":");
    char why[260] = "";
    if (!r->ok) {
        if (r->err[0] && r->code != CT_ERR_URL) snprintf(why, sizeof why, "%s: %.190s", web_err(r->code), r->err);
        else ct_copy(why, sizeof why, web_err(r->code));
    }
    ct_json_str(&b, why, strlen(why));
    ct_sb_adds(&b, "}");
    if (!b.oom) aos_hal_live_put(CT_APP_ID, "respuesta", "application/json", b.p, b.n);
    ct_sb_free(&b);
    publish(j, e);
}

/* ---- the thread ------------------------------------------------------------------------------------ */

static void job_thread(void *arg)
{
    ct_job_t *j = arg;
    uint64_t t0 = aos_hal_uptime_ms();
    switch (j->kind) {
    case CT_JOB_SEND: do_send(j); break;
    case CT_JOB_RUN: do_run(j); break;
    case CT_JOB_WEB: do_web(j); break;
    }
    ct_http_close(&j->conn);
    j->ms = (int)(aos_hal_uptime_ms() - t0);
    __sync_synchronize();
    j->done = true;
    __sync_fetch_and_sub(&s_threads, 1);
}

bool ct_job_start(ct_job_t *j)
{
    __sync_fetch_and_add(&s_threads, 1);
    if (!aos_hal_thread_start("cartero", job_thread, j, STACK, 4)) {
        __sync_fetch_and_sub(&s_threads, 1);
        return false;
    }
    return true;
}

/* ---- results ------------------------------------------------------------------------------------- */

static void jstr(ct_sb_t *b, const char *s)
{
    ct_json_str(b, s ? s : "", s ? strlen(s) : 0);
}

void ct_job_results(const ct_job_t *j, ct_sb_t *b, const char *coll_name, const char *folder)
{
    ct_sb_adds(b, "{\n\t\"collection\": ");
    jstr(b, coll_name);
    ct_sb_adds(b, ",\n\t\"folder\": ");
    jstr(b, folder);
    ct_sb_adds(b, ",\n\t\"environment\": ");
    jstr(b, j->env_name);
    ct_sb_addf(b, ",\n\t\"t\": %lld,\n\t\"ms\": %d,\n\t\"iterations\": %d,\n\t\"requests\": %d,\n\t\"tests\": %d,"
                  "\n\t\"passed\": %d,\n\t\"failed\": %d,\n\t\"errors\": %d,\n\t\"note\": ",
               aos_hal_time_is_valid() ? (long long)time(NULL) : 0LL, j->ms, j->iterations, j->nexec, j->tests,
               j->passed, j->failed, j->errors);
    jstr(b, j->note);
    ct_sb_adds(b, ",\n\t\"items\": [");
    for (int i = 0; i < j->nexec; i++) {
        const ct_exec_t *e = j->ex[i];
        ct_sb_adds(b, i ? ",\n\t\t{" : "\n\t\t{");
        ct_sb_adds(b, "\"name\": ");
        jstr(b, e->name);
        ct_sb_adds(b, ", \"method\": ");
        jstr(b, e->method);
        ct_sb_adds(b, ", \"url\": ");
        jstr(b, e->url);
        ct_sb_addf(b, ", \"iteration\": %d, \"status\": %d, \"ms\": %d, \"size\": %d, \"error\": ", e->iteration,
                   e->resp.ok ? e->resp.status : 0, e->resp.t_total, e->resp.len);
        char err[300] = "";
        if (e->pre_err[0]) ct_copy(err, sizeof err, e->pre_err);
        else if (e->sent && !e->resp.ok) snprintf(err, sizeof err, "%s %.200s", web_err(e->resp.code), e->resp.err);
        else if (e->test_err[0]) ct_copy(err, sizeof err, e->test_err);
        else if (e->skipped) ct_copy(err, sizeof err, "skipped");
        jstr(b, err);
        ct_sb_adds(b, ", \"tests\": [");
        for (uint32_t k = 0; k < ct_len(e->tests); k++) {
            ctv_t t = ct_at(e->tests, k);
            ct_sb_adds(b, k ? ", {\"name\": " : "{\"name\": ");
            jstr(b, ct_s(ct_get(t, "name")));
            ct_sb_addf(b, ", \"ok\": %s, \"error\": ", ct_truthy(ct_get(t, "ok")) ? "true" : "false");
            jstr(b, ct_s(ct_get(t, "error")));
            ct_sb_addc(b, '}');
        }
        ct_sb_adds(b, "]}");
    }
    ct_sb_adds(b, j->nexec ? "\n\t]\n}\n" : "]\n}\n");
}

ctv_t ct_exec_hist(ct_arena_t *a, const ct_exec_t *e, ctv_t item)
{
    ctv_t h = ct_obj_new(a, 9);
    ct_setv(a, h, "t", ct_num(aos_hal_time_is_valid() ? (double)time(NULL) : 0));
    ct_sets(a, h, "name", e->name);
    ct_sets(a, h, "method", e->method);
    ct_sets(a, h, "url", e->url ? e->url : "");
    ct_setv(a, h, "status", ct_num(e->resp.ok ? e->resp.status : 0));
    ct_setv(a, h, "ms", ct_num(e->resp.t_total));
    ct_setv(a, h, "size", ct_num(e->resp.len));
    ctv_t r = ct_deep_copy(a, ct_get(item, "request"));
    ct_setv(a, h, "request", r.t == CT_UNDEF ? ct_obj_new(a, 0) : r);
    return h;
}
