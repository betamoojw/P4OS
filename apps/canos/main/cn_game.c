/*
 * CAÑOS - the rules (see cn_game.h)
 */
#include "cn_game.h"
#include "cn_world.h"

#include <string.h>

static const int8_t DX[4] = { 0, 1, 0, -1 };
static const int8_t DY[4] = { -1, 0, 1, 0 };

#define PI_4 0.78539816f

/* --------------------------------------------------------------------------
 * Pieces
 * -------------------------------------------------------------------------- */

uint8_t cn_open(uint8_t kind, uint8_t rot)
{
    switch (kind) {
    case CN_STRAIGHT: return (rot & 1) ? (1 << CN_E) | (1 << CN_W) : (1 << CN_N) | (1 << CN_S);
    case CN_BEND:     return (uint8_t)((1 << (rot & 3)) | (1 << ((rot + 1) & 3)));
    case CN_CROSS:    return 0x0F;
    case CN_SOURCE:
    case CN_DRAIN:    return (uint8_t)(1 << (rot & 3));
    default:          return 0;
    }
}

int cn_type_of(uint8_t kind, uint8_t rot)
{
    switch (kind) {
    case CN_STRAIGHT: return (rot & 1) ? CN_T_SH : CN_T_SV;
    case CN_BEND:     return CN_T_NE + (rot & 3);
    case CN_CROSS:    return CN_T_X;
    default:          return -1;
    }
}

void cn_type_piece(int type, uint8_t *kind, uint8_t *rot)
{
    if (type == CN_T_SV || type == CN_T_SH) {
        *kind = CN_STRAIGHT;
        *rot = (uint8_t)(type == CN_T_SH);
    } else if (type == CN_T_X) {
        *kind = CN_CROSS;
        *rot = 0;
    } else {
        *kind = CN_BEND;
        *rot = (uint8_t)(type - CN_T_NE);
    }
}

/* the bend's turn that joins two sides */
static uint8_t bend_rot(int a, int b)
{
    uint8_t m = (uint8_t)((1 << a) | (1 << b));
    for (uint8_t r = 0; r < 4; r++) {
        if (cn_open(CN_BEND, r) == m) return r;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * The generator
 * -------------------------------------------------------------------------- */

static uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static int rndn(uint32_t *s, int n)
{
    return n > 0 ? (int)(rnd(s) % (uint32_t)n) : 0;
}

/* Size of each level, upright. Bigger as the world goes on. */
static const uint8_t COLS[CN_LEVELS] = { 5, 5, 6, 6, 6, 7, 7, 7, 7, 7 };
static const uint8_t ROWS[CN_LEVELS] = { 6, 7, 7, 8, 8, 8, 9, 9, 9, 9 };

typedef struct {
    int8_t  c, r;
    uint8_t in;         /* side it was entered by, 0xFF at the source */
    uint8_t order[4];
    uint8_t k;          /* how many of 'order' were tried */
    uint8_t cross;      /* this visit crosses an earlier straight */
} frame_t;

typedef struct {
    frame_t f[CN_MAX_COLS * CN_MAX_ROWS * 2];
    int     n;
    uint8_t vis[CN_MAX_ROWS][CN_MAX_COLS];
    int16_t first[CN_MAX_ROWS][CN_MAX_COLS];    /* frame of the first visit */
} walk_t;

static void order_dirs(frame_t *f, uint32_t *rs, int straight_bias)
{
    uint8_t d[4] = { 0, 1, 2, 3 };
    for (int i = 3; i > 0; i--) {
        int j = rndn(rs, i + 1);
        uint8_t t = d[i];
        d[i] = d[j];
        d[j] = t;
    }
    /* keep going the same way now and then: long straights look like a plant */
    if (f->in != 0xFF && rndn(rs, 100) < straight_bias) {
        uint8_t ahead = (uint8_t)CN_OPP(f->in);
        for (int i = 0; i < 4; i++) {
            if (d[i] == ahead) {
                d[i] = d[0];
                d[0] = ahead;
                break;
            }
        }
    }
    memcpy(f->order, d, 4);
    f->k = 0;
}

/* The direction from frame i to frame i + 1. */
static int step_dir(const walk_t *w, int i)
{
    int dc = w->f[i + 1].c - w->f[i].c, dr = w->f[i + 1].r - w->f[i].r;
    for (int d = 0; d < 4; d++) {
        if (DX[d] == dc && DY[d] == dr) return d;
    }
    return 0;
}

/* A random self-avoiding walk from the source, by depth-first search with a
 * budget. It may go straight across a straight it laid earlier (that cell
 * becomes a cross). It stops on a cell that becomes the drain once it is
 * long enough. */
static bool walk(walk_t *w, int cols, int rows, int sc, int sr, int lmin, int lmax,
                 int cross_pct, uint32_t *rs)
{
    memset(w->vis, 0, sizeof w->vis);
    w->n = 0;
    frame_t *f = &w->f[w->n++];
    f->c = (int8_t)sc;
    f->r = (int8_t)sr;
    f->in = 0xFF;
    f->cross = 0;
    order_dirs(f, rs, 0);
    w->vis[sr][sc] = 1;
    w->first[sr][sc] = 0;
    bool fresh = true;
    int budget = 30000;

    while (w->n > 0) {
        if (--budget <= 0) return false;
        f = &w->f[w->n - 1];
        int depth = w->n;
        if (fresh && w->n > 1 && !f->cross && depth >= lmin &&
            (depth >= lmax || rndn(rs, 100) < 30)) {
            /* this cell is the drain; not next to the source */
            int dc = f->c - w->f[0].c, dr = f->r - w->f[0].r;
            if (dc * dc + dr * dr > 2) return true;
        }
        fresh = false;
        if (f->k >= 4 || depth >= lmax + 2) {
            /* dead end: back out of this cell */
            w->vis[f->r][f->c]--;
            w->n--;
            continue;
        }
        int d = f->order[f->k++];
        if (f->in != 0xFF && d == f->in) continue;
        if (f->cross && d != CN_OPP(f->in)) continue;   /* a cross goes straight on */
        int nc = f->c + DX[d], nr = f->r + DY[d];
        if (nc < 0 || nr < 0 || nc >= cols || nr >= rows) continue;
        if (w->vis[nr][nc] == 0) {
            frame_t *g = &w->f[w->n++];
            g->c = (int8_t)nc;
            g->r = (int8_t)nr;
            g->in = (uint8_t)CN_OPP(d);
            g->cross = 0;
            order_dirs(g, rs, 45);
            w->vis[nr][nc] = 1;
            w->first[nr][nc] = (int16_t)(w->n - 1);
            fresh = true;
        } else if (w->vis[nr][nc] == 1 && cross_pct > 0 && rndn(rs, 100) < cross_pct) {
            /* across an earlier straight, at right angles to it */
            int j = w->first[nr][nc];
            if (j <= 0 || j >= w->n - 1) continue;
            int jin = w->f[j].in, jout = step_dir(w, j);
            if (jin != CN_OPP(jout) || (jout & 1) == (d & 1)) continue;
            int bc = nc + DX[d], br = nr + DY[d];
            if (bc < 0 || br < 0 || bc >= cols || br >= rows || w->vis[br][bc]) continue;
            frame_t *g = &w->f[w->n++];
            g->c = (int8_t)nc;
            g->r = (int8_t)nr;
            g->in = (uint8_t)CN_OPP(d);
            g->cross = 1;
            order_dirs(g, rs, 0);
            w->vis[nr][nc] = 2;
            fresh = false;  /* a cross is never the drain */
        }
    }
    return false;
}

static void random_pipe(cn_cell_t *c, uint32_t *rs, int cross_pct)
{
    int p = rndn(rs, 100);
    c->kind = p < cross_pct ? CN_CROSS : p < 45 ? CN_STRAIGHT : CN_BEND;
    c->rot = (uint8_t)(c->kind == CN_STRAIGHT ? rndn(rs, 2) : c->kind == CN_BEND ? rndn(rs, 4) : 0);
}

void cn_level_make(cn_level_t *lv, int mode, int diff, int world, int level)
{
    memset(lv, 0, sizeof *lv);
    if (level < 0) level = 0;
    if (level >= CN_LEVELS) level = CN_LEVELS - 1;
    lv->mode = (uint8_t)mode;
    lv->diff = (uint8_t)diff;
    lv->world = (uint8_t)world;
    lv->level = (uint8_t)level;
    int cols = COLS[level], rows = ROWS[level];
    lv->cols = (uint8_t)cols;
    lv->rows = (uint8_t)rows;

    /* The path depends on mode, world and level only: the three
     * difficulties play the same plant, with more or less help. */
    uint32_t rs = 0x9E3779B9u ^ (uint32_t)(mode * 7919 + world * 104729 + level * 1299709 + 17);
    for (int i = 0; i < 8; i++) rnd(&rs);

    int area = cols * rows;
    int lmin = area * (30 + level * 2 + world) / 100 + 2;
    int lmax = area * 62 / 100 + 2;
    int cross_pct = level < 2 ? 0 : 25 + world * 5;

    static walk_t w;
    bool ok = false;
    for (int attempt = 0; attempt < 40 && !ok; attempt++) {
        int sc, sr;
        if (rndn(&rs, 100) < 60) {
            /* on the edge, like an inlet */
            int e = rndn(&rs, 2 * (cols + rows) - 4);
            if (e < cols) { sc = e; sr = 0; }
            else if (e < 2 * cols) { sc = e - cols; sr = rows - 1; }
            else { sc = (e & 1) ? cols - 1 : 0; sr = 1 + rndn(&rs, rows - 2); }
        } else {
            sc = rndn(&rs, cols);
            sr = rndn(&rs, rows);
        }
        ok = walk(&w, cols, rows, sc, sr, lmin, lmax, cross_pct, &rs);
        if (!ok && attempt > 20) lmin = lmin * 3 / 4;
    }
    if (!ok) {
        /* never seen; a plain snake so there is always something to play */
        w.n = 0;
        for (int r = 0; r < rows; r++) {
            for (int i = 0; i < cols; i++) {
                int c = (r & 1) ? cols - 1 - i : i;
                w.f[w.n].c = (int8_t)c;
                w.f[w.n].r = (int8_t)r;
                w.f[w.n].cross = 0;
                w.n++;
            }
        }
        w.n = cols * 2;
    }

    /* the solution as pieces */
    for (int i = 0; i < w.n; i++) {
        frame_t *f = &w.f[i];
        cn_cell_t *cell = &lv->cell[f->r][f->c];
        cell->flags = CN_F_PATH;
        if (i == 0) {
            cell->kind = CN_SOURCE;
            cell->rot = (uint8_t)step_dir(&w, 0);
            lv->src_c = f->c;
            lv->src_r = f->r;
            continue;
        }
        int in = CN_OPP(step_dir(&w, i - 1));
        if (i == w.n - 1) {
            cell->kind = CN_DRAIN;
            cell->rot = (uint8_t)in;
            lv->drn_c = f->c;
            lv->drn_r = f->r;
            continue;
        }
        if (f->cross) {
            cell->kind = CN_CROSS;
            cell->rot = 0;
            continue;
        }
        int out = step_dir(&w, i);
        if (out == CN_OPP(in)) {
            cell->kind = CN_STRAIGHT;
            cell->rot = (uint8_t)(in & 1);
        } else {
            cell->kind = CN_BEND;
            cell->rot = bend_rot(in, out);
        }
    }
    lv->path_len = (int16_t)(w.n - 2);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            lv->cell[r][c].sol_kind = lv->cell[r][c].kind;
            lv->cell[r][c].sol_rot = lv->cell[r][c].rot;
        }
    }

    /* what the difficulty changes from here on */
    uint32_t ds = rs ^ (uint32_t)(diff * 0x5bd1e995u + 1);
    for (int i = 0; i < 4; i++) rnd(&ds);

    int work = 0;
    if (mode == CN_MODE_ROTATE) {
        int lock_pct = diff == CN_EASY ? 25 : diff == CN_NORMAL ? 10 : 0;
        int block_pct = 5 + world + diff * 2;
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                cn_cell_t *cell = &lv->cell[r][c];
                if (cell->flags & CN_F_PATH) {
                    if (cell->kind != CN_STRAIGHT && cell->kind != CN_BEND) {
                        cell->flags |= CN_F_FIXED;
                        continue;
                    }
                    uint8_t sol = cell->rot;
                    int turns = cell->kind == CN_STRAIGHT ? 2 : 4;
                    if (rndn(&ds, 100) < lock_pct) {
                        cell->flags |= CN_F_FIXED;
                        continue;
                    }
                    uint8_t rot = (uint8_t)rndn(&ds, turns);
                    if (rot == sol && rndn(&ds, 4) != 0) rot = (uint8_t)((sol + 1 + rndn(&ds, turns - 1)) % turns);
                    cell->rot = rot;
                    lv->par_moves += (int16_t)((sol - rot + turns) % turns);
                } else if (rndn(&ds, 100) < block_pct) {
                    cell->kind = CN_BLOCK;
                    cell->flags = CN_F_FIXED;
                } else {
                    random_pipe(cell, &ds, 8 + world * 2);
                }
            }
        }
        lv->par_time = 8.0f + 1.4f * (float)lv->par_moves;
    } else {
        int gap_pct = diff == CN_EASY ? 35 : diff == CN_NORMAL ? 50 : 62;
        /* the solution's pipes, shuffled: the first ones become gaps */
        int idx[CN_MAX_COLS * CN_MAX_ROWS], n = 0;
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                const cn_cell_t *cell = &lv->cell[r][c];
                if ((cell->flags & CN_F_PATH) && cn_type_of(cell->kind, cell->rot) >= 0) {
                    idx[n++] = r * CN_MAX_COLS + c;
                }
            }
        }
        for (int i = n - 1; i > 0; i--) {
            int j = rndn(&ds, i + 1), t = idx[i];
            idx[i] = idx[j];
            idx[j] = t;
        }
        int gaps = (n * gap_pct + 99) / 100;
        if (gaps < 2) gaps = n < 2 ? n : 2;
        for (int i = 0; i < n; i++) {
            cn_cell_t *cell = &lv->cell[idx[i] / CN_MAX_COLS][idx[i] % CN_MAX_COLS];
            if (i < gaps) {
                lv->tray[cn_type_of(cell->kind, cell->rot)]++;
                cell->kind = CN_EMPTY;
                cell->rot = 0;
            } else {
                cell->flags |= CN_F_FIXED;
            }
        }
        lv->gaps = (int16_t)gaps;
        int block_pct = 10 + world + diff * 2, decoy_pct = 12 + diff * 3;
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                cn_cell_t *cell = &lv->cell[r][c];
                if (cell->flags & CN_F_PATH) {
                    if (cell->kind == CN_SOURCE || cell->kind == CN_DRAIN) cell->flags |= CN_F_FIXED;
                    continue;
                }
                int p = rndn(&ds, 100);
                if (p < block_pct) {
                    cell->kind = CN_BLOCK;
                    cell->flags = CN_F_FIXED;
                } else if (p < block_pct + decoy_pct) {
                    random_pipe(cell, &ds, 0);
                    cell->flags = CN_F_FIXED;
                }
            }
        }
        int decoys = (diff + 1) + level / 4;
        for (int i = 0; i < decoys; i++) lv->tray[rndn(&ds, CN_TYPES - 1)]++;
        work = gaps * 3;
        lv->par_time = 8.0f + 3.0f * (float)gaps;
    }
    if (mode == CN_MODE_ROTATE) work = lv->path_len;   /* time to look, not only to turn */
    lv->countdown = diff == CN_NORMAL ? 22.0f + 1.6f * (float)work
                  : diff == CN_HARD   ? 12.0f + 1.0f * (float)work : 0.0f;
}

/* --------------------------------------------------------------------------
 * The game
 * -------------------------------------------------------------------------- */

void cn_game_start(cn_game_t *g, int mode, int diff, int world, int level)
{
    memset(g, 0, sizeof *g);
    cn_level_make(&g->lv, mode, diff, world, level);
    g->phase = CN_PH_EDIT;
    g->countdown = g->lv.countdown;
    for (int r = 0; r < CN_MAX_ROWS; r++) {
        for (int c = 0; c < CN_MAX_COLS; c++) {
            g->segi[r][c][0] = g->segi[r][c][1] = -1;
        }
    }
    g->leak_c = g->leak_r = -1;
}

float cn_game_base_speed(const cn_game_t *g)
{
    static const float SPEED[CN_DIFFS] = { 0.55f, 0.70f, 0.90f };
    return SPEED[g->lv.diff % CN_DIFFS] * cn_world(g->lv.world)->speed;
}

bool cn_game_editable(const cn_game_t *g, int c, int r)
{
    if (g->phase > CN_PH_FLOW) return false;
    if (c < 0 || r < 0 || c >= g->lv.cols || r >= g->lv.rows) return false;
    const cn_cell_t *cell = &g->lv.cell[r][c];
    if (cell->flags & CN_F_FIXED) return false;
    if (cn_game_wet(g, c, r)) return false;
    if (g->lv.mode == CN_MODE_ROTATE) return cell->kind == CN_STRAIGHT || cell->kind == CN_BEND;
    return cell->kind == CN_EMPTY || (cell->flags & CN_F_PLACED);
}

bool cn_game_rotate(cn_game_t *g, int c, int r)
{
    if (g->lv.mode != CN_MODE_ROTATE || !cn_game_editable(g, c, r)) return false;
    cn_cell_t *cell = &g->lv.cell[r][c];
    cell->rot = (uint8_t)(cell->kind == CN_STRAIGHT ? (cell->rot + 1) & 1 : (cell->rot + 1) & 3);
    g->moves++;
    return true;
}

bool cn_game_place(cn_game_t *g, int c, int r, int type)
{
    if (g->lv.mode != CN_MODE_FILL || type < 0 || type >= CN_TYPES) return false;
    if (!cn_game_editable(g, c, r) || g->lv.cell[r][c].kind != CN_EMPTY) return false;
    if (!g->lv.tray[type]) return false;
    cn_cell_t *cell = &g->lv.cell[r][c];
    cn_type_piece(type, &cell->kind, &cell->rot);
    cell->flags = (uint8_t)((cell->flags & CN_F_PATH) | CN_F_PLACED);
    g->lv.tray[type]--;
    g->moves++;
    return true;
}

int cn_game_take(cn_game_t *g, int c, int r)
{
    if (g->lv.mode != CN_MODE_FILL || !cn_game_editable(g, c, r)) return -1;
    cn_cell_t *cell = &g->lv.cell[r][c];
    if (!(cell->flags & CN_F_PLACED)) return -1;
    int type = cn_type_of(cell->kind, cell->rot);
    if (type < 0) return -1;
    g->lv.tray[type]++;
    cell->kind = CN_EMPTY;
    cell->rot = 0;
    cell->flags &= (uint8_t)~CN_F_PLACED;
    return type;
}

bool cn_game_open(cn_game_t *g)
{
    if (g->phase != CN_PH_EDIT) return false;
    g->phase = CN_PH_OPENING;
    g->valve = 0;
    return true;
}

void cn_game_set_fast(cn_game_t *g, bool on)
{
    g->fast = on;
}

void cn_game_solve(cn_game_t *g)
{
    for (int r = 0; r < g->lv.rows; r++) {
        for (int c = 0; c < g->lv.cols; c++) {
            cn_cell_t *cell = &g->lv.cell[r][c];
            if (!(cell->flags & CN_F_PATH) || cn_game_wet(g, c, r)) continue;
            if (g->lv.mode == CN_MODE_ROTATE) {
                cell->rot = cell->sol_rot;
            } else if (cell->kind == CN_EMPTY || (cell->flags & CN_F_PLACED)) {
                if (cell->flags & CN_F_PLACED) cn_game_take(g, c, r);
                int t = cn_type_of(cell->sol_kind, cell->sol_rot);
                if (t >= 0 && g->lv.tray[t]) cn_game_place(g, c, r, t);
            }
        }
    }
}

static int push_seg(cn_game_t *g, int c, int r, int in, int out, int ch, float len)
{
    if (g->nseg >= CN_MAX_SEGS) return -1;
    cn_seg_t *s = &g->seg[g->nseg];
    s->c = (int8_t)c;
    s->r = (int8_t)r;
    s->in = (uint8_t)in;
    s->out = (uint8_t)out;
    s->ch = (uint8_t)ch;
    s->len = len;
    s->start = g->nseg ? g->seg[g->nseg - 1].start + g->seg[g->nseg - 1].len : 0.0f;
    g->segi[r][c][ch] = (int16_t)g->nseg;
    return g->nseg++;
}

/* The next pipe after the last segment: 0 a pipe, 1 the drain, -1 a leak. */
static int advance(cn_game_t *g, uint32_t *ev)
{
    const cn_seg_t *last = &g->seg[g->nseg - 1];
    int d = last->out;
    int c = last->c + DX[d], r = last->r + DY[d];
    if (c < 0 || r < 0 || c >= g->lv.cols || r >= g->lv.rows) return -1;
    const cn_cell_t *cell = &g->lv.cell[r][c];
    int in = CN_OPP(d);
    if (!(cn_open(cell->kind, cell->rot) & (1 << in))) return -1;
    switch (cell->kind) {
    case CN_DRAIN:
        return push_seg(g, c, r, in, 0xFF, 0, 0.5f) >= 0 ? 1 : -1;
    case CN_CROSS: {
        int ch = in & 1;
        if (g->segi[r][c][ch] >= 0) return -1;
        if (g->segi[r][c][ch ^ 1] >= 0) {
            g->cross2++;
            g->score += 200;
            *ev |= CN_EV_CROSS2;
        }
        return push_seg(g, c, r, in, CN_OPP(in), ch, 1.0f) >= 0 ? 0 : -1;
    }
    case CN_STRAIGHT:
    case CN_BEND: {
        if (cn_game_wet(g, c, r)) return -1;
        uint8_t m = (uint8_t)(cn_open(cell->kind, cell->rot) & ~(1 << in));
        int out = 0;
        while (out < 4 && !(m & (1 << out))) out++;
        return push_seg(g, c, r, in, out, 0, cell->kind == CN_STRAIGHT ? 1.0f : PI_4) >= 0 ? 0 : -1;
    }
    default:
        return -1;
    }
}

static void finish(cn_game_t *g)
{
    const cn_level_t *lv = &g->lv;
    int stars = 1;
    if (lv->mode == CN_MODE_ROTATE) {
        if (g->moves <= lv->par_moves + 2 + lv->par_moves / 4) stars++;
    } else {
        if (g->moves <= lv->gaps + 1) stars++;
    }
    if (lv->diff == CN_EASY) {
        if (g->t_edit <= lv->par_time) stars++;
        float left = lv->par_time - g->t_edit;
        if (left > 0) g->score += (int)(left * 10.0f);
    } else if (!g->auto_opened) {
        stars++;
        g->score += (int)(g->countdown * 15.0f);
    }
    g->score += stars * 100;
    g->stars = stars;
}

uint32_t cn_game_step(cn_game_t *g, float dt)
{
    uint32_t ev = 0;
    if (dt <= 0) return 0;
    if (dt > 0.1f) dt = 0.1f;   /* a hiccup must not jump the front across a pipe */
    switch (g->phase) {
    case CN_PH_EDIT:
        g->t_edit += dt;
        if (g->lv.diff != CN_EASY) {
            int before = (int)(g->countdown + 0.999f);
            g->countdown -= dt;
            if (g->countdown <= 0) {
                g->countdown = 0;
                g->auto_opened = true;
                cn_game_open(g);
                ev |= CN_EV_OPEN;
            } else if ((int)(g->countdown + 0.999f) != before) {
                ev |= CN_EV_SECOND;
            }
        }
        break;
    case CN_PH_OPENING:
        g->valve += dt / 0.8f;
        if (g->valve >= 1.0f) {
            g->valve = 1.0f;
            g->phase = CN_PH_FLOW;
            const cn_cell_t *s = &g->lv.cell[g->lv.src_r][g->lv.src_c];
            push_seg(g, g->lv.src_c, g->lv.src_r, 0xFF, s->rot, 0, 0.5f);
            g->front = 0;
            ev |= CN_EV_FLOW;
        }
        break;
    case CN_PH_FLOW: {
        g->speed = cn_game_base_speed(g) * (g->fast ? 6.0f : 1.0f);
        g->front += g->speed * dt;
        g->flow_t += dt;
        for (;;) {
            const cn_seg_t *last = &g->seg[g->nseg - 1];
            float end = last->start + last->len;
            if (g->front < end) break;
            if (last->out == 0xFF) {
                /* through the drain's mouth */
                g->front = end;
                g->phase = CN_PH_DRAIN;
                g->end_t = 0;
                ev |= CN_EV_DRAIN;
                break;
            }
            if (last->in != 0xFF) {
                g->filled++;
                g->score += 50 + 25 * g->lv.diff;
                ev |= CN_EV_PIPE;
            }
            int k = advance(g, &ev);
            if (k < 0) {
                g->front = end;
                g->leak_c = last->c;
                g->leak_r = last->r;
                g->leak_dir = last->out;
                g->phase = CN_PH_SPILL;
                g->end_t = 0;
                ev |= CN_EV_LEAK;
                break;
            }
        }
        break;
    }
    case CN_PH_DRAIN:
        g->end_t += dt;
        g->flow_t += dt;
        if (g->end_t >= 1.6f) {
            g->phase = CN_PH_WON;
            finish(g);
            ev |= CN_EV_WON;
        }
        break;
    case CN_PH_SPILL:
        g->end_t += dt;
        g->flow_t += dt;
        if (g->end_t >= 3.2f) {
            g->phase = CN_PH_LOST;
            ev |= CN_EV_LOST;
        }
        break;
    default:
        break;
    }
    return ev;
}
