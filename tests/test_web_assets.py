"""Verify the shipped, gzip-compressed language catalogues without a device."""
import gzip
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import importlib.util

ROOT = Path(__file__).resolve().parents[1]


class WebAssets(unittest.TestCase):
    def test_closed_network_page_uses_localized_plain_text(self):
        source = (ROOT / 'components/aos_portal/aos_portal_access.c').read_text(encoding='utf-8')
        for key in (
            'Portal cerrado en esta red',
            'Esta red no está marcada como de confianza en la placa.',
            'Entrá por la red propia de la placa o por el cable USB, o habilitá el acceso con contraseña en Ajustes, Portal web.',
        ):
            self.assertIn(f'_("{key}")', source)
        self.assertIn('"text/plain; charset=utf-8"', source)
        self.assertNotIn('static const char CLOSED_PAGE', source)

    def test_portal_catalogues_cover_all_marked_text(self):
        spec = importlib.util.spec_from_file_location('gen_lang', ROOT / 'tools/gen_lang.py')
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        keys = generator.scan_js(str(ROOT / 'components/aos_portal/web/app.js'))
        for code in ('en', 'de', 'zh-Hans', 'zh-Hant'):
            with self.subTest(language=code):
                entries = generator.read_pack(str(ROOT / 'lang' / code / '_portal.lang'))
                self.assertFalse(set(keys) - set(entries))

    def test_embedded_catalogues_round_trip_all_translations_with_shared_keys(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
            output = Path(directory) / 'assets.c'
            subprocess.run([sys.executable, str(ROOT / 'tools/gen_web_assets.py'),
                            str(output), str(ROOT / 'components/aos_portal/web/app.js')], check=True)
            text = output.read_text()
        arrays = {name: bytes(map(int, data.split(','))) for name, data in
                  re.findall(r'uint8_t (a\d+)\[\d+\] = \{([^}]+)\}', text)}
        routes = re.findall(r'\{ "([^"]+)", "[^"]+", (a\d+), \d+ \}', text)
        self.assertEqual(len(routes), len(dict(routes)))
        routes = dict(routes)
        catalogs = sorted((ROOT / 'lang').glob('*/_portal.lang')) + sorted((ROOT / 'lang').glob('*/web.*.lang'))
        self.assertTrue(catalogs)
        for file in catalogs:
            with self.subTest(file=file):
                expected = {}
                for line in file.read_text(encoding='utf-8').split('\n'):
                    if not line.startswith('#') and '\t' in line:
                        key, value = line.split('\t', 1)
                        if key and value:
                            expected[key] = value
                keys = '/builtin-lang/keys/' + file.name
                values = '/builtin-lang/values/' + file.parent.name + '/' + file.name
                keys = gzip.decompress(arrays[routes[keys]]).decode().split('\n')
                values = gzip.decompress(arrays[routes[values]]).decode().split('\n')
                self.assertEqual(len(keys), len(values))
                self.assertEqual({k: v for k, v in zip(keys, values) if v}, expected)
        self.assertEqual(len([r for r in routes if r.startswith('/builtin-lang/keys/')]),
                         len({p.name for p in catalogs}))


if __name__ == '__main__':
    unittest.main()
