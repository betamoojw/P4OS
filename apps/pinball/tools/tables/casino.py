"""Casino Real: green felt and gold. The roulette saucer in the middle, a
bank of four cards, the dice targets, and the high rollers' ramp round the
right."""
from pbtable import Table, Camera, H

GOLD = 0xFFC830
RED = 0xE01830
WHITE = 0xF4F0E8
GREEN = 0x30E070


def build():
    t = Table('casino', dict(es='Casino Real', en='Royal Casino', de='Königliches Casino'), order=50,
              desc=dict(es='Hagan juego: llená la escalera de cartas, girá la ruleta y rompé la banca.',
                        en='Place your bets: fill the straight, spin the wheel and break the bank.',
                        de='Machen Sie Ihr Spiel: Straße legen, Roulette drehen und die Bank sprengen.'))
    t.theme.update(dmd=0xFFC030, bg=0x04120A, accent=GOLD, panel=0x0A2A16, text=0xFFF6E0,
                   flipper=0xF8F4EC, flipper_rubber=0x101010, cabinet=0x0C0C0E, wood=0x2A0A0E, rail=0xE6C870,
                   plastic=RED, field=(0x0F6A34, 0x06301A), gi=GOLD, ambient=0x10241A, apron=0x14141A,
                   logo=(GOLD, 0x301008))
    t.standard_shell()
    t.standard_lower(sling_color=RED)
    t.top_lanes(colors=[GOLD, RED, GOLD])

    # the chips: three bumpers in an arc
    t.bumper('bumper1', 168, 262, cap=RED, body=0xF0F0F0)
    t.bumper('bumper2', 244, 240, cap=GOLD, body=0xF0F0F0)
    t.bumper('bumper3', 314, 270, cap=0x2060E0, body=0xF0F0F0)

    # the high rollers' ramp, right
    t.wall([(296, 520), (300, 472)], h=30, style='plastic', thick=5)
    t.wall([(350, 520), (346, 472)], h=30, style='plastic', thick=5)
    t.post(296, 522, r=5, name='rampLpost')
    t.post(350, 522, r=5, name='rampRpost')
    t.ramp('ramp1', (300, 472), (346, 472),
           [(323, 472, 0), (325, 428, 16), (332, 380, 36), (346, 330, 54), (370, 288, 64), (402, 262, 70),
            (436, 264, 68), (455, 300, 62), (459, 370, 54), (458, 450, 46), (452, 528, 35), (441, 604, 23),
            (431, 672, 11), (425, 712, 3), (425, 730, 0)],
           color=GOLD, style='plastic')
    t.lamp('ramp1', 323, 545, 'arrow', 22, GOLD, 0)

    # a royal flush: five cards, drop targets on the left
    t.wall([(140, 588), (248, 551)], h=34, style='rail', thick=5)
    t.post(140, 590, r=5, name='dropLpost')
    t.post(248, 553, r=5, name='dropRpost')
    t.drops('drop', (244, 565), (146, 599), 5, color=WHITE)
    for i in range(5):
        x = 234 - i * 19.6
        t.lamp('drop%d' % (i + 1), x, 594 + i * 7, 'triangle', 11, RED, -18)

    # the dice: three standups on the left orbit's wall
    t.target('target1', (51, 312), (51, 340), color=WHITE)
    t.target('target2', (51, 350), (51, 378), color=WHITE)
    t.target('target3', (51, 388), (51, 416), color=WHITE)
    for i, y in enumerate((326, 364, 402)):
        t.lamp('target%d' % (i + 1), 72, y, 'circle', 11, GOLD)

    # THE ROULETTE (Whirlwind's spinning discs): it turns on the table and
    # flings the ball; where the ball leaves it is the number. The lit
    # colour pays, the zero pays a fortune
    t.toy('roulette', 'disc', 240, 420, 52, model='roulette', a0=2.2)
    t.lamp('red', 190, 488, 'shield', 16, RED)
    t.lamp('black', 290, 488, 'shield', 16, 0x202020)

    # THE SLOT MACHINE (Twilight Zone): a ball in its tray pulls the lever:
    # the reels spin on the DMD, and each flipper stops one
    t.wall([(80, 352), (124, 352), (124, 384), (80, 384)], h=60, style='rail', thick=4, closed=True, visible=False)
    t.deco('slotbig', 102, 368, s=1.0)
    t.saucer('saucer', 104, 412, eject_deg=60, eject_speed=1700)
    t.lamp('saucer', 132, 448, 'circle', 16, GREEN)
    t.lamp('video', 76, 448, 'star', 16, GOLD)
    t.lamp('lock', 286, 575, 'arrow', 18, GOLD, 20)
    t.lamp('extraball', 220, 520, 'circle', 14, GOLD)
    t.lamp('jackpot', 241, 640, 'star', 26, GOLD)
    for i, (x, y) in enumerate([(160, 668), (200, 650), (282, 650), (322, 668), (241, 690)]):
        t.lamp('mission%d' % (i + 1), x, y, 'circle', 14, [RED, GOLD, GOLD, RED, GREEN][i])
    for i, (x, y) in enumerate([(196, 726), (226, 740), (256, 740), (286, 726)]):
        t.lamp('x%d' % (i + 2), x, y, 'rect', 13, GOLD)
    t.flasher('flash1', 100, 170, z=70, color=GOLD, size=80)
    t.flasher('flash2', 380, 180, z=70, color=RED, size=80)


    # the toys stand where the ball never goes: the cabinet's corners and
    # the back shelf, the slingshots' tops, the apron
    t.deco('slot', 52, 46, s=1.1, z=60)
    t.deco('wheel', 260, -6, s=1.1, z=60)
    t.deco('chips', 468, 46, s=1.1, z=60)
    t.deco('dice', 91, 814, s=0.8, z=30, rot=20)
    t.deco('cards', 391, 814, s=0.8, z=30, rot=-20)
    t.deco('chips', 56, 1016, s=0.9, z=30)
    t.deco('dice', 426, 1016, s=0.8, z=30, rot=40)

    t.mission(dict(es='Escalera real', en='Royal flush', de='Royal Flush'), 'drop', 10, 60, 300000)
    t.mission(dict(es='Doble o nada', en='Double or nothing', de='Doppelt oder nichts'), 'ramp', 3, 60, 400000)
    t.mission(dict(es='Tirá los dados', en='Roll the dice', de='Würfel werfen'), 'target', 6, 60, 350000)
    t.mission(dict(es='Mesa de fichas', en='Chip table', de='Chiptisch'), 'bumper', 12, 45, 250000)
    t.mission(dict(es='Vuelta a la ruleta', en='Round the wheel', de='Rund ums Rad'), 'toy', 5, 60, 400000)
    t.mission(dict(es='La banca paga', en='The bank pays', de='Die Bank zahlt'), 'orbit', 3, 60, 400000)

    t.feat('roulette', toy='roulette')
    t.feat('video', game='slots', saucer='saucer', light='always')

    t.text('jackpot', '¡JACKPOT!', 'JACKPOT!', 'JACKPOT!')
    t.text('multiball', 'TODO AL ROJO', 'ALL ON RED', 'ALLES AUF ROT')
    t.text('wizard', 'ROMPÉ LA BANCA', 'BREAK THE BANK', 'SPRENG DIE BANK')
    t.text('lock', 'APUESTA HECHA', 'BET PLACED', 'EINSATZ GEMACHT')
    t.text('drain', 'LA CASA GANA', 'THE HOUSE WINS', 'DIE BANK GEWINNT')
    t.text('start', 'HAGAN JUEGO', 'PLACE YOUR BETS', 'MACHEN SIE IHR SPIEL')
    t.camera = Camera(64, 1700, H * 0.5)
    return t
