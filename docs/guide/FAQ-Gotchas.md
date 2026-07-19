# FAQ & Gotchas

The load-bearing traps, distilled for operator/future-you. Each answer links to the authoritative
doc.

## The OTA "finished successfully" but the device is still on the old version. Did it roll back?

Usually the **AmebaZ2 serial trap**, not a real rollback. The bootloader picks the boot slot
by the image's `FWHS.header.serial`, **not** the Matter `softwareVersion`; if the serial isn't
bumped, the update applies then reverts, looking like a rollback. **The script handles it**
(`serial = SERIAL_BASE + version`, log-verified), so this only bites if you hand-build. Always use
`ota-release.sh`. (`firmware/docs/10-firmware-ota-procedure.md` §11)

On **ESP32** the same symptom has a different cause: a device built with
`CONFIG_ENABLE_DELTA_OTA=y` **rejects a full image**. It transfers the whole thing, the provider
says "finished", and the device stays on the old build. It accepts only a delta patch built
against the **exact binary it is running**, whose SHA it verifies before applying. See
[OTA Updates](OTA-Updates#esp32-delta-ota).

## The OTA errors "Target node did not process the update file". Is it broken?

No. OTA is flaky by design. The ephemeral provider loses a discovery race on the first attempt(s).
**Retry 3–5×, ~15 s apart** (the script does). Verify the *reported* version changed; don't trust
"finished". ([OTA Updates](OTA-Updates))

## Why won't the provider serve my new firmware?

The version must be **strictly greater** than what's running (convention: running + 1), and you must
never reuse a rolled-back version number. Matter-server and the device cache by version. `--bump`
handles it; `lint` blocks a stale version. ([OTA Updates](OTA-Updates))

## I OTA'd new endpoints/entities but they don't show in HA.

HA caches the node structure. After a structure-changing OTA you need a **node re-interview**.
`flash` auto-interviews, otherwise reload the Matter integration (Settings → Devices & Services →
Matter → ⋮ → Reload). ([Commissioning & HA Setup](Commissioning-and-HA-Setup))

## The node commissioned but shows `available=False`.

Cross-VLAN **IPv6 mDNS** doesn't cross by default, and many mDNS reflectors won't relay it. Fix:
dual-home the matter-server host onto the A/C's VLAN (a tagged IPv6-only sub-interface), then restart
matter-server. ([Commissioning & HA Setup](Commissioning-and-HA-Setup))

## Why must endpoints stay contiguous?

Keep endpoints `{0,1,2,…}` with no gaps and **renumber** to close a hole rather than leave one. A gap
was one of three confounded factors in a boot-crash/rollback, never isolated, so contiguity is
treated as a **zero-cost precaution**, and `lint` blocks a non-contiguous `.zap`. (`firmware/docs/10-firmware-ota-procedure.md` §3)

## My build finished in ~90 seconds. Good?

**No. Stop.** A genuine build is ~20–30 min. A sub-2-min build didn't recompile and links a stale,
suspect image (it flashes and OTAs fine, then fails to boot). `ota-release.sh build` does the
mandatory full clean. (`firmware/docs/10-firmware-ota-procedure.md` §4)

## What is "77"?

Press the remote's swing button **6 times** → the display shows **"77"**: the module resets its
Wi-Fi and opens the Matter commissioning window. Use it before commissioning or converting a unit.
([Commissioning & HA Setup](Commissioning-and-HA-Setup))

## HA shows a duplicate Power switch and split fan/mode tiles. How do I clean it up?

That's the raw Matter device. Install the **`hisense-unified-ac`** integration to merge everything
into one climate entity (special modes become presets), then hide the redundant native Power switch
and raw special-mode switches. ([Everyday Control](Everyday-Control))

## Setpoint changes do nothing sometimes.

Temperature is only honored in **cool/heat**. A temp change in dry / fan-only / auto / off is a no-op
and shows no target (the unified integration gates this explicitly). ([Everyday Control](Everyday-Control))

## Eco/Turbo/Mute/Sleep don't respond on the raw device.

The manufacturer cluster (`0xFFF1FC00`) is **read-only** to python-matter-server. A direct write
returns null, not a real command. Control them through the unified integration's **presets**; on the
plain device they only surface as auto-detected switches/select. ([Everyday Control](Everyday-Control) · `firmware/docs/05-ha-control-and-native-ui.md`)

## The module keeps browning out / the radio drops.

Power it from the **A/C**, not a bench supply. The 5 V from the unit is what the radio expects.
([Hardware & Wiring](Hardware-and-Wiring))

## I bricked a module. Now what?

Reflash with the **CH341A clip** (not flashrom). Region write to repair (preserves commissioning),
or whole-chip write of `flash_rac-stock-v1.bin` for a clean restore. Keep `dumps/w41h1_dump1.bin` as
the net. ([Recovery & Reflash](Recovery-and-Reflash))

## Can I convert a still-stock unit without opening it?

Yes. The stock firmware has a live Matter interface + OTA requestor, so a stock W41H1 can be
converted **over the air**. No clip, no disassembly. ([Installing the Custom Firmware](Installing-Custom-Firmware) · `firmware/docs/12-ota-convert-stock-unit.md`)

## How do I find out which features MY A/C has?

Flash the **debug** image and ask it. The A/C reports its own capabilities in a `0x66/40`
ProductType reply, and the debug flavour exposes that over a `:2323` console:

```
nc <device-ip> 2323
features
```

You get the decoded flags for **that unit**, including whether it has 8 °C frost-guard heat,
purify, AI mode, an 8-position louvre, humidity and a dimmable display.

A `0` means *not on this unit*, never *not on this model*. Two units of the same model can differ,
which is why the firmware gates features at runtime on what the A/C reports rather than assuming.

The console is debug-only on purpose: it has no authentication and can drive the A/C bus, so it is
kept out of release images. Flash the release image once you have your answer.

## Which image should I flash, release or debug?

**Release**, unless you are doing bench work. Both are published with every tagged release and are
identical apart from diagnostics: the debug image adds the `:2323` console and verbose logging, and
carries the security tradeoff above. Files are named by flavour
(`flash_rac-integrated-v<N>-debug.bin` vs `flash_rac-integrated-v<N>.bin`); the version number is
the same for both, so check the filename rather than the version.
