"""Exercise the old and sized serial status entry points on ESP32-P4's ABI."""
import unittest

from rv32_harness import ROOT, run_c


class SerialStatusABI(unittest.TestCase):
    def test_legacy_current_and_partial_buffers(self):
        source = (ROOT / 'components/aos_io/aos_serial.c').read_text(encoding='utf-8')
        channel = source[source.index('typedef struct {'):source.index('/* ---- triggers')]
        status = source[source.index('bool aos_serial_stat_ex('):source.index('int aos_serial_line_ex(')]
        run_c(self, r'''
#define AOS_SERIAL_STAT_LEGACY_IMPLEMENTATION
#include "aos_io.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
void *memset(void *d, int v, size_t n) {
    unsigned char *p=d; while(n--) *p++=(unsigned char)v; return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *p=d; const unsigned char *q=s; while(n--) *p++=*q++; return d;
}
int snprintf(char *dst, size_t n, const char *fmt, ...) {
    va_list a; va_start(a, fmt); const char *s=va_arg(a, const char *); va_end(a);
    size_t i=0; while(s[i] && i+1<n) {dst[i]=s[i]; i++;} if(n) dst[i]=0; return (int)i;
}
''' + channel + status + r'''
/* Size of the original status struct in dev's ABI 101. */
_Static_assert(offsetof(aos_serial_stat_t, frame)==248, "legacy RV32 layout");
int test_main(void) {
    _Alignas(aos_serial_stat_t) unsigned char old[248+8];
    memset(old, 0xA5, sizeof old);
    s_ch[0].running=true; s_ch[0].raw_total=567;
    memcpy(s_ch[0].frame, "7E1", 4); s_ch[0].ctl=AOS_SERIAL_DTR;
    aos_serial_stat_t *st=(aos_serial_stat_t *)(void *)old;
    if(!aos_serial_stat(0,st) || !st->running || st->raw_total!=567) return 1;
    for(size_t i=248;i<sizeof old;i++) if(old[i]!=0xA5) return 2;
    aos_serial_stat_t now;
    if(!aos_serial_stat_ex(0,&now,sizeof now) || now.frame[0]!='7' ||
       now.frame[1]!='E' || now.frame[2]!='1' || now.ctl!=AOS_SERIAL_DTR) return 3;
    memset(old, 0xA5, sizeof old);
    if(!aos_serial_stat_ex(0,st,1) || old[0]!=1 || old[1]!=0xA5) return 4;
    if(aos_serial_stat(-1,st) || aos_serial_stat_ex(0,0,32) ||
       aos_serial_stat_ex(0,st,0)) return 5;
    struct {aos_serial_stat_t status; int extra; unsigned guard;} future;
    memset(&future,0xA5,sizeof future);
    if(!aos_serial_stat_ex(1,&future.status,offsetof(typeof(future),guard))) return 6;
    if(future.status.frame[0]!='8' || future.extra || future.guard!=0xA5A5A5A5) return 7;
    return 0;
}
''', '-I' + str(ROOT / 'components/aos_io/include'))

    def test_new_header_passes_caller_size(self):
        run_c(self, r'''
#include "aos_io.h"
bool aos_serial_stat_ex(int ch, aos_serial_stat_t *out, size_t size) {
    return ch == 1 && out && size == sizeof *out;
}
int test_main(void) {
    bool (*read)(int, aos_serial_stat_t *) = aos_serial_stat;
    aos_serial_stat_t status;
    return read(1,&status) ? 0 : 1;
}
''', '-I' + str(ROOT / 'components/aos_io/include'))
