# Changelog

Notable changes to this project. The format loosely follows [Keep a Changelog](https://keepachangelog.com/).
Entries up to 2026-10-10 also cover the two Matter firmwares, which are retired: version numbers
such as "AmebaZ2 1.3.50" or "ESP32 1.1.19" and paths under `firmware/src/` and
`firmware/esp32-matter/` refer to the tree at the tag `matter-final`.

From 2.0.0 on, a release of the ESPHome firmware is a `## X.Y.Z - date` section here and the git
tag `vX.Y.Z`. Changes that are merged and not yet released sit under `## Unreleased`
([Releasing](CONTRIBUTING.md#releasing)).

## Unreleased

- "Bus reply timeouts" no longer counts an isolated miss: one heartbeat or status poll met by total
  silence on a live link, with the cycle before and the rest of that cycle answered. A cut or
  damaged reply, or noise, still counts. The mainboard skips one
  status reply on a period of its own (about every two minutes on the units measured on
  2026-10-15) and answers the next poll, which had the counter climbing by hundreds a day on a
  healthy unit. Those are kept in a separate `skipped_replies` counter of the scheduler (no
  entity). A second miss in a row, a miss while the link is down, and an unanswered command all
  count as before.

- The reply window is 1000 ms (was 500). A capture of 475 polls on an ESP32 node put the
  median reply at 230 ms and one at 400 ms; the vendor module waits 3000 ms. A link that goes
  silent now shows as lost after about 12 s instead of 5 (five unanswered polls, two windows each).

- A command with horizontal swing no longer writes `14` at byte 37. The vendor firmware's
  command writer uses byte 37 for 8-degree heat (reverse-engineering doc 09); horizontal swing is
  bits 4 and 5 of byte 32 alone. The units here have no horizontal louvre, so nothing changes for
  them; a unit with 8-degree heat no longer gets a stray value in that field.

- The ProductType poll (`66 40`) is sent once per handshake: on the first cycle the A/C is
  heard, and again after a recovery handshake. It was re-sent every 60 cycles. The stock module
  asks it once per link bring-up and never again on a live link (reverse-engineering docs 10 and
  15, read from the 2026-08-19 dump). `BUS_PRODUCTTYPE_CYCLES` is gone. (The periodic single
  timeout of #186 turned out to be the mainboard skipping a reply, see the entry above.)

## 2.0.1 - 2026-10-13

The same firmware built with ESPHome 2026.9.1. Nothing changes in what a node does. Build
with ESPHome 2026.9.1 or later.

- The project builds with ESPHome 2026.9.1 (was 2026.7.4): the pin in CI, `dev.py` and the
  upstream check, and the install commands in the docs. No change to the components. A node
  built with 2026.7.4 keeps working, and nothing has to be done to it.
- ESPHome 2026.9 refuses an API key of 32 zero bytes, which was the placeholder in
  `secrets.yaml.example`. The placeholder now decodes to the text
  `this-is-a-public-placeholder-key`. It is as public as before: replace it in your own
  `secrets.yaml`.
- The status pixel package gives the colour order as `channel_colors`, the name ESPHome 2026.9
  uses (`rgb_order` is deprecated there). The `status_pixel_order` substitution is unchanged. A
  node that includes this package now needs ESPHome 2026.9 or later to build.

## 2.0.0 - 2026-10-13

The first release of the ESPHome firmware under a version number of its own.
The Matter firmwares were 1.x, under the tags `amebaz2-v*` and `esp32-v*`.

Everything below was merged between 2026-10-12 and 2026-10-13. The first section is what the
owner of a node has to do or will see change. Read it before you rebuild a node.

### Needs action, or behaves differently

- **The Matter firmwares are retired. ESPHome is the only firmware (#182).** Removed: the AmebaZ2
  Matter build (the SDK setup scripts, `patches/`, `versions.env`, the `.zap` and
  `firmware/src/sdk-edits/`, `firmware/src/version.txt` and its CI rule), the ESP32 Matter build
  (`firmware/esp32-matter/`), the Matter release steps of `dev.py` (`dev.py ota ...`), the
  matter-server helper scripts, the release and SDK-pin workflows, the Renovate config and the
  `hisense-unified-ac` submodule. The last tree with all of it is the signed tag `matter-final`.
  A unit that still runs a Matter build keeps working and gets no fixes. The way over to ESPHome
  is `firmware/scripts/matter-convert.py` (see Tooling below).
- **`secrets.yaml` needs one more entry, `wifi_ap_password`** (at least 8 characters). The node's
  hotspot has a password now. A build without it stops with a message naming the missing secret.
- **Configurations that cannot work are refused when they are validated.** A second `hisense_ac`
  climate entity on one hub, `min_temperature` above `max_temperature`, a setpoint range outside
  16 to 32 °C (61 to 90 °F), a `de_pin` in input mode, and `de_pin` together with the UART's
  `flow_control_pin`. Neither of the two gives a warning. A second hub and
  `hisense_ac.set_setup_indicator` without `state` now say what is wrong. A configuration that
  one of these rules refuses did validate before: it has to be corrected to build again.
- **Home Assistant shows another manufacturer and model for the device at its next build.** With
  a project set (see "The firmware has a version" below), Home Assistant takes them from the
  project name, so they read `AndrewDemsDS` and `hisense-w41h1` where they read the chip maker
  and the board. The node also announces the project in its mDNS record. No entity name, object
  id or unique id changes: the entity registration in the generated code is the same line for
  line on all eight board files. As with any change of configuration, a network saved through
  the setup portal is dropped at the update and the node comes up on the Wi-Fi in `secrets.yaml`.
- **Eco or turbo switched before the first status frame after boot is refused.** The write was
  taken, the switch showed on, and the frame was then not sent, because it is built from a
  status the node did not have yet (up to about 16 s on a boot with a silent unit). Now the
  switch stays off, the log says `Command not sent: no status from the unit yet`, and nothing
  counts as a failed command. Quiet and the sleep profile have frames of their own and are sent
  as before. Nothing changes once a status frame has arrived.
- **A command made while the unit is not answering is refused.** It used to go out once into the
  silence and was then carried out if the unit came back within 20 s. Now the entity keeps its
  state and the log says `Command refused: the unit is not answering`. A special-mode write
  whose turn came in the loop pass that reported the link lost is no longer sent either.
- **A command that could not be queued is no longer shown as sent.** The climate entity, the eco,
  turbo and quiet switches and the sleep select keep their state when the hub refuses a request
  (transmit queue full, special-mode queue full), where they showed it for 4 s and then went
  back. The refused values no longer stay in the command shadow, and a special-mode write that
  could not be queued is counted in `failed_commands` and no longer starts the 10 s pacing.
- **`packages/node.yaml` names the node by default** (`name: hisense-ac`, `friendly_name: Air
  Conditioner`), and the board files no longer repeat the two lines. A file that sets them keeps
  its names. A file that includes the package and sets no `name` now builds as `hisense-ac` where
  it was refused before.
- **`dev.py --board c3` builds another node than before.** It compiled `w41h1.yaml` with the C3
  board and pins as `-s` overrides, which is the node of `w41h1-esp32c3.yaml` without its status
  LED. Every `--board` value now names one board file, so `--board c3` builds
  `w41h1-esp32c3.yaml` and the node gains the status LED on GPIO8 (the SuperMini's blue LED: dark
  when healthy, blinking while Wi-Fi, Home Assistant or the A/C bus is down). No entity changes.
  `--board classic` is unchanged. `--board c3-plus` is new and builds and flashes
  `w41h1-esp32c3-plus.yaml`.
- **An implausible power estimate is published as unknown, and the current with it.** The
  estimate is `4.15 W * raw^2` of one status byte, so a wrong byte read as tens of kilowatts. It
  was clamped at 50 kW and published, and `Energy today` summed it. `hisense_ac:` has a new
  option, `max_power` (watts, default `5000W`, 500 W to 50 kW): above it the Power and Current
  sensors show unknown, and the energy sensor adds nothing for that value. 5 kW is generous for a
  single-split residential unit. A larger unit needs `max_power` raised, or its real readings
  above 5 kW go unknown. No configuration has to change otherwise: the option has a default, and
  the 50 kW clamp is gone.
- **The bus counters and the per-bit fault sensors are disabled by default in Home Assistant.**
  The six counters (`checksum_errors`, `reply_timeouts`, `unanswered_commands`,
  `command_retries`, `failed_commands`, `link_losses`) and the 18 `fault_*` binary sensors have
  `disabled_by_default: true` as their schema default. This affects new installs only: Home
  Assistant reads the flag when it first registers an entity, so a node it already knows keeps
  all of them enabled. On a new node, enable the ones you want on the device page, or set
  `disabled_by_default: false` on the entity in the YAML. The summary `Fault` sensor, `AC bus
  link` and the capability sensors are enabled as before, and the node publishes every entity
  either way.

### Added

- **The firmware has a version.** It is written once, in
  `firmware/esphome/packages/version.yaml`: semver, starting at 2.0.0. `packages/node.yaml`
  includes that file, so every node built from the packages carries it as ESPHome's `project:`
  without a change to its own YAML. Home Assistant shows it as the device's firmware, "2.0.0
  (ESPHome 2026.7.4)", and the ESPHome dashboard shows it beside the node.
- **A release is the tag `vX.Y.Z`.** Pushing it runs `.github/workflows/esphome-release.yaml`:
  the tag has to equal the version and sit on `main`, the host tests run, every board file
  compiles with the components free of warnings, and the GitHub release gets this file's section
  for that version as its text. No firmware image is attached: one built in CI holds the
  placeholder secrets, which are public. The workflow also has a dry run
  (`gh workflow run esphome-release.yaml`): every gate and compile, no tag and no release. Its
  board matrix comes from the board files. Steps: `CONTRIBUTING.md`, "Releasing".
- **The packages work from git.** `packages/hisense-ac.yaml` and `packages/amebaz2.yaml` take the
  source of the components from a substitution, `w41h1_components`, whose default is the local
  `components/` directory as before. A configuration outside this repository can now include the
  packages as ESPHome remote packages pinned to a release tag and point that substitution at the
  same tag (`firmware/esphome/README.md`, "Using a release from your own configuration").
- **Wi-Fi setup mode on the remote's "77" gesture (#189).** Horizontal airflow pressed six times
  on the remote puts "77" on the panel and opens the node's hotspot, `<name>-setup`, with
  ESPHome's captive portal to enter a new network. A `Start Wi-Fi setup` button in Home Assistant
  does the same. It ends in a restart: when the new network has connected, when the gesture is
  repeated, or after `wifi_setup_timeout` (10 minutes, to be set between 30 s and 24 h). Not run
  on hardware yet. In detail:
  - Nothing is written to flash on the way in. A network entered in the portal that never
    connected is always undone: at the end of the mode, when the node restarts for another
    reason, and at the next boot after a power cut. For that the previous SSID and password sit
    in a preference while the mode is on, zeroed when it ends.
  - A network entered just before the timeout gets 90 s to connect. A gesture while the fallback
    hotspot is already open lights "77" and does not restart the node.
  - With `fast_connect`, a hidden network or an enterprise login the hotspot cannot be opened on
    demand: the build warns, and the node logs why and stays on its network.
  - It is the `wifi_setup_mode` component. Its decisions are a pure state machine with host
    tests, and `packages/node.yaml` only wires it. The log tag is `wifi_setup_mode`. The ids
    `wifi_setup_active`, `wifi_setup_enter` and `wifi_setup_leave`, which the first version of
    2026-10-12 had as YAML, are gone.
- **The hub has the trigger `on_setup_request` and the action `hisense_ac.set_setup_indicator`**,
  and sets its warning status while the bus link is down.
- **Optional status LED**, dark while the node is healthy: `packages/status-led.yaml` for a plain
  LED on a GPIO and `packages/status-pixel.yaml` for one WS2812 pixel on an ESP32.
  `w41h1-esp32c3.yaml` now includes the first (the SuperMini's blue LED on GPIO8). The new
  `w41h1-esp32c3-plus.yaml` is the same node for the SuperMini Plus, which has an RGB pixel on
  that pin. The pixel has not been checked on a board. The LED and the pixel show setup mode from
  the request to the restart, by one shared rule.
- **The stock module's glue is a component, `w41h1_module` (#188).** What the stock module needs
  on top of a plain `rtl87xx` node moved out of YAML lambdas and a header into
  `firmware/esphome/components/w41h1_module/`: the three flash layouts and their build-time and
  boot-time checks, the reboot workaround for LibreTiny issue 396, the serial port recovery for
  the A/C bus, and the `Flash layout` and `Serial port restarts` diagnostics.
  `packages/amebaz2.yaml` is configuration only. The entities and their object ids are the same.
  The component validates only on `rtl87xx` with the RTL8710C family and has its own
  upstream-shaped test under `tests/components/w41h1_module/`. It uses nothing outside its
  directory, so it also works from a git source. Listing it twice is refused with a message that
  says so, where ESPHome's own said "expected a dictionary".
- **`NOTICE.md` credits `pslawinski/esphome_airconintl` and `deiger/AirCon`.** It says what came
  from each and lists the three literal frames (power on, power off, the status poll) that are
  the same bytes as in `esphome_airconintl`. The READMEs and the comments at those frames point
  to it.

### Fixed

- **A healthy unit is no longer logged as reporting a fault.** The log tested the fault bitmap,
  whose valid bit is always set, so every boot logged `A/C reports a fault: bitmap 0x80000000`
  at WARN on the first status frame and a fault that cleared logged the same line. Now the WARN
  line appears for a real fault only, with its bitmap, and a fault that clears logs
  `A/C fault cleared`. The fault entities were right all along.
- **Fahrenheit panels: a setpoint chosen on the remote is no longer moved by one degree.** The
  command shadow takes the status frame's own setpoint byte. It used to go through whole Celsius
  and back, which changed 13 of the 30 values (62 F became 63), and the next fan, swing or mode
  change from Home Assistant sent the changed value. Celsius panels are unaffected.
- **A preset chosen while the unit reports eco and turbo together clears both.** A plan to a
  preset with neither sent only the eco clear and left turbo on. It now sends the eco clear and
  then the turbo clear, 10 s apart like every special-mode write.
- **Special-mode requests that undo each other before they are sent merge.** Eco, turbo, quiet
  or the sleep profile changed several times within the 10 s pacing now cost one frame for the
  final value, where every change was queued and sent 10 s apart. A request the queue cannot
  take is refused and the entity keeps its state.
- **A finished frame is credited to the command it belongs to.** A display-preference frame or a
  special-mode write no longer restarts the wait of another command, so a lost command is sent
  again after about 4 s where an unrelated frame used to push that to 7 s.
- **Commands no longer hold up the heartbeat and the status poll.** Queued command frames went
  out back to back until the queue was empty, however long that took. A cycle now carries the
  frames that start within its first 600 ms (about four) and the rest follow in the next cycle,
  in order.
- **A damaged DevType reply is no longer believed.** The device type the unit reports in its
  `0x0A` reply is stamped into bytes 7/8 of every later frame, and it was taken from that reply
  without a look at its checksum: one flipped bit and the unit rejected every frame until the
  link had been lost and found again. A reply that fails its checksum is now a failed try (the
  probe goes out again 500 ms later), and it no longer counts as "heard the unit" in the
  heartbeat. The "Checksum errors" sensor counts every damaged frame, of any class, where it
  counted status frames only.
- **A short frame is no longer read into its own trailer.** The parsers took a payload byte when
  the frame was merely long enough to hold that offset, so in a short frame a checksum byte or the
  `F4 FB` tag was read as data. A status frame now needs 49 bytes, a ProductType reply 40 (44 for
  the extended tier) and a DevType reply 22. The frames real units send (160, 45 and at least 22
  bytes) decode as before.
- **Noise ahead of a reply no longer costs the reply.** A stray `F4 F5` in front of a frame, or a
  frame cut short, made the receiver miss the frame that followed (one "Reply timeouts" count and
  a missed status poll each time). It now looks again at the byte that broke a frame and follows
  a later start tag as a second candidate.
- **One "77" gesture is one setup request.** `on_setup_request` could fire twice for one gesture
  when the unit's two request bits changed while the request stood, every 10 s for a request bit
  that stayed up, and once more after a lost link. It now fires once per unbroken run of replies
  that ask. A unit that sends one pulse per gesture, as the one on the bench does, behaves as
  before.
- **A configuration with the `hisense_ac` hub and no `climate:` platform did not compile**
  (`esphome/components/climate/climate.h: No such file or directory`). The climate sources are
  now compiled only when the climate platform is in the configuration, as the switch and select
  sources already were. `tests/build.hub-only.esp32-c3-idf.yaml` covers it.

### Tooling and CI

- **`dev.py` is ESPHome-only.** The command line is `argparse` now: a wrong word gets a message
  that names it, each command has `--help`, and the word `esphome` after the command is optional.
  The `ota`, `amebaz2` and `esp32` words no longer answer with a pointer to `matter-final`.
  `$ESPHOME` is split into words and never run by a shell, so `ESPHOME="uvx esphome"` works and
  text after a `;` in it is no longer executed. Tool lookups need no bash. `dev.py erase` passes
  `--chip` for the board, so esptool refuses a port that leads to another kind of ESP.
- **The Matter conversion is `firmware/scripts/matter-convert.py`**, for a stock module that
  still runs the Matter firmware: `convert ...` wraps an ESPHome image for it, and `slots <ip>`
  and `backup <ip>` are the two read-only probes of such a unit. They were
  `dev.py ota amebaz2 revert --slots` and `--backup` in the Matter tree, and for one day
  `dev.py convert esphome ...`, `dev.py matter-slots` and `dev.py matter-backup`. The settings
  file is still `firmware/scripts/ota-release.env`.
- **The clip writers check what they write** (`firmware/flasher/`). `ch341flash-full.py` took any
  file of any size and wrote it over the whole chip. It now wants exactly 4 MB that hold a
  verifying firmware image and the module's system data, a `--backup <dump>`, and a typed
  confirmation that names the target and the size. `ch341flash.py` no longer writes
  `0x0..0x140000` (the range of the retired Matter clip image): it writes one firmware slot,
  `--layout <layout> --slot <1|2>`, after verifying the image the way the bootloader does.
  `ch341dump.py` reads twice and compares. A chip that stays busy is no longer written to.
- **`firmware/scripts/version-lint.py`** holds the version, this file and the release tag
  together and runs in `run_tests.sh`, the pre-commit hook and CI: the version is
  `MAJOR.MINOR.PATCH`, this file has a section for it or an Unreleased one, the version is not
  below the newest release tag, and every pin of the ESPHome release names the same one. In a
  pull request, CI fails when a file under `firmware/esphome/components/` or
  `firmware/esphome/packages/` changed and this file did not. A version bump is not required per
  change. The number moves when a release is cut.
- **The pre-commit hook** no longer skips the host tests on a commit with more than about 64 KB of
  file names, runs them for any file under `firmware/scripts/`, `firmware/flasher/` and the hook
  itself, and runs `docs/guide/lint.py` when a guide page is staged. `stop-slop.sh` also reads
  renamed files, files with non-ASCII names and `.mdx`.
- **`hil_esphome_actuation.py`** puts the unit back in a `finally` (a failed step, Ctrl-C and a new
  hard timeout included), now also restores the fan, reads the unit back at the end and fails
  when it is not as found, and does not touch a unit that is running unless `--allow-running` is
  given. Its default `--secrets` is this checkout's file.
- **CI compiles the ESP32 boards.** A new `esphome-esp32` job compiles `w41h1.yaml`,
  `w41h1-esp32c3.yaml`, `w41h1-esp32c3-plus.yaml` and the hub-only test, fails on a compiler
  warning from the components and checks that each board image carries the version. Until now
  the ESP32 files only went through `esphome config`. A `version` job runs the version and
  changelog rules with the full history. The ESPHome release is pinned once per workflow, and
  every job states its own permissions.
- **CI does less on a push that needs less.** A newer push cancels the run before it, a change
  that touches no firmware skips the three compile jobs, and the caches are saved when their
  inputs change and not on every push. New `lint-extra` job (actionlint, zizmor, the guide
  linter, cppcheck) and a coverage table on `main`. A nightly workflow fuzzes each harness for
  ten minutes, compiles the ESP8266 and RP2040 files, builds `esp32-recon` and runs ESPHome's
  dev-branch gates, which left the pull requests.

### Internals

Nothing in this section changes what is on the wire, the entities or what they publish.

- **Housekeeping for an upstream submission.** The hub no longer calls the deprecated
  `check_uart_settings` (validation already refuses a UART that is not 9600 8N1) and no longer
  overrides the setup priority with the default. The Python files have type hints, the component
  tests include `common.yaml` as a named package, and the docs draft follows the esphome.io
  format rules.
- **The hub's timing runs on ESPHome's loop time**, the bus scheduler included. At the top of
  the hub's `loop()` that is `millis()` or one millisecond behind it. Only
  `hisense_ac.set_setup_indicator` takes a fresh reading, because it is called from actions and
  lambdas outside the hub's loop.
- **`dump_config` of the hub prints what is fixed at setup**, which is the DE pin. It does not
  repeat the UART check that validation already makes. The link state, the "77" indication and
  the last status left it: they are in the log when they change.
- **Board files hold the chip, the pins and the extras, nothing else.** The stock module's fixed
  pins (PA14 / PA13 / PA17) are defaults in `packages/amebaz2.yaml`, and the `esp32:` block of the
  three ESP32 files is `packages/esp32.yaml`. No entity, id or generated line of code changes.
- **`w41h1_module` tidied, same behaviour:** the port is looked at from `loop()` once a second
  where a scheduler lambda did it, the hub is the component's parent (`Parented`), a missing
  update server is logged and no longer dereferenced, and `W41h1UartWatch` is `W41H1UartWatch`.
- **The old bus driver is a frozen test reference** under `firmware/test/reference-driver/`, and
  the codec parity test still compares the ESPHome component with it. `firmware/esp32-recon/`
  builds from it and owns its ESP-IDF HAL.

### Documentation

- The docs describe one firmware on three kinds of hardware. Hardware facts from the Matter
  documents moved to `firmware/docs/10-stock-module-boot-and-recovery.md` and
  `firmware/docs/16-esp32-board-notes.md`.
- The reverse-engineering pages say per page how each fact is known and whether the page is
  reference or history, and `reverse-engineering/docs/14-open-questions.md` lists what is
  unsettled. The protocol reference has a real device-type reply (captured 2026-10-12) and the
  receive rules the firmware applies.
- The guide is grouped by what a reader is doing (start, build and install, use, when something
  is wrong, developer, retired), with pages of their own for the stock module, Wi-Fi setup mode
  and the retired Matter firmware. `firmware/scripts/` and `firmware/flasher/` each have a README
  that says what protects every write. A page that moved leaves a pointer.
- Loose ends after that revamp: on the docs site, a link from a firmware or reverse-engineering
  page to a file outside the documentation now opens that file on GitHub (it was a dead link),
  the API key in `secrets.yaml.example` is 32 zero bytes so that nobody takes it for a key (the
  earlier example value is public: a node built with it unchanged needs a key of its own),
  `dev.py fetch` names all four secrets the build reads, `run_tests.sh` labels its layers in
  the order they run, and `firmware/scripts/amebaz2_image.py verify <dump-or-image>` checks the
  image chain of a stock-module dump from the command line (read-only, exit 0 or 1).
  `firmware/test/coverage/mutate.py` is safe to start as it is: four workers by default, a
  memory limit and a timeout on every mutated test binary, and a refusal of more workers than a
  quarter of the machine's memory allows.
- Setup mode has had its first run on a unit (2026-10-13, ESP32-C3 SuperMini Plus). The
  remote's gesture opened the hotspot and the timeout put the node back on the network it had.
  The notices on the setup mode pages now say what was seen and what was not.

## Matter and ESPHome side by side - 2026-10-10

Everything from July to 2026-10-10. It was never one release: the Matter firmwares had their own
tags (`amebaz2-v*`, `esp32-v*`) and the ESPHome nodes were built from `main`.

### ESPHome on the stock module: the receiver stall has a cause and a fix (2026-10-10)
- **Fixed: the bus receiver stops on the stock module.** LibreTiny keeps received bytes in the
  Arduino `RingBufferN`, whose reader decrements its count with the receive interrupt enabled. An
  interrupt inside that decrement stores a byte that is never counted, and from then on every
  frame is read one byte late and one byte short: reply timeouts, no checksum error, transmit
  still working, until the port is reopened. Reproduced on a module with the UART in loopback:
  at 9600 baud, read in groups as the main loop reads, the count fell behind 5 times in 233034
  bytes without a lock (one a minute) and 0 times in 233287 with interrupts masked around the
  read. At 115200 baud: 21 times in 142558 bytes against 0 in 285036. The component now reads
  under ESPHome's `InterruptLock` on LibreTiny. ESP32, ESP8266 and RP2040 builds are unchanged.
- **The serial port restart stays, as a fallback, and no longer fires mid-frame.** It worked
  because reopening the port makes a new buffer. It now waits while the hub holds the transceiver
  in transmit, where closing the port would put a break on the pair. `Serial port restarts`
  should stay at 0 on a unit that answers. The hub gains `bus_transmitting()` for this.
- **DE timing on the stock module, measured in the firmware:** DE falls 23.9 to 25.9 ms after the
  last stop bit, for a 25 ms hold, with Home Assistant connected. It was not the fault.
- **Open: a second converted module went silent on the bus within four minutes** and no restart,
  reset or power-on brought a reply. The receive pin never goes low after our frames, so the
  cause is outside the chip (transceiver, wiring or the A/C) and needs someone at the unit.
  `firmware/docs/15-esphome-path.md` has the measurements.

### ESPHome on the stock module: first run on hardware, and what it found (2026-10-10)
- **The factory-layout build runs on a module in an A/C.** It boots under the factory bootloader,
  joins Wi-Fi, reports its flash layout as matching, drives the bus, and has taken two ESPHome
  updates over the air (running slot 2, 1, 2, back within about 10 s each time). The module was
  converted over the air: the Matter firmware installed the LibreTiny image, re-signed with the
  next FWHS serial and wrapped as a Matter `.ota`. The sdk and native layouts still compile only.
  There is no long run yet, and the return to the Matter firmware has not been tried.
- **Fault found: the bus receiver stops.** Twice the receive side of the serial port stopped and
  stayed stopped until a power cycle, while commands still reached the unit. The cause is not
  known. Reopening the port restores it, so the AmebaZ2 board package now restarts the port when
  the hub reports the link lost and every 15 s while it stays down, logs a warning each time, and
  counts the restarts in a new `Serial port restarts` diagnostic sensor. The timing rule has a
  host test. The component itself is unchanged by this and builds as before on every board.
- **Outdoor and coil temperature read "unknown" instead of -20 C after a power cut.** For a while
  after mains power returns the unit reports both as `0xEC` (-20 C), a placeholder. The component
  publishes that pair as unknown until a real pair has arrived since the bus link came up. A lone
  -20, or -20 after real readings, is still published. One observation on one unit, marked
  VERIFY. The Matter builds are unchanged and show -20 C in that window.
- **Changes existing ESPHome nodes: Wi-Fi power saving is off.** `packages/node.yaml` now sets
  `power_save_mode: none`. The validated configuration of the ESP32, ESP32-C3 and RP2040 board
  files changes from `LIGHT` to `NONE` at their next build. ESP8266 and the stock module already
  defaulted to `NONE`. With `light`, an ESP32-C3 node on a weak signal (about -77 dBm) lost 65 %
  of pings and commands did not reach the unit. With `none`: 100 of 100, a command taken in
  0.5 s. The radio no longer sleeps between beacons, so the board draws more on average. A board
  on a weak supply can go back with `-s wifi_power_save light`.
- **Changes existing ESPHome nodes: three diagnostics on every board.** `packages/node.yaml` adds
  the sensors `Uptime` and `WiFi signal` (every 60 s) and the text sensor `Reset reason` (from
  the `debug:` component), all in the diagnostic category. The validated configuration of
  `w41h1.yaml` and of every other board file gains these three entities at its next build: 31
  become 34, the set the nodes in the field already expose from their own configs. The stock
  module's files have 36, with `Flash layout` and `Serial port restarts` on top. All three exist
  on ESP32, ESP8266, RP2040 and LibreTiny, and all seven board files compile with them. The
  wording of `Reset reason` is each platform's own.
- `dev.py convert esphome --board amebaz2-factory [--stage]` makes the conversion image: it
  re-signs the built ESPHome image, verifies it the way the bootloader will, refuses one that
  does not fit the slot, and writes the `.ota` and its manifest. Host-tested, and checked byte for
  byte against the image that was installed on the unit. New guide page:
  `docs/guide/Converting-a-Stock-Module-to-ESPHome.md`.
- Builds: `dev.py` sets `PLATFORMIO_BUILD_CACHE_DIR` for ESPHome builds, and the
  `esphome-amebaz2` CI job keeps the same cache next to the PlatformIO tree. A stock-module image
  builds in 18 s into a new directory, against 29 s cold. The ESP32 boards gain nothing from it
  (ESPHome builds those with ESP-IDF's tools and ccache).

### ESPHome: one entity definition for every board, and a build for the stock module
- The node config is split. `firmware/esphome/packages/hisense-ac.yaml` holds the bus, the hub and
  every entity, `packages/node.yaml` holds logger, API and Wi-Fi, `packages/ota.yaml` the update
  server, and each board file holds only its name, pins and platform block. `w41h1.yaml` validates to the same configuration as
  before, with or without the `-s` board overrides, so existing nodes are unaffected.
- New board files: `w41h1-esp32c3.yaml` (the C3 SuperMini, same node the `-s` overrides build),
  and two compile-only examples, `w41h1-esp8266.yaml` and `w41h1-rp2040.yaml`.
- **`w41h1-amebaz2-factory.yaml`, `-sdk.yaml`, `-native.yaml`: ESPHome for the stock module's own
  RTL8710C through LibreTiny. They compile (601 KB). At the time of this entry they had not run on
  hardware; the section above has the first run.** There is one
  file per flash layout a module can have and no default, because an image built for the wrong
  layout bricks the unit at its first update. The layouts are tracked in
  `firmware/esphome/components/w41h1_module/`. The build fails if the image's addresses are not
  those of the layout it names, and at run time a `Flash layout` diagnostic sensor compares them with the
  bootloader's partition table and switches the update server off on a mismatch. The files also
  carry the linker workaround for the LibreTiny reboot hang (issue 396). The open risks are listed
  in `firmware/docs/15-esphome-path.md`. `dev.py test|build esphome --board amebaz2-<layout>`
  validate and compile one, and `dev.py` refuses to flash any of them.
- The component needed no change to build there. Host tests now cover a UART write that blocks
  until the bytes are out (LibreTiny) and a loop that polls the bus only every 16 ms: DE is still
  released 25 ms after the last byte.
- CI validates every board file and compiles all three AmebaZ2 ones, failing if the component
  warns.

### Fixed (AmebaZ2 1.3.50)
- An AmebaZ2 node published an empty sleep profile list (ep6 ModeSelect
  `SupportedModes`), so Home Assistant showed the sleep select as unavailable. The list lives in a
  Realtek SDK file that was edited by hand, and a restored SDK brought back the stock table.
  `dev.py ota amebaz2 build` now writes the five profiles (Off, General, Old, Young, Kids) on
  endpoint 6 on every build, with a host test.

### Matter (AmebaZ2 1.3.49, ESP32 1.1.19): the controls and diagnostics the ESPHome build has
Built and host-tested. Both targets have since run it on a unit.
- **Beeper switch**, endpoint 11 on both targets: an On/Off plug-in unit labelled "Beeper", on by
  default. Off clears the buzzer bit (frame byte 23 bit 2, the stock `t_beep`) on every command
  frame the node sends, through the same stamp the ESPHome path uses (`hisense_stamp_beep()`,
  checked byte for byte against it). The setting survives a reboot: NVM attribute storage on
  AmebaZ2, a non-volatile attribute on ESP32. Endpoints 0 to 10 are unchanged. Home Assistant
  needs a re-interview of the node to see the new endpoint.
- **Bus diagnostics** on the manufacturer cluster `0xFFF1FC00` (ep1), read-only: `ChecksumErrors`
  `0x0014`, `ReplyTimeouts` `0x0015`, `UnansweredCommands` `0x0016`, `LinkLosses` `0x0017` (all
  int32u, since boot), `LinkToken` `0x0018` (int16u) and `BusLink` `0x0019` (boolean). The shared
  driver gained the three counters the ESPHome bus already kept. See `firmware/docs/14`.
- **Thermostat `MinSetpointDeadBand` is 0.** AmebaZ2 did not have the attribute, so the server
  used its 2.5 C default and refused cooling setpoints below 18.5 C and heating setpoints above
  29.5 C. ESP32 carried the esp-matter default of 2.0 C (18 C and 30 C). The unit takes 16 to 32.
- With the beeper on, every frame is byte for byte what the previous version sent.

### Tooling
- CI: the hosted ESP32 build is faster and no longer fails on Docker Hub's pull limit. It pulls
  Espressif's image through Google's public mirror, keeps a compiler cache per target and SDK pin
  pair (saved from `main`, restored everywhere), and skips changes that only touch markdown.
- `firmware/scripts/zap_edit.py` edits the AmebaZ2 `.zap` without the ZAP GUI (add an attribute,
  set a default or storage option, append an endpoint) and `zap_edit.py check` runs the build's
  generation steps on a scratch copy, failing on a dropped attribute or a new kind of ZAP warning.
  The three data-model changes above were made with it. Recipe in
  `firmware/src/sdk-edits/README.md`.

### ESPHome
- Confirm and retry. A command frame the unit does not answer is sent again in the next bus cycle
  (three sends at most). A command the unit answers and does not apply is sent again once its
  status shows that, four seconds later (twice at most, once for eco, turbo, quiet and sleep).
  After that the node logs a warning and shows what the unit reports. A newer command replaces
  the one being checked, and a lost link drops it. The unit's own overrides (its setpoint outside
  cool and heat, turbo, the fan under quiet and sleep) are not treated as lost commands. New
  optional sensors `command_retries` and `failed_commands`; `unanswered_commands` now counts every
  unanswered send.
- A mode request to a unit that is off is one frame carrying mode and power-on (byte 18 = mode
  bits with `0x0C`), as the stock module sends it. It was a power frame followed by a mode frame,
  and losing the second left the unit running in its last mode (#168). The re-send falls back to
  the two frames if the unit is still off.
- `beeper` switch: clears the buzzer bit (frame byte 23 bit 2) on every command frame the node
  sends. The bit comes from the stock module's `t_beep` handling, found by static analysis of the
  stock image. Stored on the node, on by default.

### Docs
- New guide page, `docs/guide/ESP32-Build-Environment.md`: from a clean Linux machine to a flashed
  ESP32 for both the ESPHome and the esp-matter build, with the steps through `dev.py` and by hand
  (#117). The ESP-IDF version in the guide now points at `versions.env`, and the ESPHome page
  describes the `uart:` block the YAML has.

### Tooling
- `firmware/scripts/esphome-upstream-check.sh` runs ESPHome's own CI scripts against the
  `hisense_ac` component in a checkout of `esphome/esphome`. CI requires it to pass against the
  pinned ESPHome release and also reports the result against their `dev` branch. The component
  tests moved to `firmware/esphome/tests/components/hisense_ac/`, the path they take upstream.
- ESP32 1.1.18: carries the Matter glue fixes from the feedback-loop audit (writes that follow the
  node's own command inside the status lag, out-of-range status modes, the 3-bit status mode
  field). First build of that code for the ESP32 targets.
- ESP32 1.1.17: no firmware source change. First release built with ESP-IDF v5.5.5 and esp-matter
  `release/v1.6`, and the first ESP32 release through `dev.py`. Its delta patch against 1.1.16 is
  large because the toolchain changed.
- AmebaZ2 1.3.45: no firmware change. It is the first image built, staged and flashed through
  `dev.py` after the release scripts were removed, and it carries the commit pin for
  `ameba-rtos-matter`.
- `firmware/scripts/dev.py` is now the release engine for both Matter targets. The logic of
  `ota-release.sh`, `esp32-release.sh` and `ota-guards.sh` was ported into it and the three scripts
  are removed (#143). Use `dev.py ota amebaz2 <step>` and `dev.py ota esp32 <step>`; the steps and
  flags keep their names. `dev.py` is a build-clock input for AmebaZ2, so the port moves
  `SOURCE_DATE_EPOCH` for the tree. AmebaZ2 images were byte-identical between the script and
  `dev.py` at a fixed clock.

### Firmware
- ESPHome: the `bus_link` binary sensor now shows the link going down (it could only ever publish
  on), and the post-command holdoff no longer sticks on for ~24.8 days after the `millis()` sign
  flips, which skipped every readback on a unit left untouched that long.
- ESPHome: eco, quiet, turbo and the sleep profile are now `climate` presets, named exactly as the
  `hisense-unified-ac` integration names them for Matter nodes, so a climate group can sync presets
  across both firmwares. Special-mode writes (presets, switches, sleep select) share a paced queue
  that spaces them 10 s apart, and a fan change is refused while turbo, quiet or sleep owns the fan.
  New `supports_eco/quiet/turbo/sleep` options on the climate platform. The preset table, detection
  and write plan are pure functions in `esphome_aircon_map.h` with host tests. It lives under
  `firmware/src/` but changes no Matter image.
- ESPHome: log levels are used consistently. `WARN` for a lost link, an unsent command, a bad
  checksum or a fault. `INFO` for recovery. `DEBUG` for status changes and commands. `VERBOSE` for
  every status frame and unanswered poll. `VERY_VERBOSE` for the raw bytes of every frame. The
  once-a-second status line is no longer logged at `DEBUG`.
- ESPHome, **breaking**: fan modes are now `auto`, `low`, `medium_low`, `medium`, `medium_high`,
  `high`, matching `hisense-unified-ac`. `Medium-low` / `Medium-high` are renamed, and `quiet` is
  no longer a fan mode (use the `quiet` preset; the quiet step reads back as `low`). Update
  automations that set the old names.
- ESPHome, **breaking**: the unit's own auto mode is now the climate mode `auto` instead of
  `heat_cool`, so Home Assistant labels it "Auto". Update automations and climate groups that set
  `heat_cool`. The mapping lives in `esphome_aircon_map.h`, which is under `firmware/src/`, so the
  AmebaZ2 version moves to 1.3.46 with no change to any Matter image.
- ESPHome: the component is self-contained and builds on ESP8266 as well as ESP32. The fallback
  transport that ran the shared driver's bus task is removed, along with the `tx_override` bench
  services. State is published on change, with sensors refreshed once a minute, instead of on
  every status frame.

### Fixed
- AmebaZ2 1.3.48, and the ESP32 build from its next release: four faults found by an audit of the
  Matter write and readback paths after #168. The decisions are pure functions in
  `matter_aircon_map.h` with host tests. None has been on hardware yet.
  - Choosing the fan's Auto preset, or the A/C reporting fan Auto, made the AmebaZ2 node command
    the fan to High and then back to Auto. The FanControl server nulls PercentSetting and
    SpeedSetting when FanMode becomes Auto, and the null byte was read as 255 %.
  - A write that followed one of the node's own commands inside the second or two the status
    takes to catch up was compared with the old status and dropped: On then Off left the unit
    running, a mode chosen on a powered-down unit lost the setpoint sent with it, and Cool to Heat
    lost the heating setpoint. On AmebaZ2 a powered-down frame read during a power-on could also
    switch the unit off again through OnOff, the same way #168 describes for SystemMode. While a
    command settles (3 s), power and mode are now taken from what was commanded, for the guards
    and for what OnOff and SystemMode report. A mode the A/C ignored therefore reads back as the
    old one after 3 s, not at once.
  - A status frame with a mode nibble outside the known ones (0 to 3, 5 and 6) was copied into the
    command shadow, where it encodes as a different mode: the next setpoint or fan change could
    have switched the unit to Fan or Heat. The shadow now keeps its last good mode.
  - The status mode was read as four bits. The stock capability table defines it as three (byte
    18, bits 4 to 6), so a frame with bit 7 set decoded to an unknown mode and was reported as
    Cool. The driver and the ESPHome codec port now both mask three bits.
- AmebaZ2 1.3.47: the node no longer commands its own stale SystemMode readback. A status frame
  parsed before the A/C applied a mode change is published over the client's value, and if the
  status had moved on by the time that readback was handled it was sent to the A/C as a new
  request: Auto could go back to Cool, or the unit could be switched off again. The node now notes
  the values it publishes and never commands them. The decision is a pure function in
  `matter_aircon_map.h` with a host test. Not yet confirmed on hardware as the cause of #168.
- ESPHome: `w41h1.yaml` now integrates energy with `method: left`. Since power is published on
  change, the default `right` method misplaced up to a minute of energy at each compressor start
  and stop. Nodes built from an older copy of the YAML need the same line.
- AmebaZ2 1.3.44: General Diagnostics NetworkInterfaces now lists every valid IPv6 address, not only
  the link-local one. The Ameba port copied address slot 0 and hard-coded the count to 1, so the
  SLAAC and DHCPv6 addresses in slots 1 and up were never reported. Tentative and duplicated slots
  are skipped, as on ESP32 (#139).
- AmebaZ2 builds: the image's build date (`SOURCE_DATE_EPOCH`) is now the author date of the newest
  commit touching the image inputs, not the HEAD commit time. An image built on a branch and
  flashed before merging now matches the tag rebuild of the merge commit byte for byte, which is
  what the release workflow promises. `ota-release.sh epoch` prints the value, `build` refuses a
  shallow clone and warns on uncommitted inputs, and the release workflow checks out full history.
- AmebaZ2 1.3.43: the node is now dual-stack (IPv4 enabled in connectedhomeip) so it answers mDNS
  on 224.0.0.251 as well as ff02::fb. The Realtek SDK ships with IPv4 off, which left mDNS
  IPv6-only. Behind UniFi Multicast Enhancement the AP delivers IPv4 mDNS to IGMP members and no
  IPv6 mDNS at all, so the node never heard a query: it still announced itself, but a restarted
  controller could not resolve it and CASE failed until the node happened to reassociate. The ESP32
  build was already dual-stack.
- AmebaZ2 1.3.42: the app resends MLD and IGMP reports after each 4-way handshake. The Realtek SDK
  raises the lwIP link before the first association and leaves it up across disconnects, so lwIP's
  own link-up re-report never runs and the AP forgets the node's group memberships after a
  reassociation.
- AmebaZ2 1.3.33 -> 1.3.38: PercentSetting 42 / 75 landed one fan step up (58 / 100 %). The app
  publishes FanMode by folding the six speeds into Low/Medium/High, and that readback re-commanded
  the bucket's speed along two paths: the connectedhomeip patch mapped it onto PercentSetting
  33/66/100 (now skipped for writes flagged `gW41h1AppFanModeWrite`), and it was queued to the
  app's own FanMode handler as if a client wrote it (now recorded in an own-write echo ledger,
  `matter_echo_note/consume()`, and skipped). 1.3.35-1.3.37 tried a bucket comparison instead, which
  let a stale readback undo a fan-card Medium press. ESP32 builds its FanControl in code, has neither
  path, and was not affected.
- `ota-release.sh lint` (the pre-commit hook) no longer refuses a tree whose version equals the
  recorded on-device version, so the tree that was just flashed can be committed without
  `--no-verify`. A version below the device still fails. `flash` now refuses an unbumped version
  itself instead of relying on lint having run first. `.released-version` is one mark for the
  repo, not per node, so `OTA_ALLOW_SAME_VERSION=1` lets `flash` roll the same version out to
  another unit (equal only, with a warning). (#136)

### Tooling
- C/C++ lint for every tree we own (`firmware/scripts/cpp-lint.sh check|fix`), with ESPHome's
  clang-format and clang-tidy configs and the portable rules of its `ci-custom.py`, gated in CI
  and the pre-commit hook. The shared driver, the ESP32 glue, esp32-recon and the host tests were
  reformatted and their findings cleared with no behaviour change (host objects identical), which
  is why AmebaZ2 moves to 1.3.40 and ESP32 to 1.1.16 with nothing new on the bus.
- `ota-release.sh tag` and `esp32-release.sh tag` now ask GitHub whether a runner with the
  `sdk-builder` label is online before printing the push command, and warn if none is. Both release
  workflows run only on that self-hosted runner, so a tag pushed while it is down sat in `queued`
  with no error (#138). The check only warns. If `gh` is missing, not logged in, or the API call
  fails, it says the check was skipped and carries on.

## Diagnostics exposed to Home Assistant - 2026-07-22

### Firmware
- ESP32 1.1.3 -> 1.1.6: decode CompressorHz, Features1, and Faults1 from the ep1
  manufacturer cluster (#82); extend the Wi-Fi TX-power throttle from the HTTP break-glass path to
  the Matter (BDX) OTA path via a `HisenseOTARequestorDriver` (#84); add an RX checksum helper with
  an observe-only mismatch counter, a heap-watermark task, and bus-task-scoped WDT panic (#87).
- AmebaZ2 1.3.20 -> 1.3.22: adopt the shared checksum observe-only change (1.3.21) and
  surface the mismatch counter on the `:2323` console (1.3.22, #88).

### Home Assistant
- The `hisense-unified-ac` integration renders four diagnostic entities per node (compressor
  frequency, capabilities, faults, bus link) on three units. Closes #38 and #39.

### Fixed
- #83: the ESP32 module that would not join Wi-Fi was hitting a brownout crash loop at `phy_init`
  from an RF-cal current spike on a marginal 5 V rail. CHIP already auto-reconnects; the fix is a
  470-1000 uF bulk cap, not an app-level reconnect handler.

## Initial public release - 2026-07-16

First public cut. Everything below already ran privately; this release drops the personal details.

### Firmware
- AmebaZ2 (RTL8710C) Matter `room_air_conditioner` firmware with the reverse-engineered Hisense
  RS-485 driver: HVAC mode/setpoint/fan/swing, Eco/Quiet/Turbo/Sleep, and derived power/voltage.
- ESP32 replacement firmware (esp-matter) that mirrors the same control surface for dead modules.
- OTA over Wi-Fi after the initial CH341 flash. `ota-release.sh` sets the AmebaZ2 boot-slot serial
  so an update sticks instead of rolling back.

### Tooling & docs
- `ota-release.sh` runs the build/package/stage/flash pipeline. Host-only QA (`firmware/test/`)
  covers codec, Matter↔A/C mapping, and virtual-A/C round-trip checks.
- Reverse-engineering docs: RS-485 protocol, cloud/firewall, stock-FW init, hardware.
- CI: a host lint gate on every push/PR, plus tagged-release builds that attach `.bin`/`.ota` to
  GitHub Releases (both the AmebaZ2 and ESP32 builds run on a self-hosted SDK runner).
