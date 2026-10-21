"""Exercise the app compatibility checker with real RISC-V shared objects.

Run in an activated ESP-IDF environment:
    python -m unittest discover -s tests -p test_dynamic_apps.py
"""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_apps', ROOT / 'tools/build_apps.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


@unittest.skipUnless(shutil.which('riscv32-esp-elf-gcc'), 'ESP-IDF compiler required')
class DynamicApps(unittest.TestCase):
    def setUp(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.hosts = {board: {'exports': {'host_call'}} for board in builder.BOARDS}

    def compile(self, abi=101, float_abi='ilp32f', init=True):
        source = self.folder / 'app.c'
        source.write_text(f'int aos_app_abi(void) {{ return {abi}; }}\n' +
                          ('extern void host_call(void);\n'
                           'void aos_app_init(void) { host_call(); }\n' if init else ''))
        output = self.folder / 'app.so'
        subprocess.run([builder.tool('gcc'), '-shared', '-fPIC', '-nostdlib', '-Os',
                        '-march=rv32imafc_zicsr_zifencei', '-mabi=' + float_abi,
                        '-Wl,--strip-all', str(source), '-o', str(output)], check=True,
                       capture_output=True)
        return output

    def test_current_abi_all_boards(self):
        result = builder.validate_so(self.compile(), self.hosts)
        self.assertEqual(result['abi'], 101)
        self.assertEqual(result['boards'], list(builder.BOARDS))
        self.assertEqual(result['imports'], 1)

    def test_old_abi_rejected(self):
        with self.assertRaisesRegex(ValueError, 'aos_app_abi == 101'):
            builder.validate_so(self.compile(abi=100), self.hosts)

    def test_wrong_float_abi_rejected(self):
        with self.assertRaisesRegex(ValueError, 'single-float ABI'):
            builder.validate_so(self.compile(float_abi='ilp32'), self.hosts)

    def test_one_board_missing_import_rejected(self):
        self.hosts['lcd10_1']['exports'] = set()
        with self.assertRaisesRegex(ValueError, 'lcd10_1 does not export'):
            builder.validate_so(self.compile(), self.hosts)

    def test_missing_entry_rejected(self):
        with self.assertRaisesRegex(ValueError, 'missing function aos_app_init'):
            builder.validate_so(self.compile(init=False), self.hosts)
