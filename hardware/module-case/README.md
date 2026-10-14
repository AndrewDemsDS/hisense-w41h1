# Case for the replacement module PCB

> **This case has not been printed or fitted to a unit.** It is sized around the rev B test
> board, not the stock module's housing.

A base and a lid for the board in [`../module-pcb`](../module-pcb). Two M2 x 8 self-tapping
screws go through the lid, through the board's two mounting holes and into the base, so the
same screws close the case and clamp the board.

| Dimension | Value |
|---|---|
| Length | 65.0 mm |
| Width | 26.2 mm |
| Thickness | 11.5 mm: 1.2 mm floor, 2.2 mm under the board, 1.6 mm board, 5.3 mm above it, 1.2 mm roof |
| Cable opening | 5.5 mm round, on the short end by the connector, sized for the 4-wire lead but not measured |

The board sits at the antenna end. The 9.5 mm between the connector and the end wall is for
the lead's plug and the bend in its wires.

## Building it

```bash
bash ../module-pcb/scripts/fab.sh       # writes the board's STEP model
uv run --python 3.12 --with cadquery python case.py ../module-pcb/fab/w41h1-module.step
```

`out/` (not tracked in git) then holds each part as STEP and STL. With the board model as an
argument the script also prints how much each part overlaps the board. That has to be zero,
and it is for the dimensions in the file.

All dimensions are constants at the top of `case.py`. The board's size and hole positions are
repeated there: change them together with `../module-pcb/scripts/place.py`.

## Printing

- Base: open side up. Lid: roof on the bed. Neither needs supports.
- Walls are 1.8 mm at the sides and 1.6 mm at the ends, which suits a 0.4 mm nozzle.
- The lid's lip has 0.15 mm of clearance to the base walls. Raise `LIP_GAP` if your printer
  runs tight.
- Use a plastic that holds its shape inside a running indoor unit: PETG or ASA, not PLA.

## Not covered yet

- The stock housing has an eyelet for fixing it inside the unit. This case has none, because
  how the units hold the module has not been checked.
- The status LED is not visible through the lid.
