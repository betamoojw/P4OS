/*
 * P4OS - Programmer: firmware files as regions of bytes at addresses
 * (aos_flasher.h, "images").
 *
 * Every format comes down to pieces - an address, a length, the bytes - that
 * point into one block of PSRAM: the file itself for a .bin, an .elf and a
 * .uf2, the decoded bytes for a .hex (half its size, at most). The pieces are
 * sorted, an overlap is refused (two places want different bytes at one
 * address: the file is wrong), and runs of pieces with gaps of up to
 * GAP_FILL bytes become one region, copied into a block of its own with the
 * gaps at 0xFF, the value of erased flash on every chip the families write.
 *
 *   .hex   records 00 (data), 01 (end), 02 (segment, x16), 04 (upper 16 bits
 *          of a linear address), 03/05 (start address); each line's checksum
 *          is checked.
 *   .elf   32-bit little-endian; the PT_LOAD segments with bytes in the file,
 *          at their physical address (p_paddr): the LMA, where .data's
 *          initial values live in flash, not where they are copied to.
 *   .uf2   512-byte blocks; those flagged "not main flash" are skipped, and
 *          the family ID (when the flag says there is one) is kept, so a
 *          family can refuse a file made for another chip.
 *
 * HAL-clean: only stdio and malloc, so tests/flasher_img builds it on the Mac.
 */
#include "aos_flasher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define GAP_FILL 256

typedef struct {
    uint32_t addr, len;
    const uint8_t *data;
} piece_t;

typedef struct {
    piece_t *p;
    int n, cap;
} pieces_t;

static bool fail(char *err, size_t n, const char *msg)
{
    if (err && n) snprintf(err, n, "%s", msg);
    return false;
}

static bool add(pieces_t *ps, uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (!len) return true;
    if (ps->n == ps->cap) {
        int cap = ps->cap ? ps->cap * 2 : 256;
        piece_t *p = realloc(ps->p, (size_t)cap * sizeof *p);
        if (!p) return false;
        ps->p = p;
        ps->cap = cap;
    }
    ps->p[ps->n++] = (piece_t){ addr, len, data };
    return true;
}

static uint32_t rd32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }

aos_flasher_img_fmt_t aos_flasher_image_fmt(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return AOS_IMG_BIN;
    if (!strcasecmp(dot, ".hex") || !strcasecmp(dot, ".ihex") || !strcasecmp(dot, ".ihx")) return AOS_IMG_HEX;
    if (!strcasecmp(dot, ".elf") || !strcasecmp(dot, ".axf")) return AOS_IMG_ELF;
    if (!strcasecmp(dot, ".uf2")) return AOS_IMG_UF2;
    return AOS_IMG_BIN;
}

/* ---- the formats ---------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const uint8_t *f, size_t n, uint8_t *pool, pieces_t *ps, aos_flasher_image_t *img,
                      char *err, size_t err_len)
{
    uint32_t base = 0, used = 0;
    bool end = false;
    size_t i = 0;
    while (i < n && !end) {
        while (i < n && f[i] != ':') i++;
        if (i >= n) break;
        i++;
        uint8_t rec[260];
        int len = -1, k = 0;
        uint8_t sum = 0;
        while (i + 1 < n) {
            int hi = hexval((char)f[i]), lo = hexval((char)f[i + 1]);
            if (hi < 0 || lo < 0) break;
            if (k >= (int)sizeof rec) return fail(err, err_len, "hex: a line too long");
            rec[k] = (uint8_t)(hi << 4 | lo);
            sum += rec[k++];
            i += 2;
            if (k == 1) len = rec[0];
            if (len >= 0 && k == len + 5) break;
        }
        if (len < 0 || k != len + 5) return fail(err, err_len, "hex: a line is cut short");
        if (sum) return fail(err, err_len, "hex: a line's checksum is wrong");
        uint16_t off = (uint16_t)(rec[1] << 8 | rec[2]);
        const uint8_t *d = rec + 4;
        switch (rec[3]) {
        case 0x00:
            memcpy(pool + used, d, (size_t)len);
            if (!add(ps, base + off, pool + used, (uint32_t)len)) return fail(err, err_len, "out of memory");
            used += (uint32_t)len;
            break;
        case 0x01: end = true; break;
        case 0x02: if (len == 2) base = (uint32_t)(d[0] << 8 | d[1]) << 4; break;
        case 0x04: if (len == 2) base = (uint32_t)(d[0] << 8 | d[1]) << 16; break;
        case 0x03: if (len == 4) img->entry = (uint32_t)(d[0] << 8 | d[1]) << 4 | (uint32_t)(d[2] << 8 | d[3]); break;
        case 0x05: if (len == 4) img->entry = (uint32_t)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3]; break;
        default: return fail(err, err_len, "hex: an unknown record type");
        }
    }
    return true;
}

#define PT_LOAD 1

static bool parse_elf(const uint8_t *f, size_t n, pieces_t *ps, aos_flasher_image_t *img, char *err, size_t err_len)
{
    if (n < 52 || memcmp(f, "\x7f" "ELF", 4)) return fail(err, err_len, "elf: not an ELF file");
    if (f[4] != 1 || f[5] != 1) return fail(err, err_len, "elf: only 32-bit little-endian");
    img->entry = rd32(f + 24);
    uint32_t phoff = rd32(f + 28);
    uint16_t phentsize = rd16(f + 42), phnum = rd16(f + 44);
    if (phentsize < 32 || (uint64_t)phoff + (uint64_t)phnum * phentsize > n)
        return fail(err, err_len, "elf: its program headers are cut short");
    for (int k = 0; k < phnum; k++) {
        const uint8_t *ph = f + phoff + (size_t)k * phentsize;
        uint32_t type = rd32(ph), off = rd32(ph + 4), paddr = rd32(ph + 12), filesz = rd32(ph + 16);
        if (type != PT_LOAD || !filesz) continue;
        if ((uint64_t)off + filesz > n) return fail(err, err_len, "elf: a segment is cut short");
        if (!add(ps, paddr, f + off, filesz)) return fail(err, err_len, "out of memory");
    }
    return true;
}

#define UF2_MAGIC0     0x0A324655u
#define UF2_MAGIC1     0x9E5D5157u
#define UF2_MAGIC_END  0x0AB16F30u
#define UF2_NOT_MAIN   0x00000001u
#define UF2_FAMILY     0x00002000u

static bool parse_uf2(const uint8_t *f, size_t n, pieces_t *ps, aos_flasher_image_t *img, char *err, size_t err_len)
{
    if (n < 512 || n % 512) return fail(err, err_len, "uf2: not a whole number of 512-byte blocks");
    for (size_t b = 0; b < n; b += 512) {
        const uint8_t *blk = f + b;
        if (rd32(blk) != UF2_MAGIC0 || rd32(blk + 4) != UF2_MAGIC1 || rd32(blk + 508) != UF2_MAGIC_END)
            return fail(err, err_len, "uf2: a block without its magic numbers");
        uint32_t flags = rd32(blk + 8), addr = rd32(blk + 12), size = rd32(blk + 16), fam = rd32(blk + 28);
        if (flags & UF2_NOT_MAIN) continue;
        if (size > 476) return fail(err, err_len, "uf2: a block with more than 476 bytes");
        if (flags & UF2_FAMILY) {
            if (img->uf2_family && img->uf2_family != fam)
                return fail(err, err_len, "uf2: blocks for two different chips");
            img->uf2_family = fam;
        }
        if (!add(ps, addr, blk + 32, size)) return fail(err, err_len, "out of memory");
    }
    return true;
}

/* ---- pieces to regions ---------------------------------------------------- */

static int by_addr(const void *a, const void *b)
{
    const piece_t *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static bool regions(pieces_t *ps, aos_flasher_image_t *img, char *err, size_t err_len)
{
    qsort(ps->p, (size_t)ps->n, sizeof *ps->p, by_addr);
    /* first the sizes, to take one block for all of them */
    size_t need = 0;
    int nr = 0;
    for (int i = 0; i < ps->n;) {
        uint64_t end = (uint64_t)ps->p[i].addr + ps->p[i].len;
        int j = i + 1;
        for (; j < ps->n; j++) {
            if (ps->p[j].addr < end) return fail(err, err_len, "two parts of the file write the same address");
            if (ps->p[j].addr - end > GAP_FILL) break;
            end = (uint64_t)ps->p[j].addr + ps->p[j].len;
        }
        if (end > 0x100000000ull) return fail(err, err_len, "an address past 4 GB");
        need += (size_t)(end - ps->p[i].addr);
        if (++nr > AOS_FLASHER_REGIONS_MAX) return fail(err, err_len, "too many separate regions");
        i = j;
    }
    if (!need) return fail(err, err_len, "the file has nothing to write");
    uint8_t *mem = malloc(need);
    if (!mem) return fail(err, err_len, "out of memory");
    uint8_t *w = mem;
    img->n = 0;
    img->total = 0;
    for (int i = 0; i < ps->n;) {
        uint32_t start = ps->p[i].addr, end = start;
        int j = i;
        for (; j < ps->n && (j == i || ps->p[j].addr - end <= GAP_FILL); j++) {
            memset(w + (end - start), 0xFF, ps->p[j].addr - end);
            memcpy(w + (ps->p[j].addr - start), ps->p[j].data, ps->p[j].len);
            end = ps->p[j].addr + ps->p[j].len;
        }
        img->r[img->n++] = (aos_flasher_region_t){ start, end - start, w };
        img->total += end - start;
        w += end - start;
        i = j;
    }
    img->mem = mem;
    return true;
}

bool aos_flasher_image_load(const char *path, uint32_t bin_addr, aos_flasher_image_t *img, char *err, size_t err_len)
{
    memset(img, 0, sizeof *img);
    img->fmt = aos_flasher_image_fmt(path);
    struct stat st;
    if (stat(path, &st) || st.st_size <= 0) return fail(err, err_len, "cannot read the file");
    if (st.st_size > 32 * 1024 * 1024) return fail(err, err_len, "the file is over 32 MB");
    size_t n = (size_t)st.st_size;
    uint8_t *f = malloc(n);
    uint8_t *pool = img->fmt == AOS_IMG_HEX ? malloc(n / 2 + 1) : NULL;
    if (!f || (img->fmt == AOS_IMG_HEX && !pool)) {
        free(f);
        free(pool);
        return fail(err, err_len, "out of memory");
    }
    FILE *fp = fopen(path, "rb");
    size_t got = fp ? fread(f, 1, n, fp) : 0;
    if (fp) fclose(fp);
    pieces_t ps = { 0 };
    bool ok = got == n;
    if (!ok) fail(err, err_len, "cannot read the file");
    else if (img->fmt == AOS_IMG_HEX) ok = parse_hex(f, n, pool, &ps, img, err, err_len);
    else if (img->fmt == AOS_IMG_ELF) ok = parse_elf(f, n, &ps, img, err, err_len);
    else if (img->fmt == AOS_IMG_UF2) ok = parse_uf2(f, n, &ps, img, err, err_len);
    else ok = add(&ps, bin_addr, f, (uint32_t)n) || fail(err, err_len, "out of memory");
    if (ok) ok = regions(&ps, img, err, err_len);
    free(ps.p);
    free(pool);
    free(f);
    if (!ok) memset(img, 0, sizeof *img);
    return ok;
}

void aos_flasher_image_free(aos_flasher_image_t *img)
{
    if (!img) return;
    free(img->mem);
    memset(img, 0, sizeof *img);
}
