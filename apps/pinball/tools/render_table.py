#!/usr/bin/env python3
"""
PINBALL - a table's renders: paints its playfield and backboard, writes
the scene for Blender, and runs tools/blender/build_table.py into
assets/<id>/. Then pack_p4.py makes the pack.

    python3 apps/pinball/tools/render_table.py zombis [--samples 96] [--only base,lamps,drops,flippers,spinner,ball,icons]
    python3 apps/pinball/tools/render_table.py --all

About 6-10 minutes a table on an M2 (Cycles on Metal).
"""
import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paint  # noqa: E402
import pbtable as pt  # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(TOOLS)
BLENDER = os.environ.get('BLENDER', '/Applications/Blender.app/Contents/MacOS/Blender')


def run(tid, samples, only, blend):
    t = pt.load(tid)
    out = os.path.join(APP, 'assets', tid)
    tex = os.path.join(out, 'tex')
    os.makedirs(tex, exist_ok=True)
    paint.paint_playfield(t).save(os.path.join(tex, 'playfield.png'))
    paint.paint_backboard(t).save(os.path.join(tex, 'backboard.png'))
    sc = t.scene()
    for v in sc['vis']:
        if v['t'] == 'insert':
            v['outline'] = paint.lamp_shape(v)
    sp = os.path.join(out, 'scene.json')
    json.dump(sc, open(sp, 'w'), indent=1)
    cmd = [BLENDER, '-b', '--factory-startup', '--python-exit-code', '1', '-P', os.path.join(TOOLS, 'blender', 'build_table.py'), '--',
           '--scene', sp, '--tex', tex, '--out', out, '--samples', str(samples)]
    if only:
        cmd += ['--only', only]
    if blend:
        cmd += ['--blend', os.path.join(out, tid + '.blend')]
    t0 = time.time()
    log = open(os.path.join(out, 'blender.log'), 'w')
    p = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
    print('%s: Blender %s in %.0f s (log: %s)' % (tid, 'ok' if p.returncode == 0 else 'FAILED', time.time() - t0,
                                                   log.name))
    return p.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ids', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--samples', type=int, default=96)
    ap.add_argument('--only', default='')
    ap.add_argument('--blend', action='store_true', help='also save the .blend, to look at it')
    a = ap.parse_args()
    ids = pt.all_ids() if a.all or not a.ids else a.ids
    ok = all([run(t, a.samples, a.only, a.blend) for t in ids])
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
