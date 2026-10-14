#!/usr/bin/env python3
"""Place the footprints, draw the outline and pour the ground zones.

Every assembled part sits on the top side, so the board can go through JLCPCB's single-sided
"Economic" assembly. Only bare pads (programming and test points) are on the bottom. The
outline is sized for that, not for the stock module's bay: change BOARD_L and BOARD_W here and
run the script again. Every position below is derived from them or from the connector end.

Usage: python3 scripts/place.py [--unroute] [board.kicad_pcb]
Routing is kept unless --unroute is given. Tracks that no longer reach a moved pad show up in DRC.
"""

import sys
from pathlib import Path

import pcbnew

# Outline, in mm. Origin is the corner next to pin 1 of the A/C connector.
BOARD_L = 52.0
BOARD_W = 22.0
HOLE_X = 2.7
HOLE_PITCH = BOARD_W - 5.4  # centre to centre, 2.7 mm in from each long edge
EDGE_KEEPOUT = 0.6  # no tracks this close to the edge
ORIGIN = (100.0, 100.0)  # where the board corner sits on the sheet

MID = BOARD_W / 2
# ESP32-C3-WROOM-02: the antenna end of the module is 13.1 mm from the footprint
# origin and has to sit at the board edge.
MODULE_X = BOARD_L - 13.1
REG = (11.6, 4.6)  # centre of the buck regulator
COL_A = MODULE_X - 8.7  # column of small parts just left of the module
COL_B = MODULE_X - 10.7

NOT_FITTED = {"J2"}  # bare pads for programming, no part goes on them

# reference: (x, y, rotation in degrees, side)
PLACEMENT = {
    "H1": (HOLE_X, MID - HOLE_PITCH / 2, 0, "F"),
    "H2": (HOLE_X, MID + HOLE_PITCH / 2, 0, "F"),
    "J1": (6.5, MID - 3.0, 270, "F"),
    "U1": (MODULE_X, MID, 270, "F"),
    # 5 V to 3.3 V buck, laid out around the regulator at REG
    "U2": (REG[0], REG[1], 0, "F"),
    "C1": (REG[0], REG[1] + 2.8, 180, "F"),
    "L1": (REG[0], REG[1] + 5.9, 270, "F"),
    "C2": (REG[0] + 3.3, REG[1] + 6.97, 0, "F"),
    "C3": (REG[0] + 3.3, REG[1] + 9.1, 0, "F"),
    "C8": (REG[0] + 3.7, REG[1] + 1.7, 0, "F"),
    "R2": (REG[0] + 3.7, REG[1] - 0.95, 0, "F"),
    "R1": (REG[0] + 3.7, REG[1] - 2.7, 180, "F"),
    # status LED, next to its pin on the module
    "D1": (COL_B, 6.3, 90, "F"),
    "R6": (COL_B, 9.4, 90, "F"),
    # RS-485: protection next to the connector, transceiver between it and the module
    "D2": (10.6, MID + 5.5, 0, "F"),
    "U3": (21.5, MID + 5.0, 90, "F"),
    "C7": (25.6, MID + 2.0, 90, "F"),
    "R9": (25.6, MID + 5.5, 90, "F"),
    "R8": (COL_B, MID + 7.7, 90, "F"),
    # module strapping and decoupling, in two columns beside the module
    "R4": (COL_A, 3.2, 90, "F"),
    "R5": (COL_A, 6.3, 90, "F"),
    "R3": (COL_A, 9.4, 90, "F"),
    "C6": (COL_A, 12.5, 90, "F"),
    "R7": (COL_A, 15.6, 90, "F"),
    "C5": (COL_A, 18.7, 90, "F"),
    "C4": (COL_B, 3.2, 90, "F"),
    # bare pads on the bottom, nothing to assemble there
    "J2": (MODULE_X - 3.1, MID + 6.0, 90, "B"),
    "TP1": (MODULE_X + 4.6, MID + 3.25, 0, "B"),
    "TP2": (MODULE_X + 4.6, MID + 1.25, 0, "B"),
    "TP3": (9.0, MID + 4.5, 0, "B"),
    "TP4": (9.0, MID + 6.5, 0, "B"),
}

# The module footprint has 13 vias of 0.2 mm under its ground pad. JLCPCB charges extra for
# anything under 0.3 mm, so four of them become 0.3 mm vias and the rest become plain copper.
THERMAL_VIAS_KEPT = {(0.41, -0.9), (1.51, -0.9), (0.41, 1.3), (1.51, 1.3)}


def reg(*points):
    return [(REG[0] + dx, REG[1] + dy) for dx, dy in points]


# Connections laid down before the autorouter runs, because it gets them wrong: it gave the
# regulator's feedback node a long detour.
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
    # the reset capacitor's ground pad, which the routing boxes in otherwise
    ("/GND", 0.4, "F", [(COL_A, 11.725), (COL_A - 1.2, 11.725)], True),
    # module ground pin to the bottom pour
    (
        "/GND",
        0.4,
        "F",
        [(MODULE_X - 6.0, MID - 8.75), (MODULE_X - 6.0, MID - 7.0)],
        True,
    ),
]


def mm(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(ORIGIN[0] + x), pcbnew.FromMM(ORIGIN[1] + y))


def standard_thermal_vias(fp):
    for pad in fp.Pads():
        if pad.GetAttribute() != pcbnew.PAD_ATTRIB_PTH:
            continue
        rel = pad.GetFPRelativePosition()
        at = (round(pcbnew.ToMM(rel.x), 2), round(pcbnew.ToMM(rel.y), 2))
        if at in THERMAL_VIAS_KEPT:
            pad.SetDrillSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.3), pcbnew.FromMM(0.3)))
            pad.SetSize(
                pcbnew.F_Cu, pcbnew.VECTOR2I(pcbnew.FromMM(0.6), pcbnew.FromMM(0.6))
            )
        else:
            pad.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
            pad.SetDrillSize(pcbnew.VECTOR2I(0, 0))
            layers = pcbnew.LSET()
            layers.AddLayer(pcbnew.F_Cu)
            pad.SetLayerSet(layers)


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
        if ref == "U1":
            standard_thermal_vias(fp)
        if ref in NOT_FITTED:
            fp.SetAttributes(
                fp.GetAttributes() | pcbnew.FP_DNP | pcbnew.FP_EXCLUDE_FROM_POS_FILES
            )


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
    # for the vias that take the signals to the bottom layer.
    y0, y1 = MID - 9.0 + 3.6, MID + 9.0 - 3.6
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


# Silkscreen: (text, x, y, side, height)
LABELS = [
    ("W41H1-C3 rev B", 21.0, MID - 1.5, "F", 0.8),
    ("5V", 8.9, MID - 3.0, "F", 0.8),
    ("A", 8.7, MID - 1.0, "F", 0.8),
    ("B", 8.7, MID + 1.0, "F", 0.8),
    ("G", 8.7, MID + 3.0, "F", 0.8),
    ("PROG", MODULE_X - 12.5, MID + 4.2, "B", 0.8),
    ("1 GND 2 3V3", MODULE_X - 12.5, MID + 5.25, "B", 0.8),
    ("3 EN 4 IO9", MODULE_X - 12.5, MID + 6.3, "B", 0.8),
    ("5 TX 6 RX", MODULE_X - 12.5, MID + 7.35, "B", 0.8),
    ("1", MODULE_X + 1.3, MID + 8.65, "B", 0.8),
    ("D-", MODULE_X + 6.3, MID + 3.25, "B", 0.8),
    ("D+", MODULE_X + 6.3, MID + 1.25, "B", 0.8),
    ("5V", 7.3, MID + 4.5, "B", 0.8),
    ("GND", 6.9, MID + 6.5, "B", 0.8),
]


def labels(board):
    silk = (pcbnew.F_SilkS, pcbnew.B_SilkS)
    for d in list(board.GetDrawings()):
        if d.GetClass() == "PCB_TEXT" and d.GetLayer() in silk:
            board.Delete(d)
    for text, x, y, side, height in LABELS:
        t = pcbnew.PCB_TEXT(board)
        t.SetText(text)
        t.SetPosition(mm(x, y))
        t.SetLayer(pcbnew.F_SilkS if side == "F" else pcbnew.B_SilkS)
        t.SetMirrored(side == "B")
        t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(height), pcbnew.FromMM(height)))
        t.SetTextThickness(pcbnew.FromMM(0.15))
        board.Add(t)


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
    labels(board)
    keepouts(board)
    ground_zones(board)
    board.Save(path)
    print(f"placed {len(PLACEMENT)} footprints on a {BOARD_L} x {BOARD_W} mm outline")


if __name__ == "__main__":
    main()
