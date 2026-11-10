# GHL Glance desk stand for M5Stack Core2 (54 x 54 x 16.5 mm) fitted with the
# translucent glow back (glance_back.stl). The back plate is an open frame so the
# glow reaches the wall behind. Sides are open so the power button and USB-C
# stay reachable. The Core2's battery lives in a pocket under the base.
# Print upright, no supports, PETG.
#   python3 glance_stand.py  →  glance_stand.stl
import math, numpy as np, trimesh
from manifold3d import Manifold

TILT = 15            # degrees back from vertical
CORE_W, CORE_D, CORE_H = 54.0, 16.5, 54.0
CLR = 0.4            # clearance per side
WALL = 3.0
LIP_H = 4.0
PLATE_H = 46.0       # back plate height above the shelf (Core2 sticks out 8 mm above it, easy to lift)

def box(x0, x1, y0, y1, z0, z1):
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])

def cyl_y(r, y0, y1, x, z, n=64):   # cylinder along +y
    return Manifold.cylinder(y1 - y0, r, r, n).rotate([-90, 0, 0]).translate([x, y0, z])

w2 = CORE_W / 2 + CLR + WALL
front = -CLR - 2.4          # lip in front of the glass
back_in = CORE_D + CLR      # inner face of back plate
back_out = back_in + WALL

# --- upright cradle (Core2 front face at y = 0, sitting on z = 0) ---
shelf = box(-w2, w2, front, back_out, -WALL, 0)
lip = box(-w2, w2, front, -CLR, 0, LIP_H)
plate = box(-w2, w2, back_in, back_out, -WALL, PLATE_H)

# open back: the Core2's glow back shines through this window onto the wall
cradle = shelf + lip + plate
window = box(-w2 + 5.0, w2 - 5.0, back_in - 1, back_out + 1, 3.0, PLATE_H - 3.0)   # big enough for the power cable entry in either orientation
cradle = cradle - window

# --- tilt back and set on a base ---
# The base is thick enough to hold the Core2's own battery (YS 602535: 6 x 25 x 35 mm plus its
# protection board) in a pocket underneath, closed by a separate cover (glance_battery_cover.stl)
# held with two M3 screws. The battery leads run up through a hole into the cable groove.
BAT_W, BAT_L, BAT_T = 26.5, 41.0, 7.0     # pocket: battery 25 x ~39 x 6 with clearance
COVER_T = 1.6                             # cover sits flush in a recess in the bottom face
ROOF = 2.0                                # plastic between pocket and groove
BASE_T = COVER_T + BAT_T + ROOF + 3.0     # 13.6 mm: the 3 mm cable groove sits above the roof

cradle = cradle.rotate([-TILT, 0, 0])           # top leans back, toward +y
lo = cradle.bounding_box()[2]
cradle = cradle.translate([0, 0, -lo + BASE_T])

# fill between the tilted shelf and the base
shelf_t = shelf.rotate([-TILT, 0, 0]).translate([0, 0, -lo + BASE_T])
bb = shelf_t.bounding_box()
foot = box(-w2, w2, bb[1], bb[4], 0, BASE_T)
support = (shelf_t + foot).hull()

by0, by1 = bb[1] - 6, bb[4] + 26   # deeper than before to fit the battery pocket
base = box(-w2 - 4, w2 + 4, by0, by1, 0, BASE_T)

# power cable and battery leads: drop behind the back plate, run in a groove across the top of the base,
# and the power cable leaves at the rear edge
groove = box(-3.0, 3.0, bb[4] + 1.0, by1 + 1, BASE_T - 3.0, BASE_T + 1.0)

# battery pocket from underneath, rear end under the groove
py1 = by1 - 6.0
py0 = py1 - BAT_L
pocket = box(-BAT_W / 2, BAT_W / 2, py0, py1, COVER_T - 0.01, COVER_T + BAT_T)
# cover recess (2 mm lip all round) and two M3 pilot holes in the lip ends
CW, CL = BAT_W + 8.0, BAT_L + 9.0
cy0 = py0 - 4.5
recess = box(-CW / 2, CW / 2, cy0, cy0 + CL, -1, COVER_T)
SCREW_Y = [cy0 + 2.3, cy0 + CL - 2.3]
pilots = Manifold.cylinder(8.0, 1.25, 1.25, 32).translate([0, SCREW_Y[0], -1]) + \
         Manifold.cylinder(8.0, 1.25, 1.25, 32).translate([0, SCREW_Y[1], -1])
lead_hole = Manifold.cylinder(BASE_T, 1.8, 1.8, 32).translate([0, py1 - 4.0, COVER_T])   # pocket → groove

stand = cradle + support + base - groove - pocket - recess - pilots - lead_hole

# separate cover, printed flat
cover = box(-CW / 2 + 0.2, CW / 2 - 0.2, 0.2, CL - 0.2, 0, COVER_T - 0.1)
for yy in SCREW_Y:
    cover = cover - Manifold.cylinder(4, 1.7, 1.7, 32).translate([0, yy - cy0, -1])                      # M3 clearance
    cover = cover - Manifold.cylinder(1.0, 1.7, 3.2, 32).translate([0, yy - cy0, COVER_T - 1.1])         # countersink
cm = cover.to_mesh()
trimesh.Trimesh(np.asarray(cm.vert_properties)[:, :3], np.asarray(cm.tri_verts)).export("glance_battery_cover.stl")
print("base depth %.1f mm, pocket y %.1f..%.1f inside %.1f..%.1f" % (by1 - by0, py0, py1, by0, by1))

mesh = trimesh.Trimesh(*[np.asarray(a) for a in (stand.to_mesh().vert_properties[:, :3], stand.to_mesh().tri_verts)])
mesh.export("glance_stand.stl")
b = mesh.bounds
print("watertight", mesh.is_watertight, "volume cm3 %.1f" % (mesh.volume / 1000))
print("size mm  W %.1f  D %.1f  H %.1f" % tuple(b[1] - b[0]))
