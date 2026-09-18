# Reliable, authenticated OTA across the three firmwares

Plan for issue #104. The issue is written as "stand up a persistent HTTP server", which is the
distribution leg only. A server on its own would make the weakest path in the project both permanent
and convenient, so the scope below is the whole update chain: what authorises an image, what happens
when a bad one lands, and only then where the bytes are served from.

Nothing here is implemented yet. What follows is the design, the evidence behind each claim, and the
phase order. Values that were read out of the pinned SDKs are cited by file and line; anything not
confirmed on this tree or on hardware is marked `// VERIFY`, the same convention the driver uses for
protocol bytes.

## 1. Threat model

One paragraph, because the whole design follows from it.

The attacker is on the LAN: a compromised IoT device, a guest on the same VLAN, anyone who got the
Wi-Fi PSK. They can sniff, they can ARP or NDP spoof, and they can connect to any listening port.
They are not assumed to have physical access, because physical access already means a SOIC-8 clip
and the game is over. Against that attacker the ESP32 node today has this chain:

1. `firmware/esp32-matter/main/app_main.cpp` opens the break-glass listener on TCP 2324 and accepts
   a **plaintext, replayable** shared secret. The compare is constant-time (`breakglass_token_ok()`)
   and the socket is fail-closed when no token is compiled in, which closes the cheap attacks, but
   anyone who watched one legitimate trigger has the token forever.
2. The token starts `https_ota_task()`, which fetches `HISENSE_OTA_URL`. That URL is baked at build
   time and is `http://` today. `sdkconfig.defaults` carries
   `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y` to permit it.
3. There is **no image authenticity check at all**. `sdkconfig.defaults` contains no `SECURE_*`
   option, so `esp_image_verify()` checks the SHA-256 that the image carries about itself and
   nothing else. An attacker-supplied image is a self-consistent image.

Chain those and a LAN attacker who has seen one break-glass trigger, and who can answer for the
file server's address, executes arbitrary code on a device wired to the mains side of an air
conditioner. The same listener also exposes `:wipekv`, which is destructive on its own.

Two mitigating facts worth recording so the risk is not overstated. First, the CI release workflow
(`.github/workflows/esp32-release.yaml`) passes neither `-DHISENSE_OTA_URL` nor
`HISENSE_BREAKGLASS_TOKEN`, so **published release images have no break-glass listener and a
non-resolvable placeholder URL**. The exposure is on locally built, locally flashed images, which is
every deployed node, but not on anything a stranger downloads.

Second, and this one is weaker than it looks: the AmebaZ2 bootloader does verify a manifest
signature, a per-sub-image HMAC trailer and a byte-sum before booting a slot. That is **integrity,
not authenticity**. The HMAC key is a hardcoded constant sitting in this public repo
(`firmware/scripts/amebaz2_image.py:32`, the Realtek partition-table `hash_key` default), so anyone
can produce a chain that verifies. What the AmebaZ2 bootloader actually buys is corruption
detection, and its response to a failure is to hang with no fall-back slot, which is the 2026-07-21
office brick. Treat the AmebaZ2 as having no image authentication today.

## 2. Image authenticity is the load-bearing control

TLS is not the fix. TLS authenticates a *server*; what matters is authenticating an *image*, and an
authenticated image is safe to fetch over plain HTTP from an untrusted mirror. Signing first also
keeps the door open for GitHub release assets as a distribution source later, where TLS to the
device would otherwise be mandatory.

### ESP32 (esp-matter path)

Use **signed apps without secure boot**: no eFuse is burned, nothing is one-way, and the whole
change is revertible by rebuilding with the option off. Confirmed present in the pinned IDF
(`IDF_PIN="v5.5.4"` in `versions.env`, checked against the local checkout):

| Option | Where |
|---|---|
| `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` | `components/bootloader/Kconfig.projbuild:497` |
| `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` | `components/bootloader/Kconfig.projbuild:592` |
| `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES` | `components/bootloader/Kconfig.projbuild:681` |
| `CONFIG_SECURE_BOOT_SIGNING_KEY` | `components/bootloader/Kconfig.projbuild:695` |

The enforcement point is not in our code, which is what makes this cheap. `esp_ota_end()` calls
`ota_verify_partition()` unconditionally (`components/app_update/esp_ota_ops.c:506`, definition at
`:450`), which calls `esp_image_verify(ESP_IMAGE_VERIFY, ...)`. Every write path the node has
(`esp_https_ota_finish()` for break-glass, and the Matter OTA processor) ends in `esp_ota_end()`, so
one config change covers both transports with no glue edits.

**Both options, and the second is the one that does the work.** The IDF OTA guide states the pair
directly: "The verification of signed OTA updates can be performed even without enabling hardware
secure boot. This can be achieved by setting `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` and
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`." The mechanism behind that sentence, read off the
pinned tree: in a non-bootloader build `esp_image_format.c:41` derives `SECURE_BOOT_CHECK_SIGNATURE`
from `CONFIG_SECURE_SIGNED_ON_UPDATE`, a hidden symbol that
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` selects (`Kconfig.projbuild:449`). The second option
defaults to `y` once the first is set, so this is a "do not switch it off" rather than a "remember to
switch it on", but the split matters: the first option signs the image and compiles in the verifier's
dependencies (`esp_image_format.c:979`, `:1048`), the second is what makes the check run.

**The trust root is the running app, which dictates how this is rolled out.** Verification uses the
public key embedded in the signature block of the app that is *currently running*, not a key in the
bootloader or in eFuse, and the running app's own signature is never re-checked. So the chain has to
be started by hand: the first signed image must be flashed **physically over USB or serial**, because
an unsigned node has no key to verify anything against and would reject the signed image it was sent.
Espressif is explicit that "it is essential that the initial app flashed to the device is also
signed". The rule is symmetric afterwards: once a signed image is running, a *later unsigned* image
is rejected too, and IDF further aborts at startup if the running app carries no valid signature,
deliberately, so a device cannot be left in a state where no update is possible. Sequencing follows
from that and the delta-base rule together: physically flash a signed full image, archive it as the
new delta base, and only then resume OTA releases. Espressif also notes what this mode does not do:
it "does not provide protection against an attacker with write access to flash memory", which is
exactly the trade section 1 makes, since physical access here already means a clip.

**Delta OTA and signing coexist.** This was the question that decided whether the ESP32 leg is a
plan or a wish, because `CONFIG_ENABLE_DELTA_OTA=y` is already on and losing it would cost the
~1.5 MB full-image transfer that the OTA hardening work was about. Read on the pinned esp-matter
(`ESP_MATTER_PIN="e0a089b..."`), `connectedhomeip/src/platform/ESP32/OTAImageProcessorImpl.cpp`
applies the patch through `esp_delta_ota_feed_patch()` into the passive partition via `esp_ota_write`,
then calls `esp_delta_ota_finalize()` and `esp_ota_end()` at line 395. The reconstructed image is
therefore verified like any other. Two conditions follow and both are real constraints on the
tooling:

- The patch must be generated from **signed base to signed target**, so the reconstructed bytes
  include the appended signature block. `// VERIFY` on hardware that a patch generated across two
  signed binaries reconstructs byte-exactly; the diff is over whole files, so it should, but an
  untested assumption here strands a node.
- `VerifyPatchHeader()` (same file, line 166) compares the SHA-256 of the **currently running
  firmware** against the patch header. Once signing is on, every archived delta base in
  `firmware/built-images/` must be the *signed* artifact. A base archived before the switch is
  useless. This compounds with the chain-of-trust rule above: the cutover is a physical flash of a
  signed full image, and the delta archive restarts from that artifact.

Scheme choice depends on silicon, and IDF picks it for you once the revision is right:
`SECURE_SIGNED_APPS_SCHEME` defaults to the RSA V2 scheme when the chip supports it and to V1 ECDSA
otherwise (`Kconfig.projbuild:508`). What has to be got right is the revision floor. Secure Boot V2
is available on the ESP32-C3 only from chip revision v0.3 (ECO3), which means
`CONFIG_ESP32C3_REV_MIN` must be at least v0.3; classic ESP32 needs revision 3 for V2 and falls back
to V1 ECDSA below that. `// VERIFY` the revision of the boards actually in use, and expect the two
release-matrix targets to need different values in their `sdkconfig.defaults.<target>` overlay.

### AmebaZ2

The AmebaZ2 has image integrity, not image authenticity (section 1): the HMAC key is a public
constant. It also has no safe failure mode, and the mechanism is not fully understood: issue **#75**
is open precisely because re-signed images fail to boot, and
`test_image_chain.py` records that a stale trailer anywhere in the chain hangs `boot_load` with no
fall-back slot, which is how a unit was bricked in the office on 2026-07-21.

So the honest statement is that the AmebaZ2 leg of this plan is **blocked on #75** and this document
should not promise otherwise. Two things can be done before it unblocks, and both are useful on
their own:

- **Verify before reset.** The break-glass path calls the Realtek SDK's `http_update_ota()` and then
  `ota_platform_reset()`. Port the verification half of `firmware/scripts/amebaz2_image.py` (manifest
  signature, per-sub-image HMAC trailer, byte-sum) to run on-device against the freshly written slot
  *before* the reset. Today a bad byte is discovered by the bootloader, and the bootloader's answer
  is to hang. This turns the same bad byte into a log line and a node that is still reachable.
- **Decide whether real authentication is reachable at all.** Swapping the partition table's
  `hash_key` for a project key would make the chain authenticating rather than merely
  integrity-checking, but it is a bootloader-verified field on a device whose only recovery is a
  clip, so it is exactly the kind of change #75 exists to explain first. `// VERIFY` how the
  bootloader sources that key, against `reverse-engineering/docs/07-firmware-and-ota.md` and a real
  dump, before designing around it. Until then the AmebaZ2 leg's security story is "do not serve it
  anything you did not build", which is a reason for section 5's server to be read-only and local.

### ESPHome

ESPHome builds this project on the ESP-IDF framework, so the same signed-apps options should be
reachable through `esp32: framework: sdkconfig_options:` in `w41h1.yaml`, and ESPHome's native `ota:`
platform writes through `esp_ota_ops` and should therefore inherit the `esp_ota_end()` check.
`// VERIFY` both against the pinned `esphome==2026.7.4` before claiming parity. ESPHome's own OTA
already has a password and an optional TLS transport, so this path is the least exposed of the
three today.

## 3. Reliability: rollback with a health gate that is not Matter

An authenticated image can still be a broken image. The one that motivated the break-glass work in
the first place (#61) was signed by nobody and broke nothing except Matter subscriptions, and that
was enough to make the node un-updatable.

ESP-IDF has the mechanism and this project does not use it: no `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
in `sdkconfig.defaults` (the option lives in `components/bootloader/Kconfig.app_rollback:3`), and no
call to `esp_ota_mark_app_valid_cancel_rollback()` anywhere in `firmware/esp32-matter/main/`. With
rollback enabled, a new image boots as `PENDING_VERIFY` and the bootloader reverts to the previous
partition unless the app marks itself valid.

**Nobody else will mark it valid, so the gate must be written before the option is enabled.** This
was worth checking, because CHIP has a hook that looks like it would: `DefaultOTARequestorDriver::Init`
calls `mImageProcessor->ConfirmCurrentImage()` on the first boot after an apply
(`src/app/clusters/ota-requestor/DefaultOTARequestorDriver.cpp:77`). On the pinned esp-matter the
ESP32 implementation of that hook only compares the running `softwareVersion` against the requested
target version (`src/platform/ESP32/OTAImageProcessorImpl.cpp:83`); it does not touch the OTA state
machine, and `esp_ota_mark_app_valid_cancel_rollback()` appears nowhere in esp-matter or in the CHIP
ESP32 platform layer. So enabling rollback on its own would turn **every** update into a revert
loop. Good news for the design, since it means the health gate is ours to define rather than
something CHIP closes at stack init on the one condition #61 says not to use.

The health gate is the design decision, and #61 dictates it: **do not gate on Matter.** A node with
dead subscriptions and a healthy A/C bus is the exact state that produced the incident, and rolling
that back automatically would be wrong. The gate should be the things whose failure means
"this image cannot be managed":

1. Wi-Fi associated and an IP address held.
2. The RS-485 link healthy, which the driver already tracks and already exposes as a link token
   (`hisense_get_link_token()`, used by the ESPHome `binary_sensor` and the diag console).
3. The break-glass listener bound, when a token is compiled in. An image that cannot be rescued
   should not be allowed to become permanent.

Hold for a fixed window (60 seconds is a reasonable first number, `// VERIFY` against how long
commissioning and the first bus poll take on a cold boot), then mark valid. This interacts
with the existing net-loss reboot watchdog and with `ota_activity_begin()` / `ota_activity_end()`,
so the interaction has to be worked out rather than bolted on: a watchdog reboot inside the pending
window is a rollback, which is probably the right answer but must be a deliberate one.

**A rollback desynchronises the delta base, and the tooling has to learn about it.**
`esp32-release.sh` picks the base from a local marker file, `.released-version-<target>` in
`firmware/built-images/`, written when a release is confirmed booted (`released_int()`, line 63).
A device that rolls back after that confirmation is running the previous image while the marker
still names the new one, so the next release generates its patch against the wrong base and the
node rejects it at `VerifyPatchHeader()`. The fix belongs in the same phase as the gate: `package`
must read the version off the device (Matter `softwareVersion`, or a break-glass query) rather than
trust the marker, or at minimum compare the two and refuse when they disagree.

The AmebaZ2 equivalent is section 2's verify-before-reset, plus `<token>:slots` and `<token>:revert`,
which already exist. Whether the Realtek bootloader has any boot-count or valid-flag fallback of its
own is a research item, and if the answer is no then verify-before-reset is the only line of defence
there.

## 4. Transport

Two changes, in this order.

**Break-glass challenge-response.** Replace the plaintext token with a nonce exchange: the device
sends a random nonce on connect, the client replies `HMAC-SHA256(token, nonce || command)`, the
device compares in constant time and keeps a short nonce lifetime. The shared secret stays a
compile-time constant, so the build and provisioning story does not change at all, and replay stops
working. This matters most for `:wipekv`, which is destructive and currently replayable by anyone who
sniffed one use. Keep accepting the bare token behind a build flag for one release so a node flashed
with the old scheme is still rescuable by the new tooling.

**HTTPS for the fetch: measure first, decide second.** The comment on `trigger_https_ota()` sizes the
task stack at 8 KB with "TLS-off buffers" called out explicitly. mbedTLS needs more stack and tens of
KB of heap, on a device that is already holding the Matter stack and an RS-485 task. So the plan is
to record free heap on a release node at idle before deciding, and to treat plain HTTP as an
acceptable outcome: once images are signed, the fetch transport carries no authority, and a
downgrade attack gets an attacker a rejected image and a log line. If HTTPS does fit, pin the server
certificate rather than shipping a CA bundle.

## 5. Where the images live (issue #104 proper)

Only now does hosting matter, because at this point the server is a convenience rather than a trust
anchor.

### What already exists, because #104's text is stale

The issue says the image is "served ad-hoc" at a transient address. That has not been true for a
while. `firmware/scripts/ota-guards.sh:105` defines `pi_http_publish()`, and **both** release scripts
already call it as part of `stage`: `ota-release.sh:579` for the AmebaZ2 and `esp32-release.sh:294`
for the ESP32. Each call copies the raw `.bin` into a persistent, user-owned docroot (`PI_HTTP_DIR`)
served by a `restart=always` `ota-http` container on the Pi, keeps a per-target, per-version,
per-flavour copy under `archive/`, and repoints the stable basename that deployed nodes fetch
(`rac-ota.bin` for the AmebaZ2, `esp32-ota.bin` for the ESP32). Those basenames are compile-time
constants in the firmware, so they can never be renamed for nodes already in the field.

So the persistent server asked for in #104 is running and wired in. What is missing is everything
that makes it survive its own host.

### Where each artifact should live, and why it is not one place

| Artifact | Home | Why |
|---|---|---|
| Device-facing current image | The Pi `ota-http` container | The only host the device can actually fetch from, see below |
| Per-version archive | Same docroot, `archive/` | Lets `revert` fetch an older image without a rebuild |
| Release copy of record | GitHub release assets | Durable, offsite, already how the project publishes |
| Delta bases and the signing key | `firmware/built-images/` plus an off-Pi backup | Losing either strands every node on USB |

**The device cannot fetch from GitHub, and that decides the origin.** Release asset URLs are HTTPS
and redirect, while `https_ota_task()` builds an `esp_http_client_config_t` with a URL, a timeout and
keep-alive and nothing else: no `cert_pem`, no `crt_bundle_attach`, and no cert bundle enabled in
`sdkconfig.defaults`. `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y` is what makes the current fetch work at
all. So as long as break-glass is plain HTTP (section 4 argues that is acceptable once images are
signed), the origin the device points at must be on the LAN. GitHub is the archive humans and
python-matter-server use, not a URL that can be baked into firmware.

That also settles the tempting idea of a second baked mirror for redundancy: the URL is a single
compile-time constant, so there is no failover to point at. Redundancy for this path is a docroot
that can be rebuilt quickly, not a second address.

Note `OTA_RELEASE_BASE` (#79) is currently unset, so manifests still carry a local `file://` `otaUrl`
into the Pi provider directory. Setting it is orthogonal to this section: it moves the big `.ota` for
the **Matter** path into the GitHub release, where python-matter-server downloads it over HTTPS and
re-serves it over BDX. That is worth doing and it does not help break-glass.

### What is actually left to do

1. **The `ota-http` service is not in the repo.** It exists only as state on the Pi: no compose file,
   no unit, no documentation of the port or the bind address anywhere in the tree. An SD card failure
   takes the break-glass path with it, silently, and it would be noticed on the day it is needed.
   Check in the service definition and a short bring-up note, with the real paths in the gitignored
   `ota-release.env` as usual.
2. **A documented rebuild path for the docroot.** From `firmware/built-images/` on the build box, or
   from the GitHub release assets. One command, written down, tested once.
3. **`stage` should verify, not assume.** After publishing, `GET` the current-image path back and
   compare its SHA-256 against the local file. This is the same principle as section 14 of the OTA
   procedure doc, trust the device rather than the tool, applied to the mirror.
4. **Fix the flavour hole in the current-image pointer.** `stage()` in `ota-release.sh` goes to real
   trouble to keep the debug and release `.ota` apart, because both flavours share one version int
   and staging the wrong one is invisible. The HTTP mirror does not do the same: the archive name
   carries the flavour (`rac-v$v$sfx.bin`, `$(idf_target)-v$int-$flav.bin`) but the current-image
   basename passed to `pi_http_publish` is flavour-blind in both scripts. Staging a debug build
   therefore repoints the URL **every deployed node's break-glass fetches** at a debug image, whose
   `:2323` console is unauthenticated and drives the A/C bus. Either refuse to repoint the pointer
   from a debug stage, or give debug its own basename and accept that debug nodes need their own
   baked URL.
5. **Address stability.** The URL is baked at compile time, so moving the server means reflashing
   every node. Pin it with a DHCP reservation or a name that will not move, and put that consequence
   in the traps list in `10-firmware-ota-procedure.md`.
6. **Serve the archive and an index.** The archive directory already exists on disk; exposing it
   turns a manual scp into a URL that `revert` can use.
7. **Checksums beside the images.** Not a security control once signing is on, but they catch a
   truncated upload before a node does.

One consequence worth stating plainly: once images are signed, this docroot stops being sensitive.
It can be world-readable on the LAN, rebuilt from anywhere, or mirrored without ceremony, because a
tampered image is rejected by the device rather than by the server. That is the payoff for doing
section 2 before this section.

## 6. Key management

The signing key is now a single point of failure with the same severity as the delta-OTA base, and
it belongs in the same traps list.

- The private key never enters the repo. `*.pem` should join the ignore list beside `*.key`.
- Losing it means no node can ever be updated over the air again, only over USB or a clip. Back it
  up off the build box before the first signed release, not after.
- The `sdk-builder` runner needs it to sign tag builds. Either give the runner the key as a secret,
  or set `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n` in CI and sign afterwards with
  `espsecure.py sign_data --version 2 --keyfile <key> <image>` on the box that does the flashing,
  which is the route Espressif documents for keeping the key off the build machine. The second is
  preferable and it fits what CI already does: the release workflow's own note says its images are
  not the deployed ones, because IDF builds are not byte-reproducible and delta OTA verifies the
  base hash. It also means the key never has to exist as a CI secret at all.
- **Rotation is close to impossible over the air in this mode, so treat the key as permanent.**
  Hardware Secure Boot allows up to three signature blocks with three different keys, any one of
  which can verify. Signed-app verification without secure boot does not: "only the first public key
  in the signature block is used to verify updates", and IDF requires the app to carry exactly one
  valid block in the first position. There is no two-key transitional image to rotate through.
  Changing the key means physically reflashing every node, or moving to full hardware Secure Boot.
- Once signing is on, everything archived in `firmware/built-images/` must be the signed artifact,
  and `test_image_chain.py`'s archive half should grow an equivalent assertion for the ESP32 images
  so an unsigned artifact cannot be archived as a base by accident.

## 7. Phases

Each phase is independently useful and independently revertible. Nothing after phase 1 depends on
hardware that is not already on the bench.

**Phase 0. Settle the one fact that still reshapes the plan.** Board revisions, and therefore the
signing scheme per target: ESP32-C3 takes the RSA V2 scheme, classic ESP32 needs revision 3 for it
and otherwise falls back to V1 ECDSA, so the two release-matrix targets may need different values.
Exit criterion: written into the table above with its `// VERIFY` marker removed.

**Phase 1. ESP32 signing, on the bench.** Generate a key, set the options, flash the first signed
image over USB, archive it as the new delta base, and confirm that an unsigned or tampered image is
rejected by `esp_ota_end()` rather than booted. Exit criterion: a deliberately corrupted image over
the break-glass path produces a rejection and a node that is still reachable. **Then repeat over the
Matter delta path, and treat that as the risky half of the phase, not a formality.** The reading of
`OTAImageProcessorImpl.cpp` says it should work, but there is at least one public report of signed
delta patches failing to apply under hardware Secure Boot with Flash Encryption
(esp32.com thread 40807). Neither of those is being enabled here, so the report is not directly
applicable, and it is a good enough reason to prove the delta path on the bench before any release
depends on it.

**Phase 2. Rollback and the health gate.** Write the gate and the mark-valid call FIRST, then enable
rollback, then work out the watchdog interaction and the delta-base readback. Exit criterion: an
image that deliberately fails the gate reverts to the previous partition without intervention, an
image that fails only Matter does not, and `package` refuses to build a delta when the device's
running version disagrees with the archive marker.

**Phase 3. Break-glass challenge-response.** Firmware change plus the client side in
`firmware/scripts/dev.py`. Exit criterion: a captured exchange replayed verbatim is refused, and the
old bare-token path still works for nodes flashed before the change.

**Phase 4. Make the mirror survive its host.** The seven items in section 5. The flavour hole (item 4)
is a bug rather than a feature and can land first, on its own. Exit criterion: the `ota-http` service
is reproducible from the repo, `stage` verifies what it published, and break-glass recovery works on
both targets after rebuilding the docroot from scratch.

**Phase 5. AmebaZ2 verify-before-reset.** Independent of #75 and worth doing regardless. Exit
criterion: a corrupted image written to the idle slot is rejected on-device, and the node stays up.

**Phase 6. Docs and traps.** The signing key and the baked URL join the traps list in
`10-firmware-ota-procedure.md`; `esp32-lint.sh` grows a check that a release build has signing on;
`13-path-comparison.md` gains a row for image authenticity.

## 8. What this plan does not do

- **Secure boot proper.** Burning eFuses is one-way, it makes an unsigned recovery image
  unflashable, and on a device whose recovery story is a SOIC-8 clip that is a bad trade until
  everything above has been running for a while.
- **Flash encryption.** Same reasoning, and it would also make the clip-based recovery and the
  `:backup` command useless.
- **Anti-rollback (`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`).** Attractive on paper, and it would
  permanently block the `<token>:revert` path that has already rescued a node. Not until the
  update path has a track record.
- **Per-device keys or any kind of device identity.** One project key for one small fleet.
- **Changing the Matter OTA transport.** python-matter-server's provider and BDX stay exactly as
  they are. This plan changes what the device accepts, not how it is offered.

## Sources

Espressif documentation for the pinned IDF (`v5.5.4`), plus the pinned trees themselves. Line
citations above are against the local checkouts of `IDF_PIN` and `ESP_MATTER_PIN` from
`versions.env`.

- ESP-IDF Programming Guide v5.5.4, "Over The Air Updates (OTA)": the App Rollback contract, and
  "Secure OTA Updates Without Secure Boot", which names the two config options.
- ESP-IDF Programming Guide v5.5.4, "Secure Boot v2": "Signed App Verification Without Hardware
  Secure Boot", the chain-of-trust-from-the-running-app model, the single-valid-signature-block
  limitation, and the ESP32-C3 revision v0.3 (ECO3) floor.
- *ESP32-C3 Wireless Adventure*, chapter 13.4.3, "Introduction to Software Secure Boot": the
  `espsecure.py sign_data --version 2` remote-signing route, and confirmation that only the first
  signature block counts in this mode.
- esp32.com thread 40807, signed delta-OTA patches under hardware Secure Boot with Flash Encryption.
  Not our configuration; recorded as the reason phase 1 proves the delta path rather than assuming it.
