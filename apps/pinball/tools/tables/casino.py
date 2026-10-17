"""Casino Real: green felt and gold. A straight ramp up to the slot machine
on the left, the roulette turning on the right, a royal flush of drop
targets on the right orbit's wall and the dice on the left one."""
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

    # the chips: three bumpers up in the middle
    t.bumper('bumper1', 236, 236, cap=RED, body=0xF0F0F0)
    t.bumper('bumper2', 312, 222, cap=GOLD, body=0xF0F0F0)
    t.bumper('bumper3', 282, 296, cap=0x2060E0, body=0xF0F0F0)

    # THE SLOT MACHINE (Twilight Zone), up on the left: the high rollers'
    # ramp runs dead straight from the left of the ring up to it and drops
    # the ball in its tray, which pulls the lever: the reels spin on the
    # DMD, and each flipper stops one
    # (a roof on its outline: a ball on top of it rolls off)
    t.wall([(82, 250), (82, 226), (104, 212), (126, 226), (126, 250)], h=60, style='rail', thick=4, closed=True,
           visible=False)
    t.deco('slotbig', 104, 234, s=1.0)
    t.saucer('saucer', 104, 274, eject_deg=14, eject_speed=1800)
    t.wall([(186, 520), (182, 472)], h=30, style='plastic', thick=5)
    t.wall([(132, 520), (136, 472)], h=30, style='plastic', thick=5)
    t.post(186, 522, r=5, name='rampRpost')
    t.post(132, 522, r=5, name='rampLpost')
    a, b = (159, 472), (106, 278)
    zs = (0, 14, 28, 41, 52, 60, 64, 62, 52, 34, 14, 0)
    path = [(a[0] + (b[0] - a[0]) * k / 11, a[1] + (b[1] - a[1]) * k / 11, z) for k, z in enumerate(zs)]
    t.ramp('ramp1', (136, 472), (182, 472), path, width=42, color=GOLD, style='carpet', exit_speed=300)
    t.lamp('ramp1', 159, 545, 'arrow', 22, GOLD, 0)
    t.lamp('saucer', 146, 300, 'circle', 14, GREEN)
    t.lamp('video', 66, 290, 'star', 16, GOLD)

    # a royal flush: five cards, drop targets on the right orbit's wall,
    # facing the ring
    t.drops('drop', (430, 300), (430, 440), 5, color=WHITE)
    for i in range(5):
        t.lamp('drop%d' % (i + 1), 406, 314 + i * 28, 'triangle', 11, RED, -90)

    # the dice: three standups on the left orbit's wall
    t.target('target1', (51, 312), (51, 340), color=WHITE)
    t.target('target2', (51, 350), (51, 378), color=WHITE)
    t.target('target3', (51, 388), (51, 416), color=WHITE)
    for i, y in enumerate((326, 364, 402)):
        t.lamp('target%d' % (i + 1), 72, y, 'circle', 11, GOLD)

    # THE ROULETTE (Whirlwind's spinning discs): it turns on the right of
    # the table and flings the ball; where the ball leaves it is the
    # number. The lit colour pays, the zero pays a fortune
    t.toy('roulette', 'disc', 318, 418, 52, model='roulette', a0=2.2, a1=18)
    t.lamp('red', 280, 494, 'shield', 16, RED)
    t.lamp('black', 356, 494, 'shield', 16, 0x202020)
    t.lamp('lock', 220, 575, 'arrow', 18, GOLD, -10)
    t.lamp('extraball', 220, 520, 'circle', 14, GOLD)
    t.lamp('jackpot', 241, 640, 'star', 26, GOLD)
    for i, (x, y) in enumerate([(160, 668), (200, 650), (282, 650), (322, 668), (241, 690)]):
        t.lamp('mission%d' % (i + 1), x, y, 'circle', 14, [RED, GOLD, GOLD, RED, GREEN][i])
    for i, (x, y) in enumerate([(196, 726), (226, 740), (256, 740), (286, 726)]):
        t.lamp('x%d' % (i + 2), x, y, 'rect', 13, GOLD)
    t.flasher('flash1', 52, 214, z=70, color=GOLD, size=80)
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
