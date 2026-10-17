/*
 * P4OS - Emulador, Neo Geo core: reading MAME-style .zip sets (ng_zip.h).
 *
 * Written for this core in place of gngeo's unzip.c and stb_zlib.c, which
 * read the zip byte by byte with fgetc and kept one stream per open file.
 * Here every stream has its own FILE, a 16 KB input buffer and tinfl's
 * 32 KB window, about 60 KB in all, freed when the stream closes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ng_zip.h"
#include "tinfl.h"

#define ZS_INBUF 16384

struct ng_zstream {
    FILE               *f;
    const ng_zent_t    *e;
    uint32_t            in_left;    /* compressed bytes not read from the file yet */
    uint32_t            out_left;   /* bytes still to come out */
    uint32_t            crc;
    bool                failed;
    /* deflate */
    tinfl_decompressor  inf;
    uint8_t            *in;         /* ZS_INBUF */
    uint32_t            in_pos, in_len;
    uint8_t            *dict;       /* TINFL_LZ_DICT_SIZE, the window */
    uint32_t            dict_ofs;   /* where tinfl writes next */
    uint32_t            avail_ofs;  /* decoded bytes not handed out yet */
    uint32_t            avail;
    bool                done;
};

/* ---- CRC-32 --------------------------------------------------------------- */

static uint32_t s_crc_tab[256];

static void crc_init(void)
{
    if (s_crc_tab[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc_tab[i] = c;
    }
}

uint32_t ng_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
    crc_init();
    crc = ~crc;
    while (n--) crc = s_crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- the directory ---------------------------------------------------------- */

static uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

ng_zip_t *ng_zip_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    ng_zip_t *z = NULL;
    uint8_t *tail = NULL, *cd = NULL;

    if (fseek(f, 0, SEEK_END) != 0) goto fail;
    long size = ftell(f);
    if (size < 22) goto fail;
    /* The end-of-directory record is in the last 22 bytes plus a comment
     * of up to 64 KB. */
    long tail_len = size < 65557 ? size : 65557;
    tail = malloc(tail_len);
    if (!tail) goto fail;
    if (fseek(f, size - tail_len, SEEK_SET) != 0 || fread(tail, 1, tail_len, f) != (size_t)tail_len)
        goto fail;
    long eocd = -1;
    for (long i = tail_len - 22; i >= 0; i--) {
        if (tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 5 && tail[i + 3] == 6) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) goto fail;
    uint32_t count = le16(tail + eocd + 10);
    uint32_t cd_size = le32(tail + eocd + 12);
    uint32_t cd_ofs = le32(tail + eocd + 16);
    if (count == 0 || count > 4096 || cd_size > 4u * 1024 * 1024 || (long)cd_ofs + (long)cd_size > size)
        goto fail;

    cd = malloc(cd_size);
    z = calloc(1, sizeof(*z));
    if (!cd || !z) goto fail;
    z->ent = calloc(count, sizeof(ng_zent_t));
    z->path = malloc(strlen(path) + 1);
    if (!z->ent || !z->path) goto fail;
    strcpy(z->path, path);
    if (fseek(f, cd_ofs, SEEK_SET) != 0 || fread(cd, 1, cd_size, f) != cd_size) goto fail;

    uint32_t p = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > cd_size || le32(cd + p) != 0x02014b50) goto fail;
        uint32_t nlen = le16(cd + p + 28), xlen = le16(cd + p + 30), clen = le16(cd + p + 32);
        if (p + 46 + nlen > cd_size) goto fail;
        ng_zent_t *e = &z->ent[z->n];
        e->method = le16(cd + p + 10);
        e->crc = le32(cd + p + 16);
        e->csize = le32(cd + p + 20);
        e->usize = le32(cd + p + 24);
        e->lho = le32(cd + p + 42);
        /* The name without any folder in front (some sets are zipped with
         * one). */
        const char *nm = (const char *)cd + p + 46;
        uint32_t start = 0;
        for (uint32_t k = 0; k < nlen; k++)
            if (nm[k] == '/' || nm[k] == '\\') start = k + 1;
        uint32_t l = nlen - start;
        if (l >= sizeof(e->name)) l = sizeof(e->name) - 1;
        memcpy(e->name, nm + start, l);
        e->name[l] = 0;
        if (e->name[0] && (e->method == 0 || e->method == 8)) z->n++;
        p += 46 + nlen + xlen + clen;
    }
    free(tail);
    free(cd);
    fclose(f);
    return z;

fail:
    free(tail);
    free(cd);
    if (z) {
        free(z->ent);
        free(z->path);
        free(z);
    }
    fclose(f);
    return NULL;
}

void ng_zip_close(ng_zip_t *z)
{
    if (!z) return;
    free(z->ent);
    free(z->path);
    free(z);
}

static size_t base_len(const char *s)
{
    const char *dot = strrchr(s, '.');
    return dot ? (size_t)(dot - s) : strlen(s);
}

const ng_zent_t *ng_zip_find(const ng_zip_t *z, const char *name, uint32_t crc)
{
    if (!z) return NULL;
    if (crc) {
        for (int i = 0; i < z->n; i++)
            if (z->ent[i].crc == crc) return &z->ent[i];
    }
    for (int i = 0; i < z->n; i++)
        if (strcasecmp(z->ent[i].name, name) == 0) return &z->ent[i];
    size_t bl = base_len(name);
    for (int i = 0; i < z->n; i++)
        if (base_len(z->ent[i].name) == bl && strncasecmp(z->ent[i].name, name, bl) == 0)
            return &z->ent[i];
    return NULL;
}

const ng_zent_t *ng_zip_find_prefix(const ng_zip_t *z, const char *prefix)
{
    if (!z) return NULL;
    size_t l = strlen(prefix);
    for (int i = 0; i < z->n; i++)
        if (strncasecmp(z->ent[i].name, prefix, l) == 0) return &z->ent[i];
    return NULL;
}

/* ---- streams ---------------------------------------------------------------- */

ng_zstream_t *ng_zs_open(const ng_zip_t *z, const ng_zent_t *e)
{
    ng_zstream_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->e = e;
    s->f = fopen(z->path, "rb");
    if (!s->f) goto fail;
    uint8_t lh[30];
    if (fseek(s->f, e->lho, SEEK_SET) != 0 || fread(lh, 1, 30, s->f) != 30 || le32(lh) != 0x04034b50)
        goto fail;
    /* The local header's own name and extra lengths: the extra field may
     * differ from the central directory's. */
    if (fseek(s->f, e->lho + 30 + le16(lh + 26) + le16(lh + 28), SEEK_SET) != 0) goto fail;
    s->in_left = e->csize;
    s->out_left = e->usize;
    if (e->method == 8) {
        s->in = malloc(ZS_INBUF);
        s->dict = malloc(TINFL_LZ_DICT_SIZE);
        if (!s->in || !s->dict) goto fail;
        tinfl_init(&s->inf);
    }
    return s;
fail:
    ng_zs_close(s);
    return NULL;
}

void ng_zs_close(ng_zstream_t *s)
{
    if (!s) return;
    if (s->f) fclose(s->f);
    free(s->in);
    free(s->dict);
    free(s);
}

uint32_t ng_zs_crc(const ng_zstream_t *s)
{
    return s->crc;
}

/* Runs tinfl until it has bytes to hand out or the stream is over. */
static bool zs_inflate(ng_zstream_t *s)
{
    while (s->avail == 0 && !s->done) {
        if (s->in_pos == s->in_len && s->in_left) {
            uint32_t want = s->in_left < ZS_INBUF ? s->in_left : ZS_INBUF;
            if (fread(s->in, 1, want, s->f) != want) return false;
            s->in_left -= want;
            s->in_pos = 0;
            s->in_len = want;
        }
        size_t in_n = s->in_len - s->in_pos;
        size_t out_n = TINFL_LZ_DICT_SIZE - s->dict_ofs;
        tinfl_status st = tinfl_decompress(&s->inf, s->in + s->in_pos, &in_n, s->dict,
                                           s->dict + s->dict_ofs, &out_n,
                                           s->in_left ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        s->in_pos += in_n;
        s->avail_ofs = s->dict_ofs;
        s->avail = out_n;
        s->dict_ofs = (s->dict_ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        if (st == TINFL_STATUS_DONE) s->done = true;
        else if (st < 0) return false;
        else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && !s->in_left && s->in_pos == s->in_len && !out_n)
            return false;
    }
    return true;
}

int ng_zs_read(ng_zstream_t *s, void *dst, uint32_t n)
{
    if (s->failed) return -1;
    if (n > s->out_left) n = s->out_left;
    uint8_t *d = dst;
    uint32_t got = 0;
    if (s->e->method == 0) {
        if (n && fread(d, 1, n, s->f) != n) {
            s->failed = true;
            return -1;
        }
        got = n;
    } else {
        while (got < n) {
            if (!zs_inflate(s)) {
                s->failed = true;
                return -1;
            }
            if (s->avail == 0) break; /* the stream ended early */
            uint32_t k = n - got < s->avail ? n - got : s->avail;
            memcpy(d + got, s->dict + s->avail_ofs, k);
            s->avail_ofs += k;
            s->avail -= k;
            got += k;
        }
    }
    s->crc = ng_crc32(s->crc, d, got);
    s->out_left -= got;
    return (int)got;
}

bool ng_zs_skip(ng_zstream_t *s, uint32_t n)
{
    uint8_t tmp[1024];
    while (n) {
        uint32_t k = n < sizeof(tmp) ? n : sizeof(tmp);
        int r = ng_zs_read(s, tmp, k);
        if (r <= 0) return false;
        n -= (uint32_t)r;
    }
    return true;
}

int ng_zip_read(const ng_zip_t *z, const ng_zent_t *e, void *dst, uint32_t max)
{
    ng_zstream_t *s = ng_zs_open(z, e);
    if (!s) return -1;
    uint32_t n = e->usize < max ? e->usize : max;
    int r = ng_zs_read(s, dst, n);
    ng_zs_close(s);
    return r;
}
