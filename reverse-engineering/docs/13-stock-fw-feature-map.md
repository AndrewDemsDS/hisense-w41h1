# Stock firmware feature map (fw1, `dumps/w41h1_dump1.bin`)

Synthesis of the six raw analysis notes (`analysis/cloud-jcmd-tls.md`, `ota-handler.md`,
`local-attack-surface.md`, `matter-stack.md`, `kv-sysdata-layout.md`, `strings-census.md`).
Claims below were cross-checked across the notes; where they disagreed, the note with
byte-exact recomputation or pointer-pool evidence wins. Addresses are XIP runtime
addresses unless tagged as file/flash offsets.

> **BASE CORRECTION (load-bearing for all xref work):** fw1 executes at XIP base
> **`0x9b6e0000`** (= flash base `0x9b6d0000` + slot offset `0x10000`), with a second
> alias window at **`0x9b000000`** used by the Hisense app core (AT handlers, BLE
> provisioning). The previously recorded base `0x9b7d0000` is **wrong**: string-pointer
> pools only resolve at `0x9b6e0000` (verified on ~20 independent pools in three
> analyses). file offset = XIP − `0x9b6e0000`.
>
> **fw2 (flash `0x190000`) is NOT the same firmware as fw1.** It is an older factory
> MP-test build: version `S1798.MP_TEST_VERSION_SE`, built `Aug 15 2023`, **no Matter /
> CHIP strings at all**, MP-only commands (`ATSG` GPIO test, `HS_OTA_TEST`), and a
> hardcoded HOTA URL
> `http://download.hismarttv.com/Content/WifiDeviceVersionFile/154003147849100920.bin`.
> Nearly every 16-byte block differs from fw1; FWHS size fields differ; both slots
> carry serial 100.

## Identity / build IDs

| Item | Value | Evidence |
|---|---|---|
| Module SW version (`version_mp`) | `S2292.6.02.08.MK` | fw1 file `0x1242d6` |
| fw1 build date | `May 26 2025` (`2025/05/26-14:16:37`) | fw1 file `0x125b88` / `0x13366b` |
| Bootloader build | `Apr 17 2023 15:19:48` | sysdata off `0xbc98` |
| Matter base | connectedhomeip `thermostat` example, SDK drop `matter_1102` | source paths `/home/hisense/lyd/matter_1102/ambz2_matter/...` at fw1 file `0x1481fc+` |
| Default softAP SSID | `HIS-8888` | fw1 file `0x866`, string `0x9b80a134` |

`version_ext` is a second version concept: the A/C mainboard MCU version, fetched over
the RS-485 bus.

## Cloud channel: jcmd v5 over pinned TLS

- Long-lived HTTPS (TLS 443) chunked-transfer stream to `ac-eu-link.hijuconn.com`
  (host composed at runtime as `<env>-link.hijuconn.com`; host and port stored in the
  cloud config record at flash `0x3DB061` / `0x3DB094`). RX chunks feed
  `cloud_handle_package` → `jcmd_parse_msg`.
- **TLS verification is pinned-CA `VERIFY_REQUIRED`.** `check_crt_method` picks: 1 =
  built-in CA (fresh units), 2/3 = cloud-fetched pemA/pemB slots (A/B ping-pong,
  stored AES-encrypted + base64 in flash). All three end at
  `mbedtls_ssl_conf_authmode(REQUIRED)` with a CA chain configured.
  **Method 0 (`VERIFY_NONE`) is unreachable dead code**: it needs RAM byte
  `0x10009050 == 6`, and no writer of value 6 exists in fw1. MITM of the gateway link
  is not possible without the pinned CA's private key (doc 08 reached the same verdict
  from the network side, see [08-cloud-mitm-feasibility.md](08-cloud-mitm-feasibility.md)).
- **CA rollover without bricking:** on handshake failure with method 1 (or a state
  flag at connect), `pem_update` POSTs signed JSON to
  `test-clife-eu-gateway.hijuconn.com/crt/fetchLastestGa` and stores fresh CAs into
  the two pem slots.
- **jcmd v5 wire format:** `"HKEYV2-" base64( AES-256-CBC( json ‖ ascii-hex(HMAC-SHA256(dkey, json)) ) ) "\n"`.
  AES key = the 32-byte per-device **dkey**, IV = first 16 bytes of the dkey,
  zero-padded. The v5 flag (`0x1000c124`) is set by the `CMD_SENDENCKEY` provisioning
  handler (44-char base64 payload = 32 raw bytes); before key delivery, jcmd falls
  back to plaintext JSON. dkey persists in the cloud config record (flash `0x3DB1B9`)
  and is unpacked at boot through a "hi-aes" layer.
- **No device-initiated version poll.** The only REST path in fw1 is
  `/crt/fetchLastestGa` (CA fetch). The device passively reports devinfo/version; the
  cloud decides and pushes `CLOUD_JSON_UPDATE` (version, URL, SHA-256). Firmware file
  IDs are only visible to the phone app / cloud.

## OTA image acceptance (fully decoded, byte-exact verified)

- FWHS `+0x00` (32 B) = **HMAC-SHA256 over the 0x60-byte manifest `img[0xE0:0x140]`**,
  keyed by the partition-table **hash_key** (flash `0x140` for FW1, `0x180` for FW2;
  the SDK default `0001…1e5f`, cleartext in the partition table). It covers the serial
  and sizes, **not** the image body. Recomputation matches the stored signature on
  fw1, fw2, and `firmware_is-v10307.bin`.
- **Serial at manifest `+0xF4`** (LE u32, 100 on both stock slots): the bootloader
  boots the signature-valid slot with the highest serial.
- **Transport checksum trailer:** last 4 bytes = u32 LE byte-sum of everything before
  it (appended by `checksum.linux`). Enforced by the OTA code (`USE_CHECKSUM=1`)
  against the backed-up real first 32 bytes. fw1's in-slot length: `0x16ab40` + 4.
- The device **never computes the signature**: `update_ota_signature` replaces the
  first 32 bytes with `0xFF` during download and writes the saved bytes back after the
  checksum passes. The served file must already carry a valid HMAC.
- **No cryptographic signature anywhere.** The `+0x20` "pubkey" field is the SDK
  example key on every image; secure boot is not enabled. The download itself is plain
  HTTP with a cloud-supplied SHA-256 (integrity only). See
  [07-firmware-and-ota.md](07-firmware-and-ota.md).
- Delivery paths: cloud push, UART console `HOTA=<url>,<sha-256>` and `ATWO` (raw TCP,
  no auth), Matter OTA. `DOTA=` updates the A/C mainboard over RS-485, not the module.

## Local network surface

Exhaustive `bind()` census of fw1 (every call site resolved; only two `listen()`
sites exist in the image):

| Listener | Port | When up | Notes |
|---|---|---|---|
| softAP provisioning server | TCP 5020 | pairing mode only | AP `HIS-8888`; plaintext jcmd fallback pre-dkey: set Wi-Fi creds, dkey, country code, cloud env |
| LAN discovery | UDP 5030 | always (cloud thread select loop) | scans datagram for `HS` at any offset; reply leaks identity fields unauthenticated; 0x65/0x66-gated path with 60 s penalty [OPEN] |
| self-advertisement beacon | UDP 5010 (dest) | ~60 s periodic | module beacons to the app listening port |
| Matter | UDP 5540 + mDNS 5353 | always / window-gated | IPv6; commissionable advertisement only while the window is open |
| iperf test servers | TCP/UDP 5001 (default) | while `ATWT`/`ATWU` runs | console-triggered, transient |

- **No telnet, SSH, MQTT, or hidden listener** anywhere; no hardcoded-credential
  network backdoor (the `abc`/`abcdefg` commands are inert UART test stubs).
- **Both AT consoles are UART-only** (case open): the AmebaZ2 SDK `log_service` set
  plus a Hisense custom table. Notable: `RKEY`/`WKEY` (read/write dkey),
  `WPEM`/`RPEM` (TLS PEM incl. device key), `WENV` (cloud host/port override),
  `HOTA` (OTA from arbitrary URL), `ATSE` (arbitrary MMIO write), `ATSC`/`ATSR`
  (clear / recover OTA slot signature), `ATWO`, `R485`/`RS485` (bus injection),
  `WMAC`/`WWID`/`WMOD` (identity/mode writes).

## Matter stack

- connectedhomeip AmebaZ2 **thermostat** example (CHIP ~1.0-era), Hisense-customized.
  Test credentials: passcode **20202021**, discriminator 3840 ([02-matter-local-control.md](02-matter-local-control.md)).
- **ep0 (root):** Descriptor, AccessControl, BasicInformation, GeneralCommissioning,
  NetworkCommissioning, GeneralDiagnostics, WiFiNetworkDiagnostics,
  AdministratorCommissioning, OperationalCredentials, GroupKeyManagement,
  **OtaSoftwareUpdateRequestor (0x2A)**, UserDirectedCommissioning.
- **ep1 (app):** Identify, Groups, **Thermostat 0x0201** bridged to the RS-485 bus
  (`hisense_to_matter` / `matter_uart_process`; bus protocol in
  [03-rs485-ac-protocol.md](03-rs485-ac-protocol.md)).
- **OTA Requestor has no requestor-side validation:** stock `ProcessHeader` only
  decodes the header for `totalFileBytes`; no vid/pid or version check gates the
  download. `ConfirmCurrentImage` runs after rebooting into the new image. All
  vid/pid/version filtering is provider-side, so whoever runs the provider sets the
  rules (this is both the conversion path and a revert path).
- **Commissioning window auto-opens for 900 s at boot whenever no fabric exists**
  (`OpenBasicCommissioningWindow(900, kAllSupported)`); with a fabric, BLE
  advertising is disabled. A fabric admin can re-open the window via
  AdministratorCommissioning with the same test passcode.
- **Cloud/Matter coexistence gate:** at cloud connect, if the configured cloud port is
  not `"443"`, the firmware prints "only matter running" and skips the cloud stack
  entirely. With the stock endpoint (443) both run concurrently off the same HS-core.

## Persistent storage map

Offsets and sizes only; no values. Full record layouts in
`analysis/kv-sysdata-layout.md`.

| Flash range | Content |
|---|---|
| `0x0–0x1000` | boot-image FWHS header + partition entries (hash_keys at `+0x140`/`+0x180`) |
| `0x1000–0x2000` | SoC sysdata sector; **SPI flash calibration record at `0x1060`** (self-heals if erased) |
| `0x4000–0xC000` | bootloader |
| `0x10000–0x180000` | FW1 slot (running stock app, serial 100) |
| `0x190000–0x260000` | FW2 slot (factory MP-test image, serial 100) |
| `0x2FF000–0x300000` | **stock Wi-Fi fast-reconnect profile** (`struct wlan_fast_reconnect`, `VERSION_V01`) |
| `0x3DA000` | provisioning event log (52 B ASCII entries) |
| `0x3DB000` | **cloud config record** (476 B, tag `0x1989`): host, port, wifi_id, dkey |
| `0x3DD000` | **device-identity record** (144 B, tag `0x1989`): model, uuid, key material |
| `0x3E0000–0x3EA000` | **Matter DCT2** (KVS2, fabric credentials; 10 × 4 KB modules) |
| `0x3ED000–0x3FA000` | **Matter DCT1** (KVS1, config/counters; 13 × 4 KB modules) |
| `0x3FC000` | BT FTL backup sector |

The custom Matter build reuses DCT1/DCT2 (stock Matter commissioning state is
overwritten) but never touches `0x2FF000`, `0x3DA000–0x3DE000` (verified by constant
scan of the deployed custom image).

## Strings census summary

fw1 yields 8879 strings; ~6450 are Thumb-2 false positives from interleaved
code/rodata. Approximate real counts: Wi-Fi/network ~556, Matter/CHIP ~398,
TLS/crypto ~358, BLE/BT ~294, cloud/jcmd ~256, RS-485/A-C bus ~193, AT console ~155,
OTA ~124, RTOS/HAL ~64, factory/MP test ~23, build provenance ~10.

Factory/test features: on-device TEST MODE (scan/BLE/RF/USART loopback with OK/Fail
verdicts), MP-test AT surface (`ATM?`/`ATM#`/bridge), fw2-only `HS_OTA_TEST` and GPIO
test, factory-sector auto-init on blank flash, dev/test cloud environment switching
(`dev-ac-`/`test-ac-` SSIDs, per-env pubkey). **The `ATSK` set burns secure-boot key
material to eFuse** (`ENC_KEY|HASH_KEY|SB_KEY|ROOT_KEY|ROOT_SEED|SEC_BOOT_EN`), one-way
and irreversible; never run it. Undocumented findings: the `SENDENCKEY` provisioning
key-push, a persisted `cloud_mode` flag gating cloud-vs-Matter, Matter QR/manual
pairing codes printed to the log UART, `WDOG TEST` and heap-low auto-reboot hooks,
RS-485 cmd `0x29` rebooting the module into DOTA mode. `t_*` capability keys map to
per-model gating ([11-model-capability-map.md](11-model-capability-map.md)).

## Revert-to-stock implications (GitHub issue #19)

1. **Slot-flip revert is safe.** The stock config regions `0x2FF000` (Wi-Fi profile),
   `0x3DB000` (cloud config + dkey) and `0x3DD000` (identity) stay byte-intact under
   the custom firmware (verified against `flash_is-v10211-DEPLOYED.bin`), so a
   reverted stock boot rejoins ConnectLife **without re-provisioning**. The stock
   Matter DCT state does not survive (the custom build reuses the same DCT
   addresses). Caveats: this only works while the other slot still holds stock
   (serial 100); a second custom OTA overwrites it. On virgin units the fallback slot
   holds the fw2 MP-test image, not the production app.
2. **Repackage revert recipe (proven end-to-end):** carve fw1 at flash `0x10000`,
   length `0x16ab40`; patch `*(u32le*)(img+0xF4)` to a serial above the running one
   (strictly greater); set `img[0:32] = HMAC-SHA256(00010203…1c1d1e5f, img[0xE0:0x140])`;
   append `u32le(bytesum(img))`. Verified byte-exact against fw1, fw2, and
   `firmware_is-v10307.bin`. Delivery via Matter OTA, `ATWO`, or `HOTA=`.
3. **The cloud channel cannot be spoofed** (pinned CA + per-device dkey), and **no
   public stock image exists**: the example file on `download.hismarttv.com` is from
   an older module generation with zero overlap. Official image IDs can only be
   harvested by MITM-ing the phone app (doc 08); the device never asks for them.

## Open questions

- Which writer (if any) can set state byte `0x10009050` to 6 (VERIFY_NONE path);
  treated as dead/test code.
- The UDP 5030 0x65/0x66-gated path: possible rate-limited A/C-control passthrough.
- Whether the '77' reset chain erases the Matter fabric KV; if not, previously
  commissioned units skip the auto-open window.
- Bootloader serial tie-break (both stock slots carry 100) and fw2's fallback role.
- Exact compare site of the `HOTA=` SHA-256 argument (bootloader does not need it).
