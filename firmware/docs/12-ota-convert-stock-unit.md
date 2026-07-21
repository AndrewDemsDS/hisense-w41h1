# 12: Convert a STOCK W41H1 to custom firmware, purely over-the-air (no CH341A)

**Proven on hardware 2026-07-09** (the "kitchen" unit). A second, still-stock `AEH-W41H1`
was converted to our v23 Matter firmware **entirely over Wi-Fi/BLE, no SPI clip, no
disassembly**, then commissioned into Home Assistant alongside the first unit.

This is the fleet-conversion path (issue F2). The CH341A flash ([`10`](10-firmware-ota-procedure.md))
remains the *recovery* fallback if an OTA attempt bricks a unit, but is no longer required for
the first conversion.

## Why it works

The stock W41H1 firmware is a barely-customized `connectedhomeip` example: it has a **live,
commissionable Matter interface** (test passcode `20202021`) **and an OTA Requestor cluster
(`0x2A`)**, reporting `VendorID 5004 / ProductID 13825 / SoftwareVersion 4 / ProductName
TEST_PRODUCT`. So we can commission it with a controller, hand it our firmware image over the
Matter OTA (BDX) transport, and its AmebaZ2 image processor writes+boots it, the partition
layout matches because our firmware is built on the same SDK.

## The four traps (each cost an attempt)

1. **Attestation `err 604`**: the stock CD/DAC/Basic-Info VendorIDs don't cross-reference, so a
   normal controller (incl. `python-matter-server` with `--enable-test-net-dcl`) rejects it.
   → Commission with **`chip-tool --bypass-attestation-verifier 1`** (needs a source build; the
   Python stack has no bypass hook).
2. **Operational `CHIP Error 0x32 Timeout`**: after the device joins Wi-Fi, the controller must
   reach it to finish CASE. Cross-VLAN IPv6 mDNS does **not** work. → The controller **must be on
   the same L2** as the device (join the IoT SSID) for commissioning *and* the OTA.
3. **`CommissioningComplete` → `IM 0x0501` FAILURE**: a transient on the very last handshake.
   → **Just retry the commission**; it succeeded on the 2nd try.
4. **vid/pid mismatch**: our stock `.ota` header is `0xFFF1/0x8001`; the device is `5004/13825`,
   so the provider won't offer it and the requestor would reject the header. → **Repackage the
   `.ota`** with a header matching the *target* (`ota_image_tool.py`), same payload.

## Prerequisites (one-time)

- Host `chip-tool` + `chip-ota-provider-app` built from `~/ameba-dev/connectedhomeip`.
  On a bleeding-edge GCC (16.x) disable `-Werror`:
  ```
  cd ~/ameba-dev/connectedhomeip && source scripts/activate.sh
  (cd examples/chip-tool          && gn gen out/host --args='treat_warnings_as_errors=false is_debug=false' && ninja -C out/host)
  (cd examples/ota-provider-app/linux && gn gen out/host --args='treat_warnings_as_errors=false is_debug=false' && ninja -C out/host)
  ```
- The current `.ota` image (`built-images` / the provider dir), e.g. `rac-v23.ota`.
- A laptop with Bluetooth, able to join the target's IoT SSID (BLE range of the unit).

## Procedure

Everything below is automated in
[`firmware/scripts/ota_convert_stock.sh`](../scripts/ota_convert_stock.sh); this is the *why*.

1. **Put the laptop on the device's L2**: join the IoT SSID the unit will use:
   `nmcli dev wifi connect your-iot-ssid password <pw>` (trap #2).
2. **Open the stock pairing window**: press **"77"** on the unit (swing ×6).
3. **Commission with attestation bypass** (trap #1); retry on `0x0501` (trap #3):
   ```
   chip-tool pairing code-wifi <NODE> <IoT-SSID> <pw> 34970112332 \
     --bypass-attestation-verifier 1 --storage-directory <store>
   ```
4. **Read identity + confirm the requestor exists** (go/no-go):
   ```
   chip-tool basicinformation read vendor-id       <NODE> 0 --storage-directory <store>
   chip-tool basicinformation read product-id      <NODE> 0 --storage-directory <store>
   chip-tool basicinformation read software-version <NODE> 0 --storage-directory <store>
   chip-tool descriptor read server-list           <NODE> 0 --storage-directory <store>   # expect 42 (OtaSoftwareUpdateRequestor)
   ```
   No cluster `42` → stop, this unit can't OTA; use the CH341A flash instead.
5. **Repackage the `.ota` to match the target vid/pid** (trap #4), keeping `min<=curVer<=max`:
   ```
   ota_image_tool.py extract rac-v23.ota payload.bin
   ota_image_tool.py create -v <VID> -p <PID> -vn 23 -vs "23.0" -mi 1 -ma 22 -da sha256 payload.bin target-v23.ota
   ```
6. **Serve it + drive the OTA**:
   ```
   chip-ota-provider-app --discriminator 22 --secured-device-port 5560 --KVS /tmp/prov.kvs --filepath target-v23.ota &
   chip-tool pairing onnetwork 1 20202021 --storage-directory <store>                     # commission provider as node 1
   chip-tool accesscontrol write acl '[{"fabricIndex":1,"privilege":5,"authMode":2,"subjects":[112233],"targets":null},{"fabricIndex":1,"privilege":3,"authMode":2,"subjects":null,"targets":null}]' 1 0 --storage-directory <store>
   chip-tool otasoftwareupdaterequestor write default-otaproviders '[{"fabricIndex":1,"providerNodeID":1,"endpoint":0}]' <NODE> 0 --storage-directory <store>
   chip-tool otasoftwareupdaterequestor announce-otaprovider 1 0 0 0 <NODE> 0 --storage-directory <store>
   ```
   Watch the provider log: `QueryImage → UpdateAvailable → BDX:Block …×N → BlockAckEOF →
   ApplyUpdateRequest version 23`. The unit then reboots into our firmware.
7. **Confirm it booted our firmware**: the unit drops IPv4 (our build is IPv6-only), stays
   associated to Wi-Fi, and **"77" now works** (F1). It boots uncommissioned.
8. **Commission into Home Assistant**: our firmware passes attestation normally (consistent test
   certs), so no bypass here. Press "77", then in HA: Matter → Add device → `3497-011-2332`.
   (Or `commission_with_code(code, network_only=True)` via the matter-server, which is dual-homed
   on the IoT VLAN, see [`../../reverse-engineering/docs/02`](../../reverse-engineering/docs/02-matter-local-control.md).)
9. **Back up the stock slot (do this once, right now)**: the stock image still sits intact in the
   inactive slot until a second custom OTA overwrites it. On custom firmware ≥ 1.3.9:
   ```
   ota-release.sh revert --backup <unit-ip>    # saves built-images/stock-backup-*.bin, validated
   ```
   Keep that file. It is the unit's way back to ConnectLife (`revert --repackage` +
   `revert --apply`, docs/10 §17 Path 2) even after later OTAs overwrite the stock slot.

## Verified result (kitchen unit → node 14)

`VendorID 5004→0xFFF1 · ProductID 13825→0x8001 · SoftwareVersion 4→23`, `available=True`,
climate attributes readable. Identical to a CH341A-flashed unit, but converted with zero
physical access.

## SendTrustedRootCert wedge: previously cloud-paired units (office unit, 2026-07-21)

A stock unit that was ever paired to a cloud/app fabric carries that fabric's KV entries
**through** the conversion (the Matter DCT regions survive an OTA by design). On the office
unit (stock `SoftwareVersion 2`, older than the kitchen's 4, and once paired to Google Home,
VendorID `0x6006`) that stock-era data left an **orphaned root-cert blob** the new firmware
half-reads: certificate present in the cert store, no matching fabric-table entry.

**Symptom:** every commission attempt fails at commissioning step `SendTrustedRootCert` with
`IM Error 0x00000501: General error: 0x01 (FAILURE)`. Deterministic: every controller
(python-matter-server and chip-tool), every transport, survives reboot. Do not confuse it
with trap #3 above (same IM error, but at `CommissioningComplete`, and transient).

**Why it is unfixable at the Matter level:** fabric indices allocate monotonically, so the
next index permanently points at the orphaned slot; `PersistentStorageOpCertStore` then
rejects `AddTrustedRootCertificate` with `INCORRECT_STATE` because a cert already exists at
that index. No cluster command reaches certs whose index is not in the fabric table
(`remove-fabric` of the *visible* stale fabric is necessary hygiene but not sufficient; the
office unit still wedged with `CommissionedFabrics: 0`). Diagnosis without UART: PASE into
the open window grants admin, so `chip-tool interactive` + `pairing code-paseonly` +
`operationalcredentials read fabrics ... --fabric-filtered 0` shows the stale fabric table.

**Fix (firmware >= 1.3.16):** the `:wipekv` break-glass command
([docs/10 §13](10-firmware-ota-procedure.md)) formats both Matter DCT regions and reboots.
Recovery loop used on the office unit, no physical access beyond "77" presses:

1. `<token>:revert` boots the intact stock slot. Expect stock to come back **without Wi-Fi**:
   its Matter-provisioned credentials lived in the (custom-fw-clobbered) DCT, and the old
   `0x2FF000` profile points at the ConnectLife-era network.
2. "77" + step 3 of the procedure above re-commissions stock over BLE (re-provisions Wi-Fi).
3. Step 5-6 OTA a `:wipekv`-capable image (repackaged `5004/13825`, version > the wedged one).
4. `<token>:wipekv`, then re-commission on the fresh KV. After the wipe the device is in BLE
   commissioning mode with Wi-Fi down, so commission from a laptop in BLE range
   (`chip-tool pairing code-wifi ...`), open a commissioning window, and hand off to
   the matter-server (`commission_with_code`, `network_only=True`); `pairing unpair` the
   temporary chip-tool fabric once HA is in.

Result: office unit = node 62, exactly one fabric (HA), `softwareVersion 10316` verified by
live `read_attribute`. If converting more units with a cloud history, ship a `:wipekv`-capable
image in step 5 the first time and run `:wipekv` right after step 7, before commissioning.
