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

import numpy as np

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


def glow(img, draw_fn, color, blur, strength=1.0):
    """draws a shape into a mask, blurs it and lays the colour over: neon"""
    m = Image.new('L', img.size, 0)
    draw_fn(ImageDraw.Draw(m))
    m = m.filter(ImageFilter.GaussianBlur(blur))
    if strength != 1.0:
        m = m.point(lambda v: min(255, int(v * strength)))
    img.paste(color, (0, 0), m)


def ellipse_rot(cx, cy, rx, ry, a, n=40):
    ca, sa = math.cos(a), math.sin(a)
    return [(cx + rx * math.cos(t) * ca - ry * math.sin(t) * sa, cy + rx * math.cos(t) * sa + ry * math.sin(t) * ca)
            for t in (i / n * 2 * math.pi for i in range(n))]


def shaded_disc(img, cx, cy, r, c0, c1):
    """a sphere-ish disc: lit from the top left"""
    m = Image.new('L', img.size, 0)
    ImageDraw.Draw(m).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    box = (int(cx - r), int(cy - r), int(cx + r), int(cy + r))
    g = Image.new('RGB', (box[2] - box[0], box[3] - box[1]))
    gd = ImageDraw.Draw(g)
    w = g.size[0]
    for i in range(w // 2, 0, -2):
        t = i / (w / 2)
        c = tuple(int(c0[k] * (1 - t) + c1[k] * t) for k in range(3))
        ox, oy = w * 0.36, w * 0.34
        gd.ellipse([ox - i * 1.2, oy - i * 1.2, ox + i * 1.2, oy + i * 1.2], fill=c)
    img.paste(g, box[:2], m.crop(box))


def suit(d, kind, cx, cy, r, fill):
    if kind in ('heart', 'diamond') and kind == 'diamond':
        d.polygon([(cx, cy - r), (cx + r * 0.7, cy), (cx, cy + r), (cx - r * 0.7, cy)], fill=fill)
    elif kind == 'heart':
        d.ellipse([cx - r, cy - r * 0.8, cx, cy + r * 0.1], fill=fill)
        d.ellipse([cx, cy - r * 0.8, cx + r, cy + r * 0.1], fill=fill)
        d.polygon([(cx - r * 0.97, cy - r * 0.2), (cx + r * 0.97, cy - r * 0.2), (cx, cy + r)], fill=fill)
    elif kind == 'spade':
        d.ellipse([cx - r, cy - r * 0.1, cx, cy + r * 0.7], fill=fill)
        d.ellipse([cx, cy - r * 0.1, cx + r, cy + r * 0.7], fill=fill)
        d.polygon([(cx - r * 0.97, cy + r * 0.25), (cx + r * 0.97, cy + r * 0.25), (cx, cy - r)], fill=fill)
        d.polygon([(cx, cy + r * 0.3), (cx - r * 0.4, cy + r * 1.1), (cx + r * 0.4, cy + r * 1.1)], fill=fill)
    else:
        for dx, dy in ((0, -0.45), (-0.45, 0.15), (0.45, 0.15)):
            d.ellipse([cx + (dx - 0.42) * r, cy + (dy - 0.42) * r, cx + (dx + 0.42) * r, cy + (dy + 0.42) * r], fill=fill)
        d.polygon([(cx, cy), (cx - r * 0.4, cy + r * 1.1), (cx + r * 0.4, cy + r * 1.1)], fill=fill)


def theme_zombis2(t, img, d, rnd):
    theme_zombis(t, img, d, rnd)
    w, h = img.size
    # a path of cobbles down the middle, a fence, glowing graves
    for i in range(70):
        y = rnd.uniform(0.45, 0.95) * h
        x = w * 0.46 + math.sin(y / h * 9) * w * 0.08 + rnd.uniform(-60, 60)
        r = rnd.uniform(14, 26)
        d.ellipse([x - r, y - r * 0.7, x + r, y + r * 0.7], fill=(70, 72, 64), outline=(40, 42, 38))
    for k in range(5):
        cx, cy = rnd.uniform(0.15, 0.85) * w, rnd.uniform(0.35, 0.8) * h
        glow(img, lambda m, cx=cx, cy=cy: m.ellipse([cx - 90, cy - 40, cx + 90, cy + 40], fill=150),
             (120, 255, 60), 40)
    # a big skull in the centre, faint
    cx, cy = P(241, 470)
    glow(img, lambda m: m.ellipse([cx - 220, cy - 260, cx + 220, cy + 160], fill=90), (150, 255, 110), 30)
    d.ellipse([cx - 120, cy - 130, cx - 40, cy - 40], fill=(20, 30, 16))
    d.ellipse([cx + 40, cy - 130, cx + 120, cy - 40], fill=(20, 30, 16))


def theme_robots(t, img, d, rnd):
    w, h = img.size
    img.paste(gradient(w, h, (24, 32, 96), (6, 8, 30)), (0, 0))
    d = ImageDraw.Draw(img)
    for i in range(500):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h)
        r = rnd.choice([1, 1, 1, 2, 3])
        c = rnd.choice([(255, 255, 255), (180, 220, 255), (255, 200, 240)])
        d.ellipse([x - r, y - r, x + r, y + r], fill=c)
    # nebulas
    for cx, cy, c in ((0.2, 0.25, (160, 40, 200)), (0.8, 0.55, (40, 120, 255)), (0.35, 0.8, (255, 60, 160))):
        glow(img, lambda m, cx=cx, cy=cy: m.ellipse([cx * w - 380, cy * h - 260, cx * w + 380, cy * h + 260], fill=120),
             c, 120, 0.9)
    d = ImageDraw.Draw(img)
    # planets
    shaded_disc(img, w * 0.78, h * 0.3, 150, (255, 200, 120), (140, 50, 20))
    d = ImageDraw.Draw(img)
    d.ellipse([w * 0.78 - 260, h * 0.3 - 50, w * 0.78 + 260, h * 0.3 + 50], outline=(240, 220, 170), width=10)
    shaded_disc(img, w * 0.18, h * 0.6, 90, (140, 230, 255), (20, 60, 140))
    # circuit traces in neon
    def traces(m):
        for i in range(60):
            x, y = rnd.uniform(0, w), rnd.uniform(h * 0.3, h)
            pts = [(x, y)]
            for k in range(rnd.randint(2, 5)):
                if k % 2:
                    x += rnd.choice([-1, 1]) * rnd.uniform(60, 240)
                else:
                    y += rnd.choice([-1, 1]) * rnd.uniform(60, 240)
                pts.append((x, y))
            m.line(pts, fill=255, width=5)
            m.ellipse([x - 12, y - 12, x + 12, y + 12], outline=255, width=5)
    glow(img, traces, (40, 230, 255), 2, 0.8)
    glow(img, traces, (40, 230, 255), 10, 0.6)
    # radar rings round the middle
    cx, cy = P(241, 470)
    glow(img, lambda m: [m.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=6) for r in (180, 300, 420)],
         (255, 70, 200), 4, 0.7)


def theme_jungla(t, img, d, rnd):
    w, h = img.size
    img.paste(gradient(w, h, (70, 140, 60), (14, 50, 20)), (0, 0))
    d = ImageDraw.Draw(img)
    # temple floor at the top
    for gy in range(0, int(h * 0.3), 90):
        for gx in range(-(gy // 90 % 2) * 60, w, 120):
            d.rounded_rectangle([gx + 4, gy + 4, gx + 116, gy + 86], radius=10, fill=(120, 120, 90),
                                outline=(70, 80, 50), width=4)
    # a river across
    pts_t, pts_b = [], []
    for i in range(0, w + 40, 40):
        yc = h * 0.62 + math.sin(i / w * 6) * 110
        pts_t.append((i, yc - 70))
        pts_b.append((i, yc + 70))
    d.polygon(pts_t + pts_b[::-1], fill=(40, 120, 170))
    for i in range(30):
        x, y = rnd.uniform(0, w), h * 0.62 + rnd.uniform(-50, 50)
        d.arc([x - 40, y - 10, x + 40, y + 10], 200, 340, fill=(160, 220, 240), width=4)
    # leaves
    for i in range(160):
        x, y = rnd.uniform(0, w), rnd.uniform(h * 0.25, h)
        a = rnd.uniform(0, math.pi)
        L = rnd.uniform(50, 140)
        c = rnd.choice([(40, 120, 40), (60, 150, 50), (30, 90, 30), (90, 170, 60)])
        d.polygon(ellipse_rot(x, y, L, L * 0.32, a), fill=c)
        d.line([(x - math.cos(a) * L, y - math.sin(a) * L), (x + math.cos(a) * L, y + math.sin(a) * L)],
               fill=tuple(min(255, v + 40) for v in c), width=3)
    # sun beams from the top
    glow(img, lambda m: [m.polygon([(w * 0.5 + k * 140, 0), (w * 0.5 + k * 140 + 90, 0),
                                    (w * 0.5 + k * 420 + 200, h * 0.7), (w * 0.5 + k * 420 - 100, h * 0.7)], fill=70)
                         for k in (-2, -1, 0, 1, 2)], (255, 230, 140), 40)


def theme_circo(t, img, d, rnd):
    w, h = img.size
    cx, cy = P(241, 420)
    n = 36
    for i in range(n):
        a0, a1 = i / n * 2 * math.pi, (i + 1) / n * 2 * math.pi
        R = 4000
        d.polygon([(cx, cy), (cx + R * math.cos(a0), cy + R * math.sin(a0)), (cx + R * math.cos(a1), cy + R * math.sin(a1))],
                  fill=(200, 24, 40) if i % 2 else (240, 230, 210))
    # vignette to the edges, darker reds
    v = Image.new('L', img.size, 0)
    ImageDraw.Draw(v).ellipse([cx - w * 0.75, cy - h * 0.55, cx + w * 0.75, cy + h * 0.55], fill=255)
    v = v.filter(ImageFilter.GaussianBlur(200))
    dark = Image.new('RGB', img.size, (60, 6, 12))
    img.paste(dark, (0, 0), v.point(lambda x: 255 - x))
    d = ImageDraw.Draw(img)
    # the ring: sawdust, gold edge
    d.ellipse([cx - 420, cy - 330, cx + 420, cy + 330], fill=(190, 140, 80), outline=(255, 210, 70), width=18)
    d.ellipse([cx - 380, cy - 290, cx + 380, cy + 290], outline=(140, 30, 30), width=8)
    for i in range(140):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h)
        c = rnd.choice([(255, 220, 60), (60, 140, 255), (255, 255, 255), (60, 200, 120)])
        d.polygon(star(x, y, 6, 15, 5, rnd.uniform(0, 70)), fill=c)
    # bunting
    for row, yy in enumerate((h * 0.1, h * 0.18)):
        for i in range(0, w, 70):
            c = [(255, 210, 60), (40, 110, 230), (230, 30, 50)][(i // 70 + row) % 3]
            d.polygon([(i, yy), (i + 64, yy), (i + 32, yy + 70)], fill=c)


def theme_casino(t, img, d, rnd):
    w, h = img.size
    img.paste(gradient(w, h, (18, 120, 60), (6, 50, 26)), (0, 0))
    a = np.asarray(img).astype(np.int16)
    a = a + np.random.default_rng(5).integers(-10, 10, a.shape[:2])[..., None]
    img.paste(Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)), (0, 0))
    d = ImageDraw.Draw(img)
    for i in range(70):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h)
        suit(d, rnd.choice(['heart', 'diamond', 'spade', 'club']), x, y, rnd.uniform(25, 45), (12, 90, 44))
    for m in (30, 60):
        d.rounded_rectangle([m, m, w - m, h - m], radius=120, outline=(230, 190, 70), width=8)
    # the roulette's numbers round the saucer
    cx, cy = P(240, 450)
    for i in range(37):
        a0, a1 = i / 37 * 2 * math.pi, (i + 1) / 37 * 2 * math.pi
        pts = [(cx + 150 * math.cos(a0), cy + 150 * math.sin(a0)), (cx + 230 * math.cos(a0), cy + 230 * math.sin(a0)),
               (cx + 230 * math.cos(a1), cy + 230 * math.sin(a1)), (cx + 150 * math.cos(a1), cy + 150 * math.sin(a1))]
        d.polygon(pts, fill=(20, 140, 60) if i == 0 else ((200, 20, 40) if i % 2 else (20, 20, 20)))
    d.ellipse([cx - 232, cy - 232, cx + 232, cy + 232], outline=(230, 190, 70), width=8)
    # big suits under the flippers' arc
    for k, kind in enumerate(('spade', 'heart', 'club', 'diamond')):
        x, y = P(150 + k * 60, 800)
        suit(d, kind, x, y, 60, (200, 20, 40) if kind in ('heart', 'diamond') else (15, 15, 15))


def theme_piratas(t, img, d, rnd):
    w, h = img.size
    img.paste(gradient(w, int(h * 0.62), (30, 110, 170), (40, 160, 190)), (0, 0))
    img.paste(gradient(w, h - int(h * 0.62), (232, 200, 140), (150, 110, 60)), (0, int(h * 0.62)))
    d = ImageDraw.Draw(img)
    # the shore
    pts = [(0, h * 0.6)]
    for i in range(0, w + 60, 60):
        pts.append((i, h * 0.6 + math.sin(i / 120) * 30 + rnd.uniform(-8, 8)))
    pts += [(w, h), (0, h)]
    d.polygon(pts, fill=(226, 194, 132))
    for i in range(80):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h * 0.58)
        d.arc([x - 50, y - 12, x + 50, y + 12], 200, 340, fill=(200, 235, 245), width=4)
    # planks at the bottom
    for i in range(int(h * 0.82), h, 70):
        d.rectangle([0, i, w, i + 64], fill=(110, 70, 36), outline=(70, 42, 20), width=4)
    # the treasure map's path to the chest
    path = [P(60, 760), P(140, 700), P(120, 620), P(240, 600), P(260, 520), P(340, 500), P(380, 430)]
    for (x0, y0), (x1, y1) in zip(path, path[1:]):
        n = int(math.hypot(x1 - x0, y1 - y0) / 40)
        for k in range(0, n, 2):
            a, b = k / n, min(1, (k + 1) / n)
            d.line([(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a), (x0 + (x1 - x0) * b, y0 + (y1 - y0) * b)],
                   fill=(180, 30, 30), width=12)
    x, y = P(380, 430)
    d.line([(x - 60, y - 60), (x + 60, y + 60)], fill=(200, 20, 20), width=22)
    d.line([(x - 60, y + 60), (x + 60, y - 60)], fill=(200, 20, 20), width=22)
    # a compass rose
    cx, cy = P(241, 720)
    d.ellipse([cx - 170, cy - 170, cx + 170, cy + 170], outline=(120, 80, 40), width=8)
    for k in range(8):
        a = k * math.pi / 4
        L = 200 if k % 2 == 0 else 110
        d.polygon([(cx + math.cos(a) * L, cy + math.sin(a) * L), (cx + math.cos(a + 0.25) * 40, cy + math.sin(a + 0.25) * 40),
                   (cx, cy), (cx + math.cos(a - 0.25) * 40, cy + math.sin(a - 0.25) * 40)],
                  fill=(150, 30, 30) if k % 2 == 0 else (90, 60, 30))


THEMES.update({'zombis': theme_zombis2, 'robots': theme_robots, 'jungla': theme_jungla, 'circo': theme_circo,
               'casino': theme_casino, 'piratas': theme_piratas})


def back_robots(t, img, d, rnd):
    w, h = img.size
    img.paste(gradient(w, h, (4, 6, 24), (30, 20, 80)), (0, 0))
    for i in range(300):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h)
        d.point((x, y), fill=(255, 255, 255))
    shaded_disc(img, w * 0.3, h * 0.55, 220, (255, 170, 90), (120, 40, 20))
    d = ImageDraw.Draw(img)
    d.ellipse([w * 0.3 - 380, h * 0.55 - 70, w * 0.3 + 380, h * 0.55 + 70], outline=(230, 210, 160), width=14)
    for i in range(6):
        x = 1200 + i * 130
        d.rectangle([x, h - 220 - i % 3 * 60, x + 90, h], fill=(20, 26, 50))
        d.rectangle([x + 20, h - 200 - i % 3 * 60, x + 70, h - 170 - i % 3 * 60], fill=(60, 230, 255))


def back_jungla(t, img, d, rnd):
    w, h = img.size
    img.paste(gradient(w, h, (255, 160, 60), (255, 90, 60)), (0, 0))
    d.polygon([(w * 0.35, h), (w * 0.5, h * 0.3), (w * 0.65, h)], fill=(70, 50, 40))
    for i in range(5):
        y = h * 0.3 + i * h * 0.14
        d.rectangle([w * 0.5 - (i + 1) * w * 0.03, y, w * 0.5 + (i + 1) * w * 0.03, y + h * 0.14], fill=(60 + i * 6, 46, 36))
    for i in range(40):
        x = rnd.uniform(0, w)
        a = rnd.uniform(-0.6, 0.6)
        d.polygon(ellipse_rot(x, h - rnd.uniform(0, 140), 160, 40, a), fill=(20, 60, 20))


def back_circo(t, img, d, rnd):
    w, h = img.size
    img.paste(gradient(w, h, (20, 10, 40), (80, 10, 30)), (0, 0))
    for k in range(3):
        x0 = w * (0.2 + k * 0.3)
        glow(img, lambda m, x0=x0: m.polygon([(x0, h), (x0 - 200, 0), (x0 + 200, 0)], fill=80), (255, 240, 180), 30)
    d = ImageDraw.Draw(img)
    for i in range(14):
        a0, a1 = i / 14, (i + 1) / 14
        d.polygon([(w / 2, 40), (w * (0.2 + 0.6 * a0), h), (w * (0.2 + 0.6 * a1), h)],
                  fill=(220, 30, 40) if i % 2 else (245, 235, 215))
    for i in range(0, w, 60):
        d.ellipse([i, 8, i + 24, 32], fill=(255, 220, 90))


def back_casino(t, img, d, rnd):
    w, h = img.size
    img.paste(gradient(w, h, (10, 6, 20), (60, 10, 30)), (0, 0))
    for i in range(18):
        x = i * 120
        hh = rnd.uniform(200, 520)
        d.rectangle([x, h - hh, x + 100, h], fill=(20, 16, 30))
        for k in range(int(hh / 50)):
            if rnd.random() < 0.5:
                d.rectangle([x + 20, h - hh + 20 + k * 50, x + 40, h - hh + 36 + k * 50], fill=(255, 210, 90))
    for i in range(0, w, 50):
        c = (255, 220, 120) if (i // 50) % 2 else (255, 60, 80)
        d.ellipse([i, 10, i + 22, 32], fill=c)
        d.ellipse([i, h - 34, i + 22, h - 12], fill=c)


def back_piratas(t, img, d, rnd):
    w, h = img.size
    img.paste(gradient(w, h, (250, 150, 70), (90, 40, 70)), (0, 0))
    d.ellipse([w * 0.45, h * 0.35, w * 0.6, h * 0.35 + w * 0.15], fill=(255, 220, 120))
    img.paste(gradient(w, int(h * 0.3), (40, 60, 110), (10, 20, 50)), (0, int(h * 0.7)))
    d = ImageDraw.Draw(img)
    for cx, s in ((w * 0.22, 1.0), (w * 0.8, 0.7)):
        base = h * 0.72
        d.polygon([(cx - 260 * s, base), (cx + 260 * s, base), (cx + 200 * s, base + 70 * s), (cx - 200 * s, base + 70 * s)],
                  fill=(20, 12, 10))
        for k, mx in enumerate((-100, 60)):
            d.line([(cx + mx * s, base), (cx + mx * s, base - 330 * s)], fill=(20, 12, 10), width=int(12 * s))
            d.polygon([(cx + mx * s - 110 * s, base - 300 * s), (cx + mx * s + 110 * s, base - 300 * s),
                       (cx + mx * s + 90 * s, base - 120 * s), (cx + mx * s - 90 * s, base - 120 * s)], fill=(30, 20, 18))
        d.rectangle([cx + 60 * s, base - 380 * s, cx + 140 * s, base - 330 * s], fill=(10, 10, 10))


BACKS.update({'robots': back_robots, 'jungla': back_jungla, 'circo': back_circo, 'casino': back_casino,
              'piratas': back_piratas})
