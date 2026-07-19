# Matter local control (preferred path)

The W41H1 firmware is built from the `connectedhomeip` **example thermostat** app for
AmebaZ2. It exposes a **Matter thermostat** device type over Wi-Fi, alongside the
ConnectLife cloud client. Matter is a local LAN protocol, so this is the cleanest way to
control the A/C with no cloud.

## Commissioning credentials

The firmware ships with the **standard Matter test credentials** (the connectedhomeip
example defaults, not per-device values):

| Field | Value |
|-------|-------|
| Manual pairing code | `3497-011-2332` (`34970112332`) |
| QR payload | `MT:Y.K9042C00KA0648G00` |
| Discriminator | 3840 (0x0F00) |
| Passcode | 20202021 |

Evidence they're the test defaults, not provisioned per-device:
- App is `example_matter_thermostat`; compiled-in Matter **test** DAC/PAI/CD certs.
- The KV/FTL config partition holds `uniqueId` + `location-capability` but **no** custom
  `discriminator`/`verifier`/`salt`/`pin-code`, so the provider falls back to defaults.
- The literal string `34970112332` is present in flash.

Regenerate the QR from the payload with [`tools/gen_matter_qr.py`](../tools/gen_matter_qr.py).

## Is it already commissioned? (firmware evidence)

**No.** The writable KV/FTL partition (top of flash, ~`0x3da000`–`0x3f0000`) contains the
Wi-Fi config and `uniqueId` but **no Matter fabric state**: no committed NOC, no ACL
entries, no `f/1/n`/`f/1/r`/`g/fidx` fabric keys, and **no Matter operational cert** (the
`1.3.6.1.4.1.37244` OID appears only in the *code* region as the compiled-in test DAC/PAI,
never in the writable partition). The `FabricTable`/`NOC`/`IPK`/`acl` strings are just the
Matter *implementation* in firmware, not stored data.

So the **Matter interface is un-commissioned and dormant**: it is *not* pre-associated to
Hisense or anyone. Consequences:
- Commissioning into a local controller will **not** be blocked by an existing fabric.
- **ConnectLife does not use Matter**: it runs a separate cloud stack (see
  [`04-cloud-and-firewall.md`](04-cloud-and-firewall.md)). Matter and the cloud are
  independent, so you can commission Matter locally without unbinding from ConnectLife.

## Opening the commissioning window (reset)

From the AEH-W41H1 manual, for an air conditioner:
> Press the **"Horizon Airflow" (swing) button 6 times** on the remote → buzzer beeps 5×
> → display shows **"77"**. The module clears its stored SSID/password and re-enters
> pairing, opening the Matter commissioning window.

(Alt on a wired remote: press **"Sleep" 8 times**.) To also release the cloud binding:
ConnectLife app → *menu → Preferences → Appliance Settings → Unpair* (an appliance can
only be bound to one account).

Matter is multi-admin, so a controller can be added even alongside an existing fabric,
but a fresh reset reliably guarantees an open window.

## Commissioning into a local controller (Home Assistant)

1. Run a Matter server on the LAN (e.g. `ghcr.io/home-assistant-libs/python-matter-server`,
   host-networked, D-Bus mounted for BLE).
2. **Enable test-net DCL**: because the device uses *test* attestation certs, a default
   Matter server rejects it (`AttestationVerification` failure). Run the server with
   `--enable-test-net-dcl` so it trusts the CSA **Test** PAA roots.
3. HA → Settings → Devices & Services → **Matter** → point at the server
   (`ws://<host>:5580/ws`).
4. Add device → enter `34970112332` (or scan the QR). Accept the "uncertified device"
   warning.
5. Commissioning uses **BLE**, so the controller must be within Bluetooth range of the A/C.

## Networking gotcha: cross-VLAN Matter (validated 2026-07-09)

Matter operational discovery **and** transport are **IPv6-only** (`INET_CONFIG_ENABLE_IPV4=0`);
discovery is mDNS. Neither crosses VLANs by default. With the A/C on IoT VLAN4 and the controller
(python-matter-server on the Pi) on Servers VLAN3, the node commissions but then goes
`available=False`: CASE can't discover/reach it.

**What did NOT work:** UniFi's mDNS reflector (Gateway mDNS Proxy) does not reflect **IPv6** mDNS
(`ff02::fb`) across VLANs, so operational discovery never crossed. Inter-VLAN IPv6 *routing*
Servers→IoT was fine; discovery was the missing piece.

**Working fix, put the controller on the A/C's L2 (dual-home the Pi onto VLAN4):**
1. Switch port carrying the Pi: add VLAN4 as a **tagged** network (keep Servers as the native/untagged).
2. On the Pi (NetworkManager), a **persistent IPv6-only VLAN sub-interface**: no `dhclient`
   (raw `dhclient -1` on the sub-iface hangs and once wedged the primary link on a headless box):
   ```
   nmcli con add type vlan con-name iot4 dev eth0 id 4 \
       ipv4.method disabled ipv6.method auto ipv6.addr-gen-mode eui64 \
       connection.autoconnect yes
   nmcli con up iot4
   ```
   → `eth0.4` gets a VLAN4 SLAAC address (`fd00:X::…` for your ULA prefix); saved to
   `/etc/NetworkManager/system-connections/iot4.nmconnection`, survives reboot, managed
   independently of `eth0`.
3. Restart matter-server so CHIP's mDNS resolver binds `eth0.4` (`docker restart matter-server`).
   Node becomes `available=True` within ~15 s via same-L2 IPv6 mDNS + direct routing.

Trust direction stays correct: the A/C is isolated on IoT; only the Matter controller reaches in.

## References
- Bypass attestation for test devices: HA community "Bypass matter attestation verifier".
- A Hisense A/C commissioned into a 3rd-party Matter controller (same swing-×6 reset):
  Hubitat community guide.

## Recommission ("77"): protocol internals

For the firmware-RE narrative of how "77" works on the wire (the `0x1E` heartbeat request/reply
bitfields, the stock `enter_provisioning_reset` state writes, and the bench-validated behavior of
our own firmware through v23), see
[`03-rs485-ac-protocol.md` § Recommission ("77")](03-rs485-ac-protocol.md#recommission-77--how-the-stock-firmware-does-it-firmware-re-f1).
