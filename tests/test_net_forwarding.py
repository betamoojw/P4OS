"""Enabling NAT support must not bridge networks while sharing is off."""
import unittest
from rv32_harness import ROOT, run_c


class NetworkForwarding(unittest.TestCase):
    def test_only_the_selected_shared_network_may_forward(self):
        source = (ROOT / 'components/aos_hal/aos_net_p4.c').read_text(encoding='utf-8')
        start = source.index('int aos_ip4_canforward(')
        function = source[start:source.index("/* The shared network's DHCP server", start)]
        harness = r'''
#include <stdint.h>
#include <stddef.h>
struct pbuf {int unused;};
typedef struct {uint32_t addr;} ip4_addr_t;
struct netif {ip4_addr_t ip,mask;};
static struct netif *s_share_inner,*input;
static struct netif *ip_current_input_netif(void) {return input;}
static uint32_t lwip_htonl(uint32_t n) {return __builtin_bswap32(n);}
#define netif_ip4_addr(n) (&(n)->ip)
#define netif_ip4_netmask(n) (&(n)->mask)
#define ip4_addr_netcmp(a,b,m) (((a)->addr & (m)->addr)==((b)->addr & (m)->addr))
'''
        cases = r'''
int test_main(void) {
    struct netif ap={{lwip_htonl(0xc0a80401)},{lwip_htonl(0xffffff00)}};
    struct netif wire={{lwip_htonl(0xc0a84d01)},{lwip_htonl(0xffffff00)}};
    struct netif wifi={{lwip_htonl(0xc0a80102)},{lwip_htonl(0xffffff00)}};
    input=&ap;
    if(aos_ip4_canforward(NULL,0x08080808)!=0) return 1;
    input=&wifi;
    if(aos_ip4_canforward(NULL,0xc0a80402)!=0) return 2;
    s_share_inner=&ap;
    if(aos_ip4_canforward(NULL,0xc0a80402)!=-1) return 3; /* NAT return to the shared network */
    if(aos_ip4_canforward(NULL,0xc0a84d02)!=0) return 4; /* not the unshared wire */
    if(aos_ip4_canforward(NULL,0x08080808)!=0) return 5;
    input=&ap;
    if(aos_ip4_canforward(NULL,0x08080808)!=-1) return 6; /* shared client's outbound packet */
    s_share_inner=&wire;
    if(aos_ip4_canforward(NULL,0x08080808)!=0) return 7; /* AP is no longer shared */
    input=&wire;
    if(aos_ip4_canforward(NULL,0x08080808)!=-1) return 8;
    s_share_inner=NULL;
    if(aos_ip4_canforward(NULL,0x08080808)!=0) return 9;
    return 0;
}
'''
        run_c(self, harness + function + cases, '-lgcc')


if __name__ == '__main__':
    unittest.main()
