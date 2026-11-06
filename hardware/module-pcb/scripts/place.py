#!/usr/bin/env python3
"""Place the footprints, draw the outline and pour the ground zones.

Every assembled part sits on the top side, so the board can go through JLCPCB's single-sided
"Economic" assembly. The two connectors are through-hole and not fitted: the A/C header is
the one desoldered from the stock module, the programming header is a 2.54 mm pin strip. The
outline is sized for that, not for the stock module's bay: change BOARD_L and BOARD_W here and
run the script again. Every position below is derived from them or from the connector end.

The script also writes the project footprint for the A/C header (w41h1.pretty) and puts the
footprints listed in FOOTPRINTS on the board in place of whatever the schematic update left
there, keeping the pad nets, so a footprint change needs no manual step in the GUI.

Usage: python3 scripts/place.py [--unroute] [board.kicad_pcb]
Routing is kept unless --unroute is given. Tracks that no longer reach a moved pad show up in DRC.
"""

import os
import sys
from pathlib import Path

import pcbnew

# Outline, in mm. Origin is the corner next to pin 1 of the A/C connector.
BOARD_L = 56.0
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

# A/C header J1: the stock module's own header, desoldered and soldered in by hand. Its mating
# face sits on the board edge, the pin row J1_PIN_INSET behind it. Pitch and the absence of
# pegs were measured on the stock module on 2026-11-06; the rest is unmeasured.
J1_PITCH = 2.0
J1_PIN_INSET = 4.0  # VERIFY: board edge to the pin row, measured on the stock module
J1_DRILL = 0.9  # VERIFY: the stock header's pin size, 0.9 mm takes a 0.64 mm square pin
J1_BODY = (-1.5, 7.5, 1.5)  # VERIFY: body x from pin 1, and its reach behind the pins
# Programming header J2: 2.54 mm pin strip on the top side, in the lane between the RS-485
# parts and the module's passives.
J2_X = COL_B - 2.9

NOT_FITTED = {
    "J1",
    "J2",
}  # through-hole connectors the owner solders in, nothing to assemble

LIB_DIR = Path(__file__).parent.parent / "w41h1.pretty"
SYSTEM_FOOTPRINTS = Path(
    os.environ.get("KICAD10_FOOTPRINT_DIR", "/usr/share/kicad/footprints")
)
# reference: (library nickname, library directory, footprint name)
FOOTPRINTS = {
    "J1": ("w41h1", LIB_DIR, "AC_Header_1x04_P2.00mm_Horizontal"),
    "J2": (
        "Connector_PinHeader_2.54mm",
        SYSTEM_FOOTPRINTS / "Connector_PinHeader_2.54mm.pretty",
        "PinHeader_1x06_P2.54mm_Vertical",
    ),
}

# reference: (x, y, rotation in degrees, side)
PLACEMENT = {
    "H1": (HOLE_X, MID - HOLE_PITCH / 2, 0, "F"),
    "H2": (HOLE_X, MID + HOLE_PITCH / 2, 0, "F"),
    "J1": (J1_PIN_INSET, MID - 3.0, 270, "F"),
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
    # programming header on top, a column with pin 1 at the top edge (the library footprint
    # runs along +Y); USB and supply test pads underneath
    "J2": (J2_X, MID - 6.35, 0, "F"),
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
    # VIN to the input bypass capacitor, which the autorouter leaves unconnected
    ("/+5V", 0.5, "F", reg((1.14, 1.7), (2.925, 1.7)), False),
    # the reset capacitor's ground pad, which the routing boxes in otherwise
    ("/GND", 0.4, "F", [(COL_A, 11.725), (COL_A - 1.2, 11.725)], True),
    # the 10 uF decoupling capacitor's ground pad, boxed in the same way: to the pulldown's
    # ground pad, with a via between them
    ("/GND", 0.4, "F", [(COL_A, 17.925), (COL_A - 1.0, 17.925)], True),
    ("/GND", 0.4, "F", [(COL_A - 1.0, 17.925), (COL_B, 17.875)], False),
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


def write_j1_footprint():
    """The A/C header footprint: four holes on J1_PITCH, the body outline from J1_BODY."""
    x0, x1, back = J1_BODY
    face = J1_PIN_INSET
    pads = []
    for n in range(4):
        shape = "roundrect" if n == 0 else "oval"
        extra = "\n\t\t(roundrect_rratio 0.25)" if n == 0 else ""
        pads.append(
            f'\t(pad "{n + 1}" thru_hole {shape}\n\t\t(at {n * J1_PITCH} 0)\n'
            f"\t\t(size 1.5 1.9)\n\t\t(drill {J1_DRILL})\n"
            f'\t\t(layers "*.Cu" "*.Mask")\n\t\t(remove_unused_layers no){extra}\n\t)'
        )

    def rect(layer, grow, width, face_grow=0.0):
        # the mating face sits on the board edge: the silkscreen stops EDGE_KEEPOUT short
        # of it, the courtyard reaches past it
        return (
            f"\t(fp_rect\n\t\t(start {x0 - grow} {-back - grow})\n"
            f"\t\t(end {x1 + grow} {face + face_grow})\n"
            f"\t\t(stroke\n\t\t\t(width {width})\n\t\t\t(type default)\n\t\t)\n"
            f'\t\t(fill no)\n\t\t(layer "{layer}")\n\t)'
        )

    body = "\n".join(
        [
            f'(footprint "{FOOTPRINTS["J1"][2]}"\n\t(version 20241229)\n'
            '\t(generator "place.py")\n\t(generator_version "1")\n\t(layer "F.Cu")\n'
            '\t(descr "4-pin 2.0 mm side-entry header desoldered from the stock AEH-W41H1 '
            'module; mating face at +Y, body size unverified")\n\t(tags "connector 2.00mm")\n'
            '\t(property "Reference" "REF**"\n\t\t(at 3 -2.5 0)\n\t\t(layer "F.SilkS")\n'
            "\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n\t\t\t\t(thickness 0.15)\n"
            "\t\t\t)\n\t\t)\n\t)\n"
            f'\t(property "Value" "{FOOTPRINTS["J1"][2]}"\n\t\t(at 3 {face + 1.2} 0)\n'
            '\t\t(layer "F.Fab")\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n'
            "\t\t\t\t(thickness 0.15)\n\t\t\t)\n\t\t)\n\t)\n\t(attr through_hole)",
            rect("F.SilkS", 0.11, 0.12, -EDGE_KEEPOUT),
            rect("F.Fab", 0, 0.1),
            rect("F.CrtYd", 0.5, 0.05, 0.25),
            # pin 1 marker: a short line beside the first pad, outside the body
            f"\t(fp_line\n\t\t(start {x0 - 0.9} -0.8)\n\t\t(end {x0 - 0.9} 0.8)\n"
            '\t\t(stroke\n\t\t\t(width 0.15)\n\t\t\t(type default)\n\t\t)\n\t\t(layer "F.SilkS")\n\t)',
            *pads,
            ")",
        ]
    )
    LIB_DIR.mkdir(exist_ok=True)
    (LIB_DIR / f"{FOOTPRINTS['J1'][2]}.kicad_mod").write_text(body + "\n")


def swap_footprints(board, fps):
    """Put the footprints in FOOTPRINTS on the board, keeping position, nets and the link to
    the schematic symbol, so the schematic's footprint field and the board agree. The copy is
    replaced even when the name already matches, so an edit to the written footprint reaches
    the board on the next run."""
    for ref, (nick, libdir, name) in FOOTPRINTS.items():
        old = fps[ref]
        new = pcbnew.FootprintLoad(str(libdir), name)
        if new is None:
            raise SystemExit(f"footprint {name} not found in {libdir}")
        new.SetFPID(pcbnew.LIB_ID(nick, name))
        new.SetReference(ref)
        new.SetValue(old.GetValue())
        new.SetPath(old.GetPath())
        new.SetPosition(old.GetPosition())
        for pad in new.Pads():
            twin = old.FindPadByNumber(pad.GetNumber())
            if twin is not None:
                pad.SetNet(twin.GetNet())
        board.Add(new)
        board.Delete(old)
        fps[ref] = new


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


# Silkscreen: (text, x, y, side, height[, angle])
J1_LABEL_X = J1_PIN_INSET + 2.3
J2_LABEL_X = J2_X - 2.15  # clear of the header's silkscreen outline
LABELS = [
    ("W41H1-C3 rev C", 21.0, MID - 1.5, "F", 0.8),
    ("5V", J1_LABEL_X + 0.2, MID - 3.0, "F", 0.8),
    ("A", J1_LABEL_X, MID - 1.0, "F", 0.8),
    ("B", J1_LABEL_X, MID + 1.0, "F", 0.8),
    ("G", J1_LABEL_X, MID + 3.0, "F", 0.8),
    # programming header, one name beside each pin, reading along the header
    ("GND", J2_LABEL_X, MID - 6.35, "F", 0.8, 90),
    ("3V3", J2_LABEL_X, MID - 3.81, "F", 0.8, 90),
    ("EN", J2_LABEL_X, MID - 1.27, "F", 0.8, 90),
    ("IO9", J2_LABEL_X, MID + 1.27, "F", 0.8, 90),
    ("TX", J2_LABEL_X, MID + 3.81, "F", 0.8, 90),
    ("RX", J2_LABEL_X, MID + 6.35, "F", 0.8, 90),
    ("PROG", J2_X, MID - 8.4, "F", 0.8),
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
    for text, x, y, side, height, *angle in LABELS:
        t = pcbnew.PCB_TEXT(board)
        t.SetText(text)
        t.SetPosition(mm(x, y))
        t.SetLayer(pcbnew.F_SilkS if side == "F" else pcbnew.B_SilkS)
        t.SetMirrored(side == "B")
        if angle:
            t.SetTextAngle(pcbnew.EDA_ANGLE(angle[0], pcbnew.DEGREES_T))
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
    write_j1_footprint()
    fps = {fp.GetReference(): fp for fp in board.GetFootprints()}
    swap_footprints(board, fps)
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
