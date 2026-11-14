"""Old ABI 101 callers must not receive the enlarged Ethernet structure."""
import unittest
from rv32_harness import ROOT, run_c

INCLUDE = '-I' + str(ROOT / 'components/aos_io/include')

HARNESS = r'''
#define AOS_ETH_INFO_LEGACY_IMPLEMENTATION
#include "aos_eth.h"
#include <string.h>
/* The public structure in 5481de9, as an already-built app allocated it. */
typedef struct {
    bool enabled, running, link;
    int speed_mbps;
    bool full_duplex;
    char ip[16], mask[16], gw[16];
    uint8_t mac[6];
    char port[16];
    int cs;
    uint32_t spi_hz, link_s, links;
    char err[80];
    bool paused;
    char paused_by[24];
} legacy_info_t;
_Static_assert(sizeof(legacy_info_t) == AOS_ETH_INFO_ABI101_SIZE, "legacy allocation");
_Static_assert(offsetof(aos_eth_info_t, mode) == sizeof(legacy_info_t), "legacy prefix");
#define CHECK_FIELD(f) _Static_assert(offsetof(legacy_info_t, f) == offsetof(aos_eth_info_t, f), #f)
CHECK_FIELD(enabled); CHECK_FIELD(running); CHECK_FIELD(link); CHECK_FIELD(speed_mbps);
CHECK_FIELD(full_duplex); CHECK_FIELD(ip); CHECK_FIELD(mask); CHECK_FIELD(gw);
CHECK_FIELD(mac); CHECK_FIELD(port); CHECK_FIELD(cs); CHECK_FIELD(spi_hz);
CHECK_FIELD(link_s); CHECK_FIELD(links); CHECK_FIELD(err); CHECK_FIELD(paused);
CHECK_FIELD(paused_by);
void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d=dst; const unsigned char *s=src; while(n--) *d++=*s++; return dst;
}
void *memset(void *dst, int value, size_t n) {
    unsigned char *d=dst; while(n--) *d++=(unsigned char)value; return dst;
}
static struct {aos_eth_info_t info; int64_t link_at; uint32_t rx_frames;} E;
static aos_eth_info_t s_info;
static int64_t esp_timer_get_time(void) {return 9000000;}
static void read_once(void) {}
'''

CASES = r'''
int test_main(void) {
    E.info.enabled=true; E.info.link=true; E.info.speed_mbps=100;
    E.info.ip[0]='1'; E.info.paused=true; E.info.paused_by[0]='x';
    E.info.mode=3; E.info.rx_frames=91; E.info.restarts=4;
    E.link_at=2000000; E.rx_frames=91; E.info.link_s=7;
    s_info=E.info;
    struct {uint32_t before; legacy_info_t info; uint32_t after[4];} old;
    memset(&old, 0xA5, sizeof old);
    aos_eth_info((aos_eth_info_t *)&old.info);
    if(old.before!=0xA5A5A5A5 || old.after[0]!=0xA5A5A5A5 ||
       old.after[1]!=0xA5A5A5A5 || old.after[2]!=0xA5A5A5A5 || old.after[3]!=0xA5A5A5A5) return 1;
    if(!old.info.enabled || old.info.speed_mbps!=100 || old.info.ip[0]!='1' ||
       !old.info.paused || old.info.paused_by[0]!='x' || old.info.link_s!=7) return 2;
    struct {aos_eth_info_t info; uint32_t after;} current;
    memset(&current, 0xA5, sizeof current);
    aos_eth_info_read(&current.info, sizeof current.info);
    if(current.after!=0xA5A5A5A5 || current.info.mode!=3 ||
       current.info.rx_frames!=91 || current.info.restarts!=4) return 3;
    struct {aos_eth_info_t info; uint32_t extra[2]; uint32_t after;} future;
    memset(&future, 0xA5, sizeof future);
    aos_eth_info_read(&future.info, sizeof future.info + sizeof future.extra);
    if(future.extra[0] || future.extra[1] || future.after!=0xA5A5A5A5) return 4;
    unsigned char tiny[4]={0xA5,0xA5,0xA5,0xA5};
    aos_eth_info_read((aos_eth_info_t *)tiny, 1);
    if(tiny[0]!=1 || tiny[1]!=0xA5 || tiny[2]!=0xA5 || tiny[3]!=0xA5) return 5;
    aos_eth_info_read((aos_eth_info_t *)tiny, 0);
    aos_eth_info_read(NULL, sizeof(aos_eth_info_t));
    aos_eth_info(NULL);
    return 0;
}
'''


class EthernetABI(unittest.TestCase):
    def test_legacy_current_and_future_buffers_on_both_backends(self):
        for path, end in (
            ('components/aos_io/aos_eth_p4.c', '/* The server and the lease'),
            ('sim/eth_sim.c', 'void aos_eth_dhcp('),
        ):
            with self.subTest(path=path):
                source = (ROOT / path).read_text(encoding='utf-8')
                start = source.index('void aos_eth_info_read(')
                functions = source[start:source.index(end, start)]
                run_c(self, HARNESS + functions + CASES, INCLUDE, '-lgcc')

    def test_current_header_passes_size_even_through_a_function_pointer(self):
        run_c(self, r'''
#include "aos_eth.h"
static size_t seen;
void aos_eth_info_read(aos_eth_info_t *out, size_t size) {
    seen=size; if(out) out->mode=3;
}
int test_main(void) {
    aos_eth_info_t out={0};
    void (*read)(aos_eth_info_t *)=aos_eth_info;
    read(&out);
    if(seen!=sizeof out || out.mode!=3) return 1;
    seen=0; aos_eth_info(NULL);
    return seen==sizeof out ? 0 : 2;
}
''', INCLUDE)


if __name__ == '__main__':
    unittest.main()
