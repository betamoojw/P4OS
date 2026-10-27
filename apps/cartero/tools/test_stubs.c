/* Cartero: the few HAL calls the core needs, for test_core.c on the Mac (no network: the TLS calls fail). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <sys/time.h>
uint64_t aos_hal_uptime_ms(void){ struct timeval t; gettimeofday(&t,0); return (uint64_t)t.tv_sec*1000+t.tv_usec/1000; }
uint64_t aos_hal_uptime_us(void){ struct timeval t; gettimeofday(&t,0); return (uint64_t)t.tv_sec*1000000+t.tv_usec; }
bool aos_hal_time_is_valid(void){ return true; }
typedef struct ct_tls ct_tls_t;
ct_tls_t *aos_tls_new(void){ return 0; }
void aos_tls_free(ct_tls_t *c){ (void)c; }
int aos_tls_open(ct_tls_t *c, const char *h, int p, unsigned f, int t, char *w, size_t n){ return -2; }
bool aos_tls_send(ct_tls_t *c, const void *b, int l, int t){ return false; }
int aos_tls_recv(ct_tls_t *c, void *b, int m, int t){ return -1; }
