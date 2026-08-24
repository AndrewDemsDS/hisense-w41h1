# ESPHome variant

Local Matter-free control of a Hisense A/C over the reverse-engineered RS-485 bus, exposed to
Home Assistant through ESPHome's native API. Same hardware as the esp-matter ESP32 path, same
driver, none of the Matter stack.

**Home Assistant only.** Matter is the path to Apple Home, Google Home and Alexa; this one talks
to Home Assistant and nothing else. That is the whole trade, and it is the first thing to know
before picking this build. Design rationale and the full comparison:
[`../docs/15-esphome-path.md`](../docs/15-esphome-path.md).

## What it exposes

| Entity | Notes |
|---|---|
| `climate` | power, mode (auto/cool/heat/dry/fan_only), setpoint 16 to 32, 7-step fan, swing, current temperature, action |
| `switch` | Eco, Turbo, Quiet, panel display |
| `select` | Sleep profile (Off / General / Old / Young / Kids) |
| `sensor` | indoor, outdoor and coil temperature, compressor Hz, power, voltage, current, bus checksum errors |
| `binary_sensor` | aux heat relay, bus link, aggregate fault, 18 per-bit faults, 13 capability flags |
| `text_sensor` | A/C device type (the learned link bytes) |

Plus whatever ESPHome gives for free: OTA, a captive-portal AP fallback, logs over the API, and
`total_daily_energy` feeding the HA Energy dashboard.

## Wiring

Identical to the esp-matter ESP32 path. Use the pin tables and, especially, the GPIO warnings in
[`../esp32-matter/README.md`](../esp32-matter/README.md): the C3's USB pins, the classic ESP32's
dead PSRAM pins 16/17, the DE pulldown, and the ground-loop rule for bench bring-up. Defaults in
`w41h1.yaml` are the hardware-validated classic-ESP32 set (TX 19, RX 18, DE 4); the C3 SuperMini
set is 5 / 6 / 10.

There is deliberately **no `uart:` block**. The driver's HAL opens the port itself so the DE
timing that took a multi-day debug to find stays exactly as validated. Pins are configured on the
`hisense_ac:` component instead.

## Use it

```bash
pip install esphome                       # or pipx/uv; this repo tested 2026.7.4
cp secrets.yaml.example secrets.yaml      # fill in your Wi-Fi
esphome run w41h1.yaml                    # build + flash + follow logs
esphome logs w41h1.yaml                   # just the logs
```

On a factory-fresh board, erase first (`esphome run w41h1.yaml --device <port>` after an
`esptool.py erase_flash`). A stale vendor Wi-Fi config left in NVS is a documented time sink on
these boards.

## Bring-up, staged

Same three stages as the esp-matter path, and for the same reason: never leave the A/C in an
unknown state.

1. **Bench, no A/C.** Run the host tests (`../test/run_tests.sh`, which includes the
   ESPHome mapping test), then drive the firmware against `../test/virtual_ac.py` over a USB-TTL
   adapter. Watch `AC bus link` go on and the climate entity populate.
2. **Real bus, USB-powered.** Tap **A/B only** (see the ground-loop warning) and confirm decoded
   status: indoor temperature, mode, compressor Hz. This proves the read direction.
3. **Full integration.** Power from the connector's 5 V and close it up.

## Capability gating is a YAML decision

The Matter builds hide eco/quiet/display at runtime when the A/C's ProductType reply says the
unit lacks them, because a commissioned Matter node's endpoint list is fixed. Here you do not
declare the entities your unit does not have. The `capability_*` binary sensors report what
your A/C answers, so declare everything once, look at them, then trim the YAML.

## Shared code

Nothing in this directory reimplements the protocol. `firmware/src/rs485-driver/` and the
ESP-IDF HAL under `../esp32-matter/components/` are registered as local IDF components and
compiled in place, so there is exactly one copy of every shared file in the repo and no sync step
to forget. The only ESPHome-specific mapping is `esphome_aircon_map.h` (enum translation only),
covered by `../test/test_esphome_map.cpp`.

Note that ESPHome forwards only `-D` and `-W` compiler flags on the ESP-IDF framework, so an
`-I` build flag cannot be used to reach shared headers. Registering real IDF components is what
makes the driver's `<platform_stdlib.h>` resolve.
