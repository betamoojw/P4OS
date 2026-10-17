#!/usr/bin/env python3
"""
PINBALL - one pack per table: build/pinball_<id>_p4.pak, which goes to
/apps on the card next to pinball.so.

    # the renders (tools/blender/build_table.py, README) then the packs
    python3 apps/pinball/tools/pack_p4.py                # every table
    python3 apps/pinball/tools/pack_p4.py zombis casino  # these
    python3 apps/pinball/tools/pack_p4.py --flat zombis  # without Blender

With --flat the art is drawn by flat.py (the geometry through the game's
camera, flat colours): for trying a table before its renders exist.
Otherwise it reads the renders from assets/<id>/ (build_table.py --out).

What a pack holds (pb_pak.h has the types):
    table           the definition (pbtable.Table.def_text)
    bg, depth       the table's picture and the depth of every pixel
    dmd             the DMD's surround, 720 x 160
    L_<lamp>        the light each lamp adds
    D_<target>      a drop target down: pixels and depth
    F0_nn, F1_nn    the flippers' frames, rest to up
    S_nn            the spinner's frames
    ball            the chrome ball
    launch, launch_on, pause, icon   the table's own buttons
    logo_es/en/de   its name, drawn
    thumb           the carousel's card
    s_<sound>, music
"""
import argparse
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packlib  # noqa: E402
import paint  # noqa: E402
import pbtable as pt  # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(TOOLS)
TW, TH = pt.SCREEN_W, pt.SCREEN_H - pt.DMD_H
THUMB = (432, 768)
LAUNCH_C = (624, 1140)      # the launch button's centre on the screen
LAUNCH_D = 150
PAUSE_XY = (12, 34)
PAUSE_D = 88


def rgb(c, k=1.0):
    return paint.rgb(c, k)


def logo(t, lang, width=600):
    """the table's name as a logo: big letters with an outline and a glow
    in the theme's colours"""
    name = t.names.get(lang) or t.names['es']
    top, bot = t.theme.get('logo', (t.theme['accent'], t.theme['bg']))
    size = 96
    f = paint.font(size)
    while True:
        bb = f.getbbox(name, stroke_width=6)
        if bb[2] - bb[0] <= width - 40 or size < 40:
            break
        size -= 4
        f = paint.font(size)
    w, h = bb[2] - bb[0] + 40, bb[3] - bb[1] + 40
    mask = Image.new('L', (w, h), 0)
    ImageDraw.Draw(mask).text((20 - bb[0], 20 - bb[1]), name, font=f, fill=255)
    stroke = Image.new('L', (w, h), 0)
    ImageDraw.Draw(stroke).text((20 - bb[0], 20 - bb[1]), name, font=f, fill=255, stroke_width=6, stroke_fill=255)
    glow = stroke.filter(ImageFilter.GaussianBlur(10))
    grad = paint.gradient(w, h, rgb(top, 1.25), rgb(top, 0.75))
    img = Image.new('RGBA', (w, h), rgb(top) + (0,))
    gl = Image.new('RGBA', (w, h), rgb(top) + (255,))
    img.paste(gl, (0, 0), glow.point(lambda v: v * 0.8))
    img.paste(Image.new('RGBA', (w, h), rgb(bot, 0.6) + (255,)), (0, 0), stroke)
    face = grad.convert('RGBA')
    img.paste(face, (0, 0), mask)
    return img


def dmd_surround(t):
    """the DMD's frame: dark metal, the theme's colour round the glass"""
    img = paint.gradient(pt.SCREEN_W, pt.DMD_H, (34, 34, 38), (12, 12, 14))
    d = ImageDraw.Draw(img)
    acc = rgb(t.theme['accent'])
    d.rounded_rectangle([104, 1, 718, 158], radius=14, fill=(4, 4, 4), outline=tuple(int(c * 0.8) for c in acc),
                        width=3)
    d.line([(0, 159), (720, 159)], fill=tuple(int(c * 0.6) for c in acc), width=2)
    return img


def compose_screen(dmd, bg, art):
    """the whole screen as a game starts: for the carousel's card"""
    scr = Image.new('RGB', (pt.SCREEN_W, pt.SCREEN_H))
    scr.paste(dmd, (0, 0))
    scr.paste(bg, (0, pt.DMD_H))
    for ti, frames in sorted(art.get('toys', {}).items()):
        if frames:
            im, z, x, y = frames[0]
            scr.paste(im, (x, y), im)
    for side in sorted(art['flippers']):
        fr = art['flippers'][side]
        if fr:
            img, x, y = fr[0] if isinstance(fr[0], tuple) else (fr[0], 0, pt.DMD_H)
            scr.paste(img, (x, y), img)
    return scr


def add_sprite_list(pk, prefix, frames):
    for i, fr in enumerate(frames):
        if isinstance(fr, tuple):
            img, x, y = fr
        else:
            img, x, y = fr, 0, pt.DMD_H
        pk.sprite('%s%02d' % (prefix, i), img, x, y)


def load_renders(t, d):
    """the renders build_table.py left in d (its meta.json says what)"""
    meta = json.load(open(os.path.join(d, 'meta.json')))

    def img(name):
        return Image.open(os.path.join(d, name))
    art = dict(bg=img('base.png').convert('RGB'))
    art['depth'] = np.load(os.path.join(d, 'depth.npy'))
    base = np.asarray(art['bg']).astype(np.int32)
    art['lamps'] = {}
    for name, m in meta.get('lamps', {}).items():
        on = np.asarray(img(m['file']).convert('RGB')).astype(np.int32)
        x, y = m['x'], m['y']
        h, w = on.shape[:2]
        # lit minus unlit of the same crop: the renders' noise cancels
        off = np.asarray(img(m['off']).convert('RGB')).astype(np.int32) if 'off' in m else base[y:y + h, x:x + w]
        light = np.clip(on - off, 0, 255)
        # a little bloom round what lit up
        bl = Image.fromarray(np.clip(light * 1.0, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(6))
        light = np.clip(light + np.asarray(bl).astype(np.int32) * 0.7, 0, 255)
        full = np.zeros((TH, TW, 3), np.int32)
        full[y:y + h, x:x + w] = light
        art['lamps'][name] = full
    art['drops'] = {}
    for name, m in meta.get('drops', {}).items():
        down = np.asarray(img(m['file']).convert('RGB')).astype(np.int32)
        z = np.load(os.path.join(d, m['depth']))
        x, y = m['x'], m['y']
        h, w = down.shape[:2]
        diff = np.abs(down - base[y:y + h, x:x + w]).max(axis=2) > 6
        diff |= np.abs(z - art['depth'][y:y + h, x:x + w]) > 2
        diff = np.asarray(Image.fromarray((diff * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(5))) > 0
        art['drops'][name] = (down.astype(np.uint8), z, diff, x, y)
    art['flippers'] = {}
    for s, frames in meta['flippers'].items():
        art['flippers'][int(s)] = [(img(f['file']).convert('RGBA'), f['x'], f['y'] + pt.DMD_H) for f in frames]
    # the toys: their frames with the depth of their own pixels; where only
    # their shadow is, the table's depth (it darkens the playfield, it hides
    # nothing)
    art['toys'] = {}
    for ti, frames in meta.get('toys', {}).items():
        out = []
        for f in frames:
            im = img(f['file']).convert('RGBA')
            z = np.load(os.path.join(d, f['depth'])).astype(np.float32)
            x, y = f['x'], f['y']
            h, w = z.shape
            under = art['depth'][y:y + h, x:x + w]
            shadow_only = z > 8000
            z = np.where(shadow_only, under, z)
            # the shadow catcher also catches a faint haze of the room's light:
            # only the toy's own shadow is kept
            a = np.asarray(im).copy()
            al = a[..., 3].astype(np.int32)
            al = np.where(shadow_only, np.clip((al - 40) * 1.4, 0, 255), al)
            a[..., 3] = al.astype(np.uint8)
            out.append((Image.fromarray(a), z, x, y + pt.DMD_H))
        art['toys'][int(ti)] = out
    art['spinner'] = [(img(f['file']).convert('RGBA'), f['x'], f['y'] + pt.DMD_H) for f in meta.get('spinner', [])]
    art['ball'] = img('ball.png').convert('RGBA') if os.path.exists(os.path.join(d, 'ball.png')) else None
    for k in ('launch', 'launch_on', 'pause', 'icon'):
        p = os.path.join(d, k + '.png')
        art[k] = Image.open(p).convert('RGBA') if os.path.exists(p) else None
    return art


def build(tid, flat, out_dir, sounds_on=True):
    t = pt.load(tid)
    pk = packlib.Pack()
    pk.text('table', t.def_text())
    if flat:
        import flat as flatmod
        art = flatmod.Flat(t).art()
    else:
        d = os.path.join(APP, 'assets', tid)
        if not os.path.exists(os.path.join(d, 'meta.json')):
            print('%s: no renders in %s (build_table.py), drawing it flat' % (tid, d))
            import flat as flatmod
            art = flatmod.Flat(t).art()
        else:
            art = load_renders(t, d)
    pk.rgb('bg', art['bg'], 0, pt.DMD_H)
    if art.get('depth') is not None:
        pk.depth('depth', art['depth'], 0, pt.DMD_H)
    dmd = dmd_surround(t)
    pk.rgb('dmd', dmd)
    for name, light in art['lamps'].items():
        pk.add_light('L_' + name, light, 0, pt.DMD_H)
    for name, (rgbimg, z, mask, x, y) in art['drops'].items():
        pk.patch('D_' + name, rgbimg, z, mask, x, y + pt.DMD_H)
    for i in sorted(art['flippers']):
        add_sprite_list(pk, 'F%d_' % i, art['flippers'][i])
    for ti, frames in sorted(art.get('toys', {}).items()):
        for k, (im, z, x, y) in enumerate(frames):
            pk.spritez('T%d_%02d' % (ti, k), im, z, x, y)
    add_sprite_list(pk, 'S_', art['spinner'])
    if art.get('ball') is not None:
        pk.sprite('ball', art['ball'], 0, 0, crop=False)
    for k, size, pos in (('launch', LAUNCH_D, (LAUNCH_C[0] - LAUNCH_D // 2, LAUNCH_C[1] - LAUNCH_D // 2)),
                         ('launch_on', LAUNCH_D, (LAUNCH_C[0] - LAUNCH_D // 2, LAUNCH_C[1] - LAUNCH_D // 2)),
                         ('pause', PAUSE_D, PAUSE_XY)):
        im = art.get(k)
        if im is None:
            continue
        im = im.resize((size, size), Image.LANCZOS)
        pk.sprite(k, im, pos[0], pos[1], crop=False)
    if art.get('icon') is not None:
        pk.sprite('icon', art['icon'].resize((96, 96), Image.LANCZOS), 0, 0, crop=False)
    for lang in ('es', 'en', 'de'):
        pk.sprite('logo_' + lang, logo(t, lang), 0, 0, crop=False)
    scr = compose_screen(dmd, art['bg'], art)
    pk.rgb('thumb', scr.resize(THUMB, Image.LANCZOS))
    if sounds_on:
        try:
            import sounds
            snd = sounds.make(t)
            for name, data in snd.items():
                pk.pcm(name, data)
        except ImportError:
            pass
    path = os.path.join(out_dir, 'pinball_%s_p4.pak' % tid)
    size = pk.write(path)
    raw = sum(e['raw'] for e in pk.entries)
    print('%-10s %6.2f MB (%5.1f MB unpacked, %d entries) -> %s' % (tid, size / 1e6, raw / 1e6, len(pk.entries), path))
    if size > 8_000_000:
        print('   over 8 MB: install_apps.sh would want it in parts')
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ids', nargs='*')
    ap.add_argument('--flat', action='store_true')
    ap.add_argument('--out', default=os.path.join(APP, 'build'))
    ap.add_argument('--no-sound', action='store_true')
    a = ap.parse_args()
    ids = a.ids or pt.all_ids()
    for tid in ids:
        build(tid, a.flat, a.out, not a.no_sound)


if __name__ == '__main__':
    main()
