#!/usr/bin/env python3
"""Route the board with Freerouting, from the command line.

Exports the board as a Specctra design, runs Freerouting on it, imports the session file back
into the board and cleans up after it: Freerouting 2.5 necks a few segments below the board's
minimum track width, so every track narrower than that minimum is widened to it; it leaves
vias with both tracks on the same layer, which are removed; and the ground pours are refilled
because the import leaves them stale. Freerouting is a Java program; the jar's path comes from
FREEROUTING_JAR (default ~/.local/share/freerouting/freerouting-2.5.0.jar).

Freerouting does not count the pre-routed regulator connections from place.py as routed, so
its own log ends with a few "unrouted" items on those nets. The number that matters is
KiCad's, printed on the last line. It counts the ground pour islands too, which scripts/stitch.py
joins next; what is left after that has to be zero, and the router gives a different result on
every run, so run both again if it is not.

Usage: python3 scripts/route.py [--passes N] [board.kicad_pcb]
Run scripts/place.py --unroute first, and scripts/stitch.py after.
"""

import os
import subprocess
import sys
from pathlib import Path

import pcbnew


def widen_narrow_tracks(board):
    """Bring every track below the board's minimum width up to it. Returns the count."""
    minimum = board.GetDesignSettings().m_TrackMinWidth
    widened = 0
    for track in board.GetTracks():
        if track.GetClass() == "PCB_TRACK" and track.GetWidth() < minimum:
            track.SetWidth(minimum)
            widened += 1
    return widened


def remove_same_layer_vias(board):
    """Drop every via whose tracks all meet it on one layer: the tracks still touch at its
    centre, so nothing is lost. Vias on a net with a pour are kept, the pour is their other
    side. Returns the count."""
    zone_nets = {z.GetNetCode() for z in board.Zones() if not z.GetIsRuleArea()}
    tracks = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
    removed = 0
    for via in [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]:
        if via.GetNetCode() in zone_nets:
            continue
        at = via.GetPosition()
        layers = {
            t.GetLayer()
            for t in tracks
            if t.GetNetCode() == via.GetNetCode() and at in (t.GetStart(), t.GetEnd())
        }
        if len(layers) < 2:
            board.Delete(via)
            removed += 1
    return removed


def refill_zones(board):
    zones = [z for z in board.Zones() if not z.GetIsRuleArea()]
    pcbnew.ZONE_FILLER(board).Fill(zones)


def unconnected_count(board):
    connectivity = board.GetConnectivity()
    connectivity.RecalculateRatsnest()
    return connectivity.GetUnconnectedCount(True)


def main():
    args = sys.argv[1:]
    passes = "40"
    if "--passes" in args:
        i = args.index("--passes")
        passes = args[i + 1]
        del args[i : i + 2]
    path = Path(
        args[0] if args else Path(__file__).parent.parent / "w41h1-module.kicad_pcb"
    )
    jar = Path(
        os.environ.get(
            "FREEROUTING_JAR",
            Path.home() / ".local/share/freerouting/freerouting-2.5.0.jar",
        )
    )
    if not jar.is_file():
        raise SystemExit(f"Freerouting jar not found: {jar} (set FREEROUTING_JAR)")
    dsn = path.with_suffix(".dsn")
    ses = path.with_suffix(".ses")
    board = pcbnew.LoadBoard(str(path))
    if not pcbnew.ExportSpecctraDSN(board, str(dsn)):
        raise SystemExit("Specctra export failed")
    ses.unlink(missing_ok=True)
    cmd = [
        "java",
        "-Djava.awt.headless=true",
        "-jar",
        str(jar),
        "-de",
        str(dsn),
        "-do",
        str(ses),
        "-mp",
        passes,
    ]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)
    if not ses.is_file():
        raise SystemExit("Freerouting wrote no session file")
    if not pcbnew.ImportSpecctraSES(board, str(ses)):
        raise SystemExit("Specctra session import failed")
    widened = widen_narrow_tracks(board)
    removed = remove_same_layer_vias(board)
    refill_zones(board)
    board.Save(str(path))
    tracks = sum(1 for t in board.GetTracks() if t.GetClass() == "PCB_TRACK")
    vias = sum(1 for t in board.GetTracks() if t.GetClass() == "PCB_VIA")
    print(
        f"routed: {tracks} track segments, {vias} vias, {widened} tracks widened to the "
        f"minimum, {removed} same-layer vias removed, {unconnected_count(board)} unconnected "
        "(pour islands included, stitch.py joins those)"
    )
    dsn.unlink()
    ses.unlink()


if __name__ == "__main__":
    main()
