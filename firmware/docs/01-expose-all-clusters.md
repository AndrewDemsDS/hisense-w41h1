# Exposing the full A/C over Matter: wiring plan

> Grounded in the SDK + the hardware-validated driver. Base example is **`room_air_conditioner`
> (`make aircon_port`)**, NOT the bare thermostat. The RS-485 driver
> (`firmware/src/rs485-driver/hisense_rs485.{h,cpp}`) is now fully sniff-validated
> against real W41H1 frames, both command (TX) and status (RX), so these phases
> carry **no protocol risk**, only Matter-glue plumbing.

## Real SDK files to touch (under `sdk/ameba-rtos-z2/.../application/matter/`)

- `examples/room_air_conditioner/matter_drivers.cpp`: the glue:
  - `matter_driver_uplink_update_handler(AppEvent*)`: Matter attribute WRITE → dispatch
    on `path.mClusterId`. The `Clusters::Thermostat::Id` case is **empty**: our main hook.
    `FanControl` + `OnOff` cases already exist (extend them).
  - `matter_driver_downlink_update_handler(AppEvent*)`: device state → Matter attribute
    store, via `AppEvent::kEventType_Downlink_*_SetValue`. Add Thermostat/Fan event types.
  - `matter_driver_room_aircon_init()`: currently wires a DHT11 + PWM fan. Replace with
    `hisense_init(hisense_on_status)` + a `hisense_poll_task` (see rs485 `INTEGRATION.md`).
- `drivers/device/room_aircon_driver.{h,cpp}`: `MatterRoomAirCon::{Thermostat,FanControl}`
  are PWM/in-memory stubs; the setters stay, their I/O becomes RS-485.
- `examples/room_air_conditioner/room-air-conditioner-app.zap`: cluster/attribute config.
  FanControl already has `SpeedSetting/SpeedMax/RockSetting/AirflowDirection` attributes;
  just enable the FeatureMap bits. Regenerate with the ZAP tool after editing.

## Field mapping (all encodings hardware-confirmed: see rs485 INTEGRATION.md)

| Matter | ↔ | driver | notes |
|---|---|---|---|
| Thermostat `SystemMode` | ↔ | `HisenseCommand.mode` / `HisenseState.mode` | Off→power frame; Cool3/Heat4/FanOnly7/Dry8/Auto1. **AUTO cmd byte18=0x90 (idx4), status nibble 5** |
| Thermostat `OccupiedCooling/HeatingSetpoint` | → | `.setpoint` | int16 ÷100 → whole °C, 16–32 |
| Thermostat `LocalTemperature` | ← | `.indoor_temp_c` ×100 | direct °C from status @20 |
| FanControl `SpeedSetting` (SpeedMax=6) | ↔ | fan | 6 speeds: idx 1,5,6,7,8,9 → cmd 0x03/0B/0D/0F/11/13 |
| FanControl `RockSetting` | ↔ | `.vswing`/`.hswing` | bit1=up/down (cmd byte32 0xC0), bit0=left/right (⚠ cmd untested) |
| OnOff | → | `hisense_build_power_frame` | on[]/off[] literals |
| **manuf cluster 0xFC00** | ↔ | feature/sleep/flags | see below |

## Manufacturer cluster (Phase 3): chosen approach

Cluster id `(VID << 16) | 0xFC00` (VID from the attestation certs):
- `0x0000 Eco` bool → `feature=ECO` (cmd byte33 **0x30**), status flags1 bit2
- `0x0001 Turbo` bool → `feature=TURBO` (cmd byte33 **0x0C**), status flags1 bit1
- `0x0002 Mute` bool → cmd byte35 **0x30/0x10**, status flags2 bit2
- `0x0003 SleepProfile` enum8 0-4 → cmd byte17 **profile*2+1**, status @17 profile*2
- `0x0010 CompressorHz` uint8 RO ← status @42
- `0x0011 OutdoorTemp` int8 RO ← status @44

## Phases (each a go/no-go gate)

0. **Toolchain + attestation gate.** Build STOCK `aircon_port`+`is_matter`, flash via CH341A
   SPI clip, commission in HA, REQUIRES the DAC/PAI/CD fix (docs/02, the `err 604` defect).
   Nothing proceeds until a stock image commissions.
1. **Core control (no ZAP change):** swap stubs → `hisense_init`+poll; fill Thermostat uplink
   (SystemMode/setpoints), OnOff→power; downlink LocalTemperature/SystemMode/fan% via new
   AppEvents. Gate: mode+temp+on/off from HA.
2. **Fan + swing (ZAP):** FanControl FeatureMap += MultiSpeed(SpeedMax=6)+Rocking+AirflowDir;
   Thermostat Cooling+Heating features (setpoints 16–32). Map SpeedSetting→6 fan idx,
   RockSetting→v/h swing. Gate: 6 speeds + swing from HA.
3. **Exotics:** add the 0xFC00 manufacturer cluster above; wire uplink→HisenseCommand,
   downlink←status flags.
4. **Optional (done except CompressorHz):** outdoor temp now has its own TemperatureMeasurement
   endpoint (ep2); the humidity endpoint was dropped rather than fed (W41H1 status humidity isn't
   mapped). CompressorHz attr remains gapped, see above.
5. **Ship:** rebuild, flash via clip, commission, verify every control end-to-end. Done: multiple
   fielded builds since (`docs/13`).

## Manufacturer-cluster attach mechanics + telemetry gap

The `0xFFF1FC00` manufacturer cluster attaches to **endpoint 1** with **4 live attributes**:
Eco/Turbo/Mute bool + SleepProfile enum. The uplink handler maps attr `0x0000-0x0003` →
`HisenseFeature`/`hisense_build_mute_frame`/`_sleep_frame`; downlink mirrors those status flags
back via raw `emberAfWriteAttribute`. SDK edits captured in `firmware/src/sdk-edits/`.

> ⚠️ **Telemetry gap, partially closed.** CompressorHz (`0x0010`) and OutdoorTemp (`0x0011`) are
> declared in `hisense-aircon-cluster.xml` but were never enabled in the GUI-authored `.zap`, so
> they're absent from the compiled attribute table: the mfg-cluster writes at
> `matter_drivers.cpp:1472-1473` return `UNSUPPORTED_ATTRIBUTE` and are silently dropped.
> **OutdoorTemp shipped anyway**, redirected to a standard `TemperatureMeasurement` endpoint
> (ep2, HA-readable) instead of the mfg attr, exactly as this section originally proposed; see
> `docs/07`'s parity matrix. The unfed Humidity endpoint was dropped rather than fixed, so ep2 no
> longer enumerates Humidity at all. **CompressorHz remains gapped**: the ZAP tick is the
> remaining fix, or drop it too since the mfg attr is unreadable via matter-server either way.

Attaching the cluster takes more than the GUI tick: a *new* manufacturer cluster only attaches from a
tool-authored `.zap` (hand-edits are recognized but not endpoint-counted), AND, because
CHIP only bakes the `Id`/callbacks into `zzz_generated` for clusters it ships, needs a
minimal set of generated-table edits (`ClusterId.h` + include + callback decls + no-op
defs). The clean-but-heavy alternative is `zap_regen_all.py`. Full recipe in
`firmware/src/sdk-edits/README.md`.

> This doc is the sole source of truth for the manufacturer-cluster layout and the
> CompressorHz/OutdoorTemp telemetry gap above, other docs (e.g. `docs/05`) cite it rather
> than restate the byte-level mapping.

## Residuals carried from driver validation (non-blocking)
- **hswing command** untested (this unit's app has no H-swing; remote is IR-only). Status
  read-back works; the command (byte32 bits4-5 + byte37=0x14) is reference-inherited.
- **heat-relay** bit = aux/PTC electric heat; only asserts in cold/defrost.
- Combined-frame `HisenseCommand` writes vs the reference's one-field-per-frame: bench-test
  early (rs485 INTEGRATION.md §6); fall back to per-field frames if the A/C ignores combined.
