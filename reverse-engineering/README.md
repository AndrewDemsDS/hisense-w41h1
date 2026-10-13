# Reverse engineering the Hisense AEH-W41H1

The record of how the AEH-W41H1 Wi-Fi module and the A/C's serial bus were worked out: the
hardware, the bus protocol, and the vendor firmware. The firmware in
[`../firmware/esphome/`](../firmware/esphome/) is built on these pages.

The W41H1 is the ConnectLife generation of Hisense's A/C module. The older AEH-W4A1 had local
integrations ready to use. The W41H1 had none, which is why this work was done.

Reverse engineering here serves interoperability with hardware you own.

## Where to start

| You want to | Read |
|---|---|
| know the bus protocol | [`docs/03-rs485-ac-protocol.md`](docs/03-rs485-ac-protocol.md), the reference |
| check a claim of the protocol page against the vendor's code | [`docs/10-stock-fw-init-and-comms.md`](docs/10-stock-fw-init-and-comms.md) |
| support another A/C model | [`docs/11-model-capability-map.md`](docs/11-model-capability-map.md) |
| capture and decode frames from your own unit | [`tools/README.md`](tools/README.md) |
| help settle something | [`docs/14-open-questions.md`](docs/14-open-questions.md) |
| build a working unit | not here: [`../docs/guide/User-Guide.md`](../docs/guide/User-Guide.md) |

## The pages, in reading order

"Reference" means the page is kept current and the firmware relies on it. "History" means the
page records a route that was explored or a state of the project that is over. It is kept because
the facts and the reasons in it are still true, and it says at its top what replaced it.

| Page | What it covers | Kind |
|---|---|---|
| [`docs/01-hardware.md`](docs/01-hardware.md) | The parts on the board and how the flash was read | Reference |
| [`hardware/pinouts.md`](hardware/pinouts.md) | Pin diagrams: flash, transceiver, the A/C's 4-pin port | Reference |
| [`docs/03-rs485-ac-protocol.md`](docs/03-rs485-ac-protocol.md) | The bus: physical layer, frame envelope, classes, link bring-up, status and command byte maps, the "77" request bits | Reference, the main one |
| [`docs/10-stock-fw-init-and-comms.md`](docs/10-stock-fw-init-and-comms.md) | The vendor firmware's bus side from the disassembly: boot order, pins, state machine, field maps, capability and fault tables | Reference, with two history sections on the retired Matter firmware |
| [`docs/11-model-capability-map.md`](docs/11-model-capability-map.md) | How the device type and the ProductType reply decide a unit's features, and what two units reported | Reference |
| [`docs/13-stock-fw-feature-map.md`](docs/13-stock-fw-feature-map.md) | The rest of the vendor firmware: cloud channel, image acceptance rules, listeners, Matter stack, flash map | Reference, with one history section |
| [`docs/14-open-questions.md`](docs/14-open-questions.md) | Everything unsettled, with what would settle it | Reference |
| [`docs/04-cloud-and-firewall.md`](docs/04-cloud-and-firewall.md) | The vendor cloud's hosts and ports, and how to block a module on the vendor firmware | Reference for the vendor firmware |
| [`docs/02-matter-local-control.md`](docs/02-matter-local-control.md) | The vendor firmware's own Matter interface and how it was commissioned locally | History: the first route to local control |
| [`docs/05-esp32-replacement.md`](docs/05-esp32-replacement.md) | The community payload tables the work started from, and which of them held | History |
| [`docs/07-firmware-and-ota.md`](docs/07-firmware-and-ota.md) | The vendor firmware's update paths and the first assessment of custom firmware | History, since answered on hardware |
| [`docs/08-cloud-mitm-feasibility.md`](docs/08-cloud-mitm-feasibility.md) | Why impersonating the vendor cloud was not pursued | History: a dead end and its reasons |
| [`esphome/README.md`](esphome/README.md) | A pointer: the ESPHome configuration that once lived here moved to `firmware/esphome/` | Stub |

The numbers 06, 09 and 12 were never pages in this repository's history.

## How a fact is marked

The pages say where each fact comes from: read from the vendor firmware, seen on the bus of a
real unit (with the date), taken from an earlier community project, or inferred. Doc 03 explains
the four labels. Docs 10 and 11 tag claims **[PROVEN]** or **[INFERRED]**: there, [PROVEN] means
the code was read, not that the behaviour was seen on a unit.

Three frames the firmware sends (power on, power off, the status poll) are the bytes of a
community project and were never captured on the owner's bus.
[`../NOTICE.md`](../NOTICE.md#acknowledgements-earlier-work-on-this-ac-bus) has the full credit.

## The notice on retired material

Until 2026-10-12 the project also had two Matter firmwares of its own. Sections that describe
them, their build on Realtek's SDK, their diagnostic console or their tools open with this
notice:

> **Retired.** What the section describes. It was retired on 2026-10-12, and the last tree with
> it is the tag `matter-final`. See `docs/guide/Matter-Firmware-Retired.md`. Where the current
> thing is.

Whole pages that are history say so under "Status of this page" instead.

One thing is easy to mistake for retired and is not: the request bits of the "77" gesture in the
`0x1E` reply. The Matter firmwares used them to open a commissioning window. The ESPHome firmware
uses the same bits to open its Wi-Fi setup hotspot
([doc 03](docs/03-rs485-ac-protocol.md#recommission-77-the-request-bits-in-the-0x1e-reply)).

## Layout

```
docs/      the pages above
hardware/  pinouts, and the QR code of the vendor firmware's Matter test credentials
tools/     capture, decode and disassembly scripts (tools/README.md)
esphome/   a pointer to firmware/esphome/
```

The flash is read with `firmware/flasher/ch341dump.py` (the earlier `tools/dump_flash.sh`, which
used flashrom, was removed on 2026-10-12). The dump itself is not committed: it
holds the vendor's firmware and the Wi-Fi credentials and keys of the module it came from
([`../dumps/README.md`](../dumps/README.md)). Every analysis here is derived from such a dump.

## What was done

- The SoC, the flash and the transceiver identified.
- The flash dumped, two reads identical.
- The vendor firmware found to be a Matter thermostat with test credentials (doc 02).
- The frame envelope and checksum decoded from the firmware (docs 03 and 10).
- The link frames captured from the stock module (2026-07-06), the command values corrected by a
  tap (2026-07-08), the status map built by pressing remote buttons (from 2026-07-07).
- The update mechanism and the image acceptance rules decoded (docs 07 and 13).
- The capability and fault tables decoded, two fault bits confirmed with induced faults (docs 10
  and 11).
- A real device-type reply captured (2026-10-12).

## AI assistance

Parts of this reverse-engineering and documentation were produced with AI assistance.
