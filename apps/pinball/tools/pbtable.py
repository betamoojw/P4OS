"""
PINBALL - what a table is, written once and read by everything else.

A table is a Python module in tools/tables/ with a build() that returns a
Table. The same object gives:

    - the game's definition (Table.def_text(): the "table" entry of the
      pack, a text file the app parses: geometry for the physics, the
      camera, the lamps, the missions, the theme's colours and words);
    - the scene Blender builds and renders (Table.scene(): JSON for
      tools/blender/build_table.py: walls with their height and style,
      bumpers, inserts, ramps, the theme's decorations);
    - a flat preview (preview.py), to check the geometry without Blender.

Units: millimetres. The table's x runs to the right, y DOWN the slope (0
is the far end under the DMD, H the near end under the flippers), z up
from the playfield. The camera sits behind the near end, above it, and
looks up the table pitched down (Camera); the game and Blender use the
same numbers.

Adding a table is adding a module here (tables/README in the app's
README): the game knows the roles below, not the tables.
"""
import json
import math

W = 520             # the playfield, plunger lane included
H = 1040
BALL_R = 13.5       # a 27 mm ball
LANE_X = 482        # the plunger lane's inner wall; the main field is 0..LANE_X

# screen: the DMD takes the top 160 rows, the table the rest
SCREEN_W, SCREEN_H = 720, 1280
DMD_H = 160

# physical materials (the game's table: restitution, friction)
MATS = {'wall': 0, 'rubber': 1, 'post': 2, 'plastic': 3, 'metal': 4, 'flipper': 5}

# object types the game knows (pb_table.h, OT_*)
OT = {'wall': 0, 'post': 1, 'bumper': 2, 'sling': 3, 'drop': 4, 'target': 5, 'rollover': 6,
      'spinner': 7, 'saucer': 8, 'ramp': 9, 'gate': 10, 'kickback': 11, 'flipper': 12}

# roles the rules know (pb_game.c): what a hit means
ROLES = ('none', 'lane', 'inlane', 'outlane', 'bumper', 'sling', 'drop', 'target', 'spinner',
         'saucer', 'ramp', 'orbit', 'kickback', 'post')

# what a mission can ask for (pb_game.c, the same names)
GOALS = ('bumper', 'sling', 'drop', 'target', 'spinner', 'ramp', 'orbit', 'lane', 'saucer', 'any')


def arc(cx, cy, r, a0, a1, n=None):
    """points on a circle, angles in degrees (0 = +x, 90 = +y, i.e. down)"""
    if n is None:
        n = max(2, int(abs(a1 - a0) / 6) + 1)
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / (n - 1))),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / (n - 1)))) for i in range(n)]


def mirror_pts(pts, cx=LANE_X / 2):
    return [(2 * cx - p[0],) + tuple(p[1:]) for p in pts]


def tangent_point(d, c, r):
    """where a line from d touches the circle (c, r) on its upper side (y
    down): the ball rolls from the line onto the circle without a notch"""
    vx, vy = c[0] - d[0], c[1] - d[1]
    dist = math.hypot(vx, vy)
    a = math.atan2(vy, vx)
    s = 1 if vx >= 0 else -1
    a2 = a - s * math.asin(r / dist)
    l = math.sqrt(dist * dist - r * r)
    return (d[0] + math.cos(a2) * l, d[1] + math.sin(a2) * l)


def lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def offset_line(pts, d):
    """the polyline moved d to its left (y down: left of the direction of travel)"""
    out = []
    n = len(pts)
    for i in range(n):
        p = pts[i]
        if i == 0:
            q = pts[1]
            tx, ty = q[0] - p[0], q[1] - p[1]
        elif i == n - 1:
            q = pts[i - 1]
            tx, ty = p[0] - q[0], p[1] - q[1]
        else:
            a, b = pts[i - 1], pts[i + 1]
            tx, ty = b[0] - a[0], b[1] - a[1]
        l = math.hypot(tx, ty) or 1
        out.append((p[0] + ty / l * d, p[1] - tx / l * d))
    return out


class Camera:
    """A pinhole behind the near end, pitched down, no roll.

    pos is in table coordinates (x, y, z); it looks towards -y and down by
    pitch. f and the principal point (cx, cy) are screen pixels, cy on the
    whole screen (the table's image starts at row DMD_H)."""

    def __init__(self, pitch_deg, dist, aim_y, fit_pad=8, aim_x=W / 2, extra=()):
        self.pitch = math.radians(pitch_deg)
        th = self.pitch
        d = (0.0, -math.cos(th), -math.sin(th))
        aim = (aim_x, aim_y, 0.0)
        self.pos = (aim[0] - d[0] * dist, aim[1] - d[1] * dist, aim[2] - d[2] * dist)
        self.f, self.cx, self.cy = 1.0, 0.0, 0.0
        # fit: the cabinet's outline at the playfield and at the rails' top
        pts = []
        for z in (0.0, 60.0):
            for x in (-22.0, W + 22.0):
                for y in (-12.0, H + 4.0):
                    pts.append((x, y, z))
        pts += list(extra)
        uv = [self.norm(p) for p in pts]
        u0 = min(p[0] for p in uv); u1 = max(p[0] for p in uv)
        v0 = min(p[1] for p in uv); v1 = max(p[1] for p in uv)
        aw = SCREEN_W - 2 * fit_pad
        ah = SCREEN_H - DMD_H - 2 * fit_pad
        self.f = min(aw / (u1 - u0), ah / (v1 - v0))
        self.cx = SCREEN_W / 2 - self.f * (u0 + u1) / 2
        # flush with the bottom of the screen: the flippers as low as they go
        self.cy = SCREEN_H - fit_pad - self.f * v1

    def cam(self, p):
        th = self.pitch
        q = (p[0] - self.pos[0], p[1] - self.pos[1], p[2] - self.pos[2])
        xc = q[0]
        yc = -q[1] * math.sin(th) + q[2] * math.cos(th)
        zc = -q[1] * math.cos(th) - q[2] * math.sin(th)
        return xc, yc, zc

    def norm(self, p):
        xc, yc, zc = self.cam(p)
        return xc / zc, -yc / zc

    def project(self, x, y, z=0.0):
        xc, yc, zc = self.cam((x, y, z))
        return self.cx + self.f * xc / zc, self.cy - self.f * yc / zc, zc

    def blender(self):
        """what build_table.py needs for its camera"""
        return dict(pitch=self.pitch, pos=self.pos, f=self.f, cx=self.cx, cy=self.cy - DMD_H,
                    w=SCREEN_W, h=SCREEN_H - DMD_H)


class Table:
    def __init__(self, tid, names, order=50, desc=None):
        self.id = tid
        self.names = names              # {'es':..., 'en':..., 'de':...}
        self.desc = desc or {}
        self.order = order
        self.grav = 1500.0              # mm/s^2 down the slope
        self.objs = []                  # dicts: type, name, role, idx, ...
        self.segs = []                  # x0 y0 x1 y1 mat obj
        self.circs = []                 # x y r mat obj
        self.trigs = []                 # line triggers: x0 y0 x1 y1 obj
        self.rolls = []                 # circle triggers: x y r obj
        self.flippers = []
        self.ramps = []
        self.lamps = []
        self.missions = []
        self.texts = {}                 # callouts: key -> {lang: text}
        self.vis = []                   # what Blender builds
        self.plunger = (W - 19, H - 40)
        self.camera = None
        self.theme = dict(dmd=0xFF8020, bg=0x101018, accent=0xFF8020, panel=0x202030, text=0xFFFFFF,
                          flipper=0xF0F0F0, flipper_rubber=0xD02020, cabinet=0x402010)
        self.paint = None               # fn(draw, img, px) painting the playfield's art
        self.decor = []

    # ---- objects -------------------------------------------------------
    def _obj(self, typ, name, role='none', **kw):
        o = dict(type=typ, name=name, role=role, idx=len(self.objs))
        o.update(kw)
        self.objs.append(o)
        return o['idx']

    def wall(self, pts, h=40, style='rail', mat='wall', thick=5, visible=True, color=None, phys=True,
             closed=False):
        pts = [tuple(p) for p in pts]
        if closed:
            pts = pts + [pts[0]]
        if phys:
            for a, b in zip(pts, pts[1:]):
                self.segs.append((a[0], a[1], b[0], b[1], MATS[mat], -1))
        if visible:
            self.vis.append(dict(t='wall', pts=pts, h=h, style=style, thick=thick, color=color))

    def rubber(self, pts, closed=False, h=22, posts=True):
        """a rubber band stretched round posts"""
        pts = [tuple(p) for p in pts]
        if closed:
            pts = pts + [pts[0]]
        for a, b in zip(pts, pts[1:]):
            self.segs.append((a[0], a[1], b[0], b[1], MATS['rubber'], -1))
        self.vis.append(dict(t='rubber', pts=pts, h=h, posts=posts))

    def post(self, x, y, r=6, name='post', rubber=True):
        o = self._obj('post', name, 'post')
        self.circs.append((x, y, r, MATS['rubber' if rubber else 'post'], o))
        self.vis.append(dict(t='post', x=x, y=y, r=r, rubber=rubber))
        return o

    def bumper(self, name, x, y, r=24, kick=2100, cap=None, body=None, lamp=None):
        o = self._obj('bumper', name, 'bumper', kick=kick)
        self.circs.append((x, y, r, MATS['rubber'], o))
        self.vis.append(dict(t='bumper', x=x, y=y, r=r, cap=cap, body=body, lamp=lamp or name))
        self.lamps.append(dict(name=lamp or name, kind='bumper', x=x, y=y, z=48, color=cap or 0xFFFFFF,
                               size=r))
        return o

    def sling(self, name, top, outer, inner, kick=1250, color=None, lamp=None):
        """a slingshot: the face top -> inner kicks, the other two are walls"""
        o = self._obj('sling', name, 'sling', kick=kick)
        self.segs.append((top[0], top[1], inner[0], inner[1], MATS['rubber'], o))
        self.segs.append((top[0], top[1], outer[0], outer[1], MATS['rubber'], -1))
        self.segs.append((outer[0], outer[1], inner[0], inner[1], MATS['rubber'], -1))
        for p in (top, outer, inner):
            self.circs.append((p[0], p[1], 5, MATS['post'], -1))
        self.vis.append(dict(t='sling', pts=[top, outer, inner], color=color, lamp=lamp or name))
        cx = (top[0] + outer[0] + inner[0]) / 3
        cy = (top[1] + outer[1] + inner[1]) / 3
        self.lamps.append(dict(name=lamp or name, kind='sling', x=cx, y=cy, z=30, color=color or 0xFFFFFF,
                               size=30))
        return o

    def drops(self, name, a, b, n, color=None, faces=None):
        """a bank of n drop targets from a to b; their face looks to the left
        of a -> b (y down), towards the ball"""
        idx = []
        for i in range(n):
            t0 = (i + 0.08) / n
            t1 = (i + 0.92) / n
            p0, p1 = lerp(a, b, t0), lerp(a, b, t1)
            o = self._obj('drop', '%s%d' % (name, i + 1), 'drop', bank=name)
            self.segs.append((p0[0], p0[1], p1[0], p1[1], MATS['plastic'], o))
            self.vis.append(dict(t='drop', a=p0, b=p1, color=color, obj=self.objs[o]['name'],
                                 face=(faces[i] if faces else None)))
            idx.append(o)
        return idx

    def target(self, name, a, b, color=None, role='target'):
        o = self._obj('target', name, role)
        self.segs.append((a[0], a[1], b[0], b[1], MATS['plastic'], o))
        self.vis.append(dict(t='target', a=a, b=b, color=color, obj=name))
        return o

    def rollover(self, name, x, y, role='lane', r=9):
        o = self._obj('rollover', name, role)
        self.rolls.append((x, y, r, o))
        self.vis.append(dict(t='rollover', x=x, y=y))
        return o

    def spinner(self, name, a, b, color=None):
        o = self._obj('spinner', name, 'spinner')
        self.trigs.append((a[0], a[1], b[0], b[1], o))
        self.vis.append(dict(t='spinner', a=a, b=b, color=color))
        return o

    def saucer(self, name, x, y, eject_deg, eject_speed=2200, r=16):
        o = self._obj('saucer', name, 'saucer', eject=eject_deg, speed=eject_speed)
        self.rolls.append((x, y, r, o))
        self.vis.append(dict(t='saucer', x=x, y=y, r=r))
        return o

    def gate(self, name, a, b, vis=True):
        """one way: the ball passes going to the left of a -> b (y down)"""
        o = self._obj('gate', name, 'none')
        self.segs.append((a[0], a[1], b[0], b[1], MATS['metal'], o))
        if vis:
            self.vis.append(dict(t='gate', a=a, b=b))
        return o

    def kickback(self, name, a, b, vx=0, vy=-3300):
        o = self._obj('kickback', name, 'kickback', vx=vx, vy=vy)
        self.trigs.append((a[0], a[1], b[0], b[1], o))
        self.vis.append(dict(t='kickback', a=a, b=b))
        return o

    def ramp(self, name, entry_a, entry_b, path, width=46, color=None, style='plastic', min_speed=700,
             exit_speed=None):
        """entry_a -> entry_b is the mouth, crossed to its left (y down);
        path is (x, y, z) from the mouth's middle to where the ball comes
        down again"""
        o = self._obj('ramp', name, 'ramp', min_speed=min_speed)
        self.trigs.append((entry_a[0], entry_a[1], entry_b[0], entry_b[1], o))
        self.ramps.append(dict(obj=o, path=[tuple(p) for p in path], exit_speed=exit_speed or 0))
        self.vis.append(dict(t='ramp', path=[tuple(p) for p in path], width=width, color=color, style=style,
                             a=entry_a, b=entry_b))
        return o

    def flipper(self, side, x, y, length=76, r0=12.5, r1=6.5, rest=28, up=-26, lamp=None):
        """side 'L' or 'R'; angles from the horizontal, positive down"""
        self.flippers.append(dict(side=side, x=x, y=y, len=length, r0=r0, r1=r1, rest=rest, up=up))

    def lamp(self, name, x, y, shape='circle', size=16, color=0xFFFFFF, angle=0.0, label=None):
        """an insert in the playfield; angle in degrees, 0 pointing up the table"""
        self.lamps.append(dict(name=name, kind='insert', x=x, y=y, z=0, shape=shape, size=size, color=color,
                               angle=angle))
        self.vis.append(dict(t='insert', name=name, x=x, y=y, shape=shape, size=size, color=color,
                             angle=angle))

    def flasher(self, name, x, y, z=60, color=0xFFFFFF, size=60):
        """a light over the playfield (a dome on a toy): lights an area"""
        self.lamps.append(dict(name=name, kind='flasher', x=x, y=y, z=z, color=color, size=size))
        self.vis.append(dict(t='flasher', name=name, x=x, y=y, z=z, color=color))

    def deco(self, kind, x, y, **kw):
        d = dict(t='decor', kind=kind, x=x, y=y)
        d.update(kw)
        self.vis.append(d)

    def mission(self, names, goal, count, secs, points):
        assert goal in GOALS, goal
        self.missions.append(dict(names=names, goal=goal, count=count, secs=secs, points=points))

    def text(self, key, es, en, de):
        self.texts[key] = dict(es=es, en=en, de=de)

    # ---- the standard parts ---------------------------------------------
    def standard_lower(self, sling_color=None, kickback=True):
        """outlanes, inlanes, slings and flippers, mirrored about the main
        field's middle: every table has the same bottom, so the game feels
        the same, and the top is where they differ"""
        cx = LANE_X / 2
        lx, ly = 150, 940
        self.flipper('L', lx, ly)
        self.flipper('R', 2 * cx - lx, ly)
        for m in (False, True):
            def M(pts):
                return mirror_pts(pts, cx) if m else pts
            side = 'R' if m else 'L'
            # the outer wall comes in at the bottom, under the apron
            self.wall(M([(0, 560), (0, 905), (24, 960), (24, 1040)]), h=50, style='side', visible=False)
            # the divider between outlane and inlane, its post on top
            self.wall(M([(35, 692), (35, 852)]), h=30, style='rail', thick=4)
            self.post(*M([(35, 686)])[0], r=6, name='post' + side)
            # the outlane's guard: a ball rolling down the wall bounces off
            # it into the field; one coming in across still gets past
            self.post(*M([(11, 642)])[0], r=6, name='guard' + side)
            # the inlane's floor, tangent to the flipper's pivot: the ball
            # rolls from it onto the flipper without a notch to sit in
            d0 = M([(35, 852)])[0]
            piv = M([(lx, ly)])[0]
            self.wall([d0, tangent_point(d0, piv, 12.5)], h=30, style='rail', thick=4)
            # under the apron, from the outlane to the drain
            self.wall(M([(24, 960), (100, 1040)]), h=30, style='apron', visible=False)
            self.sling('sling' + side, *M([(76, 740), (76, 836), (120, 866)]), color=sling_color)
            ix, ox = M([(57, 790)])[0], M([(19, 790)])[0]
            self.rollover('in' + side, ix[0], ix[1], role='inlane')
            self.rollover('out' + side, ox[0], ox[1], role='outlane')
            self.lamp('in' + side, ix[0], ix[1] - 44, 'arrow', 14, self.theme['accent'], 0)
            self.lamp('out' + side, ox[0], ox[1] - 44, 'arrow', 14, self.theme['accent'], 0)
        if kickback:
            self.kickback('kickback', (2, 900), (36, 900), vx=650, vy=-1700)
            self.lamp('kickback', 19, 860, 'circle', 13, 0xFF3030)
        self.lamp('shootagain', cx, 915, 'shield', 24, 0xFF2020)
        self.vis.append(dict(t='apron', y=990))

    def standard_shell(self, orbit_spinner=True):
        """the cabinet's sides, the top arch, the plunger lane and its gate,
        and the two orbit lanes that make a loop round the top: a shot up
        either one comes down the other and back into the field"""
        r = W / 2
        top = arc(W / 2, r, r, 180, 360, 46)
        self.wall([(0, 560), (0, r)] + top[1:] + [(W, 1040)], h=60, style='side', thick=8)
        # the plunger lane, and its one-way gate slanted so that a ball
        # coming down the right side rolls off it into the right orbit
        self.wall([(LANE_X, 1040), (LANE_X, 300)], h=40, style='rail', thick=5)
        self.post(LANE_X, 300, r=4, rubber=False, name='lanepost')
        self.gate('lanegate', (LANE_X + 1, 299), (W - 1, 262))
        self.plunger = (W - 19, 1004)
        self.segs.append((LANE_X, 1018, W, 1018, MATS['metal'], -1))
        self.vis.append(dict(t='plunger', x=W - 19, y=1018))
        # the orbit lanes: an inner wall, and a deflector below that turns
        # what comes down into the field instead of into the outlane
        for m in (False, True):
            def M(pts):
                return mirror_pts(pts, LANE_X / 2) if m else pts
            side = 'R' if m else 'L'
            self.wall(M([(46, 300), (46, 440), (60, 490), (84, 520)]), h=40, style='rail', thick=5)
            self.post(*M([(84, 520)])[0], r=5, name='orbitpost' + side)
            # the exit pours into the inlane's mouth, not the outlane's
            self.wall(M([(0, 470), (30, 540), (54, 600)]), h=36, style='rail', thick=5)
            self.post(*M([(54, 600)])[0], r=5, name='deflpost' + side)
            ox = M([(23, 400)])[0]
            self.rollover('orbit' + side, ox[0], ox[1], role='orbit')
            self.lamp('orbit' + side, *M([(64, 560)])[0], shape='arrow', size=18, color=self.theme['accent'],
                      angle=(25 if m else -25))
        if orbit_spinner:
            self.spinner('spinner', (3, 350), (43, 350))
            self.lamp('spinner', 23, 380, 'circle', 10, 0xFFFFFF)

    def top_lanes(self, xs=(185, 240, 295), y=150, colors=None):
        """three rollovers under the arch, with their guides; complete them
        for the bonus multiplier (the flippers rotate the lit ones)"""
        gap = (xs[1] - xs[0]) / 2
        for i, x in enumerate(xs):
            self.rollover('lane%d' % (i + 1), x, y, role='lane')
            self.lamp('lane%d' % (i + 1), x, y - 40, 'circle', 15, (colors or [self.theme['accent']] * 3)[i])
        for x in [xs[0] - gap] + [x + gap for x in xs]:
            self.wall([(x, y - 45), (x, y + 30)], h=28, style='rail', thick=4)
            self.post(x, y + 30, r=5, name='laneguide%d' % round(x))

    # ---- export ----------------------------------------------------------
    def check(self):
        names = [o['name'] for o in self.objs]
        assert len(names) == len(set(names)), [n for n in names if names.count(n) > 1]
        ln = [l['name'] for l in self.lamps]
        assert len(ln) == len(set(ln)), [n for n in ln if ln.count(n) > 1]
        assert self.camera is not None

    def def_text(self):
        """the "table" entry of the pack: one record per line, words split by
        spaces, strings in double quotes (pb_table.c)"""
        self.check()
        c = self.camera
        L = []
        q = lambda s: '"' + s.replace('"', "'") + '"'
        L.append('pinball 1')
        L.append('id %s' % self.id)
        L.append('order %d' % self.order)
        for lang, n in self.names.items():
            L.append('name %s %s' % (lang, q(n)))
        for lang, n in self.desc.items():
            L.append('desc %s %s' % (lang, q(n)))
        L.append('size %d %d' % (W, H))
        L.append('grav %.1f' % self.grav)
        L.append('ball %.2f' % BALL_R)
        L.append('cam %.4f %.3f %.3f %.3f %.3f %.3f %.6f' % (c.f, c.cx, c.cy, c.pos[0], c.pos[1], c.pos[2], c.pitch))
        L.append('plunger %.1f %.1f' % self.plunger)
        th = self.theme
        L.append('theme %06x %06x %06x %06x %06x' % (th['dmd'], th['bg'], th['accent'], th['panel'], th['text']))
        for o in self.objs:
            extra = ''
            if o['type'] == 'bumper':
                extra = ' %d' % o['kick']
            elif o['type'] == 'sling':
                extra = ' %d' % o['kick']
            elif o['type'] == 'saucer':
                extra = ' %.1f %d' % (o['eject'], o['speed'])
            elif o['type'] == 'ramp':
                extra = ' %d' % o['min_speed']
            elif o['type'] == 'kickback':
                extra = ' %d %d' % (o['vx'], o['vy'])
            elif o['type'] == 'drop':
                extra = ' %s' % o['bank']
            L.append('obj %d %s %s %s%s' % (o['idx'], o['type'], o['name'], o['role'], extra))
        for s in self.segs:
            L.append('seg %.2f %.2f %.2f %.2f %d %d' % s)
        for s in self.circs:
            L.append('circ %.2f %.2f %.2f %d %d' % s)
        for s in self.trigs:
            L.append('trig %.2f %.2f %.2f %.2f %d' % s)
        for s in self.rolls:
            L.append('roll %.2f %.2f %.2f %d' % s)
        for f in self.flippers:
            L.append('flipper %s %.2f %.2f %.2f %.2f %.2f %.2f %.2f' % (f['side'], f['x'], f['y'], f['len'],
                                                                     f['r0'], f['r1'], f['rest'], f['up']))
        for r in self.ramps:
            pts = ' '.join('%.1f %.1f %.1f' % p for p in r['path'])
            L.append('ramp %d %d %d %s' % (r['obj'], r['exit_speed'], len(r['path']), pts))
        for l in self.lamps:
            sx, sy, _ = c.project(l['x'], l['y'], l.get('z', 0))
            L.append('lamp %s %s %d %d %06x' % (l['name'], l['kind'], round(sx), round(sy), l['color']))
        for m in self.missions:
            n = m['names']
            L.append('mission %s %d %d %d %s %s %s' % (m['goal'], m['count'], m['secs'], m['points'],
                                                     q(n['es']), q(n['en']), q(n['de'])))
        for k, v in self.texts.items():
            L.append('text %s %s %s %s' % (k, q(v['es']), q(v['en']), q(v['de'])))
        return '\n'.join(L) + '\n'

    def scene(self):
        return dict(id=self.id, W=W, H=H, ball_r=BALL_R, camera=self.camera.blender(), theme=self.theme,
                    vis=self.vis, flippers=self.flippers, lamps=self.lamps, lane_x=LANE_X,
                    objs=[dict(name=o['name'], type=o['type']) for o in self.objs])

    def save_scene(self, path):
        with open(path, 'w') as f:
            json.dump(self.scene(), f, indent=1)


def load(tid):
    import importlib
    import os
    import sys
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    mod = importlib.import_module('tables.' + tid)
    return mod.build()


def all_ids():
    import os
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tables')
    return sorted(f[:-3] for f in os.listdir(here) if f.endswith('.py') and not f.startswith('_'))
