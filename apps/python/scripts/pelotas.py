# @name Pelotas
# A loop of its own: draw, aos.frame(), again. A grid frozen with
# aos.background(), so each frame only puts the balls back.
import aos
import random

W, H = aos.W, aos.H
COLORES = (0xFF375F, 0x30D158, 0x0A84FF, 0xFFD60A, 0xBF5AF2, 0xFF9F0A, 0x64D2FF, 0xF4EFE6)

aos.clear(0x101820)
for x in range(0, W, 20):
    aos.line(x, 0, x, H, 0x1E2A38)
for y in range(0, H, 20):
    aos.line(0, y, W, y, 0x1E2A38)
aos.background()

pelotas = []
for c in COLORES:
    pelotas.append([random.uniform(12, W - 12), random.uniform(12, H - 12),
                    random.uniform(-3, 3), random.uniform(-3, 3), c])

while True:
    for p in pelotas:
        p[0] += p[2]
        p[1] += p[3]
        if not 10 < p[0] < W - 10:
            p[2] = -p[2]
        if not 10 < p[1] < H - 10:
            p[3] = -p[3]
        aos.disc(p[0], p[1], 10, p[4])
    s = aos.stats()
    aos.text(4, 4, "%2d ms  %d filas" % (s["frame_ms"], s["rows"]), 0xF4EFE6)
    for ev in aos.events():
        if ev[0] == "down":                 # a touch throws them all
            for p in pelotas:
                p[2], p[3] = random.uniform(-6, 6), random.uniform(-6, 6)
            aos.beep(440, 40)
    aos.frame()
