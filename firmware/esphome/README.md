# ESPHome variant

Local, Matter-free control of a Hisense A/C over the reverse-engineered RS-485 bus, exposed to
Home Assistant through ESPHome's native API. Same ESP32 hardware and the same driver as the
esp-matter build, none of the Matter stack. **Home Assistant only.**

The user guide covers everything a builder needs, so it is not repeated here:

- entities, hardware, flashing (both boards), staged bring-up, capability gating and status:
  [`docs/guide/ESPHome-Build.md`](../../docs/guide/ESPHome-Build.md)
- wiring and the GPIO / ground-loop warnings:
  [`docs/guide/ESP32-Replacement-Build.md`](../../docs/guide/ESP32-Replacement-Build.md)
- the bench against `virtual_ac.py`, and what is hardware-verified:
  [`docs/guide/Build-Flash-Test.md`](../../docs/guide/Build-Flash-Test.md)
- design rationale and the phase log: [`../docs/15-esphome-path.md`](../docs/15-esphome-path.md)

## Quick use

```bash
cp secrets.yaml.example secrets.yaml      # Wi-Fi + API key; the real file is gitignored
esphome run w41h1.yaml                    # classic ESP32 defaults: build, flash, follow logs
esphome -s board esp32-c3-devkitm-1 -s tx_pin 5 -s rx_pin 6 -s de_pin 10 run w41h1.yaml   # C3
```

Or `firmware/scripts/dev.py flash esphome --board c3 --port <port>` from the repo root.

## Layout

| Path | What |
|---|---|
| `w41h1.yaml` | reference config; board and pins are substitutions |
| `components/hisense_ac/` | the custom component: hub, climate, switches, select, sensors |
| `tests/hisense_ac/` | every option of every platform, in the layout of ESPHome's own `tests/components/<name>/` |
| `tests/build.*.yaml` | repo-only harness so `esphome config` and `esphome compile` can run those tests (CI runs `config`) |
| `secrets.yaml.example` | template for the gitignored `secrets.yaml` |

The component code follows ESPHome's upstream standards (their `.clang-format`, ruff format, and
the `script/ci-custom.py` rules), so it can be proposed upstream without a style pass. The shared
driver under `firmware/src/rs485-driver/` does not, and is not meant to: it also has to build on
AmebaZ2.

The component registers `firmware/src/rs485-driver/` and `../esp32-matter/components/hisense_hal`
as local IDF components, so the driver compiles in place with no copy. There is deliberately no
`uart:` block: the HAL opens the port itself to keep the validated DE timing. ESPHome forwards only
`-D` and `-W` flags on the ESP-IDF framework, so an `-I` flag cannot reach the shared headers;
registering real IDF components is what makes `<platform_stdlib.h>` resolve.
