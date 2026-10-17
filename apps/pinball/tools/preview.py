#!/usr/bin/env python3
"""
PINBALL - a flat preview of a table's geometry, without Blender: the
camera's view (what the game will show) next to the table from above.

    python3 apps/pinball/tools/preview.py zombis out.png
"""
import math
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pbtable as pt  # noqa: E402


def flipper_poly(f, ang):
    a = math.radians(ang if f['side'] == 'L' else 180 - ang)
    px, py, L, r0, r1 = f['x'], f['y'], f['len'], f['r0'], f['r1']
    tx, ty = px + L * math.cos(a), py + L * math.sin(a)
    pts = []
    for i in range(13):
        b = a + math.pi / 2 + math.pi * i / 12
        pts.append((px + r0 * math.cos(b), py + r0 * math.sin(b)))
    for i in range(13):
        b = a - math.pi / 2 + math.pi * i / 12
        pts.append((tx + r1 * math.cos(b), ty + r1 * math.sin(b)))
    return pts


def main():
    t = pt.load(sys.argv[1])
    out = sys.argv[2]
    c = t.camera
    S = 0.95
    img = Image.new('RGB', (720 + 20 + int(pt.W * S) + 20, 1280), (16, 16, 20))
    d = ImageDraw.Draw(img)
    ox = 740

    def P(x, y, z=0):
        sx, sy, _ = c.project(x, y, z)
        return sx, sy

    def T(x, y, z=0):
        return ox + x * S, 40 + y * S

    d.rectangle([0, 0, 719, pt.DMD_H - 1], fill=(30, 10, 0))
    for proj, z0 in ((P, 0), (T, 0)):
        # the playfield's outline
        d.polygon([proj(0, 0), proj(pt.W, 0), proj(pt.W, pt.H), proj(0, pt.H)], outline=(60, 60, 70))
        for s in t.segs:
            col = (230, 230, 230) if s[5] < 0 else (255, 120, 60)
            d.line([proj(s[0], s[1]), proj(s[2], s[3])], fill=col, width=2)
        for s in t.circs:
            x, y, r = s[0], s[1], s[2]
            pts = [proj(x + r * math.cos(a / 8 * math.pi), y + r * math.sin(a / 8 * math.pi)) for a in range(17)]
            d.line(pts, fill=(255, 200, 80), width=2)
        for s in t.trigs:
            d.line([proj(s[0], s[1]), proj(s[2], s[3])], fill=(80, 160, 255), width=2)
        for s in t.rolls:
            x, y, r = s[0], s[1], s[2]
            pts = [proj(x + r * math.cos(a / 8 * math.pi), y + r * math.sin(a / 8 * math.pi)) for a in range(17)]
            d.line(pts, fill=(80, 255, 160), width=1)
        for f in t.flippers:
            d.polygon([proj(*p) for p in flipper_poly(f, f['rest'])], outline=(255, 255, 255))
            d.polygon([proj(*p) for p in flipper_poly(f, f['up'])], outline=(140, 140, 140))
        for r in t.ramps:
            if proj is P:
                d.line([P(*p) for p in r['path']], fill=(200, 80, 255), width=3)
            else:
                d.line([T(p[0], p[1]) for p in r['path']], fill=(200, 80, 255), width=3)
        for l in t.lamps:
            if proj is P:
                x, y = P(l['x'], l['y'], l.get('z', 0))
            else:
                x, y = T(l['x'], l['y'])
            col = ((l['color'] >> 16) & 255, (l['color'] >> 8) & 255, l['color'] & 255)
            d.ellipse([x - 4, y - 4, x + 4, y + 4], fill=col)
        for v in t.vis:
            if v['t'] == 'decor':
                x, y = (P if proj is P else T)(v['x'], v['y'])
                d.rectangle([x - 5, y - 5, x + 5, y + 5], outline=(255, 0, 255))
        # the ball at the plunger
        bx, by = t.plunger
        r = pt.BALL_R
        pts = [proj(bx + r * math.cos(a / 8 * math.pi), by + r * math.sin(a / 8 * math.pi)) for a in range(17)]
        d.line(pts, fill=(255, 255, 255), width=2)
    img.save(out)
    print('camera f %.1f c (%.1f, %.1f) pos (%.0f, %.0f, %.0f)' % (c.f, c.cx, c.cy, *c.pos))
    for (x, y) in ((0, 0), (pt.W, 0), (0, pt.H), (pt.W, pt.H)):
        sx, sy, z = c.project(x, y)
        print('  corner (%d,%d) -> (%.0f, %.0f) depth %.0f  %.2f px/mm' % (x, y, sx, sy, z, c.f / z))


if __name__ == '__main__':
    main()
