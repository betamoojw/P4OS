"""El Templo de la Jungla: the lost temple. The temple's mouth swallows the
ball on the left, the idols' drop targets stand in the middle, the river
ramp winds down the right."""
from pbtable import Table, Camera, H

LEAF = 0x6CE040
ORANGE = 0xFFA020
TEAL = 0x30D0C0
GOLD = 0xFFD050


def build():
    t = Table('jungla', dict(es='El Templo de la Jungla', en='Jungle Temple', de='Dschungeltempel'), order=30,
              desc=dict(es='Cruzá el río, despertá a los ídolos y encontrá el tesoro del templo.',
                        en='Cross the river, wake the idols and find the temple treasure.',
                        de='Überquere den Fluss, wecke die Götzen und finde den Tempelschatz.'))
    t.theme.update(dmd=0xFFB030, bg=0x0C1A0A, accent=ORANGE, panel=0x1C2E14, text=0xF4F0D8,
                   flipper=0xF4ECD8, flipper_rubber=0x1E7A2A, cabinet=0x2A1C10, wood=0x4A3018, rail=0xC8B890,
                   plastic=LEAF, field=(0x2A6A26, 0x0E2A12), gi=ORANGE, ambient=0x1E3020, apron=0x3A2A18,
                   logo=(ORANGE, 0x1A2A0A))
    t.standard_shell()
    t.standard_lower(sling_color=LEAF)
    t.top_lanes(colors=[ORANGE, TEAL, ORANGE])

    # the bumpers: three stone heads, up on the left
    t.bumper('bumper1', 160, 262, cap=ORANGE, body=0x5A5040)
    t.bumper('bumper2', 246, 250, cap=TEAL, body=0x5A5040)
    t.bumper('bumper3', 200, 334, cap=ORANGE, body=0x5A5040)

    # the river ramp, right: up the middle-right and round, down to the right inlane
    t.wall([(296, 520), (300, 472)], h=30, style='plastic', thick=5)
    t.wall([(350, 520), (346, 472)], h=30, style='plastic', thick=5)
    t.post(296, 522, r=5, name='rampLpost')
    t.post(350, 522, r=5, name='rampRpost')
    t.ramp('ramp1', (300, 472), (346, 472),
           [(323, 472, 0), (322, 432, 14), (316, 392, 30), (318, 344, 46), (334, 300, 58), (364, 266, 66),
            (400, 250, 70), (436, 262, 68), (454, 300, 62), (459, 370, 54), (458, 450, 46), (452, 530, 35),
            (441, 605, 23), (431, 672, 11), (425, 712, 3), (425, 730, 0)],
           color=TEAL, style='plastic')
    t.lamp('ramp1', 323, 545, 'arrow', 22, TEAL, 0)

    # the idols: three drop targets in the middle, facing the flippers
    # a roof over the bank, so nothing rests on top of it
    t.wall([(152, 528), (158, 514), (201, 500), (244, 514), (250, 528)], h=34, style='rail', thick=5)
    t.post(152, 530, r=5, name='dropLpost')
    t.post(250, 530, r=5, name='dropRpost')
    t.drops('drop', (238, 526), (164, 526), 3, color=GOLD)
    for i, x in enumerate((226, 201, 176)):
        t.lamp('drop%d' % (i + 1), x, 556, 'triangle', 13, GOLD, 0)

    # two standups on the right orbit's wall
    t.target('target1', (431, 318), (431, 352), color=LEAF)
    t.target('target2', (431, 364), (431, 398), color=LEAF)
    t.lamp('target1', 410, 335, 'circle', 12, LEAF)
    t.lamp('target2', 410, 381, 'circle', 12, LEAF)

    # THE BOULDER (Indiana Jones): it rolls to and fro across the temple's
    # courtyard; three hits stop it and open the Path of Adventure
    t.toy('boulder', 'mover', 182, 428, 22, model='boulder', x1=262, y1=428, period=6.0, s=1.05)
    t.lamp('toy', 222, 470, 'arrow', 18, ORANGE, 0)

    # the temple's mouth, left: up a vine-covered wire to the top lanes
    t.habitrail('temple_wire', [(112, 440, 0), (108, 410, 20), (100, 360, 45), (96, 300, 62), (104, 232, 70),
                                (126, 172, 64), (165, 126, 48), (205, 98, 28), (232, 88, 10), (240, 86, 2)])
    t.saucer('saucer', 112, 440, eject_deg=55, eject_speed=2700, vuk='temple_wire')
    t.lamp('video', 140, 470, 'star', 18, TEAL)

    # an upper flipper on the left, fed by the left orbit
    t.flipper('L', 42, 640, length=55, r0=11, r1=6, rest=28, up=-22)
    t.lamp('saucer', 132, 482, 'circle', 16, ORANGE)
    t.lamp('lock', 286, 575, 'arrow', 18, TEAL, 20)
    t.lamp('extraball', 136, 522, 'circle', 14, GOLD)
    t.lamp('jackpot', 241, 600, 'star', 26, GOLD)
    for i, (x, y) in enumerate([(160, 668), (200, 646), (241, 638), (282, 646), (322, 668)]):
        t.lamp('mission%d' % (i + 1), x, y, 'circle', 14, ORANGE if i % 2 == 0 else TEAL)
    for i, (x, y) in enumerate([(196, 720), (226, 734), (256, 734), (286, 720)]):
        t.lamp('x%d' % (i + 2), x, y, 'rect', 13, GOLD)
    t.flasher('flash1', 112, 400, z=60, color=ORANGE, size=80)
    t.flasher('flash2', 400, 160, z=70, color=TEAL, size=80)


    # the toys stand where the ball never goes: the cabinet's corners and
    # the back shelf, the slingshots' tops, the apron
    t.deco('palm', 52, 46, s=1.2, z=60)
    t.deco('temple', 260, -6, s=1.3, z=60)
    t.deco('palm', 468, 46, s=1.1, z=60, rot=50)
    t.deco('idol', 91, 814, s=0.8, z=30)
    t.deco('snake', 391, 814, s=0.8, z=30)
    t.deco('rocks', 56, 1016, s=0.9, z=30)
    t.deco('idol', 426, 1016, s=0.8, z=30)

    t.mission(dict(es='Despertá a los ídolos', en='Wake the idols', de='Wecke die Götzen'), 'drop', 6, 60, 300000)
    t.mission(dict(es='Cruzá el río', en='Cross the river', de='Überquere den Fluss'), 'ramp', 3, 60, 400000)
    t.mission(dict(es='Tambores de guerra', en='War drums', de='Kriegstrommeln'), 'bumper', 12, 45, 250000)
    t.mission(dict(es='Frená la roca', en='Stop the boulder', de='Halte den Felsen auf'), 'toy', 6, 50, 350000)

    t.feat('bash', toy='boulder', hits=3, points=200000, lights='video')
    t.feat('video', game='runner', saucer='saucer', light='bash')
    t.text('toyhit', '¡CUIDADO!', 'LOOK OUT!', 'ACHTUNG!')
    t.text('toydown', 'ROCA FRENADA', 'BOULDER STOPPED', 'FELS GESTOPPT')
    t.text('toyback', '¡OTRA ROCA!', 'ANOTHER BOULDER!', 'NOCH EIN FELS!')
    t.mission(dict(es='Escapá de la piedra', en='Outrun the boulder', de='Entkomme dem Felsen'), 'orbit', 3, 60,
              400000)

    t.text('jackpot', '¡TESORO!', 'TREASURE!', 'SCHATZ!')
    t.text('multiball', 'ESTAMPIDA', 'STAMPEDE', 'STAMPEDE')
    t.text('wizard', 'EL ÍDOLO DE ORO', 'THE GOLDEN IDOL', 'DER GOLDENE GÖTZE')
    t.text('lock', 'TRAMPA ACTIVADA', 'TRAP SPRUNG', 'FALLE AUSGELÖST')
    t.text('drain', 'TE TRAGÓ LA SELVA', 'LOST IN THE JUNGLE', 'IM DSCHUNGEL VERLOREN')
    t.text('start', 'A LA AVENTURA', 'ADVENTURE AWAITS', 'AUF ZUM ABENTEUER')
    t.camera = Camera(64, 1700, H * 0.5)
    return t
