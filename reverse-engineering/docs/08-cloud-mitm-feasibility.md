# Impersonating the cloud: feasibility

Could the module's cloud connection be pointed at a server of our own that speaks the vendor's
protocol and controls the A/C from there? This page is the assessment, written in July 2026. The
answer is no for practical purposes, and the project never tried it.

## Status of this page

History. A dead end and its reasons. The assessment was made from a first reading of the
firmware. Two points were left as "likely" here and were settled later by the deeper analysis in
[13-stock-fw-feature-map.md](13-stock-fw-feature-map.md#cloud-channel-jcmd-v5-over-pinned-tls):
the gateway certificate is verified against pinned CAs on every path, and the path without
verification cannot be reached. Nothing on this page was tested against a live module.

The page speaks of the vendor firmware. A module on this project's firmware has no cloud client.

## Why the easy route of the older modules does not apply

`deiger/AirCon` and `bannhead/pyaehw4a1` control older Hisense A/Cs locally because the AEH-W4A1
module is built on the Ayla Networks platform, which has a local LAN API: a per-device LAN key is
fetched from the Ayla cloud once, with the app's credentials, and the A/C is then driven over the
LAN. No DNS spoofing and no certificate bypass are involved.

The W41H1 is a later generation. It dropped Ayla for Hisense's own `hijuconn` cloud and the
`jcmd` protocol, and its firmware has no persistent local control API. The only local TCP server
is the provisioning server of the soft access point (`HIS-8888`), which is up in pairing mode
only.

## The two obstacles

1. The TLS server certificate is verified. The firmware decrypts and loads two CA certificates
   (`pemA` and `pemB`, stored AES-encrypted and base64-encoded) and chooses a verification method
   through `check_crt_method`. Without the private key of one of those CAs, a server of our own
   cannot present a certificate the module accepts. A path with `MBEDTLS_SSL_VERIFY_NONE` exists
   in the code. This first reading could not show whether the gateway connection ever uses it.
   Doc 13 shows that it cannot be reached.
2. The application protocol is encrypted. Inside TLS, `jcmd` v5 messages are AES-256-CBC with an
   HMAC-SHA256, base64-encoded, and the key is set during BLE or soft-AP provisioning through a
   `CMD_SENDENCKEY` handshake. To speak it, a server needs the device's key (`dkey`): extracted
   from the flash, or set by running the provisioning as the app would.

## Verdict

Even in the best case this means defeating certificate pinning and then reimplementing a
proprietary encrypted protocol with a key handshake. That is more work than the routes that give
local control, and it would break with any change on the vendor's side.

Local control without the cloud exists in two forms. The project's ESPHome firmware runs on the
stock module or on an ESP32 board in its place
([`firmware/esphome/README.md`](../../firmware/esphome/README.md)). A module left on the vendor
firmware can be driven through its own Matter interface from a local Matter server
([02-matter-local-control.md](02-matter-local-control.md)), with mode and setpoint only.

## Studying the cloud API

To learn the API for its own sake, intercept the phone app and not the device: install a user CA
on the phone, proxy the ConnectLife app through `mitmproxy`, observe the calls, and replay them
to the real cloud from a script. That drives the A/C from your own code and stays dependent on
the cloud. Doc 13 notes one more use: the official firmware image ids are visible to the app and
the cloud only, so this is also the only way to learn them.
