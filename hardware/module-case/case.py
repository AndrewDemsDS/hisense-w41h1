#!/usr/bin/env python3
"""Two-part case for the replacement module PCB (hardware/module-pcb).

The outer length and width are estimates of the stock module's housing, scaled off the
internal photo in the FCC filing (FCC ID 2AGCCAEH-W41H1) with the 40 mm board as the ruler.
The filing has no side view, so the thickness is not the stock one: it is what this board
needs. Change the numbers below and run the script again.

Frame: the board's STEP export. X runs from the connector end to the antenna end, the
board's bottom face is Z = 0 and its parts point up.

Usage: uv run --python 3.12 --with cadquery python case.py [board.step]
Writes base and lid as STEP and STL into out/. With a board STEP it also reports how much
each part overlaps the board, which has to be zero.
"""

import sys
from pathlib import Path

import cadquery as cq

# Board (keep in step with ../module-pcb/scripts/place.py)
BOARD_L = 40.0
BOARD_W = 20.5
BOARD_T = 1.6
HOLE_X = 2.7
HOLE_PITCH = 16.6

# Stock housing, estimated from the photo
CASE_L = 53.0
CASE_W = 25.0

# What this board needs
CLEARANCE = 0.3  # board edge to wall
END_WALL = 1.6
FLOOR = 1.2
ROOF = 1.2
STANDOFF = 2.2  # tallest bottom-side part is the SOIC-8 at 1.75 mm
HEADROOM = 5.3  # the JST PH header stands 4.8 mm above the board
CABLE_D = 5.5  # opening for the 4-wire lead
CABLE_Z = BOARD_T + 2.4  # the header's wire axis above the board's bottom face
BOSS_R = 2.7
PILOT_D = 1.7  # M2 self-tapping screw into the base
SCREW_D = 2.3
HEAD_D = 4.2
HEAD_DEPTH = 2.0
LIP_T = 0.8
LIP_H = 1.5
LIP_GAP = 0.15

MID = BOARD_W / 2
X1 = BOARD_L + CLEARANCE  # inner face of the antenna-end wall
X0 = X1 + END_WALL - CASE_L  # outer face of the connector-end wall
Y0 = MID - CASE_W / 2
SIDE_WALL = -CLEARANCE - Y0
Z0 = -(STANDOFF + FLOOR)
Z_SPLIT = CABLE_Z
Z1 = BOARD_T + HEADROOM + ROOF
CASE_T = Z1 - Z0
HOLES = [(HOLE_X, MID - HOLE_PITCH / 2), (HOLE_X, MID + HOLE_PITCH / 2)]


def box(x0, x1, y0, y1, z0, z1):
    return (
        cq.Workplane("XY")
        .box(x1 - x0, y1 - y0, z1 - z0, centered=False)
        .translate((x0, y0, z0))
    )


def cylinder(x, y, z0, z1, r):
    return cq.Workplane("XY").circle(r).extrude(z1 - z0).translate((x, y, z0))


def shell(z0, z1):
    """The outer block between two heights with the cavity cut out, open at both ends."""
    outer = box(X0, X1 + END_WALL, Y0, Y0 + CASE_W, z0, z1).edges("|Z").fillet(2.0)
    cavity = box(X0 + END_WALL, X1, -CLEARANCE, BOARD_W + CLEARANCE, z0 - 1, z1 + 1)
    return outer.cut(cavity)


def cable_hole():
    return (
        cq.Workplane("YZ")
        .circle(CABLE_D / 2)
        .extrude(END_WALL + 2)
        .translate((X0 - 1, MID, CABLE_Z))
    )


def base():
    part = shell(Z0, Z_SPLIT).union(
        box(X0, X1 + END_WALL, Y0, Y0 + CASE_W, Z0, Z0 + FLOOR).edges("|Z").fillet(2.0)
    )
    for x, y in HOLES:
        part = part.union(cylinder(x, y, Z0, 0, BOSS_R))
        part = part.cut(cylinder(x, y, Z0 + 0.4, 0.1, PILOT_D / 2))
    # ledges that carry the antenna end of the board
    for y0, y1 in ((-CLEARANCE, 0.9), (BOARD_W - 0.9, BOARD_W + CLEARANCE)):
        part = part.union(box(BOARD_L - 6.0, X1, y0, y1, Z0, 0))
    return part.cut(cable_hole())


def lid():
    part = shell(Z_SPLIT, Z1).union(
        box(X0, X1 + END_WALL, Y0, Y0 + CASE_W, Z1 - ROOF, Z1).edges("|Z").fillet(2.0)
    )
    # lip that drops inside the base walls on the two long sides and the connector end
    lip_z0 = Z_SPLIT - LIP_H
    xa, xb = X0 + END_WALL + LIP_GAP, BOARD_L - 8.0
    ya, yb = -CLEARANCE + LIP_GAP, BOARD_W + CLEARANCE - LIP_GAP
    ring = box(xa, xb, ya, yb, lip_z0, Z_SPLIT).cut(
        box(xa + LIP_T, xb + 1, ya + LIP_T, yb - LIP_T, lip_z0 - 1, Z_SPLIT + 1)
    )
    part = part.union(ring)
    # bosses that clamp the board onto the base standoffs, with the screws through them
    inside = box(xa, X1, ya, yb, 0, Z1)
    for x, y in HOLES:
        part = part.union(cylinder(x, y, BOARD_T + 0.05, Z1, BOSS_R).intersect(inside))
    for x, y in HOLES:
        part = part.cut(cylinder(x, y, BOARD_T - 1, Z1 + 1, SCREW_D / 2))
        part = part.cut(cylinder(x, y, Z1 - HEAD_DEPTH, Z1 + 1, HEAD_D / 2))
    # ribs that hold the antenna end of the board down, outside the module's footprint
    for y0, y1 in ((-CLEARANCE, 0.5), (BOARD_W - 0.5, BOARD_W + CLEARANCE)):
        part = part.union(
            box(BOARD_L - 4.5, BOARD_L - 0.5, y0, y1, BOARD_T + 0.1, Z1 - ROOF)
        )
    return part.cut(cable_hole())


def main():
    out = Path(__file__).parent / "out"
    out.mkdir(exist_ok=True)
    parts = {"base": base(), "lid": lid()}
    for name, part in parts.items():
        cq.exporters.export(part, str(out / f"w41h1-case-{name}.step"))
        cq.exporters.export(
            part,
            str(out / f"w41h1-case-{name}.stl"),
            tolerance=0.02,
            angularTolerance=0.1,
        )
    print(f"case {CASE_L} x {CASE_W} x {CASE_T:.1f} mm, side wall {SIDE_WALL:.2f} mm")
    overlap = parts["base"].intersect(parts["lid"]).val()
    print(f"base and lid overlap: {overlap.Volume():.3f} mm3")
    if len(sys.argv) > 1:
        board = cq.importers.importStep(sys.argv[1])
        for name, part in parts.items():
            volume = sum(s.Volume() for s in part.intersect(board).solids().vals())
            print(f"{name} overlaps the board by {volume:.3f} mm3")


if __name__ == "__main__":
    main()
