# 12 — Convert a STOCK W41H1 to custom firmware, purely over-the-air (no CH341A)

**Proven on hardware 2026-07-09** (the "kitchen" unit). A second, still-stock `AEH-W41H1`
was converted to our v23 Matter firmware **entirely over Wi-Fi/BLE — no SPI clip, no
disassembly** — then commissioned into Home Assistant alongside the first unit.

This is the fleet-conversion path (issue F2). The CH341A flash ([`10`](10-firmware-ota-procedure.md))
remains the *recovery* fallback if an OTA attempt bricks a unit, but is no longer required for
the first conversion.

## Why it works

The stock W41H1 firmware is a barely-customized `connectedhomeip` example: it has a **live,
commissionable Matter interface** (test passcode `20202021`) **and an OTA Requestor cluster
(`0x2A`)**, reporting `VendorID 5004 / ProductID 13825 / SoftwareVersion 4 / ProductName
TEST_PRODUCT`. So we can commission it with a controller, hand it our firmware image over the
Matter OTA (BDX) transport, and its AmebaZ2 image processor writes+boots it — the partition
layout matches because our firmware is built on the same SDK.

## The four traps (each cost an attempt)

1. **Attestation `err 604`** — the stock CD/DAC/Basic-Info VendorIDs don't cross-reference, so a
   normal controller (incl. `python-matter-server` with `--enable-test-net-dcl`) rejects it.
   → Commission with **`chip-tool --bypass-attestation-verifier 1`** (needs a source build; the
   Python stack has no bypass hook).
2. **Operational `CHIP Error 0x32 Timeout`** — after the device joins Wi-Fi, the controller must
   reach it to finish CASE. Cross-VLAN IPv6 mDNS does **not** work. → The controller **must be on
   the same L2** as the device (join the IoT SSID) for commissioning *and* the OTA.
3. **`CommissioningComplete` → `IM 0x0501` FAILURE** — a transient on the very last handshake.
   → **Just retry the commission**; it succeeded on the 2nd try.
4. **vid/pid mismatch** — our stock `.ota` header is `0xFFF1/0x8001`; the device is `5004/13825`,
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

1. **Put the laptop on the device's L2** — join the IoT SSID the unit will use:
   `nmcli dev wifi connect your-iot-ssid password <pw>` (trap #2).
2. **Open the stock pairing window** — press **"77"** on the unit (swing ×6).
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
7. **Confirm it booted our firmware** — the unit drops IPv4 (our build is IPv6-only), stays
   associated to Wi-Fi, and **"77" now works** (F1). It boots uncommissioned.
8. **Commission into Home Assistant** — our firmware passes attestation normally (consistent test
   certs), so no bypass here. Press "77", then in HA: Matter → Add device → `3497-011-2332`.
   (Or `commission_with_code(code, network_only=True)` via the matter-server, which is dual-homed
   on the IoT VLAN — see [`../../reverse-engineering/docs/02`](../../reverse-engineering/docs/02-matter-local-control.md).)

## Verified result (kitchen unit → node 14)

`VendorID 5004→0xFFF1 · ProductID 13825→0x8001 · SoftwareVersion 4→23`, `available=True`,
climate attributes readable. Identical to a CH341A-flashed unit — but converted with zero
physical access.
