# GHL Glance glow back: a replacement for the Core2's stock battery bottom.
# Print in natural / translucent PETG. A WS2812 Jewel x7 sits inside, LEDs down,
# on a 0.8 mm window, and lights the whole back. Nothing sticks out.
#
# Built from M5Stack's own Core2 model (M5_Hardware/Products/K010_Core2/Structures/Core2.stl,
# stock bottom extracted to core2_stock_bottom.stl), so the outline, the four M3 countersunk
# screw holes, the frame-locating rim, the PCB pegs and the M-Bus pocket are unchanged.
# Changes: battery bay emptied for the Jewel, floor holes in the bay closed,
# a locating ring for the Jewel, a wire hole from the bay into the M-Bus pocket,
# a 4.4 mm power-cable entry with a strain-relief collar, and a 3.4 mm hole for the
# battery leads (the battery moves into the stand base).
#   python3 glance_back.py  →  glance_back.stl
import numpy as np, trimesh
from manifold3d import Manifold, Mesh

def load(path):
    m = trimesh.load(path)
    return Manifold(Mesh(vert_properties=np.asarray(m.vertices, np.float32), tri_verts=np.asarray(m.faces, np.uint32)))

def cyl(r, z0, z1, x=0, y=0, n=96):
    return Manifold.cylinder(z1 - z0, r, r, n).translate([x, y, z0])

def box(x0, x1, y0, y1, z0, z1):
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])

stock = load("core2_stock_bottom.stl")

JX, JY = -6.0, -1.5          # Jewel centre, middle of the battery bay
JEWEL_D = 23.0
SEAT_R = JEWEL_D / 2 + 0.4
WIN_TOP = 0.99               # window is 0.8 mm (outer face of the floor is at z 0.19)
RING_TOP = 4.0

# 1. empty the battery bay: everything above the floor inside x -23..8, y -19..16, except the PCB pegs
bay = box(-23.0, 8.0, -19.0, 16.0, 1.19, 8.0)
pegs = box(-12, -8, 13, 18, 0, 8) + box(-12, -8, -18, -13, 0, 8) + box(7, 11, 4, 8, 0, 8) + box(7, 11, -8, -4, 0, 8)
back = stock - (bay - pegs)

# 2. close the floor openings in the bay (centre boss, keyhole and the round hole near the front edge)
back = back + box(-21.5, 7.5, -18.5, 15.5, 0.19, 1.19)
back = back + box(-4.0, 4.0, 13.0, 24.0, 0.19, 1.19)      # keyhole
back = back + cyl(4.0, 0.19, 1.19, 0, -21.2)             # round hole near the front edge

# 3. Jewel seat: thin window, locating ring with a gap toward the M-Bus side for the wires
back = back - cyl(SEAT_R, WIN_TOP, 8.0, JX, JY)
back = back + cyl(SEAT_R - 0.01, 0.19, WIN_TOP, JX, JY)
ring = cyl(SEAT_R + 1.4, WIN_TOP, RING_TOP, JX, JY) - cyl(SEAT_R, 0, 9, JX, JY) - box(JX + 8, JX + 20, JY - 3, JY + 3, 0, 9)
back = back + ring

# 4. wire hole from the bay into the M-Bus pocket (pins 1 GND, 8 G25, 28 5V)
back = back - Manifold.cylinder(8.0, 2.0, 2.0, 48).rotate([0, 90, 0]).translate([5.5, 0, 2.9])

# 5. power entry: a 2-core 5 V cable (cut USB cable, up to 4 mm OD) comes in through the floor
#    in a free corner of the bay, clear of the Jewel ring, the pegs and the screw posts.
#    A collar inside gives hot glue / a zip tie something to grip for strain relief.
PX, PY, CABLE_D = -18.0, 13.5, 4.4
collar = cyl(CABLE_D / 2 + 1.6, 1.19, 3.6, PX, PY)
back = back + collar
back = back - cyl(CABLE_D / 2, -1, 9, PX, PY)
back = back - Manifold.cylinder(0.6, CABLE_D / 2 + 0.6, CABLE_D / 2, 48).translate([PX, PY, 0.0])   # chamfer, outside edge

# 6. battery leads: the Core2's battery now lives in the stand base. Its two leads come back in here
#    and plug into the battery connector on the Core2 board, so they get their own small hole + collar.
BX, BY, LEAD_D = -21.5, 8.5, 3.4
back = back + cyl(LEAD_D / 2 + 1.4, 1.19, 3.6, BX, BY)
back = back - cyl(LEAD_D / 2, -1, 9, BX, BY)
back = back - Manifold.cylinder(0.6, LEAD_D / 2 + 0.6, LEAD_D / 2, 48).translate([BX, BY, 0.0])

m = back.to_mesh()
out = trimesh.Trimesh(np.asarray(m.vert_properties)[:, :3], np.asarray(m.tri_verts))
out.export("glance_back.stl")
print("watertight", out.is_watertight, "size", np.round(out.extents, 2), "vol cm3 %.2f" % (out.volume / 1000))
