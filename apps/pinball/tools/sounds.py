"""
PINBALL - every table's sounds, synthesised here with numpy: nothing
recorded, nothing borrowed. pack_p4.py stores them in the pack as mu-law
at 16 kHz (pb_audio.c plays them).

make(table) returns {'s_<name>': samples, ..., 'music': loop} for every
sound the game asks for (pb_game.h, PB_SND_NAMES). The mechanical ones
(flippers, rubbers, the plunger) are the same machine on every table; the
rest come from the theme's kit: its instruments, its scale, its music.
A theme without a kit gets the plain one.
"""
import math

import numpy as np

R = 16000
RNG = np.random.default_rng(1234)

SND_NAMES = ['flipper', 'bumper', 'sling', 'drop', 'bank', 'target', 'lane', 'lanes', 'spinner', 'saucer', 'eject',
             'ramp', 'rampfail', 'orbit', 'launch', 'drain', 'save', 'kickback', 'mission', 'missiondone', 'jackpot',
             'multiball', 'lock', 'extra', 'tiltwarn', 'tilt', 'skill', 'bonus', 'gameover', 'start', 'wall', 'post']


# ---- the basics -------------------------------------------------------------

def T(d):
    return np.arange(int(d * R)) / R


def hz(note):
    """'A4', 'C#5', 'Bb3' or a MIDI number -> Hz"""
    if isinstance(note, (int, float)):
        return 440.0 * 2 ** ((note - 69) / 12)
    names = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}
    n = names[note[0]]
    i = 1
    if note[i] == '#':
        n += 1
        i += 1
    elif note[i] == 'b':
        n -= 1
        i += 1
    octave = int(note[i:])
    return 440.0 * 2 ** ((n + 12 * (octave + 1) - 69) / 12)


def midi(note):
    return int(round(69 + 12 * math.log2(hz(note) / 440.0)))


def phase(f, n):
    f = np.broadcast_to(np.asarray(f, np.float64), (n,))
    return 2 * np.pi * np.cumsum(f) / R


def osc(f, d, wave='sine', harm=None):
    n = int(d * R)
    ph = phase(f, n)
    if wave == 'sine':
        return np.sin(ph)
    fmax = float(np.max(f))
    kmax = max(1, int((R / 2 - 200) / max(fmax, 1)))
    out = np.zeros(n)
    if wave == 'saw':
        for k in range(1, min(kmax, 40) + 1):
            out += np.sin(k * ph) / k
        return out * 0.6
    if wave == 'square':
        for k in range(1, min(kmax, 40) + 1, 2):
            out += np.sin(k * ph) / k
        return out * 0.9
    if wave == 'tri':
        for i, k in enumerate(range(1, min(kmax, 25) + 1, 2)):
            out += ((-1) ** i) * np.sin(k * ph) / (k * k)
        return out * 0.8
    if wave == 'add':
        for k, a in enumerate(harm, 1):
            if k <= kmax and a:
                out += a * np.sin(k * ph)
        return out / max(1e-6, sum(abs(a) for a in harm))
    raise ValueError(wave)


def env(n, a=0.004, d=0.2, s=0.0, r=0.05, hold=None):
    """attack, an exponential decay towards s, release at the end"""
    t = np.arange(n) / R
    e = np.where(t < a, t / max(a, 1e-6), s + (1 - s) * np.exp(-(t - a) / max(d, 1e-6)))
    rn = int(r * R)
    if rn and rn < n:
        e[-rn:] *= np.linspace(1, 0, rn)
    return e


def noise(d):
    return RNG.uniform(-1, 1, int(d * R))


def lp(x, fc):
    """one pole; fc may follow the sound (an array)"""
    fc = np.broadcast_to(np.asarray(fc, np.float64), x.shape)
    a = np.exp(-2 * np.pi * np.clip(fc, 20, R / 2 - 100) / R)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc = (1 - a[i]) * x[i] + a[i] * acc
        y[i] = acc
    return y


def hp(x, fc):
    return x - lp(x, fc)


def biquad_bp(x, fc, q=4.0):
    """a resonant band-pass (RBJ), fixed centre"""
    w = 2 * np.pi * fc / R
    al = np.sin(w) / (2 * q)
    b0, b1, b2 = al, 0, -al
    a0, a1, a2 = 1 + al, -2 * np.cos(w), 1 - al
    y = np.zeros_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for i in range(len(x)):
        v = (b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0
        x2, x1 = x1, x[i]
        y2, y1 = y1, v
        y[i] = v
    return y


def formant(x, vowel):
    """a voice's vowel: three band-passes"""
    F = {'a': (730, 1090, 2440), 'o': (570, 840, 2410), 'u': (300, 870, 2240), 'e': (530, 1840, 2480),
         'i': (270, 2290, 3010)}[vowel]
    return biquad_bp(x, F[0], 5) + 0.6 * biquad_bp(x, F[1], 6) + 0.3 * biquad_bp(x, F[2], 8)


def reverb(x, room=0.5, mix=0.25, tail=1.0):
    """Schroeder: four combs and two all-passes"""
    out = np.concatenate([x, np.zeros(int(tail * R))])
    acc = np.zeros_like(out)
    for dl, g in ((1116, 0.84), (1188, 0.83), (1277, 0.82), (1356, 0.81)):
        dl = int(dl * R / 44100 * (0.6 + room))
        y = out.copy()
        for i in range(dl, len(y)):
            y[i] += g * room * 1.15 * y[i - dl]
        acc += y
    acc /= 4
    for dl, g in ((556, 0.5), (441, 0.5)):
        dl = int(dl * R / 44100)
        y = np.zeros_like(acc)
        for i in range(len(acc)):
            xd = acc[i - dl] if i >= dl else 0.0
            yd = y[i - dl] if i >= dl else 0.0
            y[i] = -g * acc[i] + xd + g * yd
        acc = y
    return out * (1 - mix) + acc * mix


def place(parts, d=None):
    """[(start_s, samples, gain)] -> one buffer"""
    n = max(int(s * R) + len(x) for s, x, g in parts)
    if d:
        n = max(n, int(d * R))
    out = np.zeros(n)
    for s, x, g in parts:
        i = int(s * R)
        out[i:i + len(x)] += x * g
    return out


def mx(*parts):
    """sounds of any length together: arrays, or (array, gain)"""
    return place([(0, p[0], p[1]) if isinstance(p, tuple) else (0, p, 1.0) for p in parts])


def norm(x, peak=0.85):
    m = np.max(np.abs(x)) or 1
    return x / m * peak


def sweep(f0, f1, d, curve='exp'):
    n = int(d * R)
    if curve == 'exp':
        return f0 * (f1 / f0) ** np.linspace(0, 1, n)
    return np.linspace(f0, f1, n)


# ---- instruments --------------------------------------------------------------

def organ(f, d, vib=0.0):
    n = int(d * R)
    ff = f * (1 + vib * np.sin(2 * np.pi * 5.5 * np.arange(n) / R))
    x = osc(ff, d, 'add', [1, 0.7, 0.45, 0.35, 0, 0.25, 0, 0.18])
    return x * env(n, 0.012, 9, 1.0, 0.05)


def theremin(f, d):
    n = int(d * R)
    ff = f * (1 + 0.014 * np.sin(2 * np.pi * 6.0 * np.arange(n) / R))
    return osc(ff, d) * env(n, 0.06, 9, 1.0, 0.12)


def bell(f, d, ratio=3.5, index=3.0, decay=0.6):
    n = int(d * R)
    t = np.arange(n) / R
    mod = np.sin(2 * np.pi * f * ratio * t) * index * np.exp(-t / (decay * 0.5))
    return np.sin(2 * np.pi * f * t + mod) * env(n, 0.002, decay, 0, 0.02)


def marimba(f, d):
    n = int(d * R)
    t = np.arange(n) / R
    x = np.sin(2 * np.pi * f * t) + 0.25 * np.sin(2 * np.pi * f * 4 * t) * np.exp(-t / 0.05) \
        + 0.08 * np.sin(2 * np.pi * f * 9.8 * t) * np.exp(-t / 0.02)
    return x * env(n, 0.002, 0.35, 0, 0.02)


def flute(f, d):
    n = int(d * R)
    ff = f * (1 + 0.006 * np.sin(2 * np.pi * 5 * np.arange(n) / R))
    x = osc(ff, d, 'add', [1, 0.3, 0.12]) + 0.06 * biquad_bp(noise(d), f * 2, 3)
    return x * env(n, 0.04, 9, 1, 0.08)


def calliope(f, d):
    n = int(d * R)
    ff = f * (1 + 0.012 * np.sin(2 * np.pi * 6.5 * np.arange(n) / R))
    x = osc(ff, d, 'add', [1, 0.15, 0.55, 0.1, 0.3, 0.05, 0.15]) + 0.05 * hp(noise(d), 3000)
    return x * env(n, 0.015, 9, 1, 0.04)


def brass(f, d, bright=1.0):
    n = int(d * R)
    x = osc(f, d, 'saw')
    e = env(n, 0.03, 0.4, 0.75, 0.06)
    return lp(x, 300 + 2500 * bright * e) * e


def epiano(f, d):
    n = int(d * R)
    t = np.arange(n) / R
    mod = np.sin(2 * np.pi * f * t) * 1.6 * np.exp(-t / 0.25)
    x = np.sin(2 * np.pi * f * t + mod) + 0.15 * np.sin(2 * np.pi * f * 14 * t) * np.exp(-t / 0.03)
    return x * env(n, 0.003, 0.9, 0.0, 0.05)


def accordion(f, d):
    n = int(d * R)
    t = np.arange(n) / R
    x = 0.5 * osc(f * 1.003, d, 'saw') + 0.5 * osc(f * 0.997, d, 'saw')
    x = lp(x, 2600) * (1 + 0.18 * np.sin(2 * np.pi * 5.5 * t))
    return x * env(n, 0.03, 9, 1, 0.05)


def lead(f, d, wave='square', cut=2600):
    n = int(d * R)
    x = osc(f, d, wave)
    e = env(n, 0.005, 0.25, 0.6, 0.04)
    return lp(x, 400 + cut * e) * e


def bass(f, d, wave='saw'):
    n = int(d * R)
    x = osc(f, d, 'sine') * 0.7 + 0.4 * lp(osc(f, d, wave), 700)
    return x * env(n, 0.005, 0.5, 0.5, 0.04)


def pluck(f, d, damp=0.996):
    n = int(d * R)
    p = max(2, int(R / f))
    buf = RNG.uniform(-1, 1, p)
    out = np.empty(n)
    for i in range(n):
        out[i] = buf[i % p]
        buf[i % p] = damp * 0.5 * (buf[i % p] + buf[(i + 1) % p])
    return out


# drums
def kick(d=0.35, f0=120, f1=42):
    n = int(d * R)
    return np.sin(phase(sweep(f0, f1, d), n)) * env(n, 0.001, 0.12, 0, 0.02)


def snare(d=0.22):
    n = int(d * R)
    return (0.6 * hp(noise(d), 1200) * env(n, 0.001, 0.08) + 0.4 * osc(190, d) * env(n, 0.001, 0.05))


def hat(d=0.06, open_=False):
    n = int(d * R)
    return hp(noise(d), 6000) * env(n, 0.001, 0.12 if open_ else 0.02)


def tom(f=110, d=0.3):
    n = int(d * R)
    return np.sin(phase(sweep(f * 1.4, f, d), n)) * env(n, 0.002, 0.15)


def bongo(f=320, d=0.15):
    n = int(d * R)
    return np.sin(phase(sweep(f * 1.15, f, d), n)) * env(n, 0.001, 0.05)


def shaker(d=0.08):
    n = int(d * R)
    return biquad_bp(noise(d), 5500, 1.5) * env(n, 0.01, 0.03)


def crash(d=1.2):
    n = int(d * R)
    return hp(noise(d), 4000) * env(n, 0.002, 0.45)


# ---- a little sequencer ------------------------------------------------------------

class Song:
    def __init__(self, bpm, beats):
        self.bpm = bpm
        self.beat = 60.0 / bpm
        self.len = beats * self.beat
        self.parts = []

    def note(self, at, dur, inst, f, gain=1.0):
        self.parts.append((at * self.beat, inst(f, dur * self.beat), gain))

    def hit(self, at, x, gain=1.0):
        self.parts.append((at * self.beat, x, gain))

    def render(self, rev=(0.4, 0.2)):
        n = int(self.len * R)
        out = np.zeros(n + R * 3)
        for s, x, g in self.parts:
            i = int(s * R)
            out[i:i + len(x)] += x * g
        # what rings past the end comes round to the start: a seamless loop
        out = reverb(out, rev[0], rev[1], tail=0.0) if rev else out
        loop = out[:n].copy()
        tail = out[n:]
        loop[:len(tail)] += tail[:n]
        return norm(loop, 0.8)


def chord(root, kind='m'):
    r = midi(root) if isinstance(root, str) else root
    iv = {'M': (0, 4, 7), 'm': (0, 3, 7), '7': (0, 4, 7, 10), 'm7': (0, 3, 7, 10), 'M7': (0, 4, 7, 11),
          'dim': (0, 3, 6), 'sus': (0, 5, 7), '6': (0, 4, 7, 9)}[kind]
    return [r + i for i in iv]


# ---- the machine's own sounds: the same on every table --------------------------------

def mech():
    s = {}
    n = int(0.09 * R)
    s['flipper'] = norm(lp(noise(0.09), 900) * env(n, 0.001, 0.02) * 0.8 + kick(0.09, 160, 70) * 0.5, 0.7)
    s['sling'] = norm(mx(hp(noise(0.08), 900) * env(int(0.08 * R), 0.001, 0.015), (tom(260, 0.1), 0.6)), 0.8)
    s['wall'] = norm(lp(noise(0.06), 1800) * env(int(0.06 * R), 0.001, 0.012), 0.5)
    s['post'] = norm(mx((tom(180, 0.08), 0.8), (lp(noise(0.05), 1200) * env(int(0.05 * R), 0.001, 0.01), 0.4)), 0.45)
    s['drop'] = norm(mx(hp(noise(0.07), 400) * env(int(0.07 * R), 0.001, 0.02), (tom(140, 0.1), 0.4)), 0.75)
    s['eject'] = norm(mx(kick(0.2, 90, 40), (lp(noise(0.12), 700) * env(int(0.12 * R), 0.001, 0.03), 0.6)), 0.8)
    s['kickback'] = s['eject']
    d = 0.35
    s['launch'] = norm(place([(0, lp(noise(d), sweep(400, 3000, d)) * env(int(d * R), 0.02, 0.15), 0.6),
                              (0, kick(0.15, 100, 50), 0.8)]), 0.8)
    s['spinner'] = norm(hp(noise(0.03), 2500) * env(int(0.03 * R), 0.001, 0.006), 0.4)
    s['bonus'] = norm(bell(1800, 0.06, 2.0, 1.0, 0.03), 0.4)
    return s


# ---- the themes ----------------------------------------------------------------

def kit_zombis():
    s = {}

    def groan(f0, f1, d, vowel='o'):
        n = int(d * R)
        x = osc(sweep(f0, f1, d) * (1 + 0.03 * np.sin(2 * np.pi * 7 * np.arange(n) / R)), d, 'saw')
        return formant(x, vowel) * env(n, 0.08, 9, 1, 0.15)
    s['bumper'] = norm(place([(0, kick(0.18, 140, 55), 0.8), (0, bell(330, 0.3, 1.41, 2, 0.15), 0.4)]), 0.8)
    s['target'] = norm(bell(660, 0.25, 2.76, 2.5, 0.12), 0.6)
    s['lane'] = norm(organ(hz('E5'), 0.12), 0.45)
    s['lanes'] = norm(place([(i * 0.09, organ(hz(n), 0.18), 1) for i, n in enumerate(('A4', 'C5', 'E5', 'A5'))]),
                      0.7)
    s['bank'] = norm(place([(0, bell(hz('A3'), 1.2, 3.5, 4, 0.6), 1), (0.05, groan(160, 110, 0.7), 0.6)]), 0.85)
    s['saucer'] = norm(place([(0, lp(noise(0.5), 500) * env(int(0.5 * R), 0.01, 0.15), 0.8),
                              (0.05, groan(120, 90, 0.5, 'u'), 0.6)]), 0.8)
    s['ramp'] = norm(place([(0, theremin(sweep(300, 900, 0.5), 0.5), 1)]), 0.7)
    s['rampfail'] = norm(theremin(sweep(600, 250, 0.4), 0.4), 0.5)
    s['orbit'] = norm(reverb(theremin(sweep(400, 1200, 0.3), 0.3) + 0.3 * hp(noise(0.3), 2000) *
                             env(int(0.3 * R), 0.05, 0.1), 0.4, 0.3, 0.4), 0.6)
    s['drain'] = norm(reverb(groan(180, 70, 1.4, 'a'), 0.6, 0.35, 0.6), 0.85)
    s['save'] = norm(place([(i * 0.1, bell(hz(n), 0.5, 3.5, 2, 0.3), 1) for i, n in enumerate(('E5', 'A5'))]), 0.7)
    s['mission'] = norm(reverb(place([(0, organ(hz('A2'), 1.4), 0.6), (0, organ(hz('E3'), 1.4), 0.5),
                                      (0.2, groan(200, 140, 1.0), 0.5)]), 0.7, 0.35, 0.8), 0.85)
    s['missiondone'] = norm(reverb(place([(i * 0.14, organ(hz(n), 0.5), 1)
                                          for i, n in enumerate(('A4', 'C5', 'E5', 'A5', 'C6'))]), 0.6, 0.3, 0.7),
                            0.85)
    thunder = lp(noise(2.0), sweep(1500, 200, 2.0)) * env(int(2.0 * R), 0.01, 0.6)
    s['jackpot'] = norm(place([(0, thunder, 0.9), (0.05, groan(150, 260, 0.6, 'a'), 0.5),
                               (0.3, bell(hz('A4'), 1.0), 0.5)]), 0.9)
    s['multiball'] = norm(place([(0, thunder, 0.8)] + [(0.2 + i * 0.35, groan(110 + 30 * i, 90 + 25 * i, 0.9, 'oua'[i]),
                                                        0.6) for i in range(3)]), 0.9)
    s['lock'] = norm(place([(0, hp(noise(0.15), 600) * env(int(0.15 * R), 0.001, 0.05), 0.7),
                            (0.05, bell(hz('D4'), 0.9, 3.5, 3, 0.4), 0.6)]), 0.8)
    s['extra'] = norm(reverb(place([(i * 0.12, bell(hz(n), 0.6), 1) for i, n in enumerate(('A5', 'E5', 'A5', 'C6'))]),
                             0.5, 0.3), 0.8)
    s['tiltwarn'] = norm(organ(hz('G#3'), 0.35) + organ(hz('A3'), 0.35), 0.6)
    s['tilt'] = norm(reverb(organ(sweep(220, 110, 1.0), 1.0), 0.6, 0.3), 0.8)
    s['skill'] = norm(place([(i * 0.07, bell(hz(n), 0.4), 1) for i, n in enumerate(('E5', 'G#5', 'B5', 'E6'))]), 0.75)
    s['gameover'] = norm(reverb(place([(i * 0.45, organ(hz(n), 0.8), 1) for i, n in enumerate(('E4', 'C4', 'A3'))] +
                                      [(1.2, groan(140, 60, 1.6), 0.7)]), 0.7, 0.4, 1.0), 0.85)
    s['start'] = norm(reverb(place([(0, groan(90, 150, 1.2, 'u'), 0.7), (0.3, bell(hz('A3'), 1.2), 0.5)]), 0.6,
                             0.35), 0.8)

    def music():
        sg = Song(96, 32)
        prog = [('A2', 'm'), ('F2', 'M'), ('D2', 'm'), ('E2', 'M')] * 2
        for b, (root, kind) in enumerate(prog):
            at = b * 4
            for m in chord(midi(root) + 12, kind):
                sg.note(at, 4, lambda f, d: organ(f, d, 0.004), hz(m), 0.18)
            sg.note(at, 2, bass, hz(root), 0.5)
            sg.note(at + 2, 2, bass, hz(root), 0.4)
            sg.hit(at, kick(0.4, 90, 40), 0.6)
            sg.hit(at + 0.6, kick(0.3, 80, 40), 0.35)
        mel = [('A4', 2), ('C5', 1), ('B4', 1), ('E4', 4), ('F4', 2), ('A4', 1), ('G#4', 1), ('E4', 4),
               ('D5', 2), ('C5', 1), ('A4', 1), ('F4', 4), ('E4', 1), ('G#4', 1), ('B4', 2), ('E5', 4)]
        at = 0
        for n, d in mel:
            sg.note(at, d * 0.95, theremin, hz(n), 0.32)
            at += d
        return sg.render((0.7, 0.35))
    s['music'] = music()
    return s


def kit_generic():
    s = {}
    for name, f in (('bumper', 520), ('target', 880), ('lane', 1320), ('saucer', 300), ('ramp', 660), ('orbit', 990)):
        s[name] = norm(bell(f, 0.3, 2.0, 2.0, 0.15), 0.6)
    s['lanes'] = norm(place([(i * 0.08, bell(hz(n), 0.25), 1) for i, n in enumerate(('C5', 'E5', 'G5', 'C6'))]), 0.7)
    s['bank'] = s['lanes']
    s['rampfail'] = norm(lead(sweep(500, 200, 0.3), 0.3), 0.5)
    for k in ('drain', 'save', 'mission', 'missiondone', 'jackpot', 'multiball', 'lock', 'extra', 'tiltwarn', 'tilt',
              'skill', 'gameover', 'start'):
        s[k] = s['lanes']
    sg = Song(120, 16)
    for b in range(4):
        sg.note(b * 4, 4, bass, hz(['C2', 'A1', 'F1', 'G1'][b]), 0.5)
    s['music'] = sg.render(None)
    return s


KITS = {'zombis': kit_zombis}


def arp(inst, notes, step, d, gain=1.0):
    return norm(place([(i * step, inst(hz(n), d), gain) for i, n in enumerate(notes)]), 0.8)


def melodic(inst, sc):
    """the sounds every theme plays with its own instrument; sc is its scale
    (eight notes, low to high)"""
    s = {}
    s['lane'] = norm(inst(hz(sc[4]), 0.14), 0.45)
    s['target'] = norm(inst(hz(sc[5]), 0.22), 0.6)
    s['lanes'] = arp(inst, sc[0:8:2], 0.08, 0.2)
    s['skill'] = arp(inst, [sc[2], sc[4], sc[6], sc[7]], 0.07, 0.3)
    s['extra'] = norm(reverb(arp(inst, [sc[7], sc[4], sc[7], sc[7]], 0.12, 0.4), 0.5, 0.3), 0.8)
    s['save'] = arp(inst, [sc[4], sc[7]], 0.1, 0.4)
    s['missiondone'] = norm(reverb(arp(inst, [sc[0], sc[2], sc[4], sc[7], sc[7]], 0.13, 0.45), 0.5, 0.3), 0.85)
    s['lock'] = arp(inst, [sc[0], sc[4]], 0.12, 0.5)
    s['rampfail'] = norm(inst(hz(sc[1]), 0.3), 0.5)
    s['tiltwarn'] = norm(mx(inst(hz(sc[0]), 0.35), inst(hz(sc[0]) * 1.06, 0.35)), 0.6)
    s['gameover'] = norm(reverb(arp(inst, [sc[4], sc[2], sc[1], sc[0]], 0.35, 0.7), 0.6, 0.35), 0.85)
    return s


def kit_robots():
    sc = ['D4', 'E4', 'F4', 'G4', 'A4', 'C5', 'D5', 'F5']
    syn = lambda f, d: lead(f, d, 'square', 3000)
    s = melodic(syn, sc)

    def laser(f0, f1, d):
        n = int(d * R)
        return osc(sweep(f0, f1, d), d, 'square') * env(n, 0.002, d * 0.5)
    s['bumper'] = norm(mx((kick(0.15, 160, 60), 0.7), (laser(1800, 300, 0.12), 0.5)), 0.8)
    s['bank'] = norm(place([(i * 0.05, laser(400 + i * 300, 1600 + i * 400, 0.1), 1) for i in range(6)]), 0.8)
    s['saucer'] = norm(mx(lp(noise(0.4), sweep(3000, 300, 0.4)) * env(int(0.4 * R), 0.01, 0.2), (laser(800, 200, 0.4), 0.4)), 0.8)
    s['ramp'] = norm(laser(300, 2400, 0.45), 0.6)
    s['orbit'] = norm(reverb(laser(600, 3000, 0.3), 0.4, 0.3, 0.4), 0.6)
    ring = lambda f, d: osc(f, d) * osc(f * 0.37, d, 'square') * env(int(d * R), 0.01, 9, 1, 0.05)
    s['drain'] = norm(reverb(mx(laser(1200, 80, 1.0), (ring(220, 1.0), 0.4)), 0.5, 0.3), 0.85)
    s['mission'] = norm(reverb(place([(i * 0.12, syn(hz(n), 0.25), 1) for i, n in enumerate(('D4', 'A4', 'D5', 'F5', 'A5'))]),
                               0.5, 0.3), 0.85)
    s['jackpot'] = norm(reverb(place([(0, laser(200, 4000, 0.6), 0.6), (0.1, crash(1.2), 0.5)] +
                                     [(0.2 + i * 0.08, syn(hz(n), 0.3), 0.7) for i, n in enumerate(('D5', 'F5', 'A5', 'D6'))]),
                               0.5, 0.3), 0.9)
    s['multiball'] = norm(place([(i * 0.25, laser(300 + i * 200, 2000 + i * 500, 0.3), 1) for i in range(4)] +
                                [(1.0, crash(1.0), 0.6)]), 0.9)
    s['tilt'] = norm(mx(laser(800, 60, 1.2), (ring(110, 1.2), 0.5)), 0.85)
    s['start'] = norm(place([(i * 0.5, bell(1200, 0.3, 2, 1, 0.1), 1) for i in range(3)] +
                            [(1.5, laser(200, 3000, 0.8), 1), (1.5, lp(noise(1.2), sweep(400, 4000, 1.2)) *
                                                                env(int(1.2 * R), 0.05, 0.6), 0.6)]), 0.85)

    def music():
        sg = Song(124, 32)
        prog = [('D3', 'm7'), ('Bb2', 'M7'), ('C3', 'M'), ('A2', 'm7')] * 2
        for b, (root, kind) in enumerate(prog):
            ch = chord(midi(root) + 12, kind)
            for k in range(16):
                sg.note(b * 4 + k * 0.25, 0.22, lambda f, d: lead(f, d, 'saw', 1800), hz(ch[k % len(ch)] + 12 * (k // 8)), 0.16)
            for k in range(8):
                sg.note(b * 4 + k * 0.5, 0.45, lambda f, d: bass(f, d, 'square'), hz(midi(root) - 12 * (k % 2 == 1) + 12), 0.4)
            for k in range(4):
                sg.hit(b * 4 + k, kick(0.25, 140, 45), 0.6)
                sg.hit(b * 4 + k + 0.5, hat(0.05), 0.25)
            sg.hit(b * 4 + 1, snare(), 0.4)
            sg.hit(b * 4 + 3, snare(), 0.4)
        mel = [('A4', 1.5), ('G4', 0.5), ('F4', 1), ('D4', 1), ('C5', 2), ('A4', 2), ('G4', 1.5), ('F4', 0.5), ('E4', 1),
               ('C4', 1), ('E4', 2), ('A4', 2)]
        at = 16
        for n, d in mel:
            sg.note(at, d * 0.9, syn, hz(n), 0.22)
            at += d
        return sg.render((0.4, 0.2))
    s['music'] = music()
    return s


def kit_jungla():
    sc = ['C4', 'D4', 'E4', 'G4', 'A4', 'C5', 'D5', 'E5']
    s = melodic(marimba, sc)

    def bird(f, d):
        n = int(d * R)
        t = np.arange(n) / R
        ff = f * (1 + 0.25 * np.sin(2 * np.pi * 22 * t)) * (1 + 0.4 * t / d)
        return osc(ff, d) * env(n, 0.005, d * 0.4)
    s['bumper'] = norm(mx((bongo(240, 0.18), 0.9), (marimba(hz('G4'), 0.2), 0.4)), 0.8)
    s['bank'] = norm(place([(i * 0.08, bongo(200 + 60 * (i % 3), 0.15), 1) for i in range(8)] + [(0.3, bird(2600, 0.3), 0.5)]), 0.8)
    s['saucer'] = norm(mx(tom(80, 0.6), (formant(osc(sweep(140, 100, 0.5), 0.5, 'saw'), 'o') * env(int(0.5 * R), 0.05, 0.3), 0.6)), 0.8)
    s['ramp'] = norm(place([(i * 0.06, marimba(hz(n), 0.3), 1) for i, n in enumerate(('C5', 'D5', 'E5', 'G5', 'A5', 'C6'))]), 0.7)
    s['orbit'] = norm(place([(0, bird(2200, 0.25), 1), (0.2, bird(2800, 0.25), 1)]), 0.6)
    s['drain'] = norm(reverb(mx(tom(70, 1.0), (formant(osc(sweep(220, 90, 1.2), 1.2, 'saw'), 'a') * env(int(1.2 * R), 0.05, 0.6), 0.5)),
                             0.6, 0.3), 0.85)
    s['mission'] = norm(place([(i * 0.18, tom(100 + (i % 2) * 40, 0.3), 1) for i in range(8)] + [(0.2, flute(hz('E5'), 1.2), 0.5)]), 0.85)
    s['jackpot'] = norm(reverb(place([(i * 0.06, marimba(hz(n), 0.5), 1) for i, n in enumerate(('C5', 'E5', 'G5', 'C6', 'E6', 'G6'))] +
                                     [(0, bird(3000, 0.4), 0.5), (0.4, crash(1.0), 0.3)]), 0.5, 0.3), 0.9)
    s['multiball'] = norm(place([(i * 0.11, tom(60 + (i * 37) % 80, 0.25), 1) for i in range(16)] +
                                [(1.0, formant(osc(sweep(300, 500, 0.8), 0.8, 'saw'), 'u') * env(int(0.8 * R), 0.05, 0.5), 0.6)]), 0.9)
    s['tilt'] = norm(mx(tom(50, 1.2), (lp(noise(1.2), 300) * env(int(1.2 * R), 0.01, 0.5), 0.8)), 0.85)
    s['start'] = norm(place([(0, bird(2400, 0.3), 1), (0.35, bird(3000, 0.25), 1), (0.6, bird(2000, 0.4), 1)] +
                            [(0.8 + i * 0.15, bongo(260, 0.15), 1) for i in range(6)]), 0.85)

    def music():
        sg = Song(112, 32)
        prog = ['C3', 'A2', 'F2', 'G2'] * 2
        ost = [0, 7, 12, 7, 4, 7, 9, 7]
        for b, root in enumerate(prog):
            r = midi(root) + 12
            for k in range(8):
                sg.note(b * 4 + k * 0.5, 0.5, marimba, hz(r + ost[k] + (0 if root != 'A2' else 0)), 0.25)
            sg.note(b * 4, 2, bass, hz(root), 0.35)
            sg.note(b * 4 + 2.5, 1.5, bass, hz(root), 0.3)
            for k in range(8):
                sg.hit(b * 4 + k * 0.5, shaker(), 0.18)
            for at, f in ((0, 220), (0.75, 330), (1.5, 260), (2, 220), (2.75, 330), (3.5, 300)):
                sg.hit(b * 4 + at, bongo(f, 0.15), 0.35)
            sg.hit(b * 4, tom(90, 0.4), 0.4)
        mel = [('E5', 1), ('G5', 1), ('A5', 2), ('G5', 1), ('E5', 1), ('D5', 2), ('C5', 1), ('D5', 1), ('E5', 2),
               ('G4', 2), ('A4', 2)]
        at = 16
        for n, d in mel:
            sg.note(at, d * 0.9, flute, hz(n), 0.25)
            at += d
        return sg.render((0.5, 0.25))
    s['music'] = music()
    return s


def kit_circo():
    sc = ['C5', 'D5', 'E5', 'F5', 'G5', 'A5', 'B5', 'C6']
    s = melodic(calliope, sc)

    def slide(f0, f1, d):
        n = int(d * R)
        return osc(sweep(f0, f1, d), d) * env(n, 0.02, 9, 1, 0.05)

    def honk(f, d):
        n = int(d * R)
        return formant(osc(f, d, 'square'), 'a') * env(n, 0.01, 9, 1, 0.03)
    s['bumper'] = norm(mx(honk(330, 0.12), (tom(200, 0.1), 0.6)), 0.75)
    s['bank'] = norm(place([(i * 0.05, snare(0.08), 0.6) for i in range(12)] + [(0.6, crash(0.8), 0.7)]), 0.85)
    s['saucer'] = norm(mx(slide(1200, 300, 0.4), (kick(0.3, 100, 40), 0.6)), 0.75)
    s['ramp'] = norm(slide(400, 1800, 0.5), 0.7)
    s['orbit'] = norm(place([(0, slide(600, 1500, 0.2), 1), (0.2, slide(1500, 600, 0.2), 1)]), 0.6)
    s['drain'] = norm(place([(i * 0.3, brass(hz(n), 0.32), 1) for i, n in enumerate(('E4', 'Eb4', 'D4'))] +
                            [(0.9, brass(sweep(hz('Db4'), hz('Bb3'), 1.0), 1.0), 1)]), 0.85)
    s['mission'] = norm(place([(i * 0.04, snare(0.06), 0.5) for i in range(30)] + [(1.2, crash(1.0), 0.7)] +
                              [(1.2, brass(hz('C5'), 0.6), 0.6), (1.2, brass(hz('E5'), 0.6), 0.5)]), 0.85)
    s['jackpot'] = norm(place([(i * 0.12, brass(hz(n), 0.25 if i < 3 else 0.7), 1)
                               for i, n in enumerate(('G4', 'C5', 'E5', 'G5'))] + [(0.36, crash(1.2), 0.6)]), 0.9)
    s['multiball'] = norm(place([(i * 0.1, calliope(hz(n), 0.18), 1) for i, n in
                                 enumerate(('C5', 'E5', 'G5', 'C6', 'G5', 'E5', 'C5', 'E5', 'G5', 'C6'))] + [(1.0, crash(1.0), 0.6)]),
                          0.9)
    s['tilt'] = norm(mx(slide(800, 120, 1.0), (honk(110, 0.6), 0.5)), 0.85)
    s['start'] = norm(place([(i * 0.03, snare(0.05), 0.5) for i in range(25)] +
                            [(0.8, brass(hz(n), 0.3), 1) for n in ('C5', 'E5', 'G5')] + [(0.8, crash(1.0), 0.6)]), 0.85)

    def music():
        sg = Song(168, 48)
        prog = ['C3', 'C3', 'G2', 'G2', 'G2', 'G2', 'C3', 'C3', 'F2', 'F2', 'C3', 'C3', 'G2', 'G2', 'C3', 'C3']
        kinds = {'C3': 'M', 'G2': '7', 'F2': 'M'}
        for b, root in enumerate(prog):
            at = b * 3
            sg.note(at, 1, bass, hz(root), 0.45)
            ch = chord(midi(root) + 12, kinds[root])
            for k in (1, 2):
                for m in ch[:3]:
                    sg.note(at + k, 0.6, calliope, hz(m), 0.08)
            sg.hit(at, kick(0.2, 120, 50), 0.35)
            sg.hit(at + 1, hat(0.05), 0.2)
            sg.hit(at + 2, hat(0.05), 0.2)
        mel = [('E5', 1), ('G5', 1), ('C6', 1), ('B5', 2), ('G5', 1), ('A5', 1), ('G5', 1), ('F5', 1), ('D5', 3),
               ('D5', 1), ('F5', 1), ('B5', 1), ('A5', 2), ('F5', 1), ('G5', 1), ('F5', 1), ('E5', 1), ('C5', 3),
               ('E5', 1), ('G5', 1), ('C6', 1), ('E6', 2), ('D6', 1), ('C6', 1), ('A5', 1), ('F5', 1), ('C6', 3),
               ('B5', 1), ('A5', 1), ('G5', 1), ('F5', 1), ('E5', 1), ('D5', 1), ('C5', 3), ('G4', 3)]
        at = 0
        for n, d in mel:
            sg.note(at, d * 0.85, calliope, hz(n), 0.22)
            at += d
        return sg.render((0.4, 0.2))
    s['music'] = music()
    return s


def kit_casino():
    sc = ['Bb4', 'C5', 'D5', 'F5', 'G5', 'A5', 'Bb5', 'D6']
    s = melodic(epiano, sc)
    coin = lambda f=2400, d=0.4: mx(bell(f, d, 2.76, 1.2, 0.2), (bell(f * 1.5, d, 3.1, 1.0, 0.15), 0.6))
    s['bumper'] = norm(mx((coin(2200, 0.25), 0.7), (hp(noise(0.05), 3000) * env(int(0.05 * R), 0.001, 0.01), 0.5)), 0.75)
    s['target'] = norm(place([(0, hp(noise(0.04), 2000) * env(int(0.04 * R), 0.001, 0.01), 1),
                              (0.06, hp(noise(0.04), 2000) * env(int(0.04 * R), 0.001, 0.01), 0.7)]), 0.6)
    s['bank'] = norm(place([(i * 0.07, coin(2000 + 150 * (i % 5), 0.3), 1) for i in range(10)]), 0.8)
    s['saucer'] = norm(place([(i * (0.04 + i * 0.006), hp(noise(0.02), 2500) * env(int(0.02 * R), 0.001, 0.005), 1)
                              for i in range(20)] + [(1.0, coin(1800, 0.5), 0.8)]), 0.75)
    s['ramp'] = norm(place([(i * 0.05, coin(1600 + i * 200, 0.25), 1) for i in range(6)]), 0.7)
    s['orbit'] = norm(arp(epiano, ['F5', 'A5', 'C6', 'F6'], 0.05, 0.3), 0.6)
    s['drain'] = norm(reverb(place([(i * 0.3, brass(hz(n), 0.35, 0.6), 1) for i, n in enumerate(('A4', 'Ab4', 'G4'))] +
                                   [(0.9, brass(hz('Gb4'), 1.0, 0.6), 1)]), 0.5, 0.3), 0.85)
    s['mission'] = norm(place([(i * 0.06, coin(1500 + 100 * i, 0.3), 0.6) for i in range(8)] +
                              [(0.5, epiano(hz(n), 1.2), 0.5) for n in ('Bb4', 'D5', 'F5', 'A5')]), 0.85)
    s['jackpot'] = norm(place([(i * 0.04, coin(1800 + 250 * (i % 7), 0.3), 1) for i in range(30)] +
                              [(0, brass(hz('Bb4'), 0.8), 0.5), (0, brass(hz('F5'), 0.8), 0.4)]), 0.9)
    s['multiball'] = norm(place([(i * 0.03, coin(1500 + 300 * (i % 8), 0.3), 1) for i in range(50)]), 0.9)
    s['tilt'] = norm(mx(brass(sweep(300, 120, 1.0), 1.0, 0.5), (lp(noise(1.0), 400) * env(int(R), 0.01, 0.4), 0.6)), 0.85)
    s['start'] = norm(place([(i * 0.04, hp(noise(0.02), 2500) * env(int(0.02 * R), 0.001, 0.005), 1) for i in range(24)] +
                            [(1.0, coin(2000, 0.6), 1)]), 0.85)

    def music():
        sg = Song(118, 32)
        prog = [('Bb2', 'M7'), ('G2', 'm7'), ('C3', 'm7'), ('F2', '7')] * 2
        walk = {'Bb2': ['Bb2', 'D3', 'F3', 'A2'], 'G2': ['G2', 'Bb2', 'D3', 'F3'], 'C3': ['C3', 'Eb3', 'G3', 'E3'],
                'F2': ['F2', 'A2', 'C3', 'B2']}
        for b, (root, kind) in enumerate(prog):
            at = b * 4
            for k, n in enumerate(walk[root]):
                sg.note(at + k, 0.9, bass, hz(n), 0.45)
            ch = chord(midi(root) + 12, kind)
            for when in (1.66, 3.66):
                for m in ch:
                    sg.note(at + when, 0.5, epiano, hz(m + 12), 0.1)
            for k in range(4):
                sg.hit(at + k, hat(0.12, True), 0.12)
                sg.hit(at + k + 0.66, hat(0.05), 0.1)
        mel = [('D5', 1.66), ('F5', 0.34), ('A5', 2), ('G5', 1), ('F5', 1), ('D5', 2), ('Eb5', 1.66), ('G5', 0.34),
               ('Bb5', 2), ('A5', 2), ('F5', 2)]
        at = 16
        for n, d in mel:
            sg.note(at, d * 0.95, lambda f, dd: brass(f, dd, 0.45), hz(n), 0.25)
            at += d
        return sg.render((0.5, 0.25))
    s['music'] = music()
    return s


def kit_piratas():
    sc = ['D4', 'E4', 'F4', 'G4', 'A4', 'Bb4', 'C5', 'D5']
    s = melodic(accordion, sc)
    cannon = lambda d=1.0: mx((kick(0.5, 80, 30), 1.0), (lp(noise(d), sweep(2000, 150, d)) * env(int(d * R), 0.002, d * 0.3), 0.8))

    def squawk(f, d):
        n = int(d * R)
        t = np.arange(n) / R
        return formant(osc(f * (1 + 0.3 * np.sin(2 * np.pi * 30 * t)), d, 'saw'), 'a') * env(n, 0.01, d * 0.5)
    s['bumper'] = norm(mx((cannon(0.25), 0.6), (bell(hz('A5'), 0.2, 2.4, 1.5, 0.1), 0.3)), 0.8)
    s['bank'] = norm(place([(0, cannon(1.0), 1), (0.3, cannon(0.8), 0.8)]), 0.85)
    s['saucer'] = norm(mx(lp(noise(0.5), 1200) * env(int(0.5 * R), 0.01, 0.2), (bell(hz('D6'), 0.6, 2.4, 1.5, 0.3), 0.6)), 0.8)
    s['ramp'] = norm(lp(noise(0.6), sweep(300, 3000, 0.6)) * env(int(0.6 * R), 0.1, 0.3), 0.7)
    s['orbit'] = norm(squawk(900, 0.3), 0.6)
    s['drain'] = norm(reverb(mx(lp(noise(1.2), sweep(2500, 200, 1.2)) * env(int(1.2 * R), 0.01, 0.5),
                                (accordion(hz('D3'), 1.0), 0.4)), 0.6, 0.3), 0.85)
    s['mission'] = norm(place([(i * 0.5, bell(hz('A5'), 0.6, 2.4, 2, 0.3), 1) for i in range(2)] + [(1.0, cannon(1.0), 1)]), 0.85)
    s['jackpot'] = norm(place([(0, cannon(1.2), 1)] + [(0.3 + i * 0.05, bell(1800 + 220 * (i % 6), 0.3, 2.76, 1.2, 0.15), 0.6)
                                                       for i in range(16)]), 0.9)
    s['multiball'] = norm(place([(i * 0.35, cannon(0.8), 1) for i in range(3)] + [(1.2, squawk(800, 0.4), 0.6)]), 0.9)
    s['tilt'] = norm(mx(lp(noise(1.2), 400) * env(int(1.2 * R), 0.01, 0.5), (accordion(sweep(150, 90, 1.2), 1.2), 0.5)), 0.85)
    s['start'] = norm(place([(i * 0.4, bell(hz('A5'), 0.5, 2.4, 2, 0.3), 1) for i in range(2)] + [(0.9, squawk(1000, 0.35), 0.8)]),
                      0.85)

    def music():
        sg = Song(132, 48)
        prog = ['D3', 'D3', 'C3', 'C3', 'Bb2', 'Bb2', 'A2', 'A2', 'D3', 'D3', 'C3', 'C3', 'Bb2', 'A2', 'D3', 'D3']
        kinds = {'D3': 'm', 'C3': 'M', 'Bb2': 'M', 'A2': 'M'}
        for b, root in enumerate(prog):
            at = b * 3
            sg.note(at, 1.4, bass, hz(root), 0.45)
            sg.note(at + 1.5, 1.4, bass, hz(midi(root) + 7), 0.35)
            for m in chord(midi(root) + 12, kinds[root]):
                sg.note(at, 1.3, accordion, hz(m), 0.06)
                sg.note(at + 1.5, 1.3, accordion, hz(m), 0.05)
            sg.hit(at, tom(90, 0.3), 0.4)
            sg.hit(at + 1.5, tom(120, 0.2), 0.25)
        mel = [('A4', 1), ('D5', 0.5), ('D5', 1), ('E5', 0.5), ('F5', 1.5), ('E5', 1.5), ('C5', 1), ('E5', 0.5), ('G5', 1.5),
               ('E5', 1.5), ('D5', 1), ('F5', 0.5), ('D5', 1), ('Bb4', 0.5), ('A4', 1.5), ('C#5', 1.5), ('D5', 3),
               ('A4', 1), ('D5', 0.5), ('F5', 1), ('A5', 0.5), ('G5', 1.5), ('E5', 1.5), ('F5', 1), ('D5', 0.5), ('E5', 1),
               ('C#5', 0.5), ('D5', 3), ('A4', 3)]
        at = 0
        for n, d in mel:
            sg.note(at, d * 0.9, accordion, hz(n), 0.22)
            at += d
        return sg.render((0.4, 0.2))
    s['music'] = music()
    return s


KITS.update({'robots': kit_robots, 'jungla': kit_jungla, 'circo': kit_circo, 'casino': kit_casino,
             'piratas': kit_piratas})


def make(t):
    kit = KITS.get(t.theme.get('sound', t.id), kit_generic)()
    s = mech()
    s.update(kit)
    out = {}
    for name in SND_NAMES:
        x = s.get(name)
        if x is None:
            continue
        # a short fade at the end, so nothing clicks
        x = np.asarray(x, np.float64)
        f = min(len(x), 160)
        x[-f:] *= np.linspace(1, 0, f)
        out['s_' + name] = x
    if 'music' in s:
        out['music'] = s['music']
    return out


if __name__ == '__main__':
    # sounds.py <id> <dir>: writes them as WAV, to listen to
    import os
    import sys
    import wave
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import pbtable
    t = pbtable.load(sys.argv[1])
    d = sys.argv[2]
    os.makedirs(d, exist_ok=True)
    for name, x in make(t).items():
        with wave.open(os.path.join(d, name + '.wav'), 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(R)
            w.writeframes((np.clip(x, -1, 1) * 32000).astype('<i2').tobytes())
    print('written to', d)
