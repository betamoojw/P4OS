# @name Marquesina
# @orientation landscape
# MicroPython's framebuf over the same canvas (aos.fb): a band of text
# that runs, and below it a wave drawn in a FrameBuffer of its own, moved
# a step left each frame with scroll() and put in place with blit().
# framebuf's colours are RGB565: aos.rgb565(0xRRGGBB) gives one.
import aos
import framebuf
import math

fb = aos.fb
W, H = aos.W, aos.H
NEGRO, AMARILLO, CELESTE, GRIS = [aos.rgb565(c) for c in (0x0B0F14, 0xFFD43B, 0x3776AB, 0x2A3340)]
texto = "Python en P4OS  *  aos.fb es un framebuf de MicroPython  *  "
ancho = len(texto) * 8

OH = H - 60
onda = framebuf.FrameBuffer(bytearray(W * OH * 2), W, OH, framebuf.RGB565)
onda.fill(NEGRO)
off = 0
t = 0.0
y_antes = OH // 2
while True:
    fb.fill_rect(0, 0, W, 60, NEGRO)
    for k in range(2):                      # twice, so it comes round
        fb.text(texto, k * ancho - off, 26, AMARILLO)
    off = (off + 2) % ancho

    onda.scroll(-2, 0)
    onda.fill_rect(W - 2, 0, 2, OH, NEGRO)
    onda.hline(W - 2, OH // 2, 2, GRIS)
    t += 0.09
    y = int(OH / 2 + math.sin(t) * OH * 0.35 + math.sin(t * 3.1) * OH * 0.1)
    onda.line(W - 3, y_antes, W - 1, y, CELESTE)
    y_antes = y
    fb.blit(onda, 0, 60)
    aos.frame()
