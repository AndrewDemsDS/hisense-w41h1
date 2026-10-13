# Cloud endpoints and firewalling

How a module on the vendor firmware reaches the ConnectLife cloud, which hosts and ports it uses,
and how to keep it off the internet. A module that runs this project's firmware, or an ESP32 board
that replaces the module, has no vendor cloud client, and nothing on this page applies to it.

## Status of this page

Current for a module that still runs the vendor firmware. The hosts and the protocol outline come
from the firmware's strings and a first reading of the code (July 2026). The ports under "from
the manual" come from the vendor's manual and were not measured. The later, deeper analysis of
the same cloud client is in
[13-stock-fw-feature-map.md](13-stock-fw-feature-map.md#cloud-channel-jcmd-v5-over-pinned-tls),
and where the two differ, doc 13 is right. No traffic capture of a module talking to the cloud is
on record.

## How the module connects to ConnectLife

ConnectLife is its own cloud stack and does not use Matter
([02-matter-local-control.md](02-matter-local-control.md)).

1. Provisioning. The ConnectLife app reaches the module over BLE, or over a soft access point
   named `HIS-xxxx` (default `HIS-8888`) on which the module runs a small socket server (the
   `uap_*` functions). It runs `HisenseOverSeasIOTSetUpProtocal` to hand over the Wi-Fi name and
   password and to bind the device to the account. The settings partition keeps a log of "USER
   SSID AND PASSWORD MODIFY" events.
2. Transport. A TLS connection (mbedTLS) to `ac-eu-link.hijuconn.com`, carried as HTTPS with
   chunked transfer encoding: one long-lived stream for push. It is not MQTT. The firmware has no
   MQTT, CONNACK or port 1883 markers.
3. Commands. JSON commands named `jcmd`. The current variant, `jcmd_package_v5`, is AES-256-CBC
   with an HMAC-SHA256 and base64, keyed by a per-device key (`dkey`). The device is identified by
   its `wifi_id` and `device_id`.

## Hosts in the firmware

| Host | Purpose |
|---|---|
| `ac-eu-link.hijuconn.com` | ConnectLife device link (EU) |
| `test-clife-eu-gateway.hijuconn.com` | ConnectLife EU gateway. The firmware fetches new CA certificates from it (doc 13) |
| `*.hijuconn.com` | Hisense IoT cloud |
| `download.hismarttv.com` | firmware downloads |
| `www.hisense.com` | vendor |

The module sends `User-Agent: Hisense-WIFI-module`.

## Ports

From the manual: outbound TCP 80, 443, 55020 and 55030 have to be open for cloud onboarding.

From the firmware (doc 13, [local network surface](13-stock-fw-feature-map.md#local-network-surface)):
the module listens on TCP 5020 while pairing and on UDP 5030 always, and sends a beacon to UDP
5010 about once a minute. Whether the manual's 55020 and 55030 are the same services under other
numbers was not checked.

## Firewall policy

For a module on the vendor firmware that you want off the cloud:

- Put the A/C on a network with no internet access, or add a rule that drops everything from the
  module to the WAN.
- Blocking `*.hijuconn.com` and the download host by name does the same for the hosts known
  here.
- If the module is driven through the vendor firmware's Matter interface, the controller needs
  to reach it on the LAN: mDNS, Matter's UDP 5540, IPv6. Across VLANs that takes more than a
  firewall rule ([02-matter-local-control.md](02-matter-local-control.md#matter-across-vlans-found-2026-07-09)).
- When local control works, remove any cloud integration (for example the ConnectLife HACS
  integration) from the home-automation controller.
