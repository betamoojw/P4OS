"""Noche de Zombis: a graveyard under a full moon. The crypt's ramp on the
right, the tombs' drop targets on the left, the mausoleum's saucer in the
middle."""
from pbtable import Table, Camera, H

GREEN = 0x7CFF3A
PURPLE = 0x9A40FF
BONE = 0xE8E0C8
BLOOD = 0xD01818
MOON = 0xFFF2B0


def build():
    t = Table('zombis', dict(es='Noche de Zombis', en='Zombie Night', de='Zombienacht'), order=10,
              desc=dict(es='Que no te muerdan: cerrá las tumbas y escapá por la cripta.',
                        en="Don't get bitten: seal the tombs and escape through the crypt.",
                        de='Lass dich nicht beißen: schließ die Gräber und flieh durch die Gruft.'))
    t.theme.update(dmd=0x8CFF40, bg=0x0C1008, accent=GREEN, panel=0x1A2414, text=0xE8F0D8,
                   flipper=0xE8E0C8, flipper_rubber=0x6A1010, cabinet=0x1C1410,
                   field=(0x1A2A1A, 0x0A100C), wood=0x2A2018, rail=0x8A9088, plastic=0x5ACF2A,
                   music='zombis', font_color=GREEN, logo=(GREEN, 0x203010))
    t.standard_shell()
    t.standard_lower(sling_color=GREEN)
    t.top_lanes(colors=[GREEN, PURPLE, GREEN])

    # three pop bumpers, graves that spit, up on the left
    t.bumper('bumper1', 152, 258, cap=GREEN, body=0x303830)
    t.bumper('bumper2', 222, 244, cap=PURPLE, body=0x303830)
    t.bumper('bumper3', 188, 322, cap=GREEN, body=0x303830)

    # THE ZOMBIE: he stands on his grave in the middle; three hits send him
    # back under, and he comes back harder (Monster Bash's monsters)
    t.toy('zombie', 'bash', 282, 336, 17, model='zombie')
    t.deco('grave', 282, 336, s=1.0)
    t.lamp('toy', 282, 380, 'arrow', 18, BLOOD, 0)

    # the crypt's ramp, right: up the middle-right, over the orbit and down
    # into the right inlane
    t.wall([(296, 520), (300, 472)], h=30, style='plastic', thick=5)
    t.wall([(350, 520), (346, 472)], h=30, style='plastic', thick=5)
    t.post(296, 522, r=5, name='rampLpost')
    t.post(350, 522, r=5, name='rampRpost')
    t.ramp('ramp1', (300, 472), (346, 472),
           [(323, 472, 0), (324, 430, 16), (328, 380, 36), (338, 320, 54), (360, 268, 64), (396, 242, 68),
            (432, 250, 66), (452, 292, 60), (458, 360, 54), (458, 440, 46), (452, 520, 36), (440, 600, 24),
            (430, 670, 12), (425, 712, 3), (425, 730, 0)],
           color=0x50E030, style='plastic')
    t.lamp('ramp1', 323, 545, 'arrow', 22, GREEN, 0)

    # the tombs: three drop targets on the left, facing the flippers
    t.wall([(126, 574), (206, 548)], h=34, style='rail', thick=5)
    t.post(126, 576, r=5, name='dropLpost')
    t.post(206, 550, r=5, name='dropRpost')
    t.drops('drop', (202, 562), (132, 585), 3, color=BONE)
    for i, (x, y) in enumerate(((190, 592), (168, 599), (146, 606))):
        t.lamp('drop%d' % (i + 1), x, y, 'triangle', 13, PURPLE, -18)

    # Z-O-M-B-I: five standups on the orbits' walls light the video mode
    for i, (x, y0) in enumerate(((51, 312), (51, 350), (51, 388), (431, 318), (431, 364))):
        side = -1 if x < 240 else 1
        t.target('t%s' % 'ZOMBI'[i], (x, y0), (x, y0 + 30), color=BONE)
        t.lamp('letter%d' % (i + 1), x - side * 21, y0 + 15, 'rect', 13, GREEN)

    # the mausoleum's saucer kicks the ball up a wire to the top lanes
    t.habitrail('crypt_wire', [(240, 470, 0), (232, 455, 20), (214, 430, 40), (170, 396, 58), (122, 360, 68),
                               (98, 300, 72), (100, 232, 72), (122, 172, 66), (180, 112, 42), (222, 92, 18),
                               (240, 86, 3)])
    # the mausoleum: a saucer in the middle that starts the missions
    t.saucer('saucer', 240, 470, eject_deg=118, eject_speed=2700, vuk='crypt_wire')
    t.lamp('video', 210, 500, 'star', 18, PURPLE)
    t.lamp('saucer', 270, 505, 'circle', 16, BLOOD)
    t.lamp('lock', 286, 575, 'arrow', 18, PURPLE, 20)
    t.lamp('extraball', 240, 520, 'circle', 14, 0xFFB020)
    t.lamp('jackpot', 241, 590, 'star', 26, 0xFFD040)

    # the missions' inserts, an arc over the flippers
    for i, (x, y) in enumerate([(160, 668), (200, 646), (241, 638), (282, 646), (322, 668)]):
        t.lamp('mission%d' % (i + 1), x, y, 'circle', 14, GREEN if i % 2 == 0 else PURPLE)
    for i, (x, y) in enumerate([(196, 720), (226, 734), (256, 734), (286, 720)]):
        t.lamp('x%d' % (i + 2), x, y, 'rect', 13, 0xFFB020)

    t.flasher('flash1', 404, 150, z=70, color=MOON, size=90)       # the moon
    t.flasher('flash2', 90, 200, z=60, color=GREEN, size=70)


    # the toys stand where the ball never goes: the cabinet's corners and
    # the back shelf, the slingshots' tops, the apron
    t.deco('tomb', 52, 46, s=1.1, rot=10, z=60)
    t.deco('crypt', 260, -6, s=1.3, z=60)
    t.deco('tree', 468, 46, s=1.2, z=60)
    t.deco('hand', 91, 814, s=0.8, z=30)
    t.deco('hand', 391, 814, s=0.8, z=30, rot=-20)
    t.deco('tomb', 54, 1016, s=0.9, z=30)
    t.deco('coffin', 426, 1016, s=0.9, z=30, rot=90)

    t.mission(dict(es='La horda', en='The horde', de='Die Horde'), 'bumper', 10, 45, 250000)
    t.mission(dict(es='Cerrá las tumbas', en='Seal the tombs', de='Versiegle die Gräber'), 'drop', 6, 60, 300000)
    t.mission(dict(es='A la cripta', en='To the crypt', de='Zur Gruft'), 'ramp', 3, 60, 400000)
    t.mission(dict(es='Corré por el cementerio', en='Run through the graveyard', de='Lauf über den Friedhof'),
              'orbit', 3, 60, 400000)
    t.mission(dict(es='Mandalo a la tumba', en='Back to the grave', de='Zurück ins Grab'), 'toy', 6, 50, 350000)

    t.feat('bash', toy='zombie', hits=3, points=150000, lights='lock')
    t.feat('letters', objs='tZ,tO,tM,tB,tI', word='ZOMBI', lights='video')
    t.feat('video', game='shoot', saucer='saucer', light='letters')
    t.text('toyhit', '¡TOMÁ!', 'TAKE THAT!', 'NIMM DAS!')
    t.text('toydown', '¡A LA TUMBA!', 'BACK TO THE GRAVE!', 'INS GRAB!')
    t.text('toyback', '¡RESUCITÓ!', 'HE RISES AGAIN!', 'ER IST ZURÜCK!')

    t.text('jackpot', '¡CEREBROS!', 'BRAINS!', 'GEHIRNE!')
    t.text('multiball', 'INVASIÓN ZOMBI', 'ZOMBIE INVASION', 'ZOMBIE-INVASION')
    t.text('wizard', 'NOCHE ETERNA', 'ENDLESS NIGHT', 'EWIGE NACHT')
    t.text('lock', 'ZOMBI ATRAPADO', 'ZOMBIE TRAPPED', 'ZOMBIE GEFANGEN')
    t.text('drain', 'TE MORDIERON', 'YOU GOT BITTEN', 'GEBISSEN')
    t.text('start', 'SE LEVANTAN...', 'THEY RISE...', 'SIE ERHEBEN SICH...')

    t.camera = Camera(64, 1700, H * 0.5)
    return t
