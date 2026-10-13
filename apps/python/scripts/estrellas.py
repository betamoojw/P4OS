# @name Estrellas
# Made of functions, as Lua's scripts are: init(), tick(dt), draw(), and
# resize(w, h), which keeps it going when the screen turns. Drag to steer.
import aos
import random

estrellas = []
vx, vy = 0.0, 0.0


def init():
    global estrellas
    estrellas = [[random.uniform(0, aos.W), random.uniform(0, aos.H), random.uniform(0.3, 3)]
                 for _ in range(160)]


def resize(w, h):
    init()


def gesture(ev, x, y, *a):
    global vx, vy
    if ev == "drag":
        vx, vy = a[0] / 4, a[1] / 4
    elif ev == "double":
        vx, vy = 0, 0


def tick(dt):
    k = dt / 16
    for e in estrellas:
        e[1] += (e[2] + vy) * k
        e[0] += vx * e[2] * k
        if e[1] > aos.H:
            e[1] -= aos.H
        elif e[1] < 0:
            e[1] += aos.H
        e[0] %= aos.W


def draw():
    aos.clear(0x05050C)
    for x, y, z in estrellas:
        g = int(80 + z * 58)
        aos.rect(x, y, 1 + int(z > 2), 1 + int(z > 2), g << 16 | g << 8 | min(255, g + 30))
