"""Offline language, formatting, and Flash font-resource contracts."""
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import gen_lang

FORMAT = re.compile(r'%(?:\d+\$)?[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:hh|ll|[hlzjtL])?[diuoxXfFeEgGaAcsp%](?=$|[^a-z]|x%)')

class Chinese(unittest.TestCase):
    def test_catalogue_coverage_and_format_arguments(self):
        catalogs = gen_lang.collect()
        for code in ('zh-Hans', 'zh-Hant'):
            for name, keys in catalogs.items():
                entries = gen_lang.read_pack(str(ROOT / 'lang' / code / name))
                self.assertEqual(set(keys), set(entries), (code, name))
                english = gen_lang.read_pack(str(ROOT / 'lang/en' / name))
                for key, value in entries.items():
                    self.assertTrue(value.strip(), (code, name, key))
                    self.assertEqual(FORMAT.findall(key), FORMAT.findall(value), (code, name, key, value))
                    self.assertEqual(FORMAT.findall(english[key]), FORMAT.findall(value), (code, name, key, value))
                    self.assertEqual(english[key].count(r'\n'), value.count(r'\n'), (code, name, key))

    def test_font_integrity_and_all_chinese_characters(self):
        folder = ROOT / 'assets/cjk'
        data = (folder / 'cjk.bin').read_bytes()
        manifest = json.loads((folder / 'manifest.json').read_text())
        self.assertEqual(hashlib.sha256(data).hexdigest(), manifest['sha256'])
        magic, version, size, count, length, crc, reserved = struct.unpack('<8s6I', data[:32])
        self.assertEqual((magic, version, size, reserved), (b'P4CJK001', 1, 48, 0))
        self.assertEqual(length, len(data)-32)
        self.assertEqual(zlib.crc32(data[32:]), crc)
        header = (ROOT / 'components/aos_fonts/include/aos_cjk_resource.h').read_text()
        self.assertEqual(int(re.search(r'AOS_CJK_RESOURCE_CRC 0x([0-9a-f]+)u', header)[1], 16), crc)
        self.assertLess(len(data), 12 * 1024 * 1024)
        stride = 4 + size * size // 4
        self.assertEqual(length, count * stride)
        keys = [struct.unpack_from('<I', data, 32+i*stride)[0] for i in range(count)]
        self.assertEqual(keys, sorted(set(keys)))
        for variant, code in enumerate(('zh-Hans', 'zh-Hant')):
            points = set(manifest['glyphs'][code])
            common = '赵钱孙李周吴郑王' if variant == 0 else '趙錢孫李周吳鄭王'
            self.assertFalse({ord(c) for c in common} - points)
            self.assertGreater(len(points), 6500 if variant == 0 else 13000)
            self.assertEqual(points, {key & 0xffffff for key in keys if key >> 24 == variant})
            for file in (ROOT / 'lang' / code).glob('*.lang'):
                for value in gen_lang.read_pack(str(file)).values():
                    self.assertFalse({ord(c) for c in value if (0x2e80 <= ord(c) < 0xe000 or 0xff00 <= ord(c) <= 0xffef)} - points, file)

    def test_embedded_native_catalogues_round_trip(self):
        source = (ROOT / 'components/aos_ui/aos_lang_embedded.c').read_text(encoding='utf-8')
        arrays = re.findall(r'unsigned char (\w+)\[\] = \{(.*?)\};', source, re.S)
        self.assertTrue(arrays)
        for name, body in arrays:
            packed = bytes(map(int, re.findall(r'\d+', body)))
            raw = zlib.decompress(packed)
            self.assertIn(b'\t', raw, name)
            raw.decode('utf-8')

if __name__ == '__main__':
    unittest.main()
