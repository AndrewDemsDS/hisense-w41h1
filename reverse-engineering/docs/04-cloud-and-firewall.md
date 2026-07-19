# Cloud endpoints & firewalling

Once local control works (Matter or ESP32), block the OEM module from the internet so it
can never reach the vendor cloud again. Put the module on an isolated IoT VLAN and deny
its WAN egress.

## How the module connects to ConnectLife (firmware)

ConnectLife is a **self-contained cloud stack, independent of Matter** (Matter is
un-commissioned, see [`02-matter-local-control.md`](02-matter-local-control.md)):

1. **Provisioning**: the ConnectLife app reaches the module over **BLE** (GAP advertising)
   and/or a **softAP** named `HIS-xxxx` (an on-device micro-AP socket server: `uap_*`
   functions). It runs `HisenseOverSeasIOTSetUpProtocal` to hand over the Wi-Fi SSID +
   password and bind the device to the account. (The KV partition even keeps a log of
   "USER SSID AND PASSWORD MODIFY" events.)
2. **Transport**: a **TLS connection (mbedTLS)** to `ac-eu-link.hijuconn.com` /
   `test-clife-eu-gateway.hijuconn.com`, carried as **HTTPS with chunked transfer
   encoding** (a long-lived chunked stream for push), **not MQTT** (no MQTT/CONNACK/1883
   markers in the firmware).
3. **Command protocol**: `jcmd` JSON commands. The current variant (`jcmd_package_v5`) is
   **AES-256-CBC encrypted + HMAC-SHA256 checksum + base64**, keyed by a per-device key
   (`dkey`); the device is identified by its `wifi_id`/`device_id`.

To sever the cloud, block egress to `*.hijuconn.com` (and the OTA host), the Matter or
ESP32 local path keeps working regardless.

## Endpoints seen in the firmware

| Host / port | Purpose |
|-------------|---------|
| `ac-eu-link.hijuconn.com` | ConnectLife device link (EU) |
| `test-clife-eu-gateway.hijuconn.com` | ConnectLife EU gateway (test) |
| `*.hijuconn.com` | Hisense IoT cloud |
| `download.hismarttv.com` | firmware OTA |
| `www.hisense.com` | vendor |

The module also advertises itself with `User-Agent: Hisense-WIFI-module`.

## Ports the module uses (from the manual)

Outbound TCP **80 / 443 / 55020 / 55030** (the manual lists these as needing to be open
for cloud onboarding, so these are what to *block* for a local-only setup).

## Suggested firewall policy

- Place the A/C on an IoT VLAN with no WAN access (default-deny egress), OR
- Add an explicit block rule: source = A/C module → destination = WAN (any), drop.
- Allow only what local control needs:
  - **Matter path:** LAN reachability between the Matter controller and the module
    (mDNS + Matter operational UDP 5540 + IPv6). If they're on different VLANs, add mDNS
    reflection + an inter-VLAN allow.
  - **ESP32 path:** the ESP replaces the module, so the OEM dongle can be removed entirely
    and there's nothing to firewall.
- After local control is verified, remove any cloud integration (e.g. the ConnectLife
  HACS integration) from the home-automation controller.
