#!/usr/bin/env python3
"""Build profile-isolated ABI apps and validate them against linked firmware.

Run from an ESP-IDF environment. --all-profiles --all-boards checks the entire
hardware matrix. Outputs: apps/<app>/build/<profile>/ and build/apps-abi101/.
No files are installed on a board.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zipfile

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ('rev1_3', 'rev3_x')
BOARDS = ('lcd5', 'lcd7', 'lcd8', 'lcd10_1')
ABI = int(re.search(r'#define AOS_ABI_VERSION\s+(\d+)',
                   (ROOT / 'components/aos_ui/include/aos_app.h').read_text()).group(1))


def config(path):
    text = path.read_text()
    result = dict(re.findall(r'^(CONFIG_\w+)=(.*)$', text, re.M))
    result.update((k, 'n') for k in re.findall(r'^# (CONFIG_\w+) is not set$', text, re.M))
    return result


def lv_config(cfg):
    return {k: v for k, v in cfg.items() if k.startswith('CONFIG_LV_')}


def lv_version(build):
    description = json.loads((build / 'project_description.json').read_text())
    directory = Path(description['build_component_info']['lvgl__lvgl']['dir'])
    header = (directory / 'lv_version.h').read_text()
    return tuple(int(re.search(r'#define LVGL_VERSION_' + part + r'\s+(\d+)', header).group(1))
                 for part in ('MAJOR', 'MINOR', 'PATCH'))


def elf_at(elf, address, size=None):
    if not hasattr(elf, '_address_sections'):
        elf._address_sections = [(s['sh_addr'], s['sh_size'], s.data()) for s in elf.iter_sections()
                                 if s['sh_type'] != 'SHT_NOBITS' and s['sh_flags'] & 2]
    for start, length, data in elf._address_sections:
        if start <= address < start + length:
            offset = address - start
            return data[offset:] if size is None else data[offset:offset + size]
    raise ValueError(f'ELF address not found: {address:#x}')


def firmware_exports(path):
    """Read the actual runtime resolver tables, not just a source-code list."""
    elf = ELFFile(io.BytesIO(path.read_bytes()))
    symbols = elf.get_section_by_name('.symtab')
    exports = set()
    for name in ('aos_symbol_table', 'g_esp_libc_elfsyms', 'g_esp_espidf_elfsyms'):
        found = symbols.get_symbol_by_name(name)
        if not found:
            raise ValueError(f'{path}: missing resolver table {name}')
        table = found[0]
        data = elf_at(elf, table['st_value'], table['st_size'])
        for offset in range(0, len(data), 8):
            key, value = struct.unpack_from('<II', data, offset)
            if key and value:
                exports.add(elf_at(elf, key, 512).split(b'\0', 1)[0].decode())
    return exports


def firmware_dir(board, profile):
    candidates = [ROOT / 'build' / board / profile]
    if board == 'lcd5':
        candidates.insert(0, ROOT / 'build' / profile)
    for folder in candidates:
        if (folder / 'p4os.elf').is_file() and (folder / 'sdkconfig').is_file():
            return folder
    raise ValueError(f'Missing firmware for {board}/{profile}; build it first')


def tool(name):
    found = shutil.which('riscv32-esp-elf-' + name)
    if not found:
        raise ValueError('Export ESP-IDF first: riscv32-esp-elf-' + name + ' not found')
    return found


def validate_so(path, hosts, module=False):
    data = path.read_bytes()
    elf = ELFFile(io.BytesIO(data))
    if elf.elfclass != 32 or elf['e_machine'] != 'EM_RISCV' or elf['e_type'] != 'ET_DYN':
        raise ValueError(f'{path}: expected ELF32 RISC-V shared object')
    if elf['e_flags'] & 6 != 2:
        raise ValueError(f'{path}: not single-float ABI (ilp32f)')
    dyn = elf.get_section_by_name('.dynsym')
    if dyn is None:
        raise ValueError(f'{path}: missing dynamic symbol table')
    undefined = {s.name for s in dyn.iter_symbols() if s.name and s['st_shndx'] == 'SHN_UNDEF'}
    entry = 'emu_module_abi' if module else 'aos_app_abi'
    init = 'emu_module_core' if module else 'aos_app_init'
    for name in (entry, init):
        syms = dyn.get_symbol_by_name(name)
        if not syms or syms[0]['st_shndx'] == 'SHN_UNDEF' or syms[0]['st_info']['type'] != 'STT_FUNC':
            raise ValueError(f'{path}: missing function {name}')
    disasm = subprocess.check_output([tool('objdump'), '-d', '--disassemble=' + entry, str(path)], text=True)
    match = re.search(r'\b(?:c\.)?li\s+a0,\s*(-?\d+)', disasm)
    expected = ABI
    if module:
        expected = int(re.search(r'#define EMU_CORE_ABI\s+(\d+)',
                                (ROOT / 'apps/emulador/main/emu_core.h').read_text()).group(1))
    if not match or int(match.group(1)) != expected:
        raise ValueError(f'{path}: cannot confirm {entry} == {expected}')
    for board, host in hosts.items():
        missing = undefined - host['exports']
        if missing:
            raise ValueError(f'{path}: {board} does not export {sorted(missing)}')
    for sec in elf.iter_sections():
        if isinstance(sec, RelocationSection) and sec['sh_flags'] & 2:
            unsupported = {r['r_info_type'] for r in sec.iter_relocations()} - {0, 1, 3, 5}
            if unsupported:
                raise ValueError(f'{path}: loader cannot handle relocations {unsupported}')
    for local in (str(ROOT), ROOT.as_posix(), str(Path.home()), os.environ.get('IDF_PATH', '')):
        if local and local.encode() in data:
            raise ValueError(f'{path}: embeds local source path {local}')
    return dict(file=str(path.relative_to(ROOT)), bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                abi=expected, imports=len(undefined), boards=list(hosts), module=module)


def build_app(app, profile, hosts, output, check_only=False):
    folder = ROOT / 'apps' / app
    build = folder / 'build' / profile
    if not check_only:
        build.mkdir(parents=True, exist_ok=True)
    sync = output / 'lvgl.defaults'
    lv = lv_config(next(iter(hosts.values()))['config'])
    target = output / 'target.defaults'
    if not check_only:
        # Header dependencies are now tracked by project_so; removing the PIC
        # objects also guarantees a deliberate ABI rebuild of older checkouts.
        objects = (build / 'so_objs').resolve()
        if not objects.is_relative_to((folder / 'build').resolve()):
            raise ValueError('Unsafe object directory')
        if objects.exists():
            shutil.rmtree(objects)
        sdk = build / 'sdkconfig'
        if sdk.exists() and lv_config(config(sdk)) != lv:
            sdk.unlink()
        defaults = ';'.join(str(p) for p in (folder / 'sdkconfig.defaults', ROOT / f'sdkconfig.defaults.{profile}', target, sync))
        idf = Path(os.environ['IDF_PATH']) / 'tools/idf.py'
        cmd = [sys.executable, str(idf), '-B', str(build), '-D', 'IDF_TARGET=esp32p4',
               '-D', f'SDKCONFIG={sdk}', '-D', 'SDKCONFIG_DEFAULTS=' + defaults, 'reconfigure', 'so']
        with (build / 'idf_so.log').open('w', encoding='utf-8') as log:
            result = subprocess.run(cmd, cwd=folder, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            tail = (build / 'idf_so.log').read_text(encoding='utf-8', errors='replace').splitlines()[-14:]
            raise ValueError('\n'.join(tail))
    app_cfg = config(build / 'sdkconfig')
    if lv_config(app_cfg) != lv:
        raise ValueError(f'{app}/{profile}: LVGL configuration differs from firmware')
    expected_rev = '100' if profile == 'rev1_3' else '300'
    if app_cfg.get('CONFIG_ESP32P4_REV_MIN_FULL') != expected_rev:
        raise ValueError(f'{app}/{profile}: wrong silicon configuration')
    flags = (build / 'toolchain/cflags').read_text().strip()
    app_lv_version = lv_version(build)
    for host in hosts.values():
        if app_lv_version != host['lv_version']:
            raise ValueError(f'{app}/{profile}: LVGL version differs from firmware')
        if flags != (host['folder'] / 'toolchain/cflags').read_text().strip():
            raise ValueError(f'{app}/{profile}: instruction-set flags differ from firmware')
    so = build / (app + '.so')
    files = [validate_so(so, hosts)]
    for mod in sorted((build / 'modules').glob('*.so')):
        if (ROOT / 'apps' / mod.stem).is_dir():
            raise ValueError(f'{mod}: module name collides with an app')
        files.append(validate_so(mod, hosts, module=True))
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apps', nargs='*')
    parser.add_argument('--profile', choices=PROFILES, default=os.environ.get('PROFILE', 'rev1_3'))
    parser.add_argument('--board', choices=BOARDS, default=os.environ.get('AOS_BOARD', 'lcd5'))
    parser.add_argument('--all-profiles', action='store_true')
    parser.add_argument('--all-boards', action='store_true')
    parser.add_argument('--check-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=1, help='Independent app builds in parallel (default: 1)')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    profiles = PROFILES if args.all_profiles else (args.profile,)
    boards = BOARDS if args.all_boards else (args.board,)
    apps = list(dict.fromkeys(args.apps)) or sorted(p.name for p in (ROOT / 'apps').iterdir() if (p / 'main').is_dir())
    if any(not re.fullmatch(r'[a-z0-9_]+', a) or not (ROOT / 'apps' / a / 'main').is_dir() for a in apps):
        parser.error('Unknown app name')
    failed = 0
    for profile in profiles:
        output = ROOT / 'build' / f'apps-abi{ABI}' / profile
        output.mkdir(parents=True, exist_ok=True)
        suffix = '-selected' if args.apps else ''
        report_path = output / f'report{suffix}.json'
        archive_path = output / f'apps-abi{ABI}-{profile}{suffix}.zip'
        archive_path.unlink(missing_ok=True)
        hosts = {}
        for board in boards:
            fw = firmware_dir(board, profile)
            hosts[board] = dict(folder=fw, config=config(fw / 'sdkconfig'), lv_version=lv_version(fw),
                                exports=firmware_exports(fw / 'p4os.elf'))
            cfg = hosts[board]['config']
            if cfg.get('CONFIG_IDF_TARGET') != '"esp32p4"' or cfg.get('CONFIG_ESP32P4_REV_MIN_FULL') != ('100' if profile == 'rev1_3' else '300'):
                raise ValueError(f'{fw}: firmware target/silicon configuration does not match {profile}')
        first_lv = lv_config(next(iter(hosts.values()))['config'])
        if any(lv_config(h['config']) != first_lv for h in hosts.values()):
            raise ValueError('Firmware LVGL layouts differ across boards; cannot share apps')
        firmware_records = {board: dict(
            elf=str(h['folder'].relative_to(ROOT) / 'p4os.elf'),
            sha256=hashlib.sha256((h['folder'] / 'p4os.elf').read_bytes()).hexdigest(),
            lvgl_version=h['lv_version'],
            compiler_flags=(h['folder'] / 'toolchain/cflags').read_text().splitlines())
            for board, h in hosts.items()}
        report = dict(abi=ABI, profile=profile, boards=list(boards), firmware=firmware_records, requested_apps=apps,
                      apps={}, hardware_tested=False)
        # Write shared defaults before workers start; each worker owns its app's
        # build directory. Silicon profiles remain sequential because an app's
        # managed_components directory is shared between its profile builds.
        if not args.check_only:
            (output / 'lvgl.defaults').write_text(''.join(f'{k}={v}\n' for k, v in sorted(first_lv.items())))
            (output / 'target.defaults').write_text('CONFIG_IDF_TARGET="esp32p4"\n')
        def run(app):
            action = 'CHECK' if args.check_only else 'BUILD'
            print(f'{action} {profile} {app}', flush=True)
            return build_app(app, profile, hosts, output, args.check_only)
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            pending = {pool.submit(run, app): app for app in apps}
            for future in as_completed(pending):
                app = pending[future]
                try:
                    files = future.result()
                    report['apps'][app] = dict(status='passed', files=files)
                    print(f'PASS {profile} {app} ({len(files)} ELF files)', flush=True)
                except (ValueError, OSError, subprocess.SubprocessError) as exc:
                    report['apps'][app] = dict(status='failed', error=str(exc))
                    failed += 1
                    print(f'FAIL {profile} {app}: {exc}', flush=True)
                report_path.write_text(json.dumps(report, indent=2) + '\n')
        if all(a['status'] == 'passed' for a in report['apps'].values()):
            with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED) as archive:
                for app, result in report['apps'].items():
                    for file in result['files']:
                        path = ROOT / file['file']
                        dest = f'apps/{app}/{path.name}' if file['module'] else f'apps/{path.name}'
                        archive.write(path, dest)
                archive.write(report_path, 'build-report.json')
    print(f'Finished: {failed} failed app/profile builds. Nothing installed.', flush=True)
    return bool(failed)


if __name__ == '__main__':
    sys.exit(main())
