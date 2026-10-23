"""Run small production-C regression harnesses on the project's RV32 compiler."""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run_c(test, source, *flags):
    cc = shutil.which('riscv32-esp-elf-gcc')
    if not (cc and importlib.util.find_spec('unicorn') and importlib.util.find_spec('elftools')):
        test.skipTest('Need ESP-IDF compiler, unicorn and pyelftools')
    from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32
    from unicorn.riscv_const import UC_RISCV_REG_SP, UC_RISCV_REG_RA, UC_RISCV_REG_A0, UC_RISCV_REG_PC
    from elftools.elf.elffile import ELFFile
    with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
        binary = Path(directory) / 'test.elf'
        result = subprocess.run([cc, '-x', 'c', '-', '-O2', '-o', str(binary),
            '-march=rv32im', '-mabi=ilp32', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
            '-msmall-data-limit=0', '-nostdlib', '-Werror', *flags,
            '-Wl,--no-relax,-Ttext=0x10000,-e,test_main'], input=source, text=True, capture_output=True)
        test.assertEqual(result.returncode, 0, result.stderr)
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
        machine.emu_start(entry, 0x80000, count=1000000)
        test.assertEqual(machine.reg_read(UC_RISCV_REG_PC), 0x80000, 'harness did not terminate')
        test.assertEqual(machine.reg_read(UC_RISCV_REG_A0), 0, 'C regression case failed (return code)')
