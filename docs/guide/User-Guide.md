# User Guide

This guide takes you from a Hisense A/C with an `AEH-W41H1` Wi-Fi module to a climate device in
Home Assistant that works without the ConnectLife cloud. Start here: this page helps you choose
the hardware, says what state each choice is in, and names the page to read next.

There is one firmware, the `hisense_ac` ESPHome component. It talks to the A/C over the 4-pin
RS-485 port the Wi-Fi module plugs into, and to Home Assistant over the ESPHome native API. What
you choose is the board it runs on.

Not sure the unit is the right kind? See
[Will this work on my A/C?](https://andrewdemsds.github.io/hisense-w41h1/compatibility.html)

## Choose the hardware

| | ESP32 replacement board | The stock module | Another ESPHome chip |
|---|---|---|---|
| What it is | an ESP32-C3 SuperMini or a classic ESP32 with a 3.3 V RS-485 transceiver, plugged in where the module was | the module that came with the A/C (Realtek RTL8710C), running ESPHome through the LibreTiny platform | an ESP8266 D1 mini or a Raspberry Pi Pico W with the same transceiver |
| You add | about 5 EUR of parts and four soldered wires | nothing | parts you already have |
| First flash | USB cable | over the air from this project's retired Matter firmware (done once, with a script last carried by release `v2.0.2`). From the vendor firmware it needs a SOIC-8 clip or UART download mode, and that has not been done | USB cable |
| If an update goes wrong | the bootloader rolls back to the previous image. A USB flash always recovers the board | no rollback. An image that does not boot needs the clip and a dump of that unit taken beforehand | USB flash |
| State today | on live units since 2026-08, both boards | the factory flash layout runs on one module in an A/C since 2026-10-10. The other two layouts compile and have never run. A second converted module went silent on the bus, cause unknown | example configs that compile every night. Never run on a board |
| Read next | [ESP32 Replacement Board](ESP32-Replacement-Build) | [The Stock Module](Stock-Module-Build) | [ESPHome Firmware](ESPHome-Build#board-files) |

Use an ESP32 board unless you have a reason to keep the original module. The measured comparison
is `firmware/docs/13-path-comparison.md`.

## The route with an ESP32 board

1. Look at the module and the port it plugs into: [Hardware and Wiring](Hardware-and-Wiring).
2. Build the board, and read the pins to avoid before you solder:
   [ESP32 Replacement Board](ESP32-Replacement-Build).
3. Install ESPHome and fill in the secrets file: [Build Environment](ESP32-Build-Environment).
4. Flash it over USB. From `firmware/esphome/`:

   ```
   esphome run w41h1-esp32c3.yaml      # ESP32-C3 SuperMini
   esphome run w41h1.yaml              # classic ESP32
   ```

   Or let the wrapper script ask before each step:
   `python3 firmware/scripts/dev.py walk esphome --board c3`. Its commands are in
   [Build, Flash and Test](Build-Flash-Test).
5. Bring it up in three stages: on the bench against the simulator, on the real bus with only A
   and B connected, then powered from the A/C
   ([staged bring-up](ESP32-Replacement-Build#staged-bring-up)).
6. Add the node to Home Assistant: [ESPHome Firmware](ESPHome-Build#adopt-it-in-home-assistant).
7. Use it: [Everyday Control](Everyday-Control).

## The route with the stock module

Read [The Stock Module](Stock-Module-Build) and its risk list first. Take a full dump of the flash
chip with the clip before anything else ([Recovery and Reflash](Recovery-and-Reflash)). The rest
of the route (secrets, Home Assistant, everyday use) is the same as above.

## When something breaks

| Situation | Page |
|---|---|
| A symptom you can name (no bus link, a command that does not take, a node Home Assistant cannot find) | [Troubleshooting and FAQ](FAQ-Gotchas) |
| What an entity or a counter means | [Entities and Diagnostics](Entities-and-Diagnostics) |
| The node has to move to another Wi-Fi network | [Wi-Fi Setup Mode and the Status LED](Wifi-Setup-Mode) |
| A board or a module that no longer answers | [Recovery and Reflash](Recovery-and-Reflash) |

## What has not run on hardware

The firmware drives live A/Cs every day. These parts of the tree are host-tested or compile, and
nobody has seen them work on a unit yet:

- Wi-Fi setup mode and the remote's "77" gesture ([details](Wifi-Setup-Mode)).
- The status pixel of the ESP32-C3 SuperMini Plus.
- The stock module's sdk and native flash layouts.
- A first install of ESPHome on a stock module with the clip, and the clip tool that writes one
  firmware slot.
- The ESP8266 and RP2040 example configs.

Each page repeats the note where it matters.

## Other things to know

- Home Assistant is the only controller. Apple Home, Google Home and Alexa reach the unit through
  Home Assistant's own Matter or HomeKit bridge.
- Several units can act as one thermostat: [Climate Groups](Climate-Groups).
- This project used to ship two Matter firmwares. They are retired. If a unit of yours runs one,
  see [Matter Firmware (Retired)](Matter-Firmware-Retired).
- Contributing, the test suite and the protocol:
  [Repo Map and Build Pipeline](Repo-Map-and-Build-Pipeline), [Testing and QA](Testing-and-QA),
  [Protocol Overview](Protocol-Overview).
