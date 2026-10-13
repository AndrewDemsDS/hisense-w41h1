# 01: How the components are built

For someone who is about to change the code under `firmware/esphome/components/`. It says which
file does what, which files may know about ESPHome, where a new decision has to go and which test
covers each part. Configuration options are in
[`firmware/esphome/README.md`](../esphome/README.md), and the frame format is in
[`reverse-engineering/docs/03-rs485-ac-protocol.md`](../../reverse-engineering/docs/03-rs485-ac-protocol.md).

## The three components

| Component | What it is | Runs on |
|---|---|---|
| `hisense_ac` | the A/C bus: codec, bus master, command tracking, the hub and the entities | every platform ESPHome has a `uart:` for |
| `w41h1_module` | what the stock AEH-W41H1 module needs on top: its flash layout and the two checks on it, the reboot workaround, the serial port restart | `rtl87xx` (RTL8720C family) only |
| `wifi_setup_mode` | the node's hotspot and captive portal on demand | every platform with `wifi:` |

`hisense_ac` depends on nothing but ESPHome's `uart`. The other two depend on it, and it builds
and works without them.

## The layers of `hisense_ac`

```
                    needs nothing but <cstdint>: compiled and tested on a host
  ----------------------------------------------------------------------------------------
  hisense_protocol.{h,cpp}   frame constants, builders, parsers, the frame assembler,
        |                    the debounce of the remote's setup request
        |
        +-- hisense_bus.{h,cpp}     BusScheduler: one transaction at a time, DE timing,
        |                           the reply window, re-sends of an unanswered frame,
        |                           link up and link lost
        |
        +-- hisense_mapping.h       climate values <-> bus values, the fan ladder,
              |                     the setpoint, the rules that keep the command shadow
              |                     in step with the unit
              +-- hisense_special.h     eco, turbo, quiet, sleep: the preset table,
                    |                   the ordered writes between two states, the queue
                    +-- hisense_confirm.h   what a command asks, what the unit can be
                                            expected to show, the verdict on a status

  hisense_telemetry.h        power estimate, when a sensor is published,
                             the outdoor temperature placeholder (includes nothing)

  hisense_map.h              includes the six above and declares nothing

  hisense_commands.h         CommandTracker: the command shadow, the pending command,
                             the special-mode queue, hold-off, counters.
                             Frames leave through the CommandSink interface
  hisense_entities.h         the climate request as a plan, a status frame as what the
                             climate entity, the switches and the select show

  ----------------------------------------------------------------------------------------
                    needs ESPHome: runs on a board or in the host end-to-end test
  hisense_ac.{h,cpp}         the hub. Implements BusIO (the UART and the DE pin),
                             BusListener (results of the bus) and CommandSink (frames out)
  hisense_climate.*  hisense_switch.*  hisense_select.*     the entity classes
  __init__.py  climate.py  sensor.py  binary_sensor.py  switch.py  select.py
  text_sensor.py                                            schemas and code generation
```

The arrows are `#include` lines and nothing else: a file sees only what is above it on its
branch. `hisense_map.h` used to hold the mapping, the presets, the confirm rules and the
telemetry in one file. It is now an include of the four parts, kept so that code and tests that
include it still build.

### What each layer is responsible for

`hisense_protocol` knows bytes. A builder returns a finished frame with its checksum, a parser
returns a struct, and the `FrameAssembler` turns a byte stream into frames (start tag, the LEN
byte, doubled `0xF4`, end tag). It keeps no time and sends nothing. Every byte value traces to a
bus capture, to the vendor module's firmware or to a frame published by an earlier community
project. The three frames that were taken from `pslawinski/esphome_airconintl` (power on, power
off and the status poll) are named in the comment beside each and in
[`NOTICE.md`](../../NOTICE.md#acknowledgements-earlier-work-on-this-ac-bus). An unconfirmed value
is marked `VERIFY`.

`hisense_bus` knows time and order. Every exchange is a transaction, as on the vendor module:
flush the receiver, stamp the frame, raise DE, wait 5 ms, send, hold DE 25 ms past the last byte,
drop DE, listen up to 500 ms for a reply. At boot it sends the device type request (`0x0A`) up to
10 times, 500 ms apart, then the `0x07` frame. Each cycle of about 1 s carries the `0x1E`
heartbeat, the queued commands that fit, and the `0x66` status poll. Five status polls in a row
without a reply mean the link is lost. The ProductType poll (`0x66/40`) goes out on the first
cycle that is heard and on every 60th after. A command frame with no reply is sent again in the
next cycle, ahead of the queue, three sends at most and only while the unit answers its status
poll. The scheduler has no task, no blocking wait, no logger and no global state. It reaches the
wire through `BusIO` and reports through `BusListener`, so a host test drives it against a
simulated unit.

`hisense_mapping`, `hisense_special`, `hisense_confirm` and `hisense_telemetry` are functions
from values to values. They hold the answers to questions such as: which bus mode is this climate
mode, which five writes take the unit from eco with a sleep profile to turbo, is a setpoint that
differs from the request the unit's own behaviour in dry mode or a lost command, is -20 C a
reading or the placeholder.

`hisense_commands` is the one stateful part of the pure layer. `CommandTracker` holds what the
hub knows about commands it has sent or still has to send, and asks the functions above what to
do next. It has no clock (the caller passes the time in) and no bus (frames leave through
`CommandSink`). The hub implements that sink over the scheduler. A host test implements it over a
model of a unit that loses frames, so `firmware/test/test_esphome_confirm.cpp` runs the same
class the firmware runs.

`hisense_entities` is the entity side of the same idea. `climate_plan()` takes a climate request
as plain values and returns what to send and what to refuse. `climate_view_from_status()`,
`switch_state_from_status()` and `sleep_profile_from_status()` return what an entity shows. The
entity classes translate an ESPHome call into those structs and carry the result out.

The hub (`hisense_ac.cpp`) owns the UART, the DE pin, the log and the entity pointers. Its
`loop()` polls the scheduler first, asks ESPHome for a tight loop while DE timing is running, and
publishes a status that the scheduler parked during the poll. It makes no decisions of its own.

## The rule: a decision is a pure function with a test

Anything that chooses between two outcomes belongs in the pure layer, with a host test: is this
status my own echo, should the shadow follow the unit, did this command take, which frame goes
next, may this request be sent before the first status. The hub and the entity classes only move
values between ESPHome and those functions.

The rule comes from a defect that escaped review. On an early build a status readback was written
back as a command, which produced another readback, three to six times per user action. It lived
in a layer that no host test could reach and was found on a unit.

The reason it still holds, now that the hub can be run on a host: a case in the host end-to-end
test costs seconds, because the hub's timers are real (a 1 s bus cycle, a 4 s hold-off, 10 s
between special-mode writes). The same decision as a pure function costs microseconds and can be
tested over its whole input domain.

Two mechanical checks keep the boundary. `run_tests.sh` compiles every pure file with `g++` and
no ESPHome on the include path, so an ESPHome include breaks the build of a test.
`hisense_climate.cpp` holds a `static_assert` for each climate constant that `hisense_mapping.h`
mirrors as a plain integer, so a renumbered enum upstream is a compile error.

## Which test covers which file

| File | Host tests | Also |
|---|---|---|
| `hisense_protocol.*` | `test_esphome_codec_parity.cpp`, `test_esphome_edge_cases.cpp`, `test_esphome_codec_gaps.cpp` | every fuzz harness |
| `hisense_bus.*` | `test_esphome_bus.cpp`, `test_esphome_bus_stress.cpp`, `test_esphome_setup_request.cpp`, `test_esphome_setup_edges.cpp` | `fuzz_bus.cpp` |
| `hisense_mapping.h`, `hisense_special.h`, `hisense_confirm.h`, `hisense_telemetry.h` | `test_esphome_edge_cases.cpp`, `test_esphome_confirm.cpp`, `test_esphome_map_gaps.cpp`, `test_esphome_codec_parity.cpp` | |
| `hisense_commands.h` | `test_esphome_confirm.cpp` (second half) | |
| `hisense_entities.h` | `test_esphome_entities.cpp` | |
| the Python tables (`binary_sensor.py`, `climate.py`) | `test_esphome_tables.py`, `test_esphome_config_matrix.py` | |
| `hisense_ac.*`, the entity classes | none on the pure side | `firmware/test/host_e2e/` |
| `w41h1_module/w41h1_uart_policy.h` | `test_esphome_uart_policy.cpp` | |
| `wifi_setup_mode/setup_mode_logic.h` | `test_esphome_setup_mode.cpp` | |

What each test layer proves and what it cannot is in [`04-qa-strategy.md`](04-qa-strategy.md).

## The codec and the frozen reference driver

`firmware/test/reference-driver/` is the project's first bus driver. It ran on real units inside
the retired Matter firmwares and is frozen. The component carries its own codec because ESPHome
cannot accept a component that depends on files outside its directory, and because the old
driver's contract (a FreeRTOS task, the Realtek HAL) does not fit ESPHome's loop.

`test_esphome_codec_parity.cpp` runs every builder and parser of both over the same inputs (the
full command cartesian product, exhaustive small domains, seeded random frames) and compares the
results byte for byte and field by field. That test is in `run_tests.sh`, so CI fails on a
difference. A protocol fact that both carry is changed on both sides in one commit.

The reference is the side of that comparison that ran on hardware for months. It is not a
specification and not a second firmware: it has no confirm and retry, it sends a command once,
and it does not know the facts that were learned after it was frozen. What it is and is not is
in [`firmware/test/reference-driver/README.md`](../test/reference-driver/README.md).

## Commands: from a tap in Home Assistant to the wire and back

1. The climate entity turns the call into a `ClimateRequest` and asks `climate_plan()`. The plan
   says which fields change, whether the request is a power-off, a preset, or refused (for
   example a fan speed while turbo holds the fan, or anything but "off" before the first status).
2. The hub hands it to the tracker (`send_user_command`, `send_user_power_off`,
   `request_preset`, `enqueue_special`). The tracker updates the command shadow, builds the frame
   through the sink and remembers the request as a `PendingCommand`.
3. Power, mode, setpoint, fan and swing ride one combined `0x65` frame built from the whole
   shadow. Eco and turbo share byte 33 of that frame. Quiet and the sleep profile are frames of
   their own. Special-mode writes go through a queue that sends one every 10 s
   (`SPECIAL_SETTLE_MS`), because the unit ignores one that arrives within about 8 s of the
   previous.
4. For 4 s after a user command (`CONFIRM_SETTLE_MS`, 3.9 s) a status that still shows the old
   state does not move the entity. That is the hold-off.
5. The first status after the hold-off is judged by `confirm_decision()`: taken, sent again
   (twice at most for the main fields, once for special modes), changed from elsewhere (someone
   used the remote, the command is dropped), or given up with a warning and a count in
   `failed_commands`.
6. A newer command replaces the pending one. A lost link drops everything that is open.

`intent_expected_fields()` is where the unit's own behaviour is written down, so it is not taken
for a lost command: the unit keeps its own setpoint in auto, dry and fan-only, turbo forces cool
at 16 C on high fan, quiet and the sleep profiles hold the fan, and a unit that is off ignores
everything except power.

A mode request to a unit that is off is one frame with the power-on bits in byte 18 (`0x5C` cool,
`0x3C` heat), as the stock module sends it. If the unit is still off at the check, the re-send is
the older pair of frames, power on and then the mode (`resend_frames()`).

Confirm and retry exists because of a measurement. Before it, hardware runs on two units failed
about one check in 52, each time with `unanswered_commands` at exactly 1: the unit missed one
frame and nothing sent it again. The stock module verifies the reply to every command and packs
power and mode into one frame (`reverse-engineering/docs/10-stock-fw-init-and-comms.md`,
sections 4.6 and 5b-2).

### Special modes and presets

The interlocks between eco, turbo, quiet and sleep were measured on a live A/C (by the Home
Assistant wrapper the retired Matter builds used) and are written down in `hisense_special.h`:

- which combinations exist: eco pairs with quiet or with any sleep profile, quiet never with
  sleep, turbo with nothing (the `PRESETS` table);
- how a state in between is named: the exact row, else turbo, eco, quiet, sleep
  (`preset_detect()`);
- the order of the writes: a sleep clear first, then the byte 33 and mute clears, then the sets,
  and a sleep profile last, sent again after any byte 33 write because eco after sleep drops the
  profile (`special_plan()`, `preset_plan()`);
- a fan speed is refused while turbo, quiet or sleep holds the fan (`fan_request_allowed()`).

The climate entity keeps showing the requested preset until the queue has drained and the
hold-off has passed, so a climate group that mirrors it never copies a state in between. Every
preset reading back as itself from each starting preset has not been checked on a unit.

### The reply to a command

The reply to a command is read and classified (`command_reply_verdict()` in `hisense_bus.h`:
echo of the first two payload bytes, then `1`). The layout comes from the vendor firmware and is
marked `VERIFY`. One stock module logged `Command answered (class 0x65, echo, ack)` on
2026-10-10, which agrees with it. The verdict is logged at `DEBUG` and nothing acts on it: the
status check is what decides.

What an operator sees of all this, and the three counters, is in
[`docs/guide/Entities-and-Diagnostics.md`](../../docs/guide/Entities-and-Diagnostics.md).

## Telemetry rules

Sensor values are published when they change and at least once a minute
(`telemetry_publish_due()`, `TELEMETRY_REFRESH_MS`).

Power is `4.15 W * raw^2` of status byte 55, with a power factor of 0.95 folded in. The
calibration is in [`09-energy-monitoring.md`](09-energy-monitoring.md).

**The outdoor and coil placeholder.** Right after mains power returns, the unit fills status
bytes 44 and 45 with `0xEC`, which decodes to -20 C, until it has real readings. Seen on one unit
on 2026-10-10: both sensors at exactly -20, then 35 and 26 C the same afternoon. The component
publishes that pair as unknown (`outdoor_temps_measured()` in `hisense_telemetry.h`, marked
`VERIFY` because it rests on one observation). The rule is narrow on purpose, since -20 C is also
a temperature: both bytes must be `0xEC` in the same frame, and only until a real pair has been
seen since the bus link came up. After that, -20 is published as a reading. What it still hides
is a unit at rest in exactly -20 C weather, from the moment the node starts until either reading
moves. How long the placeholder lasts, and whether it can return while the unit stays powered,
was not measured.

## Portability

The component uses ESPHome's own API and nothing below it: `uart::UARTDevice` for the bytes, a
`GPIOPin` for DE, the loop time for the clock, ESPHome's log macros, and ESPHome's preferences
for the beeper switch. It calls neither ESP-IDF nor FreeRTOS, and its format strings do not
assume an integer width. It compiles without a warning of its own for ESP32, ESP32-C3, ESP8266,
RP2040 and the RTL8710C.

Two things differ between platforms. Both are covered in `test_esphome_bus.cpp`.

- **A UART write that blocks.** ESP-IDF queues the bytes and returns. LibreTiny waits on the
  transmit FIFO, so a 50 byte command frame holds the loop for up to about 52 ms, the frame's
  time on the wire. The DE release is counted from before the write, so it falls 25 ms after the
  last byte either way.
- **A coarse loop.** The scheduler stays correct when it is polled only every 16 ms. DE is then
  released up to one loop period late, which is why the hub asks for a tight loop while DE is
  high. How short the delay is on a platform other than ESP32 and the RTL8710C has to be measured
  there.

Every deadline is an elapsed-time or signed-difference comparison on a 32 bit millisecond clock,
tested through the 49.7 day wrap.

One platform fix lives in the hub. On LibreTiny, `HisenseAC::bus_read()` reads each byte under
ESPHome's `InterruptLock`, because LibreTiny's receive buffer loses count of a byte when its
interrupt lands inside a read. The measurements are in
[`15-esphome-path.md`](15-esphome-path.md#the-receiver-stall).

## `w41h1_module`

| File | Does |
|---|---|
| `__init__.py` | copies the named `layout-*.json` into the build directory and hands it to PlatformIO as `custom_board`, defines `W41H1_LAYOUT_<name>`, adds the two linker flags of the reboot workaround |
| `layout-factory.json`, `layout-sdk.json`, `layout-native.json` | the slot and settings addresses of each flash layout |
| `w41h1_slots.h` | fails the build when the image's addresses are not those of the named layout, and holds the run-time comparison with the bootloader's partition table |
| `w41h1_uart_policy.h` | when the bus port may be restarted. Pure, host-tested |
| `w41h1_module.{h,cpp}` | runs the layout check at boot, switches the update server off on a mismatch, looks at the hub's link state once a second and restarts the port when the policy says so, publishes the two diagnostics every `update_interval` |

Why each exists, with what was measured on a module, is in
[`15-esphome-path.md`](15-esphome-path.md).

## `wifi_setup_mode`

The component is split the way the hub is. Every decision is a pure function in
`setup_mode_logic.h`: a state machine `step(state, event, inputs, config)`, the rule for which
credentials are "the previous ones", the rule for a journal found at boot, and what a status LED
shows. `wifi_setup_mode.cpp` does the calls that the returned actions name.
`firmware/test/test_esphome_setup_mode.cpp` runs every state against every event and every input,
and every deadline across the wrap of the millisecond clock. `packages/node.yaml` holds the
wiring only. How the mode behaves for a user is in
[`firmware/esphome/README.md`](../esphome/README.md#wi-fi-setup-mode-and-the-77-gesture).

> **Setup mode has not run on a unit.** The mode and the gesture have run in host tests and in
> compiles only.

The design decisions:

- **The request on the bus.** The gesture is a bit in byte 17 of the unit's reply to the `0x1E`
  heartbeat. `0x20` alone is taken from one reply (the bench unit sends one per gesture).
  Anything with `0x08` needs three replies in a row. `0x20` is ignored for 10 s after a request
  and after the indication goes off (#69). The component is stricter than the reference driver in
  four places, because here the request is a toggle that takes the node off the network. The
  reply's checksum must be good. `0x08` is ignored while the node's own provisioning flag (the
  same bit, outbound) is on the wire, and afterwards until one reply comes without it. `0x08`
  already set at boot is ignored, or a unit that keeps asking would reopen setup after every
  restart. The lockout also starts at boot and is kept as elapsed time, so it cannot come back
  to life when the clock wraps. Tests: `test_esphome_setup_request.cpp` and
  `test_esphome_setup_edges.cpp`.
- **The hub** parks a request during the bus poll and runs the automations from `loop()`.
  `set_setup_indicator()` only changes what the next heartbeat carries.
- **Opening the hotspot on demand.** ESPHome 2026.7 has no call for it: `setup_ap_config_()` is
  protected, and `loop()` opens the hotspot only when no network has been connected for
  `ap_timeout`. Setup mode gets there with public calls only: `disable()`, `set_ap_timeout(1)`,
  `enable()`, then `set_sta()` with a network that does not exist, all in memory. That is the
  state ESPHome's fallback is written for, so the portal, its scan and the retry pacing are stock
  behaviour. `disable()`, `set_sta()`, `enable()` is also the sequence ESPHome's own
  `wifi.configure` action uses. It only holds when Wi-Fi scans before it connects. With
  `fast_connect`, or a network marked hidden, `start()` connects at once, before the placeholder
  is in place. The code generation reads the `wifi:` configuration, warns, and the node then
  refuses the request with a log line and stays on its network.
- **Two designs that were turned down.** Persisting a flag and clearing the station list before
  Wi-Fi starts makes the portal save under a preference key the next normal boot does not read
  (`start()` derives the key from `has_sta()`). Reaching into the Wi-Fi component for the
  protected call would break on an update of ESPHome.
- **Leaving is a restart,** by timeout, by a second gesture, or once a network entered in the
  portal has connected. The Wi-Fi component never clears its "hotspot was set up" flag, so
  within one boot the hotspot opens once. A restart also restores the station list and the
  timeout from the build and from flash, with nothing to undo by hand. The exception is a request
  made while the fallback hotspot is already open: nothing was parked, so there is nothing a
  restart would restore. That case only lights "77" and ends without one.
- **What cannot strand a node.** The portal writes what the user enters to ESPHome's own saved
  Wi-Fi settings. On the way in, the component writes the SSID and password to go back to into a
  journal (a preference of its own, with the build's configuration hash). On every way out it
  clears the journal, after writing those credentials back if the portal's entry has not
  connected: at the timeout, on a second gesture, and from `on_shutdown()`, which ESPHome runs
  for `App.reboot()` and `App.safe_reboot()` alike, so also for the API component's restart
  after 15 minutes without a client. A power cut runs no hook. Then the journal is still there
  at the next boot, and `setup()`, which runs after Wi-Fi has started and loaded its saved
  network, writes the previous one back when the two differ. If the hotspot does not open within
  30 s the node restarts.
- **Which credentials are the previous ones.** The station's current entry cannot say:
  `get_sta()` is empty while Wi-Fi scans. The component remembers the network at each connection
  and the one loaded from flash at boot, and falls back to the first network of the build, which
  the code generation hands it.
- **A network entered late.** A connection attempt takes up to 46 s and the pause before the
  next one, with the hotspot up, is 30 s. So an entry moves the deadline to at least 90 s away.
- **The clocks.** Deadlines are compared as a signed difference, the timeout is limited to 24 h,
  and the look at Wi-Fi runs four times a second from `loop()`.
- **The status LED.** `status_display()` decides what a LED shows, for both LED packages, so the
  two cannot disagree.

Still to be seen on a unit: which bit each unit raises, that "77" lights and clears, that the
hotspot opens on ESP-IDF and on LibreTiny after `disable()` and `enable()`, that the journal
survives a power cut on each chip, and that a restart on the stock module out of setup mode comes
back (LibreTiny issue 396).

## Code style

C++17 in the component, with ESPHome's `.clang-format` and `.clang-tidy`. Every protocol byte value has a comment that traces it to a capture, to the vendor firmware or to
its published source. `firmware/scripts/cpp-lint.sh check` is the gate
([`firmware/scripts/README.md`](../scripts/README.md)). `hisense_ac` also passes ESPHome's own
gates on the pinned release (`esphome-upstream-check.sh`), because it is meant to be submitted
upstream one day. `w41h1_module` and `wifi_setup_mode` are held to the C++ rules only.
