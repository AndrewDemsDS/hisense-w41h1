# ESPHome config: moved

The ready-to-flash ESPHome config for an ESP32 replacement now lives in
[`../../firmware/esphome/`](../../firmware/esphome/), beside the component it loads.

`w41h1-esp32.yaml` used to sit here and pulled the third-party
[`pslawinski/esphome_airconintl`](https://github.com/pslawinski/esphome_airconintl) component in
over `external_components`. That was a reasonable starting point when the protocol work was young.
It is the wrong thing to hand a user now, for two reasons:

- **Its byte map was never validated for this unit.** The frame envelope is firmware-confirmed, but
  the byte-per-field payload layout that component assumes appears in no stock builder or parser
  (see the provenance correction in
  [`../docs/10-stock-fw-init-and-comms.md`](../docs/10-stock-fw-init-and-comms.md), and the
  bus-confirmed status map in
  [`../docs/03-rs485-ac-protocol.md`](../docs/03-rs485-ac-protocol.md), which found several
  community offsets wrong here).
- **This repo has its own component now.** `hisense_ac` runs the same sniff-validated codec as the
  two Matter firmwares (`firmware/src/rs485-driver/`), and exposes the eco / quiet / turbo / sleep
  controls and the per-bit diagnostics the old config never reached.

Design rationale and status:
[`../../firmware/docs/15-esphome-path.md`](../../firmware/docs/15-esphome-path.md).
