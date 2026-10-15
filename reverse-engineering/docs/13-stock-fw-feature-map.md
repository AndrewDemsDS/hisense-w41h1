# Stock firmware feature map

Everything the vendor firmware of the AEH-W41H1 does besides driving the A/C bus: its cloud
channel, the rules an update image has to satisfy, what it listens on, its Matter stack, and where
it keeps its settings in flash. Read it before reflashing a module, before building an image for
one, and to know what a module on the vendor firmware exposes on your network.

## Status of this page

Current reference for the vendor firmware, with one history section.

- All of it is static analysis of one flash dump, `dumps/w41h1_dump1.bin` (slot fw1), which is
  not in the repository. It merges six analysis notes (`cloud-jcmd-tls`, `ota-handler`,
  `local-attack-surface`, `matter-stack`, `kv-sysdata-layout`, `strings-census`). Those notes
  quote the dump, so they are kept with it and are not published. Where the notes disagreed, the
  one with a byte-exact recomputation or pointer-pool evidence was followed.
- The image acceptance rules are the exception: they were recomputed on real image files (37 and
  29 files, as stated there), and their omission bricked a module on 2026-07-21, which confirmed
  the bootloader's check the hard way. They are implemented in
  [`firmware/scripts/amebaz2_image.py`](../../firmware/scripts/amebaz2_image.py) and tested by
  `firmware/test/test_image_chain.py`.
- Nothing about the cloud channel or the listeners was tested against a live module.
- "Revert-to-stock implications" is about the project's retired Matter firmware and carries a
  notice.
- The open questions at the end are also in [14-open-questions.md](14-open-questions.md).

Addresses are runtime addresses unless marked as file or flash offsets.

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
| Matter base | connectedhomeip `thermostat` example, SDK drop `matter_1102` | the vendor's build paths, ending `matter_1102/ambz2_matter/...`, at fw1 file `0x1481fc+` |
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

There are **two** HMAC relationships, both keyed by the same partition-table **hash_key**
(flash `0x140` for FW1, `0x180` for FW2; the SDK default `0001…1e5f`, cleartext in the
partition table). A re-signing recipe must reproduce **both**, in this order.

- **(a) Manifest signature.** FWHS `+0x00` (32 B) = **HMAC-SHA256 over the 0x60-byte
  manifest `img[0xE0:0x140]`**. It covers the serial and sizes, **not** the image body.
  Enforced by the OTA code and used for slot validity.
- **(b) Inner image HMAC, one per sub-image.** A hash-then-trailer at a **content-derived**
  offset, so fixed-offset searching does not find it. **Enforced by the bootloader**, not by
  the OTA code. The general rule, for sub-image `i` whose header sits at offset `H` (the first
  is at `H = 0xE0`):

  ```
  S     = u32le(img[H])         # segment SIZE
  END   = H + 0x60 + S          # the trailer offset
  START = 0 if i == 0 else H
  img[END : END+0x20]  ==  HMAC-SHA256(hash_key, img[START:END])
  next header = H + u32le(img[H+4])      # RELATIVE; 0xFFFFFFFF terminates the chain
  ```

  **Naming trap.** `u32le(img[0xE0])` is the SIZE field `S`. It is **not** `next_img`.
  `next_img` lives at `H+4` and is a **relative** offset. An earlier revision of this section
  and of the old release script called `img[0xE0]` "next_img"; the code read the right offset so it
  worked, but generalising the wrong name to sub-images 1..N puts the trailer at
  `0x4060` instead of `0x3d80` on a real image, and every check then reports MISMATCH.

  Because sub-image 0's span starts at image offset 0, it is the only one that covers the
  serial at `+0xF4` and the `+0x00` signature from (a), so a serial patch stales exactly that
  one trailer. Verified on 37 real files: every image has 3 sub-images. Custom builds are
  `0xE0 / 0x4000 / 0xF8000`. Stock has **two** layouts in the wild,
  `0xE0 / 0x8000 / 0x114000` (one of the owner's modules, 0x1589c0 bytes) and `0xE0 / 0x8000 / 0x120000`
  (the module of `w41h1_dump1`, 0x16ab40 bytes), so **walk the chain, never hardcode offsets**.
  The last trailer does **not** land at EOF: `elf2bin` appends 0 or 0x20 bytes of `0x87`
  padding (both occur across the archive) before the 4-byte byte-sum, so an
  `END+0x20 == len` assert rejects most genuine images.

  Omitting (b) is what bricked a unit on 2026-07-21 (issue #75): every host-side check
  still passes, but `boot_load` prints `"Hash Result Incorrect!"`, clears the flash
  Quad-Enable bit, and hangs, with **no fall-back to the other slot**. Verified across 29
  real images: (b) holds on every genuine image and failed on exactly the four payloads
  the old `revert --repackage` produced.
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

The project's Matter build (retired, tag `matter-final`) reused DCT1/DCT2 (stock Matter
commissioning state is overwritten) and never touched `0x2FF000`, `0x3DA000–0x3DE000` (verified by constant
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

> **Retired.** This section is about going back from the project's Matter firmware to the vendor firmware. It was retired on 2026-10-12, and the last tree with it is the tag
> `matter-final`. See
> [`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md).
> The image rules it relies on are current and are in "OTA image acceptance" above. How a module on
> the ESPHome firmware is recovered is in
> [`docs/guide/Recovery-and-Reflash.md`](../../docs/guide/Recovery-and-Reflash.md).

1. **Slot-flip revert is safe.** The stock config regions `0x2FF000` (Wi-Fi profile),
   `0x3DB000` (cloud config + dkey) and `0x3DD000` (identity) stay byte-intact under
   the custom firmware (verified against `flash_is-v10211-DEPLOYED.bin`), so a
   reverted stock boot rejoins ConnectLife **without re-provisioning**. The stock
   Matter DCT state does not survive (the custom build reuses the same DCT
   addresses). Caveats: this only works while the other slot still holds stock
   (serial 100); a second custom OTA overwrites it. On virgin units the fallback slot
   holds the fw2 MP-test image, not the production app.
2. **Repackage revert recipe.** Carve fw1 at flash `0x10000`, length `0x16ab40`, then,
   **in this order** (each step's input contains the previous step's output):
   1. patch `*(u32le*)(img+0xF4)` to a serial strictly above the running one;
   2. `img[0:32] = HMAC-SHA256(00010203…1c1d1e5f, img[0xE0:0x140])`  (relationship (a));
   3. `L = u32le(img[0xE0]) + 0x140; img[L:L+0x20] = HMAC-SHA256(key, img[0:L])`  ((b));
   4. append `u32le(bytesum(img))`.

   Step 3 was **missing until 2026-07-25** and is what bricked a unit (#75). Round-trip
   proof the recipe is now complete: running it with the serial left unchanged reproduces
   the original stock image byte-for-byte. Implemented and self-checked in
   `firmware/scripts/amebaz2_image.py` (the `revert --repackage` step that used it is in the
   tree at the tag `matter-final`). Still awaiting one on-hardware
   confirmation. Delivery via Matter OTA, `ATWO`, or `HOTA=`.
3. **The cloud channel cannot be spoofed** (pinned CA + per-device dkey), and **no
   public stock image exists**: the example file on `download.hismarttv.com` is from
   an older module generation with zero overlap. Official image IDs can only be
   harvested by MITM-ing the phone app (doc 08); the device never asks for them.

## The 0x1E heartbeat and the request bits (2026-08-19 dump)

A re-read of the heartbeat exchange in the 2026-08-19 flash dump (`dumps/stock-w41h1-2026-08-19.bin`,
slot fw1, not in the repository). It answers doc 14 open question 8 as far as the firmware can
answer it and corrects doc 10 §5c, §6.3 and §6.5 on three points. Offsets below are file offsets in that
dump, unlike the runtime addresses above. Pool pointers resolve with file = XIP − `0x9b6dc000`.
Every claim is tagged [PROVEN] (read from the code at the offset cited) or [INFERRED]. The raw listings are in the dump's own folder, with the dump.

### The outbound payload

Payload index N is frame byte 13 + N. The builder is `0x1fc34`, with three status structs as
arguments: A (7 bytes) for payload byte 3, B (5 bytes) for byte 4, C (3 bytes) for byte 5
[PROVEN, `0x1fc44` to `0x1fca4`].

| Payload | Frame | Value | Source |
|---|---|---|---|
| 0 to 2 | 13 to 15 | `1E 00 00` | constants |
| 3 | 16 | `0x80` \| (no reply yet) << 6 \| connect << 4 \| control << 2 | A[0] = 1, A[1] = (`0x1000093c` == 0), A[2] = getter `0x1dc74` (connect, byte `0x100092ac`), A[4] = getter `0x1dc68` (control, byte `0x100092a7`); A[3], A[5], A[6] = 0 |
| 4 | 17 | `0x80` \| self test << 5 \| scan << 4 \| provisioning << 3 | B[0] = 1, B[2] = `0x1dccc` (`0x100092ae`), B[3] = `0x1dd24` (`0x100092b0`), B[4] = `0x1dd7c` (`0x100092af`); B[1] = 0 |
| 5 | 18 | work << 5 \| route << 1 | C[0] = `0x1ddd4` (`0x100092b1`), C[1] = `0x1de2c` (`0x100092ad`), C[2] = 0. Written only when the protocol version word `0x10000540` is 0 or above `0x214` |
| 6 to 8 | 19 to 21 | `00 00 00` | never written |
| 9 | 22 | `0x40` | `payload[9] |= 0x40` at `0x1fca6` |
| 10 | 23 | `00` | never written |

The getters are one-byte reads; the setter names come from the log calls next to them:
`wifi_set_self_test_mode` (`0x1dc80`), `wifi_set_wifi_scan_status` (`0x1dcd8`),
`wifi_set_wifi_prov_status` (`0x1dd30`), `wifi_set_work_status` (`0x1dd88`) [PROVEN]. Connect and
control sit where the older dump's seven-byte layout puts them (doc 10 §6.1: control is self
test − 7, connect is self test − 2) [INFERRED from the layout, no name string read for them].

Byte 16 bit 6 is not "scan not yet finished" (doc 10 §6.4). The flag `0x1000093c` has three
writers: the scan thread sets it to 1 after "scan test finish" (`0x1ffe8`), the steady-state
sender sets it to 1 after every heartbeat the A/C answered (`0x24f48`), and clears it after four
unanswered heartbeats in a row (`0x24f9e`, counter `0x1000ac84`). On the bus it reads as "no 0x1E
reply received, or the link was lost" [PROVEN]. The driver's reading of the bit is the right one.

The version gate is a truncation, not a zero byte: when `0 < version <= 0x214` the builder returns
5 before it writes payload byte 5 or ORs `0x40` into byte 9, and both callers pass that return
value on as the payload length. An old-protocol mainboard gets a five-byte payload `1E 00 00 b3 b4`
[PROVEN, `0x1fc8a` to `0x1fcba`]. Doc 10 §6.3 said byte 5 is sent as zero.

### Two call sites, and the one that sends a wrong byte 17

**Bring-up stream** (inline at `0x260ae` to `0x26110`, inside `uart_ctl_process_main`): a 15 s
window after the device-type and version exchanges, paced at 1 s. It fills A and C from the
getters, calls the self-test, scan and provisioning getters, and then passes the payload buffer
itself as the B argument: `add r0, sp, 0xd0` (bytes `34 a8`) for the memset, `add r2, sp, 0xd0`
(bytes `34 aa`) for the call at `0x260fc`, with the three getter results left in r0 and never
stored. In this window frame byte 17 is `(payload[3] << 4) & 0xFF`, which is `(control & 3) << 6`:
the self-test, scan and provisioning flags are not on the wire at all, and with control 0 the byte
is `00`, not `80` [PROVEN from the bytes above; that the dead stores belong to a local whose
address was never passed is INFERRED]. The window also sets the heartbeat period to 1 s
(`0x1de4c(1)` at `0x2608e`) and restores the saved value after it (`0x261b2`).

**Steady state** (`0x24dbc`, called from `uart_process_num30_cmd` at `0x24ec8`, from the
report-restart path `0x24e44`, and from a cloud-side path `0x24e66`): fills all three structs from
the getters, A from a template at file `0x129dfc` (`01 00 00 00 01 00 00`) with connect and control
written over it, then `0x20578` (which mallocs 12 bytes, builds, frames, frees) [PROVEN]. This is
the builder that makes the captured `B0 80 20 ... 40` heartbeat of doc 03, with connect = 3
(`0xB0` = `0x80 | 3 << 4`): the module in that capture reported itself not connected, which is
what a bench module without a network does [INFERRED].

`uart_process_num30_cmd` runs from the main loop on every fourth pass (`0x2696e`) and sends only
when `tick / 1000 − last >= period`, with `period` the word `0x100092a8`, 3 s when unset
(`0x1de38`), 1 s during the bring-up window, during a factory test and after a run of failures
[PROVEN, `0x24ed6` to `0x24f78`]. Doc 03's "about once a second" was measured in a 22-frame run,
which fits the bring-up window; the 3 s steady-state period was not measured on a bus
[INFERRED].

### The request bits the A/C can set

The reply handler is `handle_num30_cmd_result` at `0x20140` (name string at file `0x12700f`,
pool `0x20344`). It takes the inner payload and its length, rejects payload[0] != `0x1E` with −1
and payload[1] != 0 with −2, reads payload[4] (frame byte 17), payload[5] (frame 18) only when the
length is above 5, and payload[11] (frame 24) only when the length is above 11 [PROVEN,
`0x20144` to `0x2016a`, `0x2026e`]. Tests run in this order:

| Byte | Bit | Mask | What the module does | Where |
|---|---|---|---|---|
| 17 | 5 | `20` | **Smart-config reset and reboot.** Log "smart config reboot"; `0x23e18(1)` sets a config byte (`0x1000a088 + 4`) and writes the config sector to flash (`write_data_to_flash`); `0x100092a4` := 3; then `0x20118`: heap statistics log, `cloud_mode := 0` (flash), 2000 ms delay, `app_reset_configured_network` ("clear ssid saved in flash"), `enter_provisioning_reset` (`0x1fd30`), `wifi_reboot(1)` (`0x1df6c`). Never returns | `0x20162`, `0x202ba`, `0x20118` |
| 17 | 3 | `08` | **Reset and reconfigure**, unless the device type (`0x1000931f`) is `0x15` (dehumidifier). Log "reset need reboot"; `app_reset_configured_network`; `del_device_from_cloud_server(0)` (`0x1bbc8`); `enter_provisioning_reset`; 2000 ms delay; `wifi_reboot(1)`. Never returns | `0x20178` to `0x201ca` |
| 17 | 0 | `01` | **Store a token**: `n` = 8 when the protocol version is 0 or above `0x214`, else 2; `memcpy(0x10009368, payload + 3, n)`, `0x10009364 := n`. No other action | `0x201ce` to `0x201f2` |
| 17 | 6 | `40` | Nothing. It becomes the handler's return code, which all three callers drop | `0x20174` |
| 18 | 7 | `80` | **Report restart**: builds a heartbeat with `0x24dbc` and sends it without waiting (`0x24e44`) | `0x201f6` |
| 18 | 6 | `40` | **Factory test**: if the self-test flag is already set, log "already in factory test mode"; else log "start factory test", `wifi_set_self_test_mode(1)`, heartbeat period := 1 s, `0x2bf38` ("[FTEST] INFO: start factory test"). The same branch runs when the byte `0x10009320` is 1; it is cleared afterwards | `0x20200` to `0x20262` |
| 18 | 5 | `20` | Nothing (return code 7) | `0x20266` |
| 24 | 4 to 7 | `F0` | **Mainboard OTA info**: log "need update device ota info", `0x20de8` ("[DOTA] INFO: read subsystem version"), store `payload[11] & 0xF0` at `0x10009bc4` | `0x2026e` to `0x202b0` |

Bit 5 is tested first and its branch reboots, so a reply with both bits set is a smart-config
reset. The byte `0x10009378` == 1 takes the same branch without any request bit; its writer was not
found. `enter_provisioning_reset` writes connect := 3, control := 0, provisioning := 1, work := 5,
route := 0, then report-restart, then `0x1cac4(1)` [PROVEN, `0x1fd30` to `0x1fd5a`], as doc 03
records. Both reset paths send one last heartbeat with byte 17 bit 3 set (through the correct
builder) before the 2 s delay and the reboot.

### Class 0x1D

The handler never sees `0x1D`: it rejects anything but `0x1E`. The "`0x1D` sibling" of doc 10 §6.5
is the caller's check. In the link bring-up, a reply of class `0x1D` to the device-type request
goes to `0x21a90` (`0x2603c` to `0x2604c`), which runs the device-OTA engine `0x21560` (strings
"[DOTA] INFO: start send flash ota to device : type %02d, version %s", "no ota in flash, abort
ota", "send 1#0 0 frame", "wait 2 NAK", "recv CAN, retry OTA", "wait for mcu reboot timeout") and
then queues the result as a fault code (`0x20edc`, "[DOTA] INFO: add %d fault code %d to queue")
[PROVEN]. `0x21a90` is also called from the cloud-driven device-OTA task (`0x21ab0`, at `0x21bbc`).
So `0x1D` from the A/C means "send me the mainboard image you hold in flash", an XMODEM-style
transfer (SOH, NAK, CAN), not a request for the module to reboot [PROVEN from the strings and the
caller, INFERRED on the transfer protocol]. Without an image in flash the engine aborts at once.

### What the driver does with each of these

`firmware/esphome/components/hisense_ac/hisense_protocol.h` and `.cpp`, read only:

| Stock behaviour | Driver | Same or different |
|---|---|---|
| Byte 17 bit 5: reset and reboot into provisioning | `LINK_REQ_SMARTCFG`, taken on one reply, 10 s lockout, opens the Wi-Fi setup mode | Same trigger, different action by design (no reboot, no credential wipe) |
| Byte 17 bit 3: reset and reconfigure, skipped on device type `0x15` | `LINK_REQ_RECONFIG`, counted after three replies in a row and only while the module's own flag is off the wire | Same trigger with a debounce the stock code lacks: the stock module acts on the first reply. No device-type exception, and none is needed without dehumidifier support |
| Byte 17 bit 0: store a token from payload bytes 3 to 10 | Not read | Not handled. Nothing in the driver needs the token; what the stock module does with `0x10009368` later was not traced |
| Byte 17 bit 6, byte 18 bit 5 | Return codes only | Not read; nothing lost |
| Byte 18 bit 7: send a heartbeat at once | Not read | Not handled. The next 1 s cycle sends one anyway |
| Byte 18 bit 6: factory self-test mode | Not read | Not handled, and should stay so |
| Byte 24 bits 4 to 7: mainboard OTA info | Not read | Not handled; the driver holds no mainboard image |
| Class `0x1D` reply: mainboard OTA transfer | No `0x1D` anywhere in the component | Not handled; the driver cannot serve it. A unit that asks gets silence |
| Outbound byte 16: `B0` with bit 6 = no reply yet | `build_link_heartbeat` sends `B0` once a reply was heard, `F0` before | Same, and the comment matches the flag's writers |
| Outbound byte 17: `80` plus provisioning bit 3 | Same, from the steady-state builder's layout | Same. The bring-up builder's wrong byte is not copied, which is right |
| Outbound byte 18: `20` (work = 1) | Literal `20` | Same. Doc 03's prose says `22` (route = 1); the captured frame and the driver both carry `20` |
| Heartbeat period | 1 s always | Stock: 1 s for 15 s, then the `0x100092a8` period (3 s unless changed) |

Nothing in this table asks for a driver change. The one open bus question stays open: no unit
here has raised bit 3, and the firmware cannot say which models do.

## Open questions

Also listed, with what would settle each, in [14-open-questions.md](14-open-questions.md).

- Which writer (if any) can set state byte `0x10009050` to 6 (VERIFY_NONE path);
  treated as dead/test code.
- The UDP 5030 0x65/0x66-gated path: possible rate-limited A/C-control passthrough.
- Whether the '77' reset chain erases the Matter fabric KV; if not, previously
  commissioned units skip the auto-open window.
- Bootloader serial tie-break (both stock slots carry 100) and fw2's fallback role.
- Exact compare site of the `HOTA=` SHA-256 argument (bootloader does not need it).
