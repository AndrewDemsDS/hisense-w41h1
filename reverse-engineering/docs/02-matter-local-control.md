# The vendor firmware's Matter interface

The vendor firmware of the W41H1 is built from the `connectedhomeip` example thermostat app for
AmebaZ2. Next to its ConnectLife cloud client it exposes a Matter thermostat over Wi-Fi, which a
local controller such as Home Assistant can commission. This page records how that was found and
how it was used.

## Status of this page

History, and a record of the vendor firmware. This was the project's first route to local control,
in July 2026, before any custom firmware existed. The project does not use Matter any more: its
own Matter firmwares are retired
([`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md), tag
`matter-final`), and the supported firmware is the ESPHome one
([`firmware/esphome/README.md`](../../firmware/esphome/README.md)).

The page still describes a module that runs the vendor firmware. The credentials and the storage
findings come from one flash dump. The commissioning steps and the network fix were carried out
on the owner's installation in July 2026 and have not been repeated since. The vendor's Matter
thermostat exposes mode and setpoints only (doc 10 §5b-2): no fan, swing, eco, quiet or
diagnostics.

## Commissioning credentials

The firmware ships with the standard Matter test credentials, the defaults of the connectedhomeip
example, not values of one device:

| Field | Value |
|---|---|
| Manual pairing code | `3497-011-2332` (`34970112332`) |
| QR payload | `MT:Y.K9042C00KA0648G00` |
| Discriminator | 3840 (0x0F00) |
| Passcode | 20202021 |

Evidence that these are the test defaults:

- The app is `example_matter_thermostat`, with the Matter test DAC, PAI and CD certificates
  compiled in.
- The settings partition holds `uniqueId` and `location-capability` and no `discriminator`,
  `verifier`, `salt` or `pin-code` of its own, so the provider falls back to the defaults.
- The string `34970112332` is in the flash.

[`tools/extract_matter_setup_code.py`](../tools/extract_matter_setup_code.py) runs these checks
on a dump. [`tools/gen_matter_qr.py`](../tools/gen_matter_qr.py) draws the QR code from the
payload, and [`../hardware/matter_qr.png`](../hardware/matter_qr.png) is its output for the
payload above.

## Is a module already commissioned?

Not the one that was dumped. Its writable settings partition (top of the flash, about `0x3da000`
to `0x3f0000`) holds the Wi-Fi configuration and `uniqueId` and no Matter fabric state: no
committed NOC, no ACL entries, no `f/1/n`, `f/1/r` or `g/fidx` keys, and no operational
certificate. The OID `1.3.6.1.4.1.37244` appears only in the code region, as the compiled-in test
certificates. The strings `FabricTable`, `NOC`, `IPK` and `acl` are the Matter implementation and
not stored data.

So the Matter interface was unused and not tied to Hisense or anyone else:

- Commissioning into a local controller is not blocked by an existing fabric.
- ConnectLife does not use Matter. It runs its own cloud stack
  ([04-cloud-and-firewall.md](04-cloud-and-firewall.md)). The two are independent, so the module
  can be commissioned locally without unbinding it from ConnectLife.

## Opening the commissioning window

From the AEH-W41H1 manual, for an air conditioner: press the Horizon Airflow (swing) button six
times on the remote. The buzzer beeps five times and the display shows "77". The module clears
its stored SSID and password and enters pairing, which opens the Matter commissioning window. On
a wired remote the manual gives Sleep pressed eight times.

To release the cloud binding as well: ConnectLife app, menu, Preferences, Appliance Settings,
Unpair. An appliance can be bound to one account only.

Matter allows several administrators, so a controller can be added next to an existing fabric.
A fresh reset is the reliable way to get an open window. With no fabric stored, the vendor
firmware also opens the window by itself for 900 s at boot
([13-stock-fw-feature-map.md](13-stock-fw-feature-map.md#matter-stack)).

What "77" is on the bus, and how the request bits are used today, is in
[03-rs485-ac-protocol.md](03-rs485-ac-protocol.md#recommission-77-the-request-bits-in-the-0x1e-reply).

## Commissioning into Home Assistant

1. Run a Matter server on the LAN, for example
   `ghcr.io/home-assistant-libs/python-matter-server`, with host networking and D-Bus mounted for
   BLE.
2. Enable the test-net DCL. The device uses test attestation certificates, so a default Matter
   server rejects it (`AttestationVerification` failure). Run the server with
   `--enable-test-net-dcl` so that it trusts the CSA test PAA roots.
3. In Home Assistant: Settings, Devices & Services, Matter, and point it at the server
   (`ws://<host>:5580/ws`).
4. Add device, enter `34970112332` or scan the QR code, and accept the warning about an
   uncertified device.
5. Commissioning uses BLE, so the controller has to be within Bluetooth range of the A/C.

## Matter across VLANs (found 2026-07-09)

Matter's operational discovery and transport are IPv6 only in this firmware
(`INET_CONFIG_ENABLE_IPV4=0`), and discovery is mDNS. Neither crosses VLANs by default. With the
A/C on an IoT VLAN and the Matter server on another VLAN, the node commissioned and then became
unavailable: the controller could not discover or reach it.

What did not work: the router's mDNS reflector did not reflect IPv6 mDNS (`ff02::fb`) between
VLANs. IPv6 routing from the server's VLAN to the IoT VLAN was fine. Discovery was the missing
piece.

What worked was to give the Matter server's host a second interface on the A/C's VLAN:

1. On the switch port of that host, add the IoT VLAN as a tagged network and keep the host's own
   network untagged.
2. On the host, with NetworkManager, create a persistent VLAN interface with IPv6 only. Do not
   run `dhclient` on it by hand: `dhclient -1` on the sub-interface hung, and once blocked the
   primary link of a headless machine.
   ```
   nmcli con add type vlan con-name iot dev eth0 id <vlan-id> \
       ipv4.method disabled ipv6.method auto ipv6.addr-gen-mode eui64 \
       connection.autoconnect yes
   nmcli con up iot
   ```
   The interface gets a SLAAC address on the IoT VLAN. The connection is saved, survives a
   reboot, and is managed apart from `eth0`.
3. Restart the Matter server so that its mDNS resolver binds the new interface. The node became
   available within about 15 s.

The A/C stays isolated on the IoT VLAN. Only the Matter controller reaches in.

## References

- Bypassing attestation for test devices: the Home Assistant community thread "Bypass matter
  attestation verifier".
- A Hisense A/C commissioned into a third-party Matter controller with the same six-press reset:
  a Hubitat community guide.
