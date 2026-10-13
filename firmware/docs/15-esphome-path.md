# 15: ESPHome on the stock module, and how the ESPHome firmware came about

Read this before you put ESPHome on a stock AEH-W41H1 module. It has what was measured on
modules, how to find the flash layout a unit has (the wrong one bricks it), the open risks and
what is still untested. After that come the hardware results on the ESP32 boards, and the history
of the design: the plan the firmware started from, what was reversed and the dead ends.

Other pages own the rest:

- options, entities, board files and packages: [`firmware/esphome/README.md`](../esphome/README.md)
- how the code is structured, and why setup mode and confirm and retry work the way they do:
  [`01-component-architecture.md`](01-component-architecture.md)
- how the module's bootloader picks a slot and what a rejected image looks like:
  [`10-stock-module-boot-and-recovery.md`](10-stock-module-boot-and-recovery.md)
- the short version for someone who wants a working unit:
  [`docs/guide/Stock-Module-Build.md`](../../docs/guide/Stock-Module-Build.md)
- the over-the-air conversion from the retired Matter firmware, step by step:
  [`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md#a-stock-module)

## The stock module through LibreTiny

The component uses ESPHome's `uart:` and no chip SDK, so the same YAML and the same C++ build for
the module's own MCU, the Realtek RTL8710C (AmebaZ2 family), through ESPHome's LibreTiny platform
(`rtl87xx:`). With it, the unit needs no replacement board.

Status: the factory layout has run on one module in an A/C since 2026-10-10. The sdk and native
layouts compile and have not run on hardware.

### Hardware results, 2026-10-10

One module with the factory layout, in an A/C, converted over the air from this project's
retired Matter firmware (the procedure is in
[`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md#a-stock-module)).
Measured that day:

| What | Result |
|---|---|
| Boot | the image boots under the factory bootloader and joins Wi-Fi |
| Layout guard | `Flash layout` reported `ok`: running slot 2, next write address `0x010000`, the Matter image left intact in slot 1 |
| Delivery | the LibreTiny application image, re-signed with the next FWHS serial and wrapped as a Matter `.ota`, was applied by the Matter firmware's normal OTA path |
| ESPHome to ESPHome updates | two, with `esphome upload` (`firmware.uf2`). The running slot went 2, 1, 2 and the node was back within about 10 s each time |
| Reboot | no hang, with the workaround flags in the image. One mains power cycle also came back clean |
| Bus | status frames decode and commands are answered. The reply verdict logs `Command answered (class 0x65, echo, ack)`, so the echo-plus-ACK layout the component marks VERIFY is seen on this unit |
| Receiver | stopped twice and stayed stopped until a power cycle. A cause was found and is fixed ([The receiver stall](#the-receiver-stall)) |
| A second module, read with the clip | also the factory layout, with the vendor firmware (serial 100) in both slots and nothing in `0x3D0000..0x3D8000` |
| A second module, converted | link up for about a minute, then its bus went silent at the pin within four minutes and stayed silent ([The silent unit](#the-silent-unit)) |

### The receiver stall

Twice the bus link dropped and did not come back until the unit was power cycled: once within the
first minute of the first boot after the conversion, and once right after
the first command sent to a unit that had been idle for five minutes. The unit obeyed that command
and later ones, so transmit kept working. Only receive was dead, and the node reported the link
as lost while still controlling the unit. 115 forced preference writes to flash did not reproduce
it, and neither did repeated commands.

#### A cause: LibreTiny's receive buffer loses count

LibreTiny 1.13.0 keeps received bytes in the Arduino `RingBufferN`. The reader, on the main loop,
ends with `_numElems--`. The receive
interrupt ends with `_numElems++`. The decrement is a load, a subtract and a store, and the
interrupt is not masked around it. When the interrupt lands between the load and the store, the
store writes the old count minus one and the byte the interrupt just stored is never counted. It
stays in the buffer. From then on `available()` is one short, every frame is read one byte late
and one byte short, and the frame assembler never completes a frame: reply timeouts with no
checksum error, while transmit works. Nothing puts the count right again except a new buffer.
The same race is reported against the Arduino core API (arduino/ArduinoCore-API issue 195).

Measured on a module, with UART0 in internal loopback so that the real interrupt and the real
buffer are used and nothing reaches the bus:

| Reader | Baud | Bytes | Times the count fell behind |
|---|---|---|---|
| no lock, one read at a random time within 127 us | 115200 | 142558 | 21, the first after 0.8 s. 21 bytes behind at the end |
| interrupts masked around the read (PRIMASK) | 115200 | 142551 | 0 |
| ESPHome `InterruptLock` around the read | 115200 | 142485 | 0 |
| no lock, reads in groups every 2 to 10 ms, as the main loop reads | 9600 | 233034 | 5, the first after 59 s: one a minute |
| ESPHome `InterruptLock`, same reader | 9600 | 233287 | 0 |
| no lock, one read within 127 us of each byte | 9600 | 178420 | 0: every read is over before the next byte |

No byte was lost or reordered in any run: the bytes arrive, the count is wrong. The 9600 baud
run with grouped reads is the bus's own case, with bytes arriving all the time: one loss a
minute. On the bus, bytes arrive for roughly a quarter of each second (a 160-byte status frame
and the shorter replies), which gives a loss every three to four minutes. The unit that showed
the stalls, once the restart below was in place, dropped its link at random intervals with a
median of 169 s, each drop ending 0.4 to 1.5 s later with the restart, with no checksum errors.

#### The fix

On LibreTiny the component reads the port under ESPHome's `InterruptLock`
(`HisenseAC::bus_read`), one buffered byte at a time, a few microseconds each. The other
platforms are not touched: their UART drivers do not use this buffer.

#### The port restart, now a fallback

Closing and reopening the port (`Serial0.end()`, `Serial0.begin()`) makes a new buffer, which is
why it brought the link back. With the receive
interrupt switched off on purpose, the link was reported lost after 5 silent polls and was back
0.6 s after the restart. The `w41h1_module` component still restarts the port when the hub
reports the link lost and every 15 s while it stays down, as a net for a stall with some other
cause. It logs a warning each time and counts the restarts in a `Serial port restarts` diagnostic
sensor. It no longer fires while the hub holds the transceiver in transmit: closing the port
takes the TX pin away from the UART, and with DE high that puts a break on the pair. The timing
rule is a plain function with a host test (`components/w41h1_module/w41h1_uart_policy.h`,
`firmware/test/test_esphome_uart_policy.cpp`). It lives in that component and not in the hub
because the port is LibreTiny's: ESPHome's uart component can reload a port's settings only on
ESP32 and ESP8266. The component closes and reopens the port object ESPHome's uart component
holds, which is `Serial0` for these pins. If the counter stays at 0 through a soak on a unit
that answers, the restart can go.

### DE timing, measured

With an API client and Home Assistant connected, over about 150 frames:
the write blocks for up to 12.5 ms (LibreTiny waits for room in the 16-byte transmit FIFO, so it
returns with up to 16 bytes still to send), and DE fell 23.9 to 25.9 ms after the last stop bit.
The 25 ms hold is kept to within 1 ms: the deadline is counted from before the write and
includes the frame's wire time, and the loop runs every 1 to 5 ms while DE is high. The longest
single pass of the main loop in the same period was 53 ms, so a hold of up to about 75 ms is
possible. That would cost one reply, and the link is reported lost only after five in a row. The
time the
A/C takes to answer could not be measured (the unit this was timed on did not answer at all).

### The silent unit

A second module, in another A/C, ran ESPHome with the link up for about a
minute, then lost the link five times within two and a half minutes, the fifth time for good.
The first timeout came 1.1 s after Home Assistant connected. This is not the buffer fault and no
firmware state explains it:

- The receive pin never went low: 0 of 2.3 million samples taken in the 120 ms after our frames,
  0 bytes in the receive FIFO, no framing, break or overrun flag, with the receive interrupt
  enabled throughout.
- Our side looks right: the TX pin toggles during a frame (low in 71 % of samples), the baud
  divisor gives 9604 baud, DE is low when idle (read back as an input)
  and is driven around every frame. 16 bytes left the transmit FIFO in 16.66 ms.
- It made no difference whether DE fell 25 ms or 0.95 ms after the last stop bit.
- About 100 port restarts, eight software resets and one power-on reset did not bring a reply.
  Nor did five minutes without transmitting, twice.

What is left is outside the chip: the transceiver, the wiring, or the A/C not answering. Whether
that unit still obeys commands was not checked. It needs someone at the unit: a command with the
beeper on, a scope or a second transceiver on the pair, and the same module on the unit that
works.

Not measured: the sdk and native layouts, any run longer than an afternoon, an interrupted or
failed update, the return to the Matter firmware, and the image without the reboot workaround.

### What the component does

- `firmware/esphome/w41h1-amebaz2-factory.yaml`, `-sdk.yaml` and `-native.yaml`: one board file
  per flash layout a module can have (next section). All three use board `cr3l`, UART0 on PA14
  (TX) and PA13 (RX) and DE on PA17, the module's own wiring, and share
  `packages/amebaz2.yaml`. They include the same entity package as the ESP32 board files and add
  one diagnostic text sensor, `Flash layout`, the serial port recovery described above and its
  `Serial port restarts` sensor.
- `firmware/esphome/components/w41h1_module/`: the component that carries everything the module
  needs in code. `packages/amebaz2.yaml` is configuration only and hands it the layout. Its
  options are in [`firmware/esphome/README.md`](../esphome/README.md#w41h1_module). From `layout`
  its code generation copies the matching `layout-*.json` into the build directory, names it as
  PlatformIO's `custom_board`, passes the flag the build-time check reads, and adds
  the two linker flags of the reboot workaround. At run time it checks the layout against the
  bootloader's partition table, watches the bus link and restarts the serial port, and publishes
  the two sensors. It validates only on `rtl87xx` with an RTL8720C-family board, and a
  configuration for any other chip that names it fails `esphome config` with a message that says
  so. Nothing it uses lies outside its own directory, so it also works from a git source
  (`external_components` with `path: firmware/esphome/components`, listing `hisense_ac` and
  `w41h1_module`).
- Board `cr3l` is a Tuya module with the same chip class. It is used because it is the LibreTiny
  board definition that names PA13 and PA14 as UART0. ESPHome's LibreTiny UART only accepts a
  port's fixed hardware pins, and `generic-rtl8720cm-4mb-1712k` does not define them. Its flash
  layout is not used: `w41h1_module` replaces it with one of the `layout-*.json` files in its own
  directory.
- The logger stays on LibreTiny's default port, UART2 (TX on PA16), which is the module's log
  console pad.
- CI compiles all three whenever a change touches the firmware (`esphome-amebaz2` in
  `.github/workflows/qa.yaml`) and fails if one of this repository's components warns.

### Which flash layout a unit has

There is no default layout, and the choice is not a preference. It has to be the layout already
in the module's flash.

LibreTiny takes the two slot addresses from the build: where an update is written, which image
counts as valid, and which one to invalidate once the update is in. The module's bootloader takes
them from the partition table in flash. If the image was built for other addresses than the table
holds, the first ESPHome to ESPHome update is written inside the wrong region and the running
image is then invalidated. Neither slot boots after that, and the unit needs the clip.

The fleet has two layouts, and LibreTiny brings a third:

| Layout | Board file | FW1 | FW2 | Settings (kvs) | A unit has it when |
|---|---|---|---|---|---|
| factory | `w41h1-amebaz2-factory.yaml` | `0x010000` + `0x170000` | `0x190000` + `0x170000` | `0x3D0000` | it went from the vendor firmware to this project's firmware over the air, so it kept the factory partition table |
| sdk | `w41h1-amebaz2-sdk.yaml` | `0x00C000` + `0x1AC000` | `0x1B8000` + `0x1AC000` | `0x3D0000` | its flash was first written with the clip image of this project's retired Matter build (`flash_rac-integrated-*.bin`), which carries the Realtek SDK's partition table |
| native | `w41h1-amebaz2-native.yaml` | `0x010000` + `0x1AC000` | `0x1BC000` + `0x1AC000` | `0x3F8000` | it was written from scratch with LibreTiny's own partition table and bootloader |

LibreTiny's stock layout for `cr3l` is the native row. It is right only for a module that was
given LibreTiny's partition table and bootloader with the clip. On a converted unit it is the
brick described above, and its settings area at `0x3F8000..0x3FFFFF` also lies over data the
Matter firmware and the Realtek SDK keep at the top of flash (the tail of the DCT, UART settings
at `0x3FB000`, the Bluetooth FTL at `0x3FC000`). The factory and sdk layouts put the settings at
`0x3D0000`, above the second slot in both.

To tell which layout a unit has:

- **Its history.** Converted over the air from the vendor firmware and never clipped: factory.
  First written with the retired Matter clip image: sdk.
- **Ask the retired Matter firmware,** if the unit still runs it (1.3.9 or later), for the
  address of the inactive slot:
  `matter-convert.py backup <unit-ip>` prints `addr=`. `0x10000` or `0x190000` is the
  factory layout, `0xC000` or `0x1B8000` is the sdk layout. The command only reads.
- **A clip dump** ([`firmware/flasher/README.md`](../flasher/README.md)). The second firmware
  image starts at `0x190000` (factory), `0x1B8000` (sdk) or `0x1BC000` (native).
- **A unit that already runs ESPHome** says it in its `Flash layout` sensor.

Two guards keep a wrong choice from going unnoticed. Both are in
`firmware/esphome/components/w41h1_module/w41h1_slots.h`:

- At build time, the header checks that the image's slot and settings addresses are exactly those
  of the layout the board file names. A layout file that is missing, misnamed or not applied
  fails the build. A configuration with no layout named, or with a name that is not one of the
  three, already fails `esphome config`: `layout` is a required option of `w41h1_module`.
- At run time, the image asks the Realtek SDK where the bootloader's partition table puts the next
  update (`sys_update_ota_prepare_addr()`) and compares that with its own two slots. The result is
  logged at boot and published as the `Flash layout` sensor, for example
  `factory ok slot=2 next=0x010000 fw1_sn=... fw2_sn=...`. On `MISMATCH` the image also switches
  its own update server off, so the bricking update cannot be started. ESPHome's safe mode does
  not run that check, so a unit in safe mode still accepts an update.

The run-time check has run on one module, with the factory layout, and reported `ok` there. It
has not run on the sdk or native layout, and a mismatch has not been provoked on hardware.

### What was checked without hardware

| Check | Result |
|---|---|
| `esphome config` and `esphome compile`, ESPHome 2026.7.4, LibreTiny 1.13.0, all three layouts | pass. Flash 603,889 bytes: 40.1 % of the factory layout's 1,507,328 byte slot, 34.4 % of the 1,753,088 byte slot of the other two. Static RAM 13,401 of 262,144 bytes |
| Layout applied | each build's slot and settings addresses are those of its layout (compile-time check), and the application image is built for the first slot's address (`0x010000`, `0x00C000`, `0x010000`) |
| Warnings from `components/hisense_ac/` and `components/w41h1_module/` | none, also with every option of every platform declared (`tests/build.rtl87xx-ard.yaml`) |
| UART write blocks (LibreTiny waits on the TX FIFO, about 1 ms per byte) | host test: DE still falls 25 ms after the last byte, the cycle stays at 1 s, no reply is missed |
| Receive buffer | LibreTiny's is 256 bytes and filled from an interrupt. A status frame, the longest the unit sends, is 160 bytes |
| Reboot workaround links | `arch_restart()` in the built image branches to `sys_reset` (disassembly of the ELF). With the flags set by the component: `__wrap_lt_reboot` and `sys_reset` are the same address in the ELF (`nm`) |
| DE pin bring-up order | the RE notes ([`10-stock-fw-init-and-comms.md`](../../reverse-engineering/docs/10-stock-fw-init-and-comms.md)) show `gpio_init(PA_17)` faulting before the scheduler runs. ESPHome calls `setup()` from the main task, after it |

### Open risks

1. **Platform stability.** LibreTiny rates RTL8720C 2 out of 5. Its feature table for 1.13.0 lists
   Wi-Fi, digital I/O, flash I/O and the watchdog as untested on this family and OTA as not
   implemented, although the OTA code is there and users report it working. On this module Wi-Fi,
   the DE pin, flash writes and two updates worked on the day of the test. The serial
   receive buffer loses count without a lock (risk 12).
2. **Reboot hang.** LibreTiny 1.13.0 restarts the chip with a CPU-only reset. On other RTL8720C
   modules that leaves it dark after a restart or an OTA until power has been off for about
   30 seconds (libretiny-eu/libretiny issue 396). The fix, a watchdog system reset, is proposed in
   pull request 397 and is in no release. `w41h1_module` applies the same change with two linker
   flags (`--wrap=lt_reboot`, `--defsym=__wrap_lt_reboot=sys_reset`), the workaround confirmed
   in that issue on two other modules (a Tuya WBR3 and a Xiaomi one). On this module, with the flags
   in the image, two updates and one mains power cycle came back without a hang. The image has not
   run without them, so whether this module needs them is not known.
3. **No automatic rollback.** The ESP32 bootloader goes back to the previous image when a new one
   fails to boot. Nothing does that here. ESPHome's safe mode covers an image that boots and then
   crash-loops. An image that does not get that far needs the clip.
4. **First flash.** With the factory or sdk layout the module keeps the bootloader and partition
   table it has, and only the application image is new: it goes into a firmware slot. With the
   native layout the partition table, the bootloader and the application are all LibreTiny's, by
   SOIC-8 clip on the GD25Q32 or by UART download mode: UART2 (PA15 RX, PA16 TX), with PA0 held at
   3.3 V through reset and PA13 not pulled low. Whether PA0 and PA15 can be reached on the W41H1
   board is not known. A unit that runs this project's Matter firmware needs neither: the Matter
   firmware installs the application image over the air (`matter-convert.py convert`, the guide
   linked above). Writing one slot with the clip is what `firmware/flasher/ch341flash.py` is
   for, and that script has not written a module yet
   ([`firmware/flasher/README.md`](../flasher/README.md)). That is how the one tested module got it.
5. **Calibration data.** Flash `0x1000..0x3FFF` holds per-module data, including the flash
   controller's calibration table at `0x1040` that the boot code reads. The full-flash image
   LibreTiny builds is blank there apart from two bytes at `0x1028`. Written whole with a clip, it
   erases that data. A clip write has to keep the region from the module's own dump. Whether
   LibreTiny's bootloader needs its two bytes is not known.
6. **Flash pin select.** The RE notes warn that a bootloader which selects the flash D2 line on
   PA17 would drive the DE pin from the flash controller. The stock bootloader does not.
   LibreTiny replaces the bootloader, and what its one selects has not been checked. Scope PA17
   during boot with the module off the A/C bus.
7. **DE while the chip boots.** PA17's level between reset and `setup()` is whatever LibreTiny's
   boot leaves it at. A high DE holds the A/C's bus. Read back as an input on a running module,
   PA17 is low even with the chip's pull-up switched on, so the board pulls DE down while nothing
   drives it. What the boot code does with the pin before `setup()` has not been measured.
8. **Loop blocking.** A command frame holds the main loop for about 50 ms. ESPHome warns above
   50 ms ("took a long time for an operation"), so the log may show that warning once or twice.
   The Wi-Fi stack runs in its own tasks and is not held. ESPHome's own components, the API
   among them, wait for the write. DE is released within 1 ms of its 25 ms hold, timed in the
   firmware ([DE timing, measured](#de-timing-measured)). A scope has not been on it.
9. **Settings storage.** ESPHome's preferences go to LibreTiny's key-value area, 32 KiB at the
   address the layout gives: `0x3D0000` for factory and sdk, `0x3F8000` for native. `0x3D0000` is
   above the second slot in both layouts. The converted unit booted and ran with the area as the
   Matter firmware had left it, and a second module read with the clip had nothing there under
   the vendor firmware. What the area held on the converted unit, and how FlashDB treated it on
   first boot, was not looked at. The native
   address must not be used on a unit that may go back to the Matter firmware without a full
   clip restore: it overwrites data that firmware keeps at the top of flash.
10. **Going back.** Returning to the stock or the Matter firmware is a full clip write of a dump
    taken before the first flash. Take the dump and verify it first. A return over the air has
    not been tried. After the first ESPHome update the Matter image is gone from the other slot.
11. **The wrong layout.** Covered above. The build-time guard is tested (a wrong or missing
    layout file fails the build). The run-time guard has reported `ok` on one correct unit. It
    has not been seen to report a mismatch on hardware.
12. **The receiver stall.** Described [above](#the-receiver-stall). One cause is found and
    prevented. The restart stays for any other, and a `Serial port restarts` sensor that keeps
    climbing on a unit that answers means the port is failing in a way that is not understood.
13. **A unit that stops answering.** One of two converted modules went silent on the bus within
    four minutes and nothing in the firmware brought it back ([The silent unit](#the-silent-unit)). Until the
    cause is known, a conversion can end with a unit that has to go back to the Matter firmware
    with the clip.

### What is left to test

The first module went straight into a unit in service, so the bench steps were skipped and are still
worth doing on a spare module: restart ten times, update five times, cut power in the middle of
one update, and watch DE on a scope for the settle and drain times and for its level during boot,
against `virtual_ac.py` on the bus pads.

Beyond that: the sdk and native layouts, a soak of days on the converted unit with the restart
counter watched (it should now stay at 0), the cause of the silent unit, the time the A/C takes
to answer (DE fall to first received byte), `hil_esphome_actuation.py` against this build, and a
return to the Matter firmware.

## Hardware validation on ESP32 boards

The ESP32 and ESP32-C3 builds run on live units, powered from the A/C connector's 5 V and closed
up (stage 3 of the bring-up in
[`docs/guide/ESP32-Replacement-Build.md`](../../docs/guide/ESP32-Replacement-Build.md)).

- A node on a live `CF35LR03G` was driven end to end from Home Assistant: every field of the
  climate card, the eco, turbo, quiet and display switches, and all four sleep profiles.
- Two protocol defects surfaced there and not on the bench. Byte 36 rides every combined frame
  and `0x00` means "display on", not "leave this field alone", so driving eco, turbo or quiet
  re-lit a panel the user had switched off. The hub now carries the display preference in every
  frame. And both single-field frames (quiet, sleep) lacked `frame[31] = 0x01`, so the unit
  accepted and ignored them. That story is in
  [`07-stock-parity-gaps.md`](07-stock-parity-gaps.md#mute-and-sleep-one-missing-byte-2026-08-19-fixed).
- `firmware/test/hil_esphome_actuation.py` passed 48 of 49 checks on a live A/C on 2026-09-25.
  The one miss was a `heat_cool` readback, an intermittent miss on the A/C's side. That run was
  on the build before confirm and retry.
- Runs on two units before confirm and retry failed about one check in 52, each time with
  `unanswered_commands` at exactly 1: the unit missed one frame and nothing sent it again. That
  is what confirm and retry was built for
  ([`01-component-architecture.md`](01-component-architecture.md#commands-from-a-tap-in-home-assistant-to-the-wire-and-back)).
  No run of the hardware test with `failed_commands` at 0 on both units is recorded in this
  repository since.

Still open on the ESP32 boards:

- a tap on the transceiver's DI line with a decoder, to confirm the frames on the wire byte for
  byte. Do not skip the ground-loop warning when you do it;
- one captured reply to a command, which would settle the acknowledgement layout
  (`command_reply_verdict()`, marked `VERIFY`). One stock module has logged a reply that agrees
  with it;
- every preset set from Home Assistant reading back as itself, from each starting preset;
- everything about Wi-Fi setup mode ([`firmware/esphome/README.md`](../esphome/README.md#the-check-on-a-unit)).

## History: the original plan and what became of it

> **Retired on 2026-10-12.** This section describes the design of August 2026 (issue #13), when ESPHome
> was to be a third firmware beside two Matter builds. Paths under `firmware/esp32-matter/` and
> `firmware/src/`, the `dev.py ota ...` commands, the `:2323` console and the
> `hisense-unified-ac` integration are no longer on `main`. The current firmware is ESPHome
> alone, described by the pages linked at the top and by
> [`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md). The
> last tree that has the Matter builds is the tag `matter-final`.

### What the plan set out to do

An ESPHome firmware for the ESP32 replacement board that exposes the full A/C control surface
over the native Home Assistant API, with no Matter stack, no commissioning and no companion HACS
integration. The goal was simplicity, measured as fewer moving parts between a user's A/C and
their dashboard and less project-specific machinery to maintain. Parity with the Matter feature
set was a hard constraint.

The scope was the ESP32 and ESP32-C3 on the same boards and wiring as the ESP32 Matter build.
The stock module and Matter itself were out of scope: a user who needed Apple Home, Google Home
or Alexa was to stay on the esp-matter build. Both limits moved. ESPHome became the only
firmware (issue #182), and the stock module got a LibreTiny build (the first section of this
page).

### Why it was worth building

The RS-485 protocol is the expensive part of this project. By August 2026 it was done, validated
on hardware and covered by host golden tests. Everything above it exists only to move decoded
values into Home Assistant. On the Matter path that transport cost, at the time:

| Machinery on the esp-matter path | Why it exists | ESPHome equivalent |
|---|---|---|
| `main/app_main.cpp` (1961 lines) | endpoint/cluster construction, attribute I/O, echo guards, commissioning, OTA hooks, watchdogs | ~400 line hub component + entity classes |
| `main/diag_console.cpp` (584 lines) + the `:2323` debug flavour | no other way to see decoded state on a deployed node | `logger:` + diagnostic entities, always on, no second flavour |
| `scripts/dev.py ota esp32` release steps | delta-OTA base archiving (#82), version int discipline (#77), staged provider push | `esphome run` |
| `scripts/esp32-lint.sh` | keeps `PROJECT_VER` and `sdkconfig` in sync | not applicable |
| `ElectricalPowerMeasurementDelegate.{h,cpp}` | CHIP delegate so EPM reads route correctly | three `sensor:` declarations |
| `integrations/hisense-unified-ac` (separate repo, submodule) + `test_diag_contract.py` | Matter cannot render a manufacturer cluster in HA without two upstream PRs (`firmware/docs/14-diagnostics-ha-exposure.md` at the tag `matter-final`) | native entities, no bitmap, no cross-repo contract |
| Test attestation, VID `0xFFF1`/PID `0x8001`, the uncertified-device caveat | Matter requires credentials | none |
| Commissioning, BLE, fabric management, the "77" recommission flow | Matter onboarding | Wi-Fi credentials in the YAML, the AP fallback, and "77" as a way to open that hotspot on demand |

That was about 3700 lines of ESP32-specific firmware and tooling (`app_main.cpp`,
`diag_console.cpp`, the EPM delegate, and the two scripts), plus a second repository, in service
of transport. The ESPHome path deleted all of it and kept the 2824 lines of driver and mapping
code (plus the HAL) that encoded the reverse-engineering work.

The diagnostics are the clearest case. Doc 14 (at the tag) documents that surfacing the 18
fault bits and
15 capability flags natively in HA is blocked behind PRs to two upstream projects, which is why the
firmware packs them into `Features1`/`Faults1` bitmaps that a custom HACS integration unpacks, with
a host test pinning the bit layout across two repositories. In ESPHome, each bit is a
`binary_sensor` with `entity_category: diagnostic`. The bitmaps, the integration, and the contract
test all stop being necessary.

### Decisions that were reversed

The plan had three "non-negotiables". Two were reversed so that the component can go to upstream
ESPHome, which cannot depend on files outside the component.

1. *The driver is reused unchanged* (`hisense_rs485.{h,cpp}`, `matter_aircon_map.h`,
   `power_estimate.h` as shared source, no "ESPHome flavoured" copy). Reversed: the component
   carries its own codec, held equal to the old driver by
   `firmware/test/test_esphome_codec_parity.cpp`. The old driver is the frozen reference under
   `firmware/test/reference-driver/`.
2. *The HAL is reused unchanged and ESPHome's `uart:` is not used.* The HAL owned the port
   through `uart_driver_install()` and carried the DE timing that took a multi-day debug to
   find. Rewriting it against `esphome::uart::UARTDevice` meant validating DE assert and release
   timing against a real mainboard again. Reversed: the component talks through `uart:` and
   drives DE itself, and that timing has since been validated on units. The peripheral-driven
   alternative that ESPHome's own RS-485 support uses (`flow_control_pin`) is still not
   validated against this A/C.
3. *Parity is verified against the inventory below, not asserted.* Kept.

One mapping decision also changed. The plan mapped the unit's auto mode to
`CLIMATE_MODE_HEAT_COOL`, so that Home Assistant would not show it as a scheduler. It is
`CLIMATE_MODE_AUTO` now.

### The parity inventory

Every capability the ESP32 Matter node exposed, and where it landed in ESPHome. It is kept
because it is the map for anyone who moves a unit from the Matter firmware, and the record of
what "parity" was checked against. Endpoint numbers refer to
`firmware/esp32-matter/main/app_main.cpp` at the tag `matter-final`. The driver column names
functions of the reference driver (`firmware/test/reference-driver/`).

| Matter (retired) | ESPHome | Driver source |
|---|---|---|
| ep1 OnOff | `climate` mode `OFF` vs any other | `hisense_build_power_frame()` |
| ep1 Thermostat SystemMode | `climate` mode auto/cool/heat/dry/fan_only (the unit's auto is `CLIMATE_MODE_AUTO`) | `HisenseCommand.mode`, `state.mode` |
| ep1 Occupied{Cooling,Heating}Setpoint | `climate` target temperature, 16 to 32, step 1 | `state.setpoint_c` |
| ep1 LocalTemperature | `climate` current temperature | `state.indoor_temp_c` |
| ep1 ThermostatRunningState | `climate` action | `hisense_to_running_state()` |
| ep1 Thermostat FeatureMap gating | YAML: omit `heat` from `supported_modes` | `matter_thermostat_featuremap()` |
| ep1 FanControl FanMode/Percent/Speed | `climate` fan mode: auto, low, medium_low, medium, medium_high, high (quiet is a preset) | `hisense_fan_raw_to_*()` |
| ep1 FanControl Rock | `climate` swing mode off/vertical/horizontal/both | `state.vswing_on`, `state.hswing_on` |
| ep1 EPM ActivePower/Voltage/Current | 3 `sensor` (power W, voltage V, current A) | `power_estimate.h` |
| not exposed on Matter | `Energy today`, ESPHome's `total_daily_energy` on the power sensor | none (the reference's `hisense_energy_add/mwh()` is not used) |
| ep1 mfg `0x0010` CompressorHz | `sensor` compressor frequency | `state.compressor_freq` |
| ep1 mfg `0x0012` Features1 (15 fields) | 13 diagnostic `binary_sensor` (`capability_*`). The two 2-bit fields have no entity, and the planned `text_sensor` summary was not built | `HisenseFeatures` |
| ep1 mfg `0x0013` Faults1 (18 bits) | 18 diagnostic `binary_sensor` | `HisenseFaults` |
| ep2 TemperatureMeasurement | `sensor` outdoor temperature | `state.outdoor_temp_c`, unknown while the unit sends its placeholder (below) |
| ep8 TemperatureMeasurement | `sensor` coil temperature | `state.coil_temp_c`, same rule |
| ep3 OnOff Eco | `switch`, and the `climate` presets | `HISENSE_FEATURE_ECO` / `ECO_OFF` |
| ep4 OnOff Quiet | `switch`, and the `climate` presets | `hisense_build_mute_frame()` |
| ep5 OnOff Turbo | `switch`, and the `climate` presets | `HISENSE_FEATURE_TURBO` |
| ep6 ModeSelect Sleep profile | `select` with 5 options, and the `climate` presets | `hisense_build_sleep_frame()` |
| wrapper preset (hisense-unified-ac) | `climate` preset, same 13 names | `esphome_preset_plan()` |
| ep7 ContactSensor aux heat | `binary_sensor` | `state.heat_relay_on` |
| ep9 OnOff panel display | `switch` | `HisenseDisplay` tri-state |
| ep10 BooleanState aggregate fault | `binary_sensor`, `device_class: problem` | `HisenseFaults.any` |
| ep11 OnOff beeper (label "Beeper", default on, persisted) | `switch` beeper | `hisense_set_beeper()`, `hisense_stamp_beep()` |
| ep1 mfg `0x0014` to `0x0017` bus counters | 4 diagnostic `sensor` (checksum errors, reply timeouts, unanswered commands, link losses) | `hisense_*_count()` |
| ep1 mfg `0x0018` LinkToken | diagnostic `text_sensor` | `hisense_get_link_token()` |
| ep1 mfg `0x0019` BusLink | `binary_sensor`, `device_class: connectivity` | `hisense_link_is_up()` |
| Thermostat C/F unit (#5) | no entity. The component reads the unit's flag and sends the setpoint in the unit the panel is in | `state.temp_unit_f` |
| link health nulling (#56) | `binary_sensor`, `device_class: connectivity`, plus NaN on stale sensors | `hisense_set_link_cb()` |
| `:2323` `token` | diagnostic `text_sensor` | `hisense_get_link_token()` |
| `:2323` `busstats`, checksum counter | diagnostic `sensor` | `hisense_checksum_mismatch_count()` |
| `:2323` `bootreason` | `Reset reason`, ESPHome's `debug` text sensor | `esp_reset_reason()` |
| `:2323` `poll`, `watch`, `raw` | `logger:` at DEBUG, entity history in HA | driver callbacks |
| `:2323` `decode`, `tx`, `selftest` | not built. `firmware/esp32-recon/` has them on a board of its own | `hisense_build_command_override()` |
| Identify=77 recommission (#69) | `on_setup_request:` automation trigger and `hisense_ac.set_setup_indicator`, bound in `packages/node.yaml` to Wi-Fi setup mode (the `wifi_setup_mode` component) | `hisense_set_recommission_cb()`, ported into the component's codec |
| Identify=88 HTTPS-OTA break-glass (#61, #104) | native `ota:` plus `safe_mode` | dropped |

Two entries changed character:

- **Capability gating (#72, #102)** stops being runtime firmware logic and becomes a YAML decision.
  On the Matter path the firmware hides eco/quiet/display when the `0x66/40` ProductType reply says
  the unit lacks them, because a Matter node's endpoint list is fixed once commissioned. In ESPHome
  a user does not declare the entities their unit does not have, and the `capability_*` binary
  sensors tell them which those are. Same outcome, no firmware state
  machine. A warning when a declared entity contradicts the reported capability was planned and
  is not implemented.
- **The "77" flow** loses its Matter meaning (there is no fabric to swap). It is an automation
  trigger, `on_setup_request`, that a user can bind to whatever they want, and the shipped
  configuration binds it to Wi-Fi setup mode. The panel shows "77" only while the configuration
  asks for it, so the A/C does not sit in the mode (#69).

### Dead end: sharing the driver's sources with the ESPHome build

This was Phase 0 of the plan, and the whole route was removed later. It is recorded so that
nobody tries it again. The first working answer (ESPHome 2026.7.4) was the option the plan had
listed last: the component's `__init__.py` registered
`firmware/esp32-matter/components/hisense_hal` and `hisense_rs485` as **local ESP-IDF components**
via `add_idf_component(name=..., path=...)`. They were proper IDF components with the right
include dirs, so they compiled in place, unmodified, with no copy and no sync step.

The symlink and sync-copy options were tried first and both failed for the same reason. Symlinks do
get followed when ESPHome copies component sources into the build tree, so the `.cpp` files compile
fine, but the driver includes its HAL headers with angle brackets (`<platform_stdlib.h>`), and on
the ESP-IDF framework **ESPHome forwards only `-D` and `-W` compiler flags** (see
`framework_helpers.get_project_compile_flags`), so `-I` cannot put the component directory on the
include path. The generated `src` component registers `INCLUDE_DIRS "." "esphome"` and nothing else,
which is why a copied-in header at `src/esphome/components/hisense_ac/platform_stdlib.h` is present
but unreachable. Anyone tempted to retry the symlink route will get a build that compiles the glue
and then fails on the driver's first angle include.

Two consequences of that route: pin configuration worked by `-D` (`PinNames.h` guards each
`PA_*` define with `#ifndef`, and the copy of that header in `firmware/esp32-recon/` still has
the guards), and the ESPHome build depended on the `esp32-matter` directory layout. That
dependency on files outside the component is what upstream ESPHome cannot accept, and why the
component got its own codec and moved onto `uart:`.

### Phase log

| Phase | What | Outcome |
|---|---|---|
| 1 | hub plus climate: power, mode, setpoint, fan, swing, current temperature, action | done. Bench-validated against `firmware/test/virtual_ac.py` over a USB-TTL adapter |
| 2 | eco, quiet, turbo and display switches, the sleep select | done |
| 3 | telemetry and diagnostics: outdoor, coil, compressor, power, voltage, current, energy, aux heat, the fault bits, the capability flags, bus link, counters, boot reason | done. The first build was 834 KB of flash, 45.5 % of the app partition, 47.6 KB of RAM, which is why the delta-OTA machinery of the Matter build was not needed |
| 4 | hardware validation | on live units, see [above](#hardware-validation-on-esp32-boards). The DI-tap pass is open |
| 5 | docs and CI | done: this directory's pages, the guide page, `esphome config` in CI (pinned, and checked to fail on a renamed option). The stale `reverse-engineering/esphome/w41h1-esp32.yaml` was replaced by a README that says why the third-party `airconintl` config it carried is not what to hand a user (its payload byte map is not validated for this unit) |
| 6 | special modes as climate presets | implemented, not yet validated preset by preset on a unit |
| 7 | confirm and retry, power and mode in one frame | implemented. The hardware exit criterion (the hardware test passing on two units with `failed_commands` at 0) is not recorded as met |
| later | the component's own codec and `uart:`, the stock module, setup mode as a component, the split of the pure layer, the version | 2026-10, see [`CHANGELOG.md`](../../CHANGELOG.md) |

Phase 6 exists because a climate group in Home Assistant syncs only `climate` attributes, so eco,
quiet, turbo and sleep were invisible to it as switches and a select. The presets use the names
the `hisense-unified-ac` wrapper gave the Matter builds (`none`, `eco`, `quiet`, `turbo`,
`eco_quiet`, `sleep_<profile>`, `eco_sleep_<profile>`), so units on either firmware agreed while
both existed. The interlocks were measured by that wrapper on a live A/C.

The plan left a release tag for the ESPHome build optional. The firmware now has a version and a
release workflow ([`firmware/esphome/README.md`](../esphome/README.md#version-and-releases)).

### Two points of the old design that still explain the code

**The hand-off from the bus to the loop.** The old driver ran its own FreeRTOS task and fired a
status callback from it, and ESPHome entities must publish from the main loop. The plan was for
the hub to keep one status snapshot and a dirty flag under a mutex. With the scheduler in
`loop()` there is no second task and no mutex, but the hub still parks a status during the bus
poll and publishes after it. ESPHome's `control()` is only ever called by Home Assistant, never by
the component's
own `publish_state()`, so the class of downlink, readback, uplink feedback loops that the Matter
glue had does not exist here. Per-field echo guards are not needed. A short command hold-off
still is, so that a poll that predates a user command cannot visibly revert it.

**One source, two ways to load it.** The component lives in this repository. The board files
load it with `external_components` and a local path. A user's own configuration loads the same
directory with `type: git` and `path:`. There is no second repository and no mirror to keep in
step.

### What the move to ESPHome gave up

- **Every controller that is not Home Assistant.** Matter was the only path to Apple Home, Google
  Home and Alexa.
- **Runtime capability gating.** On Matter the firmware hid endpoints the unit lacked. Here it is
  a YAML decision.
- **Project identity.** The repository's headline was a Matter firmware.

The plan also listed "the stock module" as lost. That one came back.

### Deliberately not attempted

- LibreTiny as a route to ESPHome on the stock module was on this list in the original plan. It
  has since been built and has run on one module. The reasons it was left out still describe the
  risk: at the time nothing in this project had run it, and a wrong guess leaves a unit that
  only a clip recovers.
- Rewriting the HAL against ESPHome's `uart:` component was on this list too, and was done later
  for the reason above.
- Any Matter or HACS interoperability for the ESPHome firmware. A user picks one transport.
