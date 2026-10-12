# Security Policy

This is a community reverse-engineering / firmware project maintained in personal time. It runs on
hardware **you own** and physically flash. It is **not a certified product** (see
[`NOTICE.md`](NOTICE.md)).

## Supported versions

Only the ESPHome firmware on the current `main` branch is supported. Build from the newest commit
before reporting.

The two Matter firmwares (tags `amebaz2-v*`, `esp32-v*`, and the tree at `matter-final`) are
retired and receive no fixes, security fixes included. Known properties of those builds: Matter
test credentials, and in debug images an unauthenticated console on port 2323 that can drive the
A/C bus.

## Reporting a vulnerability

**Please report privately, do not open a public issue for a security bug.**

- Preferred: **GitHub → Security → "Report a vulnerability"** (private advisory) on this repo.
- Or email **hello@andreasincode.com** with `[security] hisense-w41h1` in the subject line.

Include the affected component + version/commit, reproduction steps, and the impact; a
proof-of-concept helps. Please allow a reasonable window to fix before any public disclosure.

**Response expectations** (best-effort, personal project): acknowledgement within ~7 days, an
initial assessment within ~30 days.

## Scope

In scope, defects in this repo's **own** code:

- the ESPHome component and its board files (`firmware/esphome/`)
- the build and flash scripts (`firmware/scripts/`, `firmware/flasher/`) and the recon tools
  (`firmware/esp32-recon/`, `reverse-engineering/tools/`)
- the CI workflows (`.github/workflows/`)

Out of scope:

- **ESPHome**, **LibreTiny**, **Espressif ESP-IDF** and the **Realtek AmebaZ2 SDK**: report those
  to their respective projects
- the **Hisense stock firmware / ConnectLife cloud**: that's the *subject* of the
  reverse-engineering, not software maintained here
- anything requiring the attacker to already have **physical flash access**: that is the threat
  model this project operates under, by design
- the reference configuration's lack of an OTA password: documented, and yours to set
  (`docs/guide/ESPHome-Build.md`, "Updates")

## A note on secrets

Raw flash dumps, device keys, and Wi-Fi credentials are git-ignored and must never be committed
(see [`dumps/README.md`](dumps/README.md)). If you believe a secret was committed to history,
please report it privately as above rather than opening a public issue.
