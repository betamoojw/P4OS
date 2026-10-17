#!/usr/bin/env python3
"""the harness's heat map over the table seen from above: heatview.py <id> heat.pgm out.png"""
import math
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pbtable as pt  # noqa: E402

t = pt.load(sys.argv[1])
heat = Image.open(sys.argv[2]).resize((int(pt.W * 1.5), int(pt.H * 1.5)), Image.NEAREST)
S = 1.5
img = Image.merge('RGB', (heat, heat.point(lambda v: v // 3), Image.new('L', heat.size, 0)))
d = ImageDraw.Draw(img)
for s in t.segs:
    d.line([(s[0] * S, s[1] * S), (s[2] * S, s[3] * S)], fill=(120, 200, 255) if s[5] < 0 else (255, 255, 0), width=1)
for c in t.circs:
    d.ellipse([(c[0] - c[2]) * S, (c[1] - c[2]) * S, (c[0] + c[2]) * S, (c[1] + c[2]) * S], outline=(120, 200, 255))
for f in t.flippers:
    a = math.radians(f['rest'] if f['side'] == 'L' else 180 - f['rest'])
    d.line([(f['x'] * S, f['y'] * S), ((f['x'] + f['len'] * math.cos(a)) * S, (f['y'] + f['len'] * math.sin(a)) * S)],
           fill=(0, 255, 0), width=3)
img.save(sys.argv[3])
