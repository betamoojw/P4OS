"""Catalog extraction for the turtle's runtime-loaded level prose."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gen_lang', ROOT / 'tools/gen_lang.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class TurtleLevelCatalog(unittest.TestCase):
    def test_prose_escaping_and_executable_sections(self):
        # These values are read directly by the C .tl parser; only prose is
        # catalog text. Literal quotes/backslashes need catalog escaping.
        content = (
            '@-- @titulo es: comment\n'
            '@nivel 1-1\n'
            '@titulo es: El "camino"\\nuevo  \r\n'
            '@titulo en: The path\n'
            '@titulo de: Der Weg\n'
            '@carta  es:  Hola\t \n'
            '@pista es: Usa forward 2\n'
            '@idea es: Aprende a girar\n'
            '@mapa\n#>..M#\n'
            '@logo\nforward 2\nturn right\n'
            '@lua\nforward(2)\n'
            '@python\nforward(2)\n'
            '@titulo es: El "camino"\\nuevo\n'
        )
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
            path = Path(directory) / 'sample.tl'
            path.write_text(content, encoding='utf-8', newline='')
            entries = generator.scan_turtle_levels(path)
        self.assertEqual(list(entries), [
            'El \\"camino\\"\\\\nuevo', 'Hola', 'Usa forward 2', 'Aprende a girar',
        ])
        self.assertEqual(len(entries['El \\"camino\\"\\\\nuevo']), 2)
        self.assertTrue(entries['Hola'][0].endswith(':6'))


if __name__ == '__main__':
    unittest.main()
