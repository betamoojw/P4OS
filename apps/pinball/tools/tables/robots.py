"""Robots en Órbita: a space station of robots. The rocket ramp on the
left, the reactor's drop targets on the right, the launch pad saucer."""
from pbtable import Table, Camera, H, mirror_pts

CYAN = 0x30E0FF
MAGENTA = 0xFF40C0
ORANGE = 0xFF9A30
STEEL = 0xC8D4E0


def M(pts):
    return mirror_pts(pts)


def build():
    t = Table('robots', dict(es='Robots en Órbita', en='Robots in Orbit', de='Roboter im Orbit'), order=20,
              desc=dict(es='Armá la flota: cargá el reactor, despegá por la rampa y salvá la estación.',
                        en='Build the fleet: charge the reactor, blast off up the ramp and save the station.',
                        de='Bau die Flotte: lade den Reaktor, starte über die Rampe und rette die Station.'))
    t.theme.update(dmd=0x40E8FF, bg=0x060A1E, accent=CYAN, panel=0x101A3A, text=0xE0F4FF,
                   flipper=0xF4F6FA, flipper_rubber=0x2050D0, cabinet=0x141A2C, wood=0x1C2440, rail=0xC0CCDC,
                   plastic=MAGENTA, field=(0x182060, 0x060818), gi=CYAN, ambient=0x182040, apron=0x202C50,
                   logo=(CYAN, 0x08102A))
    t.standard_shell()
    t.standard_lower(sling_color=CYAN)
    t.top_lanes(colors=[CYAN, MAGENTA, CYAN])

    # the bumpers: three reactors, a little to the right
    t.bumper('bumper1', 230, 270, cap=CYAN, body=0x283048)
    t.bumper('bumper2', 318, 262, cap=MAGENTA, body=0x283048)
    t.bumper('bumper3', 278, 346, cap=CYAN, body=0x283048)

    # the rocket ramp, left: the mirror of the crypt's
    t.wall(M([(296, 520), (300, 472)]), h=30, style='plastic', thick=5)
    t.wall(M([(350, 520), (346, 472)]), h=30, style='plastic', thick=5)
    t.post(*M([(296, 522)])[0], r=5, name='rampLpost')
    t.post(*M([(350, 522)])[0], r=5, name='rampRpost')
    path = [(323, 472, 0), (324, 430, 16), (328, 380, 36), (338, 320, 54), (360, 268, 64), (396, 242, 68),
            (432, 250, 66), (452, 292, 60), (458, 360, 54), (458, 440, 46), (452, 520, 36), (440, 600, 24),
            (430, 670, 12), (425, 712, 3), (425, 730, 0)]
    path = [(482 - x, y, z) for x, y, z in path]
    # the mouth, crossed going up (a -> b left to right)
    t.ramp('ramp1', M([(346, 472)])[0], M([(300, 472)])[0], path, color=MAGENTA, style='plastic')
    t.lamp('ramp1', *M([(323, 545)])[0], shape='arrow', size=22, color=MAGENTA, angle=0)

    # the reactor's drop targets, right
    t.wall(M([(126, 574), (206, 548)]), h=34, style='rail', thick=5)
    t.post(*M([(126, 576)])[0], r=5, name='dropLpost')
    t.post(*M([(206, 550)])[0], r=5, name='dropRpost')
    t.drops('drop', M([(132, 585)])[0], M([(202, 562)])[0], 3, color=ORANGE)
    for i, (x, y) in enumerate(((146, 606), (168, 599), (190, 592))):
        t.lamp('drop%d' % (i + 1), *M([(x, y)])[0], shape='triangle', size=13, color=ORANGE, angle=18)

    # two standups on the right orbit's wall
    t.target('target1', (431, 318), (431, 352), color=CYAN)
    t.target('target2', (431, 364), (431, 398), color=CYAN)
    t.lamp('target1', 410, 335, 'circle', 12, CYAN)
    t.lamp('target2', 410, 381, 'circle', 12, CYAN)

    # the launch pad
    t.saucer('saucer', 242, 470, eject_deg=62, eject_speed=1700)
    t.lamp('saucer', 212, 505, 'circle', 16, ORANGE)
    t.lamp('lock', 196, 575, 'arrow', 18, MAGENTA, -20)
    t.lamp('extraball', 242, 520, 'circle', 14, 0xFFD040)
    t.lamp('jackpot', 241, 590, 'star', 26, 0xFFE060)
    for i, (x, y) in enumerate([(160, 668), (200, 646), (241, 638), (282, 646), (322, 668)]):
        t.lamp('mission%d' % (i + 1), x, y, 'circle', 14, CYAN if i % 2 == 0 else MAGENTA)
    for i, (x, y) in enumerate([(196, 720), (226, 734), (256, 734), (286, 720)]):
        t.lamp('x%d' % (i + 2), x, y, 'rect', 13, ORANGE)
    t.flasher('flash1', 90, 160, z=70, color=CYAN, size=80)
    t.flasher('flash2', 392, 170, z=70, color=MAGENTA, size=80)


    # the toys stand where the ball never goes: the cabinet's corners and
    # the back shelf, the slingshots' tops, the apron
    t.deco('robot', 52, 46, s=1.1, z=60, rot=15)
    t.deco('rocket', 260, -6, s=1.4, z=60)
    t.deco('planet', 468, 46, s=1.0, z=60)
    t.deco('dish', 91, 814, s=0.8, z=30, rot=30)
    t.deco('robot', 391, 814, s=0.7, z=30, rot=-20)
    t.deco('ufo', 56, 1016, s=0.8, z=30)
    t.deco('robot', 426, 1016, s=0.8, z=30)

    t.mission(dict(es='Cargá el reactor', en='Charge the reactor', de='Lade den Reaktor'), 'bumper', 12, 45, 250000)
    t.mission(dict(es='Despegue', en='Lift-off', de='Abheben'), 'ramp', 3, 60, 400000)
    t.mission(dict(es='Órbita baja', en='Low orbit', de='Niedriger Orbit'), 'orbit', 3, 60, 400000)
    t.mission(dict(es='Arreglá los circuitos', en='Fix the circuits', de='Repariere die Schaltkreise'), 'drop', 6,
              60, 300000)
    t.mission(dict(es='Cazá al robot rebelde', en='Catch the rogue robot', de='Fang den Rebellen-Roboter'), 'target',
              4, 60, 350000)

    t.text('jackpot', 'MEGA VOLTIOS', 'MEGAVOLTS', 'MEGAVOLT')
    t.text('multiball', 'LLUVIA DE ROBOTS', 'ROBOT STORM', 'ROBOTERSTURM')
    t.text('wizard', 'HIPERESPACIO', 'HYPERSPACE', 'HYPERRAUM')
    t.text('lock', 'ROBOT A BORDO', 'ROBOT ON BOARD', 'ROBOTER AN BORD')
    t.text('drain', 'SIN ENERGÍA', 'POWER DOWN', 'ENERGIE WEG')
    t.text('start', 'CUENTA REGRESIVA', 'COUNTDOWN', 'COUNTDOWN')
    t.camera = Camera(64, 1700, H * 0.5)
    return t
