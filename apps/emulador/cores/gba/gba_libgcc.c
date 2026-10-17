/*
 * P4OS - GBA core: two of libgcc's helpers the firmware does not lend.
 *
 * Without Zbb (the rev 1.3 P4 has none, board.cmake) gcc calls these for
 * __builtin_bswap32 and __builtin_popcount. Board only: the Mac's libgcc
 * has them.
 *
 * The byte swap is built at -O0 through a volatile array: written with
 * shifts, gcc recognises the idiom and turns the body into a call to
 * itself (apps/doom/main/port/dg_system.c measured it).
 */
#ifndef AOS_SIM

__attribute__((optimize("O0")))
unsigned int __bswapsi2(unsigned int x)
{
    volatile unsigned char b[4];
    b[0] = (unsigned char)x;
    b[1] = (unsigned char)(x >> 8);
    b[2] = (unsigned char)(x >> 16);
    b[3] = (unsigned char)(x >> 24);
    return ((unsigned int)b[0] << 24) | ((unsigned int)b[1] << 16) | ((unsigned int)b[2] << 8) | (unsigned int)b[3];
}

__attribute__((optimize("O0")))
int __popcountsi2(unsigned int x)
{
    int n = 0;
    while (x) {
        x &= x - 1;
        n++;
    }
    return n;
}

#endif
