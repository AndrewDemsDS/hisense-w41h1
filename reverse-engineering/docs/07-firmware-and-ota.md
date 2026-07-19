# Firmware update (OTA) & custom-firmware feasibility

> **Answered empirically since this analysis was written.** The "plausible, probably possible"
> verdict below has been **proven on hardware**: a stock W41H1 is now converted to custom Matter
> firmware and runs it, full stock→custom OTA conversion procedure in
> [`firmware/docs/12-ota-convert-stock-unit.md`](../../firmware/docs/12-ota-convert-stock-unit.md).
> The original feasibility RE is kept below for provenance.

## Firmware layout

- SoC **RTL8710C (AmebaZ2)**, XIP from external SPI flash (GD25Q32, 4 MB), base
  `0x9b6d0000`.
- **Dual OTA slots** (`fw1` / `fw2`): FW1 ≈ `0x000000–0x178000`, FW2 ≈ `0x190000–0x260000`;
  Wi-Fi/cloud config + KV/FTL near the top (`0x3da000+`). The bootloader picks the valid
  slot (`sys_update_ota_set_boot_fw_idx`).
- **The flash is NOT encrypted**: all code and strings are plaintext in a dump, so RSIP
  flash encryption is not enabled.

## Update paths (four of them)

| Path | What it updates | Transport | Integrity |
|------|-----------------|-----------|-----------|
| **HOTA** | the Wi-Fi module itself | HTTP download: `HOTA=<url>,<sha-256>` (default host `download.hismarttv.com/…/WifiDeviceVersionFile/<id>.bin`) | **SHA-256** of the image (a hash param, *not* a crypto signature) |
| **ATWO** (`_AT_WLAN_OTA_UPDATE_`) | the module | AmebaZ2 SDK OTA over the **debug UART console**; `ATWO=IP[PORT]` or a URL → downloads to the inactive slot | AmebaZ2 image **checksum** + slot validity marker |
| **Matter OTA** | the module | connectedhomeip **OTA Requestor** (`QueryImage`/`ApplyUpdate`/BDX) from a Matter OTA Provider | Matter software-image checks |
| **DOTA** | the **A/C mainboard** (not the module) | over the **RS-485/UART** to the subsystem | crc32 + md5 |

The module-side OTA (`http_update_ota` → `update_ota_local` → `update_ota_signature`) uses
the AmebaZ2 image **checksum** and a slot "signature" **marker** (`flash checksum … attached
checksum`, "The checksum is wrong!"). "Signature" here is the AmebaZ2 OTA slot-validity
tag, **not** an app-level cryptographic signature. No stored vendor public key is used to
verify OTA images.

## Can we install custom firmware OTA?

**Plausible, with one unverifiable gate.** Evidence *for*:
- Flash is unencrypted (no RSIP key barrier).
- OTA integrity is **checksum + SHA-256 only: no cryptographic signature** at the app
  level, so a correctly-checksummed custom image would be accepted by the OTA logic.
- The transport is open: `ATWO`/`HOTA` take an arbitrary URL.

The remaining gate is **bootloader secure boot** (RSA/ECDSA verify against a public-key
hash burned in **eFuse**). That bit **can't be read from a flash dump**: but a plaintext
flash + checksum-only OTA are the classic signature of a device with secure boot **not
enforced**. The only way to be 100% sure is to try flashing a minimal custom AmebaZ2 image
and see if it boots.

### Practical reality
- **Build tooling:** you'd need to produce a valid AmebaZ2 image (partition table +
  manifest + checksum) with the Realtek AmebaZ2 SDK or the `amebazii` tool.
- **No ready-made open firmware:** RTL8710C/AmebaZ2 support in LibreTiny (the
  ESPHome/Tasmota-on-off-brand-chips project) is experimental/incomplete, so there's no
  drop-in ESPHome build for this chip, you'd port firmware yourself.
- **Delivery without opening the case is hard:** `ATWO`/`HOTA` are debug-UART console
  commands (case open). The cloud-initiated HOTA can't be hijacked easily (the command
  channel is AES-CBC-encrypted, and the cloud supplies the SHA-256 for *its* image).

### Verdict
Custom firmware is a **research-grade path**: probably technically possible, but high
effort (port firmware for AmebaZ2, open the case for UART, confirm secure boot is off) for
no benefit over the two paths that already give full local control, **Matter** (stock
firmware) and the **ESP32 replacement**. Recommended only as a hobby / reuse-the-hardware
exercise, not the route to local control.

> Security note: the firmware embeds TLS client credentials (an RSA private key + a
> certificate) used for the ConnectLife cloud connection. These are **not** reproduced here
> and are another reason the raw dump stays private.
