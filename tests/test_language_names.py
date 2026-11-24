"""A store language update can omit meta.txt on the removable card."""
import unittest

from rv32_harness import ROOT, run_c


class LanguageNames(unittest.TestCase):
    def test_native_names_for_metadata_free_packs(self):
        source = (ROOT / 'components/aos_ui/aos_i18n.c').read_text(encoding='utf-8')
        lookup = source[source.index('static const aos_lang_pack_t *embedded_pack'):
                        source.index('/* Mutable copy of an embedded catalogue')]
        fallback = source[source.index('static const char *native_name('):
                          source.index('/* Reads the display name')]
        self.assertIn('snprintf(out, len, "%s", native_name(code));', source)
        run_c(self, '''
#include "aos_lang_embedded.h"
#include <string.h>
const int aos_lang_pack_count=2;
const aos_lang_pack_t aos_lang_packs[]={
    {.code="zh-Hans", .name="简体中文"}, {.code="zh-Hant", .name="繁體中文"}
};
int strcmp(const char *s, const char *t) {
    while(*s && *s==*t) {s++; t++;} return (unsigned char)*s - (unsigned char)*t;
}
''' + lookup + fallback + '''
int test_main(void) {
    return strcmp(native_name("zh-Hans"), "简体中文") ||
           strcmp(native_name("zh-Hant"), "繁體中文") ||
           strcmp(native_name("pt"), "Português (Brasil)") ||
           strcmp(native_name("xy"), "xy");
}
''', '-I' + str(ROOT / 'components/aos_ui'))
