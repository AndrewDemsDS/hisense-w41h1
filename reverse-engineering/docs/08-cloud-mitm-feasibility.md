# "Be the cloud": MITM / API-replay feasibility

Can we redirect the module's cloud connection to our own server, speak the protocol, and
control the A/C from there? **Technically conceivable, but the worst option for the W41H1,
not recommended.** Here's the analysis.

## Why the easy route (deiger/AirCon) does NOT apply

`deiger/AirCon` and `bannhead/pyaehw4a1` control older Hisense A/Cs locally because the
**AEH-W4A1 is built on the Ayla Networks IoT platform**, which has a *legitimate local LAN
API*: you fetch a per-device LAN key from the Ayla cloud once (with app credentials), then
talk to the A/C directly over the LAN, no DNS spoofing, no cert bypass.

The **W41H1 is a different generation**: it dropped Ayla for **Hisense's own `hijuconn`
cloud + `jcmd` protocol**, and the firmware exposes **no persistent local control API**,
the only local TCP server is the **softAP provisioning server** (`HIS-8888`), active only
in pairing mode. So there is no built-in local API to connect to.

## The two hurdles for a real MITM

1. **TLS server-cert validation (likely pinned).** The firmware decrypts and loads **two
   CA certs** (`pemA` / `pemB`, stored **AES-encrypted + base64**) and selects a
   verification method via `check_crt_method`. Loading pinned CAs into the TLS config means
   it intends to **verify the gateway certificate against them**. If so, we cannot
   impersonate the cloud, we can't forge a server cert chaining to their CA without the
   CA's private key. (A `MBEDTLS_SSL_VERIFY_NONE` path also exists, but that's almost
   certainly a fallback/non-critical path; static analysis couldn't prove the gateway
   connection uses it.)
2. **Encrypted application protocol.** Past TLS, `jcmd` v5 messages are **AES-256-CBC +
   HMAC-SHA256 + base64**, with the session key set up via a `CMD_SENDENCKEY` handshake
   during BLE/softAP provisioning. To speak it from our server we'd need the device's
   provisioned key (`dkey`), either extract it, or re-run provisioning *as the app* to set
   it ourselves.

## Verdict

Even in the best case, MITM means **defeating (or getting lucky on) cert pinning** *and*
**reimplementing a proprietary AES-CBC+HMAC protocol with a key handshake**: more work
than the paths that already give full local control, and far more fragile (it breaks on any
cloud/firmware change).

## The good news: you already have the clean version of this

- **Matter = "control from our own server", legitimately.** Commissioned into the Pi's
  local `python-matter-server`, the A/C is controlled entirely from your own box with no
  cloud and no protocol hacking. This *is* the local server you're describing.
- **ESP32 replacement** removes the cloud module entirely.

## If you only want to *understand* the API (research)

MITM the **phone app**, not the device: install a user CA on the phone and proxy the
ConnectLife app through `mitmproxy` to observe the `jcmd`/ConnectLife API calls, then replay
them to the real cloud from a script. That lets you drive the A/C from your own code, but
it stays **cloud-dependent**, so it's a protocol-learning exercise, not de-clouding.
