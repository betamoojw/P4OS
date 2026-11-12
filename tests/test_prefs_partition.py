"""NVS migration must never erase or move the offline Chinese fonts."""
import re
import unittest
from rv32_harness import ROOT, run_c


class PreferencesPartition(unittest.TestCase):
    def test_partition_table_preserves_fonts_and_has_no_overlap(self):
        def size(value):
            value = value.strip()
            if value.endswith(('K', 'M')):
                return int(value[:-1], 0) * (1024 if value[-1] == 'K' else 1024**2)
            return int(value, 0)
        parts = {}
        for line in (ROOT / 'partitions.csv').read_text().splitlines():
            if not line.strip() or line.startswith('#'):
                continue
            name, kind, subtype, address, length, *_ = line.split(',')
            parts[name.strip()] = (size(address), size(length))
        self.assertEqual(parts['storage'], (0x1000000, 12 * 1024**2))
        self.assertEqual(parts['coredump'], (0x1C00000, 1024**2))
        self.assertEqual(parts['prefs'], (0x1D00000, 256 * 1024))
        end = 0
        for address, length in sorted(parts.values()):
            self.assertGreaterEqual(address, end)
            end = address + length
        self.assertLessEqual(end, 32 * 1024**2)

    def test_existing_and_ota_tables_never_initialize_an_unsafe_area(self):
        source = (ROOT / 'components/aos_hal/aos_hal_p4.c').read_text(encoding='utf-8')
        definitions = '\n'.join(re.findall(r'^#define PREFS_(?:SIZE|ADDRESS|CARVED)\s+.*$', source, re.M))
        function = source[source.index('static const char *prefs_large('):source.index('/* Every key of the first store')]
        harness = r'''
#include <stddef.h>
#include <stdbool.h>
typedef int esp_err_t;
enum {ESP_OK=0,ESP_ERR_NVS_NO_FREE_PAGES=1,ESP_ERR_NVS_NEW_VERSION_FOUND=2,
      ESP_PARTITION_TYPE_DATA=1,ESP_PARTITION_SUBTYPE_DATA_NVS=2};
typedef struct {unsigned address,size;const char *label;} esp_partition_t;
static esp_partition_t named,registered;
static bool have_named,have_registered;
static int register_error,init_error,erase_error,register_calls,init_calls,erase_calls,bad;
#define ESP_LOGW(...) ((void)0)
static const esp_partition_t *esp_partition_find_first(int t,int s,const char *label){
    if(label[0]=='p' && label[1]=='r') return have_named ? &named : NULL;
    return have_registered ? &registered : NULL;
}
static int esp_partition_register_external(void *chip,unsigned address,unsigned size,
    const char *label,int type,int subtype,const esp_partition_t **out){
    register_calls++;
    if(chip || address!=0x1D00000 || size!=0x40000 || type!=1 || subtype!=2) bad=1;
    if(register_error) return register_error;
    registered=(esp_partition_t){address,size,label};have_registered=true;*out=&registered;
    return ESP_OK;
}
static int nvs_flash_init_partition_ptr(const esp_partition_t *p){
    init_calls++;if(p->address!=0x1D00000 || p->size!=0x40000) bad=1;
    int e=init_error;init_error=0;return e;
}
static int esp_partition_erase_range(const esp_partition_t *p,unsigned offset,unsigned size){
    erase_calls++;if(p->address!=0x1D00000 || offset || size!=0x40000) bad=1;
    return erase_error;
}
'''
        cases = r'''
int test_main(void){
    named=(esp_partition_t){0x1D00000,0x40000,"prefs"};have_named=true;
    if(!prefs_large() || init_calls!=1 || register_calls || erase_calls) return 1;
    have_named=false;
    if(!prefs_large() || register_calls!=1 || init_calls!=2 || erase_calls) return 2;
    if(!prefs_large() || register_calls!=1 || init_calls!=3) return 3;
    have_registered=false;register_error=9;
    if(prefs_large() || init_calls!=3 || erase_calls) return 4;
    have_named=true;named.address=0x1000000;
    if(prefs_large() || init_calls!=3 || erase_calls) return 5;
    named.address=0x1D00000;named.size=0x80000;
    if(prefs_large() || init_calls!=3 || erase_calls) return 6;
    named.size=0x40000;init_error=ESP_ERR_NVS_NO_FREE_PAGES;
    if(!prefs_large() || init_calls!=5 || erase_calls!=1) return 7;
    init_error=ESP_ERR_NVS_NEW_VERSION_FOUND;erase_error=9;
    if(prefs_large() || init_calls!=6 || erase_calls!=2 || bad) return 8;
    return 0;
}
'''
        run_c(self, definitions + harness + function + cases)


if __name__ == '__main__':
    unittest.main()
