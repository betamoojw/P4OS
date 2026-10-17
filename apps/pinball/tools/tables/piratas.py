"""La Isla Pirata: the treasure chest on the right, the cannon ramp on the
left, the barrels' drop targets in the middle."""
from pbtable import Table, Camera, H, mirror_pts

GOLD = 0xFFC040
RED = 0xD02828
SEA = 0x2AA0E0
SAND = 0xE8C890


def M(pts):
    return mirror_pts(pts)


def build():
    t = Table('piratas', dict(es='La Isla Pirata', en='Pirate Island', de='Pirateninsel'), order=60,
              desc=dict(es='¡Al abordaje! Hundí la flota, seguí el mapa y desenterrá el cofre.',
                        en='All hands on deck! Sink the fleet, follow the map and dig up the chest.',
                        de='Alle Mann an Deck! Versenke die Flotte, folge der Karte und grab die Truhe aus.'))
    t.theme.update(dmd=0xFF8A20, bg=0x06121E, accent=GOLD, panel=0x2A1A0E, text=0xFFF0D8,
                   flipper=0xF4E8D0, flipper_rubber=0x8A1A10, cabinet=0x24160C, wood=0x5A3418, rail=0xD8B878,
                   plastic=RED, field=(0x1A5A8A, 0x8A6A3A), gi=GOLD, ambient=0x142838, apron=0x3A2412,
                   logo=(GOLD, 0x3A1A08))
    t.standard_shell()
    t.standard_lower(sling_color=RED)
    t.top_lanes(colors=[GOLD, SEA, GOLD])

    # three bumpers: cannonball stacks, in the corners of the bay
    t.bumper('bumper1', 110, 232, cap=RED, body=0x3A2A1A)
    t.bumper('bumper2', 372, 232, cap=GOLD, body=0x3A2A1A)
    t.bumper('bumper3', 384, 300, cap=RED, body=0x3A2A1A)

    # THE GHOST SHIP (Pirates of the Caribbean): it sails to and fro across
    # the bay; four hits sink it, and it opens the sword duel
    t.toy('ship', 'mover', 158, 300, 25, model='ship', x1=300, y1=300, period=9.0)
    t.lamp('toy', 229, 352, 'arrow', 18, GOLD, 0)

    # THE KRAKEN: now and then its tentacles grab the ball and fling it
    t.toy('kraken', 'magnet', 384, 548, 40, model='kraken', x1=2700, a0=-128)
    t.deco('tentacles', 384, 548, s=1.0)
    t.lamp('magnet', 384, 548, 'circle', 22, 0x30C080)

    # THE CHEST: two hits open its lid; a ball in it while it is open is
    # locked; two locks and the treasure pours out (multiball)
    t.toy('chest', 'bash', 240, 452, 18, model='chestlid')
    t.saucer('chestpen', 240, 452, eject_deg=95, eject_speed=1600, r=14, role='lock')
    t.lamp('lockpen', 240, 492, 'arrow', 16, GOLD, 0)

    # the cannon's ramp, left
    t.wall(M([(296, 520), (300, 472)]), h=30, style='plastic', thick=5)
    t.wall(M([(350, 520), (346, 472)]), h=30, style='plastic', thick=5)
    t.post(*M([(296, 522)])[0], r=5, name='rampLpost')
    t.post(*M([(350, 522)])[0], r=5, name='rampRpost')
    path = [(323, 472, 0), (322, 430, 15), (318, 388, 32), (322, 340, 48), (340, 296, 60), (372, 264, 68),
            (408, 250, 70), (440, 262, 68), (455, 300, 62), (459, 370, 54), (458, 450, 46), (452, 528, 35),
            (441, 604, 23), (431, 672, 11), (425, 712, 3), (425, 730, 0)]
    path = [(482 - x, y, z) for x, y, z in path]
    t.ramp('ramp1', M([(346, 472)])[0], M([(300, 472)])[0], path, color=RED, style='plastic')
    t.lamp('ramp1', *M([(323, 545)])[0], shape='arrow', size=22, color=RED, angle=0)

    # the barrels: drop targets in the middle, facing the flippers
    t.wall([(232, 536), (238, 522), (281, 508), (324, 522), (330, 536)], h=34, style='rail', thick=5)
    t.post(232, 538, r=5, name='dropLpost')
    t.post(330, 538, r=5, name='dropRpost')
    t.drops('drop', (318, 534), (244, 534), 3, color=0xA06A30)
    for i, x in enumerate((306, 281, 256)):
        t.lamp('drop%d' % (i + 1), x, 564, 'triangle', 13, GOLD, 0)

    # two standups on the left orbit's wall
    t.target('target1', (51, 318), (51, 352), color=SEA)
    t.target('target2', (51, 364), (51, 398), color=SEA)
    t.lamp('target1', 72, 335, 'circle', 12, SEA)
    t.lamp('target2', 72, 381, 'circle', 12, SEA)

    # the captain's cabin: the sword duel
    t.saucer('saucer', 372, 420, eject_deg=128, eject_speed=1650)
    t.lamp('saucer', 344, 452, 'circle', 16, GOLD)
    t.lamp('video', 330, 420, 'star', 18, RED)
    t.lamp('lock', 196, 575, 'arrow', 18, RED, -20)
    t.lamp('extraball', 350, 520, 'circle', 14, GOLD)
    t.lamp('jackpot', 255, 610, 'star', 26, GOLD)
    for i, (x, y) in enumerate([(160, 668), (200, 646), (241, 638), (282, 646), (322, 668)]):
        t.lamp('mission%d' % (i + 1), x, y, 'circle', 14, GOLD if i % 2 == 0 else SEA)
    for i, (x, y) in enumerate([(196, 720), (226, 734), (256, 734), (286, 720)]):
        t.lamp('x%d' % (i + 2), x, y, 'rect', 13, GOLD)
    t.flasher('flash1', 240, 410, z=60, color=GOLD, size=80)
    t.flasher('flash2', 100, 170, z=70, color=RED, size=80)


    # the toys stand where the ball never goes: the cabinet's corners and
    # the back shelf, the slingshots' tops, the apron
    t.deco('palm', 52, 46, s=1.2, z=60)
    t.deco('ship', 260, -6, s=1.3, z=60)
    t.deco('chest', 468, 46, s=1.1, z=60)
    t.deco('skull', 91, 814, s=0.8, z=30)
    t.deco('anchor', 391, 814, s=0.8, z=30)
    t.deco('barrels', 56, 1016, s=0.8, z=30)
    t.deco('chest', 426, 1016, s=0.8, z=30, rot=90)

    t.mission(dict(es='Hundí la flota', en='Sink the fleet', de='Versenke die Flotte'), 'drop', 6, 60, 300000)
    t.mission(dict(es='Fuego de cañón', en='Cannon fire', de='Kanonenfeuer'), 'ramp', 3, 60, 400000)
    t.mission(dict(es='Seguí el mapa', en='Follow the map', de='Folge der Karte'), 'orbit', 3, 60, 400000)
    t.mission(dict(es='Abordaje', en='Boarding party', de='Entern'), 'bumper', 12, 45, 250000)
    t.mission(dict(es='Hundí al barco fantasma', en='Sink the ghost ship', de='Versenke das Geisterschiff'), 'toy',
              8, 60, 350000)

    t.feat('bash', toy='ship', hits=4, points=200000, lights='video')
    t.feat('video', game='duel', saucer='saucer', light='bash')
    t.feat('magnet', toy='kraken', every=20, on=7, points=60000, text='kraken')
    t.feat('bash', toy='chest', hits=2, points=50000, hit='chesthit', done='chestopen', back='chestshut')
    t.feat('lockpen', saucer='chestpen', toy='chest', lit=0)
    t.text('toyhit', '¡FUEGO!', 'FIRE!', 'FEUER!')
    t.text('toydown', '¡A PIQUE!', 'SUNK!', 'VERSENKT!')
    t.text('toyback', 'BARCO A LA VISTA', 'SHIP AHOY', 'SCHIFF IN SICHT')
    t.text('kraken', '¡EL KRAKEN!', 'THE KRAKEN!', 'DER KRAKE!')
    t.text('chesthit', 'EL COFRE', 'THE CHEST', 'DIE TRUHE')
    t.text('chestopen', '¡COFRE ABIERTO!', 'CHEST OPEN!', 'TRUHE OFFEN!')
    t.text('chestshut', 'COFRE CERRADO', 'CHEST SHUT', 'TRUHE ZU')

    t.text('jackpot', '¡DOBLONES!', 'DOUBLOONS!', 'DUBLONEN!')
    t.text('multiball', 'AL ABORDAJE', 'ALL HANDS', 'ALLE MANN')
    t.text('wizard', 'EL TESORO MALDITO', 'CURSED TREASURE', 'VERFLUCHTER SCHATZ')
    t.text('lock', 'BARCO CAPTURADO', 'SHIP CAPTURED', 'SCHIFF GEKAPERT')
    t.text('drain', 'AL AGUA', 'MAN OVERBOARD', 'MANN ÜBER BORD')
    t.text('start', 'LEVEN ANCLAS', 'WEIGH ANCHOR', 'ANKER LICHTEN')
    t.camera = Camera(64, 1700, H * 0.5)
    return t
