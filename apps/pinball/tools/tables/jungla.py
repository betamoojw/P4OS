"""El Templo de la Jungla: the lost temple. A rope bridge crosses the gorge
from the right to the left orbit, the boulder rolls under it, the idols'
drop targets stand in the middle and the temple's mouth swallows the ball
on the right."""
from pbtable import Table, Camera, H

LEAF = 0x6CE040
ORANGE = 0xFFA020
TEAL = 0x30D0C0
GOLD = 0xFFD050


def build():
    t = Table('jungla', dict(es='El Templo de la Jungla', en='Jungle Temple', de='Dschungeltempel'), order=30,
              desc=dict(es='Cruzá el puente colgante, despertá a los ídolos y encontrá el tesoro del templo.',
                        en='Cross the rope bridge, wake the idols and find the temple treasure.',
                        de='Überquere die Hängebrücke, wecke die Götzen und finde den Tempelschatz.'))
    t.theme.update(dmd=0xFFB030, bg=0x0C1A0A, accent=ORANGE, panel=0x1C2E14, text=0xF4F0D8,
                   flipper=0xF4ECD8, flipper_rubber=0x1E7A2A, cabinet=0x2A1C10, wood=0x4A3018, rail=0xC8B890,
                   plastic=LEAF, field=(0x2A6A26, 0x0E2A12), gi=ORANGE, ambient=0x1E3020, apron=0x3A2A18,
                   logo=(ORANGE, 0x1A2A0A))
    t.standard_shell()
    t.standard_lower(sling_color=LEAF)
    t.top_lanes(colors=[ORANGE, TEAL, ORANGE])

    # the bumpers: three stone heads, up in the middle
    t.bumper('bumper1', 170, 236, cap=ORANGE, body=0x5A5040)
    t.bumper('bumper2', 246, 226, cap=TEAL, body=0x5A5040)
    t.bumper('bumper3', 208, 296, cap=ORANGE, body=0x5A5040)

    # THE ROPE BRIDGE: up a plank stair on the right, across the gorge on
    # ropes hung between two towers, and down into the left orbit, whose way
    # out feeds the upper flipper
    t.wall([(296, 520), (300, 472)], h=30, style='plastic', thick=5)
    t.wall([(350, 520), (346, 472)], h=30, style='plastic', thick=5)
    t.post(296, 522, r=5, name='rampLpost')
    t.post(350, 522, r=5, name='rampRpost')
    t.ramp('ramp1', (300, 472), (346, 472),
           [(323, 472, 0), (322, 444, 16), (318, 418, 34), (308, 398, 50), (296, 390, 56), (260, 380, 56),
            (220, 368, 56), (180, 357, 56.5), (140, 345, 57), (104, 335, 57.5), (84, 330, 58), (62, 330, 55),
            (42, 338, 48), (29, 356, 40), (24, 384, 30), (23, 420, 16), (23, 452, 5), (23, 472, 0)],
           width=42, color=0x8A5A2A, style='bridge', towers=(4, 10))
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
    # courtyard, under the bridge; three hits stop it and open the Path of
    # Adventure
    t.toy('boulder', 'mover', 182, 438, 22, model='boulder', x1=262, y1=438, period=6.0, s=1.05)
    t.lamp('toy', 222, 480, 'arrow', 18, ORANGE, 0)

    # the temple's mouth, right: up a vine-covered wire to the top lanes
    # (it lets the ball down slowly into the right lane, not against a guide)
    t.habitrail('temple_wire', [(380, 436, 0), (386, 410, 18), (396, 364, 42), (402, 306, 60), (400, 244, 68),
                                (386, 184, 64), (364, 136, 52), (340, 104, 38), (316, 88, 24), (300, 88, 12),
                                (294, 98, 4), (294, 110, 0)], exit_speed=500)
    t.saucer('saucer', 380, 436, eject_deg=125, eject_speed=2500, vuk='temple_wire')
    t.lamp('video', 396, 478, 'star', 18, TEAL)

    # an upper flipper on the left, fed by the left orbit
    t.flipper('L', 42, 640, length=55, r0=11, r1=6, rest=28, up=-22)
    t.lamp('saucer', 362, 478, 'circle', 16, ORANGE)
    t.lamp('lock', 286, 575, 'arrow', 18, TEAL, 20)
    t.lamp('extraball', 136, 522, 'circle', 14, GOLD)
    t.lamp('jackpot', 241, 600, 'star', 26, GOLD)
    for i, (x, y) in enumerate([(160, 668), (200, 646), (241, 638), (282, 646), (322, 668)]):
        t.lamp('mission%d' % (i + 1), x, y, 'circle', 14, ORANGE if i % 2 == 0 else TEAL)
    for i, (x, y) in enumerate([(196, 720), (226, 734), (256, 734), (286, 720)]):
        t.lamp('x%d' % (i + 2), x, y, 'rect', 13, GOLD)
    t.flasher('flash1', 84, 330, z=90, color=ORANGE, size=80)
    t.flasher('flash2', 296, 390, z=90, color=TEAL, size=80)


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
    t.mission(dict(es='Cruzá el puente', en='Cross the bridge', de='Über die Brücke'), 'ramp', 3, 60, 400000)
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
