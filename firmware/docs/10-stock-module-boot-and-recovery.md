# 10: The stock module: how it boots, and how to get one back

For anyone who writes firmware to a stock AEH-W41H1 module or has one that went quiet. It says
what the module's bootloader accepts, how it picks a firmware slot, what a rejected image looks
like from outside, how to tell a dead module from a quiet one, and the rules for recovery with
the clip.

These facts were learned while this project shipped a Matter firmware for the module. That
firmware is retired (tag `matter-final`,
[`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md)). The
bootloader has not changed, and the ESPHome build for the module lives under the same rules.

Other pages own the rest:

- the flash layouts a module can have, with every slot address:
  [`15-esphome-path.md`](15-esphome-path.md#which-flash-layout-a-unit-has)
- the clip tools, their checks and their options: [`firmware/flasher/README.md`](../flasher/README.md)
- the clip procedure, the wiring and the warnings (3.3 V, never flashrom, the A/C unplugged):
  [`docs/guide/Recovery-and-Reflash.md`](../../docs/guide/Recovery-and-Reflash.md)
- the static analysis behind this page:
  [`reverse-engineering/docs/13-stock-fw-feature-map.md`](../../reverse-engineering/docs/13-stock-fw-feature-map.md)
  and [`reverse-engineering/docs/07-firmware-and-ota.md`](../../reverse-engineering/docs/07-firmware-and-ota.md)

## 1. The slot is chosen by the FWHS serial

The module has two firmware slots. The bootloader boots the slot that holds a **valid image with
the higher serial**: the `FWHS` header serial, a `u32` at image offset `0xF4`. Nothing else
decides it: no "last written" flag, no application version.

- An update that writes a valid image with a serial that is not higher than the running one
  applies and then never boots. From outside it looks like a rollback. This cost a session on
  2026-07-08: every Matter update "finished successfully" and the unit stayed on the old image,
  because every build carried the same serial.
- The Matter builds used `serial = SERIAL_BASE + softwareVersion` (`SERIAL_BASE` 1100), so the
  serial rose with every version. The vendor firmware's serial is 100.
- LibreTiny stamps a serial taken from the build time, in the tens of millions, far above both.
  A unit that has run an ESPHome image therefore never boots a Matter-numbered image again
  unless the ESPHome slot is invalidated or erased.
- `boot_load` is slot-agnostic. The image base is `0x98000000 + slot_start` from the partition
  table and the virtual base is taken from the section header. The vendor firmware has booted
  from the second slot on hardware (2026-07-27). Never add a "this image must live in slot 1"
  rule to a tool.

`matter-convert.py convert` re-signs an ESPHome image with a serial in the Matter numbering for
exactly this reason: the Matter firmware writes it to the inactive slot and the bootloader then
prefers it.

## 2. What makes an image valid

Three checks, all of which the bootloader makes (issue #75):

```
manifest signature   img[0:32] == HMAC-SHA256(key, img[0xE0:0x140])

for sub-image i whose header is at H (the first is at H = 0xE0):
  S     = u32le(img[H])            # segment SIZE at H+0x00
  END   = H + 0x60 + S
  START = 0 if i == 0 else H
  img[END : END+0x20] == HMAC-SHA256(key, img[START:END])
  next header = H + u32le(img[H+4])        # RELATIVE; 0xFFFFFFFF terminates

byte-sum             last 4 bytes == sum(all bytes before them) & 0xFFFFFFFF
```

- The key is the partition hash key at flash `0x140`. The SDK's default key is what the vendor
  image, the Matter builds and LibreTiny all sign with, which is why re-signing works at all.
- Sub-image 0's span starts at offset 0, so it covers the serial at `0xF4` **and** the manifest
  signature. Change the serial and three things must be recomputed, in this order: the manifest
  signature, sub-image 0's trailer, the byte-sum.
- `img[0xE0]` is the size field. Reading `img[0xE4]` as the size puts the trailer `0x2E0` bytes
  too late and makes every genuine image look corrupt.
- Never hardcode the chain (the vendor firmware ships two layouts) and never require the last
  trailer to end at the end of the file: the build tool pads 0 or `0x20` trailing bytes.

`firmware/scripts/amebaz2_image.py` is the one implementation of this recipe (walk, verify,
re-sign). `matter-convert.py` and the clip writers in `firmware/flasher/` use it.
`firmware/test/test_image_chain.py` holds it down with synthetic images, and with any real
images found in `firmware/built-images/` or `dumps/` (both gitignored).

## 3. A rejected image hangs the module

On a failed check `boot_load` prints `Hash Result Incorrect!` on the log UART, clears the flash
**Quad-Enable (QE) bit** and returns an error. The caller then hangs. **It does not fall back to
the other slot**, even when that slot holds a perfectly valid image.

That is what bricked a unit on 2026-07-21: a re-signed image with a stale sub-image 0 trailer.
Every host-side check of the time passed (manifest signature, byte-sum) and the flash content was
byte-perfect. Recovery was the clip.

Consequences for any tool that writes a slot:

- Verify every trailer of the exact bytes that will be written, after the last modification.
  `ch341flash.py` and `matter-convert.py convert` both do.
- An image that boots but cannot reach the network is a different failure with the same
  recovery. Nothing rolls an image back.

## 4. Silence is not a brick

A unit that has gone quiet on the network is usually alive. Two healthy states look the same as a
dead module from outside:

- A unit running the vendor firmware joins the vendor's own network setup and cloud, and is
  invisible on yours.
- A unit that boots an image without your Wi-Fi credentials is up and unreachable. An ESPHome
  image opens its `<name>-setup` access point in that case.

What settles it, cheapest first:

1. **The QE bit**, read with the clip and no soldering: `python3 firmware/flasher/ch341_sr.py`
   (status register 2, bit 1). A bootloader rejection **clears** it, a healthy boot leaves it
   **set**. This was readable the whole time on both occasions a healthy unit was taken for a
   brick (2026-07-26/27).
2. **A clip dump** (`firmware/flasher/ch341dump.py`), then a walk of the image chain in every
   slot. `amebaz2_image.py` is a module with no command line, so from the repository root:

   ```python
   import sys
   sys.path.insert(0, "firmware/scripts")
   import amebaz2_image as img

   blob = open(sys.argv[1], "rb").read()
   for off in range(0, len(blob), 0x1000):
       if img.manifest_ok(blob[off:off + 0x140]):
           ok, lines = img.verify(blob[off:off + 0x1AC000])
           print("0x%06X serial %d %s" % (off, img.read_serial(blob[off:]), "valid" if ok else "INVALID"))
           for line in img.failures(lines):
               print("   ", line)
   ```

   It prints one line per image found at a sector boundary, with the trailer that failed when
   one did. It checks the manifest signature and every sub-image trailer, not the byte-sum. A
   valid chain in the slot with the higher serial plus QE set means the firmware is fine and the
   problem is elsewhere.
3. On a unit that still runs the retired Matter firmware, its break-glass listener:
   `matter-convert.py slots <ip>` prints `ok: fw1_sn=<n> fw2_sn=<n> cur=<slot>`. An answer
   proves the unit is alive. No answer proves nothing.
4. On a unit that runs ESPHome, the `Flash layout` sensor carries the same three values.

## 5. Recovery with the clip

The rules that come from the boot process. The commands are in
[`firmware/flasher/README.md`](../flasher/README.md).

- **Dump the whole chip before the first write, twice, and compare.** That dump holds the unit's
  own calibration data, keys and vendor image. It is the only complete way back.
  `ch341dump.py` reads twice and refuses to write a file when the reads differ, and both writers
  refuse to run without a dump named with `--backup`.
- The recipe that has worked: write the unit's own dump with slot 1 replaced by the original
  vendor slot bytes (factory signature, from a `matter-convert.py backup` file or the first
  dump) and slot 2 erased to `0xFF`. `ch341flash-full.py` sets QE again at the end. The unit
  boots the vendor firmware and rejoins its cloud.
- Flash `0x1000..0x3FFF` holds the module's system data, including its calibration data, and
  must survive. A full-flash image produced by a build is blank there and must not be written
  over a module as it is. `ch341flash-full.py` refuses a file in which `0x1000..0x1FFF` is
  erased.
- Do not transplant another unit's settings area. One unit given a known-good byte range from
  another was left wedged even though every byte matched what the donor had. Let the running
  firmware format its own storage.
- Do not write another unit's dump. Per-unit data lives outside the firmware slots, and no tool
  here can tell two modules apart: the low sectors are the same on every unit dumped so far (two
  modules).
- Before a second firmware ever overwrites it, save the vendor image from the inactive slot. On
  the retired Matter firmware (1.3.9 or later) `matter-convert.py backup <ip>` streams it out
  and verifies it. Otherwise it is in your first clip dump.

## 6. Dead ends, so nobody repeats them

- **Downloading a public vendor image.** The only firmware URL compiled into the vendor firmware
  serves an older module generation, with no 4 KB block in common with a W41H1 dump. The device
  never polls for versions.
- **Spoofing the cloud to push an update.** The gateway connection verifies a pinned CA, and the
  command channel is AES-256-CBC with HMAC-SHA256 keyed per device.
- **Reading flash back without a clip.** The vendor firmware has no readback path. The retired
  Matter firmware added one (`:backup`). ESPHome on the module has none.
- **A spot-check at hand-picked offsets.** Until 2026-10-12 `ch341_readverify.py` compared nine
  offsets chosen for the retired Matter clip image. On real dumps those nine reported a match
  between a module with the vendor image in its second slot and the same module with ESPHome
  there, and between the dumps of two different modules. It now compares an even grid.
