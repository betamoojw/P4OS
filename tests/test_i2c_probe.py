"""Fault-inject the production probe: absent chips are not timeout failures."""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class I2cProbe(unittest.TestCase):
    def test_bounded_timeout_recovery(self):
        cc = shutil.which('riscv32-esp-elf-gcc')
        if not (cc and importlib.util.find_spec('unicorn') and importlib.util.find_spec('elftools')):
            self.skipTest('Need ESP-IDF compiler, unicorn and pyelftools')
        source = (ROOT / 'components/aos_io/aos_io_p4.c').read_text(encoding='utf-8')
        probe = source[source.index('bool aos_io_be_i2c_probe('):source.index('bool aos_io_be_i2c_xfer(')]
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_NOT_FOUND, ESP_ERR_TIMEOUT };
typedef struct { int bus; } i2c_be_t;
typedef struct { int pins[2]; const char *name; } port_t;
typedef struct { void *be; port_t *port; } aos_io_i2c_t;
static int responses[2], calls, delays, logs, bad;
static int i2c_master_probe(int bus, uint8_t addr, int timeout) {
    if (bus != 123 || addr != 0x44 || timeout != 30 || calls >= 2) { bad = 1; return ESP_ERR_TIMEOUT; }
    return responses[calls++];
}
static int gpio_get_level(int pin) { if (pin != 21 && pin != 22) bad = 1; return 1; }
/* 100 Hz also exercises the nonzero-delay fallback. */
#define pdMS_TO_TICKS(ms) ((ms) / 10)
static void vTaskDelay(int ticks) { if (ticks <= 0) bad = 1; delays++; }
static const char *esp_err_to_name(int err) { (void)err; return "result"; }
static void aos_hal_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; logs++; }
'''
        cases = r'''
int test_main(void) {
    port_t port = {{21, 22}, "i2c.ext"}; i2c_be_t backend = {123}; aos_io_i2c_t bus = {&backend, &port};
    for (int first = ESP_OK; first <= ESP_ERR_TIMEOUT; first++) {
        for (int second = ESP_OK; second <= ESP_ERR_TIMEOUT; second++) {
            responses[0] = first; responses[1] = second; calls = delays = logs = bad = 0;
            bool result = aos_io_be_i2c_probe(&bus, 0x44);
            bool retry = first == ESP_ERR_TIMEOUT;
            if (result != ((retry ? second : first) == ESP_OK)) return 1;
            if (calls != (retry ? 2 : 1) || delays != retry || logs != retry || bad) return 2;
        }
    }
    return 0;
}
'''
        from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32
        from unicorn.riscv_const import UC_RISCV_REG_SP, UC_RISCV_REG_RA, UC_RISCV_REG_A0, UC_RISCV_REG_PC
        from elftools.elf.elffile import ELFFile
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
            binary = Path(directory) / 'probe.elf'
            result = subprocess.run([cc, '-x', 'c', '-', '-O2', '-o', str(binary),
                '-march=rv32im', '-mabi=ilp32', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                '-msmall-data-limit=0', '-nostdlib', '-Wl,-Ttext=0x10000,-e,test_main'],
                input=harness + probe + cases, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            machine = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
            machine.mem_map(0x1000, 0x100000)
            with binary.open('rb') as f:
                elf = ELFFile(f)
                for segment in elf.iter_segments():
                    if segment['p_type'] == 'PT_LOAD':
                        machine.mem_write(segment['p_vaddr'], segment.data())
                entry = elf['e_entry']
            machine.reg_write(UC_RISCV_REG_SP, 0xf0000)
            machine.reg_write(UC_RISCV_REG_RA, 0x80000)
            machine.emu_start(entry, 0x80000, count=200000)
            self.assertEqual(machine.reg_read(UC_RISCV_REG_PC), 0x80000, 'probe retry did not terminate')
            self.assertEqual(machine.reg_read(UC_RISCV_REG_A0), 0, 'probe result/retry contract failed')


if __name__ == '__main__':
    unittest.main()
