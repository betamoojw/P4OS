"""Compile the public geometry contract without ESP-IDF or LVGL.

Run with CC pointing to a C compiler, e.g. riscv32-esp-elf-gcc.
"""
import os
from pathlib import Path
import shutil
import subprocess
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")


@unittest.skipUnless(CC, "set CC to a C compiler")
class BoardGeometry(unittest.TestCase):
    def compile(self, source, *flags, succeeds=True):
        result = subprocess.run(
            [CC, "-std=c11", "-fsyntax-only", "-DAOS_SIM",
             "-I" + str(ROOT / "components/aos_hal/include"), *flags, "-x", "c", "-"],
            input=source, capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, succeeds, result.stderr)

    def test_native_profiles(self):
        for board, width, rate in ((5, 720, 700), (7, 720, 1000),
                                   (8, 800, 1500), (101, 800, 1500)):
            with self.subTest(board=board):
                self.compile(f'''
#include <stddef.h>
#include "aos_hal.h"
_Static_assert(AOS_PANEL_W == {width}, "native width");
_Static_assert(AOS_PANEL_H == 1280, "native height");
_Static_assert(AOS_DSI_MBPS == {rate}, "lane rate");
static unsigned short framebuffer[AOS_PANEL_W * AOS_PANEL_H];
''', f"-DAOS_BOARD_ID={board}")

    def test_default_and_invalid(self):
        self.compile('#include "aos_board.h"\n_Static_assert(AOS_BOARD_ID == 5, "default");')
        self.compile('#include "aos_board.h"', "-DAOS_BOARD_ID=9", succeeds=False)

    def test_firmware_advice_matches_every_build_profile(self):
        source = (ROOT / 'components/aos_portal/aos_portal_system.c').read_text(encoding='utf-8')
        advice = source.split('#if defined(CONFIG_IDF_TARGET_ESP32P4)', 1)[1].split('#ifdef ESP_PLATFORM', 1)[0]
        for board, folder in ((5, 'lcd5'), (7, 'lcd7'), (8, 'lcd8'), (101, 'lcd10_1')):
            for rev, profile in ((100, 'rev1_3'), (300, 'rev3_x')):
                code = '#include "aos_board.h"\n#if defined(CONFIG_IDF_TARGET_ESP32P4)' + advice
                result = subprocess.run([CC, '-E', '-P', '-x', 'c', '-',
                    '-I' + str(ROOT / 'components/aos_hal/include'), '-DCONFIG_IDF_TARGET_ESP32P4=1',
                    f'-DAOS_BOARD_ID={board}', f'-DCONFIG_ESP32P4_REV_MIN_FULL={rev}'],
                    input=code, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                literals = re.findall(r'"([^"]*)"', result.stdout)
                self.assertEqual(''.join(literals[1:]), f'build/{folder}/{profile}/p4os.bin')

    def test_dynamic_apps_use_host_stride_in_every_rotation(self):
        # Deliberately compile without a board override: an app built on LCD-5
        # must obtain 800 from an LCD-8 host, including when it is landscape.
        for width in (720, 800):
            for rotation in (0, 90, 180, 270):
                w, h = (1280, width) if rotation % 180 else (width, 1280)
                with self.subTest(width=width, rotation=rotation):
                    self.compile(f'''
#include <stddef.h>
#include "aos_hal.h"
#define aos_hal_screen_w() {w}
#define aos_hal_screen_h() {h}
#define aos_hal_display_get_rotation() {rotation}
#include "gf_gfx.h"
_Static_assert(AOS_PANEL_W == {width}, "host native stride");
_Static_assert(AOS_PANEL_W * AOS_PANEL_H * 2 == {width * 1280 * 2}, "frame bytes");
_Static_assert(GF_MAXPX == {width * 1280}, "golf canvas capacity");
''', "-DAOS_DYNAMIC_APP=1", "-I" + str(ROOT / "apps/golf/main"))

    def test_header_conversion(self):
        for board in (5, 7, 8, 101):
            checks = []
            for pin in range(1, 41):
                expected = pin if board == 5 else (40 - pin if pin % 2 else 42 - pin)
                checks.append(f'_Static_assert(AOS_HEADER_PIN({pin}) == {expected}, "pin {pin}");')
            self.compile('#include "aos_board.h"\n' + '\n'.join(checks), f'-DAOS_BOARD_ID={board}')


class HeaderTables(unittest.TestCase):
    def test_physical_order_and_power(self):
        source = (ROOT / 'components/aos_io/aos_io.c').read_text(encoding='utf-8')
        tables = re.findall(r'HEADER\[40\] = \{(.*?)\n\};', source, re.S)
        self.assertEqual(len(tables), 2)
        parsed = []
        for table in tables:
            rows = re.findall(r'\{\s*(-?\d+),\s*(\d+),[^\n]*?"([^"]+)"', table)
            self.assertEqual([int(p) for _, p, _ in rows], list(range(1, 41)))
            parsed.append({int(p): (int(g), label) for g, p, label in rows})
        old, new = parsed
        self.assertEqual(old[1], (-1, '5V'))
        self.assertEqual(new[1], (48, 'GPIO48'))
        self.assertEqual(new[39], (-1, '5V'))
        self.assertEqual(new[15], (-1, 'USB D-'))
        self.assertEqual(new[13], (-1, 'USB D+'))
        for pin, value in old.items():
            self.assertEqual(new[40 - pin if pin % 2 else 42 - pin], value)


if __name__ == "__main__":
    unittest.main()
