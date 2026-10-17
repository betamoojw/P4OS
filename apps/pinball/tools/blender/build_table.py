"""
PINBALL - a table built and rendered in Blender (3.3, Cycles on Metal)

    Blender -b -P build_table.py -- --scene scene.json --tex DIR --out DIR [--samples 96] [--only base,lamps,...]

tools/render_table.py writes the scene (pbtable.Table.scene()), paints the
playfield and the backboard (paint.py) and runs this; pack_p4.py then reads
what it leaves in --out (meta.json says what):

    base.png        the table with every lamp off, the targets up, no flippers,
                    no spinner plate, no ball: 720 x 1120, the screen under the DMD
    depth.npy       the camera depth of every pixel of it, mm (float32)
    lamp_<name>.png the same with that lamp lit, cropped round it
    drop_<name>.png that drop target down, cropped, and drop_<name>.npy its depth
    flip<s>_<k>.png the flipper s at frame k (rest to up) with its shadow,
                    everything else held out, cropped to the frames' union
    spin_<k>.png    the spinner's plate at k / n of a turn
    ball.png        the chrome ball, 128 x 128
    launch.png, launch_on.png, pause.png, icon.png   the table's buttons

World: Blender metres, X right, Y up the table (Y = (H - y) / 1000), Z up.
The game's camera is rebuilt exactly (lens from f, principal point from
the shifts) and checked against the projections pbtable.py computed.
"""
import argparse
import json
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from bpy_extras.object_utils import world_to_camera_view
from mathutils import Euler, Matrix, Vector

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
ap = argparse.ArgumentParser()
ap.add_argument('--scene', required=True)
ap.add_argument('--tex', required=True)
ap.add_argument('--out', required=True)
ap.add_argument('--samples', type=int, default=96)
ap.add_argument('--only', default='')
ap.add_argument('--cpu', action='store_true')
ap.add_argument('--blend', default='')
args = ap.parse_args(argv)
ONLY = set(a for a in args.only.split(',') if a)

S = json.load(open(args.scene))
W, H = S['W'], S['H']
CAM = S['camera']
TH = S['theme']
IMG_W, IMG_H = CAM['w'], CAM['h']
os.makedirs(args.out, exist_ok=True)
TMP = os.path.join(args.out, 'tmp')
os.makedirs(TMP, exist_ok=True)


def want(what):
    return not ONLY or what in ONLY


OFF = [0.0, 0.0, 0.0]   # where things are built: the icons go far from the table, toys stand on things


def V(x, y, z=0.0):
    """table millimetres -> world"""
    x += OFF[0]
    y += OFF[1]
    return Vector(((x - W / 2) / 1000.0, (H - y) / 1000.0, (z + OFF[2]) / 1000.0))


def col(c, k=1.0):
    """0xRRGGBB (sRGB) -> linear RGBA"""
    def lin(v):
        v = v / 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    return (min(1, lin((c >> 16) & 255) * k), min(1, lin((c >> 8) & 255) * k), min(1, lin(c & 255) * k), 1.0)


# ---------------------------------------------------------------------------
# scene, camera, render settings
# ---------------------------------------------------------------------------

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
sc.render.engine = 'CYCLES'
ok = False
if not args.cpu:
    try:
        pr = bpy.context.preferences.addons['cycles'].preferences
        pr.compute_device_type = 'METAL'
        pr.get_devices()
        for d in pr.devices:
            d.use = (d.type == 'METAL')
            ok = ok or d.use
    except Exception as e:  # noqa
        print('GPU setup failed:', e)
sc.cycles.device = 'GPU' if ok else 'CPU'
print('Cycles device:', sc.cycles.device)
r = sc.render
r.resolution_x, r.resolution_y, r.resolution_percentage = IMG_W, IMG_H, 100
r.film_transparent = False
r.dither_intensity = 0.0
r.image_settings.file_format = 'OPEN_EXR'
r.image_settings.color_mode = 'RGBA'
r.image_settings.color_depth = '32'
r.image_settings.exr_codec = 'ZIP'
r.use_persistent_data = True
sc.view_settings.view_transform = 'Standard'
sc.view_settings.look = 'None'
sc.view_settings.exposure = 0.0
sc.cycles.use_adaptive_sampling = False
sc.cycles.seed = 11
sc.cycles.max_bounces = 6
sc.cycles.diffuse_bounces = 3
sc.cycles.glossy_bounces = 3
sc.cycles.transmission_bounces = 4
sc.cycles.transparent_max_bounces = 4
sc.cycles.caustics_reflective = False
sc.cycles.caustics_refractive = False
sc.cycles.sample_clamp_indirect = 4.0

cam_data = bpy.data.cameras.new('cam')
cam = bpy.data.objects.new('cam', cam_data)
sc.collection.objects.link(cam)
sc.camera = cam
pos = CAM['pos']
cam.location = V(pos[0], pos[1], pos[2])
cam.rotation_euler = Euler((math.pi / 2 - CAM['pitch'], 0, 0), 'XYZ')
cam_data.sensor_fit = 'HORIZONTAL'
cam_data.sensor_width = 36.0
cam_data.lens = 36.0 * CAM['f'] / IMG_W
cam_data.clip_start = 0.05
cam_data.clip_end = 20
cam_data.shift_x = (CAM['cx'] - IMG_W / 2) / IMG_W * -1.0
cam_data.shift_y = (CAM['cy'] - IMG_H / 2) / IMG_W


def proj(x, y, z=0.0):
    """the game's projection (pbtable.Camera), image pixels"""
    th = CAM['pitch']
    qx, qy, qz = x - pos[0], y - pos[1], z - pos[2]
    yc = -qy * math.sin(th) + qz * math.cos(th)
    zc = -qy * math.cos(th) - qz * math.sin(th)
    return CAM['cx'] + CAM['f'] * qx / zc, CAM['cy'] - CAM['f'] * yc / zc, zc


def check_camera():
    bpy.context.view_layer.update()
    worst = 0
    for (x, y, z) in ((0, 0, 0), (W, 0, 0), (0, H, 0), (W, H, 0), (W / 2, H / 2, 50), (100, 300, 30)):
        v = world_to_camera_view(sc, cam, V(x, y, z))
        bx, by = v.x * IMG_W, (1 - v.y) * IMG_H
        gx, gy, _ = proj(x, y, z)
        worst = max(worst, abs(bx - gx), abs(by - gy))
    return worst


err = check_camera()
if err > 0.5:
    cam_data.shift_x = -cam_data.shift_x
    err = check_camera()
print('camera matches the game within %.3f px' % err)
assert err < 0.5, 'the camera does not match the game'

VIEW_D = np.array([0.0, math.cos(CAM['pitch']), -math.sin(CAM['pitch'])])  # world direction of view
CAM_W = np.array(cam.location)

# world light: the theme's dark ambient
world = bpy.data.worlds.new('world')
sc.world = world
world.use_nodes = True
wbg = world.node_tree.nodes['Background']
wbg.inputs['Color'].default_value = col(TH.get('ambient', 0x202430))
wbg.inputs['Strength'].default_value = 0.35

# ---------------------------------------------------------------------------
# materials
# ---------------------------------------------------------------------------

MATS = {}


def principled(name, color, rough=0.4, metal=0.0, clear=0.0, emit=None, emit_k=0.0, trans=0.0, spec=0.5):
    if name in MATS:
        return MATS[name]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes['Principled BSDF']
    b.inputs['Base Color'].default_value = color if len(color) == 4 else col(color)
    b.inputs['Roughness'].default_value = rough
    b.inputs['Metallic'].default_value = metal
    b.inputs['Clearcoat'].default_value = clear
    b.inputs['Specular'].default_value = spec
    if trans:
        b.inputs['Transmission'].default_value = trans
    if emit is not None:
        b.inputs['Emission'].default_value = emit if len(emit) == 4 else col(emit)
        b.inputs['Emission Strength'].default_value = emit_k
    MATS[name] = m
    return m


def set_emit(m, k):
    m.node_tree.nodes['Principled BSDF'].inputs['Emission Strength'].default_value = k


def image_mat(name, path, rough=0.45, clear=0.25):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    t = nt.nodes.new('ShaderNodeTexImage')
    t.image = bpy.data.images.load(path)
    t.interpolation = 'Cubic'
    nt.links.new(t.outputs['Color'], b.inputs['Base Color'])
    b.inputs['Roughness'].default_value = rough
    b.inputs['Clearcoat'].default_value = clear
    b.inputs['Clearcoat Roughness'].default_value = 0.08
    b.inputs['Specular'].default_value = 0.35
    return m


M_RAIL = principled('rail', col(TH.get('rail', 0xB8BEC8)), rough=0.18, metal=1.0)
M_CHROME = principled('chrome', (0.9, 0.9, 0.92, 1), rough=0.06, metal=1.0)
M_RUBBER = principled('rubber', (0.012, 0.012, 0.012, 1), rough=0.55, spec=0.3)
M_WOOD = principled('wood', col(TH.get('wood', 0x3A2416)), rough=0.45, clear=0.4)
M_CAB = principled('cabinet', col(TH.get('cabinet', 0x201410)), rough=0.5, clear=0.3)
M_PLASTIC = principled('plastic', col(TH.get('plastic', TH['accent']), 0.8), rough=0.15, clear=0.6, trans=0.3)
M_WHITE = principled('white', (0.8, 0.8, 0.8, 1), rough=0.3, clear=0.4)
M_BLACK = principled('black', (0.02, 0.02, 0.02, 1), rough=0.5)
M_POSTW = principled('postwhite', (0.85, 0.85, 0.8, 1), rough=0.25)


def lamp_mat(name, color, off=0.0):
    """a lamp's material: off it is coloured plastic, on it glows"""
    return principled('lamp_' + name, col(color, 0.55), rough=0.12, clear=0.8, emit=col(color), emit_k=off,
                      trans=0.15)


# ---------------------------------------------------------------------------
# geometry helpers
# ---------------------------------------------------------------------------

ALL = []
GROUP = {}


def link(o, group=None):
    sc.collection.objects.link(o)
    ALL.append(o)
    if group:
        GROUP.setdefault(group, []).append(o)
    return o


def mesh_obj(name, verts, faces, mat, group=None, smooth=False):
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(v) for v in verts], [], faces)
    me.update()
    if smooth:
        for p in me.polygons:
            p.use_smooth = True
    o = bpy.data.objects.new(name, me)
    o.data.materials.append(mat)
    return link(o, group)


def prism(name, outline, z0, z1, mat, group=None, top_mat=None):
    """an outline (table mm, any winding) extruded from z0 to z1"""
    n = len(outline)
    verts = [V(x, y, z0) for x, y in outline] + [V(x, y, z1) for x, y in outline]
    bm = bmesh.new()
    vs = [bm.verts.new(v) for v in verts]
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new((vs[i], vs[j], vs[n + j], vs[n + i]))
    try:
        top = bm.faces.new(vs[n:])
        bot = bm.faces.new(list(reversed(vs[:n])))
    except ValueError:
        top = None
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    o = bpy.data.objects.new(name, me)
    o.data.materials.append(mat)
    if top_mat is not None:
        o.data.materials.append(top_mat)
        for p in o.data.polygons:
            if p.normal.z > 0.9:
                p.material_index = 1
    return link(o, group)


def offset(pts, d):
    out = []
    n = len(pts)
    for i in range(n):
        if i == 0:
            tx, ty = pts[1][0] - pts[0][0], pts[1][1] - pts[0][1]
        elif i == n - 1:
            tx, ty = pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]
        else:
            tx, ty = pts[i + 1][0] - pts[i - 1][0], pts[i + 1][1] - pts[i - 1][1]
        l = math.hypot(tx, ty) or 1
        out.append((pts[i][0] + ty / l * d, pts[i][1] - tx / l * d))
    return out


def strip_wall(name, pts, h, thick, mat, z0=0.0, group=None, top_mat=None):
    """a wall along a polyline, one box per segment (no concave polygons)"""
    objs = []
    for i in range(len(pts) - 1):
        a, b = pts[i], pts[i + 1]
        L = offset([a, b], thick / 2)
        R = offset([a, b], -thick / 2)
        # a little longer, so the joints close
        dx, dy = b[0] - a[0], b[1] - a[1]
        l = math.hypot(dx, dy) or 1
        ex, ey = dx / l * thick * 0.45, dy / l * thick * 0.45
        outline = [(L[0][0] - ex, L[0][1] - ey), (L[1][0] + ex, L[1][1] + ey), (R[1][0] + ex, R[1][1] + ey),
                   (R[0][0] - ex, R[0][1] - ey)]
        objs.append(prism('%s_%d' % (name, i), outline, z0, z0 + h, mat, group, top_mat))
    return objs


def cyl(name, x, y, r, z0, z1, mat, n=32, group=None):
    o = prism(name, [(x + r * math.cos(i / n * 2 * math.pi), y + r * math.sin(i / n * 2 * math.pi)) for i in range(n)],
              z0, z1, mat, group)
    for p in o.data.polygons:
        p.use_smooth = abs(p.normal.z) < 0.5
    return o


def sphere(name, x, y, z, r, mat, scale=(1, 1, 1), group=None, segs=32):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=segs, v_segments=segs // 2, radius=r / 1000.0)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    for p in me.polygons:
        p.use_smooth = True
    o = bpy.data.objects.new(name, me)
    o.data.materials.append(mat)
    o.location = V(x, y, z)
    o.scale = scale
    return link(o, group)


def cone(name, x, y, z, r1, r2, h, mat, n=32, group=None, rot=(0, 0, 0)):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=n, radius1=r1 / 1000.0, radius2=r2 / 1000.0,
                          depth=h / 1000.0)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    for p in me.polygons:
        p.use_smooth = abs(p.normal.z) < 0.7
    o = bpy.data.objects.new(name, me)
    o.data.materials.append(mat)
    o.location = V(x, y, z + h / 2)
    o.rotation_euler = rot
    return link(o, group)


def box(name, x, y, z, sx, sy, sz, mat, rot=0.0, group=None, tilt=(0, 0)):
    """a box centred on (x, y), from z to z + sz; rot in degrees about z
    (table angle, y down)"""
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    o = bpy.data.objects.new(name, me)
    o.data.materials.append(mat)
    o.location = V(x, y, z + sz / 2)
    o.scale = (sx / 1000.0, sy / 1000.0, sz / 1000.0)
    o.rotation_euler = (math.radians(tilt[0]), math.radians(tilt[1]), -math.radians(rot))
    return link(o, group)


def torus(name, x, y, z, R, r, mat, group=None, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_torus_add(major_radius=R / 1000.0, minor_radius=r / 1000.0, major_segments=40,
                                     minor_segments=12, location=V(x, y, z), rotation=rot)
    o = bpy.context.active_object
    o.data.materials.append(mat)
    for p in o.data.polygons:
        p.use_smooth = True
    ALL.append(o)
    if group:
        GROUP.setdefault(group, []).append(o)
    return o


def tube(name, pts3, r, mat, group=None, closed=False):
    """a round tube along (x, y, z) table points: rubber bands, wires"""
    cu = bpy.data.curves.new(name, 'CURVE')
    cu.dimensions = '3D'
    cu.bevel_depth = r / 1000.0
    cu.bevel_resolution = 4
    cu.use_fill_caps = True
    sp = cu.splines.new('POLY')
    sp.points.add(len(pts3) - 1)
    for i, p in enumerate(pts3):
        v = V(*p)
        sp.points[i].co = (v.x, v.y, v.z, 1)
    sp.use_cyclic_u = closed
    o = bpy.data.objects.new(name, cu)
    o.data.materials.append(mat)
    return link(o, group)


# ---------------------------------------------------------------------------
# the table
# ---------------------------------------------------------------------------

def build_playfield():
    m = image_mat('playfield', os.path.join(args.tex, 'playfield.png'))
    me = bpy.data.meshes.new('playfield')
    vs = [V(0, 0), V(W, 0), V(W, H), V(0, H)]
    me.from_pydata([tuple(v) for v in vs], [], [(3, 2, 1, 0)])
    uv = me.uv_layers.new()
    for li, (u, v) in zip(range(4), ((0, 0), (1, 0), (1, 1), (0, 1))):
        pass
    # loops follow the face (3, 2, 1, 0): (0,H) (W,H) (W,0) (0,0)
    for li, (u, v) in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[li].uv = (u, v)
    o = bpy.data.objects.new('playfield', me)
    o.data.materials.append(m)
    link(o, 'playfield')
    # under the playfield, so a target that drops has somewhere dark to go
    box('under', W / 2, H / 2, -80, W + 200, H + 200, 2, M_BLACK)
    return o


def build_cabinet():
    # the cabinet's sides and the back, past the playfield's edges
    box('cab_l', -40, H / 2, 0, 64, H + 140, 62, M_CAB)
    box('cab_r', W + 40, H / 2, 0, 64, H + 140, 62, M_CAB)
    box('rail_l', -30, H / 2, 62, 30, H + 140, 8, M_CHROME)
    box('rail_r', W + 30, H / 2, 62, 30, H + 140, 8, M_CHROME)
    # a shelf along the back, for the theme's toys
    box('shelf', W / 2, -8, 0, W + 16, 12, 60, M_CAB)
    # the backboard behind the top arch, with the theme's picture
    bb = os.path.join(args.tex, 'backboard.png')
    m = image_mat('backboard', bb, rough=0.5, clear=0.2) if os.path.exists(bb) else M_CAB
    me = bpy.data.meshes.new('backboard')
    vs = [V(-80, -14, 0), V(W + 80, -14, 0), V(W + 80, -14, 260), V(-80, -14, 260)]
    me.from_pydata([tuple(v) for v in vs], [], [(0, 1, 2, 3)])
    uv = me.uv_layers.new()
    for li, (u, v) in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[li].uv = (u, v)
    o = bpy.data.objects.new('backboard', me)
    o.data.materials.append(m)
    link(o)
    # the corners between the arch and the cabinet, filled
    n = 24
    for side in (0, 1):
        pts = []
        cx, cy, rr = W / 2, W / 2, W / 2
        for i in range(n + 1):
            a = math.pi + (i / n) * math.pi / 2 if side == 0 else 1.5 * math.pi + (i / n) * math.pi / 2
            pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
        if side == 0:
            outline = [(-8, cy), (-8, -14)] + [(0, -14)] + pts[::-1][:-1]
            outline = [(-8, cy)] + [(-8, -14)] + [p for p in reversed(pts)]
        else:
            outline = [(W + 8, -14), (W + 8, cy)] + [p for p in reversed(pts)]
        try:
            prism('corner%d' % side, outline, 0, 60, M_WOOD)
        except Exception as e:  # noqa
            print('corner', e)


def wall_material(style, color):
    if style == 'side':
        return M_WOOD
    if style == 'plastic':
        return principled('plastic_%06x' % (color or TH.get('plastic', TH['accent'])),
                          col(color or TH.get('plastic', TH['accent']), 0.8), rough=0.15, clear=0.6, trans=0.3)
    if style == 'apron':
        return principled('apron', col(TH.get('apron', TH['panel'])), rough=0.4, clear=0.4)
    return M_RAIL


def build_items():
    lamps = {l['name']: l for l in S['lamps']}
    for k, v in enumerate(S['vis']):
        t = v['t']
        nm = '%s%d' % (t, k)
        if t == 'wall':
            if v['style'] == 'apron':
                continue
            m = wall_material(v['style'], v.get('color'))
            if v['style'] == 'rail':
                # a chrome rail on a dark post line
                strip_wall(nm, v['pts'], v['h'], v['thick'], M_RAIL)
            else:
                strip_wall(nm, v['pts'], v['h'], v['thick'], m)
        elif t == 'rubber':
            tube(nm, [(p[0], p[1], 16) for p in v['pts']], 4, M_RUBBER)
        elif t == 'post':
            if v['rubber']:
                cyl(nm, v['x'], v['y'], 3.5, 0, 30, M_POSTW)
                torus(nm + 'r', v['x'], v['y'], 14, v['r'] - 2.5, 2.8, M_RUBBER)
                cyl(nm + 'c', v['x'], v['y'], 4.5, 30, 33, M_CHROME)
            else:
                cyl(nm, v['x'], v['y'], v['r'], 0, 36, M_CHROME)
        elif t == 'bumper':
            x, y, rr = v['x'], v['y'], v['r']
            body = principled('bbody_%06x' % (v.get('body') or 0x303030), col(v.get('body') or 0x303030), rough=0.3,
                              clear=0.5)
            cap = lamp_mat(v['lamp'], v.get('cap') or TH['accent'], off=0.12)
            cyl(nm + 'base', x, y, rr + 4, 0, 4, M_BLACK)
            cyl(nm + 'body', x, y, rr - 4, 4, 30, body)
            torus(nm + 'skirt', x, y, 8, rr, 2.5, M_WHITE)
            cyl(nm + 'ring', x, y, rr + 1, 24, 28, M_CHROME)
            o = cyl(nm + 'cap', x, y, rr + 1.5, 30, 40, cap, group='lamp:' + v['lamp'])
            sphere(nm + 'dome', x, y, 40, rr * 0.9, cap, scale=(1, 1, 0.35), group='lamp:' + v['lamp'])
        elif t == 'sling':
            a, b, c = v['pts']
            m = lamp_mat(v['lamp'], v.get('color') or TH['accent'], off=0.08)
            prism(nm + 'top', [a, b, c], 26, 30, m, group='lamp:' + v['lamp'])
            prism(nm + 'base', [a, b, c], 0, 26, M_BLACK)
            for p in (a, b, c):
                cyl(nm + 'p', p[0], p[1], 3.5, 0, 34, M_POSTW)
                cyl(nm + 'pc', p[0], p[1], 4.5, 34, 37, M_CHROME)
            tube(nm + 'band', [(a[0], a[1], 15), (c[0], c[1], 15), (b[0], b[1], 15)], 4.5, M_RUBBER, closed=True)
        elif t in ('drop', 'target'):
            a, b = v['a'], v['b']
            cx, cy = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
            ln = math.hypot(b[0] - a[0], b[1] - a[1])
            ang = math.degrees(math.atan2(b[1] - a[1], b[0] - a[0]))
            m = principled('tgt_%06x' % (v.get('color') or TH['accent']), col(v.get('color') or TH['accent']),
                           rough=0.25, clear=0.6)
            grp = 'drop:' + v['obj'] if t == 'drop' else None
            box(nm, cx, cy, 0, ln, 6, 32, m, rot=ang, group=grp)
            # a white stripe across its face
            box(nm + 's', cx, cy, 20, ln * 0.9, 6.6, 5, M_WHITE, rot=ang, group=grp)
            if t == 'target':
                nx, ny = (b[1] - a[1]) / ln, -(b[0] - a[0]) / ln
                cyl(nm + 'post', cx - nx * 8, cy - ny * 8, 3, 0, 26, M_CHROME)
        elif t == 'rollover':
            x, y = v['x'], v['y']
            box(nm, x, y, 2.5, 2.2, 22, 2.2, M_CHROME)
        elif t == 'spinner':
            a, b = v['a'], v['b']
            for i, p in enumerate((a, b)):
                cyl(nm + 'b%d' % i, p[0], p[1], 2.5, 0, 44, M_CHROME)
            tube(nm + 'wire', [(a[0], a[1], 42), (b[0], b[1], 42)], 1.2, M_CHROME)
        elif t == 'saucer':
            x, y, rr = v['x'], v['y'], v['r']
            cyl(nm, x, y, rr, -10, 0.4, M_BLACK)
            torus(nm + 'ring', x, y, 1.5, rr + 1.5, 2.2, M_CHROME)
        elif t == 'gate':
            a, b = v['a'], v['b']
            tube(nm, [(a[0], a[1], 26), (b[0], b[1], 26)], 1.2, M_CHROME)
            box(nm + 'flap', (a[0] + b[0]) / 2, (a[1] + b[1]) / 2, 8, math.hypot(b[0] - a[0], b[1] - a[1]) * 0.8, 1.2,
                18, M_CHROME, rot=math.degrees(math.atan2(b[1] - a[1], b[0] - a[0])))
        elif t == 'ramp':
            build_ramp(nm, v)
        elif t == 'insert':
            l = lamps.get(v['name'])
            m = lamp_mat(v['name'], v['color'], off=0.05)
            prism(nm, v['outline'], 0.0, 0.8, m, group='lamp:' + v['name'])
        elif t == 'flasher':
            m = lamp_mat(v['name'], v['color'], off=0.05)
            sphere(nm, v['x'], v['y'], v['z'], 10, m, group='lamp:' + v['name'])
            cyl(nm + 'base', v['x'], v['y'], 6, v['z'] - 16, v['z'] - 6, M_BLACK)
        elif t == 'apron':
            build_apron(v['y'])
        elif t == 'plunger':
            x, y = v['x'], v['y']
            cyl(nm + 'tip', x, y + 6, 8, 2, 20, M_BLACK)
            box(nm + 'rod', x, y + 30, 8, 5, 40, 5, M_CHROME)
        elif t == 'decor':
            build_decor(nm, v)


def build_wire(nm, v):
    """a habitrail: four chrome wires round the ball's path"""
    path = v['path']
    pts2 = [(p[0], p[1]) for p in path]
    for side in (-1, 1):
        for dz, dw in ((2, 9), (18, 15)):
            o = offset(pts2, side * dw)
            tube(nm + 'w%d%d' % (side, dz), [(o[i][0], o[i][1], path[i][2] + dz) for i in range(len(path))], 1.1,
                 M_CHROME)
    for i in range(1, len(path) - 1, 2):
        L = offset(pts2, 16)[i]
        R = offset(pts2, -16)[i]
        tube(nm + 'r%d' % i, [(L[0], L[1], path[i][2] + 18), (L[0], L[1], path[i][2]), (R[0], R[1], path[i][2]),
                              (R[0], R[1], path[i][2] + 18)], 1.0, M_CHROME)
        if path[i][2] > 25:
            cyl(nm + 'leg%d' % i, path[i][0], path[i][1], 1.6, 0, path[i][2], M_CHROME)


def build_ramp(nm, v):
    if v.get('style') == 'wire':
        build_wire(nm, v)
        return
    path = v['path']
    w = v['width'] / 2
    color = v.get('color') or TH['accent']
    m = principled('ramp_%06x' % color, col(color, 0.7), rough=0.12, clear=0.7, trans=0.4)
    pts2 = [(p[0], p[1]) for p in path]
    L = offset(pts2, w)
    R = offset(pts2, -w)
    n = len(path)
    verts, faces = [], []
    for i in range(n):
        z = path[i][2]
        verts += [V(L[i][0], L[i][1], z), V(R[i][0], R[i][1], z), V(L[i][0], L[i][1], z + 22),
                  V(R[i][0], R[i][1], z + 22)]
    for i in range(n - 1):
        a, b = i * 4, (i + 1) * 4
        faces.append((a, a + 1, b + 1, b))              # floor
        faces.append((a, b, b + 2, a + 2))              # left side
        faces.append((a + 1, a + 3, b + 3, b + 1))      # right side
    o = mesh_obj(nm, verts, faces, m)
    mod = o.modifiers.new('solid', 'SOLIDIFY')
    mod.thickness = 0.003
    # chrome lips on its sides, and legs where it is high
    tube(nm + 'lipL', [(L[i][0], L[i][1], path[i][2] + 22) for i in range(n)], 1.6, M_CHROME)
    tube(nm + 'lipR', [(R[i][0], R[i][1], path[i][2] + 22) for i in range(n)], 1.6, M_CHROME)
    for i in range(1, n - 1, 3):
        if path[i][2] > 25:
            cyl(nm + 'leg%d' % i, L[i][0], L[i][1], 2.2, 0, path[i][2], M_CHROME)
            cyl(nm + 'leg%db' % i, R[i][0], R[i][1], 2.2, 0, path[i][2], M_CHROME)


def build_apron(y0):
    m = principled('apron', col(TH.get('apron', TH['panel'])), rough=0.35, clear=0.5)
    lane = S['lane_x']
    prism('apronL', [(0, y0 + 5), (112, y0 + 5), (112, H + 6), (0, H + 6)], 0, 30, m)
    prism('apronR', [(lane - 110, y0 + 5), (lane, y0 + 5), (lane, H + 6), (lane - 110, H + 6)], 0, 30, m)
    prism('apronB', [(112, 1022), (lane - 110, 1022), (lane - 110, H + 6), (112, H + 6)], 0, 30, m)


# ---- the themes' decorations --------------------------------------------------

def m_(name, c, rough=0.5, metal=0.0, emit_k=0.0, clear=0.0):
    return principled('d_' + name, col(c), rough=rough, metal=metal, emit=col(c), emit_k=emit_k, clear=clear)


def deco_zombis(nm, kind, x, y, s, rot, v):
    stone = m_('stone', 0x6A6E68, 0.8)
    if kind == 'tomb':
        box(nm, x, y, 0, 26 * s, 8 * s, 30 * s, stone, rot=rot)
        o = cyl(nm + 't', x, y, 13 * s, 0, 8 * s, stone)
        o.rotation_euler = (math.pi / 2, 0, -math.radians(rot))
        o.location = V(x, y, 30 * s) + Vector((0, 0.004 * s, 0))
        box(nm + 'b', x, y + 6 * s, 0, 34 * s, 16 * s, 4 * s, m_('dirt', 0x2A2418, 0.9), rot=rot)
    elif kind == 'hand':
        skin = m_('skin', 0x6E8C4A, 0.6)
        cyl(nm + 'arm', x, y, 5 * s, 0, 30 * s, skin)
        box(nm + 'palm', x, y, 30 * s, 14 * s, 6 * s, 14 * s, skin, rot=rot)
        for i in range(4):
            fx = x + (-5 + i * 3.4) * s
            cyl(nm + 'f%d' % i, fx, y, 1.6 * s, 44 * s, (54 + (i % 2) * 4) * s, skin)
        cyl(nm + 'th', x + 8 * s, y, 1.8 * s, 34 * s, 44 * s, skin)
        box(nm + 'dirt', x, y, 0, 26 * s, 20 * s, 3, m_('dirt', 0x2A2418, 0.9))
    elif kind == 'crypt':
        g = m_('crypt', 0x5A5E66, 0.7)
        box(nm, x, y, 0, 44 * s, 30 * s, 6 * s, g)
        for i in (-1, 1):
            cyl(nm + 'c%d' % i, x + i * 16 * s, y + 10 * s, 3.5 * s, 6 * s, 40 * s, g)
        box(nm + 'roof', x, y, 40 * s, 48 * s, 32 * s, 5 * s, g)
        box(nm + 'door', x, y - 8 * s, 6 * s, 16 * s, 4 * s, 26 * s, m_('door', 0x101410, 0.9))
        sphere(nm + 'glow', x, y - 6 * s, 20 * s, 3 * s, m_('ghost', 0x7CFF3A, 0.4, emit_k=6))
    elif kind == 'tree':
        bark = m_('bark', 0x2A1E16, 0.9)
        cone(nm, x, y, 0, 7 * s, 3 * s, 70 * s, bark)
        for i, (bx, by, bz, a) in enumerate(((10, 0, 50, 40), (-12, 0, 40, -45), (6, 4, 62, 25))):
            o = cone(nm + 'b%d' % i, x + bx * s * 0.5, y + by, (bz - 10) * s, 2.5 * s, 0.8 * s, 28 * s, bark)
            o.rotation_euler = (0, math.radians(a), 0)
    elif kind == 'coffin':
        wood = m_('coffin', 0x3A2214, 0.5, clear=0.6)
        outline = [(-7, -20), (7, -20), (11, -8), (7, 22), (-7, 22), (-11, -8)]
        a = math.radians(rot)
        pts = [(x + (px * math.cos(a) - py * math.sin(a)) * s, y + (px * math.sin(a) + py * math.cos(a)) * s)
               for px, py in outline]
        prism(nm, pts, 0, 10 * s, wood)
        box(nm + 'x', x, y - 6 * s, 10 * s, 3 * s, 16 * s, 1, m_('gold', 0xC8A040, 0.3, metal=1))
        box(nm + 'y', x, y - 9 * s, 10 * s, 10 * s, 3 * s, 1, m_('gold', 0xC8A040, 0.3, metal=1))
    elif kind == 'moon':
        sphere(nm, x, y, 60, v.get('r', 30), m_('moon', 0xFFF2B0, 0.6, emit_k=1.5), scale=(1, 1, 1))
    elif kind == 'grave':
        sphere(nm + 'mound', x, y + 2, 0, 24 * s, m_('dirt', 0x2A2418, 0.9), scale=(1, 0.8, 0.18))
        box(nm + 'stone', x, y - 22 * s, 0, 24 * s, 7 * s, 30 * s, stone)
        cone(nm + 'top', x, y - 22 * s, 30 * s - 3.5 * s, 12 * s, 12 * s, 7 * s, stone, rot=(math.pi / 2, 0, 0))


DECO = {'zombis': deco_zombis}


def wedge_disc(nm, x, y, z0, z1, r, mats, n, r0=0.0, start=0.0):
    """a disc of n slices, materials in turn: a roulette, a tent's stripes"""
    objs = []
    for i in range(n):
        a0 = start + i / n * 2 * math.pi
        a1 = start + (i + 1) / n * 2 * math.pi
        pts = [(x + r0 * math.cos(a0), y + r0 * math.sin(a0))] if r0 else [(x, y)]
        for k in range(4):
            a = a0 + (a1 - a0) * k / 3
            pts.append((x + r * math.cos(a), y + r * math.sin(a)))
        if r0:
            pts.append((x + r0 * math.cos(a1), y + r0 * math.sin(a1)))
        objs.append(prism('%s_w%d' % (nm, i), pts, z0, z1, mats[i % len(mats)]))
    return objs


def striped_cone(nm, x, y, z, r, h, mats, n=12):
    """a cone in n coloured gores (the big top)"""
    for i in range(n):
        a0, a1 = i / n * 2 * math.pi, (i + 1) / n * 2 * math.pi
        verts = [V(x, y, z + h), V(x + r * math.cos(a0), y + r * math.sin(a0), z),
                 V(x + r * math.cos(a1), y + r * math.sin(a1), z)]
        mesh_obj('%s_g%d' % (nm, i), verts, [(0, 1, 2)], mats[i % len(mats)])


def deco_robots(nm, kind, x, y, s, rot, v):
    steel = m_('steel', 0xB8C4D4, 0.25, metal=0.9)
    blue = m_('rblue', 0x2050C0, 0.35, clear=0.5)
    glow = m_('rglow', 0x30E0FF, 0.3, emit_k=8)
    pink = m_('rpink', 0xFF40C0, 0.3, emit_k=5)
    if kind == 'robot':
        box(nm + 'legs', x - 6 * s, y, 0, 5 * s, 6 * s, 18 * s, steel, rot=rot)
        box(nm + 'legs2', x + 6 * s, y, 0, 5 * s, 6 * s, 18 * s, steel, rot=rot)
        box(nm + 'body', x, y, 18 * s, 24 * s, 16 * s, 24 * s, blue, rot=rot)
        box(nm + 'panel', x, y + 8 * s, 24 * s, 12 * s, 1.5, 10 * s, glow, rot=rot)
        box(nm + 'head', x, y, 44 * s, 18 * s, 14 * s, 14 * s, steel, rot=rot)
        for sx in (-1, 1):
            sphere(nm + 'eye%d' % sx, x + sx * 4.5 * s, y + 7 * s, 51 * s, 2.8 * s, glow)
            box(nm + 'arm%d' % sx, x + sx * 15 * s, y, 22 * s, 5 * s, 5 * s, 18 * s, steel, rot=rot)
        cyl(nm + 'ant', x, y, 0.8 * s, 58 * s, 68 * s, steel)
        sphere(nm + 'antb', x, y, 69 * s, 2.5 * s, pink)
    elif kind == 'rocket':
        white = m_('rwhite', 0xF0F2F6, 0.3, clear=0.6)
        red = m_('rred', 0xE02830, 0.3, clear=0.6)
        cyl(nm + 'body', x, y, 12 * s, 14 * s, 70 * s, white)
        cone(nm + 'nose', x, y, 70 * s, 12 * s, 0.5, 26 * s, red)
        sphere(nm + 'win', x, y + 11 * s, 52 * s, 5 * s, glow, scale=(1, 0.4, 1))
        for i in range(3):
            a = i / 3 * 2 * math.pi
            box(nm + 'fin%d' % i, x + math.cos(a) * 13 * s, y + math.sin(a) * 13 * s, 10 * s, 10 * s, 2.5 * s,
                22 * s, red, rot=math.degrees(a))
        cone(nm + 'flame', x, y, 0, 8 * s, 2 * s, 14 * s, m_('flame', 0xFF8A20, 0.5, emit_k=10))
    elif kind == 'planet':
        sphere(nm, x, y, 34 * s, 24 * s, m_('planet', 0x8A50D0, 0.6))
        torus(nm + 'ring', x, y, 34 * s, 36 * s, 3 * s, m_('ring', 0xE0C080, 0.4), rot=(0.4, 0.2, 0))
        cyl(nm + 'stand', x, y, 2 * s, 0, 12 * s, steel)
    elif kind == 'dish':
        cyl(nm + 'post', x, y, 2.5 * s, 0, 22 * s, steel)
        o = sphere(nm + 'dish', x, y, 30 * s, 16 * s, steel, scale=(1, 1, 0.35))
        o.rotation_euler = (0.6, 0, math.radians(rot))
        sphere(nm + 'tip', x, y, 36 * s, 2.5 * s, pink)
    elif kind == 'pad':
        cyl(nm, x, y, 30 * s, 0, 1.0, m_('padm', 0x404858, 0.4, metal=0.6))
        for i in range(8):
            a = i * math.pi / 4
            box(nm + 's%d' % i, x + math.cos(a) * 25 * s, y + math.sin(a) * 25 * s, 1.0, 8 * s, 3 * s, 0.6,
                m_('hazard', 0xFFC020, 0.4), rot=math.degrees(a) + 90)
    elif kind == 'ufo':
        sphere(nm, x, y, 16 * s, 22 * s, steel, scale=(1, 1, 0.3))
        sphere(nm + 'dome', x, y, 20 * s, 10 * s, m_('udome', 0x60F0FF, 0.1, emit_k=2), scale=(1, 1, 0.8))
        for i in range(8):
            a = i / 8 * 2 * math.pi
            sphere(nm + 'l%d' % i, x + math.cos(a) * 19 * s, y + math.sin(a) * 19 * s, 15 * s, 1.8 * s, pink)


def deco_jungla(nm, kind, x, y, s, rot, v):
    stone = m_('jstone', 0x7A7460, 0.85)
    moss = m_('moss', 0x4A7A30, 0.8)
    if kind == 'palm':
        trunk = m_('trunk', 0x6A4A28, 0.9)
        leaf = m_('leaf', 0x3A9A2A, 0.6)
        for i in range(6):
            cyl(nm + 't%d' % i, x + i * 1.2 * s, y, (6 - i * 0.5) * s, i * 12 * s, (i + 1) * 12 * s, trunk)
        top = (x + 7 * s, y, 72 * s)
        for i in range(7):
            a = i / 7 * 2 * math.pi + math.radians(rot)
            o = box(nm + 'l%d' % i, top[0] + math.cos(a) * 18 * s, top[1] + math.sin(a) * 18 * s, top[2] - 6 * s,
                    36 * s, 9 * s, 1.5 * s, leaf, rot=math.degrees(a), tilt=(0, 18))
        sphere(nm + 'c', top[0], top[1], top[2], 5 * s, m_('coco', 0x5A3A1A, 0.7))
    elif kind == 'temple':
        for i in range(4):
            w = (60 - i * 12) * s
            box(nm + 's%d' % i, x, y, i * 12 * s, w, w * 0.8, 12 * s, stone)
        box(nm + 'top', x, y, 48 * s, 14 * s, 12 * s, 14 * s, moss)
        box(nm + 'door', x, y + 18 * s, 0, 14 * s, 4 * s, 18 * s, m_('dark', 0x0A0806, 0.9))
        sphere(nm + 'gem', x, y + 4 * s, 56 * s, 4 * s, m_('gem', 0xFFA020, 0.2, emit_k=6))
    elif kind == 'idol':
        box(nm, x, y, 0, 22 * s, 16 * s, 40 * s, stone, rot=rot)
        box(nm + 'brow', x, y + 8 * s, 28 * s, 24 * s, 4 * s, 4 * s, moss, rot=rot)
        for sx in (-1, 1):
            sphere(nm + 'eye%d' % sx, x + sx * 5 * s, y + 8 * s, 24 * s, 3 * s, m_('ieye', 0xFFA020, 0.3, emit_k=8))
        box(nm + 'mouth', x, y + 8 * s, 8 * s, 14 * s, 3 * s, 6 * s, m_('dark', 0x0A0806, 0.9), rot=rot)
    elif kind == 'snake':
        green = m_('snake', 0x2A8A3A, 0.4, clear=0.5)
        pts = [(x + 18 * s * math.cos(a / 6), y + 10 * s * math.sin(a / 3), 4 * s + a * 1.2 * s) for a in range(0, 30)]
        tube(nm, pts, 3.2 * s, green)
        sphere(nm + 'h', pts[-1][0], pts[-1][1], pts[-1][2] + 2 * s, 5 * s, green, scale=(1.3, 1, 0.8))
    elif kind == 'rocks':
        for i, (dx, dy, r) in enumerate(((0, 0, 12), (14, 6, 9), (-12, 8, 8), (4, -10, 7))):
            sphere(nm + str(i), x + dx * s, y + dy * s, r * 0.6 * s, r * s, stone if i % 2 else moss, scale=(1, 1, 0.7))


def deco_circo(nm, kind, x, y, s, rot, v):
    red = m_('cred', 0xE8243C, 0.3, clear=0.6)
    white = m_('cwhite', 0xF4F0E8, 0.3, clear=0.6)
    gold = m_('cgold', 0xFFC840, 0.25, metal=0.8)
    blue = m_('cblue', 0x2A6AE0, 0.3, clear=0.6)
    if kind == 'tent':
        cyl(nm + 'wall', x, y, 34 * s, 0, 22 * s, red)
        wedge_disc(nm + 'band', x, y, 22 * s, 24 * s, 35 * s, [white, red], 16)
        striped_cone(nm + 'roof', x, y, 24 * s, 38 * s, 34 * s, [red, white], 16)
        cyl(nm + 'pole', x, y, 1.2 * s, 58 * s, 72 * s, gold)
        o = box(nm + 'flag', x + 6 * s, y, 66 * s, 10 * s, 1, 6 * s, blue)
    elif kind == 'ball':
        sphere(nm, x, y, 18 * s, 18 * s, red)
        torus(nm + 'b1', x, y, 18 * s, 17.6 * s, 3.5 * s, white, rot=(math.pi / 2, 0, 0.6))
        torus(nm + 'b2', x, y, 18 * s, 17.6 * s, 3.5 * s, blue, rot=(math.pi / 2, 0, 2.2))
        sphere(nm + 'star', x, y, 36 * s, 4 * s, gold)
    elif kind == 'podium':
        wedge_disc(nm, x, y, 0, 26 * s, 22 * s, [red, white], 12)
        cyl(nm + 'top', x, y, 23 * s, 26 * s, 29 * s, gold)
        prism(nm + 'star', [(x + (9 if i % 2 == 0 else 4) * s * math.cos(i * math.pi / 5 - math.pi / 2),
                             y + (9 if i % 2 == 0 else 4) * s * math.sin(i * math.pi / 5 - math.pi / 2))
                            for i in range(10)], 29 * s, 31 * s, m_('star', 0xFFE060, 0.3, emit_k=3))
    elif kind == 'balloons':
        for i, (dx, dy, c) in enumerate(((0, 0, 0xE8243C), (10, 4, 0x2A6AE0), (-9, 5, 0xFFD040), (4, -8, 0x30C060))):
            sphere(nm + str(i), x + dx * s, y + dy * s, (46 + i * 4) * s, 8 * s, m_('bal%d' % i, c, 0.2, clear=0.8),
                   scale=(1, 1, 1.2))
            tube(nm + 's%d' % i, [(x, y, 0), (x + dx * s, y + dy * s, (38 + i * 4) * s)], 0.4, white)
    elif kind == 'cannon':
        black = m_('iron', 0x202024, 0.3, metal=0.8)
        cone(nm + 'barrel', x, y, -6 * s, 7 * s, 6 * s, 40 * s, black, rot=(math.radians(55), 0, -math.radians(rot)))
        for sx in (-1, 1):
            torus(nm + 'w%d' % sx, x + sx * 9 * s, y, 10 * s, 8 * s, 2 * s, gold, rot=(0, math.pi / 2, 0))


def deco_casino(nm, kind, x, y, s, rot, v):
    red = m_('kred', 0xD01830, 0.3, clear=0.7)
    black = m_('kblack', 0x141414, 0.3, clear=0.7)
    gold = m_('kgold', 0xFFC830, 0.2, metal=0.9)
    white = m_('kwhite', 0xF4F0E8, 0.3, clear=0.5)
    green = m_('kgreen', 0x109040, 0.4)
    if kind == 'slot':
        box(nm, x, y, 0, 34 * s, 22 * s, 50 * s, red)
        box(nm + 'win', x, y + 11 * s, 26 * s, 26 * s, 1.5, 14 * s, m_('reels', 0xF8F4E0, 0.3, emit_k=1.5))
        for i in range(3):
            box(nm + 'r%d' % i, x + (i - 1) * 8 * s, y + 12 * s, 30 * s, 6 * s, 1.2, 6 * s,
                m_('sym%d' % i, [0xE01830, 0xFFC830, 0x2060E0][i], 0.3, emit_k=2))
        box(nm + 'top', x, y, 50 * s, 36 * s, 24 * s, 6 * s, gold)
        cyl(nm + 'arm', x + 20 * s, y, 1.5 * s, 20 * s, 48 * s, gold)
        sphere(nm + 'knob', x + 20 * s, y, 50 * s, 4 * s, red)
    elif kind == 'wheel':
        cyl(nm + 'base', x, y, 40 * s, 0, 10 * s, m_('wood2', 0x5A2A10, 0.4, clear=0.6))
        wedge_disc(nm, x, y, 10 * s, 12 * s, 34 * s, [red, black] * 18 + [green], 37, r0=18 * s)
        cyl(nm + 'hub', x, y, 18 * s, 10 * s, 14 * s, gold)
        cyl(nm + 'spin', x, y, 3 * s, 14 * s, 24 * s, gold)
        sphere(nm + 'ball', x + 26 * s, y, 14 * s, 2.5 * s, white)
    elif kind == 'chips':
        for i, (dx, dy, c, n) in enumerate(((0, 0, red, 6), (16, 6, black, 4), (-14, 8, m_('kblue', 0x2050C0, 0.3,
                                                                                               clear=0.7), 5))):
            for k in range(n):
                cyl(nm + '%d_%d' % (i, k), x + dx * s, y + dy * s, 9 * s, k * 3.2 * s, (k + 1) * 3.2 * s - 0.4,
                    c if k % 2 == 0 else white)
    elif kind == 'dice':
        for i, (dx, dy, rr) in enumerate(((0, 0, rot), (16 * s, 8 * s, rot + 35))):
            box(nm + str(i), x + dx, y + dy, 0, 14 * s, 14 * s, 14 * s, white, rot=rr)
            for k, (px, py) in enumerate(((-3.5, -3.5), (3.5, 3.5), (0, 0), (-3.5, 3.5), (3.5, -3.5))[:3 + i * 2]):
                sphere(nm + 'p%d%d' % (i, k), x + dx + px * s, y + dy + py * s, 14 * s, 1.5 * s, black)
    elif kind == 'slotbig':
        box(nm, x, y, 0, 44 * s, 32 * s, 60 * s, red)
        box(nm + 'top', x, y - 2 * s, 60 * s, 46 * s, 30 * s, 10 * s, gold)
        box(nm + 'win', x, y + 16 * s, 30 * s, 36 * s, 1.5, 18 * s, m_('reels', 0xF8F4E0, 0.3, emit_k=1.5))
        for i in range(3):
            box(nm + 'r%d' % i, x + (i - 1) * 11 * s, y + 17 * s, 34 * s, 8 * s, 1.2, 9 * s,
                m_('sym%d' % i, [0xE01830, 0xFFC830, 0x2060E0][i], 0.3, emit_k=2))
        cyl(nm + 'arm', x + 26 * s, y, 2 * s, 24 * s, 64 * s, gold)
        sphere(nm + 'knob', x + 26 * s, y, 66 * s, 5 * s, red)
        for i in range(10):
            sphere(nm + 'b%d' % i, x - 20 * s + i * 4.4 * s, y + 16 * s, 72 * s, 1.6 * s, m_('bulb', 0xFFE080, 0.3, emit_k=6))
    elif kind == 'cards':
        for i in range(4):
            box(nm + str(i), x + i * 5 * s, y, i * 1.2 * s, 22 * s, 32 * s, 1.0 * s, white, rot=rot + i * 12)
        box(nm + 'suit', x + 15 * s, y, 5 * s, 6 * s, 6 * s, 1, red, rot=rot + 81)


def deco_piratas(nm, kind, x, y, s, rot, v):
    wood = m_('pwood', 0x6A4020, 0.6, clear=0.3)
    dark = m_('pdark', 0x2A1A0E, 0.6)
    gold = m_('pgold', 0xFFC040, 0.2, metal=0.9)
    white = m_('psail', 0xF0E6D0, 0.6)
    if kind == 'palm':
        deco_jungla(nm, 'palm', x, y, s, rot, v)
    elif kind == 'ship':
        hull = [(-34, -10), (30, -10), (40, 0), (30, 10), (-34, 10), (-40, 0)]
        a = math.radians(rot)
        prism(nm + 'hull', [(x + px * s, y + py * s) for px, py in hull], 0, 16 * s, wood)
        box(nm + 'deck', x - 26 * s, y, 16 * s, 16 * s, 18 * s, 8 * s, dark)
        for i, mx_ in enumerate((-10, 14)):
            cyl(nm + 'mast%d' % i, x + mx_ * s, y, 1.6 * s, 16 * s, 70 * s, dark)
            box(nm + 'sail%d' % i, x + mx_ * s, y + 2 * s, 30 * s, 26 * s, 1.5, 28 * s, white)
        box(nm + 'flag', x + 18 * s, y, 66 * s, 12 * s, 1, 8 * s, m_('pflag', 0x101010, 0.6))
    elif kind == 'chest':
        box(nm, x, y, 0, 34 * s, 22 * s, 16 * s, wood, rot=rot)
        cone(nm + 'lid', x, y, -1 * s, 11 * s, 11 * s, 34 * s, wood, rot=(0, math.pi / 2, -math.radians(rot)))
        for dx in (-12, 12):
            box(nm + 'band%d' % dx, x + dx * s, y, 0, 3 * s, 23 * s, 17 * s, gold, rot=rot)
        for i in range(8):
            sphere(nm + 'g%d' % i, x + (i % 4 - 1.5) * 6 * s, y + (i // 4 - 0.5) * 6 * s, 17 * s, 3 * s,
                   m_('coin', 0xFFD040, 0.2, metal=1.0, emit_k=0.5), scale=(1, 1, 0.4))
    elif kind == 'skull':
        bone = m_('pbone', 0xE0D8C0, 0.5)
        sphere(nm, x, y, 14 * s, 12 * s, bone)
        box(nm + 'jaw', x, y + 4 * s, 2 * s, 12 * s, 10 * s, 8 * s, bone)
        for sx in (-1, 1):
            sphere(nm + 'e%d' % sx, x + sx * 4.5 * s, y + 9 * s, 15 * s, 3.2 * s, m_('psock', 0x0A0806, 0.9))
            o = box(nm + 'x%d' % sx, x, y, 1, 34 * s, 4 * s, 4 * s, bone, rot=45 * sx)
    elif kind == 'anchor':
        iron = m_('piron', 0x30343A, 0.35, metal=0.8)
        box(nm + 'shank', x, y, 4, 4 * s, 4 * s, 40 * s, iron)
        box(nm + 'stock', x, y, 36 * s, 22 * s, 3 * s, 3 * s, iron)
        torus(nm + 'ring', x, y, 46 * s, 4 * s, 1.2 * s, iron, rot=(math.pi / 2, 0, 0))
        torus(nm + 'arms', x, y, 12 * s, 14 * s, 2 * s, iron, rot=(math.pi / 2, 0, 0))
    elif kind == 'tentacles':
        green = m_('kraken', 0x2A8A6A, 0.4, clear=0.5)
        cyl(nm + 'hole', x, y, 26 * s, -6, 0.6, m_('water', 0x082838, 0.1, clear=1.0))
        for i in range(6):
            a = i / 6 * 2 * math.pi
            r0 = 34 * s
            pts = []
            for k in range(10):
                u = k / 9
                rr = r0 + 6 * math.sin(u * 3 + i)
                pts.append((x + math.cos(a + u * 0.8) * rr, y + math.sin(a + u * 0.8) * rr, 2 + u * (8 + 4 * (i % 2)) * s))
            tube(nm + 't%d' % i, pts, 3.5 * s, green)
    elif kind == 'barrels':
        for i, (dx, dy) in enumerate(((0, 0), (16, 4), (8, -12))):
            cyl(nm + str(i), x + dx * s, y + dy * s, 8 * s, 0, 22 * s, wood)
            for zz in (4, 17):
                cyl(nm + 'h%d%d' % (i, zz), x + dx * s, y + dy * s, 8.4 * s, zz * s, (zz + 1.5) * s, dark)


DECO.update({'robots': deco_robots, 'jungla': deco_jungla, 'circo': deco_circo, 'casino': deco_casino,
             'piratas': deco_piratas})


def deco_generic(nm, kind, x, y, s, rot, v):
    cyl(nm, x, y, 10 * s, 0, 30 * s, m_('generic', TH['accent'], 0.4))


def build_decor(nm, v):
    fn = DECO.get(TH.get('art', S['id']), deco_generic)
    OFF[2] = v.get('z', 0.0)
    fn(nm, v['kind'], v['x'], v['y'], v.get('s', 1.0), v.get('rot', 0.0), v)
    OFF[2] = 0.0


# ---- flippers, spinner, ball ----------------------------------------------------

def flipper_obj(f):
    """the flipper pointing along +x world from its pivot; an empty at the
    pivot turns it"""
    L, r0, r1 = f['len'], f['r0'], f['r1']
    out = []
    for i in range(13):
        b = math.pi / 2 + math.pi * i / 12
        out.append((r0 * math.cos(b), r0 * math.sin(b)))
    for i in range(13):
        b = -math.pi / 2 + math.pi * i / 12
        out.append((L + r1 * math.cos(b), r1 * math.sin(b)))
    piv = bpy.data.objects.new('flip_' + f['side'], None)
    link(piv)
    piv.location = V(f['x'], f['y'], 0)
    parts = []

    def outline_mesh(name, pts, z0, z1, mat, inset=0.0):
        n = len(pts)
        verts = [Vector((px / 1000, py / 1000, z0 / 1000)) for px, py in pts] + \
                [Vector((px / 1000, py / 1000, z1 / 1000)) for px, py in pts]
        faces = [(i, (i + 1) % n, n + (i + 1) % n, n + i) for i in range(n)]
        faces.append(tuple(range(n, 2 * n)))
        faces.append(tuple(reversed(range(n))))
        me = bpy.data.meshes.new(name)
        me.from_pydata([tuple(v) for v in verts], [], faces)
        me.update()
        o = bpy.data.objects.new(name, me)
        o.data.materials.append(mat)
        o.parent = piv
        link(o)
        return o
    body = principled('flipbody', col(TH.get('flipper', 0xF0F0F0)), rough=0.25, clear=0.6)
    rub = principled('fliprubber', col(TH.get('flipper_rubber', 0xC02020)), rough=0.5)
    shrink = []
    for px, py in out:
        l = math.hypot(px - L / 2, py) or 1
    # the rubber all round, the plastic body on top, slightly smaller
    parts.append(outline_mesh('fr_' + f['side'], out, 3, 19, rub))
    inner = []
    for i in range(13):
        b = math.pi / 2 + math.pi * i / 12
        inner.append(((r0 - 2.5) * math.cos(b), (r0 - 2.5) * math.sin(b)))
    for i in range(13):
        b = -math.pi / 2 + math.pi * i / 12
        inner.append((L + (r1 - 2.5) * math.cos(b), (r1 - 2.5) * math.sin(b)))
    parts.append(outline_mesh('fb_' + f['side'], inner, 2, 24, body))
    cap = bpy.data.objects.new('fc_' + f['side'], None)
    o = cyl('fcap_' + f['side'], f['x'], f['y'], 5, 24, 27, M_CHROME)
    o.parent = piv
    o.location = Vector((0, 0, 0))
    me = o.data
    # cyl was built in world coordinates round (x, y): move its mesh to the pivot's frame
    for vtx in me.vertices:
        vtx.co -= V(f['x'], f['y'], 0)
    parts.append(o)
    return piv, parts


def set_flipper(piv, f, k, n):
    ang = f['rest'] + (f['up'] - f['rest']) * k / (n - 1)
    a = math.radians(ang if f['side'] == 'L' else 180 - ang)
    # table angle (y down) -> world angle (Y up)
    piv.rotation_euler = (0, 0, -a)


def spinner_plate(v):
    a, b = v['a'], v['b']
    ln = math.hypot(b[0] - a[0], b[1] - a[1]) - 6
    ang = math.atan2(b[1] - a[1], b[0] - a[0])
    piv = bpy.data.objects.new('spinpiv', None)
    link(piv)
    piv.location = V((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, 42)
    piv.rotation_euler = (0, 0, -ang)
    hinge = bpy.data.objects.new('spinhinge', None)
    link(hinge)
    hinge.parent = piv
    m = principled('spinplate', col(v.get('color') or 0xD8DCE4), rough=0.15, metal=0.8)
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    me = bpy.data.meshes.new('plate')
    bm.to_mesh(me)
    bm.free()
    for vtx in me.vertices:
        vtx.co.x *= ln / 1000.0
        vtx.co.y *= 0.0015
        vtx.co.z = (vtx.co.z - 0.5) * 0.030
    o = bpy.data.objects.new('plate', me)
    o.data.materials.append(m)
    o.parent = hinge
    link(o)
    return hinge, [o]


# ---------------------------------------------------------------------------
# lights
# ---------------------------------------------------------------------------

def area(name, x, y, z, size, power, color=(1, 0.96, 0.9)):
    d = bpy.data.lights.new(name, 'AREA')
    d.size = size / 1000.0
    d.energy = power
    d.color = color
    o = bpy.data.objects.new(name, d)
    o.location = V(x, y, z)
    sc.collection.objects.link(o)
    return o


def point(name, x, y, z, power, color, radius=2.0):
    d = bpy.data.lights.new(name, 'POINT')
    d.energy = power
    d.color = color[:3]
    d.shadow_soft_size = radius / 1000.0
    o = bpy.data.objects.new(name, d)
    o.location = V(x, y, z)
    sc.collection.objects.link(o)
    return o


def build_lights():
    k = TH.get('light', 1.0)
    # the cabinet's lights over the glass: soft, from above and behind the
    # player, so their reflections on the playfield miss the camera
    for i, yy in enumerate((250, 650)):
        area('gi%d' % i, W / 2, yy, 900, 700, 7.5 * k, (1, 0.97, 0.92))
    area('key', W / 2, H + 420, 700, 500, 6.0 * k, (1, 0.95, 0.88))
    # coloured glow along the sides (the theme's GI)
    c = col(TH.get('gi', TH['accent']))
    for i, yy in enumerate((260, 520, 780)):
        point('gil%d' % i, 14, yy, 30, 0.25 * k, c, 6)
        point('gir%d' % i, W - 50, yy, 30, 0.25 * k, c, 6)


# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------

def read_exr(path):
    img = bpy.data.images.load(path, check_existing=False)
    w, h = img.size
    a = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(a)
    bpy.data.images.remove(img)
    return a.reshape(h, w, 4)[::-1].copy()


def srgb(x):
    x = np.clip(x, 0, 1)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def save_png(a, path, alpha=False):
    from PIL import Image  # noqa (not in Blender: only numpy is)


def write_png(a, path, alpha=False):
    """a float (h, w, 4) linear -> 8-bit sRGB PNG through Blender's own writer"""
    h, w = a.shape[:2]
    img = bpy.data.images.new('out', w, h, alpha=alpha, float_buffer=False)
    rgb = srgb(a[..., :3])
    out = np.concatenate([rgb, (a[..., 3:4] if alpha else np.ones((h, w, 1)))], axis=2)
    img.pixels.foreach_set(out[::-1].astype(np.float32).ravel())
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save()
    bpy.data.images.remove(img)


def border(x0, y0, x1, y1):
    x0, y0 = max(0, int(x0)), max(0, int(y0))
    x1, y1 = min(IMG_W, int(math.ceil(x1))), min(IMG_H, int(math.ceil(y1)))
    if x1 - x0 < 8 or y1 - y0 < 8:
        return None
    r.use_border = True
    r.use_crop_to_border = True
    r.border_min_x, r.border_max_x = x0 / IMG_W, x1 / IMG_W
    r.border_min_y, r.border_max_y = 1 - y1 / IMG_H, 1 - y0 / IMG_H
    return (x0, y0, x1, y1)


def no_border():
    r.use_border = False
    r.use_crop_to_border = False


def render(path, samples, denoise=True, transparent=False):
    sc.cycles.samples = samples
    sc.cycles.use_denoising = denoise
    if denoise:
        sc.cycles.denoiser = 'OPENIMAGEDENOISE'
    sc.cycles.pixel_filter_type = 'BLACKMAN_HARRIS'
    sc.cycles.filter_width = 1.5
    r.film_transparent = transparent
    r.filepath = path
    bpy.ops.render.render(write_still=True)
    return read_exr(path)


POS_MAT = None


def render_depth(path):
    """every surface's world position, one exact ray a pixel -> camera depth (mm)"""
    global POS_MAT
    if POS_MAT is None:
        POS_MAT = bpy.data.materials.new('pos')
        POS_MAT.use_nodes = True
        nt = POS_MAT.node_tree
        for n in list(nt.nodes):
            nt.nodes.remove(n)
        g = nt.nodes.new('ShaderNodeNewGeometry')
        e = nt.nodes.new('ShaderNodeEmission')
        o = nt.nodes.new('ShaderNodeOutputMaterial')
        nt.links.new(g.outputs['Position'], e.inputs['Color'])
        e.inputs['Strength'].default_value = 1.0
        nt.links.new(e.outputs['Emission'], o.inputs['Surface'])
    vl = sc.view_layers[0]
    vl.material_override = POS_MAT
    sc.cycles.samples = 1
    sc.cycles.use_denoising = False
    sc.cycles.pixel_filter_type = 'BOX'
    sc.cycles.filter_width = 0.01
    sc.cycles.max_bounces = 0
    r.film_transparent = True
    r.filepath = path
    bpy.ops.render.render(write_still=True)
    vl.material_override = None
    sc.cycles.max_bounces = 6
    a = read_exr(path)
    p = a[..., :3].astype(np.float64)
    d = ((p - CAM_W) * VIEW_D).sum(axis=2) * 1000.0
    d[a[..., 3] < 0.5] = 9000.0
    return d.astype(np.float32)


def project_bbox(objs, pad):
    xs, ys = [], []
    for o in objs:
        if o.type not in ('MESH', 'CURVE'):
            continue
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            v = world_to_camera_view(sc, cam, w)
            xs.append(v.x * IMG_W)
            ys.append((1 - v.y) * IMG_H)
    if not xs:
        return None
    return min(xs) - pad, min(ys) - pad, max(xs) + pad, max(ys) + pad


def vis(objs, camera=True, shadow=True, holdout=False):
    for o in objs:
        o.visible_camera = camera
        o.visible_shadow = shadow
        o.is_holdout = holdout


def hide(objs, h):
    """out of the render altogether: not even in the playfield's reflections"""
    for o in objs:
        o.hide_render = h


def lamp_lights(name, on):
    """a lamp's glow: its material, and for bumpers and flashers a light"""
    m = MATS.get('lamp_' + name)
    if m:
        set_emit(m, 6.0 if on else (0.12 if name.startswith('bumper') else 0.05))
    for o in LAMP_LIGHTS.get(name, []):
        o.hide_render = not on


LAMP_LIGHTS = {}


# ---------------------------------------------------------------------------
# the toys: built round an empty at their place, turned frame by frame
# ---------------------------------------------------------------------------

def parent_to(objs, piv):
    # the new objects' matrices are only right after an update
    bpy.context.view_layer.update()
    for o in objs:
        if o.parent is None or o.parent.type != 'EMPTY':
            mw = o.matrix_world.copy()
            o.parent = piv
            o.matrix_parent_inverse = piv.matrix_world.inverted()
            o.matrix_world = mw


def new_empty(name, x, y, z=0.0):
    e = bpy.data.objects.new(name, None)
    link(e)
    e.location = V(x, y, z)
    bpy.context.view_layer.update()
    return e


def collect(fn):
    """the objects a builder makes"""
    n0 = len(ALL)
    fn()
    return [o for o in ALL[n0:] if o.type != 'EMPTY']


def toy_zombie(t, x, y, s):
    skin = m_('zskin', 0x7A9A50, 0.6)
    rag = m_('zrag', 0x4A3A5A, 0.8)
    pants = m_('zpants', 0x2A3040, 0.8)
    eye = m_('zeye', 0xD0FF40, 0.3, emit_k=8)
    return collect(lambda: (
        box('zl1', x - 6 * s, y, 0, 7 * s, 8 * s, 26 * s, pants), box('zl2', x + 6 * s, y, 0, 7 * s, 8 * s, 26 * s, pants),
        box('zbody', x, y, 26 * s, 26 * s, 14 * s, 24 * s, rag, rot=4),
        box('zarm1', x - 12 * s, y + 10 * s, 40 * s, 6 * s, 22 * s, 6 * s, skin, tilt=(-8, 0)),
        box('zarm2', x + 12 * s, y + 12 * s, 38 * s, 6 * s, 24 * s, 6 * s, skin, tilt=(-14, 0)),
        sphere('zhead', x + 1 * s, y + 1 * s, 58 * s, 10 * s, skin, scale=(1, 0.95, 1.1)),
        sphere('ze1', x - 4 * s, y + 9 * s, 60 * s, 2.4 * s, eye), sphere('ze2', x + 5 * s, y + 9 * s, 60 * s, 2.4 * s, eye),
        box('zjaw', x, y + 7 * s, 50 * s, 10 * s, 3 * s, 3 * s, m_('zmouth', 0x200810, 0.9))))


def toy_robot(t, x, y, s):
    steel = m_('steel', 0xB8C4D4, 0.25, metal=0.9)
    blue = m_('rblue', 0x2050C0, 0.35, clear=0.5)
    glow = m_('rglow', 0x30E0FF, 0.3, emit_k=8)
    red = m_('rred2', 0xFF3050, 0.3, emit_k=10)
    legs = collect(lambda: (
        box('rl1', x - 15 * s, y - 4 * s, 0, 9 * s, 12 * s, 30 * s, steel), box('rl2', x + 15 * s, y - 4 * s, 0, 9 * s, 12 * s, 30 * s, steel),
        box('rhip', x, y - 10 * s, 26 * s, 40 * s, 10 * s, 8 * s, steel)))
    torso = collect(lambda: (
        box('rtorso', x, y - 6 * s, 34 * s, 44 * s, 22 * s, 30 * s, blue),
        box('rpanel', x, y + 5 * s, 40 * s, 24 * s, 2, 14 * s, glow),
        sphere('rsh1', x - 25 * s, y - 6 * s, 58 * s, 7 * s, steel), sphere('rsh2', x + 25 * s, y - 6 * s, 58 * s, 7 * s, steel),
        box('ra1', x - 28 * s, y, 34 * s, 7 * s, 8 * s, 24 * s, steel), box('ra2', x + 28 * s, y, 34 * s, 7 * s, 8 * s, 24 * s, steel),
        box('rhead', x, y - 6 * s, 64 * s, 26 * s, 20 * s, 18 * s, steel),
        box('rvisor', x, y + 4 * s, 70 * s, 20 * s, 2, 5 * s, red),
        cyl('rant', x, y - 6 * s, 1 * s, 82 * s, 92 * s, steel), sphere('rantb', x, y - 6 * s, 93 * s, 3 * s, glow)))
    return legs + torso, torso


def toy_ringmaster(t, x, y, s):
    red = m_('rmcoat', 0xD01828, 0.4, clear=0.4)
    black = m_('rmblack', 0x101010, 0.4, clear=0.5)
    white = m_('rmwhite', 0xF4F0E8, 0.4)
    skin = m_('rmskin', 0xF0C090, 0.5)
    gold = m_('rmgold', 0xFFC840, 0.25, metal=0.9)
    return collect(lambda: (
        box('rml1', x - 5 * s, y, 0, 6 * s, 7 * s, 24 * s, black), box('rml2', x + 5 * s, y, 0, 6 * s, 7 * s, 24 * s, black),
        box('rmcoat', x, y, 22 * s, 22 * s, 12 * s, 26 * s, red),
        box('rmshirt', x, y + 6 * s, 30 * s, 8 * s, 1.5, 14 * s, white),
        box('rmarm', x + 13 * s, y + 6 * s, 38 * s, 5 * s, 18 * s, 5 * s, red, tilt=(-30, 0)),
        sphere('rmglove', x + 13 * s, y + 16 * s, 44 * s, 3.5 * s, white),
        sphere('rmhead', x, y + 1 * s, 55 * s, 8.5 * s, skin),
        box('rmmus', x, y + 8 * s, 52 * s, 10 * s, 2, 2 * s, black),
        cyl('rmbrim', x, y, 12 * s, 61 * s, 63 * s, black), cyl('rmhat', x, y, 7.5 * s, 63 * s, 80 * s, black),
        cyl('rmband', x, y, 7.8 * s, 64 * s, 67 * s, gold)))


def toy_ship(t, x, y, s):
    wood = m_('gwood', 0x2E3A30, 0.6)
    sail = m_('gsail', 0xC8D8C0, 0.7)
    glow = m_('gglow', 0x60FFA0, 0.3, emit_k=8)
    hull = [(-30, -9), (24, -9), (34, 0), (24, 9), (-30, 9), (-34, 0)]
    def build():
        prism('ghull', [(x + px * s, y + py * s) for px, py in hull], 0, 16 * s, wood)
        box('gdeck', x - 22 * s, y, 16 * s, 14 * s, 16 * s, 8 * s, wood)
        for i, mx_ in enumerate((-8, 12)):
            cyl('gmast%d' % i, x + mx_ * s, y, 1.4 * s, 16 * s, 66 * s, wood)
            box('gsail%d' % i, x + mx_ * s, y + 2 * s, 28 * s, 24 * s, 1.2, 26 * s, sail)
        sphere('glamp1', x + 30 * s, y, 22 * s, 2.5 * s, glow)
        sphere('glamp2', x - 30 * s, y, 26 * s, 2.5 * s, glow)
        box('gflag', x + 16 * s, y, 62 * s, 10 * s, 1, 7 * s, m_('gflagm', 0x101010, 0.6))
    return collect(build)


def toy_boulder(t, x, y, s):
    stone = m_('boulder', 0x8A7A60, 0.85)
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=3, radius=21 * s / 1000.0)
    import random as _r
    rr = _r.Random(5)
    for v in bm.verts:
        v.co *= 1.0 + rr.uniform(-0.08, 0.08)
    me = bpy.data.meshes.new('boulder')
    bm.to_mesh(me)
    bm.free()
    o = bpy.data.objects.new('boulder', me)
    o.data.materials.append(stone)
    o.location = V(x, y, 21 * s)
    link(o)
    moss = sphere('bmoss', x - 8 * s, y - 6 * s, 30 * s, 9 * s, m_('moss', 0x4A7A30, 0.8), scale=(1, 1, 0.5))
    return [o, moss]


def toy_chestlid(t, x, y, s):
    wood = m_('pwood', 0x6A4020, 0.6, clear=0.3)
    gold = m_('pgold', 0xFFC040, 0.2, metal=0.9)
    def build():
        cone('clid', x, y, 16 * s - 17 * s, 11 * s, 11 * s, 34 * s, wood, rot=(0, math.pi / 2, 0))
        for dx in (-12, 12):
            box('cband%d' % dx, x + dx * s, y, 14 * s, 3 * s, 23 * s, 12 * s, gold)
    p = collect(build)
    return p, p


def toy_disc(t, x, y, s, kind):
    def build():
        if kind == 'roulette':
            red = m_('kred', 0xD01830, 0.3, clear=0.7)
            black = m_('kblack', 0x141414, 0.3, clear=0.7)
            gold = m_('kgold', 0xFFC830, 0.2, metal=0.9)
            wedge_disc('rw', x, y, 0.5, 3, 52 * s, [red, black], 36, r0=30 * s)
            wedge_disc('rin', x, y, 0.5, 3.5, 30 * s, [m_('wood2', 0x5A2A10, 0.4, clear=0.6)], 1)
            torus('rrim', x, y, 3, 52 * s, 2.2, gold)
            for i in range(4):
                a = i * math.pi / 2
                box('rspk%d' % i, x + math.cos(a) * 14 * s, y + math.sin(a) * 14 * s, 3.5, 26 * s, 3, 2, gold,
                    rot=math.degrees(a))
            cyl('rhub', x, y, 6 * s, 3.5, 8, gold)
        else:
            red = m_('cred', 0xE8243C, 0.3, clear=0.6)
            white = m_('cwhite', 0xF4F0E8, 0.3, clear=0.6)
            gold = m_('cgold', 0xFFC840, 0.25, metal=0.8)
            wedge_disc('cw', x, y, 0.5, 3, 46 * s, [red, white], 12)
            torus('crim', x, y, 3, 46 * s, 2.5, gold)
            for i in range(4):
                a = i * math.pi / 2 + 0.4
                hx, hy = x + math.cos(a) * 32 * s, y + math.sin(a) * 32 * s
                box('chorse%d' % i, hx, hy, 3, 14 * s, 5 * s, 3, m_('horse%d' % (i % 2), [0xFFD040, 0x60A0FF][i % 2], 0.3),
                    rot=math.degrees(a) + 90)
            cyl('chub', x, y, 7 * s, 3, 7, gold)
    return collect(build)


def toy_cannon(t, x, y, s):
    iron = m_('iron', 0x26262C, 0.3, metal=0.8)
    gold = m_('cgold', 0xFFC840, 0.25, metal=0.8)
    red = m_('cred', 0xE8243C, 0.3, clear=0.6)
    def build():
        cyl('cnbase', x, y, 16 * s, 0, 6 * s, red)
        # the barrel points along +x (the table's angle 0), up 25 degrees
        cone('cnbar', x + 10 * s, y, 4 * s, 8 * s, 6.5 * s, 34 * s, iron, rot=(0, math.radians(65), 0))
        torus('cnrim', x + 24 * s, y, 18 * s, 7 * s, 1.6, gold, rot=(0, math.radians(65), 0))
        for sy in (-1, 1):
            torus('cnw%d' % sy, x, y + sy * 11 * s, 8 * s, 7 * s, 1.8 * s, gold, rot=(math.pi / 2, 0, 0))
    return collect(build)


TOY_MODELS = {'zombie': toy_zombie, 'robot': toy_robot, 'ringmaster': toy_ringmaster, 'ship': toy_ship,
              'boulder': toy_boulder, 'chestlid': toy_chestlid, 'cannon': toy_cannon}


def build_toys():
    """every toy with frames: (toy, root, parts, set_frame)"""
    out = []
    for ty in S.get('toys', []):
        kind, model = ty['kind'], ty['model']
        x, y, sc = ty['x'], ty['y'], ty.get('s', 1.0)
        nf = S['toy_frames'].get(kind, 0)
        if not nf:
            continue
        piv = new_empty('toy_%s' % ty['name'], x, y)
        # a light of its own over every toy: they are the table's stars
        point('spot_%s' % ty['name'], x, y + 40, 220, 0.45, (1.0, 0.97, 0.9), 30)
        part = None
        if kind == 'disc':
            parts = toy_disc(ty, x, y, sc, model)
        elif model in TOY_MODELS:
            r = TOY_MODELS[model](ty, x, y, sc)
            parts, part = (r if isinstance(r, tuple) else (r, None))
        else:
            parts = collect(lambda: cyl('toy_' + ty['name'], x, y, ty['r'], 0, 40, m_('generic', TH['accent'], 0.4)))
        parent_to(parts, piv)
        sub = None
        if part:
            # the part that opens (the robot's torso, the chest's lid): an
            # empty at its hinge, on its back edge
            hy, hz = {'robot': (-17, 30), 'chestlid': (-11, 16)}.get(model, (-17, 30))
            sub = new_empty('toyp_%s' % ty['name'], x, y + hy * sc, hz * sc)
            sub.parent = piv
            sub.matrix_parent_inverse = piv.matrix_world.inverted()
            bpy.context.view_layer.update()
            for o in part:
                mw = o.matrix_world.copy()
                o.parent = sub
                o.matrix_parent_inverse = sub.matrix_world.inverted()
                o.matrix_world = mw
        base_loc = piv.location.copy()

        def set_frame(k, ty=ty, piv=piv, sub=sub, base_loc=base_loc, nf=nf, kind=kind, model=model):
            piv.location = base_loc.copy()
            piv.rotation_euler = (0, 0, 0)
            if sub:
                sub.rotation_euler = (0, 0, 0)
            if kind == 'bash':
                if k < 4:
                    piv.rotation_euler = (math.radians(-7 * k), 0, 0)
                else:
                    u = (k - 3) / (nf - 4 + 1)
                    if sub:
                        sub.rotation_euler = (math.radians((-100 if model == 'chestlid' else -75) * u), 0, 0)
                    else:
                        piv.location = base_loc + Vector((0, 0, -0.075 * u))
            elif kind == 'mover':
                u = k / (nf - 1)
                piv.location = base_loc + Vector(((ty['x1'] - ty['x']) * u / 1000.0, -(ty['y1'] - ty['y']) * u / 1000.0, 0))
                if model == 'boulder':
                    dist = math.hypot(ty['x1'] - ty['x'], ty['y1'] - ty['y']) * u
                    piv.rotation_euler = (0, dist / ty['r'], 0)
                elif model == 'ship':
                    piv.rotation_euler = (math.radians(4 * math.sin(u * 9)), 0, 0)
            elif kind == 'disc':
                rep = 2 * math.pi / max(1, ty['a1'])
                piv.rotation_euler = (0, 0, -rep * k / nf)
            elif kind == 'cannon':
                a = ty['a0'] + (ty['a1'] - ty['a0']) * k / (nf - 1)
                piv.rotation_euler = (0, 0, -math.radians(a))
            bpy.context.view_layer.update()
        set_frame(0)
        out.append((ty, piv, parts, set_frame))
    return out


def main():
    pf = build_playfield()
    build_cabinet()
    build_items()
    build_lights()
    meta = dict(lamps={}, drops={}, flippers={i: [] for i in range(len(S['flippers']))}, spinner=[], toys={})
    # lights that only lamps switch on
    for l in S['lamps']:
        if l['kind'] in ('bumper', 'flasher', 'sling'):
            c = col(l['color'])
            z = l.get('z', 40)
            pw = {'bumper': 1.2, 'flasher': 6.0, 'sling': 0.8}[l['kind']]
            o = point('ll_' + l['name'], l['x'], l['y'], z + (6 if l['kind'] == 'bumper' else 10), pw, c, 4)
            o.hide_render = True
            LAMP_LIGHTS[l['name']] = [o]
        elif l['kind'] == 'insert':
            o = point('ll_' + l['name'], l['x'], l['y'], 6, 0.12, col(l['color']), 3)
            o.hide_render = True
            LAMP_LIGHTS[l['name']] = [o]
    pivs = []
    for f in S['flippers']:
        pivs.append(flipper_obj(f))
    spin = [v for v in S['vis'] if v['t'] == 'spinner']
    hinge, plate = spinner_plate(spin[0]) if spin else (None, [])
    toys = build_toys()
    dyn = [o for p in pivs for o in p[1]] + plate + [o for tt in toys for o in tt[2]]
    statics = [o for o in ALL if o not in dyn and o.type != 'EMPTY']

    if args.blend:
        bpy.ops.wm.save_as_mainfile(filepath=args.blend)

    # 1. the table, lamps off, no flippers
    hide(dyn, True)
    if want('base'):
        no_border()
        a = render(os.path.join(TMP, 'base.exr'), args.samples)
        write_png(a, os.path.join(args.out, 'base.png'))
        d = render_depth(os.path.join(TMP, 'depth.exr'))
        np.save(os.path.join(args.out, 'depth.npy'), d)
    # 2. each lamp lit on its own
    if want('lamps'):
        for l in S['lamps']:
            name = l['name']
            objs = GROUP.get('lamp:' + name, [])
            if l['kind'] == 'flasher':
                x, y, _ = proj(l['x'], l['y'], l.get('z', 60))
                rad = 150
                bb = (x - rad, y - rad, x + rad, y + rad)
            else:
                bb = project_bbox(objs, 40 if l['kind'] == 'insert' else 70)
            if not bb or not border(*bb):
                continue
            bx = border(*bb)
            lamp_lights(name, True)
            a = render(os.path.join(TMP, 'lamp.exr'), max(32, args.samples // 2))
            lamp_lights(name, False)
            off = render(os.path.join(TMP, 'lampoff.exr'), max(32, args.samples // 2))
            # the difference is made here from the same crop, lit and not,
            # so the noise of the two renders cancels
            write_png(a, os.path.join(args.out, 'lamp_%s.png' % name))
            write_png(off, os.path.join(args.out, 'lampoff_%s.png' % name))
            meta['lamps'][name] = dict(file='lamp_%s.png' % name, off='lampoff_%s.png' % name, x=bx[0], y=bx[1])
        no_border()
    # 3. each drop target down
    if want('drops'):
        for v in S['vis']:
            if v['t'] != 'drop':
                continue
            name = v['obj']
            objs = GROUP.get('drop:' + name, [])
            bb = project_bbox(objs, 16)
            if not bb:
                continue
            bx = border(*bb)
            for o in objs:
                o.location.z -= 0.040
            a = render(os.path.join(TMP, 'drop.exr'), max(32, args.samples // 2))
            write_png(a, os.path.join(args.out, 'drop_%s.png' % name))
            d = render_depth(os.path.join(TMP, 'dropz.exr'))
            np.save(os.path.join(args.out, 'drop_%s.npy' % name), d)
            for o in objs:
                o.location.z += 0.040
            meta['drops'][name] = dict(file='drop_%s.png' % name, depth='drop_%s.npy' % name, x=bx[0], y=bx[1])
        no_border()
    # 4. the flippers' frames: the flipper and its shadow alone
    NF = 12
    if want('flippers'):
        vis(statics, camera=False, shadow=False, holdout=False)
        pf.visible_camera = True
        pf.is_shadow_catcher = True
        for s, (piv, parts) in enumerate(pivs):
            f = S['flippers'][s]
            hide(dyn, True)
            hide(parts, False)
            # every frame in the same crop: the union of the swept area
            boxes = []
            for k in range(NF):
                set_flipper(piv, f, k, NF)
                bpy.context.view_layer.update()
                boxes.append(project_bbox(parts, 14))
            bb = (min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes),
                  max(b[3] for b in boxes))
            bx = border(*bb)
            # what stands in front of a flipper hides it: those are held out
            for o in statics:
                if o is not pf and o.name.startswith(('wall', 'post', 'sling', 'apron', 'rubber')):
                    o.visible_camera = True
                    o.is_holdout = True
            for k in range(NF):
                set_flipper(piv, f, k, NF)
                a = render(os.path.join(TMP, 'flip.exr'), max(32, args.samples // 2), transparent=True)
                fn = 'flip%d_%02d.png' % (s, k)
                write_png(a, os.path.join(args.out, fn), alpha=True)
                meta['flippers'][s].append(dict(file=fn, x=bx[0], y=bx[1]))
            for o in statics:
                o.is_holdout = False
                o.visible_camera = False
            set_flipper(piv, f, 0, NF)
        pf.is_shadow_catcher = False
        vis(statics, camera=True, shadow=True)
        hide(dyn, True)
        no_border()
    # 5. the spinner's plate
    if want('spinner') and hinge:
        NS = 8
        vis(statics, camera=False, shadow=False)
        pf.visible_camera = True
        pf.is_shadow_catcher = True
        hide(plate, False)
        boxes = []
        for k in range(NS):
            hinge.rotation_euler = (k / NS * 2 * math.pi, 0, 0)
            bpy.context.view_layer.update()
            boxes.append(project_bbox(plate, 10))
        bb = (min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes), max(b[3] for b in boxes))
        bx = border(*bb)
        for k in range(NS):
            hinge.rotation_euler = (k / NS * 2 * math.pi, 0, 0)
            a = render(os.path.join(TMP, 'spin.exr'), max(32, args.samples // 2), transparent=True)
            fn = 'spin_%02d.png' % k
            write_png(a, os.path.join(args.out, fn), alpha=True)
            meta['spinner'].append(dict(file=fn, x=bx[0], y=bx[1]))
        pf.is_shadow_catcher = False
        vis(statics, camera=True, shadow=True)
        hide(plate, True)
        no_border()
    # 6. the toys' frames: each toy and its shadow, what stands in front held
    # out, and the depth of its pixels
    if want('toys'):
        for ti, (ty, piv, parts, set_frame) in enumerate(toys):
            nf = S['toy_frames'][ty['kind']]
            hide(dyn, True)
            hide(parts, False)
            boxes = []
            for k in range(nf):
                set_frame(k)
                boxes.append(project_bbox(parts, 16))
            boxes = [b for b in boxes if b]
            print('toy', ty['name'], 'boxes', [tuple(int(v) for v in b) for b in boxes[:3]], [o.name for o in parts][:6])
            if not boxes:
                continue
            bb = (min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes), max(b[3] for b in boxes))
            bx = border(*bb)
            meta['toys'][ti] = []
            for k in range(nf):
                set_frame(k)
                for o in statics:
                    o.visible_camera = o is pf or not o.name.startswith('under')
                    o.visible_shadow = False
                    o.is_holdout = o is not pf
                pf.is_shadow_catcher = True
                a = render(os.path.join(TMP, 'toy.exr'), max(32, args.samples // 2), transparent=True)
                fn = 'toy%d_%02d.png' % (ti, k)
                write_png(a, os.path.join(args.out, fn), alpha=True)
                # the depth: the toy alone
                for o in statics:
                    o.is_holdout = False
                    o.visible_camera = False
                pf.is_shadow_catcher = False
                d = render_depth(os.path.join(TMP, 'toyz.exr'))
                np.save(os.path.join(args.out, 'toy%d_%02d.npy' % (ti, k)), d.astype(np.float32))
                meta['toys'][ti].append(dict(file=fn, depth='toy%d_%02d.npy' % (ti, k), x=bx[0], y=bx[1]))
            for o in statics:
                o.is_holdout = False
            vis(statics, camera=True, shadow=True)
            set_frame(0)
        hide(dyn, True)
        no_border()
    # 7. the ball, and the buttons
    if want('ball'):
        render_ball()
    if want('icons'):
        render_icons()
    old = {}
    p = os.path.join(args.out, 'meta.json')
    if os.path.exists(p) and ONLY:
        old = json.load(open(p))
        for k, v in meta.items():
            full = any(v.values()) if isinstance(v, dict) and k in ('flippers', 'toys') else bool(v)
            if full:
                old[k] = v
        meta = old
    json.dump(meta, open(p, 'w'), indent=1)
    print('done:', args.out)


def solo_camera(target, size, look=(0.0, -0.5, 1.0)):
    """an orthographic camera looking at 'target' (world) from 'look'"""
    cd = bpy.data.cameras.new('solo')
    cd.type = 'ORTHO'
    cd.ortho_scale = size
    co = bpy.data.objects.new('solo', cd)
    sc.collection.objects.link(co)
    d = Vector(look).normalized()
    co.location = target + d * 1.0
    co.rotation_euler = (-d).to_track_quat('-Z', 'Y').to_euler()
    return co


def studio_world():
    """a photographer's backdrop: light above, a bright band, dark below;
    what a chrome ball and the buttons reflect"""
    w = bpy.data.worlds.new('studio')
    w.use_nodes = True
    nt = w.node_tree
    bg = nt.nodes['Background']
    tc = nt.nodes.new('ShaderNodeTexCoord')
    sep = nt.nodes.new('ShaderNodeSeparateXYZ')
    ramp = nt.nodes.new('ShaderNodeValToRGB')
    nt.links.new(tc.outputs['Generated'], sep.inputs['Vector'])
    nt.links.new(sep.outputs['Z'], ramp.inputs['Fac'])
    cr = ramp.color_ramp
    cr.elements[0].position = 0.2
    cr.elements[0].color = (0.12, 0.12, 0.13, 1)
    cr.elements[1].position = 0.85
    cr.elements[1].color = (1.0, 1.0, 1.0, 1)
    e = cr.elements.new(0.55)
    e.color = (0.55, 0.57, 0.6, 1)
    nt.links.new(ramp.outputs['Color'], bg.inputs['Color'])
    bg.inputs['Strength'].default_value = 1.0
    return w


def render_ball():
    br = S['ball_r']
    ball = sphere('ball', W / 2, H * 0.62, br, br, M_CHROME, segs=64)
    hidden = [o for o in ALL if o is not ball]
    # alone in the studio: at 26 pixels a ball reads as a gradient and a highlight
    lights = [o for o in sc.objects if o.type == 'LIGHT']
    for o in hidden + lights:
        o.hide_render = True
    hl = area('ball_hl', W / 2 - 60, H * 0.62 + 40, 160, 40, 4.0)
    view = Vector((0, -math.cos(CAM['pitch']), math.sin(CAM['pitch'])))
    co = solo_camera(ball.location, br * 2.04 / 1000.0, look=view)
    sc.camera = co
    old_world = sc.world
    sc.world = studio_world()
    r.resolution_x = r.resolution_y = 128
    no_border()
    # no denoiser: on this little picture it sometimes gave back a black ball
    for attempt in range(3):
        a = render(os.path.join(TMP, 'ball.exr'), 384, denoise=False, transparent=True)
        if float(np.nanmean(a[48:80, 48:80, :3])) > 0.1:
            break
        print('the ball came out dark, again')
    write_png(np.nan_to_num(a), os.path.join(args.out, 'ball.png'), alpha=True)
    r.resolution_x, r.resolution_y = IMG_W, IMG_H
    sc.camera = cam
    sc.world = old_world
    for o in hidden + lights:
        o.hide_render = False
    hl.hide_render = True
    ball.hide_render = True


# ---- the buttons: the theme's own objects, alone ----------------------------------

def icon_launch_zombis(g, pressed):
    """a skull on a gravestone slab, its eyes glowing green (brighter when pressed)"""
    bone = principled('bone%d' % pressed, col(0xB8B098), rough=0.5)
    eye = principled('eyes%d' % pressed, col(0x7CFF3A), rough=0.4, emit=col(0x7CFF3A), emit_k=25 if pressed else 6)
    dark = m_('socket', 0x0A0A08, 0.9)
    stone = m_('stone2', 0x3A3E38, 0.8)
    dz = -3 if pressed else 0
    g.append(cyl('ic_base', 0, 0, 44, -16, -4, stone, n=48))
    g.append(sphere('ic_skull', 0, -2, 26 + dz, 28, bone, scale=(1, 0.95, 0.92)))
    g.append(box('ic_jaw', 0, 12, -4 + dz, 30, 26, 18, bone))
    for sx in (-1, 1):
        g.append(sphere('ic_sock%d' % sx, sx * 10.5, 22, 26 + dz, 8.5, dark, scale=(1, 0.5, 1)))
        g.append(sphere('ic_eye%d' % sx, sx * 10.5, 24, 26 + dz, 6.5, eye, scale=(1, 0.5, 1)))
    g.append(sphere('ic_nose', 0, 25, 14 + dz, 3.5, dark, scale=(1, 0.5, 1.4)))
    for i in range(5):
        g.append(box('ic_tooth%d' % i, -10 + i * 5, 25, 0 + dz, 3.4, 2, 7, dark))


def icon_pause_zombis(g):
    stone = m_('stone2', 0x3A3E38, 0.8)
    g.append(cyl('ip_base', 0, 0, 44, -16, -4, stone, n=6))
    for sx in (-1, 1):
        g.append(box('ip_bar%d' % sx, sx * 12, 0, -4, 12, 40, 6, m_('bone', 0xE8E0C8, 0.45)))


ICONS = {'zombis': (icon_launch_zombis, icon_pause_zombis)}


def bars(g, mat, z=-4):
    for sx in (-1, 1):
        g.append(box('ip_bar%d' % sx, sx * 12, 0, z, 12, 40, 6, mat))


def icon_launch_robots(g, pressed):
    white = principled('lrw', col(0xF0F2F6), rough=0.3, clear=0.6)
    red = principled('lrr', col(0xE02830), rough=0.3, clear=0.6)
    flame = principled('lrf%d' % pressed, col(0xFF8A20), rough=0.5, emit=col(0xFF9A30), emit_k=30 if pressed else 6)
    g.append(cyl('ic_pad', 0, 0, 44, -16, -6, principled('lrpad', col(0x283048), rough=0.3, metal=0.6), n=6))
    o = cyl('ic_body', 0, 0, 11, 6, 44, white)
    g.append(o)
    g.append(cone('ic_nose', 0, 0, 44, 11, 0.5, 18, red))
    g.append(sphere('ic_win', 0, 10, 30, 5, principled('lrwin', col(0x30E0FF), emit=col(0x30E0FF), emit_k=4),
                    scale=(1, 0.4, 1)))
    for i in range(3):
        a = i / 3 * 2 * math.pi + math.pi / 2
        g.append(box('ic_fin%d' % i, math.cos(a) * 12, math.sin(a) * 12, 6, 10, 2.5, 16, red, rot=math.degrees(a)))
    g.append(cone('ic_flame', 0, 0, -6, 9, 3, 12 if pressed else 7, flame))


def icon_pause_robots(g):
    steel = principled('gear', col(0x8C98AC), rough=0.25, metal=0.9)
    g.append(cyl('ip_gear', 0, 0, 36, -16, -6, steel, n=40))
    for i in range(10):
        a = i / 10 * 2 * math.pi
        g.append(box('ip_t%d' % i, math.cos(a) * 40, math.sin(a) * 40, -16, 10, 9, 10, steel, rot=math.degrees(a)))
    bars(g, principled('pbr', col(0x30E0FF), emit=col(0x30E0FF), emit_k=3), z=-6)


def icon_launch_jungla(g, pressed):
    stone = principled('ljs', col(0x8A8470), rough=0.85)
    moss = principled('ljm', col(0x4A7A30), rough=0.8)
    eye = principled('lje%d' % pressed, col(0xFFA020), emit=col(0xFFA020), emit_k=25 if pressed else 5)
    dark = principled('ljd', col(0x0A0806), rough=0.9)
    g.append(cyl('ic_base', 0, 0, 44, -16, -6, moss, n=12))
    g.append(box('ic_head', 0, 0, -6, 46, 34, 52, stone))
    g.append(box('ic_brow', 0, 17, 22, 50, 6, 6, moss))
    for sx in (-1, 1):
        g.append(box('ic_sock%d' % sx, sx * 11, 17, 8, 13, 4, 11, dark))
        g.append(sphere('ic_eye%d' % sx, sx * 11, 19, 13, 4.5, eye, scale=(1, 0.4, 1)))
    g.append(box('ic_nose', 0, 19, -2, 8, 6, 14, stone))
    g.append(box('ic_mouth', 0, 17, -10, 30, 4, 7, dark))
    for i in range(5):
        g.append(box('ic_t%d' % i, -12 + i * 6, 19, -10, 3, 2, 7, stone))


def icon_pause_jungla(g):
    g.append(cyl('ip_stone', 0, 0, 44, -16, -6, principled('pjs', col(0x8A8470), rough=0.85), n=9))
    bars(g, principled('pjb', col(0xFFA020), emit=col(0xFFA020), emit_k=2), z=-6)


def icon_launch_circo(g, pressed):
    iron = principled('lci', col(0x26262C), rough=0.3, metal=0.8)
    gold = principled('lcg', col(0xFFC840), rough=0.25, metal=0.8)
    red = principled('lcr', col(0xE8243C), rough=0.3, clear=0.6)
    g.append(cyl('ic_base', 0, 0, 44, -16, -6, red, n=48))
    g.append(cone('ic_barrel', 0, 4, -10, 13, 11, 56, iron, rot=(math.radians(-50), 0, 0)))
    g.append(torus('ic_rim', 0, -17, 36, 12, 3, gold, rot=(math.radians(-50), 0, 0)))
    for sx in (-1, 1):
        g.append(torus('ic_w%d' % sx, sx * 18, 10, 6, 13, 3, gold, rot=(0, math.pi / 2, 0)))
    if pressed:
        g.append(sphere('ic_boom', 0, -24, 44, 14, principled('lcb', col(0xFFD060), emit=col(0xFFB030), emit_k=20)))


def icon_pause_circo(g):
    red = principled('pcr', col(0xE8243C), rough=0.3, clear=0.6)
    white = principled('pcw', col(0xF4F0E8), rough=0.3, clear=0.6)
    for i in range(12):
        a0, a1 = i / 12 * 2 * math.pi, (i + 1) / 12 * 2 * math.pi
        pts = [(0, 0)] + [(44 * math.cos(a0 + (a1 - a0) * k / 3), 44 * math.sin(a0 + (a1 - a0) * k / 3))
                          for k in range(4)]
        g.append(prism('ip_w%d' % i, pts, -16, -6, red if i % 2 else white))
    bars(g, principled('pcg', col(0xFFC840), rough=0.25, metal=0.8), z=-6)


def icon_launch_casino(g, pressed):
    red = principled('lkr', col(0xD01830), rough=0.3, clear=0.7)
    black = principled('lkb', col(0x141414), rough=0.3, clear=0.7)
    green = principled('lkg', col(0x109040), rough=0.3, clear=0.7)
    gold = principled('lkgo', col(0xFFC830), rough=0.2, metal=0.9)
    g.append(cyl('ic_rim', 0, 0, 46, -16, -2, principled('lkw', col(0x5A2A10), rough=0.4, clear=0.6), n=64))
    for i in range(37):
        a0, a1 = i / 37 * 2 * math.pi, (i + 1) / 37 * 2 * math.pi
        pts = [(20 * math.cos(a0), 20 * math.sin(a0))] + [(40 * math.cos(a0 + (a1 - a0) * k / 2),
                                                           40 * math.sin(a0 + (a1 - a0) * k / 2)) for k in range(3)] + \
              [(20 * math.cos(a1), 20 * math.sin(a1))]
        g.append(prism('ic_s%d' % i, pts, -2, 0, green if i == 36 else (red if i % 2 else black)))
    g.append(cyl('ic_hub', 0, 0, 20, -2, 3, gold, n=48))
    g.append(cyl('ic_spin', 0, 0, 4, 3, 16, gold))
    for i in range(4):
        a = i / 4 * 2 * math.pi + (0.5 if pressed else 0)
        g.append(box('ic_arm%d' % i, math.cos(a) * 9, math.sin(a) * 9, 12, 18, 3, 3, gold, rot=math.degrees(a)))
    g.append(sphere('ic_ball', 30 if not pressed else -14, 0 if not pressed else 26, 3, 3.5,
                    principled('lkball', col(0xF8F8F8), rough=0.1, clear=1.0, emit=col(0xFFFFFF),
                               emit_k=4 if pressed else 0)))


def icon_pause_casino(g):
    red = principled('pkr', col(0xD01830), rough=0.3, clear=0.7)
    white = principled('pkw', col(0xF4F0E8), rough=0.3, clear=0.7)
    g.append(cyl('ip_chip', 0, 0, 44, -16, -6, red, n=64))
    for i in range(8):
        a = i / 8 * 2 * math.pi
        g.append(box('ip_e%d' % i, math.cos(a) * 40, math.sin(a) * 40, -16, 12, 7, 10.5, white, rot=math.degrees(a) + 90))
    bars(g, white, z=-6)


def icon_launch_piratas(g, pressed):
    wood = principled('lpw', col(0x7A4A24), rough=0.5, clear=0.4)
    gold = principled('lpg%d' % pressed, col(0xFFC040), rough=0.2, metal=0.9, emit=col(0xFFC040),
                      emit_k=3 if pressed else 0)
    rot = 0.4 if pressed else 0.0
    g.append(torus('ic_ring', 0, 0, 0, 30, 4.5, wood))
    g.append(cyl('ic_hub', 0, 0, 9, -5, 5, gold))
    for i in range(8):
        a = i / 8 * 2 * math.pi + rot
        g.append(box('ic_sp%d' % i, math.cos(a) * 20, math.sin(a) * 20, -2.5, 40, 5, 5, wood, rot=math.degrees(a)))
        g.append(cyl('ic_h%d' % i, math.cos(a) * 44, math.sin(a) * 44, 3.5, -3, 3, wood))
        g.append(sphere('ic_k%d' % i, math.cos(a) * 48, math.sin(a) * 48, 0, 4.5, wood))


def icon_pause_piratas(g):
    gold = principled('ppg', col(0xFFC040), rough=0.2, metal=0.9)
    g.append(cyl('ip_coin', 0, 0, 44, -16, -6, gold, n=64))
    g.append(torus('ip_rim', 0, 0, -6, 40, 2, gold))
    bars(g, principled('ppd', col(0x5A3A10), rough=0.4), z=-6)


ICONS.update({'robots': (icon_launch_robots, icon_pause_robots), 'jungla': (icon_launch_jungla, icon_pause_jungla),
              'circo': (icon_launch_circo, icon_pause_circo), 'casino': (icon_launch_casino, icon_pause_casino),
              'piratas': (icon_launch_piratas, icon_pause_piratas)})


def icon_launch_generic(g, pressed):
    m = m_('btn', TH['accent'], 0.3, emit_k=2.0 if pressed else 0.3)
    g.append(cyl('ic_base', 0, 0, 44, -16, -4, M_CHROME, n=48))
    g.append(cyl('ic_top', 0, 0, 38, -4, 6, m, n=48))


def icon_pause_generic(g):
    g.append(cyl('ip_base', 0, 0, 44, -16, -4, m_('btn', TH['accent'], 0.3), n=48))
    for sx in (-1, 1):
        g.append(box('ip_bar%d' % sx, sx * 12, 0, -4, 12, 40, 6, M_WHITE))


def render_icons():
    """each button built at the world's origin and seen from above and a
    little in front, alone, on a transparent film"""
    launch, pause = ICONS.get(TH.get('art', S['id']), (icon_launch_generic, icon_pause_generic))
    for o in ALL:
        o.visible_camera = False
        o.visible_shadow = False
    OFF[0], OFF[1] = W / 2, -1500.0
    key = area('icon_key', 60, 260, 260, 300, 4.0)
    fill = area('icon_fill', -220, 120, 120, 300, 1.8, (0.8, 0.85, 1.0))
    old_world = sc.world
    sc.world = studio_world()
    r.resolution_x = r.resolution_y = 256
    no_border()
    for name, fn in (('launch', lambda g: launch(g, False)), ('launch_on', lambda g: launch(g, True)),
                     ('pause', pause)):
        g = []
        fn(g)
        target = V(0, 0, 0)
        co = solo_camera(target, 0.108, look=(0, -0.9, 0.75))
        sc.camera = co
        a = render(os.path.join(TMP, 'icon.exr'), max(64, args.samples), transparent=True)
        write_png(a, os.path.join(args.out, name + '.png'), alpha=True)
        for o in g:
            o.hide_render = True
    import shutil
    shutil.copy(os.path.join(args.out, 'launch.png'), os.path.join(args.out, 'icon.png'))
    key.hide_render = True
    fill.hide_render = True
    sc.world = old_world
    OFF[0] = OFF[1] = 0.0
    r.resolution_x, r.resolution_y = IMG_W, IMG_H
    sc.camera = cam
    for o in ALL:
        o.visible_camera = True
        o.visible_shadow = True


main()
