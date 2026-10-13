# @name Atrapa
# @orientation portrait
# A small game: Mila catches the fish. The finger moves her, anywhere on
# the screen, or a USB gamepad (the d-pad or the stick; START pauses).
import aos
import random

FONDO, NEGRO, PEZ = 0xBFE3F2, 0x15151A, 0xFF9F0A
x = 0.0
peces = []
puntos = 0
vidas = 3
pausa = False
espera = 0


def init():
    global x
    x = aos.W / 2
    aos.clear(FONDO)
    aos.rect(0, aos.H - 30, aos.W, 30, 0x7FC27A)        # grass
    aos.background()


def gatita(cx, cy):
    aos.ellipse(cx, cy + 10, 16, 12, NEGRO)
    aos.disc(cx, cy - 8, 11, NEGRO)
    for d in (-1, 1):
        aos.line(cx + d * 9, cy - 12, cx + d * 7, cy - 22, NEGRO)
        aos.line(cx + d * 3, cy - 17, cx + d * 7, cy - 22, NEGRO)
        aos.pixel(cx + d * 4, cy - 9, 0xC8E05A)


def pez(px, py):
    aos.ellipse(px, py, 8, 4, PEZ)
    aos.line(px + 8, py, px + 12, py - 4, PEZ)
    aos.line(px + 8, py, px + 12, py + 4, PEZ)


def tick(dt):
    global x, puntos, vidas, pausa, espera
    held, pressed, sx, sy, on = aos.pad()
    if pressed & aos.PAD_START:
        pausa = not pausa
    if pausa:
        return
    if vidas <= 0:
        if pressed & (aos.PAD_A | aos.PAD_START) or any(e[0] == "down" for e in aos.events()):
            vidas, puntos = 3, 0
            peces.clear()
        return
    tx, ty, down = aos.touch()
    if down:
        x += (tx - x) * 0.3
    if held & aos.PAD_LEFT or sx < -16000:
        x -= dt * 0.25
    if held & aos.PAD_RIGHT or sx > 16000:
        x += dt * 0.25
    x = max(16, min(aos.W - 16, x))
    espera -= dt
    if espera <= 0:
        peces.append([random.uniform(12, aos.W - 12), -10.0, random.uniform(0.06, 0.12 + puntos * 0.004)])
        espera = max(350, 1100 - puntos * 25)
    suelo = aos.H - 50
    for p in peces[:]:
        p[1] += p[2] * dt
        if p[1] > suelo - 18 and abs(p[0] - x) < 22:
            peces.remove(p)
            puntos += 1
            aos.beep(1200, 40)
        elif p[1] > aos.H - 30:
            peces.remove(p)
            vidas -= 1
            aos.beep(220, 120)


def draw():
    for p in peces:
        pez(p[0], p[1])
    gatita(int(x), aos.H - 50)
    aos.text(6, 6, "%d" % puntos, NEGRO, 2)
    aos.text(aos.W - 6 - aos.textw("<3" * vidas, 2), 6, "<3" * vidas, 0xFF375F, 2)
    if vidas <= 0:
        aos.text((aos.W - aos.textw("Fin", 4)) // 2, aos.H // 3, "Fin", NEGRO, 4)
    elif pausa:
        aos.text((aos.W - aos.textw("Pausa", 3)) // 2, aos.H // 3, "Pausa", NEGRO, 3)
