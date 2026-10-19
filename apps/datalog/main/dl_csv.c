/*
 * P4OS - Datalog: the CSV files, one per line, in <card>/datalog/<id>.csv.
 *
 *   fecha,hora,Horno (°C),nota
 *   2026-10-18,14:05:00,231.25,
 *   2026-10-18,14:05:10,,No contesta: MISO quedó en 1. ¿Alimentación, CS, cables?
 *   2026-10-18,14:05:20,251.50,ALTA: más de 250
 *
 * Or with ';' and a decimal comma, for a spreadsheet in Spanish (the app's
 * setting). Each reading opens the file, appends and closes it: the FAT
 * only commits on close, and a logger has to survive a power cut. With no
 * clock yet (no network since boot) the date says so and the time is the
 * uptime, "+123"; the charts skip those lines.
 *
 * A chart never reads a whole big file: below 1 MB it does, and above that
 * it finds where the range starts by bisection (the lines are in time
 * order) and reads a few lines at evenly spread offsets from there to the
 * end. A year of readings a second charts as fast as an hour's.
 */
#include "dl.h"

#include <sys/stat.h>

#define WHOLE_MAX   (1024 * 1024)
#define SAMPLES     (DL_CHART_N * 4)

void dl_csv_dir(char *out, size_t n)
{
    const char *root = aos_hal_path_sd_root();
    if (!root) { out[0] = 0; return; }
    snprintf(out, n, "%.200s/datalog", root);
}

void dl_csv_path(const char *id, char *out, size_t n)
{
    char dir[220];
    dl_csv_dir(dir, sizeof dir);
    if (!dir[0]) { out[0] = 0; return; }
    snprintf(out, n, "%s/%.23s.csv", dir, id);
}

/* ---- the calendar, without the C library's time zones ---- */

int64_t dl_civil(int y, int mo, int d, int hh, int mi, int ss)
{
    /* days from 1970-01-01 (H. Hinnant's days_from_civil) */
    y -= mo <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return days * 86400 + hh * 3600 + mi * 60 + ss;
}

void dl_uncivil(int64_t t, int *y, int *mo, int *d, int *hh, int *mi, int *ss)
{
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int sod = (int)(t - days * 86400);
    *hh = sod / 3600;
    *mi = sod / 60 % 60;
    *ss = sod % 60;
    int z = (int)days + 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *mo = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*mo <= 2);
}

/* ---- writing ---- */

/* A field, between quotes when it carries the separator or a quote */
static void put_field(FILE *f, const char *s, char sep)
{
    if (!strchr(s, sep) && !strchr(s, '"') && !strchr(s, '\n')) { fputs(s, f); return; }
    char t[160];
    size_t o = 0;
    t[o++] = '"';
    for (; *s && o + 3 < sizeof t; s++) {
        if (*s == '\n') continue;
        if (*s == '"') t[o++] = '"';
        t[o++] = *s;
    }
    t[o++] = '"';
    t[o] = 0;
    fputs(t, f);
}

bool dl_csv_append(bool semicolon, const dl_ch_t *c, const char *date, const char *tim,
                   bool have, float v, const char *note)
{
    char dir[220], path[256];
    dl_csv_dir(dir, sizeof dir);
    if (!dir[0]) return false;
    mkdir(dir, 0755);
    dl_csv_path(c->id, path, sizeof path);
    struct stat st;
    bool fresh = stat(path, &st) != 0 || st.st_size == 0;
    FILE *f = fopen(path, "a");
    if (!f) return false;
    char sep = semicolon ? ';' : ',', s[2] = { sep, 0 };
    if (fresh) {
        char col[64];
        const char *u = dl_src_unit(c);
        if (u[0]) snprintf(col, sizeof col, "%.31s (%.11s)", c->name, u);
        else dl_scpy(col, sizeof col, c->name);
        fputs(_("fecha"), f);
        fputs(s, f);
        fputs(_("hora"), f);
        fputs(s, f);
        put_field(f, col, sep);
        fputs(s, f);
        fputs(_("nota"), f);
        fputs("\n", f);
    }
    char num[32] = "";
    if (have) {
        snprintf(num, sizeof num, "%.*f", dl_src_decimals(c), (double)v);
        if (semicolon) for (char *p = num; *p; p++) if (*p == '.') *p = ',';
    }
    fputs(date, f);
    fputs(s, f);
    fputs(tim, f);
    fputs(s, f);
    fputs(num, f);
    fputs(s, f);
    put_field(f, note ? note : "", sep);
    fputs("\n", f);
    return fclose(f) == 0;
}

long dl_csv_size(const char *id)
{
    char path[256];
    dl_csv_path(id, path, sizeof path);
    struct stat st;
    return path[0] && stat(path, &st) == 0 ? (long)st.st_size : -1;
}

bool dl_csv_clear(const char *id)
{
    char path[256];
    dl_csv_path(id, path, sizeof path);
    return path[0] && remove(path) == 0;
}

/* ---- reading ---- */

static int num2(const char *s) { return (s[0] - '0') * 10 + (s[1] - '0'); }
static bool digits(const char *s, int n) { for (int i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false; return true; }

/* A line's time and value: 2 both, 1 only the time (a failed reading),
 * 0 neither (the header, a line without a clock, half a line) */
static int parse(const char *s, char sep, int64_t *t, float *v)
{
    /* YYYY-MM-DD<sep>HH:MM:SS<sep> */
    if (!digits(s, 4) || s[4] != '-' || !digits(s + 5, 2) || s[7] != '-' || !digits(s + 8, 2) || s[10] != sep) return 0;
    const char *h = s + 11;
    if (!digits(h, 2) || h[2] != ':' || !digits(h + 3, 2) || h[5] != ':' || !digits(h + 6, 2) || h[8] != sep) return 0;
    int y = num2(s) * 100 + num2(s + 2);
    *t = dl_civil(y, num2(s + 5), num2(s + 8), num2(h), num2(h + 3), num2(h + 6));
    const char *p = h + 9;
    char num[32];
    size_t o = 0;
    for (; *p && *p != sep && *p != '\n' && *p != '\r' && o + 1 < sizeof num; p++) num[o++] = *p == ',' ? '.' : *p;
    num[o] = 0;
    if (!o) return 1;
    char *end;
    float x = strtof(num, &end);
    if (end == num) return 1;
    *v = x;
    return 2;
}

static char sep_of(FILE *f)
{
    char line[160];
    if (fseek(f, 0, SEEK_SET) || !fgets(line, sizeof line, f)) return ',';
    return strchr(line, ';') ? ';' : ',';
}

int dl_csv_tail(const char *id, float *vals, int max, float *last, char *when, size_t wn)
{
    char path[256];
    dl_csv_path(id, path, sizeof path);
    if (when && wn) when[0] = 0;
    FILE *f = path[0] ? fopen(path, "r") : NULL;
    if (!f) return 0;
    char sep = sep_of(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > 12 * 1024 ? size - 12 * 1024 : 0;
    fseek(f, from, SEEK_SET);
    char line[200];
    if (from) fgets(line, sizeof line, f);      /* half a line */
    int n = 0;
    int64_t tl = 0;
    while (fgets(line, sizeof line, f)) {
        int64_t t;
        float v;
        if (parse(line, sep, &t, &v) != 2) continue;
        if (n == max) { memmove(vals, vals + 1, sizeof *vals * (size_t)(max - 1)); n--; }
        vals[n++] = v;
        tl = t;
        if (last) *last = v;
    }
    fclose(f);
    if (n && when && wn) {
        int y, mo, d, hh, mi, ss;
        dl_uncivil(tl, &y, &mo, &d, &hh, &mi, &ss);
        snprintf(when, wn, "%02d/%02d %02d:%02d:%02d", d, mo, hh, mi, ss);
    }
    return n;
}

/* The time of the first whole line at or after 'off', -1 none */
static int64_t time_at(FILE *f, long off, char sep, long *line_off)
{
    char line[200];
    if (fseek(f, off, SEEK_SET)) return -1;
    if (off && !fgets(line, sizeof line, f)) return -1;
    for (int tries = 0; tries < 8; tries++) {
        long here = ftell(f);
        if (!fgets(line, sizeof line, f)) return -1;
        int64_t t;
        float v;
        if (parse(line, sep, &t, &v)) {
            if (line_off) *line_off = here;
            return t;
        }
    }
    return -1;
}

static float s_sum[DL_CHART_N];
static int s_cnt[DL_CHART_N];

static void add(dl_series_t *o, int64_t t, float v)
{
    if (t < o->t0 || t > o->t1) return;
    int64_t span = o->t1 - o->t0;
    int i = span > 0 ? (int)((t - o->t0) * DL_CHART_N / (span + 1)) : DL_CHART_N - 1;
    if (i < 0 || i >= DL_CHART_N) return;
    s_sum[i] += v;
    s_cnt[i]++;
    if (v < o->lo) o->lo = v;
    if (v > o->hi) o->hi = v;
    o->sum += v;
    o->cnt++;
    o->last = v;
}

bool dl_csv_series(const char *id, int64_t span_s, int64_t now, dl_series_t *o)
{
    memset(o, 0, sizeof *o);
    o->lo = INFINITY;
    o->hi = -INFINITY;
    o->last = NAN;
    o->n = DL_CHART_N;
    for (int i = 0; i < DL_CHART_N; i++) o->v[i] = NAN;
    char path[256];
    dl_csv_path(id, path, sizeof path);
    FILE *f = path[0] ? fopen(path, "r") : NULL;
    if (!f) return false;
    char sep = sep_of(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    /* the end: now, or the last line's time when there is no clock */
    int64_t t_last = -1;
    for (long back = 256; back <= 8192 && t_last < 0; back *= 2) {
        long from = size > back ? size - back : 0;
        char line[200];
        fseek(f, from, SEEK_SET);
        if (from) fgets(line, sizeof line, f);
        while (fgets(line, sizeof line, f)) {
            int64_t t;
            float v;
            if (parse(line, sep, &t, &v)) t_last = t;
        }
        if (!from) break;
    }
    if (t_last < 0) { fclose(f); return false; }
    o->t1 = now > 0 ? now : t_last;
    if (span_s > 0) o->t0 = o->t1 - span_s;
    else {
        o->t0 = time_at(f, 0, sep, NULL);
        if (o->t0 < 0 || o->t0 >= o->t1) o->t0 = o->t1 - 60;
    }
    memset(s_sum, 0, sizeof s_sum);
    memset(s_cnt, 0, sizeof s_cnt);
    char line[200];
    if (size <= WHOLE_MAX) {
        fseek(f, 0, SEEK_SET);
        while (fgets(line, sizeof line, f)) {
            int64_t t;
            float v;
            if (parse(line, sep, &t, &v) == 2) add(o, t, v);
        }
    } else {
        o->sampled = true;
        /* where the range starts */
        long lo = 0, hi = size;
        while (hi - lo > 4096) {
            long mid = lo + (hi - lo) / 2;
            int64_t t = time_at(f, mid, sep, NULL);
            if (t >= 0 && t < o->t0) lo = mid;
            else hi = mid;
        }
        long start = lo;
        for (int k = 0; k < SAMPLES; k++) {
            long off = start + (long)((int64_t)(size - start) * k / SAMPLES);
            if (fseek(f, off, SEEK_SET)) break;
            if (off && !fgets(line, sizeof line, f)) continue;
            for (int r = 0; r < 2 && fgets(line, sizeof line, f); r++) {
                int64_t t;
                float v;
                if (parse(line, sep, &t, &v) == 2) add(o, t, v);
            }
        }
    }
    fclose(f);
    for (int i = 0; i < DL_CHART_N; i++) if (s_cnt[i]) o->v[i] = s_sum[i] / (float)s_cnt[i];
    o->ok = true;
    return true;
}
