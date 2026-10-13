#!/usr/bin/env python3
"""Place the footprints, draw the outline and pour the ground zones.

The outline numbers are estimates scaled off the FCC internal photo (FCC ID
2AGCCAEH-W41H1), not caliper readings. Change BOARD_L, BOARD_W and HOLE_* here
and run the script again: every position below is derived from them or from the
connector end of the board.

Usage: python3 scripts/place.py [--unroute] [board.kicad_pcb]
Routing is kept unless --unroute is given. Tracks that no longer reach a moved pad show up in DRC.
"""

import sys
from pathlib import Path

import pcbnew

# Outline, in mm. Origin is the corner next to pin 1 of the A/C connector.
BOARD_L = 40.0  # estimate, stock board measures about 39.7 in the photo
BOARD_W = 20.5  # estimate, about 20.6 in the photo
HOLE_X = 2.7  # estimate
HOLE_PITCH = 16.6  # estimate, centre to centre
EDGE_KEEPOUT = 0.6  # no tracks this close to the edge
ORIGIN = (100.0, 100.0)  # where the board corner sits on the sheet

MID = BOARD_W / 2
# ESP32-C3-WROOM-02: the antenna end of the module is 13.1 mm from the footprint
# origin and has to sit at the board edge.
MODULE_X = BOARD_L - 13.1
REG = (11.6, 4.6)  # centre of the buck regulator

# reference: (x, y, rotation in degrees, side)
PLACEMENT = {
    "H1": (HOLE_X, MID - HOLE_PITCH / 2, 0, "F"),
    "H2": (HOLE_X, MID + HOLE_PITCH / 2, 0, "F"),
    "J1": (6.5, MID - 3.0, 270, "F"),
    "U1": (MODULE_X, MID, 270, "F"),
    # 5 V to 3.3 V buck, top side, laid out around the regulator at REG
    "U2": (REG[0], REG[1], 0, "F"),
    "C1": (REG[0], REG[1] + 2.8, 180, "F"),
    "L1": (REG[0], REG[1] + 5.9, 270, "F"),
    "C2": (REG[0] + 3.3, REG[1] + 6.97, 0, "F"),
    "C3": (REG[0] + 3.3, REG[1] + 9.1, 0, "F"),
    "C8": (REG[0] + 3.7, REG[1] + 1.7, 0, "F"),
    "R2": (REG[0] + 3.7, REG[1] - 0.95, 0, "F"),
    "R1": (REG[0] + 3.7, REG[1] - 2.7, 180, "F"),
    # status LED, top side
    "D1": (12.0, 18.9, 0, "F"),
    "R6": (15.5, 18.9, 0, "F"),
    # RS-485, bottom side
    "U3": (16.0, MID, 90, "B"),
    "D2": (10.5, MID, 0, "B"),
    "C7": (19.6, 7.8, 90, "B"),
    "R9": (19.6, 11.0, 90, "B"),
    "R8": (16.0, 15.4, 0, "B"),
    # module strapping and decoupling, bottom side under the module
    "R4": (MODULE_X - 4.5, 4.2, 90, "B"),
    "R5": (MODULE_X - 2.7, 4.2, 90, "B"),
    "C6": (MODULE_X + 1.7, 4.2, 90, "B"),
    "R3": (MODULE_X + 3.5, 4.2, 90, "B"),
    "C4": (MODULE_X + 5.3, 4.2, 90, "B"),
    "C5": (MODULE_X + 5.3, 7.6, 90, "B"),
    "R7": (MODULE_X + 3.0, 16.3, 90, "B"),
    # programming pads, bottom side
    "J2": (MODULE_X - 3.1, 16.2, 90, "B"),
    "TP1": (MODULE_X + 4.6, 13.5, 0, "B"),
    "TP2": (MODULE_X + 4.6, 11.5, 0, "B"),
    "TP3": (9.0, 15.5, 0, "B"),
    "TP4": (9.0, 17.5, 0, "B"),
}


def reg(*points):
    return [(REG[0] + dx, REG[1] + dy) for dx, dy in points]


# Connections laid down before the autorouter runs, because it gets them wrong: it gave the
# regulator's feedback node a long detour and left the module's supply pin on an island.
# The regulator follows the datasheet's advice: input capacitor, inductor and output
# capacitor next to the part, a short switch node, a short feedback node.
# (net, width, layer, points, via at the last point)
PREROUTE = [
    # EN to VIN under the package
    (
        "/+5V",
        0.2,
        "F",
        reg((-1.14, -0.95), (0.1, -0.95), (0.1, 0.95), (1.14, 0.95)),
        False,
    ),
    # VIN to the input capacitor
    ("/+5V", 0.5, "F", reg((1.14, 0.95), (1.14, 2.6), (0.95, 2.8)), False),
    # switch node to the inductor, between the input capacitor's pads
    ("/SW", 0.3, "F", reg((-1.14, 0.95), (-0.6, 1.5), (0, 1.5), (0, 4.83)), False),
    # ground pin to the input capacitor's ground pad, with vias to the bottom pour
    ("/GND", 0.5, "F", reg((-1.14, 0), (-2.3, 0)), True),
    ("/GND", 0.5, "F", reg((-2.3, 0), (-2.3, 2.8)), True),
    ("/GND", 0.5, "F", reg((-2.3, 2.8), (-0.95, 2.8)), False),
    # feedback pin to the divider
    ("/FB", 0.2, "F", reg((1.14, -0.95), (2.875, -0.95), (2.875, -2.7)), False),
    # inductor to the output capacitors
    ("/+3V3", 0.5, "F", reg((0, 6.97), (2.35, 6.97), (2.35, 9.1)), False),
    # module ground pin to the bottom pour
    (
        "/GND",
        0.4,
        "F",
        [(MODULE_X - 6.0, MID - 8.75), (MODULE_X - 6.0, MID - 7.0)],
        True,
    ),
    # 3.3 V from the pullups (R5) across to the module's decoupling (R3, C4, C5), on the top
    # layer in the lane the under-module keepout leaves free
    ("/+3V3", 0.5, "B", [(MODULE_X - 2.7, 5.025), (MODULE_X - 2.7, 6.3)], True),
    ("/+3V3", 0.5, "F", [(MODULE_X - 2.7, 6.3), (MODULE_X + 3.5, 6.3)], True),
    ("/+3V3", 0.5, "B", [(MODULE_X + 3.5, 6.3), (MODULE_X + 3.5, 5.025)], False),
]


def mm(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(ORIGIN[0] + x), pcbnew.FromMM(ORIGIN[1] + y))


def place(fps):
    missing = set(fps) ^ set(PLACEMENT)
    if missing:
        raise SystemExit(f"board and PLACEMENT disagree on: {sorted(missing)}")
    for ref, (x, y, rot, side) in PLACEMENT.items():
        fp = fps[ref]
        if fp.IsFlipped():
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        fp.SetOrientationDegrees(0)
        fp.SetPosition(mm(x, y))
        if side == "B":
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        fp.SetOrientationDegrees(rot)
        # The board is too small for legible designators: they stay on the fab layer only.
        fp.Reference().SetVisible(False)


def outline(board):
    for d in list(board.GetDrawings()):
        if d.GetLayer() == pcbnew.Edge_Cuts:
            board.Delete(d)
    rect = pcbnew.PCB_SHAPE(board)
    rect.SetShape(pcbnew.SHAPE_T_RECT)
    rect.SetStart(mm(0, 0))
    rect.SetEnd(mm(BOARD_L, BOARD_W))
    rect.SetLayer(pcbnew.Edge_Cuts)
    rect.SetWidth(pcbnew.FromMM(0.1))
    board.Add(rect)


def ground_zones(board):
    gnd = board.FindNet("/GND")
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        zone = pcbnew.ZONE(board)
        zone.SetLayer(layer)
        zone.SetNet(gnd)
        zone.SetZoneName("GND")
        zone.SetLocalClearance(pcbnew.FromMM(0.25))
        zone.SetMinThickness(pcbnew.FromMM(0.2))
        zone.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
        zone.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        poly = zone.Outline()
        poly.NewOutline()
        for x, y in ((0, 0), (BOARD_L, 0), (BOARD_L, BOARD_W), (0, BOARD_W)):
            poly.Append(mm(x, y))
        board.Add(zone)
    pcbnew.ZONE_FILLER(board).Fill([z for z in board.Zones() if not z.GetIsRuleArea()])


def track_keepout(board, name, layers, corners, vias=True):
    """A rule area that stops tracks (and vias) but lets the ground pour through."""
    area = pcbnew.ZONE(board)
    area.SetIsRuleArea(True)
    area.SetZoneName(name)
    lset = pcbnew.LSET()
    for layer in layers:
        lset.AddLayer(layer)
    area.SetLayerSet(lset)
    area.SetDoNotAllowTracks(True)
    area.SetDoNotAllowVias(vias)
    area.SetDoNotAllowZoneFills(False)
    area.SetDoNotAllowPads(False)
    area.SetDoNotAllowFootprints(False)
    poly = area.Outline()
    poly.NewOutline()
    for x, y in corners:
        poly.Append(mm(x, y))
    board.Add(area)


def keepouts(board):
    """Keep the autorouter off the board edge and off the top layer under the module."""
    both = (pcbnew.F_Cu, pcbnew.B_Cu)
    e = EDGE_KEEPOUT
    strips = {
        "edge-top": ((0, 0), (BOARD_L, 0), (BOARD_L, e), (0, e)),
        "edge-bottom": (
            (0, BOARD_W - e),
            (BOARD_L, BOARD_W - e),
            (BOARD_L, BOARD_W),
            (0, BOARD_W),
        ),
        "edge-left": ((0, 0), (e, 0), (e, BOARD_W), (0, BOARD_W)),
    }
    for name, corners in strips.items():
        track_keepout(board, name, both, corners)
    # Module body: 18 mm wide, pads on the two long sides. Leave a lane inside each pad row
    # for the vias that take the signals to the bottom layer, wider on the side that also
    # carries the pre-routed 3.3 V link.
    y0, y1 = MID - 9.0 + 5.9, MID + 9.0 - 3.6
    x0, x1 = MODULE_X - 6.9, MODULE_X + 7.1
    track_keepout(
        board,
        "under-module",
        (pcbnew.F_Cu,),
        ((x0, y0), (x1, y0), (x1, y1), (x0, y1)),
        vias=False,
    )


def preroute(board):
    for net_name, width, layer, points, via in PREROUTE:
        net = board.FindNet(net_name)
        for start, end in zip(points, points[1:]):
            track = pcbnew.PCB_TRACK(board)
            track.SetStart(mm(*start))
            track.SetEnd(mm(*end))
            track.SetWidth(pcbnew.FromMM(width))
            track.SetLayer(pcbnew.F_Cu if layer == "F" else pcbnew.B_Cu)
            track.SetNet(net)
            board.Add(track)
        if via:
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(mm(*points[-1]))
            v.SetWidth(pcbnew.F_Cu, pcbnew.FromMM(0.6))
            v.SetDrill(pcbnew.FromMM(0.3))
            v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
            v.SetNet(net)
            board.Add(v)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path = (
        args[0]
        if args
        else str(Path(__file__).parent.parent / "w41h1-module.kicad_pcb")
    )
    board = pcbnew.LoadBoard(path)
    fps = {fp.GetReference(): fp for fp in board.GetFootprints()}
    place(fps)
    if "--unroute" in sys.argv:
        for t in list(board.GetTracks()):
            board.Delete(t)
        preroute(board)
    outline(board)
    # Gerbers, drill and placement files all take the bottom-left board corner as origin.
    board.GetDesignSettings().SetAuxOrigin(mm(0, BOARD_W))
    for z in list(board.Zones()):
        board.Delete(z)
    keepouts(board)
    ground_zones(board)
    board.Save(path)
    print(f"placed {len(PLACEMENT)} footprints on a {BOARD_L} x {BOARD_W} mm outline")


if __name__ == "__main__":
    main()
