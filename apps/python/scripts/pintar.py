# @name Pintar
# @canvas 360x640
# A canvas of its own (360x640, x2) and every finger at once: each one
# paints with the colour picked last (the last one rubs out). A double
# tap clears the paper.
import aos

PALETA = (0x15151A, 0xFF375F, 0xFF9F0A, 0xFFD60A, 0x30D158, 0x0A84FF, 0xBF5AF2, 0xF4EFE6)
PAPEL = 0xF4EFE6
TIRA = 44                                   # the palette's strip at the bottom
color = PALETA[0]
antes = {}                                  # finger id -> where it was


def papel():
    aos.clear(PAPEL)
    paleta()


def paleta():
    w = aos.W // len(PALETA)
    for i, c in enumerate(PALETA):
        aos.rect(i * w, aos.H - TIRA, w, TIRA, c)
        if c == PAPEL:                      # the eraser
            aos.box(i * w + 8, aos.H - TIRA + 8, w - 16, TIRA - 16, 0xC7C7CC)
        if c == color:
            aos.box(i * w + 3, aos.H - TIRA + 3, w - 6, TIRA - 6, 0x8E8E93)


def gesture(ev, x, y, *a):
    if ev == "double" and y < aos.H - TIRA:
        papel()


papel()
while True:
    vistos = {}
    for fid, x, y in aos.fingers():
        if y >= aos.H - TIRA:
            color = PALETA[min(len(PALETA) - 1, x * len(PALETA) // aos.W)]
            paleta()
            continue
        if fid in antes:
            px, py = antes[fid]
            # a thick line: discs along the way
            n = max(abs(x - px), abs(y - py)) // 3 + 1
            for i in range(n + 1):
                aos.disc(px + (x - px) * i // n, py + (y - py) * i // n, 5, color)
        else:
            aos.disc(x, y, 5, color)
        vistos[fid] = (x, y)
    antes = vistos
    aos.frame()
