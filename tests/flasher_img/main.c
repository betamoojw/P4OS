/* Prints what aos_flasher_image_load() makes of a file: one line per region
 * (address, length, SHA-256 of its bytes), then the family and entry. */
#include <stdio.h>
#include <stdlib.h>
#include <CommonCrypto/CommonDigest.h>
#include "aos_flasher.h"

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    aos_flasher_image_t img;
    char err[96];
    uint32_t at = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0;
    if (!aos_flasher_image_load(argv[1], at, &img, err, sizeof err)) {
        printf("ERROR %s\n", err);
        return 1;
    }
    for (int i = 0; i < img.n; i++) {
        unsigned char h[32];
        CC_SHA256(img.r[i].data, img.r[i].len, h);
        printf("%08x %u ", (unsigned)img.r[i].addr, (unsigned)img.r[i].len);
        for (int k = 0; k < 32; k++) printf("%02x", h[k]);
        printf("\n");
    }
    printf("total %u family %08x entry %08x\n", (unsigned)img.total, (unsigned)img.uf2_family, (unsigned)img.entry);
    aos_flasher_image_free(&img);
    return 0;
}
