# Security policy

This is a community reverse-engineering and firmware project, maintained in personal time. The
firmware runs on hardware you own and flash yourself, and it drives an air conditioner. It is not
a certified product (see [`NOTICE.md`](NOTICE.md)).

## Supported versions

The ESPHome firmware on the current `main` branch is supported. From 2.0.0 on, releases are tags
(`vX.Y.Z`), and the newest release is supported too. Build from the newest commit or the newest
release before reporting.

The two Matter firmwares (tags `amebaz2-v*`, `esp32-v*`, and the tree at `matter-final`) are
retired and receive no fixes, security fixes included. Known properties of those builds: Matter
test credentials, and in debug images an unauthenticated console on port 2323 that can drive the
A/C bus.

## Reporting a vulnerability

Report privately. Do not open a public issue for a security bug.

- Preferred: GitHub, Security, "Report a vulnerability" (a private advisory) on this repository.
- Or email **hello@andreasincode.com** with `[security] hisense-w41h1` in the subject line.

Include the affected component, the version or commit, the steps to reproduce and the impact. A
proof of concept helps. Allow a reasonable time for a fix before public disclosure.

Expect an acknowledgement within about 7 days and a first assessment within about 30 days. This
is best effort for a personal project.

## What counts as a security issue here

A node is on your Wi-Fi and can switch an appliance on, set it to heat, and keep it there. These
are security issues in this project:

- Driving the A/C, or reading a node's state, from the network without the node's API encryption
  key.
- Getting a node's Wi-Fi credentials, API key or hotspot password out of it over the network or
  over the bus. This includes Wi-Fi setup mode: its hotspot, its captive portal, and the copy of
  the previous network's name and password it keeps in flash while the mode is on.
- Putting a node into setup mode, or keeping it there, from somewhere other than the A/C's own
  remote or Home Assistant.
- Bytes on the RS-485 bus that make the firmware crash, read or write out of bounds, or send a
  command nobody asked for. The codec's parsers treat the bus as untrusted input and are fuzzed
  for this reason.
- A flash tool (`firmware/flasher/`) that
  writes something other than what it was given and verified, or that can be made to run a
  command from a file name, a setting or an image.
- A CI workflow that runs code from a pull request with more permission than it needs, or that
  can leak a token.
- A secret committed to the repository's history.

A defect that only makes the A/C do something unwanted, such as a wrong mode after a command, is
an ordinary bug: open an issue.

## Known properties, not vulnerabilities

These are by design and documented. A report is still welcome if you find they reach further than
described.

- **The reference configuration sets no OTA password.** Anything on the node's network can push
  an image to it. Set `password:` under `ota:` if the node shares a network you do not control
  ([`docs/guide/ESPHome-Build.md`](docs/guide/ESPHome-Build.md), "Updates").
- **`firmware/esp32-recon/` has an unauthenticated console on TCP port 2323.** It is a bench
  tool, a separate ESP-IDF program and not part of the ESPHome firmware. Whoever can reach that
  port can send any frame to the A/C bus. Run it on a network you trust and do not leave it
  installed in a unit.
- **A firmware image built in CI holds the placeholder secrets** of `secrets.yaml.example`, which
  are public. That is why releases attach no image. Do not flash a CI artifact.
- **The setup hotspot is protected by `wifi_ap_password` only.** Whoever knows it and is in radio
  range while the hotspot is open can give the node another network.
- **Physical access.** Anyone who can open the indoor unit can read the module's flash, which
  holds the Wi-Fi credentials, and can reflash it. The project's own tools work this way.

## Out of scope

- ESPHome, LibreTiny, Espressif's ESP-IDF and the Realtek AmebaZ2 SDK: report those to their own
  projects.
- The Hisense vendor firmware and the ConnectLife cloud. They are the subject of the
  reverse-engineering pages, not software maintained here. What those pages found about the
  vendor firmware's network surface is in
  [`reverse-engineering/docs/13-stock-fw-feature-map.md`](reverse-engineering/docs/13-stock-fw-feature-map.md).

In scope is this repository's own code: the ESPHome components and board files
(`firmware/esphome/`), the build, flash and conversion scripts (`firmware/scripts/`,
`firmware/flasher/`), the bench and analysis tools (`firmware/esp32-recon/`,
`reverse-engineering/tools/`), and the CI workflows (`.github/workflows/`).

## Secrets

Raw flash dumps, device keys and Wi-Fi credentials are gitignored and must never be committed
(see [`dumps/README.md`](dumps/README.md)). If you believe a secret was committed to the history,
report it privately as above.
