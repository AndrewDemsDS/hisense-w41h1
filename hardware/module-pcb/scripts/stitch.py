#!/usr/bin/env python3
"""Join the ground pour islands with vias.

After routing, the two ground pours are cut into islands by the tracks. This script finds
the islands on each layer, treats every existing ground via and through-hole ground pad as a
link between the islands it sits in, and adds a via wherever one joins two groups that are
not joined yet. A via is only placed where its whole footprint, plus a margin, lies inside
ground copper on both layers, so it cannot touch another net.

Usage: python3 scripts/stitch.py [board.kicad_pcb]
Run it after routing and again after any change to the tracks.
"""

import sys
from pathlib import Path

import pcbnew

VIA_SIZE = 0.6
VIA_DRILL = 0.3
MARGIN = 0.03  # ground copper required beyond the via's edge (the pour already keeps its clearance)
GRID = 0.1
LAYERS = (pcbnew.F_Cu, pcbnew.B_Cu)


class Groups:
    """Union-find over (layer, island) nodes."""

    def __init__(self):
        self.parent = {}

    def find(self, node):
        self.parent.setdefault(node, node)
        while self.parent[node] != node:
            self.parent[node] = self.parent[self.parent[node]]
            node = self.parent[node]
        return node

    def join(self, a, b):
        a, b = self.find(a), self.find(b)
        if a == b:
            return False
        self.parent[a] = b
        return True


def island_at(polys, point, radius):
    """Index of the island that holds the whole disc around point, or None."""
    r = pcbnew.FromMM(radius)
    probes = [(0, 0), (r, 0), (-r, 0), (0, r), (0, -r)]
    d = int(r * 0.7071)
    probes += [(d, d), (d, -d), (-d, d), (-d, -d)]
    for index in range(polys.OutlineCount()):
        if all(
            polys.Contains(pcbnew.VECTOR2I(point.x + dx, point.y + dy), index)
            for dx, dy in probes
        ):
            return index
    return None


def main():
    path = (
        sys.argv[1]
        if len(sys.argv) > 1
        else str(Path(__file__).parent.parent / "w41h1-module.kicad_pcb")
    )
    board = pcbnew.LoadBoard(path)
    gnd = board.FindNet("/GND")
    zones = [z for z in board.Zones() if not z.GetIsRuleArea()]
    pcbnew.ZONE_FILLER(board).Fill(zones)
    fills = {}
    for zone in zones:
        for layer in LAYERS:
            if zone.IsOnLayer(layer):
                fills[layer] = zone.GetFilledPolysList(layer)

    groups = Groups()
    links = [
        t.GetPosition()
        for t in board.GetTracks()
        if t.GetClass() == "PCB_VIA" and t.GetNetCode() == gnd.GetNetCode()
    ]
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if (
                pad.GetNetCode() == gnd.GetNetCode()
                and pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH
            ):
                links.append(pad.GetPosition())
    for point in links:
        nodes = [(layer, island_at(fills[layer], point, 0.01)) for layer in LAYERS]
        if None not in (n[1] for n in nodes):
            groups.join(*nodes)

    box = board.GetBoardEdgesBoundingBox()
    step = pcbnew.FromMM(GRID)
    reach = VIA_SIZE / 2 + MARGIN
    added = 0
    for x in range(box.GetLeft(), box.GetRight(), step):
        for y in range(box.GetTop(), box.GetBottom(), step):
            point = pcbnew.VECTOR2I(x, y)
            nodes = [(layer, island_at(fills[layer], point, reach)) for layer in LAYERS]
            if None in (n[1] for n in nodes) or not groups.join(*nodes):
                continue
            via = pcbnew.PCB_VIA(board)
            via.SetPosition(point)
            via.SetWidth(pcbnew.F_Cu, pcbnew.FromMM(VIA_SIZE))
            via.SetDrill(pcbnew.FromMM(VIA_DRILL))
            via.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
            via.SetNet(gnd)
            board.Add(via)
            added += 1

    pcbnew.ZONE_FILLER(board).Fill(zones)
    board.Save(path)
    islands = sum(fills[layer].OutlineCount() for layer in LAYERS)
    print(f"{islands} ground islands, {added} vias added")


if __name__ == "__main__":
    main()
