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
| `tests/components/hisense_ac/` | every option of every platform, at the path and in the shape ESPHome's own `tests/components/<name>/` uses, so it moves upstream unchanged |
| `tests/test_build_components/` | stand-ins for ESPHome's shared UART test packages, so the relative include in the test resolves here too |
| `tests/build.*.yaml` | repo-only harness so `esphome config` and `esphome compile` can run those tests (CI runs `config`) |
| `secrets.yaml.example` | template for the gitignored `secrets.yaml` |

Every file in the component passes ESPHome's own gates as of their `dev` branch in September 2026:
`script/ci-custom.py`, their `.clang-format` and `.clang-tidy`, ruff format, and pylint.

[`upstream/hisense_ac.mdx`](upstream/hisense_ac.mdx) is the draft of the component's page for
ESPHome's documentation repository (`esphome-docs`, `src/content/docs/components/`), which an
upstream pull request has to be paired with. Keep it in step with the option schemas.

### Log levels

The component logs under the tag `hisense_ac`. Set the level with ESPHome's `logger:` component.

| Level | What it shows |
|---|---|
| `WARN` | the bus link dropping, a command that could not be sent, got no reply, was sent again or was given up on, a status frame with a bad checksum, a fault reported by the unit |
| `INFO` | the bus link coming back, a fault clearing, a command the unit took on a re-send |
| `DEBUG` | each status change, every command sent, the unit's capability bitmap |
| `VERBOSE` | every decoded status frame, every poll that got no reply |
| `VERY_VERBOSE` | the raw bytes of every frame sent and received |

Use `VERY_VERBOSE` when the link is down. It shows whether the A/C sends anything at all, and you
do not need to add a `debug:` block to the UART.

### Confirm and retry

A command used to be one frame, sent once. The component now checks each one twice and sends it
again when the unit did not take it.

| Check | When | On failure |
|---|---|---|
| The unit replied to the frame | within the 500 ms reply window | the frame goes out again in the next bus cycle, ahead of anything queued after it, three sends at most, and only while the unit answers its status poll |
| The unit's status shows what was asked | first status frame 4 s after the frame left the wire | power, mode, setpoint, fan and swing: the frame is sent again, twice at most. Eco, turbo, quiet and sleep: the plan to the wanted state is made again from what the unit reports, once, and waits out the 10 s special-mode pacing |

After the last re-send the component logs a warning, counts the command in `failed_commands` and
lets the entities show what the unit reports. A newer command replaces the one still being checked,
so an older request is never sent again over it. A lost bus link drops every open command.

Some differences are the unit's own behaviour and are not treated as a lost command: it keeps its
own setpoint in auto, dry and fan-only, turbo forces cool at 16 C on high fan, quiet and the sleep
profiles hold the fan, and a unit that is off ignores everything except power. A value that is
neither the old one nor the requested one means someone used the remote, and the command is
dropped. The rules are `intent_expected_fields()` and `confirm_decision()` in `hisense_map.h`,
tested in `firmware/test/test_esphome_confirm.cpp`.

A mode request to a unit that is off is one frame that carries both the mode and power-on, as the
stock module sends it (frame byte 18: `0x5C` cool, `0x3C` heat). If the unit is still off four
seconds later, the re-send is the older pair of frames, power-on and then the mode.

The `unanswered_commands`, `command_retries` and `failed_commands` sensors count all of this since
boot.

### The codec port (`hisense_protocol.*`, `hisense_map.h`)

The shared driver under `firmware/src/rs485-driver/` cannot pass those gates and is not meant to:
it also has to build on AmebaZ2. Upstream ESPHome needs a component that carries its own protocol
code, so the component holds an ESPHome-style port of the codec: frame builders and parsers,
the enum and fan-ladder mapping, presets, and the power estimate. It leaves out only the stock
module's cloud-pairing parts ("77", smart-config, provisioning).

A second copy is only safe because it cannot drift. `firmware/test/test_esphome_codec_parity.cpp`
runs every builder and parser of both over the same inputs (the full command cartesian product,
exhaustive small domains, and seeded random frames) and compares them byte for byte and field by
field. It runs in `run_tests.sh`, so the lint gate and CI fail on any divergence. A protocol fix
lands in the shared driver first, then gets ported here until the parity test passes again.

The port is the only transport: `w41h1.yaml` has a `uart:` block and the bus is scheduled in
`loop()` (`hisense_bus.*`). It passed the hardware-in-loop actuation test on a live A/C (48/49 on
2026-09-25; the one miss, heat_cool readback, is an intermittent A/C-side flake). The earlier
fallback that ran the shared driver's own FreeRTOS bus task was removed: it pulled in files from
outside the component, which upstream ESPHome cannot accept.

## Troubleshooting

### An update uploads, then the node is back on the old firmware

The log of the old firmware shows `OTA rollback detected` and `Last reset was due to brownout`.
The board's 3.3 V supply dips when Wi-Fi starts at full power, so the new image resets on its
first boot and the bootloader rolls back. The node can look healthy all the same: ESP-IDF lowers
the transmit power on the boot that follows a brownout, so the old image comes up fine.

Check the supply and its wiring first. If the board has to run as it is, cap the transmit power
from boot in that node's YAML:

```yaml
esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_ESP_PHY_MAX_WIFI_TX_POWER: "10"

wifi:
  output_power: 8.5dB
```

After any update, confirm that the `compiled on` line in the log shows the new build time.

