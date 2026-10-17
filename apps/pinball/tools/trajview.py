#!/usr/bin/env python3
"""trajectories from the harness (TRAJ=file) over the table from above: trajview.py <id> traj.txt out.png"""
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pbtable as pt  # noqa: E402

t = pt.load(sys.argv[1])
S = 1.5
img = Image.new('RGB', (int(pt.W * S), int(pt.H * S)), (0, 0, 0))
d = ImageDraw.Draw(img)
for s in t.segs:
    d.line([(s[0] * S, s[1] * S), (s[2] * S, s[3] * S)], fill=(90, 140, 200) if s[5] < 0 else (255, 255, 0), width=1)
for c in t.circs:
    d.ellipse([(c[0] - c[2]) * S, (c[1] - c[2]) * S, (c[0] + c[2]) * S, (c[1] + c[2]) * S], outline=(90, 140, 200))
import colorsys
for i, line in enumerate(open(sys.argv[2])):
    v = [float(x) for x in line.split()]
    pts = [(v[k] * S, v[k + 1] * S) for k in range(0, len(v), 2) if v[k] > 0]
    r, g, b = colorsys.hsv_to_rgb((i * 0.13) % 1, 0.8, 1)
    if len(pts) > 1:
        d.line(pts, fill=(int(r * 255), int(g * 255), int(b * 255)), width=1)
        d.ellipse([pts[0][0] - 3, pts[0][1] - 3, pts[0][0] + 3, pts[0][1] + 3], fill=(255, 255, 255))
img.save(sys.argv[3])
