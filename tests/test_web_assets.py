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

    def test_embedded_catalogues_match_source_and_have_unique_routes(self):
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
            route = '/builtin-lang/' + file.parent.name + '/' + file.name
            with self.subTest(route=route):
                self.assertEqual(gzip.decompress(arrays[routes[route]]), file.read_bytes())
        self.assertIn('/builtin-lang/en/_portal.lang', routes)
        self.assertIn('/builtin-lang/de/_portal.lang', routes)


if __name__ == '__main__':
    unittest.main()
