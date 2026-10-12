#!/usr/bin/env python3
"""Write ONE firmware slot + set QE. Everything outside the slot is left alone: the partition
table, the bootloader, the module's system data, the other slot and the settings at the top of
flash. Transport and guards: ch341common.

    ch341flash.py <image.bin> --layout factory|sdk|native --slot 1|2 --backup <dump.bin>
                  [--confirm <layout>-ota<slot>-0x<slot length>]

<image.bin> is an application image as a build writes it (payload and byte-sum, for example
LibreTiny's image_firmware_is.0x010000.bin) or a raw slot (`matter-convert.py backup`). It is
checked the way the bootloader checks it, every sub-image trailer included, and has to fit the
slot. The whole slot is written: the image, then erased flash to the slot's end.

NOT RUN ON HARDWARE YET in this form. Until 2026-10-12 this script wrote 0x0..0x140000, the
range of the retired Matter clip image: under the current layouts that range starts at the
partition table and ends inside a firmware slot, so it had no safe use. The sector writer below
it is the same hardware-validated one. What has not been seen on a module is a boot after a
write of a slot alone.

The layout is checked against the chip: an image has to sit at one of the layout's two slot
addresses and at no other layout's. The bootloader boots the valid slot with the HIGHER serial. The serials of both slots on the
chip are printed before the confirmation: an image whose serial is not the highest is written
and then not booted.
"""
import argparse
import sys
from pathlib import Path

import ch341common as ch
from ch341common import amebaz2_image


def slot_serial(c, addr):
    """The serial of the image in a slot on the chip, or None when no signed manifest is there."""
    head = c.read(addr, amebaz2_image.HDR0 + amebaz2_image.HDR_LEN)
    return amebaz2_image.read_serial(head) if amebaz2_image.manifest_ok(head) else None


def shown(serial):
    return "no image" if serial is None else "serial %d" % serial


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", metavar="image.bin", help="the application image for the slot")
    ap.add_argument("--layout", required=True, choices=ch.LAYOUTS, help="the flash layout the unit has")
    ap.add_argument("--slot", required=True, type=int, choices=(1, 2))
    ap.add_argument("--backup", metavar="dump.bin", help="a full dump of this chip (ch341dump.py)")
    ap.add_argument("--confirm", metavar="TOKEN", help="the confirmation the prompt asks for, for scripted use")
    a = ap.parse_args(argv)
    try:
        blob = Path(a.image).read_bytes()
    except OSError as e:
        raise SystemExit("%s: %s" % (a.image, e.strerror))
    addr, length = ch.layout_slot(a.layout, a.slot)
    if len(blob) == ch.CHIP_SIZE:
        raise SystemExit("%s is a whole-chip dump, not a slot image (that is ch341flash-full.py). Nothing was written" % a.image)
    try:
        data, serial = ch.slot_data(blob)
    except ValueError as e:
        raise SystemExit("refusing to write %s: it does not verify as an application image (%s). The bootloader "
                         "hangs on such an image. Nothing was written" % (a.image, e))
    if len(data) > length:
        raise SystemExit("refusing to write %s: the image is 0x%X bytes, slot %d of the %s layout holds 0x%X. "
                         "Nothing was written" % (a.image, len(data), a.slot, a.layout, length))
    ch.need_backup(a.backup)

    c = ch.open_chip()
    # Which layout the chip has is read off the chip: a slot address holds a signed manifest or it
    # does not. An image written at another layout's address lands across both of the unit's real
    # slots, which is a module that no longer boots.
    known = sorted({ch.layout_slot(layout, n)[0] for layout in ch.LAYOUTS for n in (1, 2)})
    serials = {at: slot_serial(c, at) for at in known}
    other_addr, _ = ch.layout_slot(a.layout, 3 - a.slot)
    found = [at for at in known if serials[at] is not None]
    if not found:
        raise SystemExit("no application image at any slot address of any layout: a blank or unknown chip, so the "
                         "layout cannot be checked. Write the unit's full dump with ch341flash-full.py. Nothing was written")
    foreign = [at for at in found if at not in (addr, other_addr)]
    if foreign:
        raise SystemExit("the chip holds an image at %s, which is not a slot of the %s layout (0x%06X, 0x%06X): wrong "
                         "--layout, or a leftover of an earlier layout. For the second case use ch341flash-full.py "
                         "with an edited dump. Nothing was written"
                         % (", ".join("0x%06X" % at for at in foreign), a.layout, *sorted((addr, other_addr))))
    here, other = serials[addr], serials[other_addr]
    print("about to replace slot %d of the %s layout (0x%06X..0x%06X, %d bytes) with %s"
          % (a.slot, a.layout, addr, addr + length, length, a.image))
    print("  image: 0x%X bytes, serial %d, every signature and the byte-sum verify. It has to be the one built for "
          "this layout (--board amebaz2-%s): that cannot be read off the image" % (len(data), serial, a.layout))
    print("  on the chip now: this slot holds %s, the other slot (0x%06X) %s" % (shown(here), other_addr, shown(other)))
    if other is not None and other >= serial:
        print("  WARNING: the other slot's serial is not below this image's. If that image is valid the bootloader "
              "boots IT, not what is written here.")
    print("  the way back is %s" % a.backup)
    if not ch.confirm("%s-ota%d-0x%X" % (a.layout, a.slot, length), a.confirm):
        raise SystemExit("not confirmed. Nothing was written")
    ch.flash_region(c, data + b"\xff" * (length - len(data)), addr)
    c.close()


if __name__ == "__main__":
    main(sys.argv[1:])
