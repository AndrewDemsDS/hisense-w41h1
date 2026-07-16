# Security Policy

This is a community reverse-engineering / firmware project maintained in personal time. It runs on
hardware **you own** and physically flash; it is **not a certified product** and ships Matter
**test** credentials for development use only (see [`NOTICE.md`](NOTICE.md)).

## Supported versions

Only the **latest release** of each firmware target is supported:

| Target | Tag prefix |
|--------|------------|
| AmebaZ2 (RTL8710C) module firmware | `amebaz2-vX.Y.Z` |
| ESP32 fallback bridge | `esp32-vX.Y.Z` |

Older tags receive no fixes — update to the newest release before reporting.

## Reporting a vulnerability

**Please report privately — do not open a public issue for a security bug.**

- Preferred: **GitHub → Security → "Report a vulnerability"** (private advisory) on this repo.
- Or email **hello@andreasincode.com** with `[security] hisense-w41h1` in the subject line.

Include the affected component + version/commit, reproduction steps, and the impact; a
proof-of-concept helps. Please allow a reasonable window to fix before any public disclosure.

**Response expectations** (best-effort, personal project): acknowledgement within ~7 days, an
initial assessment within ~30 days.

## Scope

In scope — defects in this repo's **own** code:

- the RS-485 driver, Matter mapping, and firmware glue (`firmware/src/`)
- build/OTA scripts (`firmware/scripts/`) and the recon tools (`reverse-engineering/tools/`)
- the CI workflows (`.github/workflows/`)

Out of scope:

- the upstream **Realtek AmebaZ2 SDK**, **Espressif ESP-IDF / esp-matter**, and
  **connectedhomeip** — report those to their respective projects
- the **Hisense stock firmware / ConnectLife cloud** — that's the *subject* of the
  reverse-engineering, not software maintained here
- anything requiring the attacker to already have **physical flash access** — that is the threat
  model this project operates under, by design
- the use of Matter **test** DAC/PAI/CD credentials — a documented, intentional property (the
  device is uncertified and not for sale), not a vulnerability

## A note on secrets

Raw flash dumps, device keys, and Wi-Fi credentials are git-ignored and must never be committed
(see [`dumps/README.md`](dumps/README.md)). If you believe a secret was committed to history,
please report it privately as above rather than opening a public issue.
