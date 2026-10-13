# @name Hola Python
# The smallest app: it draws once and answers the finger.
# A black kitten on a light background, and a heart where you touch.
import aos

W, H = aos.W, aos.H
FONDO = 0xF4EFE6
NEGRO = 0x15151A
OJOS = 0xC8E05A


def triangulo(x0, y0, x1, y1, x2, y2, c):
    # filled, a row at a time between its two edges
    pts = sorted(((x0, y0), (x1, y1), (x2, y2)), key=lambda p: p[1])
    (ax, ay), (bx, by), (cx, cy) = pts
    for y in range(int(ay), int(cy) + 1):
        def edge(px, py, qx, qy):
            return px if qy == py else px + (qx - px) * (y - py) / (qy - py)
        xa = edge(ax, ay, cx, cy)
        xb = edge(ax, ay, bx, by) if y < by else edge(bx, by, cx, cy)
        aos.line(min(xa, xb), y, max(xa, xb), y, c)


def gatito(cx, cy):
    # the tail, a curve of discs
    for i in range(14):
        aos.disc(cx + 55 + i * 3, cy + 95 - i * 6 + (i * i) // 6, 7, NEGRO)
    aos.ellipse(cx, cy + 85, 58, 46, NEGRO)            # body
    aos.disc(cx, cy, 44, NEGRO)                         # head
    triangulo(cx - 42, cy - 12, cx - 34, cy - 58, cx - 8, cy - 36, NEGRO)
    triangulo(cx + 42, cy - 12, cx + 34, cy - 58, cx + 8, cy - 36, NEGRO)
    for dx in (-17, 17):                                # eyes
        aos.ellipse(cx + dx, cy - 2, 9, 11, OJOS)
        aos.ellipse(cx + dx, cy - 1, 3, 8, NEGRO)
    aos.disc(cx, cy + 14, 3, 0xF4A0B0)                  # nose
    for dy in (12, 18):                                 # whiskers
        aos.line(cx - 14, cy + dy, cx - 48, cy + dy - 6 + (dy - 12), 0x9A9AA0)
        aos.line(cx + 14, cy + dy, cx + 48, cy + dy - 6 + (dy - 12), 0x9A9AA0)


def corazon(x, y, c):
    aos.disc(x - 5, y - 3, 6, c)
    aos.disc(x + 5, y - 3, 6, c)
    triangulo(x - 11, y - 1, x + 11, y - 1, x, y + 12, c)


aos.clear(FONDO)
titulo = "Hola, soy Mila"
aos.text((W - aos.textw(titulo, 2)) // 2, 36, titulo, NEGRO, 2)
aos.text((W - aos.textw("Toca la pantalla")) // 2, H - 40, "Toca la pantalla", 0x6E6E78)
gatito(W // 2, H // 2 - 20)


def touch(x, y, ev):
    if ev == "down":
        corazon(x, y, 0xFF375F)
        aos.beep(880, 60)
