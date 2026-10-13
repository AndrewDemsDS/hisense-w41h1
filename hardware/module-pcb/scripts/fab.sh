#!/usr/bin/env bash
# Checks the design and writes the fabrication files to fab/ (not tracked in git):
# gerbers and drill files, a zip of both, the assembly BOM and placement file in the
# columns JLCPCB expects, a schematic PDF and a STEP model.
# Usage: bash scripts/fab.sh
set -euo pipefail
# kicad-cli is silenced because it prints assert noise on every call: a failure still stops the script.
cd "$(dirname "$0")/.."

name=w41h1-module
out=fab
rm -rf "$out"
mkdir -p "$out/gerbers"

echo "ERC and DRC"
kicad-cli sch erc --severity-error --exit-code-violations -o "$out/erc.rpt" "$name.kicad_sch" >/dev/null 2>&1
kicad-cli pcb drc --severity-error --exit-code-violations -o "$out/drc.rpt" "$name.kicad_pcb" >/dev/null 2>&1

echo "gerbers and drill"
kicad-cli pcb export gerbers --layers F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts \
    --subtract-soldermask --use-drill-file-origin -o "$out/gerbers/" "$name.kicad_pcb" >/dev/null 2>&1
kicad-cli pcb export drill --format excellon --drill-origin plot --excellon-separate-th \
    --generate-map --map-format gerberx2 -o "$out/gerbers/" "$name.kicad_pcb" >/dev/null 2>&1
(cd "$out/gerbers" && python3 -m zipfile -c "../$name-gerbers.zip" ./*)

echo "assembly files"
kicad-cli sch export bom --fields 'Value,Reference,Footprint,LCSC,MPN,Manufacturer' \
    --labels 'Comment,Designator,Footprint,LCSC,MPN,Manufacturer' --group-by Value,Footprint,LCSC \
    --exclude-dnp -o "$out/$name-bom.csv" "$name.kicad_sch" >/dev/null 2>&1
kicad-cli pcb export pos --format csv --units mm --side both --use-drill-file-origin --exclude-dnp \
    -o "$out/$name-pos-kicad.csv" "$name.kicad_pcb" >/dev/null 2>&1
# Holes, test pads and the unpopulated programming pads have no part number: drop them.
grep -v ',"","",""$' "$out/$name-bom.csv" >"$out/$name-bom.tmp" && mv "$out/$name-bom.tmp" "$out/$name-bom.csv"
python3 scripts/jlc_cpl.py "$out/$name-pos-kicad.csv" "$out/$name-cpl.csv"

echo "documents"
kicad-cli sch export pdf -o "$out/$name-schematic.pdf" "$name.kicad_sch" >/dev/null 2>&1
kicad-cli pcb export step --drill-origin --subst-models --no-dnp --force -o "$out/$name.step" "$name.kicad_pcb" >/dev/null 2>&1

ls -1 "$out"
