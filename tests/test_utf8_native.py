"""Execute the production UTF-8 helpers on the host or an emulated RISC-V CPU."""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class Utf8Native(unittest.TestCase):
    def test_utf8_and_canvas_wrapping(self):
        host=shutil.which('cc') or shutil.which('gcc')
        risc=shutil.which('riscv32-esp-elf-gcc')
        if not host and not (risc and importlib.util.find_spec('unicorn') and importlib.util.find_spec('elftools')):
            self.skipTest('Need a host C compiler, or ESP-IDF plus unicorn and pyelftools')
        (ROOT/'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as directory:
            binary=Path(directory)/('utf8.exe' if host else 'utf8.elf')
            args=[host or risc,'-O2','-I'+str(ROOT/'components/aos_ui/include'),str(ROOT/'tests/utf8_cases.c'),'-o',str(binary)]
            if host:
                args+=['-DHOST_TEST']
            else:
                args+=['-march=rv32im','-mabi=ilp32','-ffreestanding','-fno-builtin','-fno-stack-protector',
                       '-fno-tree-loop-distribute-patterns','-msmall-data-limit=0','-nostdlib','-Wl,-Ttext=0x10000,-e,test_main']
            compiled=subprocess.run(args,capture_output=True,text=True)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            if host:
                result=subprocess.run([str(binary)])
                self.assertEqual(result.returncode,0,'UTF-8 C assertion failed; exit status is its source line')
            else:
                from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32
                from unicorn.riscv_const import UC_RISCV_REG_SP,UC_RISCV_REG_RA,UC_RISCV_REG_A0,UC_RISCV_REG_PC
                from elftools.elf.elffile import ELFFile
                machine=Uc(UC_ARCH_RISCV,UC_MODE_RISCV32)
                machine.mem_map(0x1000,0x100000)
                with binary.open('rb') as f:
                    elf=ELFFile(f)
                    for segment in elf.iter_segments():
                        if segment['p_type']=='PT_LOAD':
                            machine.mem_write(segment['p_vaddr'],segment.data())
                    entry=elf['e_entry']
                stop=0x80000
                machine.reg_write(UC_RISCV_REG_SP,0xf0000)
                machine.reg_write(UC_RISCV_REG_RA,stop)
                machine.emu_start(entry,stop,count=2000000)
                self.assertEqual(machine.reg_read(UC_RISCV_REG_PC),stop,'UTF-8 operation did not terminate')
                self.assertEqual(machine.reg_read(UC_RISCV_REG_A0),0,'UTF-8 C assertion failed; register a0 is its source line')

if __name__=='__main__':unittest.main()
