# @name Reloj
# @orientation landscape
# The clock of the board (time.localtime()) in big letters, redrawn only
# when the second changes; aos.frame() waits for the next frame meanwhile.
import aos
import time

DIAS = ("lunes", "martes", "miercoles", "jueves", "viernes", "sabado", "domingo")
antes = None
while True:
    t = time.localtime()
    if t[5] != antes:
        antes = t[5]
        aos.clear(0x0B0F14)
        hora = "%02d:%02d:%02d" % (t[3], t[4], t[5])
        k = max(1, aos.W // (8 * len(hora) + 8))
        aos.text((aos.W - aos.textw(hora, k)) // 2, aos.H // 2 - 4 * k, hora, 0xF4EFE6, k)
        fecha = "%s %d/%d/%d" % (DIAS[t[6]], t[2], t[1], t[0])
        aos.text((aos.W - aos.textw(fecha, 2)) // 2, aos.H // 2 + 6 * k, fecha, 0x64D2FF, 2)
    aos.frame()
