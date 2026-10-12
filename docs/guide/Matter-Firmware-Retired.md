# Matter Firmware (Retired)

This project used to ship two Matter firmwares next to the ESPHome one. Both were retired on
2026-10-12 (issue #182). `main` now carries the ESPHome firmware only. The last tree that has the
Matter builds is the signed tag `matter-final`.

## What was retired

| Firmware | Hardware | Last version |
|---|---|---|
| AmebaZ2 Matter build (`room_air_conditioner` on Realtek's SDK and connectedhomeip) | the stock AEH-W41H1 module | 1.3.50 |
| ESP32 Matter build (esp-matter) | the ESP32 replacement board | 1.1.19 |

With them went the SDK setup scripts and patches, the ZAP data model, the Matter half of
`dev.py` (build, package, stage, flash, release, revert to the vendor firmware), the release
workflows, the SDK pin automation, and the `hisense-unified-ac` Home Assistant integration as a
submodule. That integration has its own repository and still works with a node that runs a Matter
build.

## Why

- The builds used Matter test credentials and were never certified, so in practice they were only
  used from Home Assistant, which ESPHome serves directly.
- Every control had to be a standard cluster, so each extra (the beeper, the panel display)
  became another On/Off endpoint, and the diagnostics sat on a manufacturer cluster that needed a
  second integration to show.
- The AmebaZ2 build needed about 15 GB of Realtek and connectedhomeip sources, patches and edits to
  files that cannot be redistributed.
- Three firmwares on one protocol meant every fix was made and tested three times. Command
  confirm and retry only ever existed in the ESPHome build.

The cost: no direct pairing with Apple Home, Google Home or Alexa. Home Assistant can expose the
climate entity to them through its own Matter or HomeKit bridge.

## If you run one of the Matter builds

Nothing stops working. The firmware on your unit keeps running, and the tag has everything to
build it again:

```
git checkout matter-final
```

The documentation at that tag describes the Matter builds (commissioning, OTA, the ZAP recipe, the
way back to the vendor firmware). No fixes will be made there.

To move a unit to ESPHome without opening it:

- an ESP32 board: [Migrating an ESP32 Node from Matter to ESPHome](Migrating-ESP32-Matter-to-ESPHome)
- a stock module: [Converting a Stock Module from Matter to ESPHome](Converting-a-Stock-Module-to-ESPHome)

Both procedures use tools that still live on `main` (for the stock module,
`firmware/scripts/matter-convert.py` with its `convert`, `slots` and `backup` commands). Take the image a unit needs for the way back (its delta base, its clip
image) from the tag or from your own archive before you start.

## What was kept from the Matter work

The knowledge that does not depend on Matter stayed, in documents that describe the hardware and
the protocol:

| Fact | Where |
|---|---|
| The RS-485 protocol, every byte map, the "77" request and display bits | `reverse-engineering/docs/03-rs485-ac-protocol.md`, [Protocol Overview](Protocol-Overview) |
| DE timing and bring-up order on the stock module | `reverse-engineering/docs/10-stock-fw-init-and-comms.md` |
| The stock module's flash layout, slot selection by FWHS serial, image signing, the QE-bit triage | `firmware/docs/10-stock-module-boot-and-recovery.md` |
| Clip flashing and recovery | [Recovery & Reflash](Recovery-and-Reflash) |
| ESP32 board wiring, the GPIO warnings, the stale-NVS trap | [ESP32 Replacement Board](ESP32-Replacement-Build), `firmware/docs/16-esp32-board-notes.md` |
| The old bus driver, hardware-validated on both Matter builds | `firmware/test/reference-driver/`, kept as the reference the ESPHome codec is tested against |
| The power estimate and its calibration | `firmware/docs/09-energy-monitoring.md` |
