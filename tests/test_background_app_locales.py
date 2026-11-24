"""Link the real embedded language packs, loader and both background app readers."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST_CC = os.environ.get('HOST_CC') or shutil.which('cc')
if not HOST_CC and os.name == 'nt':
    local = ROOT / 'build/toolchains/msys64/ucrt64/bin/gcc.exe'
    HOST_CC = str(local) if local.is_file() else None


@unittest.skipUnless(HOST_CC, 'requires a host C compiler with zlib')
class BackgroundAppLocales(unittest.TestCase):
    def test_flash_card_override_and_language_switch(self):
        ui = ROOT / 'components/aos_ui'
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as folder:
            tmp = Path(folder)
            (tmp / 'aos_hal.h').write_text('''
#include <stdbool.h>
#include <stddef.h>
const char *aos_hal_path_lang(void);
void aos_hal_log(const char *, const char *, ...);
bool aos_hal_pref_get_str(const char *, char *, size_t);
void aos_hal_pref_set_str(const char *,const char *);
''')
            (tmp / 'aos_cjk.h').write_text('''
#include <stdbool.h>
bool aos_cjk_init(void);
void aos_cjk_language(const char *);
''')
            app_header = '''
#include "aos_i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define pl_ps_alloc malloc
#define pl_scpy(d,n,s) snprintf(d,n,"%s",s)
#define ec_copy(d,n,s) snprintf(d,n,"%s",s)
'''
            for app, prefix in [('estado', 'ec'), ('plotter', 'pl')]:
                (tmp / (prefix + '.h')).write_text(app_header)
                shutil.copy(ROOT / 'apps' / app / 'main' / (prefix + '_i18n.c'), tmp)
            (tmp / 'exercise.c').write_text('''
#include "aos_i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void ec_i18n_load(void); void pl_i18n_load(void);
void ec_i18n_free(void); void pl_i18n_free(void);
const char *ec_tr(const char *); const char *pl_tr(const char *);
static const char *card = "no-card";
const char *aos_hal_path_lang(void) {return card;}
void aos_hal_log(const char *tag, const char *fmt, ...) {}
bool aos_hal_pref_get_str(const char *key, char *out, size_t len) {return false;}
void aos_hal_pref_set_str(const char *key, const char *val) {}
bool aos_cjk_init(void) {return true;}
void aos_cjk_language(const char *code) {}
static int check(const char *lang, const char *camera, const char *meter, const char *reading) {
    if(!aos_i18n_set(lang)) return 1;
    ec_i18n_load(); pl_i18n_load();
    aos_i18n_app_load("demo.asteroides");
    int bad = strcmp(ec_tr("Cámaras"),camera) || strcmp(pl_tr("Multímetro"),meter)
           || strcmp(pl_tr("Lectura"),reading);
    ec_i18n_free(); pl_i18n_free();
    return bad;
}
int main(void) {
    if(check("zh-Hans","摄像头","万用表","读数")) return 1;
    if(check("zh-Hant","攝影機","三用電表","讀數")) return 2;
    card="card";
    if(check("zh-Hant","Cámaras","自訂電表","讀數")) return 3;
    return check("es","Cámaras","Multímetro","Lectura");
}
''', encoding='utf-8')
            card = tmp / 'card/zh-Hant'
            card.mkdir(parents=True)
            (card / 'aos.estado.lang').write_text('Cámaras\tCámaras\n', encoding='utf-8')
            (card / 'aos.plotter.lang').write_text('Multímetro\t自訂電表\n', encoding='utf-8')
            binary = tmp / ('exercise.exe' if os.name == 'nt' else 'exercise')
            env = os.environ.copy()
            env['PATH'] = str(Path(HOST_CC).parent) + os.pathsep + env['PATH']
            command = [HOST_CC, '-DAOS_SIM', '-I' + folder, '-I' + str(ui / 'include'),
                       str(ui / 'aos_i18n.c'), str(ui / 'aos_lang_embedded.c'),
                       'ec_i18n.c', 'pl_i18n.c', 'exercise.c', '-o', str(binary), '-lz']
            for cmd in (command, [str(binary)]):
                result = subprocess.run(cmd, cwd=tmp, env=env, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
