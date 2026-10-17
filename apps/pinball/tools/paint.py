"""
PINBALL - the playfield's printed art, painted with PIL from the table's
theme: the background, the theme's pictures, the lanes' and the inserts'
outlines. Blender lays it on the playfield (build_table.py); the flat
renderer (flat.py) warps it onto the screen.

Each theme has a painter here (THEMES); a table without one gets the
plain gradient. Everything is drawn in table millimetres times K.
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter, ImageFont

import pbtable as pt

K = 3   # pixels per millimetre of the texture


def rgb(c, k=1.0):
    return (min(255, int(((c >> 16) & 255) * k)), min(255, int(((c >> 8) & 255) * k)), min(255, int((c & 255) * k)))


def font_path():
    """an open font: DejaVu Sans (Bitstream Vera licence) or Montserrat
    (OFL), from the LVGL checkout the app builds against"""
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(os.path.dirname(here)))
    for app in ['pinball'] + sorted(os.listdir(os.path.join(root, 'apps'))):
        for f in ('Montserrat-Medium.ttf', 'DejaVuSans.ttf'):
            p = os.path.join(root, 'apps', app, 'managed_components', 'lvgl__lvgl', 'scripts', 'built_in_font', f)
            if os.path.exists(p):
                return p
    return None


def font(size, which='Montserrat-Medium.ttf'):
    p = font_path()
    if p and which:
        p2 = os.path.join(os.path.dirname(p), which)
        if os.path.exists(p2):
            p = p2
    try:
        return ImageFont.truetype(p, size) if p else ImageFont.load_default()
    except OSError:
        return ImageFont.load_default()


def P(x, y):
    return (x * K, y * K)


def gradient(w, h, top, bottom):
    img = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] * (1 - t) + bottom[i] * t) for i in range(3))
        d.line([(0, y), (w, y)], fill=c)
    return img


def star(cx, cy, r0, r1, n=5, rot=-90):
    pts = []
    for i in range(n * 2):
        r = r1 if i % 2 == 0 else r0
        a = math.radians(rot + i * 180 / n)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def lamp_shape(l, scale=1.0, grow=0.0):
    """the outline of an insert, table millimetres"""
    x, y, s = l['x'], l['y'], l['size'] * scale + grow
    a = math.radians(l.get('angle', 0.0))
    shape = l.get('shape', 'circle')

    def rot(pts):
        out = []
        for px, py in pts:
            out.append((x + px * math.cos(a) - py * math.sin(a), y + px * math.sin(a) + py * math.cos(a)))
        return out
    if shape == 'circle':
        return [(x + s / 2 * math.cos(i / 24 * 2 * math.pi), y + s / 2 * math.sin(i / 24 * 2 * math.pi))
                for i in range(24)]
    if shape == 'arrow':
        h = s * 1.3
        return rot([(0, -h / 2), (s / 2, h * 0.1), (s * 0.2, h * 0.1), (s * 0.2, h / 2), (-s * 0.2, h / 2),
                    (-s * 0.2, h * 0.1), (-s / 2, h * 0.1)])
    if shape == 'triangle':
        return rot([(0, -s / 2), (s / 2, s / 2), (-s / 2, s / 2)])
    if shape == 'rect':
        return rot([(-s / 2, -s * 0.35), (s / 2, -s * 0.35), (s / 2, s * 0.35), (-s / 2, s * 0.35)])
    if shape == 'star':
        return rot(star(0, 0, s * 0.22, s / 2))
    if shape == 'shield':
        return rot([(-s / 2, -s / 2), (s / 2, -s / 2), (s / 2, 0), (0, s / 2), (-s / 2, 0)])
    return lamp_shape(dict(l, shape='circle'), scale, grow)


# ---- the themes' pictures ---------------------------------------------------

def theme_zombis(t, img, d, rnd):
    # a night sky fading into the graveyard's grass, a fog band, the moon
    w, h = img.size
    sky = gradient(w, int(h * 0.55), (14, 10, 30), (30, 44, 30))
    img.paste(sky, (0, 0))
    grass = gradient(w, h - int(h * 0.55), (30, 44, 30), (10, 16, 10))
    img.paste(grass, (0, int(h * 0.55)))
    for i in range(300):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h * 0.45)
        r = rnd.choice([1, 1, 2, 3])
        d.ellipse([x - r, y - r, x + r, y + r], fill=(200, 200, 230))
    # crooked crosses and stones painted on the field
    for i in range(26):
        x, y = rnd.uniform(40, 470) * K, rnd.uniform(380, 900) * K
        s = rnd.uniform(10, 22) * K
        c = (54, 60, 54)
        if rnd.random() < 0.5:
            d.rectangle([x - s * 0.12, y - s, x + s * 0.12, y + s * 0.4], fill=c)
            d.rectangle([x - s * 0.45, y - s * 0.6, x + s * 0.45, y - s * 0.38], fill=c)
        else:
            d.rounded_rectangle([x - s * 0.4, y - s, x + s * 0.4, y + s * 0.3], radius=s * 0.35, fill=c)
    # the moon's halo
    halo = Image.new('L', img.size, 0)
    hd = ImageDraw.Draw(halo)
    hd.ellipse([P(404 - 120, 150 - 120)[0], P(404 - 120, 150 - 120)[1], P(404 + 120, 150 + 120)[0],
                P(404 + 120, 150 + 120)[1]], fill=90)
    halo = halo.filter(ImageFilter.GaussianBlur(40 * K))
    img.paste((180, 190, 120), (0, 0), halo)
    # green fog
    fog = Image.new('L', img.size, 0)
    fd = ImageDraw.Draw(fog)
    for i in range(40):
        x, y = rnd.uniform(0, w), rnd.uniform(h * 0.5, h * 0.95)
        fd.ellipse([x - 120 * K, y - 20 * K, x + 120 * K, y + 20 * K], fill=40)
    fog = fog.filter(ImageFilter.GaussianBlur(14 * K))
    img.paste((90, 200, 60), (0, 0), fog)


def theme_generic(t, img, d, rnd):
    w, h = img.size
    top, bottom = t.theme.get('field', (0x202030, 0x101018))
    img.paste(gradient(w, h, rgb(top), rgb(bottom)), (0, 0))


THEMES = {'zombis': theme_zombis}


def paint_playfield(t):
    w, h = int(pt.W * K), int(pt.H * K)
    img = Image.new('RGB', (w, h), (20, 20, 24))
    d = ImageDraw.Draw(img)
    rnd = random.Random(sum(map(ord, t.id)))
    THEMES.get(t.theme.get('art', t.id), theme_generic)(t, img, d, rnd)
    d = ImageDraw.Draw(img)
    acc = rgb(t.theme['accent'])
    dark = (10, 10, 12)
    # the plunger lane and the orbits: a darker channel
    d.rectangle([P(pt.LANE_X, 300), P(pt.W, pt.H)], fill=(24, 22, 20))
    # the lanes' outlines: thin painted lines along the walls
    for v in t.vis:
        if v['t'] == 'wall' and v['style'] in ('rail', 'plastic'):
            pts = [P(*p[:2]) for p in v['pts']]
            d.line(pts, fill=tuple(int(c * 0.5) for c in acc), width=int(2.2 * K), joint='curve')
    # the inserts' sockets: a dark rim the insert sits in
    for l in t.lamps:
        if l['kind'] != 'insert':
            continue
        out = [P(*p) for p in lamp_shape(l, grow=3.0)]
        d.polygon(out, fill=dark)
    # rollover slots, and the slots the drop targets sink into
    for v in t.vis:
        if v['t'] == 'rollover':
            x, y = P(v['x'], v['y'])
            d.rounded_rectangle([x - 3 * K, y - 9 * K, x + 3 * K, y + 9 * K], radius=3 * K, fill=dark)
        elif v['t'] == 'drop':
            a, b = v['a'], v['b']
            l = math.hypot(b[0] - a[0], b[1] - a[1])
            nx, ny = (b[1] - a[1]) / l * 4, -(b[0] - a[0]) / l * 4
            q = [P(a[0] + nx, a[1] + ny), P(b[0] + nx, b[1] + ny), P(b[0] - nx, b[1] - ny), P(a[0] - nx, a[1] - ny)]
            d.polygon(q, fill=(4, 4, 4))
    return img


# ---- the backboard: the picture over the top of the table ----------------------

def back_zombis(t, img, d, rnd):
    w, h = img.size
    img.paste(gradient(w, h, (8, 6, 22), (30, 40, 34)), (0, 0))
    for i in range(160):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h * 0.7)
        r = rnd.choice([1, 1, 2])
        d.ellipse([x - r, y - r, x + r, y + r], fill=(210, 210, 240))
    d.ellipse([w * 0.66, h * 0.08, w * 0.66 + h * 0.42, h * 0.5], fill=(240, 232, 170))
    # the graveyard's silhouette
    sil = (6, 8, 6)
    pts = [(0, h)]
    for i in range(0, w + 40, 40):
        pts.append((i, h * 0.82 + rnd.uniform(-10, 10)))
    pts.append((w, h))
    d.polygon(pts, fill=sil)
    for i in range(14):
        x = rnd.uniform(20, w - 20)
        s = rnd.uniform(18, 34)
        y = h * 0.82
        if rnd.random() < 0.5:
            d.rectangle([x - s * 0.12, y - s * 1.6, x + s * 0.12, y], fill=sil)
            d.rectangle([x - s * 0.5, y - s * 1.25, x + s * 0.5, y - s * 1.0], fill=sil)
        else:
            d.rounded_rectangle([x - s * 0.4, y - s * 1.2, x + s * 0.4, y], radius=s * 0.35, fill=sil)
    # bare trees
    for tx in (w * 0.08, w * 0.9):
        d.line([(tx, h), (tx, h * 0.3)], fill=sil, width=14)
        for k in range(6):
            yy = h * (0.35 + k * 0.08)
            dx = rnd.choice([-1, 1]) * rnd.uniform(40, 90)
            d.line([(tx, yy), (tx + dx, yy - rnd.uniform(30, 60))], fill=sil, width=6)


def back_generic(t, img, d, rnd):
    w, h = img.size
    top, bottom = t.theme.get('field', (0x202030, 0x101018))
    img.paste(gradient(w, h, rgb(bottom), rgb(top)), (0, 0))


BACKS = {'zombis': back_zombis}


def paint_backboard(t):
    """the board standing behind the top arch: 680 mm x 260 mm"""
    img = Image.new('RGB', (2040, 780), (0, 0, 0))
    d = ImageDraw.Draw(img)
    rnd = random.Random(sum(map(ord, t.id)) + 7)
    BACKS.get(t.theme.get('art', t.id), back_generic)(t, img, d, rnd)
    return img
