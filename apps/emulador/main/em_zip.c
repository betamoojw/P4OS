/*
 * P4OS - Emulador: games in .zip files.
 *
 * Most collections come zipped. A .zip in a console's folder is a game of
 * that console (the Neo Geo's core reads its zips itself, emu_core_t.exts
 * says so); before the core sees it, the file inside whose extension the
 * console takes (or the biggest one) is inflated once into the console's
 * hidden .cache/ folder, and the core is handed that. The next time the
 * cached file is used as it is.
 *
 * Only what a ROM zip holds: stored or deflated entries, read through the
 * central directory. Inflating is miniz's tinfl (MIT, third/tinfl.c, the
 * same copy as the VNC viewer's), its names prefixed so the two do not
 * meet in the simulator, where every app is one program.
 */
#include "emu.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#define tinfl_decompress emu_tinfl_decompress
#include "../third/tinfl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define ZIP_MAX_OUT     (64u * 1024 * 1024)     /* no ROM is bigger */
#define ZIP_IN_BUF      (32 * 1024)

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

bool emu_is_zip(const char *path)
{
    size_t n = strlen(path);
    return n > 4 && !strcasecmp(path + n - 4, ".zip");
}

typedef struct {
    char     name[EMU_NAME_MAX];
    uint32_t method, csize, usize, local;
} zentry_t;

/* The entry to play: the first whose extension the console takes, or the
 * biggest when none does (a zip holding "game.rom"). */
static bool zip_pick(FILE *f, const emu_core_t *core, zentry_t *out)
{
    uint8_t tail[1024];
    if (fseek(f, 0, SEEK_END) != 0) return false;
    long size = ftell(f);
    if (size < 22) return false;
    long from = size > (long)sizeof tail ? size - (long)sizeof tail : 0;
    if (fseek(f, from, SEEK_SET) != 0) return false;
    size_t got = fread(tail, 1, sizeof tail, f);
    long eocd = -1;
    for (long i = (long)got - 22; i >= 0; i--) {
        if (rd32(tail + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) return false;
    uint32_t count = rd16(tail + eocd + 10);
    uint32_t cd = rd32(tail + eocd + 16);
    if (fseek(f, (long)cd, SEEK_SET) != 0) return false;

    bool found = false, matched = false;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t h[46];
        if (fread(h, 1, 46, f) != 46 || rd32(h) != 0x02014b50u) return found;
        uint32_t nl = rd16(h + 28), xl = rd16(h + 30), cl = rd16(h + 32);
        zentry_t e = { .method = rd16(h + 10), .csize = rd32(h + 20), .usize = rd32(h + 24), .local = rd32(h + 42) };
        char name[EMU_NAME_MAX * 2];
        uint32_t keep = nl < sizeof name - 1 ? nl : sizeof name - 1;
        if (fread(name, 1, keep, f) != keep) return found;
        name[keep] = 0;
        if (fseek(f, (long)(nl - keep + xl + cl), SEEK_CUR) != 0) return found;
        size_t len = strlen(name);
        if (!len || name[len - 1] == '/' || (e.method != 0 && e.method != 8) || e.usize > ZIP_MAX_OUT) continue;
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;
        if (base[0] == '.') continue;                   /* __MACOSX/._x */
        snprintf(e.name, sizeof e.name, "%.120s", base);
        bool m = emu_ext_match(core, base);
        if (!found || (m && !matched) || (m == matched && e.usize > out->usize)) {
            *out = e;
            found = true;
            matched = m;
        }
    }
    return found;
}

static bool inflate_to(FILE *in, const zentry_t *e, FILE *out)
{
    uint8_t *ibuf = emu_psram(ZIP_IN_BUF);
    uint8_t *dict = emu_psram(TINFL_LZ_DICT_SIZE);
    tinfl_decompressor *d = emu_psram(sizeof *d);
    bool ok = false;
    if (!ibuf || !dict || !d) goto done;
    if (e->method == 0) {
        uint32_t left = e->usize;
        while (left) {
            size_t n = left < ZIP_IN_BUF ? left : ZIP_IN_BUF;
            if (fread(ibuf, 1, n, in) != n || fwrite(ibuf, 1, n, out) != n) goto done;
            left -= (uint32_t)n;
        }
        ok = true;
        goto done;
    }
    tinfl_init(d);
    uint32_t cleft = e->csize, written = 0;
    size_t ihave = 0, ipos = 0, opos = 0;
    for (;;) {
        if (ipos == ihave && cleft) {
            ihave = cleft < ZIP_IN_BUF ? cleft : ZIP_IN_BUF;
            if (fread(ibuf, 1, ihave, in) != ihave) goto done;
            cleft -= (uint32_t)ihave;
            ipos = 0;
        }
        size_t in_n = ihave - ipos, out_n = TINFL_LZ_DICT_SIZE - opos;
        tinfl_status st = tinfl_decompress(d, ibuf + ipos, &in_n, dict, dict + opos, &out_n,
                                           cleft ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        ipos += in_n;
        if (out_n && fwrite(dict + opos, 1, out_n, out) != out_n) goto done;
        written += (uint32_t)out_n;
        opos = (opos + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        if (st == TINFL_STATUS_DONE) {
            ok = written == e->usize;
            break;
        }
        if (st < 0 || written > ZIP_MAX_OUT) break;
    }
done:
    free(ibuf);
    free(dict);
    free(d);
    return ok;
}

bool emu_unzip_rom(const emu_game_t *g, const char *zip, char *out, size_t n, char *err, size_t errlen)
{
    FILE *f = fopen(zip, "rb");
    if (!f) {
        snprintf(err, errlen, "%s", _("No se pudo abrir el zip del juego"));
        return false;
    }
    zentry_t e;
    if (!zip_pick(f, g->core, &e)) {
        fclose(f);
        snprintf(err, errlen, "%s", _("El zip no tiene un juego de esta consola"));
        return false;
    }

    /* the cache: <console>/.cache/<the zip's name>__<the file's name> */
    char dir[EMU_PATH_MAX], base[EMU_PATH_MAX];
    emu_console_dir(g->core, dir, sizeof dir);
    size_t dl = strlen(dir);
    snprintf(dir + dl, sizeof dir - dl, "/.cache");
    if (mkdir(dir, 0775) != 0 && errno != EEXIST) aos_hal_log(EMU_TAG, "cannot make %s", dir);
    snprintf(base, sizeof base, "%.150s", g->rel);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    for (char *p = base; *p; p++) if (*p == '/') *p = '_';
    snprintf(out, n, "%.140s/%.100s__%.70s", dir, base, e.name);

    struct stat st;
    if (stat(out, &st) == 0 && (uint32_t)st.st_size == e.usize) {
        fclose(f);
        return true;                            /* inflated before */
    }

    /* past the local header to the data */
    uint8_t h[30];
    if (fseek(f, (long)e.local, SEEK_SET) != 0 || fread(h, 1, 30, f) != 30 || rd32(h) != 0x04034b50u ||
        fseek(f, (long)(rd16(h + 26) + rd16(h + 28)), SEEK_CUR) != 0) {
        fclose(f);
        snprintf(err, errlen, "%s", _("El zip del juego está dañado"));
        return false;
    }
    char tmp[EMU_PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    FILE *o = fopen(tmp, "wb");
    if (!o) {
        fclose(f);
        snprintf(err, errlen, "%s", _("No hay lugar en la tarjeta para descomprimir el juego"));
        return false;
    }
    uint64_t t0 = aos_hal_uptime_us();
    bool ok = inflate_to(f, &e, o);
    fclose(o);
    fclose(f);
    if (!ok) {
        remove(tmp);
        snprintf(err, errlen, "%s", _("El zip del juego está dañado"));
        return false;
    }
    remove(out);
    if (rename(tmp, out) != 0) {
        remove(tmp);
        snprintf(err, errlen, "%s", _("No hay lugar en la tarjeta para descomprimir el juego"));
        return false;
    }
    aos_hal_log(EMU_TAG, "%s: %s inflated, %u KB in %u ms", g->rel, e.name, (unsigned)(e.usize / 1024),
                (unsigned)((aos_hal_uptime_us() - t0) / 1000));
    return true;
}
