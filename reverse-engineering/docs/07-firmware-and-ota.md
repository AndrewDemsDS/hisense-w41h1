# Firmware update paths and custom-firmware feasibility

How the vendor firmware updates itself, what it checks on an image, and the first assessment
(July 2026) of whether a custom firmware could be installed on the stock module.

## Status of this page

History, with the answer added. The assessment below concluded "probably possible, one gate
cannot be checked from a dump". It has since been answered on hardware: the stock module boots
custom firmware, and secure boot is not enforced. The module first ran the project's Matter
firmware (retired, tag `matter-final`) and now runs ESPHome through LibreTiny, with the
`w41h1_module` component. See
[`firmware/esphome/README.md`](../../firmware/esphome/README.md#the-stock-module-amebaz2) for the
build and
[`firmware/docs/10-stock-module-boot-and-recovery.md`](../../firmware/docs/10-stock-module-boot-and-recovery.md)
for how the module boots and how to recover it.

Two statements of the first assessment turned out wrong and are corrected in place below:

- "Integrity is a checksum and a SHA-256 only." The image also carries two HMAC-SHA256 values
  keyed by a key that sits in clear text in the partition table, and the bootloader enforces one
  of them. A wrong one leaves the module unable to boot. The exact rules are in
  [13-stock-fw-feature-map.md](13-stock-fw-feature-map.md#ota-image-acceptance-fully-decoded-byte-exact-verified).
- "There is no ESPHome build for this chip." There is one now, in this repository.

Everything in the tables below was read from the firmware.

## Firmware layout

- The SoC is the RTL8710C (AmebaZ2), executing in place from the external SPI flash (GD25Q32,
  4 MB), mapped at base `0x9b6d0000`.
- Two firmware slots: FW1 at about `0x000000` to `0x178000` in this first reading, FW2 at about
  `0x190000` to `0x260000`. The Wi-Fi and cloud settings and the key-value store are near the top
  (`0x3da000` and above). The precise map is in
  [doc 13](13-stock-fw-feature-map.md#persistent-storage-map), which puts FW1 at `0x10000`.
- The bootloader picks the valid slot (`sys_update_ota_set_boot_fw_idx`).
- The flash is not encrypted. All code and strings are plain text in a dump, so RSIP flash
  encryption is off.

## Update paths

| Path | What it updates | Transport | Integrity |
|---|---|---|---|
| HOTA | the Wi-Fi module | HTTP download, `HOTA=<url>,<sha-256>`, default host `download.hismarttv.com/.../WifiDeviceVersionFile/<id>.bin` | SHA-256 of the image, as a parameter |
| ATWO (`_AT_WLAN_OTA_UPDATE_`) | the module | the AmebaZ2 SDK's update over the debug UART console, `ATWO=IP[PORT]` or a URL, into the inactive slot | the AmebaZ2 image checksum and the slot's validity marker |
| Matter OTA | the module | the connectedhomeip OTA Requestor (`QueryImage`, `ApplyUpdate`, BDX) from a Matter OTA provider | the Matter image checks |
| DOTA | the A/C mainboard, not the module | over the RS-485 UART | crc32 and md5 |

The module's update code (`http_update_ota`, `update_ota_local`, `update_ota_signature`) uses the
AmebaZ2 image checksum and a slot "signature" (`flash checksum ... attached checksum`, "The
checksum is wrong!"). That "signature" is the first 32 bytes of the image, an HMAC over the
manifest (doc 13). It is not a signature made with a private key, and no vendor public key is
used to verify an image.

## The first assessment: can custom firmware be installed?

The evidence for, as it stood:

- The flash is not encrypted.
- No signature with a private key protects an image. (The assessment then said "checksum and
  SHA-256 only". The two HMACs were found later.)
- The transport is open: `ATWO` and `HOTA` take any URL.

The gate that could not be checked was secure boot in the bootloader, an RSA or ECDSA check
against a key hash in eFuse. A flash dump does not show that. Plain-text flash and an update check
without a private key are what a device without enforced secure boot looks like, and the only way
to be sure was to flash a custom image and see whether it boots. It boots.

Practical points raised then:

- A valid AmebaZ2 image (partition table, manifest, checksum) has to be produced with Realtek's
  SDK or the `amebazii` tool.
- LibreTiny's support for the RTL8710C was experimental, so there was no ready ESPHome build.
  (The project's stock-module build uses LibreTiny today, with workarounds that are listed in
  `firmware/esphome/README.md`.)
- Delivery without opening the case looked hard: `ATWO` and `HOTA` are console commands on the
  debug UART, and the cloud's own update command cannot be forged, because the command channel is
  encrypted and the cloud supplies the SHA-256 for its own image. (Doc 13 later found a way in
  that needs no console: the vendor firmware's Matter OTA Requestor checks nothing on its side,
  so whoever runs the provider decides what is installed.)

The verdict then: a research path, high effort, no benefit over the vendor firmware's Matter
interface or an ESP32 replacement board. That judgement changed once the module was shown to
boot custom firmware.

The firmware embeds TLS client credentials (an RSA private key and a certificate) for the
ConnectLife connection. They are not reproduced anywhere in this repository, and they are one
more reason a raw dump stays private.
