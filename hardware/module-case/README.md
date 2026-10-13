# Case for the replacement module PCB

> **This case has not been printed or fitted to a unit.** Its length and width are estimates of
> the stock housing, scaled off the internal photo in the FCC filing (FCC ID 2AGCCAEH-W41H1).
> The filing has no side view, so the thickness is what this board needs and may differ from
> the stock module's.

A base and a lid for the board in [`../module-pcb`](../module-pcb). Two M2 x 8 self-tapping
screws go through the lid, through the board's two mounting holes and into the base, so the
same screws close the case and clamp the board.

| Dimension | Value | Source |
|---|---|---|
| Length | 53.0 mm | photo, with the 40 mm board as the ruler |
| Width | 25.0 mm | photo |
| Thickness | 11.5 mm | 1.2 mm floor, 2.2 mm under the board, 1.6 mm board, 5.3 mm above it, 1.2 mm roof |
| Cable opening | 5.5 mm round, on the short end by the connector | sized for the 4-wire lead, not measured |

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
- Walls are 1.95 mm at the sides and 1.6 mm at the ends, which suits a 0.4 mm nozzle.
- The lid's lip has 0.15 mm of clearance to the base walls. Raise `LIP_GAP` if your printer
  runs tight.
- Use a plastic that holds its shape inside a running indoor unit: PETG or ASA, not PLA.

## Not covered yet

- The stock housing has an eyelet for fixing it inside the unit. This case has none, because
  how the units hold the module has not been checked.
- The status LED is not visible through the lid.
