"""El Gran Circo: under the big top. The human cannonball's ramp on the left,
the three rings of bumpers in the middle, the jugglers' drop targets on the
right orbit's wall."""
from pbtable import Table, Camera, H, mirror_pts

RED = 0xE8243C
GOLD = 0xFFD040
BLUE = 0x3080FF
WHITE = 0xF8F4EC


def M(pts):
    return mirror_pts(pts)


def build():
    t = Table('circo', dict(es='El Gran Circo', en='The Big Top', de='Der Große Zirkus'), order=40,
              desc=dict(es='¡Pasen y vean! Hacé malabares, cruzá la cuerda floja y volá desde el cañón.',
                        en='Roll up, roll up! Juggle, walk the tightrope and fly from the cannon.',
                        de='Hereinspaziert! Jongliere, balanciere auf dem Seil und flieg aus der Kanone.'))
    t.theme.update(dmd=0xFF9A20, bg=0x200608, accent=GOLD, panel=0x3A0C12, text=0xFFF4E0,
                   flipper=0xFFFFFF, flipper_rubber=0xD01828, cabinet=0x3A0A10, wood=0x6A1018, rail=0xE8D8A0,
                   plastic=RED, field=(0xB01828, 0x400810), gi=GOLD, ambient=0x301010, apron=0x4A0C14,
                   logo=(GOLD, 0x500810))
    t.standard_shell()
    t.standard_lower(sling_color=RED)
    t.top_lanes(colors=[RED, GOLD, BLUE])

    # the three rings: two bumpers and, in the middle, the merry-go-round
    t.bumper('bumper1', 150, 252, cap=RED, body=0xF0E8D8)
    t.bumper('bumper2', 330, 252, cap=BLUE, body=0xF0E8D8)
    t.bumper('bumper3', 240, 226, cap=GOLD, body=0xF0E8D8)
    t.toy('carousel', 'disc', 240, 330, 46, model='carousel', a0=2.6)

    # THE HUMAN CANNONBALL (Cirqus Voltaire): the ball coming down the left
    # orbit drops into the cannon; it swings, and the launch button fires
    t.toy('cannon', 'cannon', 64, 626, 19, model='cannon', x1=3300, a0=-115, a1=-45, period=2.2)
    t.lamp('cannon', 92, 676, 'arrow', 16, GOLD, 30)

    # the ringmaster pops up in the middle: two hits send him down
    t.toy('ringmaster', 'bash', 240, 470, 16, model='ringmaster')
    t.lamp('toy', 240, 510, 'arrow', 18, RED, 0)

    # the cannon's ramp, left
    t.wall(M([(296, 520), (300, 472)]), h=30, style='plastic', thick=5)
    t.wall(M([(350, 520), (346, 472)]), h=30, style='plastic', thick=5)
    t.post(*M([(296, 522)])[0], r=5, name='rampLpost')
    t.post(*M([(350, 522)])[0], r=5, name='rampRpost')
    path = [(323, 472, 0), (326, 426, 18), (334, 374, 38), (346, 324, 54), (366, 280, 64), (398, 252, 70),
            (432, 254, 68), (452, 290, 62), (458, 360, 54), (458, 440, 46), (452, 520, 36), (440, 600, 24),
            (430, 670, 12), (425, 712, 3), (425, 730, 0)]
    path = [(482 - x, y, z) for x, y, z in path]
    t.ramp('ramp1', M([(346, 472)])[0], M([(300, 472)])[0], path, color=GOLD, style='plastic')
    t.lamp('ramp1', *M([(323, 545)])[0], shape='arrow', size=22, color=GOLD, angle=0)

    # the jugglers: three drop targets on the right orbit's wall, facing left
    t.drops('drop', (430, 316), (430, 410), 3, color=WHITE)
    for i, y in enumerate((332, 363, 394)):
        t.lamp('drop%d' % (i + 1), 404, y, 'triangle', 12, BLUE, -90)

    # two standups on the left orbit's wall
    t.target('target1', (51, 318), (51, 352), color=RED)
    t.target('target2', (51, 364), (51, 398), color=RED)
    t.lamp('target1', 72, 335, 'circle', 12, RED)
    t.lamp('target2', 72, 381, 'circle', 12, RED)

    # the tent: the darts' video mode
    t.saucer('saucer', 372, 440, eject_deg=120, eject_speed=1700)
    t.lamp('saucer', 350, 478, 'circle', 16, RED)
    t.lamp('video', 380, 482, 'star', 18, GOLD)
    t.lamp('lock', 196, 575, 'arrow', 18, GOLD, -20)
    t.lamp('extraball', 300, 540, 'circle', 14, GOLD)
    t.lamp('jackpot', 241, 600, 'star', 26, GOLD)
    for i, (x, y) in enumerate([(160, 668), (200, 646), (241, 638), (282, 646), (322, 668)]):
        t.lamp('mission%d' % (i + 1), x, y, 'circle', 14, [RED, GOLD, BLUE, GOLD, RED][i])
    for i, (x, y) in enumerate([(196, 720), (226, 734), (256, 734), (286, 720)]):
        t.lamp('x%d' % (i + 2), x, y, 'rect', 13, GOLD)
    t.flasher('flash1', 112, 170, z=70, color=GOLD, size=80)
    t.flasher('flash2', 370, 170, z=70, color=RED, size=80)


    # the toys stand where the ball never goes: the cabinet's corners and
    # the back shelf, the slingshots' tops, the apron
    t.deco('ball', 52, 46, s=1.1, z=60)
    t.deco('tent', 260, -6, s=1.3, z=60)
    t.deco('podium', 468, 46, s=1.1, z=60)
    t.deco('balloons', 91, 814, s=0.7, z=30)
    t.deco('cannon', 391, 814, s=0.7, z=30, rot=-30)
    t.deco('podium', 56, 1016, s=0.8, z=30)
    t.deco('ball', 426, 1016, s=0.8, z=30)

    t.mission(dict(es='Malabares', en='Juggling act', de='Jonglage'), 'drop', 6, 60, 300000)
    t.mission(dict(es='Hombre bala', en='Human cannonball', de='Kanonenkugel-Mensch'), 'ramp', 3, 60, 400000)
    t.mission(dict(es='Los tres anillos', en='Three rings', de='Drei Manegen'), 'bumper', 12, 45, 250000)
    t.mission(dict(es='Cuerda floja', en='Tightrope', de='Hochseil'), 'orbit', 3, 60, 400000)
    t.mission(dict(es='Domá a los leones', en='Tame the lions', de='Zähme die Löwen'), 'target', 4, 60, 350000)
    t.mission(dict(es='La calesita', en='Merry-go-round', de='Karussell'), 'toy', 6, 50, 300000)

    t.feat('cannon', toy='cannon', targets='drop1,drop2,drop3,target1,target2')
    t.feat('disc', toy='carousel')
    t.feat('bash', toy='ringmaster', hits=2, points=120000, lights='video')
    t.feat('video', game='darts', saucer='saucer', light='bash')
    t.text('toyhit', '¡AY!', 'OUCH!', 'AUA!')
    t.text('toydown', '¡FUERA DE PISTA!', 'OUT OF THE RING!', 'RAUS AUS DER MANEGE!')
    t.text('toyback', 'EL PRESENTADOR', 'THE RINGMASTER', 'DER DIREKTOR')
    t.text('cannonin', '¡HOMBRE BALA!', 'HUMAN CANNONBALL!', 'KANONENKUGEL!')
    t.text('cannon', '¡BLANCO!', 'BULLSEYE!', 'VOLLTREFFER!')

    t.text('jackpot', '¡BRAVO!', 'BRAVO!', 'BRAVO!')
    t.text('multiball', 'GRAN FUNCIÓN', 'GRAND SHOW', 'GROSSE SHOW')
    t.text('wizard', 'GRAN FINAL', 'GRAND FINALE', 'GROSSES FINALE')
    t.text('lock', 'ARTISTA EN PISTA', 'STAR IN THE RING', 'STAR IN DER MANEGE')
    t.text('drain', 'SE CAYÓ DEL TRAPECIO', 'FELL OFF THE TRAPEZE', 'VOM TRAPEZ GEFALLEN')
    t.text('start', '¡PASEN Y VEAN!', 'ROLL UP!', 'HEREINSPAZIERT!')
    t.camera = Camera(64, 1700, H * 0.5)
    return t
