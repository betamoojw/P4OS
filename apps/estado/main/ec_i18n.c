/* Private catalogue for background notifications and live data. The UI
 * thread loads it before readers start, independently of the foreground app. */
#include "ec.h"

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

/* PSRAM on the board (free() takes it back); the simulator has one heap */
static void *ps_alloc(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) return p;
#endif
    return malloc(n);
}

static struct {
    char  code[16];
    char *blob;
    const char **es, **tr;
    int   n;
} I;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void unescape(char *s)
{
    char *w = s;
    for (const char *r = s; *r;) {
        if (*r != '\\' || !r[1]) { *w++ = *r++; continue; }
        switch (r[1]) {
        case 'n': *w++ = '\n'; r += 2; break;
        case 't': *w++ = '\t'; r += 2; break;
        case 'r': *w++ = '\r'; r += 2; break;
        case 'x': {
            int hi = hexval(r[2]), lo = hi >= 0 ? hexval(r[3]) : -1;
            if (lo >= 0) { *w++ = (char)((hi << 4) | lo); r += 4; }
            else { *w++ = *r++; }
            break;
        }
        default: *w++ = r[1]; r += 2; break;    /* \\ \" \' */
        }
    }
    *w = 0;
}

void ec_i18n_free(void)
{
    free(I.blob);
    free(I.es);
    free(I.tr);
    memset(&I, 0, sizeof I);
}

void ec_i18n_load(void)
{
    const char *code = aos_i18n_current();
    if (!code) code = "es";
    if (I.code[0] && !strcmp(code, I.code)) return;
    ec_i18n_free();
    ec_copy(I.code, sizeof I.code, code);
    if (!strcmp(code, "es")) return;
    I.blob = aos_i18n_app_catalog_dup("aos.estado");
    if (!I.blob) return;
    int lines = 1;
    for (const char *p = I.blob; *p; p++) lines += *p == '\n';
    I.es = ps_alloc(sizeof *I.es * (size_t)lines);
    I.tr = ps_alloc(sizeof *I.tr * (size_t)lines);
    if (!I.es || !I.tr) { ec_i18n_free(); ec_copy(I.code, sizeof I.code, code); return; }
    for (char *line = I.blob; line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[ll - 1] = 0;
        char *tab = *line != '#' ? strchr(line, '\t') : NULL;
        if (tab) {
            *tab = 0;
            unescape(line);
            unescape(tab + 1);
            if (*line && tab[1]) {
                I.es[I.n] = line;
                I.tr[I.n] = tab + 1;
                I.n++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

const char *ec_tr(const char *es)
{
    for (int i = 0; i < I.n; i++)
        if (!strcmp(I.es[i], es)) return I.tr[i];
    return es;
}
