# OTA Updates

After the first CH341A flash, every firmware update ships **wirelessly over Matter OTA**, no clip.
The whole pipeline is one script; don't hand-build (the manual path has traps that ship a
rolling-back image).

> This page is for **updating an already-custom unit**. To convert a still-stock module to custom
> firmware in the first place, see [Installing the Custom Firmware](Installing-Custom-Firmware).

Deep reference: `firmware/docs/10-firmware-ota-procedure.md`.

## The one command

```
firmware/scripts/ota-release.sh release --bump --flash
```

This runs **build → package → stage → flash** end to end:

| Stage | What it does |
|---|---|
| `build [--bump]` | sync driver/glue mirror → SDK, **mandatory full clean**, compile, verify serial + endpoints. `--bump` increments the software version first. |
| `package` | pad the clip image to 4 MB, create the `.ota`, write the sidecar JSON manifest (size + SHA-256, so they can't drift). |
| `stage` | scp `.ota` + manifest to your Home Assistant host and **restart matter-server** (it only loads manifests at init). |
| `flash` | `update_node` with retries, then verify the device's *reported* version changed. |

Individual stages exist too (`lint`, `build`, `package`, `stage`, `flash`). Run
`ota-release.sh` with one of them. `lint` is the fast offline check the git pre-commit hook runs.
Environment-specific paths/hosts live in `ota-release.env` (gitignored; copy from `.env.example`).

## OTA is flaky by design: retry

`update_node` frequently returns **error 11 "Target node did not process the update file"** on the
first attempt(s): the provider is re-commissioned ephemerally each attempt and the target loses the
discovery race with the brand-new provider. It succeeds on retry. **Always retry 3–5×, ~15 s
apart**. The script does this for you. Verify the device's *reported* `softwareVersion` changed;
don't trust matter-server's "finished" (an A/B rollback reports the old version). This is a
widespread `python-matter-server` behavior, not a fault in the image.

## Version rule (get this wrong → the provider won't serve)

- The new build's version must be **strictly greater** than what's running, or the provider declines
  to serve it. Convention: **new = running + 1**.
- `--bump` edits both the int and string in `CHIPDeviceConfig.h`. Lint compares against
  `built-images/.released-version` (the version last **confirmed booted**), so it can't be fooled.
- **Don't reuse a rolled-back version number.** Matter-server and the device cache by version. If
  v14 rolls back, the next attempt is v15, not v14 again.

## The OTA-serial trap (the script handles it)

AmebaZ2's bootloader A/B-selects the boot slot by the image's **`FWHS.header.serial`**, **not** the
Matter `softwareVersion`. If the serial isn't bumped, an OTA transfers, applies, "finishes
successfully", and the device reverts to the old version on reboot (looks like a
rollback). This cost a whole debugging session. `ota-release.sh build` sets
`serial = SERIAL_BASE + softwareVersion` and log-verifies it, so **you never touch it, as long as
you use the script and don't hand-build.** The gory detail is in
`firmware/docs/10-firmware-ota-procedure.md` (§11).

## After the OTA: new entities

If the update changed the node structure (added/removed endpoints or clusters), HA won't show the
new entities until a **re-interview**. `flash` auto-interviews on success; if entities still lag,
reload the Matter integration in HA. See [Commissioning & HA Setup](Commissioning-and-HA-Setup).

## ESP32 delta OTA

Prefer a **delta (differential) patch** over a full image. It is ~50× smaller (86 KB against
1.6 MB on our build), so it spends far less time on a lossy link, which is where OTA fails.

On a device built with `CONFIG_ENABLE_DELTA_OTA=y` the delta is **mandatory**. A full image
transfers, reports "finished", and never applies. The device accepts only a patch built against
the **exact binary it is running** and verifies that base's SHA first, so a patch built from the
wrong base is refused and the device stays put.

```sh
# 1. patch: base = the EXACT running .bin, new = your build
#    needs detools + esptool -> run it with the IDF python env
esp_delta_ota_patch_gen.py create_patch --chip esp32 \
  --base_binary <exact-running.bin> \
  --new_binary build/hisense_ac_matter.bin \
  --patch_file_name x.patch

# 2. wrap the PATCH (not the .bin) as the Matter image
ota_image_tool.py create -v 0xFFF1 -p 0x8000 -vn <int> -vs <semver> -da sha256 x.patch out.ota
```

⚠️ **Archive the exact deployed `.bin` before you rebuild.** It is the delta base, ESP-IDF builds
are not byte-reproducible, and `build/` gets overwritten. Lose it and the node is USB-flash only
until you reflash it once by cable. We lost one base that way.

The manifest `.json` must wrap its fields in a top-level `"modelVersion": {...}` object. A flat
manifest raises `KeyError: 'modelVersion'` and **crash-loops matter-server at init**, taking every
node offline. Copy the shape from a manifest that already works.

## Break-glass: manual HTTP OTA when Matter OTA will not finish

Matter OTA runs over BDX, which is stop-and-wait: one ~1 KB block per round trip. On a marginal
link it stalls and you get `error 11`. Both firmwares carry a manual fallback that pulls the image
over plain TCP instead, which has a sliding window and fast retransmit and completes where BDX
gives up. It is a deliberate escape hatch, not the primary path, and it bypasses the Matter version
negotiation entirely.

**Trigger:** write `Identify.IdentifyTime = 88` on endpoint 1.

**Serve the raw `firmware_is.bin`**, not the `.ota` wrapper. The AmebaZ2 fallback streams the body
through the same writer the Matter OTA uses, which expects the ameba payload without the Matter
envelope.

Two things decide whether this works, and both are silent when wrong:

- **The image's FWHS serial must exceed the running slot's.** `ota-release.sh build` sets it. Serve
  a stale file and the download succeeds, the write succeeds, and the device boots the old firmware
  anyway.
- **The file server must be reachable FROM the device's VLAN.** This is the one that bites. If your
  IoT VLAN cannot route to your LAN, an address on the LAN will never be reached. The AmebaZ2
  client is IPv4-only, so an IPv6-only address on that VLAN does not help either: give the server an
  IPv4 address on the VLAN the device is on.

The address is **not** compiled in from the repo. Set `OTA_HTTP_HOST` (plus `OTA_HTTP_PORT`,
`OTA_HTTP_RESOURCE`) in `ota-release.env`, and `build` injects it. Leave it unset and the firmware
keeps a placeholder that goes nowhere, which is intentional: an unconfigured build must not carry
somebody else's server address.

Verified end to end on an AmebaZ2 unit: the device fetched over TCP, wrote the idle A/B slot and
rebooted into the new version, confirmed across three fresh attribute reads.

## No infra? Use the clip

The CH341A clip path is the recovery route if OTA is unavailable. `package` also produces the 4 MB
`flash_rac-integrated-v<N>.bin` for it. See [Recovery & Reflash](Recovery-and-Reflash).

## Converting a still-stock unit over the air

A **stock** W41H1 can be converted to custom firmware with **no clip at all**. Its stock firmware
already has a live Matter interface + OTA requestor. Full walk-through:
[Installing the Custom Firmware](Installing-Custom-Firmware) (Path A), with the step-by-step *why* in
`firmware/docs/12-ota-convert-stock-unit.md`.
