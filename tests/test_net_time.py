"""Fault-inject production SNTP startup and asynchronous requests."""
import re
import unittest
from rv32_harness import ROOT, run_c


class NetworkTime(unittest.TestCase):
    def test_start_failure_retry_reconnect_and_manual_request(self):
        source = (ROOT / 'components/aos_hal/aos_net_p4.c').read_text(encoding='utf-8')
        services = source[source.index('static void time_synced('):source.index('static esp_netif_t *s_ap_netif;')]
        request = source[source.index('bool aos_hal_net_sync_time('):source.index('void aos_hal_net_low_latency(')]
        cfg = (ROOT / 'sdkconfig.defaults').read_text(encoding='utf-8')
        count = int(re.search(r'^CONFIG_LWIP_SNTP_MAX_SERVERS=(\d+)$', cfg, re.M)[1])
        self.assertGreaterEqual(count, 3)
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
struct timeval {long long tv_sec;};
typedef int esp_err_t;
typedef int esp_event_base_t;
enum {ESP_OK=0,AOS_NET_CONNECTED=1,AOS_NET_TIME_EVENT=123};
typedef struct {bool wait_for_sync,start;void (*sync_cb)(struct timeval*);int num_of_servers;const char *servers[CONFIG_LWIP_SNTP_MAX_SERVERS];} esp_sntp_config_t;
#define ESP_SNTP_SERVER_LIST(...) {__VA_ARGS__}
#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(n,list) {.start=true,.wait_for_sync=true,.num_of_servers=n,.servers=list}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static bool s_sntp;
static int s_state,init_error,start_error,queue_error,init_calls,start_calls,mdns_calls,queued,bad;
static int eq(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static int esp_netif_sntp_init(const esp_sntp_config_t *c){
    init_calls++;
    if(!c->start || c->wait_for_sync || !c->sync_cb || c->num_of_servers!=3 ||
       !eq(c->servers[0],"pool.ntp.org") || !eq(c->servers[1],"ntp.aliyun.com") || !eq(c->servers[2],"time.cloudflare.com"))bad=1;
    return init_error;
}
static int esp_netif_sntp_start(void){start_calls++;return start_error;}
static void mdns_up(void){mdns_calls++;}
static int esp_event_post(int base,int id,void *data,size_t n,int wait){
    if(base!=AOS_NET_TIME_EVENT || id || data || n || wait)bad=1;
    queued++;return queue_error;
}
'''
        cases = r'''
int test_main(void){
    /* Initial failure must leave init retryable without killing mDNS. */
    init_error=5;start_services();
    if(s_sntp || init_calls!=1 || mdns_calls!=1) return 1;
    init_error=0;start_services();
    if(!s_sntp || init_calls!=2 || mdns_calls!=2) return 2;
    start_services();if(init_calls!=2 || start_calls!=1) return 3;
    start_error=7;if(start_sntp()) return 4;
    start_error=0;if(!start_sntp()) return 5;
    s_state=0;if(aos_hal_net_sync_time() || queued) return 6;
    s_state=AOS_NET_CONNECTED;queue_error=9;if(aos_hal_net_sync_time()) return 7;
    queue_error=0;int before=start_calls;
    if(!aos_hal_net_sync_time() || !aos_hal_net_sync_time() || start_calls!=before) return 8;
    time_request(NULL,AOS_NET_TIME_EVENT,0,NULL);
    if(start_calls!=before+1 || init_calls!=2) return 9;
    s_state=0;time_request(NULL,AOS_NET_TIME_EVENT,0,NULL);
    if(start_calls!=before+1 || bad) return 10;
    return 0;
}
'''
        run_c(self, harness + services + request + cases, f'-DCONFIG_LWIP_SNTP_MAX_SERVERS={count}')


if __name__ == '__main__':
    unittest.main()
